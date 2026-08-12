/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_PHOSPHOR_NOISE_H
#define P2000M_PHOSPHOR_NOISE_H

/**
 * @file p2000m_phosphor_noise.h
 * @brief Small deterministic helpers for RGB444 phosphor-grain rendering.
 */

#include <stdint.h>

/** User-selectable density of one-DAC-step foreground dimming. */
typedef enum {
    P2000M_PHOSPHOR_NOISE_OFF = 0,
    P2000M_PHOSPHOR_NOISE_LOW = 1,
    P2000M_PHOSPHOR_NOISE_MEDIUM = 2,
    P2000M_PHOSPHOR_NOISE_HIGH = 3,
    P2000M_PHOSPHOR_NOISE_LEVEL_COUNT = 4,
} p2000m_phosphor_noise_level_t;

/**
 * Seed one source-frame-synchronous scanline noise stream.
 *
 * Repeated VGA presentations of the same source frame receive the same seed.
 * Distinct displayed lines, including lines duplicated by 5:3 scaling, use
 * distinct streams so the effect never turns into two-pixel-high blocks.
 */
static inline uint32_t p2000m_phosphor_noise_seed(uint32_t source_sequence,
                                                  unsigned visible_y) {
    uint32_t state = source_sequence * 0x9e3779b9u;
    state ^= (uint32_t)visible_y * 0x85ebca6bu;
    state ^= 0xa511e9b3u;
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    return state != 0u ? state : 0x6d2b79f5u;
}

/** Advance the inexpensive deterministic xorshift32 stream. */
static inline uint32_t p2000m_phosphor_noise_next(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

/**
 * Produce 32 independent candidate dimming positions at the selected density.
 *
 * High uses one random bit (1/2 density), medium intersects two (1/4), and low
 * intersects three (1/8). Off consumes no random state and returns no pixels.
 */
static inline uint32_t p2000m_phosphor_noise_mask(
    uint8_t level, uint32_t *state) {
    if (level == P2000M_PHOSPHOR_NOISE_OFF ||
        level >= P2000M_PHOSPHOR_NOISE_LEVEL_COUNT) {
        return 0u;
    }

    uint32_t mask = p2000m_phosphor_noise_next(state);
    if (level <= P2000M_PHOSPHOR_NOISE_MEDIUM) {
        mask &= p2000m_phosphor_noise_next(state);
    }
    if (level == P2000M_PHOSPHOR_NOISE_LOW) {
        mask &= p2000m_phosphor_noise_next(state);
    }
    return mask;
}

/**
 * Dim a BBBBGGGGRRRR scanvideo color by approximately one RGB444 DAC step.
 *
 * Scaling every nonzero channel by 14/15 preserves hue better than subtracting
 * an unconditional packed 0x111 value. Very dark one-code channels remain lit.
 */
static inline uint16_t p2000m_phosphor_noise_dim_rgb444(uint16_t color) {
    uint16_t dimmed = 0u;
    for (unsigned shift = 0u; shift <= 8u; shift += 4u) {
        const unsigned channel = (color >> shift) & 0x0fu;
        const unsigned scaled = (channel * 14u + 7u) / 15u;
        dimmed |= (uint16_t)(scaled << shift);
    }
    return dimmed;
}

#endif
