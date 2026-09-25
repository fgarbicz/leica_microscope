"""Extract a raw frame from usbspy_stream.bin and determine the Bayer layout."""
import numpy as np
from PIL import Image
import sys

W, H = 1920, 1200
FRAME = W * H * 2
path = sys.argv[1] if len(sys.argv) > 1 else 'usbspy_stream.bin'
blob = np.fromfile(path, dtype=np.uint8)
# stream = [frame bytes][1 trailer byte] repeated
raw = blob[:FRAME].view('<u2').reshape(H, W)
print('trailer byte after frame 0:', blob[FRAME], ' max value', raw.max(), ' min', raw.min())
print('2x2 means (y%2,x%2):')
for y in (0, 1):
    print('  ', [float(raw[y::2, x::2].mean()) for x in (0, 1)])

sdk = np.fromfile('frame_0.bin', dtype=np.uint8)[:W * H * 3].reshape(H, W, 3).astype(float)
for c in range(2):
    ch = sdk[:, :, c]
    best = []
    for y in (0, 1):
        for x in (0, 1):
            sub = raw[y::2, x::2].astype(float)
            ref = ch[y::2, x::2]
            r = np.corrcoef(sub.ravel(), ref.ravel())[0, 1]
            best.append(((y, x), round(r, 4)))
    print('SDK channel', c, 'corr with raw sites:', best)

def demosaic_simple(raw, pattern):
    # half-resolution demosaic for inspection
    offs = {'R': None, 'B': None}
    p = pattern
    pos = {(0, 0): p[0], (0, 1): p[1], (1, 0): p[2], (1, 1): p[3]}
    r = [raw[y::2, x::2] for (y, x), c in pos.items() if c == 'R'][0].astype(float)
    b = [raw[y::2, x::2] for (y, x), c in pos.items() if c == 'B'][0].astype(float)
    g = np.mean([raw[y::2, x::2] for (y, x), c in pos.items() if c == 'G'], axis=0)
    img = np.stack([r, g, b], -1)
    img = img / np.percentile(img.reshape(-1, 3), 99.5, axis=0)  # auto white balance
    img = np.clip(img, 0, 1) ** (1 / 2.2)
    return (img * 255).astype(np.uint8)

for pat in ('RGGB', 'GRBG', 'GBRG', 'BGGR'):
    Image.fromarray(demosaic_simple(raw, pat)).save(f'raw_{pat}.png')
print('saved raw_*.png')
