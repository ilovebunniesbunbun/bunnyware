#!/usr/bin/env bash
# baseline_capture.sh — Phase 0 CPU baseline capture for bunnyware
#
# Usage:
#   ./tools/baseline_capture.sh [label] [app_log_path]
#
# Examples:
#   ./tools/baseline_capture.sh match ~/baseline_app.log   # while IN a match
#   ./tools/baseline_capture.sh menu                       # while in CS2 main menu
#
# Env knobs:
#   SAMPLE_SECS=30   Duration of each perf/strace capture (seconds)
#   CPU_SAMPLES=5    pidstat/top iterations, 3s apart (~15s of CPU sampling)
#
# Notes:
#   - Run as your normal user. The script uses sudo ONLY to attach to the
#     root-owned overlay process (perf/strace/pgrep).
#   - The overlay must already be running, ideally attached to CS2 in a match
#     (more players -> stronger signal on the reader thread).
#   - Do NOT run other strace/perf sessions at the same time.

set -uo pipefail

LABEL="${1:-capture}"
APP_LOG="${2:-$HOME/baseline_app.log}"
SAMPLE_SECS="${SAMPLE_SECS:-30}"
CPU_SAMPLES="${CPU_SAMPLES:-5}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_DIR="$HOME/bunnyware_baseline"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$BASE_DIR/${LABEL}_${STAMP}"
mkdir -p "$OUT_DIR"
ln -sfn "$OUT_DIR" "$BASE_DIR/latest"

have() { command -v "$1" >/dev/null 2>&1; }
run() { echo "+ $*"; "$@"; }
banner() { echo; echo "=================================================================="; echo "== $*"; echo "=================================================================="; }

# --- Dependency check --------------------------------------------------------
MISSING=()
have pidstat || MISSING+=("pidstat  (pacman -S sysstat) -> fallback: top -H")
have perf    || MISSING+=("perf     (pacman -S perf)    -> fallback: strace -c")
have strace  || MISSING+=("strace   (pacman -S strace)")
if ((${#MISSING[@]})); then
    echo "WARNING: missing tools, fallbacks will be used where possible:"
    printf '  - %s\n' "${MISSING[@]}"
fi
if ! have perf && ! have strace; then
    echo "ERROR: need at least one of perf or strace for syscall counting." >&2
    exit 1
fi

# --- Find the overlay process ------------------------------------------------
PID="$(pgrep -n -x bunnyware 2>/dev/null || sudo pgrep -n -x bunnyware 2>/dev/null || true)"
if [[ -z "${PID:-}" ]]; then
    echo "ERROR: no running 'bunnyware' process found." >&2
    echo "Start it first:  sudo $REPO_ROOT/build/bunnyware 2>&1 | tee ~/baseline_app.log" >&2
    exit 1
fi

sudo -v   # cache sudo credentials up front

PERF_STEPS=2; have perf && PERF_STEPS=3
TOTAL=$((CPU_SAMPLES * 3 + SAMPLE_SECS * PERF_STEPS))
echo
echo "Target PID : $PID ($(cat /proc/$PID/comm 2>/dev/null))"
echo "Output dir : $OUT_DIR"
echo "Duration   : ~${TOTAL}s total. Keep the game running during capture."
echo

# --- 00: Machine & config context --------------------------------------------
banner "00/05: machine & config context"
{
    echo "date:        $(date -Is)"
    echo "uname:       $(uname -a)"
    echo "nproc:       $(nproc)"
    echo "cpu:         $(lscpu | grep -m1 'Model name' | sed 's/.*: *//')"
    echo "pid:         $PID"
    echo "label:       $LABEL"
    echo "sample_secs: $SAMPLE_SECS"
    echo
    echo "--- default.json (relevant settings) ---"
    python3 - "$REPO_ROOT/default.json" <<'EOF' 2>/dev/null || echo "(could not parse default.json)"
import json, sys
c = json.load(open(sys.argv[1]))
keys = ["fps", "vsync", "gamereader_local_ms", "gamereader_players_ms",
        "gamereader_entities_ms", "debug_game_reader", "debug_gpu_profiling",
        "esp_enabled", "chams_enabled", "esp_smoke_check",
        "map_visualizer_enabled", "menu_scale"]
for k in keys:
    print(f"{k}: {c.get(k)}")
EOF
    echo
    echo "--- thread list ---"
    ps -L -p "$PID" -o pid,tid,comm,%cpu --no-headers 2>/dev/null || sudo ps -L -p "$PID" -o pid,tid,comm,%cpu --no-headers
} > "$OUT_DIR/00_context.txt"
run cat "$OUT_DIR/00_context.txt"

# --- 01: Per-thread CPU -------------------------------------------------------
banner "01/05: per-thread CPU (${CPU_SAMPLES} samples, 3s apart)"
if have pidstat; then
    run pidstat -t -p "$PID" 3 "$CPU_SAMPLES" | tee "$OUT_DIR/01_thread_cpu.txt"
else
    run top -b -H -p "$PID" -n "$CPU_SAMPLES" -d 3 | tee "$OUT_DIR/01_thread_cpu.txt"
fi
sleep 2

# --- 02: Syscall rates ---------------------------------------------------------
banner "02/05: syscall rates (${SAMPLE_SECS}s)  [validates readv/ioctl findings]"
if have perf; then
    sudo perf stat \
        -e 'syscalls:sys_enter_process_vm_readv,syscalls:sys_enter_ioctl,syscalls:sys_enter_clock_nanosleep,syscalls:sys_enter_nanosleep' \
        -p "$PID" -- sleep "$SAMPLE_SECS" 2>&1 | tee "$OUT_DIR/02_syscalls.txt"
else
    echo "(perf missing — using strace -c; CPU during this window is NOT representative)"
    run sudo timeout -s TERM "${SAMPLE_SECS}s" strace -c -f -p "$PID" -o "$OUT_DIR/02_syscalls_strace.txt"
    run cat "$OUT_DIR/02_syscalls_strace.txt"
fi
sleep 2

# --- 03: Scheduler stats (context switches expose spin/mutex contention) -------
banner "03/05: scheduler stats (${SAMPLE_SECS}s)"
if have perf; then
    sudo perf stat -p "$PID" -- sleep "$SAMPLE_SECS" 2>&1 | tee "$OUT_DIR/03_perfstat.txt"
else
    run pidstat -w -t -p "$PID" 3 "$CPU_SAMPLES" | tee "$OUT_DIR/03_perfstat.txt"
fi
sleep 2

# --- 04: Hotspot profile -------------------------------------------------------
banner "04/05: hotspot profile (${SAMPLE_SECS}s)"
if have perf; then
    run sudo perf record -F 199 -g -p "$PID" -o "$OUT_DIR/04_perf.data" -- sleep "$SAMPLE_SECS"
    sudo perf report --stdio --no-children -i "$OUT_DIR/04_perf.data" 2>/dev/null \
        | head -60 | tee "$OUT_DIR/04_hotspots.txt"
    sudo chown "$USER:$(id -gn)" "$OUT_DIR/04_perf.data" 2>/dev/null || true
else
    echo "(skipped — perf not installed)" | tee "$OUT_DIR/04_hotspots.txt"
fi

# --- 05: Reader internal stats (needs debug_game_reader=true in default.json) --
banner "05/05: GameReader loop stats"
if [[ -f "$APP_LOG" ]]; then
    grep 'GameReader Loop Stats' "$APP_LOG" | tail -10 | tee "$OUT_DIR/05_reader_stats.txt" || \
        echo "(no 'GameReader Loop Stats' lines found — set \"debug_game_reader\": true in default.json and restart the overlay)" \
        | tee "$OUT_DIR/05_reader_stats.txt"
else
    echo "(app log not found at '$APP_LOG' — pass it as 2nd arg: $0 $LABEL /path/to/app.log)" \
        | tee "$OUT_DIR/05_reader_stats.txt"
fi

# --- SUMMARY -------------------------------------------------------------------
banner "capture complete"
{
    echo "# bunnyware CPU baseline — $LABEL ($STAMP)"
    for f in 00_context.txt 01_thread_cpu.txt 02_syscalls.txt 02_syscalls_strace.txt \
             03_perfstat.txt 04_hotspots.txt 05_reader_stats.txt; do
        [[ -f "$OUT_DIR/$f" ]] || continue
        echo
        echo "## $f"
        echo '```'
        cat "$OUT_DIR/$f"
        echo '```'
    done
} > "$OUT_DIR/SUMMARY.txt"

echo "Results in: $OUT_DIR"
echo "Paste back to me: $OUT_DIR/SUMMARY.txt"
