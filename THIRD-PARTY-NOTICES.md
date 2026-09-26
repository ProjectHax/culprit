# Third-party software

Culprit is Copyright (C) 2026 ProjectHax LLC and is licensed under the GNU
General Public License, version 3 or (at your option) any later version. See
[LICENSE](LICENSE).

Culprit uses the following third-party software. This source repository
contains none of its code.

## Qt 6

Culprit's user interface and event loop are built with Qt: Qt Core, Qt GUI,
Qt Widgets and Qt D-Bus, plus Qt's platform plugins (xcb, Wayland), SVG image
support and XDG desktop portal integration.

- Copyright (C) The Qt Company Ltd. and other contributors.
- License: GNU Lesser General Public License, version 3
  ([licenses/LGPL-3.0-only.txt](licenses/LGPL-3.0-only.txt)). The LGPL
  incorporates the GNU General Public License, version 3 ([LICENSE](LICENSE)).
- Website: <https://www.qt.io/>. Source code: <https://download.qt.io/official_releases/qt/>.

Culprit links to Qt dynamically. The RPM package uses the Qt libraries of your
distribution. The .deb package and the AppImage include an unmodified copy of
Qt from The Qt Company's official binary release, in a private directory (for
example, `/usr/lib/x86_64-linux-gnu/culprit` for the .deb package). You may
replace these libraries with your own build of the same Qt version. These
packages also include `THIRD-PARTY-BUNDLED.txt`. It lists the components
built into Qt (such as HarfBuzz, PCRE2, libpng and libjpeg-turbo), ICU and
any other bundled library, with their licenses. Culprit's *About* dialog shows
this list too.

## Wayland client library (libwayland-client)

Used, when available at build time, to ask the Wayland compositor whether it
draws window decorations.

```
Copyright © 2008-2012 Kristian Høgsberg
Copyright © 2010-2012 Intel Corporation
Copyright © 2011 Benjamin Franzke
Copyright © 2012 Collabora, Ltd.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## Linux kernel user-space API headers

The tracing helper uses `linux/perf_event.h` and `linux/ioprio.h` to call the
kernel through system calls. These headers are licensed under
`GPL-2.0 WITH Linux-syscall-note`. The note states that user programs that use
kernel services through normal system calls are not derived works of the
kernel.

## NVIDIA Management Library (NVML)

Not included and not linked. When the NVIDIA driver is installed, Culprit
loads `libnvidia-ml.so.1` at run time to read GPU sensors. It declares the few
functions it calls itself and contains no NVIDIA code.

## System libraries

The GNU C Library (LGPL-2.1-or-later) and the GCC runtime libraries
(GPL-3.0-or-later WITH GCC-exception-3.1) come from your operating system.
