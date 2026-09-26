#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Two busy loops pinned to one CPU: congestion on that CPU only.
# Expect: "CPU N is congested while other CPUs are free"; with per-CPU probes,
# hitches on that CPU classified as CPU contention with `sh` as suspect.
CPU=${1:-5}
S=${2:-30}
echo "Pinning 2 busy loops to CPU $CPU for $S s"
for _ in 1 2; do timeout "$S" taskset -c "$CPU" sh -c 'while :; do :; done' & done
wait
