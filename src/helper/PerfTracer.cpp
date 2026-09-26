// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "helper/PerfTracer.h"

#include "common/fs/File.h"
#include "common/parse/Text.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace culprit {

namespace {

int perfEventOpen(perf_event_attr* attr, pid_t pid, int cpu, int group, unsigned long flags)
{
    return int(syscall(SYS_perf_event_open, attr, pid, cpu, group, flags));
}

} // namespace

std::vector<int> onlineCpus()
{
    // "0-7,9,12-15"
    std::vector<int> cpus;
    std::string s;
    if (!readFirstLine("/sys/devices/system/cpu/online", s))
        return cpus;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos)
            j = s.size();
        std::string_view part(s.data() + i, j - i);
        int a = 0, b = 0;
        if (auto dash = part.find('-'); dash != std::string_view::npos) {
            if (parseInt(part.substr(0, dash), a) && parseInt(part.substr(dash + 1), b))
                for (int c = a; c <= b; ++c)
                    cpus.push_back(c);
        } else if (parseInt(part, a)) {
            cpus.push_back(a);
        }
        i = j + 1;
    }
    return cpus;
}

PerfTracer::~PerfTracer() { close(); }

int PerfTracer::indexOf(const std::string& sys, const std::string& name) const
{
    for (size_t i = 0; i < tps_.size(); ++i)
        if (tps_[i].sys == sys && tps_[i].name == name)
            return int(i);
    return -1;
}

bool PerfTracer::open(const std::string& tracefs, const std::vector<std::pair<std::string, std::string>>& wanted, int pagesPerCpu,
                      std::string& error)
{
    close();
    std::string buf;
    for (const auto& [sys, name] : wanted) {
        Tracepoint tp;
        tp.sys = sys;
        tp.name = name;
        if (!readFile(tracefs + "/events/" + sys + "/" + name + "/format", buf) || !parseTraceFormat(buf, tp.fmt))
            continue;   // not available on this kernel
        tps_.push_back(std::move(tp));
    }
    if (tps_.empty()) {
        error = "no tracepoints available under " + tracefs;
        return false;
    }

    const long page = sysconf(_SC_PAGESIZE);
    const size_t dataLen = size_t(pagesPerCpu) * size_t(page);
    for (int cpu : onlineCpus()) {
        Ring ring;
        ring.cpu = cpu;
        maxCpu_ = std::max(maxCpu_, cpu + 1);
        for (size_t t = 0; t < tps_.size(); ++t) {
            perf_event_attr attr{};
            attr.type = PERF_TYPE_TRACEPOINT;
            attr.size = sizeof attr;
            attr.config = tps_[t].fmt.id;
            attr.sample_period = 1;
            attr.sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW;
            attr.disabled = 1;
            attr.watermark = 1;
            attr.wakeup_watermark = uint32_t(dataLen / 4);
            attr.use_clockid = 1;
            attr.clockid = CLOCK_MONOTONIC;
            const int fd = perfEventOpen(&attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
            if (fd < 0) {
                error = "perf_event_open(" + tps_[t].sys + ":" + tps_[t].name + ", cpu " + std::to_string(cpu) + "): " + strerror(errno);
                continue;
            }
            if (ring.leader < 0) {
                ring.mapLen = dataLen + size_t(page);
                void* m = mmap(nullptr, ring.mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (m == MAP_FAILED) {
                    error = std::string("mmap: ") + strerror(errno);
                    ::close(fd);
                    continue;
                }
                ring.base = static_cast<uint8_t*>(m);
                auto* meta = reinterpret_cast<perf_event_mmap_page*>(ring.base);
                ring.data = ring.base + (meta->data_offset ? meta->data_offset : uint64_t(page));
                ring.dataSize = meta->data_size ? meta->data_size : dataLen;
                ring.leader = fd;
            } else if (ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, ring.leader) != 0) {
                error = std::string("PERF_EVENT_IOC_SET_OUTPUT: ") + strerror(errno);
                ::close(fd);
                continue;
            }
            uint64_t id = 0;
            if (ioctl(fd, PERF_EVENT_IOC_ID, &id) == 0)
                idToTp_[id] = int(t);
            ring.fds.push_back(fd);
        }
        if (ring.leader >= 0)
            rings_.push_back(std::move(ring));
    }
    if (rings_.empty()) {
        if (error.empty())
            error = "could not open any perf event";
        return false;
    }
    return true;
}

void PerfTracer::enable()
{
    for (Ring& r : rings_)
        for (int fd : r.fds)
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
}

void PerfTracer::disable()
{
    for (Ring& r : rings_)
        for (int fd : r.fds)
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
}

void PerfTracer::close()
{
    for (Ring& r : rings_) {
        for (int fd : r.fds)
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        if (r.base)
            munmap(r.base, r.mapLen);
        for (int fd : r.fds)
            ::close(fd);
    }
    rings_.clear();
    tps_.clear();
    idToTp_.clear();
}

void PerfTracer::drain(std::vector<TraceEvent>& out, std::vector<uint64_t>& lostPerCpu)
{
    if (lostPerCpu.size() < size_t(maxCpu_))
        lostPerCpu.resize(size_t(maxCpu_), 0);
    for (Ring& r : rings_) {
        auto* meta = reinterpret_cast<perf_event_mmap_page*>(r.base);
        const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
        uint64_t tail = meta->data_tail;
        while (tail + sizeof(perf_event_header) <= head) {
            // Records can wrap around the end of the ring: copy into scratch if so.
            const uint64_t off = tail % r.dataSize;
            perf_event_header hdr;
            if (off + sizeof hdr <= r.dataSize) {
                std::memcpy(&hdr, r.data + off, sizeof hdr);
            } else {
                const size_t first = size_t(r.dataSize - off);
                std::memcpy(&hdr, r.data + off, first);
                std::memcpy(reinterpret_cast<uint8_t*>(&hdr) + first, r.data, sizeof hdr - first);
            }
            if (hdr.size < sizeof hdr || tail + hdr.size > head)
                break;
            const uint8_t* rec;
            if (off + hdr.size <= r.dataSize) {
                rec = r.data + off;
            } else {
                scratch_.resize(hdr.size);
                const size_t first = size_t(r.dataSize - off);
                std::memcpy(scratch_.data(), r.data + off, first);
                std::memcpy(scratch_.data() + first, r.data, hdr.size - first);
                rec = scratch_.data();
            }

            if (hdr.type == PERF_RECORD_SAMPLE) {
                // IDENTIFIER, TID(pid,tid), TIME, CPU(cpu,res), RAW(size, data)
                const uint8_t* p = rec + sizeof hdr;
                const uint8_t* end = rec + hdr.size;
                if (p + 8 + 8 + 8 + 8 + 4 <= end) {
                    uint64_t id, time;
                    uint32_t pid, tid, cpu, res, rawSize;
                    std::memcpy(&id, p, 8);
                    std::memcpy(&pid, p + 8, 4);
                    std::memcpy(&tid, p + 12, 4);
                    std::memcpy(&time, p + 16, 8);
                    std::memcpy(&cpu, p + 24, 4);
                    std::memcpy(&res, p + 28, 4);
                    std::memcpy(&rawSize, p + 32, 4);
                    const uint8_t* raw = p + 36;
                    auto it = idToTp_.find(id);
                    if (it != idToTp_.end() && raw + rawSize <= end) {
                        TraceEvent ev;
                        ev.timeNs = time;
                        ev.tp = it->second;
                        ev.cpu = int32_t(cpu);
                        ev.pid = int32_t(pid);
                        ev.tid = int32_t(tid);
                        ev.rawLen = uint16_t(std::min<uint32_t>(rawSize, sizeof ev.raw));
                        std::memcpy(ev.raw, raw, ev.rawLen);
                        out.push_back(ev);
                    }
                }
            } else if (hdr.type == PERF_RECORD_LOST) {
                uint64_t lost = 0;
                if (hdr.size >= sizeof hdr + 16)
                    std::memcpy(&lost, rec + sizeof hdr + 8, 8);
                if (r.cpu >= 0 && size_t(r.cpu) < lostPerCpu.size())
                    lostPerCpu[size_t(r.cpu)] += lost;
            }
            tail += hdr.size;
        }
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    }
}

} // namespace culprit
