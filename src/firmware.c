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
#include "p2000m_capture.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

/** Semantic firmware version supplied by the top-level CMake project. */
static const char firmware_version[] = "v" P2000M_VID2VGA_VERSION;

enum {
    /** Shared system clock chosen for exact capture and VGA clock divisors. */
    SYSTEM_CLOCK_KHZ = 126000,
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
    /** Number of alternating flash sectors used for atomic settings updates. */
    SETTINGS_SLOT_COUNT = 2,
    /** First settings sector; the RP2350's final flash sector stays reserved. */
    SETTINGS_FLASH_OFFSET =
        PICO_FLASH_SIZE_BYTES - (SETTINGS_SLOT_COUNT + 1) * FLASH_SECTOR_SIZE,
    /** File-format signature: ASCII "V2GA" when viewed little-endian. */
    SETTINGS_MAGIC = 0x41473256,
    /** On-flash settings structure version. */
    SETTINGS_VERSION = 1,
    /** Maximum time to coordinate each multicore flash lockout phase. */
    FLASH_LOCKOUT_TIMEOUT_MS = 1000,
    /** Core-start handshake value sent through the multicore FIFO. */
    VGA_READY_MAGIC = 0x56474131,
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
    /** Buffer contains a complete frame waiting for VGA presentation. */
    DECODED_READY,
    /** Core 1 is currently presenting the buffer. */
    DECODED_IN_USE,
} decoded_buffer_state_t;

/** Mutually exclusive interaction modes for the USB serial terminal. */
typedef enum {
    /** Editable, echoed command line with no unsolicited statistics. */
    USB_COMMAND_MODE,
    /** Periodic statistics stream; one exit key restores the prompt. */
    USB_LOG_MODE,
} usb_interface_mode_t;

/** User-selectable colors and geometry overlay for one VGA frame. */
typedef struct {
    /** Foreground color as entered by the user, in 0xRRGGBB form. */
    uint32_t foreground_rgb;
    /** Background color as entered by the user, in 0xRRGGBB form. */
    uint32_t background_rgb;
    /** Whether to draw a one-pixel rectangle around the 640 x 288 image. */
    bool border_enabled;
    /** Whether to scale 288 source lines to all 480 active VGA lines. */
    bool vertical_stretch_enabled;
} display_style_t;

/** Versioned, checksummed display settings stored redundantly in flash. */
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
    /** Saved border flag encoded as zero or one. */
    uint8_t border_enabled;
    /** Saved vertical-scaling flag encoded as zero or one. */
    uint8_t vertical_stretch_enabled;
    /** Saved manual sampling-phase trim from -4 through +4 ticks. */
    int8_t manual_phase_ticks;
    /** Reserved zero bytes for compatible format extensions. */
    uint8_t reserved[5];
    /** CRC-32 of every preceding byte in this structure. */
    uint32_t checksum;
} persisted_settings_t;

_Static_assert(sizeof(persisted_settings_t) == 32u,
               "Persisted settings format must remain exactly 32 bytes");

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

/** Three decoded frames shared between the capture and VGA cores. */
static uint32_t decoded_frames[DECODED_BUFFER_COUNT][DECODED_WORDS_PER_FRAME];
/** Ownership state for each entry in decoded_frames. */
static decoded_buffer_state_t decoded_states[DECODED_BUFFER_COUNT];
/** P2000M capture sequence number represented by each decoded frame. */
static uint32_t decoded_sequences[DECODED_BUFFER_COUNT];
/** Cross-core lock protecting decoded_states and decoded_sequences. */
static spin_lock_t *decoded_lock;

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

/** Lifetime VGA frame counter, written by core 1. */
static volatile uint32_t generated_vga_frames;
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
 * @return White foreground, black background, border off, and native mode.
 */
static display_style_t default_display_style(void) {
    const display_style_t defaults = {
        .foreground_rgb = 0xffffffu,
        .background_rgb = 0x000000u,
        .border_enabled = false,
        .vertical_stretch_enabled = false,
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
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            const uint32_t polynomial =
                (crc & 1u) != 0u ? 0xedb88320u : 0u;
            crc = (crc >> 1u) ^ polynomial;
        }
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
        record->border_enabled <= 1u &&
        record->vertical_stretch_enabled <= 1u &&
        record->manual_phase_ticks >= -4 &&
        record->manual_phase_ticks <= 4 &&
        record->checksum ==
            settings_crc32(record, offsetof(persisted_settings_t, checksum));
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
 * @return true when settings were restored; false when defaults should remain.
 */
static bool load_saved_configuration(display_style_t *style) {
    int newest_slot = -1;
    const persisted_settings_t *newest_record = NULL;
    for (unsigned slot = 0; slot < SETTINGS_SLOT_COUNT; ++slot) {
        const persisted_settings_t *candidate = settings_slot_record(slot);
        if (settings_record_is_valid(candidate) &&
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
    style->border_enabled = newest_record->border_enabled != 0u;
    style->vertical_stretch_enabled =
        newest_record->vertical_stretch_enabled != 0u;
    saved_settings_slot = newest_slot;
    saved_settings_sequence = newest_record->sequence;
    saved_manual_phase_ticks = newest_record->manual_phase_ticks;
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
    }
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
        if (decoded_states[i] == DECODED_FREE) {
            decoded_index = (int)i;
            break;
        }
    }
    if (decoded_index < 0) {
        uint32_t oldest_sequence = UINT32_MAX;
        for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
            if (decoded_states[i] == DECODED_READY &&
                decoded_sequences[i] < oldest_sequence) {
                decoded_index = (int)i;
                oldest_sequence = decoded_sequences[i];
            }
        }
    }
    hard_assert(decoded_index >= 0);
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
    int next = -1;
    uint32_t sequence = 0;
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        if (decoded_states[i] == DECODED_READY &&
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
    decoded_states[next] = DECODED_IN_USE;
    ++source_frame_swaps;

    if (previous >= 0) {
        decoded_states[previous] = DECODED_FREE;
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
 * @brief Render one captured P2000M line using the active colors and border.
 *
 * @param scanline_buffer Scanvideo buffer that receives composable tokens.
 * @param source_y Zero-based source line in the 640 x 288 decoded frame.
 * @param horizontal_border true to replace this entire line with the border.
 * @return Nothing.
 */
static void render_source_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                                   unsigned source_y,
                                   bool horizontal_border) {
    const display_style_t *style = &display_styles[displayed_style_index];
    const uint16_t foreground = rgb888_to_scanvideo(style->foreground_rgb);
    if (horizontal_border) {
        render_solid_scanline(scanline_buffer, foreground);
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

    if (style->border_enabled) {
        tokens[1] = foreground;
        destination[-1] = foreground;
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
        select_frame_for_next_vga_frame();
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

    const bool horizontal_border = style->border_enabled &&
        (visible_y == 0u || visible_y + 1u == visible_height);
    render_source_scanline(scanline_buffer, source_y, horizontal_border);
}

/**
 * @brief Run the deadline-critical VGA scanline producer on Pico core 1.
 *
 * @return Does not return.
 */
static void __not_in_flash_func(vga_core_main)(void) {
    // Setup and timing IRQ ownership stay on core 0, matching the proven VGA
    // diagnostic. Core 1 is dedicated only to preparing scanline buffers.
    if (!flash_safe_execute_core_init()) {
        panic("Unable to initialize core 1 flash lockout");
    }
    multicore_fifo_push_blocking(VGA_READY_MAGIC);

    while (true) {
        scanvideo_scanline_buffer_t *scanline_buffer =
            scanvideo_begin_scanline_generation(true);
        render_scanline(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
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

    const uint32_t vga_frames =
        __atomic_load_n(&generated_vga_frames, __ATOMIC_RELAXED);
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

    if (capture.last_frame_period_us == 0) {
        printf("VID2VGA capture_frames=%" PRIu32
               " waiting_for_input vga_frames=%" PRIu32
               " swaps=%" PRIu32 " repeats=%" PRIu32
               " blank=%" PRIu32 " displayed_sequence=%" PRIu32
               " decoded_frames=%" PRIu32 " decode_us=%" PRIu32
               " decode_max_us=%" PRIu32
               " auto_phase_ticks=%" PRId32
               " manual_trim_ticks=%" PRId32 "\n",
               capture.captured_frames, vga_frames, swaps, repeats, blanks,
               sequence, decoded, decode_us, decode_max_us,
               capture.auto_phase_ticks,
               capture.manual_phase_ticks);
        return;
    }

    const uint32_t rate_millihz = 1000000000u / capture.last_frame_period_us;
    const bool locked = capture.last_frame_period_us >= 19000u &&
                        capture.last_frame_period_us <= 21000u;
    printf("VID2VGA capture_frames=%" PRIu32 " input_period_us=%" PRIu32
           " input_rate=%" PRIu32 ".%03" PRIu32
           "Hz locked=%s stale_replaced=%" PRIu32
           " vga_frames=%" PRIu32 " swaps=%" PRIu32
           " repeats=%" PRIu32 " blank=%" PRIu32
           " displayed_sequence=%" PRIu32
           " decoded_frames=%" PRIu32 " decode_us=%" PRIu32
           " decode_max_us=%" PRIu32
           " line_ticks=%" PRIu32 ".%03" PRIu32
           " auto_phase_ticks=%" PRId32
           " manual_trim_ticks=%" PRId32
           " autotune_runs=%" PRIu32 " tune_score=%" PRIu32
           " tune_us=%" PRIu32 " tune_max_us=%" PRIu32 "\n",
           capture.captured_frames, capture.last_frame_period_us,
           rate_millihz / 1000u, rate_millihz % 1000u,
           locked ? "yes" : "no", capture.stale_frames_replaced,
           vga_frames, swaps, repeats, blanks, sequence,
           decoded, decode_us, decode_max_us,
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
        .border_enabled = style.border_enabled ? 1u : 0u,
        .vertical_stretch_enabled =
            style.vertical_stretch_enabled ? 1u : 0u,
        .manual_phase_ticks = (int8_t)capture.manual_phase_ticks,
        .reserved = {0},
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

    const int result = flash_safe_execute(perform_settings_flash_mutation,
                                          &mutation,
                                          FLASH_LOCKOUT_TIMEOUT_MS);
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

    const int result = flash_safe_execute(perform_settings_flash_mutation,
                                          &mutation,
                                          FLASH_LOCKOUT_TIMEOUT_MS);
    if (result == PICO_OK) {
        saved_settings_slot = -1;
        saved_settings_sequence = 0u;
        restored_saved_settings = false;
        saved_manual_phase_ticks = 0;
    }
    return result;
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
           " border=%s scale=%s phase_trim=%" PRId32 " storage=%s\n",
           style.foreground_rgb, style.background_rgb,
           style.border_enabled ? "on" : "off",
           style.vertical_stretch_enabled ? "fit-5:3" : "native-1:1",
           capture.manual_phase_ticks,
           storage_state);
}

/**
 * @brief Print all named colors accepted by fg and bg commands.
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
    printf("P2000M VID2VGA firmware %s\n", firmware_version);
}

/**
 * @brief Leave statistics streaming and restore an editable command prompt.
 *
 * @return Nothing.
 */
static void enter_usb_command_mode(void) {
    usb_interface_mode = USB_COMMAND_MODE;
    usb_command_length = 0u;
    usb_command_overflow = false;
    printf("\r\nCommand mode. Enter HELP for available commands.\r\n");
    print_usb_prompt();
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
           "  log                        stream statistics every two seconds\n"
           "  settings                   current runtime and storage settings\n"
           "  border [on|off|toggle]     visible-area rectangle in text color\n"
           "  scale fit|native           5:3 full-height or centered 1:1 lines\n"
           "  fg <name|RRGGBB>           set text/foreground color\n"
           "  bg <name|RRGGBB>           set background color\n"
           "  colors                     list named color presets\n"
           "  defaults                   factory display style (RAM only)\n"
           "  save                       persist colors, geometry, and phase\n"
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
    } else if (strcmp(command, "log") == 0) {
        enter_usb_log_mode();
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
    } else if (strcmp(command, "defaults") == 0) {
        const display_style_t defaults = default_display_style();
        publish_display_style(&defaults);
        print_display_settings();
    } else if (strcmp(command, "save") == 0) {
        printf("Saving settings to flash; VGA may blink briefly...\n");
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
        printf("Erasing saved settings; VGA may blink briefly...\n");
        const int result = erase_saved_configuration();
        if (result == PICO_OK) {
            const display_style_t defaults = default_display_style();
            publish_display_style(&defaults);
            if (!p2000m_capture_set_sample_phase(0)) {
                panic("Unable to reset manual phase trim");
            }
            configuration_dirty = false;
            printf("Saved settings erased; factory defaults restored.\n");
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
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the 126 MHz system clock");
    }

    stdio_init_all();
    validate_settings_flash_region();
    display_style_t initial_style = default_display_style();
    restored_saved_settings = load_saved_configuration(&initial_style);
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
    scanvideo_timing_enable(true);

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
            usb_interface_mode = USB_COMMAND_MODE;
            usb_command_length = 0u;
            usb_command_overflow = false;
            usb_ignore_next_lf = false;
            // Keep converting at the input frame rate even without a USB host.
            // Triple buffering ensures VGA always retains a complete READY
            // frame while core 0 prepares the next one.
            sleep_ms(1);
            continue;
        }

        if (!announced) {
            sleep_ms(100);
            printf("P2000M VID2VGA firmware %s ready: 640x288 source to "
                   "640x480 VGA.\n", firmware_version);
            print_display_settings();
            printf("Enter HELP for available commands.\n");
            announced = true;
            print_usb_prompt();
        }

        poll_usb_commands();

        const uint64_t status_now = time_us_64();
        if (usb_interface_mode == USB_LOG_MODE &&
            status_now >= next_usb_log_us) {
            print_statistics();
            next_usb_log_us = status_now + USB_LOG_INTERVAL_US;
        }
        sleep_ms(1);
    }
}
