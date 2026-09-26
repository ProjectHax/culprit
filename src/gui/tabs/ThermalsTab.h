// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "gui/tabs/TabPage.h"

#include <QHash>

class QLabel;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace culprit {

class CpuHeatmap;
class Sparkline;

class ThermalsTab : public TabPage {
    Q_OBJECT
public:
    explicit ThermalsTab(QWidget* parent = nullptr);

signals:
    void processActivated(int pid);

protected:
    void accumulate(const Frame& f) override;
    void render(const Frame& f) override;

private:
    void updateSensors(const Frame& f);
    void updateContributors(const Frame& f);
    void updateGpu(const Frame& f);

    CpuHeatmap* heatmap_;
    QLabel* policy_;
    Sparkline* cpuTemp_;
    Sparkline* cpuPower_;
    Sparkline* gpuTemp_;
    Sparkline* gpuPower_;
    QLabel* gpuInfo_;
    QTreeWidget* sensors_;
    QTableWidget* contributors_;
    QHash<QString, QTreeWidgetItem*> sensorItems_;
    QHash<QString, QTreeWidgetItem*> chipItems_;
    QHash<QString, std::pair<double, double>> minMax_;
    QHash<QString, QString> notConnected_;   // sensor key -> why it's probably not connected
};

} // namespace culprit
