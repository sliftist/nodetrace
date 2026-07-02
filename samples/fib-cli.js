'use strict';
// Recursive fib + memoized fib.  Shows a deep parent-ref chain, small enough
// to be human-readable in the viewer.  Uses process.traceMeta to annotate the
// input each round.
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

const memo = new Map();
function memoFib(n) {
  if (n < 2) return n;
  if (memo.has(n)) return memo.get(n);
  const v = memoFib(n - 1) + memoFib(n - 2);
  memo.set(n, v);
  return v;
}

function report(label, value, n) {
  process.traceMeta?.('label', label);
  process.traceMeta?.('n', n);
  process.traceMeta?.('value', value);
}

function main() {
  const targets = [10, 15, 20, 22];
  for (const n of targets) {
    process.traceMeta?.('round', n);
    const a = fib(n);
    report('fib', a, n);
    memo.clear();
    const b = memoFib(n);
    report('memoFib', b, n);
  }
  console.log('done');
}

main();
