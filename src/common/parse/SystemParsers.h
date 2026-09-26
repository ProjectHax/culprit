// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Parsers for system-wide procfs files. All take the file text and fill a
// caller-owned struct, reusing its storage so steady-state sampling does not
// allocate.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace culprit {

// ---- /proc/stat -----------------------------------------------------------
struct CpuTimes {
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0, softirq = 0, steal = 0, guest = 0, guestNice = 0;

    // guest/guest_nice are already included in user/nice.
    uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    uint64_t idleAll() const { return idle + iowait; }
    uint64_t busy() const { return total() - idleAll(); }
};

struct ProcStat {
    CpuTimes all;
    std::vector<CpuTimes> cpus;   // indexed by CPU number
    std::vector<bool> present;    // CPU appeared in this sample (online)
    uint64_t ctxt = 0, intr = 0, processes = 0, btime = 0;
    uint32_t procsRunning = 0, procsBlocked = 0;
};
bool parseProcStat(std::string_view text, ProcStat& out);

// ---- /proc/schedstat (per-CPU run-queue accounting, ns) --------------------
struct CpuSchedstat {
    uint64_t runNs = 0;   // time tasks spent running on this CPU
    uint64_t waitNs = 0;  // time tasks spent runnable, waiting for this CPU
    uint64_t slices = 0;
};
bool parseSchedstat(std::string_view text, std::vector<CpuSchedstat>& out);

// ---- /proc/loadavg ---------------------------------------------------------
struct LoadAvg {
    double load1 = 0, load5 = 0, load15 = 0;
    uint32_t runnable = 0, total = 0, lastPid = 0;
};
bool parseLoadAvg(std::string_view text, LoadAvg& out);

// ---- /proc/meminfo (kB) ----------------------------------------------------
struct MemInfo {
    uint64_t totalKb = 0, freeKb = 0, availableKb = 0, buffersKb = 0, cachedKb = 0;
    uint64_t swapTotalKb = 0, swapFreeKb = 0, swapCachedKb = 0, dirtyKb = 0, writebackKb = 0;
    uint64_t shmemKb = 0, sreclaimableKb = 0, anonKb = 0, mlockedKb = 0;
};
bool parseMeminfo(std::string_view text, MemInfo& out);

// ---- /proc/vmstat (subset, cumulative counters) ----------------------------
struct VmStat {
    uint64_t pgfault = 0, pgmajfault = 0, pswpin = 0, pswpout = 0;
    uint64_t pgscanDirect = 0, pgstealDirect = 0, pgscanKswapd = 0, pgstealKswapd = 0;
    uint64_t allocstall = 0;          // sum of allocstall_* zones
    uint64_t compactStall = 0, compactFail = 0;
    uint64_t oomKill = 0, workingsetRefault = 0;
    uint64_t pgpgin = 0, pgpgout = 0, thpFaultAlloc = 0, thpCollapseAlloc = 0;
    uint64_t nrDirty = 0, nrWriteback = 0;
};
bool parseVmstat(std::string_view text, VmStat& out);

// ---- /proc/softirqs --------------------------------------------------------
enum SoftirqVec { SI_HI, SI_TIMER, SI_NET_TX, SI_NET_RX, SI_BLOCK, SI_IRQ_POLL,
                  SI_TASKLET, SI_SCHED, SI_HRTIMER, SI_RCU, kSoftirqCount };
const char* softirqName(int vec);
struct Softirqs {
    std::array<std::vector<uint64_t>, kSoftirqCount> perCpu;  // [vector][cpu]
};
bool parseSoftirqs(std::string_view text, Softirqs& out);

// ---- /proc/interrupts ------------------------------------------------------
struct IrqLine {
    std::string label;   // "120", "NMI", "LOC", ...
    std::string name;    // device/handler name, e.g. "nvidia", "Local timer interrupts"
    std::vector<uint64_t> perCpu;   // indexed by CPU number
    uint64_t total = 0;
};
struct Interrupts {
    std::vector<IrqLine> lines;
    int maxCpu = 0;      // highest CPU number + 1
};
bool parseInterrupts(std::string_view text, Interrupts& out);

// ---- /proc/diskstats -------------------------------------------------------
struct DiskStat {
    std::string name;
    uint32_t major = 0, minor = 0;
    uint64_t reads = 0, readsMerged = 0, sectorsRead = 0, msReading = 0;
    uint64_t writes = 0, writesMerged = 0, sectorsWritten = 0, msWriting = 0;
    uint64_t inFlight = 0, msIo = 0, msWeighted = 0;
    uint64_t discards = 0, sectorsDiscarded = 0, msDiscarding = 0;
    uint64_t flushes = 0, msFlushing = 0;
};
bool parseDiskstats(std::string_view text, std::vector<DiskStat>& out);

// ---- /proc/net/dev ---------------------------------------------------------
struct NetDevStat {
    std::string name;
    uint64_t rxBytes = 0, rxPackets = 0, rxErrs = 0, rxDrop = 0;
    uint64_t txBytes = 0, txPackets = 0, txErrs = 0, txDrop = 0;
};
bool parseNetDev(std::string_view text, std::vector<NetDevStat>& out);

// ---- /proc/pressure/* (PSI) ------------------------------------------------
struct PressureLine {
    double avg10 = 0, avg60 = 0, avg300 = 0;
    uint64_t totalUs = 0;
};
struct Pressure {
    PressureLine some, full;
    bool hasFull = false;
};
bool parsePressure(std::string_view text, Pressure& out);

// ---- generic "key value" files (cgroup cpu.stat, memory.events, ...) -------
template <typename F>
void forEachKeyValue(std::string_view text, F&& f);

} // namespace culprit

#include "common/parse/Text.h"

namespace culprit {

template <typename F>
void forEachKeyValue(std::string_view text, F&& f)
{
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key, val;
        if (!tok.next(key) || !tok.next(val))
            continue;
        if (!key.empty() && key.back() == ':')
            key.remove_suffix(1);
        uint64_t v = 0;
        if (parseInt(val, v))
            f(key, v);
    }
}

} // namespace culprit
