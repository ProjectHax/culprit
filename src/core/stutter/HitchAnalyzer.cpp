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

double HitchAnalyzer::PeriodDetector::add(int64_t t0)
{
    // A burst of hitches (one bad moment, several late wakeups) is one episode;
    // periodicity is judged on episode starts.
    if (!history.empty() && t0 - history.back() < 300'000'000)
        return lastPeriod;
    history.push_back(t0);
    while (history.size() > 32 || (!history.empty() && t0 - history.front() > 600'000'000'000LL))
        history.pop_front();
    lastPeriod = 0;
    if (history.size() < 5)
        return 0;
    std::vector<double> iv;
    for (size_t i = history.size() - 4; i < history.size(); ++i)
        iv.push_back(double(history[i] - history[i - 1]) / 1e9);
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
            return lastPeriod = 0;
        exact += k == 1;
    }
    lastPeriod = exact >= 3 ? med : 0;
    return lastPeriod;
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
    // Floating mode runs only a few probes, so all of them (at least 2) stalling
    // together counts.
    const size_t needCpus = cap.mode == ProbeMode::PerCpu ? std::max<size_t>(4, size_t(cap.probeCount) / 8)
                                                          : std::clamp<size_t>(size_t(cap.probeCount), 2, 3);
    h.global = simultaneous.size() >= needCpus && h.runDelayShare < 0.5;

    const size_t cpu = h.worstCpu >= 0 && h.worstCpu < cap.ncpu ? size_t(h.worstCpu) : 0;
    const size_t ncpu = size_t(cap.ncpu);

    // ---- windows: baseline (before) vs event
    const int64_t baseA = h.t0Ns - 600'000'000, baseB = h.t0Ns - 100'000'000;
    // A full flight sample lands at most one period before the stall and ~60 ms
    // after it (the recorder takes an extra one when a probe reports the hitch).
    const int64_t evA = h.t0Ns - kFlightFullPeriodNs - 5'000'000, evB = h.t1Ns + 65'000'000;
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
    // Hardware IRQ lines (snapshots every 400 ms, plus one right after the hitch)
    if (cap.irqs.size() >= 2) {
        const IrqSnapshot* ba = nullptr;
        const IrqSnapshot* bb = nullptr;
        const IrqSnapshot* ea = nullptr;
        const IrqSnapshot* eb = nullptr;
        // Snapshots come every 400 ms plus one right after the hitch, so the
        // baseline reaches further back than the counter windows above.
        const int64_t irqBaseA = h.t0Ns - 2'100'000'000;
        for (const IrqSnapshot& s : cap.irqs) {
            if (s.tNs >= irqBaseA && !ba)
                ba = &s;
            if (s.tNs <= baseB)
                bb = &s;
            if (s.tNs <= h.t0Ns - 10'000'000)
                ea = &s;
            if (s.tNs >= h.t1Ns && !eb)
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
    // GPU clock reasons from the engine's last (1 Hz) sample: throttling lasts seconds.
    uint64_t gpuReasons = 0;
    if (lf)
        for (const GpuSample& g : lf->gpus)
            if (g.reasonsValid)
                gpuReasons |= g.eventReasons;
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
    h.periodSec = probePeriod_.add(h.t0Ns);

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

namespace {

// A sample label that puts the thread inside the GPU driver, and on one of the NVIDIA driver's locks.
bool driverLabel(const QString& l)
{
    return l.contains(QLatin1String("/dev/nvidia")) || l.contains(QLatin1String("/dev/dri/")) || l.contains(QLatin1String("os_acquire_")) ||
           l.contains(QLatin1String("os_cond_acquire_"));
}

bool lockLabel(const QString& l) { return l.contains(QLatin1String("os_acquire_")) || l.contains(QLatin1String("os_cond_acquire_")); }

} // namespace

Hitch HitchAnalyzer::analyzeStall(const CompositorStall& s, const std::vector<Nvml::BusyInterval>& selfGpu)
{
    Hitch h;
    h.id = nextId_++;
    h.source = HitchSource::Compositor;
    h.t0Ns = s.t0Ns;
    h.t1Ns = s.t1Ns;
    h.wallMs = QDateTime::currentMSecsSinceEpoch() - (monoNs() - h.t0Ns) / 1'000'000;
    const double dur = double(s.t1Ns - s.t0Ns) / 1e6;
    h.maxOvershootMs = dur;
    h.pid = s.pid;
    h.tid = s.tid;
    h.process = s.process;
    h.thread = s.thread;
    h.onCpuMs = std::clamp(s.runMs, 0.0, dur);
    h.runqMs = std::clamp(s.waitMs, 0.0, dur - h.onCpuMs);
    h.blockedMs = std::max(0.0, dur - h.onCpuMs - h.runqMs);
    h.runDelayShare = dur > 0 ? h.runqMs / dur : 0;
    const int samples = std::max(1, s.samples);
    for (size_t i = 0; i < s.blockedIn.size() && i < 5; ++i)
        h.blockedIn << QObject::tr("%1 (%2 of samples)").arg(s.blockedIn[i].first, fmt::percent(100.0 * s.blockedIn[i].second / samples, 0));

    const QString who = QStringLiteral("%1 %2").arg(s.process, s.thread);
    const QString self = fmt::procLabel(s.process, s.pid);
    const QString top = s.blockedIn.empty() ? QString() : s.blockedIn.front().first;
    // Another of its threads that sat in the GPU driver for most of the freeze.
    const ThreadActivity* otherInDriver = nullptr;
    for (const ThreadActivity& a : s.others)
        if (driverLabel(a.blockedIn) && a.blockedSamples * 2 >= std::max(1, a.samples)) {
            otherInDriver = &a;
            break;
        }

    if (s.mainThread)
        h.evidence << QObject::tr("It didn't answer Culprit's Wayland ping for %1: its event loop was stuck.").arg(fmt::ms(dur));
    std::vector<Suspect> suspects;
    QString why;
    if (h.runqMs >= 0.4 * dur) {
        h.cls = HitchClass::RunQueue;
        why = QObject::tr("waited for a CPU");
        h.evidence << QObject::tr("It was runnable but waited %1 for a CPU: other tasks kept the CPUs busy. The latency-probe "
                                  "hitches at the same time name them.")
                          .arg(fmt::ms(h.runqMs));
        suspects.push_back({QStringLiteral("kernel"), QObject::tr("CPU contention"), 0, 0.5,
                            QObject::tr("%1 waiting for a CPU").arg(fmt::ms(h.runqMs))});
    } else if (h.onCpuMs >= 0.6 * dur) {
        if (s.mainThread && s.helperMs >= 0.2 * dur) {
            h.cls = HitchClass::GarbageCollection;
            why = QObject::tr("JavaScript garbage collection");
            h.evidence << QObject::tr("It ran for %1 while its JavaScript helper threads used %2 of CPU time: a garbage-collection pause.")
                              .arg(fmt::ms(h.onCpuMs), fmt::ms(s.helperMs));
            suspects.push_back({QStringLiteral("process"), QObject::tr("%1 JavaScript heap").arg(self), s.pid, 0.8,
                                QObject::tr("extensions and long uptimes make collections longer")});
        } else {
            h.cls = HitchClass::Busy;
            why = QObject::tr("busy running its own code");
            h.evidence << QObject::tr("It ran on a CPU for %1 of the %2 without getting back to its event loop. A profiler (Sysprof) "
                                      "shows which code.")
                              .arg(fmt::ms(h.onCpuMs), fmt::ms(dur));
            suspects.push_back({QStringLiteral("process"), self, s.pid, 0.7, QObject::tr("on a CPU for %1").arg(fmt::ms(h.onCpuMs))});
        }
    } else if (h.blockedMs >= 0.5 * dur && !top.isEmpty()) {
        if (driverLabel(top)) {
            h.cls = HitchClass::GpuDriver;
            why = lockLabel(top) ? QObject::tr("waited for a lock inside the NVIDIA driver") : QObject::tr("blocked inside the GPU driver");
            suspects.push_back({QStringLiteral("gpu"), QObject::tr("GPU driver"), 0, 0.6, top});
        } else if (top.startsWith(QLatin1String("futex")) && otherInDriver) {
            h.cls = HitchClass::GpuDriver;
            why = QObject::tr("waited for its %1, which was blocked inside the GPU driver").arg(otherInDriver->name);
            suspects.push_back({QStringLiteral("gpu"), QObject::tr("GPU driver"), 0, 0.6,
                                QStringLiteral("%1: %2").arg(otherInDriver->name, otherInDriver->blockedIn)});
        } else if (top.startsWith(QLatin1String("futex")) && s.mainThread && s.helperMs >= 0.3 * dur) {
            // The collector's parallel phases: the main thread waits while the helpers mark and sweep.
            h.cls = HitchClass::GarbageCollection;
            why = QObject::tr("waited for its JavaScript helper threads: garbage collection");
            h.evidence << QObject::tr("It slept on a lock while its JavaScript helper threads used %1 of CPU time: the garbage "
                                      "collector's parallel work.")
                              .arg(fmt::ms(s.helperMs));
            suspects.push_back({QStringLiteral("process"), QObject::tr("%1 JavaScript heap").arg(self), s.pid, 0.8,
                                QObject::tr("extensions and long uptimes make collections longer")});
        } else if (top.startsWith(QLatin1String("futex"))) {
            h.cls = HitchClass::ThreadWait;
            why = QObject::tr("waited for another of its threads");
        } else if (s.compactStalls > 0 || s.allocStalls > 0) {
            h.cls = HitchClass::Reclaim;
            why = QObject::tr("stalled in memory reclaim / compaction");
        } else if (top.startsWith(QLatin1String("kernel, outside a syscall"))) {
            h.cls = HitchClass::Io;
            why = QObject::tr("blocked in the kernel, probably on a page fault");
        } else {
            h.cls = HitchClass::Unknown;
            why = QObject::tr("blocked in %1").arg(top);
        }
    } else {
        h.cls = HitchClass::Unknown;
        why = QObject::tr("partly running, partly blocked");
    }

    // Other processes' threads in the GPU driver: one that isn't queued on the
    // lock, and especially one burning kernel time, is probably holding it.
    QStringList waiting;
    std::map<int, Suspect> holders;
    const int me = int(getpid());
    for (const DriverThread& d : s.driver) {
        const QString label = d.pid == me ? QObject::tr("Culprit itself") : fmt::procLabel(d.process, d.pid);
        if (d.waitingForLock && d.kernelMs < 0) {
            if (!waiting.contains(label))
                waiting << label;
            continue;
        }
        QStringList ev;
        if (d.kernelMs >= 0)
            ev << QObject::tr("thread %1 ran %2 in the kernel during the freeze").arg(d.thread, fmt::ms(d.kernelMs));
        if (!d.call.isEmpty())
            ev << QObject::tr("thread %1 was in %2%3").arg(d.thread, d.call, d.wchan.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(d.wchan));
        const double score = d.kernelMs >= 0 ? std::min(0.9, 0.3 + d.kernelMs / std::max(1.0, dur)) : 0.45;
        Suspect& su = holders[d.pid];
        if (score > su.score)
            su = {d.pid == me ? QStringLiteral("self") : QStringLiteral("process"), label, d.pid, score, ev.join(QStringLiteral("; "))};
    }
    for (auto& [pid, su] : holders) {
        if (h.cls == HitchClass::GpuDriver || h.cls == HitchClass::ThreadWait)
            suspects.push_back(su);
        else
            h.evidence << QObject::tr("%1 was in the GPU driver meanwhile: %2").arg(su.label, su.evidence);
    }
    if (!waiting.isEmpty())
        h.evidence << QObject::tr("Also waiting for the NVIDIA driver's lock: %1").arg(waiting.join(QStringLiteral(", ")));

    // Our own NVML queries hold the same driver lock; one long enough to matter is worth naming.
    for (const Nvml::BusyInterval& b : selfGpu) {
        if (b.endNs < s.t0Ns || b.startNs > s.t1Ns || double(b.endNs - b.startNs) / 1e6 < 0.25 * dur)
            continue;
        const QString took = fmt::ms(double(b.endNs - b.startNs) / 1e6);
        const bool couldBlock = h.cls == HitchClass::GpuDriver || h.cls == HitchClass::ThreadWait || h.cls == HitchClass::Unknown;
        if (couldBlock && b.startNs <= s.t0Ns + 2'000'000) {
            h.selfActivity = true;
            h.evidence << QObject::tr("Culprit's own NVIDIA %1 was in the driver when the freeze began and took %2: possibly self-inflicted.")
                              .arg(QLatin1String(b.what), took);
            suspects.push_back({QStringLiteral("self"), QObject::tr("Culprit's NVIDIA %1").arg(QLatin1String(b.what)), me, 0.9,
                                QObject::tr("%1 in the driver").arg(took)});
        } else {
            h.evidence << QObject::tr("Culprit's own NVIDIA %1 also spent %2 in the driver during the freeze.").arg(QLatin1String(b.what), took);
        }
        break;
    }

    for (const ThreadActivity& a : s.others) {
        if (!a.blockedIn.isEmpty() && a.blockedSamples * 2 >= std::max(1, a.samples))
            h.evidence << QObject::tr("Meanwhile its %1 was blocked in %2.").arg(a.name, a.blockedIn);
        else if (a.runMs >= 0.3 * dur)
            h.evidence << QObject::tr("Meanwhile its %1 ran for %2.").arg(a.name, fmt::ms(a.runMs));
    }
    if (h.cls != HitchClass::GarbageCollection && s.helperMs >= 0.2 * dur)
        h.evidence << QObject::tr("Its JavaScript helper threads used %1 of CPU time meanwhile.").arg(fmt::ms(s.helperMs));
    if (s.compactStalls > 0 || s.allocStalls > 0)
        h.evidence << QObject::tr("Direct reclaim/compaction happened during the freeze (system-wide): %1 allocation stalls, %2 compaction stalls")
                          .arg(s.allocStalls)
                          .arg(s.compactStalls);
    if (s.majorFaults >= 20)
        h.evidence << QObject::tr("%1 major page faults system-wide during the freeze").arg(s.majorFaults);
    if (s.selfLateMs >= std::max(20.0, 0.3 * dur)) {
        h.global = true;
        h.evidence << QObject::tr("Culprit's sampling thread was itself delayed %1: the whole system hitched, not just the compositor.")
                          .arg(fmt::ms(s.selfLateMs));
        if (h.cls == HitchClass::Unknown) {
            h.cls = HitchClass::Global;
            why = QObject::tr("the whole system stalled");
        }
    }

    std::sort(suspects.begin(), suspects.end(), [](const Suspect& a, const Suspect& b) { return a.score > b.score; });
    if (suspects.size() > 6)
        suspects.resize(6);
    h.suspects = std::move(suspects);
    h.periodSec = stallPeriod_.add(h.t0Ns);
    h.summary = QObject::tr("%1 froze for %2: %3").arg(who, fmt::ms(dur), why);
    if (h.selfActivity)
        h.summary += QObject::tr(" — during Culprit's own GPU query");
    return h;
}

} // namespace culprit
