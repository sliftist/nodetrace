'use strict';
// Measures the overhead of function-level tracing.
//
// Finds the worst case for Ignition tracing: a tight loop calling many small
// functions (each call = 1 TraceEnter + 1 TraceExit runtime call overhead).
//
// To measure tracing overhead, run the SAME binary with tracing on vs off:
//
//   Baseline (tracing disabled):
//     ./out/Release/node bench-overhead.js
//
//   Worst case (tracing enabled, writes to /dev/null so disk isn't the limit):
//     NODE_TRACE_FILE=/dev/null ./out/Release/node bench-overhead.js
//
// The difference between those two runs is the pure tracing overhead.
// Do NOT compare against a different Node.js version — version differences
// and our turbo_filter="heavyCompute" restriction dwarf the tracing cost.

// ── Workload: tiny functions, call-intensive, stays in Ignition ───────────────

function add(a, b)  { return a + b; }
function mul(a, b)  { return a * b; }
function xor(a, b)  { return a ^ b; }
function shr(a, b)  { return a >>> b; }

// Each call to step() makes 4 inner calls → 5 total traced calls per iteration.
function step(x) {
  const a = add(x, 13);
  const b = mul(x & 0xff, 7);
  const c = xor(a, b);
  return shr(c, 1);
}

const DURATION_MS = 2000;

function hotLoop() {
  const deadline = Date.now() + DURATION_MS;
  let acc = 1, iters = 0;
  while (Date.now() < deadline) {
    for (let i = 0; i < 1000; i++) acc = step(acc & 0xffff);
    iters += 1000;
  }
  return { acc, iters };
}

// Warm up (prevents first-call bias; V8 profile feedback settles)
hotLoop();

const t0 = Date.now();
const { acc, iters } = hotLoop();
const elapsed = Date.now() - t0;

const callsPerIter = 5;  // step + add + mul + xor + shr
const totalCalls = iters * callsPerIter;
const callsPerSec = Math.round(totalCalls / (elapsed / 1000));

const tracingLabel = process.env.NODE_TRACE_FILE
  ? `tracing → ${process.env.NODE_TRACE_FILE}`
  : 'tracing disabled';

console.log(`Node.js ${process.version}  (${process.execPath})`);
console.log(`Tracing: ${tracingLabel}`);
console.log(`Window:  ${DURATION_MS} ms`);
console.log();
const nsPerCall = (elapsed * 1e6 / totalCalls).toFixed(1);

console.log(`  ${iters.toLocaleString()} step() calls`);
console.log(`  ${totalCalls.toLocaleString()} total traced calls`);
console.log(`  ${callsPerSec.toLocaleString()} calls/sec  (${nsPerCall} ns/call)`);
console.log(`  Checksum: ${acc & 0xffffffff}`);
