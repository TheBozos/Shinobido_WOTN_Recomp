"""shino_disasm.py <SCES_539.31> <function name | 0xADDR> [byte count]: disassemble SCES_539.31 with the symbol map.

The ELF is stripped: names come from recomp/shino_symbols.txt (scripts/symbols.sh), or the map given by SHINO_SYMBOLS.
A mapped name prints up to the next mapped function; an address prints `byte count` bytes (default 0x100). Calls and
jumps are annotated with the name of their target. Needs capstone (pip install capstone). From Onimusha's oni_disasm.py.
"""
import os
import struct
import sys

import capstone

BASE, FILE_OFFSET = 0x100000, 0x80  # the one LOAD segment
MAP = os.environ.get('SHINO_SYMBOLS', os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                   'recomp', 'shino_symbols.txt'))


def symbols(_elf):
    entries = []
    for line in open(MAP, encoding='utf-8'):
        line = line.split('#', 1)[0].split()
        if len(line) >= 2 and line[1].startswith('0x'):
            entries.append((line[0], int(line[1], 16)))
    entries.sort(key=lambda e: e[1])
    for i, (name, value) in enumerate(entries):
        nxt = entries[i + 1][1] if i + 1 < len(entries) else value + 0x100
        yield name, value, min(nxt - value, 0x2000), 2


def main():
    elf = open(sys.argv[1], 'rb').read()
    by_addr, by_name = {}, {}
    for name, value, size, kind in symbols(elf):
        if name and not name.startswith('.'):
            if kind == 2 or value not in by_addr:
                by_addr[value] = name
            by_name.setdefault(name, (value, size))
    what = sys.argv[2]
    if what in by_name:
        lo, size = by_name[what]
    else:
        lo, size = int(what, 16), 0x100
    if len(sys.argv) > 3:
        size = int(sys.argv[3], 16)
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
    off = lo - BASE + FILE_OFFSET
    def label(t):
        if t in by_addr:
            return by_addr[t]
        base = max((a for a in by_addr if a <= t), default=None)
        return f'{by_addr[base]}+{t - base:#x}' if base is not None else '?'
    regs = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 's0', 's1',
            's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
    for addr in range(lo, lo + size, 4):
        word, = struct.unpack_from('<I', elf, addr - BASE + FILE_OFFSET)
        if addr in by_addr:
            print(f'{by_addr[addr]}:')
        op = word >> 26
        rs, rt, imm = (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
        if op in (0x1E, 0x1F):  # lq / sq (capstone reads these as MIPS64r2 SPECIAL3)
            simm = imm - 0x10000 if imm & 0x8000 else imm
            print(f'  {addr:08x}  {"lq" if op == 0x1E else "sq":8s} ${regs[rt]}, {simm:#x}(${regs[rs]})')
            continue
        if op == 0x1C:  # MMI
            print(f'  {addr:08x}  {"mmi":8s} {word:#010x}')
            continue
        decoded = list(md.disasm(struct.pack('<I', word), addr, 1))
        if not decoded:
            print(f'  {addr:08x}  {".word":8s} {word:#010x}')
            continue
        ins = decoded[0]
        ops = ins.op_str
        if ins.mnemonic in ('jal', 'j', 'b', 'beq', 'bne', 'beqz', 'bnez', 'bgez', 'bltz', 'blez', 'bgtz', 'beql', 'bnel',
                            'beqzl', 'bnezl'):
            last = ops.split(', ')[-1]
            if last.startswith('0x'):
                ops += f'  <{label(int(last, 16))}>'
        print(f'  {ins.address:08x}  {ins.mnemonic:8s} {ops}')


if __name__ == '__main__':
    main()
