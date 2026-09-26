#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Install what the package builds need.
#
#   packaging/ci/deps.sh ubuntu   Ubuntu 22.04: compiler, CMake and the system
#                                 libraries Qt's official binaries load
#   packaging/ci/deps.sh alma     AlmaLinux 9 (Qt 6 from EPEL) or 10
set -euo pipefail

SUDO=()
(( EUID != 0 )) && SUDO=(sudo)

case ${1:?usage: deps.sh ubuntu|alma} in
ubuntu)
  export DEBIAN_FRONTEND=noninteractive
  "${SUDO[@]}" apt-get update -q
  "${SUDO[@]}" apt-get install -y -q --no-install-recommends \
    build-essential cmake ninja-build pkg-config file curl ca-certificates xz-utils \
    python3 python3-venv dpkg-dev desktop-file-utils \
    libwayland-dev libgl-dev libegl-dev libxkbcommon-dev libvulkan-dev \
    libfontconfig1 libfreetype6 libdbus-1-3 libglib2.0-0 libx11-xcb1 libxkbcommon-x11-0 \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb-xinput0 \
    libwayland-cursor0 libwayland-egl1 libopengl0
  ;;
alma)
  # shellcheck source=/dev/null
  . /etc/os-release
  "${SUDO[@]}" dnf -y -q install dnf-plugins-core
  "${SUDO[@]}" dnf config-manager --set-enabled crb
  [[ ${VERSION_ID%%.*} == 9 ]] && "${SUDO[@]}" dnf -y -q install epel-release   # Qt 6 lives in EPEL on EL9
  "${SUDO[@]}" dnf -y -q install \
    cmake ninja-build gcc-c++ rpm-build pkgconf-pkg-config \
    qt6-qtbase-devel wayland-devel
  ;;
*)
  echo "unknown distribution: $1" >&2
  exit 2
  ;;
esac
