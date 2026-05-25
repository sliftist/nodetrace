'use strict';
const fs = require('fs');

const T_UINT8=0x01,T_UINT16=0x02,T_UINT32=0x03,T_UINT64=0x04;
const T_STRING=0x10,T_FUNCNAME=0x11,T_FUNCREF=0x12;
const EV_ENTER=0x00,EV_EXIT=0x01,EV_SUSPEND=0x02,EV_RESUME=0x03,EV_OSR=0x04,EV_TURBOFAN=0x05;

function readTrace(buf) {
  let pos = 0;
  const nameCache = [];
  const events = [];
  const u8  = () => buf[pos++];
  const u16 = () => { const v = buf.readUInt16LE(pos); pos += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const u64 = () => { const lo = buf.readUInt32LE(pos), hi = buf.readUInt32LE(pos+4); pos+=8; return BigInt(hi)*0x100000000n+BigInt(lo); };
  const str = () => { const len = u16(); const s = buf.toString('utf8', pos, pos+len); pos+=len; return s; };
  while (pos < buf.length) {
    const nf = u8(); const vals = [];
    for (let i = 0; i < nf; i++) {
      const tag = u8();
      if      (tag===T_UINT8)    vals.push(u8());
      else if (tag===T_UINT16)   vals.push(u16());
      else if (tag===T_UINT32)   vals.push(u32());
      else if (tag===T_UINT64)   vals.push(u64());
      else if (tag===T_STRING)   vals.push(str());
      else if (tag===T_FUNCNAME) { const n=str(); nameCache.unshift(n); if(nameCache.length>128)nameCache.pop(); vals.push(n); }
      else if (tag===T_FUNCREF)  { const d=u8(); vals.push(nameCache[d-1]??'(unknown)'); }
    }
    const [evType, ts, ...rest] = vals;
    if      (evType===EV_ENTER)    events.push({type:'ENTER',   ts, func:rest[0], isAsync:!!rest[1], callId:rest[2]});
    else if (evType===EV_TURBOFAN) events.push({type:'TURBOFAN',ts, tsEnd:rest[0], count:rest[1]});
    else                           events.push({type:['ENTER','EXIT','SUSPEND','RESUME','OSR'][evType]??`0x${evType.toString(16)}`, ts, func:rest[0], callId:rest[1]});
  }
  return events;
}

function analyze(events) {
  const callInfo  = Object.create(null); // callId -> {func, enterTs, exitTs, parentId, activeNs}
  const stack     = [];                  // [{callId, func, enterTs}]
  const ownNs     = Object.create(null); // func -> total own nanoseconds
  const callCount = Object.create(null); // func -> call count
  const uniqueFuncs = new Set();
  let turboFanTotal = 0n;

  for (const ev of events) {
    if (ev.type === 'TURBOFAN') { turboFanTotal += ev.count; continue; }

    if (ev.type === 'ENTER') {
      uniqueFuncs.add(ev.func);
      callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
      const parentId = stack.length > 0 ? stack[stack.length-1].callId : null;
      callInfo[ev.callId] = { func: ev.func, enterTs: ev.ts, exitTs: null, parentId, childNs: 0n };
      stack.push({ callId: ev.callId, func: ev.func, enterTs: ev.ts });
    } else if (ev.type === 'EXIT') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      const dur = ev.ts - info.enterTs;
      // own time = total duration minus time attributed to children
      const own = dur - info.childNs;
      ownNs[info.func] = (ownNs[info.func] ?? 0n) + (own > 0n ? own : 0n);
      // credit parent with this call's total duration
      if (info.parentId != null && callInfo[info.parentId]) {
        callInfo[info.parentId].childNs += dur;
      }
      // pop stack
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) stack.splice(idx, 1);
      delete callInfo[ev.callId];
    } else if (ev.type === 'SUSPEND') {
      // treat suspend as a temporary exit: credit parent with time so far
      const info = callInfo[ev.callId];
      if (!info) continue;
      info._suspendTs = ev.ts;
      const dur = ev.ts - info.enterTs;
      if (info.parentId != null && callInfo[info.parentId]) {
        callInfo[info.parentId].childNs += dur;
      }
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) stack.splice(idx, 1);
    } else if (ev.type === 'RESUME') {
      const info = callInfo[ev.callId];
      if (!info) continue;
      // re-enter: reset enterTs for next segment
      info.enterTs = ev.ts;
      info.childNs = 0n;
      stack.push({ callId: ev.callId, func: ev.func ?? info.func, enterTs: ev.ts });
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
console.log(`│  TurboFan calls (batched)  ${String(turboFanTotal.toLocaleString()).padStart(12)} │`);
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
