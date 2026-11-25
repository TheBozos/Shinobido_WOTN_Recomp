#!/usr/bin/env bash
# pick.sh <state> <frame>: the day's offered missions and titles read at the list screen (tools/play/shino_pick_read.py);
# prints the chosen k (1-based, after News), the offers and the titles; "0" when none is offered.
# Only the rows on screen have their titles in memory, so: one RAM dump as the list opens, then one after each of 12
# Downs and 12 Ups (each scroll step draws one new row, whose title is the string that changed).
cd /d/Shinobido_WOTN_Recomp
S=$1; F=$2; L=$((F+700))
DUMPS=$L
for k in $(seq 1 24); do DUMPS="$DUMPS,$((L+50*k+40))"; done
LAST=$((L+50*24+40))
python -c "
F=$F; L=$L; ev=[]; f=F+20
for b in ('CROSS','TRIANGLE','TRIANGLE','TRIANGLE','CROSS','CROSS'):
    ev.append(f'{f}:{b}/4'); f+=100
for k in range(1,25): ev.append(f'{L+50*k}:{\"DOWN\" if k<=12 else \"UP\"}/4')
open('build/cyc/pick_pad.txt','w').write(','.join(ev))"
SHINO_DUMP_RAM_AT=$DUMPS timeout 1500 build/bozo/shino-headless.exe --card-dir build/cards_test2 --load-state "$S" \
  --pad-script build/cyc/pick_pad.txt --frames $((LAST+2)) > /dev/null 2>&1
FILES=""
for d in $(echo $DUMPS | tr ',' ' '); do FILES="$FILES build/ram_$d.bin"; done
PYTHONIOENCODING=utf-8 python tools/play/shino_pick_read.py $FILES
[ -n "${KEEP:-}" ] || rm -f $FILES
