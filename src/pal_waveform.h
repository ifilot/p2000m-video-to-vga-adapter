/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PAL_WAVEFORM_H
#define PAL_WAVEFORM_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PAL_SAMPLE_RATE_HZ = 14000000,
    PAL_SAMPLES_PER_LINE = 896,
    PAL_HALF_LINE_SAMPLES = 448,
    PAL_LINES_PER_FRAME = 625,
    /** Diagnostic: two identical-phase 312-line rasters, about 50.080 Hz. */
    PAL_PROGRESSIVE_LINES_PER_RASTER = 312,
    PAL_PROGRESSIVE_LINES_PER_PAIR = 624,
    PAL_FRAME_SAMPLES = PAL_SAMPLES_PER_LINE * PAL_LINES_PER_FRAME,
    PAL_FIELD_SAMPLES = PAL_FRAME_SAMPLES / 2,
    PAL_SAMPLES_PER_WORD = 16,
    PAL_WORDS_PER_LINE = PAL_SAMPLES_PER_LINE / PAL_SAMPLES_PER_WORD,
    PAL_HSYNC_SAMPLES = 66,
    PAL_EQUALISING_SAMPLES = 33,
    PAL_BROAD_SYNC_SAMPLES = 382,
    PAL_PICTURE_START = 147,
    PAL_PICTURE_END = 875,
    PAL_SOURCE_FIRST_SAMPLE = PAL_PICTURE_START + 44,
    /** One line later than the original layout; retain all 288 source rows. */
    PAL_FIELD1_FIRST_LINE = 24,
    PAL_FIELD1_LAST_LINE = 311,
    PAL_FIELD2_FIRST_LINE = 337,
    PAL_FIELD2_LAST_LINE = 624,
    /** Signal-loss card geometry in 640 x 288 source coordinates. */
    PAL_SIGNAL_LOST_PANEL_LEFT = 90,
    PAL_SIGNAL_LOST_PANEL_RIGHT = 550,
    PAL_SIGNAL_LOST_PANEL_TOP = 48,
    PAL_SIGNAL_LOST_PANEL_BOTTOM = 240,
    PAL_SIGNAL_LOST_PANEL_BORDER = 3,
    PAL_SIGNAL_LOST_PRODUCT_TOP = 78,
    PAL_SIGNAL_LOST_MESSAGE_TOP = 118,
    PAL_SIGNAL_LOST_WAITING_TOP = 180,
};

enum pal_level {
    PAL_LEVEL_SYNC = 0x0,
    PAL_LEVEL_BLACK = 0x1,
    PAL_LEVEL_WHITE = 0x3,
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build one packed 896-sample line from an optional decoded framebuffer.
 *
 * A null framebuffer renders the monochrome signal-loss card on active lines.
 */
void pal_waveform_build_line(uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
                             const uint32_t *decoded_frame);

/** Diagnostic variant: false delay_picture restores the original position. */
void pal_waveform_build_line_configured(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern);

/** Diagnostic sync advance (0..7 half-lines); 6/7 force original picture position. */
void pal_waveform_build_line_timed(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance);

/** Experimental 312-line progressive output; interlaced=true retains 625/50. */
void pal_waveform_build_line_raster(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance, bool interlaced);

/** Progressive-only extra top blanking (0..8); extends each raster equally. */
void pal_waveform_build_line_layout(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance, bool interlaced, unsigned extra_top_lines);

/** Rounded 16.8 PIO divider: native sample rate or near-50 Hz progressive. */
uint32_t pal_waveform_clock_divider(uint32_t system_hz, unsigned raster_lines,
                                   bool near_50_hz);

#ifdef __cplusplus
}
#endif

#endif
