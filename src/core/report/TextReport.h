// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QString>

#include <vector>

namespace culprit {

// Plain-text rendering of a frame: used by `culprit --snapshot` and by the
// text report exporter.
QString frameText(const Frame& f, const std::vector<Hitch>& hitches = {});

QString hitchText(const Hitch& h);
QString findingText(const Finding& f);

} // namespace culprit
