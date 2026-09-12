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

// G726: the G650 affinity map lives in ps2_runtime.cpp's TU. Declared extern at GLOBAL scope here,
// exactly as ps2_gs_rasterizer.cpp:338 and ps2_gif_arbiter.cpp:57 do — an anonymous-namespace
// declaration would give it internal linkage and produce an unresolved external (the G568 linkage
// trap this file's own header comment records for `G712Prep`).
extern void g650PinThread(int role);

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

// ⭐⭐ G726: WHICH CORE THE PREP WORKER RUNS ON. G650 ships HARD affinity (measured -0.775 ms/f on
// `dragon`, -0.578 on `dungeon1`), so every other thread in this process is CONFINED to its own
// core: EE=0, VU1=1, GS=2, GL=3, MAIN=4, and the G713 executor pins role 3 (free under G705's fused
// transport). This pool pinned NOTHING, leaving the scheduler free to place it on the EE or VU1
// core — and under G725 it is a promoted, default-ON thread rather than a bring-up experiment.
//
// The G726 board shows the symptom: on `dragon:tail` GS own more than halved (18.98 -> 7.61) while
// **VU1 busy ROSE 18.35 -> 20.00 with no VU1 code change at all**. A worker that gets slower
// without being modified is sharing a core or being descheduled.
//
// Role 4 (MAIN / host present loop) is the least contended slice under the shipped fused-GL
// topology: it carries no per-kick or per-flush duty, whereas 0/1/2 are the three poles and 3 is
// the executor. Arm with `DC2_G726_PREP_ROLE=4`.
//
// ⛔ DEFAULT IS UNPINNED (-1), AND THAT IS DELIBERATE. This hypothesis is NOT gated: it was
// identified by reading the affinity map, not by measurement. The promoted G726 deliverable
// (SHA 2B76C9EC...) and its 23-window board were both produced with this pool UNPINNED, so an
// unmeasured default-ON pin would mean the shipped source no longer reproduces the board that
// justified shipping it. Gate it first (`DC2_G726_PREP_ROLE=4` vs unset, `dragon`, one binary,
// discarded warm-up), then flip the default.
int g712PrepRole()
{
    static const int s_role = [] {
        if (const char *v = std::getenv("DC2_G726_PREP_ROLE"))
        {
            const int parsed = std::atoi(v);
            return (parsed >= 0 && parsed < 5 /* G650_ROLE_COUNT */) ? parsed : -1;
        }
        return -1; // unpinned: the behaviour the promoted board measured
    }();
    return s_role;
}

void g712WorkerLoop()
{
    // Pinned from INSIDE the worker: g650PinThread acts on GetCurrentThread().
    if (g712PrepRole() >= 0)
        g650PinThread(g712PrepRole());
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
    // ⭐ G725: PROMOTED DEFAULT ON, together with the G713/G721 pipeline it rides on.
    //
    // Mechanism, measured on `dungeon1` (tag g722stat1, whole run): posted=168198 served=168094
    // (99.9%), helped=30733 (18.3%), waited=8065 (4.8%), reject=104, **mismatch=0** with the
    // `DC2_G712_VERIFY` oracle on over 24k batches. After the G724 split census the classify loop
    // inside the flush measures **0.008 ms/f** - i.e. the pass really is gone from the pole thread,
    // which is the entire claim. Timing: G722's pooled -1.529 was INADMISSIBLE (2.212 ms/f control
    // drift, cold first arm), and the defensible warm-pair estimate is **-0.452 ms/f**, which is
    // also what the mechanism predicts. Re-gated on the promoted binary behind a discarded warm-up
    // arm; the rollback below is the control.
    //
    // ⛔ ROLLBACK: DC2_G712_NO_PREPAHEAD=1 (the batch's `class` pass runs inline on the flush
    //    thread, exactly the pre-G712 code, and the worker thread is never started).
    static const bool on = !g712EnvOn("DC2_G712_NO_PREPAHEAD");
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
    // ⭐⭐ G726: PIN THE PREP WORKER. G650 ships HARD affinity (measured -0.775 ms/f on `dragon`,
    // -0.578 on `dungeon1`), so every OTHER thread in this process is confined to its own core:
    // EE=0, VU1=1, GS=2, GL=3, MAIN=4, and the G713 executor pins role 3 (free under G705's fused
    // transport). This pool pinned NOTHING, so the scheduler was free to place it on the EE or VU1
    // core — and it is now a promoted, default-ON thread rather than a bring-up experiment.
    //
    // The G726 board shows why that matters: on `dragon:tail` GS own more than halved
    // (18.98 -> 7.61) while **VU1 busy ROSE 18.35 -> 20.00 with no VU1 code change at all**. A
    // worker that got slower without being modified is being descheduled or sharing a core.
    //
    // Role 4 (MAIN / host present loop) is the least contended slice under the shipped fused-GL
    // topology: it has no per-kick or per-flush duty, whereas 0/1/2 are the three poles and 3 is
    // the executor. `DC2_G726_PREP_ROLE=<n>` overrides (clamped to the declared range);
    // `DC2_G726_PREP_ROLE=-1` restores the unpinned behaviour this board measured.
    // ⚠️ `g650PinThread` pins the CALLING thread (`GetCurrentThread()`), so the pin is applied by
    // each worker at the top of `g712WorkerLoop`, not from here. See g712PrepRole() below.
    std::fprintf(stderr, "[G712:prep] pin role=%d threads=%u\n", g712PrepRole(), n);
    // G725: default ON, so record WHICH decision armed it and keep the legacy positive-arm literal
    // in the image for the `grep -c <flag> <exe>` compiled-in check.
    std::fprintf(stderr, "[G712:prep] ARMED threads=%u hc=%u arm=%s\n", n, hc,
                 g712EnvOn("DC2_G712_PREPAHEAD") ? "explicit" : "default");
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
