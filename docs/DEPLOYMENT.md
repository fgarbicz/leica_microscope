# DM Imaging 1.0.0 — deployment checklist

Installer: `dist\DMImaging-Setup-1.0.0.exe` (39 MB, git tag `v1.0.0`, build `a0d3c94`).

## Before you start (per PC)

- Windows 10 or 11, 64-bit; an account that can approve an administrator prompt.
- The Leica DMC6200 camera on a **USB 3.0** port (blue connector). It may be connected before
  or after installing.
- Leica LAS X may stay installed, but **must not be running** at the same time (only one
  program can use the camera).

## Install

1. Copy `DMImaging-Setup-1.0.0.exe` to the PC and run it. Approve the administrator prompt.
2. Keep both options ticked: *desktop shortcut* and *Install the camera driver*.
3. Finish with *Start DM Imaging now*.

Silent install (IT deployment): `DMImaging-Setup-1.0.0.exe /VERYSILENT /NORESTART /TASKS="desktopicon,installdriver"`
from an elevated prompt; add `/LOG="C:\Temp\dmimaging_setup.log"` for a log.

## Verify (2 minutes)

| Check | Expected |
|---|---|
| App starts | Title bar shows *Leica DMC6200 (serial)*, live image within a few seconds |
| Frame rate | Status bar ~30–60 fps (depends on exposure) |
| Colours | Background white, DAB brown, haematoxylin blue (press *Auto white balance* (F7) on an empty field first) |
| Capture | F9 → dialog asks objective + name → *Save* → file appears in *Browse* |
| Calibration | Browse → metadata shows *Pixel size* for the chosen objective |
| Version | Help → About: *DM Imaging 1.0.0, build a0d3c94* |

## If something goes wrong

| Symptom | Action |
|---|---|
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
