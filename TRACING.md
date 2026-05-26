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
| `Runtime_TraceAsyncSuspend` | 1 (generator object) | Records ASYNC_SUSPEND |
| `Runtime_TraceAsyncResume` | 1 (generator object) | Records ASYNC_RESUME |

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

### Throttling (`INSPECT_MAX_PER_SECOND`)

Events are staged in a per-100ms window then filtered:

1. **Histogram pass** — exponential-bucket histogram of call durations to find
   a `min_duration` threshold that keeps ≈ `max_per_second × elapsed_seconds`
   calls.
2. **Bitmap pass** — mark which `call_id`s survive the threshold.
3. **Emit pass** — kept events are written out; excluded calls are counted and
   folded into an OPTIMIZED_BATCH record immediately before the next kept event.

Default: `INSPECT_MAX_PER_SECOND=100000`. Override with the env var.

### True time (`deps/v8/src/trace/truetime.cc`)

When tracing is active, `Date.now()` is replaced with an NTP-synchronized clock
that is consistent across all co-tracing processes on the same machine. Offsets
are stored in POSIX shared memory (`/dev/shm/nodetrace-timesync`) so all
processes read the same corrected time without re-querying NTP. A background
thread re-syncs every 5 minutes. The JS shim (`trueTimeShim.ts`) is suppressed
when the C++ layer is active (`globalThis.TRUE_TIME_ALREADY_SHIMMED = true`).

Exposed on `process`:
- `process.trueTimeOffset()` — current linearly-smeared NTP correction (ms)
- `process.trueTimeTargetOffset()` — raw NTP target; bit-identical across
  processes reading from shared memory (suitable for triple-equal tests)
- `process.trueTimeForceResync()` — blocking NTP re-query; for testing only

## Output

Tracing is **opt-in**: set `NODE_TRACE_FILE=<path>` to enable it. If the
variable is not set the process runs with zero overhead. Set it to `/dev/null`
to exercise the write path without storing output.

```
NODE_TRACE_FILE=my_trace.bin node my_program.js
node trace-reader.js my_trace.bin
```

Control throttling:
```
INSPECT_MAX_PER_SECOND=10000 NODE_TRACE_FILE=my_trace.bin node my_program.js
```

## Wire format reference

The binary trace is a flat stream of records. Records never cross a flush
boundary. See `deps/v8/src/trace/trace-writer.h` for the authoritative spec.

All timestamps are **nanoseconds since Unix epoch** (same epoch as `Date.now()`).
Divide by 1,000,000 to get milliseconds.

### Record layout

```
[uint8  header]   bits 7-6: ss (timestamp size selector)
                  bits 5-0: type (event type, 0x00–0x06)
[delta]           timestamp delta from previous record, LE
                  ss=0 → 1 byte, ss=1 → 2 bytes, ss=2 → 4 bytes, ss=3 → 8 bytes
[fields]          fixed layout per type — no per-field tags
```

The absolute timestamp is the running sum of all deltas since t=0.
Consecutive events typically differ by < 255 ns, so the delta fits in 1 byte.

### Event types

| Value | Name | Fields after header+delta |
|-------|------|---------------------------|
| 0x06 | NEW_NAME | name_idx(u32-LE), len(u16-LE), utf-8 bytes |
| 0x00 | FUNC_ENTER | name_idx(u32-LE), is_async(u8), call_id(u32-LE) |
| 0x01 | FUNC_EXIT | name_idx(u32-LE), call_id(u32-LE) |
| 0x02 | ASYNC_SUSPEND | name_idx(u32-LE), call_id(u32-LE) |
| 0x03 | ASYNC_RESUME | name_idx(u32-LE), call_id(u32-LE) |
| 0x04 | FUNC_ON_STACK_REPLACEMENT | name_idx(u32-LE), call_id(u32-LE) |
| 0x05 | OPTIMIZED_BATCH | count(u32-LE), min_ts(u64-LE), max_ts(u64-LE) |

**NEW_NAME** assigns a permanent string index. Always emitted immediately before
the first event that references that `name_idx`; the following event has delta=0.
`name_idx` values are assigned in order of first appearance (0, 1, 2, …).

**FUNC_ENTER / FUNC_EXIT** bracket a single synchronous call. `call_id` is a
process-global uint32 counter incremented on every ENTER and RESUME.

**ASYNC_SUSPEND / ASYNC_RESUME** bracket individual async slices. Each
synchronous slice is its own ENTER…EXIT (or ENTER…SUSPEND / RESUME…EXIT) pair.
`call_id` is fresh for each resume.

**FUNC_ON_STACK_REPLACEMENT** closes the Ignition frame when TurboFan takes
over mid-execution (OSR). `call_id` matches the ENTER that opened the frame.

**OPTIMIZED_BATCH** accounts for calls not individually recorded:
- JIT-compiled calls (TurboFan/Maglev/Sparkplug stripped the trace hooks)
- Calls dropped by the `INSPECT_MAX_PER_SECOND` throttle

`count` is the number of dropped/batched calls.
`min_ts` is the nanosecond timestamp of the start of the batch window (= the
timestamp of the last Ignition event before the batch, or the earliest dropped
ENTER timestamp for throttled drops).
`max_ts` is the nanosecond timestamp of the end of the batch window (= the
timestamp when the JIT counter was drained, or the latest dropped EXIT timestamp
for throttled drops).

### Approximate record sizes (delta fits in 1 byte — typical)

| Event | Size |
|-------|------|
| NEW_NAME (len N) | 8+N bytes |
| ENTER | 11 bytes |
| EXIT / SUSPEND / RESUME / ON_STACK_REPLACEMENT | 10 bytes |
| OPTIMIZED_BATCH | 22 bytes |

### Decoding

```
node trace-reader.js [path/to/trace.bin]   # event summary + wall-time table
```

## Runtime declarations (`deps/v8/src/runtime/runtime.h`)

```c
F(TraceEnter, 0, 1)
F(TraceExit, 1, 1)
F(TraceAsyncSuspend, 1, 1)
F(TraceAsyncResume, 1, 1)
```
