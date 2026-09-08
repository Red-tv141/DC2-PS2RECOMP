// ==============================================================================================
// G712 — CPU-CONTENTION CEILING PROBE for the parallel GS front-end
// ==============================================================================================
//
// THE QUESTION. G711 closed performance under exit condition (B) with GS own the pole on 22 of 23
// windows and every *deletion* of a flush stage returning 5-20x less than its timer (Rule 88: each
// stage is preparation whose consumer re-pays what it is denied). The one direction that argument
// does NOT close is OVERLAP: not removing the work, but running independent parts of it on another
// core. G671 measured 45.7 CPU-ms/f over 6 cores with the GPU ~95% idle, i.e. the box is ~36%
// utilised at the observed 21.5 ms/f frame — so there is idle silicon. Whether that idle silicon is
// USABLE is a different claim, and it has never been measured directly on this host.
//
// ⛔ WHY THIS IS NOT G701. G701/G305/G444/G452/G493 all moved the GL BACKEND (or its submit) to
// another thread and measured neutral-to-much-worse; G704's fourth refutation was +3.202 ms/f with
// the arm proven firing, and its autopsy is that THE BLOCKING WAIT IS WHAT YIELDS THE CORE. This
// probe moves nothing and reorders nothing. It ADDS a synthetic CPU load on helper threads and asks
// what that load costs the frame. It is a pure upper bound on contention, measured before any
// architecture is designed against it — the G492 law ("a ceiling costs one run, a design costs a
// phase") applied to parallelism instead of to deletion.
//
// HOW TO READ IT. If N helper threads each running a memory-touching kernel at duty D cost the
// frame far LESS than the CPU they consume, the host has real parallel slack and a pipelined GS
// front-end can convert it. If the frame tracks the added load ~1:1, the direction is dead on
// contention and that is exit condition (B) for the whole parallel arc — a result, not a failure.
//
// ⚠️ THE KERNEL IS DELIBERATELY MEMORY-HEAVY, not a spin loop. Every stage this phase would like to
// overlap (`class`, `verts`, `fbzPack`, texture decode, the `deps` tail) streams over entry lists,
// guest VRAM pages and decode buffers. A pure-ALU ballast would understate contention by missing
// the L3/DRAM term, which on a 6-core i7-8700 with one 12 MB L3 is the term that actually decides
// this. Footprint is settable so the L2-resident and L3/DRAM-resident regimes can be separated.
//
// ⭐ RULE 12b HYGIENE. This is a SEPARATE TRANSLATION UNIT compiled only under -DPS2X_G712_DIAG=ON,
// and it has NO call sites anywhere: the load starts from this file's own static initialiser. G710
// measured +0.362 ms/f pooled from 6 KB of *compile-time-dead* source sitting in
// ps2_gs_rasterizer.cpp, so nothing this phase builds for measurement may touch that TU.
//
// ARM (a diagnostic binary only; the option is OFF in the shipping build):
//   DC2_G712_LOAD=<n>        helper threads to run (default 0 -> no thread is created at all)
//   DC2_G712_LOAD_KB=<kb>    per-thread working set, default 512 (L2-ish). 8192 -> L3. 65536 -> DRAM
//   DC2_G712_LOAD_DUTY=<1..100>  percent of wall each helper spends in the kernel, default 100
//   DC2_G712_LOAD_SLICE_US=<us>  duty period granularity, default 2000
//   DC2_G712_LOAD_STAT=1     print [G712:load] every 10 s (achieved busy fraction + bandwidth)
//
// `[G712:load]` is the ADMISSIBILITY GATE (Rule: assert the lever is armed AND exercised). An arm
// whose achieved busy fraction does not match its requested duty did not run the load it claims,
// and its frame delta means nothing.
// ==============================================================================================

#if defined(PS2X_G712_DIAG)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

uint32_t g712EnvU32(const char *name, uint32_t dflt)
{
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return dflt;
    const unsigned long n = std::strtoul(v, nullptr, 0);
    return static_cast<uint32_t>(n);
}

std::atomic<bool> g_g712Stop{false};
std::atomic<uint64_t> g_g712BusyNs{0};
std::atomic<uint64_t> g_g712WallNs{0};
std::atomic<uint64_t> g_g712Bytes{0};
std::atomic<uint64_t> g_g712Sink{0};

// One pass of the representative kernel over `buf`. Read-modify-write at 64-byte stride: one cache
// line touched per iteration, one dependent add so the compiler cannot vectorise it into something
// unrepresentative, and a running xor that is published to an atomic so nothing is dead.
uint64_t g712Kernel(uint64_t *buf, size_t words, uint64_t seed)
{
    constexpr size_t kStride = 8; // 8 * 8 B = one 64 B line
    uint64_t acc = seed;
    for (size_t i = 0; i < words; i += kStride)
    {
        const uint64_t v = buf[i] + acc;
        buf[i] = v;
        acc ^= (v >> 3);
    }
    return acc;
}

void g712LoadThread(uint32_t index, uint32_t footprintKb, uint32_t duty, uint32_t sliceUs)
{
    const size_t bytes = static_cast<size_t>(footprintKb) * 1024u;
    const size_t words = bytes / sizeof(uint64_t);
    std::vector<uint64_t> buf(words, static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ull + 1u);

    const auto slice = std::chrono::microseconds(sliceUs);
    const auto busySlice = std::chrono::microseconds((sliceUs * duty) / 100u);
    uint64_t acc = index + 1u;
    uint64_t passes = 0u;

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_g712Stop.load(std::memory_order_relaxed))
    {
        const auto sliceStart = std::chrono::steady_clock::now();
        const auto busyUntil = sliceStart + busySlice;
        uint64_t localPasses = 0u;
        while (std::chrono::steady_clock::now() < busyUntil)
        {
            acc = g712Kernel(buf.data(), words, acc);
            ++localPasses;
            if (g_g712Stop.load(std::memory_order_relaxed))
                break;
        }
        const auto busyEnd = std::chrono::steady_clock::now();
        passes += localPasses;
        g_g712BusyNs.fetch_add(static_cast<uint64_t>(
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       busyEnd - sliceStart).count()),
                               std::memory_order_relaxed);
        g_g712Bytes.fetch_add(localPasses * static_cast<uint64_t>(bytes), std::memory_order_relaxed);
        if (duty < 100u)
        {
            const auto sliceEnd = sliceStart + slice;
            if (busyEnd < sliceEnd)
                std::this_thread::sleep_until(sliceEnd);
        }
    }
    g_g712WallNs.fetch_add(static_cast<uint64_t>(
                               std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0).count()),
                           std::memory_order_relaxed);
    g_g712Sink.fetch_add(acc + passes, std::memory_order_relaxed);
}

void g712StatThread(uint32_t threads, uint32_t footprintKb, uint32_t duty)
{
    uint64_t lastBusy = 0u, lastBytes = 0u;
    auto last = std::chrono::steady_clock::now();
    while (!g_g712Stop.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto now = std::chrono::steady_clock::now();
        const uint64_t busy = g_g712BusyNs.load(std::memory_order_relaxed);
        const uint64_t bytes = g_g712Bytes.load(std::memory_order_relaxed);
        const double wallNs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
        if (wallNs <= 0.0)
            continue;
        const double busyFrac = static_cast<double>(busy - lastBusy) / (wallNs * threads);
        const double gbps = static_cast<double>(bytes - lastBytes) / wallNs; // B/ns == GB/s
        std::fprintf(stderr,
                     "[G712:load] threads=%u kb=%u dutyReq=%u%% dutyAch=%.1f%% "
                     "cpuMsPerSec=%.1f bandwidth=%.2fGB/s\n",
                     threads, footprintKb, duty, 100.0 * busyFrac,
                     1000.0 * busyFrac * threads, gbps);
        std::fflush(stderr);
        lastBusy = busy;
        lastBytes = bytes;
        last = now;
    }
}

struct G712LoadHarness
{
    std::vector<std::thread> workers;
    std::thread stat;

    G712LoadHarness()
    {
        const uint32_t n = g712EnvU32("DC2_G712_LOAD", 0u);
        if (n == 0u)
            return; // inert: not one thread is created, nothing below runs
        const uint32_t kb = g712EnvU32("DC2_G712_LOAD_KB", 512u);
        uint32_t duty = g712EnvU32("DC2_G712_LOAD_DUTY", 100u);
        if (duty == 0u || duty > 100u)
            duty = 100u;
        const uint32_t sliceUs = g712EnvU32("DC2_G712_LOAD_SLICE_US", 2000u);
        std::fprintf(stderr,
                     "[G712:load] ARMED threads=%u kb=%u duty=%u%% sliceUs=%u\n",
                     n, kb, duty, sliceUs);
        std::fflush(stderr);
        workers.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            workers.emplace_back(g712LoadThread, i, kb, duty, sliceUs);
        if (g712EnvU32("DC2_G712_LOAD_STAT", 0u) != 0u)
            stat = std::thread(g712StatThread, n, kb, duty);
    }

    ~G712LoadHarness()
    {
        g_g712Stop.store(true, std::memory_order_relaxed);
        for (auto &t : workers)
            if (t.joinable())
                t.join();
        if (stat.joinable())
            stat.join();
    }
};

G712LoadHarness g_g712Harness;

} // namespace

#endif // PS2X_G712_DIAG
