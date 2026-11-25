#!/usr/bin/env bash
# pick.sh <state> <frame>: the day's offered missions and titles read at the list screen (tools/play/shino_pick_read.py);
# prints the chosen k (1-based, after News), the offers and the titles; "0" when none is offered.
# RAM dumps: the list as it opens, then one per Down past the fourth row (only the rows shown have their titles in
# memory: each step shows one more row, its title the new string).
cd /d/Shinobido_WOTN_Recomp
S=$1; F=$2; L=$((F+700))  # the list's four first rows all written by then
DUMPS=$L
for k in $(seq 4 12); do DUMPS="$DUMPS,$((L+40+50*(k-1)+45))"; done
LAST=$((L+40+50*11+45))
python -c "
F=$F; L=$L; ev=[]; f=F+20
for b in ('CROSS','TRIANGLE','TRIANGLE','TRIANGLE','CROSS','CROSS'):
    ev.append(f'{f}:{b}/4'); f+=100
f=L+40
for i in range(12): ev.append(f'{f}:DOWN/4'); f+=50
open('build/cyc/pick_pad.txt','w').write(','.join(ev))"
SHINO_DUMP_RAM_AT=$DUMPS timeout 1500 build/bozo/shino-headless.exe --card-dir build/cards_test2 --load-state "$S" \
  --pad-script build/cyc/pick_pad.txt --frames $((LAST+2)) > /dev/null 2>&1
FILES=""
for d in $(echo $DUMPS | tr ',' ' '); do FILES="$FILES build/ram_$d.bin"; done
PYTHONIOENCODING=utf-8 python tools/play/shino_pick_read.py $FILES
[ -n "${KEEP:-}" ] || rm -f $FILES
