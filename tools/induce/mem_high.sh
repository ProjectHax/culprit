#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Allocate and keep touching 1 GiB inside a scope limited by MemoryHigh=300M.
# Expect: "... is being throttled at its memory.high limit" and reclaim/major-fault evidence.
S=${1:-30}
echo "1 GiB working set under MemoryHigh=300M for $S s"
systemd-run --user --scope -p MemoryHigh=300M -p MemorySwapMax=2G timeout "$S" python3 -c '
import time
buf = bytearray(1 << 30)
while True:
    for i in range(0, len(buf), 4096):
        buf[i] = (buf[i] + 1) & 0xff
'
