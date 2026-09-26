// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Parser for tracefs event "format" files, e.g.
//   /sys/kernel/tracing/events/sched/sched_switch/format
// so field offsets are taken from the running kernel instead of being hard-coded.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace culprit {

struct TraceField {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool isSigned = false;
    bool dataLoc = false;   // __data_loc: u32 with (len << 16 | offset)
};

struct TraceFormat {
    std::string name;
    uint64_t id = 0;
    std::vector<TraceField> fields;
    const TraceField* find(std::string_view field) const;
};

bool parseTraceFormat(std::string_view text, TraceFormat& out);

// Accessors for a raw tracepoint record (bounds-checked; 0/"" when out of range).
inline uint64_t traceU(const uint8_t* raw, uint32_t rawLen, const TraceField* f)
{
    if (!f || f->offset + f->size > rawLen)
        return 0;
    switch (f->size) {
    case 1: return raw[f->offset];
    case 2: { uint16_t v; std::memcpy(&v, raw + f->offset, 2); return v; }
    case 4: { uint32_t v; std::memcpy(&v, raw + f->offset, 4); return v; }
    case 8: { uint64_t v; std::memcpy(&v, raw + f->offset, 8); return v; }
    default: return 0;
    }
}

inline int64_t traceS(const uint8_t* raw, uint32_t rawLen, const TraceField* f)
{
    if (!f || f->offset + f->size > rawLen)
        return 0;
    switch (f->size) {
    case 1: return int8_t(raw[f->offset]);
    case 2: { int16_t v; std::memcpy(&v, raw + f->offset, 2); return v; }
    case 4: { int32_t v; std::memcpy(&v, raw + f->offset, 4); return v; }
    case 8: { int64_t v; std::memcpy(&v, raw + f->offset, 8); return v; }
    default: return 0;
    }
}

// char[N] or __data_loc char[] field as a string (stops at NUL).
std::string_view traceStr(const uint8_t* raw, uint32_t rawLen, const TraceField* f);

} // namespace culprit
