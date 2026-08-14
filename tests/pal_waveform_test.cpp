/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <cstdint>
#include <cstdio>

#include "p2000m_capture.h"
#include "p2000m_signal_loss.h"
#include "pal_waveform.h"

namespace {

using Line = std::array<std::uint32_t, PAL_WORDS_PER_LINE>;
using Frame = std::array<std::uint32_t,
                         P2000M_CAPTURE_WIDTH * P2000M_CAPTURE_HEIGHT / 32>;

unsigned sample(const Line &line, unsigned position) {
    return (line[position / 16u] >> ((position % 16u) * 2u)) & 3u;
}

bool expectRange(const Line &line, unsigned first, unsigned end,
                 unsigned level) {
    for (unsigned position = first; position < end; ++position) {
        if (sample(line, position) != level) {
            std::fprintf(stderr, "sample %u: got %u, expected %u\n", position,
                         sample(line, position), level);
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    Line line = {};
    pal_waveform_build_line(line.data(), 8u, nullptr);
    if (!expectRange(line, 0u, 66u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 66u, PAL_SAMPLES_PER_LINE, PAL_LEVEL_BLACK)) {
        return 1;
    }

    pal_waveform_build_line(line.data(), 0u, nullptr);
    if (!expectRange(line, 0u, 33u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 33u, 448u, PAL_LEVEL_BLACK) ||
        !expectRange(line, 448u, 481u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 481u, 896u, PAL_LEVEL_BLACK)) {
        return 1;
    }

    pal_waveform_build_line(line.data(), 2u, nullptr);
    if (!expectRange(line, 0u, 33u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 33u, 448u, PAL_LEVEL_BLACK) ||
        !expectRange(line, 448u, 830u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 830u, 896u, PAL_LEVEL_BLACK)) {
        return 1;
    }

    pal_waveform_build_line(line.data(), 5u, nullptr);
    if (!expectRange(line, 0u, 33u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 33u, 448u, PAL_LEVEL_BLACK) ||
        !expectRange(line, 448u, 481u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 481u, 896u, PAL_LEVEL_BLACK)) {
        return 1;
    }

    pal_waveform_build_line(line.data(), 312u, nullptr);
    if (!expectRange(line, 0u, 66u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 66u, 448u, PAL_LEVEL_BLACK) ||
        !expectRange(line, 448u, 481u, PAL_LEVEL_SYNC) ||
        !expectRange(line, 481u, 896u, PAL_LEVEL_BLACK)) {
        return 1;
    }

    const unsigned panelLeft =
        PAL_SOURCE_FIRST_SAMPLE + PAL_SIGNAL_LOST_PANEL_LEFT;
    const unsigned panelRight =
        PAL_SOURCE_FIRST_SAMPLE + PAL_SIGNAL_LOST_PANEL_RIGHT;
    pal_waveform_build_line(
        line.data(), PAL_FIELD1_FIRST_LINE + PAL_SIGNAL_LOST_PANEL_TOP,
        nullptr);
    if (sample(line, panelLeft - 1u) != PAL_LEVEL_BLACK ||
        !expectRange(line, panelLeft, panelRight, PAL_LEVEL_WHITE) ||
        sample(line, panelRight) != PAL_LEVEL_BLACK) {
        std::fputs("signal-loss panel top border is incorrect\n", stderr);
        return 1;
    }

    pal_waveform_build_line(
        line.data(), PAL_FIELD1_FIRST_LINE + PAL_SIGNAL_LOST_PANEL_TOP +
                         PAL_SIGNAL_LOST_PANEL_BORDER,
        nullptr);
    if (!expectRange(line, panelLeft,
                     panelLeft + PAL_SIGNAL_LOST_PANEL_BORDER,
                     PAL_LEVEL_WHITE) ||
        sample(line, panelLeft + PAL_SIGNAL_LOST_PANEL_BORDER) !=
            PAL_LEVEL_BLACK ||
        sample(line, panelRight - PAL_SIGNAL_LOST_PANEL_BORDER - 1u) !=
            PAL_LEVEL_BLACK ||
        !expectRange(line, panelRight - PAL_SIGNAL_LOST_PANEL_BORDER,
                     panelRight, PAL_LEVEL_WHITE)) {
        std::fputs("signal-loss panel side borders are incorrect\n", stderr);
        return 1;
    }

    constexpr unsigned messageLength =
        sizeof(P2000M_SIGNAL_LOSS_MESSAGE) - 1u;
    constexpr unsigned messageScale = 4u;
    const unsigned messageWidth =
        ((P2000M_SIGNAL_LOSS_GLYPH_WIDTH + 1u) * messageLength - 1u) *
        messageScale;
    const unsigned messageLeft =
        PAL_SOURCE_FIRST_SAMPLE +
        (P2000M_CAPTURE_WIDTH - messageWidth) / 2u;
    pal_waveform_build_line(
        line.data(), PAL_FIELD1_FIRST_LINE + PAL_SIGNAL_LOST_MESSAGE_TOP,
        nullptr);
    if (sample(line, messageLeft) != PAL_LEVEL_BLACK ||
        !expectRange(line, messageLeft + messageScale,
                     messageLeft + 4u * messageScale, PAL_LEVEL_WHITE) ||
        sample(line, messageLeft + 4u * messageScale) != PAL_LEVEL_BLACK) {
        std::fputs("signal-loss message is not rendered as expected\n",
                   stderr);
        return 1;
    }

    const Line fieldOneLossLine = line;
    pal_waveform_build_line(
        line.data(), PAL_FIELD2_FIRST_LINE + PAL_SIGNAL_LOST_MESSAGE_TOP,
        nullptr);
    if (line != fieldOneLossLine) {
        std::fputs("signal-loss card differs between PAL fields\n", stderr);
        return 1;
    }

    Frame frame = {};
    frame[0] = 0x80000000u;
    frame[P2000M_CAPTURE_WIDTH / 32u - 1u] = 0x00000001u;
    pal_waveform_build_line(line.data(), 23u, frame.data());
    if (sample(line, 190u) != PAL_LEVEL_BLACK ||
        sample(line, 191u) != PAL_LEVEL_WHITE ||
        sample(line, 192u) != PAL_LEVEL_BLACK ||
        sample(line, 830u) != PAL_LEVEL_WHITE ||
        sample(line, 831u) != PAL_LEVEL_BLACK) {
        std::fputs("source-pixel centering or bit order is incorrect\n", stderr);
        return 1;
    }

    frame.fill(0u);
    const unsigned lastRowWord =
        (P2000M_CAPTURE_HEIGHT - 1u) * (P2000M_CAPTURE_WIDTH / 32u);
    frame[lastRowWord] = 0x80000000u;
    pal_waveform_build_line(line.data(), 310u, frame.data());
    if (sample(line, PAL_SOURCE_FIRST_SAMPLE) != PAL_LEVEL_WHITE) {
        std::fputs("field 1 last-row mapping is incorrect\n", stderr);
        return 1;
    }
    pal_waveform_build_line(line.data(), 623u, frame.data());
    if (sample(line, PAL_SOURCE_FIRST_SAMPLE) != PAL_LEVEL_WHITE) {
        std::fputs("field 2 last-row mapping is incorrect\n", stderr);
        return 1;
    }

    for (unsigned word = 0u; word < P2000M_CAPTURE_WIDTH / 32u; ++word) {
        frame[word] = 0x963ca55au ^ (0x11111111u * word);
    }
    pal_waveform_build_line(line.data(), PAL_FIELD1_FIRST_LINE, frame.data());
    for (unsigned x = 0u; x < P2000M_CAPTURE_WIDTH; ++x) {
        const bool source_white =
            (frame[x / 32u] & (1u << (31u - x % 32u))) != 0u;
        const unsigned expected = source_white ? PAL_LEVEL_WHITE
                                               : PAL_LEVEL_BLACK;
        if (sample(line, PAL_SOURCE_FIRST_SAMPLE + x) != expected) {
            std::fprintf(stderr, "packed source expansion failed at pixel %u\n",
                         x);
            return 1;
        }
    }

    frame.fill(0xffffffffu);
    for (unsigned frameLine = 0; frameLine < PAL_LINES_PER_FRAME; ++frameLine) {
        pal_waveform_build_line(line.data(), frameLine, frame.data());
        for (unsigned position = 0; position < PAL_SAMPLES_PER_LINE; ++position) {
            if (sample(line, position) == 2u) {
                std::fprintf(stderr, "forbidden 10 code at line %u sample %u\n",
                             frameLine, position);
                return 1;
            }
        }
    }

    for (unsigned frameLine = 0; frameLine < PAL_LINES_PER_FRAME;
         ++frameLine) {
        pal_waveform_build_line(line.data(), frameLine, nullptr);
        for (unsigned position = 0; position < PAL_SAMPLES_PER_LINE;
             ++position) {
            if (sample(line, position) == 2u) {
                std::fprintf(stderr,
                             "forbidden 10 code in signal-loss card at line "
                             "%u sample %u\n",
                             frameLine, position);
                return 1;
            }
        }
    }
    return 0;
}
