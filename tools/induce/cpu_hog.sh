#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Oversubscribe the CPUs: N busy loops (default 1.5 x CPUs) for S seconds.
# Expect: "CPU oversubscribed" finding, run-queue wait graph rising, stutter hitches.
N=${1:-$(( $(nproc) * 3 / 2 ))}
S=${2:-30}
echo "Starting $N busy loops for $S s"
for _ in $(seq "$N"); do timeout "$S" sh -c 'while :; do :; done' & done
wait
