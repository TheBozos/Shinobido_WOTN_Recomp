"""shino_prepare.py <SCES_539.31> <out.elf>: the executable as bozo-recomp should read it.

SCES_539.31 is stripped and has one unnamed PROGBITS section (4) for its whole image, 0x100000-0x625a00, flagged WAX:
EE code, then VU microcode and data. bozo-recomp translates everything a code section covers, so the section is cut to
the EE code alone, 0x100000 up to 0x4f1500, where the VU microcode starts, and a second code section is added for the
C++ static constructors the linker put among the data, 0x614b80-0x617484 (their table at 0x617490 is walked by
0x101340 at boot; each registers a global object's destructor through 0x101310). The other R5900 code in the data,
0x524d18.., reads kernel memory (0x80075300) and is never reached from EE code: the SDK's kernel patch, which the
engine's native _InitSys replaces. The program headers (the load image) are unchanged, and the runner loads this
file: the CD library reads the ISO, not the ELF's folder. docs/RECOMP_NOTES.md has the evidence.
"""
import hashlib
import struct
import sys
from pathlib import Path

ELF_SHA1 = 'dbadec418726d0049c4311ef6fa97204cf6fe3f8'  # SCES_539.31, PAL 1.00
CODE_SECTION = 4
CODE = (0x100000, 0x4f1500)
CONSTRUCTORS = (0x614b80, 0x617484)
SH = struct.Struct('<10I')


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    elf = bytearray(Path(sys.argv[1]).read_bytes())
    if hashlib.sha1(elf).hexdigest() != ELF_SHA1:
        print('SCES_539.31: not the PAL Shinobido: Way of the Ninja executable this port is built against', file=sys.stderr)
        return 1
    shoff = struct.unpack_from('<I', elf, 0x20)[0]
    entry = shoff + CODE_SECTION * 40
    addr = struct.unpack_from('<I', elf, entry + 12)[0]
    if addr != CODE[0]:
        print(f'section {CODE_SECTION} at {addr:#x}, expected {CODE[0]:#x}', file=sys.stderr)
        return 1
    struct.pack_into('<I', elf, entry + 20, CODE[1] - CODE[0])  # sh_size
    # The constructors: a copy of section 4's header (PROGBITS, AX) at their address and file offset; the section
    # table is rewritten at the end of the file with it appended.
    shnum = struct.unpack_from('<H', elf, 0x30)[0]
    table = [list(SH.unpack_from(elf, shoff + i * 40)) for i in range(shnum)]
    ctors = list(table[CODE_SECTION])
    file_offset = ctors[4] + (CONSTRUCTORS[0] - CODE[0])
    ctors[2] = 0x6  # SHF_ALLOC | SHF_EXECINSTR
    ctors[3], ctors[4], ctors[5] = CONSTRUCTORS[0], file_offset, CONSTRUCTORS[1] - CONSTRUCTORS[0]
    table.append(ctors)
    del elf[shoff:shoff + shnum * 40]
    elf += bytes((-len(elf)) % 4)
    struct.pack_into('<I', elf, 0x20, len(elf))
    struct.pack_into('<H', elf, 0x30, len(table))
    elf += b''.join(SH.pack(*s) for s in table)
    out = Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(elf)
    return 0


if __name__ == '__main__':
    sys.exit(main())
