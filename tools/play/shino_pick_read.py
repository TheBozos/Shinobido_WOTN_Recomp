"""pick_read.py <ram dump>: see pick.sh."""
import re
import struct
import sys

ram = open(sys.argv[1], 'rb').read()
offered = []
for i in range(40):
    a = 0x6271dc + 0xa4 * i
    if struct.unpack_from('<i', ram, a - 0x10)[0] != 1:
        break
    offered.append((struct.unpack_from('<i', ram, a)[0], struct.unpack_from('<i', ram, a - 0x14)[0]))
if not offered:
    print(0)
    sys.exit()
titles = []
news = 'News'.encode('utf-16-le') + b'\0\0'
for m in re.finditer(re.escape(news), ram):
    if m.start() % 2:
        continue
    seg = ram[m.start():m.start() + 0x400]
    found = [x.decode('utf-16-le', 'ignore') for x in re.findall(rb'(?:[\x20-\x7e]\x00){4,}', seg)][1:]
    if found and '$' not in ''.join(found):
        titles = found[:len(offered)]
lord = [i for i, t in enumerate(titles)
        if re.match(r"(Kill|Defeat|Assassinate|Execute) (Nobuteru |Hideyoshi )?(Ichijo|Sadame|Akame)$", t)]
k = lord[0] if lord else max(range(len(offered)), key=lambda i: offered[i][0])
print(k + 1, *[f'{r}:{c}' for r, c in offered], '|', ' / '.join(titles).encode('ascii', 'replace').decode())
