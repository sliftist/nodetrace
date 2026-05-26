#pragma once

namespace nodetrace {

// Called once when tracing starts (from MaybeInitTraceWriter).
void InitTrueTime();

// Returns true after InitTrueTime() has been called.
bool IsTrueTimeEnabled();

// Returns wall-clock milliseconds synchronized to NTP with monotonic guarantee.
double GetTrueTimeMs();

// Returns the current linearly-interpolated NTP offset (ms).
// This is the value that is actually added to CLOCK_REALTIME right now.
// It changes smoothly over 20-minute windows to avoid sudden jumps.
double GetCurrentOffset();

// Returns the most recently fetched NTP target offset (ms).
// Both processes sharing /nodetrace-timesync shm will return the exact same
// double value here, making it suitable for triple-equal cross-process tests.
double GetTargetOffset();

// Blocking: perform a fresh NTP query, write result to shared memory, and
// return the new target offset. For testing only — blocks up to ~9 seconds.
double ForceResync();

}  // namespace nodetrace
