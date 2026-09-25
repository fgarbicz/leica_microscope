# DM Imaging

Image acquisition and analysis software for the **Leica DM2000** bright-field
microscope with the **Leica DMC6200** camera. It replaces Leica LAS X for
daily imaging and includes its own native camera driver.

![workflow](docs/workflow.png)

## Features

**Camera and driver**
- Native USB 3.0 driver for the DMC6200 (Jenoptik GRYPHAX platform, Sony IMX174,
  1920 × 1200, 12-bit). No Leica software is needed. See [docs/DMC6200_PROTOCOL.md](docs/DMC6200_PROTOCOL.md).
- Live image at up to 50 fps at full resolution, with a smooth zoom/pan view, minimap and pixel readout.
- Exposure from 26 µs to 60 s, analog gain up to 16×, and auto exposure (continuous or once).
- Centre ROI mode for faster frame rates.
- Pixel-shift capture:
  - 4-shot true colour, where every pixel records measured R, G and B
  - 16-shot 3840 × 2400
  - 36-shot 5760 × 3600
- Automatic reconnection if the USB connection drops.
- Video recording of the live image (Motion-JPEG AVI, optional scale bar).
- Also supports any UVC/DirectShow camera and includes a simulator.

**Image quality**
- 16-bit linear processing. Captures use high-quality (Malvar–He–Cutler) demosaicing.
- White balance: automatic (bright-field background) or picked from a region.
- Black balance, levels, gamma, brightness, contrast, saturation, hue, sharpening, monochrome.
- Colour presets (IHC/DAB, H&E, publication), plus your own.
- Shading (flat-field) correction, stored per objective.
- Frame averaging for low-noise captures.
- Over/under-exposure display, live histogram with draggable level handles, focus assistant.

**Microscope**
- Objectives 2.5×, 5×, 10×, 20×, 40× and 100×.
- Calibration is nominal (from the camera adapter) or measured with a stage micrometer.
- After every capture, a dialog asks for the **magnification and image name**.
- Exposure, gain and white balance are remembered per objective.
- Calibrated scale bar, grid and crosshair overlays.

**Advanced acquisition**
- Multifocus (extended depth of field) from a manual focus sweep, with drift compensation.
- Live Image Builder: stitching while you move the stage by hand.
- Time lapse.

**Browse and process**
- File browser with thumbnails, preview and full metadata.
- Measurements: length, path, rectangle, ellipse, polygon area, angle, cell counting, arrows and text.
  - Undo/redo; annotations are stored next to the image.
  - CSV export of measurements.
- Non-destructive adjustments.
- Export with burned-in scale bar and annotations; copy to clipboard; print.
- Multifocus and stitching from existing image files.

**Files**
- TIFF output: 16-bit or 8-bit, lossless Deflate, with the calibration in the
  resolution tags (ImageJ/Fiji/QuPath read the µm scale) and all acquisition
  metadata as JSON in ImageDescription.
- PNG (8/16-bit), JPEG and BMP, with a JSON sidecar.

## Install

1. **Camera driver (once per PC):** run `driver\install_driver.ps1`, or use
   *Tools → Install / repair camera driver* in the app. Windows asks for
   administrator rights once. It binds Microsoft's WinUSB driver to the camera;
   no kernel code is installed.
2. **Application:** run `install.ps1`. It builds the app, installs it to
   `%LOCALAPPDATA%\Programs\DM Imaging` and creates Start-menu and desktop shortcuts.

## Build from source

Requirements: Visual Studio 2022 Build Tools (C++), Windows SDK, Qt 6.5 or newer (MSVC 2022 x64).

```powershell
.\build.ps1 -Test          # Release build + unit tests
.\build.ps1 -Deploy        # also copies the Qt runtime next to the exe
build\release\bin\lmtests.exe --hw     # hardware test with the camera connected
build\release\bin\enginetest.exe       # acquisition engine integration test (simulator)
```

## Project layout

| Path | Contents |
|---|---|
| `src/camera/` | camera abstraction, DMC6200 driver (`leica/`), WinUSB wrapper, Media Foundation, simulator |
| `src/imaging/` | demosaicing, colour pipeline, shading, analysis, registration, focus stacking, stitching, pixel shift |
| `src/io/` | TIFF encoder/decoder, image I/O, metadata |
| `src/app/` | acquisition engine, settings, calibration, `main.cpp` |
| `src/ui/` | Qt user interface |
| `driver/` | WinUSB INF and installer |
| `tests/` | unit, hardware and integration tests |
| `tools/` | build helpers and reverse-engineering tools used to document the camera protocol |
| `docs/` | user guide and protocol documentation |

See [docs/USER_GUIDE.md](docs/USER_GUIDE.md) for day-to-day use.
