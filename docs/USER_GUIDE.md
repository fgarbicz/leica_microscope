# DM Imaging: user guide

## 1. Start up

1. Switch on the microscope lamp and connect the camera (USB 3.0 port, blue connector).
2. Start **DM Imaging** from the desktop or Start menu. The camera connects and
   the live image starts automatically.
3. On the first start, the white balance is set automatically. For best colour,
   move to an empty area of the slide and press **Auto white balance** (F7).

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
   brightness target (default 85%) keeps the background just below white. Keep
   the gain at 1× unless the specimen is very dark: higher gain means more noise.
3. **White balance:** press *Auto white balance* on an empty field (background), or
   use *Pick area…* and drag a rectangle over background.
   *Camera colour correction* (Colour panel, on by default) applies the sensor's calibrated
   colour matrix (IMX174 under halogen light), so DAB, haematoxylin and eosin look as in the
   eyepieces. Extra *Saturation* is rarely needed.
4. **Shading correction** (*Microscope* panel): with an empty field in view, press
   *Acquire reference* and tick *Apply shading correction*. This removes darker
   corners and uneven illumination. The reference is stored per objective.
5. **Histogram:** check that the curve does not touch the right edge (saturation).
   *Show over/under exposure* marks saturated pixels red.
6. **Focus assistant** (*Focus* panel): shows a sharpness bar. Turn the fine focus
   until it reaches its peak.

## 3. Capturing

Press **Capture image** (F9, or Space while the image has focus). After the
capture, a window asks for:

- **Objective magnification:** click 2.5×, 5×, 10×, 20×, 40× or 100×, or press the
  keys 1–6. This sets the µm/pixel calibration stored in the file and the scale bar.
- **Image name:** a name is suggested (template set in *Save settings*). Just type
  to replace it.
- **Notes** (optional), stored in the image metadata.

Press **Enter / Save** to save, or **Discard**.

Capture modes (*Acquire image → Mode*):

| Mode | Result | When |
|---|---|---|
| Standard | 1920 × 1200 | everyday imaging |
| 4-shot true colour | 1920 × 1200, full colour at every pixel | fine colour detail, lowest noise |
| 16-shot | 3840 × 2400 | low-magnification overviews, publication |
| 36-shot | 5760 × 3600 | maximum detail (takes a few seconds) |

Keep the microscope still during pixel-shift captures. **Averaging** (e.g. 4
frames) reduces noise in standard captures.

Images are saved as **16-bit TIFF** by default. The calibration is stored in the
file, so ImageJ/Fiji and QuPath show the correct µm scale.

## 4. Objectives and calibration

The current objective is shown in the title bar. Each objective remembers its
own exposure, gain and white balance; they are restored when you switch
(*Remember exposure & white balance per objective* in the Microscope panel).
 Select it in the *Microscope*
panel or with Ctrl+1 … Ctrl+6. The post-capture window also updates it.

The pixel size is computed from the camera adapter factor (*Camera adapter*,
default 0.7×). For exact measurements, calibrate each objective once with a stage
micrometer: *Microscope → Calibrate…*, click two marks, and enter their distance in µm.

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
saved in the image folder and plays in Windows Media Player, VLC and PowerPoint.

## 8. Browse

Choose a folder on the left. Thumbnails, a preview and all metadata (objective,
pixel size, exposure, date, …) are shown. Double-click an image to open it in
Process.

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

| Key | Action |
|---|---|
| F5 | live on/off |
| F6 | freeze |
| F7 | auto white balance |
| F8 | auto exposure once |
| F9 / Space | capture |
| Ctrl+1…6 | select objective |
| Alt+1/2/3 | Acquire / Browse / Process |
| Mouse wheel | zoom |
| Double-click | fit / 100% |
| Ctrl+drag / middle-drag | pan |
| F11 | full screen |

## 11. Troubleshooting

| Problem | Solution |
|---|---|
| "No Leica camera found" | Check the USB cable (use a USB 3.0 port). Use *Tools → Install / repair camera driver*. |
| Camera disconnected | The application reconnects automatically when the camera is back. |
| "Sensor is not ready" | Unplug the camera's USB cable for 5 seconds and plug it back in. |
| Colours wrong | Press Auto white balance on an empty field. |
| Dark corners | Acquire a shading reference for this objective. |
| Measurements wrong | Check the selected objective and calibrate with a stage micrometer. |

The log file is `%APPDATA%\DM Imaging\DM Imaging\dmimaging.log`.
