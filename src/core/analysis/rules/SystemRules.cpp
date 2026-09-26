// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

// Platform capability hints and micro-stutter pattern findings.

#include "core/analysis/rules/Rules.h"

#include "core/Format.h"

#include <algorithm>
#include <map>

namespace culprit {

void systemRules(const RuleContext& ctx)
{
    if (!ctx.f.sys.psi.available) {
        Finding fi;
        fi.id = QStringLiteral("psi-disabled");
        fi.rule = fi.id;
        fi.severity = Severity::Info;
        fi.title = QObject::tr("Pressure stall information (PSI) is disabled in this kernel");
        fi.detail = QObject::tr("PSI reports how much time tasks lose waiting for CPU, memory and I/O. Culprit works "
                                "without it, but enabling it adds precise pressure numbers.");
        fi.advice = QObject::tr("Add psi=1 to the kernel command line: `sudo grubby --update-kernel=ALL --args=psi=1`, then reboot.");
        ctx.report(fi, 1);
    } else {
        const PsiSample& p = ctx.f.sys.psi;
        struct R {
            const char* what;
            double v;
        } res[] = {{"CPU", p.cpuSome.avg10}, {"memory", p.memFull.avg10}, {"I/O", p.ioFull.avg10}};
        for (const R& r : res) {
            if (r.v < 10)
                continue;
            Finding fi;
            fi.id = QStringLiteral("psi:%1").arg(QLatin1String(r.what));
            fi.rule = QStringLiteral("psi");
            fi.severity = r.v >= 40 ? Severity::Critical : Severity::Warning;
            fi.title = QObject::tr("%1 pressure: tasks stalled %2 of the last 10 s").arg(QLatin1String(r.what), fmt::percent(r.v, 0));
            ctx.report(fi, 2);
        }
    }
}

void stutterRules(const RuleContext& ctx)
{
    auto& hs = ctx.st.hitches;
    const int64_t now = ctx.f.tNs;
    while (!hs.empty() && now - hs.front().tNs > 300'000'000'000LL)
        hs.pop_front();

    // Frequent hitches in the last minute.
    std::map<int, int> byClass;
    std::map<QString, int> bySuspect;
    int lastMinute = 0;
    double worst = 0;
    for (const HitchNote& h : hs) {
        if (now - h.tNs > 60'000'000'000LL)
            continue;
        ++lastMinute;
        byClass[int(h.cls)]++;
        if (!h.topSuspect.isEmpty())
            bySuspect[h.topSuspect]++;
        worst = std::max(worst, h.overshootMs);
    }
    if (lastMinute >= 5) {
        auto cls = std::max_element(byClass.begin(), byClass.end(), [](auto& a, auto& b) { return a.second < b.second; });
        Finding fi;
        fi.id = QStringLiteral("stutter-frequent");
        fi.rule = fi.id;
        fi.severity = lastMinute >= 20 || worst >= 16 ? Severity::Warning : Severity::Info;
        fi.title = QObject::tr("%1 scheduling hitches in the last minute (worst %2), mostly: %3")
                       .arg(lastMinute)
                       .arg(fmt::ms(worst), hitchClassName(HitchClass(cls->first)));
        fi.detail = hitchClassDescription(HitchClass(cls->first));
        std::vector<std::pair<int, QString>> sus;
        for (auto& [name, n] : bySuspect)
            sus.push_back({n, name});
        std::sort(sus.rbegin(), sus.rend());
        for (size_t i = 0; i < sus.size() && i < 4; ++i)
            fi.suspects.push_back({QStringLiteral("suspect"), sus[i].second, 0, double(sus[i].first) / lastMinute,
                                   QObject::tr("top suspect in %1 hitch(es)").arg(sus[i].first)});
        fi.advice = QObject::tr("Open the Stutter tab for per-hitch evidence; enable Deep trace to see exactly which "
                                "task or interrupt held the CPU.");
        ctx.report(fi, 1);
    }

    // Periodic hitches: something polling on a timer.
    if (!hs.empty() && hs.back().periodSec > 0 && now - hs.back().tNs < 30'000'000'000LL) {
        const HitchNote& h = hs.back();
        Finding fi;
        fi.id = QStringLiteral("stutter-periodic");
        fi.rule = fi.id;
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("Periodic stutter every %1 s (%2)").arg(h.periodSec, 0, 'f', 2).arg(hitchClassName(h.cls));
        fi.detail = QObject::tr("Hitches repeat on a fixed period — typically a daemon or driver polling on a timer "
                                "(sensor/RGB/fan tools reading a slow SMBus/Super-I/O chip, a GPU driver query, "
                                "a monitoring agent).");
        if (!h.topSuspect.isEmpty())
            fi.suspects.push_back({QStringLiteral("suspect"), h.topSuspect, 0, 0.8, QObject::tr("most recent hitch")});
        fi.advice = QObject::tr("Look for tools that poll hardware on that interval (e.g. `systemctl list-timers`, "
                                "sensor widgets, openrgb, liquidctl, nvidia-smi loops).");
        ctx.report(fi, 1);
    }
}

} // namespace culprit
