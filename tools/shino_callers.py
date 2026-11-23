"""shino_callers.py <SCES_539.31> <0xADDR> [depth]: who calls a function (jal), up to `depth` levels (default 3).

Function bounds come from bozo-recomp's report (build/bozo/shino_prep/shino_report.txt), names from
recomp/shino_symbols.txt. A tool for naming the stripped executable's functions (docs/RECOMP_NOTES.md).
"""
import bisect
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE, FILE_OFFSET, CODE_END = 0x100000, 0x80, 0x4F1500


def load(elf_path):
    d = open(elf_path, 'rb').read()
    words = struct.unpack_from('<%dI' % ((CODE_END - BASE) // 4), d, FILE_OFFSET)
    starts = sorted(int(l.split()[1], 16) for l in open(os.path.join(ROOT, 'build', 'bozo', 'shino_prep', 'shino_report.txt'))
                    if l.startswith('function'))
    names = {}
    for line in open(os.path.join(ROOT, 'recomp', 'shino_symbols.txt'), encoding='utf-8'):
        f = line.split('#', 1)[0].split()
        if len(f) == 2:
            names[int(f[1], 16)] = f[0]
    return words, starts, names


def main():
    words, starts, names = load(sys.argv[1])
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 3

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else None

    calls = {}
    for i, w in enumerate(words):
        if w >> 26 == 3:
            calls.setdefault((w & 0x3FFFFFF) << 2, []).append(BASE + i * 4)

    def show(target, level, seen):
        sites = calls.get(target, [])
        print('  ' * level + f'0x{target:x} {names.get(target, "")}  ({len(sites)} call sites)')
        if level >= depth:
            return
        for f in sorted({owner(s) for s in sites}):
            if f is not None and f not in seen:
                show(f, level + 1, seen | {f})

    show(int(sys.argv[2], 16), 0, {int(sys.argv[2], 16)})


if __name__ == '__main__':
    main()
