// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QWidget>

namespace culprit {

// Grid of CPU cells: fill = utilisation, text = current frequency, red corner =
// run-queue contention, amber bar = IRQ+softirq share.
class CpuHeatmap : public QWidget {
    Q_OBJECT
public:
    explicit CpuHeatmap(QWidget* parent = nullptr);
    void setData(const SystemSample& sys);

    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;

private:
    int columns(int width) const;
    int cellAt(const QPoint& pos) const;

    std::vector<CpuCoreSample> cpus_;
    double maxFreq_ = 0;
};

} // namespace culprit
