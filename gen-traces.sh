#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

NODE=./out/Release/node
TSC=$(which tsc)
ZOD_DIR=/tmp/zod/packages/zod

echo "=== small_trace.bin ==="
NODE_TRACE_FILE=small_trace.bin $NODE -e "
'use strict';
function add(a, b) { return a + b; }
function mul(a, b) { return a * b; }
function compute(x) { return add(mul(x, 3), mul(x, 7)); }
let acc = 0;
for (let i = 0; i < 500; i++) acc = compute(acc & 0xff);
"
echo "  $(du -sh small_trace.bin | cut -f1)"

echo "=== large_trace.bin ==="
NODE_TRACE_FILE=large_trace.bin $NODE gen-large-trace.js
echo "  $(du -sh large_trace.bin | cut -f1)"

echo "=== zod_trace.bin ==="
if [ ! -d "$ZOD_DIR" ]; then
  echo "  cloning zod..."
  git clone --depth=1 https://github.com/colinhacks/zod /tmp/zod
fi
NODE_TRACE_FILE=zod_trace.bin $NODE $TSC -p "$ZOD_DIR/tsconfig.bench.json"
echo "  $(du -sh zod_trace.bin | cut -f1)"

echo "done."
