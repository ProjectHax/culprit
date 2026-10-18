// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/stutter/CompositorProbe.h"

#include "common/parse/PidParsers.h"
#include "common/parse/SystemParsers.h"
#include "common/parse/Text.h"
#include "common/util/Clock.h"

#include <QObject>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace culprit {

namespace {

constexpr int64_t kIdleTickNs = 40'000'000;      // thread samples at 25 Hz while all is well ...
constexpr int64_t kBusyTickNs = 10'000'000;      // ... and at 100 Hz while a thread may be stuck
constexpr int64_t kPingEveryNs = 35'000'000;     // with every 40 ms tick (jitter allowed), one outstanding at a time
constexpr int64_t kHelperEveryNs = 50'000'000;   // JavaScript helper threads' CPU time
constexpr int64_t kVmEveryNs = 100'000'000;      // /proc/vmstat
constexpr int64_t kTasksEveryNs = 2'000'000'000; // re-list threads
constexpr int64_t kFeedEveryNs = 20'000'000;     // live graph points
constexpr size_t kHistory = 1500;                // 15 s of samples per thread at 100 Hz
constexpr int64_t kNeverIdleNs = 3'000'000'000;  // a thread never seen in its event loop doesn't run one
constexpr uint32_t kCallbackId = 2;              // our only Wayland object besides wl_display, reused
constexpr size_t kMaxDriverThreads = 32;

enum Kind : uint8_t { kRunning, kIdle, kBlocked };

const char* syscallName(int64_t nr)
{
    switch (nr) {
    case 0: return "read";
    case 1: return "write";
    case 3: return "close";
    case 7: return "poll";
    case 9: return "mmap";
    case 10: return "mprotect";
    case 11: return "munmap";
    case 12: return "brk";
    case 17: return "pread64";
    case 18: return "pwrite64";
    case 23: return "select";
    case 24: return "sched_yield";
    case 28: return "madvise";
    case 35: return "nanosleep";
    case 44: return "sendto";
    case 45: return "recvfrom";
    case 46: return "sendmsg";
    case 47: return "recvmsg";
    case 56: return "clone";
    case 61: return "wait4";
    case 219: return "restart_syscall";
    case 230: return "clock_nanosleep";
    case 232: return "epoll_wait";
    case 257: return "openat";
    case 262: return "newfstatat";
    case 270: return "pselect6";
    case 271: return "ppoll";
    case 281: return "epoll_pwait";
    case 435: return "clone3";
    case 441: return "epoll_pwait2";
    default: return nullptr;
    }
}

bool fdSyscall(int64_t nr)
{
    return nr == 0 || nr == 1 || nr == 17 || nr == 18 || nr == 44 || nr == 45 || nr == 46 || nr == 47;
}

const char* drmIoctl(unsigned nr)
{
    switch (nr) {
    case 0x09: return "GEM_CLOSE";
    case 0x2d: return "PRIME_HANDLE_TO_FD";
    case 0x2e: return "PRIME_FD_TO_HANDLE";
    case 0xa2: return "MODE_SETCRTC";
    case 0xa3: return "MODE_CURSOR";
    case 0xaf: return "MODE_RMFB";
    case 0xb0: return "MODE_PAGE_FLIP";
    case 0xb8: return "MODE_ADDFB2";
    case 0xbb: return "MODE_CURSOR2";
    case 0xbc: return "MODE_ATOMIC";
    case 0xc3: return "SYNCOBJ_WAIT";
    case 0xca: return "SYNCOBJ_TIMELINE_WAIT";
    default: return nullptr;
    }
}

const char* nvIoctl(unsigned nr)   // NV_ESC_* (nv_escape.h)
{
    switch (nr) {
    case 0x27: return "RM_ALLOC_MEMORY";
    case 0x29: return "RM_FREE";
    case 0x2a: return "RM_CONTROL";
    case 0x2b: return "RM_ALLOC";
    case 0x34: return "RM_DUP_OBJECT";
    case 0x41: return "RM_IDLE_CHANNELS";
    case 0x4a: return "RM_VID_HEAP_CONTROL";
    case 0x4e: return "RM_MAP_MEMORY";
    case 0x4f: return "RM_UNMAP_MEMORY";
    case 0x57: return "RM_MAP_MEMORY_DMA";
    case 0x58: return "RM_UNMAP_MEMORY_DMA";
    default: return nullptr;
    }
}

// Waits of an event loop: the thread is idle, not stuck.
bool idleWchan(std::string_view w)
{
    return startsWith(w, "poll_schedule_timeout") || w == "ep_poll" || w == "do_epoll_wait" || startsWith(w, "do_select");
}

bool nvidiaLockWchan(std::string_view w) { return startsWith(w, "os_acquire_") || startsWith(w, "os_cond_acquire_"); }

bool gpuDevice(std::string_view path) { return startsWith(path, "/dev/nvidia") || startsWith(path, "/dev/dri/"); }

// A sample label that puts the thread inside the GPU driver.
bool driverLabel(std::string_view l)
{
    return l.find("/dev/nvidia") != std::string_view::npos || l.find("/dev/dri/") != std::string_view::npos ||
           l.find("os_acquire_") != std::string_view::npos || l.find("os_cond_acquire_") != std::string_view::npos;
}

bool parseHex64(std::string_view s, uint64_t& out)
{
    if (startsWith(s, "0x"))
        s.remove_prefix(2);
    return !s.empty() && std::from_chars(s.data(), s.data() + s.size(), out, 16).ec == std::errc();
}

std::string ioctlName(const std::string& dev, uint64_t req)
{
    const unsigned type = unsigned(req >> 8) & 0xff, nr = unsigned(req) & 0xff;
    char hex[8];
    std::snprintf(hex, sizeof hex, "0x%02x", nr);
    std::string s = "ioctl " + dev + " ";
    if (type == 'd') {
        const char* n = drmIoctl(nr);
        return s + "DRM " + (n ? n : nr >= 0x40 && nr < 0xa0 ? std::string("driver ") + hex : std::string(hex));
    }
    if (type == 'F') {
        const char* n = nvIoctl(nr);
        return s + "NV " + (n ? n : hex);
    }
    if (std::isprint(int(type)))
        return s + char(type) + ":" + hex;
    char t[8];
    std::snprintf(t, sizeof t, "0x%02x", type);
    return s + t + ":" + hex;
}

std::string readLink(const std::string& path)
{
    char buf[256];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::string waylandSocketPath()
{
    // CULPRIT_WAYLAND_DISPLAY watches another compositor than the one showing our window.
    const char* d = std::getenv("CULPRIT_WAYLAND_DISPLAY");
    if (!d || !*d)
        d = std::getenv("WAYLAND_DISPLAY");
    const std::string name = d && *d ? d : "wayland-0";
    if (name.front() == '/')
        return name;
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    return (rt && *rt ? std::string(rt) : "/run/user/" + std::to_string(getuid())) + "/" + name;
}

std::string commOf(int pid)
{
    std::string c;
    readFirstLine(SysPaths::proc_(std::to_string(pid) + "/comm"), c);
    return c;
}

// kwin_wayland_wrapper (and the like) owns the socket and runs the compositor as its child.
int childOf(int pid)
{
    DIR* d = opendir(SysPaths::proc_("").c_str());
    if (!d)
        return 0;
    int child = 0;
    std::string buf;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        PidStat ps;
        if (readFile(SysPaths::proc_(std::string(e->d_name) + "/stat"), buf) && parsePidStat(buf, ps) && ps.ppid == pid) {
            child = ps.pid;
            break;
        }
    }
    closedir(d);
    return child;
}

// Threads worth watching for freezes besides the main thread: the ones that
// move the cursor and flip frames, and unnamed ones (Mutter's input thread).
bool watchedName(const std::string& name, const std::string& processComm)
{
    if (name == processComm)
        return true;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* k : {"kms", "input", "render", "cursor", "compos"})
        if (lower.find(k) != std::string::npos)
            return true;
    return false;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

} // namespace

struct CompositorProbe::Thread {
    enum class Role { Main, Watched, Helper };
    struct Sample {
        int64_t tNs;
        uint64_t runNs, waitNs;
        Kind kind;
        uint16_t label;   // labels_ index while blocked
    };
    int tid = 0;
    std::string name;
    Role role = Role::Watched;
    CachedFile schedstat, wchan, syscall;
    std::deque<Sample> hist;
    int64_t bornNs = 0;
    int64_t idleAt = 0;      // last sample in its event loop
    int64_t busyFrom = 0;    // start of the current span outside it (0 = inside)
    uint64_t seq = 0;        // set once that span counts as a freeze
    bool syscallDenied = false;
};

struct CompositorProbe::Window {
    uint64_t runNs = 0, waitNs = 0;
    int samples = 0, blocked = 0, idle = 0;
    std::unordered_map<uint16_t, int> labels;
};

struct CompositorProbe::DriverScan {
    int64_t tNs = 0;
    std::vector<DriverThread> threads;
    struct Base {
        int pid, tid;
        uint64_t stimeTicks;
        QString process, thread;
    };
    std::vector<Base> base;   // every thread of the GPU clients, for their kernel time afterwards
};

CompositorProbe::CompositorProbe(std::shared_ptr<LatencyFeed> feed, StallFn onStall, OngoingFn onOngoing)
    : feed_(std::move(feed)), onStall_(std::move(onStall)), onOngoing_(std::move(onOngoing))
{
    status_.message = QObject::tr("Off (Settings → Stutter detection)");
}

CompositorProbe::~CompositorProbe() { stop(); }

void CompositorProbe::start(double thresholdMs)
{
    stop();
    thresholdNs_ = int64_t(thresholdMs * 1e6);
    feed_->thresholdMs = thresholdMs;
    {
        std::lock_guard lock(mutex_);
        status_.message = QObject::tr("Looking for the Wayland compositor…");
    }
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void CompositorProbe::stop()
{
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    feed_->probes = 0;
    std::lock_guard lock(mutex_);
    status_.watching = false;
    status_.message = QObject::tr("Off (Settings → Stutter detection)");
    rtts_.clear();
}

void CompositorProbe::setGpuClients(std::vector<int> pids)
{
    std::lock_guard lock(mutex_);
    gpuClients_ = std::move(pids);
}

void CompositorProbe::summary(CompositorSummary& out)
{
    std::lock_guard lock(mutex_);
    out = status_;
    out.thresholdMs = double(thresholdNs_) / 1e6;
    const int64_t now = monoNs();
    while (!rtts_.empty() && now - rtts_.front().first > 1'000'000'000)
        rtts_.pop_front();
    if (!rtts_.empty()) {
        std::vector<int64_t> v;
        for (const auto& r : rtts_)
            v.push_back(r.second);
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        out.typicalMs = double(v[v.size() / 2]) / 1e6;
    }
    out.worstMs = status_.watching ? double(feed_->takeWorstUs()) / 1000.0 : -1;
}

void CompositorProbe::setStatus(bool watching, const QString& message)
{
    std::lock_guard lock(mutex_);
    status_.watching = watching;
    status_.message = message;
    status_.name = watching ? qs(comm_) : QString();
    status_.pid = watching ? pid_ : 0;
}

void CompositorProbe::loop()
{
    pthread_setname_np(pthread_self(), "culprit-comp");
    // Our deadlines are coarse: let the kernel batch our wakeups with others.
    prctl(PR_SET_TIMERSLACK, 1'000'000UL, 0, 0, 0);
    while (!stop_.load(std::memory_order_relaxed)) {
        QString why;
        if (connect(why)) {
            watch();
            disconnect();
        } else {
            setStatus(false, why);
        }
        // The compositor may start (or restart) later.
        for (int i = 0; i < 50 && !stop_.load(std::memory_order_relaxed); ++i)
            usleep(100'000);
    }
    disconnect();
}

bool CompositorProbe::connect(QString& why)
{
    const std::string path = waylandSocketPath();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        why = QObject::tr("Wayland socket path too long");
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    sock_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_ < 0 || ::connect(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        why = QObject::tr("No Wayland compositor at %1 (an X11 session?)").arg(qs(path));
        disconnect();
        return false;
    }
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(sock_, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.pid <= 0) {
        why = QObject::tr("Could not tell which process runs the Wayland compositor");
        disconnect();
        return false;
    }
    int pid = cred.pid;
    std::string comm = commOf(pid);
    if (endsWith(comm, "_wrapper"))
        if (const int child = childOf(pid)) {
            pid = child;
            comm = commOf(pid);
        }
    struct stat st {};
    if (::stat(SysPaths::proc_(std::to_string(pid)).c_str(), &st) != 0 || st.st_uid != getuid()) {
        why = QObject::tr("The compositor (%1) runs as another user, so its threads can't be inspected").arg(qs(comm));
        disconnect();
        return false;
    }
    fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL) | O_NONBLOCK);

    pid_ = pid;
    comm_ = comm;
    threads_.clear();
    ignored_.clear();
    labels_ = {std::string(), "(other)"};
    labelIds_.clear();
    fdCache_.clear();
    rx_.clear();
    pongs_.clear();
    driverScans_.clear();
    late_.clear();
    vm_.clear();
    pingSentNs_ = lastPingNs_ = lastDoneNs_ = pingPrevDoneNs_ = 0;
    lastTasksNs_ = lastVmNs_ = lastHelperNs_ = lastFeedNs_ = monoNs();
    dense_ = false;
    callbackFree_ = true;
    discardPing_ = false;
    mainSeq_ = 0;
    lastMainT0_ = lastMainT1_ = 0;
    suspendOffsetNs_ = bootNs() - monoNs();
    vmFile_.setPath(SysPaths::proc_("vmstat"));
    if (!refreshThreads(monoNs())) {
        why = QObject::tr("Could not read the threads of %1").arg(qs(comm));
        disconnect();
        return false;
    }
    feed_->probes = 1;
    setStatus(true, QString());
    return true;
}

void CompositorProbe::disconnect()
{
    if (sock_ >= 0)
        ::close(sock_);
    sock_ = -1;
    threads_.clear();
    feed_->probes = 0;
}

void CompositorProbe::watch()
{
    int64_t next = monoNs();
    while (!stop_.load(std::memory_order_relaxed)) {
        next += dense_ ? kBusyTickNs : kIdleTickNs;
        // Our own late wakeups say the whole system stalled, unless our last tick simply overran.
        const bool overran = monoNs() >= next;
        // Sleep until the next sample. A late answer to a ping wakes us when it
        // arrives (prompt ones were already collected during the tick).
        for (;;) {
            const int64_t now = monoNs();
            if (now >= next)
                break;
            pollfd p{sock_, POLLIN, 0};
            const timespec ts = nsToTimespec(next - now);
            const int r = ppoll(&p, 1, &ts, nullptr);
            if (r > 0 && !receive(monoNs()))
                return;
            if (r < 0 && errno != EINTR)
                return;
        }
        const int64_t now = monoNs();
        const int64_t offset = bootNs() - now;
        if (offset - suspendOffsetNs_ > 50'000'000) {   // the system was suspended
            suspendOffsetNs_ = offset;
            forgetSpans(now);
        }
        late_.push_back({now, overran ? 0 : now - next});
        if (late_.size() > kHistory)
            late_.pop_front();
        if (now - next > 1'000'000'000)
            next = now;   // starved: don't try to catch up
        if (!tick(now))
            return;
    }
}

void CompositorProbe::forgetSpans(int64_t now)
{
    // Spans and pings from before a suspend measure the suspend, not the compositor.
    discardPing_ = pingSentNs_ != 0;
    pongs_.clear();
    for (auto& t : threads_) {
        t->idleAt = t->busyFrom = 0;
        t->seq = 0;
        t->bornNs = now;   // give it time to show its event loop again
        t->hist.clear();
    }
}

bool CompositorProbe::receive(int64_t now)
{
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(sock_, buf, sizeof buf, MSG_DONTWAIT);
        if (n == 0) {
            setStatus(false, QObject::tr("%1 closed the Wayland connection").arg(qs(comm_)));
            return false;
        }
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        rx_.append(buf, size_t(n));
        // Wire format: object id, (size << 16 | opcode), arguments.
        while (rx_.size() >= 8) {
            uint32_t hdr[2];
            std::memcpy(hdr, rx_.data(), sizeof hdr);
            const size_t size = hdr[1] >> 16;
            const uint32_t op = hdr[1] & 0xffff;
            if (size < 8)
                return false;
            if (rx_.size() < size)
                break;
            if (hdr[0] == kCallbackId && op == 0) {   // wl_callback.done: the main loop answered
                if (pingSentNs_ && !discardPing_)
                    pongs_.push_back({pingSentNs_, now, pingPrevDoneNs_});
                lastDoneNs_ = now;
                pingSentNs_ = 0;
                discardPing_ = false;
            } else if (hdr[0] == 1 && op == 1 && size >= 12) {   // wl_display.delete_id
                uint32_t id;
                std::memcpy(&id, rx_.data() + 8, sizeof id);
                if (id == kCallbackId)
                    callbackFree_ = true;
            } else if (hdr[0] == 1 && op == 0) {   // wl_display.error
                setStatus(false, QObject::tr("%1 reported a Wayland protocol error").arg(qs(comm_)));
                return false;
            }
            rx_.erase(0, size);
        }
    }
}

bool CompositorProbe::sendPing(int64_t now)
{
    const uint32_t msg[3] = {1, 12u << 16, kCallbackId};   // wl_display.sync(new wl_callback)
    const ssize_t n = ::send(sock_, msg, sizeof msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == ssize_t(sizeof msg)) {
        pingSentNs_ = lastPingNs_ = now;
        pingPrevDoneNs_ = lastDoneNs_;
        callbackFree_ = false;
        mainSeq_ = 0;
        return true;
    }
    return n < 0 && (errno == EAGAIN || errno == EINTR);
}

CompositorProbe::Thread* CompositorProbe::mainThread() const
{
    for (const auto& t : threads_)
        if (t->role == Thread::Role::Main)
            return t.get();
    return nullptr;
}

bool CompositorProbe::refreshThreads(int64_t now)
{
    const std::string taskDir = SysPaths::proc_(std::to_string(pid_) + "/task");
    DIR* d = opendir(taskDir.c_str());
    if (!d)
        return false;
    std::unordered_set<int> present;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        const int tid = std::atoi(e->d_name);
        present.insert(tid);
        if (ignored_.count(tid) || std::any_of(threads_.begin(), threads_.end(), [tid](const auto& t) { return t->tid == tid; }))
            continue;
        const std::string base = taskDir + "/" + e->d_name + "/";
        auto t = std::make_unique<Thread>();
        t->tid = tid;
        readFirstLine(base + "comm", t->name);
        if (tid == pid_)
            t->role = Thread::Role::Main;
        else if (t->name == "JS Helper")   // SpiderMonkey's GC / compilation threads (GNOME Shell)
            t->role = Thread::Role::Helper;
        else if (!watchedName(t->name, comm_)) {
            ignored_.insert(tid);
            continue;
        }
        t->bornNs = now;
        t->schedstat.setPath(base + "schedstat");
        if (t->role != Thread::Role::Helper) {
            t->wchan.setPath(base + "wchan");
            t->syscall.setPath(base + "syscall");
        }
        threads_.push_back(std::move(t));
    }
    closedir(d);
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(), [&](const auto& t) { return !present.count(t->tid); }), threads_.end());
    for (auto it = ignored_.begin(); it != ignored_.end();)
        it = present.count(*it) ? std::next(it) : ignored_.erase(it);
    return mainThread() != nullptr;
}

std::string CompositorProbe::fdTarget(int fd)
{
    auto [it, inserted] = fdCache_.try_emplace(fd);
    if (inserted) {
        it->second = readLink(SysPaths::proc_(std::to_string(pid_) + "/fd/" + std::to_string(fd)));
        if (it->second.empty())
            it->second = "fd " + std::to_string(fd);
    }
    return it->second;
}

uint16_t CompositorProbe::label(const std::string& s)
{
    if (auto it = labelIds_.find(s); it != labelIds_.end())
        return it->second;
    if (labels_.size() >= 4096)
        return 1;   // "(other)"
    const auto id = uint16_t(labels_.size());
    labels_.push_back(s);
    labelIds_.emplace(s, id);
    return id;
}

std::string CompositorProbe::describe(Thread& t, std::string_view wchan)
{
    std::string what;
    // Yama ptrace_scope >= 1 hides the syscall from us; the wait site alone still says a lot.
    if (!t.syscallDenied && t.syscall.readSingle(buf2_)) {
        Tokens tk(buf2_);
        std::string_view nr, a0, a1;
        int64_t n = 0;
        if (tk.next(nr) && nr == "-1") {
            what = "kernel, outside a syscall (page fault?)";
        } else if (parseInt(nr, n) && tk.next(a0) && tk.next(a1)) {
            uint64_t fd = 0, req = 0;
            parseHex64(a0, fd);
            parseHex64(a1, req);
            if (n == 16) {
                what = ioctlName(fdTarget(int(fd)), req);
            } else if (n == 202 || n == 449) {
                what = "futex";
            } else {
                const char* name = syscallName(n);
                what = name ? name : "syscall " + std::to_string(n);
                if (fdSyscall(n))
                    what += " " + fdTarget(int(fd));
            }
        }
    } else {
        t.syscallDenied = true;
    }
    if (what.empty())
        return std::string(wchan);
    return what + " in " + std::string(wchan);
}

bool CompositorProbe::sample(Thread& t, int64_t now)
{
    TaskSchedstat ss;
    if (!t.schedstat.readSingle(buf_) || !parseTaskSchedstat(buf_, ss))
        return false;   // exited
    Thread::Sample s{now, ss.runNs, ss.waitNs, kRunning, 0};
    if (t.role != Thread::Role::Helper) {
        if (!t.wchan.readSingle(buf_))
            return false;
        const std::string_view w = trim(buf_);
        if (idleWchan(w)) {
            s.kind = kIdle;
        } else if (w != "0") {   // "0": on a CPU or runnable
            s.kind = kBlocked;
            s.label = label(describe(t, w));
        }
    }
    t.hist.push_back(s);
    if (t.hist.size() > kHistory)
        t.hist.pop_front();
    return true;
}

CompositorProbe::Window CompositorProbe::window(const Thread& t, int64_t a, int64_t b) const
{
    Window w;
    const Thread::Sample* first = nullptr;
    const Thread::Sample* last = nullptr;
    for (const Thread::Sample& s : t.hist) {
        if (s.tNs > b)
            break;
        if (s.tNs <= a) {
            first = &s;
        } else {
            ++w.samples;
            if (s.kind == kBlocked) {
                ++w.blocked;
                ++w.labels[s.label];
            } else if (s.kind == kIdle) {
                ++w.idle;
            }
        }
        last = &s;
    }
    if (!first && !t.hist.empty())
        first = &t.hist.front();
    if (first && last && last->runNs >= first->runNs && last->waitNs >= first->waitNs) {
        w.runNs = last->runNs - first->runNs;
        w.waitNs = last->waitNs - first->waitNs;
    }
    return w;
}

bool CompositorProbe::stuck(const Thread& t, int64_t a, int64_t b) const
{
    // Out of its event loop because it ran (or waited to run), or because it
    // slept somewhere else — not merely cycling faster than we sample.
    const Window w = window(t, a, b);
    return double(w.runNs + w.waitNs) >= 0.6 * double(b - a) || (w.samples > 0 && w.blocked * 2 >= w.samples);
}

int64_t CompositorProbe::stallStart(const Thread& main, int64_t sentNs, int64_t prevDoneNs) const
{
    // A freeze counts from the ping, unless the main thread was already asleep
    // outside its event loop when the ping went out (sampled in the same tick).
    // Then it began after the loop last ran — its last sample in the loop, or
    // the previous ping's answer — and we take the middle of that gap. A main
    // thread that is just busy drawing frames answers promptly, so it never
    // looks frozen.
    auto it = main.hist.rbegin();
    while (it != main.hist.rend() && it->tNs > sentNs)
        ++it;
    if (it == main.hist.rend() || it->tNs != sentNs || it->kind != kBlocked)
        return sentNs;
    int64_t ran = std::max(prevDoneNs, sentNs - kIdleTickNs);
    for (; it != main.hist.rend() && it->tNs > ran; ++it)
        if (it->kind == kIdle) {
            ran = it->tNs;
            break;
        }
    return ran + (sentNs - ran) / 2;
}

bool CompositorProbe::duringMainFreeze(int64_t a, int64_t b) const
{
    // A freeze of the main thread is reported with this thread's activity in it.
    if (pingSentNs_)
        if (const Thread* m = mainThread(); m && b - stallStart(*m, pingSentNs_, pingPrevDoneNs_) >= thresholdNs_ / 2)
            return true;
    const int64_t overlap = std::min(b, lastMainT1_) - std::max(a, lastMainT0_);
    return overlap > 0 && overlap * 2 >= b - a;
}

bool CompositorProbe::track(Thread& t, int64_t now)
{
    const Thread::Sample& s = t.hist.back();
    if (s.kind == kIdle) {
        if (t.busyFrom && t.role != Thread::Role::Main && now - t.busyFrom >= thresholdNs_) {
            if (stuck(t, t.busyFrom, now) && !duringMainFreeze(t.busyFrom, now))
                finish(t, t.busyFrom, now, t.seq);
            else if (t.seq)
                driverScans_.erase(t.seq);
        }
        t.idleAt = now;
        t.busyFrom = 0;
        t.seq = 0;
        return true;
    }
    if (!t.idleAt)
        return t.role == Thread::Role::Main || now - t.bornNs < kNeverIdleNs;   // a parked worker, not an event loop
    if (!t.busyFrom)
        t.busyFrom = t.idleAt;
    if (t.role != Thread::Role::Main && !t.seq && now - t.busyFrom >= thresholdNs_ && stuck(t, t.busyFrom, now) &&
        !duringMainFreeze(t.busyFrom, now))
        t.seq = ongoing(t);
    return true;
}

bool CompositorProbe::tick(int64_t now)
{
    // Ping first: the compositor answers while we sample (well under a
    // millisecond), so the answer is usually waiting when we are done and
    // costs no wakeup of its own.
    if (!pingSentNs_ && callbackFree_ && now - lastPingNs_ >= kPingEveryNs && !sendPing(now)) {
        setStatus(false, QObject::tr("Lost the Wayland connection to %1").arg(qs(comm_)));
        return false;
    }
    if (now - lastTasksNs_ >= kTasksEveryNs) {
        lastTasksNs_ = now;
        if (!refreshThreads(now)) {
            setStatus(false, QObject::tr("%1 exited").arg(qs(comm_)));
            return false;
        }
    }
    if (now - lastVmNs_ >= kVmEveryNs) {
        lastVmNs_ = now;
        sampleVm(now);
    }
    if (now - fdCacheNs_ > 10'000'000'000LL) {   // fds get closed and reused
        fdCache_.clear();
        fdCacheNs_ = now;
    }

    const bool helpers = now - lastHelperNs_ >= kHelperEveryNs;
    if (helpers)
        lastHelperNs_ = now;
    for (auto it = threads_.begin(); it != threads_.end();) {
        Thread& t = **it;
        if (t.role == Thread::Role::Helper && !helpers) {
            ++it;
            continue;
        }
        if (!sample(t, now)) {
            it = threads_.erase(it);
            continue;
        }
        ++it;
    }
    Thread* main = mainThread();
    if (!main) {
        setStatus(false, QObject::tr("%1 exited").arg(qs(comm_)));
        return false;
    }
    if (!receive(monoNs()))   // the answer to this tick's ping, usually
        return false;

    // Answered pings, now that the samples cover them.
    for (const auto& [sent, done, prevDone] : pongs_) {
        const int64_t rtt = done - sent;
        bucketWorstNs_ = std::max(bucketWorstNs_, rtt);
        {
            std::lock_guard lock(mutex_);
            rtts_.push_back({done, rtt});
            while (rtts_.size() > 200)
                rtts_.pop_front();
        }
        const int64_t t0 = stallStart(*main, sent, prevDone);
        if (done - t0 >= thresholdNs_) {
            finish(*main, t0, done, mainSeq_);
            lastMainT0_ = t0;
            lastMainT1_ = done;
        } else if (mainSeq_) {
            driverScans_.erase(mainSeq_);
        }
        mainSeq_ = 0;
    }
    pongs_.clear();

    bool watchedBusy = false;
    for (auto it = threads_.begin(); it != threads_.end();) {
        Thread& t = **it;
        if (t.role != Thread::Role::Helper && t.hist.back().tNs == now && !track(t, now)) {
            ignored_.insert(t.tid);
            it = threads_.erase(it);
            continue;
        }
        watchedBusy = watchedBusy || (t.role == Thread::Role::Watched && t.idleAt && t.hist.back().kind != kIdle);
        ++it;
    }

    // A ping still unanswered past the threshold: the main thread is frozen right now.
    if (pingSentNs_ && !discardPing_) {
        const int64_t age = now - stallStart(*main, pingSentNs_, pingPrevDoneNs_);
        bucketWorstNs_ = std::max(bucketWorstNs_, age);
        if (!mainSeq_ && age >= thresholdNs_)
            mainSeq_ = ongoing(*main);
    }
    if (now - lastFeedNs_ >= kFeedEveryNs) {
        LatencyFeed::Point pt;
        pt.tNs = now;
        pt.maxUs = float(double(bucketWorstNs_) / 1000.0);
        feed_->push(pt);
        bucketWorstNs_ = 0;
        lastFeedNs_ = now;
    }
    // Sample densely while something may be stuck: a ping unanswered for a
    // tick, or a cursor/input thread seen outside its event loop.
    dense_ = (pingSentNs_ && now - pingSentNs_ >= kBusyTickNs) || watchedBusy;
    return true;
}

uint64_t CompositorProbe::ongoing(const Thread& t)
{
    const uint64_t seq = ++seq_;
    std::vector<int> tids{t.tid};
    bool driver = false;
    for (const auto& o : threads_) {
        if (o->role == Thread::Role::Helper || o->hist.empty())
            continue;
        if (o.get() != &t)
            tids.push_back(o->tid);
        // Asleep in the GPU driver or on a lock (maybe held by a thread that is): who else is in there?
        const Thread::Sample& s = o->hist.back();
        if (s.kind == kBlocked) {
            const std::string& l = labels_[s.label];
            driver = driver || driverLabel(l) || (o.get() == &t && startsWith(l, "futex"));
        }
    }
    if (driver) {
        auto scan = std::make_unique<DriverScan>(scanDriver(t.tid));
        for (const DriverThread& d : scan->threads)
            if (!d.waitingForLock && tids.size() < 16)
                tids.push_back(d.tid);
        driverScans_[seq] = std::move(scan);
        while (driverScans_.size() > 8)
            driverScans_.erase(driverScans_.begin());
    }
    if (onOngoing_)
        onOngoing_(seq, std::move(tids));
    return seq;
}

CompositorProbe::DriverScan CompositorProbe::scanDriver(int skipTid)
{
    DriverScan scan;
    scan.tNs = monoNs();
    std::vector<int> pids;
    {
        std::lock_guard lock(mutex_);
        pids = gpuClients_;
    }
    pids.push_back(pid_);
    pids.push_back(int(getpid()));
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    for (int pid : pids) {
        const std::string base = SysPaths::proc_(std::to_string(pid));
        DIR* d = opendir((base + "/task").c_str());
        if (!d)
            continue;
        const QString process = qs(commOf(pid));
        std::unordered_map<int, std::string> fds;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9')
                continue;
            const int tid = std::atoi(e->d_name);
            if (tid == skipTid)
                continue;
            const std::string dir = base + "/task/" + e->d_name;
            PidStat ps;
            if (!readFile(dir + "/stat", buf2_) || !parsePidStat(buf2_, ps))
                continue;
            const QString thread = QString::fromUtf8(ps.comm.data(), qsizetype(ps.comm.size()));
            if (scan.base.size() < 2048)
                scan.base.push_back({pid, tid, ps.stime, process, thread});
            if (scan.threads.size() >= kMaxDriverThreads)
                continue;
            std::string wchan;
            readFirstLine(dir + "/wchan", wchan);
            DriverThread dt;
            dt.waitingForLock = nvidiaLockWchan(wchan);
            bool inDriver = dt.waitingForLock;
            // A thread on a CPU shows "running" here, not its syscall: kernelTimes() catches those.
            if (readFile(dir + "/syscall", buf2_)) {
                Tokens tk(buf2_);
                std::string_view nr, a0, a1;
                uint64_t fd = 0, req = 0;
                if (tk.next(nr) && nr == "16" && tk.next(a0) && tk.next(a1) && parseHex64(a0, fd) && parseHex64(a1, req)) {
                    auto [it, inserted] = fds.try_emplace(int(fd));
                    if (inserted)
                        it->second = readLink(base + "/fd/" + std::to_string(fd));
                    if (gpuDevice(it->second)) {
                        inDriver = true;
                        dt.call = qs(ioctlName(it->second, req));
                    }
                }
            }
            if (!inDriver)
                continue;
            dt.pid = pid;
            dt.tid = tid;
            dt.process = process;
            dt.thread = thread;
            dt.wchan = wchan == "0" ? QString() : qs(wchan);
            scan.threads.push_back(std::move(dt));
        }
        closedir(d);
    }
    return scan;
}

void CompositorProbe::kernelTimes(const DriverScan& scan, int64_t endNs, std::vector<DriverThread>& out)
{
    // A thread that holds the driver's lock and works inside it runs in the
    // kernel: its system time grows while everyone else waits.
    static const double msPerTick = 1000.0 / double(sysconf(_SC_CLK_TCK));
    const double windowMs = double(endNs - scan.tNs) / 1e6;
    for (const DriverScan::Base& b : scan.base) {
        PidStat ps;
        if (!readFile(SysPaths::proc_(std::to_string(b.pid) + "/task/" + std::to_string(b.tid) + "/stat"), buf2_) ||
            !parsePidStat(buf2_, ps) || ps.stime < b.stimeTicks)
            continue;
        const double ms = double(ps.stime - b.stimeTicks) * msPerTick;
        if (ms < std::max(30.0, 0.25 * windowMs))
            continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const DriverThread& d) { return d.tid == b.tid; });
        if (it == out.end()) {
            if (out.size() >= kMaxDriverThreads)
                continue;
            DriverThread d;
            d.pid = b.pid;
            d.tid = b.tid;
            d.process = b.process;
            d.thread = b.thread;
            out.push_back(d);
            it = std::prev(out.end());
        }
        it->kernelMs = ms;
    }
}

void CompositorProbe::finish(const Thread& t, int64_t t0, int64_t t1, uint64_t seq)
{
    auto s = std::make_shared<CompositorStall>();
    s->seq = seq ? seq : ++seq_;
    s->pid = pid_;
    s->tid = t.tid;
    s->process = qs(comm_);
    s->mainThread = t.role == Thread::Role::Main;
    auto threadName = [this](const Thread& x) {
        return x.role == Thread::Role::Main ? QObject::tr("main thread")
               : x.name == comm_            ? QObject::tr("thread %1").arg(x.tid)
                                            : qs(x.name);
    };
    s->thread = threadName(t);
    s->t0Ns = t0;
    s->t1Ns = t1;
    const Window w = window(t, t0, t1);
    s->runMs = double(w.runNs) / 1e6;
    s->waitMs = double(w.waitNs) / 1e6;
    s->samples = w.samples;
    for (const auto& [id, count] : w.labels)
        s->blockedIn.push_back({qs(labels_[id]), count});
    // The ping went unanswered, so a main thread "in poll" was waiting inside some call, not in its event loop.
    if (s->mainThread && w.idle > 0)
        s->blockedIn.push_back({QObject::tr("poll / epoll wait inside a call (not its event loop)"), w.idle});
    std::sort(s->blockedIn.begin(), s->blockedIn.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& o : threads_) {
        if (o.get() == &t)
            continue;
        const Window ow = window(*o, t0, t1);
        if (o->role == Thread::Role::Helper) {
            s->helperMs += double(ow.runNs) / 1e6;
            continue;
        }
        if (o->role != Thread::Role::Main && !o->idleAt)
            continue;
        ThreadActivity a;
        a.name = threadName(*o);
        a.runMs = double(ow.runNs) / 1e6;
        a.waitMs = double(ow.waitNs) / 1e6;
        a.samples = ow.samples;
        a.blockedSamples = ow.blocked;
        const auto top = std::max_element(ow.labels.begin(), ow.labels.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        if (top != ow.labels.end())
            a.blockedIn = qs(labels_[top->first]);
        s->others.push_back(a);
    }
    if (auto it = driverScans_.find(s->seq); it != driverScans_.end()) {
        s->driver = std::move(it->second->threads);
        kernelTimes(*it->second, t1, s->driver);
        driverScans_.erase(it);
    }
    for (const auto& [tick, late] : late_)
        if (tick >= t0 && tick <= t1)
            s->selfLateMs = std::max(s->selfLateMs, double(late) / 1e6);
    if (!vm_.empty()) {
        const VmPoint* a = &vm_.front();
        for (const VmPoint& p : vm_)
            if (p.tNs <= t0)
                a = &p;
        const VmPoint& b = vm_.back();
        s->compactStalls = b.compact - std::min(b.compact, a->compact);
        s->allocStalls = b.alloc - std::min(b.alloc, a->alloc);
        s->majorFaults = b.majflt - std::min(b.majflt, a->majflt);
    }
    {
        std::lock_guard lock(mutex_);
        ++status_.freezes;
    }
    feed_->hitches.fetch_add(1);
    if (onStall_)
        onStall_(std::move(s));
}

void CompositorProbe::sampleVm(int64_t now)
{
    VmStat vm;
    if (!vmFile_.read(buf_) || !parseVmstat(buf_, vm))
        return;
    vm_.push_back({now, vm.compactStall, vm.allocstall, vm.pgmajfault});
    while (vm_.size() > 300)   // 30 s
        vm_.pop_front();
}

} // namespace culprit
