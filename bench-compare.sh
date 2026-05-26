#!/usr/bin/env bash
# bench-compare.sh — compare vanilla v26 vs patched v27 across workloads and throttle levels
#
# Usage:  ./bench-compare.sh [runs]   (default: 3 runs, best of N)
#
# Requires:
#   /opt/bin/node      — vanilla Node.js v26
#   ./out/Release/node — patched nodetrace build
#   /tmp/zod/          — zod source (cloned by gen-traces.sh if missing)

set -euo pipefail
cd "$(dirname "$0")"

VANILLA=/opt/bin/node
PATCHED=./out/Release/node
TSC=$(which tsc)
ZOD_DIR=/tmp/zod/packages/zod
RUNS=${1:-3}

# ── Helpers ────────────────────────────────────────────────────────────────────

best_ms() {
  local cmd="$1" best=999999 ms
  for i in $(seq 1 "$RUNS"); do
    ms=$( { time eval "$cmd" >/dev/null 2>&1; } 2>&1 \
          | grep real | sed 's/.*m//;s/s//' | awk '{printf "%.0f", $1*1000}' )
    [ "$ms" -lt "$best" ] && best=$ms
  done
  echo "$best"
}

trace_stats() {
  local bin="$1"
  local sz enter total
  sz=$(( $(stat -c%s "$bin") / 1024 ))
  enter=$(node trace-reader.js "$bin" 2>&1 | grep "^  ENTER " | grep -oP '[\d,]+' | tr -d ',' | head -1)
  total=$(node trace-reader.js "$bin" 2>&1 | grep "Total accounted" | grep -oP '[\d,]+' | tr -d ',' | head -1)
  printf "%6dKB  enter=%-10s total=%s" "$sz" "${enter:-?}" "${total:-?}"
}

print_row() {
  local label="$1" ms="$2" extra="${3:-}"
  printf "  %-28s %5dms%s\n" "$label" "$ms" "$extra"
}

# ── Ensure zod source ──────────────────────────────────────────────────────────

if [ ! -f "$ZOD_DIR/tsconfig.bench.json" ]; then
  echo "Cloning zod..."
  git clone --depth=1 https://github.com/colinhacks/zod /tmp/zod
fi

# ── Workload 1: bench-overhead.js (pure Ignition micro, no JIT) ───────────────

echo ""
echo "══════════════════════════════════════════════════════════════════════"
echo "  bench-overhead.js — 10M × step() (5 fns each), Ignition only"
echo "══════════════════════════════════════════════════════════════════════"
printf "  %-28s %5s\n" "configuration" "best/${RUNS}"
echo "  ────────────────────────────────────────────────────────────────────"

print_row "vanilla v26 (no trace)"     "$(best_ms "$VANILLA bench-overhead.js")"
print_row "patched v27 (no trace)"     "$(best_ms "$PATCHED bench-overhead.js")"

for lim in 1000 10000 100000 1000000; do
  f="/tmp/boh_${lim}.bin"
  ms=$(best_ms "INSPECT_MAX_PER_SECOND=$lim NODE_TRACE_FILE=$f $PATCHED bench-overhead.js")
  print_row "patched v27 trace ${lim}/sec" "$ms" "  $(trace_stats "$f")"
done

# ── Workload 2: zod tsc compilation ───────────────────────────────────────────

echo ""
echo "══════════════════════════════════════════════════════════════════════"
echo "  zod tsc — compile zod TypeScript source (~2s real workload)"
echo "══════════════════════════════════════════════════════════════════════"
printf "  %-28s %5s\n" "configuration" "best/${RUNS}"
echo "  ────────────────────────────────────────────────────────────────────"

ZOD_CMD="$TSC -p $ZOD_DIR/tsconfig.bench.json"

print_row "vanilla v26 (no trace)"     "$(best_ms "$VANILLA $ZOD_CMD")"
print_row "patched v27 (no trace)"     "$(best_ms "$PATCHED $ZOD_CMD")"

for lim in 1000 10000 100000 1000000; do
  f="/tmp/bzod_${lim}.bin"
  ms=$(best_ms "INSPECT_MAX_PER_SECOND=$lim NODE_TRACE_FILE=$f $PATCHED $ZOD_CMD")
  print_row "patched v27 trace ${lim}/sec" "$ms" "  $(trace_stats "$f")"
done

# ── Workload 3: workload.js (async multi-function, timer fires) ───────────────

echo ""
echo "══════════════════════════════════════════════════════════════════════"
echo "  workload.js — 40 rounds, mixed sync/async, ~60ms"
echo "══════════════════════════════════════════════════════════════════════"
printf "  %-28s %5s\n" "configuration" "best/${RUNS}"
echo "  ────────────────────────────────────────────────────────────────────"

print_row "patched v27 (no trace)"     "$(best_ms "$PATCHED workload.js")"

for lim in 1000 10000 100000 1000000; do
  f="/tmp/bwl_${lim}.bin"
  ms=$(best_ms "INSPECT_MAX_PER_SECOND=$lim NODE_TRACE_FILE=$f $PATCHED workload.js")
  print_row "patched v27 trace ${lim}/sec" "$ms" "  $(trace_stats "$f")"
done

echo ""
