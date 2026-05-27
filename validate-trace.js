'use strict';
const fs = require('fs');
const { readTrace } = require('./trace-parser');

function validate(events, logPath) {
  const stack = [];  // [{ callId, func }]
  let errors = 0;
  let enters = 0, exits = 0, suspends = 0, resumes = 0, osrs = 0;
  let orphanEnds = 0, wrongOrderEnds = 0;
  let maxDepth = 0, maxDepthStack = [];

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
      if (endEvent(ev)) stack.pop();
    }
  }

  if (stack.length > 0) {
    console.error(`ERROR: ${stack.length} frame(s) still on stack at end of trace:`);
    for (const f of stack.slice(-5))
      console.error(`  callId=${f.callId} func=${f.func}`);
    errors++;
  }

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
console.log(`Validating ${path} (${(buf.length / 1e6).toFixed(1)} MB)...`);

const { events } = readTrace(buf);
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
