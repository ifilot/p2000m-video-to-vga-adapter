/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file metric_graph_widget.cpp
 * @brief Painting and sampling implementation for MetricGraphWidget.
 */

#include "metric_graph_widget.h"

#include <algorithm>
#include <utility>

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QSizePolicy>

MetricGraphWidget::MetricGraphWidget(QString title, QColor color,
                                     QWidget *parent)
    : QWidget(parent), title_(std::move(title)), color_(std::move(color)) {
    setMinimumSize(270, 68);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    samples_.reserve(kHistoryCapacity);
}

void MetricGraphWidget::setSample(double value, QString text, bool plot) {
    if (sampleClock_.isValid() &&
        sampleClock_.elapsed() < kSampleIntervalMilliseconds) {
        return;
    }

    valueText_ = std::move(text);
    if (plot) {
        if (samples_.size() == kHistoryCapacity) {
            samples_.removeFirst();
        }
        samples_.append(std::max(0.0, value));
    }
    if (sampleClock_.isValid()) {
        sampleClock_.restart();
    } else {
        sampleClock_.start();
    }
    update();
}

void MetricGraphWidget::clear() {
    samples_.clear();
    valueText_ = QStringLiteral("Waiting for data");
    sampleClock_.invalidate();
    update();
}

QSize MetricGraphWidget::sizeHint() const {
    return QSize(290, 72);
}

void MetricGraphWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor windowColor = palette().color(QPalette::Window);
    const QColor textColor = palette().color(QPalette::WindowText);
    const QColor gridColor = palette().color(QPalette::Mid);
    painter.fillRect(rect(), windowColor);

    const QRectF graphRect(7.5, 7.5, 96.0, height() - 15.0);
    painter.setPen(QPen(gridColor, 1.0));
    painter.setBrush(palette().color(QPalette::Base));
    painter.drawRect(graphRect);

    // Subtle subdivisions make changes readable without adding axis labels.
    painter.setPen(QPen(QColor(gridColor.red(), gridColor.green(),
                              gridColor.blue(), 70),
                        1.0));
    for (int division = 1; division < 4; ++division) {
        const qreal x = graphRect.left() +
                        graphRect.width() * division / 4.0;
        painter.drawLine(QPointF(x, graphRect.top()),
                         QPointF(x, graphRect.bottom()));
    }
    for (int division = 1; division < 3; ++division) {
        const qreal y = graphRect.top() +
                        graphRect.height() * division / 3.0;
        painter.drawLine(QPointF(graphRect.left(), y),
                         QPointF(graphRect.right(), y));
    }

    if (!samples_.isEmpty()) {
        const double maximum = std::max(
            1.0, *std::max_element(samples_.cbegin(), samples_.cend()) * 1.1);
        QPainterPath line;
        const qsizetype count = samples_.size();
        const qreal firstX = graphRect.right() - graphRect.width() *
            static_cast<qreal>(count - 1) /
            static_cast<qreal>(kHistoryCapacity - 1);
        for (qsizetype index = 0; index < count; ++index) {
            const qreal x = firstX + graphRect.width() *
                static_cast<qreal>(index) /
                static_cast<qreal>(kHistoryCapacity - 1);
            const qreal normalized =
                std::clamp(samples_.at(index) / maximum, 0.0, 1.0);
            const qreal y = graphRect.bottom() -
                            normalized * graphRect.height();
            if (index == 0) {
                line.moveTo(x, y);
            } else {
                line.lineTo(x, y);
            }
        }

        QPainterPath fill = line;
        fill.lineTo(graphRect.right(), graphRect.bottom());
        fill.lineTo(firstX, graphRect.bottom());
        fill.closeSubpath();
        QColor fillColor = color_;
        fillColor.setAlpha(45);
        painter.fillPath(fill, fillColor);
        painter.setPen(QPen(color_, 1.5));
        painter.drawPath(line);
    }

    const int textLeft = static_cast<int>(graphRect.right()) + 12;
    const QRect titleRect(textLeft, 8, width() - textLeft - 6, 23);
    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 0.5);
    painter.setFont(titleFont);
    painter.setPen(textColor);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title_);

    QFont valueFont = font();
    valueFont.setPointSizeF(std::max(7.0, valueFont.pointSizeF() - 0.5));
    painter.setFont(valueFont);
    painter.setPen(palette().color(QPalette::Text));
    const QRect valueRect(textLeft, 29, width() - textLeft - 6,
                          height() - 34);
    painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignTop |
                                    Qt::TextWordWrap,
                     valueText_);
}
