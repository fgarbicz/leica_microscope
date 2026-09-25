# Leica DMC6200 USB protocol

The Leica DMC6200 is built by Jenoptik on the ProgRes GRYPHAX platform: a
Cypress FX3 USB 3.0 bridge, an FPGA and a Sony IMX174 global-shutter sensor
(1920 x 1200, 5.86 µm pixels, 12-bit ADC, GBRG Bayer mosaic) on a piezo stage
used for pixel shift. The camera enumerates as `VID 0x1711 PID 0x30E0` with
manufacturer string "Jenoptik Optical Systems GmbH" and product "USB 3.0 Camera".

This document was reconstructed from USB traces of the vendor SDK
(`tools/re/usbspy`) and verified with our own driver
(`src/camera/leica/Dmc6200Protocol.*`, `tools/dmctest`).

## Transport

A single vendor-class interface (class 0xFF), bound to WinUSB by
`driver/LeicaUsb3Cam.inf`.

| Endpoint | Type | Use |
|---|---|---|
| 0x01 | bulk OUT | commands |
| 0x81 | bulk IN | command responses |
| 0x82 | interrupt IN, 64 B | frame-ready events |
| 0x83 | bulk IN, burst 16 | image data |

Alternate setting 1 turns 0x83 into an isochronous endpoint; it is not used.

## Command framing

Request (little endian):

```
u16 cmd | u16 payloadLength | u32 maxResponseLength | u32 0 | payload...
```

Response on 0x81 (read `maxResponseLength + 8` bytes):

```
u16 cmd | u16 payloadLength | u16 status | u16 tag | payload...

(The tag word was 0x7BBB on one firmware boot and 0x3B3A on another; do not validate it.)
```

`status` 0 means success, and non-zero values are negative error codes
(0xFFFC for an unknown register, 0xFFE9 for an unsupported info block).
Unknown commands are echoed back with status 0xFFFF.

### Large commands

The sequence table (0x0108) does not fit the 16-bit length field, so it is
sent in two transfers:

1. `0108 0000 00010000 <totalBytes>` (the 0x10000 flag announces a large command)
2. `0108 0000 <4 + 16*N> <N>` followed by N entries of `{u32 reg, u32 shot, u32 value, u32 0}`

Response: `0108 0000 0000 7BBB`.

## Commands

| Cmd | Payload | Response |
|---|---|---|
| 0x0001 | – | u32 max response size (500) |
| 0x0002 | – | 32 bytes of capability flags |
| 0x0003 | – | serial number string |
| 0x0010 | – | image info: u32 1, u16 0, u16 width, u16 height, u8 1, u8 adcBits, u8 adcBits, u8 16, u8 16 |
| 0x0011 | u32 block | info block; 5 = sensor name "IMX174", 7 = timing |
| 0x0012 | u32 block | info block 2; 1 = firmware/board data |
| 0x0020 | u32 mode | acquisition control: 0 stop, 2 start live, 3 flush |
| 0x0101 | u32 n, n × {u32 reg, u32 value} | write registers |
| 0x0102 | u32 n, n × u32 reg | u32 n, n × {u32 reg, u32 value} |
| 0x0108 | sequence table (see above) | – |
| 0x1005 | 0x1E4-byte key/value request | board info ("BoardNumber" and so on) |
| 0x2000 | 0x84-byte request | flash data (calibration and defect maps), streamed |

## Registers

| Reg | Meaning | Default |
|---|---|---|
| 0x0001 | sensor width | 1920 |
| 0x0002 | sensor height | 1200 |
| 0x0003 | ADC bits | 12 |
| 0x0004 | ? | 4 |
| 0x0005 | ? | 0x20000 |
| **0x1012** | **exposure time, µs** (26 … 60 000 000), applied on the next frame | 20000 |
| **0x1013** | **analog gain, 16.16 fixed point** (0x10000 = 1.0), applied live | 0x10000 |
| 0x1020 | ? | 0 |
| 0x1030 / 0x1031 | ROI x / y | 0 / 0 |
| 0x1032 / 0x1033 | ROI width / height | 1920 / 1200 |
| 0x1060 | sequence: shot enable (per shot) | 1 for shot 0 |
| 0x1070 / 0x1071 | sequence: piezo X / Y position, 0…255 (per shot); 46 is the rest position | 46 / 46 |
| 0x1110 … 0x1112 | ? | 0 |

Sequence registers can only be changed while acquisition is stopped (a
`0x0108` sent during live mode is rejected). The vendor SDK always uploads 36
entries per register, one for each possible shot of the 6 × 6 pixel-shift mode.

## Streaming

1. `0x0020 0` stop, then `0x0020 3` flush
2. upload the sequence table (`0x0108`)
3. `0x0020 2` start
4. For every frame:
   * read a 64-byte event from 0x82:
     ```
     u16 type(1) u16 len(0x3C) u32 frameNumber u16 width u16 height u32 bytes
     u8 adcBits u8 bits u8 0 u8 1 u32 exposureUs u32 ? ... u16 piezoX u16 piezoY ...
     ```
   * read `bytes` (1920 × 1200 × 2 = 4 608 000) from 0x83, then the 1-byte
     trailer (0x01) that terminates the transfer.
5. Pixel format: 16-bit little endian, 12 significant bits, Bayer **GBRG**
   (row 0 = G B, row 1 = R G). The black level is about 8 DN.

Measured throughput: 53 fps at full resolution (about 245 MB/s).

## Recovery when the sensor reports "SynthSensor" or commands time out

Info block 5 (`0x0011`) names the sensor. After a USB-level reset while the
camera was powered (port reset, device removal, controller restart) the FX3
firmware may come up with `SynthSensor` instead of `IMX174` and never deliver
frames. Only removing power fixes this.

* **Do not send `0x1010` on the bulk channel.** The vendor firmware updater
  sends it (DijSDK ioctl `0x3010`) after writing firmware. On a running camera
  it stops command processing entirely: EP0 standard requests still work, but
  bulk commands time out and the class control request (bmRequestType `0x20`,
  bRequest 0, 12-byte header; DijSDK ioctl `0x3012`) stalls.
* Supported command codes (response of `0x0002`): `0001 0002 0003 0004 0101
  0102 1005 0010 0011 0012 0020 F000 1010 0108 2000 1000`. `0x1000` carries
  firmware data (120 s timeout) and `0xF000` is a debug read/write; both are
  left alone.
* **Power cycle in software:** `tools/camera_power_cycle.ps1` (run as admin)
  sends a UCSI `CONNECTOR_RESET` (hard reset) to the USB-C connector that powers
  the camera, using `UcsiControl.exe` from Microsoft's MUTT package. The PD
  controller drops VBUS, the camera reboots cleanly and reports `IMX174` again.
  The script enables the UCSI test interface only for the duration of the reset.

  **Reliability:** this recovered the stuck camera once (2026-09-25: the port
  read *disconnected* 10 s after the reset, and the camera came back with a
  fresh firmware boot). Later hard resets sent to the same port while the camera
  was working were accepted (CCI "command completed") but ignored: the port never
  disconnected, and streaming continued. The PD firmware apparently only acts on
  it in some states. `dmctest --usbc-cycle` (native implementation in
  `src/camera/usb/UsbCPower.cpp`, admin rights needed) watches the connector and
  reports whether power was really removed. If it was not, unplug the camera.

## Driver signing on PCs without the Windows SDK

`install_driver.ps1` falls back to `New-FileCatalog` + `Set-AuthenticodeSignature` when
`makecat`/`signtool` are not installed (typical lab PC). Verified on 2026-09-25: a package
with a changed INF, catalogued and signed this way with the local "DM Imaging Driver
Signing" certificate, was accepted by `pnputil /add-driver` as a new package (then removed).
