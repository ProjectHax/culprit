// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/analysis/RuleEngine.h"

#include "core/Format.h"
#include "core/analysis/rules/Rules.h"

#include <algorithm>
#include <map>

namespace culprit {

// ------------------------------------------------------------------ rule helpers

QString procName(const ProcSample& p) { return fmt::procLabel(p.comm, p.key.pid); }

Suspect processSuspect(const ProcSample& p, double score, const QString& evidence)
{
    return {QStringLiteral("process"), procName(p), p.key.pid, score, evidence};
}

// Kernel worker threads get unique names ("kworker/u129:15-kcryptd-253:0-1",
// "ksoftirqd/5"); group them by what they are doing instead.
QString groupName(const ProcSample& p)
{
    if (!p.kernelThread)
        return p.comm;
    const QString& c = p.comm;
    if (c.startsWith(QLatin1String("kworker/"))) {
        const qsizetype colon = c.indexOf(QLatin1Char(':'));
        qsizetype i = colon < 0 ? -1 : colon + 1;
        while (i > 0 && i < c.size() && c[i].isDigit())
            ++i;
        if (i > 0 && i < c.size() && (c[i] == QLatin1Char('+') || c[i] == QLatin1Char('-'))) {
            QString wq = c.mid(i + 1);
            // drop a trailing instance number ("kcryptd-253:0-1" -> "kcryptd-253:0")
            const qsizetype dash = wq.lastIndexOf(QLatin1Char('-'));
            if (dash > 0 && dash + 1 < wq.size() && wq.mid(dash + 1).toInt() > 0)
                wq.truncate(dash);
            return QStringLiteral("kworker (%1)").arg(wq);
        }
        return QStringLiteral("kworker");
    }
    const qsizetype slash = c.lastIndexOf(QLatin1Char('/'));
    if (slash > 0) {
        bool num = false;
        c.mid(slash + 1).toInt(&num);
        if (num)
            return c.left(slash);   // ksoftirqd/5 -> ksoftirqd
    }
    return c;
}

std::vector<ProcGroup> groupByName(const std::vector<ProcSample>& procs, const std::function<double(const ProcSample&)>& value,
                                   double minValue)
{
    std::map<QString, ProcGroup> m;
    for (const ProcSample& p : procs) {
        const double v = value(p);
        if (v <= 0)
            continue;
        const QString name = groupName(p);
        ProcGroup& g = m[name];
        g.comm = name;
        g.count++;
        g.total += v;
        if (!g.top || v > value(*g.top))
            g.top = &p;
    }
    std::vector<ProcGroup> out;
    for (auto& [k, g] : m)
        if (g.total >= minValue)
            out.push_back(g);
    std::sort(out.begin(), out.end(), [](const ProcGroup& a, const ProcGroup& b) { return a.total > b.total; });
    return out;
}

QString groupLabel(const ProcGroup& g)
{
    if (g.count == 1)
        return procName(*g.top);
    return QStringLiteral("%1 ×%2").arg(g.comm).arg(g.count);
}

// ------------------------------------------------------------------ engine

RuleEngine::RuleEngine(const Settings& settings) : settings_(settings), state_(std::make_unique<RuleState>()) {}

RuleEngine::~RuleEngine() = default;

void RuleEngine::noteHitch(const Hitch& h)
{
    HitchNote n;
    n.tNs = h.t0Ns;
    n.overshootMs = h.maxOvershootMs;
    n.cls = h.cls;
    n.periodSec = h.periodSec;
    // Only well-supported suspects count, grouped by name ("sh x24" and "sh (123)" -> "sh").
    if (!h.suspects.empty() && h.suspects.front().score >= 0.3) {
        QString name = h.suspects.front().label;
        for (const QString& sep : {QStringLiteral(" \u00d7"), QStringLiteral(" (")})
            if (const qsizetype i = name.indexOf(sep); i > 0)
                name.truncate(i);
        n.topSuspect = name;
    }
    state_->hitches.push_back(n);
}

void RuleEngine::evaluate(Frame& frame)
{
    raised_.clear();
    cleared_.clear();
    std::vector<Candidate> candidates;
    RuleContext ctx{frame, settings_, *state_, candidates};
    cpuRules(ctx);
    thermalRules(ctx);
    memoryIoRules(ctx);
    systemRules(ctx);
    stutterRules(ctx);
    state_->evaluations++;

    QSet<QString> present;
    for (Candidate& c : candidates) {
        const QString id = c.finding.id;
        present.insert(id);
        Tracked& t = tracked_[id];
        if (t.clearedNs)   // came back after clearing: a new episode
            t = Tracked{};
        const bool newEpisode = t.hits == 0 && !t.shown;
        const int64_t first = newEpisode ? frame.tNs : t.finding.firstSeenNs;
        const qint64 firstWall = newEpisode ? frame.wallMs : t.finding.firstSeenWallMs;
        t.finding = std::move(c.finding);
        t.finding.firstSeenNs = first;
        t.finding.firstSeenWallMs = firstWall;
        t.finding.lastSeenNs = frame.tNs;
        t.finding.lastSeenWallMs = frame.wallMs;
        t.finding.active = true;
        t.hits++;
        t.misses = 0;
        const int need = c.raiseAfter > 0 ? c.raiseAfter : settings_.findingRaiseAfter;
        if (!t.shown && t.hits >= need) {
            t.shown = true;
            raised_.push_back(t.finding);
        }
    }

    for (auto it = tracked_.begin(); it != tracked_.end();) {
        Tracked& t = it.value();
        if (!present.contains(it.key())) {
            if (t.clearedNs == 0) {
                t.misses++;
                t.hits = 0;
                if (!t.shown) {
                    it = tracked_.erase(it);
                    continue;
                }
                if (t.misses >= settings_.findingClearAfter) {
                    t.clearedNs = frame.tNs;
                    t.finding.active = false;
                    cleared_.push_back(t.finding);
                }
            } else if (frame.tNs - t.clearedNs > 600'000'000'000LL) {   // keep cleared ones 10 min
                it = tracked_.erase(it);
                continue;
            }
        }
        ++it;
    }

    // Publish: active first (by severity, then age), then recently cleared.
    frame.findings.clear();
    for (const Tracked& t : tracked_)
        if (t.shown)
            frame.findings.push_back(t.finding);
    std::sort(frame.findings.begin(), frame.findings.end(), [](const Finding& a, const Finding& b) {
        if (a.active != b.active)
            return a.active;
        if (a.severity != b.severity)
            return a.severity > b.severity;
        return a.firstSeenNs < b.firstSeenNs;
    });
    if (frame.findings.size() > 60)
        frame.findings.resize(60);
}

} // namespace culprit
