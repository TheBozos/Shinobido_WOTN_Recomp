#!/usr/bin/env bash
# chain.sh <start state> <start frame> <first number> <count> [k]: cycle.sh repeated, each from the last one's state.
# States build/<PREFIX><n>.state (PREFIX h by default); k best picks with shino_pick.sh (LORD=<n>: serve that lord;
# PICK_MODE=auto: the way to play from the title). STOP_ON_DEATH=1 stops when a lord's respect falls to 0 (his death).
cd "$(dirname "$0")/../.." || exit 1
S=$1; F=$2; N=$3; C=$4; K=${5:-1}
for ((i=0;i<C;i++)); do
  OUT=build/${PREFIX:-h}$((N+i)).state
  MODE=done; [ "${PREV_ENDS:-1}" = 0 ] && [ "${PREV_MODE:-done}" = done ] && MODE=garden
  KK=$K; [ "$K" = 0 ] && KK=$(( (N+i) % 4 + 1 ))
  if [ "$K" = best ]; then
    P=$(bash tools/play/shino_pick.sh "$S" "$F"); KK=$(echo "$P" | awk '{print $1}'); echo "pick: $P"
    MODE=${PICK_MODE:-done}; [ "$MODE" = auto ] && MODE=$(echo "$P" | awk '{print $2}')
    [ "$KK" = 0 ] && { MODE=garden; KK=1; }
    [ "${PREV_ENDS:-1}" = 0 ] && [ "${PREV_MODE:-done}" != garden ] && MODE=garden
  fi
  [ -n "${FIRST_MODE:-}" ] && [ $i = 0 ] && MODE=$FIRST_MODE
  R=$(bash tools/play/shino_cycle.sh "$S" "$F" "$KK" "$OUT" $MODE); echo "$R $MODE k=$KK"
  PREV_ENDS=$(echo "$R" | awk '{print $4}'); PREV_MODE=$MODE
  RES=$(echo "$R" | sed -n 's/.*respect=\([-0-9,]*\).*/\1/p') # a lord's respect at 0 from above: he died
  if [ -n "${STOP_ON_DEATH:-}" ] && [ -n "${PREV_RES:-}" ] && [ -n "$RES" ]; then
    for j in 1 2 3; do a=$(echo $PREV_RES | cut -d, -f$j); b=$(echo $RES | cut -d, -f$j)
      [ "$a" -gt 0 ] && [ "$b" = 0 ] && { echo "lord $j's respect $a -> 0: stopped"; exit 2; }; done
  fi
  PREV_RES=$RES
  [ -f "$OUT" ] || { echo "no state $OUT: stopped"; exit 1; }
  F=$(( $(cat build/cyc/${PREFIX:-h}$((N+i))/end.txt) - 10 ))
  S=$OUT
  echo "$S $F" > build/cyc/last_${PREFIX:-h}.txt
done
