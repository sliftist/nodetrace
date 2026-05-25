'use strict';
// Accuracy test: verifies that Ignition ENTER count + TurboFan batch count
// equals the known number of JS function calls.
//
// Also benchmarks Ignition-traced vs TurboFan-compiled hot loops to show
// the performance difference (tracing overhead disappears once JIT'd).
//
// Usage:
//   NODE_TRACE_FILE=accuracy.bin ./out/Release/node test-accuracy.js
//   node trace-reader.js accuracy.bin

// ── Helpers ────────────────────────────────────────────────────────────────────

function nowMs() { return Date.now(); }

// ── Accuracy test ──────────────────────────────────────────────────────────────
// Call a simple function exactly N times and report the actual call count so
// the trace reader can verify Ignition_ENTER + TurboFan_batch === N.

function add(a, b) { return a + b; }

function runAccuracyTest(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = add(s, i);
  return s;
}

const ACCURACY_N = 50_000;
const accuracyResult = runAccuracyTest(ACCURACY_N);
console.log(`Accuracy test: called add() exactly ${ACCURACY_N.toLocaleString()} times`);
console.log(`  Checksum: ${accuracyResult}`);
console.log(`  Expected trace: ENTER(add) + TurboFan_batch_total(add) === ${ACCURACY_N.toLocaleString()}`);
console.log(`  (The trace reader reports total accounted calls which should be >= ${ACCURACY_N.toLocaleString()})`);
console.log();

// ── Performance: worst case (Ignition, many short calls) ───────────────────────
// Anything named "heavyCompute" gets TurboFan'd; everything else stays in
// Ignition (due to turbo_filter="heavyCompute" in our build).

function inner(x) { return x * x + 1; }
function mid(x)   { return inner(x) + inner(x - 1); }
function outer(x) { return mid(x) + mid(x + 1); }

function ignitionHotLoop() {
  const start = nowMs();
  let acc = 0, iters = 0;
  while (nowMs() - start < 200) {
    for (let i = 0; i < 100; i++) acc = outer(acc & 0xfff);
    iters += 100;
  }
  return { acc, iters };
}

const t0 = nowMs();
const { acc: acc1, iters: iters1 } = ignitionHotLoop();
const ignitionMs = nowMs() - t0;

console.log(`Ignition hot loop (200 ms window):`);
console.log(`  ${iters1.toLocaleString()} outer() calls in ${ignitionMs} ms`);
console.log(`  ${(iters1 / ignitionMs * 1000).toFixed(0)} calls/sec`);
console.log(`  Checksum: ${acc1 & 0xffffffff}`);
console.log();

// ── Performance: TurboFan-compiled (name it "heavyCompute") ───────────────────
// Because turbo_filter="heavyCompute", V8 will JIT this function.
// After JIT, calls stop appearing in the Ignition trace — zero tracing overhead.

function heavyCompute(x) {
  // Equivalent work to outer() above but in a single function so JIT covers it.
  const a = x * x + 1;
  const b = (x - 1) * (x - 1) + 1;
  const c = (x + 1) * (x + 1) + 1;
  return a + b + c;
}

function jitHotLoop() {
  const start = nowMs();
  let acc = 0, iters = 0;
  while (nowMs() - start < 200) {
    for (let i = 0; i < 100; i++) acc = heavyCompute(acc & 0xfff);
    iters += 100;
  }
  return { acc, iters };
}

const t1 = nowMs();
const { acc: acc2, iters: iters2 } = jitHotLoop();
const jitMs = nowMs() - t1;

console.log(`TurboFan hot loop (200 ms window, function named "heavyCompute"):`);
console.log(`  ${iters2.toLocaleString()} heavyCompute() calls in ${jitMs} ms`);
console.log(`  ${(iters2 / jitMs * 1000).toFixed(0)} calls/sec`);
console.log(`  Checksum: ${acc2 & 0xffffffff}`);
console.log();

const ratio = iters2 / iters1;
console.log(`Speedup from JIT (no tracing): ${ratio.toFixed(1)}x`);
console.log();

// ── Nested async (exercises SUSPEND/RESUME) ────────────────────────────────────

async function asyncStep(n) {
  await Promise.resolve();
  return n + 1;
}

async function asyncChain(depth) {
  let v = 0;
  for (let i = 0; i < depth; i++) v = await asyncStep(v);
  return v;
}

const chainResult = await asyncChain(20);
console.log(`Async chain (depth 20): result=${chainResult} (expect 20)`);
console.log('Done.');
