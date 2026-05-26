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
// EVENT TYPES (type field, bottom 6 bits of header):
//   0x00  FUNC_ENTER    name_idx(u32-LE), is_async(u8), call_id(u32-LE)
//   0x01  FUNC_EXIT     name_idx(u32-LE), call_id(u32-LE)
//   0x02  ASYNC_SUSPEND name_idx(u32-LE), call_id(u32-LE)
//   0x03  ASYNC_RESUME  name_idx(u32-LE), call_id(u32-LE)
//   0x04  FUNC_ON_STACK_REPLACEMENT  name_idx(u32-LE), call_id(u32-LE)
//   0x05  OPTIMIZED_BATCH  count(u32-LE)
//            ts in header = ts_end of the JIT window
//            ts_start is implicitly the previous record's timestamp
//   0x06  NEW_NAME      name_idx(u32-LE), len(u16-LE), utf-8 bytes
//            Assigns a permanent index to a function name.  Always emitted
//            before the first call event that references that name_idx.
//            The following call event has delta=0 (same timestamp).
//
// name_idx is assigned in order of first appearance (0, 1, 2, ...).
// The reader maintains a flat array: names[name_idx] = string.
//
// call_id is a process-global u32 counter incremented on every ENTER.
// SUSPEND/RESUME/EXIT carry the same call_id as their matching ENTER.
//
// Approximate record sizes (delta fits in 1 byte — typical between events):
//   NEW_NAME (len N):                  1 + 1 + 4 + 2 + N = 8+N bytes
//   ENTER:                             1 + 1 + 4 + 1 + 4 = 11 bytes
//   EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT: 1 + 1 + 4 + 4 = 10 bytes
//   OPTIMIZED_BATCH:                   1 + 1 + 4 = 6 bytes

namespace v8 {
namespace internal {

static constexpr size_t kTraceChunkSize = 128ULL * 1024 * 1024;
static constexpr size_t kMaxTraceRecord = 512;

enum TraceEventType : uint8_t {
  kTraceFuncEnter              = 0x00,
  kTraceFuncExit               = 0x01,
  kTraceAsyncSuspend           = 0x02,
  kTraceAsyncResume            = 0x03,
  kTraceFuncOnStackReplacement = 0x04,
  kTraceOptimizedBatch         = 0x05,
  kTraceNewName                = 0x06,
};

// ── SFI pointer → name index hash table ──────────────────────────────────────
// Open addressing, linear probing.  Load factor capped at 1/4.
// Starts at 16 slots, grows ×4 on overflow.
class SfiNameTable {
 public:
  struct Slot { const void* ptr; uint32_t idx; };

  SfiNameTable() : slots_(nullptr), capacity_(0), count_(0), next_idx_(0) {
    Resize(16);
  }
  ~SfiNameTable() { free(slots_); }

  // Returns the existing index for ptr, or assigns next_idx_ and returns that.
  // Sets *is_new=true when a fresh index was assigned.
  __attribute__((always_inline)) inline uint32_t Upsert(const void* ptr, bool* is_new) {
    if (__builtin_expect(count_ > (capacity_ >> 2), 0)) Resize(capacity_ << 2);
    uint32_t h = Hash(ptr), i = 0;
    for (;;) {
      uint32_t s = (h + i++) & (capacity_ - 1);
      if (!slots_[s].ptr) {
        slots_[s] = {ptr, next_idx_};
        count_++;
        *is_new = true;
        return next_idx_++;
      }
      if (slots_[s].ptr == ptr) { *is_new = false; return slots_[s].idx; }
    }
  }

  __attribute__((always_inline)) inline bool Has(const void* ptr) const {
    uint32_t h = Hash(ptr), i = 0;
    for (;;) {
      uint32_t s = (h + i++) & (capacity_ - 1);
      if (!slots_[s].ptr) return false;
      if (slots_[s].ptr == ptr) return true;
    }
  }

 private:
  Slot*    slots_;
  uint32_t capacity_;
  uint32_t count_;
  uint32_t next_idx_;

  static __attribute__((always_inline)) inline uint32_t Hash(const void* ptr) {
    uintptr_t h = reinterpret_cast<uintptr_t>(ptr) >> 3;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return static_cast<uint32_t>(h);
  }

  __attribute__((noinline)) void Resize(uint32_t new_cap) {
    Slot* ns = static_cast<Slot*>(calloc(new_cap, sizeof(Slot)));
    for (uint32_t i = 0; i < capacity_; i++) {
      if (!slots_[i].ptr) continue;
      uint32_t h = Hash(slots_[i].ptr), j = 0;
      for (;;) {
        uint32_t s = (h + j++) & (new_cap - 1);
        if (!ns[s].ptr) { ns[s] = slots_[i]; break; }
      }
    }
    free(slots_);
    slots_ = ns;
    capacity_ = new_cap;
  }
};

class TraceWriter {
 public:
  TraceWriter()
      : buf_(nullptr), ptr_(nullptr), buf_end_(nullptr), fd_(-1),
        next_call_id_(0), last_event_ts_(0) {}

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

  // Returns true if this SFI pointer has already been assigned a name index.
  // The caller can use this to skip the expensive DebugNameCStr() call.
  __attribute__((always_inline)) inline bool HasName(const void* sfi_key) const {
    return name_table_.Has(sfi_key);
  }

  __attribute__((always_inline)) inline void WriteFuncEnter(
      const void* sfi_key, const char* name, int name_len, bool is_async) {
    EnsureSpace();
    uint32_t call_id = next_call_id_++;
    call_stack_.push_back(call_id);
    uint64_t ts = NowNs();
    uint32_t idx = EnsureNamed(sfi_key, name, name_len, ts);
    WriteHeader(kTraceFuncEnter, ts);
    W4(idx);
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

  // ── Ensure name is indexed; emit NEW_NAME if first time seen ────────────────
  // Returns the name_idx to embed in the following call record.
  // If new: emits a NEW_NAME record with the given ts (call record gets delta=0).
  __attribute__((always_inline)) inline uint32_t EnsureNamed(
      const void* sfi_key, const char* name, int name_len, uint64_t ts) {
    bool is_new;
    uint32_t idx = name_table_.Upsert(sfi_key, &is_new);
    if (__builtin_expect(is_new, 0)) {
      WriteHeader(kTraceNewName, ts);
      W4(idx);
      W2((uint16_t)name_len);
      WBytes(name, name_len);
    }
    return idx;
  }

  // ── Timestamp of last emitted event ─────────────────────────────────────────
  uint64_t last_event_ts_;

  // ── Call ID + call stack ─────────────────────────────────────────────────────
  uint32_t              next_call_id_;
  std::vector<uint32_t> call_stack_;
  std::unordered_map<uintptr_t, uint32_t> suspended_calls_;

  // ── SFI pointer → name index table ──────────────────────────────────────────
  SfiNameTable name_table_;

  // ── Core emitter for EXIT / SUSPEND / RESUME / ON_STACK_REPLACEMENT ─────────
  __attribute__((always_inline)) inline void EmitFuncEvent(
      TraceEventType type, const void* sfi_key,
      const char* name, int name_len, uint32_t call_id) {
    EnsureSpace();
    uint64_t ts = NowNs();
    uint32_t idx = EnsureNamed(sfi_key, name, name_len, ts);
    WriteHeader(type, ts);
    W4(idx);
    W4(call_id);
  }
};

}  // namespace internal
}  // namespace v8
