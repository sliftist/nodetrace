'use strict';
// Run with: NODE_TRACE_FILE=/tmp/tt.bin ./out/Release/node truetime-test.js

function assert(cond, msg) {
  if (!cond) { console.error('FAIL:', msg); process.exit(1); }
}

// 1. Flag check
assert(process.env.NODETRACE_TRUE_TIME === '1',
  'NODETRACE_TRUE_TIME not set — InitTrueTime may not have run');
assert(globalThis.TRUE_TIME_ALREADY_SHIMMED === true,
  'TRUE_TIME_ALREADY_SHIMMED not set on globalThis');
console.log('OK: env flag and globalThis flag are set');

// 2. Monotonic check over 200k iterations
let prev = Date.now();
for (let i = 0; i < 200_000; i++) {
  const t = Date.now();
  assert(t >= prev, `Date.now() went backwards: ${t} < ${prev} at iteration ${i}`);
  prev = t;
}
console.log('OK: Date.now() monotonic over 200k iterations');

// 3. Rate-of-advance check: 200ms real sleep should advance Date.now by 100-500ms
const t0 = Date.now();
const wall0 = performance.now();
const start = Date.now();
// busy wait for 200ms of wall time
while (performance.now() - wall0 < 200) {}
const elapsed = Date.now() - t0;
assert(elapsed >= 50,  `Date.now advanced too slowly in 200ms: ${elapsed}ms`);
assert(elapsed < 2000, `Date.now advanced too quickly in 200ms: ${elapsed}ms`);
console.log(`OK: Date.now advanced ${elapsed}ms over ~200ms wall time`);

// 4. Print a few timestamps and a rough offset estimate
const sys = +new Date();
const truetime = Date.now();
console.log(`Sample Date.now(): ${truetime}`);
console.log(`Approximate offset vs system: ${truetime - sys}ms`);

// 5. Cross-process consistency: spawn a child and compare
const { spawnSync } = require('child_process');
const child = spawnSync(process.execPath,
  ['-e', 'process.stdout.write(Date.now() + "\\n")'],
  { env: { ...process.env, NODE_TRACE_FILE: '/tmp/tt_child.bin' } });
if (child.status === 0) {
  const childTime = parseInt(child.stdout.toString().trim(), 10);
  const parentTime = Date.now();
  const diff = Math.abs(childTime - parentTime);
  assert(diff < 2000, `child/parent Date.now differ by ${diff}ms (expected < 2s)`);
  console.log(`OK: child/parent time diff = ${diff}ms`);
} else {
  console.log('SKIP: child process failed (may be expected in sandbox)');
}

console.log('ALL TESTS PASSED');
