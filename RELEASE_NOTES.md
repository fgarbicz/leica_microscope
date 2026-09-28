# DM Imaging release notes

## Unreleased

- **The camera adapter was wrong, and with it every nominal pixel size.** The
  calibration assumed a 0.7x C-mount adapter, a value nobody had measured. It is
  1.0x: LAS X records 1124.83 um across 3840 px at 10x and 281.21 um at 40x on this
  microscope, which is exactly 5.86 um / (objective x 1.0) for both. Everything
  saved with the nominal scale therefore recorded a pixel size 43 % too large, in
  the file itself, so ImageJ and QuPath read it too. Objectives calibrated with a
  stage micrometer were never affected.
  The setting corrects itself on first start (a value entered by hand is left
  alone), and *Tools > Correct pixel size of saved images…* rewrites the scale
  recorded in images already saved - the TIFF resolution tags, the pixel density
  and the metadata - for a whole folder at a time. The pixels are not touched.
  It only changes an image whose scale is provably the nominal one at 0.7x:
  images from a calibrated objective or with a pixel size set by hand are listed
  as left alone, with the reason, and running it twice changes nothing more.
  Images now record where their pixel size came from (nominal, calibrated or set
  by hand), so this can be told apart in future.

- **Leica .lif files can be read and written.** A .lif is the experiment file LAS X
  saves, holding many images with their names and calibration. *File → Open image*
  now accepts one and lists what is inside so an image can be picked; its pixel size
  comes from the file, so scale bars and measurements are right immediately. *File →
  Export captured images to a Leica .lif* writes a session into a single .lif.
  The format is documented in docs/LIF_FORMAT.md, read back from the files this
  microscope produces and checked against a 442 MB LAS X file of 16 images.

- **The captured images can be a vertical list.** They were always a horizontal reel
  under the live image, which suits a wide screen but shows few images and no readable
  names. *View → Captured images in a vertical list* puts them in a column beside the
  image instead, thumbnail and file name per row; the live image keeps the rest of the
  space. The choice is remembered, and both layouts follow the interface size. An empty
  strip now says what it is for rather than looking like a fault.

- **Windows build verified, and the three platforms checked against each other.**
  The Windows version was compiled with MSVC, tested, packaged and installed for the
  first time since the cross-platform change (on Windows 11 on ARM, which runs the
  x64 build under emulation), and the Linux version was built and tested on Debian 12
  and 13. The build scripts now take `-Arch arm64` for Windows on ARM, `build.ps1
  -Test` runs all four test programs as `build.sh --test` does, and Qt 6.4 is enough
  (Debian 12 and Ubuntu 24.04 ship it).

- **Capturing is easier to find and to follow.** The capture panel is now the first
  thing in Acquire's left column. The save window has a *Save in* row with *Change…* to pick the
  folder, and while an image is written a turning wheel with "Saving…" shows in the
  middle of the live image.

- **Captured images: Reel, List or Compact**, switched with three buttons above them
  (also on the View menu and in Settings). *Compact* shows small thumbnails and names
  on one line each, to see many images at once.

- **Focus peak that follows the specimen.** The peak holds through a focus sweep,
  fades slowly after, and starts again on its own on another field or objective,
  instead of keeping the sharpest value ever seen.

- **Auto exposure on an objective change** starts from the exposure that objective
  last used and no longer chases the darkness while the turret turns (it used to
  run to very long exposures and back). The default brightness target is 80 %.

**Fixes**
- Browse froze, for many seconds, on a folder in OneDrive or iCloud or with a large
  .lif: the folder tree read the start of each file to name its type, and the
  preview and folder listing ran on the interface thread. All three now happen in
  the background.
- The frame rate in the status bar read up to 400 fps for a 30 fps camera.
- The camera details said "WinUSB driver" on macOS and Linux.
- A new capture was added to the selection of the earlier ones.
- Setup reported success even when Windows refused the camera driver, which then only
  showed up as "No Leica camera found". It now says so, and keeps the driver
  installer's log next to the driver.
- Correcting the pixel size of a PNG failed on Windows ("Access is denied"), and in
  a PNG or JPEG this program kept showing the old scale after the repair.
- 16-bit colour images in a .lif were read with the wrong colours; fluorescence
  images, which store each channel separately, now open (as their first channel)
  instead of being reported as damaged. A damaged or truncated .lif is refused
  instead of crashing, and saving over an existing .lif no longer destroys it if
  the disk fills up.
- Very large stitched TIFFs (over 65535 pixels a side) could be saved but not opened.
- A pixel size typed with a decimal comma (Polish or German Windows) was read as 0,
  wiping the calibration.
- Removing an objective from the table could silently switch the current objective.
- The stage-micrometer calibration was off by 2x if the image was zoomed between the
  two clicks.
- Deleting an image that another program held open still sent its metadata and
  annotations to the Trash. Rename now checks the new name and keeps an image and
  its companion files together.
- Pressing Esc in *Settings* kept the previewed theme and interface size.
- The exposure and gain controls ignored what the camera can do, so they could be
  enabled for a camera without manual exposure, or stay disabled for one that has it.
- The zoom shortcuts only worked in *Acquire*; *Help → Keyboard shortcuts* showed
  "100%%" and the wrong Redo key on macOS and Linux.
- Annotations were written to disk on every mouse movement while drawing; they are
  now saved when you finish. Annotations added to an image opened from a .lif were
  lost without asking.
- *Reset all settings* skipped the question about an unsaved multifocus or stitched
  result.
- Colour presets with a "/" in their name were lost on restart.
- Windows: *Show in folder* could open Documents instead of the image's folder. The
  Media Foundation camera could show sheared or upside-down frames, and spun at 100 %
  CPU after the stream ended.
- macOS: a race when changing resolution, and a possible crash when closing a UVC
  camera.
- Linux: unplugging a UVC camera ended the program; installing the camera access
  rule failed on Debian and Ubuntu (it needs bash, not sh); the installed program
  did not find its own udev rule; files passed from the file manager were ignored.
- macOS and Linux: stopping live view on the DMC6200 froze the window for up to the
  exposure time plus 1.5 s. Its camera ID no longer includes the USB address, so it
  is recognised after being plugged into another port.
- Averaged captures from a camera with padded rows came out sheared.
- The camera's own serial number is recorded in the images on every platform.
- The unused *Sensor shift rest X/Y* settings of the DMC6200 were removed.

## 1.1.1 (2026-09-26)

Serial sections: finding, capturing and comparing the same area stained for
different markers.

**Reference images on a second screen.** Double-click a thumbnail in the strip below
the live image (or press Enter) to open it in the system's image viewer (Photos,
Preview or the desktop's viewer) while the live image keeps running, e.g. to find the
same area on a serial section stained with another marker. *Open in Process* moved to
the thumbnail's right-click menu. Or overlay it on the live image instead
(right-click → *Overlay on live image*, Ctrl+R), semi-transparent, and move the stage
until the two match. Afterwards, *Process → Compare two images* aligns the two
sections automatically, so synchronised zoom and pan show the same cells.

**HDR capture** (*Acquire image → Mode*): two or three exposures (1×, 4×, 16×) of the
same field merged in the raw sensor data into one 16-bit image. Dark DAB and
haematoxylin get up to 16× the signal, so they are recorded with much less noise,
while the background stays unclipped. The camera already reads 12 bits and pixel
shift already gives 3840 × 2400 and 5760 × 3600; HDR adds the dynamic range.

**Fixes**
- After an automatic reconnect the camera list showed the first camera (the simulator)
  instead of the connected one.
- The Windows installer build script found no version number after the
  cross-platform change.

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

**The interface size is adjustable**, from 75% to 200% (*File → Settings → Interface
size*, or *View → Interface size* with Ctrl+Shift+Plus / Minus / 0). It scales the
text, the controls, the icons, the spacing and the panel widths together rather than
only enlarging the font, applies immediately without restarting, and is remembered.
The list in Settings previews each size as it is selected.

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
- A coloured dot in the status bar for the camera: grey when there is none, blue when
  connected, green when live, red when the connection was lost.
- Collapsed sections show what they are set to, e.g. the camera in use and
  "20 ms · 2.0×" for the exposure.
- The Browse and Process tool bars show a label beside each icon.

**Safer acquisition and analysis.**
- *Help → User guide* (F1) opens the user guide on every platform.
- Exposure and gain are locked while a capture collects its frames or pixel-shift shots, and
  auto exposure pauses, so a capture is never taken at a changing exposure.
- A capture that receives no frames from the camera gives up with a message instead of
  waiting forever.
- Multifocus and live stitching build a new preview only when the display has taken the
  previous one, so they no longer fall behind on slower PCs; thumbnails in Browse load on
  their own threads and no longer delay saving a capture.
- Process asks before an unsaved multifocus or stitched result is replaced or the program is
  closed, and a new capture is not opened over it.
- Counting nuclei on an image without a pixel size asks to set it first.
- The batch IHC results ask before closing when nothing was exported or copied.
- Annotations that cannot be saved next to the image (read-only folder) are reported
  instead of silently lost.
- The IHC panel is split into *Stained area and H-score* and *Cell counting*.

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
