// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/MainWindow.h"

#include "core/Engine.h"
#include "core/Format.h"
#include "core/Version.h"
#include "gui/AboutDialog.h"
#include "gui/Dialogs.h"
#include "gui/SettingsDialog.h"
#include "gui/ThemeManager.h"
#include "gui/tabs/LoadIoTab.h"
#include "gui/tabs/OverviewTab.h"
#include "core/record/Recorder.h"
#include "core/report/ReportExporter.h"
#include "gui/tabs/ProcessesTab.h"
#include "gui/tabs/RecordingsTab.h"
#include "gui/tabs/StutterTab.h"
#include "gui/widgets/LayoutBarrier.h"
#include "gui/tabs/ThermalsTab.h"
#include "gui/widgets/FindingsPanel.h"
#include "gui/widgets/TitleBar.h"
#include "gui/widgets/WindowChrome.h"

#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>

#include <cstdio>

namespace culprit {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), settings_(Settings::load())
{
    setWindowTitle(QStringLiteral("%1 — Linux diagnostic monitor").arg(QString::fromLatin1(kAppName)));
    resize(1360, 880);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    setCentralWidget(tabs_);

    overview_ = new OverviewTab;
    addTab(overview_, QStringLiteral("overview"), tr("Overview"));
    connect(overview_, &OverviewTab::navigate, this, &MainWindow::showTab);
    findings_ = new FindingsPanel;
    overview_->setBottomWidget(findings_);

    // Actions live on the window itself so their shortcuts work whichever bar
    // (system toolbar or integrated title bar) shows them.
    pauseAct_ = new QAction(tr("Pause"), this);
    pauseAct_->setCheckable(true);
    pauseAct_->setShortcut(Qt::Key_Space);
    pauseAct_->setToolTip(tr("Freeze the display (sampling pauses too) — Space"));
    recordAct_ = new QAction(tr("Record"), this);
    recordAct_->setCheckable(true);
    recordAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    recordAct_->setToolTip(tr("Record hitches, findings and 1 Hz summaries to a file you can review later (Recordings tab) — Ctrl+R"));
    exportAct_ = new QAction(tr("Export report…"), this);
    exportAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    exportAct_->setToolTip(tr("Save a text or JSON diagnostic report of the current state, findings and recent hitches — Ctrl+E"));
    settingsAct_ = new QAction(tr("Settings…"), this);
    settingsAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settingsAct_->setToolTip(tr("Appearance, sampling, stutter detection and diagnosis settings — Ctrl+,"));
    aboutAct_ = new QAction(tr("About"), this);
    aboutAct_->setToolTip(tr("Version, copyright and licenses"));
    addActions({pauseAct_, recordAct_, exportAct_, settingsAct_, aboutAct_});
    connect(recordAct_, &QAction::toggled, this, &MainWindow::toggleRecording);
    connect(exportAct_, &QAction::triggered, this, &MainWindow::exportReport);
    connect(settingsAct_, &QAction::triggered, this, &MainWindow::showSettings);
    connect(aboutAct_, &QAction::triggered, this, &MainWindow::showAbout);

    toolbar_ = addToolBar(tr("Main"));
    toolbar_->setObjectName(QStringLiteral("mainToolbar"));
    toolbar_->setMovable(false);
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar_->addActions({pauseAct_, recordAct_, exportAct_});
    toolbar_->addSeparator();
    toolbar_->addAction(settingsAct_);
    toolbar_->addAction(aboutAct_);
    for (QToolButton* b : toolbar_->findChildren<QToolButton*>())
        b->setFocusPolicy(Qt::NoFocus);   // no stray focus highlight when the window is re-created

    chrome_ = new WindowChrome(this);
    applyChrome();

    statusTime_ = new QLabel;
    statusSelf_ = new QLabel;
    statusRec_ = new QLabel;
    statusBar()->addWidget(statusTime_, 1);
    statusBar()->addPermanentWidget(statusRec_);
    statusBar()->addPermanentWidget(statusSelf_);

    engine_ = std::make_unique<EngineHost>(settings_);
    connect(engine_->engine(), &Engine::frameReady, this, &MainWindow::onFrame, Qt::QueuedConnection);

    processes_ = new ProcessesTab(engine_->engine());
    addTab(processes_, QStringLiteral("processes"), tr("Processes"));
    connect(processes_, &ProcessesTab::statusMessage, this, [this](const QString& m) { statusBar()->showMessage(m, 8000); });

    auto* thermals = new ThermalsTab;
    connect(tabs_, &QTabWidget::currentChanged, this,
            [this, thermals](int) { engine_->engine()->setSensorsWatched(tabs_->currentWidget() == thermals->parentWidget()); });
    addTab(thermals, QStringLiteral("thermals"), tr("Thermals && Power"));
    connect(thermals, &ThermalsTab::processActivated, this, &MainWindow::showProcess);

    auto* load = new LoadIoTab;
    addTab(load, QStringLiteral("load"), tr("Load && I/O"));
    connect(load, &LoadIoTab::processActivated, this, &MainWindow::showProcess);
    connect(findings_, &FindingsPanel::processActivated, this, &MainWindow::showProcess);

    stutter_ = new StutterTab(engine_->engine()->latencyFeed(), settings_);
    addTab(stutter_, QStringLiteral("stutter"), tr("Stutter"));
    connect(engine_->engine(), &Engine::hitchDetected, stutter_, &StutterTab::addHitch, Qt::QueuedConnection);
    connect(stutter_, &StutterTab::processActivated, this, &MainWindow::showProcess);
    connect(stutter_, &StutterTab::probeSettingsChanged, this, [this](ProbeMode mode, double thresholdMs) {
        settings_.probeMode = mode;
        if (mode == ProbeMode::PerCpu)
            settings_.perCpuThresholdMs = thresholdMs;
        else if (mode == ProbeMode::Realtime)
            settings_.realtimeThresholdMs = thresholdMs;
        else if (mode == ProbeMode::Floating)
            settings_.floatingThresholdMs = thresholdMs;
        settings_.save();
        engine_->engine()->applySettings(settings_);
    });
    connect(processes_, &ProcessesTab::focusRequested, this, [this](int pid, const QString& name) {
        stutter_->setFocusProcess(pid, name);
        engine_->engine()->setFocusPid(pid);
        showTab(QStringLiteral("stutter"));
    });
    connect(stutter_, &StutterTab::focusCleared, this, [this] { engine_->engine()->setFocusPid(0); });
    connect(stutter_, &StutterTab::deepTraceToggled, this, [this](bool on) { engine_->engine()->setDeepTrace(on); });
    connect(engine_->engine(), &Engine::hitchUpdated, stutter_, &StutterTab::updateHitch, Qt::QueuedConnection);

    recordings_ = new RecordingsTab(settings_.effectiveRecordingsDir());
    addTab(recordings_, QStringLiteral("recordings"), tr("Recordings"));
    connect(recordings_, &RecordingsTab::statusMessage, this, [this](const QString& m) { statusBar()->showMessage(m, 8000); });
    connect(engine_->engine(), &Engine::hitchDetected, this, [this](const Hitch& h) {
        if (recorder_)
            recorder_->writeHitch(h);
    }, Qt::QueuedConnection);
    connect(engine_->engine(), &Engine::hitchUpdated, this, [this](const Hitch& h) {
        if (recorder_)
            recorder_->writeHitch(h, true);
    }, Qt::QueuedConnection);
    connect(pauseAct_, &QAction::toggled, this, [this](bool on) { engine_->engine()->setPaused(on); });
    engine_->start();
}

MainWindow::~MainWindow()
{
    engine_->shutdown();
    if (recorder_)
        recorder_->close();
}

void MainWindow::applyChrome()
{
    const TitleBarDecision d = decideTitleBar(settings_.titleBarMode);
    WindowChrome::setIntegratedTitleBars(d.integrated);
    if (qEnvironmentVariableIsSet("CULPRIT_DEBUG_THEME"))
        std::fprintf(stderr, "culprit: title bar: %s\n", qPrintable(d.reason));
    const bool wasVisible = isVisible();
    const bool maximized = isMaximized();
    const bool frameless = windowFlags() & Qt::FramelessWindowHint;
    if (d.integrated) {
        if (!titleBar_) {
            titleBar_ = new TitleBar(this);
            titleBar_->addAction(pauseAct_);
            titleBar_->addAction(recordAct_);
            titleBar_->addAction(exportAct_);
            titleBar_->addSeparator();
            titleBar_->addAction(settingsAct_);
            titleBar_->addAction(aboutAct_);
            setMenuWidget(titleBar_);
        }
        toolbar_->hide();
    } else {
        if (titleBar_)
            setMenuWidget(nullptr);   // QMainWindow deletes the old one
        toolbar_->show();
    }
    chrome_->setActive(d.integrated);
    setContentsMargins(d.integrated && !maximized ? QMargins(1, 1, 1, 1) : QMargins());
    if (frameless != d.integrated) {
        // Changing frame flags recreates the native window; show it again as it was.
        setWindowFlag(Qt::FramelessWindowHint, d.integrated);
        if (wasVisible)
            maximized ? showMaximized() : show();
    }
}

void MainWindow::paintEvent(QPaintEvent* e)
{
    QMainWindow::paintEvent(e);
    if (chrome_ && chrome_->isActive())
        WindowChrome::paintOutline(this);
}

void MainWindow::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && chrome_ && chrome_->isActive())
        setContentsMargins(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen) ? QMargins() : QMargins(1, 1, 1, 1));
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(settings_, lastFrame_ ? lastFrame_->thermal.tjmaxC : 95, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const Settings before = settings_;
    settings_ = dlg.settings();
    settings_.save();
    if (settings_.themeMode != before.themeMode)
        ThemeManager::instance().setMode(settings_.themeMode);
    if (settings_.titleBarMode != before.titleBarMode)
        applyChrome();
    engine_->engine()->applySettings(settings_);
    stutter_->setSettings(settings_);
    recordings_->setDirectory(settings_.effectiveRecordingsDir());
    statusBar()->showMessage(tr("Settings saved."), 4000);
}

void MainWindow::showAbout()
{
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::openRecording(const QString& path)
{
    showTab(QStringLiteral("recordings"));
    recordings_->openFile(path);
}

void MainWindow::toggleRecording(bool on)
{
    if (on) {
        recorder_ = std::make_unique<Recorder>();
        QString err;
        if (!recorder_->open(settings_.effectiveRecordingsDir(), &err)) {
            recorder_.reset();
            recordAct_->blockSignals(true);
            recordAct_->setChecked(false);
            recordAct_->blockSignals(false);
            Dialogs::warning(this, tr("Cannot record"), err);
            return;
        }
        statusBar()->showMessage(tr("Recording to %1").arg(recorder_->path()), 8000);
    } else if (recorder_) {
        const QString path = recorder_->path();
        recorder_->close();
        recorder_.reset();
        statusRec_->clear();
        statusBar()->showMessage(tr("Recording saved: %1").arg(path), 10000);
        recordings_->refresh();
    }
}

void MainWindow::exportReport()
{
    if (!lastFrame_)
        return;
    const QString name = QStringLiteral("culprit-report-%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString filter;
    QString path = QFileDialog::getSaveFileName(this, tr("Export diagnostic report"), QDir::home().filePath(name),
                                                tr("Text report (*.txt);;JSON report (*.json)"), &filter);
    if (path.isEmpty())
        return;
    if (filter.contains(QLatin1String("json")) && !path.endsWith(QLatin1String(".json")))
        path += QStringLiteral(".json");
    const std::vector<Hitch> hitches(stutter_->hitches().begin(), stutter_->hitches().end());
    QString err;
    if (!Report::saveSnapshot(path, *lastFrame_, hitches, &err))
        Dialogs::warning(this, tr("Export failed"), err);
    else
        statusBar()->showMessage(tr("Report written to %1").arg(path), 10000);
}

void MainWindow::addTab(TabPage* page, const QString& name, const QString& title)
{
    pages_.emplace_back(name, page);
    tabs_->addTab(new LayoutBarrier(page), title);
}

void MainWindow::showTab(const QString& name)
{
    for (const auto& [n, page] : pages_) {
        if (n == name) {
            tabs_->setCurrentWidget(page->parentWidget());   // the page's LayoutBarrier
            return;
        }
    }
}

void MainWindow::showProcess(int pid)
{
    showTab(QStringLiteral("processes"));
    processes_->selectPid(pid);
}

void MainWindow::onFrame(const FramePtr& frame)
{
    static const bool timing = qEnvironmentVariableIsSet("CULPRIT_DEBUG_TIMING");
    QElapsedTimer t;
    t.start();
    QString report;
    for (const auto& [name, page] : pages_) {
        const qint64 t0 = t.nsecsElapsed();
        page->onFrame(frame);
        if (timing)
            report += QStringLiteral("%1 %2 ms, ").arg(name).arg(double(t.nsecsElapsed() - t0) / 1e6, 0, 'f', 2);
    }
    const qint64 t1 = t.nsecsElapsed();
    findings_->setFindings(frame->findings);
    lastFrame_ = frame;
    if (recorder_) {
        recorder_->writeFrame(*frame);
        statusRec_->setText(tr("● Recording %1 · %2 hitch(es)").arg(fmt::bytes(double(recorder_->bytes()))).arg(recorder_->hitchCount()));
        statusRec_->setStyleSheet(QStringLiteral("color:#e5484d"));
    }
    if (timing)
        std::fprintf(stderr, "gui: %sfindings %.2f ms\n", qPrintable(report), double(t.nsecsElapsed() - t1) / 1e6);

    statusTime_->setText(tr("%1 · up %2 · %3 threads")
                             .arg(fmt::wallTime(frame->wallMs).left(8), fmt::duration(frame->sys.uptimeSec))
                             .arg(frame->sys.threadsTotal));
    statusSelf_->setText(tr("Culprit %1 CPU · %2").arg(fmt::percent(frame->self.cpuPct), fmt::bytes(double(frame->self.rssBytes))));
    statusSelf_->setToolTip(tr("Culprit's own CPU use and memory; last sample took %1.").arg(fmt::ms(frame->self.collectMs)));
}

} // namespace culprit
