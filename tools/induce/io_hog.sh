#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Sustained direct-I/O writes: rewrites a 2 GiB file in $HOME for S seconds
# (removed afterwards).
# Expect: disk utilisation near 100 %, "... is saturated" finding with dd as
# the writer, and dd threads showing up in D state.
S=${1:-30}
FILE=${2:-$HOME/.culprit-io-test.bin}
echo "Rewriting 2 GiB with O_DIRECT to $FILE for $S s"
trap 'rm -f "$FILE"' EXIT
timeout "$S" sh -c "while :; do dd if=/dev/zero of='$FILE' bs=1M count=2048 oflag=direct conv=notrunc status=none; done"
