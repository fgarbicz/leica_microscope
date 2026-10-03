"""Checks DM Imaging's .lif reader and ImageJ export against an independent reader.

    conda env create -f tools/lifcheck/environment.yml
    conda run -n dmi-lifcheck python tools/lifcheck/check.py LIFINFO file.lif [file.lif ...]

LIFINFO is the lifinfo program from the build (build/release/bin/lifinfo).
For every image, every channel of (up to 24) planes is read by lifinfo --raw
and must be identical to a reference read here with numpy straight from the
byte increments in the XML. readlif is consulted as well, but it ignores the
channel BytesInc and assumes channels are stored one after another, so where a
file interleaves channels per z slice its planes come out in another order;
that is reported, not counted as a failure. Then every image is exported as an
ImageJ hyperstack (lifinfo --export) and read back with tifffile: the axes,
the pixels, the calibration and the channel colours must match the .lif.
Exit status 0 when everything matched.
"""

import itertools
import os
import struct
import subprocess
import xml.etree.ElementTree as ET
import sys
import tempfile

import numpy as np
import tifffile
from readlif.reader import LifFile

MAX_PLANES = 24


def lifinfo_raw(lifinfo, path, image, channel, coord, shape, mosaic=False):
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as f:
        out = f.name
    try:
        cmd = [lifinfo, path, "--mosaic" if mosaic else "--raw", str(image), str(channel), out] + [str(i) for i in coord]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        w, h = (int(v) for v in r.stdout.split()[0:3:2])
        data = np.fromfile(out, dtype="<u2").reshape(h, w)
        if shape is not None and data.shape != shape:
            raise RuntimeError(f"shape {data.shape} != {shape}")
        return data
    finally:
        os.unlink(out)


def lif_header(path):
    """The XML and every memory block's (offset, size), read independently of DM Imaging."""
    with open(path, "rb") as f:
        _, _ = struct.unpack("<iI", f.read(8))
        f.read(1)
        (n,) = struct.unpack("<I", f.read(4))
        xml = f.read(2 * n).decode("utf-16-le")
        version = int(ET.fromstring(xml).get("Version", "2"))
        blocks = {}
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            code, _ = struct.unpack("<iI", head)
            if code != 0x70:
                break
            f.read(1)
            (size,) = struct.unpack("<Q", f.read(8)) if version >= 2 else struct.unpack("<I", f.read(4))
            f.read(1)
            (n,) = struct.unpack("<I", f.read(4))
            bid = f.read(2 * n).decode("utf-16-le")
            blocks[bid] = (f.tell(), size)
            f.seek(size, 1)
    return ET.fromstring(xml), blocks


def image_elements(root, blocks):
    """The Elements holding pixels, in document order (the order lifinfo numbers them)."""
    out = []
    for el in root.iter("Element"):
        image = el.find("Data/Image")
        mem = el.find("Memory")
        if image is None or mem is None:
            continue
        bid = mem.get("MemoryBlockID")
        if bid in blocks and blocks[bid][1] > 0:
            out.append(el)
    return out


def numpy_plane(path, el, blocks, channel, index):
    """One channel of one plane from the byte increments: index maps DimID -> position."""
    desc = el.find("Data/Image/ImageDescription")
    chans = desc.findall("Channels/ChannelDescription")
    dims = {int(d.get("DimID")): d for d in desc.findall("Dimensions/DimensionDescription")}
    res = int(chans[channel].get("Resolution"))
    sb = 1 if res <= 8 else 2
    off = blocks[el.find("Memory").get("MemoryBlockID")][0] + int(chans[channel].get("BytesInc"))
    for did, d in dims.items():
        if did not in (1, 2):
            off += index.get(did, 0) * int(d.get("BytesInc"))
    w, h = int(dims[1].get("NumberOfElements")), int(dims[2].get("NumberOfElements"))
    xinc, yinc = int(dims[1].get("BytesInc")), int(dims[2].get("BytesInc"))
    mm = np.memmap(path, dtype=np.uint8, mode="r")
    out = np.empty((h, w), np.uint16)
    for y in range(h):
        row = mm[off + y * yinc: off + y * yinc + (w - 1) * xinc + sb]
        if sb == 1:
            out[y] = row[::xinc]
        else:
            out[y] = row[0::xinc].astype(np.uint16) | (row[1::xinc].astype(np.uint16) << 8)
    return out


def our_dims(lifinfo, path):
    """The images as lifinfo lists them: name, plane dimension ids and sizes."""
    r = subprocess.run([lifinfo, path], capture_output=True, text=True, check=True)
    images, cur = [], None
    for line in r.stdout.splitlines():
        s = line.strip()
        if s.startswith("[") and "]" in s:
            cur = {"name": s.split("] ", 1)[1].split("  ")[0], "dims": []}
            images.append(cur)
        elif cur is not None and ": " in s and ", inc " in s and not s.startswith("ch "):
            label, rest = s.split(": ", 1)
            cur["dims"].append((label, int(rest.split(",")[0])))
    return images


def check_file(lifinfo, path):
    failures = 0
    lif = LifFile(path)
    root, blocks = lif_header(path)
    elements = image_elements(root, blocks)
    ours = our_dims(lifinfo, path)
    theirs = list(lif.get_iter_image())
    print(f"{os.path.basename(path)}: {len(ours)} images (numpy reference {len(elements)}, readlif {len(theirs)})")
    if len(ours) != len(elements):
        print("  the number of images differs")
        return 1
    by_name = {}
    for im in theirs:
        by_name.setdefault(im.name.split("/")[-1], []).append(im)
    ids = {"Z": 3, "T": 4, "λ": 5, "Tile": 10}
    for idx, (mine, el) in enumerate(zip(ours, elements)):
        if el.get("Name") != mine["name"]:
            print(f"  [{idx}] {mine['name']}: the reference has {el.get('Name')} here")
            failures += 1
            continue
        labels = [d[0] for d in mine["dims"]]
        sizes = [d[1] for d in mine["dims"]]
        nch = len(el.findall("Data/Image/ImageDescription/Channels/ChannelDescription"))
        planes = list(itertools.product(*[range(n) for n in sizes]))
        if len(planes) > MAX_PLANES:
            step = len(planes) / MAX_PLANES
            planes = [planes[int(i * step)] for i in range(MAX_PLANES)] + [planes[-1]]
        cands = by_name.get(mine["name"], [])
        im = cands.pop(0) if cands else None
        bad = readlif_order = 0
        for coord in planes:
            index = {ids.get(l, -1): i for l, i in zip(labels, coord)}
            for c in range(nch):
                ref = numpy_plane(path, el, blocks, c, index)
                got = lifinfo_raw(lifinfo, path, idx, c, list(coord), ref.shape)
                if not np.array_equal(got, ref):
                    diff = np.abs(got.astype(int) - ref.astype(int))
                    print(f"  [{idx}] {mine['name']} {coord} c{c}: {np.count_nonzero(diff)} pixels differ, max {diff.max()}")
                    bad += 1
                    continue
                if im is not None and not (set(labels) - {"Z", "T", "Tile"}):
                    z, t, m = index.get(3, 0), index.get(4, 0), index.get(10, 0)
                    other = np.asarray(im.get_frame(z=z, t=t, c=c, m=m))
                    if other.ndim == 3:
                        other = other[..., 0]
                    if not np.array_equal(other.astype(np.uint16), ref):
                        readlif_order += 1
        note = f"; readlif reads {readlif_order} of them in another order" if readlif_order else ""
        status = "ok" if bad == 0 else f"{bad} planes DIFFER"
        print(f"  [{idx}] {mine['name']}: {len(planes)} planes x {nch} channels {status}{note}")
        failures += bad
    return failures


def check_export(lifinfo, path):
    """The ImageJ hyperstacks against readlif, image by image."""
    failures = 0
    lif = LifFile(path)
    root, blocks = lif_header(path)
    elements = image_elements(root, blocks)
    elements_by_path = {}
    parents = {child: parent for parent in root.iter("Element") for child in parent.findall("Children/Element")}
    for el in elements:
        names, p = [el.get("Name")], parents.get(el)
        while p is not None and p in parents:  # the top Element is the file itself
            names.insert(0, p.get("Name"))
            p = parents.get(p)
        elements_by_path[" - ".join(names)] = el
    with tempfile.TemporaryDirectory() as out:
        r = subprocess.run([lifinfo, path, "--export", out, "--tiles"], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  export failed: {r.stdout} {r.stderr}")
            return 1
        written = [l[len("wrote "):] for l in r.stdout.splitlines() if l.startswith("wrote ") and l.endswith(".tif")]
        theirs = {}
        for im in lif.get_iter_image():
            # readlif names an image in a folder "folder/name"; the export "folder - name"
            theirs.setdefault(im.name.replace("/", " - "), []).append(im)
        for f in written:
            with tifffile.TiffFile(f) as tf:
                ij = tf.imagej_metadata or {}
                arr = tf.asarray()
                series_axes = tf.series[0].axes
                page = tf.pages[0]
                xres = page.tags["XResolution"].value
            base = os.path.basename(f)
            # which image: the longest image name the file name starts with
            match = None
            for name, ims in theirs.items():
                safe = "".join("_" if ch in '\\/:*?"<>|' else ch for ch in name)
                if base.startswith(safe):
                    if match is None or len(name) > len(match):
                        match = name
            if match is None:
                print(f"  {base}: no image of that name")
                failures += 1
                continue
            im = theirs[match][0]
            x, y, z, t, m = im.dims[:5]
            c = im.channels
            tile = 0
            if "_tile" in base:
                tile = int(base.split("_tile")[1].split(".")[0].split("_")[0]) - 1
            # tifffile: T Z C Y X with the axes of size 1 left out
            expect = [n for n, k in ((t, "T"), (z, "Z"), (c, "C")) if n > 1] + [y, x]
            if list(arr.shape) != expect:
                print(f"  {base}: shape {arr.shape} axes {series_axes}, expected {expect}")
                failures += 1
                continue
            full = arr.reshape(t, z, c, y, x)
            el = next(e for e in elements if e.get("Name") == match.split(" - ")[-1]
                      and e is elements_by_path.get(match, e))
            bad = 0
            for (zi, ti, ci) in itertools.islice(itertools.product(range(z), range(t), range(c)), 48):
                ref = numpy_plane(path, el, blocks, ci, {3: zi, 4: ti, 10: tile})
                if not np.array_equal(full[ti, zi, ci], ref.astype(full.dtype)):
                    bad += 1
            scale = im.scale[0]  # pixels per micron: (n - 1) / length, as LAS X defines it
            got_scale = xres[0] / xres[1]
            cal_ok = scale is None or abs(got_scale - scale) / scale < 1e-4
            unit_ok = scale is None or ij.get("unit") in ("micron", "um", "\\u00B5m")
            luts_ok = c == 1 or ("LUTs" in ij and len(ij["LUTs"]) == c)
            if bad or not cal_ok or not unit_ok or not luts_ok:
                print(f"  {base}: {bad} planes differ, scale {got_scale:.5g} vs {scale}, unit {ij.get('unit')}, "
                      f"luts {len(ij.get('LUTs', []))}/{c}")
                failures += 1
            else:
                print(f"  {base}: {series_axes} {arr.shape} {arr.dtype} ok")
    return failures


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    lifinfo = sys.argv[1]
    failures = 0
    for path in sys.argv[2:]:
        failures += check_file(lifinfo, path)
        failures += check_export(lifinfo, path)
    print("PASSED" if failures == 0 else f"FAILED ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
