#!/usr/bin/env bash
# Run bunnyware under gdb and capture a full backtrace when it crashes.
#
# The overlay needs root, so this invokes gdb through sudo. When a fatal signal
# fires, gdb stops and writes a report to crash-gdb.log (repo root) before
# quitting. Reproduce the tab-swap crash, then inspect that log.
#
# Usage:
#   tools/debug-crash.sh [-- <bunnyware args>]
#   tools/debug-crash.sh -- --x11
#
# Override the binary with BUNNYWARE_BIN=/path/to/bunnyware.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BUNNYWARE_BIN:-./build-debug/bunnyware}"
GDB_SCRIPT="tools/gdb-crash.txt"
LOG="crash-gdb.log"

if [[ ! -x "$BIN" ]]; then
    echo "error: '$BIN' not found or not executable." >&2
    echo "Build a debug-symbol binary first:" >&2
    echo "  cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUNNYWARE_BUILD_TESTS=OFF" >&2
    echo "  make -j\"\$(nproc)\" -C build-debug" >&2
    exit 1
fi

command -v gdb >/dev/null || { echo "error: gdb not installed" >&2; exit 1; }

echo "==> Running $BIN under gdb"
echo "==> Reproduce the crash (swap menu tabs). Report will be written to: $LOG"
exec sudo gdb -q -x "$GDB_SCRIPT" --args "$BIN" "$@"
