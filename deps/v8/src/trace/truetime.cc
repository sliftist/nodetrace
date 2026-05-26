#include "src/trace/truetime.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>

#ifdef _WIN32
// ── Windows platform headers ──────────────────────────────────────────────────
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
// ── POSIX platform headers ────────────────────────────────────────────────────
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <pthread.h>
#  include <sched.h>
#  include <sys/mman.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
#endif

namespace nodetrace {

// ── Constants ─────────────────────────────────────────────────────────────────
static const double   kTransitionGapMs  = 20.0 * 60.0 * 1000.0;  // 20 min
static const double   kCheckIntervalMs  = 5.0 * 60.0 * 1000.0;   // 5 min
static const int      kCheckIntervalSec = 5 * 60;
static const double   kMinTimeRate      = 0.5;
static const int      kNTPVerifyCount   = 3;
static const char     kNTPServer[]      = "time.google.com";
static const int      kNTPPort          = 123;
static const uint64_t kNTPEpochOffsetMs = 2208988800ULL * 1000ULL;

#ifdef _WIN32
static const char kShmName[] = "Local\\nodetrace-timesync";
#else
static const char kShmPath[] = "/nodetrace-timesync";
#endif

// ── Shared memory layout — exactly one cache line (64 bytes) ─────────────────
// version is a seqlock counter: even = consistent, odd = write in progress.
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

// ── Platform-specific helpers ─────────────────────────────────────────────────

// Wall clock: milliseconds since Unix epoch.
static double NowMs() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-ns intervals since 1601-01-01; 116444736000000000 = offset to Unix epoch
    return (double)(u.QuadPart - 116444736000000000ULL) / 10000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static void YieldCpu() {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void SleepSec(int sec) {
#ifdef _WIN32
    Sleep((DWORD)(sec * 1000));
#else
    struct timespec ts = { (long)sec, 0 };
    nanosleep(&ts, nullptr);
#endif
}

static void SetEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, /*overwrite=*/1);
#endif
}

// ── Seqlock atomic helpers ────────────────────────────────────────────────────
// __atomic_* builtins (GCC/Clang) map directly to hardware ops that work on
// MAP_SHARED memory.  On Windows x64 aligned 64-bit loads/stores are atomic
// by the hardware; we add compiler barriers via _ReadBarrier/_WriteBarrier and
// use InterlockedCompareExchange64 / InterlockedIncrement64 for the CAS/add.

static inline uint64_t SeqLoad(volatile uint64_t* p) {
#ifdef _WIN32
    uint64_t v = *p;
    _ReadBarrier();
    return v;
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}

static inline bool SeqCas(volatile uint64_t* p, uint64_t* expected, uint64_t desired) {
#ifdef _WIN32
    uint64_t prev = (uint64_t)InterlockedCompareExchange64(
        (volatile LONGLONG*)p, (LONGLONG)desired, (LONGLONG)*expected);
    if (prev == *expected) return true;
    *expected = prev;
    return false;
#else
    return __atomic_compare_exchange_n(p, expected, desired, /*weak=*/true,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
#endif
}

static inline void SeqFetchAdd(volatile uint64_t* p) {
#ifdef _WIN32
    _WriteBarrier();
    InterlockedIncrement64((volatile LONGLONG*)p);
#else
    __atomic_fetch_add(p, 1, __ATOMIC_RELEASE);
#endif
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
        uint64_t v1 = SeqLoad(&d->version);
        if (v1 & 1) continue;  // write in progress — spin
        s.last_offset      = d->last_offset;
        s.last_update_time = d->last_update_time;
        s.offset           = d->offset;
        s.update_time      = d->update_time;
        s.next_offset      = d->next_offset;
        s.next_update_time = d->next_update_time;
        uint64_t v2 = SeqLoad(&d->version);
        if (v1 == v2) return s;
    }
}

static void WriteState(const TimeState& s) {
    ShmData* d = g_use_local ? &g_local_data : g_shm;
    if (!d) return;
    // CAS even → odd to acquire write lock
    for (;;) {
        uint64_t v = SeqLoad(&d->version);
        if (v & 1) { YieldCpu(); continue; }
        uint64_t expected = v;
        if (SeqCas(&d->version, &expected, v + 1)) break;
    }
    d->last_offset      = s.last_offset;
    d->last_update_time = s.last_update_time;
    d->offset           = s.offset;
    d->update_time      = s.update_time;
    d->next_offset      = s.next_offset;
    d->next_update_time = s.next_update_time;
    SeqFetchAdd(&d->version);  // odd → even
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

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { freeaddrinfo(res); return kNaN; }
    DWORD tv_ms = 3000;  // 3s receive timeout (Windows: DWORD milliseconds)
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { freeaddrinfo(res); return kNaN; }
    struct timeval tv = { 3, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint8_t pkt[48]{};
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3 (client)

    double send_time = NowMs();
    if (sendto(sock, (const char*)pkt, 48, 0, res->ai_addr, (int)res->ai_addrlen) != 48) {
        freeaddrinfo(res);
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return kNaN;
    }
    freeaddrinfo(res);

    if (recv(sock, (char*)pkt, 48, 0) != 48) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return kNaN;
    }
    double recv_time = NowMs();
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    // Transmit timestamp: bytes 40-47, NTP fixed-point (seconds since 1900)
    uint32_t sec  = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16)
                  | ((uint32_t)pkt[42] <<  8) |  (uint32_t)pkt[43];
    uint32_t frac = ((uint32_t)pkt[44] << 24) | ((uint32_t)pkt[45] << 16)
                  | ((uint32_t)pkt[46] <<  8) |  (uint32_t)pkt[47];
    double server_ms = (double)sec * 1000.0
                     + (double)frac * 1000.0 / 4294967296.0
                     - (double)kNTPEpochOffsetMs;

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
    if (std::isnan(new_offset)) return;

    double now = NowMs();
    TimeState cur = ReadState();
    TimeState next;

    if (cur.update_time == 0.0 || now >= cur.next_update_time) {
        next = { new_offset, now,
                 new_offset, now + kTransitionGapMs,
                 new_offset, now + kTransitionGapMs * 2.0 };
    } else if (now >= cur.update_time) {
        next = { cur.offset,      cur.update_time,
                 cur.next_offset, cur.next_update_time,
                 new_offset,      cur.next_update_time + kTransitionGapMs };
    } else {
        next = cur;
        next.next_offset = new_offset;
    }
    WriteState(next);
}

// ── Background thread ─────────────────────────────────────────────────────────
#ifdef _WIN32
static DWORD WINAPI TimeSyncThread(LPVOID) {
#else
static void* TimeSyncThread(void*) {
#endif
    SyncTime();
    for (;;) {
        SleepSec(kCheckIntervalSec);
        SyncTime();
    }
#ifndef _WIN32
    return nullptr;
#endif
}

// ── Shared memory open ────────────────────────────────────────────────────────
static ShmData* OpenSharedMem() {
#ifdef _WIN32
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, (DWORD)sizeof(ShmData), kShmName);
    if (!h) return nullptr;
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmData));
    // Keep h open so the named mapping persists for other processes.
    // We intentionally leak this handle — it lives for the process lifetime.
    if (!p) { CloseHandle(h); return nullptr; }
    return static_cast<ShmData*>(p);
#else
    int fd = shm_open(kShmPath, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, (off_t)sizeof(ShmData)) != 0) { close(fd); return nullptr; }
    void* p = mmap(nullptr, sizeof(ShmData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? nullptr : static_cast<ShmData*>(p);
#endif
}

// ── One-time initialization ───────────────────────────────────────────────────
static void InitOnce() {
#ifdef _WIN32
    // Initialize Winsock before any socket calls.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    g_shm = OpenSharedMem();
    if (!g_shm) {
        g_use_local = true;
        memset(&g_local_data, 0, sizeof(g_local_data));
    }

    SetEnvVar("NODETRACE_TRUE_TIME", "1");

    g_enabled = true;

#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, TimeSyncThread, NULL, 0, NULL);
    if (h) CloseHandle(h);  // detach: thread runs until process exit
#else
    pthread_t t;
    pthread_create(&t, nullptr, TimeSyncThread, nullptr);
    pthread_detach(t);
#endif
}

// ── Once guard ────────────────────────────────────────────────────────────────
#ifdef _WIN32
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK InitOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    InitOnce(); return TRUE;
}
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
#endif

// ── Public API ────────────────────────────────────────────────────────────────
void InitTrueTime() {
#ifdef _WIN32
    InitOnceExecuteOnce(&g_once, InitOnceCallback, nullptr, nullptr);
#else
    pthread_once(&g_once, InitOnce);
#endif
}

bool IsTrueTimeEnabled() { return g_enabled; }

// ── Shared interpolation helper ───────────────────────────────────────────────
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
    return ReadState().next_offset;
}

double ForceResync() {
    SyncTime();
    return GetTargetOffset();
}

}  // namespace nodetrace
