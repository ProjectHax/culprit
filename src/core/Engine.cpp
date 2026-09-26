// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/Engine.h"

#include "common/fs/File.h"
#include "common/parse/PidParsers.h"
#include "common/util/Clock.h"
#include "core/analysis/Attribution.h"
#include "core/analysis/RuleEngine.h"
#include "core/collect/CgroupCollector.h"
#include "core/collect/GpuCollector.h"
#include "core/collect/HwmonCollector.h"
#include "core/collect/PowerCollector.h"
#include "core/collect/ProcessCollector.h"
#include "core/collect/SystemCollector.h"
#include "core/helper/HelperClient.h"
#include "core/stutter/FlightRecorder.h"
#include "core/stutter/HitchAnalyzer.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSettings>
#include <QTimer>

#include <cstdio>
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

namespace culprit {

Engine::Engine(const Settings& settings) : settings_(settings), feed_(std::make_shared<LatencyFeed>()) {}

Engine::~Engine() = default;

void Engine::start()
{
    // Our own sampling must not compete with what we're diagnosing.
    setpriority(PRIO_PROCESS, 0, 5);

    // Collectors keep procfs fds open (one per process + per hot thread).
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 65536);
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    system_ = std::make_unique<SystemCollector>();
    cpufreq_ = std::make_unique<CpufreqCollector>();
    procs_ = std::make_unique<ProcessCollector>();
    hwmon_ = std::make_unique<HwmonCollector>();
    power_ = std::make_unique<PowerCollector>();
    power_->setKnownFloor(QSettings().value(QStringLiteral("power/idleFloorW")).toDouble());
    gpu_ = std::make_unique<GpuCollector>();
    cgroups_ = std::make_unique<CgroupCollector>();
    rules_ = std::make_unique<RuleEngine>(settings_);
    analyzer_ = std::make_unique<HitchAnalyzer>();
    flight_ = std::make_unique<FlightRecorder>(feed_, [this](std::shared_ptr<HitchCapture> cap) {
        // Called on the flight-recorder thread; analyse on ours.
        QMetaObject::invokeMethod(this, [this, cap] { onCapture(cap); }, Qt::QueuedConnection);
    });
    startProbes();

    helper_ = new HelperClient(this);
    connect(helper_, &HelperClient::received, this, &Engine::onHelperEvent);
    connect(helper_, &HelperClient::stateChanged, this, [this](HelperStatus::State st, const QString& msg) {
        helperStatus_.state = st;
        helperStatus_.message = msg;
        if (st != HelperStatus::State::Running) {
            awaitingWindow_.clear();
            helperDState_.clear();
            helperIo_.clear();
            deepLatest_ = {};
        } else if (focusPid_ > 0) {
            helper_->setFocus(focusPid_);
        }
    });

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &Engine::tick);
    timer_->start(settings_.intervalMs);
    tick();   // prime the delta counters
}

void Engine::startProbes()
{
    probeError_.clear();
    const int ncpu = int(sysconf(_SC_NPROCESSORS_CONF));
    if (!flight_->start(settings_, hwmon_->cpuTempPath(), ncpu, &probeError_))
        return;
    if (settings_.probeMode == ProbeMode::Realtime) {
        for (pid_t tid : flight_->probeTids()) {
            QString err;
            if (!makeThreadRealtime(tid, 10, &err)) {
                probeError_ = tr("Could not get real-time priority for the probes (%1); measuring as normal threads.").arg(err);
                break;
            }
        }
    }
}

void Engine::onCapture(const std::shared_ptr<HitchCapture>& cap)
{
    const double tjmax = lastFrame_ ? lastFrame_->thermal.tjmaxC : 95;
    const int64_t t0 = cap->hits.empty() ? 0 : cap->hits.front().expectedNs;
    Hitch h = analyzer_->analyze(*cap, lastFrame_.get(), hwmon_->busyIntervals(t0 - 1'000'000'000), tjmax);
    rules_->noteHitch(h);
    emit hitchDetected(h);
    // Ask the helper what exactly occupied that CPU during the stall.
    if (helper_ && helper_->state() == HelperStatus::State::Running && h.worstCpu >= 0) {
        awaitingWindow_[h.id] = h;
        helper_->requestWindow(h.id, h.worstCpu, h.t0Ns - 1'000'000, h.t1Ns);
        if (awaitingWindow_.size() > 64)
            awaitingWindow_.erase(awaitingWindow_.begin());
    }
}

void Engine::stop()
{
    if (timer_)
        timer_->stop();
    delete helper_;   // tells the root helper to quit (it also exits on stdin EOF)
    helper_ = nullptr;
    if (flight_)
        flight_->stop();
    if (power_ && power_->floor() > 0)
        QSettings().setValue(QStringLiteral("power/idleFloorW"), power_->floor());
}

void Engine::applySettings(const Settings& s)
{
    QMetaObject::invokeMethod(this, [this, s] {
        const bool probesChanged = s.probeMode != settings_.probeMode || s.floatingProbes != settings_.floatingProbes ||
                                   s.floatingThresholdMs != settings_.floatingThresholdMs ||
                                   s.perCpuThresholdMs != settings_.perCpuThresholdMs ||
                                   s.realtimeThresholdMs != settings_.realtimeThresholdMs;
        settings_ = s;
        if (rules_)
            rules_->setSettings(s);
        if (probesChanged && flight_) {
            flight_->stop();
            startProbes();
        }
        if (timer_)
            timer_->setInterval(s.intervalMs);
    }, Qt::QueuedConnection);
}

void Engine::setPaused(bool paused)
{
    QMetaObject::invokeMethod(this, [this, paused] { paused_ = paused; }, Qt::QueuedConnection);
}

void Engine::setDetailPids(const QList<int>& pids)
{
    QMetaObject::invokeMethod(this, [this, pids] { detailPids_ = std::unordered_set<int>(pids.begin(), pids.end()); },
                              Qt::QueuedConnection);
}

void Engine::setFocusPid(int pid)
{
    QMetaObject::invokeMethod(this, [this, pid] {
        focusPid_ = pid;
        if (helper_)
            helper_->setFocus(pid);
    }, Qt::QueuedConnection);
}

void Engine::setDeepTrace(bool on)
{
    QMetaObject::invokeMethod(this, [this, on] {
        if (!helper_)
            return;
        if (on) {
            const double thr = settings_.probeMode == ProbeMode::PerCpu     ? settings_.perCpuThresholdMs
                               : settings_.probeMode == ProbeMode::Realtime ? settings_.realtimeThresholdMs
                                                                            : settings_.floatingThresholdMs;
            helper_->start(std::max(200, int(thr * 1000)));
        } else {
            helper_->stop();
        }
    }, Qt::QueuedConnection);
}

namespace {

qint64 wallFromMono(int64_t tNs)
{
    return QDateTime::currentMSecsSinceEpoch() - (monoNs() - tNs) / 1'000'000;
}

std::vector<double> toVector(const QJsonArray& a)
{
    std::vector<double> v;
    v.reserve(size_t(a.size()));
    for (const QJsonValue& x : a)
        v.push_back(x.toDouble());
    return v;
}

} // namespace

void Engine::onHelperEvent(const QJsonObject& ev)
{
    const QString t = ev.value(QStringLiteral("t")).toString();
    auto str = [&](const char* k) { return ev.value(QLatin1String(k)).toString(); };
    auto num = [&](const char* k) { return ev.value(QLatin1String(k)).toDouble(); };

    if (t == QLatin1String("window")) {
        auto it = awaitingWindow_.find(quint64(num("id")));
        if (it == awaitingWindow_.end())
            return;
        Hitch h = std::move(it->second);
        awaitingWindow_.erase(it);
        h.blockers.clear();
        for (const QJsonValue& v : ev.value(QStringLiteral("slices")).toArray()) {
            const QJsonObject o = v.toObject();
            DeepBlocker b;
            b.kind = o.value(QStringLiteral("k")).toString();
            b.tid = o.value(QStringLiteral("tid")).toInt();
            b.pid = o.value(QStringLiteral("pid")).toInt();
            b.name = o.value(QStringLiteral("name")).toString();
            b.ms = o.value(QStringLiteral("us")).toDouble() / 1000.0;
            if (b.kind == QLatin1String("idle"))
                b.name = tr("idle (CPU had nothing to run)");
            h.blockers.push_back(b);
        }
        h.deepResolved = true;
        // Deep trace names the task that actually held the CPU: promote it.
        for (const DeepBlocker& b : h.blockers) {
            if (b.kind != QLatin1String("task") || b.ms < h.maxOvershootMs * 0.3)
                continue;
            h.suspects.insert(h.suspects.begin(), Suspect{QStringLiteral("process"), QStringLiteral("%1 (%2)").arg(b.name).arg(b.pid ? b.pid : b.tid),
                                                         b.pid ? b.pid : b.tid, 1.0, tr("held CPU %1 for %2 of the stall (deep trace)").arg(h.worstCpu).arg(b.ms, 0, 'f', 2) + tr(" ms")});
            if (h.cls == HitchClass::RunQueue)
                h.summary = tr("%1 ms stall on CPU %2: waited for the CPU while %3 ran").arg(h.maxOvershootMs, 0, 'f', 2).arg(h.worstCpu).arg(b.name);
            break;
        }
        emit hitchUpdated(h);
    } else if (t == QLatin1String("rqlat") || t == QLatin1String("irq") || t == QLatin1String("softirq") ||
               t == QLatin1String("reclaim") || t == QLatin1String("compact") || t == QLatin1String("blk")) {
        DeepEvent d;
        d.kind = t;
        d.tNs = qint64(num("ts"));
        d.wallMs = wallFromMono(d.tNs);
        d.cpu = ev.contains(QStringLiteral("cpu")) ? ev.value(QStringLiteral("cpu")).toInt() : -1;
        d.pid = ev.value(QStringLiteral("pid")).toInt();
        d.tid = ev.value(QStringLiteral("tid")).toInt();
        d.ms = num("us") / 1000.0;
        if (t == QLatin1String("rqlat")) {
            d.name = str("comm");
            QStringList parts;
            for (const QJsonValue& v : ev.value(QStringLiteral("blockers")).toArray()) {
                const QJsonObject o = v.toObject();
                parts << QStringLiteral("%1 %2 ms").arg(o.value(QStringLiteral("name")).toString())
                             .arg(o.value(QStringLiteral("us")).toDouble() / 1000.0, 0, 'f', 2);
            }
            d.detail = tr("waited (%1) while: %2").arg(str("why"), parts.join(QStringLiteral(", ")));
        } else if (t == QLatin1String("irq")) {
            d.name = tr("IRQ %1 %2").arg(ev.value(QStringLiteral("irq")).toInt()).arg(str("name"));
        } else if (t == QLatin1String("softirq")) {
            d.name = tr("softirq %1").arg(str("vec"));
        } else if (t == QLatin1String("blk")) {
            d.name = tr("%1 %2 %3 bytes").arg(str("dev"), str("rwbs")).arg(qint64(num("bytes")));
            d.detail = tr("issued by %1").arg(str("comm"));
        } else {
            d.name = str("comm");
            d.detail = t == QLatin1String("reclaim") ? tr("direct memory reclaim") : tr("memory compaction");
        }
        deepEvents_.push_back(d);
        if (deepEvents_.size() > 500)
            deepEvents_.erase(deepEvents_.begin(), deepEvents_.begin() + 100);
    } else if (t == QLatin1String("rqtop")) {
        deepLatest_.topWaiters.clear();
        for (const QJsonValue& v : ev.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject o = v.toObject();
            DeepWaiter w;
            w.pid = o.value(QStringLiteral("pid")).toInt();
            w.tid = o.value(QStringLiteral("tid")).toInt();
            w.comm = o.value(QStringLiteral("comm")).toString();
            w.count = o.value(QStringLiteral("n")).toInt();
            w.sumMs = o.value(QStringLiteral("sum_us")).toDouble() / 1000.0;
            w.maxMs = o.value(QStringLiteral("max_us")).toDouble() / 1000.0;
            deepLatest_.topWaiters.push_back(w);
        }
    } else if (t == QLatin1String("hist")) {
        deepLatest_.rqHist = toVector(ev.value(QStringLiteral("rq")).toArray());
        deepLatest_.irqHist = toVector(ev.value(QStringLiteral("irq")).toArray());
        deepLatest_.softirqHist = toVector(ev.value(QStringLiteral("softirq")).toArray());
    } else if (t == QLatin1String("dstate")) {
        helperDState_.clear();
        for (const QJsonValue& v : ev.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject o = v.toObject();
            DStateThread d;
            d.pid = o.value(QStringLiteral("pid")).toInt();
            d.tid = o.value(QStringLiteral("tid")).toInt();
            d.comm = o.value(QStringLiteral("comm")).toString();
            d.procComm = o.value(QStringLiteral("proc")).toString();
            d.wchan = o.value(QStringLiteral("wchan")).toString();
            for (const QJsonValue& fr : o.value(QStringLiteral("stack")).toArray())
                d.stack << fr.toString();
            d.ageSec = o.value(QStringLiteral("age_ms")).toDouble() / 1000.0;
            helperDState_.push_back(d);
        }
        helperDStateNs_ = monoNs();
    } else if (t == QLatin1String("pio")) {
        helperIo_.clear();
        for (const QJsonValue& v : ev.value(QStringLiteral("rows")).toArray()) {
            const QJsonArray r = v.toArray();
            if (r.size() == 3)
                helperIo_[r[0].toInt()] = {r[1].toDouble(), r[2].toDouble()};
        }
        helperIoNs_ = monoNs();
    } else if (t == QLatin1String("lost")) {
        helperStatus_.lostEvents += quint64(num("n"));
    } else if (t == QLatin1String("status")) {
        helperStatus_.eventsPerSec = num("events_ps");
        helperStatus_.cpuPct = num("cpu_pct");
    } else if (t == QLatin1String("started")) {
        helperStatus_.caps.clear();
        for (const QJsonValue& v : ev.value(QStringLiteral("tracepoints")).toArray())
            helperStatus_.caps << v.toString();
    } else if (t == QLatin1String("err") || t == QLatin1String("warn")) {
        helperStatus_.message = str("msg");
    }
}

void Engine::mergeHelperData(Frame& f)
{
    f.helper = helperStatus_;
    if (helperStatus_.state != HelperStatus::State::Running)
        return;
    const int64_t now = monoNs();
    // Root sees every blocked thread with its kernel stack: replace our partial view.
    if (now - helperDStateNs_ < 3'000'000'000LL) {
        if (!helperDState_.empty() || f.sys.procsBlocked == 0)
            f.dstate = helperDState_;
    }
    if (now - helperIoNs_ < 3'000'000'000LL) {
        for (ProcSample& p : f.procs) {
            auto it = helperIo_.find(p.key.pid);
            if (it != helperIo_.end()) {
                p.ioReadBps = it->second.first;
                p.ioWriteBps = it->second.second;
            } else if (p.ioReadBps < 0) {
                p.ioReadBps = p.ioWriteBps = 0;
            }
        }
    }
    f.deep = deepLatest_;
    f.deep.events = std::move(deepEvents_);
    deepEvents_.clear();
}

void Engine::setSensorsWatched(bool watched)
{
    QMetaObject::invokeMethod(this, [this, watched] {
        if (hwmon_)
            hwmon_->setWatched(watched);
    }, Qt::QueuedConnection);
}

void Engine::sampleSelf(SelfStats& out, int64_t nowNs)
{
    // Sum per-thread schedstat run time: ns precision, all of our threads.
    std::string buf;
    uint64_t runNs = 0;
    const std::string taskDir = SysPaths::proc_("self/task");
    if (DIR* d = opendir(taskDir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            TaskSchedstat ts;
            if (readFile(taskDir + "/" + e->d_name + "/schedstat", buf) && parseTaskSchedstat(buf, ts))
                runNs += ts.runNs;
        }
        closedir(d);
    }
    if (lastSelfNs_ > 0 && nowNs > lastSelfNs_ && runNs >= lastSelfRunNs_)
        out.cpuPct = double(runNs - lastSelfRunNs_) / double(nowNs - lastSelfNs_) * 100.0;
    lastSelfRunNs_ = runNs;
    lastSelfNs_ = nowNs;

    PidStat ps;
    if (readFile(SysPaths::proc_("self/stat"), buf) && parsePidStat(buf, ps))
        out.rssBytes = uint64_t(ps.rssPages) * uint64_t(sysconf(_SC_PAGESIZE));
}

void Engine::tick()
{
    if (paused_)
        return;
    const int64_t t0 = monoNs();
    auto frame = std::make_shared<Frame>();
    frame->seq = ++seq_;
    frame->tNs = t0;
    frame->wallMs = QDateTime::currentMSecsSinceEpoch();

    system_->sample(frame->sys, t0);
    cpufreq_->sample(frame->sys, t0);
    static const bool timing = qEnvironmentVariableIsSet("CULPRIT_DEBUG_TIMING");
    if (timing)
        std::fprintf(stderr, "system+cpufreq: %.2f ms\n", double(monoNs() - t0) / 1e6);

    const int64_t tpStart = monoNs();
    ProcessCollector::Config pc;
    pc.hotCount = settings_.hotProcessCount;
    pc.detailPids = detailPids_;
    pc.focusPid = focusPid_;
    if (focusPid_ > 0)
        pc.detailPids.insert(focusPid_);   // per-thread rows for the focus panel
    procs_->sample(*frame, t0, pc);
    flight_->setHotPids(procs_->hotPids());
    flight_->takeAverages(frame->sys.avgRunnable, frame->sys.avgBlocked);
    StutterSummary& st = frame->stutter;
    st.mode = feed_->mode;
    st.probes = feed_->probes;
    st.thresholdMs = feed_->thresholdMs;
    st.worstMs = st.probes > 0 ? double(feed_->takeWorstUs()) / 1000.0 : -1;
    st.hitchesTotal = feed_->hitches;
    st.suppressed = feed_->suppressed;
    st.error = probeError_;

    const int64_t tp = monoNs();
    hwmon_->sample(frame->sensors, frame->thermal, settings_.tjmaxC);
    power_->sample(frame->power, t0);
    const int64_t tg = monoNs();
    gpu_->sample(*frame);
    attributeCpuPower(*frame);
    mergeHelperData(*frame);
    const int64_t tr = monoNs();
    cgroups_->sample(*frame, t0);
    rules_->evaluate(*frame);
    if (timing)
        std::fprintf(stderr, "processes %.2f ms, hwmon+rapl %.2f ms, gpu %.2f ms, cgroups+rules %.2f ms\n",
                     double(tp - tpStart) / 1e6, double(tg - tp) / 1e6, double(tr - tg) / 1e6, double(monoNs() - tr) / 1e6);

    sampleSelf(frame->self, t0);
    frame->self.collectMs = double(monoNs() - t0) / 1e6;

    if (seq_ > 1) {   // the first frame has no deltas yet
        lastFrame_ = frame;
        emit frameReady(std::move(frame));
    }
}

// ------------------------------------------------------------------ host

EngineHost::EngineHost(const Settings& settings, QObject* parent) : QObject(parent)
{
    qRegisterMetaType<culprit::FramePtr>();
    qRegisterMetaType<culprit::Hitch>();
    engine_ = new Engine(settings);
    engine_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("culprit-engine"));
    connect(&thread_, &QThread::started, engine_, &Engine::start);
    connect(&thread_, &QThread::finished, engine_, &QObject::deleteLater);
}

EngineHost::~EngineHost() { shutdown(); }

void EngineHost::start()
{
    if (running_)
        return;
    running_ = true;
    thread_.start();
}

void EngineHost::shutdown()
{
    if (!running_)
        return;
    running_ = false;
    QMetaObject::invokeMethod(engine_, &Engine::stop, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}

} // namespace culprit
