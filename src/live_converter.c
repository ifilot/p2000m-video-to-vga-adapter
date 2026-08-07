#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "p2000m_capture.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

enum {
    SYSTEM_CLOCK_KHZ = 126000,
    VGA_WIDTH = 640,
    VGA_HEIGHT = 480,
    VGA_TOP_MARGIN = (VGA_HEIGHT - P2000M_CAPTURE_HEIGHT) / 2,
    VGA_BOTTOM_MARGIN = VGA_HEIGHT - VGA_TOP_MARGIN - P2000M_CAPTURE_HEIGHT,
    // RAW_RUN header + remaining source pixels + one explicit black pixel + EOL.
    RAW_SCANLINE_TOKENS = (3 + VGA_WIDTH - 1) + 2 + 2,
    RAW_SCANLINE_WORDS = RAW_SCANLINE_TOKENS / 2,
    DECODED_WORDS_PER_LINE = P2000M_CAPTURE_WIDTH / 32,
    DECODED_WORDS_PER_FRAME =
        DECODED_WORDS_PER_LINE * P2000M_CAPTURE_HEIGHT,
    DECODED_BUFFER_COUNT = 2,
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

static const scanvideo_mode_t live_vga_mode = {
    .default_timing = &vga_timing_640x480_60,
    .pio_program = &video_24mhz_composable,
    .width = VGA_WIDTH,
    .height = VGA_HEIGHT,
    .xscale = 1,
    .yscale = 1,
    .yscale_denominator = 1,
};

typedef enum {
    DECODED_FREE,
    DECODED_FILLING,
    DECODED_READY,
    DECODED_IN_USE,
} decoded_buffer_state_t;

static uint32_t decoded_frames[DECODED_BUFFER_COUNT][DECODED_WORDS_PER_FRAME];
static decoded_buffer_state_t decoded_states[DECODED_BUFFER_COUNT];
static uint32_t decoded_sequences[DECODED_BUFFER_COUNT];
static spin_lock_t *decoded_lock;

static uint16_t monochrome_pixels[256][8];
static int displayed_buffer = -1;
static uint32_t displayed_sequence;

static volatile uint32_t generated_vga_frames;
static volatile uint32_t source_frame_swaps;
static volatile uint32_t repeated_vga_frames;
static volatile uint32_t blank_vga_frames;

static void initialize_monochrome_lookup(void) {
    for (unsigned value = 0; value < 256u; ++value) {
        for (unsigned bit = 0; bit < 8u; ++bit) {
            monochrome_pixels[value][bit] =
                (value & (0x80u >> bit)) != 0u ? 0x0fffu : 0x0000u;
        }
    }
}

static void initialize_decoded_buffers(void) {
    decoded_lock = spin_lock_instance((unsigned)spin_lock_claim_unused(true));
    for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
        decoded_states[i] = DECODED_FREE;
        decoded_sequences[i] = 0;
    }
}

static bool decode_latest_source_frame(void) {
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
        for (unsigned i = 0; i < DECODED_BUFFER_COUNT; ++i) {
            if (decoded_states[i] == DECODED_READY) {
                decoded_index = (int)i;
                break;
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
    return true;
}

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

static void render_black_scanline(scanvideo_scanline_buffer_t *scanline_buffer) {
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_COLOR_RUN;
    tokens[1] = 0x0000;
    tokens[2] = VGA_WIDTH - 3;
    tokens[3] = COMPOSABLE_EOL_ALIGN;
    scanline_buffer->data_used = 2;
    scanline_buffer->status = SCANLINE_OK;
}

static void render_source_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                                   unsigned source_y) {
    const uint32_t *line =
        decoded_frames[displayed_buffer] +
        source_y * DECODED_WORDS_PER_LINE;
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;

    tokens[0] = COMPOSABLE_RAW_RUN;
    const uint8_t first_byte = (uint8_t)(line[0] >> 24u);
    tokens[1] = monochrome_pixels[first_byte][0];
    tokens[2] = VGA_WIDTH - 3;

    uint16_t *destination = &tokens[3];
    for (unsigned bit = 1; bit < 8u; ++bit) {
        *destination++ = monochrome_pixels[first_byte][bit];
    }
    for (unsigned word_index = 0;
         word_index < DECODED_WORDS_PER_LINE; ++word_index) {
        const uint32_t word = line[word_index];
        const unsigned first_shift = word_index == 0u ? 16u : 24u;
        for (int shift = (int)first_shift; shift >= 0; shift -= 8) {
            const uint16_t *pixels =
                monochrome_pixels[(uint8_t)(word >> (unsigned)shift)];
            for (unsigned bit = 0; bit < 8u; ++bit) {
                *destination++ = pixels[bit];
            }
        }
    }

    // scanvideo requires the color pins to be returned to black before the
    // horizontal blanking interval. This 641st output lies in the front porch;
    // all 640 source pixels remain in the active VGA area.
    tokens[RAW_SCANLINE_TOKENS - 4] = COMPOSABLE_RAW_1P;
    tokens[RAW_SCANLINE_TOKENS - 3] = 0x0000;
    tokens[RAW_SCANLINE_TOKENS - 2] = COMPOSABLE_EOL_SKIP_ALIGN;
    tokens[RAW_SCANLINE_TOKENS - 1] = 0;
    scanline_buffer->data_used = RAW_SCANLINE_WORDS;
    scanline_buffer->status = SCANLINE_OK;
}

static void __not_in_flash_func(render_scanline)(
    scanvideo_scanline_buffer_t *scanline_buffer) {
    const unsigned y = scanvideo_scanline_number(scanline_buffer->scanline_id);

    if (scanline_buffer->data_max < RAW_SCANLINE_WORDS) {
        scanline_buffer->data_used = 0;
        scanline_buffer->status = SCANLINE_ERROR;
        return;
    }

    if (y == 0u) {
        select_frame_for_next_vga_frame();
    }

    if (displayed_buffer < 0 || y < VGA_TOP_MARGIN ||
        y >= VGA_HEIGHT - VGA_BOTTOM_MARGIN) {
        render_black_scanline(scanline_buffer);
        return;
    }

    render_source_scanline(scanline_buffer, y - VGA_TOP_MARGIN);
}

static void __not_in_flash_func(vga_core_main)(void) {
    // Setup and timing IRQ ownership stay on core 0, matching the proven VGA
    // diagnostic. Core 1 is dedicated only to preparing scanline buffers.
    multicore_fifo_push_blocking(VGA_READY_MAGIC);

    while (true) {
        scanvideo_scanline_buffer_t *scanline_buffer =
            scanvideo_begin_scanline_generation(true);
        render_scanline(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

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

    if (capture.last_frame_period_us == 0) {
        printf("LIVE capture_frames=%" PRIu32
               " waiting_for_input vga_frames=%" PRIu32
               " swaps=%" PRIu32 " repeats=%" PRIu32
               " blank=%" PRIu32 " displayed_sequence=%" PRIu32
               " auto_phase_ticks=%" PRId32
               " manual_trim_ticks=%" PRId32 "\n",
               capture.captured_frames, vga_frames, swaps, repeats, blanks,
               sequence, capture.auto_phase_ticks,
               capture.manual_phase_ticks);
        return;
    }

    const uint32_t rate_millihz = 1000000000u / capture.last_frame_period_us;
    const bool locked = capture.last_frame_period_us >= 19000u &&
                        capture.last_frame_period_us <= 21000u;
    printf("LIVE capture_frames=%" PRIu32 " input_period_us=%" PRIu32
           " input_rate=%" PRIu32 ".%03" PRIu32
           "Hz locked=%s stale_replaced=%" PRIu32
           " vga_frames=%" PRIu32 " swaps=%" PRIu32
           " repeats=%" PRIu32 " blank=%" PRIu32
           " displayed_sequence=%" PRIu32
           " line_ticks=%" PRIu32 ".%03" PRIu32
           " auto_phase_ticks=%" PRId32
           " manual_trim_ticks=%" PRId32
           " autotune_runs=%" PRIu32 " tune_score=%" PRIu32 "\n",
           capture.captured_frames, capture.last_frame_period_us,
           rate_millihz / 1000u, rate_millihz % 1000u,
           locked ? "yes" : "no", capture.stale_frames_replaced,
           vga_frames, swaps, repeats, blanks, sequence,
           capture.recovered_line_ticks_q16 >> 16,
           ((capture.recovered_line_ticks_q16 & 0xffffu) * 1000u) >> 16,
           capture.auto_phase_ticks, capture.manual_phase_ticks,
           capture.autotune_runs, capture.autotune_score);
}

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

static void print_help(void) {
    printf("Commands: s=live statistics, g=source geometry, "
           "j=resampler diagnostic/tune, +=trim later, -=trim earlier, "
           "0=clear manual trim, h=help\n");
}

static void adjust_sample_phase(int change, bool reset) {
    p2000m_capture_stats_t stats;
    p2000m_capture_get_stats(&stats);
    const int requested = reset ? 0 : stats.manual_phase_ticks + change;
    if (!p2000m_capture_set_sample_phase(requested)) {
        printf("Manual trim limit reached (-4 through +4 capture ticks).\n");
        return;
    }
    printf("Manual resampler trim is now %d tick(s), 15.87 ns each (%s).\n",
           requested,
           requested > 0 ? "later" : requested < 0 ? "earlier" : "automatic");
}

int main(void) {
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the 126 MHz system clock");
    }

    stdio_init_all();
    initialize_monochrome_lookup();

    // scanvideo has fixed ownership of DMA channel 0, so configure it before
    // capture dynamically claims two other channels.
    if (!scanvideo_setup(&live_vga_mode)) {
        panic("Unable to initialize VGA scanvideo");
    }
    initialize_decoded_buffers();

    // Capture owns dynamically allocated DMA channels, so it must start after
    // scanvideo has claimed channel 0 but before core 1 can request a frame.
    p2000m_capture_start();
    multicore_launch_core1(vga_core_main);
    if (multicore_fifo_pop_blocking() != VGA_READY_MAGIC) {
        panic("Unable to start VGA rendering core");
    }
    scanvideo_timing_enable(true);

    bool announced = false;
    uint64_t next_periodic_status = 0;
    uint64_t next_automatic_tune = time_us_64() + 250000u;
    while (true) {
        const uint64_t now = time_us_64();
        if (now >= next_automatic_tune) {
            const bool tuned = p2000m_capture_autotune(NULL);
            next_automatic_tune = now + (tuned ? 1000000u : 100000u);
        }
        (void)decode_latest_source_frame();

        if (!stdio_usb_connected()) {
            announced = false;
            sleep_ms(20);
            continue;
        }

        if (!announced) {
            sleep_ms(100);
            printf("P2000M live VGA bridge ready: 640x288 centered in 640x480.\n");
            print_help();
            announced = true;
            next_periodic_status = time_us_64() + 2000000u;
        }

        const int command = getchar_timeout_us(0);
        if (command == 's') {
            print_statistics();
        } else if (command == 'g') {
            print_source_geometry();
        } else if (command == 'j') {
            print_sample_diagnostics();
        } else if (command == '+') {
            adjust_sample_phase(1, false);
        } else if (command == '-') {
            adjust_sample_phase(-1, false);
        } else if (command == '0') {
            adjust_sample_phase(0, true);
        } else if (command == 'h' || command == '?') {
            print_help();
        }

        const uint64_t status_now = time_us_64();
        if (status_now >= next_periodic_status) {
            print_statistics();
            next_periodic_status = status_now + 2000000u;
        }
        sleep_ms(1);
    }
}
