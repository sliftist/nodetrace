#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// ── Binary trace format ───────────────────────────────────────────────────────
//
// Flat stream of records, back-to-back.  Records never cross a flush boundary.
//
// RECORD:
//   [uint8]   n_fields
//   [field]*  n_fields entries
//
// FIELD:
//   [uint8]   type_tag
//   [...]     data (size depends on type_tag)
//
// TYPE TAGS:
//   0x01  UINT8     1 byte value
//   0x02  UINT16    2 bytes LE
//   0x03  UINT32    4 bytes LE
//   0x04  UINT64    8 bytes LE
//   0x10  STRING    uint16-LE length + <length> bytes UTF-8
//   0x11  FUNC_NAME uint16-LE length + <length> bytes UTF-8
//                   Reader pushes this name onto its cache ring head.
//   0x12  FUNC_REF  uint8 distance (1 = most-recently-pushed FUNC_NAME, ...)
//
// EVENT TYPES (UINT8 field value):
//   0x00  FUNC_ENTER    — initial call.   fields: event, ts, func, is_async, call_id
//   0x01  FUNC_EXIT     — final return.   fields: event, ts, func, call_id
//   0x02  ASYNC_SUSPEND — hit an await.   fields: event, ts, func, call_id
//   0x03  ASYNC_RESUME  — resumed.        fields: event, ts, func, call_id
//   0x04  FUNC_OSR      — JIT handoff (informational; EXIT still fires).
//                         fields: event, ts, func, call_id
//   0x05  TURBOFAN_BATCH — N TurboFan-compiled calls since the last Ignition
//                         event.  fields: event, ts_start(UINT64), ts_end(UINT64),
//                         count(UINT64).  ts_start = timestamp of the last
//                         Ignition event; ts_end = now.  Emitted just before
//                         the next ENTER/EXIT/SUSPEND/RESUME.
//
// call_id is a process-global UINT32 counter incremented on every ENTER.
// SUSPEND/RESUME/EXIT carry the same call_id as their matching ENTER, so
// recursive calls and interleaved async continuations are unambiguous.
//
//   ENTER  (cached):  1 + 2 + 9 + 2 + 2 + 5 = 21 bytes
//   ENTER  (new, N):  1 + 2 + 9 + 3+N + 2 + 5 = 22+N bytes
//   EXIT/SUSPEND/RESUME (cached): 1 + 2 + 9 + 2 + 5 = 19 bytes

namespace v8 {
namespace internal {

static constexpr size_t kTraceChunkSize = 128ULL * 1024 * 1024;
static constexpr size_t kMaxTraceRecord = 512;
static constexpr int    kTraceCacheSize = 128;

enum TraceFieldType : uint8_t {
  kTraceUInt8    = 0x01,
  kTraceUInt16   = 0x02,
  kTraceUInt32   = 0x03,
  kTraceUInt64   = 0x04,
  kTraceString   = 0x10,
  kTraceFuncName = 0x11,
  kTraceFuncRef  = 0x12,
};

enum TraceEventType : uint8_t {
  kTraceFuncEnter    = 0x00,
  kTraceFuncExit     = 0x01,
  kTraceAsyncSuspend = 0x02,
  kTraceAsyncResume  = 0x03,
  kTraceFuncOSR      = 0x04,  // Ignition handed off to TurboFan mid-execution
  kTraceTurboFanBatch = 0x05, // N TurboFan calls since the last Ignition event
};

class TraceWriter {
 public:
  TraceWriter()
      : buf_(nullptr), ptr_(nullptr), buf_end_(nullptr), fd_(-1),
        next_call_id_(0), cache_head_(0), cache_count_(0),
        last_event_ts_(0) {
    memset(cache_, 0, sizeof(cache_));
  }

  ~TraceWriter() {
    Flush();
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (buf_)     { free(buf_); buf_ = nullptr; }
  }

  bool Initialize(const char* output_path) {
    fd_ = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) { perror("TraceWriter: open"); return false; }

    void* mem = malloc(kTraceChunkSize);
    if (!mem) {
      perror("TraceWriter: malloc"); close(fd_); fd_ = -1; return false;
    }
    buf_     = static_cast<uint8_t*>(mem);
    ptr_     = buf_;
    buf_end_ = buf_ + kTraceChunkSize;
    return true;
  }

  __attribute__((always_inline)) inline void WriteFuncEnter(
      const void* sfi_key, const char* name, int name_len, bool is_async) {
    EnsureSpace();
    uint32_t call_id = next_call_id_++;
    call_stack_.push_back(call_id);
    uint64_t ts   = NowNs();
    int      dist = CacheLookup(sfi_key);
    W1(5);
    FieldU8((uint8_t)kTraceFuncEnter);
    FieldU64(ts);
    if (dist > 0) { FieldFuncRef((uint8_t)dist); }
    else          { CacheInsert(sfi_key); FieldFuncName(name, name_len); }
    FieldU8(is_async ? 1 : 0);
    FieldU32(call_id);
    last_event_ts_ = ts;
  }

  __attribute__((always_inline)) inline void WriteFuncExit(
      const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    if (!call_stack_.empty()) call_stack_.pop_back();
    EmitFuncEvent(kTraceFuncExit, sfi_key, name, name_len, call_id);
  }

  // SUSPEND: pop the call_id so nested suspended children don't pollute the
  // parent's id, and park it in suspended_calls_ keyed on the identity hash.
  // noinline: always_inline + -O3 was causing pop_back() to be elided.
  __attribute__((noinline)) void WriteAsyncSuspend(
      uintptr_t gen_key, const void* sfi_key,
      const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    if (!call_stack_.empty()) call_stack_.pop_back();
    suspended_calls_[gen_key] = call_id;
    EmitFuncEvent(kTraceAsyncSuspend, sfi_key, name, name_len, call_id);
  }

  // OSR: Ignition is handing this call off to TurboFan mid-loop.
  // TurboFan WILL still fire TraceExit at the function's Return bytecode, so
  // the ENTER+EXIT pair is intact — do NOT pop the call_stack here.
  // This event is purely informational: marks where the JIT handoff occurred.
  __attribute__((noinline)) void WriteOSR(
      const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    EmitFuncEvent(kTraceFuncOSR, sfi_key, name, name_len, call_id);
  }

  // TURBOFAN_BATCH: drain the per-process TurboFan call counter.
  // Emits a time window [last_event_ts_, now] covering the JIT'd calls.
  void WriteTurboFanBatch(uint64_t count) {
    EnsureSpace();
    uint64_t ts_end = NowNs();
    W1(4);  // 4 fields: evType, ts_start, ts_end, count
    FieldU8((uint8_t)kTraceTurboFanBatch);
    FieldU64(last_event_ts_);
    FieldU64(ts_end);
    FieldU64(count);
    last_event_ts_ = ts_end;
  }

  // RESUME: look up the parked call_id and push it back so EXIT can pop it.
  // noinline: same reason as WriteAsyncSuspend above.
  __attribute__((noinline)) void WriteAsyncResume(
      uintptr_t gen_key, const void* sfi_key,
      const char* name, int name_len) {
    auto it = suspended_calls_.find(gen_key);
    uint32_t call_id = 0;
    if (it != suspended_calls_.end()) {
      call_id = it->second;
      call_stack_.push_back(call_id);
      suspended_calls_.erase(it);
    }
    EmitFuncEvent(kTraceAsyncResume, sfi_key, name, name_len, call_id);
  }

  void Flush() { FlushBuffer(); }

  // noinline so the compiler keeps ptr_ in a register in the hot path.
  __attribute__((noinline)) void FlushBuffer() {
    if (!buf_ || fd_ < 0) return;
    size_t used = static_cast<size_t>(ptr_ - buf_);
    if (used == 0) return;
    uint8_t* p = buf_;
    size_t rem = used;
    while (rem > 0) {
      ssize_t n = write(fd_, p, rem);
      if (n <= 0) { perror("TraceWriter: write"); break; }
      p += n; rem -= static_cast<size_t>(n);
    }
    ptr_ = buf_;
  }

 private:
  uint8_t* buf_;
  uint8_t* ptr_;
  uint8_t* buf_end_;
  int      fd_;

  __attribute__((always_inline)) inline void EnsureSpace() {
    if (__builtin_expect(ptr_ + kMaxTraceRecord > buf_end_, 0))
      FlushBuffer();
  }

  // ── Primitive stores ────────────────────────────────────────────────────────
  __attribute__((always_inline)) inline void W1(uint8_t v)   { *ptr_++ = v; }

  __attribute__((always_inline)) inline void W2(uint16_t v) {
    ptr_[0] = (uint8_t)(v); ptr_[1] = (uint8_t)(v >> 8); ptr_ += 2;
  }

  __attribute__((always_inline)) inline void W8(uint64_t v) {
    ptr_[0] = (uint8_t)(v);       ptr_[1] = (uint8_t)(v >>  8);
    ptr_[2] = (uint8_t)(v >> 16); ptr_[3] = (uint8_t)(v >> 24);
    ptr_[4] = (uint8_t)(v >> 32); ptr_[5] = (uint8_t)(v >> 40);
    ptr_[6] = (uint8_t)(v >> 48); ptr_[7] = (uint8_t)(v >> 56);
    ptr_ += 8;
  }

  __attribute__((always_inline)) inline void WBytes(const void* src, int n) {
    memcpy(ptr_, src, static_cast<size_t>(n)); ptr_ += n;
  }

  // ── Field emitters ──────────────────────────────────────────────────────────
  __attribute__((always_inline)) inline void FieldU8(uint8_t v)
    { W1(kTraceUInt8); W1(v); }

  __attribute__((always_inline)) inline void FieldU64(uint64_t v)
    { W1(kTraceUInt64); W8(v); }

  __attribute__((always_inline)) inline void FieldU32(uint32_t v) {
    W1(kTraceUInt32);
    ptr_[0] = (uint8_t)(v);       ptr_[1] = (uint8_t)(v >>  8);
    ptr_[2] = (uint8_t)(v >> 16); ptr_[3] = (uint8_t)(v >> 24);
    ptr_ += 4;
  }

  __attribute__((always_inline)) inline void FieldFuncRef(uint8_t dist)
    { W1(kTraceFuncRef); W1(dist); }

  __attribute__((always_inline)) inline void FieldFuncName(const char* name, int len)
    { W1(kTraceFuncName); W2((uint16_t)len); WBytes(name, len); }

  // ── Timestamp ───────────────────────────────────────────────────────────────
  __attribute__((always_inline)) inline uint64_t NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;
  }

  // ── Timestamp of last emitted Ignition/TurboFan event ───────────────────────
  uint64_t last_event_ts_;

  // ── Call ID counter + call stack ─────────────────────────────────────────────
  // call_stack_: each ENTER pushes, EXIT pops.  SUSPEND pops too (child async
  // frames must not shadow the parent's id) and parks the id in
  // suspended_calls_ keyed on the generator's identity hash.  RESUME restores.
  uint32_t              next_call_id_;
  std::vector<uint32_t> call_stack_;
  std::unordered_map<uintptr_t, uint32_t> suspended_calls_;

  // ── Function-name ring buffer ────────────────────────────────────────────────
  const void* cache_[kTraceCacheSize];
  int cache_head_;
  int cache_count_;

  __attribute__((always_inline)) inline int CacheLookup(const void* key) {
    int n = cache_count_;
    for (int i = 1; i <= n; i++) {
      int slot = cache_head_ - i;
      if (slot < 0) slot += kTraceCacheSize;
      if (cache_[slot] == key) return i;
    }
    return 0;
  }

  __attribute__((always_inline)) inline void CacheInsert(const void* key) {
    cache_[cache_head_] = key;
    if (++cache_head_ == kTraceCacheSize) cache_head_ = 0;
    if (cache_count_ < kTraceCacheSize) ++cache_count_;
  }

  // ── Core emitter (EXIT / SUSPEND / RESUME — 4 fields) ───────────────────────
  __attribute__((always_inline)) inline void EmitFuncEvent(
      TraceEventType type, const void* sfi_key,
      const char* name, int name_len, uint32_t call_id) {
    EnsureSpace();
    uint64_t ts   = NowNs();
    int      dist = CacheLookup(sfi_key);
    W1(4);
    FieldU8((uint8_t)type);
    FieldU64(ts);
    if (dist > 0) { FieldFuncRef((uint8_t)dist); }
    else          { CacheInsert(sfi_key); FieldFuncName(name, name_len); }
    FieldU32(call_id);
    last_event_ts_ = ts;
  }
};

}  // namespace internal
}  // namespace v8
