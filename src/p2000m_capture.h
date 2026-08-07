#ifndef P2000M_CAPTURE_H
#define P2000M_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    P2000M_CAPTURE_WIDTH = 640,
    P2000M_CAPTURE_HEIGHT = 288,
    P2000M_CAPTURE_SAMPLE_CLOCK_HZ = 63000000,
    P2000M_CAPTURE_SAMPLES_PER_WORD = 28,
    P2000M_CAPTURE_TICKS_PER_WORD = 30,
    P2000M_CAPTURE_WORDS_PER_LINE = 114,
    P2000M_CAPTURE_SAMPLES_PER_LINE =
        P2000M_CAPTURE_WORDS_PER_LINE * P2000M_CAPTURE_SAMPLES_PER_WORD,
    P2000M_CAPTURE_WORDS_PER_FRAME =
        P2000M_CAPTURE_WORDS_PER_LINE * P2000M_CAPTURE_HEIGHT,
    P2000M_CAPTURE_BYTES_PER_FRAME =
        P2000M_CAPTURE_WORDS_PER_FRAME * sizeof(uint32_t),
    P2000M_CAPTURE_TUNING_CANDIDATES = 5,
};

typedef struct {
    uint32_t first_word_mask;
    uint32_t second_word_mask;
    uint16_t first_word;
    uint16_t second_word;
} p2000m_capture_pixel_map_t;

typedef struct {
    uint32_t captured_frames;
    uint32_t stale_frames_replaced;
    uint32_t last_frame_period_us;
    uint32_t recovered_line_ticks_q16;
    uint32_t autotune_runs;
    uint32_t autotune_score;
    uint32_t last_autotune_us;
    uint32_t maximum_autotune_us;
    int32_t auto_phase_ticks;
    int32_t manual_phase_ticks;
} p2000m_capture_stats_t;

typedef struct {
    uint32_t sequence;
    uint32_t scores[P2000M_CAPTURE_TUNING_CANDIDATES];
    int32_t first_candidate_tick;
    int32_t selected_phase_tick;
} p2000m_capture_tuning_report_t;

void p2000m_capture_start(void);

// Apply a manual trim in 63 MHz capture ticks (15.87 ns, about 0.19 dot).
// Supported values are -4 through +4. The automatic phase remains active.
bool p2000m_capture_set_sample_phase(int phase_ticks);

// Fit the horizontal dot grid to the measured source period and choose the
// phase which sees the most asserted white samples. Returns false when no
// completed frame or reliable frame-period measurement is available.
bool p2000m_capture_autotune(p2000m_capture_tuning_report_t *report);

// The returned buffer remains immutable until p2000m_capture_release_frame().
// Returns -1 when no completed frame is available.
int p2000m_capture_acquire_latest_frame(uint32_t *sequence);
void p2000m_capture_release_frame(unsigned buffer_index);
const uint32_t *p2000m_capture_buffer(unsigned buffer_index);

// The map remains allocated permanently. Load it once per rendered scanline so
// an automatic update cannot mix mappings within one source line.
const p2000m_capture_pixel_map_t *p2000m_capture_pixel_map(void);

static inline bool p2000m_capture_mapped_line_pixel_is_white(
    const uint32_t *line,
    const p2000m_capture_pixel_map_t *map,
    unsigned x) {
    const p2000m_capture_pixel_map_t *pixel = &map[x];
    bool all_samples_black =
        (line[pixel->first_word] & pixel->first_word_mask) ==
        pixel->first_word_mask;
    if (pixel->second_word_mask != 0u) {
        all_samples_black = all_samples_black &&
            (line[pixel->second_word] & pixel->second_word_mask) ==
            pixel->second_word_mask;
    }
    return !all_samples_black;
}

bool p2000m_capture_pixel_is_white(const uint32_t *frame,
                                   unsigned x, unsigned y);
void p2000m_capture_get_stats(p2000m_capture_stats_t *stats);

#endif
