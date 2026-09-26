// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "gui/tabs/TabPage.h"

class QLabel;
class QTabWidget;

namespace culprit {

class DataTable;
class Sparkline;

class LoadIoTab : public TabPage {
    Q_OBJECT
public:
    explicit LoadIoTab(QWidget* parent = nullptr);

signals:
    void processActivated(int pid);

protected:
    void accumulate(const Frame& f) override;
    void render(const Frame& f) override;

private:
    QLabel* summary_;
    Sparkline* loadGraph_;
    Sparkline* blockedGraph_;
    QTabWidget* sub_;
    DataTable* cpus_;
    DataTable* dstate_;
    DataTable* disks_;
    DataTable* irqs_;
    DataTable* softirqs_;
    DataTable* cgroups_;
    DataTable* nets_;
};

} // namespace culprit
