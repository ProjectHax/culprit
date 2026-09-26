// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Allocation-free helpers for tokenizing procfs/sysfs text.

#include <charconv>
#include <cstdint>
#include <string_view>

namespace culprit {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

template <typename T>
inline bool parseInt(std::string_view s, T& out)
{
    s = trim(s);
    if (!s.empty() && s.front() == '+')
        s.remove_prefix(1);
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

inline bool parseHex(std::string_view s, unsigned& out)
{
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return ec == std::errc() && p == s.data() + s.size();
}

inline bool parseDouble(std::string_view s, double& out)
{
    s = trim(s);
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p != s.data();
}

// Splits text into lines; call next() until it returns false.
class LineReader {
public:
    explicit LineReader(std::string_view text) : rest_(text) {}
    bool next(std::string_view& line)
    {
        if (rest_.empty())
            return false;
        auto nl = rest_.find('\n');
        if (nl == std::string_view::npos) {
            line = rest_;
            rest_ = {};
        } else {
            line = rest_.substr(0, nl);
            rest_.remove_prefix(nl + 1);
        }
        return true;
    }

private:
    std::string_view rest_;
};

// Whitespace tokenizer over one line.
class Tokens {
public:
    explicit Tokens(std::string_view line) : rest_(line) {}
    bool next(std::string_view& tok)
    {
        while (!rest_.empty() && isSpace(rest_.front()))
            rest_.remove_prefix(1);
        if (rest_.empty())
            return false;
        size_t i = 0;
        while (i < rest_.size() && !isSpace(rest_[i]))
            ++i;
        tok = rest_.substr(0, i);
        rest_.remove_prefix(i);
        return true;
    }
    template <typename T>
    bool nextInt(T& out)
    {
        std::string_view t;
        return next(t) && parseInt(t, out);
    }
    std::string_view rest() const { return trim(rest_); }

private:
    std::string_view rest_;
};

// "key: value" / "key value" helpers
inline bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix;
}

inline bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

} // namespace culprit
