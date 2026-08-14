/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PAL_OUTPUT_H
#define PAL_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

/** Runtime counters for the continuous monochrome 625/50 output. */
typedef struct {
    uint32_t generated_fields;
    uint32_t source_frame_swaps;
    uint32_t repeated_fields;
    uint32_t blank_fields;
    uint32_t dma_underruns;
    uint32_t pause_count;
    uint32_t displayed_sequence;
    uint16_t output_line;
    bool running;
} pal_output_stats_t;

/**
 * Select the immutable decoded source frame for an upcoming PAL field.
 *
 * The callback is made on core 1 after the preceding field's final active line
 * has completed. It may retain the preceding pointer when no newer frame is
 * available. A null result produces a black field.
 */
typedef const uint32_t *(*pal_output_frame_provider_t)(unsigned field,
                                                       uint32_t *sequence);

/** Claim the PAL PIO/DMA resources and register the framebuffer provider. */
void pal_output_initialize(pal_output_frame_provider_t frame_provider);

/** Start a fresh, line-zero 625-line waveform. */
void pal_output_start(void);

/** Poll DMA completion and prepare the next pair of scanlines. */
void pal_output_service(void);

/** Stop DMA/PIO cleanly and hold the analogue output at black level. */
void pal_output_stop(void);

/** Copy a coherent snapshot of the output counters. */
void pal_output_get_stats(pal_output_stats_t *stats);

#endif
