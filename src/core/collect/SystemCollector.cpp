// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/SystemCollector.h"

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace culprit {

namespace {

template <typename T>
double deltaRate(T now, T prev, double dt)
{
    return (dt > 0 && now >= prev) ? double(now - prev) / dt : 0.0;
}

CpuLoad loadFromDelta(const CpuTimes& a, const CpuTimes& b)
{
    CpuLoad l;
    const double total = double(a.total()) - double(b.total());
    if (total <= 0)
        return l;
    auto pct = [total](uint64_t x, uint64_t y) { return float(x >= y ? double(x - y) * 100.0 / total : 0.0); };
    l.user = pct(a.user + a.nice, b.user + b.nice);
    l.system = pct(a.system, b.system);
    l.irq = pct(a.irq, b.irq);
    l.softirq = pct(a.softirq, b.softirq);
    l.iowait = pct(a.iowait, b.iowait);
    l.steal = pct(a.steal, b.steal);
    l.busy = pct(a.busy(), b.busy());
    return l;
}

} // namespace

SystemCollector::SystemCollector()
    : statFile_(SysPaths::proc_("stat")),
      schedstatFile_(SysPaths::proc_("schedstat")),
      loadFile_(SysPaths::proc_("loadavg")),
      memFile_(SysPaths::proc_("meminfo")),
      vmFile_(SysPaths::proc_("vmstat")),
      softirqFile_(SysPaths::proc_("softirqs")),
      irqFile_(SysPaths::proc_("interrupts")),
      diskFile_(SysPaths::proc_("diskstats")),
      netFile_(SysPaths::proc_("net/dev")),
      uptimeFile_(SysPaths::proc_("uptime")),
      psiCpu_(SysPaths::proc_("pressure/cpu")),
      psiMem_(SysPaths::proc_("pressure/memory")),
      psiIo_(SysPaths::proc_("pressure/io"))
{
}

void SystemCollector::sample(SystemSample& out, int64_t nowNs)
{
    const double dt = havePrev_ ? double(nowNs - lastNs_) / 1e9 : 0.0;
    out.intervalSec = dt;

    if (uptimeFile_.read(buf_)) {
        Tokens t(buf_);
        std::string_view up;
        if (t.next(up))
            parseDouble(up, out.uptimeSec);
    }

    LoadAvg la;
    if (loadFile_.read(buf_) && parseLoadAvg(buf_, la)) {
        out.load1 = la.load1;
        out.load5 = la.load5;
        out.load15 = la.load15;
        out.threadsTotal = la.total;
    }

    sampleCpu(out, dt);
    sampleMemory(out, dt);
    sampleIrqs(out, dt);
    sampleDisks(out, dt);
    sampleNet(out, dt);
    samplePsi(out);

    lastNs_ = nowNs;
    havePrev_ = true;
}

void SystemCollector::sampleCpu(SystemSample& out, double dt)
{
    std::swap(stat_, prevStat_);
    if (!statFile_.read(buf_) || !parseProcStat(buf_, stat_))
        return;

    std::swap(sched_, prevSched_);
    if (schedstatFile_.read(buf_))
        parseSchedstat(buf_, sched_);

    const int ncpu = int(stat_.cpus.size());
    out.ncpu = ncpu;
    out.procsRunning = stat_.procsRunning;
    out.procsBlocked = stat_.procsBlocked;
    out.cpus.resize(size_t(ncpu));
    out.onlineCpus = 0;
    out.runDelayTotalPct = 0;

    for (int c = 0; c < ncpu; ++c) {
        CpuCoreSample& cs = out.cpus[size_t(c)];
        cs.cpu = c;
        cs.online = c < int(stat_.present.size()) && stat_.present[size_t(c)];
        if (cs.online)
            ++out.onlineCpus;
        if (havePrev_ && c < int(prevStat_.cpus.size()))
            cs.load = loadFromDelta(stat_.cpus[size_t(c)], prevStat_.cpus[size_t(c)]);
        if (havePrev_ && dt > 0 && c < int(sched_.size()) && c < int(prevSched_.size())) {
            const auto& a = sched_[size_t(c)];
            const auto& b = prevSched_[size_t(c)];
            cs.runDelayPct = a.waitNs >= b.waitNs ? float(double(a.waitNs - b.waitNs) / (dt * 1e9) * 100.0) : 0.f;
            out.runDelayTotalPct += cs.runDelayPct;
        }
    }
    if (havePrev_) {
        out.total = loadFromDelta(stat_.all, prevStat_.all);
        out.ctxtPs = deltaRate(stat_.ctxt, prevStat_.ctxt, dt);
        out.intrPs = deltaRate(stat_.intr, prevStat_.intr, dt);
        out.forksPs = deltaRate(stat_.processes, prevStat_.processes, dt);
    }
}

void SystemCollector::sampleMemory(SystemSample& out, double dt)
{
    MemInfo mi;
    if (memFile_.read(buf_) && parseMeminfo(buf_, mi)) {
        MemSample& m = out.mem;
        m.totalKb = mi.totalKb;
        m.availableKb = mi.availableKb;
        m.usedKb = mi.totalKb > mi.availableKb ? mi.totalKb - mi.availableKb : 0;
        m.cachedKb = mi.cachedKb + mi.sreclaimableKb;
        m.buffersKb = mi.buffersKb;
        m.swapTotalKb = mi.swapTotalKb;
        m.swapUsedKb = mi.swapTotalKb > mi.swapFreeKb ? mi.swapTotalKb - mi.swapFreeKb : 0;
        m.dirtyKb = mi.dirtyKb;
        m.writebackKb = mi.writebackKb;
        m.shmemKb = mi.shmemKb;
    }

    std::swap(vm_, prevVm_);
    if (!vmFile_.read(buf_) || !parseVmstat(buf_, vm_) || !havePrev_)
        return;
    VmRates& r = out.vm;
    const VmStat &a = vm_, &b = prevVm_;
    r.pgfault = deltaRate(a.pgfault, b.pgfault, dt);
    r.pgmajfault = deltaRate(a.pgmajfault, b.pgmajfault, dt);
    r.pswpin = deltaRate(a.pswpin, b.pswpin, dt);
    r.pswpout = deltaRate(a.pswpout, b.pswpout, dt);
    r.pgscanDirect = deltaRate(a.pgscanDirect, b.pgscanDirect, dt);
    r.pgstealDirect = deltaRate(a.pgstealDirect, b.pgstealDirect, dt);
    r.pgscanKswapd = deltaRate(a.pgscanKswapd, b.pgscanKswapd, dt);
    r.allocstall = deltaRate(a.allocstall, b.allocstall, dt);
    r.compactStall = deltaRate(a.compactStall, b.compactStall, dt);
    r.oomKill = deltaRate(a.oomKill, b.oomKill, dt);
    r.workingsetRefault = deltaRate(a.workingsetRefault, b.workingsetRefault, dt);
    r.pgpginKBs = deltaRate(a.pgpgin, b.pgpgin, dt);
    r.pgpgoutKBs = deltaRate(a.pgpgout, b.pgpgout, dt);
}

void SystemCollector::sampleIrqs(SystemSample& out, double dt)
{
    const size_t ncpu = out.cpus.size();

    // Softirqs
    std::swap(softirqs_, prevSoftirqs_);
    if (softirqFile_.read(buf_) && parseSoftirqs(buf_, softirqs_) && havePrev_ && dt > 0) {
        out.softirqs.clear();
        for (int v = 0; v < kSoftirqCount; ++v) {
            const auto& a = softirqs_.perCpu[size_t(v)];
            const auto& b = prevSoftirqs_.perCpu[size_t(v)];
            SoftirqRate sr;
            sr.vec = v;
            sr.name = QString::fromLatin1(softirqName(v));
            for (size_t c = 0; c < a.size() && c < b.size(); ++c) {
                const double r = deltaRate(a[c], b[c], dt);
                sr.perSec += r;
                if (r > sr.topCpuPerSec) {
                    sr.topCpuPerSec = r;
                    sr.topCpu = int(c);
                }
                if (c < ncpu)
                    out.cpus[c].softirqPs += r;
            }
            out.softirqs.push_back(sr);
        }
    }

    // Hardware interrupts
    if (!irqFile_.read(buf_) || !parseInterrupts(buf_, irqs_))
        return;
    out.irqs.clear();
    for (const IrqLine& line : irqs_.lines) {
        auto it = prevIrqPerCpu_.find(line.label);
        if (it != prevIrqPerCpu_.end() && havePrev_ && dt > 0) {
            IrqRate ir;
            ir.label = QString::fromStdString(line.label);
            ir.name = QString::fromStdString(line.name);
            const auto& prev = it->second;
            for (size_t c = 0; c < line.perCpu.size() && c < prev.size(); ++c) {
                const double r = deltaRate(line.perCpu[c], prev[c], dt);
                ir.perSec += r;
                if (r > ir.topCpuPerSec) {
                    ir.topCpuPerSec = r;
                    ir.topCpu = int(c);
                }
                if (c < ncpu)
                    out.cpus[c].irqPs += r;
            }
            if (ir.perSec > 0)
                out.irqs.push_back(std::move(ir));
            it->second.assign(line.perCpu.begin(), line.perCpu.end());
        } else {
            prevIrqPerCpu_[line.label] = line.perCpu;
        }
    }
    std::sort(out.irqs.begin(), out.irqs.end(),
              [](const IrqRate& a, const IrqRate& b) { return a.perSec > b.perSec; });
    if (out.irqs.size() > 64)
        out.irqs.resize(64);
}

bool SystemCollector::isWholeDisk(const std::string& name)
{
    auto it = wholeDiskCache_.find(name);
    if (it != wholeDiskCache_.end())
        return it->second;
    bool whole = false;
    if (!startsWith(name, "loop") && !startsWith(name, "ram")) {
        struct stat st {};
        whole = ::stat(SysPaths::sys_("block/" + name).c_str(), &st) == 0;
    }
    wholeDiskCache_[name] = whole;
    return whole;
}

QString SystemCollector::diskLabel(const std::string& name)
{
    auto it = diskLabelCache_.find(name);
    if (it != diskLabelCache_.end())
        return it->second;
    QString label = QString::fromStdString(name);
    std::string dmName;
    if (startsWith(name, "dm-") && readFirstLine(SysPaths::sys_("block/" + name + "/dm/name"), dmName) && !dmName.empty())
        label = QString::fromStdString(dmName);
    diskLabelCache_[name] = label;
    return label;
}

void SystemCollector::sampleDisks(SystemSample& out, double dt)
{
    if (!diskFile_.read(buf_) || !parseDiskstats(buf_, disks_))
        return;
    out.disks.clear();
    for (const DiskStat& d : disks_) {
        if (!isWholeDisk(d.name))
            continue;
        auto it = prevDisks_.find(d.name);
        if (it != prevDisks_.end() && havePrev_ && dt > 0) {
            const DiskStat& p = it->second;
            DiskSample s;
            s.name = QString::fromStdString(d.name);
            s.label = diskLabel(d.name);
            s.readsPs = deltaRate(d.reads, p.reads, dt);
            s.writesPs = deltaRate(d.writes, p.writes, dt);
            s.readBps = deltaRate(d.sectorsRead, p.sectorsRead, dt) * 512.0;
            s.writeBps = deltaRate(d.sectorsWritten, p.sectorsWritten, dt) * 512.0;
            s.utilPct = std::min(100.0, deltaRate(d.msIo, p.msIo, dt) / 10.0);
            const double dr = double(d.reads - std::min(d.reads, p.reads));
            const double dw = double(d.writes - std::min(d.writes, p.writes));
            const double mr = double(d.msReading - std::min(d.msReading, p.msReading));
            const double mw = double(d.msWriting - std::min(d.msWriting, p.msWriting));
            s.readAwaitMs = dr > 0 ? mr / dr : 0;
            s.writeAwaitMs = dw > 0 ? mw / dw : 0;
            s.awaitMs = (dr + dw) > 0 ? (mr + mw) / (dr + dw) : 0;
            s.inFlight = d.inFlight;
            out.disks.push_back(std::move(s));
        }
        prevDisks_[d.name] = d;
    }
}

void SystemCollector::sampleNet(SystemSample& out, double dt)
{
    if (!netFile_.read(buf_) || !parseNetDev(buf_, nets_))
        return;
    out.nets.clear();
    for (const NetDevStat& n : nets_) {
        auto it = prevNets_.find(n.name);
        if (it != prevNets_.end() && havePrev_ && dt > 0) {
            const NetDevStat& p = it->second;
            NetSample s;
            s.name = QString::fromStdString(n.name);
            s.rxBps = deltaRate(n.rxBytes, p.rxBytes, dt);
            s.txBps = deltaRate(n.txBytes, p.txBytes, dt);
            s.rxPps = deltaRate(n.rxPackets, p.rxPackets, dt);
            s.txPps = deltaRate(n.txPackets, p.txPackets, dt);
            s.errsPs = deltaRate(n.rxErrs + n.txErrs, p.rxErrs + p.txErrs, dt);
            s.dropsPs = deltaRate(n.rxDrop + n.txDrop, p.rxDrop + p.txDrop, dt);
            out.nets.push_back(std::move(s));
        }
        prevNets_[n.name] = n;
    }
}

void SystemCollector::samplePsi(SystemSample& out)
{
    Pressure p;
    out.psi.available = false;
    if (psiCpu_.read(buf_) && parsePressure(buf_, p)) {
        out.psi.available = true;
        out.psi.cpuSome = {p.some.avg10, p.some.avg60};
    }
    if (psiMem_.read(buf_) && parsePressure(buf_, p)) {
        out.psi.memSome = {p.some.avg10, p.some.avg60};
        out.psi.memFull = {p.full.avg10, p.full.avg60};
    }
    if (psiIo_.read(buf_) && parsePressure(buf_, p)) {
        out.psi.ioSome = {p.some.avg10, p.some.avg60};
        out.psi.ioFull = {p.full.avg10, p.full.avg60};
    }
}

// ------------------------------------------------------------------ cpufreq

void CpufreqCollector::sample(SystemSample& out, int64_t nowNs)
{
    const size_t ncpu = out.cpus.size();
    if (freqFiles_.size() != ncpu) {
        freqFiles_.clear();
        freqFiles_.resize(ncpu);
        for (size_t c = 0; c < ncpu; ++c) {
            const std::string base = SysPaths::sys_("devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/");
            // cpuinfo_avg_freq (APERF/MPERF average) is more truthful than the requested frequency.
            struct stat st {};
            freqFiles_[c].setPath(::stat((base + "cpuinfo_avg_freq").c_str(), &st) == 0 ? base + "cpuinfo_avg_freq"
                                                                                         : base + "scaling_cur_freq");
        }
    }
    for (size_t c = 0; c < ncpu; ++c) {
        int64_t khz = 0;
        out.cpus[c].freqMHz = freqFiles_[c].readInt64(khz) ? float(double(khz) / 1000.0) : 0.f;
    }

    if (lastPolicyNs_ == 0 || nowNs - lastPolicyNs_ > 5'000'000'000LL) {
        readPolicy(policy_);
        lastPolicyNs_ = nowNs;
    }
    out.policy = policy_;
}

void CpufreqCollector::readPolicy(CpuPolicy& p)
{
    const std::string base = SysPaths::sys_("devices/system/cpu/cpu0/cpufreq/");
    std::string s;
    int64_t v = 0;
    p.driver = readFirstLine(base + "scaling_driver", s) ? QString::fromStdString(s) : QString();
    p.governor = readFirstLine(base + "scaling_governor", s) ? QString::fromStdString(s) : QString();
    p.epp = readFirstLine(base + "energy_performance_preference", s) ? QString::fromStdString(s) : QString();
    p.hwMaxMHz = readInt64(base + "cpuinfo_max_freq", v) ? double(v) / 1000.0 : 0;
    p.hwMinMHz = readInt64(base + "cpuinfo_min_freq", v) ? double(v) / 1000.0 : 0;
    p.scalingMaxMHz = readInt64(base + "scaling_max_freq", v) ? double(v) / 1000.0 : 0;
    if (readInt64(base + "boost", v) || readInt64(SysPaths::sys_("devices/system/cpu/cpufreq/boost"), v))
        p.boost = v ? 1 : 0;
    else
        p.boost = -1;
}

} // namespace culprit
