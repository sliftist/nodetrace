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

### TurboFan OSR (`deps/v8/src/runtime/runtime-compiler.cc`)

When Ignition hands a hot function off to TurboFan mid-loop (OSR), the normal
TraceExit will still fire from TurboFan's Return. To mark the handoff point,
`Runtime_LogOrTraceOptimizedOSREntry` is hooked to emit a `FUNC_OSR` event.
The `--node-trace-funcs` flag sets `v8_flags.log_or_trace_osr` at init time so
this builtin is called.

### Compiler flags (`deps/v8/src/flags/flag-definitions.h`)

- Maglev and Sparkplug (baseline JIT) are disabled by default — they bypass
  Ignition and would miss trace calls.
- `turbo_filter` defaults to `"heavyCompute"` so TurboFan only kicks in for
  functions you explicitly opt in to, keeping most code in Ignition where the
  trace hooks live.

### Trace writer (`deps/v8/src/trace/trace-writer.h`, `trace-writer.cc`)

`TraceWriter` is a lightweight, header-only class:

- **128 MB malloc'd ring buffer** flushed to disk in chunks; physical pages are
  only committed as written.
- **Binary wire format**: each record is `[uint8 n_fields][field*]`. Fields are
  typed (UINT8, UINT64, STRING, FUNC_NAME, FUNC_REF). See the header for the
  full format spec.
- **Function-name deduplication**: a 128-entry ring cache maps SFI pointer →
  cache slot. Repeated calls to the same function emit a 2-byte `FUNC_REF`
  instead of the full name string.
- **Async call tracking**: `call_stack_` (a `std::vector<uint32_t>`) pairs each
  ENTER with a unique `call_id`. SUSPEND pops the id into `suspended_calls_`
  (keyed on generator identity hash); RESUME pushes it back so EXIT sees the
  correct id.
- **Thread safety**: single-threaded by design — one writer per process,
  accessed only from the V8 main thread via `g_trace_writer`.

## Output

The trace is written to `node_trace.bin` by default. Set `NODE_TRACE_FILE` to
override the path, or set it to `/dev/null` to suppress tracing entirely.

Decode with `trace-reader.js` (in the repo root).

## Design objective: zero overhead on hot code

The tracing system is designed so that **hot code is never slowed down**.

- All trace hooks live in Ignition (the bytecode interpreter). There is a small
  overhead per function call, but only while the function runs in Ignition.
- Once V8 decides a function is hot enough to JIT-compile with TurboFan, it
  exits Ignition entirely. The TurboFan-compiled machine code has no trace
  hooks, so those function calls are **not captured in the trace at all** — but
  they also pay zero tracing overhead.
- Maglev and Sparkplug (baseline JIT) are disabled so they don't create a
  middle tier that also lacks hooks.

The practical consequence: if you don't see a function in the trace, it may
simply have been JIT-compiled. Absence from the trace ≠ not called. Use the
OSR events (type `OSR` in the reader output) as a signal that TurboFan took
over for a particular call frame — after an OSR event, the inner calls of that
frame are no longer visible.

## Runtime declarations (`deps/v8/src/runtime/runtime.h`)

```c
F(TraceEnter, 0, 1)
F(TraceExit, 1, 1)
F(TraceAsyncSuspend, 1, 1)
F(TraceAsyncResume, 1, 1)
```
