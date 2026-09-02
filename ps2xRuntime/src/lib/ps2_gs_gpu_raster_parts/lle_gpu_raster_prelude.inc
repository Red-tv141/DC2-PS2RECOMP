// ===== G662: split out of lle_gpu_raster_backend.inc (was 10,426 lines) =========================
//
// WHY THIS FILE EXISTS. The backend was one 10.4k-line / 551 KB include whose last 9.4k lines are a
// single `class G178Backend`, so everything BEFORE the class - the env-flag predicates, the G643
// 0x139 kernel data path, the G659 rounding-direction levers and the GLSL source helpers - could
// only be read by scrolling past, or into, the class body.
//
// ⛔ THIS IS A PURE TEXT MOVE AND MUST STAY ONE. This file and its parent are both `#include`d into
// `ps2_gs_gpu_raster.cpp`, consecutively and in the original order, so the preprocessor output is
// byte-identical and the generated code cannot change. That is the ONLY kind of split that is safe
// in a hot TU (rule 12b/12c: this object's code layout is load-bearing - G653 P25 measured ~60
// lines of inert code at +0.484 ms/f, and G662 P4 measured -0.448 ms/f for a 21.6 KB removal).
// If you MOVE a definition across the boundary rather than cutting at it, that guarantee is void.
//
// Contents: backend env-flag predicates - G643 0x139 kernel data path - G659 P18/P19/P22 shade
// rounding - G661 P9 destination-blend arms - shader/program helpers. The class itself, and
// everything after it, stays in lle_gpu_raster_backend.inc.

bool g178EnvEnabled()
{
    static const bool on = g178DefaultOn();
    return on;
}

// G541 is diagnostic-only. Startup must provision compute for either the standalone arm or the
// paired within-process selector; neither form changes the CPU-authoritative VU1/GS result.
bool g541NullComputeRequested()
{
    static const bool on = [] {
        if (g158EnvFlag("DC2_G541_NULL_GPU"))
            return true;
        const char *ab = std::getenv("DC2_G419_AB");
        return ab != nullptr && std::strcmp(ab, "vu1null") == 0;
    }();
    return on;
}

// G542 offline ABI replay is explicit and default-off. It never changes the CPU-authoritative VU
// or GS path; it only provisions compute long enough to validate a saved G541 corpus.
bool g542AbiReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G542_ABI_REPLAY");
    return on;
}

// G543 executes saved VU1 records on the GPU, but is still an offline, default-off oracle. CPU
// state, VU RAM, packets, and the live graphics path remain authoritative.
bool g543SemanticReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G543_VU_REPLAY");
    return on;
}

// G544 chains every saved record through one persistent GPU invocation and ordered packet ring.
// It remains a default-off offline oracle; gameplay still consumes only the CPU executor's state.
bool g544ChainReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G544_CHAIN_REPLAY");
    return on;
}

// G545 executes the real worker stream as a persistent, batched GPU shadow. It is default-off;
// CPU VU state and packets remain authoritative and any mismatch permanently disables the shadow.
bool g545LiveShadowRequested()
{
    static const bool on = g158EnvFlag("DC2_G545_LIVE_SHADOW");
    return on;
}

inline void dc2DumpPpm(const char *filepath, int w, int h, const uint32_t *rgbaPixels, bool flipY = false)
{
    FILE *f = std::fopen(filepath, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * 3u);
    for (int y = 0; y < h; ++y)
    {
        const int srcY = flipY ? (h - 1 - y) : y;
        const uint32_t *row = rgbaPixels + static_cast<size_t>(srcY) * w;
        for (int x = 0; x < w; ++x)
        {
            uint32_t px = row[x];
            rgb[x * 3 + 0] = static_cast<uint8_t>(px & 0xFF);
            rgb[x * 3 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
            rgb[x * 3 + 2] = static_cast<uint8_t>((px >> 16) & 0xFF);
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[SURFACE_DUMP] Saved %s (%dx%d)\n", filepath, w, h);
}

// G566 consumes the same exact persistent executor but publishes generated XGKICK bytes and skips
// the CPU VU interpreter. It is experimental/default-off until every route and visual gate passes.
bool g566VuAuthorityRequested()
{
    static const bool on = g158EnvFlag("DC2_G566_GPU_VU");
    return on;
}

// G606 re-runs G543's proven-exact semantic source at a CONFIGURABLE workgroup width over a real
// captured corpus, times it, and (with DC2_G606_TRACE) publishes per-record VU RAM read/write
// bitmaps for the kick-independence census. Offline, default-off, never authoritative: every prior
// GPU-VU number in this project came from a 1-lane dispatch and this is what re-prices it.
bool g606WideBenchRequested()
{
    static const bool on = g158EnvFlag("DC2_G606_VUBENCH");
    return on;
}

#if defined(PS2X_G665_GPUVU)
// G665: the TRANSLATED GPU VU1 backend. Unlike G607's ceiling bench this executes REAL kicks
// through G543's exact semantics and is graded by G543's own section oracle, so its `bad` column
// is a promotion-grade exactness result rather than a rate probe.
bool g665TranslatedBenchRequested()
{
    static const bool on = g158EnvFlag("DC2_G665_TVU");
    return on;
}
#endif

// G607 prices the OTHER half of the translated-backend question G606 left open: how much cheaper
// a TRANSLATED pair-step is than an interpreted one, at the widths the control-flow profile says
// are actually available (p50 = 5 iterations per loop entry). Ceiling bench, offline, default-off.
bool g607TBenchRequestedFwd()
{
    static const bool on = g158EnvFlag("DC2_G607_TBENCH");
    return on;
}

// G570: exact compute authority for the narrowly validated FBP 0x139 shadow-volume batch.
// Provision the worker for either the candidate or its full-surface CPU oracle.
bool g570Shadow139Requested()
{
    // G571 promotion: provision exact 0x139 compute by default.  The legacy arm remains harmless;
    // DC2_G570_NO_GPU_139=1 is the single backend/front-end rollback.
    static const bool on = !g158EnvFlag("DC2_G570_NO_GPU_139");
    return on;
}

// G629: the exact CPU-band-replay compute kernel. Provisioned only when armed, because it
// allocates seven images sized to the largest batch the route offers.
// G629 exclusive split (default-off): where the per-batch submit time actually goes. The payoff
// gate measured the whole backend at +1.411 ms/f WORSE than the CPU band replay against a
// -4.93 ms/f deletion ceiling, so the only question that matters for the next phase is which of
// upload / dispatch / readback owns the 1.88 ms/batch — asserting it would be `g527_lineclip`.
bool g629SplitOn()
{
    static const bool on = g158EnvFlag("DC2_G629_SPLIT");
    return on;
}

bool g629BackendRequested()
{
    static const bool on = (g158EnvFlag("DC2_G629_GPU") || g158EnvFlag("DC2_G630_RESIDENT")) &&
                           !g158EnvFlag("DC2_G629_NO_GPU") &&
                           !g158EnvFlag("DC2_G630_NO_RESIDENT");
    return on;
}

// G571: diagnostic-only exclusive split for the G570 synchronous compute job.  This remains
// default-off because the extra clocks and stderr traffic perturb the motion window it measures.
bool g571ShadowSplitOn()
{
    static const bool on = g158EnvFlag("DC2_G571_SPLIT");
    return on;
}

// G641 (diagnosis only, default OFF): `[G571:gpuwork] read=2.978` is a FUSED lap — the compute's
// execution, the 1 MiB transfer and the sync stall are all charged to one `glGetTexImage`. Nothing
// can be priced off it: if the 2.98 ms is mostly the shader running, narrowing the transfer buys
// nothing. This arms a `glFinish()` between the memory barrier and the read, which changes no
// result, no GL state and no command order (G495's rule) but moves the compute's execution out of
// the read lap and into its own. Never valid in a timing run.
bool g641ShadowSplitOn()
{
    static const bool on = g158EnvFlag("DC2_G641_SHSPLIT");
    return on;
}

// ⛔ ===== G647: the producer-surface sampler-parameter cache — MEASURED NULL, DEFAULT-OFF =====
// The mechanism works exactly as designed: `[G647:srcparam]` on `dragon` reports **98.8–99.3% of
// the four `glTexParameteri` calls per `srcFbp` draw skipped**, ~2,970 GL calls per frame deleted
// over 11.9 M observed. The PAYOFF is not there. Two order-balanced A B B A gates on `dragon`'s
// static tail produced three readable blocks (the fourth was drift-dominated: its two control arms
// disagreed by 3.1 ms/f) and all three read **+0.12 to +0.31 ms/f**, i.e. no better than control.
//
// ⭐ THE LAW: on this backend the render bucket is NOT command-rate-bound. G496 already deduped
// every other state write, G640 priced the whole render-batch GL bucket at ≈1.7 ms/f for 47
// batches and 14,526 draws (≈120 ns/draw — already the driver's floor), and a redundant
// `glTexParameteri` whose value is unchanged costs essentially nothing. Deleting GL CALLS from this
// loop cannot buy frame time; only deleting GPU work or transfers can.
//
// Retained as its own probe: DC2_G647_SRCPARAM=1 arms it, DC2_G647_STAT=1 prints the census.
// File-scope const, not a function-local static: this is read once per srcFbp draw.
static const bool s_g647SrcParamOn = (std::getenv("DC2_G647_SRCPARAM") != nullptr);
inline bool g647SrcParamOn() { return s_g647SrcParamOn; }
static const bool s_g647StatOn = (std::getenv("DC2_G647_STAT") != nullptr);
static std::atomic<uint64_t> g_g647Issued{0ull}, g_g647Skipped{0ull};

inline void g647NoteParam(uint32_t issued, uint32_t skipped)
{
    if (!s_g647StatOn)
        return;
    const uint64_t i = g_g647Issued.fetch_add(issued, std::memory_order_relaxed) + issued;
    const uint64_t s = g_g647Skipped.fetch_add(skipped, std::memory_order_relaxed) + skipped;
    if (((i + s) % 2000000ull) < 4ull)
        std::fprintf(stderr, "[G647:srcparam] issued=%llu skipped=%llu saved=%.1f%%\n",
                     static_cast<unsigned long long>(i), static_cast<unsigned long long>(s),
                     (i + s) ? 100.0 * static_cast<double>(s) / static_cast<double>(i + s) : 0.0);
}

// ===== G643: the 0x139 kernel's DATA PATH (default-ON) =====
// `[G641:sh] comp = 1.57 ms/f` is the largest single item left in the shadow chain and the second
// largest in the whole GL worker. Its inner loop fetches up to 26 words per triangle reference, and
// every one of them goes through `imageLoad` on an `r32ui` `image2D` — a texel path that pays
// coordinate decomposition (`i & 1023`, `i >> 10`), image-format plumbing and the texture cache
// rather than the buffer cache. The payload arrays are plain flat `uint` vectors, so `std430`
// SSBOs describe them exactly and index them with the linear index the producer already computes.
// ⚠️ `precise float` stays: it is the exactness contract with the CPU kernel, and the SSBO arm
// changes ONLY where the four accessor functions read from.
// Rollback: DC2_G643_NO_SSBO=1.
bool g643SsboOn()
{
    static const bool on = !g158EnvFlag("DC2_G643_NO_SSBO");
    return on;
}

bool g652RingBenchRequestedFwd()
{
    static const bool on = g158EnvFlag("DC2_G652_RINGBENCH");
    return on;
}

// G652 P10: rotate each payload through three non-overlapping ranges. Retained as an opt-in
// prototype: the paired dragon gate was +0.941 ms/f slower raw and +0.243 ms/f slower by held
// blocks (t=1.97), so extra capacity/range binds cost more than any removed upload serialization.
bool g652SsboRingOn()
{
    const int arm = g652SsboRingArm();
    if (arm >= 0)
        return arm != 0;
    static const bool on = g158EnvFlag("DC2_G652_SSBORING") &&
                           !g158EnvFlag("DC2_G652_NO_SSBORING");
    return on;
}

bool g652SsboRingStatOn()
{
    static const bool on = g158EnvFlag("DC2_G652_SSBORING_STAT");
    return on;
}

// G324 diagnostics mirror the front-end armed/census split. They time only the existing logical
// composite and T8-view QueueItems; default runs pay cached-boolean branches only.
bool g324BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G324_CENSUS");
    return on;
}
bool g324BackendArmed()
{
    static const bool on = g324BackendCensusOn() || g158EnvFlag("DC2_G324_EMPTY");
    return on;
}

// G332 (diagnosis only, default OFF): clean per-type GS-backend GL-busy census. The backend is a
// single worker thread and (async default-off) synchronous, so backend GL busy over a frame ≈ the
// GS worker's fut.get() backend-wait. Timing ONE choke point (the worker loop) classified by item
// type avoids per-draw self-cost (G312/G316). Buckets: 0=render(submit) 1=readback 2=copy 3=other.
std::atomic<uint64_t> g_g332BackendNs[4] = {};
std::atomic<uint64_t> g_g332BackendCount[4] = {};
bool g332BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G332_CENSUS");
    return on;
}
bool g332BackendArmed()
{
    static const bool on = g332BackendCensusOn() || g158EnvFlag("DC2_G332_EMPTY");
    return on;
}
// G453 (diagnosis only, default OFF): split G332's broad backend-other class into the four
// directly-dispatched operation families. One timestamp at the existing worker choke point avoids
// perturbing the GL bodies. Buckets: 0=T8 view, 1=CT24 view, 2=composite, 3=color upload.
std::atomic<uint64_t> g_g453BackendOtherNs[4] = {};
std::atomic<uint64_t> g_g453BackendOtherCount[4] = {};
bool g453BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G453_CENSUS");
    return on;
}
// G447 (diagnosis only, default OFF, DC2_G447_EDGE=1): time the GS worker's blocking wait at EVERY
// backend entry point, classified by item type. G332 above times the same work from INSIDE the GL
// worker; this times it from the caller's side, so the difference is the promise/future round-trip
// and the queue latency. Counted only on the GS worker thread — the EE thread reaches several of
// these at the frame barrier and must not be pooled in.
struct G447WaitScope
{
    std::chrono::steady_clock::time_point t0;
    int edge;
    bool on;
    explicit G447WaitScope(int e)
        : edge(e), on(g447EdgeOn() && g447OnGsWorker())
    {
        if (on)
            t0 = std::chrono::steady_clock::now();
    }
    ~G447WaitScope()
    {
        if (on)
            g447NoteWait(edge, static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()));
    }
};

// G447 LEVER — spin-then-block on the backend round trip (default OFF, DC2_G447_SPIN_US=<us>, or
// the paired within-process arm DC2_G419_AB=spinwait).
//
// G447's edge census measured the GS worker (the pole, sensitivity 0.98) blocked 16.7 ms/f, ALL of
// it in these round trips, over ~74 backend items/frame — while [G332:gsw] times the GL worker's own
// work at only 13.0-13.7 ms/f. The ~2.7 ms/f difference is the promise/future handoff: the caller
// sleeps, the kernel must schedule it back on when set_value lands, and the item is typically only
// ~130-200 us of GL work. Spinning on a plain release/acquire flag for a bounded budget removes the
// caller-side wake for every item that completes inside the budget.
//
// Exactness: the result still comes from the future and the GL command stream is untouched. The only
// change is HOW the caller waits. The cost is CPU burn on a thread that is otherwise blocked — which
// is why it is measured, not assumed.
// PROMOTED default-ON (G447): DC2_G419_AB=spinwait measured −1.23…−1.56 ms/f standalone
// (t = −4.9…−8.7, 4/4 windows favouring) and −0.44 ms/f marginal over the worker-side spin. The win
// saturates by 120 us of budget, which is the tell that what it removes is one thread wake, not
// work. Rollback: DC2_G447_NO_SPIN=1. Budget override: DC2_G447_SPIN_US=<us>.
long long g447SpinBudgetUs()
{
    static const long long us = [] {
        const char *v = std::getenv("DC2_G447_SPIN_US");
        const long long n = (v != nullptr) ? std::atoll(v) : 0LL;
        return (n > 0LL && n <= 5000LL) ? n : 400LL;
    }();
    return us;
}

bool g447SpinActive()
{
    const int arm = g447SpinArm(); // -1 unless DC2_G419_AB=spinwait
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G447_NO_SPIN") != nullptr);
    return !off;
}

// The SUBMIT direction of the same round trip: the GL worker is asleep when an item is queued, so
// the item does not start until the kernel schedules that thread. Separate lever id because it is a
// separate architectural decision (which thread burns CPU to avoid a wake), measured separately.
long long g447WorkerSpinBudgetUs()
{
    static const long long us = [] {
        const char *v = std::getenv("DC2_G447_WSPIN_US");
        const long long n = (v != nullptr) ? std::atoll(v) : 0LL;
        return (n > 0LL && n <= 20000LL) ? n : 0LL;
    }();
    return us > 0LL ? us : 400LL;
}

// PROMOTED default-ON (G447): −0.88…−0.95 ms/f standalone (t = −4.55, 4/4 favouring) and −0.55 ms/f
// marginal over the caller-side spin. Unlike the caller spin this one needs the FULL budget — at
// 150 us its marginal value collapses to −0.20 (t = −0.97), because the mean gap between backend
// items on this route is ~430 us. Rollback: DC2_G447_NO_WSPIN=1.
bool g447WorkerSpinActive()
{
    const int arm = g447WorkerSpinArm(); // -1 unless DC2_G419_AB=wspin
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G447_NO_WSPIN") != nullptr);
    return !off;
}

// G471 CLOSED the shape of these two spins; do not re-open it.
//
// Both loops are "load the flag, read steady_clock::now(), PAUSE" — one QueryPerformanceCounter per
// PAUSE — and that looked like pure waste: G471's PC sampler on the GS worker measured the shipped
// default spending ~18% of the thread's whole span inside the spin bodies and a further ~9% in
// RtlQueryPerformanceCounter plus the CRT's QPC wrappers, and rolling BOTH spins back
// (DC2_G447_NO_SPIN=1 + DC2_G447_NO_WSPIN=1) removed both buckets entirely.
//
// It is not waste. Polling the flag 32x between clock reads measured **+0.18 ms/f (t = +3.51, 0/7
// windows favouring candidate)** on the paired MAP-0 gate: removing the clock makes the loop
// tighter, so it hammers the shared completion line ~30x more often, and that coherence traffic
// costs more than the clock. Inverting it — one flag load per 16 PAUSEs, backing BOTH rates off —
// recovered the loss but only to neutral (−0.05 ms/f, t = −0.08, 3/7 windows). The clock read is
// doing the job of a backoff, and the loop is at a local optimum in both directions. Neither arm was
// promoted and both were removed; the ~2.4 ms/f of QPC inside these spins is NOT recoverable frame
// time. See plans/phase-G471-fix-log.md.
// G491 — DO NOT SPIN ON THE READBACK EDGE.
//
// The spin above is a flat 400 us budget on EVERY backend round trip, promoted by G447 on edges
// whose GL work is "typically only ~130-200 us". `[G447:edge]` prices backend-readcolor at
// 3.2-3.4 ms/f over SIX calls, i.e. ~545 us each: the readback wait CANNOT complete inside the
// budget, so every one of those six spins burns its full 400 us and then blocks anyway. That is
// ~2.3 ms/f of a core spent on the pole thread with no possible return, and `[G446:gsprof]` sees
// it directly — 17.66% of the whole GS-worker span sits on one instruction inside this loop.
//
// Exactness: identical to g447WaitFuture with the spin disabled. The result still comes from the
// future; only the caller's wait strategy changes. Kept as a SEPARATE function so the control
// arm's call shape stays byte-identical (17a law 17 — the lever-OFF path IS the control arm).
//
// Levers: DC2_G419_AB=rbspin (paired gate), DC2_G491_NO_RB_SPIN=1 (standalone arm),
// DC2_G491_RB_SPIN=1 (force the historical spin back on).
bool g491RbNoSpinActive()
{
    const int arm = g491RbSpinArm(); // -1 unless DC2_G419_AB=rbspin
    if (arm >= 0)
        return arm == 1; // candidate arm = NO spin on this edge
    static const bool on = (std::getenv("DC2_G491_NO_RB_SPIN") != nullptr) &&
                           (std::getenv("DC2_G491_RB_SPIN") == nullptr);
    return on;
}

bool g491WaitFutureNoSpin(std::future<bool> &fut)
{
    try
    {
        return fut.get();
    }
    catch (...)
    {
        return false;
    }
}

bool g447WaitFuture(std::future<bool> &fut, std::atomic<uint32_t> &done)
{
    if (g447SpinActive())
    {
        static const long long budgetUs =
            g447SpinBudgetUs() > 0LL ? g447SpinBudgetUs() : 400LL; // A/B default
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::microseconds(budgetUs);
        while (done.load(std::memory_order_acquire) == 0u)
        {
            if (std::chrono::steady_clock::now() >= until)
                break;
            YieldProcessor();
        }
    }
    try
    {
        return fut.get();
    }
    catch (...)
    {
        return false;
    }
}

struct SyncWait
{
    std::atomic<uint32_t> done{0u};
    std::shared_ptr<std::promise<bool>> promise;
    std::future<bool> future;

    SyncWait() : promise(std::make_shared<std::promise<bool>>()),
                 future(promise->get_future()) {}

    bool wait()
    {
        return g447WaitFuture(future, done);
    }
};

// NOTE: this whole .inc is inside the anonymous namespace opened in persistent_t8_decoder.inc
// (closed at the end of this file). The cross-TU accessor g332_backend_snapshot() is therefore
// defined AFTER that close (see bottom of file) so it keeps external linkage.

// G256/G258 diagnostic only: exact PS2 fragment arithmetic for the dependency-preserving G255
// RTT path.  G258 found this backend performance-neutral after parity, so do not expose it as a
// standalone behavior arm: require the CPU shadow oracle whenever it is requested.
bool g256ExactRttOn()
{
    static const bool on = g158EnvFlag("DC2_G256_EXACT_RTT") &&
                           g158EnvFlag("DC2_G255_VERIFY");
    return on;
}

// G584 (default ON, roll back with DC2_G584_NO_EXACT_BLENDA=1): the G286/G287 exact-sampler
// branch is the only fragment path that returns before the outA blend-factor encoding, so a
// BLENDED draw on it weighted by As/255 instead of the GS As/128. Two coupled halves, one flag:
//   1. uMode bit 15 makes that branch emit the same encoding the legacy branch does;
//   2. the raw-alpha two-pass (which then supplies the unblended framebuffer A byte) is extended
//      to the transient targets that already assert G178Batch::rttRawAlpha.
// Both must move together — encoding alone would store the doubled A, the two-pass alone would
// still blend with the raw weight.
bool g584ExactBlendAOn()
{
    static const bool on = !g158EnvFlag("DC2_G584_NO_EXACT_BLENDA");
    return on;
}

// G257 (default OFF): prove image-store, barrier, coverage, and direct texture readback on the
// actual persistent G178 worker context without touching a guest batch.
bool g257ImageStoreTestOn()
{
    static const bool on = g158EnvFlag("DC2_G257_IMAGESTORE_TEST");
    return on;
}

// G258 (default OFF): synchronously read one fbp=0x146 destination pixel after selected
// exact-shader draws.  This is a localization probe only; never enable it for timing.
bool g258TraceOn()
{
    static const bool on = g158EnvFlag("DC2_G258_TRACE");
    return on;
}

bool g281T8ViewRequested()
{
    static const bool g281 =
        (g158EnvFlag("DC2_G281_T8_VIEW") ||
         (g158EnvFlag("DC2_G282_TIGHT_ROWS") &&
          !g158EnvFlag("DC2_G282_NO_TIGHT_ROWS") &&
          !g158EnvDisabled("DC2_G282_TIGHT_ROWS"))) &&
                           !g158EnvFlag("DC2_G281_NO_T8_VIEW") &&
                           !g158EnvDisabled("DC2_G281_T8_VIEW") &&
                           !g158EnvFlag("DC2_G280_NO_ALIAS_VIEW");
    static const bool g310 =
        !g158EnvFlag("DC2_G310_NO_LOGICAL") &&
        !g158EnvDisabled("DC2_G310_LOGICAL") &&
        !g158EnvFlag("DC2_G310_NO_T8_VIEW") &&
        !g158EnvDisabled("DC2_G310_T8_VIEW");
    return g281 || g310;
}

bool g309Requested()
{
    static const bool g309 =
        !g158EnvFlag("DC2_G309_NO_MULTI_SOURCE") &&
        !g158EnvDisabled("DC2_G309_MULTI_SOURCE") &&
        (g158EnvFlag("DC2_G309_MULTI_SOURCE") ||
         g158EnvFlag("DC2_G309_VERIFY"));
    // G310 reuses the exact G309 atlas-composition program to populate one ordinary
    // persistent texture. Its front-end arm is independent, so request shader setup here
    // without enabling G309's direct multi-source display path.
    static const bool g310 =
        !g158EnvFlag("DC2_G310_NO_LOGICAL") &&
        !g158EnvDisabled("DC2_G310_LOGICAL");
    return g309 || g310;
}

// G338: the resident CT24-alpha view program is needed when the CT24 display-composite consumer
// of a resident CT32 FBO is being retired. G352 PROMOTION (2026-07-23): DEFAULT-ON (accept-floor
// close of arc-total-closure); rollback DC2_G338_NO_CT24_VIEW=1 or DC2_G338_CT24_VIEW=0. Mirrors
// the front-end g338Ct24ViewOn() gate so the program is built iff the consumer is retired.
bool g338Ct24ViewRequested()
{
    static const bool on = !g158EnvFlag("DC2_G338_NO_CT24_VIEW") &&
                           !g158EnvDisabled("DC2_G338_CT24_VIEW");
    return on;
}

static constexpr uint32_t kG289ZeroFbpToken = 0xfffffffdu;
static constexpr uint32_t kG289AliasCt32Token = 0xfffffffcu;
static constexpr uint32_t kG289AliasFbp = 0xfffffffbu;
static constexpr uint32_t kG289AliasCt24Token = 0xfffffffau;
static constexpr uint32_t kG289WorkCt24Token = 0xfffffff9u;
static constexpr uint32_t kG289WorkCt32Token = 0xfffffff8u;
static constexpr uint32_t kG309CompositeToken = 0xfffffff7u;
static constexpr uint32_t kG309CompositeFbp = 0xfffffff6u;
// G556/G557: G310's page map stores the resident-target index plus one. Keep the backend's
// sampler ordering identical to kG261Fbp so appended opt-in targets can participate in the
// exact logical-atlas composite without forcing their FBOs to exist in unrelated scenes.
static constexpr uint32_t kG309SourceCount = 8u;
static constexpr uint32_t kG309Sources[kG309SourceCount] = {
    0x139u, 0x13cu, 0x143u, 0x146u, 0x155u, 0x159u, 0x181u, 0x13au
};
static constexpr uint32_t kG309PageMapUnit = kG309SourceCount;
static constexpr uint32_t kG309BaseUnit = kG309SourceCount + 1u;
// G310: one persistent 512x512 logical CT32 backing for the physical 0x2720/tbw=8 atlas.
// It is an ordinary private FBO source (not a shader token): producer-time page copies and
// guest-authoritative page uploads keep it current, so the display draw performs one normal
// texture sample. Kept outside the public LLE header by cpp-to-cpp bridges below.
static constexpr uint32_t kG310LogicalFbp = 0xfffffff5u;
static constexpr uint32_t kG310LogicalCt24Token = 0xfffffff4u;
// G349 (closure core, color side): the 0x139 display-strip residency-teardown root. [G349:fbolife]
// confirmed 0x139's FBO is destroyed+recreated ~910x/run (ONLY 0x139) because m_fbos is keyed by
// fbp ALONE while 0x139 is written at BOTH fbw=8 (512-wide display strip) and fbw=2 (128-wide narrow
// population, G347 w2=22%). Each width flip clears the other's FBO → the snapInv 100% FBO-read-fail.
// De-collide: route the narrow (w!=512) 0x139 population to a private sibling FBO key so the 512-wide
// display FBO PERSISTS across frames. This is the substrate for the G350 front-end upload-skip; the
// shadow oracle (DC2_G347_SHADOW) eq% on the now-persistent wide FBO is that skip's GO/NO-GO signal.
// Default-off (DC2_G349_DECOL=1; DC2_G349_NO_DECOL=1 wins); a DC2_G345_CLOSURE stack member. VRAM
// stays the coherence medium (the narrow batch still reads back to VRAM), so composition is exact by
// construction; verified visually. Only the three (w,h)-checked FBO accessors are remapped — every
// other consumer reads 0x139 at 512 (the wide display FBO), unaffected.
static constexpr uint32_t kG349Narrow139 = 0xfffffff3u;
static bool g349DecolOn()
{
    static const bool on = g158EnvFlag("DC2_G349_DECOL") && !g158EnvDisabled("DC2_G349_DECOL") &&
                           !g158EnvFlag("DC2_G349_NO_DECOL");
    return on;
}

// G603: which GS blend modes need GL_FUNC_REVERSE_SUBTRACT rather than GL_FUNC_ADD. The equation is
// PERSISTENT GL state and is the one blend term the G560 dual-source path cannot carry per
// primitive, so it is also the term every run splitter below must group by. Mode 4 is `Cd - Cs`
// (FIX=128); mode 6 is `Cd - Cs*As/128`, the same equation with the ordinary GL_SRC_ALPHA factor.
// Keeping this in ONE predicate is the point: the splitters and the two glBlendEquationSeparate
// call sites previously spelled `blend == 4` in five separate places.
// G604: the set of G527-discovered targets admitted under the non-persistent transient contract,
// published by ps2_gs_rasterizer.cpp at TRUE global scope (every .inc in that TU is inside an
// anonymous namespace, so the predicate itself has internal linkage and cannot be called from
// here). This keeps the raw-alpha rule below asking for the PROPERTY instead of carrying a second
// hardcoded address list that would drift from the front-end's.
// (the extern itself lives in ps2_gs_gpu_raster.cpp, ABOVE this TU's anonymous namespace)
static inline bool g604RawAlphaTarget(uint32_t fbp)
{
    return fbp < 512u &&
           (g_g604RawAlphaFbpBits[(fbp >> 6) & 7u].load(std::memory_order_relaxed) &
            (1ull << (fbp & 63u))) != 0ull;
}

static inline bool g603RevSubBlend(uint32_t blend)
{
    return blend == 4u || blend == 6u;
}

// ===== G659 P19 — the `ridepod` discovered-target SHADE ROUNDING DIRECTION ======================
// G659's depth-bucketed census found the 0x141/0x142 value error is FLAT in write depth: +0.732 /
// +0.716 / +0.677 / +0.720 / +0.733 mean green LSB at depths 1/2/3/4/5-8. A destination-history
// mechanism accumulates with depth (the display path's climbs +0.015 -> +1.388 over the same
// range); a constant offset is a PER-WRITE SOURCE error. The dominant family is untextured
// (`prim=4/6 tme=0`), so its whole value pipeline is: interpolate the Gouraud vertex colour, fog,
// blend, store. And the two arms disagree at the first step:
//
//   CPU (g609_tri_scan.inc): shadeR = clampU8(static_cast<int>(v0.r*w0 + v1.r*w1 + v2.r*w2))
//                            -> the int cast TRUNCATES
//   GPU (this shader):       ivec4 vc = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255)
//                            -> ROUNDS, i.e. +0.5 LSB on average, biased positive
//
// +0.5 LSB per channel per write, positive, depth-independent, is exactly the measured signature.
// Bit 65536 asks the shader to truncate like the CPU, and it is scoped to the G604-discovered
// transient targets so the display path (where G658 P6 measured truncation as a REGRESSION on the
// textured mode-6 family) is untouched. Different family, different measured cause.
//
// ⛔ Read by the BACKEND ONLY, never by g178ClassifyEntry — a flag the classifier reads changes
// batching and admission and is therefore not a value probe at all (G657 measurement law 9).
static bool g659RttTruncOn()
{
    static const bool on = g158EnvFlag("DC2_G659_RTT_TRUNC") &&
                           !g158EnvFlag("DC2_G659_NO_RTT_TRUNC");
    return on;
}

// ===== G659 P18 — the `dungeon1` FBW=2 TEXTURED-SPRITE tap =====================================
// The same census cleared every other candidate for this family: the address probe never fired (no
// untouched pixel diverged, so CPU and GPU derive the narrow FBW=2 addresses identically), `zbad`
// is 0, and coverage divergence is 11 CPU-only / 0 GPU-only pixels out of 4,610,581. What is left
// is a bounded value difference on `prim=6 tme=1` sprites with max channel delta 18-27 and a
// SYMMETRIC signed mean of +0.005 / -0.010 — i.e. a filter/quantisation difference, not a bias.
// `mode |= 4096` routes the draw through the exact 12.4 integer tap with GS's 4-bit bilinear
// weights, which is the CPU sampler's own arithmetic.
//
// ⚠️ G604 measured this same bit as NULL on the mode-6 DISPLAY family (27.72% -> 28.27%). That is
// not evidence about this one: that family is PSMT8/MODULATE on a 512-wide display target and this
// one is a 128-wide discovered target whose error is 10x smaller in magnitude.
static bool g659Fbw2ExactOn()
{
    static const bool on = g158EnvFlag("DC2_G659_FBW2_EXACT") &&
                           !g158EnvFlag("DC2_G659_NO_FBW2_EXACT");
    return on;
}

// ===== G659 P22 — the SAME rounding defect on the WHOLE GPU path ================================
// The census that named P19's mechanism also measured it on the display batches the shipped default
// ALREADY admits, and the signature is identical: `arm=NOSUBORD` mean signed green delta is
// +0.295 / +0.602 / +0.816 at ordinary write depths 1/2/3 on `s03` and +0.263 / +0.469 on
// `lavaboss` -- positive, i.e. the GPU stores high, i.e. round-vs-truncate. That population carries
// 7.2-22.0% of written pixels off by more than 1 LSB with a max channel delta of 255, and it is the
// floor every admission gate in this project has been read against for six phases.
//
// ⚠️ G658 P6 measured "truncate the shade" as a REGRESSION (4,441,305 -> 4,502,262) and this is NOT
// a re-run of it. That arm applied truncation to the mode-6 draws ALONE, leaving the ordinary draws
// sharing those pixels on round-to-nearest -- it made one batch internally inconsistent. Applying it
// to EVERY draw is a different experiment and has never been run.
// ⭐ PROMOTED TO DEFAULT-ON (G659). Rollback: DC2_G659_NO_SHADE_TRUNC=1.
// Measured: display-path `bigRgb` on `s03` 2.60079% -> 1.70653% at an identical 654,311,424-pixel
// denominator (-34.4%), and the `ridepod` discovered-target family 2.93053% -> 0.64917% (-77.8%),
// with `zbad` unchanged in both. Cost: `dragon:tail` A/B/B/A block deltas -0.051 / -0.088 ms/f
// against a control drift of 0.025 -- free. Pixel effect on the `ridepod` opening: meanDiff 0.999
// levels, 20 of 3,264 tiles over 20, visually indistinguishable, no structural change.
static bool g659ShadeTruncOn()
{
    static const bool on = !g158EnvFlag("DC2_G659_NO_SHADE_TRUNC");
    return on;
}

#if defined(PS2X_G658_DIAG)
// GPU half of the process-constant stage selector. Kept local to this TU so no standard header or
// hot cross-TU call boundary is introduced.
static int g658Mode6StageGpu()
{
    static const int stage = []() {
        const char *value = std::getenv("DC2_G658_STAGE");
        const int parsed = value ? std::atoi(value) : 0;
        return parsed >= 0 && parsed <= 7 ? parsed : 0;
    }();
    return stage;
}
#else
static constexpr int g658Mode6StageGpu() { return 0; }
#endif

// G560: adjacent draws whose complete raster state differs only by GS blend mode can share one
// ordered glDrawArrays call.  Flat G178Vtx draw state and a dual-source fragment output supply the
// per-primitive shader inputs and source/destination factors.  Default-off until the route and
// exact-output gates are complete; DC2_G560_NO_MULTI_DRAW is the one-flag rollback.
#if defined(PS2X_G684_LEGACY_G560_RUNS)
static bool g560MultiDrawRequested()
{
    static const bool on = g158EnvFlag("DC2_G560_MULTI_DRAW") &&
                           !g158EnvFlag("DC2_G560_NO_MULTI_DRAW");
    return on;
}
#else
static constexpr bool g560MultiDrawRequested() { return false; }
#endif

// G624: rollback for the flip-aware G406/G407 V rounding on `srcFbp`-bound draws. Namespace scope,
// not a function-local `static const bool` — G619 measured that form as a per-access TLS guard on
// MSVC, and this predicate sits in the per-draw submit loop.
static const bool s_g624NoUvFlip = g158EnvFlag("DC2_G624_NO_UVFLIP");

// G661 P9 — EXACT PS2 DESTINATION-SIDE BLEND ROUNDING.
// Namespace scope for the same reason as s_g624NoUvFlip above: this predicate is read inside the
// per-draw submit loop and a function-local `static const bool` compiles to a per-access TLS guard
// on MSVC (G619).
//
// ⭐ PROMOTED TO DEFAULT-ON IN G662 P1 — but only its EXACT half. Rollback DC2_G661_NO_BLEND_FLOOR=1.
//
// G661 built the lever as two mechanisms with different certainty and correctly refused to promote
// the pair on one flag. G662 gated them SEPARATELY on `dragon:tail`, one binary, env arms only:
//
//   fold + bias (the whole lever)  frame +0.109 ms/f, blocks +0.131 / +0.086 (SAME sign),
//                                  control drift 0.017  -> a REAL, consistently-signed COST
//   fold ONLY   (DC2_G661_NO_FLOOR_BIAS=1)
//                                  frame -0.129 ms/f, blocks -0.300 / +0.043 (MIXED sign),
//                                  control drift 0.256  -> a measured NULL, i.e. FREE
//
// and G661's own oracle already separated them on exactness: the exact fold is "a strict
// improvement on every bucket and every metric" (bigRgb 4.739% -> 3.031%, meanAbsG 0.2262 ->
// 0.1916, no bucket regressing on any of the three), whereas the bias buys a bigger bigRgb drop
// (-> 1.425%) by TRADING many +1/+2 pixels for fewer -1 pixels — meanAbsG RISES at d1 and d5-8 —
// and its signed overshoot has no established cause (the float-combine hypothesis was built, armed
// as DC2_G661_SRC_QUANT and measured bit-identical).
//
// So the free, strictly-better half ships and the paid, trade-shaped half does not.
static const bool s_g661BlendFloor = !g158EnvFlag("DC2_G661_NO_BLEND_FLOOR");
//   blend 2  an EXACT integer fold, correct whatever precision the driver blends at.  <- PROMOTED
//   blend 1/5 a sub-LSB FLOOR BIAS. It DOES survive this driver (G661 measured it working), so it
//            is not a null — it is a measured +0.109 ms/f whose exactness benefit is a trade rather
//            than a strict improvement. DEFAULT OFF; DC2_G661_FLOOR_BIAS=1 arms it.
static const bool s_g661FloorBias = g158EnvFlag("DC2_G661_FLOOR_BIAS") &&
                                    !g158EnvFlag("DC2_G661_NO_FLOOR_BIAS");
// The third arm: snapping the legacy program's FLOAT texture combine onto the 8-bit grid the CPU's
// integer combine lands on. Built to explain the floor repair's signed overshoot; MEASURED NULL on
// two routes (see g661BlendFloorRgb). DEFAULT OFF so the shipped shader pays no ALU for it, kept
// arm-selectable because the null is the evidence that the overshoot has some other cause.
static const bool s_g661SrcQuant = g158EnvFlag("DC2_G661_SRC_QUANT") &&
                                   !g158EnvFlag("DC2_G661_NO_SRC_QUANT");
#if defined(PS2X_G661_DIAG)
// Law 10 counter proof: a run that reports nothing must be distinguishable from a run whose lever
// never reached a draw. ⛔ Rule 12c: this counter and its reporter live in a HOT TU
// (ps2_gs_gpu_raster.obj), and the first build measured them at +279 code bytes and +1 COMDAT — the
// exact failure class G653 P25 priced at +0.484 ms/f for a behaviourally inert addition. So they
// are a compile-time build mode, and the timing/deliverable binary carries only the arm selection.
static std::atomic<uint32_t> g_g661FloorDraws{0};
#endif

#if defined(PS2X_G684_LEGACY_G560_RUNS)
static bool g561TextureRunsRequested()
{
    static const bool on = g158EnvFlag("DC2_G561_TEXTURE_RUNS") &&
                           !g158EnvFlag("DC2_G561_NO_TEXTURE_RUNS");
    return on;
}
#else
static constexpr bool g561TextureRunsRequested() { return false; }
#endif

// G570: the G560 shader already carries every per-primitive state field needed by an
// untextured draw. Sampler/UV state is dead when both adjacent draws have no texture source,
// so it must not split an otherwise ordered run. Keep this narrowly opt-in while the combat
// spike and full-frame gates are evaluated.
#if defined(PS2X_G684_LEGACY_G560_RUNS)
static bool g570UntexturedRunsRequested()
{
    static const bool on = g158EnvFlag("DC2_G570_UNTEXTURED_RUNS") &&
                           !g158EnvFlag("DC2_G570_NO_UNTEXTURED_RUNS");
    return on;
}
#else
static constexpr bool g570UntexturedRunsRequested() { return false; }
#endif
static uint32_t g349FboKey(uint32_t fbp, int w)
{
    return (g349DecolOn() && fbp == 0x139u && w != 512) ? kG349Narrow139 : fbp;
}
// ⭐⭐⭐ G636: the resident G629 replay used to mirror its result into `ensureFbo(fbp, ...)` — the
// SAME per-`fbp` FBO the native renderer draws into — and the shader's `imageStore(gLinearView,..)`
// runs for EVERY invocation of the `width x height` dispatch grid, not only for pixels a record
// covered. So one resident batch overwrote that whole rect of the native FBO with raw-image
// content, destroying any native draw the raw image does not contain. That is the measured
// `ridepod` chassis loss. RETIRED BY DEFAULT: the mirror is written to a private key and the
// shader's view store is disabled, so the raw image reaches the native renderer only through the
// guest-VRAM publication that `g636` now settles before every FBO seed.
// `DC2_G636_LINVIEW=1` restores the pre-G636 behaviour verbatim (defect included).
static bool g636LinViewOn()
{
    static const bool on = g158EnvFlag("DC2_G636_LINVIEW");
    return on;
}
static uint32_t g289RealSourceFbp(uint32_t fbp)
{
    if (fbp == kG289ZeroFbpToken)
        return 0u;
    if (fbp == kG289AliasCt32Token || fbp == kG289AliasCt24Token)
        return kG289AliasFbp;
    if (fbp == kG289WorkCt24Token || fbp == kG289WorkCt32Token)
        return 0x13bu;
    if (fbp == kG309CompositeToken)
        return kG309CompositeFbp;
    if (fbp == kG310LogicalCt24Token)
        return kG310LogicalFbp;
    return fbp;
}
static bool g289SourceIsCt24(uint32_t fbp)
{
    return fbp == kG289AliasCt24Token || fbp == kG289WorkCt24Token ||
           fbp == kG310LogicalCt24Token;
}
static bool g289SourceIsOwner(uint32_t fbp)
{
    return fbp == kG289AliasCt32Token || fbp == kG289AliasCt24Token ||
           fbp == kG289WorkCt24Token || fbp == kG289WorkCt32Token;
}

// GL entry points needed beyond GLFns (loaded once on the G178 thread).
typedef void(APIENTRY *PFN178BLENDFUNCSEPARATE)(GLenum, GLenum, GLenum, GLenum);
typedef void(APIENTRY *PFN178BLENDEQUATIONSEPARATE)(GLenum, GLenum);
typedef void(APIENTRY *PFN178GENRENDERBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN178BINDRENDERBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFN178RENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY *PFN178FRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef void(APIENTRY *PFN178DELETERENDERBUFFERS)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFN178UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void(APIENTRY *PFN178BINDIMAGETEXTURE)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void(APIENTRY *PFN178MEMORYBARRIER)(GLbitfield);
typedef void(APIENTRY *PFN178GETINTERNALFORMATIV)(GLenum, GLenum, GLenum, GLsizei, GLint *);
typedef void(APIENTRY *PFN541DISPATCHCOMPUTE)(GLuint, GLuint, GLuint);
// G643: the two entry points the SSBO data path for the 0x139 compute needs beyond GLFns.
typedef void(APIENTRY *PFN643BINDBUFFERBASE)(GLenum, GLuint, GLuint);
typedef void(APIENTRY *PFN643BINDBUFFERRANGE)(GLenum, GLuint, GLuint, GLsizeiptrLocal, GLsizeiptrLocal);
typedef void(APIENTRY *PFN643BUFFERSUBDATA)(GLenum, GLsizeiptrLocal, GLsizeiptrLocal, const void *);
typedef void(APIENTRY *PFN652BUFFERSTORAGE)(GLenum, GLsizeiptrLocal, const void *, GLbitfield);
typedef void *(APIENTRY *PFN652MAPBUFFERRANGE)(GLenum, GLsizeiptrLocal, GLsizeiptrLocal, GLbitfield);
typedef GLboolean(APIENTRY *PFN652UNMAPBUFFER)(GLenum);

struct G178GLExt
{
    PFN178BLENDFUNCSEPARATE glBlendFuncSeparate_ = nullptr;
    PFN178BLENDEQUATIONSEPARATE glBlendEquationSeparate_ = nullptr;
    PFN178GENRENDERBUFFERS glGenRenderbuffers_ = nullptr;
    PFN178BINDRENDERBUFFER glBindRenderbuffer_ = nullptr;
    PFN178RENDERBUFFERSTORAGE glRenderbufferStorage_ = nullptr;
    PFN178FRAMEBUFFERRENDERBUFFER glFramebufferRenderbuffer_ = nullptr;
    PFN178DELETERENDERBUFFERS glDeleteRenderbuffers_ = nullptr;
    PFN178UNIFORM2F glUniform2f_ = nullptr;
    PFN178BINDIMAGETEXTURE glBindImageTexture_ = nullptr;
    PFN178MEMORYBARRIER glMemoryBarrier_ = nullptr;
    PFN178GETINTERNALFORMATIV glGetInternalformativ_ = nullptr;
    PFN541DISPATCHCOMPUTE glDispatchCompute_ = nullptr;
    PFN643BINDBUFFERBASE glBindBufferBase_ = nullptr;
    PFN643BINDBUFFERRANGE glBindBufferRange_ = nullptr;
    PFN643BUFFERSUBDATA glBufferSubData_ = nullptr;
    PFN652BUFFERSTORAGE glBufferStorage_ = nullptr;
    PFN652MAPBUFFERRANGE glMapBufferRange_ = nullptr;
    PFN652UNMAPBUFFER glUnmapBuffer_ = nullptr;
};

bool g178LoadGLExt(G178GLExt &e)
{
    bool ok = true;
    ok &= loadProc(e.glBlendFuncSeparate_, "glBlendFuncSeparate");
    ok &= loadProc(e.glBlendEquationSeparate_, "glBlendEquationSeparate");
    ok &= loadProc(e.glGenRenderbuffers_, "glGenRenderbuffers");
    ok &= loadProc(e.glBindRenderbuffer_, "glBindRenderbuffer");
    ok &= loadProc(e.glRenderbufferStorage_, "glRenderbufferStorage");
    ok &= loadProc(e.glFramebufferRenderbuffer_, "glFramebufferRenderbuffer");
    ok &= loadProc(e.glDeleteRenderbuffers_, "glDeleteRenderbuffers");
    ok &= loadProc(e.glUniform2f_, "glUniform2f");
    if (g256ExactRttOn() || g257ImageStoreTestOn())
    {
        ok &= loadProc(e.glBindImageTexture_, "glBindImageTexture");
        ok &= loadProc(e.glMemoryBarrier_, "glMemoryBarrier");
        ok &= loadProc(e.glGetInternalformativ_, "glGetInternalformativ");
    }
    else if (g541NullComputeRequested() || g542AbiReplayRequested() ||
             g543SemanticReplayRequested() || g544ChainReplayRequested() ||
             g545LiveShadowRequested() || g566VuAuthorityRequested() ||
             g606WideBenchRequested() || g607TBenchRequestedFwd() ||
             g570Shadow139Requested() || g629BackendRequested())
    {
        // Optional prototype: missing compute support must not disable the proven graphics backend.
        (void)loadProc(e.glBindImageTexture_, "glBindImageTexture");
        (void)loadProc(e.glMemoryBarrier_, "glMemoryBarrier");
    }
    if (g541NullComputeRequested() || g542AbiReplayRequested() ||
        g543SemanticReplayRequested() || g544ChainReplayRequested() ||
        g545LiveShadowRequested() || g566VuAuthorityRequested() ||
        g606WideBenchRequested() || g607TBenchRequestedFwd() || g570Shadow139Requested() ||
        g629BackendRequested())
        (void)loadProc(e.glDispatchCompute_, "glDispatchCompute");
    // G643: optional, like the compute pointers above — a driver without them keeps the proven
    // image2D data path for the 0x139 kernel instead of disabling anything.
    if (g570Shadow139Requested())
    {
        (void)loadProc(e.glBindBufferBase_, "glBindBufferBase");
        (void)loadProc(e.glBindBufferRange_, "glBindBufferRange");
        (void)loadProc(e.glBufferSubData_, "glBufferSubData");
    }
    if (g652RingBenchRequestedFwd())
    {
        (void)loadProc(e.glBufferStorage_, "glBufferStorage");
        (void)loadProc(e.glMapBufferRange_, "glMapBufferRange");
        (void)loadProc(e.glUnmapBuffer_, "glUnmapBuffer");
        (void)loadProc(e.glBindBufferBase_, "glBindBufferBase");
    }
    return ok;
}

constexpr GLenum GL178_FUNC_ADD = 0x8006;
constexpr GLenum GL178_FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum GL178_SRC1_COLOR = 0x88F9;
constexpr GLenum GL178_SRC1_ALPHA = 0x8589;
constexpr GLenum GL178_RENDERBUFFER = 0x8D41;
constexpr GLenum GL178_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL178_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL178_STREAM_DRAW = 0x88E0;
constexpr GLenum GL178_READ_WRITE = 0x88BA;
constexpr GLenum GL178_WRITE_ONLY = 0x88B9;
constexpr GLbitfield GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT = 0x00000020;
constexpr GLbitfield GL178_TEXTURE_FETCH_BARRIER_BIT = 0x00000008;
constexpr GLbitfield GL178_TEXTURE_UPDATE_BARRIER_BIT = 0x00000100;
constexpr GLenum GL643_SHADER_STORAGE_BUFFER = 0x90D2;
constexpr GLenum GL643_DYNAMIC_DRAW = 0x88E8;
constexpr GLbitfield GL643_SHADER_STORAGE_BARRIER_BIT = 0x00002000;
constexpr GLbitfield GL178_FRAMEBUFFER_BARRIER_BIT = 0x00000400;
constexpr GLenum GL541_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL541_R32UI = 0x8236;
constexpr GLenum GL541_RED_INTEGER = 0x8D94;
constexpr GLenum GL257_SHADER_IMAGE_LOAD = 0x82A4;
constexpr GLenum GL257_SHADER_IMAGE_STORE = 0x82A5;
// G641: the pixel-store strides that let a sub-rectangle travel straight out of / into a
// full-surface caller buffer, with no CPU compaction. Core since GL 1.1, but the minimal GL header
// this TU uses does not declare them.
constexpr GLenum GL641_UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum GL641_UNPACK_SKIP_ROWS = 0x0CF3;
constexpr GLenum GL641_UNPACK_SKIP_PIXELS = 0x0CF4;
constexpr GLenum GL641_PACK_ROW_LENGTH = 0x0D02;
constexpr GLenum GL641_PACK_SKIP_ROWS = 0x0D03;
constexpr GLenum GL641_PACK_SKIP_PIXELS = 0x0D04;
// G631: run-coalesced raw-VRAM page patch upload. Included HERE (not from ps2_gs_gpu_raster.cpp)
// because it needs GL541_RED_INTEGER, which is declared two lines above.
#include "g631_page_patch.inc"
#include "g542_vu_gpu_abi.inc"
#include "g543_vu_gpu_semantics.inc"
#include "g544_vu_gpu_chain.inc"
#include "g545_vu_gpu_live.inc"
#include "g606_wide_vu.inc"
// G607 after G606: it reuses g606LinkProgram, and its body comes from the generated
// translated-kernel include.
#include "g607_translated_body.inc"
#include "g607_tbench.inc"
#include "g652_command_ring_bench.inc"
// G665: the translated GPU VU1 backend. MUST come after g543 (it reuses `g543SemanticShaderSource`,
// `g543CompareSection` and `G543Mismatch`) and after g606_wide_vu.inc (it reuses `g606EnvU32` and
// `g606LinkProgram`). Both files are entirely `#if PS2X_G665_GPUVU`, so the default image - whose
// hot TU is layout-guarded at budget 0 - carries nothing.
#include "g665_vu_translate.inc"
#include "g665_vu_bench.inc"
// GPU texture-cache residency cap (bytes of decoded RGBA8). Title uses a handful of ≤512²
// textures (~1-4MB); the cap only matters on texture-heavy routes. LRU by last-used batch.
constexpr size_t kG178TexCacheCapBytes = 256ull * 1024 * 1024;

// G415 private .cpp-to-.cpp side channel for the colour-readback window of the NEXT synchronous
// batch submit. G178Batch lives in the frozen ps2_gs_gpu_lle.h, so the window travels beside it
// instead of inside it (the same idiom G242 used for depthKey). Only the synchronous submit path
// publishes a window: the async path never reads colour back, and the front-end/GS-worker thread
// is the sole writer, consumed by the worker under the queue mutex handoff.
struct G415ColorWindow
{
    bool valid = false;
    int glY = 0;   // first GL (bottom-first) row to read
    int rows = 0;  // number of GL rows to read
};
G415ColorWindow g_g415PendWindow;

// G415 aggregate counters for the narrowed colour readback ([G415:narrow] under DC2_G415_STAT=1).
std::atomic<uint64_t> g_g415NarrowBatches{0};
std::atomic<uint64_t> g_g415NarrowRows{0};
std::atomic<uint64_t> g_g415FullBatches{0};
std::atomic<uint64_t> g_g415FullRows{0};

// G541: one queued GL command can absorb every logical VU1 kick submitted before that command is
// popped. The count is mutated only while m_mtx is held; after pop, the worker owns it exclusively.
// This is the prototype form of a sequential GPU command buffer: many ordered kicks, one dispatch.
struct G541NullBatch
{
    uint64_t kicks = 0ull;
};

