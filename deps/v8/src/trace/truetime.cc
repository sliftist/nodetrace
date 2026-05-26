#include "src/trace/truetime.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace nodetrace {

// ── Constants ─────────────────────────────────────────────────────────────────
static const double   kTransitionGapMs  = 20.0 * 60.0 * 1000.0;  // 20 min
static const double   kCheckIntervalSec = 5.0 * 60.0;             // 5 min
static const double   kMinTimeRate      = 0.5;
static const int      kNTPVerifyCount   = 3;
static const char     kNTPServer[]      = "time.google.com";
static const int      kNTPPort          = 123;
static const char     kShmPath[]        = "/nodetrace-timesync";
static const uint64_t kNTPEpochOffsetMs = 2208988800ULL * 1000ULL;

// ── Shared memory layout — exactly one cache line (64 bytes) ─────────────────
// version is a seqlock counter: even = consistent, odd = write in progress.
// We use __atomic_* builtins because std::atomic across process boundaries is
// technically UB, but __atomic_* maps directly to the hardware operations that
// work correctly on MAP_SHARED memory on Linux/x86-64/aarch64.
struct alignas(64) ShmData {
    volatile uint64_t version;       //  8 bytes — seqlock
    double last_offset;              //  8 bytes
    double last_update_time;         //  8 bytes  (wall-clock ms since epoch)
    double offset;                   //  8 bytes
    double update_time;              //  8 bytes
    double next_offset;              //  8 bytes
    double next_update_time;         //  8 bytes
    uint8_t _pad[8];                 //  8 bytes  — pad to 64
};
static_assert(sizeof(ShmData) == 64, "ShmData must be exactly 64 bytes");

// ── Process-local globals ─────────────────────────────────────────────────────
static bool           g_enabled   = false;
static ShmData*       g_shm       = nullptr;
static bool           g_use_local = false;
static ShmData        g_local_data{};
static pthread_t      g_thread;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// ── Wall-clock helper (REALTIME = same epoch as Date.now) ─────────────────────
static double NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// ── Seqlock read / write ──────────────────────────────────────────────────────
struct TimeState {
    double last_offset, last_update_time;
    double offset,      update_time;
    double next_offset, next_update_time;
};

static TimeState ReadState() {
    ShmData* d = g_use_local ? &g_local_data : g_shm;
    if (!d) return {};
    TimeState s;
    for (;;) {
        uint64_t v1 = __atomic_load_n(&d->version, __ATOMIC_ACQUIRE);
        if (v1 & 1) continue;  // write in progress — spin
        s.last_offset      = d->last_offset;
        s.last_update_time = d->last_update_time;
        s.offset           = d->offset;
        s.update_time      = d->update_time;
        s.next_offset      = d->next_offset;
        s.next_update_time = d->next_update_time;
        uint64_t v2 = __atomic_load_n(&d->version, __ATOMIC_ACQUIRE);
        if (v1 == v2) return s;  // consistent read
    }
}

static void WriteState(const TimeState& s) {
    ShmData* d = g_use_local ? &g_local_data : g_shm;
    if (!d) return;
    // CAS even → odd to acquire write lock (handles concurrent writers across processes)
    for (;;) {
        uint64_t v = __atomic_load_n(&d->version, __ATOMIC_ACQUIRE);
        if (v & 1) { sched_yield(); continue; }
        uint64_t expected = v;
        if (__atomic_compare_exchange_n(&d->version, &expected, v + 1,
                                        /*weak=*/true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) break;
    }
    d->last_offset      = s.last_offset;
    d->last_update_time = s.last_update_time;
    d->offset           = s.offset;
    d->update_time      = s.update_time;
    d->next_offset      = s.next_offset;
    d->next_update_time = s.next_update_time;
    __atomic_fetch_add(&d->version, 1, __ATOMIC_RELEASE);  // release: odd → even
}

// ── NTP client ────────────────────────────────────────────────────────────────
static double FetchNTPOffsetOnce() {
    const double kNaN = std::numeric_limits<double>::quiet_NaN();

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", kNTPPort);
    if (getaddrinfo(kNTPServer, port_str, &hints, &res) != 0 || !res) return kNaN;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { freeaddrinfo(res); return kNaN; }

    struct timeval tv = { 3, 0 };  // 3 s recv timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[48]{};
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3 (client)

    double send_time = NowMs();
    if (sendto(sock, pkt, 48, 0, res->ai_addr, res->ai_addrlen) != 48) {
        freeaddrinfo(res); close(sock); return kNaN;
    }
    freeaddrinfo(res);

    if (recv(sock, pkt, 48, 0) != 48) { close(sock); return kNaN; }
    double recv_time = NowMs();
    close(sock);

    // Transmit timestamp: bytes 40-47, NTP fixed-point (seconds since 1900)
    uint32_t sec  = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16)
                  | ((uint32_t)pkt[42] <<  8) |  (uint32_t)pkt[43];
    uint32_t frac = ((uint32_t)pkt[44] << 24) | ((uint32_t)pkt[45] << 16)
                  | ((uint32_t)pkt[46] <<  8) |  (uint32_t)pkt[47];
    double server_ms = (double)sec * 1000.0
                     + (double)frac * 1000.0 / 4294967296.0
                     - (double)kNTPEpochOffsetMs;

    // offset = trueTime - systemTime, evaluated at recv_time
    double rtt_half = (recv_time - send_time) / 2.0;
    return server_ms + rtt_half - recv_time;
}

static int CmpDouble(const void* a, const void* b) {
    double d = *(const double*)a - *(const double*)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static double FetchNTPOffset() {
    double buf[kNTPVerifyCount];
    int n = 0;
    for (int i = 0; i < kNTPVerifyCount; i++) {
        double v = FetchNTPOffsetOnce();
        if (!std::isnan(v)) buf[n++] = v;
    }
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    qsort(buf, (size_t)n, sizeof(double), CmpDouble);
    return buf[n / 2];
}

// ── Offset state rotation ─────────────────────────────────────────────────────
static void SyncTime() {
    double new_offset = FetchNTPOffset();
    if (std::isnan(new_offset)) return;  // NTP unavailable; keep existing state

    double now = NowMs();
    TimeState cur = ReadState();
    TimeState next;

    if (cur.update_time == 0.0 || now >= cur.next_update_time) {
        // Fresh init or stale state: set all three windows to the new offset
        next = { new_offset, now,
                 new_offset, now + kTransitionGapMs,
                 new_offset, now + kTransitionGapMs * 2.0 };
    } else if (now >= cur.update_time) {
        // Advance the window: promote next → current, queue new as future
        next = { cur.offset,      cur.update_time,
                 cur.next_offset, cur.next_update_time,
                 new_offset,      cur.next_update_time + kTransitionGapMs };
    } else {
        // Too early to rotate; just refresh the queued next offset
        next = cur;
        next.next_offset = new_offset;
    }
    WriteState(next);
}

// ── Background thread ─────────────────────────────────────────────────────────
static void* TimeSyncThread(void*) {
    SyncTime();  // immediate first sync
    for (;;) {
        struct timespec ts = { (long)kCheckIntervalSec, 0 };
        nanosleep(&ts, nullptr);
        SyncTime();
    }
    return nullptr;
}

// ── One-time initialization ───────────────────────────────────────────────────
static void InitOnce() {
    // Try POSIX shared memory so all tracing processes share the same offset
    int fd = shm_open(kShmPath, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)sizeof(ShmData)) == 0) {
            void* p = mmap(nullptr, sizeof(ShmData),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) g_shm = static_cast<ShmData*>(p);
        }
        close(fd);
    }
    if (!g_shm) {
        // Fall back to process-local state (no cross-process sync)
        g_use_local = true;
        memset(&g_local_data, 0, sizeof(g_local_data));
    }

    // Signal the JS shim that Date.now is already accurate.
    // trueTimeShim.ts checks globalThis.TRUE_TIME_ALREADY_SHIMMED before shimming.
    // We set the env var as a belt-and-suspenders signal for processes that
    // read it before the first JS tick.
    setenv("NODETRACE_TRUE_TIME", "1", /*overwrite=*/1);

    g_enabled = true;
    pthread_create(&g_thread, nullptr, TimeSyncThread, nullptr);
    pthread_detach(g_thread);
}

// ── Public API ────────────────────────────────────────────────────────────────
void InitTrueTime() {
    pthread_once(&g_once, InitOnce);
}

bool IsTrueTimeEnabled() { return g_enabled; }

// ── Shared interpolation helper ───────────────────────────────────────────────
// Returns the NTP offset (ms) to add to sys, linearly smeared across the
// current transition window.  Returns 0 if not yet synced.
static double ComputeOffset(const TimeState& s, double sys) {
    if (s.update_time == 0.0 || sys < s.last_update_time) return 0.0;
    double from_offset, to_offset, from_time, to_time;
    if (sys < s.update_time) {
        from_offset = s.last_offset;  to_offset = s.offset;
        from_time   = s.last_update_time;  to_time = s.update_time;
    } else if (sys < s.next_update_time) {
        from_offset = s.offset;       to_offset = s.next_offset;
        from_time   = s.update_time;  to_time   = s.next_update_time;
    } else {
        return s.next_offset;
    }
    double duration = to_time - from_time;
    double frac     = duration > 0.0 ? std::min(1.0, (sys - from_time) / duration) : 0.0;
    return from_offset + (to_offset - from_offset) * frac;
}

double GetTrueTimeMs() {
    TimeState s  = ReadState();
    double sys   = NowMs();
    double time  = sys + ComputeOffset(s, sys);

    // Hard monotonic guarantee: never return a value smaller than the previous
    // call on this thread.
    thread_local double last_time      = 0.0;
    thread_local double last_base_time = 0.0;
    if (last_time != 0.0 && time < last_time) {
        double diff = sys - last_base_time;
        time = last_time + (diff > 0.0 ? diff * kMinTimeRate : 0.0);
    }
    last_time      = time;
    last_base_time = sys;
    return time;
}

double GetCurrentOffset() {
    TimeState s = ReadState();
    return ComputeOffset(s, NowMs());
}

double GetTargetOffset() {
    // next_offset is the most recently NTP-queried value and is stored verbatim
    // in shared memory, so all processes reading it get bit-identical doubles.
    return ReadState().next_offset;
}

double ForceResync() {
    SyncTime();  // blocking: NTP query (up to ~9s) then write to shm
    return GetTargetOffset();
}

}  // namespace nodetrace
