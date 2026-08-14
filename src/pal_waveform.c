/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pal_waveform.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "p2000m_capture.h"
#include "p2000m_signal_loss.h"

#if defined(PICO_ON_DEVICE)
#include "pico.h"
#define PAL_TIME_CRITICAL(name) __not_in_flash_func(name)
#define PAL_TIME_CRITICAL_NOINLINE(name) __no_inline_not_in_flash_func(name)
#define PAL_TIME_CRITICAL_DATA(group) __not_in_flash(group)
#else
#define PAL_TIME_CRITICAL(name) name
#define PAL_TIME_CRITICAL_NOINLINE(name) __attribute__((noinline)) name
#define PAL_TIME_CRITICAL_DATA(group)
#endif

#define PAL_COLD_NOINLINE(name) __attribute__((noinline)) name

_Static_assert(PAL_PICTURE_END - PAL_PICTURE_START == 728,
               "PAL picture interval must retain the proven width");
_Static_assert(PAL_SOURCE_FIRST_SAMPLE + P2000M_CAPTURE_WIDTH == 831,
               "The 640 source samples must be centered in the PAL picture");
_Static_assert(PAL_SAMPLES_PER_LINE % PAL_SAMPLES_PER_WORD == 0,
               "PAL scanlines must contain complete DMA words");
_Static_assert(PAL_FIELD_SAMPLES == 280000,
               "Field 2 must begin exactly 312.5 lines after field 1");

/** Replace a local scanline range with one repeated two-bit PAL level. */
static void PAL_TIME_CRITICAL_NOINLINE(fill_sample_range)(
    uint32_t *words, unsigned first, unsigned end, enum pal_level level) {
    if (first >= end) {
        return;
    }

    const uint32_t pattern = (uint32_t)level * 0x55555555u;
    const unsigned first_word = first / PAL_SAMPLES_PER_WORD;
    const unsigned last_word = (end - 1u) / PAL_SAMPLES_PER_WORD;
    for (unsigned word = first_word; word <= last_word; ++word) {
        const unsigned word_first = word * PAL_SAMPLES_PER_WORD;
        const unsigned local_first = first > word_first
                                         ? first - word_first
                                         : 0u;
        const unsigned word_end = word_first + PAL_SAMPLES_PER_WORD;
        const unsigned local_end = end < word_end
                                       ? end - word_first
                                       : PAL_SAMPLES_PER_WORD;
        const unsigned first_bit = local_first * 2u;
        const unsigned end_bit = local_end * 2u;
        const uint32_t low_mask = UINT32_MAX << first_bit;
        const uint32_t high_mask = end_bit == 32u
                                       ? UINT32_MAX
                                       : (1u << end_bit) - 1u;
        const uint32_t mask = low_mask & high_mask;
        words[word] = (words[word] & ~mask) | (pattern & mask);
    }
}

/** Fill the intersection of a frame-coordinate range and one scanline. */
static void PAL_TIME_CRITICAL_NOINLINE(fill_frame_range)(
    uint32_t *words, unsigned line, unsigned first, unsigned end,
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
    fill_sample_range(words, local_first, local_end, level);
}

/** Replace ordinary sync with one true-interlaced field-sync sequence. */
static void PAL_TIME_CRITICAL_NOINLINE(add_field_sync)(uint32_t *words,
                                                       unsigned line,
                                                       unsigned field_start) {
    const unsigned interval_end =
        field_start + 15u * PAL_HALF_LINE_SAMPLES;
    const unsigned line_first = line * PAL_SAMPLES_PER_LINE;
    const unsigned line_end = line_first + PAL_SAMPLES_PER_LINE;
    if (interval_end <= line_first || field_start >= line_end) {
        return;
    }
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

/** Expand four source bits into the high bits of four two-bit PAL samples. */
static const uint8_t PAL_TIME_CRITICAL_DATA("pal_white_nibble")
    white_nibble[16] = {
    0x00u, 0x80u, 0x20u, 0xa0u,
    0x08u, 0x88u, 0x28u, 0xa8u,
    0x02u, 0x82u, 0x22u, 0xa2u,
    0x0au, 0x8au, 0x2au, 0xaau,
};

/** Expand eight source bits in display order into eight packed PAL samples. */
static inline uint16_t expand_source_byte(uint8_t pixels) {
    return (uint16_t)white_nibble[pixels >> 4u] |
           (uint16_t)((uint16_t)white_nibble[pixels & 0x0fu] << 8u);
}

/** Expand sixteen source bits in display order into one PAL sample word. */
static inline uint32_t expand_source_halfword(uint16_t pixels) {
    return (uint32_t)expand_source_byte((uint8_t)(pixels >> 8u)) |
           ((uint32_t)expand_source_byte((uint8_t)pixels) << 16u);
}

/** Merge sixteen white-level bits at the unaligned source-picture origin. */
static inline void merge_source_halfword(uint32_t *words, unsigned word,
                                         uint32_t expanded) {
    enum {
        SOURCE_SHIFT = (PAL_SOURCE_FIRST_SAMPLE % PAL_SAMPLES_PER_WORD) * 2u,
    };
    words[word] |= expanded << SOURCE_SHIFT;
    words[word + 1u] |= expanded >> (32u - SOURCE_SHIFT);
}

/** Draw one centered status line in 640-pixel source coordinates. */
static void PAL_COLD_NOINLINE(draw_signal_lost_text)(
    uint32_t *words, unsigned source_y, const char *message, unsigned top,
    unsigned scale) {
    if (source_y < top ||
        source_y >= top + P2000M_SIGNAL_LOSS_GLYPH_HEIGHT * scale) {
        return;
    }

    const size_t length = strlen(message);
    if (length == 0u) {
        return;
    }
    const unsigned text_width =
        (unsigned)(((P2000M_SIGNAL_LOSS_GLYPH_WIDTH + 1u) * length - 1u) *
                   scale);
    const unsigned text_left = (P2000M_CAPTURE_WIDTH - text_width) / 2u;
    const unsigned glyph_row = (source_y - top) / scale;

    for (size_t character = 0u; character < length; ++character) {
        const uint8_t row =
            p2000m_signal_loss_glyph_row(message[character], glyph_row);
        const unsigned character_x = text_left + (unsigned)character *
            (P2000M_SIGNAL_LOSS_GLYPH_WIDTH + 1u) * scale;
        for (unsigned column = 0u;
             column < P2000M_SIGNAL_LOSS_GLYPH_WIDTH; ++column) {
            if ((row & (0x10u >> column)) == 0u) {
                continue;
            }
            const unsigned first = PAL_SOURCE_FIRST_SAMPLE + character_x +
                                   column * scale;
            fill_sample_range(words, first, first + scale, PAL_LEVEL_WHITE);
        }
    }
}

/** Draw the PAL warning procedurally, avoiding another full framebuffer. */
static void PAL_COLD_NOINLINE(draw_signal_lost_line)(uint32_t *words,
                                                      unsigned source_y) {
    if (source_y < PAL_SIGNAL_LOST_PANEL_TOP ||
        source_y >= PAL_SIGNAL_LOST_PANEL_BOTTOM) {
        return;
    }

    const unsigned panel_left =
        PAL_SOURCE_FIRST_SAMPLE + PAL_SIGNAL_LOST_PANEL_LEFT;
    const unsigned panel_right =
        PAL_SOURCE_FIRST_SAMPLE + PAL_SIGNAL_LOST_PANEL_RIGHT;
    const bool horizontal_border =
        source_y < PAL_SIGNAL_LOST_PANEL_TOP + PAL_SIGNAL_LOST_PANEL_BORDER ||
        source_y >=
            PAL_SIGNAL_LOST_PANEL_BOTTOM - PAL_SIGNAL_LOST_PANEL_BORDER;
    if (horizontal_border) {
        fill_sample_range(words, panel_left, panel_right, PAL_LEVEL_WHITE);
    } else {
        fill_sample_range(words, panel_left,
                          panel_left + PAL_SIGNAL_LOST_PANEL_BORDER,
                          PAL_LEVEL_WHITE);
        fill_sample_range(words,
                          panel_right - PAL_SIGNAL_LOST_PANEL_BORDER,
                          panel_right, PAL_LEVEL_WHITE);
    }

    draw_signal_lost_text(words, source_y, P2000M_SIGNAL_LOSS_PRODUCT,
                          PAL_SIGNAL_LOST_PRODUCT_TOP, 2u);
    draw_signal_lost_text(words, source_y, P2000M_SIGNAL_LOSS_MESSAGE,
                          PAL_SIGNAL_LOST_MESSAGE_TOP, 4u);
    draw_signal_lost_text(words, source_y, P2000M_SIGNAL_LOSS_WAITING,
                          PAL_SIGNAL_LOST_WAITING_TOP, 1u);
}

/** Expand one packed 640-pixel decoded source row into composite samples. */
static void PAL_TIME_CRITICAL_NOINLINE(draw_source_line)(
    uint32_t *words, const uint32_t *decoded_frame, unsigned source_y) {
    if (decoded_frame == NULL) {
        draw_signal_lost_line(words, source_y);
        return;
    }
    const uint32_t *source =
        decoded_frame + source_y * (P2000M_CAPTURE_WIDTH / 32u);
    unsigned destination = PAL_SOURCE_FIRST_SAMPLE / PAL_SAMPLES_PER_WORD;
    for (unsigned source_word = 0u;
         source_word < P2000M_CAPTURE_WIDTH / 32u; ++source_word) {
        const uint32_t pixels = source[source_word];
        merge_source_halfword(
            words, destination++,
            expand_source_halfword((uint16_t)(pixels >> 16u)));
        merge_source_halfword(
            words, destination++,
            expand_source_halfword((uint16_t)pixels));
    }
}

void PAL_TIME_CRITICAL(pal_waveform_build_line)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame) {
    for (unsigned word = 0; word < PAL_WORDS_PER_LINE; ++word) {
        words[word] = 0x55555555u;
    }
    fill_sample_range(words, 0u, PAL_HSYNC_SAMPLES, PAL_LEVEL_SYNC);
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
