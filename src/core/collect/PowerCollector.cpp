// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/PowerCollector.h"

#include "common/parse/Text.h"

#include <algorithm>
#include <dirent.h>

namespace culprit {

PowerCollector::PowerCollector()
{
    // On AMD the powercap "core" zone reports the energy counter of whichever
    // core reads the MSR, not the sum of all cores, so it is useless here.
    std::string cpuinfo;
    if (readFile(SysPaths::proc_("cpuinfo"), cpuinfo) && cpuinfo.find("AuthenticAMD") != std::string::npos)
        coreZoneTrustworthy_ = false;

    const std::string root = SysPaths::sys_("class/powercap");
    DIR* d = opendir(root.c_str());
    if (!d)
        return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        // intel-rapl:0 (package), intel-rapl:0:0 (subzone); skip the "intel-rapl" control type itself
        if (startsWith(e->d_name, "intel-rapl:") || startsWith(e->d_name, "amd-rapl:"))
            names.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        const std::string dir = root + "/" + n;
        std::string name;
        if (!readFirstLine(dir + "/name", name))
            continue;
        Zone z;
        if (startsWith(name, "package"))
            z.kind = Kind::Package;
        else if (name == "core" && coreZoneTrustworthy_)
            z.kind = Kind::Core;
        else if (name == "uncore")
            z.kind = Kind::Uncore;
        else if (name == "dram")
            z.kind = Kind::Dram;
        else
            continue;   // psys etc. would double count
        int64_t range = 0;
        readInt64(dir + "/max_energy_range_uj", range);
        z.maxRangeUj = uint64_t(std::max<int64_t>(0, range));
        z.energy.setPath(dir + "/energy_uj");
        int64_t v = 0;
        if (!z.energy.readInt64(v))
            continue;   // not readable (root-only on many kernels)
        zones_.push_back(std::move(z));
    }
}

void PowerCollector::sample(PowerSample& out, int64_t nowNs)
{
    const double dt = lastNs_ > 0 ? double(nowNs - lastNs_) / 1e9 : 0;
    double pkg = 0, core = 0, uncore = 0, dram = 0;
    bool havePkg = false, haveCore = false, haveUncore = false, haveDram = false, any = false;
    for (Zone& z : zones_) {
        int64_t v = 0;
        if (!z.energy.readInt64(v))
            continue;
        const uint64_t uj = uint64_t(v);
        if (z.have && dt > 0) {
            uint64_t delta = uj >= z.lastUj ? uj - z.lastUj : (z.maxRangeUj - z.lastUj) + uj;   // counter wrap
            const double w = double(delta) / 1e6 / dt;
            any = true;
            switch (z.kind) {
            case Kind::Package: pkg += w; havePkg = true; break;
            case Kind::Core: core += w; haveCore = true; break;
            case Kind::Uncore: uncore += w; haveUncore = true; break;
            case Kind::Dram: dram += w; haveDram = true; break;
            case Kind::Other: break;
            }
        }
        z.lastUj = uj;
        z.have = true;
    }
    lastNs_ = nowNs;
    out.available = any;
    out.packageW = havePkg ? pkg : -1;
    out.coreW = haveCore ? core : -1;
    out.uncoreW = haveUncore ? uncore : (havePkg && haveCore ? std::max(0.0, pkg - core) : -1);
    out.dramW = haveDram ? dram : -1;
    if (havePkg && any) {
        floorW_ = std::min(floorW_, pkg);
        out.idleFloorW = floorW_;
    }
    out.attributableW = haveCore ? core : (havePkg ? std::max(0.0, pkg - floorW_) : -1);
}

} // namespace culprit
