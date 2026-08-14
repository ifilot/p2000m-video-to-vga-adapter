/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pal_waveform.h"

#include <stddef.h>
#include <string.h>

#include "p2000m_capture.h"

_Static_assert(PAL_PICTURE_END - PAL_PICTURE_START == 728,
               "PAL picture interval must retain the proven width");
_Static_assert(PAL_SOURCE_FIRST_SAMPLE + P2000M_CAPTURE_WIDTH == 831,
               "The 640 source samples must be centered in the PAL picture");
_Static_assert(PAL_SAMPLES_PER_LINE % PAL_SAMPLES_PER_WORD == 0,
               "PAL scanlines must contain complete DMA words");
_Static_assert(PAL_FIELD_SAMPLES == 280000,
               "Field 2 must begin exactly 312.5 lines after field 1");

/** Store one two-bit sample in a packed scanline. */
static inline void set_line_sample(uint32_t *words, unsigned sample,
                                   enum pal_level level) {
    const unsigned word = sample >> 4u;
    const unsigned shift = (sample & 15u) << 1u;
    words[word] = (words[word] & ~(0x3u << shift)) |
                  ((uint32_t)level << shift);
}

/** Fill the intersection of a frame-coordinate range and one scanline. */
static void fill_frame_range(uint32_t *words, unsigned line,
                             unsigned first, unsigned end,
                             enum pal_level level) {
    const unsigned line_first = line * PAL_SAMPLES_PER_LINE;
    const unsigned line_end = line_first + PAL_SAMPLES_PER_LINE;
    if (end <= line_first || first >= line_end) {
        return;
    }
    const unsigned local_first = first > line_first ? first - line_first : 0u;
    const unsigned local_end = end < line_end
                                   ? end - line_first
                                   : PAL_SAMPLES_PER_LINE;
    for (unsigned sample = local_first; sample < local_end; ++sample) {
        set_line_sample(words, sample, level);
    }
}

/** Replace ordinary sync with one true-interlaced field-sync sequence. */
static void add_field_sync(uint32_t *words, unsigned line,
                           unsigned field_start) {
    const unsigned interval_end =
        field_start + 15u * PAL_HALF_LINE_SAMPLES;
    fill_frame_range(words, line, field_start, interval_end, PAL_LEVEL_BLACK);

    for (unsigned pulse = 0; pulse < 5u; ++pulse) {
        const unsigned start = field_start + pulse * PAL_HALF_LINE_SAMPLES;
        fill_frame_range(words, line, start, start + PAL_EQUALISING_SAMPLES,
                         PAL_LEVEL_SYNC);
    }
    for (unsigned pulse = 5u; pulse < 10u; ++pulse) {
        const unsigned start = field_start + pulse * PAL_HALF_LINE_SAMPLES;
        fill_frame_range(words, line, start, start + PAL_BROAD_SYNC_SAMPLES,
                         PAL_LEVEL_SYNC);
    }
    for (unsigned pulse = 10u; pulse < 15u; ++pulse) {
        const unsigned start = field_start + pulse * PAL_HALF_LINE_SAMPLES;
        fill_frame_range(words, line, start, start + PAL_EQUALISING_SAMPLES,
                         PAL_LEVEL_SYNC);
    }
}

/** Expand one packed 640-pixel decoded source row into composite samples. */
static void draw_source_line(uint32_t *words, const uint32_t *decoded_frame,
                             unsigned source_y) {
    if (decoded_frame == NULL) {
        return;
    }
    const uint32_t *source =
        decoded_frame + source_y * (P2000M_CAPTURE_WIDTH / 32u);
    for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; ++x) {
        if ((source[x >> 5u] & (1u << (31u - (x & 31u)))) != 0u) {
            set_line_sample(words, PAL_SOURCE_FIRST_SAMPLE + x,
                            PAL_LEVEL_WHITE);
        }
    }
}

void pal_waveform_build_line(uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
                             const uint32_t *decoded_frame) {
    for (unsigned word = 0; word < PAL_WORDS_PER_LINE; ++word) {
        words[word] = 0x55555555u;
    }
    for (unsigned sample = 0; sample < PAL_HSYNC_SAMPLES; ++sample) {
        set_line_sample(words, sample, PAL_LEVEL_SYNC);
    }
    add_field_sync(words, line, 0u);
    add_field_sync(words, line, PAL_FIELD_SAMPLES);

    if (line >= PAL_FIELD1_FIRST_LINE && line <= PAL_FIELD1_LAST_LINE) {
        draw_source_line(words, decoded_frame,
                         line - PAL_FIELD1_FIRST_LINE);
    } else if (line >= PAL_FIELD2_FIRST_LINE &&
               line <= PAL_FIELD2_LAST_LINE) {
        draw_source_line(words, decoded_frame,
                         line - PAL_FIELD2_FIRST_LINE);
    }
}
