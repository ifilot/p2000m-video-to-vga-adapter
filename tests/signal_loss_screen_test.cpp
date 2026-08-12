/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdlib>
#include <iostream>

#include <QImage>

#include "signal_loss_screen.h"

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    const QImage image = p2000m::renderSignalLossScreen(
        QStringLiteral("v0.3.2"));
    require(image.size() == QSize(640, 480),
            "Signal-loss screen must use native VGA dimensions");
    require(image.pixel(0, 0) == 0xff000000u,
            "Canvas must be black");
    require(image.pixel(90, 140) == 0xffee3333u,
            "Panel outline must use firmware alert red");
    require(image.pixel(93, 143) == 0xff112222u,
            "Panel interior must use firmware RGB444 fill");
    require(image.pixel(194, 200) == 0xffffffffu,
            "Primary warning text must be present");
    require(image.pixel(550, 340) == 0xff000000u,
            "Panel coordinates must remain right/bottom exclusive");
    return EXIT_SUCCESS;
}
