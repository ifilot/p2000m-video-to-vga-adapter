/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file phosphor_afterglow.h
 * @brief Deterministic per-pixel CRT phosphor persistence filter.
 */

#ifndef P2000M_PHOSPHOR_AFTERGLOW_H
#define P2000M_PHOSPHOR_AFTERGLOW_H

#include <QImage>

namespace p2000m {

/**
 * Retain decayed light from the previous image behind the current image.
 *
 * Every RGB channel keeps the brighter of the current excitation and the
 * decayed previous phosphor value. This leaves bright-to-dark transitions
 * trailing without brightening pixels that remain continuously illuminated.
 *
 * @param current Newly reconstructed RGB frame.
 * @param previous Previously filtered frame, or a null image on reset.
 * @param retention Previous brightness retained in the inclusive [0, 1] range.
 * @return A detached Format_RGB32 image containing the filtered frame.
 */
QImage applyPhosphorAfterglow(const QImage &current,
                              const QImage &previous,
                              double retention);

}  // namespace p2000m

#endif
