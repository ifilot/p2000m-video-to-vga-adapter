/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000m_capture.c
 * @brief PIO/DMA acquisition and line-locked software resampling for P2000M.
 */

#include "p2000m_capture.h"

#include <stdint.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "p2000m_capture.pio.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"

enum {
    /** Clock required for exact 63 MHz PIO sampling and 25.2 MHz VGA. */
    REQUIRED_SYSTEM_CLOCK_HZ = 126000000,

    /** Conditioned, active-low monochrome video input. */
    VIDEO_PIN = 16,
    /** Conditioned, active-low horizontal synchronization input. */
    HSYNC_PIN = 17,
    /** Conditioned, active-low vertical synchronization input. */
    VSYNC_PIN = 18,

    /** Raw buffers used to decouple DMA capture from software consumers. */
    CAPTURE_BUFFER_COUNT = 3,
    /** Initial frame-line count plus one sample-group count per visible line. */
    TX_COMMAND_COUNT = 1 + P2000M_CAPTURE_HEIGHT,
    /** PIO y-loop initializer for 288 lines. */
    FRAME_LINE_COUNT = P2000M_CAPTURE_HEIGHT - 1,
    /** Fourteen-sample PIO groups captured from each visible source line. */
    LINE_SAMPLE_GROUP_COUNT = 228,
    /** PIO x-loop initializer for 228 groups. */
    LINE_SAMPLE_GROUP_COUNT_MINUS_ONE = LINE_SAMPLE_GROUP_COUNT - 1,

    /** Earliest supported user trim in capture ticks. */
    MANUAL_PHASE_MIN = -4,
    /** Latest supported user trim in capture ticks. */
    MANUAL_PHASE_MAX = 4,
    /** First of the five automatic phase candidates. */
    TUNING_FIRST_PHASE_TICK = 12,
    /** Safe phase used before the first content-based tune. */
    TUNING_INITIAL_PHASE_TICK = 14,
    /** Nominal 64 microsecond line at the 63 MHz capture clock. */
    NOMINAL_LINE_TICKS = 4032,
    /** Complete P2000M horizontal period expressed in source dot periods. */
    SOURCE_DOTS_PER_LINE = 768,
    /** Early and late samples taken around every mapped pixel center. */
    TUNING_WINDOW_RADIUS_TICKS = 1,
};

/** Ownership states for one entry in the triple raw-frame buffer. */
typedef enum {
    /** Available as the next DMA destination. */
    BUFFER_FREE,
    /** Currently being written by capture DMA. */
    BUFFER_FILLING,
    /** Complete and available for a consumer. */
    BUFFER_READY,
    /** Held immutable by one software consumer. */
    BUFFER_IN_USE,
} buffer_state_t;

_Static_assert(P2000M_CAPTURE_SAMPLES_PER_WORD == 28,
               "PIO packing assumes 28 useful samples per word");
_Static_assert(P2000M_CAPTURE_TICKS_PER_WORD == 30,
               "PIO timing assumes two branch cycles per packed word");
_Static_assert(P2000M_CAPTURE_WORDS_PER_LINE * 2 == LINE_SAMPLE_GROUP_COUNT,
               "Each FIFO word must contain two sample groups");
_Static_assert(REQUIRED_SYSTEM_CLOCK_HZ / 2u ==
                   P2000M_CAPTURE_SAMPLE_CLOCK_HZ,
               "Capture PIO must use an integer divide-by-two clock");

/** PIO block reserved for input capture; scanvideo uses PIO0. */
static PIO capture_pio = pio1;
/** Claimed state-machine number within capture_pio. */
static unsigned capture_sm;
/** Instruction-memory offset of p2000m_capture_program. */
static unsigned capture_program_offset;
/** DMA channel moving PIO RX FIFO data into capture buffers. */
static int capture_rx_dma;
/** DMA channel feeding per-line loop counts to the PIO TX FIFO. */
static int capture_tx_dma;
/** Lock protecting raw-buffer ownership and capture statistics. */
static spin_lock_t *buffer_lock;

/** Triple-buffered packed oversampling data. */
static uint32_t capture_buffers[CAPTURE_BUFFER_COUNT][P2000M_CAPTURE_WORDS_PER_FRAME];
/** DMA-fed PIO loop counts: frame line count followed by line group counts. */
static uint32_t tx_commands[TX_COMMAND_COUNT];
/** Double-buffered horizontal maps, atomically exchanged after rebuilding. */
static p2000m_capture_pixel_map_t
    pixel_maps[2][P2000M_CAPTURE_WIDTH];

/** Ownership state corresponding to each capture_buffers entry. */
static buffer_state_t buffer_states[CAPTURE_BUFFER_COUNT];
/** Monotonic frame sequence corresponding to each capture buffer. */
static uint32_t buffer_sequences[CAPTURE_BUFFER_COUNT];
/** Buffer currently receiving capture DMA data. */
static unsigned capture_fill_index;
/** Number of completed raw frames since boot. */
static uint32_t captured_frames;
/** Number of unread ready frames replaced by newer capture data. */
static uint32_t stale_frames_replaced;
/** Latest measured frame period in microseconds. */
static uint32_t last_frame_period_us;
/** Completion timestamp used to derive last_frame_period_us. */
static uint64_t last_frame_time_us;
/** Low-pass-filtered frame period in 24.8 fixed-point microseconds. */
static uint32_t filtered_frame_period_us_q8;
/** Recovered line period in 16.16 fixed-point capture ticks. */
static uint32_t recovered_line_ticks_q16 = NOMINAL_LINE_TICKS << 16;
/** Number of successful content-based phase tuning passes. */
static uint32_t autotune_runs;
/** White-sample score at the currently selected phase. */
static uint32_t autotune_score;
/** Duration of the latest tuning pass in microseconds. */
static uint32_t last_autotune_us;
/** Longest tuning pass observed since boot in microseconds. */
static uint32_t maximum_autotune_us;
/** Phase selected automatically, in 63 MHz capture ticks. */
static int32_t auto_phase_ticks = TUNING_INITIAL_PHASE_TICK;
/** User-controlled trim added to auto_phase_ticks. */
static int32_t manual_phase_ticks;
/** Index of the pixel map visible to consumers. */
static unsigned active_pixel_map;

/** Packed-word address and bit mask corresponding to one PIO capture cycle. */
typedef struct {
    /** Word offset from the beginning of the raw scanline. */
    uint16_t word;
    /** Single useful sample bit within word. */
    uint32_t mask;
} raw_sample_location_t;

/**
 * @brief Translate a PIO line-capture cycle into its packed DMA bit.
 *
 * The two known loop-branch gaps in every 30-cycle word are mapped to their
 * nearest real sample so callers never address padding bits.
 *
 * @param cycle Zero-based 63 MHz cycle relative to the capture window.
 * @return Packed word index and one-bit mask for the nearest captured sample.
 */
static raw_sample_location_t capture_cycle_to_location(int cycle) {
    const int last_cycle =
        P2000M_CAPTURE_WORDS_PER_LINE * P2000M_CAPTURE_TICKS_PER_WORD - 2;
    if (cycle < 0) {
        cycle = 0;
    } else if (cycle > last_cycle) {
        cycle = last_cycle;
    }

    unsigned word = (unsigned)cycle / P2000M_CAPTURE_TICKS_PER_WORD;
    const unsigned tick = (unsigned)cycle % P2000M_CAPTURE_TICKS_PER_WORD;
    unsigned sample;

    if (tick <= 13u) {
        sample = tick;
    } else if (tick == 14u) {
        sample = 13u;
    } else if (tick <= 28u) {
        sample = tick - 1u;
    } else if (word + 1u < P2000M_CAPTURE_WORDS_PER_LINE) {
        ++word;
        sample = 0;
    } else {
        sample = P2000M_CAPTURE_SAMPLES_PER_WORD - 1u;
    }

    raw_sample_location_t location = {
        .word = (uint16_t)word,
        .mask = 1u << (P2000M_CAPTURE_SAMPLES_PER_WORD - 1u - sample),
    };
    return location;
}

/**
 * @brief Add one raw sample to a three-sample output-pixel descriptor.
 *
 * @param pixel Descriptor being assembled.
 * @param location Packed raw sample to merge into the descriptor.
 * @return Nothing.
 */
static void add_sample_to_pixel_map(p2000m_capture_pixel_map_t *pixel,
                                    raw_sample_location_t location) {
    if (pixel->first_word_mask == 0u ||
        pixel->first_word == location.word) {
        pixel->first_word = location.word;
        pixel->first_word_mask |= location.mask;
        return;
    }

    if (pixel->second_word_mask == 0u ||
        pixel->second_word == location.word) {
        pixel->second_word = location.word;
        pixel->second_word_mask |= location.mask;
        return;
    }

    // A three-tick window can cross at most one packed-word boundary.
    hard_assert(false);
}

/**
 * @brief Build all 640 resampling descriptors for a recovered source grid.
 *
 * @param map Destination array of P2000M_CAPTURE_WIDTH descriptors.
 * @param line_ticks_q16 Recovered line period in 16.16 capture ticks.
 * @param phase_ticks Sampling phase relative to the PIO window start.
 * @return Nothing.
 */
static void build_pixel_map(p2000m_capture_pixel_map_t *map,
                            uint32_t line_ticks_q16,
                            int phase_ticks) {
    const uint32_t dot_ticks_q16 = line_ticks_q16 / SOURCE_DOTS_PER_LINE;
    int64_t target_q16 = (int64_t)phase_ticks << 16;

    for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; ++x) {
        p2000m_capture_pixel_map_t *pixel = &map[x];
        *pixel = (p2000m_capture_pixel_map_t){0};

        const int target_cycle =
            (int)((target_q16 + (1 << 15)) >> 16);
        for (int offset = -TUNING_WINDOW_RADIUS_TICKS;
             offset <= TUNING_WINDOW_RADIUS_TICKS; ++offset) {
            add_sample_to_pixel_map(
                pixel, capture_cycle_to_location(target_cycle + offset));
        }
        target_q16 += dot_ticks_q16;
    }
}

/**
 * @brief Rebuild the inactive map and publish it atomically to consumers.
 *
 * @param line_ticks_q16 Recovered line period in 16.16 capture ticks.
 * @param automatic_phase Automatically selected phase in capture ticks.
 * @param manual_trim User trim added to automatic_phase.
 * @return Nothing.
 */
static void publish_pixel_map(uint32_t line_ticks_q16,
                              int automatic_phase,
                              int manual_trim) {
    const unsigned inactive =
        __atomic_load_n(&active_pixel_map, __ATOMIC_RELAXED) ^ 1u;
    build_pixel_map(pixel_maps[inactive], line_ticks_q16,
                    automatic_phase + manual_trim);
    __atomic_store_n(&active_pixel_map, inactive, __ATOMIC_RELEASE);
}

/**
 * @brief Score how many pixel centers read as foreground at one phase.
 *
 * @param frame Complete immutable raw capture buffer.
 * @param line_ticks_q16 Recovered line period in 16.16 capture ticks.
 * @param phase_ticks Candidate sampling phase.
 * @return Number of asserted, active-low foreground center samples.
 */
static uint32_t calculate_phase_score(const uint32_t *frame,
                                      uint32_t line_ticks_q16,
                                      int phase_ticks) {
    const uint32_t dot_ticks_q16 = line_ticks_q16 / SOURCE_DOTS_PER_LINE;
    int64_t target_q16 = (int64_t)phase_ticks << 16;
    uint32_t score = 0;

    for (unsigned x = 0; x < P2000M_CAPTURE_WIDTH; ++x) {
        const int target_cycle =
            (int)((target_q16 + (1 << 15)) >> 16);
        const raw_sample_location_t location =
            capture_cycle_to_location(target_cycle);
        for (unsigned y = 0; y < P2000M_CAPTURE_HEIGHT; ++y) {
            const uint32_t word =
                frame[y * P2000M_CAPTURE_WORDS_PER_LINE + location.word];
            if ((word & location.mask) == 0u) {
                ++score;
            }
        }
        target_q16 += dot_ticks_q16;
    }
    return score;
}

/**
 * @brief Arm synchronized RX and TX DMA transfers for one complete frame.
 *
 * @param buffer_index Raw destination buffer to fill.
 * @return Nothing.
 */
static void arm_capture_dma(unsigned buffer_index) {
    dma_channel_set_write_addr(capture_rx_dma, capture_buffers[buffer_index], false);
    dma_channel_set_trans_count(capture_rx_dma, P2000M_CAPTURE_WORDS_PER_FRAME, false);
    dma_channel_set_read_addr(capture_tx_dma, tx_commands, false);
    dma_channel_set_trans_count(capture_tx_dma, TX_COMMAND_COUNT, false);
    dma_start_channel_mask((1u << capture_rx_dma) | (1u << capture_tx_dma));
}

/**
 * @brief Select the next DMA destination while buffer_lock is held.
 *
 * A free buffer is preferred. Otherwise, the oldest unclaimed ready frame is
 * safely replaced so acquisition never stalls behind a slow consumer.
 *
 * @return Index of the buffer that capture DMA may overwrite next.
 */
static unsigned choose_next_fill_buffer(void) {
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_FREE) {
            return i;
        }
    }

    unsigned oldest = CAPTURE_BUFFER_COUNT;
    uint32_t oldest_sequence = UINT32_MAX;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY &&
            buffer_sequences[i] < oldest_sequence) {
            oldest = i;
            oldest_sequence = buffer_sequences[i];
        }
    }

    // The frame which just completed is READY, so at least that buffer can be
    // reused even if a consumer temporarily holds both old and new frames.
    hard_assert(oldest < CAPTURE_BUFFER_COUNT);
    ++stale_frames_replaced;
    return oldest;
}

/**
 * @brief Complete one raw frame and restart DMA on another buffer.
 *
 * The IRQ also timestamps frames and advances their sequence numbers while
 * holding buffer_lock.
 *
 * @return Nothing.
 */
static void capture_dma_irq(void) {
    dma_hw->ints1 = 1u << capture_rx_dma;
    const uint64_t now = time_us_64();

    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (last_frame_time_us != 0) {
        last_frame_period_us = (uint32_t)(now - last_frame_time_us);
    }
    last_frame_time_us = now;

    const unsigned completed = capture_fill_index;
    const uint32_t sequence = ++captured_frames;
    buffer_sequences[completed] = sequence;
    buffer_states[completed] = BUFFER_READY;

    const unsigned next = choose_next_fill_buffer();
    capture_fill_index = next;
    buffer_states[next] = BUFFER_FILLING;
    arm_capture_dma(next);
    spin_unlock(buffer_lock, saved);
}

/**
 * @brief Load and configure the PIO1 state machine for 63 MHz input capture.
 *
 * @return Nothing.
 */
static void initialize_capture_pio(void) {
    capture_program_offset = pio_add_program(capture_pio, &p2000m_capture_program);
    capture_sm = pio_claim_unused_sm(capture_pio, true);

    for (unsigned pin = VIDEO_PIN; pin <= VSYNC_PIN; ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
    }

    pio_sm_config config =
        p2000m_capture_program_get_default_config(capture_program_offset);
    sm_config_set_in_pins(&config, VIDEO_PIN);
    // Shifting left places the first of 28 samples at bit 27. The upper four
    // bits are ignored; two fourteen-sample groups make every FIFO word.
    sm_config_set_in_shift(&config, false, true,
                           P2000M_CAPTURE_SAMPLES_PER_WORD);
    sm_config_set_clkdiv_int_frac8(&config, 2, 0);  // 126 MHz / 2 = 63 MHz

    pio_sm_set_consecutive_pindirs(capture_pio, capture_sm, VIDEO_PIN, 3, false);
    pio_sm_init(capture_pio, capture_sm, capture_program_offset, &config);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
}

/**
 * @brief Claim and configure the PIO RX and command TX DMA channels.
 *
 * @return Nothing.
 */
static void initialize_capture_dma(void) {
    capture_rx_dma = dma_claim_unused_channel(true);
    capture_tx_dma = dma_claim_unused_channel(true);

    dma_channel_config rx_config = dma_channel_get_default_config(capture_rx_dma);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_32);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, pio_get_dreq(capture_pio, capture_sm, false));
    dma_channel_configure(capture_rx_dma, &rx_config,
                          capture_buffers[0], &capture_pio->rxf[capture_sm],
                          P2000M_CAPTURE_WORDS_PER_FRAME, false);

    dma_channel_config tx_config = dma_channel_get_default_config(capture_tx_dma);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_32);
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, pio_get_dreq(capture_pio, capture_sm, true));
    dma_channel_configure(capture_tx_dma, &tx_config,
                          &capture_pio->txf[capture_sm], tx_commands,
                          TX_COMMAND_COUNT, false);

    dma_channel_set_irq1_enabled(capture_rx_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_1, capture_dma_irq);
    irq_set_priority(DMA_IRQ_1, 0x80);
    irq_set_enabled(DMA_IRQ_1, true);
}

/**
 * @brief Initialize all capture state and start continuous acquisition.
 *
 * @return Nothing.
 */
void p2000m_capture_start(void) {
    bi_decl(bi_3pins_with_names(VIDEO_PIN, "P2000M VIDEO_IN",
                                HSYNC_PIN, "P2000M HSYNC_IN",
                                VSYNC_PIN, "P2000M VSYNC_IN"));

    const int lock_number = spin_lock_claim_unused(true);
    buffer_lock = spin_lock_instance((unsigned)lock_number);

    tx_commands[0] = FRAME_LINE_COUNT;
    for (unsigned i = 1; i < TX_COMMAND_COUNT; ++i) {
        tx_commands[i] = LINE_SAMPLE_GROUP_COUNT_MINUS_ONE;
    }

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        buffer_states[i] = BUFFER_FREE;
        buffer_sequences[i] = 0;
    }
    capture_fill_index = 0;
    buffer_states[0] = BUFFER_FILLING;

    build_pixel_map(pixel_maps[0], recovered_line_ticks_q16,
                    auto_phase_ticks + manual_phase_ticks);
    active_pixel_map = 0;

    initialize_capture_pio();
    initialize_capture_dma();
    arm_capture_dma(0);
    pio_sm_set_enabled(capture_pio, capture_sm, true);
}

/**
 * @brief Apply a bounded manual trim and atomically rebuild the pixel map.
 *
 * @param phase_ticks Requested trim from -4 through +4 capture ticks.
 * @return true when accepted; false when outside the supported range.
 */
bool p2000m_capture_set_sample_phase(int phase_ticks) {
    if (phase_ticks < MANUAL_PHASE_MIN || phase_ticks > MANUAL_PHASE_MAX) {
        return false;
    }

    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (phase_ticks == manual_phase_ticks) {
        spin_unlock(buffer_lock, saved);
        return true;
    }
    const uint32_t line_ticks_q16 = recovered_line_ticks_q16;
    const int automatic_phase = auto_phase_ticks;
    spin_unlock(buffer_lock, saved);

    publish_pixel_map(line_ticks_q16, automatic_phase, phase_ticks);

    const uint32_t update_saved = spin_lock_blocking(buffer_lock);
    manual_phase_ticks = phase_ticks;
    spin_unlock(buffer_lock, update_saved);
    return true;
}

/**
 * @brief Claim the newest complete capture buffer for immutable reading.
 *
 * @param sequence Receives the claimed frame's monotonic sequence number.
 * @return Buffer index, or -1 when no completed frame is ready.
 */
int p2000m_capture_acquire_latest_frame(uint32_t *sequence) {
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    int latest = -1;
    uint32_t latest_sequence = 0;

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY &&
            buffer_sequences[i] >= latest_sequence) {
            latest = (int)i;
            latest_sequence = buffer_sequences[i];
        }
    }

    if (latest >= 0) {
        buffer_states[latest] = BUFFER_IN_USE;
        *sequence = latest_sequence;
    }
    spin_unlock(buffer_lock, saved);
    return latest;
}

/**
 * @brief Return a claimed raw frame to the capture buffer pool.
 *
 * @param buffer_index Index previously returned by the acquire function.
 * @return Nothing.
 */
void p2000m_capture_release_frame(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (buffer_states[buffer_index] == BUFFER_IN_USE) {
        buffer_states[buffer_index] = BUFFER_FREE;
    }
    spin_unlock(buffer_lock, saved);
}

/**
 * @brief Return the storage address for a valid raw capture buffer.
 *
 * @param buffer_index Capture buffer index.
 * @return Read-only pointer to the packed frame storage.
 */
const uint32_t *p2000m_capture_buffer(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    return capture_buffers[buffer_index];
}

/**
 * @brief Load the currently published horizontal resampling map.
 *
 * @return Read-only pointer to 640 pixel descriptors.
 */
const p2000m_capture_pixel_map_t *p2000m_capture_pixel_map(void) {
    const unsigned index =
        __atomic_load_n(&active_pixel_map, __ATOMIC_ACQUIRE);
    return pixel_maps[index];
}

/**
 * @brief Recover line period and content-based sampling phase from one frame.
 *
 * @param report Optional destination for detailed candidate scores; may be
 *               NULL for a background maintenance tune.
 * @return true on success; false without a complete frame or credible period.
 */
bool p2000m_capture_autotune(p2000m_capture_tuning_report_t *report) {
    uint32_t sequence;
    const int buffer_index = p2000m_capture_acquire_latest_frame(&sequence);
    if (buffer_index < 0) {
        return false;
    }
    const uint64_t tune_started = time_us_64();

    const uint32_t saved = spin_lock_blocking(buffer_lock);
    const uint32_t period_us = last_frame_period_us;
    const uint32_t previous_runs = autotune_runs;
    const int previous_phase = auto_phase_ticks;
    const int manual_trim = manual_phase_ticks;
    const uint32_t previous_filtered_period = filtered_frame_period_us_q8;
    spin_unlock(buffer_lock, saved);

    if (period_us < 19000u || period_us > 21000u) {
        p2000m_capture_release_frame((unsigned)buffer_index);
        return false;
    }

    uint32_t filtered_period_q8 = previous_filtered_period;
    if (filtered_period_q8 == 0u) {
        filtered_period_q8 = period_us << 8;
    } else {
        const int32_t period_error =
            (int32_t)(period_us << 8) - (int32_t)filtered_period_q8;
        filtered_period_q8 =
            (uint32_t)((int32_t)filtered_period_q8 + period_error / 8);
    }
    const uint32_t line_ticks_q16 =
        (uint32_t)(((uint64_t)filtered_period_q8 * 63u << 8) / 312u);
    const uint32_t *frame = capture_buffers[buffer_index];
    uint32_t scores[P2000M_CAPTURE_TUNING_CANDIDATES];
    uint32_t best_score = 0;
    unsigned best_index = 0;

    for (unsigned i = 0; i < P2000M_CAPTURE_TUNING_CANDIDATES; ++i) {
        const int candidate = TUNING_FIRST_PHASE_TICK + (int)i;
        scores[i] = calculate_phase_score(frame, line_ticks_q16, candidate);

        const int best_candidate =
            TUNING_FIRST_PHASE_TICK + (int)best_index;
        const unsigned candidate_distance =
            (unsigned)(candidate > previous_phase
                           ? candidate - previous_phase
                           : previous_phase - candidate);
        const unsigned best_distance =
            (unsigned)(best_candidate > previous_phase
                           ? best_candidate - previous_phase
                           : previous_phase - best_candidate);
        if (scores[i] > best_score ||
            (scores[i] == best_score && candidate_distance < best_distance)) {
            best_score = scores[i];
            best_index = i;
        }
    }

    int selected_phase = TUNING_FIRST_PHASE_TICK + (int)best_index;
    // A blank or almost blank source frame does not carry enough information
    // to justify moving a previously established phase lock.
    if (previous_runs != 0u && best_score < 256u) {
        selected_phase = previous_phase;
        if (previous_phase >= TUNING_FIRST_PHASE_TICK &&
            previous_phase < TUNING_FIRST_PHASE_TICK +
                                 P2000M_CAPTURE_TUNING_CANDIDATES) {
            best_score = scores[(unsigned)(previous_phase -
                                           TUNING_FIRST_PHASE_TICK)];
        }
    }
    if (previous_runs != 0u &&
        previous_phase >= TUNING_FIRST_PHASE_TICK &&
        previous_phase < TUNING_FIRST_PHASE_TICK +
                             P2000M_CAPTURE_TUNING_CANDIDATES) {
        const uint32_t previous_score =
            scores[(unsigned)(previous_phase - TUNING_FIRST_PHASE_TICK)];
        const uint32_t hysteresis = previous_score / 200u + 16u;
        if (best_score <= previous_score + hysteresis) {
            selected_phase = previous_phase;
            best_score = previous_score;
        }
    }

    publish_pixel_map(line_ticks_q16, selected_phase, manual_trim);
    const uint32_t tune_us = (uint32_t)(time_us_64() - tune_started);

    const uint32_t update_saved = spin_lock_blocking(buffer_lock);
    filtered_frame_period_us_q8 = filtered_period_q8;
    recovered_line_ticks_q16 = line_ticks_q16;
    auto_phase_ticks = selected_phase;
    autotune_score = best_score;
    last_autotune_us = tune_us;
    if (tune_us > maximum_autotune_us) {
        maximum_autotune_us = tune_us;
    }
    ++autotune_runs;
    spin_unlock(buffer_lock, update_saved);

    if (report != NULL) {
        report->sequence = sequence;
        report->first_candidate_tick = TUNING_FIRST_PHASE_TICK;
        report->selected_phase_tick = selected_phase;
        for (unsigned i = 0; i < P2000M_CAPTURE_TUNING_CANDIDATES; ++i) {
            report->scores[i] = scores[i];
        }
    }

    p2000m_capture_release_frame((unsigned)buffer_index);
    return true;
}

/**
 * @brief Decode one resampled pixel from a complete raw frame.
 *
 * @param frame Start of a complete packed capture buffer.
 * @param x Zero-based horizontal output coordinate.
 * @param y Zero-based visible source-line coordinate.
 * @return true for foreground/white; false for background/black.
 */
bool p2000m_capture_pixel_is_white(const uint32_t *frame,
                                   unsigned x, unsigned y) {
    const uint32_t *line =
        frame + y * P2000M_CAPTURE_WORDS_PER_LINE;
    return p2000m_capture_mapped_line_pixel_is_white(
        line, p2000m_capture_pixel_map(), x);
}

/**
 * @brief Copy capture and resampler telemetry under the capture lock.
 *
 * @param stats Destination for a consistent statistics snapshot.
 * @return Nothing.
 */
void p2000m_capture_get_stats(p2000m_capture_stats_t *stats) {
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    stats->captured_frames = captured_frames;
    stats->stale_frames_replaced = stale_frames_replaced;
    stats->last_frame_period_us = last_frame_period_us;
    stats->recovered_line_ticks_q16 = recovered_line_ticks_q16;
    stats->autotune_runs = autotune_runs;
    stats->autotune_score = autotune_score;
    stats->last_autotune_us = last_autotune_us;
    stats->maximum_autotune_us = maximum_autotune_us;
    stats->auto_phase_ticks = auto_phase_ticks;
    stats->manual_phase_ticks = manual_phase_ticks;
    spin_unlock(buffer_lock, saved);
}
