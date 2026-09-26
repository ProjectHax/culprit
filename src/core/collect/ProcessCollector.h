// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "core/model/Frame.h"

#include <QString>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culprit {

// Tiered process sampling:
//  * every process: /proc/<pid>/stat (+ metadata once per process instance)
//  * the "hot" set (busiest N, R/D leaders, focus/detail pids): every thread's
//    schedstat + stat, so run-queue wait and per-thread state are exact
//  * D-state: when the kernel reports blocked tasks, a full thread scan (rate-limited)
class ProcessCollector {
public:
    struct Config {
        int hotCount = 40;
        int switchCountTop = 6;           // hot processes that also get ctx-switch counts
        std::unordered_set<int> detailPids;   // UI wants per-thread rows for these
        int focusPid = 0;
    };

    ProcessCollector();

    void sample(Frame& frame, int64_t nowNs, const Config& cfg);

    // Busiest processes from the last sample, for the flight recorder.
    const std::vector<int>& hotPids() const { return hotPids_; }

private:
    // Open file descriptors are kept per process/thread and re-read with pread():
    // open()+close() on procfs costs ~3x more than the read itself.
    struct TaskState {
        CachedFile schedFile, statFile, statusFile;
        uint64_t waitNs = 0, runNs = 0, cpuTicks = 0, nivcsw = 0, nvcsw = 0;
        bool haveSwitches = false;
        int64_t seenNs = 0;
    };
    struct ProcState {
        CachedFile statFile, ioFile;
        std::string commRaw;
        QString comm, cmdline, cgroup, unit, user;
        uint32_t uid = 0;
        bool metaLoaded = false;
        uint64_t cpuTicks = 0, majflt = 0, minflt = 0, blkio = 0;
        uint64_t ioRead = 0, ioWrite = 0;
        bool ioTried = false, ioReadable = false, haveIo = false;
        int64_t seenNs = 0;
        bool havePrev = false;
        std::unordered_map<int, TaskState> tasks;   // hot scans only
        int64_t lastHotNs = 0;
    };

    void loadMeta(int pid, ProcState& st);
    void scanThreads(int pid, ProcState& st, ProcSample& out, double dt, int64_t nowNs, bool withSwitches,
                     bool withDetails, bool withStates, std::vector<DStateThread>& dstate);
    void fullDStateScan(const std::unordered_map<int, const ProcSample*>& byPid, std::vector<DStateThread>& dstate,
                        const std::unordered_set<int>& alreadyScanned);
    QString userName(uint32_t uid);
    void finishDState(std::vector<DStateThread>& dstate, int64_t nowNs, bool fullScan);

    std::unordered_map<ProcKey, ProcState, ProcKeyHash> procs_;
    std::unordered_map<int, ProcKey> pidKeys_;   // pid -> current instance
    std::unordered_map<uint32_t, QString> users_;
    std::unordered_map<int, int64_t> dSince_;   // tid -> first seen in D
    std::vector<int> hotPids_;
    std::unordered_set<int> prevHot_;
    int64_t lastNs_ = 0;
    uint64_t tickCount_ = 0;
    int64_t lastFullDScanNs_ = 0;
    uint32_t myUid_ = 0;
    double hz_ = 100;
    uint64_t pageSize_ = 4096;
    std::string buf_;
};

} // namespace culprit
