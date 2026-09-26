// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "common/fs/File.h"

#include "common/parse/Text.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace culprit {

SysPaths& SysPaths::instance()
{
    static SysPaths paths;
    return paths;
}

std::string SysPaths::proc_(std::string_view rel)
{
    std::string p = instance().proc;
    p += '/';
    p += rel;
    return p;
}

std::string SysPaths::sys_(std::string_view rel)
{
    std::string p = instance().sys;
    p += '/';
    p += rel;
    return p;
}

static bool preadAll(int fd, std::string& out)
{
    // Grow in chunks instead of resizing to the full capacity: resize() zero-fills,
    // and a shared buffer that once held a large file would otherwise be cleared
    // in full before every small read.
    size_t used = 0;
    size_t chunk = 4096;
    for (;;) {
        if (out.size() < used + chunk)
            out.resize(used + chunk);
        ssize_t n = ::pread(fd, out.data() + used, chunk, off_t(used));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            out.clear();
            return false;
        }
        if (n == 0)
            break;
        used += size_t(n);
        if (size_t(n) == chunk && chunk < (1u << 20))
            chunk *= 2;
    }
    out.resize(used);
    return true;
}

bool readFile(const std::string& path, std::string& out)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out.clear();
        return false;
    }
    bool ok = preadAll(fd, out);
    ::close(fd);
    return ok;
}

bool readInt64(const std::string& path, int64_t& out)
{
    std::string buf;
    if (!readFile(path, buf))
        return false;
    return parseInt(trim(buf), out);
}

bool readFirstLine(const std::string& path, std::string& out)
{
    if (!readFile(path, out))
        return false;
    auto nl = out.find('\n');
    if (nl != std::string::npos)
        out.resize(nl);
    return true;
}

CachedFile::CachedFile(std::string path) : path_(std::move(path)) {}

CachedFile::~CachedFile() { close(); }

CachedFile::CachedFile(CachedFile&& o) noexcept
    : path_(std::move(o.path_)), fd_(o.fd_), failed_(o.failed_)
{
    o.fd_ = -1;
}

CachedFile& CachedFile::operator=(CachedFile&& o) noexcept
{
    if (this != &o) {
        close();
        path_ = std::move(o.path_);
        fd_ = o.fd_;
        failed_ = o.failed_;
        o.fd_ = -1;
    }
    return *this;
}

void CachedFile::setPath(std::string path)
{
    close();
    path_ = std::move(path);
    failed_ = false;
}

void CachedFile::close()
{
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

bool CachedFile::ensureOpen()
{
    if (fd_ >= 0)
        return true;
    if (failed_ || path_.empty())
        return false;
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0)
        failed_ = true;
    return fd_ >= 0;
}

bool CachedFile::exists() { return ensureOpen(); }

bool CachedFile::read(std::string& buf)
{
    if (!ensureOpen()) {
        buf.clear();
        return false;
    }
    if (!preadAll(fd_, buf)) {
        // e.g. the process behind a /proc/<pid> file exited.
        close();
        failed_ = true;
        return false;
    }
    return true;
}

bool CachedFile::readSingle(std::string& buf)
{
    if (!ensureOpen()) {
        buf.clear();
        return false;
    }
    constexpr size_t kChunk = 8192;
    if (buf.size() < kChunk)
        buf.resize(kChunk);
    ssize_t n;
    do {
        n = ::pread(fd_, buf.data(), kChunk, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        close();
        failed_ = true;
        buf.clear();
        return false;
    }
    if (size_t(n) == kChunk)   // didn't fit: fall back to the general path
        return read(buf);
    buf.resize(size_t(n));
    return true;
}

bool CachedFile::readInt64(int64_t& out)
{
    char tmp[64];
    if (!ensureOpen())
        return false;
    ssize_t n;
    do {
        n = ::pread(fd_, tmp, sizeof tmp - 1, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return false;
    return parseInt(trim(std::string_view(tmp, size_t(n))), out);
}

} // namespace culprit
