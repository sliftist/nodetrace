'use strict';
// Generates a large trace file (~50 MB) by running a workload that exercises
// many Ignition-traced function calls.
//
// The TraceWriter buffers up to 128 MB in memory and flushes at process exit,
// so we can't check the output file size during the run.  Instead we target
// a fixed wall-clock duration calibrated to produce ~50 MB.
//
// Usage:
//   NODE_TRACE_FILE=large_trace.bin ./out/Release/node gen-large-trace.js
//   node trace-reader.js large_trace.bin

// ── Inner functions ────────────────────────────────────────────────────────────

function f1(x) { return x * 17 + 3; }
function f2(x) { return f1(x) ^ (x >>> 1); }
function f3(x) { return f2(x) + f1(x + 1); }
function f4(x) { return f3(x) - f2(x - 1); }
function f5(x) { return f4(x) + f3(x + 2); }

function batchA(n) {
  let a = 0;
  for (let i = 0; i < n; i++) a = f5(a & 0xffff);
  return a;
}

function batchB(n) {
  let a = 0;
  for (let i = 0; i < n; i++) {
    a = f3(a & 0xffff);
    a = f4(a & 0xffff);
  }
  return a;
}

// ── Async wrapper to exercise SUSPEND/RESUME events ───────────────────────────

async function asyncYield(n) {
  await Promise.resolve();
  return batchA(n);
}

async function asyncLoop(iters, batchSize) {
  let total = 0;
  for (let i = 0; i < iters; i++) {
    total += batchB(batchSize);
    if (i % 50 === 0) total += await asyncYield(batchSize >> 1);
  }
  return total;
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
  const tracePath = process.env.NODE_TRACE_FILE;
  if (!tracePath) {
    console.error('Set NODE_TRACE_FILE before running');
    process.exit(1);
  }

  // Calibration: each asyncLoop(100, 150) call produces ~5.5 MB of trace data.
  // To reach ~50 MB we need ~9 such calls.
  const CALLS  = 9;
  const ITERS  = 100;
  const BATCH  = 150;

  console.log(`Generating ~50 MB trace to ${tracePath}`);
  console.log(`Running ${CALLS} × asyncLoop(${ITERS}, ${BATCH})...`);

  const t0 = Date.now();
  let total = 0;
  for (let c = 0; c < CALLS; c++) {
    total = await asyncLoop(ITERS, BATCH);
    process.stdout.write(`\r  pass ${c + 1}/${CALLS} done  (${((Date.now() - t0) / 1000).toFixed(1)}s elapsed)`);
  }

  const elapsed = ((Date.now() - t0) / 1000).toFixed(1);
  console.log(`\nDone in ${elapsed}s.  Checksum: ${total & 0xffffffff}`);
  console.log('Trace will be flushed to disk on exit.');
}

main().catch(err => { console.error(err); process.exit(1); });
