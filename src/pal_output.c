/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file pal_output.c
 * @brief DMA-driven, monochrome 625-line/50-field composite output.
 */

#include "pal_output.h"

#include <stdbool.h>

#include "composite.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pal_waveform.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"

enum {
    PAL_PIN_BASE = 14,
    PAL_PIN_LEVEL = 15,
    PAL_PIO_CLOCK_DIVIDER = 18,
    PAL_LINES_PER_DMA_BUFFER = 2,
    PAL_WORDS_PER_DMA_BUFFER =
        PAL_LINES_PER_DMA_BUFFER * PAL_WORDS_PER_LINE,
};

static PIO pal_pio = pio1;
static int pal_sm = -1;
static int pal_program_offset = -1;
static int pal_dma[2] = {-1, -1};
/** Four lines arranged as two alternating, contiguous two-line DMA buffers. */
static uint32_t pal_lines[2][PAL_LINES_PER_DMA_BUFFER][PAL_WORDS_PER_LINE];
static unsigned active_dma;
static unsigned next_line;
static bool initialized;
static bool running;
static pal_output_frame_provider_t provide_frame;
static const uint32_t *field_frame;
static uint32_t field_sequence;
static volatile pal_output_stats_t output_stats;

/** Adopt an immutable decoded framebuffer for one complete field. */
static void select_field_frame(unsigned field) {
    uint32_t sequence = field_sequence;
    const uint32_t *next = provide_frame(field, &sequence);
    ++output_stats.generated_fields;
    if (next == NULL) {
        ++output_stats.blank_fields;
    } else if (field_frame == NULL || sequence != field_sequence) {
        ++output_stats.source_frame_swaps;
    } else {
        ++output_stats.repeated_fields;
    }
    field_frame = next;
    field_sequence = sequence;
    output_stats.displayed_sequence = sequence;
}

/** Construct one complete 64 us PAL scanline. */
static void build_line(uint32_t *words, unsigned line) {
    if (line == 0u) {
        select_field_frame(0u);
    } else if (line == 312u) {
        // The second field starts halfway through this scanline. Field 1's
        // final active line has already left the DMA FIFO before this build.
        select_field_frame(1u);
    }

    pal_waveform_build_line(words, line, field_frame);
}

/** Build the next two sequential lines into one DMA buffer. */
static void build_dma_buffer(unsigned index) {
    for (unsigned offset = 0; offset < PAL_LINES_PER_DMA_BUFFER; ++offset) {
        build_line(pal_lines[index][offset], next_line);
        next_line = (next_line + 1u) % PAL_LINES_PER_FRAME;
    }
}

/** Configure one two-line channel transfer without triggering it. */
static void arm_dma(unsigned index) {
    dma_channel_set_read_addr((uint)pal_dma[index], pal_lines[index][0], false);
    dma_channel_set_trans_count((uint)pal_dma[index],
                                PAL_WORDS_PER_DMA_BUFFER, false);
}

/** Begin a fresh output frame using both ping-pong line buffers. */
static void start_stream(void) {
    field_frame = NULL;
    field_sequence = 0u;
    next_line = 0u;
    build_dma_buffer(0u);
    build_dma_buffer(1u);
    arm_dma(0u);
    arm_dma(1u);
    active_dma = 0u;
    pio_sm_clear_fifos(pal_pio, (uint)pal_sm);
    pio_sm_restart(pal_pio, (uint)pal_sm);
    pio_sm_set_enabled(pal_pio, (uint)pal_sm, true);
    dma_start_channel_mask(1u << (uint)pal_dma[0]);
    running = true;
    output_stats.running = true;
    output_stats.output_line = 0u;
}

void pal_output_initialize(pal_output_frame_provider_t frame_provider) {
    hard_assert(!initialized);
    hard_assert(frame_provider != NULL);
    hard_assert(clock_get_hz(clk_sys) == 252000000u);
    provide_frame = frame_provider;

    pal_program_offset = pio_add_program(pal_pio, &composite_program);
    pal_sm = (int)pio_claim_unused_sm(pal_pio, true);
    gpio_set_drive_strength(PAL_PIN_BASE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PAL_PIN_LEVEL, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(PAL_PIN_BASE, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PAL_PIN_LEVEL, GPIO_SLEW_RATE_FAST);
    composite_program_init(pal_pio, (uint)pal_sm, (uint)pal_program_offset,
                           PAL_PIN_BASE, PAL_PIO_CLOCK_DIVIDER);

    pal_dma[0] = dma_claim_unused_channel(true);
    pal_dma[1] = dma_claim_unused_channel(true);
    for (unsigned index = 0; index < 2u; ++index) {
        dma_channel_config config =
            dma_channel_get_default_config((uint)pal_dma[index]);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
        channel_config_set_read_increment(&config, true);
        channel_config_set_write_increment(&config, false);
        channel_config_set_dreq(
            &config, pio_get_dreq(pal_pio, (uint)pal_sm, true));
        channel_config_set_chain_to(&config, (uint)pal_dma[index ^ 1u]);
        dma_channel_configure((uint)pal_dma[index], &config,
                              &pal_pio->txf[pal_sm], pal_lines[index][0],
                              PAL_WORDS_PER_DMA_BUFFER, false);
    }

    bi_decl(bi_2pins_with_names(PAL_PIN_BASE, "PAL COMPOSITE BIT 0",
                                PAL_PIN_LEVEL, "PAL COMPOSITE BIT 1"));
    initialized = true;
}

void pal_output_start(void) {
    hard_assert(initialized);
    if (!running) {
        start_stream();
    }
}

void pal_output_service(void) {
    if (!running || dma_channel_is_busy((uint)pal_dma[active_dma])) {
        return;
    }

    const unsigned completed = active_dma;
    const unsigned following = active_dma ^ 1u;
    if (!dma_channel_is_busy((uint)pal_dma[following])) {
        // Core 1 failed to re-arm a channel within the following two lines.
        // Restart at an unambiguous frame boundary so the monitor can relock.
        ++output_stats.dma_underruns;
        dma_channel_abort((uint)pal_dma[0]);
        dma_channel_abort((uint)pal_dma[1]);
        pio_sm_set_enabled(pal_pio, (uint)pal_sm, false);
        start_stream();
        return;
    }

    build_dma_buffer(completed);
    arm_dma(completed);
    active_dma = following;
    output_stats.output_line =
        (uint16_t)((next_line + PAL_LINES_PER_FRAME - 4u) %
                   PAL_LINES_PER_FRAME);
}

void pal_output_stop(void) {
    if (!running) {
        return;
    }
    running = false;
    output_stats.running = false;
    ++output_stats.pause_count;
    dma_channel_abort((uint)pal_dma[0]);
    dma_channel_abort((uint)pal_dma[1]);
    pio_sm_set_enabled(pal_pio, (uint)pal_sm, false);
    pio_sm_clear_fifos(pal_pio, (uint)pal_sm);
    // 01 is the black/blanking DAC code. Holding black is less disruptive than
    // leaving the last arbitrary picture sample on the output pins.
    pio_sm_set_pins_with_mask(pal_pio, (uint)pal_sm,
                              1u << PAL_PIN_BASE,
                              3u << PAL_PIN_BASE);
}

void pal_output_get_stats(pal_output_stats_t *stats) {
    hard_assert(stats != NULL);
    stats->generated_fields =
        __atomic_load_n(&output_stats.generated_fields, __ATOMIC_RELAXED);
    stats->source_frame_swaps =
        __atomic_load_n(&output_stats.source_frame_swaps, __ATOMIC_RELAXED);
    stats->repeated_fields =
        __atomic_load_n(&output_stats.repeated_fields, __ATOMIC_RELAXED);
    stats->blank_fields =
        __atomic_load_n(&output_stats.blank_fields, __ATOMIC_RELAXED);
    stats->dma_underruns =
        __atomic_load_n(&output_stats.dma_underruns, __ATOMIC_RELAXED);
    stats->pause_count =
        __atomic_load_n(&output_stats.pause_count, __ATOMIC_RELAXED);
    stats->displayed_sequence =
        __atomic_load_n(&output_stats.displayed_sequence, __ATOMIC_RELAXED);
    stats->output_line =
        __atomic_load_n(&output_stats.output_line, __ATOMIC_RELAXED);
    stats->running =
        __atomic_load_n(&output_stats.running, __ATOMIC_RELAXED);
}
