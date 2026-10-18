#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Starts a throwaway headless mutter and freezes it (SIGSTOP) for MS milliseconds
# every 3 s, for S seconds. No root needed, and your own desktop isn't touched.
# Watch it with:  CULPRIT_WAYLAND_DISPLAY=culprit-induce ./build/culprit --tab stutter
# Expect: "mutter main thread froze for ~MS ms", the compositor response line spiking.
MS=${1:-300}
S=${2:-30}
NAME=culprit-induce
command -v mutter >/dev/null || { echo "needs mutter"; exit 1; }
dbus-run-session -- mutter --headless --wayland-display=$NAME --virtual-monitor 1280x720 --no-x11 >/dev/null 2>&1 &
SESSION=$!
PID=
for _ in $(seq 50); do
  for p in $(pgrep -x mutter); do
    tr '\0' ' ' < /proc/$p/cmdline | grep -q -- "--wayland-display=$NAME" && PID=$p
  done
  [[ -n $PID ]] && break
  sleep 0.1
done
[[ -n $PID ]] || { echo "mutter didn't start"; kill $SESSION; exit 1; }
trap 'kill -CONT $PID 2>/dev/null; kill $PID 2>/dev/null; wait' EXIT
echo "Headless mutter $PID on Wayland display $NAME; freezing it for $MS ms every 3 s for $S s"
sleep 3
end=$(( SECONDS + S ))
while (( SECONDS < end )); do
  kill -STOP $PID
  sleep "$(awk "BEGIN { print $MS / 1000 }")"
  kill -CONT $PID
  sleep 3
done
