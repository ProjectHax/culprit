#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

# Install Qt's official Linux binaries (for the .deb and the AppImage, which
# bundle them) and the license texts their notices need:
#
#   packaging/ci/install-qt.sh 6.10.3 ~/qt
#
# Result:  ~/qt/6.10.3/gcc_64       Qt (QT_ROOT_DIR)
#          ~/qt/licenses/qt<module> the LICENSES folders of Qt's sources
#          ~/qt/licenses/icu        ICU's license, for the ICU Qt ships with
# Under GitHub Actions, QT_ROOT_DIR is also exported to later steps.
set -euo pipefail

QT_VERSION=${1:?usage: install-qt.sh VERSION DIR}
DEST=$(realpath -m "${2:?usage: install-qt.sh VERSION DIR}")
AQT_VERSION=3.3.0
QT_ROOT_DIR="$DEST/$QT_VERSION/gcc_64"

if [[ ! -x "$QT_ROOT_DIR/bin/qmake" ]]; then
  python3 -m venv "$DEST/.venv"
  "$DEST/.venv/bin/pip" install -q "aqtinstall==$AQT_VERSION"
  "$DEST/.venv/bin/aqt" install-qt -O "$DEST" linux desktop "$QT_VERSION" linux_gcc_64
fi

for module in qtbase qtsvg qtwayland; do
  dir="$DEST/licenses/$module"
  [[ -d $dir ]] && continue
  mkdir -p "$dir.tmp"
  curl -fsSL "https://download.qt.io/archive/qt/${QT_VERSION%.*}/$QT_VERSION/submodules/$module-everywhere-src-$QT_VERSION.tar.xz" |
    tar -xJ -C "$dir.tmp" --strip-components=2 --wildcards "*/LICENSES/*"
  mv "$dir.tmp" "$dir"
done

icu=$(find "$QT_ROOT_DIR/lib" -name 'libicuuc.so.*.*' -printf '%f\n' | sed 's/^libicuuc\.so\.//' | head -1)
if [[ -n $icu && ! -f "$DEST/licenses/icu/LICENSE" ]]; then
  mkdir -p "$DEST/licenses/icu"
  curl -fsSL "https://raw.githubusercontent.com/unicode-org/icu/release-${icu//./-}/icu4c/LICENSE" \
    -o "$DEST/licenses/icu/LICENSE"
fi

echo "Qt $QT_VERSION in $QT_ROOT_DIR (ICU ${icu:-none})"
[[ -n ${GITHUB_ENV:-} ]] && echo "QT_ROOT_DIR=$QT_ROOT_DIR" >> "$GITHUB_ENV"
exit 0
