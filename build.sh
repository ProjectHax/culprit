#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Configure and build Culprit using half the available CPU cores.
#
#   ./build.sh            configure (first run) + build
#   ./build.sh --clean    wipe the build directory first
#   BUILD_TYPE=Debug ./build.sh
#   CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=/usr" ./build.sh   extra configure options
#
# Extra arguments after the options are passed to `cmake --build`
# (e.g. ./build.sh --target culprit-helper).
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
JOBS=$(( $(nproc) / 2 ))
(( JOBS < 1 )) && JOBS=1

if [[ ${1:-} == "--clean" ]]; then
  rm -rf "$BUILD_DIR"
  shift
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  GENERATOR=()
  command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of options
  cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCULPRIT_JOBS="$JOBS" ${CMAKE_ARGS:-}
fi

echo "Building with $JOBS parallel jobs"
export CMAKE_BUILD_PARALLEL_LEVEL=$JOBS
cmake --build "$BUILD_DIR" --parallel "$JOBS" "$@"
