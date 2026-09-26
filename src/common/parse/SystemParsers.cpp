// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "common/parse/SystemParsers.h"

#include <algorithm>

namespace culprit {

namespace {

bool parseCpuIndex(std::string_view tok, std::string_view prefix, int& cpu)
{
    if (!startsWith(tok, prefix))
        return false;
    return parseInt(tok.substr(prefix.size()), cpu) && cpu >= 0;
}

template <typename Vec>
void ensureSize(Vec& v, size_t n)
{
    if (v.size() < n)
        v.resize(n);
}

} // namespace

bool parseProcStat(std::string_view text, ProcStat& out)
{
    std::fill(out.present.begin(), out.present.end(), false);
    LineReader lines(text);
    std::string_view line;
    bool sawCpu = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        if (!tok.next(key))
            continue;
        if (startsWith(key, "cpu")) {
            CpuTimes* t = nullptr;
            if (key == "cpu") {
                t = &out.all;
                sawCpu = true;
            } else {
                int cpu = 0;
                if (!parseCpuIndex(key, "cpu", cpu))
                    continue;
                ensureSize(out.cpus, size_t(cpu) + 1);
                ensureSize(out.present, size_t(cpu) + 1);
                out.present[size_t(cpu)] = true;
                t = &out.cpus[size_t(cpu)];
            }
            uint64_t* fields[] = {&t->user, &t->nice, &t->system, &t->idle, &t->iowait,
                                  &t->irq, &t->softirq, &t->steal, &t->guest, &t->guestNice};
            for (uint64_t* f : fields) {
                if (!tok.nextInt(*f))
                    *f = 0;
            }
        } else if (key == "ctxt") {
            tok.nextInt(out.ctxt);
        } else if (key == "intr") {
            tok.nextInt(out.intr);   // first number is the total
        } else if (key == "processes") {
            tok.nextInt(out.processes);
        } else if (key == "btime") {
            tok.nextInt(out.btime);
        } else if (key == "procs_running") {
            tok.nextInt(out.procsRunning);
        } else if (key == "procs_blocked") {
            tok.nextInt(out.procsBlocked);
        }
    }
    return sawCpu;
}

bool parseSchedstat(std::string_view text, std::vector<CpuSchedstat>& out)
{
    LineReader lines(text);
    std::string_view line;
    bool any = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        int cpu = 0;
        if (!tok.next(key) || !parseCpuIndex(key, "cpu", cpu))
            continue;
        // cpuN yld_count legacy sched_count sched_goidle ttwu_count ttwu_local
        //      rq_cpu_time run_delay pcount
        uint64_t v[9] = {};
        int n = 0;
        while (n < 9 && tok.nextInt(v[n]))
            ++n;
        if (n < 9)
            continue;
        ensureSize(out, size_t(cpu) + 1);
        out[size_t(cpu)] = CpuSchedstat{v[6], v[7], v[8]};
        any = true;
    }
    return any;
}

bool parseLoadAvg(std::string_view text, LoadAvg& out)
{
    Tokens tok(text);
    std::string_view a, b, c, rt, last;
    if (!tok.next(a) || !tok.next(b) || !tok.next(c) || !tok.next(rt))
        return false;
    parseDouble(a, out.load1);
    parseDouble(b, out.load5);
    parseDouble(c, out.load15);
    auto slash = rt.find('/');
    if (slash != std::string_view::npos) {
        parseInt(rt.substr(0, slash), out.runnable);
        parseInt(rt.substr(slash + 1), out.total);
    }
    if (tok.next(last))
        parseInt(last, out.lastPid);
    return true;
}

bool parseMeminfo(std::string_view text, MemInfo& out)
{
    struct Key {
        std::string_view name;
        uint64_t MemInfo::*field;
    };
    static constexpr Key keys[] = {
        {"MemTotal:", &MemInfo::totalKb},
        {"MemFree:", &MemInfo::freeKb},
        {"MemAvailable:", &MemInfo::availableKb},
        {"Buffers:", &MemInfo::buffersKb},
        {"Cached:", &MemInfo::cachedKb},
        {"SwapCached:", &MemInfo::swapCachedKb},
        {"SwapTotal:", &MemInfo::swapTotalKb},
        {"SwapFree:", &MemInfo::swapFreeKb},
        {"Dirty:", &MemInfo::dirtyKb},
        {"Writeback:", &MemInfo::writebackKb},
        {"Shmem:", &MemInfo::shmemKb},
        {"SReclaimable:", &MemInfo::sreclaimableKb},
        {"AnonPages:", &MemInfo::anonKb},
        {"Mlocked:", &MemInfo::mlockedKb},
    };
    LineReader lines(text);
    std::string_view line;
    bool any = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        if (!tok.next(key))
            continue;
        for (const Key& k : keys) {
            if (key == k.name) {
                tok.nextInt(out.*(k.field));
                any = true;
                break;
            }
        }
    }
    return any;
}

bool parseVmstat(std::string_view text, VmStat& out)
{
    struct Key {
        std::string_view name;
        uint64_t VmStat::*field;
    };
    static constexpr Key keys[] = {
        {"pgfault", &VmStat::pgfault},
        {"pgmajfault", &VmStat::pgmajfault},
        {"pswpin", &VmStat::pswpin},
        {"pswpout", &VmStat::pswpout},
        {"pgscan_direct", &VmStat::pgscanDirect},
        {"pgsteal_direct", &VmStat::pgstealDirect},
        {"pgscan_kswapd", &VmStat::pgscanKswapd},
        {"pgsteal_kswapd", &VmStat::pgstealKswapd},
        {"compact_stall", &VmStat::compactStall},
        {"compact_fail", &VmStat::compactFail},
        {"oom_kill", &VmStat::oomKill},
        {"pgpgin", &VmStat::pgpgin},
        {"pgpgout", &VmStat::pgpgout},
        {"thp_fault_alloc", &VmStat::thpFaultAlloc},
        {"thp_collapse_alloc", &VmStat::thpCollapseAlloc},
        {"nr_dirty", &VmStat::nrDirty},
        {"nr_writeback", &VmStat::nrWriteback},
    };
    uint64_t allocstall = 0, refault = 0;
    LineReader lines(text);
    std::string_view line;
    bool any = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        uint64_t v = 0;
        if (!tok.next(key) || !tok.nextInt(v))
            continue;
        any = true;
        if (startsWith(key, "allocstall_")) {
            allocstall += v;
            continue;
        }
        if (key == "workingset_refault_anon" || key == "workingset_refault_file") {
            refault += v;
            continue;
        }
        for (const Key& k : keys) {
            if (key == k.name) {
                out.*(k.field) = v;
                break;
            }
        }
    }
    out.allocstall = allocstall;
    out.workingsetRefault = refault;
    return any;
}

const char* softirqName(int vec)
{
    static constexpr const char* names[kSoftirqCount] = {
        "HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU"};
    return vec >= 0 && vec < kSoftirqCount ? names[vec] : "?";
}

namespace {

// Parses the "CPU0 CPU1 ..." header shared by /proc/softirqs and /proc/interrupts
// into a column -> CPU number map (columns skip offline CPUs).
bool parseCpuHeader(std::string_view line, std::vector<int>& colCpu)
{
    colCpu.clear();
    Tokens tok(line);
    std::string_view t;
    while (tok.next(t)) {
        int cpu = 0;
        if (parseCpuIndex(t, "CPU", cpu))
            colCpu.push_back(cpu);
    }
    return !colCpu.empty();
}

int maxCpuOf(const std::vector<int>& colCpu)
{
    int m = 0;
    for (int c : colCpu)
        m = std::max(m, c + 1);
    return m;
}

} // namespace

bool parseSoftirqs(std::string_view text, Softirqs& out)
{
    thread_local std::vector<int> colCpu;
    LineReader lines(text);
    std::string_view line;
    if (!lines.next(line) || !parseCpuHeader(line, colCpu))
        return false;
    const size_t ncpu = size_t(maxCpuOf(colCpu));
    for (auto& v : out.perCpu) {
        v.assign(ncpu, 0);
    }
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        if (!tok.next(key) || key.empty() || key.back() != ':')
            continue;
        key.remove_suffix(1);
        int vec = -1;
        for (int i = 0; i < kSoftirqCount; ++i) {
            if (key == softirqName(i)) {
                vec = i;
                break;
            }
        }
        if (vec < 0)
            continue;
        auto& dst = out.perCpu[size_t(vec)];
        for (int cpu : colCpu) {
            uint64_t v = 0;
            if (!tok.nextInt(v))
                break;
            dst[size_t(cpu)] = v;
        }
    }
    return true;
}

bool parseInterrupts(std::string_view text, Interrupts& out)
{
    thread_local std::vector<int> colCpu;
    LineReader lines(text);
    std::string_view line;
    if (!lines.next(line) || !parseCpuHeader(line, colCpu))
        return false;
    out.maxCpu = maxCpuOf(colCpu);
    size_t n = 0;
    while (lines.next(line)) {
        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        std::string_view label = trim(line.substr(0, colon));
        if (label.empty())
            continue;
        if (n == out.lines.size())
            out.lines.emplace_back();
        IrqLine& irq = out.lines[n++];
        irq.label.assign(label);
        irq.perCpu.assign(size_t(out.maxCpu), 0);
        irq.total = 0;

        // Counts are right-aligned columns; stop at the first non-numeric token.
        std::string_view rest = line.substr(colon + 1);
        size_t col = 0;
        while (col < colCpu.size()) {
            std::string_view r = rest;
            while (!r.empty() && isSpace(r.front()))
                r.remove_prefix(1);
            size_t i = 0;
            while (i < r.size() && r[i] >= '0' && r[i] <= '9')
                ++i;
            if (i == 0 || (i < r.size() && !isSpace(r[i])))
                break;
            uint64_t v = 0;
            parseInt(r.substr(0, i), v);
            irq.perCpu[size_t(colCpu[col])] = v;
            irq.total += v;
            rest = r.substr(i);
            ++col;
        }
        std::string_view desc = trim(rest);

        // Numeric IRQs: "IR-PCI-MSIX-0000:0b:00.0  0-edge  nvidia" -> name after the
        // trigger-type token. Named rows ("NMI") just have a description.
        std::string_view name = desc;
        Tokens dt(desc);
        std::string_view t;
        while (dt.next(t)) {
            if (endsWith(t, "-edge") || endsWith(t, "-level") || endsWith(t, "-fasteoi")) {
                std::string_view after = dt.rest();
                if (!after.empty())
                    name = after;
                break;
            }
        }
        irq.name.assign(name);
    }
    out.lines.resize(n);
    return n > 0;
}

bool parseDiskstats(std::string_view text, std::vector<DiskStat>& out)
{
    size_t n = 0;
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Tokens tok(line);
        DiskStat d;
        std::string_view name;
        if (!tok.nextInt(d.major) || !tok.nextInt(d.minor) || !tok.next(name))
            continue;
        uint64_t* fields[] = {&d.reads, &d.readsMerged, &d.sectorsRead, &d.msReading,
                              &d.writes, &d.writesMerged, &d.sectorsWritten, &d.msWriting,
                              &d.inFlight, &d.msIo, &d.msWeighted,
                              &d.discards, nullptr, &d.sectorsDiscarded, &d.msDiscarding,
                              &d.flushes, &d.msFlushing};
        int got = 0;
        for (uint64_t* f : fields) {
            uint64_t v = 0;
            if (!tok.nextInt(v))
                break;
            if (f)
                *f = v;
            ++got;
        }
        if (got < 11)
            continue;
        d.name.assign(name);
        if (n == out.size())
            out.push_back(std::move(d));
        else
            out[n] = std::move(d);
        ++n;
    }
    out.resize(n);
    return n > 0;
}

bool parseNetDev(std::string_view text, std::vector<NetDevStat>& out)
{
    size_t n = 0;
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        std::string_view name = trim(line.substr(0, colon));
        if (name.empty() || name.find('|') != std::string_view::npos)
            continue;
        Tokens tok(line.substr(colon + 1));
        uint64_t v[16] = {};
        int got = 0;
        while (got < 16 && tok.nextInt(v[got]))
            ++got;
        if (got < 16)
            continue;
        if (n == out.size())
            out.emplace_back();
        NetDevStat& s = out[n++];
        s.name.assign(name);
        s.rxBytes = v[0];
        s.rxPackets = v[1];
        s.rxErrs = v[2];
        s.rxDrop = v[3];
        s.txBytes = v[8];
        s.txPackets = v[9];
        s.txErrs = v[10];
        s.txDrop = v[11];
    }
    out.resize(n);
    return n > 0;
}

bool parsePressure(std::string_view text, Pressure& out)
{
    out.hasFull = false;
    bool any = false;
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view kind;
        if (!tok.next(kind))
            continue;
        PressureLine* pl = nullptr;
        if (kind == "some") {
            pl = &out.some;
        } else if (kind == "full") {
            pl = &out.full;
            out.hasFull = true;
        } else {
            continue;
        }
        std::string_view kv;
        while (tok.next(kv)) {
            auto eq = kv.find('=');
            if (eq == std::string_view::npos)
                continue;
            auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "avg10")
                parseDouble(v, pl->avg10);
            else if (k == "avg60")
                parseDouble(v, pl->avg60);
            else if (k == "avg300")
                parseDouble(v, pl->avg300);
            else if (k == "total")
                parseInt(v, pl->totalUs);
        }
        any = true;
    }
    return any;
}

} // namespace culprit
