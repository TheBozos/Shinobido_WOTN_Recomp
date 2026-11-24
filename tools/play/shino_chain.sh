#!/usr/bin/env bash
# chain.sh <start state> <start frame> <first number> <count> [k]: cycle.sh repeated, each from the last one's state.
cd /d/Shinobido_WOTN_Recomp
S=$1; F=$2; N=$3; C=$4; K=${5:-1}
for ((i=0;i<C;i++)); do
  OUT=build/h$((N+i)).state
  MODE=done; [ "${PREV_ENDS:-1}" = 0 ] && [ "${PREV_MODE:-done}" = done ] && MODE=garden
  KK=$K; [ "$K" = 0 ] && KK=$(( (N+i) % 4 + 1 ))
  if [ "$K" = best ]; then
    P=$(bash tools/play/shino_pick.sh "$S" "$F"); KK=$(echo "$P" | awk '{print $1}'); echo "pick: $P"
    MODE=${PICK_MODE:-done}; [ "$KK" = 0 ] && { MODE=garden; KK=1; }
    [ "${PREV_ENDS:-1}" = 0 ] && [ "${PREV_MODE:-done}" != garden ] && MODE=garden
  fi
  [ -n "${FIRST_MODE:-}" ] && [ $i = 0 ] && MODE=$FIRST_MODE
  R=$(bash tools/play/shino_cycle.sh "$S" "$F" "$KK" "$OUT" $MODE); echo "$R $MODE k=$KK"
  PREV_ENDS=$(echo "$R" | awk '{print $4}'); PREV_MODE=$MODE
  [ -f "$OUT" ] || { echo "no state $OUT: stopped"; exit 1; }
  F=$(( $(cat build/cyc/h$((N+i))/end.txt) - 10 ))
  S=$OUT
  echo "$S $F" > build/cyc/last.txt
done
