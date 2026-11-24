#!/usr/bin/env bash
# A play-test driver (docs/RECOMP_NOTES.md, "Playing the story with the cheats"): the window build, hidden, from a save state.
# chain.sh <start state> <start frame> <first number> <count> [k]: cycle.sh repeated, each from the last one's state.
cd /d/Shinobido_WOTN_Recomp
S=$1; F=$2; N=$3; C=$4; K=${5:-1}
for ((i=0;i<C;i++)); do
  OUT=build/h$((N+i)).state
  MODE=done; [ "${PREV_ENDS:-1}" = 0 ] && [ "${PREV_MODE:-done}" = done ] && MODE=garden
  KK=$K; [ "$K" = 0 ] && KK=$(( (N+i) % 4 + 1 ))
  [ -n "${FIRST_MODE:-}" ] && [ $i = 0 ] && MODE=$FIRST_MODE
  R=$(bash tools/play/shino_cycle.sh "$S" "$F" "$KK" "$OUT" $MODE); echo "$R $MODE k=$KK"
  PREV_ENDS=$(echo "$R" | awk '{print $4}'); PREV_MODE=$MODE
  [ -f "$OUT" ] || { echo "no state $OUT: stopped"; exit 1; }
  F=$(( $(cat build/cyc/h$((N+i))/end.txt) - 10 ))
  S=$OUT
  echo "$S $F" > build/cyc/last.txt
done
