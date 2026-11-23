"""shino_entries.py <shino.elf> <bozo-recomp report> <out.txt>: entry points for bozo-recomp --entries.

bozo-recomp resolves a switch's jump table when it sees the table's base loaded next to the `jr`; Shinobido's code
often loads the base far from it (0x4b5498: `lw s0,(t3+index)`, t3 set long before), so the cases are reached at
addresses that are no label (a fault: "resume at ...: not a label of its function"). For every function with an
unresolved indirect jump (the report's `unresolved <site> <function>` lines), each word of the data
(0x4f1500-0x625a00) that points inside that function, on an instruction, is given as an entry: a jump table's targets
are among them. An entry inside a function only adds a label (BozoEngine docs/explicit-entries.md), so extra ones are
harmless.
"""
import bisect
import struct
import sys

BASE, FILE_OFFSET = 0x100000, 0x80
DATA = (0x4F1500, 0x625A00)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    elf = open(sys.argv[1], 'rb').read()
    functions, unresolved = [], set()
    for line in open(sys.argv[2], encoding='utf-8'):
        f = line.split()
        if len(f) >= 3 and f[0] == 'function':
            functions.append((int(f[1], 16), int(f[2], 16)))
        elif len(f) >= 3 and f[0] == 'unresolved':
            unresolved.add(int(f[2], 16))
    functions.sort()
    starts = [s for s, _ in functions]
    bounds = {s: e for s, e in functions if s in unresolved}
    words = struct.unpack_from('<%dI' % ((DATA[1] - DATA[0]) // 4), elf, FILE_OFFSET + DATA[0] - BASE)
    entries = set()
    for w in words:
        if w & 3 or not (BASE <= w < DATA[0]):
            continue
        i = bisect.bisect_right(starts, w) - 1
        if i >= 0 and starts[i] in bounds and w < bounds[starts[i]] and w != starts[i]:
            entries.add(w)
    with open(sys.argv[3], 'w', encoding='utf-8') as out:
        out.write('# jump-table targets in functions with unresolved indirect jumps (tools/shino_entries.py)\n')
        out.writelines(f'0x{a:x}\n' for a in sorted(entries))
    print(f'shino_entries: {len(entries)} entries in {len(bounds)} functions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
