# DM Imaging release notes

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
