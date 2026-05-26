'use strict';
// Run with: NODE_TRACE_FILE=/tmp/ttm-main.bin node truetime-multi-test.js
// Requires root (uses `date -s` to shift system clock).

const { spawnSync } = require('child_process');

function assert(cond, msg) {
  if (!cond) { console.error('FAIL:', msg); process.exit(1); }
}

// Trigger tracing init in this process
function _dummy() {} _dummy();

assert(typeof process.trueTimeOffset === 'function',
  'process.trueTimeOffset not set — tracing may not be active');

// ── Helper: run a short-lived child with tracing on ────────────────────────────
function child(code, traceFile = '/tmp/ttm-child.bin') {
  const r = spawnSync(process.execPath, ['-e', `
    // Trigger tracing init
    (function _init() {})();
    ${code}
  `], {
    env: { ...process.env, NODE_TRACE_FILE: traceFile },
    timeout: 30000,
    encoding: 'utf8',
  });
  if (r.status !== 0) {
    console.error('child stderr:', r.stderr);
    throw new Error(`child exited ${r.status}`);
  }
  return r.stdout.trim();
}

// ── Helper: change system clock by delta seconds (requires root) ────────────────
function shiftClock(deltaSec) {
  const newEpoch = Math.floor(Date.now() / 1000) + deltaSec;
  const r = spawnSync('date', ['-s', `@${newEpoch}`], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('date -s failed: ' + r.stderr);
}

// ── Test 1: cross-process target offset equality ─────────────────────────────
console.log('\n=== Test 1: cross-process target offset (triple-equal) ===');

// Process A: force fresh NTP sync, print its target offset
const rawA = child(`
  const t = process.trueTimeForceResync();  // blocking NTP, writes to shm
  process.stdout.write(String(t));
`, '/tmp/ttm-A.bin');
const targetA = parseFloat(rawA);
console.log('Process A target offset (after resync):', targetA, 'ms');

// Process B: read target offset from shm WITHOUT re-syncing
const rawB = child(`
  const t = process.trueTimeTargetOffset();  // reads shm, no NTP query
  process.stdout.write(String(t));
`, '/tmp/ttm-B.bin');
const targetB = parseFloat(rawB);
console.log('Process B target offset (from shm)    :', targetB, 'ms');

assert(targetA === targetB,
  `Target offsets differ: A=${targetA} B=${targetB} (diff=${targetA - targetB}ms)`);
console.log('OK: process A and B have identical target offset (triple-equal)');

// ── Test 2: current (smeared) offsets should also be close ───────────────────
console.log('\n=== Test 2: current offset consistency ===');

const curA = parseFloat(child(
  'process.stdout.write(String(process.trueTimeOffset()))', '/tmp/ttm-A.bin'));
const curB = parseFloat(child(
  'process.stdout.write(String(process.trueTimeOffset()))', '/tmp/ttm-B.bin'));
console.log('Process A current offset:', curA.toFixed(3), 'ms');
console.log('Process B current offset:', curB.toFixed(3), 'ms');
const curDiff = Math.abs(curA - curB);
assert(curDiff < 10, `Current offsets differ by ${curDiff.toFixed(3)}ms (expected < 10ms)`);
console.log(`OK: current offsets agree within ${curDiff.toFixed(3)}ms`);

// ── Test 3: system time change → both processes pick up new offset ────────────
console.log('\n=== Test 3: system time shift by -60s, both processes update ===');

const SHIFT_SEC = -60;
const before_A = parseFloat(child(
  'process.stdout.write(String(process.trueTimeTargetOffset()))', '/tmp/ttm-A.bin'));
console.log('Before shift — target offset:', before_A.toFixed(1), 'ms');

console.log(`Shifting system clock by ${SHIFT_SEC}s...`);
shiftClock(SHIFT_SEC);

// NTP will report that real time is ~60s AHEAD of system time now
// so the new offset should be roughly +60000ms
const afterA = parseFloat(child(`
  const t = process.trueTimeForceResync();
  process.stdout.write(String(t));
`, '/tmp/ttm-A.bin'));
console.log('After shift — Process A new target offset:', afterA.toFixed(1), 'ms');

const afterB = parseFloat(child(
  'process.stdout.write(String(process.trueTimeTargetOffset()))',
  '/tmp/ttm-B.bin'));
console.log('After shift — Process B target offset (from shm):', afterB.toFixed(1), 'ms');

assert(afterA === afterB,
  `After shift: A=${afterA} B=${afterB} not equal`);
console.log('OK: both processes have identical target offset after shift (triple-equal)');

const expectedOffset = -SHIFT_SEC * 1000;  // +60000ms
const offsetDelta = Math.abs(afterA - expectedOffset);
assert(offsetDelta < 5000,
  `New offset ${afterA}ms is not close to expected +${expectedOffset}ms (delta=${offsetDelta}ms)`);
console.log(`OK: new offset ${afterA.toFixed(0)}ms ≈ expected ${expectedOffset}ms (delta=${offsetDelta.toFixed(0)}ms)`);

// ── Restore system clock ─────────────────────────────────────────────────────
console.log('\nRestoring system clock...');
shiftClock(-SHIFT_SEC);  // undo the shift
child('process.trueTimeForceResync(); process.stdout.write("ok")', '/tmp/ttm-A.bin');

const restored = parseFloat(child(
  'process.stdout.write(String(process.trueTimeTargetOffset()))', '/tmp/ttm-A.bin'));
console.log('After restore — target offset:', restored.toFixed(1), 'ms');
assert(Math.abs(restored) < 5000,
  `Offset after restore is ${restored}ms — clock may not have been restored properly`);
console.log('OK: clock restored, offset back near zero');

// ── Wipe shm (clean up state for future runs) ────────────────────────────────
const { unlinkSync } = require('fs');
try { unlinkSync('/dev/shm/nodetrace-timesync'); } catch (_) {}

console.log('\nALL TESTS PASSED');
