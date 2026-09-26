#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Build a Culprit package into dist/. Compiles with half the CPU cores (build.sh).
#
#   packaging/ci/package.sh deb        Ubuntu 22.04+; bundles the Qt in $QT_ROOT_DIR
#   packaging/ci/package.sh appimage   same Qt, bundled with linuxdeploy
#   packaging/ci/package.sh rpm        AlmaLinux/RHEL 9 (Qt from EPEL) or 10; system Qt
#
# For deb and appimage, run packaging/ci/install-qt.sh first; QT_ROOT_DIR is
# <dir>/<version>/gcc_64 and the license texts are in <dir>/licenses.
set -euo pipefail
cd "$(dirname "$0")/../.."

kind=${1:?usage: package.sh deb|rpm|appimage}
version=$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt)
if [[ ${GITHUB_REF_TYPE:-} == tag && ${GITHUB_REF_NAME#v} != "$version" ]]; then
  echo "tag $GITHUB_REF_NAME does not match the project version $version in CMakeLists.txt" >&2
  exit 1
fi
mkdir -p dist

qt_args() {
  : "${QT_ROOT_DIR:?run packaging/ci/install-qt.sh first}"
  licenses=$(realpath "$QT_ROOT_DIR/../../licenses")
}

case $kind in
deb)
  qt_args
  BUILD_DIR=build-deb BUILD_TYPE=Release CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_PREFIX_PATH=$QT_ROOT_DIR
    -DCULPRIT_BUNDLE_QT=ON -DCULPRIT_DEBIAN_COPYRIGHT=ON
    -DCULPRIT_QT_LICENSE_DIRS=$licenses/qtbase;$licenses/qtsvg;$licenses/qtwayland
    -DCULPRIT_EXTRA_LICENSES=ICU=$licenses/icu/LICENSE" ./build.sh
  (cd build-deb && cpack -G DEB)
  cp build-deb/packages/*.deb dist/
  ;;

rpm)
  BUILD_DIR=build-rpm BUILD_TYPE=Release CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=/usr" ./build.sh
  (cd build-rpm && cpack -G RPM)
  cp build-rpm/packages/*.rpm dist/
  ;;

appimage)
  qt_args
  BUILD_DIR=build-appimage BUILD_TYPE=Release CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_PREFIX_PATH=$QT_ROOT_DIR" ./build.sh
  appdir=$PWD/build-appimage/AppDir
  rm -rf "$appdir"
  DESTDIR=$appdir cmake --install build-appimage --strip

  tools=$PWD/build-appimage/tools
  mkdir -p "$tools"
  for tool in linuxdeploy/linuxdeploy linuxdeploy/linuxdeploy-plugin-qt AppImage/appimagetool; do
    file="$tools/${tool#*/}-x86_64.AppImage"
    [[ -x $file ]] || { curl -fsSL -o "$file" \
      "https://github.com/$tool/releases/download/continuous/${tool#*/}-x86_64.AppImage" && chmod +x "$file"; }
  done

  export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1   # no FUSE needed; Culprit is stripped by the install
  export QMAKE=$QT_ROOT_DIR/bin/qmake LD_LIBRARY_PATH=$QT_ROOT_DIR/lib
  export EXTRA_QT_MODULES="svg;waylandclient" EXTRA_QT_PLUGINS="svg;waylandclient"
  export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
  # Every desktop has libdbus; bundling Ubuntu's would drag in libsystemd and libgcrypt.
  export LINUXDEPLOY_EXCLUDED_LIBRARIES="libdbus-1.so*"
  "$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
    --executable "$appdir/usr/bin/culprit" \
    --desktop-file "$appdir/usr/share/applications/culprit.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/culprit.svg" \
    --exclude-library "libdbus-1.so*" \
    --plugin qt
  # File dialogs and settings through the XDG desktop portal (depends only on Qt).
  install -D -m 755 "$QT_ROOT_DIR/plugins/platformthemes/libqxdgdesktopportal.so" \
    "$appdir/usr/plugins/platformthemes/libqxdgdesktopportal.so"
  # linuxdeploy still copies the dependencies of excluded libraries; drop every
  # library that neither Culprit nor a plugin needs.
  python3 - "$appdir/usr" <<'PRUNE'
import os, subprocess, sys
root = sys.argv[1]
libdir = os.path.join(root, "lib")
def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return [l.split("[", 1)[1].split("]", 1)[0] for l in out.splitlines() if "(NEEDED)" in l]
todo = [os.path.join(root, "bin", "culprit")]
todo += [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(root, "plugins")) for f in fs if f.endswith(".so")]
keep = set()
while todo:
    for name in needed(todo.pop()):
        if name not in keep and os.path.exists(os.path.join(libdir, name)):
            keep.add(name)
            todo.append(os.path.join(libdir, name))
for name in sorted(os.listdir(libdir)):
    if name not in keep:
        print("AppImage: dropping unused", name)
        os.remove(os.path.join(libdir, name))
PRUNE

  python3 packaging/bundle-notices.py --bundle "$appdir/usr" --qt-prefix "$QT_ROOT_DIR" \
    --qt-licenses "$licenses/qtbase" --qt-licenses "$licenses/qtsvg" --qt-licenses "$licenses/qtwayland" \
    --license "ICU=$licenses/icu/LICENSE" --dpkg --strict \
    --output "$appdir/usr/share/doc/culprit/THIRD-PARTY-BUNDLED.txt"

  # appimagetool packs the AppDir as it is (linuxdeploy's AppImage output would
  # re-deploy the libraries pruned above).
  ARCH=x86_64 "$tools/appimagetool-x86_64.AppImage" --no-appstream "$appdir" "dist/Culprit-$version-x86_64.AppImage"
  ;;

*)
  echo "unknown package type: $kind" >&2
  exit 2
  ;;
esac

ls -l dist/
