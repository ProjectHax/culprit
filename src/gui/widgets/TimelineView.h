// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QColor>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <vector>

namespace culprit {

// Time-series plot with clickable event markers. X is in seconds (relative to
// "now" for live views, or to a recording's start). Dense data is reduced to
// min/max per pixel column so spikes are never lost.
class TimelineView : public QWidget {
    Q_OBJECT
public:
    struct Series {
        QString name;
        QColor color;
        bool fill = false;
        bool rightAxis = false;   // plotted against the secondary (right) axis
        std::vector<QPointF> pts; // sorted by x
    };
    struct Marker {
        double x = 0;
        QColor color;
        QString tooltip;
        qint64 id = -1;
    };

    explicit TimelineView(QWidget* parent = nullptr);

    void setSeries(const std::vector<Series>& s);
    void setPoints(int index, std::vector<QPointF> pts);
    void setMarkers(std::vector<Marker> m);
    void setXRange(double min, double max);
    void setYRange(double min, double max, bool autoMax);
    void setRightYRange(double min, double max, bool autoMax);
    void setYUnit(const QString& unit) { yUnit_ = unit; }
    void setRightUnit(const QString& unit) { rightUnit_ = unit; }
    void setThreshold(double y, const QString& label);
    void setWallClockBase(qint64 wallMsAtZero);   // label the x axis with clock times
    void setEmptyText(const QString& t) { empty_ = t; }
    void setSelectedMarker(qint64 id);

    QSize sizeHint() const override { return {800, 220}; }
    QSize minimumSizeHint() const override { return {300, 140}; }

signals:
    void markerClicked(qint64 id);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF plotRect() const;
    double xToPx(double x, const QRectF& r) const;
    double pxToX(double px, const QRectF& r) const;
    int markerAt(const QPoint& p) const;
    QString xLabel(double x) const;

    std::vector<Series> series_;
    std::vector<Marker> markers_;
    double xMin_ = -10, xMax_ = 0;
    double yMin_ = 0, yMax_ = 1, yAutoFloor_ = 1;
    bool yAuto_ = true;
    double ryMin_ = 0, ryMax_ = 1, ryAutoFloor_ = 1;
    bool ryAuto_ = true;
    QString yUnit_, rightUnit_;
    double threshold_ = -1;
    QString thresholdLabel_;
    qint64 wallBase_ = -1;
    QString empty_;
    qint64 selected_ = -1;
    int hoverX_ = -1;
};

} // namespace culprit
