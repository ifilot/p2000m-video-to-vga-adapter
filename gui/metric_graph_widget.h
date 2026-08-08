/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_METRIC_GRAPH_WIDGET_H
#define P2000M_METRIC_GRAPH_WIDGET_H

/**
 * @file metric_graph_widget.h
 * @brief Compact continuously updating history graph for one viewer metric.
 */

#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <QWidget>

/** Task-Manager-style graph with a label, current value, and rolling history. */
class MetricGraphWidget final : public QWidget {
public:
    /**
     * Construct an empty graph.
     *
     * @param title Short user-facing metric name.
     * @param color Line and fill color assigned to the metric.
     * @param parent Optional Qt parent widget.
     */
    explicit MetricGraphWidget(QString title, QColor color,
                               QWidget *parent = nullptr);

    /**
     * Update the displayed value and periodically append it to the history.
     *
     * Calls may arrive at full video-frame rate. History is sampled at a
     * lower fixed cadence so the graph covers approximately one minute.
     *
     * @param value Numeric value used by the graph.
     * @param text Fully formatted current-value text.
     * @param plot Whether value is currently valid and should enter history.
     */
    void setSample(double value, QString text, bool plot = true);

    /** Remove all history and restore the unavailable state. */
    void clear();

    /** Return the compact preferred size used by the statistics sidebar. */
    QSize sizeHint() const override;

protected:
    /** Paint the graph, title, and current value using the active Qt palette. */
    void paintEvent(QPaintEvent *event) override;

private:
    /** Maximum plotted samples: 300 at 5 Hz covers approximately 60 seconds. */
    static constexpr qsizetype kHistoryCapacity = 300;
    /** Minimum interval between plotted samples. */
    static constexpr qint64 kSampleIntervalMilliseconds = 200;

    /** User-facing metric title. */
    QString title_;
    /** Already formatted current value. */
    QString valueText_ = QStringLiteral("Waiting for data");
    /** Line and translucent fill color. */
    QColor color_;
    /** Oldest-to-newest plotted samples. */
    QVector<double> samples_;
    /** Cadence timer separating display updates from history samples. */
    QElapsedTimer sampleClock_;
};

#endif
