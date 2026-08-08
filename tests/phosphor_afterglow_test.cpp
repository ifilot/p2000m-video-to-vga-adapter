/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file phosphor_afterglow_test.cpp
 * @brief Deterministic checks for CRT phosphor persistence blending.
 */

#include <cstdio>

#include <QImage>

#include "phosphor_afterglow.h"

namespace {

/** Require one pixel to equal an expected opaque RGB value. */
bool expectPixel(const QImage &image, int x, int y, QRgb expected,
                 const char *description) {
    const QRgb actual = image.pixel(x, y);
    if (actual == expected) {
        return true;
    }
    std::fprintf(stderr, "%s: expected #%06x, got #%06x\n", description,
                 expected & 0x00ffffffu, actual & 0x00ffffffu);
    return false;
}

}  // namespace

/** Exercise reset, decay, excitation-floor, clamping, and resize behavior. */
int main() {
    QImage current(2, 1, QImage::Format_RGB32);
    current.setPixel(0, 0, qRgb(10, 120, 20));
    current.setPixel(1, 0, qRgb(200, 5, 80));

    QImage previous(2, 1, QImage::Format_RGB32);
    previous.setPixel(0, 0, qRgb(200, 100, 80));
    previous.setPixel(1, 0, qRgb(100, 240, 20));

    const QImage half = p2000m::applyPhosphorAfterglow(
        current, previous, 0.5);
    if (!expectPixel(half, 0, 0, qRgb(100, 120, 40),
                     "half-life decay") ||
        !expectPixel(half, 1, 0, qRgb(200, 120, 80),
                     "current excitation floor")) {
        return 1;
    }

    const QImage disabled = p2000m::applyPhosphorAfterglow(
        current, previous, 0.0);
    if (disabled != current) {
        std::fputs("Zero retention changed the current frame\n", stderr);
        return 1;
    }

    const QImage clamped = p2000m::applyPhosphorAfterglow(
        current, previous, 2.0);
    if (!expectPixel(clamped, 0, 0, qRgb(200, 120, 80),
                     "upper retention clamp")) {
        return 1;
    }

    QImage wrongSize(1, 1, QImage::Format_RGB32);
    wrongSize.fill(Qt::white);
    const QImage reset = p2000m::applyPhosphorAfterglow(
        current, wrongSize, 1.0);
    if (reset != current) {
        std::fputs("A size change did not reset phosphor history\n", stderr);
        return 1;
    }

    return 0;
}
