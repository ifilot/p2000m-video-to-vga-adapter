/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <cstdint>
#include <cstdio>

#include "p2000m_capture.h"
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
    return 0;
}
