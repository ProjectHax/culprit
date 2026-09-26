// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/CgroupCollector.h"

#include "common/parse/PidParsers.h"
#include "common/parse/SystemParsers.h"

#include <algorithm>
#include <unordered_set>

namespace culprit {

void CgroupCollector::sample(Frame& frame, int64_t nowNs)
{

    // Cgroups of the busiest processes, plus every ancestor.
    std::vector<const ProcSample*> busy;
    for (const ProcSample& p : frame.procs)
        if (!p.cgroup.isEmpty() && p.cpuPct >= 1.0)
            busy.push_back(&p);
    std::sort(busy.begin(), busy.end(), [](auto* a, auto* b) { return a->cpuPct > b->cpuPct; });
    if (busy.size() > 40)
        busy.resize(40);
    std::unordered_set<std::string> wanted;
    for (const ProcSample* p : busy) {
        std::string path = p->cgroup.toStdString();
        while (!path.empty() && path != "/") {
            wanted.insert(path);
            path.resize(path.rfind('/'));
        }
    }

    const std::string root = SysPaths::sys_("fs/cgroup");
    for (const std::string& path : wanted) {
        auto [it, inserted] = cgroups_.try_emplace(path);
        State& st = it->second;
        if (inserted) {
            st.cpuStat.setPath(root + path + "/cpu.stat");
            // .local: events caused by this cgroup's own limit, not its descendants'.
            st.memEvents.setPath(root + path + "/memory.events.local");
            st.cpuMax.setPath(root + path + "/cpu.max");
            st.memHigh.setPath(root + path + "/memory.high");
        }
        // Rates over this cgroup's own gap (it may not have been sampled last interval).
        const double dt = st.have ? double(nowNs - st.seenNs) / 1e9 : 0;
        st.seenNs = nowNs;

        uint64_t periods = 0, throttled = 0, throttledUs = 0, high = 0, max = 0, oom = 0;
        if (st.cpuStat.readSingle(buf_))
            forEachKeyValue(buf_, [&](std::string_view k, uint64_t v) {
                if (k == "nr_periods") periods = v;
                else if (k == "nr_throttled") throttled = v;
                else if (k == "throttled_usec") throttledUs = v;
            });
        if (st.memEvents.readSingle(buf_))
            forEachKeyValue(buf_, [&](std::string_view k, uint64_t v) {
                if (k == "high") high = v;
                else if (k == "max") max = v;
                else if (k == "oom_kill") oom = v;
            });

        if (st.have && dt > 0) {
            CgroupSample cs;
            cs.path = QString::fromStdString(path);
            const std::string_view unit = cgroupUnitName(path);
            cs.unit = unit.empty() ? cs.path : QString::fromUtf8(unit.data(), qsizetype(unit.size()));
            const uint64_t dp = periods - std::min(periods, st.periods);
            const uint64_t dthr = throttled - std::min(throttled, st.throttled);
            cs.throttledPct = dp ? double(dthr) * 100.0 / double(dp) : 0;
            cs.throttledMsPs = double(throttledUs - std::min(throttledUs, st.throttledUs)) / 1000.0 / dt;
            cs.memHighEvents = high - std::min(high, st.high);
            cs.memMaxEvents = max - std::min(max, st.max);
            cs.oomKills = oom - std::min(oom, st.oom);
            if (st.cpuMax.readSingle(buf_)) {
                Tokens t(buf_);
                std::string_view quota, period;
                double q = 0, pr = 0;
                if (t.next(quota) && t.next(period) && quota != "max" && parseDouble(quota, q) && parseDouble(period, pr) && pr > 0)
                    cs.cpuQuotaCores = q / pr;
            }
            if (st.memHigh.readSingle(buf_)) {
                double h = 0;
                std::string_view v = trim(buf_);
                if (v != "max" && parseDouble(v, h))
                    cs.memHighBytes = h;
            }
            if (cs.throttledPct > 0 || cs.memHighEvents || cs.memMaxEvents || cs.oomKills || cs.cpuQuotaCores > 0)
                frame.cgroups.push_back(std::move(cs));
        }
        st.periods = periods;
        st.throttled = throttled;
        st.throttledUs = throttledUs;
        st.high = high;
        st.max = max;
        st.oom = oom;
        st.have = true;
    }

    // Forget cgroups we haven't looked at for a while.
    for (auto it = cgroups_.begin(); it != cgroups_.end();) {
        if (nowNs - it->second.seenNs > 60'000'000'000LL)
            it = cgroups_.erase(it);
        else
            ++it;
    }
    std::sort(frame.cgroups.begin(), frame.cgroups.end(),
              [](const CgroupSample& a, const CgroupSample& b) { return a.throttledMsPs > b.throttledMsPs; });
}

} // namespace culprit
