"""shino_vifwalk.py <ram.bin> <trace.txt> [first] [count]: the VIF1 commands each DMA tag of SHINO_TRACE_DMA=<flip> carries (the RAM dumped at that frame with SHINO_DUMP_RAM_AT)."""
import struct, sys
ram = open(sys.argv[1], 'rb').read()
tags = [l.split() for l in open(sys.argv[2]) if l.startswith('[shino:dma]')]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 200
NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3', 0x07: 'MARK',
         0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT', 0x20: 'STMASK',
         0x30: 'STROW', 0x31: 'STCOL', 0x4a: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
VN = ['S', 'V2', 'V3', 'V4']; VL = ['32', '16', '8', '5']
def codes(data, tagwords):
    out = []; i = 0; words = struct.unpack_from(f'<{len(data)//4}I', data)
    while i < len(words):
        w = words[i]; cmd = (w >> 24) & 0x7f; imm = w & 0xffff; num = (w >> 16) & 0xff; i += 1
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3; n = num or 256
            bits = (32 >> vl) * (vn + 1) if vl != 3 else 16
            words_needed = (n * bits + 31) // 32
            out.append(f'UNPACK {VN[vn]}-{VL[vl]}{"m" if cmd & 0x10 else ""} n={n} @{imm & 0x3ff:#x}{"+tops" if imm & 0x8000 else ""}'); i += words_needed
        elif cmd in (0x50, 0x51):
            out.append(f'DIRECT {imm}qw'); i = ((i + 3) // 4) * 4 + imm * 4
        elif cmd == 0x4a:
            out.append(f'MPG {num or 256} @{imm:#x}'); i += (num or 256) * 2
        elif cmd in (0x30, 0x31): out.append(NAMES[cmd]); i += 4
        elif cmd == 0x20: out.append('STMASK'); i += 1
        elif cmd == 0 and w == 0: continue
        else: out.append(f'{NAMES.get(cmd, hex(cmd))} {imm:#x}' if cmd not in (0x00,) else 'NOP')
    return out
for t in tags[first:first + count]:
    tadr, kind, qwc, addr = int(t[1], 16), t[2], int(t[4]), int(t[6], 16)
    tagw = ram[tadr + 8:tadr + 16]
    pre = codes(tagw, True)
    if kind in ('cnt', 'next', 'call', 'ret', 'end'): payload = ram[tadr + 16: tadr + 16 + qwc * 16]
    else: payload = ram[addr: addr + qwc * 16]
    print(f'{tadr:08x} {kind:4} {qwc:4} {addr:08x} | ' + '; '.join(pre + codes(payload, False))[:220])
