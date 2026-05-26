# Function-level tracing in nodetrace

This fork instruments Node.js's V8/Ignition interpreter to emit a binary trace
of every JavaScript function call. The trace captures enter, exit, async
suspend, and async resume events with nanosecond timestamps, producing a compact
binary file that can be decoded offline.

## How it works

### Bytecode injection (`deps/v8/src/interpreter/bytecode-generator.cc`)

Every JS function gets a `CallRuntime(TraceEnter)` injected at entry and a
`CallRuntime(TraceExit)` at every `Return` bytecode. Async functions also get
`CallRuntime(TraceAsyncSuspend)` just before each `SuspendGenerator` bytecode
and `CallRuntime(TraceAsyncResume)` just after each `ResumeGenerator` bytecode.
The calls are unconditional — the writer itself is gated on `NODE_TRACE_FILE`,
so there is zero overhead when tracing is disabled.

### Runtime functions (`deps/v8/src/runtime/runtime-test.cc`)

Four new runtime functions handle the events:

| Function | Args | Description |
|---|---|---|
| `Runtime_TraceEnter` | 0 | Records FUNC_ENTER for the current top frame |
| `Runtime_TraceExit` | 1 (return value) | Records FUNC_EXIT, passes return value through unchanged |
| `Runtime_TraceAsyncSuspend` | 1 (generator object) | Records ASYNC_SUSPEND, parks call_id keyed on generator identity hash |
| `Runtime_TraceAsyncResume` | 1 (generator object) | Records ASYNC_RESUME, restores call_id from parked map |

Each function calls `MaybeInitTraceWriter()` which lazily opens the output file
on first call (skipped entirely in `mksnapshot` to avoid bootstrapping issues).

### JIT hook stripping

When Sparkplug, Maglev, or TurboFan compiles a hot function it strips the
`CallRuntime(TraceEnter/Exit)` bytecodes completely — they are replaced with
no-ops. This means **JIT-compiled functions run with zero tracing overhead**.
Their call counts are still tracked via a single `incq` counter at the JIT'd
function prologue (see OPTIMIZED_BATCH).

- TurboFan: `deps/v8/src/compiler/bytecode-graph-builder.cc` (VisitCallRuntime)
- Maglev: `deps/v8/src/maglev/maglev-graph-builder.cc` (VisitCallRuntime)

### JIT prologue counter

A single `incq` instruction is injected at the entry of every Maglev- or
TurboFan-compiled function. This increments `g_turbofan_call_count`. Before
each Ignition trace event, the counter is drained into an `OPTIMIZED_BATCH`
record if non-zero.

- TurboFan: `deps/v8/src/compiler/backend/code-generator.cc`
- Maglev: `deps/v8/src/maglev/maglev-code-generator.cc`

### Compiler flags (`deps/v8/src/flags/flag-definitions.h`)

- `turbo_filter` is `"*"` — V8 naturally JITs hot functions as it normally would.
- Sparkplug and Maglev are enabled. Hot functions run at full speed with zero
  tracing overhead; only Ignition-interpreted functions are traced.

### Trace writer (`deps/v8/src/trace/trace-writer.h`)

`TraceWriter` is a lightweight, header-only class:

- **128 MB malloc'd ring buffer** flushed to disk in chunks.
- **Compact binary wire format** (see below): delta-encoded variable-width
  timestamps, no per-field tags, 128-entry function-name ring cache.
- **Async call tracking**: `call_stack_` pairs each ENTER with a unique
  `call_id`. SUSPEND pops the id into `suspended_calls_` (keyed on generator
  identity hash); RESUME pushes it back so EXIT sees the correct id.
- **Thread safety**: single-threaded by design — one writer per process.

## Output

Tracing is **opt-in**: set `NODE_TRACE_FILE=<path>` to enable it. If the
variable is not set the process runs with zero overhead. Set it to `/dev/null`
to exercise the write path without storing output.

```
NODE_TRACE_FILE=my_trace.bin node my_program.js
node trace-reader.js my_trace.bin
node analyze-trace.js my_trace.bin
```

## Wire format reference

The binary trace is a flat stream of records. Records never cross a flush
boundary. See `deps/v8/src/trace/trace-writer.h` for the authoritative spec.

### Record layout

```
[uint8  header]   bits 7-6: ss (timestamp size selector)
                  bits 5-0: type (event type, 0x00–0x05)
[delta]           timestamp delta from previous record, LE
                  ss=0 → 1 byte, ss=1 → 2 bytes, ss=2 → 4 bytes, ss=3 → 8 bytes
[fields]          fixed layout per type — no per-field tags
```

The absolute timestamp is the running sum of all deltas since t=0.
Consecutive events typically differ by < 255 ns, so the delta fits in 1 byte.

### Event types

| Value | Name           | Fields after header+delta                        |
|-------|----------------|--------------------------------------------------|
| 0x06  | NEW_NAME       | len(u16-LE), utf-8 bytes                         |
| 0x00  | FUNC_ENTER     | cache_ref(u8), is_async(u8), call_id(u32-LE)     |
| 0x01  | FUNC_EXIT      | cache_ref(u8), call_id(u32-LE)                   |
| 0x02  | ASYNC_SUSPEND  | cache_ref(u8), call_id(u32-LE)                   |
| 0x03  | ASYNC_RESUME   | cache_ref(u8), call_id(u32-LE)                   |
| 0x04  | FUNC_ON_STACK_REPLACEMENT | cache_ref(u8), call_id(u32-LE)          |
| 0x05  | OPTIMIZED_BATCH | count(u32-LE)                                    |

NEW_NAME pushes the name string onto the 128-entry ring cache.
It is always emitted immediately before the first call event that uses that
name, sharing the same timestamp (the following call event has delta=0).

`cache_ref` is always 1–128 (1 = most-recently-pushed name).

`call_id` is a process-global uint32 counter incremented on every ENTER.
SUSPEND/RESUME/EXIT carry the same `call_id` as their matching ENTER.

For OPTIMIZED_BATCH: `ts` in the header is the end of the JIT window;
`ts_start` is implicitly the previous record's timestamp.

### Approximate record sizes (delta fits in 1 byte — typical)

| Event                   | Size   |
|-------------------------|--------|
| NEW_NAME (len N)        | 4+N B  |
| ENTER                   | 8 B    |
| EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT | 7 B |
| OPTIMIZED_BATCH          | 6 B    |

### Decoding

```
node trace-reader.js [path/to/trace.bin]   # full event dump + wall-time table
node analyze-trace.js [path/to/trace.bin]  # own-time analysis, top 3 functions
```

## Runtime declarations (`deps/v8/src/runtime/runtime.h`)

```c
F(TraceEnter, 0, 1)
F(TraceExit, 1, 1)
F(TraceAsyncSuspend, 1, 1)
F(TraceAsyncResume, 1, 1)
```
