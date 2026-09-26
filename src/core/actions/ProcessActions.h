// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace culprit {

struct ActionResult {
    bool ok = false;
    bool permissionDenied = false;   // retry via pkexec makes sense
    bool gone = false;               // process exited / pid reused
    QString message;
};

// Acts on a specific process instance (pid + start time). Scheduling knobs on
// Linux are per-thread, so renice/ionice/affinity are applied to every thread.
namespace ProcessActions {

ActionResult sendSignal(const ProcKey& key, int sig);
ActionResult renice(const ProcKey& key, int nice);
ActionResult setIoPriority(const ProcKey& key, int ioClass, int level);   // class: 1 RT, 2 BE, 3 IDLE
ActionResult setAffinity(const ProcKey& key, const std::vector<int>& cpus);

// Current affinity of the main thread.
std::vector<int> affinity(int pid);

// Equivalent command line (for `pkexec`) when the direct call hit EPERM.
// Returns program + args, or an empty list if not applicable.
QStringList privilegedCommand(const QString& action, const ProcKey& key, const QStringList& params);

} // namespace ProcessActions
} // namespace culprit
