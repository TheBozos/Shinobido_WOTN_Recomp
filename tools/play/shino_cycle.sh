#!/usr/bin/env bash
# cycle.sh <in-state> <in-frame> <mission index k> <out-state> [mode]
# One Hideout cycle: normalize the menus, open the mission list, accept mission k, complete it (mode done: the cheat;
# kill: the kill loop), pass the rating and scenes, save the next Hideout state. Captures in build/cyc/<out name>/.
set -u
IN=$1; F=$2; K=$3; OUT=$4; MODE=${5:-done}
cd /d/Shinobido_WOTN_Recomp
NAME=$(basename "$OUT" .state)
DIR=build/cyc/$NAME; rm -rf "$DIR"; mkdir -p "$DIR"
python - "$F" "$K" "$MODE" "$DIR" <<'EOF'
import sys
F,K,mode,d=int(sys.argv[1]),int(sys.argv[2]),sys.argv[3],sys.argv[4]
ev=[]; f=F+20
if mode=="garden": # the raid's dialog dismissed, To Garden, Defend, "Shall I have some fun?" Yes
    for b in ("CROSS","TRIANGLE","TRIANGLE","TRIANGLE","DOWN","DOWN","DOWN","CROSS","CROSS","CROSS","CROSS"):
        ev.append(f"{f}:{b}/4"); f+=100
    listframe=f-10; acc=f-1000
else:
    for b in ("CROSS","TRIANGLE","TRIANGLE","TRIANGLE","CROSS","CROSS"):
        ev.append(f"{f}:{b}/4"); f+=100
    listframe=f-10
    for i in range(K): ev.append(f"{f}:DOWN/4"); f+=30
    ev+=[f"{f+10}:CROSS/4",f"{f+120}:START/4",f"{f+200}:LEFT/4",f"{f+240}:CROSS/4"]
    acc=f+240
tp=[]
if mode=="done":
    tp=[f"{acc+1500}:done",f"{acc+2100}:done",f"{acc+2700}:done"]
    spam=acc+3200
elif mode=="mixed": # the kill loop (a target really dies), then the completion if the mission has not ended
    for i in range(24):
        t=acc+1100+120*i; tp.append(f"{t}:enemy"); ev+=[f"{t+dd}:SQUARE/6" for dd in (15,35,55,75)]
    t=acc+1100+120*24
    tp+=[f"{t}:done",f"{t+600}:done"]
    spam=t+1100
else:
    for i in range(40):
        t=acc+1100+120*i; tp.append(f"{t}:enemy"); ev+=[f"{t+dd}:SQUARE/6" for dd in (15,35,55,75)]
    spam=acc+1100+4900
end=spam+5200
t=spam
while t<end-700:
    ev.append(f"{t}:CROSS/4"); ev.append(f"{t+120}:TRIANGLE/4"); t+=300
for i in range(4): ev.append(f"{end-600+100*i}:TRIANGLE/4")
open(f"{d}/pad.txt","w").write(",".join(ev)); open(f"{d}/tp.txt","w").write(";".join(tp))
caps=[listframe]+list(range(acc+1300,end,250))+[end-5]
open(f"{d}/caps.txt","w").write(",".join(map(str,caps)))
open(f"{d}/end.txt","w").write(str(end))
EOF
END=$(cat $DIR/end.txt)
KILLS=""; [ "$MODE" != done ] && KILLS="SHINO_CHEAT_KILLS=1"
env $KILLS PS2X_FRAME_CAP=0 PS2X_CAPTURE_SCENE=1 PS2X_CAPTURE_WINDOW_DIR=$DIR SHINO_CAPTURE_FRAMES=$(cat $DIR/caps.txt) \
  SHINO_TRACE_MISSION_EVENTS=1 SHINO_CHEAT_HEALTH=1 SHINO_CHEAT_UNSEEN=1 SHINO_TELEPORT_AT="$(cat $DIR/tp.txt)" \
  timeout 3500 build/bozo/shinobido.exe --hidden --mute --card-dir build/cards_test2 --load-state "$IN" \
  --pad-script $DIR/pad.txt --save-state "$OUT" --save-state-at $((END-10)) --frames $END > $DIR/run.txt 2>&1
echo "$NAME end=$END rc=$? $(grep -c 'state 1 -> 1[25]' $DIR/run.txt) ends $(grep -ci 'fault' $DIR/run.txt) faults"
py tools/play/shino_sheet.py $DIR build/sheet_$NAME.png 10 >/dev/null 2>&1
