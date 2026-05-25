'use strict';
// Tracing overhead benchmark — fixed call count, measures total wall time.
//
// Run twice and compare:
//   Baseline (Node.js v26, no tracing instrumentation):
//     /opt/bin/node bench-overhead.js
//
//   With tracing (our node, worst case — writes to /dev/null):
//     NODE_TRACE_FILE=/dev/null ./out/Release/node bench-overhead.js

// ── Workload: tiny functions, no JIT (names don't match turbo_filter) ─────────

function add(a, b)  { return a + b; }
function mul(a, b)  { return a * b; }
function xor(a, b)  { return a ^ b; }
function shr(a, b)  { return a >>> b; }

// Each step() call makes 4 inner calls → 5 total traced calls per iteration.
function step(x) {
  const a = add(x, 13);
  const b = mul(x & 0xff, 7);
  const c = xor(a, b);
  return shr(c, 1);
}

const N = 10_000_000;  // fixed number of step() calls

// Warm-up pass (lets V8 settle its profile feedback)
let acc = 1;
for (let i = 0; i < 100_000; i++) acc = step(acc & 0xffff);

// Timed run
acc = 1;
const t0 = Date.now();
for (let i = 0; i < N; i++) acc = step(acc & 0xffff);
const elapsed = Date.now() - t0;

const totalCalls = N * 5;  // step + add + mul + xor + shr
const tracingLabel = process.env.NODE_TRACE_FILE
  ? `tracing enabled → ${process.env.NODE_TRACE_FILE}`
  : 'tracing disabled';

console.log(`Node.js ${process.version}  (${process.execPath})`);
console.log(`Tracing: ${tracingLabel}`);
console.log(`Calls:   ${N.toLocaleString()} × step()  =  ${totalCalls.toLocaleString()} total`);
console.log(`Time:    ${elapsed} ms`);
console.log(`Checksum: ${acc & 0xffffffff}`);
