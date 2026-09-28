# Third-party software

DM Imaging is Copyright (c) 2026 Filip Garbicz and licensed under the GNU General
Public License version 3 or later ([LICENSE](LICENSE)). It uses the following
libraries, which keep their own licenses. They are linked dynamically and ship as
separate files, so they can be replaced with other builds of the same version.

## Qt 6

- Used modules: Qt Core, Gui, Widgets, Concurrent, SVG, PrintSupport and, on
  Linux, D-Bus, plus the platform, style, image-format and icon-engine plugins
  that the build tools deploy with them.
- License: GNU Lesser General Public License version 3
  ([licenses/LGPL-3.0.txt](licenses/LGPL-3.0.txt), which refers to the GPL v3 in
  [LICENSE](LICENSE)).
- Copyright (C) The Qt Company Ltd. and other contributors.
- Source code: the Qt release used for the build (6.8.3 for the Windows
  installer) is available unmodified from https://download.qt.io/official_releases/qt/
  and https://code.qt.io. On request we provide the same source.
- Qt itself contains third-party components (e.g. zlib, libpng, libjpeg,
  HarfBuzz, FreeType, PCRE2) under their own permissive licenses, listed at
  https://doc.qt.io/qt-6/licenses-used-in-qt.html.

## libusb 1.0 (macOS and Linux builds)

- Talks to the Leica DMC6200 over USB on macOS and Linux. The Windows build uses
  WinUSB, which is part of Windows.
- License: GNU Lesser General Public License version 2.1
  ([licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt)).
- Copyright (C) the libusb authors. Source: https://github.com/libusb/libusb

## Microsoft Visual C++ Redistributable (Windows installer)

- The Windows installer carries Microsoft's `vc_redist.x64.exe` and runs it when
  the runtime is missing. It is distributed under Microsoft's redistribution terms
  for Visual Studio and is not covered by the GPL.

## Inno Setup (Windows installer)

- The Windows installer is built with Inno Setup, Copyright (C) Jordan Russell
  and Martijn Laan (https://jrsoftware.org). Only its setup program is part of the
  installer; it is not part of DM Imaging.
