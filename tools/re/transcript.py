"""Condense a usbspy log into a readable command/response transcript."""
import re, sys

def parse(path):
    events = []
    cur = None
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*([\d.]+) (.*)', line)
        if m and not line.startswith('      '):
            cur = {'t': float(m.group(1)), 'text': m.group(2).strip(), 'data': []}
            events.append(cur)
        elif line.startswith('      ') and cur is not None:
            parts = line.split(':', 1)
            if len(parts) == 2 and re.fullmatch(r'\s*[0-9a-f]{4}', parts[0]):
                cur['data'] += [int(x, 16) for x in parts[1].split()]
    return events

def u16(d, o): return d[o] | d[o + 1] << 8
def u32(d, o): return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24

def main(path, show_stream=False):
    for e in parse(path):
        t, text, d = e['t'], e['text'], e['data']
        if text.startswith('=========='):
            print(f"\n{t:9.1f} >>> {text.strip('= ')}")
        elif text.startswith('BULK OUT ep=01'):
            if len(d) >= 12:
                cmd, plen, rlen = u16(d, 0), u16(d, 2), u32(d, 4)
                payload = d[12:]
                words = [u32(payload, i) for i in range(0, len(payload) - 3, 4)]
                ws = ' '.join(f'{w:x}' for w in words[:24]) + (' ...' if len(words) > 24 else '')
                print(f"{t:9.1f}  -> cmd {cmd:04x} plen={plen} resp={rlen:x} | {ws}")
        elif text.startswith('BULK IN  ep=81'):
            if len(d) >= 8:
                cmd, plen, st, magic = u16(d, 0), u16(d, 2), u16(d, 4), u16(d, 6)
                payload = d[8:]
                words = [u32(payload, i) for i in range(0, len(payload) - 3, 4)]
                ws = ' '.join(f'{w:x}' for w in words[:24]) + (' ...' if len(words) > 24 else '')
                asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in payload[:64])
                print(f"{t:9.1f}  <- cmd {cmd:04x} plen={plen} status={st:x} | {ws}   '{asc}'")
        elif 'ep=82' in text and 'DONE' in text and d:
            print(f"{t:9.1f}  EVT frame={u32(d,4)} {u16(d,8)}x{u16(d,10)} bytes={u32(d,12)} bits={d[16]},{d[17]} exp={u32(d,20)} f24={u32(d,24):x} f32={u32(d,32):x} gains={u16(d,36)},{u16(d,38)} f48={u32(d,48)}")
        elif 'ep=83' in text and show_stream:
            print(f"{t:9.1f}  {text}")
        elif text.startswith(('set_', 'claim', 'release', 'libusb_', 'reset')):
            print(f"{t:9.1f}  {text}")

if __name__ == '__main__':
    main(sys.argv[1], len(sys.argv) > 2)
