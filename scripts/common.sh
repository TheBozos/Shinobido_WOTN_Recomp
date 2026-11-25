# Sourced by the other scripts. Resolves the repo root and tool paths.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="/c/msys64/ucrt64/bin:$HOME/.local/share/mise/shims:$PATH"

GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SCES_539.31"
GAME_BUILD="${GAME_BUILD:-$ROOT/build/bozo}"
JOBS="${JOBS:-$(nproc)}"

# Native executables carry .exe on Windows (MSYS2).
EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

# GCC needs a temporary folder (a shell started without TMP/TEMP sends it to C:\Windows, refused); it is always the
# build tree's, so nothing the build makes lands on C:.
if true; then
    mkdir -p "$ROOT/build/tmp"
    export TMP="$ROOT/build/tmp" TEMP="$ROOT/build/tmp"
fi
