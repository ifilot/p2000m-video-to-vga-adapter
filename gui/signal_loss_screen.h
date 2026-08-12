/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_SIGNAL_LOSS_SCREEN_H
#define P2000M_SIGNAL_LOSS_SCREEN_H

#include <QImage>
#include <QString>

namespace p2000m {

/** Render the same fixed 640 x 480 signal-loss card as the firmware. */
QImage renderSignalLossScreen(const QString &firmwareVersion);

}  // namespace p2000m

#endif
