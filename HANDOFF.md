# DM Imaging — session handoff

*2026-09-27 · version 1.1.2, not yet committed (see "State of the tree")*

This session reviewed the whole code base, fixed what the review found, brought the
documentation up to date, and built, installed and ran the Windows release for the
first time since the cross-platform change. All three platforms now build and pass
the same four tests from one tree.

## Verification

| | Builds | 4/4 tests | Application run | Camera exercised |
|---|---|---|---|---|
| macOS 26 (Apple silicon) | yes | yes | yes, with the DMC6200: live 60 fps, 16-shot pixel shift, save, Browse on OneDrive | **yes, Leica DMC6200 over libusb** (2026-09-27) |
| Linux, Debian 13 (Qt 6.8) and 12 (Qt 6.4.2) | yes | yes | no display | no |
| Windows 11 on ARM (VMware VM), x64 build under emulation, MSVC 14.44 | yes | yes | yes: installer, capture, Browse, Process, measuring, pixel-size repair | UVC via Media Foundation (the VM's virtual webcam); **not** the DMC6200 |

The Windows VM (`fgwindows`, 192.168.138.128 on VMware's NAT) now has OpenSSH, VS 2022
Build Tools (x64 and ARM64 compilers), Qt 6.8.3 msvc2022_64, CMake, Ninja, Inno Setup
and an x64 Python with aqtinstall. Code reaches it through git bundles, never scp.

## What the review found and fixed

Three reviews (Windows/cross-platform, camera and file I/O, interface) and an
adversarial review of the fixes themselves. The fixes that matter most:

- **Pixel-size repair** could have rescaled micrometer-calibrated images by 0.7x; now
  it only corrects an image whose scale is provably the nominal one, and images record
  where their pixel size came from (`pixelSizeSource`). PNG repair failed on Windows
  (the reader held the file open), and PNG/JPEG repairs were not seen by the program.
- **.lif**: 16-bit colour read the wrong channels; planar fluorescence images are
  read (first channel) instead of rejected; damaged files are refused, not crashed on.
- **Decimal comma** in the objectives table was read as 0 (and a first fix read
  "0.293" as 293 in German; the final rule accepts one '.' or ',' in any locale).
- **Windows installer**: hid a refused camera driver; asked for a restart on every
  upgrade; the driver log was nearly empty. All fixed and verified in the VM.
- **Frame rate** readout averaged 1/dt, so bursty USB showed 88–395 fps for a 30 fps
  camera; now it averages the interval.
- Camera threading (V4L2 unplug crash, AVF races, libusb stop blocking the UI),
  Media Foundation row pitch, and about thirty interface fixes.

`RELEASE_NOTES.md` (Unreleased) lists them for users.

## Open items

- [ ] **Camera driver on Windows.** In the VM, Windows refused the self-signed
      driver package: *"Driver package signer is not trusted by system, and Code
      Integrity is enforced"*. It is Windows on ARM with Smart App Control in
      evaluation mode, so it is not known whether a clean x64 Windows 11 (where Smart
      App Control is also on by default) behaves the same. **Before installing on a
      new lab PC, check this.** The lasting fix is attestation signing of the driver
      package through Microsoft Partner Center (needs an EV certificate).
- [ ] **Run the DMC6200 on the Windows lab PC with 1.1.2.** It was tested on the Mac
      (libusb); the WinUSB path has only been compiled and run without the camera.
- [ ] **Auto exposure with a real turret change.** Tested by switching objective in
      software only; turning the turret (the dark moment) needs a person at the scope.
- [ ] **Check that LAS X opens a `.lif` we wrote** (unchanged from before).
- [ ] **Commit.** The work is uncommitted on `main` (61+ files). Suggested: one
      commit renormalising line endings under the new `.gitattributes`, then the
      fixes, on a branch merged with `--no-ff` as before.
- [ ] Decide on the Windows 11 ARM VM at `/Volumes/8tbmac/vms/` (unchanged).

## Build, test and release

`build.sh` and `install.sh` are the macOS/Linux counterparts of `build.ps1` and
`install.ps1` and take the same options.

```bash
# macOS   brew install qt libusb
# Linux   sudo apt install qt6-base-dev qt6-svg-dev libusb-1.0-0-dev cmake ninja-build build-essential
./build.sh --test            # Release build + the four tests
./build.sh --deploy          # bundle the Qt runtime (.app on macOS, AppImage on Linux)
./install.sh                 # build, bundle, install (/Applications, or /usr/local)
```

```powershell
.\build.ps1 -Test            # Windows: Release build + tests (-Arch arm64 for ARM)
.\build.ps1 -Deploy
```

The four tests are `lmtests` (imaging), `iotest` (file formats, `.lif`,
calibration repair), `enginetest` (acquisition against the simulator) and
`uitest` (wheel guard, interface scaling, icon set, gallery layouts, compare
alignment). `--test` / `-Test` run all four through ctest with
`QT_QPA_PLATFORM=offscreen`, so they also run over SSH. `.\tools\make_installer.ps1`
refuses to package unless all four pass.

Two macOS traps worth knowing:

- `macdeployqt` rewrites a bundle in place, so a later plain rebuild mixes
  bundled Qt with Homebrew Qt and the app dies with *"You might be loading two
  sets of Qt binaries"*. `--deploy` now works on a copy in
  `build/release/deploy/` and leaves the build tree's own bundle alone.
- Every rebuild re-signs the bundle ad-hoc, so macOS forgets the camera
  permission and asks again. An installed, properly signed build asks once.

Camera access differs per platform — the WinUSB driver on Windows
(`driver/install_driver.ps1`), a udev rule on Linux
(`driver/install_udev_rule.sh`), nothing at all on macOS. `docs/DEPLOYMENT.md`
has the per-platform checklist.

## Where the new code lives

| Module | Owns |
|---|---|
| `src/camera/usb/UsbDevice.h` | The portable USB interface; `UsbDeviceWin.cpp` is WinUSB, `UsbDeviceLibusb.cpp` is libusb for macOS and Linux |
| `src/camera/AvfCamera.mm` | UVC on macOS. No manual exposure: AVFoundation's manual mode is iOS-only, so `canSetExposure()` returns false and the panel says why |
| `src/camera/V4l2Camera.cpp` | UVC on Linux, uncompressed formats only (no JPEG decoder in `lmcore`) |
| `src/io/LifFile.cpp` | The Leica `.lif` container, read and write. Format written up in `docs/LIF_FORMAT.md` |
| `src/io/CalibrationRepair.cpp` | Finding and rewriting the recorded pixel size of images already saved |
| `src/ui/Theme.cpp` | Palette, scaled pixels (`px()`, `iconSize()`) and a `QProxyStyle` so Qt's own metrics follow the interface size |
| `src/ui/Icons.cpp` | The icon set, drawn from SVG in theme colours at four resolutions |
| `src/ui/WheelGuard.cpp` | Stops the wheel changing values; forwards it to the enclosing panel |
| `src/ui/PlatformUi.cpp` | The few things that differ per platform: revealing a file, the Trash vs the recycle bin, camera access setup |
| `src/ui/PanelGroup.cpp` | The top level of the side panels — coloured group, then collapsible sections, then controls |

Two conventions worth keeping. Any length in the interface goes through `px()`
so it follows the interface size — a literal pixel value will clip at 150 % and
above, which is how several bugs in this session were found. And a camera
reports what it can actually do (`canSetExposure()`, `canSetGain()`) rather than
offering controls that silently do nothing.
