"""shino_vu1_recomp.py <SCES_539.31> <out dir> [--files N]: the game's VU1 microprograms recompiled to C++.

The game keeps its VU1 code in its executable as DMA packets of VIF codes (a tag, then MPG codes and their 64-bit
instruction pairs) and sends one to VIF1 before the programs that use it. Every such packet is found by its shape
(a tag whose data are only NOP and MPG codes, ending where the tag's quadwords end) and compiled: an "image", the code
the packet loads, becomes one C++ function that runs a program from any of its entries (runtime/shino_vu.h).

The compiled code does what runtime/shino_vu1.cpp's interpreter does, pair by pair, with the same operations
(runtime/shino_vu.h): the upper instruction computed from the registers before the lower one, committed after it; the
branches' delay slot; the E bit ending the program after the next pair; Q landing 7 pairs after DIV / SQRT (13 after
RSQRT) or at WAITQ; FMAND / FMEQ / FMOR reading the MAC flags of 4 pairs earlier. What no instruction can observe is
left out: Q's countdown where no result can be pending, the MAC flags' history where nothing reads it within 4 pairs,
the MAC flags themselves where the next result replaces them unread. The rarer instructions run through the
interpreter's own functions. A pair the compiled code cannot follow (a branch in a delay slot, an E bit on a branch)
hands the program to the interpreter there.

Writes <out dir>/shino_vu1_table.cpp and shino_vu1_NN.cpp (N files, 8 by default). Nothing from the disc is kept in the
repository: the output goes to the build.
"""
import os
import struct
import sys

MASK = 0x3FF8  # VU1's code memory, bytes
PAIRS = 2048
SEGMENT = 64   # pairs: runtime/shino_vu.h's kSegmentPairs


# ---- The packets ------------------------------------------------------------------------------------------------------

def load_segments(elf):
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    phnum = struct.unpack_from('<H', elf, 0x2C)[0]
    out = []
    for i in range(phnum):
        kind, off, va, _, size, _, _, _ = struct.unpack_from('<8I', elf, phoff + i * 32)
        if kind == 1 and size:
            out.append((va, elf[off:off + size]))
    return out


def packets(elf):
    """(EE address, [(micro pair address, pairs, bytes)]) for every microcode packet."""
    for va, data in load_segments(elf):
        for o in range(0, len(data) - 16, 16):
            t0, t1, c0, c1 = struct.unpack_from('<4I', data, o)
            qwc = t0 & 0xFFFF
            if qwc == 0 or (t0 >> 16) & 0xFFF or t1 != 0:
                continue
            if (c0 >> 24) & 0x7F != 0x4A and (c1 >> 24) & 0x7F != 0x4A:
                continue
            stream = data[o + 8:o + 16 + qwc * 16]
            if len(stream) != 8 + qwc * 16:
                continue
            at, loads, ok = 0, [], True
            while at + 4 <= len(stream):
                code = struct.unpack_from('<I', stream, at)[0]
                at += 4
                cmd = (code >> 24) & 0x7F
                if cmd == 0:
                    continue
                if cmd == 0x4A and at % 8 == 0:
                    num = (code >> 16) & 0xFF or 256
                    loads.append((code & 0xFFFF, num, stream[at:at + num * 8]))
                    at += num * 8
                    continue
                ok = False
                break
            if ok and loads and at == len(stream):
                yield va + o, loads


def images(elf):
    """The packets' code as contiguous runs of pairs: (EE address, first pair, [(lo, up)]), duplicates once."""
    seen, result = set(), []
    for address, loads in packets(elf):
        pairs = {}
        for base, num, data in loads:
            for k in range(num):
                pairs[(base + k) & (PAIRS - 1)] = struct.unpack_from('<2I', data, 8 * k)
        ordered = sorted(pairs)
        start = 0
        while start < len(ordered):
            end = start
            while end + 1 < len(ordered) and ordered[end + 1] == ordered[end] + 1:
                end += 1
            first = ordered[start]
            code = [pairs[p] for p in ordered[start:end + 1]]
            key = (first, tuple(code))
            if key not in seen:
                seen.add(key)
                result.append((address, first, code))
            start = end + 1
    return result


# ---- Decoding (runtime/shino_vu1.cpp's decodePair and decodeLower) --------------------------------------------------

ARITH_TABLE = [
    (0, 1), (0, 1), (0, 1), (0, 1), (1, 1), (1, 1), (1, 1), (1, 1), (2, 1), (2, 1), (2, 1), (2, 1),
    (3, 1), (3, 1), (3, 1), (3, 1), (5, 1), (5, 1), (5, 1), (5, 1), (6, 1), (6, 1), (6, 1), (6, 1),
    (4, 1), (4, 1), (4, 1), (4, 1), (4, 2), (5, 3), (4, 3), (6, 3), (0, 2), (2, 2), (0, 3), (2, 3),
    (1, 2), (3, 2), (1, 3), (3, 3), (0, 0), (2, 0), (4, 0), (5, 0), (1, 0), (3, 0), (-1, 0), (6, 0)]
ACCQ = [(0, 2), (2, 2), (0, 3), (2, 3), (1, 2), (3, 2), (1, 3), (3, 3)]
BRANCHES = {'B', 'BAL', 'JR', 'JALR', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'}


class Pair:
    def __init__(self, lo, up, pair):
        self.lo, self.up, self.pair = lo, up, pair
        pc = pair * 8
        self.pc = pc
        self.loi = bool((up >> 31) & 1)
        self.end = bool((up >> 30) & 1)
        # upper
        self.dest, self.ft, self.fs, self.fd, self.bc = (up >> 21) & 15, (up >> 16) & 31, (up >> 11) & 31, (up >> 6) & 31, up & 3
        self.kind, self.fn, self.operand, self.to_acc = 'scalar', 0, 0, False
        op = up & 0x3F
        if op < 0x3C:
            if op < 0x30 and ARITH_TABLE[op][0] >= 0:
                self.arith(*ARITH_TABLE[op], False)
        else:
            sp = ((up >> 6) & 0x1F) << 2 | (up & 3)
            if sp <= 0x0F:
                self.arith(sp >> 2, 1, True)
            elif 0x18 <= sp <= 0x1B:
                self.arith(4, 1, True)
            elif sp == 0x1C:
                self.arith(4, 2, True)
            elif sp == 0x1E:
                self.arith(4, 3, True)
            elif 0x20 <= sp <= 0x27:
                self.arith(*ACCQ[sp - 0x20], True)
            elif sp == 0x28:
                self.arith(0, 0, True)
            elif sp == 0x29:
                self.arith(2, 0, True)
            elif sp == 0x2A:
                self.arith(4, 0, True)
            elif sp == 0x2C:
                self.arith(1, 0, True)
            elif sp == 0x2D:
                self.arith(3, 0, True)
            elif sp == 0x2F:
                self.kind = 'nop'
        self.sets_mac = self.kind == 'arith' and self.fn <= 4
        # Whether the upper instruction writes the MAC flags: the arithmetic but on vf0; OPMSUB, OPMULA (upper()).
        if self.kind == 'arith':
            self.writes_mac = self.sets_mac and (self.to_acc or self.fd != 0)
        else:
            sp = ((up >> 6) & 0x1F) << 2 | (up & 3)
            self.writes_mac = (op == 0x2E and self.fd != 0) or (op >= 0x3C and sp == 0x2E)
        # XTOP / XITOP (decoded from the lower word whatever the I bit says, as decodePair does)
        self.xtop, self.xreg = 0, 0
        if (lo >> 31) and (lo & 0x3F) >= 0x3C:
            lsp = ((lo >> 6) & 0x1F) << 2 | (lo & 3)
            if lsp in (0x68, 0x69):
                self.xtop, self.xreg = (1 if lsp == 0x68 else 2), (lo >> 16) & 15
        # lower
        self.is_, self.it, self.id = (lo >> 11) & 31, (lo >> 16) & 31, (lo >> 6) & 31
        self.ldest, self.fsf, self.ftf = (lo >> 21) & 15, (lo >> 21) & 3, (lo >> 23) & 3
        imm11 = ((lo & 0x7FF) ^ 0x400) - 0x400
        self.imm = 0
        self.target = (pc + 8 + imm11 * 8) & MASK
        self.lk = self.decode_lower(lo, imm11)

    def arith(self, fn, operand, to_acc):
        self.kind, self.fn, self.operand, self.to_acc = 'arith', fn, operand, to_acc

    def decode_lower(self, lo, imm11):
        if self.loi:
            return 'NONE'
        if lo >> 31:
            op = lo & 0x3F
            simple = {0x30: 'IADD', 0x31: 'ISUB', 0x34: 'IAND', 0x35: 'IOR'}
            if op in simple:
                return simple[op]
            if op == 0x32:
                self.imm = (((lo >> 6) & 31) ^ 16) - 16
                return 'IADDI'
            if op < 0x3C:
                return 'SLOW'
            sp = ((lo >> 6) & 0x1F) << 2 | (lo & 3)
            return {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV',
                    0x39: 'SQRT', 0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR',
                    0x68: 'NONE', 0x69: 'NONE'}.get(sp, 'SLOW')
        op = lo >> 25
        if op in (0x00, 0x01, 0x04, 0x05):
            self.imm = imm11
            return {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW'}[op]
        if op in (0x08, 0x09):
            self.imm = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
            return 'IADDIU' if op == 0x08 else 'ISUBIU'
        return {0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ',
                0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}.get(op, 'SLOW')

    @property
    def branch(self):
        return self.lk in BRANCHES

    @property
    def reads_history(self):  # FMEQ, FMAND, FMOR
        return not self.loi and not (self.lo >> 31) and (self.lo >> 25) in (0x18, 0x1A, 0x1B)

    @property
    def reads_status(self):  # FSEQ, FSSET, FSAND, FSOR
        return not self.loi and not (self.lo >> 31) and 0x14 <= (self.lo >> 25) <= 0x17


# ---- One image ----------------------------------------------------------------------------------------------------------

EXIT = ('exit',)


class Image:
    def __init__(self, ident, address, first, code):
        self.id, self.address, self.first = ident, address, first
        self.pairs = {first + k: Pair(lo, up, first + k) for k, (lo, up) in enumerate(code)}
        self.last = first + len(code) - 1
        self.code = code

    def has(self, p):
        return self.first <= p <= self.last

    def bail(self, p):
        """A pair the compiled code hands to the interpreter (before running it)."""
        x = self.pairs[p]
        if not (x.branch or x.end):
            return False
        if x.branch and x.end:
            return True
        s = p + 1
        if not self.has(s):
            return True
        slot = self.pairs[s]
        return slot.branch or slot.end

    def next_pair(self, p):
        return p + 1 if self.has(p + 1) and (p + 1) * 8 <= MASK else None

    # Nodes: ('s', p) the pair p entered on its own; ('c', p) the pair p as the delay slot of p - 1 (or after its E bit).
    def successors(self, node):
        kind, p = node
        x = self.pairs[p]
        if kind == 's':
            if self.bail(p):
                return []
            if x.branch or x.end:
                return [('c', p + 1)]
            n = self.next_pair(p)
            return [('s', n)] if n is not None else [EXIT]
        b = self.pairs[p - 1]
        if b.end:
            return [EXIT]
        out = []
        if b.lk in ('JR', 'JALR'):
            return [EXIT]
        t = b.target // 8
        out.append(('s', t) if self.has(t) else EXIT)
        if b.lk not in ('B', 'BAL'):
            n = self.next_pair(p)
            out.append(('s', n) if n is not None else EXIT)
        return out

    def analyse(self):
        # Every pair can be entered (a program's start, a JR's target, where the interpreter hands a program back),
        # with what the code there assumes checked at the entry: Q's countdown no longer than the code counts down.
        entries = [('s', p) for p in self.pairs]
        reach, todo = set(), list(entries)
        while todo:
            n = todo.pop()
            if n == EXIT or n in reach:
                continue
            reach.add(n)
            todo.extend(self.successors(n))
        self.reach = reach
        self.entry_set = set(p for _, p in entries)
        preds = {n: [] for n in reach}
        for n in reach:
            for s in self.successors(n):
                if s != EXIT:
                    preds[s].append(n)
        bails = {n for n in reach if n[0] == 's' and self.bail(n[1])}
        # Q: the largest countdown possible at each node's start (forward) from the DIV / SQRT / RSQRT before it; an
        # entry with a longer one pending goes to the interpreter (the dispatch's check).
        q = {n: 0 for n in reach}
        work = list(reach)
        while work:
            n = work.pop()
            if n in bails:
                continue
            x = self.pairs[n[1]]
            out = max(q[n] - 1, 0)
            if x.lk in ('DIV', 'SQRT'):
                out = 7
            elif x.lk == 'RSQRT':
                out = 13
            elif x.lk == 'WAITQ':
                out = 0
            for s in self.successors(n):
                if s != EXIT and out > q[s]:
                    q[s] = out
                    work.append(s)
        self.q = q
        # MAC history: the distance to the next reader (or exit) along any path (backward)
        INF = 99
        dist = {n: INF for n in reach}
        changed = True
        while changed:
            changed = False
            for n in reach:
                if n in bails:
                    continue
                d = INF
                for s in self.successors(n):
                    if s == EXIT or s in bails:
                        d = min(d, 1)
                    else:
                        d = min(d, 1 if self.pairs[s[1]].reads_history else 1 + dist[s])
                if d < dist[n]:
                    dist[n] = d
                    changed = True
        self.push = {n: dist[n] <= 4 for n in reach}
        # MAC flags: whether a result's flags are read (by a push, a status read, the end) before the next result
        live_in = {n: False for n in reach}
        changed = True
        while changed:
            changed = False
            for n in reach:
                if n in bails:
                    new = True
                else:
                    x = self.pairs[n[1]]
                    out = self.live_out(n, live_in, bails)
                    new = x.reads_status or (not x.writes_mac and (self.push[n] or out))
                if new != live_in[n]:
                    live_in[n] = new
                    changed = True
        self.live_in = live_in
        self.bails = bails

    def live_out(self, n, live_in, bails):
        for s in self.successors(n):
            if s == EXIT or s in bails or live_in[s]:
                return True
        return False

    def flags(self, n):
        x = self.pairs[n[1]]
        return x.writes_mac and (self.push[n] or self.live_out(n, self.live_in, self.bails))


# ---- C++ ------------------------------------------------------------------------------------------------------------------

def emit_pair(img, node, out, branch_vars=False):
    """One pair's code (in braces); a branch's lower instruction sets `take` / `to` of the enclosing block."""
    p = node[1]
    x = img.pairs[p]
    w = out.append
    w('    {')
    w('        ++v.pairs;')
    if img.q[node] > 0:
        w('        qTick(v);')
    if x.kind == 'arith':
        w(f'        const __m128 u = arith(v, {x.fn}, {x.operand}, {x.fs}, {x.ft}, {x.bc});')
    elif x.kind == 'scalar':
        w('        UpperResult u;')
        w(f'        upperSlow(0x{x.up:08x}u, u);')
    if x.xtop and x.xreg:
        w(f'        v.vi[{x.xreg}] = static_cast<uint16_t>({"top" if x.xtop == 1 else "itop"}());')
    lk, s_, t_, d_ = x.lk, x.is_, x.it, x.id
    vi = lambda r: '0u' if r & 15 == 0 else f'vi(v, {r})'  # vi0 is never written: always 0
    L = {
        'NONE': None,
        'IADD': f'setVi(v, {d_}, {vi(s_)} + {vi(t_)});',
        'ISUB': f'setVi(v, {d_}, {vi(s_)} - {vi(t_)});',
        'IADDI': f'setVi(v, {t_}, {vi(s_)} + {x.imm & 0xFFFFFFFF}u);',
        'IAND': f'setVi(v, {d_}, {vi(s_)} & {vi(t_)});',
        'IOR': f'setVi(v, {d_}, {vi(s_)} | {vi(t_)});',
        'IADDIU': f'setVi(v, {t_}, {vi(s_)} + {x.imm}u);',
        'ISUBIU': f'setVi(v, {t_}, {vi(s_)} - {x.imm}u);',
        'MOVE': f'setVf(v, {t_}, {x.ldest}, vf(v, {s_}));',
        'MR32': f'setVf(v, {t_}, {x.ldest}, mr32(v, {s_}));',
        'LQI': f'setVf(v, {t_}, {x.ldest}, load(v, {vi(s_)})); setVi(v, {s_}, {vi(s_)} + 1);',
        'SQI': f'store(v, {vi(t_)}, {s_}, {x.ldest}); setVi(v, {t_}, {vi(t_)} + 1);',
        'LQD': f'setVi(v, {s_}, {vi(s_)} - 1); setVf(v, {t_}, {x.ldest}, load(v, {vi(s_)}));',
        'SQD': f'setVi(v, {t_}, {vi(t_)} - 1); store(v, {vi(t_)}, {s_}, {x.ldest});',
        'LQ': f'setVf(v, {t_}, {x.ldest}, load(v, {vi(s_)} + {x.imm & 0xFFFFFFFF}u));',
        'SQ': f'store(v, {vi(t_)} + {x.imm & 0xFFFFFFFF}u, {s_}, {x.ldest});',
        'ILW': f'ilw(v, {t_}, {vi(s_)} + {x.imm & 0xFFFFFFFF}u, {x.ldest});',
        'ILWR': f'ilw(v, {t_}, {vi(s_)}, {x.ldest});',
        'ISW': f'isw(v, {t_}, {vi(s_)} + {x.imm & 0xFFFFFFFF}u, {x.ldest});',
        'ISWR': f'isw(v, {t_}, {vi(s_)}, {x.ldest});',
        'MTIR': f'mtir(v, {t_}, {s_}, {x.fsf});',
        'MFIR': f'mfir(v, {t_}, {s_}, {x.ldest});',
        'DIV': f'divide(v, 0, {s_}, {x.fsf}, {t_}, {x.ftf});',
        'SQRT': f'divide(v, 1, {s_}, {x.fsf}, {t_}, {x.ftf});',
        'RSQRT': f'divide(v, 2, {s_}, {x.fsf}, {t_}, {x.ftf});',
        'WAITQ': 'waitQ(v);',
        'SLOW': f'lowerSlow(0x{x.lo:08x}u, 0x{x.pc:04x}u);',
        'B': 'take = true;',
        'BAL': f'setVi(v, {t_}, {(x.pc + 16) // 8}u); take = true;',
        'JR': f'to = {vi(s_)} * 8u;',
        'JALR': f'to = {vi(s_)} * 8u; setVi(v, {t_}, {(x.pc + 16) // 8}u);',
        'IBEQ': f'take = {vi(s_)} == {vi(t_)};',
        'IBNE': f'take = {vi(s_)} != {vi(t_)};',
        'IBLTZ': f'take = s16({vi(s_)}) < 0;',
        'IBGTZ': f'take = s16({vi(s_)}) > 0;',
        'IBLEZ': f'take = s16({vi(s_)}) <= 0;',
        'IBGEZ': f'take = s16({vi(s_)}) >= 0;',
    }[lk]
    if L:
        w('        ' + L)
    if x.kind == 'arith':
        flags = 'true' if img.flags(node) else 'false'
        w(f'        commitArith(v, {"true" if x.to_acc else "false"}, {x.fd}, {x.dest}, {flags}, u);')
    elif x.kind == 'scalar':
        w('        commitSlow(u);')
    if img.push[node]:
        w('        macPush(v);')
    if x.loi:
        w(f'        v.i = asF(0x{x.lo:08x}u);')
    w('    }')


def segment_check(img, p, ret):
    return f'    if (!segmentOk({img.id}, {p // SEGMENT})) return {ret};'


def emit_image(img):
    img.analyse()
    out = []
    w = out.append
    name = f'shino_vu1_image_{img.id}'
    w(f'// From the packet at 0x{img.address:x}: pairs 0x{img.first:03x} to 0x{img.last:03x}.')
    w(f'uint32_t {name}(Unit &v, uint32_t pc)')
    w('{')
    w('    uint32_t guard = 0;')
    w('dispatch:')
    w('    switch (pc)')
    w('    {')
    for p in sorted(img.entry_set):
        node = ('s', p)
        if node in img.bails:
            continue
        if img.q[node] >= 13:
            w(f'    case 0x{p * 8:04x}u: goto L{p:03x};')
        else:
            w(f'    case 0x{p * 8:04x}u: if (v.qDelay > {img.q[node]}) return pc | kInterpret; goto L{p:03x};')
    w('    default: return pc | kInterpret;')
    w('    }')
    for p in range(img.first, img.last + 1):
        node = ('s', p)
        if node not in img.reach:
            continue
        x = img.pairs[p]
        w(f'L{p:03x}:')
        if p % SEGMENT == 0 and p != img.first:
            w(segment_check(img, p, f'0x{p * 8:04x}u'))
        if node in img.bails:
            w(f'    return 0x{p * 8:04x}u | kInterpret;')
            continue
        if not (x.branch or x.end):
            emit_pair(img, node, out)
            n = img.next_pair(p)
            if n is None:
                w(f'    return 0x{((p + 1) * 8) & MASK:04x}u;')
            elif ('s', n) not in img.reach:
                raise AssertionError(f'image {img.id}: pair {n:x} after {p:x} unreachable')
            continue
        s = p + 1
        if s // SEGMENT != p // SEGMENT:
            w(segment_check(img, s, f'0x{p * 8:04x}u | kInterpret'))
        w('    {')
        w('    bool take = false;')
        w('    uint32_t to = 0;')
        emit_pair(img, node, out)
        emit_pair(img, ('c', s), out)
        slot_pc = (p * 8 + 8) & MASK
        if x.end:
            w(f'    v.tpc = 0x{slot_pc + 8:04x}u;')
            w('    return kEnd;')
        elif x.lk in ('JR', 'JALR'):
            w(f'    pc = to & 0x{MASK:04x}u;')
            w(f'    if (pc < 0x{img.first * 8:04x}u || pc > 0x{img.last * 8:04x}u || !segmentOk({img.id}, pc / {SEGMENT * 8})) return pc;')
            w('    goto dispatch;')
        else:
            t = x.target // 8
            if img.has(t):
                jump = f'goto L{t:03x};'
                if t // SEGMENT not in (p // SEGMENT, s // SEGMENT) and t % SEGMENT != 0:
                    jump = f'{{ if (!segmentOk({img.id}, {t // SEGMENT})) return 0x{t * 8:04x}u; goto L{t:03x}; }}'
                if t <= p:
                    jump = f'{{ if (++guard > (1u << 22)) return 0x{t * 8:04x}u | kInterpret; {jump} }}'
            else:
                jump = f'return 0x{t * 8:04x}u;'
            if x.lk in ('B', 'BAL'):
                w(f'    {jump}')
            else:
                w(f'    if (take) {jump}')
                n = img.next_pair(s)
                if n is None:
                    w(f'    return 0x{(slot_pc + 8) & MASK:04x}u;')
                else:
                    w(f'    goto L{n:03x};')
        w('    }')
    w('}')
    return out


def main():
    args = sys.argv[1:]
    files = 8
    if '--files' in args:
        i = args.index('--files')
        files = int(args[i + 1])
        del args[i:i + 2]
    elf_path, out_dir = args
    elf = open(elf_path, 'rb').read()
    os.makedirs(out_dir, exist_ok=True)
    found = images(elf)
    imgs = [Image(i, address, first, code) for i, (address, first, code) in enumerate(found)]
    # spread the images over the files by size
    buckets = [[] for _ in range(files)]
    sizes = [0] * files
    for img in sorted(imgs, key=lambda m: -len(m.code)):
        k = sizes.index(min(sizes))
        buckets[k].append(img)
        sizes[k] += len(img.code)
    head = ['// Generated by tools/shino_vu1_recomp.py from the player\'s SCES_539.31: do not edit, do not commit.',
            '#include "shino_vu.h"', '', 'namespace shino::vu', '{']
    stats = {'pairs': 0, 'bails': 0, 'entries': 0}
    for k, bucket in enumerate(buckets):
        lines = list(head)
        for img in sorted(bucket, key=lambda m: m.id):
            words = ', '.join(f'0x{lo:08x}u, 0x{up:08x}u' for lo, up in img.code)
            lines.append(f'extern const uint32_t shino_vu1_code_{img.id}[] = {{{words}}};')
            lines.extend(emit_image(img))
            stats['pairs'] += len(img.code)
            stats['bails'] += len(img.bails)
            stats['entries'] += len(img.entry_set)
        lines.append('} // namespace shino::vu')
        write(os.path.join(out_dir, f'shino_vu1_{k:02d}.cpp'), lines)
    table = list(head)
    for img in imgs:
        table.append(f'extern const uint32_t shino_vu1_code_{img.id}[];')
        table.append(f'uint32_t shino_vu1_image_{img.id}(Unit &v, uint32_t pc);')
    table.append('const Image kImages[] = {')
    for img in imgs:
        table.append(f'    {{0x{img.first:03x}u, {len(img.code)}u, shino_vu1_code_{img.id}, shino_vu1_image_{img.id}, 0x{img.address:x}u}},')
    table.append('};')
    table.append(f'const uint32_t kImageCount = {len(imgs)};')
    table.append('} // namespace shino::vu')
    write(os.path.join(out_dir, 'shino_vu1_table.cpp'), table)
    print(f'shino_vu1_recomp: {len(imgs)} images, {stats["pairs"]} pairs, {stats["entries"]} entries, '
          f'{stats["bails"]} pairs left to the interpreter')


def write(path, lines):
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
