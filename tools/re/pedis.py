"""Small disassembly helper for reverse engineering the Jenoptik DijSDK (x86 PE)."""
import sys, pefile, capstone

def load(path):
    pe = pefile.PE(path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64 if pe.FILE_HEADER.Machine == 0x8664 else capstone.CS_MODE_32)
    md.detail = False
    return pe, md

def exports(pe):
    out = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if e.name:
                out[e.name.decode()] = pe.OPTIONAL_HEADER.ImageBase + e.address
    return out

def imports(pe):
    out = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                name = (i.name or b'ord%d' % i.ordinal).decode()
                out[i.address] = d.dll.decode() + '!' + name
    return out

def read(pe, va, n):
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    return pe.get_data(rva, n)

def dis(pe, md, va, count=80, stop_at_ret=True, imps=None, strings=True):
    code = read(pe, va, count * 16)
    lines = []
    for ins in md.disasm(code, va):
        s = f"{ins.address:08x}: {ins.mnemonic:6} {ins.op_str}"
        if imps:
            for tok in ins.op_str.replace('[', ' ').replace(']', ' ').split():
                try:
                    v = int(tok, 16)
                except ValueError:
                    continue
                if v in imps:
                    s += f"    ; {imps[v]}"
                elif strings and pe.OPTIONAL_HEADER.ImageBase <= v < pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.SizeOfImage:
                    try:
                        b = read(pe, v, 80)
                        z = b.split(b'\0')[0]
                        if len(z) >= 4 and all(32 <= c < 127 for c in z):
                            s += f'    ; "{z.decode()}"'
                        elif len(b) > 8 and b[1] == 0 and b[3] == 0:
                            w = b.decode('utf-16le', 'ignore').split('\0')[0]
                            if len(w) >= 4 and w.isprintable():
                                s += f'    ; L"{w}"'
                    except Exception:
                        pass
        lines.append(s)
        if len(lines) >= count or (stop_at_ret and ins.mnemonic in ('ret', 'retn')):
            break
    return '\n'.join(lines)

if __name__ == '__main__':
    path = sys.argv[1]
    pe, md = load(path)
    ex = exports(pe)
    imps = imports(pe)
    names = sys.argv[2:] or sorted(ex)
    for n in names:
        if n.startswith('0x'):
            print(f'== {n}')
            print(dis(pe, md, int(n, 16), 200, True, imps))
        else:
            print(f'== {n} @ {ex[n]:08x}')
            print(dis(pe, md, ex[n], 60, True, imps))
        print()
