'use strict';

// ── Async helpers ──────────────────────────────────────────────────────────────

async function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function fetchData(id) {
  await delay(1);
  return { id, value: id * id };
}

async function processItem(item) {
  // Simulate a small async transform
  const partial = item.value * 2 + Math.sqrt(item.value);
  await Promise.resolve();
  return partial + Math.log(item.value + 1);
}

async function loadAndProcess(ids) {
  const items = await Promise.all(ids.map(fetchData));
  const results = [];
  for (const item of items) {
    results.push(await processItem(item));
  }
  return results;
}

// ── Hot inner functions (called in tight loop) ─────────────────────────────────

function multiply(a, b) {
  return a * b;
}

function square(x) {
  return multiply(x, x);
}

function accumulate(acc, val) {
  // Deliberately uses multiple helper calls so the call graph is interesting
  return acc + square(val) - multiply(val - 1, val + 1);
}

function innerBatch(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) {
    acc = accumulate(acc, i);
  }
  return acc;
}

// ── Async work injected into the hot loop ──────────────────────────────────────

async function asyncSubWork(seed) {
  // A quick async step — suspends and resumes once per call
  await Promise.resolve();
  let v = seed;
  for (let i = 0; i < 200; i++) v = accumulate(v, i);
  return v;
}

// ── Hot section: runs for ~100ms ───────────────────────────────────────────────

async function hotLoop() {
  const deadline = Date.now() + 100;
  let iterations = 0;
  let total = 0;

  while (Date.now() < deadline) {
    // Synchronous batch — this is the hot path seen by the profiler
    total += innerBatch(500);
    iterations++;

    // Every 20 batches, do an async sub-call to exercise suspend/resume
    if (iterations % 20 === 0) {
      total += await asyncSubWork(total & 0xffff);
    }
  }

  return { iterations, total };
}

// ── Some extra async work to round out the trace ───────────────────────────────

async function pipeline(depth) {
  if (depth <= 0) return 0;
  await delay(2);
  const child = await pipeline(depth - 1);
  return child + depth;
}

// ── Main ───────────────────────────────────────────────────────────────────────

async function main() {
  console.log('Starting trace test…');
  console.log(`Output: ${process.env.NODE_TRACE_FILE ?? 'node_trace.bin'}`);

  // Async warm-up with real suspension points
  const processed = await loadAndProcess([1, 2, 3, 4, 5]);
  console.log(`Processed ${processed.length} items`);

  // Recursive async pipeline
  const pipelineResult = await pipeline(4);
  console.log(`Pipeline result: ${pipelineResult}`);

  // Hot section
  console.log('Running hot loop for ~100 ms…');
  const { iterations, total } = await hotLoop();
  console.log(`Hot loop: ${iterations} iterations, checksum=${total & 0xffffffff}`);

  // Async cool-down
  await delay(5);
  console.log('Done.');
}

main().catch(err => { console.error(err); process.exit(1); });
