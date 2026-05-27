'use strict';
const fs = require('fs');
const { readTrace } = require('./trace-parser');

function analyze(events) {
  const callInfo    = Object.create(null);
  const stack       = [];
  const ownNs       = Object.create(null);
  const callCount   = Object.create(null);
  const uniqueFuncs = new Set();
  let turboFanTotal = 0n;

  for (const ev of events) {
    if (ev.type === 'OPTIMIZED_BATCH') { turboFanTotal += ev.count; continue; }

    if (ev.type === 'ENTER') {
      uniqueFuncs.add(ev.func);
      callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
      const parentId = stack.length > 0 ? stack[stack.length - 1].callId : null;
      callInfo[ev.callId] = { func: ev.func, enterTs: ev.ts, parentId, childNs: 0n };
      stack.push({ callId: ev.callId, func: ev.func });
    } else if (ev.type === 'EXIT') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      const dur = ev.ts - info.enterTs;
      const own = dur - info.childNs;
      ownNs[info.func] = (ownNs[info.func] ?? 0n) + (own > 0n ? own : 0n);
      if (info.parentId != null && callInfo[info.parentId])
        callInfo[info.parentId].childNs += dur;
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) stack.splice(idx, 1);
      delete callInfo[ev.callId];
    } else if (ev.type === 'SUSPEND') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      const dur = ev.ts - info.enterTs;
      if (info.parentId != null && callInfo[info.parentId])
        callInfo[info.parentId].childNs += dur;
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) stack.splice(idx, 1);
    } else if (ev.type === 'RESUME') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      info.enterTs = ev.ts;
      info.childNs = 0n;
      stack.push({ callId: ev.callId, func: ev.func ?? info.func });
    }
  }

  return { ownNs, callCount, uniqueFuncs: uniqueFuncs.size,
           totalCalls: Object.values(callCount).reduce((a, b) => a + b, 0),
           turboFanTotal };
}

const path = process.argv[2] ?? '/tmp/zod_trace.bin';
const buf  = fs.readFileSync(path);
console.log(`Trace: ${path}  (${(buf.length / 1e6).toFixed(1)} MB)\n`);

const { events } = readTrace(buf);
const { ownNs, callCount, uniqueFuncs, totalCalls, turboFanTotal } = analyze(events);

console.log('┌─────────────────────────────────────────┐');
console.log('│           Trace summary                 │');
console.log('├─────────────────────────────────────────┤');
console.log(`│  Unique functions traced   ${String(uniqueFuncs).padStart(12)} │`);
console.log(`│  Total Ignition calls      ${String(totalCalls.toLocaleString()).padStart(12)} │`);
console.log(`│  Optimized calls (batched) ${String(turboFanTotal.toLocaleString()).padStart(12)} │`);
console.log('└─────────────────────────────────────────┘');

const top = Object.entries(ownNs)
  .sort((a, b) => (a[1] > b[1] ? -1 : a[1] < b[1] ? 1 : 0))
  .slice(0, 3);

console.log('\nTop 3 functions by own time:\n');
console.log(`  ${'own ms'.padStart(10)}  ${'calls'.padStart(8)}  name`);
for (const [fn, ns] of top) {
  const ms  = (Number(ns) / 1e6).toFixed(2).padStart(10);
  const cnt = (callCount[fn] ?? 0).toLocaleString().padStart(8);
  console.log(`  ${ms}  ${cnt}  ${fn}`);
}
