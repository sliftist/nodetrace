#pragma once

namespace nodetrace {

// Called once when tracing starts (from MaybeInitTraceWriter).
// Opens/creates POSIX shared memory, sets NODETRACE_TRUE_TIME env var,
// and spawns a background thread that NTP-syncs every 5 minutes.
void InitTrueTime();

// Returns true after InitTrueTime() has been called.
// Used by JSDate::CurrentTimeValue to decide whether to use our clock.
bool IsTrueTimeEnabled();

// Returns wall-clock milliseconds synchronized to NTP, with:
//   - 20-minute linear smear between offset updates (no sudden jumps)
//   - per-thread monotonic guarantee (time never goes backward)
// Falls back to raw system time until the first NTP sync completes.
double GetTrueTimeMs();

}  // namespace nodetrace
