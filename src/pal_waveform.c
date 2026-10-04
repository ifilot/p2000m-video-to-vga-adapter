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

uint32_t pal_waveform_clock_divider(uint32_t system_hz, unsigned raster_lines,
                                   bool near_50_hz) {
    const uint32_t sample_hz = near_50_hz && raster_lines >= 312u &&
        raster_lines <= 320u ? 50u * raster_lines * PAL_SAMPLES_PER_LINE :
        PAL_SAMPLE_RATE_HZ;
    return (uint32_t)(((uint64_t)system_hz * 256u + sample_hz / 2u) / sample_hz);
}

_Static_assert(PAL_PICTURE_END - PAL_PICTURE_START == 728,
               "PAL picture interval must retain the proven width");
_Static_assert(PAL_SOURCE_FIRST_SAMPLE + P2000M_CAPTURE_WIDTH == 831,
               "The 640 source samples must be centered in the PAL picture");
_Static_assert(PAL_SAMPLES_PER_LINE % PAL_SAMPLES_PER_WORD == 0,
               "PAL scanlines must contain complete DMA words");
_Static_assert(PAL_FIELD_SAMPLES == 280000,
               "Field 2 must begin exactly 312.5 lines after field 1");
_Static_assert(PAL_FIELD1_LAST_LINE - PAL_FIELD1_FIRST_LINE + 1 ==
                   P2000M_CAPTURE_HEIGHT &&
               PAL_FIELD2_LAST_LINE - PAL_FIELD2_FIRST_LINE + 1 ==
                   P2000M_CAPTURE_HEIGHT,
               "Both fields must retain every source row without scaling");
_Static_assert(PAL_FIELD1_LAST_LINE < 312 &&
               PAL_FIELD2_LAST_LINE < PAL_LINES_PER_FRAME,
               "Picture must finish before the next field sync sequence");

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
    uint32_t *words, unsigned line, int first, int end,
    enum pal_level level) {
    const int line_first = (int)line * PAL_SAMPLES_PER_LINE;
    const int line_end = line_first + PAL_SAMPLES_PER_LINE;
    if (end <= line_first || first >= line_end) {
        return;
    }
    const unsigned local_first = first > line_first ? (unsigned)(first - line_first) : 0u;
    const unsigned local_end = end < line_end
                                   ? (unsigned)(end - line_first)
                                   : PAL_SAMPLES_PER_LINE;
    fill_sample_range(words, local_first, local_end, level);
}

/** Replace ordinary sync with one true-interlaced field-sync sequence. */
static void PAL_TIME_CRITICAL_NOINLINE(add_field_sync)(uint32_t *words,
                                                       unsigned line,
                                                       int field_start,
                                                       unsigned advance) {
    const unsigned pre_pulses = advance < 5u ? 5u - advance : 0u;
    const int interval_end =
        field_start + (int)(pre_pulses + 10u) * PAL_HALF_LINE_SAMPLES;
    const int line_first = (int)line * PAL_SAMPLES_PER_LINE;
    const int line_end = line_first + PAL_SAMPLES_PER_LINE;
    if (interval_end <= line_first || field_start >= line_end) {
        return;
    }
    fill_frame_range(words, line, field_start, interval_end, PAL_LEVEL_BLACK);

    for (unsigned pulse = 0; pulse < pre_pulses; ++pulse) {
        const int start = field_start + (int)pulse * PAL_HALF_LINE_SAMPLES;
        fill_frame_range(words, line, start, start + PAL_EQUALISING_SAMPLES,
                         PAL_LEVEL_SYNC);
    }
    for (unsigned pulse = pre_pulses; pulse < pre_pulses + 5u; ++pulse) {
        const int start = field_start + (int)pulse * PAL_HALF_LINE_SAMPLES;
        fill_frame_range(words, line, start, start + PAL_BROAD_SYNC_SAMPLES,
                         PAL_LEVEL_SYNC);
    }
    for (unsigned pulse = pre_pulses + 5u; pulse < pre_pulses + 10u; ++pulse) {
        const int start = field_start + (int)pulse * PAL_HALF_LINE_SAMPLES;
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

/** Edge outline, scanline rulers and 24 numbered text rows, without a frame. */
static void PAL_COLD_NOINLINE(draw_test_line)(uint32_t *words,
                                             unsigned source_y) {
    const unsigned left = PAL_SOURCE_FIRST_SAMPLE;
    const unsigned right = left + P2000M_CAPTURE_WIDTH;
    if (source_y == 0u || source_y == P2000M_CAPTURE_HEIGHT - 1u) {
        fill_sample_range(words, left, right, PAL_LEVEL_WHITE);
        return;
    }
    fill_sample_range(words, left, left + 1u, PAL_LEVEL_WHITE);
    fill_sample_range(words, right - 1u, right, PAL_LEVEL_WHITE);
    // Four inset guides at known distances from each edge distinguish a
    // missing border from several missing picture lines. Mirror the guides
    // at the bottom; keep them clear of the central text and side rulers.
    const unsigned edge_distance = source_y < P2000M_CAPTURE_HEIGHT / 2u
        ? source_y : P2000M_CAPTURE_HEIGHT - 1u - source_y;
    if (edge_distance == 2u || edge_distance == 4u ||
        edge_distance == 8u || edge_distance == 12u) {
        const unsigned inset = 88u + edge_distance * 4u;
        fill_sample_range(words, left + inset, left + 216u, PAL_LEVEL_WHITE);
        fill_sample_range(words, right - 216u, right - inset, PAL_LEVEL_WHITE);
    }
    // Count individual white/black scanlines at either edge. Longer ticks
    // delimit the 12-scanline character rows.
    if (source_y % 2u == 0u) {
        const unsigned width = source_y % 12u == 0u ? 64u : 32u;
        fill_sample_range(words, left + 16u, left + 16u + width,
                          PAL_LEVEL_WHITE);
        fill_sample_range(words, right - 16u - width, right - 16u,
                          PAL_LEVEL_WHITE);
    }
    // A solid white panel exposes brightness modulation independently of
    // sampled source pixels. Beside it, fixed two-pixel stripes expose edge
    // shimmer. Keep the border/rulers and all top/bottom calibration guides.
    if (source_y >= 72u && source_y < 216u) {
        fill_sample_range(words, left + 128u, left + 448u, PAL_LEVEL_WHITE);
        for (unsigned x = 480u; x < 544u; x += 4u) {
            fill_sample_range(words, left + x, left + x + 2u, PAL_LEVEL_WHITE);
        }
        return;
    }
    const unsigned row = source_y / 12u;
    char label[] = "ROW 00   HHHHH   XXXXX   00000";
    label[4] = (char)('0' + row / 10u);
    label[5] = (char)('0' + row % 10u);
    draw_signal_lost_text(words, source_y, label, row * 12u + 2u, 1u);
}

void PAL_TIME_CRITICAL(pal_waveform_build_line_layout)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance, bool interlaced, unsigned extra_top_lines) {
    if (interlaced || extra_top_lines > 8u) extra_top_lines = 0u;
    const unsigned raster_lines = PAL_PROGRESSIVE_LINES_PER_RASTER + extra_top_lines;
    if (sync_advance > 7u) {
        sync_advance = 0u;
    }
    for (unsigned word = 0; word < PAL_WORDS_PER_LINE; ++word) {
        words[word] = 0x55555555u;
    }
    fill_sample_range(words, 0u, PAL_HSYNC_SAMPLES, PAL_LEVEL_SYNC);
    // The 3/3.5-line diagnostics move the pre-equaliser-free sequence
    // half/one line across the frame boundary. Keep signed coordinates for its head
    // and emit its wrapped tail at the end of this frame as well.
    const int early = sync_advance > 5u
        ? (int)(sync_advance - 5u) * PAL_HALF_LINE_SAMPLES : 0;
    add_field_sync(words, line, -early, sync_advance);
    const unsigned field_samples = interlaced ? PAL_FIELD_SAMPLES :
        raster_lines * PAL_SAMPLES_PER_LINE;
    const unsigned frame_samples = field_samples * 2u;
    add_field_sync(words, line, (int)field_samples - early, sync_advance);
    if (early != 0) {
        add_field_sync(words, line, (int)frame_samples - early, sync_advance);
        // The delayed final picture row would overlap this wrapped sync.
        delay_picture = false;
    }

    const unsigned first = PAL_FIELD1_FIRST_LINE + extra_top_lines -
        (delay_picture ? 0u : 1u);
    const unsigned last = first + P2000M_CAPTURE_HEIGHT - 1u;
    const unsigned second_first = interlaced ? PAL_FIELD2_FIRST_LINE :
        PAL_FIELD1_FIRST_LINE + extra_top_lines + raster_lines;
    const unsigned second_last = interlaced ? PAL_FIELD2_LAST_LINE :
        PAL_FIELD1_LAST_LINE + extra_top_lines + raster_lines;
    const unsigned advance = delay_picture ? 0u : 1u;
    unsigned source_y;
    if (line >= first && line <= last) {
        source_y = line - first;
    } else if (line >= second_first - advance &&
               line <= second_last - advance) {
        source_y = line - (second_first - advance);
    } else {
        return;
    }
    if (test_pattern) {
        draw_test_line(words, source_y);
    } else {
        draw_source_line(words, decoded_frame, source_y);
    }
}

void PAL_TIME_CRITICAL(pal_waveform_build_line_raster)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance, bool interlaced) {
    pal_waveform_build_line_layout(words, line, decoded_frame, delay_picture,
                                   test_pattern, sync_advance, interlaced, 0u);
}

void PAL_TIME_CRITICAL(pal_waveform_build_line_timed)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern,
    unsigned sync_advance) {
    pal_waveform_build_line_raster(words, line, decoded_frame, delay_picture,
                                   test_pattern, sync_advance, true);
}

void PAL_TIME_CRITICAL(pal_waveform_build_line_configured)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame, bool delay_picture, bool test_pattern) {
    pal_waveform_build_line_timed(words, line, decoded_frame, delay_picture,
                                  test_pattern, 0u);
}

void PAL_TIME_CRITICAL(pal_waveform_build_line)(
    uint32_t words[PAL_WORDS_PER_LINE], unsigned line,
    const uint32_t *decoded_frame) {
    pal_waveform_build_line_configured(words, line, decoded_frame, true, false);
}
