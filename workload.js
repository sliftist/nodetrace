'use strict';

function hash(x)    { return (x ^ (x >>> 16)) * 0x45d9f3b | 0; }
function mix(a, b)  { return (hash(a) + b * 0x9e3779b9) | 0; }
function fold(x)    { return x ^ (x >>> 11) ^ (x >>> 22); }
function rot(x, n)  { return (x << n | x >>> (32 - n)) | 0; }
function sat(x)     { return x < 0 ? 0 : x > 0xffff ? 0xffff : x; }

function kernel(a, b, c) {
  return fold(mix(rot(a, 5), b) ^ hash(c));
}

function reduce(arr) {
  let acc = 0;
  for (let i = 0; i < arr.length; i++) acc = kernel(acc, arr[i], i);
  return acc;
}

function fill(buf, seed) {
  for (let i = 0; i < buf.length; i++) buf[i] = hash(seed + i) & 0xffff;
}

function transform(buf) {
  for (let i = 1; i < buf.length; i++) buf[i] = kernel(buf[i - 1], buf[i], i) & 0xffff;
}

function accumulate(buf) {
  let s = 0;
  for (let i = 0; i < buf.length; i++) s = mix(s, buf[i]) & 0x7fffffff;
  return s;
}

function pipeline(seed, len) {
  const buf = new Int32Array(len);
  fill(buf, seed);
  transform(buf);
  return accumulate(buf);
}

function stageA(x) { return pipeline(x,        64); }
function stageB(x) { return pipeline(x ^ 0xaa, 48); }
function stageC(x) { return pipeline(x * 3,    32); }
function stageD(x) { return pipeline(x + 7,    80); }
function stageE(x) { return pipeline(x - 13,   56); }

function runStages(x) { return stageA(x) ^ stageB(x) ^ stageC(x) ^ stageD(x) ^ stageE(x); }

function combine(a, b)     { return mix(a, b) & 0x7fffffff; }
function compress(a, b, c) { return combine(combine(a, b), c); }

function batchRun(n, seed) {
  let r = seed;
  for (let i = 0; i < n; i++) r = combine(r, runStages(r & 0xfff));
  return r;
}

function summarize(results) {
  let s = 0;
  for (const v of results) s = mix(s, v) & 0x7fffffff;
  return s;
}

function dispatch(op, x) {
  if (op === 0) return stageA(x);
  if (op === 1) return stageB(x);
  if (op === 2) return stageC(x);
  if (op === 3) return stageD(x);
  return stageE(x);
}

function multiDispatch(x, n) {
  const results = [];
  for (let i = 0; i < n; i++) results.push(dispatch(i % 5, x + i));
  return summarize(results);
}

function sweep(lo, hi, step) {
  const out = [];
  for (let x = lo; x < hi; x += step) out.push(runStages(x));
  return out;
}

function mergeSweeps(a, b) {
  const out = [];
  for (let i = 0; i < Math.min(a.length, b.length); i++) out.push(combine(a[i], b[i]));
  return out;
}

function foldSweep(arr) { return summarize(arr); }

function runSweep(seed) {
  const a = sweep(seed,       seed + 20, 1);
  const b = sweep(seed + 100, seed + 120, 1);
  return foldSweep(mergeSweeps(a, b));
}

function probe(x, depth) {
  if (depth === 0) return runStages(x);
  return combine(probe(x ^ 1, depth - 1), probe(x ^ 2, depth - 1));
}

function sampleProbe(seed) {
  let acc = 0;
  for (let i = 0; i < 8; i++) acc = combine(acc, probe(seed + i, 2));
  return acc;
}

async function asyncStage(x) {
  await Promise.resolve();
  return batchRun(20, x);
}

async function asyncPipeline(seeds) {
  let acc = 0;
  for (const s of seeds) acc = combine(acc, await asyncStage(s));
  return acc;
}

async function asyncSweep(seed) {
  await Promise.resolve();
  return runSweep(seed);
}

async function orchestrate(rounds) {
  let total = 0;
  for (let r = 0; r < rounds; r++) {
    const seeds = [r * 7, r * 13, r * 31, r * 37];
    const ap = await asyncPipeline(seeds);
    const as = await asyncSweep(r * 17);
    const sp = sampleProbe(r * 5);
    const md = multiDispatch(r * 11, 5);
    const br = batchRun(50, r);
    total = compress(compress(ap, as), compress(sp ^ md, br));
  }
  return total;
}

async function main() {
  const tracePath = process.env.NODE_TRACE_FILE;
  if (!tracePath) { console.error('Set NODE_TRACE_FILE'); process.exit(1); }

  const ROUNDS = 40;
  console.log(`Running ${ROUNDS} rounds → ${tracePath}`);
  const t0 = Date.now();
  const result = await orchestrate(ROUNDS);
  console.log(`Done in ${Date.now() - t0}ms  checksum=${result >>> 0}`);
}

main().catch(e => { console.error(e); process.exit(1); });
