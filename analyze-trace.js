'use strict';
const fs = require('fs');

const EV_ENTER    = 0x00;
const EV_EXIT     = 0x01;
const EV_SUSPEND  = 0x02;
const EV_RESUME   = 0x03;
const EV_ON_STACK_REPLACEMENT = 0x04;
const EV_OPTIMIZED_BATCH = 0x05;
const EV_NEWNAME  = 0x06;

function readTrace(buf) {
  let pos = 0;
  const names = [];
  const events = [];
  let lastTs = 0n;

  const u8  = () => buf[pos++];
  const u16 = () => { const v = buf.readUInt16LE(pos); pos += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const u64 = () => {
    const lo = buf.readUInt32LE(pos);
    const hi = buf.readUInt32LE(pos + 4);
    pos += 8;
    return BigInt(hi) * 0x100000000n + BigInt(lo);
  };

  const readDelta = (ss) => {
    switch (ss) {
      case 0: return BigInt(u8());
      case 1: return BigInt(u16());
      case 2: return BigInt(u32());
      case 3: return u64();
    }
  };

  const readRef = () => names[u32()] ?? '(unknown)';

  while (pos < buf.length) {
    const header = u8();
    const ss   = (header >> 6) & 0x03;
    const type = header & 0x3F;

    const delta = readDelta(ss);
    lastTs += delta;
    const ts = lastTs;

    if (type === EV_NEWNAME) {
      const idx  = u32();
      const len  = u16();
      const name = buf.toString('utf8', pos, pos + len);
      pos += len;
      names[idx] = name;
    } else if (type === EV_ENTER) {
      const func    = readRef();
      const isAsync = u8();
      const callId  = u32();
      events.push({ type: 'ENTER', ts, func, isAsync: !!isAsync, callId });
    } else if (type === EV_OPTIMIZED_BATCH) {
      const count = BigInt(u32());
      const minTs = u64();
      const maxTs = u64();
      events.push({ type: 'OPTIMIZED_BATCH', ts, count, minTs, maxTs });
    } else if (type <= EV_ON_STACK_REPLACEMENT) {
      const func   = readRef();
      const callId = u32();
      events.push({ type: ['ENTER','EXIT','SUSPEND','RESUME','ON_STACK_REPLACEMENT'][type], ts, func, callId });
    } else {
      throw new Error(`Unknown event type 0x${type.toString(16)} at offset ${pos}`);
    }
  }

  return events;
}

function analyze(events) {
  const callInfo  = Object.create(null);
  const stack     = [];
  const ownNs     = Object.create(null);
  const callCount = Object.create(null);
  const uniqueFuncs = new Set();
  let turboFanTotal = 0n;

  for (const ev of events) {
    if (ev.type === 'OPTIMIZED_BATCH') { turboFanTotal += ev.count; continue; }

    if (ev.type === 'ENTER') {
      uniqueFuncs.add(ev.func);
      callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
      const parentId = stack.length > 0 ? stack[stack.length-1].callId : null;
      callInfo[ev.callId] = { func: ev.func, enterTs: ev.ts, parentId, childNs: 0n };
      stack.push({ callId: ev.callId, func: ev.func });
    } else if (ev.type === 'EXIT') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      const dur = ev.ts - info.enterTs;
      const own = dur - info.childNs;
      ownNs[info.func] = (ownNs[info.func] ?? 0n) + (own > 0n ? own : 0n);
      if (info.parentId != null && callInfo[info.parentId]) {
        callInfo[info.parentId].childNs += dur;
      }
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) stack.splice(idx, 1);
      delete callInfo[ev.callId];
    } else if (ev.type === 'SUSPEND') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      const dur = ev.ts - info.enterTs;
      if (info.parentId != null && callInfo[info.parentId]) {
        callInfo[info.parentId].childNs += dur;
      }
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

  return { ownNs, callCount, uniqueFuncs: uniqueFuncs.size, totalCalls: Object.values(callCount).reduce((a,b)=>a+b,0), turboFanTotal };
}

const path = process.argv[2] ?? '/tmp/zod_trace.bin';
const buf = fs.readFileSync(path);
console.log(`Trace: ${path}  (${(buf.length/1e6).toFixed(1)} MB)\n`);

const events = readTrace(buf);
const { ownNs, callCount, uniqueFuncs, totalCalls, turboFanTotal } = analyze(events);

console.log('┌─────────────────────────────────────────┐');
console.log('│           Trace summary                 │');
console.log('├─────────────────────────────────────────┤');
console.log(`│  Unique functions traced   ${String(uniqueFuncs).padStart(12)} │`);
console.log(`│  Total Ignition calls      ${String(totalCalls.toLocaleString()).padStart(12)} │`);
console.log(`│  Optimized calls (batched) ${String(turboFanTotal.toLocaleString()).padStart(12)} │`);
console.log('└─────────────────────────────────────────┘');

const top = Object.entries(ownNs)
  .map(([fn, ns]) => [fn, ns])
  .sort((a, b) => (a[1] > b[1] ? -1 : a[1] < b[1] ? 1 : 0))
  .slice(0, 3);

console.log('\nTop 3 functions by own time:\n');
console.log(`  ${'own ms'.padStart(10)}  ${'calls'.padStart(8)}  name`);
for (const [fn, ns] of top) {
  const ms  = (Number(ns) / 1e6).toFixed(2).padStart(10);
  const cnt = (callCount[fn] ?? 0).toLocaleString().padStart(8);
  console.log(`  ${ms}  ${cnt}  ${fn}`);
}
