'use strict';
// Decodes the binary trace produced by TraceWriter (deps/v8/src/trace/trace-writer.h).
//
// Usage:  node trace-reader.js [node_trace.bin]

const fs = require('fs');

// ── Wire format constants ──────────────────────────────────────────────────────

const T_UINT8    = 0x01;
const T_UINT16   = 0x02;
const T_UINT32   = 0x03;
const T_UINT64   = 0x04;
const T_STRING   = 0x10;
const T_FUNCNAME = 0x11;  // string + push to name cache
const T_FUNCREF  = 0x12;  // 1-byte back-distance into name cache

const EV_ENTER   = 0x00;
const EV_EXIT    = 0x01;
const EV_SUSPEND = 0x02;
const EV_RESUME  = 0x03;
const EV_OSR     = 0x04;

const EV_NAME = ['ENTER', 'EXIT', 'SUSPEND', 'RESUME', 'OSR'];

// ── Binary reader ──────────────────────────────────────────────────────────────

function readTrace(buf) {
  let pos = 0;
  const nameCache = [];   // ring: index 0 = most-recently seen name
  const events = [];

  const u8  = () => buf[pos++];
  const u16 = () => { const v = buf.readUInt16LE(pos); pos += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const u64 = () => {
    const lo = buf.readUInt32LE(pos);
    const hi = buf.readUInt32LE(pos + 4);
    pos += 8;
    return BigInt(hi) * 0x100000000n + BigInt(lo);
  };
  const str = () => { const len = u16(); const s = buf.toString('utf8', pos, pos + len); pos += len; return s; };

  while (pos < buf.length) {
    const nFields = u8();
    const vals = [];

    for (let i = 0; i < nFields; i++) {
      const tag = u8();
      if      (tag === T_UINT8)    vals.push(u8());
      else if (tag === T_UINT16)   vals.push(u16());
      else if (tag === T_UINT32)   vals.push(u32());
      else if (tag === T_UINT64)   vals.push(u64());
      else if (tag === T_STRING)   vals.push(str());
      else if (tag === T_FUNCNAME) { const n = str(); nameCache.unshift(n); if (nameCache.length > 128) nameCache.pop(); vals.push(n); }
      else if (tag === T_FUNCREF)  { const d = u8(); vals.push(nameCache[d - 1] ?? '(unknown)'); }
      else { throw new Error(`Unknown field tag 0x${tag.toString(16)} at offset ${pos}`); }
    }

    // vals layout:
    //   ENTER:   [evType, ts, funcName, isAsync, callId]
    //   others:  [evType, ts, funcName, callId]
    const [evType, ts, func, ...rest] = vals;
    if (evType === EV_ENTER) {
      events.push({ type: 'ENTER',                   ts, func, isAsync: !!rest[0], callId: rest[1] });
    } else {
      events.push({ type: EV_NAME[evType] ?? `0x${evType.toString(16)}`, ts, func, callId: rest[0] });
    }
  }

  return events;
}

// ── Analysis ───────────────────────────────────────────────────────────────────

function summarize(events) {
  const counts = {};
  const wallNs = {};    // total wall time per function name (ENTER→EXIT)
  const callCount = {}; // how many times each function was entered
  const stack = [];     // { func, ts, callId }

  for (const ev of events) {
    counts[ev.type] = (counts[ev.type] ?? 0) + 1;

    if (ev.type === 'ENTER') {
      stack.push({ func: ev.func, ts: ev.ts, callId: ev.callId });
      callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
    } else if (ev.type === 'EXIT') {
      // Find the matching frame by callId (handles recursion correctly)
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx >= 0) {
        const { func, ts } = stack.splice(idx, 1)[0];
        const dur = Number(ev.ts - ts);
        wallNs[func] = (wallNs[func] ?? 0) + dur;
      }
    }
  }

  return { counts, wallNs, callCount };
}

// ── Main ───────────────────────────────────────────────────────────────────────

const path = process.argv[2] ?? 'node_trace.bin';
if (!fs.existsSync(path)) {
  console.error(`File not found: ${path}`);
  process.exit(1);
}

const buf = fs.readFileSync(path);
console.log(`Read ${buf.length.toLocaleString()} bytes from ${path}`);

const events = readTrace(buf);
console.log(`Decoded ${events.length.toLocaleString()} events\n`);

const { counts, wallNs, callCount } = summarize(events);

console.log('Event type breakdown:');
for (const [type, n] of Object.entries(counts)) {
  console.log(`  ${type.padEnd(10)} ${n.toLocaleString()}`);
}

const top = Object.entries(wallNs)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 25);

console.log('\nTop 25 functions by total wall time:');
console.log(`  ${'ms'.padStart(12)}  ${'calls'.padStart(8)}  name`);
for (const [fn, ns] of top) {
  const ms   = (ns / 1e6).toFixed(2).padStart(12);
  const calls = (callCount[fn] ?? 0).toLocaleString().padStart(8);
  console.log(`  ${ms}  ${calls}  ${fn}`);
}
