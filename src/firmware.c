/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file firmware.c
 * @brief Production P2000M capture, resampling, VGA, and USB-control firmware.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pal_output.h"
#include "p2000m_capture.h"
#include "p2000m_packbits.h"
#include "p2000m_phosphor_noise.h"
#include "p2000m_signal_loss.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

/** Semantic firmware version supplied by the top-level CMake project. */
static const char firmware_version[] = "v" P2000M_VID2VGA_VERSION;
/** Compile-time firmware identification shown without requiring USB. */
static const char signal_lost_firmware[] =
    "FIRMWARE v" P2000M_VID2VGA_VERSION;

enum {
    /** Experimental 2x clock preserving exact capture and VGA divisors. */
    SYSTEM_CLOCK_KHZ = 252000,
    /** Maximum SDK-supported regulator setting used for the overclock. */
    SYSTEM_CORE_VOLTAGE_MV = 1300,
    /** Active VGA pixels per line. */
    VGA_WIDTH = 640,
    /** Active VGA lines per frame. */
    VGA_HEIGHT = 480,
    /** Background lines above the centered source image. */
    VGA_TOP_MARGIN = (VGA_HEIGHT - P2000M_CAPTURE_HEIGHT) / 2,
    /** Background lines below the centered source image. */
    VGA_BOTTOM_MARGIN = VGA_HEIGHT - VGA_TOP_MARGIN - P2000M_CAPTURE_HEIGHT,
    /** RAW_RUN header, 639 pixels, black reset pixel, and EOL tokens. */
    RAW_SCANLINE_TOKENS = (3 + VGA_WIDTH - 1) + 2 + 2,
    /** Maximum 32-bit scanvideo words required by a source line. */
    RAW_SCANLINE_WORDS = RAW_SCANLINE_TOKENS / 2,
    /** Packed one-bit words in a decoded source line. */
    DECODED_WORDS_PER_LINE = P2000M_CAPTURE_WIDTH / 32,
    /** Packed one-bit words in a decoded source frame. */
    DECODED_WORDS_PER_FRAME =
        DECODED_WORDS_PER_LINE * P2000M_CAPTURE_HEIGHT,
    /** Triple buffers decoupling core 0 decoding from VGA presentation. */
    DECODED_BUFFER_COUNT = 3,
    /** Lookup/style buffers used for atomic visual-setting changes. */
    DISPLAY_STYLE_COUNT = 2,
    /** Maximum USB command length including its null terminator. */
    COMMAND_BUFFER_SIZE = 64,
    /** Delay between statistics records while USB log streaming is active. */
    USB_LOG_INTERVAL_US = 2000000,
    /** Fixed binary header prepended to every streamed screen frame. */
    SCREEN_FRAME_HEADER_SIZE = 48,
    /** Packed bytes in one decoded 640 x 288 monochrome framebuffer. */
    SCREEN_FRAME_PAYLOAD_SIZE = DECODED_WORDS_PER_FRAME * sizeof(uint32_t),
    /** Maximum bytes offered to TinyUSB during one foreground-loop pass. */
    SCREEN_TX_SERVICE_BUDGET = 1024,
    /** PackBits bytes staged before each TinyUSB FIFO write. */
    SCREEN_PACKBITS_STAGING_SIZE = 1024,
    /** Source-frame sequence advance used to limit streaming to about 25 Hz. */
    SCREEN_SEQUENCE_STEP = 2,
    /** Number of alternating flash sectors used for atomic settings updates. */
    SETTINGS_SLOT_COUNT = 2,
    /** First settings sector; the RP2350's final flash sector stays reserved. */
    SETTINGS_FLASH_OFFSET =
        PICO_FLASH_SIZE_BYTES - (SETTINGS_SLOT_COUNT + 1) * FLASH_SECTOR_SIZE,
    /** File-format signature: ASCII "V2GA" when viewed little-endian. */
    SETTINGS_MAGIC = 0x41473256,
    /** On-flash settings structure version. */
    SETTINGS_VERSION = 4,
    /** Maximum time to coordinate each multicore flash lockout phase. */
    FLASH_LOCKOUT_TIMEOUT_MS = 1000,
    /** Core-start handshake value sent through the multicore FIFO. */
    VGA_READY_MAGIC = 0x56474131,
    /** Integer enlargement applied to the signal-loss message. */
    SIGNAL_LOST_FONT_SCALE = 4,
    /** Integer enlargement applied to signal-loss status details. */
    SIGNAL_LOST_INFO_SCALE = 2,
    /** Left edge of the centered signal-loss panel. */
    SIGNAL_LOST_PANEL_LEFT = 90,
    /** Right edge, exclusive, of the centered signal-loss panel. */
    SIGNAL_LOST_PANEL_RIGHT = 550,
    /** Top edge of the centered signal-loss panel. */
    SIGNAL_LOST_PANEL_TOP = 140,
    /** Bottom edge, exclusive, of the centered signal-loss panel. */
    SIGNAL_LOST_PANEL_BOTTOM = 340,
    /** Thickness of the red panel outline. */
    SIGNAL_LOST_PANEL_BORDER = 3,
    /** Top edge of the product-name line. */
    SIGNAL_LOST_PRODUCT_TOP = 164,
    /** Top edge of the primary warning line. */
    SIGNAL_LOST_MESSAGE_TOP = 200,
    /** Top edge of the firmware-version line. */
    SIGNAL_LOST_FIRMWARE_TOP = 254,
    /** Top edge of the synchronization-wait line. */
    SIGNAL_LOST_WAITING_TOP = 294,
};

_Static_assert((unsigned)VGA_WIDTH == (unsigned)P2000M_CAPTURE_WIDTH,
               "Step 3 requires one-to-one horizontal pixels");
_Static_assert(VGA_TOP_MARGIN == 96 && VGA_BOTTOM_MARGIN == 96,
               "The source image must be vertically centered");
_Static_assert((RAW_SCANLINE_TOKENS & 1u) == 0,
               "Composable scanline must occupy whole words");
_Static_assert(P2000M_CAPTURE_WIDTH % 32u == 0u,
               "Decoded scanlines must contain whole words");
_Static_assert(SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u,
               "Settings storage must start on a flash-sector boundary");

/** Standard 640 x 480, nominal 60 Hz VGA timing at a 25.2 MHz pixel clock. */
static const scanvideo_timing_t vga_timing_640x480_60 = {
    .clock_freq = 25200000,
    .h_active = VGA_WIDTH,
    .v_active = VGA_HEIGHT,
    .h_front_porch = 16,
    .h_pulse = 96,
    .h_total = 800,
    .h_sync_polarity = 1,
    .v_front_porch = 10,
    .v_pulse = 2,
    .v_total = 525,
    .v_sync_polarity = 1,
    .enable_clock = 0,
    .clock_polarity = 0,
    .enable_den = 0,
};

/** Scanvideo mode pairing the timing above with composable RGB444 output. */
static const scanvideo_mode_t firmware_vga_mode = {
    .default_timing = &vga_timing_640x480_60,
    .pio_program = &video_24mhz_composable,
    .width = VGA_WIDTH,
    .height = VGA_HEIGHT,
    .xscale = 1,
    .yscale = 1,
    .yscale_denominator = 1,
};

typedef enum {
    /** Buffer is available to the capture-to-monochrome decoder. */
    DECODED_FREE,
    /** Core 0 is currently writing the buffer. */
    DECODED_FILLING,
    /** Buffer contains a complete immutable frame available to consumers. */
    DECODED_READY,
} decoded_buffer_state_t;

/** Mutually exclusive interaction modes for the USB serial terminal. */
typedef enum {
    /** Editable, echoed command line with no unsolicited statistics. */
    USB_COMMAND_MODE,
    /** Periodic statistics stream; one exit key restores the prompt. */
    USB_LOG_MODE,
    /** Continuous binary 640 x 288 monochrome framebuffer stream. */
    USB_SCREEN_MODE,
} usb_interface_mode_t;

enum {
    /** Persisted VGA overlay enable bit. */
    DISPLAY_FLAG_BORDER_ENABLED = 1u << 0,
    /** Persisted VGA dotted-border selection bit. */
    DISPLAY_FLAG_BORDER_DOTTED = 1u << 1,
    /** Persisted full-height vertical-scaling selection bit. */
    DISPLAY_FLAG_VERTICAL_STRETCH = 1u << 2,
    /** Persisted physical VGA output enable bit. */
    OUTPUT_FLAG_VGA_ENABLED = 1u << 0,
    /** Persisted physical PAL composite output enable bit. */
    OUTPUT_FLAG_PAL_ENABLED = 1u << 1,
};

/** User-selectable colors and geometry overlay for one VGA frame. */
typedef struct {
    /** Foreground color as entered by the user, in 0xRRGGBB form. */
    uint32_t foreground_rgb;
    /** Background color as entered by the user, in 0xRRGGBB form. */
    uint32_t background_rgb;
    /** Border color as entered by the user, in 0xRRGGBB form. */
    uint32_t border_rgb;
    /** Whether to draw a one-pixel rectangle around the 640 x 288 image. */
    bool border_enabled;
    /** Whether the border alternates two colored and two clear pixels. */
    bool border_dotted;
    /** Whether to scale 288 source lines to all 480 active VGA lines. */
    bool vertical_stretch_enabled;
    /** Density of one-DAC-step phosphor-grain dimming on foreground pixels. */
    uint8_t phosphor_noise_level;
} display_style_t;

/** Versioned, checksummed display/output settings stored redundantly in flash. */
typedef struct {
    /** SETTINGS_MAGIC identifies records owned by this firmware. */
    uint32_t magic;
    /** SETTINGS_VERSION identifies the record layout. */
    uint16_t version;
    /** Exact structure size rejects incompatible future layouts. */
    uint16_t length;
    /** Monotonic counter used to select the newest of two valid slots. */
    uint32_t sequence;
    /** Saved foreground color in 0xRRGGBB form. */
    uint32_t foreground_rgb;
    /** Saved background color in 0xRRGGBB form. */
    uint32_t background_rgb;
    /** Saved border color in 0xRRGGBB form. */
    uint32_t border_rgb;
    /** Packed DISPLAY_FLAG_* values. */
    uint8_t display_flags;
    /** Saved p2000m_phosphor_noise_level_t value. */
    uint8_t phosphor_noise_level;
    /** Saved manual sampling-phase trim from -4 through +4 ticks. */
    int8_t manual_phase_ticks;
    /** Packed OUTPUT_FLAG_* values. */
    uint8_t output_flags;
    /** CRC-32 of every preceding byte in this structure. */
    uint32_t checksum;
} persisted_settings_t;

_Static_assert(sizeof(persisted_settings_t) == 32u,
               "Persisted settings format must remain exactly 32 bytes");
_Static_assert(offsetof(persisted_settings_t, checksum) == 28u,
               "Persisted settings checksum must remain at byte 28");

/** Version-three record retained solely to migrate existing saved settings. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint32_t border_rgb;
    uint8_t display_flags;
    uint8_t phosphor_noise_level;
    int8_t manual_phase_ticks;
    uint8_t reserved;
    uint32_t checksum;
} persisted_settings_v3_t;

_Static_assert(sizeof(persisted_settings_v3_t) == 32u,
               "Version-three settings format must remain exactly 32 bytes");
_Static_assert(offsetof(persisted_settings_v3_t, checksum) == 28u,
               "Version-three settings checksum must remain at byte 28");

/** Version-two record retained solely to migrate existing saved settings. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint32_t border_rgb;
    uint8_t border_enabled;
    uint8_t border_dotted;
    uint8_t vertical_stretch_enabled;
    int8_t manual_phase_ticks;
    uint32_t checksum;
} persisted_settings_v2_t;

_Static_assert(sizeof(persisted_settings_v2_t) == 32u,
               "Version-two settings format must remain exactly 32 bytes");
_Static_assert(offsetof(persisted_settings_v2_t, checksum) == 28u,
               "Version-two settings checksum must remain at byte 28");

/** Version-one record retained solely to migrate existing saved settings. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint8_t border_enabled;
    uint8_t vertical_stretch_enabled;
    int8_t manual_phase_ticks;
    uint8_t reserved[5];
    uint32_t checksum;
} persisted_settings_v1_t;

_Static_assert(sizeof(persisted_settings_v1_t) == 32u,
               "Legacy settings format must remain exactly 32 bytes");
_Static_assert(offsetof(persisted_settings_v1_t, checksum) == 28u,
               "Legacy settings checksum must remain at byte 28");

/** RAM-resident parameters consumed by the safe flash callback. */
typedef struct {
    /** Sector-aligned flash offset to erase. */
    uint32_t offset;
    /** Sector-multiple byte count to erase. */
    size_t erase_size;
    /** Whether to program page after erasing. */
    bool program_page;
    /** Full flash page containing the settings record and erased padding. */
    uint8_t page[FLASH_PAGE_SIZE];
} flash_mutation_t;

/** Named RGB color accepted by the USB command interface. */
typedef struct {
    /** Case-insensitive name typed by the user. */
    const char *name;
    /** Eight-bit-per-channel color in 0xRRGGBB form. */
    uint32_t rgb;
} color_preset_t;

/** Presets shared by the foreground and background commands. */
static const color_preset_t color_presets[] = {
    {.name = "black",   .rgb = 0x000000u},
    {.name = "white",   .rgb = 0xffffffu},
    {.name = "green",   .rgb = 0x00ff00u},
    {.name = "amber",   .rgb = 0xffb000u},
    {.name = "cyan",    .rgb = 0x00ffffu},
    {.name = "magenta", .rgb = 0xff00ffu},
    {.name = "red",     .rgb = 0xff0000u},
    {.name = "blue",    .rgb = 0x0000ffu},
    {.name = "yellow",  .rgb = 0xffff00u},
    {.name = "gray",    .rgb = 0x808080u},
};

/** Three decoded frames shared by capture, USB, VGA, and PAL output. */
static uint32_t decoded_frames[DECODED_BUFFER_COUNT][DECODED_WORDS_PER_FRAME];
/** Ownership state for each entry in decoded_frames. */
static decoded_buffer_state_t decoded_states[DECODED_BUFFER_COUNT];
/** P2000M capture sequence number represented by each decoded frame. */
static uint32_t decoded_sequences[DECODED_BUFFER_COUNT];
/** Cross-core lock protecting decoded_states and decoded_sequences. */
static spin_lock_t *decoded_lock;
/** Whether the USB transmitter currently holds each decoded buffer immutable. */
static bool decoded_usb_holds[DECODED_BUFFER_COUNT];
/** Whether VGA currently needs each complete decoded buffer to stay immutable. */
static bool decoded_vga_holds[DECODED_BUFFER_COUNT];
/** Whether PAL currently needs each complete decoded buffer to stay immutable. */
static bool decoded_pal_holds[DECODED_BUFFER_COUNT];

/** Double-buffered byte-to-eight-pixel lookup tables for arbitrary colors. */
static uint16_t monochrome_pixels[DISPLAY_STYLE_COUNT][256][8];
/** Double-buffered user display settings corresponding to the lookup tables. */
static display_style_t display_styles[DISPLAY_STYLE_COUNT];
/** Style most recently published by core 0 for the next VGA frame. */
static volatile unsigned requested_style_index;
/** Style accepted by core 1 at a VGA frame boundary. */
static volatile unsigned applied_style_index;
/** Style used by core 1 throughout the current VGA frame. */
static unsigned displayed_style_index;

/** Decoded buffer currently being presented, or -1 before the first frame. */
static int displayed_buffer = -1;
/** Source capture sequence currently being presented. */
static uint32_t displayed_sequence;
/** Input-lock state sampled by core 1 at the current VGA frame boundary. */
static bool displayed_signal_present;

/** Decoded buffer currently presented on PAL, or -1 before the first field. */
static int pal_displayed_buffer = -1;
/** Source capture sequence currently presented on PAL. */
static uint32_t pal_displayed_sequence;
/** Requested VGA producer pause; true also means the physical output is off. */
static volatile bool vga_pause_requested;
/** VGA producer pause state acknowledged by core 1. */
static volatile bool vga_pause_applied;
/** Requested PAL pause; true also means the physical output is off. */
static volatile bool pal_pause_requested;
/** PAL pause state acknowledged by the core-1 output loop. */
static volatile bool pal_pause_applied;

/** Lifetime VGA frame counter, written by core 1. */
static volatile uint32_t generated_vga_frames;
/** Discontinuities proving scanvideo advanced before a line was generated. */
static volatile uint32_t vga_scanline_gaps;
/** Number of VGA frame boundaries that selected a new decoded source frame. */
static volatile uint32_t source_frame_swaps;
/** Number of VGA frames that deliberately repeated the preceding source. */
static volatile uint32_t repeated_vga_frames;
/** Number of VGA frames generated before the first decoded source existed. */
static volatile uint32_t blank_vga_frames;
/** Number of complete raw frames converted into decoded monochrome frames. */
static volatile uint32_t decoded_source_frames;
/** Duration in microseconds of the most recent full-frame decode. */
static volatile uint32_t last_decode_us;
/** Longest full-frame decode observed since boot, in microseconds. */
static volatile uint32_t maximum_decode_us;
/** Partially received newline-terminated command from the USB CDC port. */
static char usb_command_buffer[COMMAND_BUFFER_SIZE];
/** Number of valid characters currently held in usb_command_buffer. */
static size_t usb_command_length;
/** True while discarding an overlong command up to its terminating newline. */
static bool usb_command_overflow;
/** Current interactive or streaming behavior of the USB terminal. */
static usb_interface_mode_t usb_interface_mode = USB_COMMAND_MODE;
/** Suppresses LF when a terminal sends CR/LF as one line terminator. */
static bool usb_ignore_next_lf;
/** Timestamp for the next periodic record in USB_LOG_MODE. */
static uint64_t next_usb_log_us;
/** One complete screen-frame header assembled without structure padding. */
static uint8_t screen_frame_header[SCREEN_FRAME_HEADER_SIZE];
/** Decoded buffer currently being transmitted, or -1 while idle. */
static int screen_tx_buffer = -1;
/** Next unsent byte within screen_frame_header. */
static size_t screen_tx_header_offset;
/** Next unsent byte within the packed decoded framebuffer. */
static size_t screen_tx_payload_offset;
/** Sequence represented by screen_tx_buffer. */
static uint32_t screen_tx_sequence;
/** Most recently completed USB frame sequence. */
static uint32_t screen_last_sequence;
/** Whether screen_last_sequence contains a completed frame. */
static bool screen_last_sequence_valid;
/** Whether the current screen session may send PackBits records. */
static bool screen_packbits_enabled;
/** Whether the frame in screen_tx_buffer selected PackBits encoding. */
static bool screen_tx_packbits;
/** Bytes in the encoded or raw payload currently being transmitted. */
static size_t screen_tx_payload_size;
/** Raw input position used by the streaming PackBits encoder. */
static size_t screen_tx_packbits_input_offset;
/** Aggregated PackBits runs submitted to TinyUSB in large writes. */
static uint8_t screen_tx_packbits_staging[SCREEN_PACKBITS_STAGING_SIZE];
/** Valid bytes in screen_tx_packbits_staging. */
static size_t screen_tx_packbits_staging_size;
/** Next unsent byte in screen_tx_packbits_staging. */
static size_t screen_tx_packbits_staging_offset;
/** CPU microseconds spent encoding the active frame into staging blocks. */
static uint32_t screen_tx_encode_us;
/** Timestamp at which the active record was prepared for transmission. */
static uint64_t screen_tx_started_us;
/** Complete screen frames queued to TinyUSB since boot. */
static uint32_t screen_frames_sent;
/** Raw fallback records queued since boot. */
static uint32_t screen_raw_frames_sent;
/** PackBits records queued since boot. */
static uint32_t screen_packbits_frames_sent;
/** Header and payload bytes queued since boot. */
static uint64_t screen_bytes_sent;
/** Payload bytes in the most recently completed record. */
static uint32_t screen_last_payload_size;
/** Most recent and maximum preparation durations. */
static uint32_t screen_last_prepare_us;
static uint32_t screen_maximum_prepare_us;
/** Most recent and maximum streaming-encoder CPU durations. */
static uint32_t screen_last_encode_us;
static uint32_t screen_maximum_encode_us;
/** Most recent and maximum end-to-end record queue durations. */
static uint32_t screen_last_tx_us;
static uint32_t screen_maximum_tx_us;

/** Refill the CDC transmit FIFO from both the main loop and long decodes. */
static void service_screen_output(void);
/** Flash slot holding the newest valid settings record, or -1 when absent. */
static int saved_settings_slot = -1;
/** Sequence number of the newest valid settings record. */
static uint32_t saved_settings_sequence;
/** Whether startup successfully restored a valid flash record. */
static bool restored_saved_settings;
/** Whether current runtime settings differ from the last saved/default state. */
static bool configuration_dirty;
/** Manual phase trim restored from the newest valid flash record. */
static int8_t saved_manual_phase_ticks;

/** Linker symbol marking the first byte after the flash-resident application. */
extern char __flash_binary_end;

/**
 * @brief Convert an RGB888 value to the adapter's RGB444 resistor-DAC format.
 *
 * @param rgb Color encoded as 0xRRGGBB.
 * @return Color encoded for the scanvideo RGB pins as BBBBGGGGRRRR.
 */
static uint16_t rgb888_to_scanvideo(uint32_t rgb) {
    const uint16_t red = (uint16_t)((rgb >> 20u) & 0x0fu);
    const uint16_t green = (uint16_t)((rgb >> 12u) & 0x0fu);
    const uint16_t blue = (uint16_t)((rgb >> 4u) & 0x0fu);
    return (uint16_t)(red | (green << 4u) | (blue << 8u));
}

/**
 * @brief Construct the factory display configuration.
 *
 * @return White foreground, black background, magenta solid border off, and
 *         native mode.
 */
static display_style_t default_display_style(void) {
    const display_style_t defaults = {
        .foreground_rgb = 0xffffffu,
        .background_rgb = 0x000000u,
        .border_rgb = 0xff00ffu,
        .border_enabled = false,
        .border_dotted = false,
        .vertical_stretch_enabled = false,
        .phosphor_noise_level = P2000M_PHOSPHOR_NOISE_OFF,
    };
    return defaults;
}

/**
 * @brief Calculate the standard reflected CRC-32 used by settings records.
 *
 * @param data Bytes to checksum.
 * @param length Number of bytes at data.
 * @return CRC-32 using polynomial 0xEDB88320 and standard inversion.
 */
static uint32_t settings_crc32(const void *data, size_t length) {
    // A 16-entry reflected CRC table is a useful middle ground on the Pico:
    // it removes the eight per-bit branches previously paid for every screen
    // byte while consuming only 64 bytes of flash-resident constant data.
    static const uint32_t nibble_table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
    }
    return ~crc;
}

/**
 * @brief Return the XIP address of one reserved settings slot.
 *
 * @param slot Slot index from zero through SETTINGS_SLOT_COUNT - 1.
 * @return Read-only memory-mapped flash record address.
 */
static const persisted_settings_t *settings_slot_record(unsigned slot) {
    hard_assert(slot < SETTINGS_SLOT_COUNT);
    const uintptr_t address = XIP_BASE + SETTINGS_FLASH_OFFSET +
        slot * FLASH_SECTOR_SIZE;
    return (const persisted_settings_t *)address;
}

/**
 * @brief Validate the signature, schema, values, and CRC of a flash record.
 *
 * @param record Memory-mapped settings record to validate.
 * @return true only when every structural and integrity check succeeds.
 */
static bool settings_record_is_valid(const persisted_settings_t *record) {
    return record->magic == SETTINGS_MAGIC &&
        record->version == SETTINGS_VERSION &&
        record->length == sizeof(*record) &&
        (record->foreground_rgb & 0xff000000u) == 0u &&
        (record->background_rgb & 0xff000000u) == 0u &&
        (record->border_rgb & 0xff000000u) == 0u &&
        (record->display_flags & ~(DISPLAY_FLAG_BORDER_ENABLED |
                                   DISPLAY_FLAG_BORDER_DOTTED |
                                   DISPLAY_FLAG_VERTICAL_STRETCH)) == 0u &&
        record->phosphor_noise_level <
            P2000M_PHOSPHOR_NOISE_LEVEL_COUNT &&
        record->manual_phase_ticks >= -4 &&
        record->manual_phase_ticks <= 4 &&
        (record->output_flags & ~(OUTPUT_FLAG_VGA_ENABLED |
                                  OUTPUT_FLAG_PAL_ENABLED)) == 0u &&
        record->checksum ==
            settings_crc32(record, offsetof(persisted_settings_t, checksum));
}

/** Validate a version-three flash record for settings migration. */
static bool settings_v3_record_is_valid(
    const persisted_settings_v3_t *record) {
    return record->magic == SETTINGS_MAGIC &&
        record->version == 3u &&
        record->length == sizeof(*record) &&
        (record->foreground_rgb & 0xff000000u) == 0u &&
        (record->background_rgb & 0xff000000u) == 0u &&
        (record->border_rgb & 0xff000000u) == 0u &&
        (record->display_flags & ~(DISPLAY_FLAG_BORDER_ENABLED |
                                   DISPLAY_FLAG_BORDER_DOTTED |
                                   DISPLAY_FLAG_VERTICAL_STRETCH)) == 0u &&
        record->phosphor_noise_level <
            P2000M_PHOSPHOR_NOISE_LEVEL_COUNT &&
        record->manual_phase_ticks >= -4 &&
        record->manual_phase_ticks <= 4 &&
        record->checksum == settings_crc32(
            record, offsetof(persisted_settings_v3_t, checksum));
}

/** Validate a version-two flash record for settings migration. */
static bool settings_v2_record_is_valid(
    const persisted_settings_v2_t *record) {
    return record->magic == SETTINGS_MAGIC &&
        record->version == 2u &&
        record->length == sizeof(*record) &&
        (record->foreground_rgb & 0xff000000u) == 0u &&
        (record->background_rgb & 0xff000000u) == 0u &&
        (record->border_rgb & 0xff000000u) == 0u &&
        record->border_enabled <= 1u &&
        record->border_dotted <= 1u &&
        record->vertical_stretch_enabled <= 1u &&
        record->manual_phase_ticks >= -4 &&
        record->manual_phase_ticks <= 4 &&
        record->checksum == settings_crc32(
            record, offsetof(persisted_settings_v2_t, checksum));
}

/**
 * @brief Validate a version-one flash record for settings migration.
 *
 * @param record Memory-mapped legacy record to validate.
 * @return true only when the complete legacy record is valid.
 */
static bool settings_v1_record_is_valid(
    const persisted_settings_v1_t *record) {
    return record->magic == SETTINGS_MAGIC &&
        record->version == 1u &&
        record->length == sizeof(*record) &&
        (record->foreground_rgb & 0xff000000u) == 0u &&
        (record->background_rgb & 0xff000000u) == 0u &&
        record->border_enabled <= 1u &&
        record->vertical_stretch_enabled <= 1u &&
        record->manual_phase_ticks >= -4 &&
        record->manual_phase_ticks <= 4 &&
        record->checksum == settings_crc32(
            record, offsetof(persisted_settings_v1_t, checksum));
}

/**
 * @brief Determine whether sequence a is newer than sequence b with wraparound.
 *
 * @param a Candidate sequence number.
 * @param b Reference sequence number.
 * @return true when a follows b within the unsigned 32-bit sequence space.
 */
static bool settings_sequence_is_newer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

/**
 * @brief Load the newest valid redundant flash record into runtime settings.
 *
 * The display fields are returned through style. The manual capture-phase trim
 * and flash bookkeeping are restored into their corresponding module globals.
 *
 * @param style Destination display style, unchanged without a valid record.
 * @param vga_enabled Destination VGA state, unchanged for legacy/no records.
 * @param pal_enabled Destination PAL state, unchanged for legacy/no records.
 * @return true when settings were restored; false when defaults should remain.
 */
static bool load_saved_configuration(display_style_t *style,
                                     bool *vga_enabled,
                                     bool *pal_enabled) {
    int newest_slot = -1;
    const persisted_settings_t *newest_record = NULL;
    for (unsigned slot = 0; slot < SETTINGS_SLOT_COUNT; ++slot) {
        const persisted_settings_t *candidate = settings_slot_record(slot);
        const bool valid = settings_record_is_valid(candidate) ||
            settings_v3_record_is_valid(
                (const persisted_settings_v3_t *)candidate) ||
            settings_v2_record_is_valid(
                (const persisted_settings_v2_t *)candidate) ||
            settings_v1_record_is_valid(
                (const persisted_settings_v1_t *)candidate);
        if (valid &&
            (newest_record == NULL ||
             settings_sequence_is_newer(candidate->sequence,
                                        newest_record->sequence))) {
            newest_slot = (int)slot;
            newest_record = candidate;
        }
    }
    if (newest_record == NULL) {
        return false;
    }

    style->foreground_rgb = newest_record->foreground_rgb;
    style->background_rgb = newest_record->background_rgb;
    if (newest_record->version == SETTINGS_VERSION ||
        newest_record->version == 3u) {
        style->border_rgb = newest_record->border_rgb;
        style->border_enabled =
            (newest_record->display_flags & DISPLAY_FLAG_BORDER_ENABLED) != 0u;
        style->border_dotted =
            (newest_record->display_flags & DISPLAY_FLAG_BORDER_DOTTED) != 0u;
        style->vertical_stretch_enabled =
            (newest_record->display_flags &
             DISPLAY_FLAG_VERTICAL_STRETCH) != 0u;
        style->phosphor_noise_level = newest_record->phosphor_noise_level;
        saved_manual_phase_ticks = newest_record->manual_phase_ticks;
        if (newest_record->version == SETTINGS_VERSION) {
            *vga_enabled =
                (newest_record->output_flags & OUTPUT_FLAG_VGA_ENABLED) != 0u;
            *pal_enabled =
                (newest_record->output_flags & OUTPUT_FLAG_PAL_ENABLED) != 0u;
        }
    } else if (newest_record->version == 2u) {
        const persisted_settings_v2_t *legacy =
            (const persisted_settings_v2_t *)newest_record;
        style->border_rgb = legacy->border_rgb;
        style->border_enabled = legacy->border_enabled != 0u;
        style->border_dotted = legacy->border_dotted != 0u;
        style->vertical_stretch_enabled =
            legacy->vertical_stretch_enabled != 0u;
        style->phosphor_noise_level = P2000M_PHOSPHOR_NOISE_OFF;
        saved_manual_phase_ticks = legacy->manual_phase_ticks;
    } else {
        const persisted_settings_v1_t *legacy =
            (const persisted_settings_v1_t *)newest_record;
        // A legacy border used the foreground color and was always solid.
        style->border_rgb = legacy->foreground_rgb;
        style->border_enabled = legacy->border_enabled != 0u;
        style->border_dotted = false;
        style->vertical_stretch_enabled =
            legacy->vertical_stretch_enabled != 0u;
        style->phosphor_noise_level = P2000M_PHOSPHOR_NOISE_OFF;
        saved_manual_phase_ticks = legacy->manual_phase_ticks;
    }
    saved_settings_slot = newest_slot;
    saved_settings_sequence = newest_record->sequence;
    return true;
}

/**
 * @brief Verify that the linked application does not overlap settings flash.
 *
 * @return Nothing; panics when the firmware image has grown into the reserved
 *         settings sectors.
 */
static void validate_settings_flash_region(void) {
    const uintptr_t binary_end_offset =
        (uintptr_t)&__flash_binary_end - XIP_BASE;
    if (binary_end_offset > SETTINGS_FLASH_OFFSET) {
        panic("Firmware overlaps reserved settings flash sectors");
    }
}

/**
 * @brief Build the fast byte-to-pixel table for one display style.
 *
 * @param style_index Destination style/table index in the double buffer.
 */
static void build_monochrome_lookup(unsigned style_index) {
    const display_style_t *style = &display_styles[style_index];
    const uint16_t foreground = rgb888_to_scanvideo(style->foreground_rgb);
    const uint16_t background = rgb888_to_scanvideo(style->background_rgb);
    for (unsigned value = 0; value < 256u; ++value) {
        for (unsigned bit = 0; bit < 8u; ++bit) {
            monochrome_pixels[style_index][value][bit] =
                (value & (0x80u >> bit)) != 0u ? foreground : background;
        }
    }
}

/**
 * @brief Initialize both display-style buffers from one startup configuration.
 *
 * @param initial_style Configuration loaded from flash or factory defaults.
 * @return Nothing.
 */
static void initialize_display_styles(const display_style_t *initial_style) {
    for (unsigned i = 0; i < DISPLAY_STYLE_COUNT; ++i) {
        display_styles[i] = *initial_style;
        build_monochrome_lookup(i);
    }
    requested_style_index = 0u;
    applied_style_index = 0u;
    displayed_style_index = 0u;
}

/**
 * @brief Publish new display settings for atomic adoption at the next frame.
 *
 * The function waits for any prior update to be accepted before rebuilding the
 * inactive lookup table. This prevents core 0 from modifying a table that core
 * 1 may still be reading.
 *
 * @param style Complete color, border, and vertical-scaling settings to apply.
 * @return Nothing.
 */
static void publish_display_style(const display_style_t *style) {
    if (__atomic_load_n(&vga_pause_applied, __ATOMIC_ACQUIRE)) {
        // Core 1 cannot adopt a pending style at a frame boundary while VGA
        // is stopped. Its pause acknowledgement makes both lookup buffers
        // safe to replace directly, ready for the next enable operation.
        for (unsigned i = 0u; i < DISPLAY_STYLE_COUNT; ++i) {
            display_styles[i] = *style;
            build_monochrome_lookup(i);
        }
        displayed_style_index = 0u;
        __atomic_store_n(&requested_style_index, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&applied_style_index, 0u, __ATOMIC_RELEASE);
        configuration_dirty = true;
        return;
    }

    while (__atomic_load_n(&requested_style_index, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&applied_style_index, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    const unsigned inactive =
        __atomic_load_n(&applied_style_index, __ATOMIC_RELAXED) ^ 1u;
    display_styles[inactive] = *style;
    build_monochrome_lookup(inactive);
    __atomic_store_n(&requested_style_index, inactive, __ATOMIC_RELEASE);
    configuration_dirty = true;
}

/**
 * @brief Initialize ownership metadata for the decoded frame buffers.
 *
 * @return Nothing.
 */
static void initialize_decoded_buffers(void) {
    decoded_lock = spin_lock_instance((unsigned)spin_lock_claim_unused(true));
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        decoded_states[i] = DECODED_FREE;
        decoded_sequences[i] = 0;
        decoded_usb_holds[i] = false;
        decoded_vga_holds[i] = false;
        decoded_pal_holds[i] = false;
    }
}

/**
 * @brief Claim the newest eligible decoded frame for immutable USB reading.
 *
 * READY frames may be held independently by VGA, PAL, and USB. Sequence
 * filtering limits a responsive host to every second decoded source frame
 * while still letting a slow host take the newest available image.
 *
 * @param sequence Receives the claimed source sequence number.
 * @return Buffer index, or -1 when no eligible complete frame exists.
 */
static int acquire_decoded_frame_for_usb(uint32_t *sequence) {
    const uint32_t saved = spin_lock_blocking(decoded_lock);
    int latest = -1;
    uint32_t latest_sequence = 0;

    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        if (decoded_states[i] == DECODED_READY && !decoded_usb_holds[i] &&
            (latest < 0 || decoded_sequences[i] >= latest_sequence)) {
            latest = (int)i;
            latest_sequence = decoded_sequences[i];
        }
    }

    if (latest >= 0 && screen_last_sequence_valid &&
        (int32_t)(latest_sequence - screen_last_sequence) <
            SCREEN_SEQUENCE_STEP) {
        latest = -1;
    }
    if (latest >= 0) {
        decoded_usb_holds[latest] = true;
        *sequence = latest_sequence;
    }
    spin_unlock(decoded_lock, saved);
    return latest;
}

/**
 * @brief Release a decoded framebuffer previously claimed by USB.
 *
 * @param buffer_index Valid decoded buffer index.
 */
static void release_decoded_frame_from_usb(unsigned buffer_index) {
    hard_assert(buffer_index < DECODED_BUFFER_COUNT);
    const uint32_t saved = spin_lock_blocking(decoded_lock);
    decoded_usb_holds[buffer_index] = false;
    spin_unlock(decoded_lock, saved);
}

/**
 * @brief Decode the newest complete oversampled capture into one-bit pixels.
 *
 * The newest raw buffer is held immutable while its 640 x 288 pixels are
 * resampled. The finished decoded frame is then published for core 1.
 *
 * @return true when a frame was decoded; false when no raw frame was ready.
 */
static bool decode_latest_source_frame(void) {
    const uint64_t decode_started = time_us_64();
    uint32_t sequence;
    const int raw_index = p2000m_capture_acquire_latest_frame(&sequence);
    if (raw_index < 0) {
        return false;
    }

    const uint32_t saved = spin_lock_blocking(decoded_lock);
    int decoded_index = -1;
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        const bool held = decoded_usb_holds[i] || decoded_vga_holds[i] ||
                          decoded_pal_holds[i];
        if (decoded_states[i] == DECODED_FREE && !held) {
            decoded_index = (int)i;
            break;
        }
    }
    if (decoded_index < 0) {
        uint32_t oldest_sequence = UINT32_MAX;
        for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
            const bool held = decoded_usb_holds[i] || decoded_vga_holds[i] ||
                              decoded_pal_holds[i];
            if (decoded_states[i] == DECODED_READY &&
                !held &&
                decoded_sequences[i] < oldest_sequence) {
                decoded_index = (int)i;
                oldest_sequence = decoded_sequences[i];
            }
        }
    }
    if (decoded_index < 0) {
        spin_unlock(decoded_lock, saved);
        p2000m_capture_release_frame((unsigned)raw_index);
        return false;
    }
    decoded_states[decoded_index] = DECODED_FILLING;
    spin_unlock(decoded_lock, saved);

    const uint32_t *raw = p2000m_capture_buffer((unsigned)raw_index);
    const p2000m_capture_pixel_map_t *map = p2000m_capture_pixel_map();
    uint32_t *decoded = decoded_frames[decoded_index];
    for (unsigned y = 0; y < P2000M_CAPTURE_HEIGHT; ++y) {
        const uint32_t *raw_line =
            raw + y * P2000M_CAPTURE_WORDS_PER_LINE;
        uint32_t output = 0;
        for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; ++x) {
            output = (output << 1) |
                (p2000m_capture_mapped_line_pixel_is_white(raw_line, map, x)
                     ? 1u
                     : 0u);
            if ((x & 31u) == 31u) {
                *decoded++ = output;
                output = 0;
            }
        }

        // Frame decoding can occupy core 0 for long enough to starve a USB
        // stream. Refill the asynchronously drained CDC FIFO at bounded
        // intervals without changing decoder or display-buffer ownership.
        if (usb_interface_mode == USB_SCREEN_MODE && (y & 7u) == 7u) {
            service_screen_output();
        }
    }
    p2000m_capture_release_frame((unsigned)raw_index);

    const uint32_t publish_saved = spin_lock_blocking(decoded_lock);
    decoded_sequences[decoded_index] = sequence;
    decoded_states[decoded_index] = DECODED_READY;
    spin_unlock(decoded_lock, publish_saved);

    const uint32_t decode_us = (uint32_t)(time_us_64() - decode_started);
    __atomic_store_n(&last_decode_us, decode_us, __ATOMIC_RELAXED);
    const uint32_t previous_maximum =
        __atomic_load_n(&maximum_decode_us, __ATOMIC_RELAXED);
    if (decode_us > previous_maximum) {
        __atomic_store_n(&maximum_decode_us, decode_us, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&decoded_source_frames, 1u, __ATOMIC_RELAXED);
    return true;
}

/**
 * @brief Select the newest decoded frame at the start of a VGA frame.
 *
 * Called only by core 1. If no new source is ready, the previous source frame
 * is deliberately repeated to bridge the asynchronous 50 Hz and 60 Hz rates.
 *
 * @return Nothing.
 */
static void select_frame_for_next_vga_frame(void) {
    ++generated_vga_frames;

    const uint32_t saved = spin_lock_blocking(decoded_lock);
    if (!displayed_signal_present) {
        if (displayed_buffer >= 0) {
            decoded_vga_holds[displayed_buffer] = false;
            displayed_buffer = -1;
        }
        ++blank_vga_frames;
        spin_unlock(decoded_lock, saved);
        return;
    }

    int next = -1;
    uint32_t sequence = 0;
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        const bool newer = displayed_buffer < 0 ||
            (int32_t)(decoded_sequences[i] - displayed_sequence) > 0;
        if (decoded_states[i] == DECODED_READY && newer &&
            (next < 0 || decoded_sequences[i] >= sequence)) {
            next = (int)i;
            sequence = decoded_sequences[i];
        }
    }

    if (next < 0) {
        if (displayed_buffer < 0) {
            ++blank_vga_frames;
        } else {
            ++repeated_vga_frames;
        }
        spin_unlock(decoded_lock, saved);
        return;
    }

    const int previous = displayed_buffer;
    displayed_buffer = next;
    displayed_sequence = sequence;
    decoded_vga_holds[next] = true;
    ++source_frame_swaps;

    if (previous >= 0 && previous != next) {
        decoded_vga_holds[previous] = false;
    }
    spin_unlock(decoded_lock, saved);
}

/**
 * @brief Select and retain the newest decoded source for one PAL field.
 *
 * PAL calls this only after the preceding field's final active scanline has
 * completed. VGA and USB may independently retain the same buffer.
 */
static const uint32_t *select_frame_for_next_pal_field(unsigned field,
                                                       uint32_t *sequence) {
    (void)field;
    const bool signal_present = p2000m_capture_signal_present();
    const uint32_t saved = spin_lock_blocking(decoded_lock);

    if (!signal_present) {
        if (pal_displayed_buffer >= 0) {
            decoded_pal_holds[pal_displayed_buffer] = false;
            pal_displayed_buffer = -1;
        }
        *sequence = pal_displayed_sequence;
        spin_unlock(decoded_lock, saved);
        return NULL;
    }

    int next = -1;
    uint32_t next_sequence = 0u;
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        const bool newer = pal_displayed_buffer < 0 ||
            (int32_t)(decoded_sequences[i] - pal_displayed_sequence) > 0;
        if (decoded_states[i] == DECODED_READY && newer &&
            (next < 0 || decoded_sequences[i] >= next_sequence)) {
            next = (int)i;
            next_sequence = decoded_sequences[i];
        }
    }

    if (next >= 0) {
        const int previous = pal_displayed_buffer;
        pal_displayed_buffer = next;
        pal_displayed_sequence = next_sequence;
        decoded_pal_holds[next] = true;
        if (previous >= 0 && previous != next) {
            decoded_pal_holds[previous] = false;
        }
    }

    *sequence = pal_displayed_sequence;
    const uint32_t *frame = pal_displayed_buffer >= 0
        ? decoded_frames[pal_displayed_buffer]
        : NULL;
    spin_unlock(decoded_lock, saved);
    return frame;
}

/** Release the decoded framebuffer retained by the stopped VGA output. */
static void release_vga_frame(void) {
    const uint32_t saved = spin_lock_blocking(decoded_lock);
    if (displayed_buffer >= 0) {
        decoded_vga_holds[displayed_buffer] = false;
        displayed_buffer = -1;
    }
    spin_unlock(decoded_lock, saved);
}

/** Release the decoded framebuffer retained by the stopped PAL output. */
static void release_pal_frame(void) {
    const uint32_t saved = spin_lock_blocking(decoded_lock);
    if (pal_displayed_buffer >= 0) {
        decoded_pal_holds[pal_displayed_buffer] = false;
        pal_displayed_buffer = -1;
    }
    spin_unlock(decoded_lock, saved);
}

/**
 * @brief Render a solid active VGA line and reset RGB before horizontal blank.
 *
 * @param scanline_buffer Scanvideo buffer that receives composable tokens.
 * @param color RGB444 color used for all 640 active pixels.
 * @return Nothing.
 */
static void render_solid_scanline(
    scanvideo_scanline_buffer_t *scanline_buffer, uint16_t color) {
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_COLOR_RUN;
    tokens[1] = color;
    tokens[2] = VGA_WIDTH - 3;
    tokens[3] = COMPOSABLE_RAW_1P;
    tokens[4] = 0x0000;
    tokens[5] = COMPOSABLE_EOL_ALIGN;
    scanline_buffer->data_used = 3;
    scanline_buffer->status = SCANLINE_OK;
}

/**
 * @brief Overlay one centered line of status text on a raw VGA scanline.
 *
 * @param tokens Raw composable-scanline token storage.
 * @param y Current active VGA line.
 * @param message Null-terminated message to draw.
 * @param top Top VGA coordinate of the text line.
 * @param scale Integer pixel enlargement applied to the 5 x 7 glyphs.
 * @param color RGB444 scanvideo color for lit glyph pixels.
 * @return Nothing.
 */
static void render_signal_lost_text(uint16_t *tokens, unsigned y,
                                    const char *message, unsigned top,
                                    unsigned scale, uint16_t color) {
    if (y < top ||
        y >= top + P2000M_SIGNAL_LOSS_GLYPH_HEIGHT * scale) {
        return;
    }

    const size_t length = strlen(message);
    if (length == 0u) {
        return;
    }
    const unsigned text_width =
        (unsigned)(((P2000M_SIGNAL_LOSS_GLYPH_WIDTH + 1u) * length - 1u) *
                   scale);
    const unsigned text_left = (VGA_WIDTH - text_width) / 2u;
    const unsigned font_row = (y - top) / scale;

    for (size_t character = 0u; character < length; ++character) {
        const unsigned character_x = text_left +
            (unsigned)character *
                (P2000M_SIGNAL_LOSS_GLYPH_WIDTH + 1u) * scale;
        const uint8_t row =
            p2000m_signal_loss_glyph_row(message[character], font_row);
        for (unsigned column = 0u;
             column < P2000M_SIGNAL_LOSS_GLYPH_WIDTH; ++column) {
            if ((row & (0x10u >> column)) == 0u) {
                continue;
            }
            const unsigned pixel_x = character_x + column * scale;
            for (unsigned offset = 0u; offset < scale; ++offset) {
                tokens[pixel_x + offset + 2u] = color;
            }
        }
    }
}

/**
 * @brief Render the fixed warning shown while source synchronization is lost.
 *
 * The warning deliberately ignores user colors and geometry so a black user
 * foreground/background combination cannot hide this operational state.
 *
 * @param scanline_buffer Scanvideo buffer that receives composable tokens.
 * @param y Zero-based active VGA line from 0 through 479.
 * @return Nothing.
 */
static void render_signal_lost_scanline(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned y) {
    const uint16_t canvas = rgb888_to_scanvideo(0x000000u);
    const uint16_t panel = rgb888_to_scanvideo(0x182028u);
    const uint16_t alert = rgb888_to_scanvideo(0xe03030u);
    const uint16_t text = rgb888_to_scanvideo(0xffffffu);
    const bool panel_line = y >= SIGNAL_LOST_PANEL_TOP &&
                            y < SIGNAL_LOST_PANEL_BOTTOM;
    const bool panel_horizontal_border = panel_line &&
        (y < SIGNAL_LOST_PANEL_TOP + SIGNAL_LOST_PANEL_BORDER ||
         y >= SIGNAL_LOST_PANEL_BOTTOM - SIGNAL_LOST_PANEL_BORDER);

    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_RAW_RUN;
    tokens[1] = canvas;
    tokens[2] = VGA_WIDTH - 3;
    for (unsigned x = 1u; x < VGA_WIDTH; ++x) {
        tokens[x + 2u] = canvas;
    }

    if (panel_line) {
        const uint16_t fill = panel_horizontal_border ? alert : panel;
        for (unsigned x = SIGNAL_LOST_PANEL_LEFT;
             x < SIGNAL_LOST_PANEL_RIGHT; ++x) {
            tokens[x + 2u] = fill;
        }
        if (!panel_horizontal_border) {
            for (unsigned offset = 0u;
                 offset < SIGNAL_LOST_PANEL_BORDER; ++offset) {
                tokens[SIGNAL_LOST_PANEL_LEFT + offset + 2u] = alert;
                tokens[SIGNAL_LOST_PANEL_RIGHT - offset + 1u] = alert;
            }
        }
    }

    render_signal_lost_text(tokens, y, P2000M_SIGNAL_LOSS_PRODUCT,
                            SIGNAL_LOST_PRODUCT_TOP,
                            SIGNAL_LOST_INFO_SCALE, text);
    render_signal_lost_text(tokens, y, P2000M_SIGNAL_LOSS_MESSAGE,
                            SIGNAL_LOST_MESSAGE_TOP,
                            SIGNAL_LOST_FONT_SCALE, text);
    render_signal_lost_text(tokens, y, signal_lost_firmware,
                            SIGNAL_LOST_FIRMWARE_TOP,
                            SIGNAL_LOST_INFO_SCALE, text);
    render_signal_lost_text(tokens, y, P2000M_SIGNAL_LOSS_WAITING,
                            SIGNAL_LOST_WAITING_TOP,
                            SIGNAL_LOST_INFO_SCALE, text);

    tokens[RAW_SCANLINE_TOKENS - 4] = COMPOSABLE_RAW_1P;
    tokens[RAW_SCANLINE_TOKENS - 3] = 0x0000;
    tokens[RAW_SCANLINE_TOKENS - 2] = COMPOSABLE_EOL_SKIP_ALIGN;
    tokens[RAW_SCANLINE_TOKENS - 1] = 0;
    scanline_buffer->data_used = RAW_SCANLINE_WORDS;
    scanline_buffer->status = SCANLINE_OK;
}

/**
 * @brief Map one active VGA line to the corresponding source-video line.
 *
 * Fit mode uses symmetric nearest-neighbour 5:3 scaling: each group of three
 * source lines becomes five output lines with a 2,1,2 repetition pattern.
 * Native mode retains the original 288 lines between 96-line margins.
 *
 * @param vga_y Zero-based active VGA line from 0 through 479.
 * @param stretch true for 5:3 fit mode; false for centered one-to-one mode.
 * @param source_y Receives the mapped source line when the function succeeds.
 * @param visible_y Receives the line coordinate within the displayed image.
 * @param visible_height Receives the displayed image height in VGA lines.
 * @return true for a source-image line; false for a native-mode margin line.
 */
static bool map_vga_line_to_source(unsigned vga_y, bool stretch,
                                   unsigned *source_y, unsigned *visible_y,
                                   unsigned *visible_height) {
    if (stretch) {
        *visible_y = vga_y;
        *visible_height = VGA_HEIGHT;
        *source_y = (vga_y * 3u + 1u) / 5u;
        return true;
    }

    if (vga_y < VGA_TOP_MARGIN || vga_y >= VGA_HEIGHT - VGA_BOTTOM_MARGIN) {
        return false;
    }
    *visible_y = vga_y - VGA_TOP_MARGIN;
    *visible_height = P2000M_CAPTURE_HEIGHT;
    *source_y = *visible_y;
    return true;
}

/**
 * @brief Dim a sparse, deterministic selection of lit scanline pixels.
 *
 * The source sequence, rather than the asynchronous VGA frame number, seeds
 * the pattern. A source frame repeated to bridge 50.1 Hz input to 60 Hz output
 * therefore remains visually stable. No background pixel is ever raised.
 *
 * @param tokens Completed raw scanvideo tokens for the active line.
 * @param line Packed one-bit source line, with foreground represented by one.
 * @param visible_y Output-line coordinate inside the visible source image.
 * @param level Selected P2000M_PHOSPHOR_NOISE_* density.
 * @param dimmed_foreground One-DAC-step-dimmer RGB444 foreground color.
 */
static void apply_phosphor_noise(uint16_t *tokens, const uint32_t *line,
                                 unsigned visible_y, uint8_t level,
                                 uint16_t dimmed_foreground) {
    uint32_t state = p2000m_phosphor_noise_seed(displayed_sequence,
                                                visible_y);
    for (unsigned word_index = 0;
         word_index < DECODED_WORDS_PER_LINE; ++word_index) {
        uint32_t selected = line[word_index] &
            p2000m_phosphor_noise_mask(level, &state);
        while (selected != 0u) {
            const unsigned source_bit = (unsigned)__builtin_ctz(selected);
            const unsigned x = word_index * 32u + (31u - source_bit);
            tokens[x == 0u ? 1u : x + 2u] = dimmed_foreground;
            selected &= selected - 1u;
        }
    }
}

/**
 * @brief Render one captured P2000M line using the active colors and border.
 *
 * @param scanline_buffer Scanvideo buffer that receives composable tokens.
 * @param source_y Zero-based source line in the 640 x 288 decoded frame.
 * @param visible_y Line coordinate within the displayed source image.
 * @param visible_height Height of the displayed source image in VGA lines.
 * @return Nothing.
 */
static void render_source_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                                   unsigned source_y,
                                   unsigned visible_y,
                                   unsigned visible_height) {
    const display_style_t *style = &display_styles[displayed_style_index];
    const bool horizontal_border = style->border_enabled &&
        (visible_y == 0u || visible_y + 1u == visible_height);
    const uint16_t border = rgb888_to_scanvideo(style->border_rgb);
    if (horizontal_border && !style->border_dotted) {
        render_solid_scanline(scanline_buffer, border);
        return;
    }

    const uint32_t *line =
        decoded_frames[displayed_buffer] +
        source_y * DECODED_WORDS_PER_LINE;
    uint16_t (*lookup)[8] = monochrome_pixels[displayed_style_index];
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;

    tokens[0] = COMPOSABLE_RAW_RUN;
    const uint8_t first_byte = (uint8_t)(line[0] >> 24u);
    tokens[1] = lookup[first_byte][0];
    tokens[2] = VGA_WIDTH - 3;

    uint16_t *destination = &tokens[3];
    for (unsigned bit = 1; bit < 8u; ++bit) {
        *destination++ = lookup[first_byte][bit];
    }
    for (unsigned word_index = 0;
         word_index < DECODED_WORDS_PER_LINE; ++word_index) {
        const uint32_t word = line[word_index];
        const unsigned first_shift = word_index == 0u ? 16u : 24u;
        for (int shift = (int)first_shift; shift >= 0; shift -= 8) {
            const uint16_t *pixels =
                lookup[(uint8_t)(word >> (unsigned)shift)];
            for (unsigned bit = 0; bit < 8u; ++bit) {
                *destination++ = pixels[bit];
            }
        }
    }

    if (style->phosphor_noise_level != P2000M_PHOSPHOR_NOISE_OFF) {
        const uint16_t foreground =
            rgb888_to_scanvideo(style->foreground_rgb);
        const uint16_t dimmed_foreground =
            p2000m_phosphor_noise_dim_rgb444(foreground);
        if (dimmed_foreground != foreground) {
            apply_phosphor_noise(tokens, line, visible_y,
                                 style->phosphor_noise_level,
                                 dimmed_foreground);
        }
    }

    if (style->border_enabled) {
        if (style->border_dotted) {
            if (horizontal_border) {
                // Two colored pixels followed by two untouched source pixels.
                for (unsigned x = 0; x < VGA_WIDTH; ++x) {
                    if ((x & 3u) < 2u) {
                        if (x == 0u) {
                            tokens[1] = border;
                        } else {
                            tokens[x + 2u] = border;
                        }
                    }
                }
            }
            // Always close the four corners even when a pattern ends in a gap.
            if (horizontal_border || (visible_y & 3u) < 2u) {
                tokens[1] = border;
                destination[-1] = border;
            }
        } else {
            tokens[1] = border;
            destination[-1] = border;
        }
    }

    // The 641st pixel is outside the active image and returns the resistor DAC
    // to black before synchronization, irrespective of the chosen background.
    tokens[RAW_SCANLINE_TOKENS - 4] = COMPOSABLE_RAW_1P;
    tokens[RAW_SCANLINE_TOKENS - 3] = 0x0000;
    tokens[RAW_SCANLINE_TOKENS - 2] = COMPOSABLE_EOL_SKIP_ALIGN;
    tokens[RAW_SCANLINE_TOKENS - 1] = 0;
    scanline_buffer->data_used = RAW_SCANLINE_WORDS;
    scanline_buffer->status = SCANLINE_OK;
}

/**
 * @brief Generate one VGA scanline on core 1.
 *
 * @param scanline_buffer Scanvideo buffer obtained for the requested line.
 * @return Nothing.
 */
static void __not_in_flash_func(render_scanline)(
    scanvideo_scanline_buffer_t *scanline_buffer) {
    const unsigned y = scanvideo_scanline_number(scanline_buffer->scanline_id);

    if (scanline_buffer->data_max < RAW_SCANLINE_WORDS) {
        scanline_buffer->data_used = 0;
        scanline_buffer->status = SCANLINE_ERROR;
        return;
    }

    if (y == 0u) {
        displayed_style_index =
            __atomic_load_n(&requested_style_index, __ATOMIC_ACQUIRE);
        __atomic_store_n(&applied_style_index, displayed_style_index,
                         __ATOMIC_RELEASE);
        displayed_signal_present = p2000m_capture_signal_present();
        select_frame_for_next_vga_frame();
    }

    if (!displayed_signal_present) {
        render_signal_lost_scanline(scanline_buffer, y);
        return;
    }

    const display_style_t *style = &display_styles[displayed_style_index];
    const uint16_t background = rgb888_to_scanvideo(style->background_rgb);
    unsigned source_y;
    unsigned visible_y;
    unsigned visible_height;
    if (displayed_buffer < 0 ||
        !map_vga_line_to_source(y, style->vertical_stretch_enabled,
                                &source_y, &visible_y, &visible_height)) {
        render_solid_scanline(scanline_buffer, background);
        return;
    }

    render_source_scanline(scanline_buffer, source_y, visible_y, visible_height);
}

/**
 * @brief Apply a pending PAL pause/resume request on its owning core.
 */
static void service_pal_pause_request(void) {
    const bool requested =
        __atomic_load_n(&pal_pause_requested, __ATOMIC_ACQUIRE);
    const bool applied = __atomic_load_n(&pal_pause_applied, __ATOMIC_RELAXED);
    if (requested == applied) {
        return;
    }
    if (requested) {
        pal_output_stop();
        release_pal_frame();
    } else {
        pal_output_start();
    }
    __atomic_store_n(&pal_pause_applied, requested, __ATOMIC_RELEASE);
}

/** Acknowledge VGA producer pausing only after core 1 is outside rendering. */
static void service_vga_pause_request(void) {
    const bool requested =
        __atomic_load_n(&vga_pause_requested, __ATOMIC_ACQUIRE);
    const bool applied =
        __atomic_load_n(&vga_pause_applied, __ATOMIC_RELAXED);
    if (requested == applied) {
        return;
    }
    if (requested) {
        release_vga_frame();
    }
    __atomic_store_n(&vga_pause_applied, requested, __ATOMIC_RELEASE);
}

/**
 * @brief Run the deadline-critical VGA and PAL producers on Pico core 1.
 *
 * @return Does not return.
 */
static void __not_in_flash_func(vga_core_main)(void) {
    // Scanvideo setup and timing IRQ ownership stay on core 0. Core 1 prepares
    // both VGA scanlines and the lower-rate PAL DMA ping-pong buffers.
    if (!flash_safe_execute_core_init()) {
        panic("Unable to initialize core 1 flash lockout");
    }
    pal_output_initialize(select_frame_for_next_pal_field);
    const bool pal_paused =
        __atomic_load_n(&pal_pause_requested, __ATOMIC_ACQUIRE);
    if (!pal_paused) {
        pal_output_start();
    }
    __atomic_store_n(&pal_pause_applied, pal_paused, __ATOMIC_RELEASE);
    const bool vga_paused =
        __atomic_load_n(&vga_pause_requested, __ATOMIC_ACQUIRE);
    __atomic_store_n(&vga_pause_applied, vga_paused, __ATOMIC_RELEASE);
    multicore_fifo_push_blocking(VGA_READY_MAGIC);

    uint32_t expected_scanline_id = 0u;
    bool expected_scanline_valid = false;
    while (true) {
        service_pal_pause_request();
        service_vga_pause_request();
        scanvideo_scanline_buffer_t *scanline_buffer = NULL;
        if (!__atomic_load_n(&vga_pause_applied, __ATOMIC_ACQUIRE)) {
            scanline_buffer = scanvideo_begin_scanline_generation(false);
        }
        if (scanline_buffer != NULL) {
            const uint32_t scanline_id = scanline_buffer->scanline_id;
            if (expected_scanline_valid && scanline_id != expected_scanline_id) {
                __atomic_fetch_add(&vga_scanline_gaps, 1u, __ATOMIC_RELAXED);
            }
            const uint16_t frame = scanvideo_frame_number(scanline_id);
            const uint16_t line = scanvideo_scanline_number(scanline_id);
            expected_scanline_id = line + 1u < VGA_HEIGHT
                                       ? ((uint32_t)frame << 16u) | (line + 1u)
                                       : (uint32_t)(uint16_t)(frame + 1u) << 16u;
            expected_scanline_valid = true;

            // A ready VGA slot has the tighter deadline. PAL still retains a
            // complete two-line DMA buffer while this one line is rendered.
            render_scanline(scanline_buffer);
            scanvideo_end_scanline_generation(scanline_buffer);
        }
        pal_output_service();
        if (scanline_buffer == NULL) {
            // Continue polling PAL while all VGA buffers are queued ahead.
            tight_loop_contents();
        }
    }
}

/**
 * @brief Print capture, decode, resampling, and VGA presentation statistics.
 *
 * @return Nothing.
 */
static void print_statistics(void) {
    p2000m_capture_stats_t capture;
    p2000m_capture_get_stats(&capture);
    pal_output_stats_t pal;
    pal_output_get_stats(&pal);

    const uint32_t vga_frames =
        __atomic_load_n(&generated_vga_frames, __ATOMIC_RELAXED);
    const uint32_t vga_gaps =
        __atomic_load_n(&vga_scanline_gaps, __ATOMIC_RELAXED);
    const uint32_t swaps =
        __atomic_load_n(&source_frame_swaps, __ATOMIC_RELAXED);
    const uint32_t repeats =
        __atomic_load_n(&repeated_vga_frames, __ATOMIC_RELAXED);
    const uint32_t blanks =
        __atomic_load_n(&blank_vga_frames, __ATOMIC_RELAXED);
    const uint32_t sequence =
        __atomic_load_n(&displayed_sequence, __ATOMIC_RELAXED);
    const uint32_t decoded =
        __atomic_load_n(&decoded_source_frames, __ATOMIC_RELAXED);
    const uint32_t decode_us =
        __atomic_load_n(&last_decode_us, __ATOMIC_RELAXED);
    const uint32_t decode_max_us =
        __atomic_load_n(&maximum_decode_us, __ATOMIC_RELAXED);

    printf("VID2VGA_USB experimental_overclock=yes sys_clock_khz=%u "
           "core_voltage_mv=%u screen_frames=%" PRIu32
           " screen_bytes=%llu raw_frames=%" PRIu32
           " packbits_frames=%" PRIu32 " payload_bytes=%" PRIu32
           " prepare_us=%" PRIu32 " prepare_max_us=%" PRIu32
           " encode_us=%" PRIu32 " encode_max_us=%" PRIu32
           " tx_us=%" PRIu32 " tx_max_us=%" PRIu32 "\n",
           SYSTEM_CLOCK_KHZ, SYSTEM_CORE_VOLTAGE_MV, screen_frames_sent,
           (unsigned long long)screen_bytes_sent, screen_raw_frames_sent,
           screen_packbits_frames_sent, screen_last_payload_size,
           screen_last_prepare_us, screen_maximum_prepare_us,
           screen_last_encode_us, screen_maximum_encode_us,
           screen_last_tx_us, screen_maximum_tx_us);

    printf("VID2PAL standard=625/50 sample_rate_hz=14000000 running=%s "
           "fields=%" PRIu32 " swaps=%" PRIu32 " repeats=%" PRIu32
           " blank=%" PRIu32 " underruns=%" PRIu32 " pauses=%" PRIu32
           " displayed_sequence=%" PRIu32 " output_line=%u\n",
           pal.running ? "yes" : "no", pal.generated_fields,
           pal.source_frame_swaps, pal.repeated_fields, pal.blank_fields,
           pal.dma_underruns, pal.pause_count, pal.displayed_sequence,
           (unsigned)pal.output_line);

    if (capture.last_frame_period_us == 0) {
        printf("VID2VGA capture_frames=%" PRIu32
               " waiting_for_input vga_frames=%" PRIu32
               " vga_scanline_gaps=%" PRIu32
               " swaps=%" PRIu32 " repeats=%" PRIu32
               " blank=%" PRIu32 " displayed_sequence=%" PRIu32
               " decoded_frames=%" PRIu32 " decode_us=%" PRIu32
               " decode_max_us=%" PRIu32
               " screen_frames=%" PRIu32
               " auto_phase_ticks=%" PRId32
               " manual_trim_ticks=%" PRId32 "\n",
               capture.captured_frames, vga_frames, vga_gaps, swaps, repeats,
               blanks, sequence, decoded, decode_us, decode_max_us,
               screen_frames_sent,
               capture.auto_phase_ticks,
               capture.manual_phase_ticks);
        return;
    }

    const uint32_t rate_millihz = 1000000000u / capture.last_frame_period_us;
    printf("VID2VGA capture_frames=%" PRIu32 " input_period_us=%" PRIu32
           " input_rate=%" PRIu32 ".%03" PRIu32
           "Hz locked=%s stale_replaced=%" PRIu32
           " vga_frames=%" PRIu32 " vga_scanline_gaps=%" PRIu32
           " swaps=%" PRIu32
           " repeats=%" PRIu32 " blank=%" PRIu32
           " displayed_sequence=%" PRIu32
           " decoded_frames=%" PRIu32 " decode_us=%" PRIu32
           " decode_max_us=%" PRIu32
           " screen_frames=%" PRIu32
           " line_ticks=%" PRIu32 ".%03" PRIu32
           " auto_phase_ticks=%" PRId32
           " manual_trim_ticks=%" PRId32
           " autotune_runs=%" PRIu32 " tune_score=%" PRIu32
           " tune_us=%" PRIu32 " tune_max_us=%" PRIu32 "\n",
           capture.captured_frames, capture.last_frame_period_us,
           rate_millihz / 1000u, rate_millihz % 1000u,
           capture.signal_present ? "yes" : "no",
           capture.stale_frames_replaced,
           vga_frames, vga_gaps, swaps, repeats, blanks, sequence,
           decoded, decode_us, decode_max_us, screen_frames_sent,
           capture.recovered_line_ticks_q16 >> 16,
           ((capture.recovered_line_ticks_q16 & 0xffffu) * 1000u) >> 16,
           capture.auto_phase_ticks, capture.manual_phase_ticks,
           capture.autotune_runs, capture.autotune_score,
           capture.last_autotune_us, capture.maximum_autotune_us);
}

/**
 * @brief Print white-pixel totals for every character row and column.
 *
 * The diagnostic helps verify that all 80 x 24 character cells reach the
 * resampler, including the outermost row and column.
 *
 * @return Nothing.
 */
static void print_source_geometry(void) {
    enum {
        CHARACTER_COLUMNS = 80,
        CHARACTER_ROWS = 24,
        CHARACTER_WIDTH = 8,
        CHARACTER_HEIGHT = 12,
    };

    uint32_t sequence;
    const int buffer_index = p2000m_capture_acquire_latest_frame(&sequence);
    if (buffer_index < 0) {
        printf("No unclaimed completed frame is available; try g again.\n");
        return;
    }

    uint32_t row_white_pixels[CHARACTER_ROWS] = {0};
    uint32_t column_white_pixels[CHARACTER_COLUMNS] = {0};
    const uint32_t *frame = p2000m_capture_buffer((unsigned)buffer_index);

    for (unsigned y = 0; y < P2000M_CAPTURE_HEIGHT; ++y) {
        for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; ++x) {
            if (p2000m_capture_pixel_is_white(frame, x, y)) {
                ++row_white_pixels[y / CHARACTER_HEIGHT];
                ++column_white_pixels[x / CHARACTER_WIDTH];
            }
        }
    }

    printf("GEOMETRY sequence=%" PRIu32
           " values=white_pixels_per_character_band\n", sequence);
    printf("ROWS");
    for (unsigned row = 0; row < CHARACTER_ROWS; ++row) {
        printf(" R%02u=%" PRIu32, row + 1u, row_white_pixels[row]);
        if ((row + 1u) % 8u == 0u && row + 1u < CHARACTER_ROWS) {
            printf("\n    ");
        }
    }
    printf("\nCOLUMNS");
    for (unsigned column = 0; column < CHARACTER_COLUMNS; ++column) {
        printf(" C%02u=%" PRIu32, column + 1u, column_white_pixels[column]);
        if ((column + 1u) % 8u == 0u && column + 1u < CHARACTER_COLUMNS) {
            printf("\n       ");
        }
    }
    printf("\nBOUNDARIES top=%" PRIu32 " bottom=%" PRIu32
           " left=%" PRIu32 " right=%" PRIu32 "\n",
           row_white_pixels[0], row_white_pixels[CHARACTER_ROWS - 1u],
           column_white_pixels[0], column_white_pixels[CHARACTER_COLUMNS - 1u]);

    p2000m_capture_release_frame((unsigned)buffer_index);
}

/**
 * @brief Force a resampler tune and print the five candidate phase scores.
 *
 * @return Nothing.
 */
static void print_sample_diagnostics(void) {
    p2000m_capture_tuning_report_t report;
    if (!p2000m_capture_autotune(&report)) {
        printf("No completed locked frame is available; try j again.\n");
        return;
    }

    p2000m_capture_stats_t stats;
    p2000m_capture_get_stats(&stats);
    printf("RESAMPLER sequence=%" PRIu32
           " sample_clock_hz=%u line_ticks=%" PRIu32 ".%03" PRIu32
           " selected_phase_tick=%" PRId32
           " manual_trim_tick=%" PRId32 "\n",
           report.sequence, P2000M_CAPTURE_SAMPLE_CLOCK_HZ,
           stats.recovered_line_ticks_q16 >> 16,
           ((stats.recovered_line_ticks_q16 & 0xffffu) * 1000u) >> 16,
           report.selected_phase_tick, stats.manual_phase_ticks);
    printf("PHASE_SCORES values=white_centre_samples");
    for (unsigned i = 0; i < P2000M_CAPTURE_TUNING_CANDIDATES; ++i) {
        printf(" T%02" PRId32 "=%" PRIu32,
               report.first_candidate_tick + (int32_t)i,
               report.scores[i]);
    }
    printf("\nEND_RESAMPLER\n");
}

/**
 * @brief Apply or reset the manual phase trim layered over automatic tuning.
 *
 * @param change Signed number of 63 MHz capture ticks to add.
 * @param reset When true, ignore change and restore zero manual trim.
 * @return Nothing.
 */
static void adjust_sample_phase(int change, bool reset) {
    p2000m_capture_stats_t stats;
    p2000m_capture_get_stats(&stats);
    const int requested = reset ? 0 : stats.manual_phase_ticks + change;
    if (!p2000m_capture_set_sample_phase(requested)) {
        printf("Manual trim limit reached (-4 through +4 capture ticks).\n");
        return;
    }
    configuration_dirty = true;
    printf("Manual resampler trim is now %d tick(s), 15.87 ns each (%s).\n",
           requested,
           requested > 0 ? "later" : requested < 0 ? "earlier" : "automatic");
}

/**
 * @brief Convert one hexadecimal character to its numeric value.
 *
 * @param character ASCII hexadecimal digit.
 * @return Value from 0 through 15, or -1 when character is not hexadecimal.
 */
static int hexadecimal_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

/**
 * @brief Normalize a USB command to trimmed, lowercase ASCII.
 *
 * @param command Mutable, null-terminated command line.
 * @return Pointer to the first non-space character within command.
 */
static char *normalize_command(char *command) {
    while (*command == ' ' || *command == '\t') {
        ++command;
    }

    char *end = command + strlen(command);
    while (end > command && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    for (char *cursor = command; *cursor != '\0'; ++cursor) {
        if (*cursor >= 'A' && *cursor <= 'Z') {
            *cursor = (char)(*cursor - 'A' + 'a');
        }
    }
    return command;
}

/**
 * @brief Split a normalized command into its verb and optional argument.
 *
 * @param command Mutable command string; its first separator is replaced by
 *                a null terminator.
 * @return Pointer to the trimmed argument, or an empty string when absent.
 */
static char *split_command_argument(char *command) {
    char *argument = command;
    while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
        ++argument;
    }
    if (*argument == '\0') {
        return argument;
    }

    *argument++ = '\0';
    while (*argument == ' ' || *argument == '\t') {
        ++argument;
    }
    return argument;
}

/**
 * @brief Parse a named preset or six-digit RGB value.
 *
 * Hex values may be written as RRGGBB, #RRGGBB, or 0xRRGGBB. Input is
 * expected to have been normalized to lowercase.
 *
 * @param text Color name or hexadecimal value.
 * @param rgb Receives the parsed 0xRRGGBB value on success.
 * @return true when text is valid; false otherwise.
 */
static bool parse_color(const char *text, uint32_t *rgb) {
    for (unsigned i = 0;
         i < sizeof(color_presets) / sizeof(color_presets[0]); ++i) {
        if (strcmp(text, color_presets[i].name) == 0) {
            *rgb = color_presets[i].rgb;
            return true;
        }
    }

    if (*text == '#') {
        ++text;
    } else if (text[0] == '0' && text[1] == 'x') {
        text += 2;
    }
    if (strlen(text) != 6u) {
        return false;
    }

    uint32_t value = 0;
    for (unsigned i = 0; i < 6u; ++i) {
        const int digit = hexadecimal_value(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4u) | (uint32_t)digit;
    }
    *rgb = value;
    return true;
}

/**
 * @brief Return a stable copy of the most recently requested display style.
 *
 * @return Foreground, background, and border settings pending or displayed.
 */
static display_style_t current_display_style(void) {
    const unsigned index =
        __atomic_load_n(&requested_style_index, __ATOMIC_ACQUIRE);
    return display_styles[index];
}

/** Store an unsigned 16-bit value in the screen protocol's little-endian form. */
static void screen_store_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

/** Store an unsigned 32-bit value in the screen protocol's little-endian form. */
static void screen_store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

/**
 * @brief Stop an in-progress screen transfer and release its framebuffer.
 */
static void abort_screen_transfer(void) {
    if (screen_tx_buffer >= 0) {
        release_decoded_frame_from_usb((unsigned)screen_tx_buffer);
    }
    screen_tx_buffer = -1;
    screen_tx_header_offset = 0u;
    screen_tx_payload_offset = 0u;
    screen_tx_payload_size = 0u;
    screen_tx_packbits = false;
    screen_tx_packbits_input_offset = 0u;
    screen_tx_packbits_staging_size = 0u;
    screen_tx_packbits_staging_offset = 0u;
    screen_tx_encode_us = 0u;
    screen_tx_started_us = 0u;
}

/**
 * @brief Assemble a stable version-one binary header for one decoded frame.
 *
 * Type one carries the RP2350's native little-endian array of 32-bit words;
 * type two independently PackBits-encodes those same bytes. In each numeric
 * word, the leftmost pixel is bit 31 and foreground is one. Display colors and
 * visual-style flags let the host reproduce the VGA view.
 */
static void build_screen_frame_header(uint32_t sequence,
                                      const uint32_t *frame) {
    enum {
        PAYLOAD_WORDS_LITTLE_ENDIAN = 1u << 0,
        PAYLOAD_PIXELS_MSB_FIRST = 1u << 1,
        PAYLOAD_TIMING_DIAGNOSTICS = 1u << 2,
        STYLE_BORDER_ENABLED = 1u << 0,
        STYLE_BORDER_DOTTED = 1u << 1,
        STYLE_VERTICAL_STRETCH = 1u << 2,
        STYLE_PHOSPHOR_NOISE_SHIFT = 3,
    };
    const uint64_t prepare_started = time_us_64();
    const display_style_t style = current_display_style();
    const uint8_t *frame_bytes = (const uint8_t *)frame;
    const size_t packbits_size = screen_packbits_enabled
                                     ? p2000m_packbits_encoded_size(
                                           frame_bytes,
                                           SCREEN_FRAME_PAYLOAD_SIZE)
                                     : SCREEN_FRAME_PAYLOAD_SIZE;
    screen_tx_packbits = packbits_size < SCREEN_FRAME_PAYLOAD_SIZE;
    screen_tx_payload_size = screen_tx_packbits
                                 ? packbits_size
                                 : SCREEN_FRAME_PAYLOAD_SIZE;
    const uint32_t checksum =
        settings_crc32(frame, SCREEN_FRAME_PAYLOAD_SIZE);
    screen_last_prepare_us =
        (uint32_t)(time_us_64() - prepare_started);
    if (screen_last_prepare_us > screen_maximum_prepare_us) {
        screen_maximum_prepare_us = screen_last_prepare_us;
    }
    uint32_t style_flags = 0u;
    if (style.border_enabled) {
        style_flags |= STYLE_BORDER_ENABLED;
    }
    if (style.border_dotted) {
        style_flags |= STYLE_BORDER_DOTTED;
    }
    if (style.vertical_stretch_enabled) {
        style_flags |= STYLE_VERTICAL_STRETCH;
    }
    style_flags |=
        (uint32_t)style.phosphor_noise_level << STYLE_PHOSPHOR_NOISE_SHIFT;

    memset(screen_frame_header, 0, sizeof(screen_frame_header));
    memcpy(screen_frame_header, "P2VF", 4u);
    // Protocol version one is retained for raw-record compatibility.
    screen_frame_header[4] = 1u;
    // Both record types represent one independent complete framebuffer.
    screen_frame_header[5] = screen_tx_packbits ? 2u : 1u;
    screen_store_u16(&screen_frame_header[6],
                     PAYLOAD_WORDS_LITTLE_ENDIAN |
                         PAYLOAD_PIXELS_MSB_FIRST |
                         PAYLOAD_TIMING_DIAGNOSTICS);
    screen_store_u32(&screen_frame_header[8], sequence);
    const uint32_t timing_diagnostics =
        (screen_last_prepare_us > UINT16_MAX
             ? UINT16_MAX
             : screen_last_prepare_us) |
        ((screen_last_encode_us > UINT16_MAX
              ? UINT16_MAX
              : screen_last_encode_us)
         << 16u);
    screen_store_u32(&screen_frame_header[12], timing_diagnostics);
    screen_store_u16(&screen_frame_header[16], P2000M_CAPTURE_WIDTH);
    screen_store_u16(&screen_frame_header[18], P2000M_CAPTURE_HEIGHT);
    screen_store_u16(&screen_frame_header[20],
                     P2000M_CAPTURE_WIDTH / 8u);
    screen_store_u16(&screen_frame_header[22], SCREEN_FRAME_HEADER_SIZE);
    screen_store_u32(&screen_frame_header[24],
                     (uint32_t)screen_tx_payload_size);
    screen_store_u32(&screen_frame_header[28], checksum);
    screen_store_u32(&screen_frame_header[32], style.foreground_rgb);
    screen_store_u32(&screen_frame_header[36], style.background_rgb);
    screen_store_u32(&screen_frame_header[40], style.border_rgb);
    screen_store_u32(&screen_frame_header[44], style_flags);
}

/**
 * @brief Claim the newest eligible frame when the transmitter is idle.
 */
static void begin_available_screen_frame(void) {
    if (screen_tx_buffer >= 0) {
        return;
    }

    uint32_t sequence = 0u;
    const int buffer_index = acquire_decoded_frame_for_usb(&sequence);
    if (buffer_index < 0) {
        return;
    }

    screen_tx_buffer = buffer_index;
    screen_tx_sequence = sequence;
    screen_tx_header_offset = 0u;
    screen_tx_payload_offset = 0u;
    screen_tx_packbits_input_offset = 0u;
    screen_tx_packbits_staging_size = 0u;
    screen_tx_packbits_staging_offset = 0u;
    screen_tx_encode_us = 0u;
    build_screen_frame_header(
        sequence, decoded_frames[(unsigned)buffer_index]);
    screen_tx_started_us = time_us_64();
}

/** Aggregate PackBits runs into one large TinyUSB source buffer. */
static void fill_screen_packbits_staging(void) {
    screen_tx_packbits_staging_size = 0u;
    screen_tx_packbits_staging_offset = 0u;
    const uint64_t encode_started = time_us_64();
    while (screen_tx_packbits_input_offset < SCREEN_FRAME_PAYLOAD_SIZE &&
           SCREEN_PACKBITS_STAGING_SIZE - screen_tx_packbits_staging_size >=
               P2000M_PACKBITS_MAX_CHUNK) {
        const size_t chunk_size = p2000m_packbits_next_chunk(
            (const uint8_t *)decoded_frames[screen_tx_buffer],
            SCREEN_FRAME_PAYLOAD_SIZE,
            &screen_tx_packbits_input_offset,
            &screen_tx_packbits_staging[screen_tx_packbits_staging_size]);
        hard_assert(chunk_size != 0u);
        screen_tx_packbits_staging_size += chunk_size;
    }
    screen_tx_encode_us += (uint32_t)(time_us_64() - encode_started);
    hard_assert(screen_tx_packbits_staging_size != 0u);
}

/**
 * @brief Feed a bounded part of the current binary frame to TinyUSB.
 *
 * The function never waits for FIFO capacity. A host which stops reading merely
 * stops progress until it disconnects or requests console mode; VGA and capture
 * continue independently.
 */
static void service_screen_output(void) {
    if (usb_interface_mode != USB_SCREEN_MODE || !stdio_usb_connected()) {
        abort_screen_transfer();
        return;
    }

    begin_available_screen_frame();
    if (screen_tx_buffer < 0) {
        return;
    }

    size_t budget = SCREEN_TX_SERVICE_BUDGET;
    while (budget != 0u) {
        const uint32_t available = tud_cdc_write_available();
        if (available == 0u) {
            break;
        }

        const uint8_t *source;
        size_t remaining;
        if (screen_tx_header_offset < SCREEN_FRAME_HEADER_SIZE) {
            source = &screen_frame_header[screen_tx_header_offset];
            remaining = SCREEN_FRAME_HEADER_SIZE - screen_tx_header_offset;
        } else if (screen_tx_packbits) {
            if (screen_tx_packbits_staging_offset ==
                screen_tx_packbits_staging_size) {
                fill_screen_packbits_staging();
            }
            source = &screen_tx_packbits_staging[
                screen_tx_packbits_staging_offset];
            remaining = screen_tx_packbits_staging_size -
                        screen_tx_packbits_staging_offset;
        } else {
            source = (const uint8_t *)decoded_frames[screen_tx_buffer] +
                     screen_tx_payload_offset;
            remaining = screen_tx_payload_size - screen_tx_payload_offset;
        }

        size_t count = remaining;
        if (count > available) {
            count = available;
        }
        if (count > budget) {
            count = budget;
        }
        const uint32_t written = tud_cdc_write(source, (uint32_t)count);
        if (written == 0u) {
            break;
        }
        budget -= written;

        if (screen_tx_header_offset < SCREEN_FRAME_HEADER_SIZE) {
            screen_tx_header_offset += written;
        } else {
            screen_tx_payload_offset += written;
            if (screen_tx_packbits) {
                screen_tx_packbits_staging_offset += written;
            }
        }

        if (screen_tx_header_offset == SCREEN_FRAME_HEADER_SIZE &&
            screen_tx_payload_offset == screen_tx_payload_size) {
            release_decoded_frame_from_usb((unsigned)screen_tx_buffer);
            screen_tx_buffer = -1;
            screen_last_sequence = screen_tx_sequence;
            screen_last_sequence_valid = true;
            ++screen_frames_sent;
            screen_bytes_sent += SCREEN_FRAME_HEADER_SIZE +
                                 screen_tx_payload_size;
            screen_last_payload_size = (uint32_t)screen_tx_payload_size;
            if (screen_tx_packbits) {
                ++screen_packbits_frames_sent;
            } else {
                ++screen_raw_frames_sent;
            }
            screen_last_encode_us = screen_tx_encode_us;
            if (screen_last_encode_us > screen_maximum_encode_us) {
                screen_maximum_encode_us = screen_last_encode_us;
            }
            screen_last_tx_us =
                (uint32_t)(time_us_64() - screen_tx_started_us);
            if (screen_last_tx_us > screen_maximum_tx_us) {
                screen_maximum_tx_us = screen_last_tx_us;
            }
            if (budget == 0u) {
                break;
            }
            // Continue directly into the newest eligible frame when USB FIFO
            // capacity remains. TinyUSB itself supplies backpressure if the
            // host stops reading, so no per-frame command round trip is needed.
            begin_available_screen_frame();
            if (screen_tx_buffer < 0) {
                break;
            }
        }
    }
    tud_cdc_write_flush();
}

/**
 * @brief Erase sectors and optionally program one settings page while XIP-safe.
 *
 * This callback executes from SRAM under flash_safe_execute(), with interrupts
 * disabled and core 1 held in the multicore lockout handler.
 *
 * @param parameter Pointer to a RAM-resident flash_mutation_t operation.
 * @return Nothing.
 */
static void __not_in_flash_func(perform_settings_flash_mutation)(
    void *parameter) {
    const flash_mutation_t *mutation = (const flash_mutation_t *)parameter;
    flash_range_erase(mutation->offset, mutation->erase_size);
    if (mutation->program_page) {
        flash_range_program(mutation->offset, mutation->page, FLASH_PAGE_SIZE);
    }
}

/** Wait for core 1 to acknowledge a requested PAL run state. */
static bool request_pal_pause(bool paused) {
    __atomic_store_n(&pal_pause_requested, paused, __ATOMIC_RELEASE);
    const uint64_t deadline =
        time_us_64() + (uint64_t)FLASH_LOCKOUT_TIMEOUT_MS * 1000u;
    while (__atomic_load_n(&pal_pause_applied, __ATOMIC_ACQUIRE) != paused) {
        if (time_us_64() >= deadline) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

/** Wait for core 1 to stop or resume VGA scanline production. */
static bool request_vga_pause(bool paused) {
    __atomic_store_n(&vga_pause_requested, paused, __ATOMIC_RELEASE);
    const uint64_t deadline =
        time_us_64() + (uint64_t)FLASH_LOCKOUT_TIMEOUT_MS * 1000u;
    while (__atomic_load_n(&vga_pause_applied, __ATOMIC_ACQUIRE) != paused) {
        if (time_us_64() >= deadline) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

/** Return whether the physical VGA timing generator is requested on. */
static bool vga_output_is_enabled(void) {
    return !__atomic_load_n(&vga_pause_requested, __ATOMIC_ACQUIRE);
}

/** Return whether the PAL DMA/PIO stream is requested on. */
static bool pal_output_is_enabled(void) {
    return !__atomic_load_n(&pal_pause_requested, __ATOMIC_ACQUIRE);
}

/** Enable or disable VGA timing while safely coordinating its producer. */
static bool set_vga_output_enabled(bool enabled) {
    if (vga_output_is_enabled() == enabled) {
        return true;
    }

    if (enabled) {
        if (!request_vga_pause(false)) {
            return false;
        }
        scanvideo_timing_enable(true);
    } else {
        if (!request_vga_pause(true)) {
            return false;
        }
        scanvideo_timing_enable(false);
    }
    configuration_dirty = true;
    return true;
}

/** Enable or disable the PAL stream on its owning core. */
static bool set_pal_output_enabled(bool enabled) {
    if (pal_output_is_enabled() == enabled) {
        return true;
    }
    if (!request_pal_pause(!enabled)) {
        return false;
    }
    configuration_dirty = true;
    return true;
}

/** Pause PAL before a flash lockout and resume it after the mutation. */
static int execute_settings_flash_mutation(flash_mutation_t *mutation) {
    const bool was_paused =
        __atomic_load_n(&pal_pause_requested, __ATOMIC_ACQUIRE);
    if (!request_pal_pause(true)) {
        (void)request_pal_pause(was_paused);
        return PICO_ERROR_TIMEOUT;
    }

    const int result = flash_safe_execute(perform_settings_flash_mutation,
                                          mutation,
                                          FLASH_LOCKOUT_TIMEOUT_MS);

    if (!request_pal_pause(was_paused)) {
        return PICO_ERROR_TIMEOUT;
    }
    return result;
}

/**
 * @brief Persist display settings and manual phase trim to the inactive slot.
 *
 * The old valid slot is retained until the new sector has been erased,
 * programmed, and verified, so loss of power during a save cannot corrupt both
 * copies.
 *
 * @return PICO_OK on verified success or a Pico error code on failure.
 */
static int save_current_configuration(void) {
    const display_style_t style = current_display_style();
    p2000m_capture_stats_t capture;
    p2000m_capture_get_stats(&capture);
    uint8_t display_flags = 0u;
    if (style.border_enabled) {
        display_flags |= DISPLAY_FLAG_BORDER_ENABLED;
    }
    if (style.border_dotted) {
        display_flags |= DISPLAY_FLAG_BORDER_DOTTED;
    }
    if (style.vertical_stretch_enabled) {
        display_flags |= DISPLAY_FLAG_VERTICAL_STRETCH;
    }
    uint8_t output_flags = 0u;
    if (vga_output_is_enabled()) {
        output_flags |= OUTPUT_FLAG_VGA_ENABLED;
    }
    if (pal_output_is_enabled()) {
        output_flags |= OUTPUT_FLAG_PAL_ENABLED;
    }
    const unsigned target_slot = saved_settings_slot < 0
                                     ? 0u
                                     : ((unsigned)saved_settings_slot ^ 1u);
    persisted_settings_t record = {
        .magic = SETTINGS_MAGIC,
        .version = SETTINGS_VERSION,
        .length = sizeof(persisted_settings_t),
        .sequence = saved_settings_sequence + 1u,
        .foreground_rgb = style.foreground_rgb,
        .background_rgb = style.background_rgb,
        .border_rgb = style.border_rgb,
        .display_flags = display_flags,
        .phosphor_noise_level = style.phosphor_noise_level,
        .manual_phase_ticks = (int8_t)capture.manual_phase_ticks,
        .output_flags = output_flags,
        .checksum = 0u,
    };
    record.checksum =
        settings_crc32(&record, offsetof(persisted_settings_t, checksum));

    flash_mutation_t mutation;
    memset(&mutation, 0xff, sizeof(mutation));
    mutation.offset = SETTINGS_FLASH_OFFSET + target_slot * FLASH_SECTOR_SIZE;
    mutation.erase_size = FLASH_SECTOR_SIZE;
    mutation.program_page = true;
    memcpy(mutation.page, &record, sizeof(record));

    const int result = execute_settings_flash_mutation(&mutation);
    if (result != PICO_OK) {
        return result;
    }

    const persisted_settings_t *saved = settings_slot_record(target_slot);
    if (!settings_record_is_valid(saved) ||
        saved->sequence != record.sequence) {
        return PICO_ERROR_GENERIC;
    }
    saved_settings_slot = (int)target_slot;
    saved_settings_sequence = record.sequence;
    saved_manual_phase_ticks = record.manual_phase_ticks;
    restored_saved_settings = true;
    configuration_dirty = false;
    return PICO_OK;
}

/**
 * @brief Erase both redundant settings sectors safely.
 *
 * @return PICO_OK on success or a Pico error code when lockout/erase fails.
 */
static int erase_saved_configuration(void) {
    flash_mutation_t mutation;
    memset(&mutation, 0xff, sizeof(mutation));
    mutation.offset = SETTINGS_FLASH_OFFSET;
    mutation.erase_size = SETTINGS_SLOT_COUNT * FLASH_SECTOR_SIZE;
    mutation.program_page = false;

    const int result = execute_settings_flash_mutation(&mutation);
    if (result == PICO_OK) {
        saved_settings_slot = -1;
        saved_settings_sequence = 0u;
        restored_saved_settings = false;
        saved_manual_phase_ticks = 0;
    }
    return result;
}

/** Return the stable console name for one phosphor-noise level. */
static const char *phosphor_noise_level_name(uint8_t level) {
    switch (level) {
        case P2000M_PHOSPHOR_NOISE_LOW:
            return "low";
        case P2000M_PHOSPHOR_NOISE_MEDIUM:
            return "medium";
        case P2000M_PHOSPHOR_NOISE_HIGH:
            return "high";
        default:
            return "off";
    }
}

/** Parse an off/low/medium/high phosphor-noise command argument. */
static bool parse_phosphor_noise_level(const char *argument,
                                       uint8_t *level) {
    if (strcmp(argument, "off") == 0 || strcmp(argument, "0") == 0) {
        *level = P2000M_PHOSPHOR_NOISE_OFF;
    } else if (strcmp(argument, "low") == 0 ||
               strcmp(argument, "1") == 0) {
        *level = P2000M_PHOSPHOR_NOISE_LOW;
    } else if (strcmp(argument, "medium") == 0 ||
               strcmp(argument, "2") == 0) {
        *level = P2000M_PHOSPHOR_NOISE_MEDIUM;
    } else if (strcmp(argument, "high") == 0 ||
               strcmp(argument, "3") == 0) {
        *level = P2000M_PHOSPHOR_NOISE_HIGH;
    } else {
        return false;
    }
    return true;
}

/**
 * @brief Print all current user settings and their persistence state.
 *
 * @return Nothing.
 */
static void print_display_settings(void) {
    const display_style_t style = current_display_style();
    p2000m_capture_stats_t capture;
    p2000m_capture_get_stats(&capture);
    const char *storage_state = configuration_dirty
                                    ? "modified"
                                    : restored_saved_settings ? "saved"
                                                              : "default";
    printf("DISPLAY foreground=#%06" PRIx32 " background=#%06" PRIx32
           " border=%s border_color=#%06" PRIx32
           " border_style=%s scale=%s noise=%s phase_trim=%" PRId32
           " storage=%s vga=%s pal=%s\n",
           style.foreground_rgb, style.background_rgb,
           style.border_enabled ? "on" : "off",
           style.border_rgb,
           style.border_dotted ? "dotted" : "solid",
           style.vertical_stretch_enabled ? "fit-5:3" : "native-1:1",
           phosphor_noise_level_name(style.phosphor_noise_level),
           capture.manual_phase_ticks,
           storage_state,
           vga_output_is_enabled() ? "on" : "off",
           pal_output_is_enabled() ? "on" : "off");
}

/**
 * @brief Print all named colors accepted by the display-color commands.
 *
 * @return Nothing.
 */
static void print_color_presets(void) {
    printf("COLORS");
    for (unsigned i = 0;
         i < sizeof(color_presets) / sizeof(color_presets[0]); ++i) {
        printf(" %s=#%06" PRIx32,
               color_presets[i].name, color_presets[i].rgb);
    }
    printf("\n");
}

/**
 * @brief Write the command-mode prompt to the USB terminal.
 *
 * @return Nothing.
 */
static void print_usb_prompt(void) {
    printf("vid2vga> ");
}

/**
 * @brief Print the product name and semantic firmware version.
 *
 * @return Nothing.
 */
static void print_firmware_version(void) {
    printf("P2000M VID2VGA firmware %s\n"
           "EXPERIMENTAL clock=%uMHz core_voltage=%u.%03uV "
           "official_limit=150MHz\n",
           firmware_version, SYSTEM_CLOCK_KHZ / 1000u,
           SYSTEM_CORE_VOLTAGE_MV / 1000u,
           SYSTEM_CORE_VOLTAGE_MV % 1000u);
}

/**
 * @brief Print the firmware copyright, license, and source location.
 *
 * @return Nothing.
 */
static void print_firmware_license(void) {
    printf("Copyright (C) 2026 Ivo Filot.\n"
           "Free software under GNU GPLv3 or later; there is NO WARRANTY.\n"
           "License and source: "
           "https://github.com/ifilot/p2000m-video-to-vga-adapter\n");
}

/**
 * @brief Leave statistics streaming and restore an editable command prompt.
 *
 * @return Nothing.
 */
static void enter_usb_command_mode(void) {
    abort_screen_transfer();
    usb_interface_mode = USB_COMMAND_MODE;
    usb_command_length = 0u;
    usb_command_overflow = false;
    printf("\r\nCommand mode. Enter HELP for available commands.\r\n");
    print_usb_prompt();
}

/**
 * @brief Leave the console and prepare continuous binary screen output.
 */
static void enter_usb_screen_mode(const char *encoding) {
    if (*encoding == '\0' || strcmp(encoding, "raw") == 0) {
        screen_packbits_enabled = false;
    } else if (strcmp(encoding, "packbits") == 0 ||
               strcmp(encoding, "rle") == 0) {
        screen_packbits_enabled = true;
    } else {
        printf("Unknown screen encoding '%s'; use RAW or PACKBITS.\n",
               encoding);
        return;
    }
    abort_screen_transfer();
    screen_last_sequence_valid = false;
    usb_command_length = 0u;
    usb_command_overflow = false;
    printf("SCREEN mode=binary version=1 width=%u height=%u fps=%u.%03u "
           "header=%u payload=%u flow=continuous encoding=%s "
           "clock_khz=%u experimental=yes exit=console\n",
           P2000M_CAPTURE_WIDTH, P2000M_CAPTURE_HEIGHT,
           25047u / 1000u, 25047u % 1000u,
           SCREEN_FRAME_HEADER_SIZE, SCREEN_FRAME_PAYLOAD_SIZE,
           screen_packbits_enabled ? "packbits+raw" : "raw",
           SYSTEM_CLOCK_KHZ);
    stdio_flush();
    usb_interface_mode = USB_SCREEN_MODE;
}

/**
 * @brief Start periodic statistics streaming and suppress the command prompt.
 *
 * @return Nothing.
 */
static void enter_usb_log_mode(void) {
    usb_interface_mode = USB_LOG_MODE;
    next_usb_log_us = 0u;
    printf("Log mode. Press Enter, Escape, or Q to return to command mode.\n");
}

/**
 * @brief Print the newline-oriented USB command reference.
 *
 * @return Nothing.
 */
static void print_help(void) {
    printf("Commands (press Enter after each command):\n"
           "  status | s                 timing and buffer statistics\n"
           "  version | v                show the firmware version\n"
           "  license                    copyright and license information\n"
           "  log                        stream statistics every two seconds\n"
           "  screen [raw|packbits]      enter continuous binary screen mode\n"
           "  settings                   current runtime and storage settings\n"
           "  vga on|off|toggle          control the physical VGA output\n"
           "  pal on|off|toggle          control PAL composite output\n"
           "  border [on|off|toggle]     control the visible-area rectangle\n"
           "  border-color <color>       set the independent border color\n"
           "  border-style solid|dotted  select the border pattern\n"
           "  scale fit|native           5:3 full-height or centered 1:1 lines\n"
           "  noise off|low|medium|high  foreground phosphor-grain density\n"
           "  fg <name|RRGGBB>           set text/foreground color\n"
           "  bg <name|RRGGBB>           set background color\n"
           "  colors                     list named color presets\n"
           "  defaults                   factory display style (RAM only)\n"
           "  save                       persist display settings and phase\n"
           "  factory-reset              erase saved settings and use defaults\n"
           "  tune | j                   run and report automatic phase tuning\n"
           "  phase +|-|auto             adjust or clear manual phase trim\n"
           "  geometry | g               report 80 x 24 source coverage\n"
           "  help | h | ?               show this command list\n");
}

/**
 * @brief Apply a foreground or background color from a command argument.
 *
 * @param argument Preset name or six-digit RGB code.
 * @param foreground true to change foreground; false to change background.
 * @return Nothing.
 */
static void set_display_color(const char *argument, bool foreground) {
    uint32_t rgb;
    if (!parse_color(argument, &rgb)) {
        printf("Invalid color '%s'. Use COLORS to list names or enter RRGGBB.\n",
               argument);
        return;
    }

    display_style_t style = current_display_style();
    if (foreground) {
        style.foreground_rgb = rgb;
    } else {
        style.background_rgb = rgb;
    }
    publish_display_style(&style);
    print_display_settings();
}

/**
 * @brief Apply an independent border color from a command argument.
 *
 * @param argument Preset name or six-digit RGB code.
 * @return Nothing.
 */
static void set_border_color(const char *argument) {
    uint32_t rgb;
    if (!parse_color(argument, &rgb)) {
        printf("Invalid color '%s'. Use COLORS to list names or enter RRGGBB.\n",
               argument);
        return;
    }

    display_style_t style = current_display_style();
    style.border_rgb = rgb;
    publish_display_style(&style);
    print_display_settings();
}

/** Apply an on/off/toggle command to one physical video output. */
static void configure_physical_output(const char *name, const char *argument,
                                      bool vga) {
    const bool current = vga ? vga_output_is_enabled()
                             : pal_output_is_enabled();
    bool enabled;
    if (strcmp(argument, "on") == 0 || strcmp(argument, "enable") == 0) {
        enabled = true;
    } else if (strcmp(argument, "off") == 0 ||
               strcmp(argument, "disable") == 0) {
        enabled = false;
    } else if (strcmp(argument, "toggle") == 0) {
        enabled = !current;
    } else if (*argument == '\0') {
        print_display_settings();
        return;
    } else {
        printf("Usage: %s on|off|toggle\n", name);
        return;
    }

    const bool changed = vga ? set_vga_output_enabled(enabled)
                             : set_pal_output_enabled(enabled);
    if (!changed) {
        printf("Unable to %s %s output within the coordination timeout.\n",
               enabled ? "enable" : "disable", name);
        return;
    }
    print_display_settings();
}

/**
 * @brief Interpret and execute one complete USB command line.
 *
 * @param command Mutable, null-terminated line without CR/LF characters.
 * @return Nothing.
 */
static void process_usb_command(char *command) {
    command = normalize_command(command);
    if (*command == '\0') {
        return;
    }
    char *argument = split_command_argument(command);

    if (strcmp(command, "status") == 0 || strcmp(command, "s") == 0) {
        print_statistics();
    } else if (strcmp(command, "version") == 0 || strcmp(command, "v") == 0) {
        print_firmware_version();
    } else if (strcmp(command, "license") == 0 ||
               strcmp(command, "copying") == 0) {
        print_firmware_license();
    } else if (strcmp(command, "log") == 0) {
        enter_usb_log_mode();
    } else if (strcmp(command, "screen") == 0 ||
               strcmp(command, "stream") == 0) {
        enter_usb_screen_mode(argument);
    } else if (strcmp(command, "vga") == 0) {
        configure_physical_output("vga", argument, true);
    } else if (strcmp(command, "pal") == 0 ||
               strcmp(command, "composite") == 0) {
        configure_physical_output("pal", argument, false);
    } else if (strcmp(command, "settings") == 0) {
        print_display_settings();
    } else if (strcmp(command, "colors") == 0) {
        print_color_presets();
    } else if (strcmp(command, "fg") == 0 ||
               strcmp(command, "foreground") == 0) {
        if (*argument == '\0') {
            print_display_settings();
        } else {
            set_display_color(argument, true);
        }
    } else if (strcmp(command, "bg") == 0 ||
               strcmp(command, "background") == 0) {
        if (*argument == '\0') {
            print_display_settings();
        } else {
            set_display_color(argument, false);
        }
    } else if (strcmp(command, "border-color") == 0 ||
               strcmp(command, "border_color") == 0 ||
               strcmp(command, "bordercolor") == 0) {
        if (*argument == '\0') {
            print_display_settings();
        } else {
            set_border_color(argument);
        }
    } else if (strcmp(command, "border-style") == 0 ||
               strcmp(command, "border_style") == 0 ||
               strcmp(command, "borderstyle") == 0) {
        display_style_t style = current_display_style();
        if (strcmp(argument, "solid") == 0) {
            style.border_dotted = false;
        } else if (strcmp(argument, "dotted") == 0 ||
                   strcmp(argument, "dot") == 0) {
            style.border_dotted = true;
        } else {
            printf("Usage: border-style solid|dotted\n");
            return;
        }
        publish_display_style(&style);
        print_display_settings();
    } else if (strcmp(command, "border") == 0) {
        display_style_t style = current_display_style();
        if (*argument == '\0' || strcmp(argument, "toggle") == 0) {
            style.border_enabled = !style.border_enabled;
        } else if (strcmp(argument, "on") == 0) {
            style.border_enabled = true;
        } else if (strcmp(argument, "off") == 0) {
            style.border_enabled = false;
        } else {
            printf("Usage: border [on|off|toggle]\n");
            return;
        }
        publish_display_style(&style);
        print_display_settings();
    } else if (strcmp(command, "noise") == 0 ||
               strcmp(command, "grain") == 0) {
        if (*argument == '\0') {
            print_display_settings();
        } else {
            uint8_t level;
            if (!parse_phosphor_noise_level(argument, &level)) {
                printf("Usage: noise off|low|medium|high\n");
                return;
            }
            display_style_t style = current_display_style();
            style.phosphor_noise_level = level;
            publish_display_style(&style);
            print_display_settings();
        }
    } else if (strcmp(command, "defaults") == 0) {
        const display_style_t defaults = default_display_style();
        publish_display_style(&defaults);
        print_display_settings();
    } else if (strcmp(command, "save") == 0) {
        printf("Saving settings to flash; VGA and PAL may blink briefly...\n");
        const int result = save_current_configuration();
        if (result == PICO_OK) {
            printf("Settings saved redundantly (sequence=%" PRIu32 ").\n",
                   saved_settings_sequence);
            print_display_settings();
        } else {
            printf("Unable to save settings (error=%d).\n", result);
        }
    } else if (strcmp(command, "factory-reset") == 0 ||
               strcmp(command, "factory_reset") == 0) {
        printf("Erasing saved settings; VGA and PAL may blink briefly...\n");
        const int result = erase_saved_configuration();
        if (result == PICO_OK) {
            const display_style_t defaults = default_display_style();
            publish_display_style(&defaults);
            if (!p2000m_capture_set_sample_phase(0)) {
                panic("Unable to reset manual phase trim");
            }
            const bool vga_restored = set_vga_output_enabled(true);
            const bool pal_restored = set_pal_output_enabled(true);
            configuration_dirty = !(vga_restored && pal_restored);
            printf(vga_restored && pal_restored
                       ? "Saved settings erased; factory defaults restored.\n"
                       : "Saved settings erased, but an output could not be "
                         "re-enabled.\n");
            print_display_settings();
        } else {
            printf("Unable to erase saved settings (error=%d).\n", result);
        }
    } else if (strcmp(command, "scale") == 0) {
        display_style_t style = current_display_style();
        if (strcmp(argument, "fit") == 0 ||
            strcmp(argument, "stretch") == 0 ||
            strcmp(argument, "5:3") == 0) {
            style.vertical_stretch_enabled = true;
        } else if (strcmp(argument, "native") == 0 ||
                   strcmp(argument, "1:1") == 0) {
            style.vertical_stretch_enabled = false;
        } else {
            printf("Usage: scale fit|native\n");
            return;
        }
        publish_display_style(&style);
        print_display_settings();
    } else if (strcmp(command, "tune") == 0 || strcmp(command, "j") == 0) {
        print_sample_diagnostics();
    } else if (strcmp(command, "geometry") == 0 ||
               strcmp(command, "g") == 0) {
        print_source_geometry();
    } else if (strcmp(command, "phase") == 0) {
        if (strcmp(argument, "+") == 0 || strcmp(argument, "later") == 0) {
            adjust_sample_phase(1, false);
        } else if (strcmp(argument, "-") == 0 ||
                   strcmp(argument, "earlier") == 0) {
            adjust_sample_phase(-1, false);
        } else if (strcmp(argument, "auto") == 0 ||
                   strcmp(argument, "0") == 0) {
            adjust_sample_phase(0, true);
        } else {
            printf("Usage: phase +|-|auto\n");
        }
    } else if (strcmp(command, "+") == 0) {
        adjust_sample_phase(1, false);
    } else if (strcmp(command, "-") == 0) {
        adjust_sample_phase(-1, false);
    } else if (strcmp(command, "0") == 0) {
        adjust_sample_phase(0, true);
    } else if (strcmp(command, "help") == 0 || strcmp(command, "h") == 0 ||
               strcmp(command, "?") == 0) {
        print_help();
    } else {
        printf("Unknown command '%s'. Enter HELP for a command list.\n",
               command);
    }
}

/**
 * @brief Service command editing or the log-mode exit key without blocking.
 *
 * Command mode echoes printable characters, implements destructive Backspace,
 * and executes at CR or LF. Log mode consumes no ordinary input and returns to
 * command mode on Enter, Escape, Q, or q.
 *
 * @return Nothing.
 */
static void poll_usb_commands(void) {
    int character;
    while ((character = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (usb_ignore_next_lf) {
            usb_ignore_next_lf = false;
            if (character == '\n') {
                continue;
            }
        }

        if (usb_interface_mode == USB_SCREEN_MODE) {
            if (character == '\r' || character == '\n') {
                usb_ignore_next_lf = character == '\r';
                if (!usb_command_overflow && usb_command_length != 0u) {
                    usb_command_buffer[usb_command_length] = '\0';
                    char *screen_command =
                        normalize_command(usb_command_buffer);
                    if (strcmp(screen_command, "frame") == 0) {
                        // Compatibility with credit-based version-one viewers.
                        // Continuous mode has already made the request implicit.
                    } else if (strcmp(screen_command, "console") == 0 ||
                               strcmp(screen_command, "stop") == 0) {
                        tud_cdc_write_clear();
                        enter_usb_command_mode();
                    }
                }
                usb_command_length = 0u;
                usb_command_overflow = false;
            } else if (character >= 0x20 && character <= 0x7e) {
                if (usb_command_length + 1u < COMMAND_BUFFER_SIZE) {
                    usb_command_buffer[usb_command_length++] =
                        (char)character;
                } else {
                    usb_command_overflow = true;
                }
            }
            continue;
        }

        if (usb_interface_mode == USB_LOG_MODE) {
            if (character == '\r' || character == '\n' ||
                character == 0x1b || character == 'q' || character == 'Q') {
                usb_ignore_next_lf = character == '\r';
                enter_usb_command_mode();
            }
            continue;
        }

        if (character == '\r' || character == '\n') {
            usb_ignore_next_lf = character == '\r';
            putchar_raw('\r');
            putchar_raw('\n');
            if (usb_command_overflow) {
                printf("Command too long; maximum length is %u characters.\n",
                       COMMAND_BUFFER_SIZE - 1u);
            } else if (usb_command_length != 0u) {
                usb_command_buffer[usb_command_length] = '\0';
                process_usb_command(usb_command_buffer);
            }
            usb_command_length = 0u;
            usb_command_overflow = false;
            if (usb_interface_mode == USB_COMMAND_MODE) {
                print_usb_prompt();
            }
        } else if (character == '\b' || character == 0x7f) {
            if (!usb_command_overflow && usb_command_length != 0u) {
                --usb_command_length;
                putchar_raw('\b');
                putchar_raw(' ');
                putchar_raw('\b');
            }
        } else if (character >= 0x20 && character <= 0x7e) {
            if (usb_command_length + 1u < COMMAND_BUFFER_SIZE) {
                usb_command_buffer[usb_command_length++] = (char)character;
                putchar_raw((char)character);
            } else {
                usb_command_overflow = true;
            }
        }
    }
}

/**
 * @brief Initialize capture, VGA, USB control, and the two-core processing loop.
 *
 * @return Does not return under normal operation.
 */
int main(void) {
    // EXPERIMENTAL: 252 MHz is well beyond the RP2350's 150 MHz rating. Raise
    // the regulator first and allow it to settle. This deliberately stays at
    // the SDK's 1.30 V limit rather than disabling the voltage safety limit.
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_us(1000u);
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the experimental 252 MHz system clock");
    }

    // The application initializes TinyUSB while stdio_usb retains the default
    // CDC descriptors and installs its low-priority endpoint-service worker.
    tusb_init();
    stdio_init_all();
    validate_settings_flash_region();
    display_style_t initial_style = default_display_style();
    bool initial_vga_enabled = true;
    bool initial_pal_enabled = true;
    restored_saved_settings = load_saved_configuration(
        &initial_style, &initial_vga_enabled, &initial_pal_enabled);
    __atomic_store_n(&vga_pause_requested, !initial_vga_enabled,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&pal_pause_requested, !initial_pal_enabled,
                     __ATOMIC_RELEASE);
    configuration_dirty = false;
    initialize_display_styles(&initial_style);

    // scanvideo has fixed ownership of DMA channel 0, so configure it before
    // capture dynamically claims two other channels.
    if (!scanvideo_setup(&firmware_vga_mode)) {
        panic("Unable to initialize VGA scanvideo");
    }
    initialize_decoded_buffers();

    // Capture owns dynamically allocated DMA channels, so it must start after
    // scanvideo has claimed channel 0 but before core 1 can request a frame.
    p2000m_capture_start();
    if (!p2000m_capture_set_sample_phase(saved_manual_phase_ticks)) {
        panic("Saved manual phase trim is invalid");
    }
    multicore_launch_core1(vga_core_main);
    if (multicore_fifo_pop_blocking() != VGA_READY_MAGIC) {
        panic("Unable to start VGA rendering core");
    }
    if (initial_vga_enabled) {
        scanvideo_timing_enable(true);
    }

    bool announced = false;
    uint64_t next_automatic_tune = time_us_64() + 250000u;
    while (true) {
        const uint64_t now = time_us_64();
        if (now >= next_automatic_tune) {
            const bool tuned = p2000m_capture_autotune(NULL);
            next_automatic_tune = now + (tuned ? 5000000u : 100000u);
        }
        (void)decode_latest_source_frame();

        if (!stdio_usb_connected()) {
            announced = false;
            abort_screen_transfer();
            usb_interface_mode = USB_COMMAND_MODE;
            usb_command_length = 0u;
            usb_command_overflow = false;
            usb_ignore_next_lf = false;
            // Keep converting at the input frame rate even without a USB host.
            // Triple buffering lets both display consumers retain complete
            // frames while core 0 prepares another whenever space is free.
            sleep_ms(1);
            continue;
        }

        if (!announced) {
            sleep_ms(100);
            printf("P2000M VID2VGA firmware %s ready: 640x288 source to "
                   "640x480 VGA and monochrome 625/50 composite.\n",
                   firmware_version);
            print_firmware_license();
            print_display_settings();
            printf("Enter HELP for available commands.\n");
            announced = true;
            print_usb_prompt();
        }

        poll_usb_commands();

        const uint64_t status_now = time_us_64();
        if (usb_interface_mode == USB_SCREEN_MODE) {
            service_screen_output();
        } else if (usb_interface_mode == USB_LOG_MODE &&
            status_now >= next_usb_log_us) {
            print_statistics();
            next_usb_log_us = status_now + USB_LOG_INTERVAL_US;
        }
        sleep_ms(1);
    }
}
