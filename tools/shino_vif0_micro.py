"""shino_vif0_micro.py <SCES_539.31> <chain address> [out.bin]: the VU0 micro memory image a static VIF0 chain loads.

Walks the source chain (TTE: each tag's upper 64 bits are two VIF codes) as runtime/shino_render.cpp does and applies
its MPG codes to a 4 KB micro memory, written to out.bin (default build/vu0_micro_<address>.bin) for
tools/shino_vu1_disasm.py. Lists the chain's VIF codes.
"""
import struct
import sys

elf = open(sys.argv[1], 'rb').read()
phoff = struct.unpack_from('<I', elf, 0x1C)[0]
phnum = struct.unpack_from('<H', elf, 0x2C)[0]
segments = []
for i in range(phnum):
    kind, off, va, _, size, _, _, _ = struct.unpack_from('<8I', elf, phoff + i * 32)
    if kind == 1:
        segments.append((va, off, size))


def mem(address, n):
    for va, off, size in segments:
        if va <= address and address + n <= va + size:
            return elf[off + address - va:off + address - va + n]
    raise ValueError('0x%x not in the ELF' % address)


micro = bytearray(4096)
stream = bytearray()
tadr = int(sys.argv[2], 16)
for _ in range(10000):
    lo, hi = struct.unpack('<QQ', mem(tadr, 16))
    qwc, ident, addr = lo & 0xFFFF, (lo >> 28) & 7, (lo >> 32) & 0x7FFFFFF0
    stream += struct.pack('<Q', hi)
    if ident == 1:  # cnt
        stream += mem(tadr + 16, qwc * 16)
        tadr += 16 + qwc * 16
    elif ident == 2:  # next
        stream += mem(tadr + 16, qwc * 16)
        tadr = addr
    elif ident in (3, 4):  # ref
        stream += mem(addr, qwc * 16)
        tadr += 16
    elif ident == 0:  # refe
        stream += mem(addr, qwc * 16)
        break
    else:  # end (no call / ret in these chains)
        stream += mem(tadr + 16, qwc * 16)
        break
at = 0
while at + 4 <= len(stream):
    code = struct.unpack_from('<I', stream, at)[0]
    at += 4
    cmd, imm, num = (code >> 24) & 0x7F, code & 0xFFFF, (code >> 16) & 0xFF
    if cmd == 0x4A:
        n = num or 256
        micro[imm * 8:imm * 8 + n * 8] = stream[at:at + n * 8]
        print('MPG %d pairs at 0x%03x' % (n, imm * 8))
        at += n * 8
    elif cmd >= 0x60:
        print('UNPACK %08x (vn %d vl %d num %d addr 0x%x)' % (code, (cmd >> 2) & 3, cmd & 3, num, imm & 0x3FF))
        vn, vl = (cmd >> 2) & 3, cmd & 3
        bits = 16 if vl == 3 else (32 >> vl) * (vn + 1)
        at += ((num or 256) * bits + 31) // 32 * 4
    elif cmd in (0x20,):
        at += 4
    elif cmd in (0x30, 0x31):
        at += 16
    elif cmd in (0x50, 0x51):
        at += (imm or 0x10000) * 16
    elif code:
        print('code %08x' % code)
out = sys.argv[3] if len(sys.argv) > 3 else 'build/vu0_micro_%x.bin' % int(sys.argv[2], 16)
open(out, 'wb').write(micro)
print('->', out)
