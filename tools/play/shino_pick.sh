#!/usr/bin/env bash
# pick.sh <state> <frame>: the day's offered missions and titles read at the list screen (tools/play/shino_pick_read.py);
# prints the chosen k (1-based, after News), the offers and the titles; "0" when none is offered.
cd /d/Shinobido_WOTN_Recomp
S=$1; F=$2; L=$((F+610))
python -c "
F=$F; ev=[]; f=F+20
for b in ('CROSS','TRIANGLE','TRIANGLE','TRIANGLE','CROSS','CROSS'):
    ev.append(f'{f}:{b}/4'); f+=100
open('build/cyc/pick_pad.txt','w').write(','.join(ev))"
SHINO_DUMP_RAM_AT=$L timeout 1500 build/bozo/shino-headless.exe --card-dir build/cards_test2 --load-state "$S" \
  --pad-script build/cyc/pick_pad.txt --frames $((L+2)) > /dev/null 2>&1
PYTHONIOENCODING=utf-8 python tools/play/shino_pick_read.py "build/ram_$L.bin"
[ -n "${KEEP:-}" ] || rm -f "build/ram_$L.bin"
