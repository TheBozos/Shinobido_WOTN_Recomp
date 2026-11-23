"""shino_vu1_disasm.py <micro.bin> [start byte address] [end]: disassemble a VU1 micro memory dump.

The dump is the 16 KB micro memory (pairs: lower word, upper word) runtime/shino_vu1.cpp writes with
SHINO_DUMP_MICRO=<flip> (build/vu1_micro.bin). The decoding follows the interpreter's tables, so the two can be read
side by side.
"""
import struct
import sys

F = 'xyzw'


def dest(w):
    d = (w >> 21) & 15
    return ''.join(F[i] for i in range(4) if (d >> (3 - i)) & 1)


def upper(w):
    d, ft, fs, fd, bc = dest(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 3
    flags = ('I' if w >> 31 & 1 else '') + ('E' if w >> 30 & 1 else '') + ('M' if w >> 29 & 1 else '') + \
            ('D' if w >> 28 & 1 else '') + ('T' if w >> 27 & 1 else '')
    op = w & 0x3F
    names = {0: 'add', 1: 'sub', 2: 'madd', 3: 'msub', 4: 'max', 5: 'mini', 6: 'mul'}
    if op < 0x1C:
        n = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul'][op >> 2]
        s = f'{n}{F[bc]}.{d} vf{fd}, vf{fs}, vf{ft}{F[bc]}'
    elif op < 0x3C:
        table = {0x1C: 'mulq', 0x1D: 'maxi', 0x1E: 'muli', 0x1F: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
                 0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
                 0x2A: 'mul', 0x2B: 'max', 0x2C: 'sub', 0x2D: 'msub', 0x2E: 'opmsub', 0x2F: 'mini'}
        n = table.get(op, f'?{op:02x}')
        src = f'vf{ft}' if op >= 0x28 else ('Q' if n.endswith('q') else 'I')
        s = f'{n}.{d} vf{fd}, vf{fs}, {src}'
    else:
        sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
        if sp < 0x10:
            n = ['adda', 'suba', 'madda', 'msuba'][sp >> 2]
            s = f'{n}{F[sp & 3]}.{d} ACC, vf{fs}, vf{ft}{F[sp & 3]}'
        elif sp < 0x18:
            n = ['itof0', 'itof4', 'itof12', 'itof15', 'ftoi0', 'ftoi4', 'ftoi12', 'ftoi15'][sp - 0x10]
            s = f'{n}.{d} vf{ft}, vf{fs}'
        elif sp < 0x1C:
            s = f'mula{F[sp & 3]}.{d} ACC, vf{fs}, vf{ft}{F[sp & 3]}'
        else:
            table = {0x1C: 'mulaq', 0x1D: 'abs', 0x1E: 'mulai', 0x1F: 'clipw', 0x20: 'addaq', 0x21: 'maddaq', 0x22: 'addai',
                     0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq', 0x26: 'subai', 0x27: 'msubai', 0x28: 'adda',
                     0x29: 'madda', 0x2A: 'mula', 0x2C: 'suba', 0x2D: 'msuba', 0x2E: 'opmula', 0x2F: 'nop'}
            n = table.get(sp, f'?sp{sp:02x}')
            if n == 'nop':
                s = 'nop'
            elif n == 'abs':
                s = f'abs.{d} vf{ft}, vf{fs}'
            elif n == 'clipw':
                s = f'clipw.xyz vf{fs}, vf{ft}w'
            else:
                s = f'{n}.{d} ACC, vf{fs}, vf{ft}'
    return s + (f'  [{flags}]' if flags else '')


def lower(w, pc):
    d, it, is_, id_ = dest(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = ((w & 0x7FF) ^ 0x400) - 0x400
    target = pc + 8 + imm11 * 8
    if w >> 31:
        op = w & 0x3F
        simple = {0x30: f'iadd vi{id_}, vi{is_}, vi{it}', 0x31: f'isub vi{id_}, vi{is_}, vi{it}',
                  0x32: f'iaddi vi{it}, vi{is_}, {((w >> 6 & 31) ^ 16) - 16}', 0x34: f'iand vi{id_}, vi{is_}, vi{it}',
                  0x35: f'ior vi{id_}, vi{is_}, vi{it}'}
        if op in simple:
            return simple[op]
        sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
        fsf, ftf = F[(w >> 21) & 3], F[(w >> 23) & 3]
        table = {0x30: f'move.{d} vf{it}, vf{is_}', 0x31: f'mr32.{d} vf{it}, vf{is_}', 0x34: f'lqi.{d} vf{it}, (vi{is_}++)',
                 0x35: f'sqi.{d} vf{is_}, (vi{it}++)', 0x36: f'lqd.{d} vf{it}, (--vi{is_})', 0x37: f'sqd.{d} vf{is_}, (--vi{it})',
                 0x38: f'div Q, vf{is_}{fsf}, vf{it}{ftf}', 0x39: f'sqrt Q, vf{it}{ftf}', 0x3A: f'rsqrt Q, vf{is_}{fsf}, vf{it}{ftf}',
                 0x3B: 'waitq', 0x3C: f'mtir vi{it}, vf{is_}{fsf}', 0x3D: f'mfir.{d} vf{it}, vi{is_}',
                 0x3E: f'ilwr.{d} vi{it}, (vi{is_})', 0x3F: f'iswr.{d} vi{it}, (vi{is_})', 0x40: f'rnext.{d} vf{it}, R',
                 0x41: f'rget.{d} vf{it}, R', 0x42: f'rinit R, vf{is_}{fsf}', 0x43: f'rxor R, vf{is_}{fsf}',
                 0x64: f'mfp.{d} vf{it}, P', 0x68: f'xtop vi{it}', 0x69: f'xitop vi{it}', 0x6C: f'xgkick vi{is_}',
                 0x70: f'esadd P, vf{is_}', 0x71: f'ersadd P, vf{is_}', 0x72: f'eleng P, vf{is_}', 0x73: f'erleng P, vf{is_}',
                 0x74: f'eatanxy P, vf{is_}', 0x75: f'eatanxz P, vf{is_}', 0x76: f'esum P, vf{is_}', 0x78: f'esqrt P, vf{is_}{fsf}',
                 0x79: f'ersqrt P, vf{is_}{fsf}', 0x7A: f'ercpr P, vf{is_}{fsf}', 0x7B: 'waitp', 0x7C: f'esin P, vf{is_}{fsf}',
                 0x7D: f'eatan P, vf{is_}{fsf}', 0x7E: f'eexp P, vf{is_}{fsf}'}
        return table.get(sp, f'?lower {w:08x}') if op >= 0x3C else f'?lower {w:08x}'
    op = w >> 25
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    imm12 = ((w >> 10) & 0x800) | (w & 0x7FF)
    table = {0x00: f'lq.{d} vf{it}, {imm11}(vi{is_})', 0x01: f'sq.{d} vf{is_}, {imm11}(vi{it})',
             0x04: f'ilw.{d} vi{it}, {imm11}(vi{is_})', 0x05: f'isw.{d} vi{it}, {imm11}(vi{is_})',
             0x08: f'iaddiu vi{it}, vi{is_}, {imm15}', 0x09: f'isubiu vi{it}, vi{is_}, {imm15}',
             0x10: f'fceq vi1, {w & 0xFFFFFF:06x}', 0x11: f'fcset {w & 0xFFFFFF:06x}', 0x12: f'fcand vi1, {w & 0xFFFFFF:06x}',
             0x13: f'fcor vi1, {w & 0xFFFFFF:06x}', 0x14: f'fseq vi{it}, {imm12:03x}', 0x15: f'fsset {imm12:03x}',
             0x16: f'fsand vi{it}, {imm12:03x}', 0x17: f'fsor vi{it}, {imm12:03x}', 0x18: f'fmeq vi{it}, vi{is_}',
             0x1A: f'fmand vi{it}, vi{is_}', 0x1B: f'fmor vi{it}, vi{is_}', 0x1C: f'fcget vi{it}',
             0x20: f'b {target:04x}', 0x21: f'bal vi{it}, {target:04x}', 0x24: f'jr vi{is_}', 0x25: f'jalr vi{it}, vi{is_}',
             0x28: f'ibeq vi{it}, vi{is_}, {target:04x}', 0x29: f'ibne vi{it}, vi{is_}, {target:04x}',
             0x2C: f'ibltz vi{is_}, {target:04x}', 0x2D: f'ibgtz vi{is_}, {target:04x}', 0x2E: f'iblez vi{is_}, {target:04x}',
             0x2F: f'ibgez vi{is_}, {target:04x}'}
    return table.get(op, f'?lower {w:08x}')


def main():
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x4000
    for pc in range(start, end, 8):
        lo, up = struct.unpack_from('<II', data, pc)
        low = f'loi {struct.unpack("<f", struct.pack("<I", lo))[0]:g}' if up >> 31 & 1 else lower(lo, pc)
        print(f'{pc:04x}: {upper(up):42s} {low}')


if __name__ == '__main__':
    main()
