// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/GpuCollector.h"

#include "common/util/Clock.h"

#include <algorithm>
#include <dlfcn.h>

namespace culprit {

namespace {

// Minimal NVML ABI (stable across driver versions).
using nvmlReturn_t = int;
using nvmlDevice_t = void*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr nvmlReturn_t NVML_ERROR_INSUFFICIENT_SIZE = 7;

struct nvmlUtilization_t {
    unsigned gpu, memory;
};
struct nvmlMemory_t {
    unsigned long long total, free, used;
};
struct nvmlProcessUtilizationSample_t {
    unsigned pid;
    unsigned long long timeStamp;
    unsigned smUtil, memUtil, encUtil, decUtil;
};
struct nvmlProcessInfo_t {   // _v2/_v3 layout
    unsigned pid;
    unsigned long long usedGpuMemory;
    unsigned gpuInstanceId, computeInstanceId;
};

enum { NVML_TEMPERATURE_GPU = 0 };
enum { NVML_TEMP_THRESHOLD_SHUTDOWN = 0, NVML_TEMP_THRESHOLD_SLOWDOWN = 1, NVML_TEMP_THRESHOLD_GPU_MAX = 3 };
enum { NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_SM = 1, NVML_CLOCK_MEM = 2 };

} // namespace

struct Nvml::Api {
    void* lib = nullptr;
    nvmlReturn_t (*init)() = nullptr;
    nvmlReturn_t (*getCount)(unsigned*) = nullptr;
    nvmlReturn_t (*getHandle)(unsigned, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*getName)(nvmlDevice_t, char*, unsigned) = nullptr;
    nvmlReturn_t (*getTemp)(nvmlDevice_t, int, unsigned*) = nullptr;
    nvmlReturn_t (*getTempThreshold)(nvmlDevice_t, int, unsigned*) = nullptr;
    nvmlReturn_t (*getPower)(nvmlDevice_t, unsigned*) = nullptr;
    nvmlReturn_t (*getPowerLimit)(nvmlDevice_t, unsigned*) = nullptr;
    nvmlReturn_t (*getUtil)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (*getMemory)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
    nvmlReturn_t (*getClock)(nvmlDevice_t, int, unsigned*) = nullptr;
    nvmlReturn_t (*getMaxClock)(nvmlDevice_t, int, unsigned*) = nullptr;
    nvmlReturn_t (*getFan)(nvmlDevice_t, unsigned*) = nullptr;
    nvmlReturn_t (*getReasons)(nvmlDevice_t, unsigned long long*) = nullptr;
    nvmlReturn_t (*getProcUtil)(nvmlDevice_t, nvmlProcessUtilizationSample_t*, unsigned*, unsigned long long) = nullptr;
    nvmlReturn_t (*getGraphicsProcs)(nvmlDevice_t, unsigned*, nvmlProcessInfo_t*) = nullptr;
    nvmlReturn_t (*getComputeProcs)(nvmlDevice_t, unsigned*, nvmlProcessInfo_t*) = nullptr;
};

Nvml& Nvml::instance()
{
    static Nvml nvml;
    return nvml;
}

Nvml::Nvml()
{
    void* lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return;
    api_ = new Api;
    api_->lib = lib;
    auto sym = [lib](const char* a, const char* b = nullptr) -> void* {
        void* p = dlsym(lib, a);
        if (!p && b)
            p = dlsym(lib, b);
        return p;
    };
    auto bind = [&](auto& fn, const char* a, const char* b = nullptr) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(a, b)); };
    bind(api_->init, "nvmlInit_v2", "nvmlInit");
    bind(api_->getCount, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    bind(api_->getHandle, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    bind(api_->getName, "nvmlDeviceGetName");
    bind(api_->getTemp, "nvmlDeviceGetTemperature");
    bind(api_->getTempThreshold, "nvmlDeviceGetTemperatureThreshold");
    bind(api_->getPower, "nvmlDeviceGetPowerUsage");
    bind(api_->getPowerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    bind(api_->getUtil, "nvmlDeviceGetUtilizationRates");
    bind(api_->getMemory, "nvmlDeviceGetMemoryInfo");
    bind(api_->getClock, "nvmlDeviceGetClockInfo");
    bind(api_->getMaxClock, "nvmlDeviceGetMaxClockInfo");
    bind(api_->getFan, "nvmlDeviceGetFanSpeed");
    bind(api_->getReasons, "nvmlDeviceGetCurrentClocksEventReasons", "nvmlDeviceGetCurrentClocksThrottleReasons");
    bind(api_->getProcUtil, "nvmlDeviceGetProcessUtilization");
    bind(api_->getGraphicsProcs, "nvmlDeviceGetGraphicsRunningProcesses_v3", "nvmlDeviceGetGraphicsRunningProcesses_v2");
    bind(api_->getComputeProcs, "nvmlDeviceGetComputeRunningProcesses_v3", "nvmlDeviceGetComputeRunningProcesses_v2");

    if (!api_->init || !api_->getCount || !api_->getHandle || api_->init() != NVML_SUCCESS)
        return;
    unsigned count = 0;
    if (api_->getCount(&count) != NVML_SUCCESS)
        return;
    for (unsigned i = 0; i < count; ++i) {
        nvmlDevice_t dev = nullptr;
        if (api_->getHandle(i, &dev) == NVML_SUCCESS)
            devices_.push_back(dev);
    }
    lastSeenTs_.assign(devices_.size(), 0);
    static_.resize(devices_.size());
    ok_ = !devices_.empty();
}

void Nvml::sample(int index, GpuSample& g)
{
    if (!ok_ || index < 0 || index >= int(devices_.size()))
        return;
    std::lock_guard lock(mutex_);
    nvmlDevice_t d = devices_[size_t(index)];
    g.index = index;
    unsigned v = 0;
    Static& st = static_[size_t(index)];
    if (!st.loaded) {
        char name[96] = {};
        if (api_->getName && api_->getName(d, name, sizeof name) == NVML_SUCCESS)
            st.name = QString::fromUtf8(name);
        if (api_->getTempThreshold) {
            if (api_->getTempThreshold(d, NVML_TEMP_THRESHOLD_SLOWDOWN, &v) == NVML_SUCCESS)
                st.slowdownC = v;
            if (api_->getTempThreshold(d, NVML_TEMP_THRESHOLD_GPU_MAX, &v) == NVML_SUCCESS)
                st.maxOperatingC = v;
            if (api_->getTempThreshold(d, NVML_TEMP_THRESHOLD_SHUTDOWN, &v) == NVML_SUCCESS)
                st.shutdownC = v;
        }
        if (api_->getMaxClock && api_->getMaxClock(d, NVML_CLOCK_SM, &v) == NVML_SUCCESS)
            st.clockSmMaxMHz = int(v);
        st.loaded = true;
    }
    const int64_t now = monoNs();
    if (now - st.slowRefreshNs > 5'000'000'000LL) {
        if (api_->getPowerLimit && api_->getPowerLimit(d, &v) == NVML_SUCCESS)
            st.powerLimitW = v / 1000.0;
        if (api_->getFan && api_->getFan(d, &v) == NVML_SUCCESS)
            st.fanPct = int(v);
        st.slowRefreshNs = now;
    }
    g.name = st.name;
    g.slowdownC = st.slowdownC;
    g.maxOperatingC = st.maxOperatingC;
    g.shutdownC = st.shutdownC;
    g.clockSmMaxMHz = st.clockSmMaxMHz;
    g.powerLimitW = st.powerLimitW;
    g.fanPct = st.fanPct;
    if (api_->getTemp && api_->getTemp(d, NVML_TEMPERATURE_GPU, &v) == NVML_SUCCESS)
        g.tempC = v;
    if (api_->getPower && api_->getPower(d, &v) == NVML_SUCCESS)
        g.powerW = v / 1000.0;
    nvmlUtilization_t u{};
    if (api_->getUtil && api_->getUtil(d, &u) == NVML_SUCCESS) {
        g.utilGpu = int(u.gpu);
        g.utilMem = int(u.memory);
    }
    nvmlMemory_t m{};
    if (api_->getMemory && api_->getMemory(d, &m) == NVML_SUCCESS) {
        g.memUsed = m.used;
        g.memTotal = m.total;
    }
    if (api_->getClock) {
        if (api_->getClock(d, NVML_CLOCK_SM, &v) == NVML_SUCCESS)
            g.clockSmMHz = int(v);
        if (api_->getClock(d, NVML_CLOCK_MEM, &v) == NVML_SUCCESS)
            g.clockMemMHz = int(v);
    }
    unsigned long long reasons = 0;
    if (api_->getReasons && api_->getReasons(d, &reasons) == NVML_SUCCESS) {
        g.eventReasons = reasons;
        g.reasonsValid = true;
    }

    // Memory per process (graphics + compute contexts).
    std::unordered_map<int, uint64_t> mem;
    for (auto fn : {api_->getGraphicsProcs, api_->getComputeProcs}) {
        if (!fn)
            continue;
        nvmlProcessInfo_t infos[128];
        unsigned n = 128;
        if (fn(d, &n, infos) == NVML_SUCCESS)
            for (unsigned i = 0; i < n; ++i)
                mem[int(infos[i].pid)] += infos[i].usedGpuMemory == ~0ULL ? 0 : infos[i].usedGpuMemory;
    }
    for (auto [pid, bytes] : mem) {
        GpuProc p;
        p.pid = pid;
        p.usedMemBytes = bytes;
        g.procs.push_back(p);
    }
}

bool Nvml::eventReasons(int index, uint64_t& reasons)
{
    if (!ok_ || index < 0 || index >= int(devices_.size()) || !api_->getReasons)
        return false;
    std::lock_guard lock(mutex_);
    nvmlDevice_t d = devices_[size_t(index)];
    unsigned long long r = 0;
    if (api_->getReasons(d, &r) != NVML_SUCCESS)
        return false;
    reasons = r;
    return true;
}

void Nvml::processUtilization(int index, std::unordered_map<int, GpuProc>& out)
{
    if (!ok_ || index < 0 || index >= int(devices_.size()) || !api_->getProcUtil)
        return;
    std::lock_guard lock(mutex_);
    nvmlDevice_t d = devices_[size_t(index)];
    // The first call reports buffer capacity rather than a sample count, so
    // just pass a generous buffer and ask for samples newer than the last seen.
    nvmlProcessUtilizationSample_t samples[256];
    unsigned n = 256;
    const nvmlReturn_t rc = api_->getProcUtil(d, samples, &n, lastSeenTs_[size_t(index)]);
    if (rc != NVML_SUCCESS && rc != NVML_ERROR_INSUFFICIENT_SIZE)
        return;   // NOT_FOUND: no new samples (nothing used the GPU)
    unsigned long long newest = lastSeenTs_[size_t(index)];
    for (unsigned i = 0; i < n && i < 256; ++i) {
        const auto& s = samples[i];
        if (s.timeStamp <= lastSeenTs_[size_t(index)] || s.pid == 0)
            continue;
        GpuProc& p = out[int(s.pid)];
        p.pid = int(s.pid);
        p.smUtil = std::max(p.smUtil, s.smUtil);
        p.memUtil = std::max(p.memUtil, s.memUtil);
        p.encUtil = std::max(p.encUtil, s.encUtil);
        p.decUtil = std::max(p.decUtil, s.decUtil);
        newest = std::max(newest, s.timeStamp);
    }
    lastSeenTs_[size_t(index)] = newest;
}

// ------------------------------------------------------------------ collector

void GpuCollector::sample(Frame& frame)
{
    Nvml& nvml = Nvml::instance();
    if (!nvml.ok())
        return;
    const int n = nvml.deviceCount();
    idleBaselineW_.resize(size_t(n), 1e9);
    frame.gpus.resize(size_t(n));

    std::unordered_map<int, ProcSample*> byPid;
    for (ProcSample& p : frame.procs)
        byPid[p.key.pid] = &p;

    for (int i = 0; i < n; ++i) {
        GpuSample& g = frame.gpus[size_t(i)];
        nvml.sample(i, g);
        std::unordered_map<int, GpuProc> util;
        nvml.processUtilization(i, util);
        // Merge utilisation into the memory list.
        for (GpuProc& p : g.procs) {
            if (auto it = util.find(p.pid); it != util.end()) {
                p.smUtil = it->second.smUtil;
                p.memUtil = it->second.memUtil;
                p.encUtil = it->second.encUtil;
                p.decUtil = it->second.decUtil;
                util.erase(it);
            }
        }
        for (auto& [pid, p] : util)
            g.procs.push_back(p);

        // Power attribution: only the part above the idle floor is "caused" by apps.
        if (g.powerW > 0)
            idleBaselineW_[size_t(i)] = std::min(idleBaselineW_[size_t(i)], g.powerW);
        const double dynamicW = g.powerW > 0 ? std::max(0.0, g.powerW - idleBaselineW_[size_t(i)]) : 0;
        double totalSm = 0;
        for (const GpuProc& p : g.procs)
            totalSm += p.smUtil;
        for (const GpuProc& p : g.procs) {
            auto it = byPid.find(p.pid);
            if (it == byPid.end())
                continue;
            ProcSample& s = *it->second;
            s.gpuPct = std::max(0.0, s.gpuPct) + p.smUtil;
            s.gpuMemBytes += p.usedMemBytes;
            if (totalSm > 0)
                s.estGpuW = std::max(0.0, s.estGpuW) + dynamicW * p.smUtil / totalSm;
        }
        if (i == 0)
            frame.thermal.gpuTempC = g.tempC >= 0 ? g.tempC : kNoTemp;
    }
}

} // namespace culprit
