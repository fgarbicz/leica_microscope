# DM Imaging release notes

## 1.1.0 (2026-09-25)

**macOS and Linux support.** DM Imaging is now one program that builds and runs on
Windows, macOS 12+ and Linux, with the same features and the same interface.

- The native DMC6200 driver now talks to the camera through libusb on macOS and Linux
  and through WinUSB on Windows, behind one shared USB interface. Full manual control
  of exposure, gain, ROI and pixel-shift capture on all three.
- UVC cameras use Media Foundation (Windows), AVFoundation (macOS) or Video4Linux2
  (Linux). macOS has no manual exposure API for UVC devices, so those controls are
  disabled and say so rather than silently doing nothing.
- macOS: signed application bundle with the Qt runtime and libusb inside; no driver to
  install. Linux: desktop entry, icon and a udev rule (`driver/install_udev_rule.sh`)
  for camera access without root, installable from the Tools menu.
- `build.sh` and `install.sh` are the macOS/Linux counterparts of `build.ps1` and
  `install.ps1` and take the same options.
- Platform differences (file manager wording, the recycle bin vs. the Trash, the
  Settings/Quit/About menu entries macOS moves into the application menu) are handled
  where they belong instead of being spelled "Explorer" everywhere.

**The mouse wheel no longer changes settings.** Scrolling a side panel used to alter
whatever slider, spin box, combo box or tab happened to be under the pointer — silently
changing the exposure, the objective or an analysis threshold. The wheel now always
scrolls the panel (and still zooms the image); values are changed by dragging, typing or
the arrow keys.

**Reorganised interface.**
- The side panels now have three levels instead of one flat list: a coloured, named group
  (Camera, Microscope, Capture, Advanced acquisition, Image, Adjust, Overlays), the
  collapsible sections inside it, and the controls. The four multi-frame modes
  (multifocus, live stitching, video, time lapse) moved out of the capture list into their
  own group.
- Panels follow the workflow: camera, then microscope, then capture on the left; what the
  image is, how it is adjusted and what is drawn over it on the right.
- An icon set drawn in the theme's colours throughout: workflow tabs, panel groups,
  sections, buttons, menus and toolbars.
- Reworked dark and light palettes with semantic colours, so a warning (an uncalibrated
  objective, for instance) reads as a warning. A collapsed section now shows a summary,
  e.g. the selected objective and its µm/pixel.
- Consistent styling of every control, and a per-platform base font size so the Windows,
  macOS and Linux builds look alike.

**Fixes**
- The file browser no longer reads the image folder at startup; on macOS that asked for
  permission to the Pictures folder before the user had done anything.
- Long control labels are no longer clipped at macOS and Linux font sizes.
- The image area keeps a dark surround in the light theme, so a bright frame does not
  change how a stain looks.

## 1.0.1 (2026-09-25)

- **Light filter** (Colour panel): emulates colour filters in front of the lamp, from
  strong blue (80A) through daylight / light blue (80B, 80C, 82A, 82) to warming (81, 81B,
  85, 85B) and green / magenta correction, plus fine *Warm ↔ Cool* and *Green ↔ Magenta*
  sliders. Makes the slightly yellow halogen light look like daylight. Applied to the live
  image and captures, saved with presets and settings, and recorded in the image metadata.

## 1.0.0 (2026-09-25)

First release for the Leica DM2000 with the Leica DMC6200 camera.

**Acquisition**
- Native USB driver for the DMC6200 (no Leica software needed): live image up to ~59 fps at
  1920 × 1200, exposure, gain, ROI, auto exposure, white balance, shading correction.
- Calibrated camera colour correction (IMX174 under halogen light).
- Capture with prompt for objective and image name; 16-bit TIFF with µm/pixel calibration
  (readable by ImageJ/Fiji and QuPath); pixel-shift modes (4, 16 and 36 shots, up to
  5760 × 3600); frame averaging; multifocus (extended depth of field); live image builder
  (stitching); time lapse; video recording.

**Analysis**
- Measurements and annotations (length, area, angle, counts), export with overlays.
- IHC quantification (DAB): colour deconvolution, positive area, intensity classes, H-score,
  stain colour estimation, nucleus / positive-cell counting (labelling index), live DAB
  overlay, batch quantification with CSV and PDF report.

**Deployment**
- Windows installer with optional camera driver installation and uninstaller.

**Known limitations**
- If the camera reports "Sensor is not ready", it must be power-cycled (unplug for 5 s).
- IHC results depend on stain colours and the DAB threshold: use the same settings, and
  images taken with the same version, for all slides of a study.
- Images from before the colour correction (captured on 2026-09-24/25 during development)
  have different colours; batch IHC warns when such images are mixed.
- Videos are limited to about 1.9 GB per file.
