"""shino_vu1_live.py <micro.bin> <entry pair> [vi<n>=pair,pair ...]: what a VU1 program takes from the ones before.

For a program entered at a pair: the registers (vf lanes, vi, ACC lanes, Q, I, P, R, the MAC and clip flags) whose
value from before the program can reach something it does (a store, a kick, a branch, a jump), through any path. A
lane computed and never used does not count (each lane's inputs are needed only when its result is). A `jr vi<n>`
follows the targets given for vi<n> (pairs, decimal or 0x...), else it is reported.

Simplifications: a division writes Q at once (a read within its latency reads the old Q, which this does not see);
the status flags are not followed; `sub d, s, s` (a zeroing) reads nothing; the MAC flags are one resource.

Used to find what the native renderers must leave behind when they take a whole program over (docs/NATIVE_3D.md).
"""
import re
import struct
import sys

L = 'xyzw'
ALWAYS = '*'  # a group whose effect is outside the registers (memory, the GS, control)


def lanes(d):
    return [L[i] for i in range(4) if (d >> (3 - i)) & 1]


def vf(r, ls):
    return set() if r == 0 else {f'vf{r}.{c}' for c in ls}


def vi(r):
    return set() if r == 0 else {f'vi{r}'}


def per_lane(d, out, ins, mac=False):
    """Groups (outputs, inputs), one a lane: out(c) and ins(c) give each lane's sets."""
    groups = []
    for c in d:
        o = set(out(c)) | ({'MAC'} if mac else set())
        if o:
            groups.append((o, set(ins(c))))
    return groups


def upper(w):
    """The upper instruction's groups."""
    d = lanes((w >> 21) & 15)
    ft, fs, fd, bc = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 3
    op = w & 0x3F
    if op < 0x1C:
        kind = op >> 2  # add sub madd msub max mini mul
        if kind == 1 and fs == ft:
            return per_lane(d, lambda c: vf(fd, c), lambda c: (), True)
        return per_lane(d, lambda c: vf(fd, c),
                        lambda c: vf(fs, c) | vf(ft, L[bc]) | ({f'ACC.{c}'} if kind in (2, 3) else set()),
                        kind not in (4, 5))
    if op < 0x3C:
        n = {0x1C: 'mulq', 0x1D: 'maxi', 0x1E: 'muli', 0x1F: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
             0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
             0x2A: 'mul', 0x2B: 'max', 0x2C: 'sub', 0x2D: 'msub', 0x2E: 'opmsub', 0x2F: 'mini'}.get(op)
        if n is None:
            return [({ALWAYS}, {'?'})]
        mac = n not in ('max', 'mini', 'maxi', 'minii')
        accum = n.startswith('madd') or n.startswith('msub') or n == 'opmsub'
        if n == 'opmsub':  # crosses the lanes
            return [(vf(fd, d) | {'MAC'}, vf(fs, 'xyz') | vf(ft, 'xyz') | {'ACC.x', 'ACC.y', 'ACC.z'})]
        if n == 'sub' and fs == ft:
            return per_lane(d, lambda c: vf(fd, c), lambda c: (), True)
        other = (lambda c: vf(ft, c)) if op >= 0x28 else (lambda c: {'Q' if n.endswith('q') else 'I'})
        return per_lane(d, lambda c: vf(fd, c),
                        lambda c: vf(fs, c) | other(c) | ({f'ACC.{c}'} if accum else set()), mac)
    sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
    to_acc = lambda c: {f'ACC.{c}'}
    if sp < 0x10:  # adda suba madda msuba, broadcast
        return per_lane(d, to_acc, lambda c: vf(fs, c) | vf(ft, L[sp & 3]) | (to_acc(c) if sp >= 8 else set()), True)
    if sp < 0x18:  # itof / ftoi
        return per_lane(d, lambda c: vf(ft, c), lambda c: vf(fs, c))
    if sp < 0x1C:  # mula, broadcast
        return per_lane(d, to_acc, lambda c: vf(fs, c) | vf(ft, L[sp & 3]), True)
    if sp == 0x1D:  # abs
        return per_lane(d, lambda c: vf(ft, c), lambda c: vf(fs, c))
    if sp == 0x1F:  # clipw: the flags shift (the old ones kept)
        return [({'CLIP'}, vf(fs, 'xyz') | vf(ft, 'w') | {'CLIP'})]
    if sp == 0x2F:
        return []
    if sp in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
        src = 'Q' if sp in (0x1C, 0x20, 0x21, 0x24, 0x25) else 'I'
        accum = sp in (0x21, 0x23, 0x25, 0x27)
        return per_lane(d, to_acc, lambda c: vf(fs, c) | {src} | (to_acc(c) if accum else set()), True)
    if sp == 0x2E:  # opmula
        return [({'ACC.x', 'ACC.y', 'ACC.z', 'MAC'}, vf(fs, 'xyz') | vf(ft, 'xyz'))]
    if sp in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
        return per_lane(d, to_acc, lambda c: vf(fs, c) | vf(ft, c) | (to_acc(c) if sp in (0x29, 0x2D) else set()), True)
    return [({ALWAYS}, {'?'})]


def lower(w, pc):
    """The lower instruction's groups and its control: None, ('b', target), ('cb', target), ('jr', reg)."""
    d = lanes((w >> 21) & 15)
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = ((w & 0x7FF) ^ 0x400) - 0x400
    target = (pc + 8 + imm11 * 8) & 0x3FFF
    fsf, ftf = L[(w >> 21) & 3], L[(w >> 23) & 3]
    g = lambda outs, ins: [(set(outs), set(ins))] if outs else []
    if w >> 31:
        op = w & 0x3F
        if op in (0x30, 0x31, 0x34, 0x35):
            return g(vi(id_), vi(is_) | vi(it)), None
        if op == 0x32:
            return g(vi(it), vi(is_)), None
        if op < 0x3C:
            return g({ALWAYS}, {'?'}), None
        sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
        if sp == 0x30:
            return per_lane(d, lambda c: vf(it, c), lambda c: vf(is_, c)), None
        if sp == 0x31:  # mr32: x from y, y from z, z from w, w from x
            return per_lane(d, lambda c: vf(it, c), lambda c: vf(is_, L[(L.index(c) + 1) & 3])), None
        if sp in (0x34, 0x36):  # lqi / lqd
            return per_lane(d, lambda c: vf(it, c), lambda c: vi(is_)) + g(vi(is_), vi(is_)), None
        if sp in (0x35, 0x37):  # sqi / sqd
            return g({ALWAYS}, vf(is_, d) | vi(it)) + g(vi(it), vi(it)), None
        if sp in (0x38, 0x3A):
            return g({'Q'}, vf(is_, fsf) | vf(it, ftf)), None
        if sp == 0x39:
            return g({'Q'}, vf(it, ftf)), None
        if sp in (0x3B, 0x7B):
            return [], None
        if sp == 0x3C:
            return g(vi(it), vf(is_, fsf)), None
        if sp == 0x3D:
            return per_lane(d, lambda c: vf(it, c), lambda c: vi(is_)), None
        if sp == 0x3E:
            return g(vi(it), vi(is_)), None
        if sp == 0x3F:
            return g({ALWAYS}, vi(it) | vi(is_)), None
        if sp == 0x40:
            return per_lane(d, lambda c: vf(it, c), lambda c: {'R'}) + g({'R'}, {'R'}), None
        if sp == 0x41:
            return per_lane(d, lambda c: vf(it, c), lambda c: {'R'}), None
        if sp in (0x42, 0x43):
            return g({'R'}, vf(is_, fsf) | ({'R'} if sp == 0x43 else set())), None
        if sp == 0x64:
            return per_lane(d, lambda c: vf(it, c), lambda c: {'P'}), None
        if sp in (0x68, 0x69):
            return g(vi(it), ()), None
        if sp == 0x6C:
            return g({ALWAYS}, vi(is_)), None
        if 0x70 <= sp <= 0x7E:
            src = vf(is_, fsf) if sp >= 0x78 else vf(is_, 'xyzw' if sp == 0x76 else 'xyz')
            return g({'P'}, src), None
        return g({ALWAYS}, {'?'}), None
    op = w >> 25
    if op == 0x00:
        return per_lane(d, lambda c: vf(it, c), lambda c: vi(is_)), None
    if op == 0x01:
        return g({ALWAYS}, vf(is_, d) | vi(it)), None
    if op == 0x04:
        return g(vi(it), vi(is_)), None
    if op == 0x05:
        return g({ALWAYS}, vi(it) | vi(is_)), None
    if op in (0x08, 0x09):
        return g(vi(it), vi(is_)), None
    if op in (0x10, 0x12, 0x13):
        return g({'vi1'}, {'CLIP'}), None
    if op == 0x11:
        return g({'CLIP'}, ()), None
    if op in (0x14, 0x16, 0x17):
        return g(vi(it), ()), None
    if op == 0x15:
        return [], None
    if op in (0x18, 0x1A, 0x1B):
        return g(vi(it), {'MAC'} | vi(is_)), None
    if op == 0x1C:
        return g(vi(it), {'CLIP'}), None
    if op == 0x20:
        return [], ('b', target)
    if op == 0x21:
        return g(vi(it), ()), ('b', target)
    if op == 0x24:
        return g({ALWAYS}, vi(is_)), ('jr', is_)
    if op == 0x25:
        return g({ALWAYS}, vi(is_)) + g(vi(it), ()), ('jr', is_)
    if op in (0x28, 0x29):
        return g({ALWAYS}, vi(it) | vi(is_)), ('cb', target)
    if op in (0x2C, 0x2D, 0x2E, 0x2F):
        return g({ALWAYS}, vi(is_)), ('cb', target)
    return g({ALWAYS}, {'?'}), None


def analyse(data, entry, hints, size=0x4000):
    pairs = {}
    for pc in range(0, size, 8):
        lo, up = struct.unpack_from('<II', data, pc)
        groups = upper(up)
        if up >> 31 & 1:
            groups = groups + [({'I'}, set())]
            ctl = None
        else:
            lg, ctl = lower(lo, pc)
            groups = groups + lg
        kills = set().union(*(o for o, _ in groups)) - {ALWAYS} if groups else set()
        pairs[pc] = (groups, kills, ctl, bool(up >> 30 & 1))
    unresolved = set()

    # nodes: a pc, or (pc, continuation) for a delay slot: what follows it ('end', or the branch's control)
    def next_nodes(node):
        if isinstance(node, tuple):
            pc, cont = node
            if cont == 'end':
                return []
            kind, val = cont
            if kind == 'b':
                return [val]
            if kind == 'cb':
                return [val, pc + 8]
            if val in hints:
                return list(hints[val])
            unresolved.add((pc - 8, f'vi{val}'))
            return []
        _, _, ctl, end = pairs[node]
        if end:
            return [(node + 8, 'end')]
        if ctl:
            return [(node + 8, ctl)]
        return [node + 8]

    def at(node):
        return pairs[(node[0] if isinstance(node, tuple) else node) & 0x3FFF]

    seen, work = set(), [entry]
    while work:
        n = work.pop()
        if n in seen:
            continue
        seen.add(n)
        work += next_nodes(n)
    succ = {n: next_nodes(n) for n in seen}
    live = {n: set() for n in seen}
    needed_reads = {n: set() for n in seen}
    changed = True
    while changed:
        changed = False
        for n in seen:
            groups, kills, _, _ = at(n)
            out = set().union(*(live[s] for s in succ[n])) if succ[n] else set()
            reads = set()
            for o, i in groups:
                if ALWAYS in o or o & out:
                    reads |= i
            new = reads | (out - kills)
            needed_reads[n] = reads
            if new != live[n]:
                live[n] = new
                changed = True
    where = {}
    for n in seen:
        for r in needed_reads[n] & live[n] & live[entry]:
            where.setdefault(r, set()).add((n[0] if isinstance(n, tuple) else n) & 0x3FFF)
    return live[entry], unresolved, seen, where


def main():
    data = open(sys.argv[1], 'rb').read().ljust(0x4000, bytes(1))
    entry = int(sys.argv[2], 0) * 8
    hints = {}
    for h in sys.argv[3:]:
        reg, ts = h.split('=')
        hints[int(reg.lstrip('vi'))] = [int(t, 0) * 8 for t in ts.split(',')]
    live, unresolved, seen, where = analyse(data, entry, hints)

    def key(r):
        m = re.match(r'([a-zA-Z?*]+)(\d*)\.?(\w?)', r)
        return (m.group(1), int(m.group(2) or 0), m.group(3))
    print(f'entry pair {entry // 8} (0x{entry:04x}): {len(seen)} nodes reached')
    print('taken from before:', ' '.join(sorted(live, key=key)))
    for r in sorted(live, key=key):
        print(f'  {r}: read at ' + ' '.join(f'{pc:04x}' for pc in sorted(where.get(r, ()))[:12]))
    for pc, reg in sorted(unresolved):
        print(f'  jr at 0x{pc:04x} through {reg}: no targets given')


if __name__ == '__main__':
    main()
