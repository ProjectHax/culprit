// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "common/parse/PidParsers.h"

#include "common/parse/Text.h"

namespace culprit {

bool parsePidStat(std::string_view text, PidStat& out)
{
    // "pid (comm) S ppid ..." — comm may contain spaces and ')' so find the last ')'.
    auto open = text.find('(');
    auto close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return false;
    if (!parseInt(text.substr(0, open), out.pid))
        return false;
    out.comm = text.substr(open + 1, close - open - 1);

    Tokens tok(text.substr(close + 1));
    std::string_view st;
    if (!tok.next(st) || st.empty())
        return false;
    out.state = st.front();

    // Field numbers below follow proc(5), starting at field 4 (ppid).
    uint64_t f[52] = {};
    int n = 4;
    std::string_view t;
    while (n < 52 && tok.next(t)) {
        int64_t sv = 0;
        if (!t.empty() && t.front() == '-') {
            parseInt(t, sv);
            f[n] = uint64_t(sv);
        } else {
            parseInt(t, f[n]);
        }
        ++n;
    }
    if (n < 25)
        return false;
    out.ppid = int(f[4]);
    out.pgrp = int(f[5]);
    out.session = int(f[6]);
    out.ttyNr = int(f[7]);
    out.flags = unsigned(f[9]);
    out.minflt = f[10];
    out.majflt = f[12];
    out.utime = f[14];
    out.stime = f[15];
    out.priority = int64_t(f[18]);
    out.nice = int64_t(f[19]);
    out.numThreads = int64_t(f[20]);
    out.starttime = f[22];
    out.vsize = f[23];
    out.rssPages = int64_t(f[24]);
    out.processor = n > 39 ? int(f[39]) : -1;
    out.rtPriority = n > 40 ? unsigned(f[40]) : 0;
    out.policy = n > 41 ? unsigned(f[41]) : 0;
    out.blkioTicks = n > 42 ? f[42] : 0;
    return true;
}

bool parseTaskSchedstat(std::string_view text, TaskSchedstat& out)
{
    Tokens tok(text);
    return tok.nextInt(out.runNs) && tok.nextInt(out.waitNs) && tok.nextInt(out.slices);
}

bool parsePidStatus(std::string_view text, PidStatus& out)
{
    LineReader lines(text);
    std::string_view line;
    bool any = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        if (!tok.next(key))
            continue;
        if (key == "Tgid:")
            any |= tok.nextInt(out.tgid);
        else if (key == "Uid:")
            tok.nextInt(out.uid);
        else if (key == "VmSwap:")
            tok.nextInt(out.vmSwapKb);
        else if (key == "RssAnon:")
            tok.nextInt(out.rssAnonKb);
        else if (key == "RssFile:")
            tok.nextInt(out.rssFileKb);
        else if (key == "RssShmem:")
            tok.nextInt(out.rssShmemKb);
        else if (key == "voluntary_ctxt_switches:")
            tok.nextInt(out.volCtxsw);
        else if (key == "nonvoluntary_ctxt_switches:")
            tok.nextInt(out.nonvolCtxsw);
    }
    return any;
}

bool parsePidIo(std::string_view text, PidIo& out)
{
    LineReader lines(text);
    std::string_view line;
    bool any = false;
    while (lines.next(line)) {
        Tokens tok(line);
        std::string_view key;
        uint64_t v = 0;
        if (!tok.next(key) || !tok.nextInt(v))
            continue;
        any = true;
        if (key == "rchar:")
            out.rchar = v;
        else if (key == "wchar:")
            out.wchar = v;
        else if (key == "read_bytes:")
            out.readBytes = v;
        else if (key == "write_bytes:")
            out.writeBytes = v;
        else if (key == "cancelled_write_bytes:")
            out.cancelledWriteBytes = v;
    }
    return any;
}

std::string_view parseCgroupV2Path(std::string_view text)
{
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        if (startsWith(line, "0::"))
            return trim(line.substr(3));
    }
    return {};
}

std::string_view cgroupUnitName(std::string_view path)
{
    while (!path.empty() && path.back() == '/')
        path.remove_suffix(1);
    // Walk components from the leaf up; prefer the deepest .service/.scope.
    while (!path.empty()) {
        auto slash = path.rfind('/');
        std::string_view comp = slash == std::string_view::npos ? path : path.substr(slash + 1);
        if (endsWith(comp, ".service") || endsWith(comp, ".scope"))
            return comp;
        if (slash == std::string_view::npos)
            break;
        path = path.substr(0, slash);
    }
    return {};
}

} // namespace culprit
