#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Every PERIOD seconds, start 2 x CPUs busy loops for 150 ms (no root needed).
# Expect: hitches recurring every PERIOD s ("Periodic stutter every … s").
PERIOD=${1:-2}
S=${2:-30}
N=$(( $(nproc) * 2 ))
echo "Bursting $N busy loops for 150 ms every $PERIOD s for $S s"
end=$(( SECONDS + S ))
while (( SECONDS < end )); do
  for _ in $(seq "$N"); do timeout 0.15 sh -c 'while :; do :; done' & done
  sleep "$PERIOD"
done
wait
