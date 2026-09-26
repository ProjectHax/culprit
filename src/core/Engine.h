// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"
#include "core/model/Frame.h"

#include <QObject>
#include <QThread>

#include <QJsonObject>
#include <QList>

#include <memory>
#include <unordered_map>
#include <unordered_set>

class QTimer;

namespace culprit {

class SystemCollector;
class CpufreqCollector;
class ProcessCollector;
class HwmonCollector;
class PowerCollector;
class GpuCollector;
class CgroupCollector;
class RuleEngine;
class FlightRecorder;
class HitchAnalyzer;
class LatencyFeed;
class HelperClient;
struct HitchCapture;

// The sampling engine. Lives in its own thread; every interval it runs all
// collectors, builds an immutable Frame and publishes it.
class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(const Settings& settings);
    ~Engine() override;

    // Thread-safe entry points (queued into the engine thread).
    void applySettings(const Settings& s);
    void setPaused(bool paused);
    void setDetailPids(const QList<int>& pids);   // processes the UI shows per-thread rows for
    void setFocusPid(int pid);                    // 0 = none
    void setSensorsWatched(bool watched);         // poll slow sensor chips more often
    void setDeepTrace(bool on);                   // start/stop the root helper

    // Live probe latency series, readable from any thread.
    std::shared_ptr<LatencyFeed> latencyFeed() const { return feed_; }

signals:
    void frameReady(culprit::FramePtr frame);
    void hitchDetected(const culprit::Hitch& hitch);
    void hitchUpdated(const culprit::Hitch& hitch);   // deep-trace blockers arrived

public slots:
    void start();
    void stop();

private:
    void tick();
    void sampleSelf(SelfStats& out, int64_t nowNs);

    Settings settings_;
    QTimer* timer_ = nullptr;
    bool paused_ = false;
    uint64_t seq_ = 0;

    std::unique_ptr<SystemCollector> system_;
    std::unique_ptr<CpufreqCollector> cpufreq_;
    std::unique_ptr<ProcessCollector> procs_;
    std::unique_ptr<HwmonCollector> hwmon_;
    std::unique_ptr<PowerCollector> power_;
    std::unique_ptr<GpuCollector> gpu_;
    std::unique_ptr<CgroupCollector> cgroups_;
    std::unique_ptr<RuleEngine> rules_;

    void startProbes();
    void onCapture(const std::shared_ptr<HitchCapture>& cap);
    std::shared_ptr<LatencyFeed> feed_;
    std::unique_ptr<FlightRecorder> flight_;
    std::unique_ptr<HitchAnalyzer> analyzer_;
    FramePtr lastFrame_;
    QString probeError_;

    void onHelperEvent(const QJsonObject& ev);
    void mergeHelperData(Frame& f);
    HelperClient* helper_ = nullptr;
    std::unordered_map<quint64, Hitch> awaitingWindow_;
    std::vector<DeepEvent> deepEvents_;
    DeepSummary deepLatest_;
    std::vector<DStateThread> helperDState_;
    int64_t helperDStateNs_ = 0;
    std::unordered_map<int, std::pair<double, double>> helperIo_;
    int64_t helperIoNs_ = 0;
    HelperStatus helperStatus_;
    std::unordered_set<int> detailPids_;
    int focusPid_ = 0;

    uint64_t lastSelfRunNs_ = 0;
    int64_t lastSelfNs_ = 0;
};

// Owns the engine thread. Create in the GUI (or recorder) thread.
class EngineHost : public QObject {
    Q_OBJECT
public:
    explicit EngineHost(const Settings& settings, QObject* parent = nullptr);
    ~EngineHost() override;

    Engine* engine() const { return engine_; }
    void start();
    void shutdown();

private:
    QThread thread_;
    Engine* engine_ = nullptr;
    bool running_ = false;
};

} // namespace culprit
