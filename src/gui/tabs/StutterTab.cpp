// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/StutterTab.h"

#include "common/util/Clock.h"
#include "core/Format.h"
#include "core/stutter/FlightRecorder.h"
#include "gui/Theme.h"
#include "gui/widgets/DataTable.h"
#include "gui/widgets/HitchDetail.h"
#include "gui/widgets/Sparkline.h"
#include "gui/widgets/TimelineView.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace culprit {

StutterTab::StutterTab(std::shared_ptr<LatencyFeed> feed, const Settings& settings, QWidget* parent)
    : TabPage(parent), feed_(std::move(feed)), settings_(settings)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);

    auto* bar = new QHBoxLayout;
    mode_ = new QComboBox;
    mode_->addItem(tr("Off"), int(ProbeMode::Off));
    mode_->addItem(tr("Floating probes"), int(ProbeMode::Floating));
    mode_->addItem(tr("Per-CPU sweep"), int(ProbeMode::PerCpu));
    mode_->addItem(tr("Kernel latency (RT)"), int(ProbeMode::Realtime));
    mode_->setCurrentIndex(mode_->findData(int(settings_.probeMode)));
    mode_->setToolTip(tr("Latency probes are threads that sleep for 1 ms and measure how late they wake up.\n"
                         "Floating: a few probes placed by the scheduler, like an app's threads.\n"
                         "Per-CPU: one probe pinned to every CPU (more overhead) to locate per-CPU problems.\n"
                         "Kernel latency: real-time probes that measure only IRQ/kernel/firmware delays."));
    threshold_ = new QDoubleSpinBox;
    threshold_->setRange(0.1, 100);
    threshold_->setDecimals(1);
    threshold_->setSingleStep(0.5);
    threshold_->setSuffix(tr(" ms"));
    threshold_->setValue(thresholdFor(settings_.probeMode));
    threshold_->setToolTip(tr("A wakeup later than this counts as a hitch."));
    window_ = new QComboBox;
    window_->addItem(tr("10 s"), 10);
    window_->addItem(tr("30 s"), 30);
    window_->addItem(tr("2 min"), 120);
    deep_ = new QPushButton(tr("Deep trace…"));
    deep_->setCheckable(true);
    deep_->setToolTip(tr("Start the root helper (asks for your password via polkit) to trace the scheduler and "
                         "interrupts: every hitch then lists exactly which tasks/IRQs held the CPU."));
    auto* clear = new QPushButton(tr("Clear"));
    status_ = new QLabel;
    bar->addWidget(new QLabel(tr("Probes:")));
    bar->addWidget(mode_);
    bar->addWidget(new QLabel(tr("Threshold:")));
    bar->addWidget(threshold_);
    bar->addWidget(new QLabel(tr("Window:")));
    bar->addWidget(window_);
    bar->addWidget(deep_);
    bar->addWidget(clear);
    bar->addStretch(1);
    bar->addWidget(status_);
    v->addLayout(bar);
    deepStatus_ = new QLabel;
    deepStatus_->setWordWrap(true);
    deepStatus_->hide();
    v->addWidget(deepStatus_);

    graph_ = new TimelineView;
    graph_->setSeries({{tr("wakeup delay"), theme::blue(), true, false, {}}});
    graph_->setYUnit(tr("ms"));
    graph_->setYRange(0, 3, true);
    graph_->setEmptyText(tr("Latency probes are off."));
    graph_->setMinimumHeight(200);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(graph_);
    auto* lower = new QSplitter(Qt::Horizontal);
    table_ = new DataTable({tr("Time"), tr("Delay"), tr("Cause"), tr("CPU"), tr("Explanation")});
    table_->setNumericColumns({1, 3});
    table_->setEmptyText(tr("No hitches yet."));
    table_->sortByColumn(0, Qt::DescendingOrder);
    table_->setColumnWidth(0, 95);
    table_->setColumnWidth(1, 75);
    table_->setColumnWidth(2, 140);
    table_->setColumnWidth(3, 45);
    detail_ = new QTextBrowser;
    detail_->setOpenLinks(false);
    detail_->setPlaceholderText(tr("Select a hitch."));
    lists_ = new QTabWidget;
    lists_->addTab(table_, tr("Hitches"));
    waiters_ = new DataTable({tr("Thread"), tr("PID"), tr("TID"), tr("Waits"), tr("Total wait"), tr("Longest")});
    waiters_->setNumericColumns({1, 2, 3, 4, 5});
    waiters_->setEmptyText(tr("Requires Deep trace."));
    waiters_->sortByColumn(4, Qt::DescendingOrder);
    lists_->addTab(waiters_, tr("Run-queue waiters (deep)"));
    events_ = new DataTable({tr("Time"), tr("Kind"), tr("CPU"), tr("Duration"), tr("What"), tr("Details")});
    events_->setNumericColumns({2, 3});
    events_->setEmptyText(tr("Requires Deep trace."));
    events_->sortByColumn(0, Qt::DescendingOrder);
    lists_->addTab(events_, tr("Kernel events (deep)"));
    lower->addWidget(lists_);
    lower->addWidget(detail_);
    lower->setStretchFactor(0, 3);
    lower->setStretchFactor(1, 2);
    split->addWidget(lower);

    focusBox_ = new QWidget;
    auto* fh = new QHBoxLayout(focusBox_);
    fh->setContentsMargins(0, 0, 0, 0);
    focusLabel_ = new QLabel;
    focusLabel_->setWordWrap(true);
    focusLabel_->setTextFormat(Qt::RichText);
    focusLabel_->setToolTip(tr("What the focused process itself experiences: time its threads waited for a CPU, "
                               "preemptions and major page faults. The probes above show what disturbs the system."));
    focusGraph_ = new Sparkline(tr("Focus: run-queue wait"));
    focusGraph_->setSeries({tr("RQ wait ms/s"), tr("preempt/s ÷10")});
    focusGraph_->setAutoRange(10);
    focusGraph_->setFixedWidth(380);
    auto* clearFocus = new QPushButton(tr("Stop focusing"));
    fh->addWidget(focusLabel_, 1);
    fh->addWidget(focusGraph_);
    fh->addWidget(clearFocus, 0, Qt::AlignTop);
    focusBox_->hide();
    split->addWidget(focusBox_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    v->addWidget(split, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(100);   // 10 fps while visible
    connect(timer_, &QTimer::timeout, this, &StutterTab::refreshGraph);

    connect(mode_, &QComboBox::currentIndexChanged, this, &StutterTab::onModeChanged);
    connect(threshold_, &QDoubleSpinBox::editingFinished, this, [this] {
        emit probeSettingsChanged(ProbeMode(mode_->currentData().toInt()), threshold_->value());
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        hitches_.clear();
        selected_ = -1;
        detail_->clear();
        rebuildTable();
    });
    connect(deep_, &QPushButton::toggled, this, &StutterTab::deepTraceToggled);
    connect(graph_, &TimelineView::markerClicked, this, [this](qint64 id) { showHitch(id); });
    connect(table_, &DataTable::currentCellChanged, this, [this](int row) {
        if (row < 0)
            return;
        if (auto* it = table_->item(row, 0))
            showHitch(it->data(Qt::UserRole + 1).toLongLong());
    });
    connect(detail_, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) {
        if (u.scheme() == QLatin1String("pid"))
            emit processActivated(u.path().toInt());
    });
    connect(clearFocus, &QPushButton::clicked, this, [this] {
        setFocusProcess(0, QString());
        emit focusCleared();
    });
}

double StutterTab::thresholdFor(ProbeMode m) const
{
    switch (m) {
    case ProbeMode::PerCpu: return settings_.perCpuThresholdMs;
    case ProbeMode::Realtime: return settings_.realtimeThresholdMs;
    default: return settings_.floatingThresholdMs;
    }
}

void StutterTab::setSettings(const Settings& s)
{
    settings_ = s;
    mode_->blockSignals(true);
    mode_->setCurrentIndex(mode_->findData(int(s.probeMode)));
    mode_->blockSignals(false);
    threshold_->blockSignals(true);
    threshold_->setValue(thresholdFor(s.probeMode));
    threshold_->blockSignals(false);
}

void StutterTab::onModeChanged()
{
    const ProbeMode m = ProbeMode(mode_->currentData().toInt());
    settings_.probeMode = m;
    threshold_->blockSignals(true);
    threshold_->setValue(thresholdFor(m));
    threshold_->blockSignals(false);
    emit probeSettingsChanged(m, threshold_->value());
}

void StutterTab::showEvent(QShowEvent* e)
{
    TabPage::showEvent(e);
    timer_->start();
    if (tableDirty_)
        rebuildTable();
}

void StutterTab::hideEvent(QHideEvent* e)
{
    TabPage::hideEvent(e);
    timer_->stop();
}

void StutterTab::setDeepTraceState(bool running, const QString& status)
{
    deep_->blockSignals(true);
    deep_->setChecked(running);
    deep_->blockSignals(false);
    deep_->setText(running ? tr("Deep trace: on") : tr("Deep trace…"));
    deepStatus_->setVisible(!status.isEmpty());
    deepStatus_->setText(status);
}

void StutterTab::addHitch(const Hitch& h)
{
    // Only record here; the view catches up on the (throttled) refresh timer,
    // so a burst of hitches can't make the GUI itself a source of stutter.
    hitches_.push_back(h);
    while (hitches_.size() > 500)
        hitches_.pop_front();
    tableDirty_ = true;
}

void StutterTab::updateHitch(const Hitch& h)
{
    for (Hitch& x : hitches_)
        if (x.id == h.id)
            x = h;
    if (qint64(h.id) == selected_)
        showHitch(selected_);
}

void StutterTab::rebuildTable()
{
    tableDirty_ = false;
    std::vector<DataTable::Row> rows;
    rows.reserve(hitches_.size());
    for (const Hitch& h : hitches_) {
        DataTable::Cell time(fmt::wallTime(h.wallMs), double(h.wallMs));
        time.id = qint64(h.id);
        DataTable::Cell cls(hitchClassName(h.cls));
        cls.color = hitchColor(h.cls);
        QString expl = h.summary;
        if (h.periodSec > 0)
            expl += tr("  [every %1 s]").arg(h.periodSec, 0, 'f', 2);
        rows.push_back({time, DataTable::Cell(fmt::ms(h.maxOvershootMs), h.maxOvershootMs, h.maxOvershootMs >= 16 ? theme::red() : QColor()),
                        cls, DataTable::Cell(QString::number(h.worstCpu), h.worstCpu), DataTable::Cell(expl)});
    }
    table_->setRows(rows);
}

void StutterTab::showHitch(qint64 id)
{
    followLatest_ = false;
    selected_ = id;
    graph_->setSelectedMarker(id);
    for (const Hitch& h : hitches_)
        if (qint64(h.id) == id) {
            detail_->setHtml(hitchHtml(h, palette()));
            return;
        }
}

void StutterTab::refreshGraph()
{
    if (++refreshCount_ % 5 == 0 && tableDirty_) {   // table/detail at most 2x per second
        rebuildTable();
        if (followLatest_ && !hitches_.empty()) {
            showHitch(qint64(hitches_.back().id));
            followLatest_ = true;
        }
    }
    const int windowSec = window_->currentData().toInt();
    const int64_t now = monoNs();
    std::vector<LatencyFeed::Point> pts;
    feed_->copySince(now - int64_t(windowSec) * 1'000'000'000, pts);
    std::vector<QPointF> lat;
    lat.reserve(pts.size());
    for (const auto& p : pts)
        lat.emplace_back(double(p.tNs - now) / 1e9, p.maxUs / 1000.0);
    graph_->setXRange(-windowSec, 0);
    graph_->setPoints(0, std::move(lat));
    const double thr = feed_->thresholdMs;
    graph_->setThreshold(feed_->probes > 0 ? thr : -1, tr("threshold %1").arg(fmt::ms(thr)));
    std::vector<TimelineView::Marker> markers;
    for (const Hitch& h : hitches_) {
        const double x = double(h.t0Ns - now) / 1e9;
        if (x >= -windowSec)
            markers.push_back({x, hitchColor(h.cls), QStringLiteral("%1\n%2").arg(fmt::wallTime(h.wallMs), h.summary), qint64(h.id)});
    }
    graph_->setMarkers(std::move(markers));
}

void StutterTab::setFocusProcess(int pid, const QString& name)
{
    focusPid_ = pid;
    focusName_ = name;
    focusGraph_->clear();
    focusBox_->setVisible(pid > 0);
}

void StutterTab::accumulate(const Frame& f)
{
    const bool deepOn = f.helper.state == HelperStatus::State::Running || f.helper.state == HelperStatus::State::Starting;
    QString deepMsg;
    if (f.helper.state == HelperStatus::State::Running)
        deepMsg = tr("Deep trace: %1 events/s · helper %2 CPU%3")
                      .arg(fmt::count(f.helper.eventsPerSec), fmt::percent(f.helper.cpuPct))
                      .arg(f.helper.lostEvents ? tr(" · %1 lost").arg(f.helper.lostEvents) : QString());
    else if (!f.helper.message.isEmpty())
        deepMsg = f.helper.message;
    setDeepTraceState(deepOn, deepMsg);
    if (!f.deep.events.empty()) {
        deepEvents_.insert(deepEvents_.end(), f.deep.events.begin(), f.deep.events.end());
        while (deepEvents_.size() > 400)
            deepEvents_.pop_front();
        eventsDirty_ = true;
    }
    if (isVisible() && f.helper.state == HelperStatus::State::Running) {
        std::vector<DataTable::Row> rows;
        for (const DeepWaiter& w : f.deep.topWaiters)
            rows.push_back({DataTable::Cell(w.comm), DataTable::Cell(QString::number(w.pid), w.pid),
                            DataTable::Cell(QString::number(w.tid), w.tid), DataTable::Cell(QString::number(w.count), w.count),
                            DataTable::Cell(fmt::ms(w.sumMs), w.sumMs, w.sumMs >= 100 ? theme::red() : QColor()),
                            DataTable::Cell(fmt::ms(w.maxMs), w.maxMs, w.maxMs >= 16 ? theme::red() : QColor())});
        waiters_->setRows(rows);
    }
    if (isVisible() && eventsDirty_) {
        eventsDirty_ = false;
        std::vector<DataTable::Row> rows;
        for (const DeepEvent& e : deepEvents_) {
            DataTable::Cell time(fmt::wallTime(e.wallMs), double(e.wallMs));
            rows.push_back({time, DataTable::Cell(e.kind), DataTable::Cell(e.cpu >= 0 ? QString::number(e.cpu) : QString(), e.cpu),
                            DataTable::Cell(fmt::ms(e.ms), e.ms, e.ms >= 10 ? theme::red() : QColor()),
                            DataTable::Cell(e.pid > 0 ? QStringLiteral("%1 (%2)").arg(e.name).arg(e.pid) : e.name), DataTable::Cell(e.detail)});
        }
        events_->setRows(rows);
    }

    if (focusPid_ > 0) {
        const ProcSample* p = f.findProc(focusPid_);
        if (!p) {
            focusLabel_->setText(tr("<b>Focus:</b> %1 (%2) has exited.").arg(focusName_.toHtmlEscaped()).arg(focusPid_));
        } else {
            focusGraph_->push({std::max(0.0, p->runDelayMsPs), std::max(0.0, p->nivcswPs) / 10.0});
            focusGraph_->setValueText(fmt::ms(std::max(0.0, p->runDelayMsPs)) + tr("/s"), p->runDelayMsPs >= 50);
            const ThreadSample* worst = nullptr;
            for (const ThreadSample& t : p->threadDetails)
                if (!worst || t.runDelayMsPs > worst->runDelayMsPs)
                    worst = &t;
            focusLabel_->setText(
                tr("<b>%1 (%2)</b> · CPU %3 · RQ wait %4/s · preempted %5/s · maj faults %6/s · %7 threads%8%9")
                    .arg(p->comm.toHtmlEscaped())
                    .arg(p->key.pid)
                    .arg(fmt::percent(p->cpuPct), fmt::ms(std::max(0.0, p->runDelayMsPs)),
                         p->nivcswPs >= 0 ? fmt::count(p->nivcswPs) : QStringLiteral("–"), fmt::count(p->majfltPs))
                    .arg(p->threads)
                    .arg(p->dThreads ? tr(" · <b>%1 in D state</b>").arg(p->dThreads) : QString())
                    .arg(worst && worst->runDelayMsPs > 1
                             ? tr(" · slowest thread %1 (%2/s)").arg(worst->comm.toHtmlEscaped(), fmt::ms(worst->runDelayMsPs))
                             : QString()));
        }
    }
}

void StutterTab::render(const Frame& f)
{
    const StutterSummary& s = f.stutter;
    int lastMinute = 0;
    const int64_t now = f.tNs;
    for (const Hitch& h : hitches_)
        if (now - h.t0Ns < 60'000'000'000LL)
            ++lastMinute;
    if (s.probes == 0)
        status_->setText(s.error.isEmpty() ? tr("Probes off") : s.error);
    else
        status_->setText(tr("%1 probes · worst %2 · %3 hitches/min").arg(s.probes).arg(fmt::ms(s.worstMs)).arg(lastMinute));
    status_->setToolTip(!s.error.isEmpty() ? s.error
                        : tr("Worst probe wakeup delay in the last second; hitches in the last minute.%1")
                              .arg(s.suppressed ? tr(" %1 hitches were not analysed (rate limit).").arg(s.suppressed) : QString()));
}

} // namespace culprit
