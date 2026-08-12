/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file phosphor_noise_test.cpp
 * @brief Deterministic checks for the shared phosphor-grain primitives.
 */

#include <bitset>
#include <cstdint>
#include <cstdio>

#include "p2000m_phosphor_noise.h"

namespace {

/** Count selected noise positions over a repeatable sample of 32-bit masks. */
unsigned selectedBits(uint8_t level) {
    uint32_t state = p2000m_phosphor_noise_seed(1234u, 87u);
    unsigned selected = 0u;
    for (unsigned i = 0; i < 1024u; ++i) {
        selected += std::bitset<32>(
            p2000m_phosphor_noise_mask(level, &state)).count();
    }
    return selected;
}

}  // namespace

int main() {
    const uint32_t seed = p2000m_phosphor_noise_seed(42u, 17u);
    if (seed == 0u || seed != p2000m_phosphor_noise_seed(42u, 17u) ||
        seed == p2000m_phosphor_noise_seed(43u, 17u) ||
        seed == p2000m_phosphor_noise_seed(42u, 18u)) {
        std::fputs("Noise seeding is not deterministic and coordinate-aware\n",
                   stderr);
        return 1;
    }

    uint32_t offState = seed;
    if (p2000m_phosphor_noise_mask(P2000M_PHOSPHOR_NOISE_OFF,
                                   &offState) != 0u ||
        offState != seed) {
        std::fputs("Disabled noise changed pixels or consumed random state\n",
                   stderr);
        return 1;
    }

    const unsigned low = selectedBits(P2000M_PHOSPHOR_NOISE_LOW);
    const unsigned medium = selectedBits(P2000M_PHOSPHOR_NOISE_MEDIUM);
    const unsigned high = selectedBits(P2000M_PHOSPHOR_NOISE_HIGH);
    if (low < 3600u || low > 4600u ||
        medium < 7600u || medium > 8800u ||
        high < 15500u || high > 17300u) {
        std::fprintf(stderr,
                     "Unexpected noise densities: low=%u medium=%u high=%u\n",
                     low, medium, high);
        return 1;
    }

    if (p2000m_phosphor_noise_dim_rgb444(0x0fffu) != 0x0eeeu ||
        p2000m_phosphor_noise_dim_rgb444(0x00f0u) != 0x00e0u ||
        p2000m_phosphor_noise_dim_rgb444(0x00bfu) != 0x00aeu ||
        p2000m_phosphor_noise_dim_rgb444(0x0111u) != 0x0111u ||
        p2000m_phosphor_noise_dim_rgb444(0x0000u) != 0x0000u) {
        std::fputs("RGB444 dimming did not preserve the expected levels\n",
                   stderr);
        return 1;
    }

    return 0;
}
