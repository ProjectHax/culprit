#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# A busy loop in a scope limited to 20 % of one CPU.
# Expect: "... is throttled by its CPU quota" finding (Load & I/O -> Cgroups).
S=${1:-30}
echo "Busy loop under CPUQuota=20% for $S s"
systemd-run --user --scope -p CPUQuota=20% timeout "$S" sh -c 'while :; do :; done'
