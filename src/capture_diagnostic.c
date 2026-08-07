#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "p2000m_capture.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

enum {
    SYSTEM_CLOCK_KHZ = 126000,
    PREVIEW_SCALE_X = 8,
    PREVIEW_SCALE_Y = 8,
};

static void put_raw_newline(void) {
    // Raw output bypasses the SDK's optional LF-to-CRLF translation.
    putchar_raw('\r');
    putchar_raw('\n');
}

static void print_capture_stats(void) {
    p2000m_capture_stats_t stats;
    p2000m_capture_get_stats(&stats);

    if (stats.last_frame_period_us == 0) {
        printf("CAPTURE frames=%" PRIu32
               " waiting_for_period stale_replaced=%" PRIu32
               " auto_phase_ticks=%" PRId32
               " manual_trim_ticks=%" PRId32 "\n",
               stats.captured_frames, stats.stale_frames_replaced,
               stats.auto_phase_ticks, stats.manual_phase_ticks);
        return;
    }

    const uint32_t period = stats.last_frame_period_us;
    const uint32_t frame_rate_millihz = 1000000000u / period;
    const uint32_t line_period_ns =
        (uint32_t)(((uint64_t)period * 1000u) / 312u);
    const bool locked = period >= 19000u && period <= 21000u;
    printf("CAPTURE frames=%" PRIu32 " period_us=%" PRIu32
           " line_us=%" PRIu32 ".%03" PRIu32
           " rate=%" PRIu32 ".%03" PRIu32
           "Hz locked=%s stale_replaced=%" PRIu32
           " line_ticks=%" PRIu32 ".%03" PRIu32
           " auto_phase_ticks=%" PRId32
           " manual_trim_ticks=%" PRId32
           " autotune_runs=%" PRIu32 "\n",
           stats.captured_frames, period,
           line_period_ns / 1000u, line_period_ns % 1000u,
           frame_rate_millihz / 1000u, frame_rate_millihz % 1000u,
           locked ? "yes" : "no", stats.stale_frames_replaced,
           stats.recovered_line_ticks_q16 >> 16,
           ((stats.recovered_line_ticks_q16 & 0xffffu) * 1000u) >> 16,
           stats.auto_phase_ticks, stats.manual_phase_ticks,
           stats.autotune_runs);
}

static void print_ascii_preview(void) {
    uint32_t sequence;
    const int buffer_index = p2000m_capture_acquire_latest_frame(&sequence);
    if (buffer_index < 0) {
        printf("No completed frame is available yet.\n");
        return;
    }

    const uint32_t *frame = p2000m_capture_buffer((unsigned)buffer_index);
    printf("PREVIEW sequence=%" PRIu32
           " 80x36 ('#' contains white source pixels)\n", sequence);
    for (unsigned y = 0; y < P2000M_CAPTURE_HEIGHT; y += PREVIEW_SCALE_Y) {
        for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; x += PREVIEW_SCALE_X) {
            bool white = false;
            for (unsigned dy = 0; dy < PREVIEW_SCALE_Y && !white; ++dy) {
                for (unsigned dx = 0; dx < PREVIEW_SCALE_X; ++dx) {
                    if (p2000m_capture_pixel_is_white(frame, x + dx, y + dy)) {
                        white = true;
                        break;
                    }
                }
            }
            putchar_raw(white ? '#' : ' ');
        }
        put_raw_newline();
    }
    printf("END_PREVIEW\n");
    p2000m_capture_release_frame((unsigned)buffer_index);
}

static void print_hex_frame_dump(void) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t sequence;
    const int buffer_index = p2000m_capture_acquire_latest_frame(&sequence);
    if (buffer_index < 0) {
        printf("No completed frame is available yet.\n");
        return;
    }

    const uint32_t *frame = p2000m_capture_buffer((unsigned)buffer_index);
    printf("BEGIN_FRAME sequence=%" PRIu32
           " width=640 height=288 sample_clock_hz=%u bytes=%u "
           "encoding=28-samples-per-word-msb-first pad_bits=31:28 "
           "zero=white decoder=line-locked-resampler\n",
           sequence, P2000M_CAPTURE_SAMPLE_CLOCK_HZ,
           (unsigned)P2000M_CAPTURE_BYTES_PER_FRAME);

    unsigned bytes_on_line = 0;
    for (unsigned word_index = 0;
         word_index < P2000M_CAPTURE_WORDS_PER_FRAME; ++word_index) {
        const uint32_t word = frame[word_index];
        for (int shift = 24; shift >= 0; shift -= 8) {
            const uint8_t byte = (uint8_t)(word >> (unsigned)shift);
            putchar_raw(hex[byte >> 4u]);
            putchar_raw(hex[byte & 0x0fu]);
            if (++bytes_on_line == 64u) {
                put_raw_newline();
                bytes_on_line = 0;
            }
        }
    }
    if (bytes_on_line != 0) {
        put_raw_newline();
    }
    printf("END_FRAME\n");
    p2000m_capture_release_frame((unsigned)buffer_index);
}

static void print_resampler_diagnostics(void) {
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
    printf("Commands: s=statistics, p=80x36 preview, d=hex framebuffer, "
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
    p2000m_capture_start();

    bool announced = false;
    uint64_t next_periodic_status = 0;
    uint64_t next_automatic_tune = time_us_64() + 250000u;
    while (true) {
        const uint64_t now = time_us_64();
        if (now >= next_automatic_tune) {
            const bool tuned = p2000m_capture_autotune(NULL);
            next_automatic_tune = now + (tuned ? 1000000u : 100000u);
        }

        if (!stdio_usb_connected()) {
            announced = false;
            sleep_ms(20);
            continue;
        }

        if (!announced) {
            sleep_ms(100);
            printf("P2000M capture diagnostic ready. Input GPIO16/17/18 is active-low.\n");
            print_help();
            announced = true;
            next_periodic_status = time_us_64() + 2000000u;
        }

        const int command = getchar_timeout_us(0);
        if (command == 's') {
            print_capture_stats();
        } else if (command == 'p') {
            print_ascii_preview();
        } else if (command == 'd') {
            print_hex_frame_dump();
        } else if (command == 'j') {
            print_resampler_diagnostics();
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
            print_capture_stats();
            next_periodic_status = status_now + 2000000u;
        }
        sleep_ms(1);
    }
}
