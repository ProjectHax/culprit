// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace culprit {

// /proc data that only root can read for every process: kernel stacks and
// wait channels of blocked (D-state) threads, and per-process storage I/O.
class PrivProc {
public:
    void dstate(int64_t nowNs, std::string& out);
    void pio(int64_t nowNs, std::string& out);

private:
    std::unordered_map<int, int64_t> dSince_;
    bool lastHadRows_ = false;
    struct Io {
        uint64_t rd = 0, wr = 0;
        int64_t t = 0;
    };
    std::unordered_map<int, Io> io_;
    std::string buf_;
};

// Temporarily changes whitelisted sysctls and restores them — also after a
// crash or kill -9, via a state file checked on the next start.
class SysctlGuard {
public:
    static void recoverStale();
    // name: "delayacct" or "schedstats"
    bool set(const std::string& name, bool on, std::string& error);
    void restoreAll();
    ~SysctlGuard() { restoreAll(); }

private:
    struct Entry {
        std::string path, original, applied;
    };
    void persist() const;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace culprit
