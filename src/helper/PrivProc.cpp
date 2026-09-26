// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "helper/PrivProc.h"

#include "common/fs/File.h"
#include "common/parse/PidParsers.h"
#include "common/parse/SystemParsers.h"
#include "common/util/JsonWriter.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <unordered_set>
#include <vector>

namespace culprit {

namespace {

constexpr const char* kStateFile = "/run/culprit-helper.state";

template <typename F>
void forEachPid(const std::string& dir, F&& f)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* e = readdir(d))
        if (e->d_name[0] >= '1' && e->d_name[0] <= '9')
            f(atoi(e->d_name));
    closedir(d);
}

// "[<0>] folio_wait_bit_common+0x13e/0x340" -> "folio_wait_bit_common"
std::string_view stackFrame(std::string_view line)
{
    if (auto p = line.find("] "); p != std::string_view::npos)
        line.remove_prefix(p + 2);
    if (auto p = line.find('+'); p != std::string_view::npos)
        line = line.substr(0, p);
    return trim(line);
}

bool writeFile(const std::string& path, const std::string& value)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::write(fd, value.data(), value.size()) == ssize_t(value.size());
    ::close(fd);
    return ok;
}

} // namespace

void PrivProc::dstate(int64_t nowNs, std::string& out)
{
    ProcStat st;
    if (readFile("/proc/stat", buf_))
        parseProcStat(buf_, st);
    if (st.procsBlocked == 0) {
        if (lastHadRows_) {
            JsonObj("dstate").raw("rows", "[]").line(out);
            lastHadRows_ = false;
        }
        dSince_.clear();
        return;
    }
    JsonArr rows;
    std::unordered_set<int> seen;
    int n = 0;
    forEachPid("/proc", [&](int pid) {
        const std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
        forEachPid(taskDir, [&](int tid) {
            if (n >= 64)
                return;
            const std::string base = taskDir + "/" + std::to_string(tid);
            PidStat ps;
            if (!readFile(base + "/stat", buf_) || !parsePidStat(buf_, ps) || ps.state != 'D')
                return;
            const std::string comm(ps.comm);
            seen.insert(tid);
            const int64_t since = dSince_.try_emplace(tid, nowNs).first->second;
            std::string wchan;
            readFirstLine(base + "/wchan", wchan);
            JsonArr stack;
            if (readFile(base + "/stack", buf_)) {
                LineReader lines(buf_);
                std::string_view line;
                int frames = 0;
                while (lines.next(line) && frames < 16) {
                    const std::string_view f = stackFrame(line);
                    if (!f.empty()) {
                        stack.addStr(f);
                        ++frames;
                    }
                }
            }
            std::string procComm;
            readFirstLine("/proc/" + std::to_string(pid) + "/comm", procComm);
            rows.add(JsonObj()
                         .num("pid", pid)
                         .num("tid", tid)
                         .str("comm", comm)
                         .str("proc", procComm)
                         .str("wchan", wchan == "0" ? std::string() : wchan)
                         .raw("stack", stack.done())
                         .dbl("age_ms", double(nowNs - since) / 1e6, 0)
                         .done());
            ++n;
        });
    });
    for (auto it = dSince_.begin(); it != dSince_.end();)
        it = seen.count(it->first) ? std::next(it) : dSince_.erase(it);
    JsonObj("dstate").raw("rows", rows.done()).line(out);
    lastHadRows_ = n > 0;
}

void PrivProc::pio(int64_t nowNs, std::string& out)
{
    JsonArr rows;
    std::unordered_set<int> seen;
    forEachPid("/proc", [&](int pid) {
        PidIo io;
        if (!readFile("/proc/" + std::to_string(pid) + "/io", buf_) || !parsePidIo(buf_, io))
            return;
        seen.insert(pid);
        auto [it, inserted] = io_.try_emplace(pid);
        Io& prev = it->second;
        if (!inserted && nowNs > prev.t) {
            const double dt = double(nowNs - prev.t) / 1e9;
            const double r = io.readBytes >= prev.rd ? double(io.readBytes - prev.rd) / dt : 0;
            const double w = io.writeBytes >= prev.wr ? double(io.writeBytes - prev.wr) / dt : 0;
            if (r >= 4096 || w >= 4096) {
                JsonArr row;
                row.addNum(pid).addNum(r).addNum(w);
                rows.add(row.done());
            }
        }
        prev = {io.readBytes, io.writeBytes, nowNs};
    });
    for (auto it = io_.begin(); it != io_.end();)
        it = seen.count(it->first) ? std::next(it) : io_.erase(it);
    JsonObj("pio").raw("rows", rows.done()).line(out);
}

// ------------------------------------------------------------------ sysctl

namespace {
std::string sysctlPath(const std::string& name)
{
    if (name == "delayacct")
        return "/proc/sys/kernel/task_delayacct";
    if (name == "schedstats")
        return "/proc/sys/kernel/sched_schedstats";
    return {};
}
} // namespace

void SysctlGuard::recoverStale()
{
    std::string text;
    if (!readFile(kStateFile, text))
        return;
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        Tokens t(line);
        std::string_view path, orig, applied;
        if (!t.next(path) || !t.next(orig) || !t.next(applied))
            continue;
        const std::string p(path);
        if (p != sysctlPath("delayacct") && p != sysctlPath("schedstats"))
            continue;   // only ever touch our own whitelist
        std::string cur;
        if (readFirstLine(p, cur) && cur == applied)
            writeFile(p, std::string(orig));
    }
    ::unlink(kStateFile);
}

bool SysctlGuard::set(const std::string& name, bool on, std::string& error)
{
    const std::string path = sysctlPath(name);
    if (path.empty()) {
        error = "unknown sysctl";
        return false;
    }
    std::string cur;
    if (!readFirstLine(path, cur)) {
        error = "cannot read " + path;
        return false;
    }
    const std::string want = on ? "1" : "0";
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        if (cur == want)
            return true;   // already as requested: nothing to restore later
        entries_[name] = {path, cur, want};
    } else {
        it->second.applied = want;
    }
    persist();
    if (!writeFile(path, want)) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

void SysctlGuard::persist() const
{
    std::string s;
    for (const auto& [name, e] : entries_)
        s += e.path + " " + e.original + " " + e.applied + "\n";
    const std::string tmp = std::string(kStateFile) + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    if (::write(fd, s.data(), s.size()) != ssize_t(s.size())) {
    }
    ::fsync(fd);
    ::close(fd);
    ::rename(tmp.c_str(), kStateFile);
}

void SysctlGuard::restoreAll()
{
    for (const auto& [name, e] : entries_) {
        std::string cur;
        if (readFirstLine(e.path, cur) && cur == e.applied)
            writeFile(e.path, e.original);
    }
    entries_.clear();
    ::unlink(kStateFile);
}

} // namespace culprit
