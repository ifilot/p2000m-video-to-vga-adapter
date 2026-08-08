/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file phosphor_afterglow.cpp
 * @brief Deterministic per-pixel CRT phosphor persistence filter.
 */

#include "phosphor_afterglow.h"

#include <algorithm>
#include <cmath>

#include <QtGlobal>

namespace p2000m {

QImage applyPhosphorAfterglow(const QImage &current,
                              const QImage &previous,
                              double retention) {
    if (current.isNull()) {
        return {};
    }

    QImage result = current.convertToFormat(QImage::Format_RGB32);
    if (previous.isNull() || previous.size() != result.size()) {
        return result;
    }

    const double retained = std::clamp(retention, 0.0, 1.0);
    if (retained <= 0.0) {
        return result;
    }
    const QImage history = previous.convertToFormat(QImage::Format_RGB32);

    for (int y = 0; y < result.height(); ++y) {
        auto *destination = reinterpret_cast<QRgb *>(result.scanLine(y));
        const auto *prior =
            reinterpret_cast<const QRgb *>(history.constScanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QRgb currentPixel = destination[x];
            const QRgb previousPixel = prior[x];
            const int red = std::max(
                qRed(currentPixel), qRound(qRed(previousPixel) * retained));
            const int green = std::max(
                qGreen(currentPixel),
                qRound(qGreen(previousPixel) * retained));
            const int blue = std::max(
                qBlue(currentPixel),
                qRound(qBlue(previousPixel) * retained));
            destination[x] = qRgb(red, green, blue);
        }
    }
    return result;
}

}  // namespace p2000m
