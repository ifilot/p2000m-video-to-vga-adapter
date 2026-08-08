/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_PACKBITS_H
#define P2000M_PACKBITS_H

/**
 * @file p2000m_packbits.h
 * @brief Allocation-free PackBits codec shared by firmware and viewer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    /** Largest literal or repeated run represented by one control byte. */
    P2000M_PACKBITS_MAX_RUN = 128,
    /** Control plus the largest possible literal run. */
    P2000M_PACKBITS_MAX_CHUNK = 1 + P2000M_PACKBITS_MAX_RUN,
};

/**
 * Return the repeated-byte run length at offset, capped at 128 bytes.
 *
 * @param input Complete uncompressed input.
 * @param input_size Number of bytes in input.
 * @param offset Valid starting position within input.
 * @return Repeated run length from offset.
 */
static inline size_t p2000m_packbits_repeat_length(const uint8_t *input,
                                                   size_t input_size,
                                                   size_t offset) {
    size_t limit = input_size - offset;
    if (limit > P2000M_PACKBITS_MAX_RUN) {
        limit = P2000M_PACKBITS_MAX_RUN;
    }

    size_t length = 1u;
    while (length < limit && input[offset + length] == input[offset]) {
        ++length;
    }
    return length;
}

/**
 * Encode one independently writable PackBits chunk.
 *
 * Control bytes 0..127 introduce 1..128 literals. Values 129..255 introduce
 * 128..2 copies of the following byte; 128 is reserved as a no-op. Repeated
 * runs shorter than three bytes remain literals to avoid expanding them.
 *
 * @param input Complete uncompressed input.
 * @param input_size Number of bytes in input.
 * @param input_offset Updated past the bytes represented by this chunk.
 * @param output Optional 129-byte destination; NULL only calculates length.
 * @return Encoded chunk length, or zero when input is exhausted.
 */
static inline size_t p2000m_packbits_next_chunk(
    const uint8_t *input, size_t input_size, size_t *input_offset,
    uint8_t *output) {
    if (*input_offset >= input_size) {
        return 0u;
    }

    const size_t repeat = p2000m_packbits_repeat_length(
        input, input_size, *input_offset);
    if (repeat >= 3u) {
        if (output != NULL) {
            output[0] = (uint8_t)(257u - repeat);
            output[1] = input[*input_offset];
        }
        *input_offset += repeat;
        return 2u;
    }

    const size_t literal_start = *input_offset;
    size_t literal_length = 0u;
    while (*input_offset < input_size &&
           literal_length < P2000M_PACKBITS_MAX_RUN) {
        const size_t next_repeat = p2000m_packbits_repeat_length(
            input, input_size, *input_offset);
        if (next_repeat >= 3u) {
            break;
        }
        ++*input_offset;
        ++literal_length;
    }

    if (output != NULL) {
        output[0] = (uint8_t)(literal_length - 1u);
        memcpy(&output[1], &input[literal_start], literal_length);
    }
    return 1u + literal_length;
}

/**
 * Calculate the exact PackBits size without allocating an output buffer.
 *
 * @param input Complete uncompressed input.
 * @param input_size Number of bytes in input.
 * @return Exact encoded size in bytes.
 */
static inline size_t p2000m_packbits_encoded_size(const uint8_t *input,
                                                  size_t input_size) {
    size_t input_offset = 0u;
    size_t encoded_size = 0u;
    while (input_offset < input_size) {
        encoded_size += p2000m_packbits_next_chunk(
            input, input_size, &input_offset, NULL);
    }
    return encoded_size;
}

/**
 * Decode one complete PackBits record into a fixed-size destination.
 *
 * @param input Complete encoded record.
 * @param input_size Number of encoded bytes.
 * @param output Fixed-size uncompressed destination.
 * @param output_size Required reconstructed byte count.
 * @return true only when the entire record is structurally valid and produces
 * exactly output_size bytes.
 */
static inline bool p2000m_packbits_decode(const uint8_t *input,
                                          size_t input_size,
                                          uint8_t *output,
                                          size_t output_size) {
    size_t input_offset = 0u;
    size_t output_offset = 0u;
    while (input_offset < input_size) {
        const uint8_t control = input[input_offset++];
        if (control <= 127u) {
            const size_t count = (size_t)control + 1u;
            if (count > input_size - input_offset ||
                count > output_size - output_offset) {
                return false;
            }
            memcpy(&output[output_offset], &input[input_offset], count);
            input_offset += count;
            output_offset += count;
        } else if (control >= 129u) {
            const size_t count = 257u - (size_t)control;
            if (input_offset >= input_size ||
                count > output_size - output_offset) {
                return false;
            }
            memset(&output[output_offset], input[input_offset++], count);
            output_offset += count;
        }
        // Control 128 is the PackBits no-op and deliberately consumes no data.
    }
    return output_offset == output_size;
}

#endif
