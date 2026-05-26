#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
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
// Async semantics — each synchronous slice is its own call:
//   ENTER  (is_async=1) → start of first slice; call_id = fresh id
//   SUSPEND             → end of that slice;  call_id matches its ENTER
//   RESUME              → start of next slice; call_id = fresh id (not the ENTER's)
//   EXIT                → end of the last slice (after RESUME or directly after ENTER)
// call_id is a process-global u32 counter incremented on every ENTER and RESUME.
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
    CloseRemainingFrames();
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
      const void* sfi_key, const char* name, int name_len, bool is_async,
      uintptr_t frame_ptr) {
    EnsureSpace();
    uint32_t call_id = next_call_id_++;
    uint64_t ts = NowNs();
    uint32_t idx = EnsureNamed(sfi_key, name, name_len, ts);
    call_stack_.push_back({call_id, sfi_key, idx, frame_ptr});
    WriteHeader(kTraceFuncEnter, ts);
    W4(idx);
    W1(is_async ? 1 : 0);
    W4(call_id);
  }

  __attribute__((always_inline)) inline void WriteFuncExit(
      uintptr_t frame_ptr, const void* sfi_key, const char* name, int name_len) {
    if (!call_stack_.empty() && call_stack_.back().frame_ptr == frame_ptr) {
      uint32_t call_id = call_stack_.back().call_id;
      call_stack_.pop_back();
      EmitFuncEvent(kTraceFuncExit, sfi_key, name, name_len, call_id);
      return;
    }
    HandleMismatchedExit(frame_ptr, sfi_key, name, name_len);
  }

  __attribute__((noinline)) void WriteAsyncSuspend(
      uintptr_t frame_ptr, const void* sfi_key, const char* name, int name_len) {
    if (!call_stack_.empty() && call_stack_.back().frame_ptr == frame_ptr) {
      uint32_t call_id = call_stack_.back().call_id;
      call_stack_.pop_back();
      EmitFuncEvent(kTraceAsyncSuspend, sfi_key, name, name_len, call_id);
      return;
    }
    HandleMismatchedSuspend(frame_ptr, sfi_key, name, name_len);
  }

  __attribute__((noinline)) void WriteOnStackReplacement(
      const void* sfi_key, const char* name, int name_len) {
    // TurboFan takes over this frame — pop it, Ignition will never EXIT it.
    if (!call_stack_.empty() && call_stack_.back().sfi_key == sfi_key) {
      uint32_t call_id = call_stack_.back().call_id;
      call_stack_.pop_back();
      EmitFuncEvent(kTraceFuncOnStackReplacement, sfi_key, name, name_len, call_id);
      return;
    }
    int idx = FindInStackBySfi(sfi_key);
    if (idx < 0) return;  // JIT-compiled entry — no Ignition ENTER to close
    if (StrictMode()) {
      fprintf(stderr, "NODE_TRACE_STRICT: ON_STACK_REPLACEMENT for '%s' not at top "
              "(%d frame(s) above it)\n", name, (int)call_stack_.size() - 1 - idx);
      _exit(1);
    }
    ForceCloseAbove(idx);
    uint32_t call_id = call_stack_.back().call_id;
    call_stack_.pop_back();
    EmitFuncEvent(kTraceFuncOnStackReplacement, sfi_key, name, name_len, call_id);
  }

  void WriteOptimizedBatch(uint64_t count) {
    EnsureSpace();
    uint64_t ts_end = NowNs();
    WriteHeader(kTraceOptimizedBatch, ts_end);
    W4((uint32_t)count);
  }

  __attribute__((noinline)) void WriteAsyncResume(
      uintptr_t frame_ptr, const void* sfi_key, const char* name, int name_len) {
    EnsureSpace();
    uint32_t call_id = next_call_id_++;
    uint64_t ts = NowNs();
    uint32_t idx = EnsureNamed(sfi_key, name, name_len, ts);
    call_stack_.push_back({call_id, sfi_key, idx, frame_ptr});
    WriteHeader(kTraceAsyncResume, ts);
    W4(idx);
    W4(call_id);
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
      if (StrictMode()) {
        if (idx >= debug_name_strs_.size()) debug_name_strs_.resize(idx + 1);
        debug_name_strs_[idx] = (name && name_len > 0)
            ? std::string(name, name_len) : "(anonymous)";
      }
    }
    return idx;
  }

  // ── Timestamp of last emitted event ─────────────────────────────────────────
  uint64_t last_event_ts_;

  // ── Call ID + call stack ─────────────────────────────────────────────────────
  struct StackFrame { uint32_t call_id; const void* sfi_key; uint32_t name_idx; uintptr_t frame_ptr; };
  uint32_t                next_call_id_;
  std::vector<StackFrame> call_stack_;
  std::vector<std::string> debug_name_strs_;  // idx→name, only in strict mode

  // ── SFI pointer → name index table ──────────────────────────────────────────
  SfiNameTable name_table_;

  // ── Close all open frames on shutdown ───────────────────────────────────────
  __attribute__((noinline)) void CloseRemainingFrames() {
    ForceCloseAbove(-1);
  }

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

  // ── Stack helpers ────────────────────────────────────────────────────────────
  // Returns the index of frame_ptr in call_stack_ (searching from top), or -1.
  int FindInStack(uintptr_t frame_ptr) const {
    for (int i = (int)call_stack_.size() - 1; i >= 0; i--) {
      if (call_stack_[i].frame_ptr == frame_ptr) return i;
    }
    return -1;
  }

  // Returns the index of sfi_key in call_stack_ (searching from top), or -1.
  // Used only for OSR matching where frame_ptr is from TurboFan, not Ignition.
  int FindInStackBySfi(const void* sfi_key) const {
    for (int i = (int)call_stack_.size() - 1; i >= 0; i--) {
      if (call_stack_[i].sfi_key == sfi_key) return i;
    }
    return -1;
  }

  // Emits EXIT for every frame above target_idx, popping as it goes.
  // Pass target_idx=-1 to close everything.
  __attribute__((noinline)) void ForceCloseAbove(int target_idx) {
    while ((int)call_stack_.size() - 1 > target_idx) {
      const StackFrame f = call_stack_.back();
      call_stack_.pop_back();
      EmitFuncEvent(kTraceFuncExit, f.sfi_key, nullptr, 0, f.call_id);
    }
  }

  static bool StrictMode() {
    static bool checked = false, strict = false;
    if (__builtin_expect(!checked, 0)) {
      strict = getenv("NODE_TRACE_STRICT") != nullptr;
      checked = true;
    }
    return strict;
  }

  // Called when EXIT arrives for a frame_ptr that isn't at the top of the stack.
  // Not-found (idx<0) means the start was JIT'd — silently ignored, not an error.
  // Found-but-not-top means we missed intervening exits — close them first.
  __attribute__((noinline)) void HandleMismatchedExit(
      uintptr_t frame_ptr, const void* sfi_key, const char* name, int name_len) {
    int idx = FindInStack(frame_ptr);
    if (idx < 0) return;  // start was JIT'd, no matching ENTER — ignore
    if (StrictMode()) {
      int n_above = (int)call_stack_.size() - 1 - idx;
      const char* fn = (name && name_len > 0) ? name
          : (call_stack_[idx].name_idx < debug_name_strs_.size()
              ? debug_name_strs_[call_stack_[idx].name_idx].c_str() : "?");
      fprintf(stderr, "NODE_TRACE_STRICT: EXIT for '%s' (callId=%u) not at top — "
              "%d frame(s) above it\n", fn, call_stack_[idx].call_id, n_above);
      // Print up to 15 ancestor frames (callers of the mismatched function)
      int anc_start = idx - 1;
      int anc_show  = 15;
      if (idx - anc_show > 0) {
        fprintf(stderr, "  ... (%d more frames below) ...\n", idx - anc_show);
        anc_start = anc_show - 1;
      }
      for (int i = (idx <= anc_show ? idx - 1 : anc_show - 1); i >= 0; i--) {
        int actual = (idx <= anc_show) ? i : (idx - anc_show + i);
        const char* frame_name = call_stack_[actual].name_idx < debug_name_strs_.size()
            ? debug_name_strs_[call_stack_[actual].name_idx].c_str() : "?";
        fprintf(stderr, "  [caller %d] callId=%u  %s\n",
                idx - actual, call_stack_[actual].call_id, frame_name);
      }
      fprintf(stderr, "  >>> callId=%u  %s  (EXIT mismatch here)\n",
              call_stack_[idx].call_id, fn);
      // Print all frames above (children still on stack)
      for (int i = idx + 1; i < (int)call_stack_.size(); i++) {
        const char* frame_name = call_stack_[i].name_idx < debug_name_strs_.size()
            ? debug_name_strs_[call_stack_[i].name_idx].c_str() : "?";
        fprintf(stderr, "  [+%d] callId=%u  %s\n",
                i - idx, call_stack_[i].call_id, frame_name);
      }
      FlushBuffer();
      _exit(1);
    }
    ForceCloseAbove(idx);
    uint32_t call_id = call_stack_.back().call_id;
    call_stack_.pop_back();
    EmitFuncEvent(kTraceFuncExit, sfi_key, name, name_len, call_id);
  }

  __attribute__((noinline)) void HandleMismatchedSuspend(
      uintptr_t frame_ptr, const void* sfi_key, const char* name, int name_len) {
    int idx = FindInStack(frame_ptr);
    if (idx < 0) return;
    if (StrictMode()) {
      fprintf(stderr, "NODE_TRACE_STRICT: SUSPEND for '%s' not at top of stack "
              "(%d frame(s) above it)\n", name,
              (int)call_stack_.size() - 1 - idx);
      _exit(1);
    }
    ForceCloseAbove(idx);
    uint32_t call_id = call_stack_.back().call_id;
    call_stack_.pop_back();
    EmitFuncEvent(kTraceAsyncSuspend, sfi_key, name, name_len, call_id);
  }
};

}  // namespace internal
}  // namespace v8
