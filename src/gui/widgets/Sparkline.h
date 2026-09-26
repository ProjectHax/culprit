// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QColor>
#include <QStringList>
#include <QWidget>

#include <deque>
#include <vector>

namespace culprit {

// A small "card" with a title, current value and a scrolling history graph of
// up to a few series.
class Sparkline : public QWidget {
    Q_OBJECT
public:
    explicit Sparkline(const QString& title, QWidget* parent = nullptr);

    void setSeries(const QStringList& names);          // default: one unnamed series
    void setFixedRange(double min, double max);
    void setAutoRange(double minimumMax);              // y max = max(data, minimumMax)
    void setThreshold(double v);                       // dashed guide line (NaN = none)
    void setCapacity(int points);
    void setUnitSuffix(const QString& s) { suffix_ = s; }

    void push(const std::vector<double>& values);      // one value per series (NaN = gap)
    void push(double v) { push(std::vector<double>{v}); }
    void setValueText(const QString& text, bool alert = false);
    void setSubText(const QString& text);
    void clear();

    QSize sizeHint() const override { return {280, 132}; }
    QSize minimumSizeHint() const override { return {180, 96}; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QString title_, value_, sub_, suffix_;
    bool alert_ = false;
    QStringList names_;
    std::vector<std::deque<double>> data_;
    int capacity_ = 120;
    bool fixed_ = false;
    double min_ = 0, max_ = 100, minimumMax_ = 1;
    double threshold_;
    bool pressed_ = false;
};

} // namespace culprit
