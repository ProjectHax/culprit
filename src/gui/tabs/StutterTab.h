// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"
#include "gui/tabs/TabPage.h"

#include <deque>
#include <memory>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTextBrowser;
class QTimer;

namespace culprit {

class DataTable;
class LatencyFeed;
class Sparkline;
class TimelineView;

class StutterTab : public TabPage {
    Q_OBJECT
public:
    StutterTab(std::shared_ptr<LatencyFeed> feed, const Settings& settings, QWidget* parent = nullptr);

    void addHitch(const Hitch& h);
    void updateHitch(const Hitch& h);   // deep-trace details arrived
    void setFocusProcess(int pid, const QString& name);
    void setDeepTraceState(bool running, const QString& status);
    void setSettings(const Settings& s);
    const std::deque<Hitch>& hitches() const { return hitches_; }

signals:
    void probeSettingsChanged(culprit::ProbeMode mode, double thresholdMs);
    void focusCleared();
    void processActivated(int pid);
    void deepTraceToggled(bool on);
    void exportRequested(qint64 hitchId);

protected:
    void accumulate(const Frame& f) override;
    void render(const Frame& f) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void refreshGraph();
    void rebuildTable();
    void showHitch(qint64 id);
    void onModeChanged();
    double thresholdFor(ProbeMode m) const;

    std::shared_ptr<LatencyFeed> feed_;
    Settings settings_;
    QComboBox* mode_;
    QDoubleSpinBox* threshold_;
    QComboBox* window_;
    QPushButton* deep_;
    QLabel* status_;
    QLabel* deepStatus_;
    TimelineView* graph_;
    QTabWidget* lists_;
    DataTable* table_;
    DataTable* waiters_;
    DataTable* events_;
    std::deque<DeepEvent> deepEvents_;
    bool eventsDirty_ = false;
    QTextBrowser* detail_;
    QWidget* focusBox_;
    QLabel* focusLabel_;
    Sparkline* focusGraph_;
    QTimer* timer_;
    std::deque<Hitch> hitches_;
    qint64 selected_ = -1;
    int focusPid_ = 0;
    QString focusName_;
    bool tableDirty_ = false;
    bool followLatest_ = true;
    int refreshCount_ = 0;   // show the newest hitch until the user picks one
};

} // namespace culprit
