// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Minimal JSON builder for the helper's line protocol. Every string is fully
// escaped — process names are attacker-controlled and must never be able to
// break out of their field or inject a protocol line. Bytes >= 0x80 are
// emitted as \u00XX so the output is always valid ASCII JSON.

#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>

namespace culprit {

inline void jsonEscape(std::string& o, std::string_view s)
{
    static const char hex[] = "0123456789abcdef";
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                o += "\\u00";
                o += hex[c >> 4];
                o += hex[c & 0xf];
            } else {
                o += char(c);
            }
        }
    }
    o += '"';
}

// Builds one JSON object; `line()` appends it plus '\n' to an output buffer.
class JsonObj {
public:
    JsonObj() { s_ += '{'; }
    explicit JsonObj(const char* type)
    {
        s_ += '{';
        str("t", type);
    }

    JsonObj& num(const char* k, int64_t v)
    {
        key(k);
        char b[32];
        std::snprintf(b, sizeof b, "%" PRId64, v);
        s_ += b;
        return *this;
    }
    JsonObj& unum(const char* k, uint64_t v)
    {
        key(k);
        char b[32];
        std::snprintf(b, sizeof b, "%" PRIu64, v);
        s_ += b;
        return *this;
    }
    JsonObj& dbl(const char* k, double v, int decimals = 1)
    {
        key(k);
        char b[48];
        std::snprintf(b, sizeof b, "%.*f", decimals, v);
        s_ += b;
        return *this;
    }
    JsonObj& str(const char* k, std::string_view v)
    {
        key(k);
        jsonEscape(s_, v);
        return *this;
    }
    // Pre-built JSON (an array or object produced by this builder).
    JsonObj& raw(const char* k, std::string_view json)
    {
        key(k);
        s_ += json;
        return *this;
    }

    std::string done()
    {
        s_ += '}';
        return std::move(s_);
    }
    void line(std::string& out)
    {
        out += done();
        out += '\n';
    }

private:
    void key(const char* k)
    {
        if (!first_)
            s_ += ',';
        first_ = false;
        jsonEscape(s_, k);
        s_ += ':';
    }
    std::string s_;
    bool first_ = true;
};

// "[a,b,c]" helper for arrays of pre-built elements.
class JsonArr {
public:
    JsonArr() { s_ += '['; }
    JsonArr& add(std::string_view json)
    {
        if (!first_)
            s_ += ',';
        first_ = false;
        s_ += json;
        return *this;
    }
    JsonArr& addNum(double v, int decimals = 0)
    {
        char b[48];
        std::snprintf(b, sizeof b, "%.*f", decimals, v);
        return add(b);
    }
    JsonArr& addStr(std::string_view v)
    {
        std::string t;
        jsonEscape(t, v);
        return add(t);
    }
    std::string done()
    {
        s_ += ']';
        return std::move(s_);
    }

private:
    std::string s_;
    bool first_ = true;
};

} // namespace culprit
