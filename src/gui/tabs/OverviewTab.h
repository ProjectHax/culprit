// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "gui/tabs/TabPage.h"

class QGridLayout;
class QSplitter;
class QVBoxLayout;

namespace culprit {

class Sparkline;

class OverviewTab : public TabPage {
    Q_OBJECT
public:
    explicit OverviewTab(QWidget* parent = nullptr);

    // Space under the cards for the findings panel.
    void setBottomWidget(QWidget* w);

signals:
    void navigate(const QString& tabName);

protected:
    void accumulate(const Frame& f) override;
    void render(const Frame&) override {}

private:
    Sparkline* addCard(const QString& title, const QString& target);

    QSplitter* splitter_ = nullptr;
    QGridLayout* grid_ = nullptr;
    int cardCount_ = 0;

    Sparkline* cpu_ = nullptr;
    Sparkline* mem_ = nullptr;
    Sparkline* load_ = nullptr;
    Sparkline* runq_ = nullptr;
    Sparkline* cpuTemp_ = nullptr;
    Sparkline* power_ = nullptr;
    Sparkline* gpu_ = nullptr;
    Sparkline* gpuTemp_ = nullptr;
    Sparkline* disk_ = nullptr;
    Sparkline* net_ = nullptr;
    Sparkline* faults_ = nullptr;
    Sparkline* latency_ = nullptr;
};

} // namespace culprit
