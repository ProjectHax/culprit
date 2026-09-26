// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"
#include "core/model/Frame.h"

#include <QMainWindow>
#include <QPointer>

#include <memory>

class QAction;
class QLabel;
class QTabWidget;
class QToolBar;

namespace culprit {

class EngineHost;
class FindingsPanel;
class OverviewTab;
class ProcessesTab;
class StutterTab;
class RecordingsTab;
class Recorder;
class TabPage;
class TitleBar;
class WindowChrome;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void showTab(const QString& name);
    void showProcess(int pid);
    void openRecording(const QString& path);

protected:
    void paintEvent(QPaintEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void applyChrome();
    void onFrame(const FramePtr& frame);
    void addTab(TabPage* page, const QString& name, const QString& title);

    Settings settings_;
    std::unique_ptr<EngineHost> engine_;
    QTabWidget* tabs_ = nullptr;
    std::vector<std::pair<QString, TabPage*>> pages_;
    OverviewTab* overview_ = nullptr;
    ProcessesTab* processes_ = nullptr;
    FindingsPanel* findings_ = nullptr;
    StutterTab* stutter_ = nullptr;
    RecordingsTab* recordings_ = nullptr;
    std::unique_ptr<Recorder> recorder_;
    QAction* recordAct_ = nullptr;
    QLabel* statusRec_ = nullptr;
    FramePtr lastFrame_;

    void toggleRecording(bool on);
    void showSettings();
    void showAbout();
    void exportReport();

    QAction* pauseAct_ = nullptr;
    QAction* exportAct_ = nullptr;
    QAction* settingsAct_ = nullptr;
    QAction* aboutAct_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QPointer<TitleBar> titleBar_;
    WindowChrome* chrome_ = nullptr;
    QLabel* statusSelf_ = nullptr;
    QLabel* statusTime_ = nullptr;
};

} // namespace culprit
