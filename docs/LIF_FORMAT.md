# The Leica .lif container

What LAS X saves an experiment into: one file holding many images with their
names, calibration, channels, z stacks, time series and tile scans. Read back from
the files LAS X writes on this microscope and from confocal files of other Leica
systems. Implemented in `src/io/LifFile.cpp`; exporting in `src/io/LifExport.cpp`.

## Layout

Little endian throughout. Strings are UTF-16LE, with a length in **code units**,
not bytes.

```
header
  int32   0x70            file marker
  uint32  size            bytes that follow in this header
  uint8   0x2A
  uint32  chars           length of the XML in UTF-16 code units
  bytes   XML             <LMSDataContainerHeader Version="2"> …

then one memory block per image, in the order the XML names them
  int32   0x70
  uint32  size            bytes that follow, up to the pixel data
  uint8   0x2A
  uint64  dataBytes       size of the pixel data  (uint32 in Version="1")
  uint8   0x2A
  uint32  chars           length of the block id
  bytes   id              "MemBlock_180"
  bytes   pixel data      dataBytes
```

The `size` fields are redundant with the rest, but LAS X writes them and this
reader checks them, because a mismatch is the clearest sign the file is not
what it claims.

## The XML

Nested `Element`s form the tree LAS X shows. An element with a `Data/Image`
child is an image; its `Memory` element names the block holding the pixels:

```xml
<Element Name="BLST1 sampl1 WT TFF2 GIF 10x">
  <Data><Image><ImageDescription>
    <Channels>
      <ChannelDescription ChannelTag="3" Resolution="8" BytesInc="0" LUTName="Blue"/>
      <ChannelDescription ChannelTag="2" Resolution="8" BytesInc="1" LUTName="Green"/>
      <ChannelDescription ChannelTag="1" Resolution="8" BytesInc="2" LUTName="Red"/>
    </Channels>
    <Dimensions>
      <DimensionDescription DimID="1" NumberOfElements="3840" Length="1.124827e-003" Unit="m" BytesInc="3"/>
      <DimensionDescription DimID="2" NumberOfElements="2400" Length="7.029070e-004" Unit="m" BytesInc="11520"/>
    </Dimensions>
  </ImageDescription></Image></Data>
  <Memory Size="27648000" MemoryBlockID="MemBlock_193"/>
</Element>
```

- **Channels.** `BytesInc` is the byte offset of that channel inside a pixel, so
  a colour image from a Leica camera is **interleaved BGR**: blue at 0, green at
  1, red at 2. Checked against the PNG LAS X exports from the same image: read
  as BGR the two agree to within ±1 (LAS X's own export rounding); read as RGB
  they differ by up to 106.
- **Dimensions.** `DimID` 1 is x and 2 is y (the sidecar XML next to an exported
  PNG says `X`/`Y` instead). `BytesInc` on x is the bytes per pixel and on y the
  row stride. `Length` is the distance **in metres** from the centre of the first
  pixel to the centre of the last, so the pixel size is
  `Length / (NumberOfElements − 1)` (see *Calibration*).
- **More dimensions.** 3 is z, 4 is time (`Length` in seconds), 5 the emission
  wavelength of a spectral scan, 9 the excitation wavelength, 10 the tiles of a
  tile scan (the "mosaic"); 6–8 are rarer. Each has its own `BytesInc`.
- **Resolution** is significant bits per sample: 8, 12 or 16 (12 is stored in two
  bytes); `DataType="1"` is 32-bit floating point.

## Where a sample is

Every channel and every dimension has a byte increment, and a sample lies at

```
channel.BytesInc + x·inc(x) + y·inc(y) + z·inc(z) + t·inc(t) + tile·inc(tile) + …
```

from the start of the image's block. That one rule covers every layout LAS X uses,
and the order of the increments differs between files: the camera image above
interleaves its channels within a pixel; a confocal z stack may store each channel
as a plane with the channels of one slice together (`Project007.lif`: channel 262144,
z 1048576 bytes apart) or each channel as a whole stack (`Cell 1`: z 262144, channel
3145728 apart). A reader that assumes one order reads the other kind in the wrong
order: Python's `readlif` 0.6.5 does, so `tools/lifcheck/check.py` checks this reader
against numpy reading straight from the increments instead.

## Tile scans

A tile scan has a dimension 10 and an attachment listing the tiles in that order:

```xml
<Attachment Name="TileScanInfo" FlipX="0" FlipY="0" SwapXY="0">
  <Tile FieldX="0" FieldY="0" PosX="0.0758103931" PosY="0.0388879510"/>
  <Tile FieldX="1" FieldY="0" PosX="0.0760318216" PosY="0.0388879510"/>
  …
```

`PosX`/`PosY` are the stage position in metres; divided by the pixel size they place
each tile, which is how the viewer merges them (falling back to the `FieldX`/`FieldY`
grid). LAS X's own merge also re-aligns the tiles on their content (by up to about
20 pixels in `Project007.lif`); that merged image is saved as an image of its own.

## Settings and time

Each image's `HardwareSetting` attachment holds an `ATLCameraSettingDefinition` or
`ATLConfocalSettingDefinition` with the objective (`ObjectiveName`,
`NumericalAperture`, `Magnification`, `Immersion`), the microscope, and for a camera
the exposure (`WideFieldChannelInfo ExposureTime`, seconds) and for a confocal the
zoom, pinhole (metres, and Airy units), scan speed and the active detectors.
`TimeStampList` holds the time of each plane as hexadecimal Windows FILETIMEs
(100 ns since 1601) separated by spaces; older files use `TimeStamp` elements with
`HighInteger`/`LowInteger`.

## Calibration

The 16-shot pixel-shift files this microscope produces give, for the objectives
in use:

| Objective | Pixels | Length | µm/pixel = Length / (n − 1) |
|---|---|---|---|
| 10x | 3840 | 1124.83 µm | 0.29300 |
| 40x | 3840 | 281.21 µm | 0.07325 |

Both are `5.86 µm / (objective × 1.0)` halved for the doubled pixel count (0.29300
and 0.07325), i.e. the sensor pixel pitch of the IMX174 with a **1.0× camera
adapter**. Dividing by n instead gives 0.29292 and 0.07323, 0.03 % off: `Length`
spans n − 1 pixel steps. (Before 1.2.0 this program divided by n, and wrote `Length`
the same way; the difference is far below anything a measurement can see.)

## Checking the reader

`tools/lifcheck/check.py` (with its conda environment, `environment.yml`) reads every
image of the files given to it with `lifinfo --raw` and compares each plane with an
independent numpy reader, then exports each file as ImageJ hyperstacks and reads them
back with `tifffile`: axes, pixels, calibration and LUTs. It passes on four public
LAS X files from the OME sample collection (confocal z stacks with 4 channels, FRAP
time series, FRET, a tile scan with z and time) and on files from this microscope.

```bash
conda env create -f tools/lifcheck/environment.yml
conda run -n dmi-lifcheck python tools/lifcheck/check.py build/release/bin/lifinfo file.lif ...
```

## What is not read

`.xlef`/`.lof` projects (LAS X's newer format with one file per image) are not read,
nor are the frame-by-frame acquisition attachments beyond the first time stamp.
Writing always produces 2D, 8- or 16-bit BGR images, which is what LAS X writes for
camera images.
