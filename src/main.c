#include <stdint.h>

#include "hardware/clocks.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdlib.h"

enum {
    SYSTEM_CLOCK_KHZ = 126000,
    VGA_WIDTH = 640,
    VGA_HEIGHT = 480,
    BLACK_GUARD_SIZE = 4,
    BORDER_SIZE = 8,
    CONTENT_INSET = BLACK_GUARD_SIZE + BORDER_SIZE,
    COLOR_BAR_COUNT = 8,
    COLOR_BAR_WIDTH = (VGA_WIDTH - 2 * CONTENT_INSET) / COLOR_BAR_COUNT,
    SCANLINE_BUFFER_WORDS = 65,
};

static_assert(COLOR_BAR_WIDTH * COLOR_BAR_COUNT == VGA_WIDTH - 2 * CONTENT_INSET,
              "Color bars must exactly fill the bordered raster");

static const uint16_t BORDER_COLOR = 0x0f0f;

static const scanvideo_timing_t vga_timing_640x480_60 = {
    .clock_freq = 25200000,

    .h_active = VGA_WIDTH,
    .v_active = VGA_HEIGHT,

    .h_front_porch = 16,
    .h_pulse = 96,
    .h_total = 800,
    .h_sync_polarity = 1,

    .v_front_porch = 10,
    .v_pulse = 2,
    .v_total = 525,
    .v_sync_polarity = 1,

    .enable_clock = 0,
    .clock_polarity = 0,
    .enable_den = 0,
};

static const scanvideo_mode_t diagnostic_vga_mode = {
    .default_timing = &vga_timing_640x480_60,
    .pio_program = &video_24mhz_composable,
    .width = VGA_WIDTH,
    .height = VGA_HEIGHT,
    .xscale = 1,
    .yscale = 1,
    .yscale_denominator = 1,
};

static inline uint16_t rgb444(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint16_t)((red & 0x0fu) | ((green & 0x0fu) << 4u) |
                      ((blue & 0x0fu) << 8u));
}

static bool append_color_run(scanvideo_scanline_buffer_t *scanline_buffer,
                             uint16_t *token_count, uint16_t color,
                             uint16_t length) {
    if (length < 3u || *token_count + 3u > scanline_buffer->data_max * 2u) {
        return false;
    }

    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[(*token_count)++] = COMPOSABLE_COLOR_RUN;
    tokens[(*token_count)++] = color;
    tokens[(*token_count)++] = length - 3u;
    return true;
}

static bool finish_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                            uint16_t *token_count) {
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;

    if ((*token_count & 1u) != 0u) {
        if (*token_count + 1u > scanline_buffer->data_max * 2u) {
            return false;
        }
        tokens[(*token_count)++] = COMPOSABLE_EOL_ALIGN;
    } else {
        if (*token_count + 2u > scanline_buffer->data_max * 2u) {
            return false;
        }
        tokens[(*token_count)++] = COMPOSABLE_EOL_SKIP_ALIGN;
        tokens[(*token_count)++] = 0;
    }

    scanline_buffer->data_used = *token_count / 2u;
    return true;
}

static bool render_color_bars(scanvideo_scanline_buffer_t *scanline_buffer,
                              uint16_t *token_count) {
    static const uint16_t colors[COLOR_BAR_COUNT] = {
        0x0fff,  // White
        0x00ff,  // Yellow
        0x0ff0,  // Cyan
        0x00f0,  // Green
        0x0f0f,  // Magenta
        0x000f,  // Red
        0x0f00,  // Blue
        0x0000,  // Black
    };

    bool ok = append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    for (uint16_t bar = 0; bar < COLOR_BAR_COUNT && ok; ++bar) {
        ok = append_color_run(scanline_buffer, token_count, colors[bar], COLOR_BAR_WIDTH);
    }
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    return ok && append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
}

static bool render_ramp(scanvideo_scanline_buffer_t *scanline_buffer,
                        uint16_t *token_count, uint16_t y) {
    bool ok = append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    for (uint8_t level = 0; level < 16u && ok; ++level) {
        const uint16_t length = (level & 1u) == 0u ? 39u : 38u;
        uint16_t color;
        if (y < 352u) {
            color = rgb444(level, 0, 0);
        } else if (y < 384u) {
            color = rgb444(0, level, 0);
        } else {
            color = rgb444(0, 0, level);
        }
        ok = append_color_run(scanline_buffer, token_count, color, length);
    }
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    return ok && append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
}

static bool render_checkerboard(scanvideo_scanline_buffer_t *scanline_buffer,
                                uint16_t *token_count, uint16_t y) {
    bool ok = append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    uint16_t x = CONTENT_INSET;

    while (x < VGA_WIDTH - CONTENT_INSET && ok) {
        const uint16_t relative_x = x - CONTENT_INSET;
        uint16_t next_x = CONTENT_INSET + (uint16_t)(((relative_x / 16u) + 1u) * 16u);
        if (next_x > VGA_WIDTH - CONTENT_INSET) {
            next_x = VGA_WIDTH - CONTENT_INSET;
        }
        const bool white = ((((relative_x / 16u)) ^ ((y - 416u) / 16u)) & 1u) != 0u;
        ok = append_color_run(scanline_buffer, token_count,
                              white ? 0x0fff : 0x0000, next_x - x);
        x = next_x;
    }

    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR, BORDER_SIZE);
    return ok && append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
}

static bool render_horizontal_border(scanvideo_scanline_buffer_t *scanline_buffer,
                                     uint16_t *token_count) {
    bool ok = append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
    ok = ok && append_color_run(scanline_buffer, token_count, BORDER_COLOR,
                                VGA_WIDTH - 2 * BLACK_GUARD_SIZE);
    return ok && append_color_run(scanline_buffer, token_count, 0, BLACK_GUARD_SIZE);
}

static void render_scanline(scanvideo_scanline_buffer_t *scanline_buffer) {
    const uint16_t y = scanvideo_scanline_number(scanline_buffer->scanline_id);
    uint16_t token_count = 0;
    bool ok;

    if (scanline_buffer->data_max < SCANLINE_BUFFER_WORDS) {
        scanline_buffer->data_used = 0;
        scanline_buffer->status = SCANLINE_ERROR;
        return;
    }

    if (y < BLACK_GUARD_SIZE || y >= VGA_HEIGHT - BLACK_GUARD_SIZE) {
        ok = append_color_run(scanline_buffer, &token_count, 0, VGA_WIDTH);
    } else if (y < CONTENT_INSET || y >= VGA_HEIGHT - CONTENT_INSET) {
        ok = render_horizontal_border(scanline_buffer, &token_count);
    } else if (y < 320u) {
        ok = render_color_bars(scanline_buffer, &token_count);
    } else if (y < 416u) {
        ok = render_ramp(scanline_buffer, &token_count, y);
    } else {
        ok = render_checkerboard(scanline_buffer, &token_count, y);
    }

    ok = ok && finish_scanline(scanline_buffer, &token_count);
    if (!ok) {
        scanline_buffer->data_used = 0;
    }
    scanline_buffer->status = ok ? SCANLINE_OK : SCANLINE_ERROR;
}

int main(void) {
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the 126 MHz system clock");
    }

    if (!scanvideo_setup(&diagnostic_vga_mode)) {
        panic("Unable to initialize VGA scanvideo");
    }

    scanvideo_timing_enable(true);

    while (true) {
        scanvideo_scanline_buffer_t *scanline_buffer =
            scanvideo_begin_scanline_generation(true);
        render_scanline(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}
