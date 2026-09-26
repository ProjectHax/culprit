// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/actions/ProcessActions.h"

#include "common/fs/File.h"
#include "common/parse/PidParsers.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace culprit::ProcessActions {

namespace {

// glibc's <sys/pidfd.h> lacks C++ linkage guards on some versions; use the syscalls.
int pidfdOpen(int pid) { return int(syscall(SYS_pidfd_open, pid, 0)); }
int pidfdSendSignal(int fd, int sig) { return int(syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0)); }

bool matchesKey(const ProcKey& key)
{
    std::string buf;
    PidStat ps;
    return readFile(SysPaths::proc_(std::to_string(key.pid) + "/stat"), buf) && parsePidStat(buf, ps) &&
           ps.starttime == key.starttime;
}

std::vector<int> threadsOf(int pid)
{
    std::vector<int> tids;
    const std::string dir = SysPaths::proc_(std::to_string(pid) + "/task");
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
                tids.push_back(atoi(e->d_name));
        }
        closedir(d);
    }
    return tids;
}

ActionResult goneResult()
{
    ActionResult r;
    r.gone = true;
    r.message = QStringLiteral("The process has exited (or its pid was reused).");
    return r;
}

ActionResult errnoResult(int err, const QString& what)
{
    ActionResult r;
    r.permissionDenied = (err == EPERM || err == EACCES);
    r.gone = (err == ESRCH);
    r.message = QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(strerror(err)));
    return r;
}

// Applies fn(tid) to every thread; stops at the first permission error.
template <typename F>
ActionResult forAllThreads(const ProcKey& key, const QString& what, F&& fn)
{
    if (!matchesKey(key))
        return goneResult();
    const auto tids = threadsOf(key.pid);
    int done = 0;
    for (int tid : tids) {
        if (fn(tid) != 0) {
            const int err = errno;
            if (err == ESRCH)
                continue;   // thread exited meanwhile
            return errnoResult(err, what);
        }
        ++done;
    }
    ActionResult r;
    r.ok = true;
    r.message = QStringLiteral("%1 applied to %2 thread(s).").arg(what).arg(done);
    return r;
}

} // namespace

ActionResult sendSignal(const ProcKey& key, int sig)
{
    // A pidfd pins the process identity: verify the start time *after* opening it
    // and the signal can't hit a recycled pid.
    const int fd = pidfdOpen(key.pid);
    if (fd < 0)
        return errno == ESRCH ? goneResult() : errnoResult(errno, QStringLiteral("pidfd_open"));
    if (!matchesKey(key)) {
        ::close(fd);
        return goneResult();
    }
    const int rc = pidfdSendSignal(fd, sig);
    const int err = errno;
    ::close(fd);
    if (rc != 0)
        return errnoResult(err, QStringLiteral("Sending %1").arg(QString::fromLatin1(sigabbrev_np(sig) ? sigabbrev_np(sig) : "signal")));
    ActionResult r;
    r.ok = true;
    r.message = QStringLiteral("Sent SIG%1 to %2.").arg(QString::fromLatin1(sigabbrev_np(sig))).arg(key.pid);
    return r;
}

ActionResult renice(const ProcKey& key, int nice)
{
    return forAllThreads(key, QStringLiteral("Nice %1").arg(nice),
                         [nice](int tid) { return setpriority(PRIO_PROCESS, id_t(tid), nice); });
}

ActionResult setIoPriority(const ProcKey& key, int ioClass, int level)
{
    const int value = IOPRIO_PRIO_VALUE(ioClass, ioClass == IOPRIO_CLASS_IDLE ? 0 : level);
    return forAllThreads(key, QStringLiteral("I/O priority"),
                         [value](int tid) { return int(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid, value)); });
}

ActionResult setAffinity(const ProcKey& key, const std::vector<int>& cpus)
{
    if (cpus.empty()) {
        ActionResult r;
        r.message = QStringLiteral("Select at least one CPU.");
        return r;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus)
        CPU_SET(c, &set);
    return forAllThreads(key, QStringLiteral("CPU affinity"),
                         [&set](int tid) { return sched_setaffinity(tid, sizeof set, &set); });
}

std::vector<int> affinity(int pid)
{
    std::vector<int> out;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(pid, sizeof set, &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; ++c)
            if (CPU_ISSET(c, &set))
                out.push_back(c);
    }
    return out;
}

QStringList privilegedCommand(const QString& action, const ProcKey& key, const QStringList& params)
{
    if (!matchesKey(key))
        return {};
    QStringList tids;
    for (int tid : threadsOf(key.pid))
        tids << QString::number(tid);
    const QString pid = QString::number(key.pid);
    if (action == QLatin1String("signal") && params.size() == 1)
        return {QStringLiteral("/usr/bin/kill"), QStringLiteral("-s"), params[0], pid};
    if (action == QLatin1String("renice") && params.size() == 1)
        return QStringList{QStringLiteral("/usr/bin/renice"), QStringLiteral("-n"), params[0], QStringLiteral("-p")} + tids;
    if (action == QLatin1String("ionice") && params.size() == 2)
        return QStringList{QStringLiteral("/usr/bin/ionice"), QStringLiteral("-c"), params[0], QStringLiteral("-n"), params[1],
                           QStringLiteral("-p")} + tids;
    if (action == QLatin1String("affinity") && params.size() == 1)
        return {QStringLiteral("/usr/bin/taskset"), QStringLiteral("-a"), QStringLiteral("-p"), QStringLiteral("-c"), params[0], pid};
    return {};
}

} // namespace culprit::ProcessActions
