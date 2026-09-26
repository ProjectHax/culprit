// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace culprit {

// Root prefixes for procfs/sysfs so parsers and collectors can be pointed at
// captured fixture trees instead of the live system.
struct SysPaths {
    std::string proc = "/proc";
    std::string sys = "/sys";

    static SysPaths& instance();
    static std::string proc_(std::string_view rel);   // e.g. proc_("stat") -> "/proc/stat"
    static std::string sys_(std::string_view rel);
};

// Reads a whole (small, virtual) file in one go. Returns false if it can't be opened/read.
bool readFile(const std::string& path, std::string& out);
bool readInt64(const std::string& path, int64_t& out);
bool readFirstLine(const std::string& path, std::string& out);

// Keeps a procfs/sysfs file descriptor open and re-reads it with pread() from
// offset 0, which makes the kernel regenerate the contents. Much cheaper than
// open/read/close on every sample.
class CachedFile {
public:
    CachedFile() = default;
    explicit CachedFile(std::string path);
    ~CachedFile();
    CachedFile(const CachedFile&) = delete;
    CachedFile& operator=(const CachedFile&) = delete;
    CachedFile(CachedFile&& o) noexcept;
    CachedFile& operator=(CachedFile&& o) noexcept;

    void setPath(std::string path);
    const std::string& path() const { return path_; }

    // Returns false (and keeps returning false without retrying every call)
    // if the file doesn't exist or isn't readable.
    bool read(std::string& buf);
    // For single-record files (/proc/<pid>/* and sysfs attributes): one pread,
    // a short read is EOF. Saves the second "is that all?" syscall.
    bool readSingle(std::string& buf);
    bool readInt64(int64_t& out);
    bool exists();

private:
    bool ensureOpen();
    void close();

    std::string path_;
    int fd_ = -1;
    bool failed_ = false;
};

} // namespace culprit
