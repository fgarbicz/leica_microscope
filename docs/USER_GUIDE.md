# DM Imaging: user guide

## 1. Start up

**Installation**

- *Windows:* run `DMImaging-Setup-<version>.exe` once on the microscope PC (Windows asks
  for administrator permission). Keep *Install the camera driver* ticked. Afterwards DM
  Imaging is in the Start menu and on the desktop.
- *macOS:* open `DMImaging-macOS-arm64.dmg` and drag DM Imaging onto Applications
  (the first time, open it with right-click → *Open*). No driver is needed.
- *Linux:* make `DMImaging-Linux-x86_64.AppImage` executable and start it; then install
  the camera access rule once (*Tools → Install camera access rule*, or
  `sudo bash driver/install_udev_rule.sh`) and replug the camera.

The downloads are on the project's GitHub releases page (see the README).

DM Imaging looks and works the same on all three; only the installation differs.
Where this guide says **Ctrl**, use **Cmd** on a Mac.

1. Switch on the microscope lamp and connect the camera (USB 3.0 port, blue connector).
2. Start **DM Imaging** from the desktop or Start menu. The camera connects and
   the live image starts automatically.
3. On the first start, the white balance is set automatically. For best colour,
   move to an empty area of the slide and press **Auto white balance** (F7).

**The mouse wheel** scrolls the side panels and zooms the image. It never changes a
setting: to change a value, drag its slider, type in its box, or use the arrow keys.
This is deliberate — scrolling past a control used to alter the exposure or the
objective by accident.

**Pixel size and the camera adapter.** The nominal calibration is the sensor pixel
pitch (5.86 µm) divided by the objective magnification and the *camera adapter*
factor in the *Microscope* panel. On this microscope the adapter is **1.0×**, which
is what LAS X uses for the same objectives. Until version 1.1.2 the program assumed
0.7×, which made every nominal pixel size 43 % too large. The setting is corrected
automatically on first start; images saved before that still carry the wrong scale
in the file, and *Tools → Correct pixel size of saved images…* rewrites it for a
whole folder (the images themselves are not changed). Measuring a stage micrometer
with *Calibrate…* overrides the nominal value for that objective and is unaffected.

The repair lists every image it would change, with the old and new pixel size, and
changes nothing until you confirm. It only corrects an image whose recorded pixel
size is the nominal one at 0.7×; images from a calibrated objective, with a pixel
size set by hand, or whose scale cannot be traced back to the nominal value are
listed as *left alone*, with the reason. TIFF, PNG and BMP files are rewritten
losslessly. A JPEG cannot be changed without re-compressing it, so only its `.json`
sidecar is corrected (or created): DM Imaging then shows the right scale, but other
programs still read the old one. Running the repair again on the same folder finds
nothing more to do.

**Leica .lif files** (what LAS X saves) open in a viewer of their own: *File → Open
Leica .lif…* (Ctrl+L), double-click a `.lif` in *Browse*, drag it onto DM Imaging, or
pick it in *File → Open image*. Several files can be open at once, each in its own
window. See [Leica .lif viewer](#12-leica-lif-viewer). Going the other way, *File →
Export captured images to a Leica .lif* writes this session's captures into one .lif.

An image sent from the viewer to *Process* has no file of its own, so annotations or a
pixel size added to it are kept only by saving it (*Save as*); DM Imaging asks before
discarding them.

**Opening images from elsewhere:** images and folders can also be opened by
dragging them onto the DM Imaging icon, with *Open with* in Explorer, Finder or the
Linux file manager, or on the command line (`DMImaging image.tif`). An image opens
in *Process*, a folder in *Browse*.

**The images you capture** appear beside or under the live image. The three buttons
above them choose how:

| Button | Shows |
|---|---|
| **Reel** | a row of thumbnails under the live image |
| **List** | a column beside the image, thumbnail and file name per row |
| **Compact** | a column of small thumbnails, one line each, to see many images at once |

The same choice is in *View → Captured images* and in *File → Settings*, and it is
remembered.

**If the text is too small** (or too large), change *Interface size* in
*File → Settings*: it ranges from 75% to 200% and scales the text, the controls and
the icons together. The list previews each size as you pick it. It is also on the
*View → Interface size* menu, with Ctrl+Shift+Plus and Ctrl+Shift+Minus (Cmd on a
Mac) to step up and down and Ctrl+Shift+0 to go back to the default. The setting is
remembered and applies immediately — there is no need to restart.

The window has three workspaces (tabs at the top):

| Workspace | Purpose |
|---|---|
| **Acquire** | live image, camera and colour settings, capturing |
| **Browse** | find, preview, rename and delete saved images |
| **Process** | measure, annotate, adjust and export images |

## 2. Getting a good image

1. **Köhler illumination and focus** as usual on the DM2000.
2. **Exposure:** set the exposure time in the *Exposure* panel, or press
   **Auto once** (F8). *Auto exposure* keeps adjusting continuously. Its
   brightness target (default 80%) keeps the background just below white. When you
   change objective, the exposure that objective last used is restored first and auto
   exposure only fine-tunes from there; the moment of darkness while the turret turns
   is not chased. Keep
   the gain at 1× unless the specimen is very dark: higher gain means more noise.
3. **White balance:** press *Auto white balance* on an empty field (background), or
   use *Pick area…* and drag a rectangle over background.
   *Camera colour correction* (Colour panel, on by default) applies the sensor's calibrated
   colour matrix (IMX174 under halogen light), so DAB, haematoxylin and eosin look as in the
   eyepieces. Extra *Saturation* is rarely needed.
   **Light filter** (Colour panel): emulates a colour filter in front of the lamp. The halogen
   lamp is slightly yellow; a blue filter (*Light blue 82A*, *Medium blue 80C*, *Daylight blue
   80B*, ...) gives a cleaner, whiter-looking background and crisper stain contrast. Warming
   (81, 85) and green / magenta correction filters are also available, and *Warm ↔ Cool* and
   *Green ↔ Magenta* allow fine adjustment (0 = no filter; the ⟲ button resets).
   The filter is applied to the live image and to captures and is recorded in the image
   information. For IHC measurements use the same filter for all images of a study.
4. **Shading correction** (*Microscope* panel): with an empty field in view, press
   *Acquire reference* and tick *Apply shading correction*. This removes darker
   corners and uneven illumination. The reference is stored per objective.
5. **Histogram:** check that the curve does not touch the right edge (saturation).
   *Show over/under exposure* marks saturated pixels red.
6. **Focus assistant** (*Focus* panel): shows the sharpness and the best value seen
   ("peak"). Turn the fine focus until the bar is full and green. The peak holds while
   you turn past the best point and back, fades slowly afterwards, and starts again by
   itself when you move to another field or change the objective; *Reset peak* starts
   it by hand.

## 3. Capturing

Press **Capture image** (the blue button at the top of the left column, F9, or Space
while the image has focus). After the capture, a window asks for:

- **Objective magnification:** click 2.5×, 5×, 10×, 20×, 40× or 100×, or press the
  keys 1–6. This sets the µm/pixel calibration stored in the file and the scale bar.
- **Image name:** a name is suggested (template set in *Save settings*). Just type
  to replace it.
- **Notes** (optional), stored in the image metadata.
- **Save in**: the folder. *Change…* picks another one, which is then used for the
  following images too.

Press **Enter / Save** to save, or **Discard**. While the image is being written, a
turning wheel with "Saving…" shows in the middle of the live image; wait for it to
disappear before unplugging a USB disk or network drive the image goes to.

Capture modes (*Acquire image → Mode*):

| Mode | Result | When |
|---|---|---|
| Standard | 1920 × 1200 | everyday imaging |
| HDR, 2 exposures | 1920 × 1200, 16-bit, 4× more signal in dark areas | dark DAB / haematoxylin next to bright background |
| HDR, 3 exposures | 1920 × 1200, 16-bit, 16× more signal in dark areas | very dark stains, publication |
| 4-shot true colour | 1920 × 1200, full colour at every pixel | fine colour detail, lowest noise |
| 16-shot | 3840 × 2400 | low-magnification overviews, publication |
| 36-shot | 5760 × 3600 | maximum detail (takes a few seconds) |

**HDR** takes the same field at the current exposure and at 4× (and 16×) longer
exposures and merges them: the background comes from the short exposure, the dark
stain from the long ones, so dark areas are recorded with far less noise and the
background is not clipped. Set the exposure for the background first (auto
exposure does this), then capture. *Averaging* applies to every exposure. HDR is
available with the Leica DMC6200 on Windows, macOS and Linux; other (UVC) cameras
deliver processed images that cannot be merged this way, so they do not offer it.

Keep the microscope still during pixel-shift and HDR captures. **Averaging** (e.g. 4
frames) reduces noise in standard captures.

Images are saved as **16-bit TIFF** by default. The calibration is stored in the
file, so ImageJ/Fiji and QuPath show the correct µm scale.

Saved images appear in the strip below the live image. **Double-click** one (or
select it and press Enter) to open it in the computer's image viewer (Photos on
Windows, Preview on macOS, the desktop's viewer on Linux) in its own window, while
the live image keeps running. Drag that window to a second screen to use it as a
reference, for example to find the same area on a serial section stained with
another marker. Right-click a thumbnail to open it in Process, show it in the file
manager, rename it or delete it.

**Rename** (right-click → *Rename…*, or F2) renames the image file on disk together
with its metadata and annotations; only the name is typed, the extension stays.

**Projects.** Images are listed in the order they were taken, oldest first. In the
*List* and *Compact* layouts they are grouped under the name of the folder they were
saved in, which works as the project name: saving into another folder starts a new
project below. Double-click a project name (or right-click it) to rename it; this
renames the folder on disk, and the save folder follows. *File → Open project
folder…* (Ctrl+Shift+O) lists the images already in a folder and saves new captures
there, so a project can be continued another day.

The sizes of the panes (side panels, the image and the list of captured images,
Browse and Process) are remembered when DM Imaging is closed.

**Reference overlay** (same area on the next section): right-click a thumbnail →
*Overlay on live image*, or select it and press **Ctrl+R** (Cmd+R on a Mac). The image is shown
semi-transparently over the live image; move the stage until the two match, then
press Ctrl+R again to hide it and capture. The overlay is never part of the saved
image. Its opacity (25 / 50 / 75 %) is under *View → Reference overlay*.

## 4. Objectives and calibration

The current objective is shown in the title bar. Select it in the *Microscope*
panel or with Ctrl+1 … Ctrl+6; the post-capture window also updates it. Each
objective remembers its own exposure, gain and white balance, and they are restored
when you switch (*Remember exposure & white balance per objective* in the
Microscope panel).

The pixel size is computed from the sensor pitch and the camera adapter factor
(*Camera adapter*, 1.0× on this microscope; see section 1). For exact measurements,
calibrate each objective once with a stage micrometer: *Microscope → Calibrate…*,
click two marks, and enter their distance in µm. You may zoom between the two
clicks. The calibrated value can also be typed into the objectives table, with a
decimal point or a decimal comma.

## 5. Multifocus (extended depth of field)

For thick sections where not everything is in focus at once:
*Multifocus → Start*, then turn the fine focus slowly through the whole
specimen. The display shows the merged result as it builds up. Press *Finish &
save* when every area is sharp.

## 6. Live Image Builder (stitching)

For areas larger than one field of view: *Live image builder → Start*, then move
the stage slowly. New areas are added automatically; the blue rectangle shows the
current position, and red means the position was lost (move back over the already
scanned area). Press *Finish & save*.

## 7. Video

*Video recording* (Acquire panel): choose the frame rate, optionally include the
scale bar, and press **Record video**. Press it again to stop. The AVI file is
saved in the image folder and plays in VLC, QuickTime, Windows Media Player and PowerPoint.

## 8. Browse

Choose a folder on the left. Thumbnails, a preview and all metadata (objective,
pixel size, exposure, date, …) are shown. Double-click an image to open it in
Process, or use **Open in image viewer** to open it in a window of its own. A Leica
`.lif` previews its first image and says how many it holds; double-click it to browse
them all in the [.lif viewer](#12-leica-lif-viewer).

**Rename** and **Delete** keep an image and its companion files (the `.json`
metadata and the annotations) together. Delete moves them to the Recycle Bin / Trash;
if an image cannot be moved (for example because another program has it open), it
and its companion files stay where they are and a message lists it.

## 9. Process: measure and annotate

Tools in the toolbar:

| Tool | Use |
|---|---|
| Line | distance (µm) |
| Path | curved length. Double-click to finish. |
| Rect / Ellipse | size and area |
| Area | polygon area. Double-click to finish. |
| Angle | three clicks |
| Count | click objects to count them; right-click removes one |
| Arrow / Text | annotations |

The measurements appear in the table on the right (*Export measurements* saves a
CSV file). Annotations are saved automatically next to the image. *Export with
overlays* writes an image with the scale bar and annotations burned in, ready
for presentations.

**Compare two images** (*Process → Compare two images*, Ctrl+K) shows two images
side by side with zoom and pan synchronised, e.g. the same area stained for two
markers on serial sections. *Align images* finds how far the tissue is shifted
between them, so both panes show the same cells as you zoom and pan (shifts only,
not rotation).

### IHC quantification (DAB)

In the *IHC quantification* panel, press **Analyse image**, or draw a rectangle, ellipse
or area around the region of interest, select it, and press **Analyse selection**. The
image is separated into haematoxylin and DAB (colour deconvolution). You get
the DAB-positive percentage of the tissue, the areas, the intensity distribution
(weak / moderate / strong) and an H-score. The overlay shows DAB-positive
pixels in red and negative tissue in blue. Adjust the *DAB positivity threshold*
if needed, and use *Copy results* to paste a table row into Excel.

**Live DAB overlay:** in *Acquire*, tick *Overlays → Live DAB overlay (IHC)* to see DAB-positive
areas in red on the live image, with an approximate DAB-positive percentage in *Information*. It
uses the same stain colours and threshold on a reduced image, to help choose fields and check the
threshold; measure captured images for exact values.

**Stain colours:** the analysis uses standard haematoxylin/DAB colours. For your own
staining, open a representative image with both stains and press **Estimate stain
colours** (Macenko method); the measured colours are used for all IHC analyses, including
batch quantification and the CSV (column `stain_vectors`), until you press *Standard*.

**Counting nuclei / positive cells:** choose the *Marker* type and press **Count nuclei** (or
*Count in selection*). *Nuclear* markers (Ki-67, p53, ER/PR): every nucleus is found and counted
positive if it is DAB stained; the result is the **labelling index** (% positive nuclei).
*Cytoplasmic / membranous* markers: nuclei are found from the haematoxylin and a cell is
positive if the DAB around its nucleus exceeds the threshold. Set the typical *Nucleus diameter*
(about 7 µm for most cells) and the *Sensitivity* (*High* also finds pale nuclei). Positive
nuclei/cells are circled red, negative ones blue. `ihctool.exe` does the same from the command line.

**Many images at once:** in *Browse*, select the images (or none for the whole
folder) and press **IHC quantification…**. Each image gets one row: DAB-positive %,
weak / moderate / strong %, H-score and areas, plus the mean ± SD over all images.
If an image has rectangle, ellipse or area annotations, only those regions are
analysed (untick the option to use the whole image). *Export CSV…* writes the table
for Excel, R or Prism; *Save overlay images* writes a red/blue check image per
file into an `ihc` folder. *PDF report…* writes a report with the method (stain colours,
threshold), summary statistics, the results table and every image next to its overlay. Tick *Also count
nuclei / cells* to add cell counts and the labelling index (or % positive cells) per image,
using the marker type, nucleus size and sensitivity set in Process.

## 10. Keyboard shortcuts

*Help → Keyboard shortcuts* lists every shortcut as it applies on your computer.

| Key | Action |
|---|---|
| F1 | user guide |
| Ctrl+R | reference overlay on/off |
| F2 | rename the selected captured image |
| Ctrl+Shift+O | open a project folder |
| F5 | live on/off |
| F6 | freeze |
| F7 | auto white balance |
| F8 | auto exposure once |
| F9 / Space | capture |
| Ctrl+1…6 | select objective |
| Alt+1/2/3 | Acquire / Browse / Process |
| Ctrl+0 / Ctrl+Plus / Ctrl+Minus | fit / zoom in / zoom out (the image on screen) |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo an annotation (Ctrl+Y also redoes on Windows) |
| Mouse wheel | zoom |
| Double-click | fit / 100% |
| Ctrl+drag / middle-drag | pan |
| F11 | full screen |
| Ctrl+L | open a Leica .lif |
| Page Up / Page Down | .lif viewer: previous / next image |
| , / . | .lif viewer: z slice down / up |
| [ / ] | .lif viewer: time point back / forward |

## 11. Troubleshooting

| Problem | Solution |
|---|---|
| "No Leica camera found" | Check the USB cable (use a USB 3.0 port). On Windows use *Tools → Install / repair camera driver*; on Linux use *Tools → Install camera access rule*; on macOS no driver is needed, so just replug the camera. |
| Camera disconnected | The application reconnects automatically when the camera is back. |
| "Sensor is not ready" | Unplug the camera's USB cable for 5 seconds and plug it back in. (Resetting the USB port or restarting the computer is not enough: the camera must lose power.) |
| Linux: "Cannot open the camera: Access denied" | The udev rule is missing. Run `sudo bash driver/install_udev_rule.sh`, then unplug and replug the camera. |
| macOS: a UVC camera's exposure and gain sliders are greyed out | macOS provides no manual exposure control for UVC cameras, so that camera runs on its own automatic exposure. The Leica DMC6200 is not affected. |
| "Little bare glass in this field" (IHC) | The analysis needs some empty glass to know what "white" is. Include a little background in the image. |
| Video stopped by itself | Videos are limited to about 1.9 GB (a few minutes at full resolution); start a new recording. |
| Colours wrong | Press Auto white balance on an empty field. |
| Dark corners | Acquire a shading reference for this objective. |
| Measurements wrong | Check the selected objective and calibrate with a stage micrometer. |

The log file is `dmimaging.log`, with the previous run's in `dmimaging.1.log`. Send
both when reporting a problem:

| | Folder |
|---|---|
| Windows | `%APPDATA%\DM Imaging\DM Imaging` |
| macOS | `~/Library/Application Support/DM Imaging/DM Imaging` |
| Linux | `~/.local/share/DM Imaging/DM Imaging` |

## 12. Leica .lif viewer

A `.lif` is the experiment file LAS X saves: every image of a session, from this
microscope's camera or from a confocal or widefield system, with how it was recorded.
The viewer shows all of it.

**The file.** On the left is the file's tree as LAS X shows it, folders and images,
with a thumbnail and the size of each. Type in *Filter by name* to find an image in a
long session. Page Up / Page Down step through the images.

**The image.** It fills the middle, with the scale bar from the file's calibration;
zoom and pan as everywhere else. Under it is one slider per dimension the image has:
**Z** (with the height in µm), **T** (with the time), **Tile** for a tile scan, and any
other dimension LAS X recorded (λ for a spectral scan). The ▶ button beside a slider
plays through it. *Maximum projection along z* shows the brightest value of every
pixel through the stack. *Merge tiles* puts the tiles of a tile scan together where
the stage was (they are placed, not re-aligned; if LAS X saved its own merged image,
that is in the file too). The line under the image gives the value of every channel
under the cursor, in the image's own units (0–4095 for a 12-bit image).

**Channels.** On the right, each channel in its LUT colour, with its detector or dye
when the file names one. Untick a channel to hide it, click its colour to change it,
and set what is shown as black and as full intensity, or press the wand for an
automatic contrast. *Auto* does that for every channel, *Full range* shows them as
recorded. A colour camera image (this microscope's) opens exactly as recorded;
fluorescence opens with an automatic contrast, like ImageJ's *Auto*. The settings are
kept for each image while the file is open.

**Information.** Below the channels: size, pixel size and field of view, z step and
time interval, and the settings the image was recorded with: objective, numerical
aperture, zoom, pinhole, exposure, camera, date. *Metadata…* shows everything LAS X
recorded about the image (XML), searchable, and saves it.

**Using an image.** *Open in Process* sends the picture as shown (the visible channels
in their colours, at full resolution) to *Process* for measuring and IHC
quantification, with its calibration. *Copy* (Ctrl+C) puts it on the clipboard.

**Exporting.** *Export…* exports the image shown, the images selected in the list
(Ctrl/Shift+click, or right-click → *Export selected images*), or every image in the
file (*Export all…*), in one of two forms:

- *Original data: ImageJ TIFF* writes each image as a hyperstack with every channel,
  z slice and time point, unscaled (8- or 16-bit), calibrated in µm with the z step and
  time interval, and with the channel colours and display ranges. Fiji/ImageJ opens it
  as a hyperstack; QuPath, napari and Python (tifffile) read it too. This is the form
  to measure in. A tile scan is one merged mosaic or a file per tile; a stack too
  large for one TIFF (4 GB) is split into a file per time point.
- *Pictures as shown* writes colour pictures with the channels, colours and contrast
  set in the viewer, as JPEG, PNG or TIFF, optionally with a scale bar, for slides and
  reports: of the plane shown, of every plane, or a maximum projection along z.

The files go into a new folder named after the `.lif` (unless unticked) and are named
after the images and their folders in the file (`Folder - Image_z03_t001.tif`). With
the summary ticked, `images.csv` lists every image exported (size, channels, z, t,
pixel size, objective, date, files) and `<file>_metadata.xml` holds everything LAS X
recorded. The export runs in the background and can be cancelled.

Large files: nothing is read until it is needed, and in the background, so a
gigabyte `.lif` (or one in OneDrive that has to download first) opens without the
window freezing. An image too large to show at once (a big merged tile scan) is shown
at a reduced resolution, which the status line says; exports and *Open in Process*
always use every pixel.
