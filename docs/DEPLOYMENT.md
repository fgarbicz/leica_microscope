# DM Imaging — deployment checklist

The main part of this document is the **Windows** rollout (the microscope PC).
macOS and Linux are covered at the end.

Installer: `DMImaging-Setup-<version>.exe` (about 40 MB), attached to each
[GitHub release](https://github.com/fgarbicz/leica_microscope/releases/latest) with its
SHA-256, or `dist\DMImaging-Setup-<version>.exe` when made locally with
`tools\make_installer.ps1` (see the README). Below, `<version>` is its version, e.g. 1.1.2;
*Help → About* shows the same version and the git build it came from.

## Before you start (per PC)

- Windows 10 or 11, 64-bit (x64); an account that can approve an administrator prompt.
  Windows 11 on ARM runs the same installer under x64 emulation; a native ARM64 build is
  `tools\make_installer.ps1 -Arch arm64`. The application was verified that way in a VM
  without a camera, but Windows there refused the camera driver (see *If something goes
  wrong*), so the camera is untested on ARM.
- The Leica DMC6200 camera on a **USB 3.0** port (blue connector). It may be connected before
  or after installing.
- Leica LAS X may stay installed, but **must not be running** at the same time (only one
  program can use the camera).

## Install

1. Copy `DMImaging-Setup-<version>.exe` to the PC and run it. Approve the administrator prompt.
2. Keep both options ticked: *desktop shortcut* and *Install the camera driver*.
3. Finish with *Start DM Imaging now*.

Silent install (IT deployment): `DMImaging-Setup-<version>.exe /VERYSILENT /NORESTART /TASKS="desktopicon,installdriver"`
from an elevated prompt; add `/LOG="C:\Temp\dmimaging_setup.log"` for a log.

## Verify (2 minutes)

| Check | Expected |
|---|---|
| App starts | Title bar shows *Leica DMC6200 (serial)*, live image within a few seconds |
| Frame rate | Status bar ~30–60 fps (depends on exposure) |
| Colours | Background white, DAB brown, haematoxylin blue (press *Auto white balance* (F7) on an empty field first) |
| Capture | F9 → dialog asks objective + name → *Save* → file appears in *Browse* |
| Calibration | Browse → metadata shows *Pixel size* for the chosen objective |
| Version | Help → About: *DM Imaging `<version>`*, with the build it came from |

## If something goes wrong

| Symptom | Action |
|---|---|
| Setup says *Windows did not accept the camera driver* | Setup names a log, `C:\Program Files\DM Imaging\driver\install_driver.log`. The camera driver is signed on the PC itself with a local certificate; Windows refuses such a driver when a Code Integrity policy is enforced (`C:\Windows\INF\setupapi.dev.log` then says *"Driver package signer is not trusted by system, and Code Integrity is enforced"*). This was seen on Windows 11 on ARM (build 26200) with Smart App Control in evaluation mode, not on the x64 lab PC. Check *Windows Security → App & browser control → Smart App Control*, and ask IT before changing it: switching it off cannot be undone without reinstalling Windows. The lasting fix is a driver package signed through Microsoft's attestation service. |
| "No Leica camera found" | USB 3.0 port? Then *Tools → Install / repair camera driver*. Check *Device Manager*: the camera should appear under *Universal Serial Bus devices* as **USB 3.0 Camera**. |
| "Sensor is not ready" | Unplug the camera's USB cable for 5 seconds and plug it in again. A USB reset or Windows restart is not enough. |
| Camera works in LAS X but not here | Close LAS X (only one program can use the camera). |
| LAS X no longer finds the camera | Not tested with this release: DM Imaging binds its own driver to the camera. To return to LAS X, uninstall DM Imaging (it removes its driver package), reconnect the camera and, if needed, repair the Leica driver. |
| App does not start ("MSVCP140.dll") | Run Setup again (it installs the Visual C++ runtime), or install Microsoft's *vc_redist.x64.exe*. |
| Anything else | Send `%APPDATA%\DM Imaging\DM Imaging\dmimaging.log` (and `dmimaging.1.log` from the previous run). |

## Rollback

*Windows Settings → Apps → DM Imaging → Uninstall*. This removes the application and its
camera driver package; images and settings are kept. Reconnect the camera and, if needed,
reinstall the Leica driver / LAS X.

## Good to know for users

- Images are saved as 16-bit TIFF with µm/pixel calibration (ImageJ/Fiji, QuPath read it).
  Default folder: *Pictures\DM Imaging* (change it in *Save settings* or *File → Settings* (Ctrl+,)).
- Each objective remembers its own exposure and white balance; select the objective in the
  capture dialog (keys 1–6).
- IHC: use the same DAB threshold and stain colours for all slides of a study.
- Help → Keyboard shortcuts; the user guide is in the Start menu (*DM Imaging User Guide*).

## macOS

Requirements: Qt 6 and libusb to build (`brew install qt libusb`), Apple silicon or Intel.

The app bundles the Homebrew Qt and libusb it was built with, so it runs on the macOS
version those were built for, or newer: with current Homebrew that is the macOS of
the build machine (libusb) and at least macOS 14 (Qt). Building on the Mac that will
use it, which is what `./install.sh` does, always works. To support an older macOS,
build there.

```bash
./install.sh            # build, bundle Qt + libusb into the .app, install to /Applications
./install.sh --uninstall
```

- No driver and no administrator rights are needed: macOS lets the application open the
  vendor-class camera directly.
- The bundle carries an **ad-hoc signature**, which is enough on the machine that built it.
  To hand the `.app` to another Mac without Gatekeeper complaints, sign and notarise it with
  a Developer ID:
  ```bash
  codesign --force --deep --options runtime --sign "Developer ID Application: …" "/Applications/DM Imaging.app"
  xcrun notarytool submit … && xcrun stapler staple "/Applications/DM Imaging.app"
  ```
  Without that, a copied bundle opens via right-click → *Open* the first time.
- While developing on a Mac, note that every rebuild re-signs the bundle with a
  fresh ad-hoc identity, so macOS forgets the camera permission and asks again on
  the next launch. An installed, properly signed build asks once. The application
  says what is happening instead of showing a connected camera with a dead image.
- macOS asks for camera permission the first time a **UVC** camera is used (not for the
  DMC6200), and for access to the Pictures folder the first time an image is saved there.
  Both are one-off and can be reviewed in *System Settings → Privacy & Security*.

## Linux

Requirements: Qt 6.4+, libusb-1.0, CMake 3.24+, a C++20 compiler (GCC 12 or newer).
Built and tested on Debian 12 (Qt 6.4) and Debian 13 (Qt 6.8). Other distributions
with Qt 6.4 or newer should work (Ubuntu 24.04, current Fedora); Ubuntu 22.04 ships
Qt 6.2, which is too old.

```bash
sudo apt install qt6-base-dev qt6-svg-dev libusb-1.0-0-dev cmake ninja-build build-essential
./install.sh                            # installs into /usr/local (--prefix DIR to change)
sudo bash driver/install_udev_rule.sh     # camera access without root
```

- The udev rule tags the device with `uaccess`, so the user logged in at the seat can open
  it. **Unplug and replug the camera** after installing the rule.
- Without the rule the camera appears in the list but fails to open with "Access denied";
  the message says what to do, and *Tools → Install camera access rule* runs the same
  script through `pkexec`.
- `./build.sh --deploy` produces an AppImage when `linuxdeploy` is on PATH; otherwise the
  installed binary uses the system Qt.

## Verify on any platform

| Check | Expected |
|---|---|
| *Help → About* | version, build hash, the operating system and the USB backend in use |
| Camera panel | the DMC6200 is listed and connects; the live image starts |
| Scroll a side panel with the wheel | the panel scrolls; no slider, spin box or combo box changes |
| Capture an image | the objective/name dialog appears; a 16-bit TIFF lands in the image folder |
