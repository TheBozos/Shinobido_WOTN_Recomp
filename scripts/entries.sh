#!/usr/bin/env bash
# Rebuilds recomp/shino_entries.txt (bozo-recomp --entries: the jump-table targets of functions whose indirect jumps
# it could not resolve) from the last build's report. Run after a build; the next build uses it.
source "$(dirname "$0")/common.sh"
python -I "$ROOT/tools/shino_entries.py" "$GAME_BUILD/shino_prep/shino.elf" "$GAME_BUILD/shino_prep/shino_report.txt" \
    "$ROOT/recomp/shino_entries.txt"
