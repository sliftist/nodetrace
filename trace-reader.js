'use strict';
const fs = require('fs');
const { readTrace, PARAM_TYPE_NAMES } = require('./trace-parser');

function summarize(events) {
  const counts    = Object.create(null);
  const wallNs    = Object.create(null);
  const callCount = Object.create(null);
  const osrCount  = Object.create(null);
  const stack     = [];
  let turboFanTotal = 0n;

  for (const ev of events) {
    counts[ev.type] = (counts[ev.type] ?? 0) + 1;

    if (ev.type === 'ENTER') {
      stack.push({ func: ev.func, ts: ev.ts, callId: ev.callId });
      callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
    } else if (ev.type === 'EXIT') {
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) {
        const { func, ts } = stack.splice(idx, 1)[0];
        wallNs[func] = (wallNs[func] ?? 0) + Number(ev.ts - ts);
      }
    } else if (ev.type === 'ON_STACK_REPLACEMENT') {
      osrCount[ev.func] = (osrCount[ev.func] ?? 0) + 1;
    } else if (ev.type === 'OPTIMIZED_BATCH') {
      turboFanTotal += ev.count;
    }
  }

  return { counts, wallNs, callCount, osrCount, turboFanTotal };
}

const path = process.argv[2] ?? 'node_trace.bin';
if (!fs.existsSync(path)) { console.error(`File not found: ${path}`); process.exit(1); }

const buf = fs.readFileSync(path);
console.log(`Read ${buf.length.toLocaleString()} bytes from ${path}`);

const { events, names } = readTrace(buf);
console.log(`Decoded ${events.length.toLocaleString()} events\n`);

const { counts, wallNs, callCount, osrCount, turboFanTotal } = summarize(events);

const ignitionTotal = counts['ENTER'] ?? 0;
console.log('Event type breakdown:');
for (const [type, n] of Object.entries(counts))
  console.log(`  ${type.padEnd(25)} ${n.toLocaleString()}`);
console.log(`  ${'Unique names'.padEnd(25)} ${names.filter(Boolean).length.toLocaleString()}`);
console.log(`\n  Ignition calls (ENTER):    ${ignitionTotal.toLocaleString()}`);
console.log(`  Optimized calls (batched): ${turboFanTotal.toLocaleString()}`);
console.log(`  Total accounted calls:     ${(BigInt(ignitionTotal) + turboFanTotal).toLocaleString()}`);

const top = Object.entries(wallNs).sort((a, b) => b[1] - a[1]).slice(0, 25);
console.log('\nTop 25 functions by total wall time (Ignition only):');
console.log(`  ${'ms'.padStart(12)}  ${'calls'.padStart(8)}  ${'osr'.padStart(5)}  name`);
for (const [fn, ns] of top) {
  const ms    = (ns / 1e6).toFixed(2).padStart(12);
  const calls = (callCount[fn] ?? 0).toLocaleString().padStart(8);
  const osr   = (osrCount[fn] ?? 0).toLocaleString().padStart(5);
  console.log(`  ${ms}  ${calls}  ${osr}  ${fn}`);
}
