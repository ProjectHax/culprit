// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "core/model/Frame.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace culprit {

// hwmon sensors (temperatures, fans, voltages, power).
//
// Super-I/O / EC / SMM based chips (nct67xx, it87, dell_smm, ...) can take tens
// of milliseconds of kernel time per read — or trigger SMIs — so they are read
// on a separate slow thread and never from latency-sensitive code. Any other
// chip that is observed taking > 5 ms per read is demoted to that tier too.
class HwmonCollector {
public:
    HwmonCollector();
    ~HwmonCollector();

    // Fast chips are read inline; slow chips contribute their latest readings.
    void sample(std::vector<SensorReading>& out, ThermalSummary& thermal, double tjmaxOverride);

    // sysfs path of the best CPU temperature sensor (for the 50 Hz flight recorder), or empty.
    std::string cpuTempPath() const { return cpuTempPath_; }

    // Periods during which Culprit itself was busy reading a slow chip, so a
    // hitch that coincides with one can be labelled as self-inflicted.
    struct BusyInterval {
        int64_t startNs, endNs;
        QString chip;
    };
    std::vector<BusyInterval> busyIntervals(int64_t sinceNs) const;

    QStringList slowChipNames() const;

    // Slow chips cost ~70 ms of kernel CPU per refresh: poll them every 30 s,
    // or every 5 s while someone is looking at the sensors.
    void setWatched(bool watched);

private:
    struct Sensor {
        CachedFile file;
        SensorKind kind = SensorKind::Other;
        QString label, key;
        double scale = 1;
        double crit = 0, max = 0;
    };
    struct Chip {
        std::string dir;
        QString name;
        std::vector<Sensor> sensors;
        bool slow = false;
        std::vector<SensorReading> latest;   // slow chips: written by the slow thread
    };

    void enumerate();
    static void readChip(Chip& chip, std::vector<SensorReading>& out);
    void slowLoop();
    double detectTjmax() const;

    std::vector<std::unique_ptr<Chip>> chips_;
    std::string cpuTempPath_;
    QString cpuTempLabel_;
    std::string cpuTempKey_;
    double autoTjmax_ = 95;

    mutable std::mutex mutex_;   // guards Chip::latest of slow chips and busy_
    std::vector<BusyInterval> busy_;
    std::thread slowThread_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> watched_{false};
    std::string buf_;
};

} // namespace culprit
