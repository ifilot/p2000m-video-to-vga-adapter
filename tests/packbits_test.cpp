/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file packbits_test.cpp
 * @brief Deterministic round-trip and malformed-input tests for PackBits.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "p2000m_packbits.h"

namespace {

/** Encode and decode one byte vector, checking size analysis and equality. */
bool roundTrip(const std::vector<std::uint8_t> &input) {
    const std::size_t expectedSize =
        p2000m_packbits_encoded_size(input.data(), input.size());
    std::vector<std::uint8_t> encoded;
    encoded.reserve(expectedSize);
    std::array<std::uint8_t, P2000M_PACKBITS_MAX_CHUNK> chunk = {};
    std::size_t inputOffset = 0;
    while (inputOffset < input.size()) {
        const std::size_t chunkSize = p2000m_packbits_next_chunk(
            input.data(), input.size(), &inputOffset, chunk.data());
        encoded.insert(encoded.end(), chunk.begin(),
                       chunk.begin() + static_cast<std::ptrdiff_t>(chunkSize));
    }
    if (encoded.size() != expectedSize) {
        return false;
    }

    std::vector<std::uint8_t> decoded(input.size());
    return p2000m_packbits_decode(encoded.data(), encoded.size(),
                                  decoded.data(), decoded.size()) &&
           decoded == input;
}

}  // namespace

/** Run boundary, random-frame, and malformed-record codec checks. */
int main() {
    std::vector<std::vector<std::uint8_t>> cases;
    cases.emplace_back();
    cases.emplace_back(1, 0x00);
    cases.emplace_back(2, 0xff);
    cases.emplace_back(3, 0x55);
    cases.emplace_back(128, 0xaa);
    cases.emplace_back(129, 0xaa);
    cases.emplace_back(256, 0xaa);

    std::vector<std::uint8_t> alternating(23040);
    for (std::size_t i = 0; i < alternating.size(); ++i) {
        alternating[i] = (i & 1u) != 0u ? 0x55 : 0xaa;
    }
    cases.push_back(alternating);

    std::mt19937 generator(0x50325646u);
    std::uniform_int_distribution<unsigned> byteDistribution(0, 255);
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
        const std::size_t length = iteration < 260 ? iteration : 23040;
        std::vector<std::uint8_t> random(length);
        std::generate(random.begin(), random.end(), [&] {
            return static_cast<std::uint8_t>(byteDistribution(generator));
        });
        cases.push_back(std::move(random));
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (!roundTrip(cases[i])) {
            std::fprintf(stderr, "PackBits round-trip failed for case %zu\n", i);
            return 1;
        }
    }

    const std::array<std::uint8_t, 2> invalidLiteral = {2, 0x42};
    std::array<std::uint8_t, 4> output = {};
    if (p2000m_packbits_decode(invalidLiteral.data(), invalidLiteral.size(),
                               output.data(), output.size())) {
        std::fputs("Truncated PackBits literal was accepted\n", stderr);
        return 1;
    }

    const std::array<std::uint8_t, 1> invalidRepeat = {255};
    if (p2000m_packbits_decode(invalidRepeat.data(), invalidRepeat.size(),
                               output.data(), output.size())) {
        std::fputs("Truncated PackBits repeat was accepted\n", stderr);
        return 1;
    }
    return 0;
}
