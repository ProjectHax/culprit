#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

"""Write THIRD-PARTY-BUNDLED.txt for a package that bundles Qt (and, for the
AppImage, other libraries): what is bundled, under which license, and the full
license texts.

  --bundle DIR          directory tree with the bundled libraries and plugins
  --qt-prefix DIR       the Qt installation they came from (for its SBOM files)
  --qt-licenses DIR     a LICENSES folder of Qt's sources (repeatable)
  --license NAME=FILE   license text of another bundled component (repeatable),
                        e.g. ICU=/path/to/LICENSE
  --dpkg                look up any other bundled library's Debian package and
                        include its copyright file
  --strict              fail if a license text is missing
  --output FILE
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

QT_SOURCE_URL = "https://download.qt.io/archive/qt/{mm}/{v}/submodules/"
QT_MODULE_LICENSE = "LGPL-3.0-only"

# SBOM entries that are not part of what gets bundled.
SKIP_PREFIXES = ("Bootstrap", "Test", "QSQLite", "WaylandCompositor")


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def bundled_files(root):
    out = []
    for d, _, files in os.walk(root):
        for name in files:
            p = os.path.join(d, name)
            if not os.path.islink(p) and is_elf(p):
                out.append(os.path.relpath(p, root))
    return sorted(out)


def license_ids(expr):
    return {t for t in re.split(r"[\s()]+", expr or "") if t and t not in ("AND", "OR", "WITH")}


def load_sboms(qt_prefix, modules):
    docs = []
    for m in modules:
        for f in glob.glob(os.path.join(qt_prefix, "sbom", f"{m}-*.spdx.json")):
            with open(f, encoding="utf-8") as fh:
                docs.append(json.load(fh))
    return docs


def qt_attributions(docs, network_bundled):
    seen, items, extracted = set(), [], {}
    for d in docs:
        for e in d.get("hasExtractedLicensingInfos", []):
            extracted[e["licenseId"]] = e.get("extractedText", "")
        root = d.get("name", "").split("-")[0]
        for p in d.get("packages", []):
            name, lic = p.get("name", ""), p.get("licenseConcluded", "NOASSERTION")
            if name in ("GNU", root) or lic == "NOASSERTION" or "Qt-GPL-exception" in lic:
                continue   # compiler, module root, external system libraries, build tools
            if lic.startswith("LicenseRef-Qt-Commercial"):
                continue   # Qt's own modules and plugins (covered by the Qt section)
            if name.startswith(SKIP_PREFIXES) or (name.startswith("Network_") and not network_bundled):
                continue
            component = name.split("_Attribution_", 1)[-1]
            key = (component.lower(), p.get("versionInfo"))
            if key in seen:
                continue
            seen.add(key)
            items.append({
                "name": component,
                "version": p.get("versionInfo") or "",
                "license": lic,
                "copyright": (p.get("copyrightText") or "").strip(),
                "homepage": p.get("homepage") if p.get("homepage") not in (None, "NONE", "NOASSERTION") else "",
            })
    items.sort(key=lambda i: i["name"].lower())
    return items, extracted


def dpkg_owner(lib_name):
    try:
        out = subprocess.run(["dpkg", "-S", lib_name], capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        return None
    for line in out.splitlines():
        pkg, _, path = line.partition(": ")
        if path.endswith("/" + lib_name):
            return pkg.split(":")[0].split(",")[0].strip()
    return None


def dpkg_version(pkg):
    out = subprocess.run(["dpkg-query", "-W", "-f=${Version}", pkg], capture_output=True, text=True, check=False)
    return out.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--qt-prefix")
    ap.add_argument("--qt-licenses", action="append", default=[])
    ap.add_argument("--license", action="append", default=[])
    ap.add_argument("--dpkg", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--output", required=True)
    a = ap.parse_args()

    files = bundled_files(a.bundle)
    qt_files = [f for f in files if os.path.basename(f).startswith("libQt6") or f.startswith("plugins/")
                or "/plugins/" in f]
    icu_files = [f for f in files if os.path.basename(f).startswith("libicu")]
    culprit_files = [f for f in files if os.path.basename(f).startswith("culprit")]
    other_files = [f for f in files if f not in qt_files and f not in icu_files and f not in culprit_files]

    missing = []
    texts = {}   # license id -> text

    def add_text(lid, extracted):
        if lid in texts or lid.startswith("LicenseRef-Qt-Commercial"):
            return
        if lid in extracted:
            texts[lid] = extracted[lid]
            return
        for d in a.qt_licenses:
            p = os.path.join(d, lid + ".txt")
            if os.path.exists(p):
                with open(p, encoding="utf-8", errors="replace") as fh:
                    texts[lid] = fh.read()
                return
        missing.append(lid)

    out = []
    w = out.append
    w("Third-party software bundled with Culprit")
    w("=========================================")
    w("")
    w("Culprit is Copyright (C) 2026 ProjectHax LLC and licensed under the GNU")
    w("General Public License, version 3 or later (see LICENSE). This package also")
    w("contains the following third-party software, each under its own license.")
    w("")

    # Versions come from Qt's SBOM: linuxdeploy copies libraries under their
    # soname only (libQt6Core.so.6), so file names don't always carry them.
    docs = load_sboms(a.qt_prefix, ("qtbase", "qtsvg", "qtwayland")) if a.qt_prefix else []
    qt_version = next((d["name"].split("-", 1)[1] for d in docs if d.get("name", "").startswith("qtbase-")), "")
    icu_version = next((p.get("versionInfo", "") for d in docs for p in d.get("packages", []) if p.get("name") == "ICU"), "")
    if qt_files:
        for f in files:
            m = re.match(r"libQt6Core\.so\.(\d+\.\d+\.\d+)$", os.path.basename(f))
            if m and not qt_version:
                qt_version = m.group(1)
        mm = ".".join(qt_version.split(".")[:2])
        network = any(os.path.basename(f).startswith("libQt6Network") for f in files)
        items, extracted = qt_attributions(docs, network)
        w(f"Qt {qt_version}")
        w("-" * (3 + len(qt_version)))
        w("Copyright (C) The Qt Company Ltd. and other contributors.")
        w("Used under the GNU Lesser General Public License, version 3 (LGPL-3.0-only;")
        w("text below). The LGPL incorporates the GNU General Public License,")
        w("version 3, which is in this package's LICENSE file.")
        w("")
        w("These files are unmodified copies from The Qt Company's official binary")
        w("release. Culprit loads them as shared libraries, so you can replace them")
        w("with your own build of the same Qt version. The corresponding source code")
        w(f"is available at {QT_SOURCE_URL.format(mm=mm, v=qt_version)}")
        w("(qtbase, qtsvg and qtwayland).")
        w("")
        for f in qt_files:
            w(f"  {f}")
        w("")
        w("Components built into these Qt files:")
        w("")
        for i in items:
            w(f"* {i['name']} {i['version']}".rstrip())
            w(f"  License: {i['license']}")
            if i["homepage"]:
                w(f"  Website: {i['homepage']}")
            for line in i["copyright"].splitlines():
                w(f"  {line}".rstrip())
            w("")
            for lid in license_ids(i["license"]):
                add_text(lid, extracted)
        add_text(QT_MODULE_LICENSE, extracted)

    for spec in a.license:
        name, _, path = spec.partition("=")
        if name.upper().startswith("ICU"):
            if not icu_files:
                continue
            m = re.search(r"\.so\.(\d+(?:\.\d+)*)$", icu_files[0])
            version = icu_version or (m.group(1) if m else "")
            name = f"ICU {version}".rstrip() if name == "ICU" else name
        with open(path, encoding="utf-8", errors="replace") as fh:
            body = fh.read()
        w(name)
        w("-" * len(name))
        if name.upper().startswith("ICU"):
            for f in icu_files:
                w(f"  {f}")
            w("")
        w(body.rstrip())
        w("")

    if other_files:
        packages = {}
        for f in other_files:
            pkg = dpkg_owner(os.path.basename(f)) if a.dpkg else None
            packages.setdefault(pkg, []).append(f)
        for pkg in sorted(packages, key=lambda p: (p is None, p or "")):
            if pkg is None:
                w("Other libraries")
                w("---------------")
                for f in packages[pkg]:
                    w(f"  {f}")
                    missing.append(f"copyright of {f}")
                w("")
                continue
            title = f"{pkg} {dpkg_version(pkg)} (Debian/Ubuntu package)"
            w(title)
            w("-" * len(title))
            for f in packages[pkg]:
                w(f"  {f}")
            w("")
            copyright_file = f"/usr/share/doc/{pkg}/copyright"
            if os.path.exists(copyright_file):
                with open(copyright_file, encoding="utf-8", errors="replace") as fh:
                    w(fh.read().rstrip())
            else:
                missing.append(copyright_file)
            w("")

    if texts:
        w("")
        w("License texts")
        w("=============")
        for lid in sorted(texts):
            w("")
            w(f"----- {lid} " + "-" * max(3, 70 - len(lid)))
            w("")
            w(texts[lid].rstrip())

    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    with open(a.output, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")

    if missing:
        print("bundle-notices: missing: " + ", ".join(sorted(set(missing))), file=sys.stderr)
        if a.strict:
            return 1
    print(f"bundle-notices: {a.output}: {len(qt_files)} Qt files, {len(icu_files)} ICU, "
          f"{len(other_files)} other, {len(texts)} license texts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
