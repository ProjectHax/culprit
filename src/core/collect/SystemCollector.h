// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "common/parse/SystemParsers.h"
#include "core/model/Frame.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace culprit {

// System-wide counters from procfs, turned into per-interval rates.
class SystemCollector {
public:
    SystemCollector();

    // Fills everything in `out` except per-CPU frequency (CpufreqCollector) and
    // the flight-recorder averages.
    void sample(SystemSample& out, int64_t nowNs);

    const ProcStat& lastProcStat() const { return stat_; }

private:
    void sampleCpu(SystemSample& out, double dt);
    void sampleMemory(SystemSample& out, double dt);
    void sampleIrqs(SystemSample& out, double dt);
    void sampleDisks(SystemSample& out, double dt);
    void sampleNet(SystemSample& out, double dt);
    void samplePsi(SystemSample& out);
    QString diskLabel(const std::string& name);
    bool isWholeDisk(const std::string& name);

    std::string buf_;
    CachedFile statFile_, schedstatFile_, loadFile_, memFile_, vmFile_, softirqFile_,
        irqFile_, diskFile_, netFile_, uptimeFile_;
    CachedFile psiCpu_, psiMem_, psiIo_;

    int64_t lastNs_ = 0;
    bool havePrev_ = false;

    ProcStat stat_, prevStat_;
    std::vector<CpuSchedstat> sched_, prevSched_;
    VmStat vm_, prevVm_;
    Softirqs softirqs_, prevSoftirqs_;
    Interrupts irqs_;
    std::unordered_map<std::string, std::vector<uint64_t>> prevIrqPerCpu_;
    std::vector<DiskStat> disks_;
    std::unordered_map<std::string, DiskStat> prevDisks_;
    std::unordered_map<std::string, bool> wholeDiskCache_;
    std::unordered_map<std::string, QString> diskLabelCache_;
    std::vector<NetDevStat> nets_;
    std::unordered_map<std::string, NetDevStat> prevNets_;
};

// Per-CPU current frequency and the cpufreq policy (governor / EPP / boost).
class CpufreqCollector {
public:
    void sample(SystemSample& out, int64_t nowNs);

private:
    void readPolicy(CpuPolicy& p);

    std::vector<CachedFile> freqFiles_;
    int64_t lastPolicyNs_ = 0;
    CpuPolicy policy_;
};

} // namespace culprit
