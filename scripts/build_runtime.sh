#!/usr/bin/env bash
# Configure and build shino-headless and shinobido (the window).
# Usage: scripts/build_runtime.sh [extra cmake -D options]
source "$(dirname "$0")/common.sh"
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "$@"
cmake --build "$GAME_BUILD" -j "$JOBS"
