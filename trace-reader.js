'use strict';
const fs = require('fs');
const { readTrace, PARAM_TYPE_NAMES } = require('./trace-parser');

function summarize(events) {
  const counts    = Object.create(null);
  const wallNs    = Object.create(null);
  const callCount = Object.create(null);
  const osrCount  = Object.create(null);
  const start     = new Map();  // callId → { func, ts }
  let turboFanTotal = 0n;
  let metaTotal = 0;
  const roots = new Set();

  for (const ev of events) {
    counts[ev.type] = (counts[ev.type] ?? 0) + 1;

    if (ev.type === 'ENTER' || ev.type === 'RESUME') {
      start.set(ev.callId, { func: ev.func, ts: ev.ts });
      if (ev.type === 'ENTER') {
        callCount[ev.func] = (callCount[ev.func] ?? 0) + 1;
        if (ev.parentId === null) roots.add(ev.callId);
      }
    } else if (ev.type === 'EXIT' || ev.type === 'SUSPEND') {
      const s = start.get(ev.callId);
      if (s) {
        wallNs[s.func] = (wallNs[s.func] ?? 0) + Number(ev.ts - s.ts);
        start.delete(ev.callId);
      }
    } else if (ev.type === 'ON_STACK_REPLACEMENT') {
      osrCount[ev.func] = (osrCount[ev.func] ?? 0) + 1;
      start.delete(ev.callId);
    } else if (ev.type === 'OPTIMIZED_BATCH') {
      turboFanTotal += ev.count;
    } else if (ev.type === 'META') {
      metaTotal++;
    }
  }

  return { counts, wallNs, callCount, osrCount, turboFanTotal, metaTotal, rootCount: roots.size };
}

const path = process.argv[2] ?? 'node_trace.bin';
if (!fs.existsSync(path)) { console.error(`File not found: ${path}`); process.exit(1); }

const buf = fs.readFileSync(path);
console.log(`Read ${buf.length.toLocaleString()} bytes from ${path}`);

const { events, names, header } = readTrace(buf);
if (header) {
  console.log(`Header: version=${header.version} flags=0x${header.flags.toString(16)} hasParams=${header.hasParams}`);
} else {
  console.log(`Header: (none — legacy v1 file)`);
}
console.log(`Decoded ${events.length.toLocaleString()} events\n`);

const {
  counts, wallNs, callCount, osrCount, turboFanTotal, metaTotal, rootCount,
} = summarize(events);

const ignitionTotal = counts['ENTER'] ?? 0;
console.log('Event type breakdown:');
for (const [type, n] of Object.entries(counts))
  console.log(`  ${type.padEnd(25)} ${n.toLocaleString()}`);
console.log(`  ${'Unique names'.padEnd(25)} ${names.filter(Boolean).length.toLocaleString()}`);
console.log(`  ${'META records'.padEnd(25)} ${metaTotal.toLocaleString()}`);
console.log(`  ${'Root calls'.padEnd(25)} ${rootCount.toLocaleString()}`);
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

// Sample META records so we can eyeball the annotation stream.
const metas = events.filter(e => e.type === 'META');
if (metas.length > 0) {
  console.log('\nFirst 10 META records:');
  for (const m of metas.slice(0, 10)) {
    const val = m.tag >= 5 && m.tag !== 5 ? `<${PARAM_TYPE_NAMES[m.tag]}>`
              : m.tag === 5              ? JSON.stringify(m.value)
              : String(m.value);
    console.log(`  holder=${m.holderId ?? '(none)'}  ${m.key} = ${val}`);
  }
}
