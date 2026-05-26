'use strict';
// Decodes the binary trace produced by TraceWriter (deps/v8/src/trace/trace-writer.h).
//
// Usage:  node trace-reader.js [node_trace.bin]

const fs = require('fs');

const EV_ENTER    = 0x00;
const EV_EXIT     = 0x01;
const EV_SUSPEND  = 0x02;
const EV_RESUME   = 0x03;
const EV_ON_STACK_REPLACEMENT = 0x04;
const EV_OPTIMIZED_BATCH = 0x05;
const EV_NEWNAME  = 0x06;

// 0x05 OPTIMIZED_BATCH covers both JIT-compiled calls (TurboFan/Maglev/Sparkplug)
// and calls dropped by the INSPECT_MAX_PER_SECOND throttle.
const EV_NAME = ['ENTER', 'EXIT', 'SUSPEND', 'RESUME', 'ON_STACK_REPLACEMENT', 'OPTIMIZED_BATCH', 'NEW_NAME'];

// ── Binary reader ──────────────────────────────────────────────────────────────
// Format: flat stream of records, no per-field tags.
//   Header byte: [ss:2 | type:6]
//     ss=0 → 1-byte delta, ss=1 → 2-byte, ss=2 → 4-byte, ss=3 → 8-byte
//   Delta timestamp (LE, ss bytes) added to running absolute timestamp
//   Fixed fields per type (no tags):
//     NEW_NAME: name_idx(u32), len(u16), utf-8 bytes
//     ENTER:    name_idx(u32), is_async(u8), call_id(u32)
//     EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT: name_idx(u32), call_id(u32)
//     OPTIMIZED_BATCH: count(u32)
//   name_idx is permanent; reader keeps a flat array names[idx]=string.

function readTrace(buf) {
  let pos = 0;
  const names = [];   // names[idx] = string
  const events = [];
  let lastTs = 0n;
  let newNameEvents = 0;

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
      newNameEvents++;
    } else if (type === EV_ENTER) {
      const func    = readRef();
      const isAsync = u8();
      const callId  = u32();
      events.push({ type: 'ENTER', ts, func, isAsync: !!isAsync, callId });
    } else if (type === EV_OPTIMIZED_BATCH) {
      const count = BigInt(u32());
      events.push({ type: 'OPTIMIZED_BATCH', ts, count });
    } else if (type <= EV_ON_STACK_REPLACEMENT) {
      const func   = readRef();
      const callId = u32();
      events.push({ type: EV_NAME[type], ts, func, callId });
    } else {
      throw new Error(`Unknown event type 0x${type.toString(16)} at offset ${pos}`);
    }
  }

  return { events, uniqueNames: names.length, newNameEvents };
}

// ── Analysis ───────────────────────────────────────────────────────────────────

function summarize(events) {
  const counts    = Object.create(null);
  const wallNs    = Object.create(null);
  const callCount = Object.create(null);
  const osrCount  = Object.create(null);
  const stack = [];
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
        const dur = Number(ev.ts - ts);
        wallNs[func] = (wallNs[func] ?? 0) + dur;
      }
    } else if (ev.type === 'ON_STACK_REPLACEMENT') {
      osrCount[ev.func] = (osrCount[ev.func] ?? 0) + 1;
    } else if (ev.type === 'OPTIMIZED_BATCH') {
      turboFanTotal += ev.count;
    }
  }

  return { counts, wallNs, callCount, osrCount, turboFanTotal };
}

// ── Main ───────────────────────────────────────────────────────────────────────

const path = process.argv[2] ?? 'node_trace.bin';
if (!fs.existsSync(path)) {
  console.error(`File not found: ${path}`);
  process.exit(1);
}

const buf = fs.readFileSync(path);
console.log(`Read ${buf.length.toLocaleString()} bytes from ${path}`);

const { events, uniqueNames, newNameEvents } = readTrace(buf);
console.log(`Decoded ${events.length.toLocaleString()} events\n`);

const { counts, wallNs, callCount, osrCount, turboFanTotal } = summarize(events);

const ignitionTotal = counts['ENTER'] ?? 0;
console.log('Event type breakdown:');
for (const [type, n] of Object.entries(counts)) {
  console.log(`  ${type.padEnd(25)} ${n.toLocaleString()}`);
}
console.log(`  ${'NEW_NAME (emitted)'.padEnd(25)} ${newNameEvents.toLocaleString()}`);
console.log(`  ${'Unique function names'.padEnd(25)} ${uniqueNames.toLocaleString()}`);
console.log(`\n  Ignition calls (ENTER):    ${ignitionTotal.toLocaleString()}`);
console.log(`  Optimized calls (batched): ${turboFanTotal.toLocaleString()}`);
console.log(`  Total accounted calls:    ${(BigInt(ignitionTotal) + turboFanTotal).toLocaleString()}`);

const top = Object.entries(wallNs)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 25);

console.log('\nTop 25 functions by total wall time (Ignition only):');
console.log(`  ${'ms'.padStart(12)}  ${'calls'.padStart(8)}  ${'osr'.padStart(5)}  name`);
for (const [fn, ns] of top) {
  const ms    = (ns / 1e6).toFixed(2).padStart(12);
  const calls = (callCount[fn] ?? 0).toLocaleString().padStart(8);
  const osr   = (osrCount[fn] ?? 0).toLocaleString().padStart(5);
  console.log(`  ${ms}  ${calls}  ${osr}  ${fn}`);
}
