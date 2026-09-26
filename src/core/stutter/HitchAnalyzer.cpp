// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/stutter/HitchAnalyzer.h"

#include "common/fs/File.h"
#include "common/util/Clock.h"
#include "core/Format.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unistd.h>

namespace culprit {

namespace {

// First and last sample in [a, b] that have `bit` set.
struct Span {
    const FlightSample* first = nullptr;
    const FlightSample* last = nullptr;
    double seconds() const { return first && last ? double(last->tNs - first->tNs) / 1e9 : 0; }
    bool ok() const { return first && last && last != first && last->tNs > first->tNs; }
};

Span spanOf(const std::vector<FlightSample>& w, int64_t a, int64_t b, uint32_t bit)
{
    Span s;
    for (const FlightSample& f : w) {
        if (f.tNs < a || f.tNs > b || !(f.valid & bit))
            continue;
        if (!s.first)
            s.first = &f;
        s.last = &f;
    }
    return s;
}

double rateOf(const Span& s, const std::vector<uint64_t> FlightSample::*field, size_t idx)
{
    if (!s.ok())
        return 0;
    const auto& a = (s.first->*field);
    const auto& b = (s.last->*field);
    if (idx >= a.size() || idx >= b.size() || b[idx] < a[idx])
        return 0;
    return double(b[idx] - a[idx]) / s.seconds();
}

// Share of a CPU's time spent in `num` ticks relative to total ticks.
double tickShare(const Span& s, const std::vector<uint64_t> FlightSample::*num, size_t cpu)
{
    if (!s.ok() || cpu >= s.first->totalTicks.size())
        return 0;
    const double total = double(s.last->totalTicks[cpu]) - double(s.first->totalTicks[cpu]);
    const double part = double((s.last->*num)[cpu]) - double((s.first->*num)[cpu]);
    return total > 0 ? std::clamp(part / total, 0.0, 1.0) : 0;
}

uint64_t vmDelta(const Span& s, uint64_t VmStat::*field)
{
    if (!s.ok())
        return 0;
    const uint64_t a = s.first->vm.*field, b = s.last->vm.*field;
    return b >= a ? b - a : 0;
}

QString commOf(int pid, const Frame* f)
{
    if (f)
        if (const ProcSample* p = f->findProc(pid))
            return p->comm;
    std::string c;
    if (readFirstLine(SysPaths::proc_(std::to_string(pid) + "/comm"), c))
        return QString::fromStdString(c);
    return QStringLiteral("pid %1").arg(pid);
}

} // namespace

double HitchAnalyzer::detectPeriod(int64_t t0)
{
    // A burst of hitches (one bad moment, several late wakeups) is one episode;
    // periodicity is judged on episode starts.
    if (!history_.empty() && t0 - history_.back() < 300'000'000)
        return lastPeriod_;
    history_.push_back(t0);
    while (history_.size() > 32 || (!history_.empty() && t0 - history_.front() > 600'000'000'000LL))
        history_.pop_front();
    lastPeriod_ = 0;
    if (history_.size() < 5)
        return 0;
    std::vector<double> iv;
    for (size_t i = history_.size() - 4; i < history_.size(); ++i)
        iv.push_back(double(history_[i] - history_[i - 1]) / 1e9);
    std::vector<double> sorted = iv;
    std::sort(sorted.begin(), sorted.end());
    const double med = (sorted[1] + sorted[2]) / 2;
    if (med < 0.25)
        return 0;
    // Each interval must be ~1x (or 2x/3x: a missed occurrence) of the period.
    int exact = 0;
    for (double v : iv) {
        const double k = std::round(v / med);
        if (k < 1 || k > 3 || std::fabs(v - k * med) > 0.05 * med + 0.02)
            return lastPeriod_ = 0;
        exact += k == 1;
    }
    lastPeriod_ = exact >= 3 ? med : 0;
    return lastPeriod_;
}

Hitch HitchAnalyzer::analyze(const HitchCapture& cap, const Frame* lf, const std::vector<HwmonCollector::BusyInterval>& selfBusy,
                             double tjmaxC)
{
    Hitch h;
    h.id = nextId_++;
    if (cap.hits.empty())
        return h;

    // ---- the stall itself
    const ProbeSample* worst = &cap.hits.front();
    std::set<int> cpus;
    h.t0Ns = cap.hits.front().expectedNs;
    h.t1Ns = cap.hits.front().actualNs;
    for (const ProbeSample& s : cap.hits) {
        if (s.overshootNs() > worst->overshootNs())
            worst = &s;
        cpus.insert(s.cpu);
        h.t0Ns = std::min(h.t0Ns, s.expectedNs);
        h.t1Ns = std::max(h.t1Ns, s.actualNs);
    }
    h.maxOvershootMs = double(worst->overshootNs()) / 1e6;
    h.runDelayShare = worst->overshootNs() > 0 ? std::clamp(double(worst->runDelayNs) / double(worst->overshootNs()), 0.0, 1.0) : 0;
    h.worstCpu = worst->cpu;
    h.cpus.assign(cpus.begin(), cpus.end());
    h.probesHit = int(cap.hits.size());
    h.wallMs = QDateTime::currentMSecsSinceEpoch() - (monoNs() - h.t0Ns) / 1'000'000;

    // Several CPUs stalling within 2 ms of each other = system-wide event.
    std::set<int> simultaneous;
    for (const ProbeSample& s : cap.hits)
        if (std::llabs(s.expectedNs - worst->expectedNs) < 2'000'000 && s.overshootNs() >= worst->overshootNs() / 2)
            simultaneous.insert(s.cpu);
    // A real system-wide stall (SMI, stop_machine, IPI storm) delays the timer
    // wakeups themselves; probes that merely waited runnable on busy CPUs at the
    // same moment are coincidental contention.
    const size_t needCpus = cap.mode == ProbeMode::PerCpu ? std::max<size_t>(4, size_t(cap.probeCount) / 8) : 3;
    h.global = simultaneous.size() >= needCpus && h.runDelayShare < 0.5;

    const size_t cpu = h.worstCpu >= 0 && h.worstCpu < cap.ncpu ? size_t(h.worstCpu) : 0;
    const size_t ncpu = size_t(cap.ncpu);

    // ---- windows: baseline (before) vs event
    const int64_t baseA = h.t0Ns - 600'000'000, baseB = h.t0Ns - 100'000'000;
    const int64_t evA = h.t0Ns - 45'000'000, evB = h.t1Ns + 45'000'000;
    const Span bSched = spanOf(cap.window, baseA, baseB, FlightSample::kSched);
    const Span eSched = spanOf(cap.window, evA, evB, FlightSample::kSched);
    const Span bStat = spanOf(cap.window, baseA, baseB, FlightSample::kStat);
    const Span eStat = spanOf(cap.window, evA - 20'000'000, evB + 20'000'000, FlightSample::kStat);
    const Span bSoft = spanOf(cap.window, baseA, baseB, FlightSample::kSoftirq);
    const Span eSoft = spanOf(cap.window, evA - 20'000'000, evB + 20'000'000, FlightSample::kSoftirq);
    const Span eVm = spanOf(cap.window, evA - 40'000'000, evB + 40'000'000, FlightSample::kVm);

    double score[int(HitchClass::Unknown) + 1] = {};
    std::vector<Suspect> suspects;

    // CPU contention on the stalled CPU
    const double waitBase = rateOf(bSched, &FlightSample::waitNs, cpu) / 1e9;
    const double waitEv = rateOf(eSched, &FlightSample::waitNs, cpu) / 1e9;
    if (h.runDelayShare >= 0.5) {
        score[int(HitchClass::RunQueue)] += 1.0 + h.runDelayShare;
        h.evidence << QObject::tr("The probe was runnable for %1 of its %2 delay: another task held CPU %3.")
                          .arg(fmt::ms(double(worst->runDelayNs) / 1e6), fmt::ms(h.maxOvershootMs))
                          .arg(h.worstCpu);
    } else {
        score[int(HitchClass::IrqKernel)] += 1.0 + (1.0 - h.runDelayShare);
        h.evidence << QObject::tr("Only %1 of the %2 delay was spent waiting on a run queue — the wakeup itself was late "
                                  "(interrupt/softirq work, a non-preemptible kernel section or firmware).")
                          .arg(fmt::ms(double(worst->runDelayNs) / 1e6), fmt::ms(h.maxOvershootMs));
    }
    if (eSched.ok())
        h.evidence << QObject::tr("CPU %1 run-queue wait: %2 of wall time during the stall vs %3 before")
                          .arg(h.worstCpu)
                          .arg(fmt::percent(waitEv * 100, 0), fmt::percent(waitBase * 100, 0));

    // Who ran around the stall (5 Hz CPU-time samples of the busiest processes)
    if (!cap.procs.empty()) {
        const HotSnapshot* a = nullptr;
        const HotSnapshot* b = nullptr;
        for (const HotSnapshot& s : cap.procs) {
            if (s.tNs <= h.t0Ns - 50'000'000)
                a = &s;
            if (s.tNs >= h.t1Ns && !b)
                b = &s;
        }
        if (!b)
            b = &cap.procs.back();
        if (a && b && b->tNs > a->tNs) {
            const double dt = double(b->tNs - a->tNs) / 1e9;
            const double hz = double(sysconf(_SC_CLK_TCK));
            std::map<int, HotProc> before;
            for (const HotProc& p : a->procs)
                before[p.pid] = p;
            struct Cand {
                int pid;
                double share;
                int cpu;
                int policy;
            };
            std::vector<Cand> cands;
            const int self = getpid();
            for (const HotProc& p : b->procs) {
                auto it = before.find(p.pid);
                if (it == before.end() || p.ticks < it->second.ticks)
                    continue;
                if (p.pid == self) {
                    // Our own sampling threads sharing a CPU with a probe is measurement noise.
                    if (p.cpu == h.worstCpu)
                        h.evidence << QObject::tr("Culprit's own sampling thread last ran on CPU %1 (self-interference is possible).").arg(p.cpu);
                    continue;
                }
                const double share = double(p.ticks - it->second.ticks) / hz / dt;   // cores
                if (share >= 0.05)
                    cands.push_back({p.pid, share, p.cpu, p.policy});
            }
            // Processes on the stalled CPU are listed individually; identical
            // processes elsewhere (48 x "sh") are folded into one entry.
            std::map<QString, std::pair<int, double>> elsewhere;
            std::map<QString, int> elsewherePid;
            for (const Cand& c : cands) {
                const bool sameCpu = c.cpu == h.worstCpu;
                const bool rt = c.policy == 1 || c.policy == 2;
                const QString name = commOf(c.pid, lf);
                if (!sameCpu && !rt) {
                    auto& e = elsewhere[name];
                    e.first++;
                    e.second += c.share;
                    elsewherePid[name] = c.pid;
                    continue;
                }
                double s = std::min(1.0, c.share) * (sameCpu ? 1.0 : 0.35) * (rt ? 1.5 : 1.0);
                if (h.runDelayShare < 0.5)
                    s *= 0.4;   // contention isn't the main story
                suspects.push_back({QStringLiteral("process"), fmt::procLabel(name, c.pid), c.pid, std::min(1.0, s),
                                    QObject::tr("%1 of a core%2%3")
                                        .arg(fmt::percent(c.share * 100, 0))
                                        .arg(sameCpu ? QObject::tr(", on CPU %1").arg(c.cpu) : QString())
                                        .arg(rt ? QObject::tr(", real-time priority") : QString())});
            }
            for (const auto& [name, e] : elsewhere) {
                // Load elsewhere only explains a stall indirectly: keep it below any on-CPU suspect.
                double s = std::min(0.3, 0.3 * e.second / std::max(1.0, double(cap.ncpu) / 4)) * (h.runDelayShare < 0.5 ? 0.4 : 1.0);
                suspects.push_back({QStringLiteral("process"),
                                    e.first == 1 ? fmt::procLabel(name, elsewherePid[name]) : QStringLiteral("%1 ×%2").arg(name).arg(e.first),
                                    elsewherePid[name], std::max(0.05, s),
                                    QObject::tr("%1 of a core on other CPUs").arg(fmt::percent(e.second * 100, 0))});
            }
        }
    }

    // Interrupt / softirq activity on the stalled CPU
    const double irqShareEv = tickShare(eStat, &FlightSample::irqTicks, cpu) + tickShare(eStat, &FlightSample::softirqTicks, cpu);
    const double irqShareBase = tickShare(bStat, &FlightSample::irqTicks, cpu) + tickShare(bStat, &FlightSample::softirqTicks, cpu);
    if (irqShareEv >= 0.15 && irqShareEv > irqShareBase * 2) {
        score[int(HitchClass::IrqKernel)] += 1.0;
        h.evidence << QObject::tr("CPU %1 spent %2 of its time in IRQ/softirq handlers around the stall (vs %3 before)")
                          .arg(h.worstCpu)
                          .arg(fmt::percent(irqShareEv * 100, 0), fmt::percent(irqShareBase * 100, 0));
    }
    for (int v = 0; v < kSoftirqCount; ++v) {
        const size_t idx = size_t(v) * ncpu + cpu;
        const double ev = rateOf(eSoft, &FlightSample::softirq, idx);
        const double base = rateOf(bSoft, &FlightSample::softirq, idx);
        if (ev >= 2000 && ev >= base * 3 + 500) {
            const double s = std::min(1.0, ev / 20000.0) * (h.runDelayShare < 0.5 ? 1.0 : 0.5);
            score[int(HitchClass::IrqKernel)] += s;
            suspects.push_back({QStringLiteral("softirq"), QObject::tr("%1 softirq on CPU %2").arg(QLatin1String(softirqName(v))).arg(cpu), 0, s,
                                QObject::tr("%1 vs %2 before").arg(fmt::rate(ev), fmt::rate(base))});
        }
    }
    // Hardware IRQ lines (10 Hz snapshots)
    if (cap.irqs.size() >= 2) {
        const IrqSnapshot* ba = nullptr;
        const IrqSnapshot* bb = nullptr;
        const IrqSnapshot* ea = nullptr;
        const IrqSnapshot* eb = nullptr;
        for (const IrqSnapshot& s : cap.irqs) {
            if (s.tNs >= baseA && !ba)
                ba = &s;
            if (s.tNs <= baseB)
                bb = &s;
            if (s.tNs <= h.t0Ns - 10'000'000)
                ea = &s;
            if (s.tNs >= h.t1Ns + 10'000'000 && !eb)
                eb = &s;
        }
        if (ea && eb && eb->lines == ea->lines && eb->tNs > ea->tNs) {
            const double edt = double(eb->tNs - ea->tNs) / 1e9;
            const bool haveBase = ba && bb && bb->lines == ba->lines && bb->lines == ea->lines && bb->tNs > ba->tNs;
            const double bdt = haveBase ? double(bb->tNs - ba->tNs) / 1e9 : 0;
            struct L {
                size_t line;
                double ev, base;
            };
            std::vector<L> hot;
            for (size_t i = 0; i < ea->lines->size(); ++i) {
                const size_t k = i * size_t(ea->ncpu) + cpu;
                if (k >= eb->counts.size() || k >= ea->counts.size())
                    continue;
                const double ev = double(eb->counts[k] - std::min(eb->counts[k], ea->counts[k])) / edt;
                const double base = haveBase ? double(bb->counts[k] - std::min(bb->counts[k], ba->counts[k])) / bdt : 0;
                if (ev >= 1000 && ev >= base * 3 + 300)
                    hot.push_back({i, ev, base});
            }
            std::sort(hot.begin(), hot.end(), [](const L& a, const L& b) { return a.ev - a.base > b.ev - b.base; });
            for (size_t i = 0; i < hot.size() && i < 3; ++i) {
                const auto& [label, name] = (*ea->lines)[hot[i].line];
                const double s = std::min(1.0, hot[i].ev / 20000.0) * (h.runDelayShare < 0.5 ? 1.0 : 0.5);
                score[int(HitchClass::IrqKernel)] += s;
                bool numeric = false;
                label.toInt(&numeric);
                suspects.push_back({QStringLiteral("irq"),
                                    numeric ? QObject::tr("IRQ %1 (%2) on CPU %3").arg(label, name).arg(cpu)
                                            : QObject::tr("%1 (%2) on CPU %3").arg(name, label).arg(cpu),
                                    0, s, QObject::tr("%1 vs %2 before").arg(fmt::rate(hot[i].ev), fmt::rate(hot[i].base))});
            }
        }
    }

    // Memory reclaim / compaction during the stall
    const uint64_t scan = vmDelta(eVm, &VmStat::pgscanDirect), stall = vmDelta(eVm, &VmStat::allocstall),
                   compact = vmDelta(eVm, &VmStat::compactStall), majflt = vmDelta(eVm, &VmStat::pgmajfault),
                   swapin = vmDelta(eVm, &VmStat::pswpin);
    if (scan > 0 || stall > 0 || compact > 0) {
        score[int(HitchClass::Reclaim)] += 2.5;
        h.evidence << QObject::tr("Direct reclaim/compaction during the stall: %1 pages scanned, %2 allocation stalls, %3 compaction stalls")
                          .arg(scan)
                          .arg(stall)
                          .arg(compact);
        suspects.push_back({QStringLiteral("kernel"), QObject::tr("memory reclaim / compaction"), 0, 0.8,
                            QObject::tr("allocating tasks had to free memory first")});
    }
    if (majflt >= 50 || swapin > 0) {
        score[int(HitchClass::Io)] += 0.5;
        h.evidence << QObject::tr("%1 major page faults (%2 pages swapped in) around the stall").arg(majflt).arg(swapin);
    }

    // Blocked tasks / thermals / frequency / GPU
    int evBlocked = 0, evRunnable = 0;
    double baseBlocked = 0, baseRunnable = 0;
    int nb = 0;
    float tctl = 0;
    uint32_t freqEv = UINT32_MAX;
    double freqBase = 0;
    int nfb = 0;
    uint64_t gpuReasons = 0;
    for (const FlightSample& f : cap.window) {
        const bool inEv = f.tNs >= evA && f.tNs <= evB;
        const bool inBase = f.tNs >= baseA && f.tNs <= baseB;
        if (f.valid & FlightSample::kStat) {
            if (inEv) {
                evBlocked = std::max<int>(evBlocked, f.procsBlocked);
                evRunnable = std::max<int>(evRunnable, f.procsRunning);
            } else if (inBase) {
                baseBlocked += f.procsBlocked;
                baseRunnable += f.procsRunning;
                ++nb;
            }
        }
        if ((f.valid & FlightSample::kTemp) && f.tNs >= evA - 200'000'000 && f.tNs <= evB + 200'000'000)
            tctl = std::max(tctl, f.tctl);
        if ((f.valid & FlightSample::kFreq) && cpu < f.freqKHz.size() && f.freqKHz[cpu]) {
            if (f.tNs >= evA - 100'000'000 && f.tNs <= evB + 100'000'000)
                freqEv = std::min(freqEv, f.freqKHz[cpu]);
            else if (inBase) {
                freqBase += f.freqKHz[cpu];
                ++nfb;
            }
        }
        if ((f.valid & FlightSample::kGpu) && f.tNs >= evA - 100'000'000 && f.tNs <= evB + 100'000'000)
            gpuReasons |= f.gpuReasons;
    }
    if (nb) {
        baseBlocked /= nb;
        baseRunnable /= nb;
    }
    if (evRunnable > 0 && evRunnable >= baseRunnable + std::max(4.0, baseRunnable))
        h.evidence << QObject::tr("Runnable tasks jumped to %1 (≈%2 before)").arg(evRunnable).arg(baseRunnable, 0, 'f', 1);

    // Short-lived processes never make it into the busy-process samples; a fork
    // burst around the stall is the tell-tale (build jobs, scripts, spawners).
    const Span eFork = spanOf(cap.window, h.t0Ns - 250'000'000, evB, FlightSample::kStat);
    const Span bFork = spanOf(cap.window, baseA, h.t0Ns - 250'000'000, FlightSample::kStat);
    if (eFork.ok()) {
        const double evForks = double(eFork.last->forks - std::min(eFork.last->forks, eFork.first->forks));
        const double baseRate = bFork.ok() ? double(bFork.last->forks - std::min(bFork.last->forks, bFork.first->forks)) / bFork.seconds() : 0;
        const double evRate = evForks / std::max(0.05, eFork.seconds());
        if (evForks >= 8 && evRate >= baseRate * 5 + 20) {
            h.evidence << QObject::tr("%1 new processes were started around the stall (%2/s vs %3/s before)")
                              .arg(evForks, 0, 'f', 0)
                              .arg(evRate, 0, 'f', 0)
                              .arg(baseRate, 0, 'f', 0);
            suspects.push_back({QStringLiteral("process"), QObject::tr("short-lived processes (%1 started)").arg(evForks, 0, 'f', 0), 0,
                                h.runDelayShare >= 0.5 ? 0.5 : 0.2,
                                QObject::tr("too brief to be sampled individually; Deep trace names them")});
        }
    }
    if (evBlocked >= baseBlocked + 2) {
        score[int(HitchClass::Io)] += 0.4;
        h.evidence << QObject::tr("%1 tasks blocked on I/O during the stall (≈%2 before)").arg(evBlocked).arg(baseBlocked, 0, 'f', 1);
    }
    if (tctl > 0 && tjmaxC > 0 && tctl >= tjmaxC - 2) {
        score[int(HitchClass::Thermal)] += 0.8;
        h.evidence << QObject::tr("CPU temperature %1 — at its thermal limit").arg(fmt::celsius(tctl));
    }
    if (nfb && freqEv != UINT32_MAX && freqEv < freqBase / nfb * 0.6) {
        score[int(HitchClass::Thermal)] += 0.6;
        h.evidence << QObject::tr("CPU %1 clock dropped to %2 (≈%3 before)").arg(cpu).arg(fmt::mhz(freqEv / 1000.0), fmt::mhz(freqBase / nfb / 1000.0));
    }
    const uint64_t badGpu = gpuReasons & (GpuReason::SwThermal | GpuReason::HwThermal | GpuReason::HwSlowdown | GpuReason::HwPowerBrake);
    if (badGpu) {
        score[int(HitchClass::Gpu)] += 0.7;
        h.evidence << QObject::tr("GPU was throttling: %1").arg(decodeGpuEventReasons(badGpu).join(QStringLiteral(", ")));
    }

    // Culprit's own slow sensor reads
    for (const auto& b : selfBusy) {
        if (b.endNs >= h.t0Ns - 2'000'000 && b.startNs <= h.t1Ns) {
            h.selfActivity = true;
            h.evidence << QObject::tr("Coincided with Culprit reading the %1 sensor chip (a slow Super-I/O read) — likely self-inflicted.").arg(b.chip);
            suspects.push_back({QStringLiteral("self"), QObject::tr("Culprit's %1 sensor read").arg(b.chip), int(getpid()), 0.9,
                                QObject::tr("%1 read").arg(fmt::ms(double(b.endNs - b.startNs) / 1e6))});
            break;
        }
    }

    if (h.global) {
        score[int(HitchClass::Global)] += 2.0;
        h.evidence << QObject::tr("%1 CPUs stalled at the same moment.").arg(simultaneous.size());
        suspects.push_back({QStringLiteral("kernel"), QObject::tr("system-wide stall (SMI/firmware, stop_machine, IPI/TLB storm)"), 0, 0.6,
                            QObject::tr("%1 CPUs affected").arg(simultaneous.size())});
    }

    // ---- verdict
    int best = int(HitchClass::Unknown);
    for (int c = 0; c < int(HitchClass::Unknown); ++c)
        if (score[c] > (best == int(HitchClass::Unknown) ? 0.0 : score[best]))
            best = c;
    h.cls = HitchClass(best);

    std::sort(suspects.begin(), suspects.end(), [](const Suspect& a, const Suspect& b) { return a.score > b.score; });
    if (suspects.size() > 6)
        suspects.resize(6);
    if (h.cls == HitchClass::IrqKernel && std::none_of(suspects.begin(), suspects.end(), [](const Suspect& s) {
            return s.kind == QLatin1String("irq") || s.kind == QLatin1String("softirq");
        })) {
        suspects.insert(suspects.begin(), {QStringLiteral("kernel"), QObject::tr("non-preemptible kernel work or firmware (SMI)"), 0, 0.5,
                                           QObject::tr("no interrupt/softirq spike was visible; Deep trace can identify it")});
    }
    h.suspects = std::move(suspects);
    h.periodSec = detectPeriod(h.t0Ns);

    // Only name a suspect in the one-line summary when the evidence is strong.
    const QString top = !h.suspects.empty() && h.suspects.front().score >= 0.3 ? h.suspects.front().label : QString();
    const QString where = QObject::tr("CPU %1").arg(h.worstCpu);
    switch (h.cls) {
    case HitchClass::RunQueue:
        h.summary = top.isEmpty() ? QObject::tr("%1 stall on %2: waited for the CPU (no single busy process stood out)").arg(fmt::ms(h.maxOvershootMs), where)
                                  : QObject::tr("%1 stall on %2: waited for the CPU while %3 ran").arg(fmt::ms(h.maxOvershootMs), where, top);
        break;
    case HitchClass::IrqKernel:
        h.summary = top.isEmpty() ? QObject::tr("%1 late wakeup on %2").arg(fmt::ms(h.maxOvershootMs), where)
                                  : QObject::tr("%1 late wakeup on %2: %3").arg(fmt::ms(h.maxOvershootMs), where, top);
        break;
    case HitchClass::Reclaim:
        h.summary = QObject::tr("%1 stall during direct memory reclaim/compaction").arg(fmt::ms(h.maxOvershootMs));
        break;
    case HitchClass::Global:
        h.summary = QObject::tr("%1 stall on %2 CPUs at once").arg(fmt::ms(h.maxOvershootMs)).arg(simultaneous.size());
        break;
    default:
        h.summary = QObject::tr("%1 stall on %2 (%3)").arg(fmt::ms(h.maxOvershootMs), where, hitchClassName(h.cls));
        break;
    }
    if (h.selfActivity)
        h.summary += QObject::tr(" — during Culprit's own sensor read");
    return h;
}

} // namespace culprit
