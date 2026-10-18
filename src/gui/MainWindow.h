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
class QSystemTrayIcon;
class QTimer;
class QToolBar;

namespace culprit {

class EngineHost;
class FindingsPanel;
class OverviewTab;
class ProcessesTab;
class StutterTab;
class ThermalsTab;
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
    // Hide the window to the system tray: Culprit keeps watching and logging
    // (hitches, freezes, findings, recordings) but draws nothing until shown.
    void hideToTray();

protected:
    void paintEvent(QPaintEvent* e) override;
    void changeEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void applyChrome();
    void onFrame(const FramePtr& frame);
    void showFromTray();
    void updateBackground();
    void noteBackgroundHitch(const Hitch& h);
    void updateTrayToolTip();
    void addTab(TabPage* page, const QString& name, const QString& title);

    Settings settings_;
    std::unique_ptr<EngineHost> engine_;
    QTabWidget* tabs_ = nullptr;
    std::vector<std::pair<QString, TabPage*>> pages_;
    OverviewTab* overview_ = nullptr;
    ProcessesTab* processes_ = nullptr;
    FindingsPanel* findings_ = nullptr;
    StutterTab* stutter_ = nullptr;
    ThermalsTab* thermals_ = nullptr;
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
    QAction* trayAct_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QPointer<TitleBar> titleBar_;
    WindowChrome* chrome_ = nullptr;
    QLabel* statusSelf_ = nullptr;
    QLabel* statusTime_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    QTimer* trayTipTimer_ = nullptr;
    bool background_ = false;      // hidden or minimized: log only, render nothing
    uint64_t renderedSeq_ = 0;     // last frame the views were given
    int bgHitches_ = 0, bgFreezes_ = 0;
    double bgWorstFreezeMs_ = 0;
};

} // namespace culprit
