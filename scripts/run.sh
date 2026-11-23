#!/usr/bin/env bash
# The headless runner: scripts/run.sh [--frames N] [...]; SHINO_WINDOW_RUN=1 runs the window build instead.
source "$(dirname "$0")/common.sh"
cd "$ROOT"
BIN="$GAME_BUILD/shino-headless$EXE"
[ -n "${SHINO_WINDOW_RUN:-}" ] && BIN="$GAME_BUILD/shinobido$EXE"
[ -x "$BIN" ] || { echo "missing $BIN; run scripts/build_runtime.sh first"; exit 1; }
exec "$BIN" "$@"
