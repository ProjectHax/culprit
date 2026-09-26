#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# A SCHED_FIFO task that spins for 50 ms every 2 s on one CPU (needs root for chrt -f).
# Expect: periodic hitches (period ~2.0 s) classified as CPU contention, and a
# "Real-time task" finding while it spins.
CPU=${1:-7}
S=${2:-40}
echo "RT burst on CPU $CPU every 2 s for $S s (sudo)"
sudo timeout "$S" chrt -f 50 taskset -c "$CPU" python3 -c '
import time
while True:
    end = time.monotonic() + 0.05
    while time.monotonic() < end:
        pass
    time.sleep(1.95)
'
