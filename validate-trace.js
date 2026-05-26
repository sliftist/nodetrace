'use strict';
const fs = require('fs');

const EV_ENTER                = 0x00;
const EV_EXIT                 = 0x01;
const EV_SUSPEND              = 0x02;
const EV_RESUME               = 0x03;
const EV_ON_STACK_REPLACEMENT = 0x04;
const EV_OPTIMIZED_BATCH      = 0x05;
const EV_NEWNAME              = 0x06;

function readTrace(buf) {
  const names = [];
  const events = [];
  let pos = 0, lastTs = 0n;

  const u8  = () => buf[pos++];
  const u16 = () => { const v = buf.readUInt16LE(pos); pos += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const u64 = () => {
    const lo = buf.readUInt32LE(pos), hi = buf.readUInt32LE(pos+4);
    pos += 8; return BigInt(hi) * 0x100000000n + BigInt(lo);
  };
  const readDelta = (ss) => {
    switch (ss) {
      case 0: return BigInt(u8());
      case 1: return BigInt(u16());
      case 2: return BigInt(u32());
      case 3: return u64();
    }
  };

  while (pos < buf.length) {
    const header = u8();
    const ss = (header >> 6) & 3, type = header & 0x3f;
    lastTs += readDelta(ss);
    const ts = lastTs;

    if (type === EV_NEWNAME) {
      const idx = u32(), len = u16();
      names[idx] = buf.toString('utf8', pos, pos + len); pos += len;
    } else if (type === EV_ENTER) {
      const func = names[u32()] ?? '(unknown)', isAsync = u8(), callId = u32();
      events.push({ type: 'ENTER', ts, func, isAsync: !!isAsync, callId });
    } else if (type === EV_RESUME) {
      const func = names[u32()] ?? '(unknown)', callId = u32();
      events.push({ type: 'RESUME', ts, func, callId });
    } else if (type === EV_OPTIMIZED_BATCH) {
      pos += 4;
    } else if (type <= EV_ON_STACK_REPLACEMENT) {
      const func = names[u32()] ?? '(unknown)', callId = u32();
      const name = ['ENTER','EXIT','SUSPEND','RESUME','ON_STACK_REPLACEMENT'][type];
      events.push({ type: name, ts, func, callId });
    } else {
      throw new Error(`Unknown event type 0x${type.toString(16)} at offset ${pos}`);
    }
  }
  return events;
}

function validate(events, logPath) {
  const stack = [];   // [{callId, func}]
  let errors = 0;
  let enters = 0, exits = 0, suspends = 0, resumes = 0, osrs = 0;
  let orphanEnds = 0;      // END with no matching callId anywhere in the stack
  let wrongOrderEnds = 0;  // END where callId is in the stack but not at top
  let maxDepth = 0;
  let maxDepthStack = [];

  const endEvent = (ev) => {
    if (stack.length === 0) {
      orphanEnds++;
      console.error(`ERROR: ${ev.type} callId=${ev.callId} func=${ev.func} on empty stack`);
      errors++;
      return false;
    }
    const top = stack[stack.length - 1];
    if (top.callId !== ev.callId) {
      const idx = stack.findLastIndex(f => f.callId === ev.callId);
      if (idx < 0) {
        orphanEnds++;
        console.error(`ERROR: ${ev.type} callId=${ev.callId} func=${ev.func} not found in stack (depth=${stack.length})`);
      } else {
        wrongOrderEnds++;
        console.error(`ERROR: ${ev.type} callId=${ev.callId} func=${ev.func} not at top — ` +
          `${stack.length - 1 - idx} frame(s) above it (top is callId=${top.callId} func=${top.func})`);
      }
      errors++;
      return false;
    }
    return true;
  };

  const trackDepth = () => {
    if (stack.length > maxDepth) {
      maxDepth = stack.length;
      maxDepthStack = stack.map(f => ({ callId: f.callId, func: f.func }));
    }
  };

  for (const ev of events) {
    if (ev.type === 'ENTER') {
      enters++;
      stack.push({ callId: ev.callId, func: ev.func });
      trackDepth();
    } else if (ev.type === 'EXIT') {
      exits++;
      if (endEvent(ev)) stack.pop();
    } else if (ev.type === 'SUSPEND') {
      suspends++;
      if (endEvent(ev)) stack.pop();
    } else if (ev.type === 'RESUME') {
      resumes++;
      stack.push({ callId: ev.callId, func: ev.func });
      trackDepth();
    } else if (ev.type === 'ON_STACK_REPLACEMENT') {
      osrs++;
      // OSR pops the frame — TurboFan takes over, Ignition won't EXIT it.
      if (endEvent(ev)) stack.pop();
    }
  }

  if (stack.length > 0) {
    console.error(`ERROR: ${stack.length} frame(s) still on stack at end of trace:`);
    for (const f of stack.slice(-5)) {
      console.error(`  callId=${f.callId} func=${f.func}`);
    }
    errors++;
  }

  // Write max-depth report to log file
  const lines = [
    `Max call stack depth: ${maxDepth}`,
    ``,
    `Stack at max depth (bottom → top):`,
    ...maxDepthStack.map((f, i) => `  [${i}] callId=${f.callId}  ${f.func}`),
  ];
  fs.writeFileSync(logPath, lines.join('\n') + '\n');

  return { errors, enters, exits, suspends, resumes, osrs, orphanEnds, wrongOrderEnds };
}

const path = process.argv[2];
if (!path) { console.error('Usage: node validate-trace.js <trace.bin>'); process.exit(1); }

const logPath = path.replace(/\.bin$/, '') + '-stack-depth.log';

const buf = fs.readFileSync(path);
console.log(`Validating ${path} (${(buf.length/1e6).toFixed(1)} MB)...`);

const events = readTrace(buf);
const { errors, enters, exits, suspends, resumes, osrs, orphanEnds, wrongOrderEnds } = validate(events, logPath);

console.log(`  ENTER=${enters.toLocaleString()}  EXIT=${exits.toLocaleString()}  SUSPEND=${suspends.toLocaleString()}  RESUME=${resumes.toLocaleString()}  OSR=${osrs.toLocaleString()}`);
console.log(`  orphan-ends (no start)=${orphanEnds.toLocaleString()}  wrong-order-ends (start buried)=${wrongOrderEnds.toLocaleString()}`);
console.log(`  Max stack depth logged to: ${logPath}`);
if (errors === 0) {
  console.log(`  OK — no ordering errors found`);
} else {
  console.log(`  FAILED — ${errors} error(s) found`);
  process.exit(1);
}
