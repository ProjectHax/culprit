// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/stutter/FlightRecorder.h"

#include "common/parse/PidParsers.h"
#include "common/util/Clock.h"
#include "core/collect/GpuCollector.h"

#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <pthread.h>

namespace culprit {

namespace {
constexpr int64_t kTickNs = 20'000'000;          // 50 Hz
constexpr size_t kRingSize = 512;                // ~10 s
constexpr int64_t kPreNs = 600'000'000;          // capture before the hitch
constexpr int64_t kPostNs = 200'000'000;         // ... and after
constexpr int64_t kMergeNs = 50'000'000;         // late wakeups closer than this are one hitch
constexpr int64_t kMaxHitchNs = 1'000'000'000;   // split continuous stalling into 1 s pieces
constexpr int kMaxCapturesPerSec = 4;
constexpr uint64_t kFullEvery = uint64_t(kFlightFullPeriodNs / kTickNs);   // 10 Hz
constexpr uint64_t kIrqEvery = 20;                                          // 2.5 Hz
} // namespace

// ------------------------------------------------------------------ feed

void LatencyFeed::push(const Point& p)
{
    std::lock_guard lock(m_);
    pts_.push_back(p);
    worstUs_ = std::max(worstUs_, p.maxUs);
    while (!pts_.empty() && p.tNs - pts_.front().tNs > 120'000'000'000LL)
        pts_.pop_front();
}

void LatencyFeed::copySince(int64_t sinceNs, std::vector<Point>& out) const
{
    std::lock_guard lock(m_);
    out.clear();
    auto it = std::lower_bound(pts_.begin(), pts_.end(), sinceNs, [](const Point& p, int64_t t) { return p.tNs < t; });
    out.assign(it, pts_.end());
}

float LatencyFeed::takeWorstUs()
{
    std::lock_guard lock(m_);
    const float w = worstUs_;
    worstUs_ = 0;
    return w;
}

// ------------------------------------------------------------------ recorder

FlightRecorder::FlightRecorder(std::shared_ptr<LatencyFeed> feed, CaptureFn onCapture)
    : feed_(std::move(feed)), onCapture_(std::move(onCapture))
{
}

FlightRecorder::~FlightRecorder() { stop(); }

void FlightRecorder::initSample(FlightSample& s) const
{
    const size_t n = size_t(ncpu_);
    s.runNs.assign(n, 0);
    s.waitNs.assign(n, 0);
    s.busyTicks.assign(n, 0);
    s.irqTicks.assign(n, 0);
    s.softirqTicks.assign(n, 0);
    s.totalTicks.assign(n, 0);
    s.softirq.assign(n * kSoftirqCount, 0);
    s.freqKHz.assign(n, 0);
}

bool FlightRecorder::start(const Settings& settings, const std::string& cpuTempPath, int ncpu, QString* error)
{
    stop();
    ncpu_ = std::max(1, ncpu);
    ring_.assign(kRingSize, {});
    for (FlightSample& s : ring_)
        initSample(s);
    head_ = filled_ = 0;
    statFile_.setPath(SysPaths::proc_("stat"));
    schedFile_.setPath(SysPaths::proc_("schedstat"));
    softirqFile_.setPath(SysPaths::proc_("softirqs"));
    vmFile_.setPath(SysPaths::proc_("vmstat"));
    irqFile_.setPath(SysPaths::proc_("interrupts"));
    if (!cpuTempPath.empty())
        tempFile_.setPath(cpuTempPath);
    freqFiles_.clear();
    freqFiles_.resize(size_t(ncpu_));
    for (int c = 0; c < ncpu_; ++c) {
        const std::string base = SysPaths::sys_("devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/");
        CachedFile avg(base + "cpuinfo_avg_freq");
        freqFiles_[size_t(c)].setPath(avg.exists() ? base + "cpuinfo_avg_freq" : base + "scaling_cur_freq");
    }
    if (!applyProbeMode(settings, error))
        return false;
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void FlightRecorder::stop()
{
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    std::lock_guard lock(probeMutex_);
    probes_.stop();
}

bool FlightRecorder::applyProbeMode(const Settings& settings, QString* error)
{
    std::lock_guard lock(probeMutex_);
    probes_.stop();
    pending_.clear();
    mode_ = settings.probeMode;
    ProbeSet::Config cfg;
    cfg.mode = settings.probeMode == ProbeMode::Realtime ? ProbeMode::Floating : settings.probeMode;
    cfg.count = settings.floatingProbes;
    double thresholdMs = settings.floatingThresholdMs;
    switch (settings.probeMode) {
    case ProbeMode::PerCpu:
        cfg.periodUs = 2000;
        thresholdMs = settings.perCpuThresholdMs;
        break;
    case ProbeMode::Realtime:
        cfg.periodUs = 1000;
        thresholdMs = settings.realtimeThresholdMs;
        break;
    default:
        // 500 Hz: a stall 2 ms past the threshold is always caught, and two
        // probes wake the CPU 1000 times a second instead of 4000.
        cfg.periodUs = 2000;
        break;
    }
    thresholdNs_ = int64_t(thresholdMs * 1e6);
    const bool ok = probes_.start(cfg, ncpu_, error);
    feed_->probes = probes_.count();
    feed_->mode = int(mode_);
    feed_->thresholdMs = thresholdMs;
    return ok;
}

std::vector<pid_t> FlightRecorder::probeTids() const
{
    std::lock_guard lock(probeMutex_);
    return probes_.tids();
}

void FlightRecorder::setHotPids(std::vector<int> pids)
{
    std::lock_guard lock(hotMutex_);
    hotPids_ = std::move(pids);
}

void FlightRecorder::takeAverages(double& runnable, double& blocked)
{
    std::lock_guard lock(avgMutex_);
    if (avgCount_ == 0) {
        runnable = blocked = -1;
        return;
    }
    runnable = sumRunnable_ / avgCount_;
    blocked = sumBlocked_ / avgCount_;
    sumRunnable_ = sumBlocked_ = 0;
    avgCount_ = 0;
}

void FlightRecorder::loop()
{
    pthread_setname_np(pthread_self(), "culprit-flight");
    uint64_t n = 0;
    int64_t next = monoNs();
    while (!stop_.load(std::memory_order_relaxed)) {
        next += kTickNs;
        const timespec ts = nsToTimespec(next);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        const int64_t now = monoNs();
        if (now - next > 5 * kTickNs)
            next = now;   // we were starved/suspended; don't try to catch up
        tick(now, n++);
    }
}

void FlightRecorder::tick(int64_t now, uint64_t n)
{
    // Probes drain at 50 Hz. The procfs reads are expensive on many-CPU machines
    // (/proc/schedstat ~240 µs, /proc/interrupts ~520 µs with 32 CPUs), so they
    // run at 10 Hz and 2.5 Hz, plus once right after a probe reports a hitch:
    // that keeps a sample within ~60 ms after every stall at a third of the cost.
    static const bool timing = qEnvironmentVariableIsSet("CULPRIT_DEBUG_TIMING");
    const int64_t tA = timing ? monoNs() : 0;
    if (drainProbes(now))
        wantFull_ = wantIrq_ = true;
    const int64_t tB = timing ? monoNs() : 0;
    FlightSample& s = ring_[head_];
    s.tNs = now;
    s.valid = 0;
    if (n % kFullEvery == 0 || (wantFull_ && n - lastFullTick_ >= 2)) {
        readSched(s);
        readStat(s);
        readSoftirq(s);
        if (vmFile_.read(buf_) && parseVmstat(buf_, s.vm))
            s.valid |= FlightSample::kVm;
        lastFullTick_ = n;
        wantFull_ = false;
    }
    const int64_t tC = timing ? monoNs() : 0;
    if (n % 5 == 0) {
        int64_t mc = 0;
        if (tempFile_.readInt64(mc)) {
            s.tctl = float(double(mc) / 1000.0);
            s.valid |= FlightSample::kTemp;
        }
        for (int c = 0; c < ncpu_; ++c) {
            int64_t khz = 0;
            s.freqKHz[size_t(c)] = freqFiles_[size_t(c)].readInt64(khz) ? uint32_t(khz) : 0;
        }
        s.valid |= FlightSample::kFreq;
        uint64_t reasons = 0;
        if (Nvml::instance().ok() && Nvml::instance().eventReasons(0, reasons)) {
            s.gpuReasons = reasons;
            s.valid |= FlightSample::kGpu;
        }
    }
    const int64_t tD = timing ? monoNs() : 0;
    if (n % kIrqEvery == 0 || (wantIrq_ && n - lastIrqTick_ >= 5)) {
        readIrqs(now);
        lastIrqTick_ = n;
        wantIrq_ = false;
    }
    const int64_t tE = timing ? monoNs() : 0;
    if (n % 10 == 5)
        readHot(now);

    head_ = (head_ + 1) % ring_.size();
    filled_ = std::min(filled_ + 1, ring_.size());
    finalizeReady(now);
    if (timing) {
        // Per-second totals: probe drain, full counters, freq/temp/GPU, interrupts, hot processes.
        static double sum[5];
        const int64_t tF = monoNs();
        const int64_t parts[5] = {tB - tA, tC - tB, tD - tC, tE - tD, tF - tE};
        for (int i = 0; i < 5; ++i)
            sum[i] += double(parts[i]) / 1e6;
        if (n % 50 == 49) {
            std::fprintf(stderr, "flight/s: drain %.2f ms, counters %.2f ms, freq+temp+gpu %.2f ms, irqs %.2f ms, hot %.2f ms\n",
                         sum[0], sum[1], sum[2], sum[3], sum[4]);
            std::fill(std::begin(sum), std::end(sum), 0.0);
        }
    }
}

void FlightRecorder::readSched(FlightSample& s)
{
    if (!schedFile_.read(buf_) || !parseSchedstat(buf_, sched_))
        return;
    for (size_t c = 0; c < sched_.size() && c < size_t(ncpu_); ++c) {
        s.runNs[c] = sched_[c].runNs;
        s.waitNs[c] = sched_[c].waitNs;
    }
    s.valid |= FlightSample::kSched;
}

void FlightRecorder::readStat(FlightSample& s)
{
    if (!statFile_.read(buf_) || !parseProcStat(buf_, stat_))
        return;
    for (size_t c = 0; c < stat_.cpus.size() && c < size_t(ncpu_); ++c) {
        const CpuTimes& t = stat_.cpus[c];
        s.busyTicks[c] = t.busy();
        s.irqTicks[c] = t.irq;
        s.softirqTicks[c] = t.softirq;
        s.totalTicks[c] = t.total();
    }
    // procs_running counts us (we're running while reading it).
    s.procsRunning = uint16_t(stat_.procsRunning > 0 ? stat_.procsRunning - 1 : 0);
    s.procsBlocked = uint16_t(stat_.procsBlocked);
    s.forks = stat_.processes;
    s.valid |= FlightSample::kStat;
    std::lock_guard lock(avgMutex_);
    sumRunnable_ += s.procsRunning;
    sumBlocked_ += s.procsBlocked;
    ++avgCount_;
}

void FlightRecorder::readSoftirq(FlightSample& s)
{
    if (!softirqFile_.read(buf_) || !parseSoftirqs(buf_, softirqs_))
        return;
    for (int v = 0; v < kSoftirqCount; ++v) {
        const auto& per = softirqs_.perCpu[size_t(v)];
        for (size_t c = 0; c < per.size() && c < size_t(ncpu_); ++c)
            s.softirq[size_t(v) * size_t(ncpu_) + c] = per[c];
    }
    s.valid |= FlightSample::kSoftirq;
}

void FlightRecorder::readIrqs(int64_t now)
{
    if (!irqFile_.read(buf_) || !parseInterrupts(buf_, interrupts_))
        return;
    // Share the (label, name) table between snapshots until the IRQ set changes.
    bool same = irqLines_ && irqLabels_.size() == interrupts_.lines.size();
    for (size_t i = 0; same && i < irqLabels_.size(); ++i)
        same = irqLabels_[i] == interrupts_.lines[i].label;
    if (!same) {
        auto lines = std::make_shared<std::vector<std::pair<QString, QString>>>();
        irqLabels_.clear();
        for (const IrqLine& l : interrupts_.lines) {
            lines->emplace_back(QString::fromStdString(l.label), QString::fromStdString(l.name));
            irqLabels_.push_back(l.label);
        }
        irqLines_ = lines;
    }
    IrqSnapshot snap;
    if (irqRing_.size() >= 30) {   // ≥ 5 s at 2.5 Hz plus hitch snapshots
        snap = std::move(irqRing_.front());   // reuse the oldest buffer
        irqRing_.pop_front();
    }
    snap.tNs = now;
    snap.lines = irqLines_;
    snap.ncpu = ncpu_;
    snap.counts.assign(interrupts_.lines.size() * size_t(ncpu_), 0);
    for (size_t i = 0; i < interrupts_.lines.size(); ++i) {
        const auto& per = interrupts_.lines[i].perCpu;
        for (size_t c = 0; c < per.size() && c < size_t(ncpu_); ++c)
            snap.counts[i * size_t(ncpu_) + c] = per[c];
    }
    irqRing_.push_back(std::move(snap));
}

void FlightRecorder::readHot(int64_t now)
{
    std::vector<int> pids;
    {
        std::lock_guard lock(hotMutex_);
        pids = hotPids_;
    }
    // Keep the stat files of hot processes open: open+close costs more than the read.
    for (auto it = hotFiles_.begin(); it != hotFiles_.end();)
        it = std::find(pids.begin(), pids.end(), it->first) == pids.end() ? hotFiles_.erase(it) : std::next(it);
    HotSnapshot snap;
    snap.tNs = now;
    for (int pid : pids) {
        auto [it, inserted] = hotFiles_.try_emplace(pid);
        if (inserted)
            it->second.setPath(SysPaths::proc_(std::to_string(pid) + "/stat"));
        PidStat ps;
        if (!it->second.readSingle(buf_) || !parsePidStat(buf_, ps)) {
            hotFiles_.erase(it);   // exited (a stale fd fails rather than reading a reused pid)
            continue;
        }
        snap.procs.push_back({pid, ps.utime + ps.stime, ps.processor, int(ps.policy)});
    }
    hotRing_.push_back(std::move(snap));
    while (hotRing_.size() > 30)   // 6 s at 5 Hz
        hotRing_.pop_front();
}

bool FlightRecorder::drainProbes(int64_t now)
{
    bool hitch = false;
    LatencyFeed::Point pt;
    pt.tNs = now;
    int64_t worst = 0, worstRq = 0;
    {
        std::lock_guard lock(probeMutex_);
        probes_.drain([&](const ProbeSample& s) {
            if (s.flags & ProbeSample::kSuspended)
                return;
            const int64_t ov = s.overshootNs();
            if (ov > worst) {
                worst = ov;
                worstRq = s.runDelayNs;
            }
            if (ov < thresholdNs_)
                return;
            hitch = true;
            if (pending_.empty() || s.expectedNs > pending_.back().lastNs + kMergeNs ||
                s.expectedNs - pending_.back().firstNs > kMaxHitchNs) {
                pending_.push_back({});
                pending_.back().firstNs = s.expectedNs;
            }
            Pending& p = pending_.back();
            p.hits.push_back(s);
            p.lastNs = std::max(p.lastNs, s.actualNs);
        });
    }
    pt.maxUs = float(double(worst) / 1000.0);
    pt.rqShare = worst > 0 ? float(std::clamp(double(worstRq) / double(worst), 0.0, 1.0)) : 0.f;
    feed_->push(pt);
    return hitch;
}

void FlightRecorder::finalizeReady(int64_t now)
{
    // Hand over hitches once the post-window has been recorded.
    while (!pending_.empty() && now - pending_.front().lastNs >= kPostNs) {
        finalize(pending_.front(), now);
        pending_.erase(pending_.begin());
    }
}

void FlightRecorder::finalize(Pending& p, int64_t now)
{
    feed_->hitches.fetch_add(1);
    while (!recentCaptures_.empty() && now - recentCaptures_.front() > 1'000'000'000LL)
        recentCaptures_.pop_front();
    if (int(recentCaptures_.size()) >= kMaxCapturesPerSec) {
        feed_->suppressed.fetch_add(1);
        return;
    }
    recentCaptures_.push_back(now);

    auto cap = std::make_shared<HitchCapture>();
    cap->hits = std::move(p.hits);
    cap->ncpu = ncpu_;
    cap->probeCount = feed_->probes;
    cap->thresholdMs = double(thresholdNs_) / 1e6;
    cap->mode = mode_;
    const int64_t from = p.firstNs - kPreNs;
    // Oldest to newest.
    for (size_t i = 0; i < filled_; ++i) {
        const FlightSample& s = ring_[(head_ + ring_.size() - filled_ + i) % ring_.size()];
        if (s.tNs >= from)
            cap->window.push_back(s);
    }
    for (const IrqSnapshot& s : irqRing_)   // IRQ snapshots are sparse: a longer baseline
        if (s.tNs >= from - 1'500'000'000)
            cap->irqs.push_back(s);
    for (const HotSnapshot& s : hotRing_)
        if (s.tNs >= from - 400'000'000)
            cap->procs.push_back(s);
    onCapture_(std::move(cap));
}

} // namespace culprit
