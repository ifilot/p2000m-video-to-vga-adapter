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
#include "pal_demo_screens.h"

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
    pal_waveform_build_line(line.data(), 24u, frame.data());
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
    pal_waveform_build_line(line.data(), 311u, frame.data());
    if (sample(line, PAL_SOURCE_FIRST_SAMPLE) != PAL_LEVEL_WHITE) {
        std::fputs("field 1 last-row mapping is incorrect\n", stderr);
        return 1;
    }
    pal_waveform_build_line(line.data(), 624u, frame.data());
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
    // Check the complete waveform, including the frame wrap after the new
    // final picture line. Picture movement must never modify a sync pulse.
    for (unsigned advance = 0u; advance <= 7u; ++advance) {
        for (unsigned delayed = 0u; delayed <= 1u; ++delayed) {
            for (unsigned frameLine = 0u; frameLine < 625u; ++frameLine) {
                pal_waveform_build_line_timed(
                    line.data(), frameLine, frame.data(), delayed != 0u, false, advance);
                const unsigned pictureDelay = advance >= 6u ? 0u : delayed;
                const bool active =
                    (frameLine >= 23u + pictureDelay && frameLine <= 310u + pictureDelay) ||
                    (frameLine >= 336u + pictureDelay && frameLine <= 623u + pictureDelay);
                for (unsigned x = 0u; x < 896u; ++x) {
                    const unsigned time = frameLine * 896u + x;
                    const unsigned fieldTime = time % 280000u;
                    bool sync = x < 66u;
                    if (advance >= 6u) {
                        const unsigned relative = (time + (advance - 5u) * 448u) % 280000u;
                        if (relative < 4480u) {
                            sync = relative % 448u <
                                (relative < 2240u ? 382u : 33u);
                        }
                    } else if (fieldTime < (15u - advance) * 448u) {
                        const unsigned pulse = fieldTime / 448u;
                        sync = fieldTime % 448u <
                            (pulse >= 5u - advance && pulse < 10u - advance ? 382u : 33u);
                    }
                    const unsigned expected = sync ? PAL_LEVEL_SYNC :
                        active && x >= 191u && x < 831u ? PAL_LEVEL_WHITE :
                        PAL_LEVEL_BLACK;
                    if (sample(line, x) != expected) {
                        std::fprintf(stderr,
                            "waveform mismatch: advance %u delay %u line %u sample %u\n",
                            advance, delayed, frameLine, x);
                        return 1;
                    }
                }
            }
        }
    }

    // Every source row must appear exactly once, in order, in each field.
    for (unsigned y = 0u; y < 288u; ++y) {
        for (unsigned word = 0u; word < 20u; ++word) {
            frame[y * 20u + word] = (0x963ca55au * (y + 1u)) ^ word;
        }
    }
    for (unsigned delayed = 0u; delayed <= 1u; ++delayed) {
        for (unsigned first : {23u + delayed, 336u + delayed}) {
            for (unsigned y = 0u; y < 288u; ++y) {
                pal_waveform_build_line_configured(
                    line.data(), first + y, frame.data(), delayed != 0u, false);
                for (unsigned x = 0u; x < 640u; ++x) {
                    const bool white = (frame[y * 20u + x / 32u] &
                                        (1u << (31u - x % 32u))) != 0u;
                    if (sample(line, 191u + x) !=
                        (white ? PAL_LEVEL_WHITE : PAL_LEVEL_BLACK)) {
                        std::fputs("source row lost or resampled\n", stderr);
                        return 1;
                    }
                }
            }
        }
    }

    // The standalone test card must work without capture and keep both edge
    // lines and identical geometry in each field for either timing setting.
    for (unsigned delayed = 0u; delayed <= 1u; ++delayed) {
        for (unsigned y = 0u; y < 288u; ++y) {
            Line second;
            pal_waveform_build_line_configured(
                line.data(), 23u + delayed + y, nullptr, delayed != 0u, true);
            pal_waveform_build_line_configured(
                second.data(), 336u + delayed + y, nullptr, delayed != 0u, true);
            if (line != second || sample(line, 191u) != PAL_LEVEL_WHITE ||
                sample(line, 830u) != PAL_LEVEL_WHITE ||
                !expectRange(line, 0u, 66u, PAL_LEVEL_SYNC) ||
                !expectRange(line, 66u, 191u, PAL_LEVEL_BLACK) ||
                !expectRange(line, 831u, 896u, PAL_LEVEL_BLACK) ||
                ((y == 0u || y == 287u) &&
                 !expectRange(line, 191u, 831u, PAL_LEVEL_WHITE))) {
                std::fputs("test card geometry is incorrect\n", stderr);
                return 1;
            }
            const unsigned distance = y < 144u ? y : 287u - y;
            const bool guide = distance == 2u || distance == 4u ||
                               distance == 8u || distance == 12u;
            for (unsigned x = 88u; x < 216u; ++x) {
                const unsigned expected =
                    distance == 0u || (guide && x >= 88u + distance * 4u)
                        ? PAL_LEVEL_WHITE : PAL_LEVEL_BLACK;
                if (sample(line, 191u + x) != expected ||
                    sample(line, 191u + 639u - x) != expected) {
                    std::fputs("edge-distance guide is incorrect\n", stderr);
                    return 1;
                }
            }
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
    // Each authored pixel must survive packed-word expansion in both fields,
    // including the working advanced-sync mode and standard timing.
    for (const auto *art : {pal_demo_radar, pal_demo_circuit, pal_demo_scope}) {
        for (unsigned mode : {0u, 7u}) {
            for (unsigned fieldStart : {23u, 336u}) {
                for (unsigned y = 0; y < 288u; ++y) {
                    pal_waveform_build_line_timed(line.data(), fieldStart + y,
                                                  art, false, false, mode);
                    for (unsigned x = 0; x < 640u; ++x) {
                        const unsigned bit = (art[y * 20u + x / 32u] >>
                                              (31u - x % 32u)) & 1u;
                        if (sample(line, PAL_SOURCE_FIRST_SAMPLE + x) !=
                            (bit ? PAL_LEVEL_WHITE : PAL_LEVEL_BLACK)) {
                            std::fputs("PAL artwork pixel mismatch\n", stderr);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
