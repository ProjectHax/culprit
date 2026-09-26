// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "common/trace/TraceFormat.h"

#include "common/parse/Text.h"

namespace culprit {

const TraceField* TraceFormat::find(std::string_view field) const
{
    for (const TraceField& f : fields)
        if (f.name == field)
            return &f;
    return nullptr;
}

namespace {

// "offset:24" -> 24
bool keyValue(std::string_view part, std::string_view key, uint32_t& out)
{
    part = trim(part);
    if (!startsWith(part, key) || part.size() <= key.size() || part[key.size()] != ':')
        return false;
    return parseInt(part.substr(key.size() + 1), out);
}

} // namespace

bool parseTraceFormat(std::string_view text, TraceFormat& out)
{
    out.fields.clear();
    LineReader lines(text);
    std::string_view line;
    while (lines.next(line)) {
        std::string_view t = trim(line);
        if (startsWith(t, "name:")) {
            out.name = std::string(trim(t.substr(5)));
            continue;
        }
        if (startsWith(t, "ID:")) {
            parseInt(trim(t.substr(3)), out.id);
            continue;
        }
        if (!startsWith(t, "field:"))
            continue;
        // field:<decl>;\toffset:N;\tsize:N;\tsigned:N;
        std::vector<std::string_view> parts;
        size_t start = 0;
        for (size_t i = 0; i <= t.size(); ++i) {
            if (i == t.size() || t[i] == ';') {
                parts.push_back(t.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() < 3)
            continue;
        std::string_view decl = trim(parts[0].substr(6));   // after "field:"
        TraceField f;
        f.dataLoc = startsWith(decl, "__data_loc");
        // The field name is the last identifier: "char prev_comm[16]" -> prev_comm,
        // "__data_loc char[] name" -> name.
        std::string_view name = decl;
        if (!name.empty() && name.back() == ']')
            if (auto br = name.rfind('['); br != std::string_view::npos)
                name = trim(name.substr(0, br));
        if (auto sp = name.find_last_of(" \t*"); sp != std::string_view::npos)
            name = name.substr(sp + 1);
        f.name = std::string(name);
        uint32_t v = 0;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (keyValue(parts[i], "offset", v))
                f.offset = v;
            else if (keyValue(parts[i], "size", v))
                f.size = v;
            else if (keyValue(parts[i], "signed", v))
                f.isSigned = v != 0;
        }
        out.fields.push_back(std::move(f));
    }
    return out.id != 0 && !out.fields.empty();
}

std::string_view traceStr(const uint8_t* raw, uint32_t rawLen, const TraceField* f)
{
    if (!f)
        return {};
    uint32_t off = f->offset, len = f->size;
    if (f->dataLoc) {
        if (f->offset + 4 > rawLen)
            return {};
        uint32_t loc;
        std::memcpy(&loc, raw + f->offset, 4);
        off = loc & 0xffff;
        len = loc >> 16;
    }
    if (off >= rawLen)
        return {};
    if (off + len > rawLen)
        len = rawLen - off;
    const char* s = reinterpret_cast<const char*>(raw + off);
    size_t n = 0;
    while (n < len && s[n])
        ++n;
    return {s, n};
}

} // namespace culprit
