# The Leica .lif container

What LAS X saves an experiment into: one file holding many images with their
names and calibration. Read back from the files LAS X writes on this
microscope, and checked against a 442 MB file of 16 pixel-shift captures
(`BL ST 1 SPEM id.lif`). Implemented in `src/io/LifFile.cpp`.

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
  row stride. `Length` is the physical size of the whole axis **in metres**, so
  the pixel size is `Length / NumberOfElements`.
- **Resolution** is bits per sample: 8, or 16 for a deep image.

## Calibration

The 16-shot pixel-shift files this microscope produces give, for the objectives
in use:

| Objective | Pixels | Length | µm/pixel |
|---|---|---|---|
| 10x | 3840 | 1124.83 µm | 0.29292 |
| 40x | 3840 | 281.21 µm | 0.07323 |

Both are `5.86 µm / (objective × 1.0)` halved for the doubled pixel count,
i.e. the sensor pixel pitch of the IMX174 with a **1.0× camera adapter**.

## What this reader does not do

Only 2D images are read: the camera on this microscope produces nothing else.
Files with z stacks, time series, tiled scans or more than three channels list
their images correctly but only the first plane of each is read. Writing always
produces 2D, 8-bit BGR images, which is what LAS X writes for camera images.
