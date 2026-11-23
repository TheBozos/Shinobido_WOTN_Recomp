"""List the executable's VCALLMS entries (VU0 microprogram calls) and VCALLMSR sites: the port registers a native VU0
program for each entry (runtime/shino_render.cpp). Usage: py tools/shino_vcallms.py game/SCES_539.31"""
import struct
import sys
from collections import defaultdict

data = open(sys.argv[1], 'rb').read()
phoff = struct.unpack_from('<I', data, 0x1C)[0]
phnum = struct.unpack_from('<H', data, 0x2C)[0]
sites = defaultdict(list)
for i in range(phnum):
    kind, off, va, _, size, _, _, _ = struct.unpack_from('<8I', data, phoff + i * 32)
    if kind != 1:
        continue
    for a in range(max(va, 0x100000), min(va + size, 0x4F1500), 4):  # the code (tools/shino_prepare.py)
        w = struct.unpack_from('<I', data, off + a - va)[0]
        if (w & 0xFE00003F) == 0x4A000038:  # VCALLMS imm15
            sites[(w >> 6) & 0x7FFF].append(a)
        elif (w & 0xFE00003F) == 0x4A000039:  # VCALLMSR
            sites['msr'].append(a)
for entry in sorted(sites, key=lambda e: -1 if e == 'msr' else e):
    name = 'VCALLMSR' if entry == 'msr' else 'entry 0x%03x (byte 0x%03x)' % (entry, entry * 8)
    print('%s: %d sites, %s' % (name, len(sites[entry]), ' '.join('0x%x' % a for a in sites[entry])))
