// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

namespace culprit {

// Splits measured CPU power across processes. Power scales with cycles, so
// each process's share is its CPU time weighted by the frequency of the core
// it runs on. The result is an estimate (shown as such in the UI).
void attributeCpuPower(Frame& frame);

} // namespace culprit
