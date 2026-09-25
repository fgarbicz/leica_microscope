"""Extract every 0x0108 sequence table (piezo positions per shot) from usbspy logs."""
import sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from transcript import parse, u32

for path in sys.argv[1:]:
    print('==', path)
    for e in parse(path):
        d = e['data']
        if e['text'].startswith('BULK OUT ep=01') and len(d) > 12 and d[0] == 0x08 and d[1] == 0x01 and u32(d, 8) == len(d[12:]) // 16:
            n = u32(d, 8)
            entries = [(u32(d, 12 + 16 * i), u32(d, 16 + 16 * i), u32(d, 20 + 16 * i)) for i in range(n)]
            shots = {}
            for reg, shot, val in entries:
                shots.setdefault(shot, {})[reg] = val
            active = [(s, v.get(0x1070), v.get(0x1071)) for s, v in sorted(shots.items()) if v.get(0x1060)]
            print(f'  table: {len(active)} active shots: ' + ' '.join(f'({x},{y})' for _, x, y in active))
