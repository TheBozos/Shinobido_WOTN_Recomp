#!/usr/bin/env bash
# Configure and build shino-headless and shinobido (the window).
# Usage: scripts/build_runtime.sh [extra cmake -D options]
source "$(dirname "$0")/common.sh"
# The movies' minimal static FFmpeg, once (scripts/build_ffmpeg.sh).
[ -f "$ROOT/build/ffmpeg/lib/libavcodec.a" ] || bash "$ROOT/scripts/build_ffmpeg.sh"
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "$@"
cmake --build "$GAME_BUILD" -j "$JOBS"
