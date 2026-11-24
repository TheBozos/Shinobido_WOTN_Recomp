"""shino_samples.py <executable> [samples.txt] [top N]: SHINO_SAMPLE's samples by function.

The runtime writes build/samples.txt (module, offset in it, samples); the functions are the executable's symbol
table (nm), at its image base. Prints the functions with the most samples and their share.
"""
import bisect
import subprocess
import sys

exe = sys.argv[1]
path = sys.argv[2] if len(sys.argv) > 2 else 'build/samples.txt'
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

headers = subprocess.run(['objdump', '-p', exe], capture_output=True, text=True).stdout
base = next(int(line.split()[1], 16) for line in headers.splitlines() if line.startswith('ImageBase'))
symbols = []
for line in subprocess.run(['nm', '-C', '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in 'tT':
        symbols.append((int(parts[0], 16) - base, parts[2]))
symbols.sort()
starts = [s for s, _ in symbols]

exports = {}  # another module's exported functions (objdump's export table), by module path


def exported(module):
    if module not in exports:
        # objdump -p: the Export Address Table ("[ i] +base[ o] <rva> Export RVA") and the name table
        # ("[ k] +base[ o] <i in hex> <name>"), joined on i.
        text = subprocess.run(['objdump', '-p', module], capture_output=True, text=True).stdout
        rva, names = {}, {}
        for line in text.splitlines():
            line = line.strip()
            if not line.startswith('[') or '+base[' not in line:
                continue
            index = int(line[1:line.index(']')])
            rest = line[line.index(']', line.index('+base[')) + 1:].split()
            if len(rest) >= 3 and rest[1:3] == ['Export', 'RVA']:
                rva[index] = int(rest[0], 16)
            elif len(rest) >= 2:
                try:
                    names[int(rest[0], 16)] = rest[1]
                except ValueError:
                    pass
        exports[module] = sorted((rva[i], name) for i, name in names.items() if i in rva)
    return exports[module]


def function_at(offset):
    i = bisect.bisect_right(starts, offset) - 1
    return symbols[i][1] if i >= 0 else '?'


by_function, callers, total = {}, {}, 0
for line in open(path):
    module, offset, n = line.split()
    offset, n = int(offset, 16), int(n)
    if module == 'caller':  # the executable's function a sample in another module was called from
        name = function_at(offset)
        callers[name] = callers.get(name, 0) + n
        continue
    total += n
    if module == 'exe':
        i = bisect.bisect_right(starts, offset) - 1
        name = symbols[i][1] if i >= 0 else '?'
    else:  # another module: its exported function before the offset (the C runtime's memcpy / malloc, the system's)
        table = exported(module)
        i = bisect.bisect_right(table, (offset, '￿')) - 1
        name = '[' + module.replace('\\', '/').split('/')[-1] + '] ' + (table[i][1] if i >= 0 else '?')
    by_function[name] = by_function.get(name, 0) + n
print(f'{total} samples')
for name, n in sorted(by_function.items(), key=lambda kv: -kv[1])[:top]:
    print(f'{100.0 * n / total:6.2f}% {n:7d}  {name[:150]}')
if callers:
    print('time in other modules (the C runtime, the system), by the executable function that called them:')
    for name, n in sorted(callers.items(), key=lambda kv: -kv[1])[:top // 2]:
        print(f'{100.0 * n / total:6.2f}% {n:7d}  {name[:150]}')
