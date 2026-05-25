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

Tracing is **opt-in**: set `NODE_TRACE_FILE=<path>` to enable it.  If the
variable is not set the process runs with zero overhead (no file is opened, no
trace hooks fire).  Set it to `/dev/null` if you want to exercise the write
path without storing output.

Decode with `trace-reader.js` (in the repo root).

## Design: tracing overhead and JIT interaction

- All trace hooks are injected as `CallRuntime(TraceEnter/Exit)` bytecodes by
  `bytecode-generator.cc`.  Every function call in Ignition fires these hooks.
- TurboFan **preserves** those `CallRuntime` bytecodes when it compiles a
  function, so `TraceEnter`/`TraceExit` still fire from JIT-compiled code.
  This means **every call is captured in the trace**, whether Ignition or JIT'd.
  Calls are never silently dropped.
- JIT'd functions are still faster: TurboFan compiles the function body to
  native code, so the total time per call drops significantly even though the
  tracing overhead (a runtime call per entry/exit) is similar in absolute ns.
  Empirically, simple functions see ~7× speedup under JIT with tracing active.
- Maglev and Sparkplug (baseline JIT) are disabled to avoid a middle tier
  that bypasses the injected `CallRuntime` calls.

### TURBOFAN_BATCH events

In addition to the standard ENTER/EXIT trace, a single `incq` instruction is
injected at the entry of every TurboFan-compiled function (in `code-generator.cc`).
This increments a global counter `g_turbofan_call_count`.  Before each Ignition
trace event (ENTER/EXIT/SUSPEND/RESUME), the counter is drained and emitted as
a `TURBOFAN_BATCH` event if non-zero.

This lets you see **which calls were JIT'd** in the trace stream without adding
per-call overhead to the main trace path:

- `TURBOFAN_BATCH(N)` immediately before `ENTER(f)` means the immediately
  preceding N function calls (since the last traced event) went through
  TurboFan code.  Because TurboFan also fires TraceEnter, those N calls will
  also produce ENTER events; the batch count confirms they were JIT'd.

## Wire format reference

The binary trace is a flat stream of records.  See
`deps/v8/src/trace/trace-writer.h` for the authoritative spec; this is a
summary for readers.

### Record layout

```
[uint8  n_fields]
[field] × n_fields
```

### Field tags

| Tag  | Name       | Payload               |
|------|------------|-----------------------|
| 0x01 | UINT8      | 1-byte value          |
| 0x02 | UINT16     | 2-byte LE             |
| 0x03 | UINT32     | 4-byte LE             |
| 0x04 | UINT64     | 8-byte LE             |
| 0x10 | STRING     | uint16-LE len + bytes |
| 0x11 | FUNC_NAME  | same as STRING; reader pushes name onto a 128-entry ring cache |
| 0x12 | FUNC_REF   | 1-byte distance (1 = most-recently-pushed name) |

### Event types (first field of every record, UINT8)

| Value | Name           | Fields                                     |
|-------|----------------|--------------------------------------------|
| 0x00  | FUNC_ENTER     | evType, ts(u64), func, is_async(u8), call_id(u32) |
| 0x01  | FUNC_EXIT      | evType, ts(u64), func, call_id(u32)        |
| 0x02  | ASYNC_SUSPEND  | evType, ts(u64), func, call_id(u32)        |
| 0x03  | ASYNC_RESUME   | evType, ts(u64), func, call_id(u32)        |
| 0x04  | FUNC_OSR       | evType, ts(u64), func, call_id(u32)        |
| 0x05  | TURBOFAN_BATCH | evType, ts_start(u64), ts_end(u64), count(u64) |

`call_id` is a process-global uint32 counter incremented on every ENTER.
SUSPEND/RESUME/EXIT carry the same `call_id` as their matching ENTER.

### Decoding (`trace-reader.js`)

```
node trace-reader.js [path/to/trace.bin]
```

Prints: event-type breakdown, total Ignition vs TurboFan calls, and the top 25
functions by total wall time with call counts and OSR handoff counts.

## Runtime declarations (`deps/v8/src/runtime/runtime.h`)

```c
F(TraceEnter, 0, 1)
F(TraceExit, 1, 1)
F(TraceAsyncSuspend, 1, 1)
F(TraceAsyncResume, 1, 1)
```
