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
//
//   START events — each has a matching END identified by call_id:
//   0x00  FUNC_ENTER    name_idx(u32-LE), is_async(u8), call_id(u32-LE)
//   0x03  ASYNC_RESUME  name_idx(u32-LE), call_id(u32-LE)
//
//   END events — matched to their START by call_id:
//   0x01  FUNC_EXIT     name_idx(u32-LE), call_id(u32-LE)
//   0x02  ASYNC_SUSPEND name_idx(u32-LE), call_id(u32-LE)
//   0x04  FUNC_ON_STACK_REPLACEMENT  name_idx(u32-LE), call_id(u32-LE)
//
//   Bookkeeping — no call_id, no stack effect:
//   0x05  OPTIMIZED_BATCH  count(u32-LE)
//            count calls that were not individually recorded: either JIT-compiled
//            (TurboFan/Maglev/Sparkplug stripped the trace hooks) or dropped by
//            the INSPECT_MAX_PER_SECOND throttle.  Emitted immediately before the
//            next kept event; ts = that next event's timestamp.
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
// Throttling (INSPECT_MAX_PER_SECOND, default 100 000):
//   Events are staged until a libuv timer fires (every ~100 ms), then filtered.
//   Calls shorter than a computed duration threshold are dropped and folded into
//   OPTIMIZED_BATCH along with any JIT-compiled calls.
//   The threshold is derived by exponential-bucket histogram so that the
//   surviving call count ≈ max_per_second × elapsed_seconds per flush window.
//   A synthetic FUNC_ENTER / FUNC_EXIT pair named "(trace-flush)" wraps each
//   flush period, showing exactly how long the filter pass took.
//
// Approximate record sizes (delta fits in 1 byte — typical between events):
//   NEW_NAME (len N):                     1 + 1 + 4 + 2 + N = 8+N bytes
//   ENTER:                                1 + 1 + 4 + 1 + 4 = 11 bytes
//   EXIT/SUSPEND/RESUME/ON_STACK_REPLACEMENT: 1 + 1 + 4 + 4 = 10 bytes
//   OPTIMIZED_BATCH:                      1 + 1 + 4 = 6 bytes

namespace v8 {
namespace internal {

static constexpr size_t kTraceChunkSize = 128ULL * 1024 * 1024;
static constexpr size_t kMaxTraceRecord = 512;

enum TraceEventType : uint8_t {
  // START events (matched to END by call_id):
  kTraceFuncEnter              = 0x00,
  kTraceAsyncResume            = 0x03,
  // END events (matched to START by call_id):
  kTraceFuncExit               = 0x01,
  kTraceAsyncSuspend           = 0x02,
  kTraceFuncOnStackReplacement = 0x04,
  // Bookkeeping (no call_id):
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
        next_call_id_(0), last_event_ts_(0), max_per_second_(100000),
        last_flush_ts_(0) {}

  ~TraceWriter() {
    ForceCloseAbove(-1);
    FlushStage();
    FlushBuffer();
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (buf_)       { free(buf_);       buf_       = nullptr; }
    if (flush_stk_) { free(flush_stk_); flush_stk_ = nullptr; }
    if (keep_bm_)   { free(keep_bm_);   keep_bm_   = nullptr; }
  }

  bool Initialize(const char* output_path) {
    fd_ = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) { perror("TraceWriter: open"); return false; }
    void* mem = malloc(kTraceChunkSize);
    if (!mem) { perror("TraceWriter: malloc"); close(fd_); fd_ = -1; return false; }
    buf_     = static_cast<uint8_t*>(mem);
    ptr_     = buf_;
    buf_end_ = buf_ + kTraceChunkSize;

    const char* mps = getenv("INSPECT_MAX_PER_SECOND");
    if (mps) {
      uint64_t v = strtoull(mps, nullptr, 10);
      if (v > 0 && v <= UINT32_MAX) max_per_second_ = static_cast<uint32_t>(v);
    }
    return true;
  }

  // Returns true if this SFI pointer has already been assigned a name index.
  __attribute__((always_inline)) inline bool HasName(const void* sfi_key) const {
    return name_table_.Has(sfi_key);
  }

  // Public timing helper so the flush-timer callback can bracket the flush.
  __attribute__((always_inline)) inline uint64_t NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;
  }

  // ── Trace event writers — push to stage_, call_stack_ tracked in real-time ──

  __attribute__((always_inline)) inline void WriteFuncEnter(
      const void* sfi_key, const char* name, int name_len, bool is_async) {
    uint32_t call_id = next_call_id_++;
    uint32_t idx = EnsureNamed(sfi_key, name, name_len);
    uint64_t ts = NowNs();
    call_stack_.push_back({call_id, sfi_key, idx});
    stage_.push_back({.ts = ts, .name_idx = idx, .call_id = call_id,
                      .type = kTraceFuncEnter, .is_async = (uint8_t)(is_async ? 1 : 0)});
  }

  // Pops the top of call_stack_. Ignition always EXITs in LIFO order so no
  // frame-pointer matching is needed.
  __attribute__((always_inline)) inline void WriteFuncExit() {
    if (call_stack_.empty()) return;
    uint32_t call_id  = call_stack_.back().call_id;
    uint32_t name_idx = call_stack_.back().name_idx;
    call_stack_.pop_back();
    stage_.push_back({.ts = NowNs(), .name_idx = name_idx, .call_id = call_id,
                      .type = kTraceFuncExit});
  }

  __attribute__((noinline)) void WriteAsyncSuspend(
      const void* sfi_key, const char* name, int name_len) {
    if (call_stack_.empty()) return;
    uint32_t call_id  = call_stack_.back().call_id;
    uint32_t name_idx = call_stack_.back().name_idx;
    call_stack_.pop_back();
    stage_.push_back({.ts = NowNs(), .name_idx = name_idx, .call_id = call_id,
                      .type = kTraceAsyncSuspend});
  }

  __attribute__((noinline)) void WriteAsyncResume(
      const void* sfi_key, const char* name, int name_len) {
    uint32_t call_id = next_call_id_++;
    uint32_t idx = EnsureNamed(sfi_key, name, name_len);
    uint64_t ts = NowNs();
    call_stack_.push_back({call_id, sfi_key, idx});
    stage_.push_back({.ts = ts, .name_idx = idx, .call_id = call_id,
                      .type = kTraceAsyncResume});
  }

  __attribute__((noinline)) void WriteOnStackReplacement(
      const void* sfi_key, const char* name, int name_len) {
    // TurboFan takes over this frame — pop it, Ignition will never EXIT it.
    if (!call_stack_.empty() && call_stack_.back().sfi_key == sfi_key) {
      uint32_t call_id  = call_stack_.back().call_id;
      uint32_t name_idx = call_stack_.back().name_idx;
      call_stack_.pop_back();
      stage_.push_back({.ts = NowNs(), .name_idx = name_idx, .call_id = call_id,
                        .type = kTraceFuncOnStackReplacement});
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
    uint32_t call_id  = call_stack_.back().call_id;
    uint32_t name_idx = call_stack_.back().name_idx;
    call_stack_.pop_back();
    stage_.push_back({.ts = NowNs(), .name_idx = name_idx, .call_id = call_id,
                      .type = kTraceFuncOnStackReplacement});
  }

  void WriteOptimizedBatch(uint64_t count) {
    stage_.push_back({.ts = NowNs(), .batch_count = static_cast<uint32_t>(count),
                      .type = kTraceOptimizedBatch});
  }

  // ── Buffer I/O ──────────────────────────────────────────────────────────────

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

  // ── Stage flush — called by the 100ms libuv timer ──────────────────────────

  // Filter staged events to meet INSPECT_MAX_PER_SECOND, then emit to buf_.
  // Three passes:
  //   1. Stack-based matching → exponential histogram → min_duration threshold
  //   2. Stack-based matching → compact keep bitmap (1 byte per call_id slot)
  //   3. Emit: kept events pass through; excluded calls + OPTIMIZED_BATCH events
  //      fold into a single pending count emitted as OPTIMIZED_BATCH before the
  //      next kept event.
  __attribute__((noinline)) void FlushStage() {
    if (stage_.empty()) return;

    // Count starts; find call_id range for keep bitmap.
    uint32_t start_count = 0;
    uint32_t min_id = UINT32_MAX, max_id = 0;
    for (const auto& e : stage_) {
      if (e.type == kTraceFuncEnter || e.type == kTraceAsyncResume) {
        ++start_count;
        if (e.call_id < min_id) min_id = e.call_id;
        if (e.call_id > max_id) max_id = e.call_id;
      }
    }

    uint64_t window_start = last_flush_ts_ ? last_flush_ts_ : stage_.front().ts;
    uint64_t window_ns    = stage_.back().ts - window_start;
    if (window_ns == 0) window_ns = 1;
    uint32_t target = static_cast<uint32_t>(
        static_cast<uint64_t>(max_per_second_) * window_ns / 1000000000ULL);

    if (start_count <= target || start_count == 0) {
      FlushStageAll();
      return;
    }

    // ── Pass 1: exponential histogram ───────────────────────────────────────
    // Bucket b covers durations in [2^b, 2^(b+1)) ns; bucket 0 covers [0, 2).
    FlushStkReset();
    uint32_t histogram[64] = {};

    for (const auto& e : stage_) {
      if (e.type == kTraceFuncEnter || e.type == kTraceAsyncResume) {
        FlushStkPush(e.call_id, e.ts);
      } else if (e.type == kTraceFuncExit || e.type == kTraceAsyncSuspend ||
                 e.type == kTraceFuncOnStackReplacement) {
        int idx = FlushStkFind(e.call_id);
        if (idx >= 0) {
          uint64_t dur = e.ts - flush_stk_[idx].ts;
          histogram[(dur == 0) ? 0 : (63 - __builtin_clzll(dur))]++;
          flush_stk_top_ = idx;  // pop match + everything above
        }
      }
    }

    // ── Compute threshold via interpolation ──────────────────────────────────
    // Walk from longest bucket down; interpolate within the straddling bucket
    // so that kept_count ≈ target.
    uint64_t min_duration = [&]() -> uint64_t {
      uint32_t cum = 0;
      for (int b = 63; b >= 0; b--) {
        if (cum + histogram[b] <= target) { cum += histogram[b]; continue; }
        uint32_t needed = target - cum;
        uint64_t bmin = (b == 0) ? 0ULL : (1ULL << b);
        uint64_t bmax = (b >= 63) ? UINT64_MAX : (2ULL << b);
        uint64_t span  = bmax - bmin;
        if (needed == 0 || histogram[b] == 0) return bmax;
        return bmin + span * (uint64_t)(histogram[b] - needed) / histogram[b];
      }
      return 0;
    }();

    // ── Allocate/grow keep bitmap ────────────────────────────────────────────
    // One byte per call_id slot in [min_id, max_id].  Preserved across flushes
    // up to 16 MB; larger windows use a temporary allocation.
    uint32_t id_span = max_id - min_id + 1;
    static constexpr uint32_t kMaxPersistBm = 16u * 1024 * 1024;
    uint8_t* bm;
    bool bm_is_temp = false;
    if (id_span <= kMaxPersistBm) {
      if (id_span > keep_bm_cap_) {
        keep_bm_     = static_cast<uint8_t*>(realloc(keep_bm_, id_span));
        keep_bm_cap_ = id_span;
      }
      bm = keep_bm_;
    } else {
      bm = static_cast<uint8_t*>(malloc(id_span));
      bm_is_temp = true;
    }
    memset(bm, 0, id_span);

    // ── Pass 2: populate keep bitmap ─────────────────────────────────────────
    FlushStkReset();
    for (const auto& e : stage_) {
      if (e.type == kTraceFuncEnter || e.type == kTraceAsyncResume) {
        FlushStkPush(e.call_id, e.ts);
      } else if (e.type == kTraceFuncExit || e.type == kTraceAsyncSuspend ||
                 e.type == kTraceFuncOnStackReplacement) {
        int idx = FlushStkFind(e.call_id);
        if (idx < 0) {
          // Cross-boundary exit (no matching enter in this window): always keep.
          if (e.call_id >= min_id && e.call_id <= max_id)
            bm[e.call_id - min_id] = 1;
        } else {
          // Entries above idx were interrupted by this exit: always keep them.
          for (int i = idx + 1; i < flush_stk_top_; i++)
            bm[flush_stk_[i].call_id - min_id] = 1;
          uint64_t dur = e.ts - flush_stk_[idx].ts;
          bm[e.call_id - min_id] = (dur >= min_duration) ? 1 : 0;
          flush_stk_top_ = idx;  // pop match + everything above
        }
      }
    }
    // Open at end of window = cross-boundary enters: keep.
    for (int i = 0; i < flush_stk_top_; i++)
      bm[flush_stk_[i].call_id - min_id] = 1;

    // ── Pass 3: emit, folding excluded + batch counts into OPTIMIZED_BATCH ───
    uint32_t pending = 0;
    auto emit_pending = [&](uint64_t ts) __attribute__((always_inline)) {
      if (!pending) return;
      EnsureSpace();
      WriteHeader(kTraceOptimizedBatch, ts);
      W4(pending);
      pending = 0;
    };

    for (const auto& e : stage_) {
      if (e.type == kTraceFuncEnter || e.type == kTraceAsyncResume) {
        bool keep = (e.call_id < min_id || e.call_id > max_id)
                    || bm[e.call_id - min_id];
        if (keep) { emit_pending(e.ts); EmitStagedToBuf(e); }
        else       pending++;
      } else if (e.type == kTraceFuncExit || e.type == kTraceAsyncSuspend ||
                 e.type == kTraceFuncOnStackReplacement) {
        bool keep = (e.call_id < min_id || e.call_id > max_id)
                    || bm[e.call_id - min_id];
        if (keep) { emit_pending(e.ts); EmitStagedToBuf(e); }
        // excluded ends silently dropped
      } else if (e.type == kTraceOptimizedBatch) {
        pending += e.batch_count;
      }
    }
    if (pending) emit_pending(stage_.back().ts);

    if (bm_is_temp) free(bm);
    if (!stage_.empty()) last_flush_ts_ = stage_.back().ts;
    stage_.clear();
  }

  // Emit all staged events without filtering (shutdown / forced drain).
  __attribute__((noinline)) void FlushStageAll() {
    if (!stage_.empty()) last_flush_ts_ = stage_.back().ts;
    for (const auto& ev : stage_) EmitStagedToBuf(ev);
    stage_.clear();
  }

  // Emit a synthetic ENTER/EXIT pair named "(trace-flush)" spanning [start,end].
  // Call after FlushStage() so the timestamps come after all staged events.
  __attribute__((noinline)) void EmitFlushSyntheticEvent(uint64_t start_ts, uint64_t end_ts) {
    static const char kName[] = "(trace-flush)";
    static const int  kNameLen = 13;
    // Use a sentinel key that can never collide with a real SFI pointer (odd,
    // below any heap object address).
    static const void* const kKey = reinterpret_cast<const void*>(uintptr_t{1});

    uint32_t call_id = next_call_id_++;
    bool is_new;
    uint32_t idx = name_table_.Upsert(kKey, &is_new);
    EnsureSpace();
    if (is_new) {
      WriteHeader(kTraceNewName, start_ts);
      W4(idx); W2((uint16_t)kNameLen); WBytes(kName, kNameLen);
    }
    EnsureSpace();
    WriteHeader(kTraceFuncEnter, start_ts);
    W4(idx); W1(0); W4(call_id);
    EnsureSpace();
    WriteHeader(kTraceFuncExit, end_ts);
    W4(idx); W4(call_id);
  }

 private:
  // ── Binary write buffer ──────────────────────────────────────────────────────
  uint8_t* buf_;
  uint8_t* ptr_;
  uint8_t* buf_end_;
  int      fd_;

  __attribute__((always_inline)) inline void EnsureSpace() {
    if (__builtin_expect(ptr_ + 2 * kMaxTraceRecord > buf_end_, 0))
      FlushBuffer();
  }

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

  // ── Timestamp delta encoding ─────────────────────────────────────────────────
  uint64_t last_event_ts_;

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

  // ── Call ID, call stack, staged events ───────────────────────────────────────
  uint32_t next_call_id_;
  uint32_t max_per_second_;
  uint64_t last_flush_ts_;

  // ── Flush-pass stack (preserved across flushes to avoid repeated malloc) ────
  struct FlushStkE { uint32_t call_id; uint64_t ts; };
  FlushStkE* flush_stk_     = nullptr;
  int        flush_stk_top_ = 0;
  int        flush_stk_cap_ = 0;

  void FlushStkReset() { flush_stk_top_ = 0; }

  void FlushStkPush(uint32_t id, uint64_t ts) {
    if (flush_stk_top_ == flush_stk_cap_) {
      flush_stk_cap_ = flush_stk_cap_ ? flush_stk_cap_ * 2 : 64;
      flush_stk_ = static_cast<FlushStkE*>(
          realloc(flush_stk_, flush_stk_cap_ * sizeof(FlushStkE)));
    }
    flush_stk_[flush_stk_top_++] = {id, ts};
  }

  int FlushStkFind(uint32_t id) const {
    for (int i = flush_stk_top_ - 1; i >= 0; i--)
      if (flush_stk_[i].call_id == id) return i;
    return -1;
  }

  // ── Keep bitmap (preserved across flushes up to 16 MB) ──────────────────────
  uint8_t* keep_bm_     = nullptr;
  uint32_t keep_bm_cap_ = 0;

  struct StackFrame {
    uint32_t    call_id;
    const void* sfi_key;
    uint32_t    name_idx;
  };
  std::vector<StackFrame> call_stack_;

  // One entry per event staged for the current 100ms window.
  // NEW_NAME events are emitted directly to buf_ (never staged) so new_name
  // is not needed here.  24-byte struct fits 2.5× more events per cache line.
  struct StagedEvent {
    uint64_t       ts          = 0;
    uint32_t       name_idx    = 0;
    uint32_t       call_id     = 0;
    uint32_t       batch_count = 0;
    TraceEventType type        = kTraceFuncEnter;
    uint8_t        is_async    = 0;
    uint16_t       _pad        = 0;
  };
  static_assert(sizeof(StagedEvent) == 24, "StagedEvent must be 24 bytes");
  std::vector<StagedEvent> stage_;

  std::vector<std::string> debug_name_strs_;  // idx→name, only populated in strict mode

  // ── SFI pointer → name index table ──────────────────────────────────────────
  SfiNameTable name_table_;

  // ── EnsureNamed: assign name_idx, emit NEW_NAME directly to buf_ on first use.
  // NEW_NAME is emitted with ts=last_event_ts_ (zero delta from previous buf_
  // write) so that staged events with earlier timestamps still produce positive
  // deltas when FlushStage emits them later.
  __attribute__((always_inline)) inline uint32_t EnsureNamed(
      const void* sfi_key, const char* name, int name_len) {
    bool is_new;
    uint32_t idx = name_table_.Upsert(sfi_key, &is_new);
    if (__builtin_expect(is_new, 0)) {
      if (!name || name_len == 0) { name = "(anonymous)"; name_len = 11; }
      if (StrictMode()) {
        if (idx >= debug_name_strs_.size()) debug_name_strs_.resize(idx + 1);
        debug_name_strs_[idx] = std::string(name, name_len);
      }
      EnsureSpace();
      WriteHeader(kTraceNewName, last_event_ts_);
      W4(idx);
      W2((uint16_t)name_len);
      WBytes(name, name_len);
    }
    return idx;
  }

  // ── Emit one staged event to the binary buffer ───────────────────────────────
  // NEW_NAME events are emitted directly in EnsureNamed and never appear in stage_.
  __attribute__((noinline)) void EmitStagedToBuf(const StagedEvent& ev) {
    EnsureSpace();
    switch (ev.type) {
      case kTraceFuncEnter:
        WriteHeader(kTraceFuncEnter, ev.ts);
        W4(ev.name_idx); W1(ev.is_async); W4(ev.call_id);
        break;
      case kTraceFuncExit:
      case kTraceAsyncSuspend:
      case kTraceAsyncResume:
      case kTraceFuncOnStackReplacement:
        WriteHeader(ev.type, ev.ts);
        W4(ev.name_idx); W4(ev.call_id);
        break;
      case kTraceOptimizedBatch:
        WriteHeader(kTraceOptimizedBatch, ev.ts);
        W4(ev.batch_count);
        break;
      default: break;
    }
  }

  // ── Stack helpers ────────────────────────────────────────────────────────────

  // Search by SFI key — used only for OSR where the FP is from TurboFan.
  int FindInStackBySfi(const void* sfi_key) const {
    for (int i = (int)call_stack_.size() - 1; i >= 0; i--) {
      if (call_stack_[i].sfi_key == sfi_key) return i;
    }
    return -1;
  }

  // Emit EXIT for every frame above target_idx (inclusive of boundary), then pop.
  // Pass target_idx=-1 to close everything.
  __attribute__((noinline)) void ForceCloseAbove(int target_idx) {
    while ((int)call_stack_.size() - 1 > target_idx) {
      const StackFrame f = call_stack_.back();
      call_stack_.pop_back();
      stage_.push_back({.ts = NowNs(), .name_idx = f.name_idx, .call_id = f.call_id,
                        .type = kTraceFuncExit});
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

};


}  // namespace internal
}  // namespace v8
