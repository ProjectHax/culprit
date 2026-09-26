// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/HwmonCollector.h"

#include "common/parse/Text.h"
#include "common/util/Clock.h"

#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace culprit {

namespace {

// Drivers that talk to Super-I/O chips, embedded controllers or SMM firmware.
bool knownSlowDriver(const std::string& name)
{
    static const char* prefixes[] = {"nct6", "nct7", "it87", "it86", "w83", "f718", "f7188", "sch5",
                                     "dell_smm", "asus_wmi", "asus-ec", "asusec", "thinkpad", "applesmc",
                                     "hp_wmi", "gigabyte_wmi", "corsair", "nzxt"};
    for (const char* p : prefixes)
        if (startsWith(name, p))
            return true;
    return false;
}

bool fileExists(const std::string& p)
{
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

constexpr int64_t kSlowReadNs = 5'000'000;   // demote chips whose read takes longer
constexpr int kSlowPeriodSec = 30;
constexpr int kWatchedPeriodSec = 5;

} // namespace

HwmonCollector::HwmonCollector()
{
    enumerate();
    autoTjmax_ = detectTjmax();
    bool anySlow = false;
    for (auto& c : chips_)
        anySlow |= c->slow;
    if (anySlow)
        slowThread_ = std::thread([this] { slowLoop(); });
}

HwmonCollector::~HwmonCollector()
{
    stop_ = true;
    cv_.notify_all();
    if (slowThread_.joinable())
        slowThread_.join();
}

void HwmonCollector::enumerate()
{
    const std::string root = SysPaths::sys_("class/hwmon");
    DIR* d = opendir(root.c_str());
    if (!d)
        return;
    std::vector<std::string> dirs;
    while (dirent* e = readdir(d)) {
        if (startsWith(e->d_name, "hwmon"))
            dirs.push_back(root + "/" + e->d_name);
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());

    for (const std::string& dir : dirs) {
        auto chip = std::make_unique<Chip>();
        chip->dir = dir;
        std::string name;
        if (!readFirstLine(dir + "/name", name))
            continue;
        chip->name = QString::fromStdString(name);
        chip->slow = knownSlowDriver(name);

        DIR* cd = opendir(dir.c_str());
        if (!cd)
            continue;
        std::vector<std::string> inputs;
        while (dirent* e = readdir(cd)) {
            std::string_view f = e->d_name;
            if (endsWith(f, "_input") || (startsWith(f, "power") && endsWith(f, "_average")))
                inputs.emplace_back(f);
        }
        closedir(cd);
        std::sort(inputs.begin(), inputs.end(), [](const std::string& a, const std::string& b) {
            // temp2 before temp10
            auto num = [](const std::string& s) {
                size_t i = 0;
                while (i < s.size() && !isdigit(static_cast<unsigned char>(s[i]))) ++i;
                return std::make_pair(s.substr(0, i), atoi(s.c_str() + i));
            };
            return num(a) < num(b);
        });

        for (const std::string& in : inputs) {
            Sensor s;
            const std::string base = in.substr(0, in.rfind('_'));   // "temp1"
            if (startsWith(base, "temp")) {
                s.kind = SensorKind::Temp;
                s.scale = 1000.0;
            } else if (startsWith(base, "fan")) {
                s.kind = SensorKind::Fan;
                s.scale = 1.0;
            } else if (startsWith(base, "in")) {
                s.kind = SensorKind::Voltage;
                s.scale = 1000.0;
            } else if (startsWith(base, "power")) {
                s.kind = SensorKind::Power;
                s.scale = 1e6;
            } else if (startsWith(base, "curr")) {
                s.kind = SensorKind::Current;
                s.scale = 1000.0;
            } else {
                continue;
            }
            if (s.kind == SensorKind::Power && endsWith(in, "_input") && fileExists(dir + "/" + base + "_average"))
                continue;   // prefer the averaged power reading
            std::string label;
            s.label = readFirstLine(dir + "/" + base + "_label", label) && !label.empty() ? QString::fromStdString(label)
                                                                                          : QString::fromStdString(base);
            s.key = chip->name + QLatin1Char('/') + QString::fromStdString(base);
            int64_t v = 0;
            if (readInt64(dir + "/" + base + "_crit", v) && v > 0)
                s.crit = double(v) / s.scale;
            if (readInt64(dir + "/" + base + "_max", v) && v > 0)
                s.max = double(v) / s.scale;
            s.file.setPath(dir + "/" + in);
            chip->sensors.push_back(std::move(s));
        }
        if (chip->sensors.empty())
            continue;

        // Time a first read of fast-looking chips.
        if (!chip->slow) {
            std::vector<SensorReading> tmp;
            const int64_t t0 = monoNs();
            readChip(*chip, tmp);
            if (monoNs() - t0 > kSlowReadNs)
                chip->slow = true;
        }
        chips_.push_back(std::move(chip));
    }

    // Pick the CPU temperature sensor.
    struct Pref {
        const char* chip;
        const char* label;
    };
    static const Pref prefs[] = {{"k10temp", "Tctl"}, {"k10temp", "Tdie"}, {"zenpower", "Tdie"}, {"zenpower", "Tctl"},
                                 {"coretemp", "Package id 0"}, {"cpu_thermal", nullptr}, {"acpitz", nullptr}};
    for (const Pref& p : prefs) {
        for (auto& c : chips_) {
            if (c->name != QLatin1String(p.chip) || c->slow)
                continue;
            for (auto& s : c->sensors) {
                if (s.kind == SensorKind::Temp && (!p.label || s.label == QLatin1String(p.label))) {
                    cpuTempPath_ = s.file.path();
                    cpuTempLabel_ = c->name + QLatin1Char(' ') + s.label;
                    cpuTempKey_ = s.key.toStdString();
                    goto found;
                }
            }
        }
    }
found:;
}

double HwmonCollector::detectTjmax() const
{
    // Intel coretemp exposes the limit; AMD k10temp doesn't, so use a table.
    for (const auto& c : chips_) {
        if (c->name == QLatin1String("coretemp"))
            for (const auto& s : c->sensors)
                if (s.kind == SensorKind::Temp && s.crit > 0)
                    return s.crit;
    }
    std::string cpuinfo;
    if (!readFile(SysPaths::proc_("cpuinfo"), cpuinfo))
        return 95;
    int family = 0, model = 0;
    bool amd = cpuinfo.find("AuthenticAMD") != std::string::npos;
    LineReader lines(cpuinfo);
    std::string_view line;
    while (lines.next(line)) {
        if (startsWith(line, "cpu family"))
            parseInt(line.substr(line.find(':') + 1), family);
        else if (startsWith(line, "model\t") || startsWith(line, "model "))
            parseInt(line.substr(line.find(':') + 1), model);
        else if (line.empty() && family)
            break;
    }
    if (!amd)
        return 100;
    if (family == 0x19) {
        if (model >= 0x20 && model <= 0x2f)
            return 90;    // Zen 3 desktop (Vermeer)
        if (model >= 0x70)
            return 100;   // Zen 4 mobile (Phoenix)
        return 95;        // Zen 3 APU, Zen 4 desktop
    }
    if (family == 0x17)
        return 95;        // Zen / Zen+ / Zen 2
    return 95;            // Zen 5 and newer
}

void HwmonCollector::readChip(Chip& chip, std::vector<SensorReading>& out)
{
    for (Sensor& s : chip.sensors) {
        int64_t raw = 0;
        if (!s.file.readInt64(raw))
            continue;
        SensorReading r;
        r.chip = chip.name;
        r.label = s.label;
        r.key = s.key;
        r.kind = s.kind;
        r.value = double(raw) / s.scale;
        r.crit = s.crit;
        r.max = s.max;
        r.slowChip = chip.slow;
        out.push_back(std::move(r));
    }
}

void HwmonCollector::sample(std::vector<SensorReading>& out, ThermalSummary& thermal, double tjmaxOverride)
{
    for (auto& c : chips_) {
        if (c->slow) {
            std::lock_guard lock(mutex_);
            out.insert(out.end(), c->latest.begin(), c->latest.end());
            continue;
        }
        const size_t before = out.size();
        const int64_t t0 = monoNs();
        readChip(*c, out);
        if (monoNs() - t0 > kSlowReadNs) {
            // Demote: from now on this chip is only read on the slow thread.
            std::fprintf(stderr, "culprit: hwmon chip %s took %.1f ms to read; moving it to the slow tier\n",
                         qPrintable(c->name), double(monoNs() - t0) / 1e6);
            {
                std::lock_guard lock(mutex_);
                c->slow = true;
                c->latest.assign(out.begin() + long(before), out.end());
                for (size_t i = before; i < out.size(); ++i)
                    out[i].slowChip = true;
            }
            if (!slowThread_.joinable())
                slowThread_ = std::thread([this] { slowLoop(); });
        }
    }

    thermal.tjmaxC = tjmaxOverride > 0 ? tjmaxOverride : autoTjmax_;
    thermal.cpuTempLabel = cpuTempLabel_;
    for (const SensorReading& r : out) {
        if (r.key.toStdString() == cpuTempKey_) {
            thermal.cpuTempC = r.value;
            break;
        }
    }
}

void HwmonCollector::slowLoop()
{
    pthread_setname_np(pthread_self(), "culprit-sensors");
    setpriority(PRIO_PROCESS, 0, 10);
    while (!stop_) {
        for (auto& c : chips_) {
            bool slow;
            {
                std::lock_guard lock(mutex_);
                slow = c->slow;
            }
            if (!slow || stop_)
                continue;
            std::vector<SensorReading> tmp;
            const int64_t t0 = monoNs();
            readChip(*c, tmp);
            const int64_t t1 = monoNs();
            std::lock_guard lock(mutex_);
            c->latest = std::move(tmp);
            busy_.push_back({t0, t1, c->name});
            while (!busy_.empty() && busy_.front().endNs < t1 - 60'000'000'000LL)
                busy_.erase(busy_.begin());
        }
        std::unique_lock lock(mutex_);
        // Each refresh of a Super-I/O chip costs tens of ms of kernel time; board
        // temperatures and fans change slowly.
        const bool watched = watched_;
        cv_.wait_for(lock, std::chrono::seconds(watched ? kWatchedPeriodSec : kSlowPeriodSec),
                     [this, watched] { return stop_.load() || watched_ != watched; });
    }
}

void HwmonCollector::setWatched(bool watched)
{
    if (watched_.exchange(watched) != watched)
        cv_.notify_all();
}

std::vector<HwmonCollector::BusyInterval> HwmonCollector::busyIntervals(int64_t sinceNs) const
{
    std::lock_guard lock(mutex_);
    std::vector<BusyInterval> out;
    for (const auto& b : busy_)
        if (b.endNs >= sinceNs)
            out.push_back(b);
    return out;
}

QStringList HwmonCollector::slowChipNames() const
{
    std::lock_guard lock(mutex_);
    QStringList out;
    for (const auto& c : chips_)
        if (c->slow)
            out << c->name;
    return out;
}

} // namespace culprit
