# Function-Level Tracing for Node.js

This fork of Node.js v22.x adds deterministic, always-on binary function
tracing baked directly into the V8 Ignition interpreter. Every JavaScript
function call emits timestamped ENTER/EXIT events; async functions also emit
SUSPEND/RESUME events. The result is a compact binary file you can decode
offline.

## Quick start

```bash
# Run any script — trace is written to node_trace.bin by default
node script.js

# Decode it
NODE_TRACE_FILE=/dev/null node trace-reader.js node_trace.bin

# Suppress tracing entirely
NODE_TRACE_FILE=/dev/null node script.js

# Write to a custom path
NODE_TRACE_FILE=/tmp/my.bin node script.js
```

`NODE_TRACE_FILE=/dev/null` when running the reader prevents the reader
process (which is itself a traced node) from truncating the file it is trying
to read.

---

## What is recorded

| Event | When |
|-------|------|
| `ENTER` | A JS function begins executing in Ignition |
| `EXIT`  | A JS function returns (from Ignition **or** TurboFan) |
| `SUSP`  | An async function hits an `await` |
| `RESM`  | An async function resumes after `await` |
| `OSR`   | Informational: Ignition handed a mid-loop call off to TurboFan |

Every event carries:
- **Monotonic nanosecond timestamp** (`CLOCK_MONOTONIC`)
- **Function name** (deduplicated via a 128-entry ring cache; repeated calls
  use a 2-byte back-reference instead of re-emitting the name)
- **call_id** — a process-global 32-bit counter incremented on every ENTER.
  SUSP/RESM/EXIT carry the same call_id as their matching ENTER, so recursive
  calls and interleaved async continuations are unambiguous.
- **is_async** flag on ENTER events only

### Async correctness

The generator object's **identity hash** (a stable Smi stored inside the
object) is used as the key for parking/restoring call_ids across SUSP/RESM
pairs. Raw pointers are not used because the garbage collector can move heap
objects between a SUSPEND and its matching RESUME.

### OSR events

When TurboFan compiles a hot loop via On-Stack Replacement (OSR), the switch
from Ignition to TurboFan is caught in
`Runtime_LogOrTraceOptimizedOSREntry`. The ENTER was already emitted by
Ignition; the EXIT is still emitted by TurboFan (which compiles the
`TraceExit` bytecode at the function's `Return`). The OSR event is purely
informational — it marks the nanosecond when the JIT took over.

---

## JIT tiers and visibility

| Tier | Visibility |
|------|-----------|
| Ignition (interpreter) | Fully visible — ENTER + EXIT always paired |
| Sparkplug (baseline JIT) | **Disabled** in this fork |
| Maglev (mid-tier JIT) | **Disabled** in this fork |
| TurboFan (optimizing JIT) | Visible — TurboFan compiles `TraceEnter`/`TraceExit` bytecodes |

Because Sparkplug and Maglev are disabled, every function runs in Ignition
unless it matches the TurboFan filter (see below). TurboFan-compiled
functions are also fully tracked: `TraceEnter` and `TraceExit` are real
bytecodes that TurboFan compiles into direct runtime calls.

**There are no orphaned ENTERs.** A function is either fully visible
(ENTER+EXIT) or — in the rare pre-compiled TurboFan case — also visible
(TurboFan includes both hooks).

---

## Flags changed from upstream Node.js defaults

These defaults are baked into `deps/v8/src/flags/flag-definitions.h` and
apply whenever this binary runs, with no command-line flags needed.

| Flag | Upstream default | This fork | Reason |
|------|-----------------|-----------|--------|
| `--sparkplug` | `true` | `false` | Prevents a tracing gap between Ignition and TurboFan |
| `--maglev` | `true` (on supported platforms) | `false` | Same reason; also avoids a separate mid-tier with no trace hooks |
| `--turbo-filter` | `"*"` (all functions) | `"heavyCompute"` | Only explicitly named compute-heavy functions get TurboFan; everything else stays in Ignition |

You can override any of these at runtime:

```bash
# Re-enable all JIT tiers (tracing still works but OSR events become more common)
node --sparkplug --maglev --turbo-filter="*" script.js

# Restrict TurboFan to a different function name pattern
node --turbo-filter="myHotFn" script.js

# Disable TurboFan entirely (pure Ignition, maximum tracing completeness)
node --no-opt script.js
```

---

## Performance

### Tracing overhead

| Operation | Cost |
|-----------|------|
| ENTER/EXIT per call (name cached) | ~21–19 bytes written; ~1 `clock_gettime` + memcpy |
| ENTER/EXIT per call (new name, length N) | ~22+N bytes written |
| SUSP/RESM per await | same as ENTER/EXIT; plus one `unordered_map` lookup/insert |
| Buffer flush (128 MB full) | one `write()` syscall; amortized cost is negligible |

The writer uses a 128 MB in-process ring buffer. Flushes happen only when
that buffer is full, so in most scripts there is at most one flush at process
exit via the `atexit` handler.

The `clock_gettime(CLOCK_MONOTONIC)` call on Linux with vDSO is ~20–30 ns.
For very tight loops this adds up. For typical application code (I/O-bound,
async-heavy) the overhead is imperceptible.

### Why Sparkplug and Maglev are disabled

Sparkplug and Maglev do not include the `TraceEnter`/`TraceExit` bytecode
calls that Ignition and TurboFan do. If they were enabled, functions that
tier up to Sparkplug or Maglev would silently lose their ENTER events,
producing orphaned or missing records. Disabling them keeps every function
either in Ignition (fully traced) or in TurboFan (also fully traced).

The performance cost of disabling Sparkplug/Maglev is significant for
CPU-bound workloads but irrelevant for I/O-bound Node.js servers. For
compute-heavy work, name the function `heavyCompute` (or adjust
`--turbo-filter`) and it will be compiled by TurboFan.

### Suppressing tracing

```bash
NODE_TRACE_FILE=/dev/null node script.js
```

With `/dev/null` as the output, `open()` succeeds and the `TraceWriter` is
initialized, but all writes are discarded by the OS. The runtime call
overhead (two `clock_gettime` calls per function call) still occurs.

To truly zero out all overhead, rebuild with the `CallRuntime(kTraceEnter)`
and `CallRuntime(kTraceExit)` lines removed from
`deps/v8/src/interpreter/bytecode-generator.cc`.

---

## Binary wire format

Flat stream of records. Records never cross a flush boundary.

```
RECORD:
  [uint8]   n_fields
  [field]*  n_fields entries

FIELD:
  [uint8]   type_tag
  [...]     data (size depends on type_tag)

TYPE TAGS:
  0x01  UINT8     1 byte value
  0x02  UINT16    2 bytes LE
  0x03  UINT32    4 bytes LE
  0x04  UINT64    8 bytes LE
  0x10  STRING    uint16-LE length + <length> bytes UTF-8
  0x11  FUNC_NAME uint16-LE length + <length> bytes UTF-8
                  Reader pushes this name onto its cache ring head.
  0x12  FUNC_REF  uint8 distance (1 = most-recently-pushed FUNC_NAME, ...)

EVENT TYPES (first UINT8 field):
  0x00  FUNC_ENTER    fields: event, ts, func, is_async(u8), call_id(u32)
  0x01  FUNC_EXIT     fields: event, ts, func, call_id(u32)
  0x02  ASYNC_SUSPEND fields: event, ts, func, call_id(u32)
  0x03  ASYNC_RESUME  fields: event, ts, func, call_id(u32)
  0x04  FUNC_OSR      fields: event, ts, func, call_id(u32)  [informational]

Typical record sizes:
  ENTER (cached name):   21 bytes
  ENTER (new name, N):   22+N bytes
  EXIT/SUSP/RESM (cached): 19 bytes
```

---

## Files changed from upstream

| File | Change |
|------|--------|
| `deps/v8/src/trace/trace-writer.h` | New: header-only `TraceWriter` class |
| `deps/v8/src/trace/trace-writer.cc` | New: minimal companion (included in build) |
| `deps/v8/src/interpreter/bytecode-generator.cc` | Insert `CallRuntime(kTraceEnter)` at function entry; `CallRuntime(kTraceExit)` at every `Return`; `CallRuntime(kTraceAsyncSuspend/Resume)` around every `await` |
| `deps/v8/src/runtime/runtime-test.cc` | Implement `Runtime_TraceEnter/Exit/AsyncSuspend/AsyncResume`; `MaybeInitTraceWriter()`; `GetGlobalTraceWriter()` |
| `deps/v8/src/runtime/runtime-compiler.cc` | Hook `Runtime_LogOrTraceOptimizedOSREntry` to emit `FUNC_OSR` events |
| `deps/v8/src/runtime/runtime.h` | Declare the four new `F(Trace*)` runtime functions |
| `deps/v8/src/flags/flag-definitions.h` | Default `sparkplug=false`, `maglev=false`, `turbo_filter="heavyCompute"` |

---

## Decoding traces

`trace-reader.js` in the repository root decodes the binary format:

```bash
# Full event log
NODE_TRACE_FILE=/dev/null node trace-reader.js node_trace.bin

# First 500 events only
NODE_TRACE_FILE=/dev/null node trace-reader.js node_trace.bin --limit 500

# Statistics only (no per-event output)
NODE_TRACE_FILE=/dev/null node trace-reader.js node_trace.bin --stats
```

Output columns: `EVENT   timestamp_ms   function_name [async] #call_id`
