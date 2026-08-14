/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_SIGNAL_LOSS_H
#define P2000M_SIGNAL_LOSS_H

#include <stdint.h>

#define P2000M_SIGNAL_LOSS_PRODUCT "P2000M VID2VGA"
#define P2000M_SIGNAL_LOSS_MESSAGE "SIGNAL LOST"
#define P2000M_SIGNAL_LOSS_WAITING "WAITING FOR HSYNC + VSYNC"

enum {
    P2000M_SIGNAL_LOSS_GLYPH_WIDTH = 5,
    P2000M_SIGNAL_LOSS_GLYPH_HEIGHT = 7,
    P2000M_SIGNAL_LOSS_FONT_GLYPHS = 36,
};

#ifdef __cplusplus
extern "C" {
#endif

/** Obtain one five-bit row from the shared signal-loss status font. */
uint8_t p2000m_signal_loss_glyph_row(char character, unsigned row);

#ifdef __cplusplus
}
#endif

#endif
