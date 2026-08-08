/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_CAPTURE_H
#define P2000M_CAPTURE_H

/**
 * @file p2000m_capture.h
 * @brief Public interface for P2000M raw-video capture and resampling.
 */

#include <stdbool.h>
#include <stdint.h>

enum {
    /** Resampled monochrome pixels in each visible source line. */
    P2000M_CAPTURE_WIDTH = 640,
    /** Visible source lines: 24 character rows times 12 scanlines. */
    P2000M_CAPTURE_HEIGHT = 288,
    /** Uniform PIO input sampling rate. */
    P2000M_CAPTURE_SAMPLE_CLOCK_HZ = 63000000,
    /** Useful input bits packed into each DMA word. */
    P2000M_CAPTURE_SAMPLES_PER_WORD = 28,
    /** PIO cycles represented by a word, including two branch gaps. */
    P2000M_CAPTURE_TICKS_PER_WORD = 30,
    /** Packed oversampling words captured for each visible line. */
    P2000M_CAPTURE_WORDS_PER_LINE = 114,
    /** Useful VIDEO samples stored for each visible line. */
    P2000M_CAPTURE_SAMPLES_PER_LINE =
        P2000M_CAPTURE_WORDS_PER_LINE * P2000M_CAPTURE_SAMPLES_PER_WORD,
    /** Packed words in one complete oversampled frame. */
    P2000M_CAPTURE_WORDS_PER_FRAME =
        P2000M_CAPTURE_WORDS_PER_LINE * P2000M_CAPTURE_HEIGHT,
    /** Byte size of one complete oversampled frame. */
    P2000M_CAPTURE_BYTES_PER_FRAME =
        P2000M_CAPTURE_WORDS_PER_FRAME * sizeof(uint32_t),
    /** Number of adjacent automatic phase positions evaluated per tune. */
    P2000M_CAPTURE_TUNING_CANDIDATES = 5,
};

/** Three-sample decode mask for one output pixel, spanning at most two words. */
typedef struct {
    /** Mask applied to first_word; a cleared input bit represents white. */
    uint32_t first_word_mask;
    /** Optional mask in second_word when the window crosses a word boundary. */
    uint32_t second_word_mask;
    /** Index of the first packed capture word. */
    uint16_t first_word;
    /** Index of the optional second packed capture word. */
    uint16_t second_word;
} p2000m_capture_pixel_map_t;

/** Snapshot of capture, synchronization, and automatic-tuning telemetry. */
typedef struct {
    /** Complete raw frames captured since boot. */
    uint32_t captured_frames;
    /** Ready frames overwritten because no consumer needed the older data. */
    uint32_t stale_frames_replaced;
    /** Most recently measured VSYNC-to-VSYNC period in microseconds. */
    uint32_t last_frame_period_us;
    /** Recovered line period in 16.16 fixed-point 63 MHz ticks. */
    uint32_t recovered_line_ticks_q16;
    /** Successful automatic tuning passes since boot. */
    uint32_t autotune_runs;
    /** White-sample score at the selected phase during the latest tune. */
    uint32_t autotune_score;
    /** Duration of the latest successful tuning pass in microseconds. */
    uint32_t last_autotune_us;
    /** Longest successful tuning pass observed since boot. */
    uint32_t maximum_autotune_us;
    /** Automatically selected phase in 63 MHz capture ticks. */
    int32_t auto_phase_ticks;
    /** User trim added to auto_phase_ticks. */
    int32_t manual_phase_ticks;
    /** Whether complete frames are still arriving at a credible source rate. */
    bool signal_present;
} p2000m_capture_stats_t;

/** Detailed scores produced by a requested automatic phase-tuning pass. */
typedef struct {
    /** Capture sequence number used for scoring. */
    uint32_t sequence;
    /** Score for each consecutive candidate phase. */
    uint32_t scores[P2000M_CAPTURE_TUNING_CANDIDATES];
    /** Phase tick represented by scores[0]. */
    int32_t first_candidate_tick;
    /** Phase selected after confidence and hysteresis checks. */
    int32_t selected_phase_tick;
} p2000m_capture_tuning_report_t;

/**
 * @brief Initialize and start continuous P2000M capture.
 *
 * @return Nothing.
 */
void p2000m_capture_start(void);

/**
 * @brief Apply a manual trim without disabling automatic phase recovery.
 *
 * @param phase_ticks Trim from -4 through +4 in 63 MHz ticks (15.87 ns each).
 * @return true when accepted; false when phase_ticks is outside the range.
 */
bool p2000m_capture_set_sample_phase(int phase_ticks);

/**
 * @brief Recover the horizontal dot grid and select its best sampling phase.
 *
 * @param report Optional destination for per-candidate scores; may be NULL.
 * @return true after a successful tune; false without a locked complete frame.
 */
bool p2000m_capture_autotune(p2000m_capture_tuning_report_t *report);

/**
 * @brief Report whether valid HSYNC/VSYNC-driven frames are still arriving.
 *
 * A short watchdog bridges isolated missed frames. Lock is restored only after
 * two consecutive frame completions establish a credible source period.
 *
 * @return true while the input timing is locked; false at startup or timeout.
 */
bool p2000m_capture_signal_present(void);

/**
 * @brief Acquire the newest complete immutable raw capture buffer.
 *
 * @param sequence Receives the capture sequence number.
 * @return Buffer index, or -1 when no completed frame is available.
 */
int p2000m_capture_acquire_latest_frame(uint32_t *sequence);

/**
 * @brief Release a raw buffer previously acquired by the caller.
 *
 * @param buffer_index Index returned by p2000m_capture_acquire_latest_frame().
 * @return Nothing.
 */
void p2000m_capture_release_frame(unsigned buffer_index);

/**
 * @brief Obtain the read-only storage address for a capture buffer.
 *
 * @param buffer_index Valid raw capture buffer index.
 * @return Pointer to P2000M_CAPTURE_WORDS_PER_FRAME packed words.
 */
const uint32_t *p2000m_capture_buffer(unsigned buffer_index);

/**
 * @brief Obtain the current immutable horizontal resampling map.
 *
 * @return Pointer to P2000M_CAPTURE_WIDTH pixel descriptors.
 */
const p2000m_capture_pixel_map_t *p2000m_capture_pixel_map(void);

/**
 * @brief Decode one mapped output pixel from an oversampled raw line.
 *
 * @param line Start of a packed raw source line.
 * @param map Resampling map held constant for this decoding operation.
 * @param x Zero-based output pixel coordinate.
 * @return true for foreground/white; false for background/black.
 */
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

/**
 * @brief Decode one pixel from a complete raw frame using the current map.
 *
 * @param frame Start of a complete raw capture buffer.
 * @param x Zero-based horizontal pixel coordinate.
 * @param y Zero-based visible source-line coordinate.
 * @return true for foreground/white; false for background/black.
 */
bool p2000m_capture_pixel_is_white(const uint32_t *frame,
                                   unsigned x, unsigned y);

/**
 * @brief Copy a consistent snapshot of all capture telemetry.
 *
 * @param stats Destination structure filled by the function.
 * @return Nothing.
 */
void p2000m_capture_get_stats(p2000m_capture_stats_t *stats);

#endif
