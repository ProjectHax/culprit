// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Parsers for /proc/<pid>/... and /proc/<pid>/task/<tid>/... files.

#include <cstdint>
#include <string_view>

namespace culprit {

inline constexpr unsigned kPfKthread = 0x00200000;   // PF_KTHREAD in stat flags

struct PidStat {
    int pid = 0;
    std::string_view comm;   // points into the parsed buffer
    char state = '?';
    int ppid = 0, pgrp = 0, session = 0, ttyNr = 0;
    unsigned flags = 0;
    uint64_t minflt = 0, majflt = 0;
    uint64_t utime = 0, stime = 0;       // clock ticks, summed over all threads for a tgid
    int64_t priority = 0, nice = 0, numThreads = 0;
    uint64_t starttime = 0;              // clock ticks since boot
    uint64_t vsize = 0;
    int64_t rssPages = 0;
    int processor = -1;                  // CPU last run on
    unsigned rtPriority = 0, policy = 0;
    uint64_t blkioTicks = 0;             // delayacct_blkio_ticks (needs task_delayacct=1)

    bool isKernelThread() const { return (flags & kPfKthread) != 0; }
};
bool parsePidStat(std::string_view text, PidStat& out);

// /proc/<pid>/schedstat and /proc/<pid>/task/<tid>/schedstat (per thread!)
struct TaskSchedstat {
    uint64_t runNs = 0;    // time on CPU
    uint64_t waitNs = 0;   // time runnable but waiting on a run queue
    uint64_t slices = 0;
};
bool parseTaskSchedstat(std::string_view text, TaskSchedstat& out);

// Selected /proc/<pid>/status fields. Note ctxt switch counts are per thread.
struct PidStatus {
    int tgid = 0;
    uint32_t uid = 0;
    uint64_t vmSwapKb = 0, rssAnonKb = 0, rssFileKb = 0, rssShmemKb = 0;
    uint64_t volCtxsw = 0, nonvolCtxsw = 0;
};
bool parsePidStatus(std::string_view text, PidStatus& out);

// /proc/<pid>/io (own processes only unless root)
struct PidIo {
    uint64_t rchar = 0, wchar = 0, readBytes = 0, writeBytes = 0, cancelledWriteBytes = 0;
};
bool parsePidIo(std::string_view text, PidIo& out);

// Returns the cgroup v2 path from /proc/<pid>/cgroup ("0::/user.slice/...").
std::string_view parseCgroupV2Path(std::string_view text);

// "/user.slice/user-1000.slice/user@1000.service/app.slice/app-foo.scope" -> "app-foo.scope"
std::string_view cgroupUnitName(std::string_view path);

} // namespace culprit
