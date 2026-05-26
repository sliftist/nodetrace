#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
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
//   [uint8]  header  =  ss:2 | type:6
//            ss encodes the byte-width of the delta-timestamp that follows:
//              0 → 1 byte   1 → 2 bytes   2 → 4 bytes   3 → 8 bytes
//   [delta]  timestamp delta from the previous record (LE, ss bytes)
//            absolute ts = running sum of all deltas since t=0
//   [fields] fixed layout determined by type — no per-field tags
//
// FUNCTION NAME FIELD (used in ENTER/EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT):
//   [uint8]  prefix
//     0       → new name: [uint16-LE len][utf-8 bytes], pushed to ring cache
//     1..128  → cache ref: distance into ring (1 = most recently seen)
//
// EVENT TYPES (type field, bottom 6 bits of header):
//   0x00  FUNC_ENTER    cache_ref(u8), is_async(u8), call_id(u32-LE)
//   0x01  FUNC_EXIT     cache_ref(u8), call_id(u32-LE)
//   0x02  ASYNC_SUSPEND cache_ref(u8), call_id(u32-LE)
//   0x03  ASYNC_RESUME  cache_ref(u8), call_id(u32-LE)
//   0x04  FUNC_ON_STACK_REPLACEMENT  cache_ref(u8), call_id(u32-LE)
//   0x05  OPTIMIZED_BATCH  count(u32-LE)
//            ts in header = ts_end of the JIT window
//            ts_start is implicitly the previous record's timestamp
//   0x06  NEW_NAME      len(u16-LE), utf-8 bytes
//            Pushes a new name onto the ring cache.  Always emitted
//            immediately before the first call event that uses it,
//            with the same timestamp (delta = 0 on the following record).
//
// cache_ref is always 1-128 (1 = most-recently-pushed name).
// NEW_NAME is emitted automatically; readers never see cache_ref = 0.
//
// call_id is a process-global u32 counter incremented on every ENTER.
// SUSPEND/RESUME/EXIT carry the same call_id as their matching ENTER.
//
// Approximate record sizes (delta fits in 1 byte — typical between events):
//   NEW_NAME (len N):       1 + 1 + 2 + N = 4+N bytes
//   ENTER:                  1 + 1 + 1 + 1 + 4 = 8 bytes  (delta=0 if name was new)
//   EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT: 1 + 1 + 1 + 4 = 7 bytes
//   OPTIMIZED_BATCH:        1 + 1 + 4 = 6 bytes

namespace v8 {
namespace internal {

static constexpr size_t kTraceChunkSize = 128ULL * 1024 * 1024;
static constexpr size_t kMaxTraceRecord = 512;
static constexpr int    kTraceCacheSize = 128;

enum TraceEventType : uint8_t {
  kTraceFuncEnter     = 0x00,
  kTraceFuncExit      = 0x01,
  kTraceAsyncSuspend  = 0x02,
  kTraceAsyncResume   = 0x03,
  kTraceFuncOnStackReplacement = 0x04,
  kTraceOptimizedBatch = 0x05,
  kTraceNewName       = 0x06,
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
    if (!mem) { perror("TraceWriter: malloc"); close(fd_); fd_ = -1; return false; }
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
    uint64_t ts = NowNs();
    uint8_t ref = EnsureCached(sfi_key, name, name_len, ts);
    WriteHeader(kTraceFuncEnter, ts);
    W1(ref);
    W1(is_async ? 1 : 0);
    W4(call_id);
  }

  __attribute__((always_inline)) inline void WriteFuncExit(
      const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    if (!call_stack_.empty()) call_stack_.pop_back();
    EmitFuncEvent(kTraceFuncExit, sfi_key, name, name_len, call_id);
  }

  __attribute__((noinline)) void WriteAsyncSuspend(
      uintptr_t gen_key, const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    if (!call_stack_.empty()) call_stack_.pop_back();
    suspended_calls_[gen_key] = call_id;
    EmitFuncEvent(kTraceAsyncSuspend, sfi_key, name, name_len, call_id);
  }

  __attribute__((noinline)) void WriteOnStackReplacement(
      const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = call_stack_.empty() ? 0 : call_stack_.back();
    EmitFuncEvent(kTraceFuncOnStackReplacement, sfi_key, name, name_len, call_id);
  }

  // ts_start of the JIT window = last_event_ts_; ts_end = NowNs() (in header).
  void WriteOptimizedBatch(uint64_t count) {
    EnsureSpace();
    uint64_t ts_end = NowNs();
    WriteHeader(kTraceOptimizedBatch, ts_end);
    W4((uint32_t)count);
  }

  __attribute__((noinline)) void WriteAsyncResume(
      uintptr_t gen_key, const void* sfi_key, const char* name, int name_len) {
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
    if (__builtin_expect(ptr_ + 2 * kMaxTraceRecord > buf_end_, 0))
      FlushBuffer();
  }

  // ── Raw stores ──────────────────────────────────────────────────────────────
  __attribute__((always_inline)) inline void W1(uint8_t v)  { *ptr_++ = v; }

  __attribute__((always_inline)) inline void W2(uint16_t v) {
    ptr_[0] = (uint8_t)(v); ptr_[1] = (uint8_t)(v >> 8); ptr_ += 2;
  }

  __attribute__((always_inline)) inline void W4(uint32_t v) {
    ptr_[0] = (uint8_t)(v);       ptr_[1] = (uint8_t)(v >>  8);
    ptr_[2] = (uint8_t)(v >> 16); ptr_[3] = (uint8_t)(v >> 24);
    ptr_ += 4;
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

  // ── Timestamp ───────────────────────────────────────────────────────────────
  __attribute__((always_inline)) inline uint64_t NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;
  }

  // ── Header: [ss:2 | type:6] + variable-width delta timestamp ────────────────
  __attribute__((always_inline)) inline void WriteHeader(uint8_t type, uint64_t ts) {
    uint64_t delta = ts - last_event_ts_;
    int ss;
    if      (delta <= 0xFFU)         ss = 0;
    else if (delta <= 0xFFFFU)       ss = 1;
    else if (delta <= 0xFFFFFFFFU)   ss = 2;
    else                             ss = 3;
    W1((uint8_t)((ss << 6) | (type & 0x3F)));
    switch (ss) {
      case 0: W1((uint8_t)delta);           break;
      case 1: W2((uint16_t)delta);          break;
      case 2: W4((uint32_t)delta);          break;
      case 3: W8(delta);                    break;
    }
    last_event_ts_ = ts;
  }

  // ── Ensure name is in cache; emit NEW_NAME event if not ────────────────────
  // Returns the cache ref (1-128) to write into the following call record.
  // If the name is new, a NEW_NAME record is emitted with the given ts so
  // the subsequent call record has delta=0.
  __attribute__((always_inline)) inline uint8_t EnsureCached(
      const void* sfi_key, const char* name, int name_len, uint64_t ts) {
    int dist = CacheLookup(sfi_key);
    if (__builtin_expect(dist > 0, 1)) return (uint8_t)dist;
    WriteHeader(kTraceNewName, ts);
    W2((uint16_t)name_len);
    WBytes(name, name_len);
    CacheInsert(sfi_key);
    return 1;
  }

  // ── Timestamp of last emitted event ─────────────────────────────────────────
  uint64_t last_event_ts_;

  // ── Call ID + call stack ─────────────────────────────────────────────────────
  uint32_t              next_call_id_;
  std::vector<uint32_t> call_stack_;
  std::unordered_map<uintptr_t, uint32_t> suspended_calls_;

  // ── Function-name ring cache ─────────────────────────────────────────────────
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

  // ── Core emitter for EXIT / SUSPEND / RESUME / ON_STACK_REPLACEMENT ─────────
  __attribute__((always_inline)) inline void EmitFuncEvent(
      TraceEventType type, const void* sfi_key,
      const char* name, int name_len, uint32_t call_id) {
    EnsureSpace();
    uint64_t ts = NowNs();
    uint8_t ref = EnsureCached(sfi_key, name, name_len, ts);
    WriteHeader(type, ts);
    W1(ref);
    W4(call_id);
  }
};

}  // namespace internal
}  // namespace v8
