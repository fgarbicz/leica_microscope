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

The current objective is shown in the title bar. Select it in the *Microscope*
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

## 7. Browse

Choose a folder on the left. Thumbnails, a preview and all metadata (objective,
pixel size, exposure, date, …) are shown. Double-click an image to open it in
Process.

## 8. Process: measure and annotate

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

## 9. Keyboard shortcuts

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

## 10. Troubleshooting

| Problem | Solution |
|---|---|
| "No Leica camera found" | Check the USB cable (use a USB 3.0 port). Use *Tools → Install / repair camera driver*. |
| Camera disconnected | The application reconnects automatically when the camera is back. |
| Colours wrong | Press Auto white balance on an empty field. |
| Dark corners | Acquire a shading reference for this objective. |
| Measurements wrong | Check the selected objective and calibrate with a stage micrometer. |

The log file is `%APPDATA%\DM Imaging\DM Imaging\dmimaging.log`.
