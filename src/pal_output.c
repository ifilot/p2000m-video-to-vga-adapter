/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file pal_output.c
 * @brief DMA-driven, monochrome 625-line/50-field composite output.
 */

#include "pal_output.h"
#include "pal_demo_screens.h"

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

_Static_assert(NUM_PIOS > 2, "PAL output requires the RP2350's third PIO");

/** PIO block dedicated to composite; VGA uses PIO0 and capture fills PIO1. */
static PIO pal_pio = pio2;
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
enum { PAL_OPTION_TEST = 1u, PAL_OPTION_DELAY = 2u,
       PAL_OPTION_PROGRESSIVE = 128u, PAL_OPTION_RATE_50 = 4096u,
       PAL_CLOCK_OPTION_MASK = 0x1f80u, PAL_OPTION_PAIR_HOLD = 8192u };
// Radar remains the default fallback; saved artwork preferences may replace it.
static uint32_t requested_options = PAL_OPTION_DELAY | (1u << 5u);
static uint32_t frame_options;
static uint32_t applied_clock_options;

#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
enum { PAL_STATIC_RASTER_LINES = PAL_PROGRESSIVE_LINES_PER_RASTER + 6u };
static uint32_t static_raster[PAL_STATIC_RASTER_LINES][PAL_WORDS_PER_LINE];
static bool static_raster_active;
static uint32_t static_raster_options;

static bool use_static_raster(uint32_t options) {
    // Restrict the diagnostic to the progressive white-panel layout in use.
    const uint32_t mask = PAL_OPTION_TEST | PAL_OPTION_PROGRESSIVE | 0xf00u;
    return (options & mask) ==
        (PAL_OPTION_TEST | PAL_OPTION_PROGRESSIVE | (6u << 8u));
}
#endif

void pal_output_set_pair_hold(bool enabled) {
    if (enabled) {
        __atomic_fetch_or(&requested_options, PAL_OPTION_PAIR_HOLD, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&requested_options, ~PAL_OPTION_PAIR_HOLD, __ATOMIC_RELAXED);
    }
}

bool pal_output_pair_hold(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) &
            PAL_OPTION_PAIR_HOLD) != 0u;
}

static uint32_t clock_divider_for_options(uint32_t options) {
    const bool progressive = (options & PAL_OPTION_PROGRESSIVE) != 0u;
    const unsigned lines = PAL_PROGRESSIVE_LINES_PER_RASTER +
        ((options >> 8u) & 15u);
    return pal_waveform_clock_divider(clock_get_hz(clk_sys), lines,
        progressive && (options & PAL_OPTION_RATE_50) != 0u);
}

void pal_output_set_rate_50(bool enabled) {
    if (enabled) {
        __atomic_fetch_or(&requested_options, PAL_OPTION_RATE_50, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&requested_options, ~PAL_OPTION_RATE_50, __ATOMIC_RELAXED);
    }
}

static unsigned extra_top_lines(void) {
    return (frame_options & PAL_OPTION_PROGRESSIVE) != 0u
        ? (frame_options >> 8u) & 15u : 0u;
}

static unsigned frame_line_count(void) {
    return (frame_options & PAL_OPTION_PROGRESSIVE) != 0u
        ? 2u * (PAL_PROGRESSIVE_LINES_PER_RASTER + extra_top_lines())
        : PAL_LINES_PER_FRAME;
}

void pal_output_set_extra_top_lines(unsigned lines) {
    if (lines > 8u) return;
    uint32_t previous = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (previous & ~0xf00u) | (lines << 8u);
    } while (!__atomic_compare_exchange_n(&requested_options, &previous, next,
                 false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

unsigned pal_output_extra_top_lines(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) >> 8u) & 15u;
}

void pal_output_set_interlaced(bool enabled) {
    if (enabled) {
        __atomic_fetch_and(&requested_options, ~PAL_OPTION_PROGRESSIVE,
                          __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_or(&requested_options, PAL_OPTION_PROGRESSIVE,
                         __ATOMIC_RELAXED);
    }
}

bool pal_output_interlaced(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) &
            PAL_OPTION_PROGRESSIVE) == 0u;
}

/** Adopt an immutable decoded framebuffer for one complete field. */
static void select_field_frame(unsigned field) {
    uint32_t sequence = field_sequence;
    // Keep the provider's framebuffer hold until the next first field. Calling
    // it for field 2 could adopt a newer capture with different sampled edges.
    const uint32_t *next = field == 1u &&
        (frame_options & PAL_OPTION_PAIR_HOLD) != 0u
        ? field_frame : provide_frame(field, &sequence);
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
        frame_options = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
        output_stats.interlaced = (frame_options & PAL_OPTION_PROGRESSIVE) == 0u;
        output_stats.raster_lines = (uint16_t)(frame_line_count() / 2u);
        select_field_frame(0u);
    } else if (line == frame_line_count() / 2u) {
        // In interlaced mode field 2 starts halfway through this line;
        // progressive mode starts the next raster at its leading edge. The
        // final active line has already been copied into its DMA buffer.
        select_field_frame(1u);
    }

    // Captured video always takes priority. The saved artwork is only the
    // fallback while the frame provider reports missing source sync.
    const uint32_t *picture = field_frame;
    if (picture == NULL) {
        switch ((frame_options >> 5u) & 3u) {
            case 1u: picture = pal_demo_radar; break;
            case 2u: picture = pal_demo_circuit; break;
            case 3u: picture = pal_demo_scope; break;
            default: break; // The waveform renderer supplies the plain card.
        }
    }
    pal_waveform_build_line_layout(
        words, line, picture, (frame_options & PAL_OPTION_DELAY) != 0u,
        (frame_options & PAL_OPTION_TEST) != 0u, (frame_options >> 2u) & 7u,
        (frame_options & PAL_OPTION_PROGRESSIVE) == 0u, extra_top_lines());
}

void pal_output_set_sync_advance(unsigned half_lines) {
    if (half_lines > 7u) {
        return;
    }
    uint32_t previous = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (previous & ~28u) | (half_lines << 2u);
        if (half_lines >= 6u) {
            next &= ~PAL_OPTION_DELAY;
        }
    } while (!__atomic_compare_exchange_n(&requested_options, &previous, next,
                 false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

unsigned pal_output_sync_advance(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) >> 2u) & 7u;
}

/** Demo and calibration modes are mutually exclusive and frame-latched. */
void pal_output_set_demo(unsigned screen) {
    if (screen > 3u) return;
    uint32_t previous = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (previous & ~(96u | PAL_OPTION_TEST)) | (screen << 5u);
    } while (!__atomic_compare_exchange_n(&requested_options, &previous, next,
                 false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

unsigned pal_output_demo(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) >> 5u) & 3u;
}

void pal_output_set_test_pattern(bool enabled) {
    uint32_t previous = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (previous & ~(96u | PAL_OPTION_TEST)) |
               (enabled ? PAL_OPTION_TEST : 0u);
    } while (!__atomic_compare_exchange_n(&requested_options, &previous, next,
                 false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

bool pal_output_test_pattern_enabled(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) &
            PAL_OPTION_TEST) != 0u;
}

void pal_output_set_picture_delay(bool enabled) {
    if (enabled && pal_output_sync_advance() >= 6u) {
        return;
    }
    if (enabled) {
        __atomic_fetch_or(&requested_options, PAL_OPTION_DELAY, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&requested_options, ~PAL_OPTION_DELAY, __ATOMIC_RELAXED);
    }
}

bool pal_output_picture_delay_enabled(void) {
    return (__atomic_load_n(&requested_options, __ATOMIC_RELAXED) &
            PAL_OPTION_DELAY) != 0u;
}

/** Build the next two sequential lines into one DMA buffer. */
static void build_dma_buffer(unsigned index) {
    for (unsigned offset = 0; offset < PAL_LINES_PER_DMA_BUFFER; ++offset) {
        build_line(pal_lines[index][offset], next_line);
        next_line = (next_line + 1u) % frame_line_count();
    }
}

/** Configure one two-line channel transfer without triggering it. */
static void arm_dma(unsigned index) {
#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
    if (static_raster_active) {
        dma_channel_set_read_addr((uint)pal_dma[index], static_raster[0], false);
        dma_channel_set_trans_count((uint)pal_dma[index],
            PAL_STATIC_RASTER_LINES * PAL_WORDS_PER_LINE, false);
        return;
    }
#endif
    dma_channel_set_read_addr((uint)pal_dma[index], pal_lines[index][0], false);
    dma_channel_set_trans_count((uint)pal_dma[index],
                                PAL_WORDS_PER_DMA_BUFFER, false);
}

/** Begin a fresh output frame using both ping-pong line buffers. */
static void start_stream(void) {
    field_frame = NULL;
    field_sequence = 0u;
    next_line = 0u;
#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
    frame_options = __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    static_raster_active = use_static_raster(frame_options);
    static_raster_options = frame_options;
    output_stats.static_raster = static_raster_active;
    if (static_raster_active) {
        // Both DMA channels read the same complete raster. Nothing writes to
        // it until the stream has been stopped for a configuration change.
        for (unsigned line = 0; line < PAL_STATIC_RASTER_LINES; ++line) {
            pal_waveform_build_line_layout(static_raster[line], line, NULL,
                (frame_options & PAL_OPTION_DELAY) != 0u, true,
                (frame_options >> 2u) & 7u, false, 6u);
        }
        output_stats.interlaced = false;
        output_stats.raster_lines = PAL_STATIC_RASTER_LINES;
    } else
#endif
    {
        build_dma_buffer(0u);
        build_dma_buffer(1u);
    }
    const uint32_t divider = clock_divider_for_options(frame_options);
    pio_sm_set_clkdiv_int_frac8(pal_pio, (uint)pal_sm, divider >> 8u,
                               (uint8_t)divider);
    output_stats.clock_divider_q8 = divider;
    applied_clock_options = frame_options & PAL_CLOCK_OPTION_MASK;
    arm_dma(0u);
    arm_dma(1u);
    active_dma = 0u;
    pio_sm_clear_fifos(pal_pio, (uint)pal_sm);
    pio_sm_restart(pal_pio, (uint)pal_sm);
    pio_sm_clkdiv_restart(pal_pio, (uint)pal_sm);
    dma_start_channel_mask(1u << (uint)pal_dma[0]);
    // Prime the FIFO before the first OUT. Starting an empty state machine
    // first causes a startup stall and makes the first sync edge DMA-dependent.
    while (!pio_sm_is_tx_fifo_full(pal_pio, (uint)pal_sm)) {
        tight_loop_contents();
    }
    pal_pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + (uint)pal_sm);
    pio_sm_set_enabled(pal_pio, (uint)pal_sm, true);
    running = true;
    output_stats.running = true;
    output_stats.output_line = 0u;
}

void pal_output_initialize(pal_output_frame_provider_t frame_provider) {
    hard_assert(!initialized);
    hard_assert(frame_provider != NULL);
    hard_assert(clock_get_hz(clk_sys) == 252000000u);
    provide_frame = frame_provider;

    hard_assert(pio_can_add_program(pal_pio, &composite_program));
    pal_program_offset = pio_add_program(pal_pio, &composite_program);
    hard_assert(pal_program_offset >= 0);
    pal_sm = (int)pio_claim_unused_sm(pal_pio, true);
    gpio_set_drive_strength(PAL_PIN_BASE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PAL_PIN_LEVEL, GPIO_DRIVE_STRENGTH_12MA);
    // Diagnostic: reduce edge slew without changing DAC codes or scan timing.
    gpio_set_slew_rate(PAL_PIN_BASE, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(PAL_PIN_LEVEL, GPIO_SLEW_RATE_SLOW);
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
        // The sample clock cannot tolerate backpressure: favour these small,
        // paced transfers over bulk capture traffic in the DMA scheduler.
        channel_config_set_high_priority(&config, true);
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

void __not_in_flash_func(pal_output_service)(void) {
    if (!running) {
        return;
    }
#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
    const uint32_t options =
        __atomic_load_n(&requested_options, __ATOMIC_RELAXED);
    if ((static_raster_active && options != static_raster_options) ||
        (!static_raster_active && use_static_raster(options))) {
        dma_channel_abort((uint)pal_dma[0]);
        dma_channel_abort((uint)pal_dma[1]);
        pio_sm_set_enabled(pal_pio, (uint)pal_sm, false);
        start_stream();
        return;
    }
#endif
    const uint32_t clock_options =
        __atomic_load_n(&requested_options, __ATOMIC_RELAXED) & PAL_CLOCK_OPTION_MASK;
    if (clock_options != applied_clock_options) {
        // A clock change requires one clean restart, not divider updates during
        // active sync. Only recompute the divider when clock options change.
        if (clock_divider_for_options(clock_options) != output_stats.clock_divider_q8) {
            dma_channel_abort((uint)pal_dma[0]);
            dma_channel_abort((uint)pal_dma[1]);
            pio_sm_set_enabled(pal_pio, (uint)pal_sm, false);
            start_stream();
            return;
        }
        applied_clock_options = clock_options;
    }
    // DMA underruns only detect an exhausted pair of line buffers. A shorter
    // empty-FIFO stall can stretch sync/picture timing without either channel
    // stopping. TXSTALL is sticky, so count observations, not individual events.
    const uint32_t stall_mask =
        1u << (PIO_FDEBUG_TXSTALL_LSB + (uint)pal_sm);
    if ((pal_pio->fdebug & stall_mask) != 0u) {
        pal_pio->fdebug = stall_mask;
        ++output_stats.pio_stall_observations;
    }
    if (dma_channel_is_busy((uint)pal_dma[active_dma])) {
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

#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
    if (static_raster_active) {
        ++output_stats.generated_fields;
    } else
#endif
    {
        build_dma_buffer(completed);
    }
    arm_dma(completed);
    active_dma = following;
#if defined(P2000M_PAL_STATIC_RASTER_DIAGNOSTIC)
    if (static_raster_active) {
        const uint32_t remaining = dma_channel_hw_addr(
            (uint)pal_dma[following])->transfer_count;
        output_stats.output_line = (uint16_t)(
            (PAL_STATIC_RASTER_LINES * PAL_WORDS_PER_LINE - remaining) /
            PAL_WORDS_PER_LINE);
        return;
    }
#endif
    output_stats.output_line =
        (uint16_t)((next_line + frame_line_count() - 4u) % frame_line_count());
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
    stats->pio_stall_observations =
        __atomic_load_n(&output_stats.pio_stall_observations, __ATOMIC_RELAXED);
    stats->pause_count =
        __atomic_load_n(&output_stats.pause_count, __ATOMIC_RELAXED);
    stats->displayed_sequence =
        __atomic_load_n(&output_stats.displayed_sequence, __ATOMIC_RELAXED);
    stats->output_line =
        __atomic_load_n(&output_stats.output_line, __ATOMIC_RELAXED);
    stats->raster_lines =
        __atomic_load_n(&output_stats.raster_lines, __ATOMIC_RELAXED);
    stats->clock_divider_q8 =
        __atomic_load_n(&output_stats.clock_divider_q8, __ATOMIC_RELAXED);
    stats->running =
        __atomic_load_n(&output_stats.running, __ATOMIC_RELAXED);
    stats->interlaced =
        __atomic_load_n(&output_stats.interlaced, __ATOMIC_RELAXED);
    stats->static_raster =
        __atomic_load_n(&output_stats.static_raster, __ATOMIC_RELAXED);
}
