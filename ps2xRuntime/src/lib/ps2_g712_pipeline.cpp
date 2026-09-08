// ==============================================================================================
// G712 — THE PREP-AHEAD TASK POOL (the scheduling half of the parallel GS front end)
// ==============================================================================================
//
// WHAT THIS IS. `g178TryFlushGpu` runs six ordered per-entry passes over a closed G260 batch —
// `class`, `fbz`, `deps`, `tex`, `verts`, `submit`. G711 proved none of them can be DELETED
// (Rule 88: every stage is preparation whose consumer re-pays what it is denied), but two of them,
// `class` and `verts`, are PURE FUNCTIONS OF THE CAPTURED ENTRY LIST:
//
//   * `g178ClassifyEntry(e, st)` reads `e` and two static env gates and nothing else — no VRAM, no
//     residency, no GL. (Verified by reading its body: the only globals it touches are the
//     `g_g178Reject*` atomics, `s_zTestDisabled` and a null hook.)
//   * the vertex/draw builder reads `e` and `s_states[i]`, plus the FBO-source and texture-variant
//     tables — and those two only for the entries the bind pre-pass actually bound.
//
// A batch becomes IMMUTABLE the moment `g260CloseOpenBatch()` moves it into `g_g260Graph`, and the
// census on `lavaboss` measures **37.3 batch closes per frame against 12.6 drains**, i.e. a batch
// waits behind ~3 others before it executes. That wait is dead time on a host G671 measured at 36%
// CPU utilisation. So the work is posted HERE, at the close, and consumed at the flush.
//
// ⛔⛔ WHY THIS IS NOT G701 / G305 / G444 / G452 / G493. Every one of those moved the GL BACKEND or
// its submit onto another thread and measured neutral-to-+3.2 ms/f, and G704's autopsy is that
// **the blocking wait is what yields the core** — an async submit removes the yield and creates
// contention. Nothing here touches GL, the backend, the submit, or the thread that owns the
// context. The OpenGL topology is exactly the G705 fused one, unchanged. What moves is CPU-only
// preparation of a batch that is not yet executing.
//
// ⚠️ WHY A POOL AND NOT `GSRowPool::run`. The pool that already exists is a FORK/JOIN: the caller
// publishes a partition, participates, and blocks at the join. Its dispatch floor is ~0.085 ms
// (G419/G420) and G686 measured the cooperative variant's floor at ~0.28 ms/f. At 30-50 flushes a
// frame a per-flush fork/join costs 2.5-4.2 ms/f — more than every stage it could parallelise put
// together. The only affordable shape is a PERSISTENT worker consuming a queue, where the post is
// an enqueue and the join has usually already happened. That is what this is.
//
// THE HELP-OR-WAIT PROTOCOL. Each job owns a three-state atomic:
//   0 PENDING  -> nobody has started it
//   1 CLAIMED  -> exactly one thread is running it
//   2 DONE     -> results are published (release/acquire)
// Producer and worker both CAS 0->1 before running, so a job is executed exactly once and the
// consumer NEVER blocks on a job the worker has not started: it simply runs it itself, inline, at
// the point the old code would have run it anyway. The only blocking case is "the worker is already
// inside this job", which is bounded by one batch's classify time (~36 us on `lavaboss`).
//
// ARM: `DC2_G712_PREPAHEAD=1` (default OFF while the mechanism is being gated).
//      `DC2_G712_PREP_THREADS=<n>` worker threads, default 1.
//      `DC2_G712_PREP_STAT=1` print `[G712:prep]` every 4000 jobs — the admissibility gate: an arm
//      whose `served` is not ~100% of `posted` did not overlap what it claims to have overlapped.
// ==============================================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

bool g712EnvOn(const char *name)
{
    const char *v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

uint32_t g712EnvU32(const char *name, uint32_t dflt)
{
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return dflt;
    return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
}

using G712PrepJobFn = void (*)(void *job);

constexpr size_t kG712QueueCap = 512;

struct G712Pool
{
    std::mutex mtx;
    std::condition_variable cv;
    void *ring[kG712QueueCap]{};
    size_t head = 0, tail = 0, count = 0;
    bool stop = false;
    G712PrepJobFn fn = nullptr;
    std::vector<std::thread> threads;
};

G712Pool &g712Pool()
{
    static G712Pool p;
    return p;
}

void g712WorkerLoop()
{
    G712Pool &p = g712Pool();
    for (;;)
    {
        void *job = nullptr;
        {
            std::unique_lock<std::mutex> lk(p.mtx);
            p.cv.wait(lk, [&] { return p.stop || p.count != 0; });
            if (p.stop && p.count == 0)
                return;
            job = p.ring[p.head];
            p.head = (p.head + 1u) % kG712QueueCap;
            --p.count;
        }
        if (job && p.fn)
            p.fn(job); // the body CASes 0->1 itself and returns immediately if it loses the race
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public surface. Declared in ps2_gs_rasterizer_parts/g712_prep_types.inc.
// ---------------------------------------------------------------------------------------------

bool g712PrepAheadEnabled()
{
    static const bool on = g712EnvOn("DC2_G712_PREPAHEAD");
    return on;
}

bool g712PrepStatOn()
{
    static const bool on = g712EnvOn("DC2_G712_PREP_STAT");
    return on;
}

// Start the pool once, with the hot TU's job body. Returns false if the pool could not be started
// (in which case every post runs inline and the mechanism degrades to the shipped behaviour).
bool g712PrepPoolStart(G712PrepJobFn fn)
{
    static bool s_started = false;
    static bool s_ok = false;
    if (s_started)
        return s_ok;
    s_started = true;
    if (!g712PrepAheadEnabled() || fn == nullptr)
        return false;
    uint32_t n = g712EnvU32("DC2_G712_PREP_THREADS", 1u);
    if (n < 1u) n = 1u;
    if (n > 4u) n = 4u;
    const unsigned hc = std::thread::hardware_concurrency();
    if (hc != 0u && n + 2u > hc) // never leave the EE and GS threads without a core each
        n = (hc > 2u) ? (hc - 2u) : 1u;
    G712Pool &p = g712Pool();
    p.fn = fn;
    p.threads.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
        p.threads.emplace_back(g712WorkerLoop);
    std::fprintf(stderr, "[G712:prep] ARMED threads=%u hc=%u\n", n, hc);
    std::fflush(stderr);
    s_ok = true;
    return true;
}

// Enqueue a job. NEVER blocks and never runs the job on the caller: a full queue simply drops the
// post, and the consumer's help-or-wait then runs the job inline exactly where the shipped code
// would have. Dropping is therefore behaviour-neutral, not a correctness event.
void g712PrepPoolPost(void *job)
{
    G712Pool &p = g712Pool();
    if (p.threads.empty())
        return;
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        if (p.count == kG712QueueCap)
            return; // full: consumer will run it inline
        p.ring[p.tail] = job;
        p.tail = (p.tail + 1u) % kG712QueueCap;
        ++p.count;
    }
    p.cv.notify_one();
}

void g712PrepPoolStop()
{
    G712Pool &p = g712Pool();
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.stop = true;
    }
    p.cv.notify_all();
    for (auto &t : p.threads)
        if (t.joinable())
            t.join();
    p.threads.clear();
}
