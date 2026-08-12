/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "signal_loss_screen.h"

#include <array>

#include <QColor>
#include <QPainter>

namespace p2000m {
namespace {

constexpr int kScreenWidth = 640;
constexpr int kScreenHeight = 480;
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kPanelLeft = 90;
constexpr int kPanelRight = 550;
constexpr int kPanelTop = 140;
constexpr int kPanelBottom = 340;
constexpr int kPanelBorder = 3;

/** Five-bit rows for uppercase letters A-Z followed by digits 0-9. */
constexpr std::array<std::array<quint8, kGlyphHeight>, 36> kFont = {{
    {{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, // A
    {{0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}, // B
    {{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, // C
    {{0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, // D
    {{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, // E
    {{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}, // F
    {{0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e}}, // G
    {{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, // H
    {{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}}, // I
    {{0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}}, // J
    {{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, // K
    {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}}, // L
    {{0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, // M
    {{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, // N
    {{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, // O
    {{0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}}, // P
    {{0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, // Q
    {{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, // R
    {{0x0e, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, // S
    {{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, // T
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, // U
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}}, // V
    {{0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11}}, // W
    {{0x11, 0x0a, 0x04, 0x04, 0x04, 0x0a, 0x11}}, // X
    {{0x11, 0x0a, 0x04, 0x04, 0x04, 0x04, 0x04}}, // Y
    {{0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}}, // Z
    {{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, // 0
    {{0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}}, // 1
    {{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, // 2
    {{0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}}, // 3
    {{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, // 4
    {{0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}}, // 5
    {{0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, // 6
    {{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, // 7
    {{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, // 8
    {{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}}, // 9
}};

/** Return one row from the firmware's compact status font. */
quint8 glyphRow(QChar character, int row) {
    character = character.toUpper();
    if (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) {
        return kFont[static_cast<size_t>(character.unicode() - 'A')]
                    [static_cast<size_t>(row)];
    }
    if (character >= QLatin1Char('0') && character <= QLatin1Char('9')) {
        return kFont[26u + static_cast<size_t>(character.unicode() - '0')]
                    [static_cast<size_t>(row)];
    }
    if (character == QLatin1Char('.')) {
        return row == 6 ? 0x04 : 0x00;
    }
    if (character == QLatin1Char('+')) {
        constexpr std::array<quint8, kGlyphHeight> plus = {
            0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00,
        };
        return plus[static_cast<size_t>(row)];
    }
    if (character == QLatin1Char('-')) {
        return row == 3 ? 0x1f : 0x00;
    }
    return 0x00;
}

/** Draw one centered line using the firmware's exact bitmap geometry. */
void drawText(QImage *image, const QString &message, int top, int scale,
              QRgb color) {
    if (message.isEmpty()) {
        return;
    }
    const int textWidth =
        ((kGlyphWidth + 1) * message.size() - 1) * scale;
    const int textLeft = (kScreenWidth - textWidth) / 2;
    for (int character = 0; character < message.size(); ++character) {
        const int characterX =
            textLeft + character * (kGlyphWidth + 1) * scale;
        for (int row = 0; row < kGlyphHeight; ++row) {
            const quint8 bits = glyphRow(message.at(character), row);
            for (int column = 0; column < kGlyphWidth; ++column) {
                if ((bits & (0x10u >> column)) == 0u) {
                    continue;
                }
                for (int y = 0; y < scale; ++y) {
                    auto *pixels = reinterpret_cast<QRgb *>(
                        image->scanLine(top + row * scale + y));
                    for (int x = 0; x < scale; ++x) {
                        pixels[characterX + column * scale + x] = color;
                    }
                }
            }
        }
    }
}

}  // namespace

QImage renderSignalLossScreen(const QString &firmwareVersion) {
    // Expand the firmware's RGB444 values to their exact digital equivalents.
    constexpr QRgb canvas = 0xff000000u;
    constexpr QRgb panel = 0xff112222u;
    constexpr QRgb alert = 0xffee3333u;
    constexpr QRgb text = 0xffffffffu;

    QImage image(kScreenWidth, kScreenHeight, QImage::Format_RGB32);
    image.fill(canvas);
    QPainter painter(&image);
    painter.fillRect(kPanelLeft, kPanelTop,
                     kPanelRight - kPanelLeft,
                     kPanelBottom - kPanelTop, panel);
    painter.fillRect(kPanelLeft, kPanelTop,
                     kPanelRight - kPanelLeft, kPanelBorder, alert);
    painter.fillRect(kPanelLeft, kPanelBottom - kPanelBorder,
                     kPanelRight - kPanelLeft, kPanelBorder, alert);
    painter.fillRect(kPanelLeft, kPanelTop,
                     kPanelBorder, kPanelBottom - kPanelTop, alert);
    painter.fillRect(kPanelRight - kPanelBorder, kPanelTop,
                     kPanelBorder, kPanelBottom - kPanelTop, alert);
    painter.end();

    drawText(&image, QStringLiteral("P2000M VID2VGA"), 164, 2, text);
    drawText(&image, QStringLiteral("SIGNAL LOST"), 200, 4, text);
    drawText(&image,
             QStringLiteral("FIRMWARE %1").arg(firmwareVersion),
             254, 2, text);
    drawText(&image, QStringLiteral("WAITING FOR HSYNC + VSYNC"),
             294, 2, text);
    return image;
}

}  // namespace p2000m
