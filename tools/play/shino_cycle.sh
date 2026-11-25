#!/usr/bin/env bash
# cycle.sh <in-state> <in-frame> <mission index k> <out-state> [mode]
# One Hideout cycle: normalize the menus, open the mission list, accept mission k, complete it (mode done: the cheat;
# kill: the kill loop; mixed: the kill loop, then the cheat; thief: the item, then an exit; goal: the goal markers;
# garden: defend the garden), pass the rating and scenes, save the next Hideout state. Captures in build/cyc/<name>/.
# The cheat completes a mission's goals without doing them: a collection then fails ("Embezzler", nothing to send).
# LETTERS=1 first opens the newest letter (Mission Scroll, Letters; its scene plays). The lords' respect is
# printed at the end: Ichijo's, Akame's and Sadame's, the words at 0x6264e8, 0x6265a8 and 0x626668 (percent).
set -u
IN=$1; F=$2; K=$3; OUT=$4; MODE=${5:-done}
cd /d/Shinobido_WOTN_Recomp
NAME=$(basename "$OUT" .state)
DIR=build/cyc/$NAME; rm -rf "$DIR"; mkdir -p "$DIR"
python - "$F" "$K" "$MODE" "$DIR" "${LETTERS:-0}" <<'EOF'
import sys
F,K,mode,d,letters=int(sys.argv[1]),int(sys.argv[2]),sys.argv[3],sys.argv[4],int(sys.argv[5])
ev=[]; f=F+20
lcaps=[]
if letters and mode!="garden": # Mission Scroll, Letters, the newest letters opened one by one, back to the top menu
    for b in ("CROSS","TRIANGLE","TRIANGLE","TRIANGLE","CROSS","DOWN","CROSS"):
        ev.append(f"{f}:{b}/4"); f+=100
    # the newest letter only: a Down here can reach the top menu after a scene (the menus wrap), then Equipment
    for dd in (0,150,300,450): ev.append(f"{f+dd}:CROSS/4")
    lcaps+=[f+200,f+600,f+1200]
    ev.append(f"{f+2600}:TRIANGLE/4"); f+=2750
    for i in range(3): ev.append(f"{f}:TRIANGLE/4"); f+=100
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
elif mode=="drop": # a rival clan holding a soul fragment: the kill loop, Grab after each kill (the leader drops it)
    for i in range(30):
        t=acc+1100+150*i; tp.append(f"{t}:enemy")
        ev+=[f"{t+dd}:SQUARE/6" for dd in (15,35,55,75)]+[f"{t+dd}:TRIANGLE/4" for dd in (95,110,125,140)]
    t=acc+1100+150*30
    tp+=[f"{t}:done",f"{t+600}:done"]
    spam=t+1100
elif mode=="thief": # Thievery: beside the item, one Grab (a second drops it), then the nearest exit
    tp=[f"{acc+1300}:item",f"{acc+1900}:goal",f"{acc+2500}:goal"]
    ev.append(f"{acc+1400}:TRIANGLE/4")
    spam=acc+3300
elif mode=="goal": # Transport: the goal markers
    tp=[f"{acc+1300}:goal",f"{acc+1900}:goal",f"{acc+2500}:goal"]
    spam=acc+3300
else:
    for i in range(40):
        t=acc+1100+120*i; tp.append(f"{t}:enemy"); ev+=[f"{t+dd}:SQUARE/6" for dd in (15,35,55,75)]
    spam=acc+1100+4900
for e in tp: # the delivery prompts after a completion ("Send to the recipient right now?", "Send only the requested?"):
    if e.endswith((":done",":goal")): # Yes, before the spam's Triangle can answer No (the lord then loses trust)
        t=int(e.split(":")[0]); ev+=[f"{t+100}:LEFT/4"]+[f"{t+dd}:CROSS/4" for dd in (160,260,360,460)]
end=spam+9200 # room for a rival ninja's encounter after the mission (SHINO_ANSWER_DANGER)
t=spam
while t<end-700:
    ev.append(f"{t}:CROSS/4"); ev.append(f"{t+120}:TRIANGLE/4"); t+=300
for i in range(4): ev.append(f"{end-600+100*i}:TRIANGLE/4")
ev.sort(key=lambda e:int(e.split(":")[0])); open(f"{d}/pad.txt","w").write(",".join(ev)); open(f"{d}/tp.txt","w").write(";".join(tp))
caps=lcaps+[listframe]+list(range(acc+1300,end,250))+[end-5]
open(f"{d}/caps.txt","w").write(",".join(map(str,caps)))
open(f"{d}/end.txt","w").write(str(end))
EOF
END=$(cat $DIR/end.txt)
KILLS=""; [ "$MODE" != done ] && KILLS="SHINO_CHEAT_KILLS=1"
env $KILLS PS2X_FRAME_CAP=0 PS2X_CAPTURE_SCENE=1 PS2X_CAPTURE_WINDOW_DIR=$DIR SHINO_CAPTURE_FRAMES=$(cat $DIR/caps.txt) \
  SHINO_DUMP_RAM_AT=$((END-20)) SHINO_ANSWER_DANGER=1 SHINO_TRACE_MISSION_EVENTS=1 SHINO_CHEAT_HEALTH=1 SHINO_CHEAT_UNSEEN=1 SHINO_TELEPORT_AT="$(cat $DIR/tp.txt)" \
  timeout 3500 build/bozo/shinobido.exe --hidden --mute --card-dir build/cards_test2 --load-state "$IN" \
  --pad-script $DIR/pad.txt --save-state "$OUT" --save-state-at $((END-10)) --frames $END > $DIR/run.txt 2>&1
RC=$?
RESPECT=$(python -c "import struct,sys; r=open(sys.argv[1],'rb').read(); print(','.join(str(struct.unpack_from('<i',r,o)[0]) for o in (0x6264e8,0x6265a8,0x626668)))" build/ram_$((END-20)).bin 2>/dev/null)
rm -f build/ram_$((END-20)).bin
echo "$NAME end=$END rc=$RC $(grep -c 'state 1 -> 1[25]' $DIR/run.txt) ends $(grep -ci 'fault' $DIR/run.txt) faults respect=$RESPECT"
py tools/play/shino_sheet.py $DIR build/sheet_$NAME.png 10 >/dev/null 2>&1
