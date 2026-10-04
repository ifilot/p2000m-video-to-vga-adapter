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
    /** Service intervals with a hardware TX stall; multiple stalls may coalesce. */
    uint32_t pio_stall_observations;
    uint32_t pause_count;
    uint32_t displayed_sequence;
    uint16_t output_line;
    uint16_t raster_lines;
    uint32_t clock_divider_q8;
    bool running;
    bool interlaced;
    /** Dedicated diagnostic build: DMA reads a precomputed immutable raster. */
    bool static_raster;
} pal_output_stats_t;

/**
 * Select the immutable decoded source frame for an upcoming PAL field.
 *
 * The callback is made on core 1 after the preceding field's final active line
 * has been copied into a DMA buffer. It may retain the pointer when no newer
 * frame is available. A null result produces a monochrome signal-loss card
 * while PAL synchronization continues.
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

/** Runtime options, adopted together at the next frame boundary.
 * Firmware save/load persists timing and artwork; the test pattern remains temporary. */
/** Persistable PAL signal-loss artwork: 0 live, 1 radar, 2 circuit, 3 scope. */
void pal_output_set_demo(unsigned screen);
unsigned pal_output_demo(void);
void pal_output_set_test_pattern(bool enabled);
bool pal_output_test_pattern_enabled(void);
void pal_output_set_picture_delay(bool enabled);
bool pal_output_picture_delay_enabled(void);
void pal_output_set_sync_advance(unsigned half_lines);
unsigned pal_output_sync_advance(void);
/** Runtime diagnostic; firmware applies the progressive boot profile. */
void pal_output_set_interlaced(bool enabled);
bool pal_output_interlaced(void);
/** Temporary extra progressive blanking; ignored by interlaced output. */
void pal_output_set_extra_top_lines(unsigned lines);
unsigned pal_output_extra_top_lines(void);
/** Temporary progressive clock adjustment; interlace retains native timing. */
void pal_output_set_rate_50(bool enabled);
/** Temporary trial: retain one immutable source frame for both scans/fields. */
void pal_output_set_pair_hold(bool enabled);
bool pal_output_pair_hold(void);
#endif
