// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// Internal interface between the RuleEngine and the individual diagnosis rules.

#include "core/Settings.h"
#include "core/model/Frame.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culprit {

struct Candidate {
    Finding finding;
    int raiseAfter = -1;   // consecutive evaluations before showing (-1 = settings default)
};

struct HitchNote {
    int64_t tNs = 0;
    double overshootMs = 0;
    HitchClass cls = HitchClass::Unknown;
    QString topSuspect;
    double periodSec = 0;
};

// State that rules keep between evaluations.
struct RuleState {
    std::unordered_map<ProcKey, int, ProcKeyHash> highCpuSecs;
    QHash<QString, int> unitHighCpuSecs;
    double runnableEwma = -1, blockedEwma = -1;
    QHash<QString, double> irqBaseline;          // per IRQ line, slow EWMA of rate
    QHash<QString, int> irqHotSecs;
    QHash<QString, double> softirqBaseline;      // "NET_RX/5" -> rate
    QHash<QString, int> diskBusySecs;
    QSet<QString> fansSeenSpinning;
    int reclaimSecs = 0;
    int runqSecs = 0;
    std::vector<int> cpuHotspotSecs;
    std::vector<int> irqCpuSecs;
    int evaluations = 0;
    std::deque<HitchNote> hitches;               // last few minutes of stutter events
};

struct RuleContext {
    const Frame& f;
    const Settings& settings;
    RuleState& st;
    std::vector<Candidate>& out;

    void report(Finding fi, int raiseAfter = -1) const { out.push_back({std::move(fi), raiseAfter}); }
};

// Helpers shared by rules
QString procName(const ProcSample& p);   // "firefox (4242)"
Suspect processSuspect(const ProcSample& p, double score, const QString& evidence);

struct ProcGroup {
    QString comm;
    int count = 0;
    double total = 0;
    const ProcSample* top = nullptr;
};
// Groups processes by name (e.g. 16 × openssl) and sums value(p); sorted descending.
std::vector<ProcGroup> groupByName(const std::vector<ProcSample>& procs, const std::function<double(const ProcSample&)>& value,
                                   double minValue);
QString groupLabel(const ProcGroup& g);   // "openssl ×16" or "firefox (4242)"

// Rule families
void cpuRules(const RuleContext& ctx);
void thermalRules(const RuleContext& ctx);
void memoryIoRules(const RuleContext& ctx);
void systemRules(const RuleContext& ctx);
void stutterRules(const RuleContext& ctx);

} // namespace culprit
