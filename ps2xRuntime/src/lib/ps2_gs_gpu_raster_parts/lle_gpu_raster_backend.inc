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

// G560: adjacent draws whose complete raster state differs only by GS blend mode can share one
// ordered glDrawArrays call.  Flat G178Vtx draw state and a dual-source fragment output supply the
// per-primitive shader inputs and source/destination factors.  Default-off until the route and
// exact-output gates are complete; DC2_G560_NO_MULTI_DRAW is the one-flag rollback.
static bool g560MultiDrawRequested()
{
    static const bool on = g158EnvFlag("DC2_G560_MULTI_DRAW") &&
                           !g158EnvFlag("DC2_G560_NO_MULTI_DRAW");
    return on;
}

// G624: rollback for the flip-aware G406/G407 V rounding on `srcFbp`-bound draws. Namespace scope,
// not a function-local `static const bool` — G619 measured that form as a per-access TLS guard on
// MSVC, and this predicate sits in the per-draw submit loop.
static const bool s_g624NoUvFlip = g158EnvFlag("DC2_G624_NO_UVFLIP");

static bool g561TextureRunsRequested()
{
    static const bool on = g158EnvFlag("DC2_G561_TEXTURE_RUNS") &&
                           !g158EnvFlag("DC2_G561_NO_TEXTURE_RUNS");
    return on;
}

// G570: the G560 shader already carries every per-primitive state field needed by an
// untextured draw. Sampler/UV state is dead when both adjacent draws have no texture source,
// so it must not split an otherwise ordered run. Keep this narrowly opt-in while the combat
// spike and full-frame gates are evaluated.
static bool g570UntexturedRunsRequested()
{
    static const bool on = g158EnvFlag("DC2_G570_UNTEXTURED_RUNS") &&
                           !g158EnvFlag("DC2_G570_NO_UNTEXTURED_RUNS");
    return on;
}
static uint32_t g349FboKey(uint32_t fbp, int w)
{
    return (g349DecolOn() && fbp == 0x139u && w != 512) ? kG349Narrow139 : fbp;
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
constexpr GLbitfield GL178_FRAMEBUFFER_BARRIER_BIT = 0x00000400;
constexpr GLenum GL541_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL541_R32UI = 0x8236;
constexpr GLenum GL541_RED_INTEGER = 0x8D94;
constexpr GLenum GL257_SHADER_IMAGE_LOAD = 0x82A4;
constexpr GLenum GL257_SHADER_IMAGE_STORE = 0x82A5;
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

class G178Backend
{
public:
    static G178Backend &instance()
    {
        static G178Backend b;
        return b;
    }

    ~G178Backend()
    {
        if (!m_started)
            return;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_cvWork.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    // Same startup dance as G162GpuDecoder::startEager (release main context -> spawn thread ->
    // create+share context there -> restore main). Called once from PS2Runtime::initialize().
    void startEager()
    {
        if (m_started)
            return;
        if (!s_g158MainRC || !s_g158MainDC)
        {
            std::fprintf(stderr, "[G178:backend] SKIP: no captured main context\n");
            return;
        }
        if (!wglMakeCurrent(nullptr, nullptr))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not release main context, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return;
        }
        m_started = true;
        m_thread = std::thread([this] { worker(); });
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvReady.wait(lk, [&] { return m_ready.load() || m_failed.load(); });
        }
        if (!wglMakeCurrent(s_g158MainDC, s_g158MainRC))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not restore main context, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
        }
    }

    bool ready() const { return m_ready.load(std::memory_order_acquire); }

    // G541: enqueue-only transport. The VU1 worker never waits and the GL worker never reads its
    // persistent image back. Logical kicks coalesce into the one still-queued command; once the GL
    // worker pops it, the next kick creates a successor command before publishing its CPU packets.
    // Therefore every kick still has a compute command ahead of its dependent GS work in the same
    // FIFO, without paying one host queue item and one driver dispatch per kick.
    bool submitG541NullCompute()
    {
        if (!g541NullComputeRequested() || !m_ready.load(std::memory_order_acquire) ||
            !m_g541Available)
        {
            m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        size_t depth = 0u;
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
            {
                m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            std::shared_ptr<G541NullBatch> batch = m_g541QueuedBatch.lock();
            if (!batch)
            {
                if (m_queue.size() >= 8192u)
                {
                    m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                batch = std::make_shared<G541NullBatch>();
                QueueItem item;
                item.g541NullBatch = batch;
                m_queue.push_back(std::move(item));
                m_g541QueuedBatch = batch;
                pushed = true;
            }
            ++batch->kicks;
            depth = m_queue.size();
        }
        m_g541Submitted.fetch_add(1u, std::memory_order_relaxed);
        if (pushed)
        {
            uint64_t peak = m_g541HighWater.load(std::memory_order_relaxed);
            while (peak < depth && !m_g541HighWater.compare_exchange_weak(
                       peak, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            m_submitSeq.fetch_add(1u, std::memory_order_release);
            m_cvWork.notify_one();
        }
        return true;
    }

    // G545: move-owned, enqueue-only live command. Commands coalesce only while their queue item
    // has not been popped; the mutex therefore makes each batch immutable before the GL worker
    // reads it. A size boundary creates a FIFO successor instead of rejecting or blocking VU1.
    bool submitG545Live(std::vector<uint8_t> &&command)
    {
        std::string why;
        if (!g545LiveShadowRequested() || !m_ready.load(std::memory_order_acquire) ||
            !m_g545Available || m_g545Disabled.load(std::memory_order_acquire) ||
            !g545ValidateBlob(command, why))
        {
            m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }

        size_t depth = 0u;
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire) ||
                m_g545Disabled.load(std::memory_order_relaxed))
            {
                m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            std::shared_ptr<G545LiveBatch> batch = m_g545QueuedBatch.lock();
            if (batch && (!batch->authorityResults.empty() ||
                          batch->blobs.size() >= G545_LIVE_MAX_BATCH_COMMANDS ||
                          batch->bytes + command.size() > G545_LIVE_MAX_BATCH_BYTES))
                batch.reset();
            if (!batch)
            {
                if (m_queue.size() >= 8192u)
                {
                    m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                batch = std::make_shared<G545LiveBatch>();
                QueueItem item;
                item.g545LiveBatch = batch;
                m_queue.push_back(std::move(item));
                m_g545QueuedBatch = batch;
                pushed = true;
            }
            batch->bytes += command.size();
            batch->blobs.push_back(std::move(command));
            depth = m_queue.size();
        }
        m_g545Submitted.fetch_add(1u, std::memory_order_relaxed);
        if (pushed)
        {
            uint64_t peak = m_g545HighWater.load(std::memory_order_relaxed);
            while (peak < depth && !m_g545HighWater.compare_exchange_weak(
                       peak, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            m_submitSeq.fetch_add(1u, std::memory_order_release);
            m_cvWork.notify_one();
        }
        return true;
    }

    bool submitG566Authority(std::vector<uint8_t> &&command,
                             const std::shared_ptr<G566VuAuthorityResult> &result)
    {
        std::string why;
        if (!g566VuAuthorityRequested() || !result ||
            !m_ready.load(std::memory_order_acquire) || !m_g545Available ||
            m_g545Disabled.load(std::memory_order_acquire) ||
            !g545ValidateBlob(command, why))
        {
            m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }

        size_t depth = 0u;
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire) ||
                m_g545Disabled.load(std::memory_order_relaxed))
            {
                m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            std::shared_ptr<G545LiveBatch> batch = m_g545QueuedBatch.lock();
            if (batch && ((batch->blobs.size() != batch->authorityResults.size()) ||
                          batch->blobs.size() >= G545_LIVE_MAX_BATCH_COMMANDS ||
                          batch->bytes + command.size() > G545_LIVE_MAX_BATCH_BYTES))
                batch.reset();
            if (!batch)
            {
                if (m_queue.size() >= 8192u)
                {
                    m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                batch = std::make_shared<G545LiveBatch>();
                QueueItem item;
                item.g545LiveBatch = batch;
                m_queue.push_back(std::move(item));
                m_g545QueuedBatch = batch;
                pushed = true;
            }
            batch->bytes += command.size();
            batch->blobs.push_back(std::move(command));
            batch->authorityResults.push_back(result);
            depth = m_queue.size();
        }
        m_g545Submitted.fetch_add(1u, std::memory_order_relaxed);
        if (pushed)
        {
            uint64_t peak = m_g545HighWater.load(std::memory_order_relaxed);
            while (peak < depth && !m_g545HighWater.compare_exchange_weak(
                       peak, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            m_submitSeq.fetch_add(1u, std::memory_order_release);
            m_cvWork.notify_one();
        }
        return true;
    }

    bool hasTex(uint64_t key)
    {
        std::lock_guard<std::mutex> lk(m_residentMtx);
        return m_resident.count(key) != 0;
    }

    bool submitG570Shadow139(const std::vector<uint32_t> &color,
                             const std::vector<uint32_t> &depth,
                             const std::vector<uint32_t> &triWords,
                             const std::vector<uint32_t> &spanWords,
                             const std::vector<uint32_t> &tileOffsets,
                             const std::vector<uint32_t> &tileTris,
                             std::vector<uint32_t> &out)
    {
        constexpr size_t surfacePixels = 512u * 512u;
        if (!g570Shadow139Requested() || !m_g570Available ||
            !m_ready.load(std::memory_order_acquire) ||
            color.size() != surfacePixels || depth.size() != surfacePixels ||
            triWords.empty() || spanWords.empty() || tileOffsets.size() < 1025u ||
            tileTris.empty() || (triWords.size() % 1024u) != 0u ||
            (spanWords.size() % 1024u) != 0u ||
            (tileOffsets.size() % 1024u) != 0u || (tileTris.size() % 1024u) != 0u)
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g570Color = &color;
            item.g570Depth = &depth;
            item.g570TriWords = &triWords;
            item.g570SpanWords = &spanWords;
            item.g570TileOffsets = &tileOffsets;
            item.g570TileTris = &tileTris;
            item.g570Out = &out;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            if (g571ShadowSplitOn())
                item.g571Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }


    bool submitG629(const std::vector<uint32_t> &recs, const std::vector<uint32_t> &texDesc,
                    const std::vector<uint32_t> &texData, const std::vector<uint32_t> &tileOff,
                    const std::vector<uint32_t> &tileRef, std::vector<uint32_t> &color,
                    std::vector<uint32_t> &depth, int width, int height, int tilesX,
                    int block, int fbw, int firstPage, bool wantDepth)
    {
        if (!m_g629Available || !m_ready.load(std::memory_order_acquire) ||
            recs.empty() || tileOff.empty() || color.empty() || width <= 0 || height <= 0)
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g629Recs = &recs;
            item.g629TexDesc = &texDesc;
            item.g629TexData = &texData;
            item.g629TileOff = &tileOff;
            item.g629TileRef = &tileRef;
            item.g629Color = &color;
            item.g629Depth = &depth;
            item.g629Width = width;
            item.g629Height = height;
            item.g629TilesX = tilesX;
            item.g629Block = block;
            item.g629Fbw = fbw;
            item.g629FirstPage = firstPage;
            item.g629WantDepth = wantDepth;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G630: move-owned, no-readback replay. Jobs share this backend's FIFO with native renders,
    // copies, and view builds, so GPU order is the GS order without a front-thread wait.
    bool submitG630(const std::shared_ptr<G630ReplayPacket> &p)
    {
        if (!p || !m_g629Available || !m_ready.load(std::memory_order_acquire) ||
            p->recs.empty() || p->tileOff.empty() || p->width <= 0 || p->height <= 0)
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g630Replay = p;
            item.g629Recs = &p->recs;
            item.g629TexDesc = &p->texDesc;
            item.g629TexData = &p->texData;
            item.g629TileOff = &p->tileOff;
            item.g629TileRef = &p->tileRef;
            item.g629Width = p->width;
            item.g629Height = p->height;
            item.g629TilesX = p->tilesX;
            item.g629Block = p->block;
            item.g629Fbw = p->fbw;
            item.g629FirstPage = 0;
            item.g629WantDepth = p->wantDepth;
            item.g629Resident = true;
            item.promise = promise;
            m_queue.push_back(std::move(item));
            m_g630Futures.push_back(std::move(future));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    bool setupG630Upload()
    {
        static const char *kCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) writeonly uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gPixels;
uniform int uBlock;
uniform int uFbw;
uniform int uOriginX;
uniform int uOriginY;
uniform int uWidth;
uniform int uHeight;
const int kBlk32[32] = int[32](
     0, 1, 4, 5,16,17,20,21, 2, 3, 6, 7,18,19,22,23,
     8, 9,12,13,24,25,28,29,10,11,14,15,26,27,30,31);
const int kCol32[64] = int[64](
     0, 1, 4, 5, 8, 9,12,13, 2, 3, 6, 7,10,11,14,15,
    16,17,20,21,24,25,28,29,18,19,22,23,26,27,30,31,
    32,33,36,37,40,41,44,45,34,35,38,39,42,43,46,47,
    48,49,52,53,56,57,60,61,50,51,54,55,58,59,62,63);
ivec2 dc1024(uint i) { return ivec2(int(i & 1023u), int(i >> 10u)); }
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
uint ct32Word(int x, int y)
{
    int page = (uBlock >> 5) + (y >> 5) * uFbw + (x >> 6);
    int blockId = (uBlock & 31) + kBlk32[((y >> 3) & 3) * 8 + ((x >> 3) & 7)];
    int off = (page << 13) + ((blockId >> 5) << 13) +
              (blockId & 31) * 256 + kCol32[(y & 7) * 8 + (x & 7)] * 4;
    return (uint(off) >> 2u) & 1048575u;
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uWidth || p.y >= uHeight) return;
    uint src = uint(p.y * uWidth + p.x);
    uint value = imageLoad(gPixels, dc1024(src)).r;
    imageStore(gVram, dc2048(ct32Word(uOriginX + p.x, uOriginY + p.y)),
               uvec4(value, 0u, 0u, 0u));
}
)GLSL";
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G630:gpu] CT32 upload compile failed: %s\n", log.c_str());
            return false;
        }
        m_g630UploadProg = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g630UploadProg, cs);
        m_f.glLinkProgram_(m_g630UploadProg);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g630UploadProg, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g630UploadProg, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(m_g630UploadProg, static_cast<GLsizei>(buf.size()), nullptr,
                                     buf.data());
            std::fprintf(stderr, "[G630:gpu] CT32 upload link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g630UploadProg);
            m_g630UploadProg = 0u;
            return false;
        }
        m_locG630UploadBlock = m_f.glGetUniformLocation_(m_g630UploadProg, "uBlock");
        m_locG630UploadFbw = m_f.glGetUniformLocation_(m_g630UploadProg, "uFbw");
        m_locG630UploadX = m_f.glGetUniformLocation_(m_g630UploadProg, "uOriginX");
        m_locG630UploadY = m_f.glGetUniformLocation_(m_g630UploadProg, "uOriginY");
        m_locG630UploadWidth = m_f.glGetUniformLocation_(m_g630UploadProg, "uWidth");
        m_locG630UploadHeight = m_f.glGetUniformLocation_(m_g630UploadProg, "uHeight");
        return m_locG630UploadBlock >= 0 && m_locG630UploadFbw >= 0 &&
               m_locG630UploadX >= 0 && m_locG630UploadY >= 0 &&
               m_locG630UploadWidth >= 0 && m_locG630UploadHeight >= 0 && setupG630T8();
    }

    bool setupG630T8()
    {
        static const char *kCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) readonly uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gMap;
layout(r32ui, binding=2) readonly uniform uimage2D gClut;
layout(rgba8, binding=3) writeonly uniform image2D gOut;
uniform int uWidth;
uniform int uHeight;
uniform int uTbp;
uniform int uTbw;
ivec2 dc256(uint i) { return ivec2(int(i & 255u), int(i >> 8u)); }
ivec2 dc1024(uint i) { return ivec2(int(i & 1023u), int(i >> 10u)); }
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uWidth || p.y >= uHeight) return;
    uint x = uint(p.x), y = uint(p.y);
    uint pi = (y & 63u) * 128u + (x & 127u);
    uint perm = imageLoad(gMap, dc1024(pi)).r;
    uint off = (uint(uTbp) * 256u +
                (((y >> 6u) * uint(uTbw) + (x >> 7u)) << 13u) + perm) & 4194303u;
    uint raw = imageLoad(gVram, dc2048(off >> 2u)).r;
    uint index = (raw >> ((off & 3u) * 8u)) & 255u;
    uint c = imageLoad(gClut, dc256(index)).r;
    vec4 rgba = vec4(float(c & 255u), float((c >> 8u) & 255u),
                     float((c >> 16u) & 255u), float((c >> 24u) & 255u)) / 255.0;
    imageStore(gOut, p, rgba);
}
)GLSL";
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G630:gpu] T8 view compile failed: %s\n", log.c_str());
            return false;
        }
        m_g630T8Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g630T8Prog, cs);
        m_f.glLinkProgram_(m_g630T8Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g630T8Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g630T8Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(m_g630T8Prog, static_cast<GLsizei>(buf.size()), nullptr,
                                     buf.data());
            std::fprintf(stderr, "[G630:gpu] T8 view link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g630T8Prog);
            m_g630T8Prog = 0u;
            return false;
        }
        m_locG630T8Width = m_f.glGetUniformLocation_(m_g630T8Prog, "uWidth");
        m_locG630T8Height = m_f.glGetUniformLocation_(m_g630T8Prog, "uHeight");
        m_locG630T8Tbp = m_f.glGetUniformLocation_(m_g630T8Prog, "uTbp");
        m_locG630T8Tbw = m_f.glGetUniformLocation_(m_g630T8Prog, "uTbw");
        return m_locG630T8Width >= 0 && m_locG630T8Height >= 0 &&
               m_locG630T8Tbp >= 0 && m_locG630T8Tbw >= 0;
    }

    bool setupG630View()
    {
        static const char *kCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) readonly uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gPageMap;
layout(rgba8, binding=2) writeonly uniform image2D gOut;
uniform int uBlock;
uniform int uFbw;
uniform int uWidth;
uniform int uHeight;
uniform int uOriginY;
uniform int uRows;
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
uint ct32Word(int x, int y)
{
    int page = (uBlock >> 5) + (y >> 5) * uFbw + (x >> 6);
    uint local = uint((y & 31) * 64 + (x & 63));
    return uint(page) * 2048u + imageLoad(gPageMap, ivec2(int(local), 0)).r;
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uWidth || p.y >= uRows) return;
    int y = p.y + uOriginY;
    uint w = imageLoad(gVram, dc2048(ct32Word(p.x, y))).r;
    vec4 c = vec4(float(w & 255u), float((w >> 8u) & 255u),
                  float((w >> 16u) & 255u), float((w >> 24u) & 255u)) / 255.0;
    imageStore(gOut, ivec2(p.x, uHeight - 1 - y), c);
}
)GLSL";
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G630:gpu] CT32 view compile failed: %s\n", log.c_str());
            return false;
        }
        m_g630ViewProg = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g630ViewProg, cs);
        m_f.glLinkProgram_(m_g630ViewProg);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g630ViewProg, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g630ViewProg, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(m_g630ViewProg, static_cast<GLsizei>(buf.size()), nullptr,
                                     buf.data());
            std::fprintf(stderr, "[G630:gpu] CT32 view link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g630ViewProg);
            m_g630ViewProg = 0u;
            return false;
        }
        m_locG630ViewBlock = m_f.glGetUniformLocation_(m_g630ViewProg, "uBlock");
        m_locG630ViewFbw = m_f.glGetUniformLocation_(m_g630ViewProg, "uFbw");
        m_locG630ViewWidth = m_f.glGetUniformLocation_(m_g630ViewProg, "uWidth");
        m_locG630ViewHeight = m_f.glGetUniformLocation_(m_g630ViewProg, "uHeight");
        m_locG630ViewOriginY = m_f.glGetUniformLocation_(m_g630ViewProg, "uOriginY");
        m_locG630ViewRows = m_f.glGetUniformLocation_(m_g630ViewProg, "uRows");
        return m_locG630ViewBlock >= 0 && m_locG630ViewFbw >= 0 &&
               m_locG630ViewWidth >= 0 && m_locG630ViewHeight >= 0 &&
               m_locG630ViewOriginY >= 0 && m_locG630ViewRows >= 0 && setupG630Upload();
    }

    bool prepareG630Ct32View(const std::shared_ptr<G630ViewPacket> &p)
    {
        if (!p || !m_g629Available || !m_ready.load(std::memory_order_acquire) ||
            p->fbw <= 0 || p->width <= 0 || p->height <= 0 || p->rows <= 0 ||
            p->originY < 0 || p->rows > p->height || p->originY + p->rows > p->height ||
            (!p->pageWords.empty() && p->pageWords.size() != 64u * 32u))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g630View = p;
            item.promise = promise;
            m_queue.push_back(std::move(item));
            m_g630Futures.push_back(std::move(future));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    bool submitG630Upload(const std::shared_ptr<G630UploadPacket> &p)
    {
        if (!p || m_g630UploadProg == 0u || !m_ready.load(std::memory_order_acquire) ||
            p->pixels.size() != static_cast<size_t>(p->width) * p->height ||
            p->fbw <= 0 || p->width <= 0 || p->height <= 0)
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g630Upload = p;
            item.promise = promise;
            m_queue.push_back(std::move(item));
            m_g630Futures.push_back(std::move(future));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    bool prepareG630T8View(const std::shared_ptr<G630T8Packet> &p)
    {
        if (!p || m_g630T8Prog == 0u || !m_ready.load(std::memory_order_acquire) ||
            p->texKey == 0u || p->width <= 0 || p->height <= 0 ||
            (!p->mapWords.empty() && p->mapWords.size() != 128u * 64u) || p->tbw == 0u ||
            p->clut.size() != 256u ||
            p->patchWords.size() != p->patchPages.size() * 2048u)
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g630T8 = p;
            item.promise = promise;
            m_queue.push_back(std::move(item));
            m_g630Futures.push_back(std::move(future));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    bool pollG630(bool wait, std::vector<uint8_t> &results)
    {
        results.clear();
        while (!m_g630Futures.empty())
        {
            std::future<bool> &f = m_g630Futures.front();
            if (!wait && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                break;
            bool ok = false;
            try { ok = f.get(); } catch (...) { ok = false; }
            m_g630Futures.pop_front();
            results.push_back(ok ? 1u : 0u);
        }
        return true;
    }

    bool readG630(bool depth, std::vector<uint32_t> &out,
                  uint32_t rowLo = 0u, uint32_t rowCount = 0u)
    {
        if (!m_ready.load(std::memory_order_acquire))
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            if (depth) item.g630ReadDepth = &out;
            else item.g630ReadVram = &out;
            item.g630ReadRowLo = rowLo;
            item.g630ReadRowCount = rowCount;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // Synchronous: blocks the calling (EE or MTGS-worker) thread until the batch is rendered and
    // read back. The batch is used in place (no copy); the caller owns its lifetime.
    bool submit(G178Batch &batch)
    {
        return submitImpl(&batch, 0u, nullptr, 0, 0, nullptr);
    }

    // G305: non-blocking submit of a skip-readback color batch (no guest depth, no readback out).
    // Moves the caller's batch vectors into a backend-owned slot (scalars copied) and returns
    // WITHOUT waiting; the render overlaps the caller's next-flush preparation. Fails closed (the
    // caller falls back to synchronous submit) if the backend is not ready or a packet is already
    // in flight — the caller is contracted to drainAsync() before every submit, so the in-flight
    // guard only trips on a contract violation. All calls are on the single GS-worker/front-end
    // thread, so m_asyncPending / m_asyncFut / m_asyncSlot need no cross-thread synchronization
    // beyond the queue mutex that hands the slot to the worker.
    bool submitAsync(G178Batch &batch)
    {
        static constexpr size_t kAsyncSlots = 1u;
        if (!m_ready.load(std::memory_order_acquire) || m_asyncSlots.size() >= kAsyncSlots)
            return false;
        // G415: an async packet is skip-readback by contract, so no colour window can apply.
        // Consume any published window here so it can never leak onto a later submit.
        g_g415PendWindow.valid = false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            // Recycle completed vector storage back to the caller before installing the new packet.
            // Scalar fields deliberately remain copied in `batch`: the optimistic epilogue reads
            // them after submission, exactly as it did under the original move-assignment.
            G178Batch next = std::move(batch);
            batch.fbPixels = std::move(m_asyncRecycle.fbPixels);
            batch.texUploads = std::move(m_asyncRecycle.texUploads);
            batch.verts = std::move(m_asyncRecycle.verts);
            batch.draws = std::move(m_asyncRecycle.draws);
            batch.readback = std::move(m_asyncRecycle.readback);
            AsyncSlot slot;
            slot.batch = std::move(next);
            slot.future = std::move(fut);
            m_asyncSlots.push_back(std::move(slot));
            QueueItem item;
            item.batch = &m_asyncSlots.back().batch;
            item.promise = promise;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    // G305: block until the in-flight async packet (if any) completes. Returns false when there
    // was nothing pending; otherwise sets ok to the backend render/readback result. After this
    // returns the worker is done with m_asyncSlot, so the front-end may reuse it.
    bool drainAsync(bool &ok)
    {
        if (m_asyncSlots.empty())
            return false;
        AsyncSlot &slot = m_asyncSlots.front();
        try { ok = slot.future.get(); }
        catch (...) { ok = false; }
        m_asyncRecycle = std::move(slot.batch);
        m_asyncSlots.pop_front();
        return true;
    }

    // G242: universal-Z batches carry the authoritative guest PSMZ24 rows beside the existing
    // color-only G178 interface. Keep this private side channel out of ps2_gs_gpu_lle.h: that
    // header is deliberately frozen for the project's no-header-rebuild rule. Submission is
    // synchronous, so the caller-owned vectors remain alive until the worker finishes.
    bool submitDepth(G178Batch &batch, uint64_t depthKey,
                     const std::vector<float> *depthUpload, int depthY, int depthRows)
    {
        return submitImpl(&batch, depthKey, depthUpload, depthY, depthRows, nullptr);
    }

    // G275: render a guest-depth batch and read its written row window while that texture is
    // still attached. This preserves the synchronous GPU->VRAM contract while avoiding a second
    // worker job, promise wait, scratch-FBO bind, and depth reattachment for every write wave.
    bool submitDepthReadback(G178Batch &batch, uint64_t depthKey,
                             const std::vector<float> *depthUpload,
                             int depthY, int depthRows,
                             int readY, int readRows,
                             std::vector<float> &depthReadback)
    {
        if (depthKey == 0u || readY < 0 || readRows <= 0 ||
            readY > batch.fbH || readRows > batch.fbH - readY)
            return false;
        return submitImpl(&batch, depthKey, depthUpload, depthY, depthRows,
                          &depthReadback, readY, readRows, true);
    }

    bool readDepth(uint64_t depthKey, int width, int height,
                   int depthY, int depthRows, std::vector<float> &depthReadback)
    {
        if (depthKey == 0u || width <= 0 || height <= 0 || depthY < 0 || depthRows <= 0 ||
            depthY > height || depthRows > height - depthY)
            return false;
        G178Batch readBatch;
        readBatch.fbW = width;
        readBatch.fbH = height;
        return submitImpl(&readBatch, depthKey, nullptr, depthY, depthRows,
                          &depthReadback, depthY, depthRows, false);
    }

    // G261: synchronous FBO color row-window readback (deferred RTT-wave materialization).
    bool readColor(uint32_t fbp, int width, int height, int glY, int rows,
                   std::vector<uint32_t> &out)
    {
        if (width <= 0 || height <= 0 || glY < 0 || rows <= 0 ||
            glY > height || rows > height - glY || !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeReadColor, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = fbp;
            item.colorReadback = &out;
            item.colorTexW = width;
            item.colorTexH = height;
            item.colorW = width;
            item.colorY = glY;
            item.colorRows = rows;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(1);
        // G491: this edge (and only this edge) can elect not to spin — see g491RbNoSpinActive.
        // Six calls per frame, so the branch itself is free.
        if (g491RbNoSpinActive())
            return g491WaitFutureNoSpin(wait.future);
        return wait.wait();
    }

    bool writeColor(uint32_t fbp, int texWidth, int texHeight,
                    int glX, int glY, int width, int rows,
                    const std::vector<uint32_t> &in)
    {
        if (texWidth <= 0 || texHeight <= 0 || glX < 0 || glY < 0 ||
            width <= 0 || rows <= 0 || glX > texWidth || width > texWidth - glX ||
            glY > texHeight || rows > texHeight - glY ||
            in.size() != static_cast<size_t>(width) * rows ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeWriteColor, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = fbp;
            item.colorWrite = &in;
            item.colorTexW = texWidth;
            item.colorTexH = texHeight;
            item.colorX = glX;
            item.colorW = width;
            item.colorY = glY;
            item.colorRows = rows;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(2);
        return wait.wait();
    }

    // G627: same synchronous contract as writeColor, but each destination CT32 texel carries a
    // per-byte-lane write mask (channel != 0 => that lane is replaced). Lanes with a clear mask
    // bit keep the FBO's current byte, which is what makes a PSMT8 copy whose y range is not
    // 4-aligned expressible without publishing the surface first.
    bool writeColorMasked(uint32_t fbp, int texWidth, int texHeight,
                          int glX, int glY, int width, int rows,
                          const std::vector<uint32_t> &in,
                          const std::vector<uint32_t> &mask)
    {
        if (texWidth <= 0 || texHeight <= 0 || glX < 0 || glY < 0 ||
            width <= 0 || rows <= 0 || glX > texWidth || width > texWidth - glX ||
            glY > texHeight || rows > texHeight - glY ||
            in.size() != static_cast<size_t>(width) * rows || mask.size() != in.size() ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeWriteColor, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = fbp;
            item.colorWrite = &in;
            item.g627Mask = &mask;
            item.colorTexW = texWidth;
            item.colorTexH = texHeight;
            item.colorX = glX;
            item.colorW = width;
            item.colorY = glY;
            item.colorRows = rows;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(2);
        return wait.wait();
    }

    // G280: synchronous GPU->GPU page-tile copies between two persistent target FBOs (alias
    // view resolve). Geometry is validated on the worker against both FBOs' live dimensions.
    bool copyColor(uint32_t srcFbp, uint32_t dstFbp, const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || rects6.empty() || (rects6.size() % 6u) != 0u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeCopy, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = dstFbp;
            item.copySrcFbp = srcFbp;
            item.copyRects = &rects6;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(3);
        return wait.wait();
    }

    // Native High-Res Presentation Read: read active display FBO directly at its native physical resolution.
    bool readDisplayFbo(uint32_t fbp, uint32_t fallbackFbp, uint32_t &outW, uint32_t &outH,
                        std::vector<uint8_t> &out)
    {
        if (!m_ready.load(std::memory_order_acquire))
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.isDisplayFboRead = true;
            item.colorFbp = fbp;
            item.displayReadFallbackFbp = fallbackFbp;
            item.displayOutW = &outW;
            item.displayOutH = &outH;
            item.displayOutRgba = &out;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(1);
        return wait.wait();
    }

    // G289: same synchronous bit-exact color copy as G280, but the one measured work target may
    // be first-use. Allocate its 512x512 FBO on the worker before validating/copying.
    bool copyColorEnsureDest(uint32_t srcFbp, uint32_t dstFbp, int dstW, int dstH,
                             const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || dstW <= 0 || dstH <= 0 || rects6.empty() ||
            (rects6.size() % 6u) != 0u || !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeCopy, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = dstFbp;
            item.copySrcFbp = srcFbp;
            item.copyRects = &rects6;
            item.copyEnsureDest = true;
            item.colorTexW = dstW;
            item.colorTexH = dstH;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(3);
        return wait.wait();
    }

    // G281: synchronously build/update one decoded RGBA texture directly from a resident CT32
    // target FBO. The front-end supplies an exact PSMT8 byte-address map and current CLUT.
    bool prepareT8View(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                       int texW, int texH,
                       const std::vector<uint32_t> &mapRgba,
                       const std::vector<uint32_t> &clutRgba,
                       bool verify, uint64_t &verifyBad)
    {
        verifyBad = 0u;
        if (texKey == 0u || srcFbp == 0u || srcFbW <= 0 || texW <= 0 || texH <= 0 ||
            mapRgba.size() != static_cast<size_t>(texW) * texH ||
            clutRgba.size() != 256u || !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable): the resident view texture keeps last frame's
        // content on a candidate frame. Excluded when the verifier out-param is requested.
        {
            const bool g492Skip = g492NoOtherActive() && !verify;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g281TexKey = texKey;
            item.g281SrcFbp = srcFbp;
            item.g281SrcFbW = srcFbW;
            item.g281TexW = texW;
            item.g281TexH = texH;
            item.g281Map = &mapRgba;
            item.g281Clut = &clutRgba;
            item.g281Verify = verify;
            item.g281VerifyBad = &verifyBad;
            if (g324BackendArmed())
            {
                item.g324Class = 1u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G338: build/update one resident CT24-alpha view texture from a resident CT32 target FBO.
    // Same worker-queue contract as prepareT8View; alpha is re-derived from TEXA (ta0/aem).
    bool prepareCt24View(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                         int texW, int texH, int ta0, int aem,
                         bool verify, uint64_t &verifyBad)
    {
        verifyBad = 0u;
        if (texKey == 0u || srcFbp == 0u || srcFbW <= 0 || texW <= 0 || texH <= 0 ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable) — see prepareT8View above.
        {
            const bool g492Skip = g492NoOtherActive() && !verify;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g338Ct24View = true;
            item.g281TexKey = texKey;
            item.g281SrcFbp = srcFbp;
            item.g281SrcFbW = srcFbW;
            item.g281TexW = texW;
            item.g281TexH = texH;
            item.g338Ta0 = ta0;
            item.g338Aem = aem;
            item.g281Verify = verify;
            item.g281VerifyBad = &verifyBad;
            if (g324BackendArmed())
            {
                item.g324Class = 1u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G309: synchronously prepare live target FBOs plus the measured 8x16 authority map.
    // The timing path samples them directly; only the verifier builds/reads a 512x512 reference.
    // Caller-owned staging remains live until this job completes.
    bool buildAuthoritativeComposite(const std::vector<uint32_t> &pageMap,
                                     const std::vector<uint32_t> &basePixels,
                                     bool readback, std::vector<uint32_t> &out)
    {
        out.clear();
        if (pageMap.size() != 128u || basePixels.size() != 512u * 512u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable). Excluded when the caller wants the composite read
        // back — `out` would then stay empty and the caller's size check would flip.
        {
            const bool g492Skip = g492NoOtherActive() && !readback;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309Readback = readback ? &out : nullptr;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G310: use G309's exact atlas compositor once to populate the persistent logical FBO.
    // Unlike G309's timing arm, downstream draws sample this one ordinary texture; unlike the
    // verifier, no readback follows the composition pass.
    bool buildLogicalComposite(const std::vector<uint32_t> &pageMap,
                               const std::vector<uint32_t> &basePixels)
    {
        if (pageMap.size() != 128u || basePixels.size() != 512u * 512u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable): the persistent logical FBO keeps last frame's
        // composition on a candidate frame. No out-param, so nothing downstream changes size.
        {
            const bool g492Skip = g492NoOtherActive();
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309ForceDraw = true;
            item.g309OutputFbp = kG310LogicalFbp;
            if (g324BackendArmed())
            {
                item.g324Class = 2u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

private:
    bool submitImpl(G178Batch *batch, uint64_t depthKey,
                    const std::vector<float> *depthUpload, int depthY, int depthRows,
                    std::vector<float> *depthReadback,
                    int depthReadY = 0, int depthReadRows = 0,
                    bool renderThenReadDepth = false)
    {
        if (batch == nullptr || !m_ready.load(std::memory_order_acquire))
            return false;
        // G415: consume the front-end's published colour-readback window for THIS submit only.
        // Consume-and-clear on every submit so a window can never survive onto an unrelated job
        // (readDepth() reaches this same path with a synthetic batch).
        const G415ColorWindow g415Window = g_g415PendWindow;
        g_g415PendWindow.valid = false;
        // G492 CEILING PROBE (never promotable). Delete the whole round trip at this edge and
        // report success, so the caller keeps its post-submit epilogue and its VRAM unpack instead
        // of falling into the CPU-replay path. Items carrying a depth-readback OUT vector are
        // excluded: their size is validated by the caller, and a stale one would change its control
        // flow. See g492_overlap_ceiling.inc.
        {
            // The arm accessor marks the frame as having consumed an arm, so it must be called on
            // EVERY entry — never behind a short-circuit that a depth item could skip.
            const bool g492Arm = g492NoRenderActive();
            const bool g492Skip = g492Arm && batch != nullptr && depthReadback == nullptr;
            g492Note(kG492EdgeRender,
                     batch != nullptr ? static_cast<unsigned long long>(batch->draws.size()) : 0ull,
                     g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.batch = batch;
            item.g415Window = g415Window;
            item.depthKey = depthKey;
            item.depthUpload = depthUpload;
            item.depthY = depthY;
            item.depthRows = depthRows;
            item.depthReadback = depthReadback;
            item.depthReadY = depthReadY;
            item.depthReadRows = depthReadRows;
            item.renderThenReadDepth = renderThenReadDepth;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(0);
        return wait.wait();
    }

    static inline std::atomic<uint64_t> &dc2RenderEventCounter()
    {
        static std::atomic<uint64_t> s_count{0u};
        return s_count;
    }

    static inline bool dc2ShouldExecuteRenderEvent(const char *type, uint32_t fbp, uint64_t texKey, uint32_t srcFbp,
                                                  float minX, float minY, float maxX, float maxY,
                                                  int verts, uint32_t blend, uint32_t ztst, uint32_t zw, int mode)
    {
        const uint64_t evId = dc2RenderEventCounter().fetch_add(1u, std::memory_order_relaxed) + 1u;
        static const int s_eventLimit = [] {
            const char *v = std::getenv("DC2_RENDER_EVENT_LIMIT");
            return (v != nullptr) ? std::atoi(v) : -1;
        }();
        static const bool s_eventDump = (std::getenv("DC2_RENDER_EVENT_DUMP") != nullptr);

        if (s_eventDump || (s_eventLimit >= 0 && evId <= static_cast<uint64_t>(s_eventLimit + 10)))
        {
            std::fprintf(stderr,
                "[EVENT_DEBUG] ev=%llu type=%s FBP=0x%03x texKey=0x%llx srcFbp=0x%x "
                "bounds=[(%.1f,%.1f)..(%.1f,%.1f)] verts=%d blend=%u ztst=%u zw=%u mode=%d%s\n",
                static_cast<unsigned long long>(evId), type, fbp,
                static_cast<unsigned long long>(texKey), srcFbp,
                minX, minY, maxX, maxY, verts, blend, ztst, zw, mode,
                (s_eventLimit >= 0 && evId > static_cast<uint64_t>(s_eventLimit)) ? " [SKIPPED BY LIMIT]" : "");
            std::fflush(stderr);
        }

        if (s_eventLimit >= 0 && evId > static_cast<uint64_t>(s_eventLimit))
            return false;
        return true;
    }

    struct QueueItem
    {
        std::shared_ptr<G541NullBatch> g541NullBatch;
        std::shared_ptr<G545LiveBatch> g545LiveBatch;
        G178Batch *batch = nullptr;
        uint64_t depthKey = 0u;
        const std::vector<float> *depthUpload = nullptr;
        int depthY = 0;
        int depthRows = 0;
        std::vector<float> *depthReadback = nullptr;
        int depthReadY = 0;
        int depthReadRows = 0;
        bool renderThenReadDepth = false;
        // G261 color-readback job (batch == nullptr, colorReadback != nullptr)
        uint32_t colorFbp = 0u;
        std::vector<uint32_t> *colorReadback = nullptr;
        // G264 color-write job (batch == nullptr, colorWrite != nullptr): mirror guest upload
        // bytes into the resident FBO texture instead of materializing around them.
        const std::vector<uint32_t> *colorWrite = nullptr;
        // G627 per-byte-lane write mask, parallel to colorWrite. Non-null selects the masked
        // patch draw instead of glTexSubImage2D; every other field keeps its colorWrite meaning.
        const std::vector<uint32_t> *g627Mask = nullptr;
        int colorTexW = 0, colorTexH = 0;
        int colorX = 0, colorY = 0, colorW = 0, colorRows = 0;
        // G280 GPU->GPU alias-page copy job (batch == nullptr, copyRects != nullptr): copy CT32
        // page tiles from copySrcFbp's FBO into colorFbp's FBO color texture, 6 ints per rect.
        uint32_t copySrcFbp = 0u;
        const std::vector<int32_t> *copyRects = nullptr;
        bool copyEnsureDest = false;
        // G281 resident-CT32 -> decoded-PSMT8 texture-view job.
        uint64_t g281TexKey = 0u;
        uint32_t g281SrcFbp = 0u;
        int g281SrcFbW = 0, g281TexW = 0, g281TexH = 0;
        const std::vector<uint32_t> *g281Map = nullptr;
        const std::vector<uint32_t> *g281Clut = nullptr;
        bool g281Verify = false;
        uint64_t *g281VerifyBad = nullptr;
        // G338 resident-CT32 -> CT24-alpha texture-view job (reuses the g281* common fields for
        // texKey/srcFbp/dims/verify; distinguished by g338Ct24View).
        bool g338Ct24View = false;
        int g338Ta0 = 0, g338Aem = 0;
        // G309 one-pass resident-source authoritative CT32 atlas composite.
        const std::vector<uint32_t> *g309PageMap = nullptr;
        const std::vector<uint32_t> *g309BasePixels = nullptr;
        std::vector<uint32_t> *g309Readback = nullptr;
        bool g309ForceDraw = false;
        uint32_t g309OutputFbp = kG309CompositeFbp;
        // G570 exact shadow-volume compute job. All vectors are caller-owned and synchronous.
        const std::vector<uint32_t> *g570Color = nullptr;
        const std::vector<uint32_t> *g570Depth = nullptr;
        const std::vector<uint32_t> *g570TriWords = nullptr;
        const std::vector<uint32_t> *g570SpanWords = nullptr;
        const std::vector<uint32_t> *g570TileOffsets = nullptr;
        const std::vector<uint32_t> *g570TileTris = nullptr;
        std::vector<uint32_t> *g570Out = nullptr;
        // G629 exact CPU-band-replay compute job. All vectors are caller-owned and synchronous:
        // the front-end blocks on this exactly as it blocked on the CPU row pool it replaces, so
        // the batch's ordering contract with the rest of the flush is unchanged.
        const std::vector<uint32_t> *g629Recs = nullptr;
        const std::vector<uint32_t> *g629TexDesc = nullptr;
        const std::vector<uint32_t> *g629TexData = nullptr;
        const std::vector<uint32_t> *g629TileOff = nullptr;
        const std::vector<uint32_t> *g629TileRef = nullptr;
        std::vector<uint32_t> *g629Color = nullptr;
        std::vector<uint32_t> *g629Depth = nullptr;
        int g629Width = 0, g629Height = 0, g629TilesX = 0;
        int g629Block = 0, g629Fbw = 0, g629FirstPage = 0;
        bool g629WantDepth = false;
        bool g629Resident = false;
        std::shared_ptr<G630ReplayPacket> g630Replay;
        std::shared_ptr<G630ViewPacket> g630View;
        std::shared_ptr<G630UploadPacket> g630Upload;
        std::shared_ptr<G630T8Packet> g630T8;
        std::vector<uint32_t> *g630ReadVram = nullptr;
        std::vector<uint32_t> *g630ReadDepth = nullptr;
        // G631: row band of the persistent raw-VRAM image to read back (row p == guest page p).
        // rowCount 0 keeps the legacy whole-image glGetTexImage.
        uint32_t g630ReadRowLo = 0u, g630ReadRowCount = 0u;
        std::shared_ptr<std::promise<bool>> promise;
        // G447: optional caller-owned completion flag, stored (release) by the worker IMMEDIATELY
        // BEFORE promise->set_value so a spinning caller can never observe completion earlier than
        // the promise is satisfied — the caller's stack frame therefore always outlives this store.
        // Null for the async (G305) path, whose caller does not stay on its frame.
        std::atomic<uint32_t> *doneFlag = nullptr;
        // G415 exact colour-readback window published by the front-end for this submit only.
        G415ColorWindow g415Window;
        std::chrono::steady_clock::time_point g299Enqueue{};
        uint8_t g324Class = 0u; // 1=G310 T8 view, 2=G310 logical composite
        std::chrono::steady_clock::time_point g324Enqueue{};
        // Native High-Res Presentation Read job
        bool isDisplayFboRead = false;
        uint32_t displayReadFallbackFbp = 0u;
        uint32_t *displayOutW = nullptr;
        uint32_t *displayOutH = nullptr;
        std::vector<uint8_t> *displayOutRgba = nullptr;
        std::chrono::steady_clock::time_point g571Enqueue{};
    };
    struct TexEntry
    {
        GLuint id = 0;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
        // G492: set only by prepareCt24ViewOnWorker, and only for storage IT allocated at (w,h).
        // Lives on the entry so eviction/recreation or a build by any other path clears it — the
        // storage-reuse arm can then never skip an allocation it did not make.
        uint32_t g492Ct24Alloc = 0;
        // G496b: this texture OBJECT's sampler parameters, as last set through g496TexFilter /
        // g496TexWrap. 0 = never set by us (GL's own defaults), which always forces a real call.
        // Object state, so it survives rebinds and batches; glTexImage2D does not disturb it. Only
        // prepareCt24ViewOnWorker parameterises an m_texCache entry outside the draw loop, and it
        // records what it set.
        int g496MinF = 0, g496MagF = 0, g496WrapS = 0, g496WrapT = 0;
    };
    struct FboEntry
    {
        GLuint fbo = 0, colorTex = 0, depthRb = 0;
        int w = 0, h = 0;
        int physW = 0, physH = 0;
        // G415: one parked geometry per key. A target written at two widths (DC2's fbp=0x139 at
        // fbw=8 and fbw=2) otherwise destroys and reallocates both attachments on every flip.
        GLuint spareFbo = 0, spareColorTex = 0, spareDepthRb = 0;
        int spareW = 0, spareH = 0;
        int sparePhysW = 0, sparePhysH = 0;
    };
    struct DepthEntry
    {
        GLuint tex = 0;
        int w = 0, h = 0;
        int physW = 0, physH = 0;
        bool initialized = false;
    };

    void worker()
    {
        HGLRC rc = wglCreateContext(s_g158MainDC);
        if (!rc || !wglShareLists(s_g158MainRC, rc) || !wglMakeCurrent(s_g158MainDC, rc))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: context setup, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            if (rc)
                wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (!g158LoadGL(m_f) || !g178LoadGLExt(m_e))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not resolve GL entry points\n");
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (!setupProgram() || (g256ExactRttOn() && !setupExactProgram()) ||
            (g281T8ViewRequested() && !setupT8ViewProgram()) ||
            (g338Ct24ViewRequested() && !setupCt24ViewProgram()) ||
            (g309Requested() && !setupAuthoritativeCompositeProgram()) ||
            !setupBuffers() ||
            (g257ImageStoreTestOn() && !runG257ImageStoreDiagnostic()))
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (g541NullComputeRequested())
        {
            m_g541Available = setupG541NullCompute();
            std::fprintf(stderr, "[G541:null] compute transport %s (async, no readback)\n",
                         m_g541Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g629BackendRequested())
        {
            m_g629Available = setupG629();
            std::fprintf(stderr, "[G629:gpu] exact band-replay compute %s\n",
                         m_g629Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g570Shadow139Requested())
        {
            m_g570Available = setupG570Shadow139();
            std::fprintf(stderr, "[G570:gpu139] exact shadow compute %s\n",
                         m_g570Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g545LiveShadowRequested() || g566VuAuthorityRequested())
        {
            m_g545Available = setupG545LiveShadow();
            if (g566VuAuthorityRequested())
                std::fprintf(stderr,
                             "[G566:authority] persistent exact executor %s (batched packet publication)\n",
                             m_g545Available ? "READY" : "UNAVAILABLE");
            else
                std::fprintf(stderr,
                             "[G545:live] persistent semantic shadow %s (batched, delayed readback)\n",
                             m_g545Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g542AbiReplayRequested())
            (void)g542RunAbiReplay(m_f, m_e);
        if (g543SemanticReplayRequested())
            (void)g543RunSemanticReplay(m_f, m_e);
        if (g544ChainReplayRequested())
            (void)g544RunChainReplay(m_f, m_e);
        if (g606WideBenchRequested())
            (void)g606RunWideBench(m_f, m_e);
        if (g607TBenchRequested())
            (void)g607RunTBench(m_f, m_e);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        m_ready.store(true, std::memory_order_release);
        m_cvReady.notify_all();
        std::fprintf(stderr, "[G178:backend] PASS: persistent LLE GPU raster thread ready\n");

        for (;;)
        {
            QueueItem item;
            {
                // G447 (second half of the round trip, DC2_G419_AB=wspin / DC2_G447_WSPIN_US): the
                // caller-side spin removes the GS worker's wake, but the SUBMIT direction has the
                // same latency — this thread is asleep when the item is queued and the kernel must
                // schedule it back on. Spin on a submit sequence counter for a bounded budget before
                // falling into the condition variable. Purely a hint: the cv predicate below is
                // still the authority, so the queue discipline and ordering are unchanged.
                // G471 tested both a cheaper and a more backed-off poll here too (the two spins read
                // the same shared QPC page, so they were armed together); see g447WaitFuture above.
                if (g447WorkerSpinActive())
                {
                    const uint64_t seen = m_submitSeq.load(std::memory_order_acquire);
                    const auto until = std::chrono::steady_clock::now() +
                                       std::chrono::microseconds(g447WorkerSpinBudgetUs());
                    while (m_submitSeq.load(std::memory_order_acquire) == seen)
                    {
                        if (std::chrono::steady_clock::now() >= until)
                            break;
                        YieldProcessor();
                    }
                }
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty())
                    break;
                item = std::move(m_queue.front());
                if (item.g541NullBatch)
                {
                    const std::shared_ptr<G541NullBatch> queued = m_g541QueuedBatch.lock();
                    if (queued.get() == item.g541NullBatch.get())
                        m_g541QueuedBatch.reset();
                }
                if (item.g545LiveBatch)
                {
                    const std::shared_ptr<G545LiveBatch> queued = m_g545QueuedBatch.lock();
                    if (queued.get() == item.g545LiveBatch.get())
                        m_g545QueuedBatch.reset();
                }
                m_queue.pop_front();
            }
            if (item.g545LiveBatch)
            {
                if (m_g545Available && !m_g545Disabled.load(std::memory_order_acquire))
                    (void)runG545LiveBatch(*item.g545LiveBatch);
                else
                {
                    m_g545Rejected.fetch_add(item.g545LiveBatch->blobs.size(),
                                              std::memory_order_relaxed);
                    g566PublishBatchFailure(*item.g545LiveBatch);
                }
                continue;
            }
            if (item.g541NullBatch)
            {
                const uint64_t batchKicks = item.g541NullBatch->kicks;
                if (m_g541Available && batchKicks != 0ull)
                {
                    // One GPU-resident dependency word models persistent cross-kick VU state. A
                    // queued batch is one sequential command-buffer dispatch in the normal case.
                    // The barrier orders the following GS consumer on-GPU; no CPU wait/readback.
                    m_f.glUseProgram_(m_g541NullProg);
                    m_e.glBindImageTexture_(7u, m_g541StateTex, 0, GL_FALSE, 0,
                                            GL178_READ_WRITE, GL541_R32UI);
                    uint64_t remaining = batchKicks;
                    uint64_t dispatches = 0ull;
                    while (remaining != 0ull)
                    {
                        const uint32_t chunk = static_cast<uint32_t>(
                            std::min<uint64_t>(remaining, 0x7fffffffull));
                        m_f.glUniform1i_(m_locG541KickCount, static_cast<GLint>(chunk));
                        m_e.glDispatchCompute_(1u, 1u, 1u);
                        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                        remaining -= chunk;
                        ++dispatches;
                    }
                    m_g541Batches.fetch_add(dispatches, std::memory_order_relaxed);
                    uint64_t maxBatch = m_g541MaxBatch.load(std::memory_order_relaxed);
                    while (maxBatch < batchKicks && !m_g541MaxBatch.compare_exchange_weak(
                               maxBatch, batchKicks, std::memory_order_relaxed,
                               std::memory_order_relaxed)) {}
                    const uint64_t n = m_g541Executed.fetch_add(
                        batchKicks, std::memory_order_relaxed) + batchKicks;
                    if (n >= m_g541NextReport)
                    {
                        m_g541NextReport = ((n >> 16u) + 1ull) << 16u;
                        std::fprintf(stderr,
                                     "[G541:null] kicks(sub=%llu exec=%llu reject=%llu) "
                                     "batches=%llu maxBatch=%llu qPeak=%llu\n",
                                     static_cast<unsigned long long>(
                                         m_g541Submitted.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(n),
                                     static_cast<unsigned long long>(
                                         m_g541Rejected.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541Batches.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541MaxBatch.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541HighWater.load(std::memory_order_relaxed)));
                    }
                }
                else
                    m_g541Rejected.fetch_add(batchKicks, std::memory_order_relaxed);
                continue;
            }
            // G472 premise census: price how many synchronous render round trips could collapse
            // into one worker item without crossing an observable backend dependency.  Restrict the
            // candidate runs to skip-readback colour batches with no guest-depth side channel and
            // no framebuffer upload; any other backend item, FBO geometry change, or key change is
            // a hard boundary.  Diagnostic only: no queue, promise, or GL behaviour changes.
            static const bool s_g472Census = g158EnvFlag("DC2_G472_CENSUS");
            if (s_g472Census)
            {
                static uint64_t s_renderItems = 0u;
                static uint64_t s_eligibleItems = 0u;
                static uint64_t s_closedRuns = 0u;
                static uint64_t s_avoidableTrips = 0u;
                static uint64_t s_runBins[8]{}; // 1,2,3,4,5-8,9-16,17-32,33+
                // Item classes: eligible render, depth render, upload/other render, readback render,
                // read job, write job, copy job, other.  The successor row explains exactly what
                // prevents eligible submits from becoming a run.
                static uint64_t s_classCounts[8]{};
                static uint64_t s_afterEligible[8]{};
                static unsigned s_prevClass = 8u;
                static uint64_t s_renderPairs = 0u, s_sameFboRenderPairs = 0u;
                static uint64_t s_renderPairClass[16]{};
                static uint64_t s_sameFboPairClass[16]{};
                static bool s_prevRender = false;
                static unsigned s_prevRenderClass = 0u;
                static uint32_t s_prevRenderKey = 0u;
                static int s_prevRenderW = 0, s_prevRenderH = 0;
                static uint64_t s_run = 0u;
                static uint32_t s_key = 0u;
                static int s_w = 0, s_h = 0;
                const bool renderItem =
                    item.batch != nullptr &&
                    !(item.depthReadback != nullptr && !item.renderThenReadDepth);
                const bool eligible =
                    renderItem && item.batch->skipReadback && item.depthKey == 0u &&
                    !item.batch->uploadFb;
                const uint32_t key = eligible
                    ? (item.batch->fboKey != 0u ? item.batch->fboKey : item.batch->fbp)
                    : 0u;
                const int w = eligible ? item.batch->fbW : 0;
                const int h = eligible ? item.batch->fbH : 0;
                unsigned itemClass = 7u;
                if (renderItem)
                {
                    if (eligible)
                        itemClass = 0u;
                    else if (item.depthKey != 0u)
                        itemClass = 1u;
                    else if (item.batch->uploadFb)
                        itemClass = 2u;
                    else
                        itemClass = 3u;
                }
                else if (item.colorReadback != nullptr || item.depthReadback != nullptr)
                    itemClass = 4u;
                else if (item.colorWrite != nullptr)
                    itemClass = 5u;
                else if (item.copyRects != nullptr)
                    itemClass = 6u;
                ++s_classCounts[itemClass];
                if (s_prevClass == 0u)
                    ++s_afterEligible[itemClass];
                s_prevClass = itemClass;
                if (renderItem)
                {
                    const uint32_t renderKey =
                        item.batch->fboKey != 0u ? item.batch->fboKey : item.batch->fbp;
                    if (s_prevRender)
                    {
                        ++s_renderPairs;
                        ++s_renderPairClass[s_prevRenderClass * 4u + itemClass];
                        if (renderKey == s_prevRenderKey && item.batch->fbW == s_prevRenderW &&
                            item.batch->fbH == s_prevRenderH)
                        {
                            ++s_sameFboRenderPairs;
                            ++s_sameFboPairClass[s_prevRenderClass * 4u + itemClass];
                        }
                    }
                    s_prevRender = true;
                    s_prevRenderClass = itemClass;
                    s_prevRenderKey = renderKey;
                    s_prevRenderW = item.batch->fbW;
                    s_prevRenderH = item.batch->fbH;
                }
                else
                {
                    s_prevRender = false;
                }
                const auto closeRun = [&] {
                    if (s_run == 0u)
                        return;
                    ++s_closedRuns;
                    s_avoidableTrips += s_run - 1u;
                    const unsigned bin = s_run <= 4u
                        ? static_cast<unsigned>(s_run - 1u)
                        : (s_run <= 8u ? 4u : (s_run <= 16u ? 5u : (s_run <= 32u ? 6u : 7u)));
                    ++s_runBins[bin];
                    s_run = 0u;
                };
                if (eligible && s_run != 0u && key == s_key && w == s_w && h == s_h)
                {
                    ++s_run;
                }
                else
                {
                    closeRun();
                    if (eligible)
                    {
                        s_run = 1u;
                        s_key = key;
                        s_w = w;
                        s_h = h;
                    }
                }
                if (renderItem)
                    ++s_renderItems;
                if (eligible)
                    ++s_eligibleItems;
                if (renderItem && (s_renderItems % 4096u) == 0u)
                {
                    const uint64_t liveAvoid = s_run != 0u ? s_run - 1u : 0u;
                    std::fprintf(stderr,
                                 "[G472:census] render=%llu eligible=%llu runs=%llu live=%llu "
                                 "avoidable=%llu bins=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "class=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "afterElig=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "renderPairs=%llu sameFbo=%llu "
                                 "pairClass=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                                 "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "sameClass=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                                 "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
                                 static_cast<unsigned long long>(s_renderItems),
                                 static_cast<unsigned long long>(s_eligibleItems),
                                 static_cast<unsigned long long>(s_closedRuns + (s_run != 0u ? 1u : 0u)),
                                 static_cast<unsigned long long>(s_run),
                                 static_cast<unsigned long long>(s_avoidableTrips + liveAvoid),
                                 static_cast<unsigned long long>(s_runBins[0]),
                                 static_cast<unsigned long long>(s_runBins[1]),
                                 static_cast<unsigned long long>(s_runBins[2]),
                                 static_cast<unsigned long long>(s_runBins[3]),
                                 static_cast<unsigned long long>(s_runBins[4]),
                                 static_cast<unsigned long long>(s_runBins[5]),
                                 static_cast<unsigned long long>(s_runBins[6]),
                                 static_cast<unsigned long long>(s_runBins[7]),
                                 static_cast<unsigned long long>(s_classCounts[0]),
                                 static_cast<unsigned long long>(s_classCounts[1]),
                                 static_cast<unsigned long long>(s_classCounts[2]),
                                 static_cast<unsigned long long>(s_classCounts[3]),
                                 static_cast<unsigned long long>(s_classCounts[4]),
                                 static_cast<unsigned long long>(s_classCounts[5]),
                                 static_cast<unsigned long long>(s_classCounts[6]),
                                 static_cast<unsigned long long>(s_classCounts[7]),
                                 static_cast<unsigned long long>(s_afterEligible[0]),
                                 static_cast<unsigned long long>(s_afterEligible[1]),
                                 static_cast<unsigned long long>(s_afterEligible[2]),
                                 static_cast<unsigned long long>(s_afterEligible[3]),
                                 static_cast<unsigned long long>(s_afterEligible[4]),
                                 static_cast<unsigned long long>(s_afterEligible[5]),
                                 static_cast<unsigned long long>(s_afterEligible[6]),
                                 static_cast<unsigned long long>(s_afterEligible[7]),
                                 static_cast<unsigned long long>(s_renderPairs),
                                 static_cast<unsigned long long>(s_sameFboRenderPairs),
                                 static_cast<unsigned long long>(s_renderPairClass[0]),
                                 static_cast<unsigned long long>(s_renderPairClass[1]),
                                 static_cast<unsigned long long>(s_renderPairClass[2]),
                                 static_cast<unsigned long long>(s_renderPairClass[3]),
                                 static_cast<unsigned long long>(s_renderPairClass[4]),
                                 static_cast<unsigned long long>(s_renderPairClass[5]),
                                 static_cast<unsigned long long>(s_renderPairClass[6]),
                                 static_cast<unsigned long long>(s_renderPairClass[7]),
                                 static_cast<unsigned long long>(s_renderPairClass[8]),
                                 static_cast<unsigned long long>(s_renderPairClass[9]),
                                 static_cast<unsigned long long>(s_renderPairClass[10]),
                                 static_cast<unsigned long long>(s_renderPairClass[11]),
                                 static_cast<unsigned long long>(s_renderPairClass[12]),
                                 static_cast<unsigned long long>(s_renderPairClass[13]),
                                 static_cast<unsigned long long>(s_renderPairClass[14]),
                                 static_cast<unsigned long long>(s_renderPairClass[15]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[0]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[1]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[2]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[3]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[4]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[5]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[6]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[7]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[8]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[9]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[10]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[11]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[12]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[13]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[14]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[15]));
                }
            }
            bool ok = false;
            const bool g299ProfileBatch =
                item.batch != nullptr && item.depthReadback == nullptr;
            const auto g299WorkStart =
                g299ProfileBatch && g299ProfileOn()
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
            const bool g324Profile =
                item.g324Class != 0u && g324BackendArmed();
            const auto g324WorkStart = g324Profile
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            // G332: classify this item into one backend GL-busy bucket before the work runs.
            const bool g332Armed = g332BackendArmed();
            uint32_t g332Cls = 0u; // 0=render(submit) 1=readback 2=copy 3=other
            if (g332Armed)
            {
                if (item.g309PageMap != nullptr || item.g281Map != nullptr ||
                    item.g338Ct24View || item.colorWrite != nullptr)
                    g332Cls = 3u; // decode / composite / upload
                else if (item.copyRects != nullptr)
                    g332Cls = 2u; // GPU->GPU alias copy
                else if (item.colorReadback != nullptr ||
                         (item.depthReadback != nullptr && !item.renderThenReadDepth))
                    g332Cls = 1u; // blocking glReadPixels
                else
                    g332Cls = 0u; // renderBatch native submission (incl. renderThenReadDepth)
            }
            const auto g332WorkStart = g332Armed ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
            const bool g453On = g453BackendCensusOn();
            uint32_t g453Cls = 4u;
            if (g453On)
            {
                if (item.g281Map != nullptr)
                    g453Cls = 0u;
                else if (item.g338Ct24View)
                    g453Cls = 1u;
                else if (item.g309PageMap != nullptr)
                    g453Cls = 2u;
                else if (item.colorWrite != nullptr)
                    g453Cls = 3u;
            }
            const auto g453WorkStart = g453Cls < 4u ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
            // G425 readback census keeps its own clock so it does not depend on G332 being armed.
            static const bool s_g425RbOn = (std::getenv("DC2_G425_RBCENSUS") != nullptr);
            const auto g425WorkStart = s_g425RbOn ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
            // G495 queued-ahead census (DC2_G495_RB=1, default OFF). Classify and price this item
            // BEFORE it runs, so a readback never counts itself in its own "ahead" set, and mark
            // whether it is a pipeline fence so the accumulator can be cleared after it.
            bool g495Fence = false;
            int g495Cls = kG495ClsRenderSkip;
            if (g495RbCensusOn() || g495GpuCensusOn())
            {
                uint64_t g495Draws = 0ull, g495Verts = 0ull, g495TexN = 0ull;
                uint64_t g495TexBytes = 0ull, g495FbBytes = 0ull;
                if (item.g629Recs != nullptr)
                    g495Cls = kG495ClsG630Replay;
                else if (item.g630View != nullptr)
                    g495Cls = kG495ClsG630View;
                else if (item.g630Upload != nullptr)
                    g495Cls = kG495ClsG630Upload;
                else if (item.g630T8 != nullptr)
                    g495Cls = kG495ClsG630T8;
                else if (item.g630ReadVram != nullptr || item.g630ReadDepth != nullptr)
                    g495Cls = kG495ClsG630Read;
                else if (item.g570TriWords != nullptr)
                {
                    g495Cls = kG495ClsRenderRb;
                    g495Fence = true;
                    g495TexBytes = static_cast<uint64_t>(
                        item.g570Color->size() + item.g570Depth->size() +
                        item.g570TriWords->size() + item.g570SpanWords->size() +
                        item.g570TileOffsets->size() + item.g570TileTris->size()) * 4ull;
                }
                else if (item.g309PageMap != nullptr)
                    g495Cls = kG495ClsComposite;
                else if (item.g281Map != nullptr)
                    g495Cls = kG495ClsT8;
                else if (item.g338Ct24View)
                    g495Cls = kG495ClsCt24;
                else if (item.copyRects != nullptr)
                    g495Cls = kG495ClsCopy;
                else if (item.colorReadback != nullptr)
                {
                    g495Cls = kG495ClsReadColor;
                    g495Fence = true;
                }
                else if (item.colorWrite != nullptr)
                {
                    g495Cls = kG495ClsWriteColor;
                    g495TexBytes = static_cast<uint64_t>(item.colorWrite->size()) * 4ull;
                }
                else if (item.depthReadback != nullptr && !item.renderThenReadDepth)
                {
                    g495Cls = kG495ClsReadDepth;
                    g495Fence = true;
                }
                else if (item.batch != nullptr)
                {
                    g495Cls = item.batch->skipReadback ? kG495ClsRenderSkip : kG495ClsRenderRb;
                    g495Fence = !item.batch->skipReadback;
                    g495Draws = item.batch->draws.size();
                    g495Verts = item.batch->verts.size();
                    g495TexN = item.batch->texUploads.size();
                    for (const auto &u : item.batch->texUploads)
                        g495TexBytes += static_cast<uint64_t>(u.px.size()) * 4ull;
                    g495FbBytes = static_cast<uint64_t>(item.batch->fbPixels.size()) * 4ull;
                }
                g495NoteItem(g495Cls, g495Draws, g495Verts, g495TexN, g495TexBytes, g495FbBytes);
            }
            // G495b GPU-side clock (DC2_G495_GPU=1): GL_TIME_ELAPSED around this item's whole GL
            // body, so the per-class sum is GPU busy ms/f. Never quote a frame time from this run.
            g495GpuBegin(g495Cls);
            try
            {
                if (item.g629Recs != nullptr)
                {
                    ok = runG629OnWorker(item);
                }
                else if (item.g630View != nullptr)
                    ok = runG630ViewOnWorker(*item.g630View);
                else if (item.g630Upload != nullptr)
                    ok = runG630UploadOnWorker(*item.g630Upload);
                else if (item.g630T8 != nullptr)
                    ok = runG630T8OnWorker(*item.g630T8);
                else if (item.g630ReadVram != nullptr)
                    ok = readG630ImageOnWorker(false, *item.g630ReadVram,
                                               item.g630ReadRowLo, item.g630ReadRowCount);
                else if (item.g630ReadDepth != nullptr)
                    ok = readG630ImageOnWorker(true, *item.g630ReadDepth,
                                               item.g630ReadRowLo, item.g630ReadRowCount);
                else if (item.g570TriWords != nullptr)
                {
                    const bool g571On = g571ShadowSplitOn();
                    const auto g571WorkStart = g571On ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
                    ok = runG570Shadow139OnWorker(
                        *item.g570Color, *item.g570Depth, *item.g570TriWords,
                        *item.g570SpanWords, *item.g570TileOffsets,
                        *item.g570TileTris, *item.g570Out);
                    if (g571On)
                    {
                        const auto done = std::chrono::steady_clock::now();
                        const double queueMs = std::chrono::duration<double, std::milli>(
                            g571WorkStart - item.g571Enqueue).count();
                        const double workerMs = std::chrono::duration<double, std::milli>(
                            done - g571WorkStart).count();
                        std::fprintf(stderr,
                                     "[G571:gpuqueue] queue=%.3fms worker=%.3fms ok=%u\n",
                                     queueMs, workerMs, ok ? 1u : 0u);
                    }
                }
                else if (item.g309PageMap != nullptr)
                    ok = buildAuthoritativeCompositeOnWorker(
                        *item.g309PageMap, *item.g309BasePixels,
                        item.g309Readback, item.g309ForceDraw,
                        item.g309OutputFbp);
                else if (item.g281Map != nullptr)
                    ok = prepareT8ViewOnWorker(
                        item.g281TexKey, item.g281SrcFbp, item.g281SrcFbW,
                        item.g281TexW, item.g281TexH, *item.g281Map, *item.g281Clut,
                        item.g281Verify, *item.g281VerifyBad);
                else if (item.g338Ct24View)
                    ok = prepareCt24ViewOnWorker(
                        item.g281TexKey, item.g281SrcFbp, item.g281SrcFbW,
                        item.g281TexW, item.g281TexH, item.g338Ta0, item.g338Aem,
                        item.g281Verify, *item.g281VerifyBad);
                else if (item.copyRects != nullptr)
                {
                    if (item.copyEnsureDest)
                    {
                        FboEntry &dst =
                            ensureFbo(item.colorFbp, item.colorTexW, item.colorTexH);
                        ok = dst.fbo != 0 &&
                             copyColorRectsOnWorker(item.copySrcFbp, item.colorFbp,
                                                    *item.copyRects);
                    }
                    else
                        ok = copyColorRectsOnWorker(item.copySrcFbp, item.colorFbp,
                                                    *item.copyRects);
                }
                else if (item.colorReadback != nullptr)
                    ok = readColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                           item.colorY, item.colorRows, *item.colorReadback);
                else if (item.colorWrite != nullptr && item.g627Mask != nullptr)
                    ok = writeColorMaskedOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                                  item.colorX, item.colorY, item.colorW,
                                                  item.colorRows, *item.colorWrite,
                                                  *item.g627Mask);
                else if (item.colorWrite != nullptr)
                    ok = writeColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                            item.colorX, item.colorY, item.colorW,
                                            item.colorRows, *item.colorWrite);
                else if (item.isDisplayFboRead)
                    ok = readDisplayFboOnWorker(item.colorFbp, item.displayReadFallbackFbp,
                                                *item.displayOutW, *item.displayOutH,
                                                *item.displayOutRgba);
                else if (item.depthReadback != nullptr && !item.renderThenReadDepth)
                    ok = readDepthOnWorker(item.depthKey, item.batch->fbW, item.batch->fbH,
                                           item.depthReadY, item.depthReadRows,
                                           *item.depthReadback);
                else
                    ok = renderBatch(*item.batch, item.depthKey, item.depthUpload,
                                     item.depthY, item.depthRows,
                                     item.renderThenReadDepth ? item.depthReadback : nullptr,
                                     item.depthReadY, item.depthReadRows,
                                     item.g415Window);
            }
            catch (...)
            {
                ok = false;
            }
            g495GpuEnd();
            // G425: readback-redundancy ceiling census (default OFF, DC2_G425_RBCENSUS=1).
            // Question: how much of the GS worker's blocking colour-readback time re-reads a
            // window that NOTHING has mutated since the previous readback of the same window?
            // The mutation clock is deliberately GLOBAL (any render/copy/write/composite item
            // ticks it), so the answer is a strict LOWER bound on what a per-surface readback
            // cache could recover — exactly the ceiling G420's rule demands before building one.
            {
                if (s_g425RbOn)
                {
                    static uint64_t s_mutSeq = 0ull;
                    static std::unordered_map<uint64_t, uint64_t> s_lastSeq;
                    static uint64_t s_reads = 0ull, s_redundant = 0ull;
                    static uint64_t s_readNs = 0ull, s_redundantNs = 0ull;
                    const bool isReadback = item.colorReadback != nullptr;
                    if (!isReadback)
                        ++s_mutSeq;
                    else
                    {
                        const uint64_t key =
                            ((uint64_t)item.colorFbp << 40) ^
                            ((uint64_t)(uint32_t)item.colorY << 24) ^
                            ((uint64_t)(uint32_t)item.colorRows << 12) ^
                            (uint64_t)(uint32_t)item.colorTexW;
                        const uint64_t ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g425WorkStart).count());
                        ++s_reads;
                        s_readNs += ns;
                        auto it = s_lastSeq.find(key);
                        if (it != s_lastSeq.end() && it->second == s_mutSeq)
                        {
                            ++s_redundant;
                            s_redundantNs += ns;
                        }
                        if (it != s_lastSeq.end())
                            it->second = s_mutSeq;
                        else
                            s_lastSeq.emplace(key, s_mutSeq);
                        if ((s_reads % 2000ull) == 0ull)
                            std::fprintf(stderr,
                                "[G425:rb] reads=%llu ms=%.1f | redundant=%llu (%.1f%%) ms=%.1f "
                                "(%.1f%% ofReadbackMs) distinctWindows=%zu\n",
                                (unsigned long long)s_reads, s_readNs / 1e6,
                                (unsigned long long)s_redundant,
                                100.0 * (double)s_redundant / (double)s_reads,
                                s_redundantNs / 1e6,
                                100.0 * (double)s_redundantNs /
                                    (double)(s_readNs ? s_readNs : 1ull),
                                s_lastSeq.size());
                    }
                }
            }
            // G445 extension probe: G418's kick covers only the skip-readback RENDER batches. The
            // alias copies and the decode/composite/upload class leave GL work queued and never
            // fence either, so they feed the same readback drain. Kick them on the same exactness
            // argument as G418 (glFlush changes no result, no GL state, no command order, no
            // ownership). Never kick a readback item: it has already fenced by construction.
            {
                const bool g445Kickable =
                    item.g309PageMap != nullptr || item.g281Map != nullptr ||
                    item.g338Ct24View || item.colorWrite != nullptr ||
                    item.copyRects != nullptr;
                if (g445Kickable && g445KickAllActive())
                    glFlush();
            }
            if (g332Armed)
            {
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g332WorkStart).count());
                if (g332BackendCensusOn())
                {
                    g_g332BackendNs[g332Cls].fetch_add(workNs, std::memory_order_relaxed);
                    g_g332BackendCount[g332Cls].fetch_add(1u, std::memory_order_relaxed);
                }
            }
            if (g453Cls < 4u)
            {
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g453WorkStart).count());
                g_g453BackendOtherNs[g453Cls].fetch_add(workNs, std::memory_order_relaxed);
                g_g453BackendOtherCount[g453Cls].fetch_add(1u, std::memory_order_relaxed);
            }
            // G494 failsafe: while the sync-query elision is armed, one real glGetError every 64th
            // backend item (~1.1/frame against ~110 elided) catches anything the elided checks would
            // have caught, one frame late, and latches them all back on. Inert otherwise.
            g494SentinelTick();
            // G495: a fence has just emptied the pipeline; everything after it is a new "ahead" set.
            if (g495Fence)
                g495FenceReset();
            // G495 per-surface mutation clock (census only). Only items whose OUTPUT surface is
            // identified by an fbp tick it; anything else bumps the unknown epoch, which unproves
            // every cached window. T8/CT24 views write a texture object, not a target FBO.
            if (g495RbCensusOn())
            {
                if ((g495Cls == kG495ClsRenderSkip || g495Cls == kG495ClsRenderRb) &&
                    item.batch != nullptr)
                    g495NoteWrite(item.batch->fbp, item.batch->fboKey != 0u);
                else if (g495Cls == kG495ClsCopy || g495Cls == kG495ClsWriteColor)
                    g495NoteWrite(item.colorFbp, false);
                else if (g495Cls == kG495ClsComposite)
                    g495NoteWrite(item.g309OutputFbp, item.g309OutputFbp == 0u);
            }
            if (g324Profile)
            {
                const auto done = std::chrono::steady_clock::now();
                const uint64_t queueNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g324WorkStart - item.g324Enqueue).count());
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        done - g324WorkStart).count());
                if (g324BackendCensusOn())
                {
                    const unsigned cls = std::min<unsigned>(item.g324Class, 2u);
                    ++m_g324Count[cls];
                    m_g324QueueNs[cls] += queueNs;
                    m_g324WorkNs[cls] += workNs;
                    ++m_g324Total;
                    if ((m_g324Total % 256u) == 0u)
                    {
                        std::fprintf(stderr, "[G324:backend] total=%llu",
                                     static_cast<unsigned long long>(m_g324Total));
                        for (unsigned i = 1u; i <= 2u; ++i)
                        {
                            const uint64_t n = m_g324Count[i];
                            if (n == 0u)
                                continue;
                            std::fprintf(
                                stderr,
                                " c%u(n=%llu q=%.3fms work=%.3fms qTotal=%.3fms workTotal=%.3fms)",
                                i, static_cast<unsigned long long>(n),
                                static_cast<double>(m_g324QueueNs[i]) / 1.0e6 / n,
                                static_cast<double>(m_g324WorkNs[i]) / 1.0e6 / n,
                                static_cast<double>(m_g324QueueNs[i]) / 1.0e6,
                                static_cast<double>(m_g324WorkNs[i]) / 1.0e6);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            if (g299ProfileBatch && g299ProfileOn())
            {
                const auto done = std::chrono::steady_clock::now();
                const uint64_t queueNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g299WorkStart - item.g299Enqueue).count());
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        done - g299WorkStart).count());
                const G178Batch &b = *item.batch;
                uint64_t packetBytes =
                    static_cast<uint64_t>(b.fbPixels.size()) * sizeof(uint32_t) +
                    static_cast<uint64_t>(b.verts.size()) * sizeof(G178Vtx) +
                    static_cast<uint64_t>(b.draws.size()) * sizeof(G178Draw);
                for (const G178TexUpload &up : b.texUploads)
                    packetBytes += static_cast<uint64_t>(up.px.size()) * sizeof(uint32_t);
                if (item.depthUpload != nullptr)
                    packetBytes += static_cast<uint64_t>(item.depthUpload->size()) *
                                   sizeof(float);
                const unsigned cls =
                    (b.skipReadback ? 1u : 0u) |
                    (item.depthKey != 0u ? 2u : 0u);
                ++m_g299Count[cls];
                m_g299QueueNs[cls] += queueNs;
                m_g299WorkNs[cls] += workNs;
                m_g299PacketBytes[cls] += packetBytes;
                m_g299Draws[cls] += b.draws.size();
                ++m_g299Total;
                if ((m_g299Total % 300u) == 0u)
                {
                    std::fprintf(stderr, "[G299:backend] total=%llu",
                                 static_cast<unsigned long long>(m_g299Total));
                    for (unsigned i = 0; i < 4u; ++i)
                    {
                        const uint64_t n = m_g299Count[i];
                        if (n == 0u)
                            continue;
                        std::fprintf(
                            stderr,
                            " c%u(n=%llu q=%.3fms work=%.3fms packet=%.1fKiB draws=%.1f)",
                            i, static_cast<unsigned long long>(n),
                            static_cast<double>(m_g299QueueNs[i]) / 1.0e6 / n,
                            static_cast<double>(m_g299WorkNs[i]) / 1.0e6 / n,
                            static_cast<double>(m_g299PacketBytes[i]) / 1024.0 / n,
                            static_cast<double>(m_g299Draws[i]) / n);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            // G447: publish completion to a spinning caller BEFORE satisfying the promise (see the
            // lifetime argument on QueueItem::doneFlag). Exact: the caller still takes its result
            // from the future, so nothing about the value or its ordering changes.
            if (item.doneFlag != nullptr)
                item.doneFlag->store(1u, std::memory_order_release);
            item.promise->set_value(ok);
        }

        if (m_g545Available && !g566VuAuthorityRequested())
            reportG545Status(true);
        m_ready.store(false, std::memory_order_release);
        for (auto &kv : m_fbos)
            destroyFbo(kv.second);
        m_fbos.clear();
        for (auto &kv : m_depths)
            if (kv.second.tex != 0)
                glDeleteTextures(1, &kv.second.tex);
        m_depths.clear();
        for (auto &kv : m_texCache)
            if (kv.second.id != 0)
                glDeleteTextures(1, &kv.second.id);
        m_texCache.clear();
        m_texBytes = 0;
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.clear();
        }
        if (m_vbo != 0)
            m_f.glDeleteBuffers_(1, &m_vbo);
        if (m_vao != 0)
            m_f.glDeleteVertexArrays_(1, &m_vao);
        if (m_prog != 0)
            m_f.glDeleteProgram_(m_prog);
        if (m_progG560 != 0)
            m_f.glDeleteProgram_(m_progG560);
        if (m_progExact != 0)
            m_f.glDeleteProgram_(m_progExact);
        if (m_g541NullProg != 0)
            m_f.glDeleteProgram_(m_g541NullProg);
        if (m_g541StateTex != 0)
            glDeleteTextures(1, &m_g541StateTex);
        if (m_g629Prog != 0)
            m_f.glDeleteProgram_(m_g629Prog);
        if (m_g630ViewProg != 0)
            m_f.glDeleteProgram_(m_g630ViewProg);
        if (m_g630UploadProg != 0)
            m_f.glDeleteProgram_(m_g630UploadProg);
        if (m_g630T8Prog != 0)
            m_f.glDeleteProgram_(m_g630T8Prog);
        const GLuint g629Textures[] = {
            m_g629ColorTex, m_g629DepthTex, m_g629RecTex, m_g629TexDataTex,
            m_g629TexDescTex, m_g629TileOffTex, m_g629TileRefTex, m_g630UploadTex,
            m_g630ViewMapTex, m_g630T8MapTex, m_g630T8ClutTex
        };
        for (GLuint tex : g629Textures)
            if (tex != 0u)
                glDeleteTextures(1, &tex);
        if (m_g570Prog != 0)
            m_f.glDeleteProgram_(m_g570Prog);
        const GLuint g570Textures[] = {
            m_g570ColorTex, m_g570DepthTex, m_g570TriTex,
            m_g570SpanTex, m_g570TileOffsetTex, m_g570TileTriTex
        };
        for (GLuint tex : g570Textures)
            if (tex != 0u)
                glDeleteTextures(1, &tex);
        if (m_g545Prog != 0)
            m_f.glDeleteProgram_(m_g545Prog);
        if (m_g545CommandTex != 0)
            glDeleteTextures(1, &m_g545CommandTex);
        if (m_g545PayloadTex != 0)
            glDeleteTextures(1, &m_g545PayloadTex);
        if (m_g545WorkspaceTex != 0)
            glDeleteTextures(1, &m_g545WorkspaceTex);
        if (m_g566OutputTex != 0)
            glDeleteTextures(1, &m_g566OutputTex);
        if (m_g627Prog != 0)
            m_f.glDeleteProgram_(m_g627Prog);
        if (m_g627ValTex != 0)
            glDeleteTextures(1, &m_g627ValTex);
        if (m_g627MaskTex != 0)
            glDeleteTextures(1, &m_g627MaskTex);
        if (m_g281Prog != 0)
            m_f.glDeleteProgram_(m_g281Prog);
        if (m_g281MapTex != 0)
            glDeleteTextures(1, &m_g281MapTex);
        if (m_g281ClutTex != 0)
            glDeleteTextures(1, &m_g281ClutTex);
        if (m_g281Fbo != 0)
            m_f.glDeleteFramebuffers_(1, &m_g281Fbo);
        if (m_g309Prog != 0)
            m_f.glDeleteProgram_(m_g309Prog);
        if (m_g309MapTex != 0)
            glDeleteTextures(1, &m_g309MapTex);
        if (m_g309BaseTex != 0)
            glDeleteTextures(1, &m_g309BaseTex);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }

    bool setupG545LiveShadow()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        std::string why;
        if (!g545LinkProgram(m_f, m_g545Prog, m_locG545CommandCount, why))
        {
            std::fprintf(stderr, "[G545:live] setup failed: %s\n", why.c_str());
            if (m_g545Prog != 0u)
                m_f.glDeleteProgram_(m_g545Prog);
            m_g545Prog = 0u;
            return false;
        }

        while (glGetError() != GL_NO_ERROR) {}
        const uint32_t rows = (G545_LIVE_WORKSPACE_WORDS + G542_IMAGE_WIDTH - 1u) /
                              G542_IMAGE_WIDTH;
        std::vector<uint32_t> zero(static_cast<size_t>(rows) * G542_IMAGE_WIDTH, 0u);
        glGenTextures(1, &m_g545WorkspaceTex);
        glBindTexture(GL_TEXTURE_2D, m_g545WorkspaceTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                     static_cast<GLsizei>(G542_IMAGE_WIDTH), static_cast<GLsizei>(rows), 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, zero.data());
        const GLenum err = glGetError();
        if (m_g545WorkspaceTex == 0u || err != GL_NO_ERROR)
        {
            std::fprintf(stderr, "[G545:live] workspace setup failed gl=0x%x\n",
                         static_cast<unsigned>(err));
            return false;
        }

        const uint32_t g566Rows = (G566_OUTPUT_WORDS + G542_IMAGE_WIDTH - 1u) /
                                  G542_IMAGE_WIDTH;
        std::vector<uint32_t> g566Zero(static_cast<size_t>(g566Rows) * G542_IMAGE_WIDTH, 0u);
        glGenTextures(1, &m_g566OutputTex);
        glBindTexture(GL_TEXTURE_2D, m_g566OutputTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                     static_cast<GLsizei>(G542_IMAGE_WIDTH), static_cast<GLsizei>(g566Rows), 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, g566Zero.data());
        const GLenum g566Err = glGetError();
        if (m_g566OutputTex == 0u || g566Err != GL_NO_ERROR)
        {
            std::fprintf(stderr, "[G566:authority] output setup failed gl=0x%x\n",
                         static_cast<unsigned>(g566Err));
            return false;
        }
        m_g566Readback.resize(static_cast<size_t>(g566Rows) * G542_IMAGE_WIDTH);

        m_g545CheckInterval = [] {
            const char *v = std::getenv("DC2_G545_STATUS_CHECK_KICKS");
            if (v == nullptr || v[0] == '\0')
                return 16384ull;
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(v, &end, 0);
            if (end == v)
                return 16384ull;
            return std::max<unsigned long long>(256ull,
                std::min<unsigned long long>(1048576ull, parsed));
        }();
        m_g545NextCheck = m_g545CheckInterval;
        return true;
    }

    void reportG545Status(bool final)
    {
        const uint64_t executed = m_g545Executed.load(std::memory_order_relaxed);
        if (executed == 0ull || (final && executed == m_g545Verified))
            return;
        const uint32_t rows = (G545_LIVE_WORKSPACE_WORDS + G542_IMAGE_WIDTH - 1u) /
                              G542_IMAGE_WIDTH;
        std::vector<uint32_t> image(static_cast<size_t>(rows) * G542_IMAGE_WIDTH, 0u);
        while (glGetError() != GL_NO_ERROR) {}
        glBindTexture(GL_TEXTURE_2D, m_g545WorkspaceTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, image.data());
        const GLenum err = glGetError();
        ++m_g545Readbacks;
        if (err != GL_NO_ERROR || image[0] != G545_LIVE_STATUS_MAGIC || image[1] != 1u)
        {
            std::fprintf(stderr,
                         "[G545:live] CHECK FAIL status/readback gl=0x%x magic=%08x version=%u; shadow disabled, CPU path unchanged\n",
                         static_cast<unsigned>(err), image[0], image[1]);
            m_g545Disabled.store(true, std::memory_order_release);
            return;
        }
        const uint64_t gpuExecuted = static_cast<uint64_t>(image[8]) |
                                     (static_cast<uint64_t>(image[9]) << 32u);
        const uint64_t lastSequence = static_cast<uint64_t>(image[10]) |
                                      (static_cast<uint64_t>(image[11]) << 32u);
        if (image[2] != 0u || gpuExecuted != executed)
        {
            const uint64_t failSequence = static_cast<uint64_t>(image[3]) |
                                          (static_cast<uint64_t>(image[4]) << 32u);
            std::fprintf(stderr,
                         "[G545:live] CHECK FAIL errors=%08x failSeq=%llu batchRec=%u pc=%04x word=%08x gpuExec=%llu hostExec=%llu; shadow disabled, CPU path unchanged\n",
                         image[2], static_cast<unsigned long long>(failSequence), image[5],
                          image[6], image[7], static_cast<unsigned long long>(gpuExecuted),
                          static_cast<unsigned long long>(executed));
            if ((image[2] & 0x20u) != 0u)
            {
                std::fprintf(stderr,
                             "[G545:live] packet detail gpu=%02x cpu=%02x offset=%u\n",
                             (image[7] >> 24u) & 0xffu, (image[7] >> 16u) & 0xffu,
                             image[7] & 0xffffu);
            }
            m_g545Disabled.store(true, std::memory_order_release);
            return;
        }

        m_g545Verified = executed;
        std::fprintf(stderr,
                     "[G545:live] %s PASS kicks=%llu lastSeq=%llu batches=%llu maxBatch=%llu packets=%u dataChecks=%u seeds=%u readbacks=%llu qPeak=%llu CPU-authoritative=1\n",
                     final ? "FINAL" : "CHECK", static_cast<unsigned long long>(executed),
                     static_cast<unsigned long long>(lastSequence),
                     static_cast<unsigned long long>(m_g545Batches),
                     static_cast<unsigned long long>(m_g545MaxBatch), image[13], image[14],
                     image[15], static_cast<unsigned long long>(m_g545Readbacks),
                     static_cast<unsigned long long>(m_g545HighWater.load(std::memory_order_relaxed)));
    }

    bool runG545LiveBatch(const G545LiveBatch &batch)
    {
        const bool authority = !batch.authorityResults.empty();
        const auto g566T0 = authority ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
        std::vector<uint32_t> commands;
        std::vector<uint32_t> payload;
        std::string why;
        if (!g545PackBatch(batch, commands, payload, why))
        {
            std::fprintf(stderr, "[G545:live] batch rejected: %s; shadow disabled\n", why.c_str());
            m_g545Disabled.store(true, std::memory_order_release);
            g566PublishBatchFailure(batch);
            return false;
        }
        if (m_g545Batches == 0ull)
            std::fprintf(stderr, authority
                         ? "[G566:authority] first dispatch commands=%zu payloadWords=%zu watchdogCap=%zu\n"
                         : "[G545:live] first dispatch commands=%zu payloadWords=%zu watchdogCap=%zu\n",
                         batch.blobs.size(), payload.size(), G545_LIVE_MAX_BATCH_COMMANDS);
        while (glGetError() != GL_NO_ERROR) {}
        if (!g545UploadImage(m_g545CommandTex, m_g545CommandRows, commands, why) ||
            !g545UploadImage(m_g545PayloadTex, m_g545PayloadRows, payload, why))
        {
            std::fprintf(stderr, "[G545:live] upload failed: %s; shadow disabled\n", why.c_str());
            m_g545Disabled.store(true, std::memory_order_release);
            g566PublishBatchFailure(batch);
            return false;
        }

        std::vector<uint32_t> seedWorkspace;
        if (!g545BuildSeedWorkspace(commands, payload, seedWorkspace, why))
        {
            std::fprintf(stderr, "[G545:live] seed rejected: %s; shadow disabled\n", why.c_str());
            m_g545Disabled.store(true, std::memory_order_release);
            g566PublishBatchFailure(batch);
            return false;
        }
        if (!seedWorkspace.empty())
        {
            const uint32_t rows = (G545_LIVE_WORKSPACE_WORDS + G542_IMAGE_WIDTH - 1u) /
                                  G542_IMAGE_WIDTH;
            glBindTexture(GL_TEXTURE_2D, m_g545WorkspaceTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            static_cast<GLsizei>(G542_IMAGE_WIDTH),
                            static_cast<GLsizei>(rows), GL541_RED_INTEGER,
                            GL_UNSIGNED_INT, seedWorkspace.data());
            const GLenum seedErr = glGetError();
            if (seedErr != GL_NO_ERROR)
            {
                std::fprintf(stderr,
                             "[G545:live] seed upload failed gl=0x%x; shadow disabled\n",
                             static_cast<unsigned>(seedErr));
                m_g545Disabled.store(true, std::memory_order_release);
                g566PublishBatchFailure(batch);
                return false;
            }
        }

        m_f.glUseProgram_(m_g545Prog);
        m_e.glBindImageTexture_(2u, m_g545CommandTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(3u, m_g545PayloadTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(4u, m_g545WorkspaceTex, 0, GL_FALSE, 0,
                                GL178_READ_WRITE, GL541_R32UI);
        m_e.glBindImageTexture_(5u, m_g566OutputTex, 0, GL_FALSE, 0,
                                GL178_READ_WRITE, GL541_R32UI);
        m_f.glUniform1i_(m_locG545CommandCount, static_cast<GLint>(batch.blobs.size()));
        m_e.glDispatchCompute_(1u, 1u, 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT);
        const GLenum err = glGetError();
        const auto g566T1 = authority ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
        if (err != GL_NO_ERROR)
        {
            std::fprintf(stderr,
                         "[G545:live] dispatch failed gl=0x%x; shadow disabled, CPU path unchanged\n",
                         static_cast<unsigned>(err));
            m_g545Disabled.store(true, std::memory_order_release);
            g566PublishBatchFailure(batch);
            return false;
        }

        if (authority)
        {
            const uint32_t outputRows = (G566_OUTPUT_WORDS + G542_IMAGE_WIDTH - 1u) /
                                        G542_IMAGE_WIDTH;
            if (m_g566Readback.size() != static_cast<size_t>(outputRows) * G542_IMAGE_WIDTH)
                m_g566Readback.resize(static_cast<size_t>(outputRows) * G542_IMAGE_WIDTH);
            while (glGetError() != GL_NO_ERROR) {}
            glBindTexture(GL_TEXTURE_2D, m_g566OutputTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT,
                          m_g566Readback.data());
            const GLenum readErr = glGetError();
            if (readErr != GL_NO_ERROR ||
                !g566DecodeAuthorityBatch(batch, m_g566Readback, m_g566Packets,
                                          m_g566PacketBytes, why))
            {
                std::fprintf(stderr,
                             "[G566:authority] output failure gl=0x%x %s; disabled\n",
                             static_cast<unsigned>(readErr), why.c_str());
                m_g545Disabled.store(true, std::memory_order_release);
                g566PublishBatchFailure(batch);
                return false;
            }
            const auto g566T2 = std::chrono::steady_clock::now();
            if (m_g545Batches < 12ull)
                std::fprintf(stderr,
                             "[G566:batch] n=%zu dispatchMs=%.3f readMs=%.3f usedWords=%u packets=%llu bytes=%llu\n",
                             batch.blobs.size(),
                             std::chrono::duration<double, std::milli>(g566T1 - g566T0).count(),
                             std::chrono::duration<double, std::milli>(g566T2 - g566T1).count(),
                             m_g566Readback[3],
                             static_cast<unsigned long long>(m_g566Packets),
                             static_cast<unsigned long long>(m_g566PacketBytes));
        }

        ++m_g545Batches;
        m_g545MaxBatch = std::max<uint64_t>(m_g545MaxBatch, batch.blobs.size());
        const uint64_t executed = m_g545Executed.fetch_add(batch.blobs.size(),
            std::memory_order_relaxed) + batch.blobs.size();
        if (!authority && executed >= m_g545NextCheck)
        {
            reportG545Status(false);
            m_g545NextCheck = executed + m_g545CheckInterval;
        }
        return !m_g545Disabled.load(std::memory_order_acquire);
    }


    // ======================= G629: the exact CPU-band-replay compute kernel =======================
    bool setupG629()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        // ⛔ MSVC caps a single string literal at 16,380 bytes (C2026). The shader is
        // larger, so it is assembled from several raw-string chunks at runtime; the
        // chunk boundaries are line boundaries and therefore cannot alter the GLSL.
        static const std::string kCSStr = std::string(
            R"GLSL(#version 430 core
#extension GL_ARB_gpu_shader_fp64 : enable
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) uniform uimage2D gColor;    // RAW VRAM words of the target's page window
layout(r32ui, binding=1) uniform uimage2D gDepth;    // G403 private mirror, linear 512x512
layout(r32ui, binding=2) readonly uniform uimage2D gRecs;
layout(r32ui, binding=3) readonly uniform uimage2D gTexData;
layout(r32ui, binding=4) readonly uniform uimage2D gTexDesc;
layout(r32ui, binding=5) readonly uniform uimage2D gTileOff;
layout(r32ui, binding=6) readonly uniform uimage2D gTileRef;
layout(rgba8, binding=7) writeonly uniform image2D gLinearView;

uniform int uWidth;      // target width in pixels (fbw * 64)
uniform int uHeight;     // maxY + 1
uniform int uTilesX;
uniform int uBlock;      // framePageBaseToBlock(fbp)
uniform int uFbw;
uniform int uFirstPage;  // first VRAM page uploaded into gColor
uniform int uWriteView;
uniform int uViewHeight;

// ---- the CT32 destination swizzle: GSPSMCT32's own two tables, verbatim ----
const int kBlk32[32] = int[32](
     0, 1, 4, 5,16,17,20,21,
     2, 3, 6, 7,18,19,22,23,
     8, 9,12,13,24,25,28,29,
    10,11,14,15,26,27,30,31);
const int kCol32[64] = int[64](
     0, 1, 4, 5, 8, 9,12,13,
     2, 3, 6, 7,10,11,14,15,
    16,17,20,21,24,25,28,29,
    18,19,22,23,26,27,30,31,
    32,33,36,37,40,41,44,45,
    34,35,38,39,42,43,46,47,
    48,49,52,53,56,57,60,61,
    50,51,54,55,58,59,62,63);

ivec2 dc1024(uint i) { return ivec2(int(i & 1023u), int(i >> 10u)); }
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
uint recWord(uint i)   { return imageLoad(gRecs,    dc1024(i)).r; }
uint texWord(uint i)   { return imageLoad(gTexData, dc2048(i)).r; }
uint descWord(uint i)  { return imageLoad(gTexDesc, dc1024(i)).r; }
uint offWord(uint i)   { return imageLoad(gTileOff, dc1024(i)).r; }
uint refWord(uint i)   { return imageLoad(gTileRef, dc1024(i)).r; }
float recF(uint i)     { return uintBitsToFloat(recWord(i)); }

// Word index inside the uploaded colour window. Mirrors GSPSMCT32::addrPSMCT32 exactly, then
// rebases to uFirstPage. The CPU proved the whole batch fits before dispatching.
uint ct32Word(int x, int y)
{
    int pagesPerRow = (uFbw != 0) ? uFbw : 1;
    int page    = (uBlock >> 5) + (y >> 5) * pagesPerRow + (x >> 6);
    int blockId = (uBlock & 31) + kBlk32[((y >> 3) & 3) * 8 + ((x >> 3) & 7)];
    int off     = ((page - uFirstPage) << 13) + ((blockId >> 5) << 13)
                + (blockId & 31) * 256 + kCol32[(y & 7) * 8 + (x & 7)] * 4;
    return uint(off) >> 2u;
}

int clampU8i(int v) { return (v < 0) ? 0 : ((v > 255) ? 255 : v); }

// rasterizer_row_pool.inc:181, statement for statement.
int wrapCoord(int c, int mode, int size, int rmin, int rmax)
{
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return (c < 0) ? 0 : ((c > size - 1) ? size - 1 : c);
    if (mode == 2) return (c < rmin) ? rmin : ((c > rmax) ? rmax : c);
    return int((uint(c) & uint(rmin)) | uint(rmax));
}

// g609Lerp8 (g609_tri_scan.inc:415), statement for statement.
int lerp8(int c00, int c10, int c01, int c11, float fx, float fy)
{
    // Every mul-then-add is split into its own `precise` temporary. Written as one expression the
    // NVIDIA compiler contracts `a + b*c` into a single FMA, whose rounding differs from MSVC's
    // separate mul and add under the default /fp:precise -- that showed up as a 2-LSB blue-channel
    // difference on 3 words of 217,088 in the second oracle run.
    precise float d10 = float(c10) - float(c00);
    precise float m10 = d10 * fx;
    precise float top = float(c00) + m10;
    precise float d11 = float(c11) - float(c01);
    precise float m11 = d11 * fx;
    precise float bottom = float(c01) + m11;
    precise float dtb = bottom - top;
    precise float mtb = dtb * fy;
    precise float v = top + mtb;
    int n = int(v);
    precise float frac = v - float(n);
    return clampU8i((frac >= 0.5) ? (n + 1) : n);
}

// g364FogLerp (rasterizer_tilebinning_and_probes.inc:236): unsigned, +127, /255.
int fogLerp(int c, uint f, uint fc)
{
    return int((uint(c) * f + fc * (255u - f) + 127u) / 255u);
}

// g605CommitWord (g605_tri_span.inc:315), statement for statement, with `pick` inlined.
int pickC(int sel, int cs, int cd) { return (sel == 0) ? cs : ((sel == 1) ? cd : 0); }

// ⛔⛔ GLSL leaves `>>` on a NEGATIVE signed int UNDEFINED; C++ performs an arithmetic shift, i.e.
// floor division. The GS blend's `((A - B) * C) >> 7` is negative whenever the destination exceeds
// the source, so the two languages disagree exactly there. This is `floor(v / 128)` written so it
// is defined in GLSL for every input -- it was worth 3 words of 217,088 (a 2-LSB red-channel
// difference) that survived both the sampler fix and the FMA decomposition.
int asr7(int v)
{
    return (v >= 0) ? (v >> 7) : -int((uint(-v) + 127u) >> 7u);
}

uint commitWord(uint blendPack, bool abe, uint dst, int sr, int sg, int sb, int sa)
{
    int r = sr, g = sg, b = sb;
    if (abe)
    {
        int dr = int(dst & 255u);
        int dg = int((dst >> 8u) & 255u);
        int db = int((dst >> 16u) & 255u);
        int da = int((dst >> 24u) & 255u);
        int asel = int(blendPack & 3u);
        int bsel = int((blendPack >> 2u) & 3u);
        int csel = int((blendPack >> 4u) & 3u);
        int dsel = int((blendPack >> 6u) & 3u);
        int fix  = int((blendPack >> 8u) & 255u);
        int cA = (csel == 0) ? sa : ((csel == 1) ? da : fix);
        r = clampU8i(asr7((pickC(asel, sr, dr) - pickC(bsel, sr, dr)) * cA) + pickC(dsel, sr, dr));
        g = clampU8i(asr7((pickC(asel, sg, dg) - pickC(bsel, sg, dg)) * cA) + pickC(dsel, sg, dg));
        b = clampU8i(asr7((pickC(asel, sb, db) - pickC(bsel, sb, db)) * cA) + pickC(dsel, sb, db));
    }
    return uint(r) | (uint(g) << 8u) | (uint(b) << 16u) | (uint(sa) << 24u);
}

void main()
{
    ivec2 ip = ivec2(gl_GlobalInvocationID.xy);
    if (ip.x >= uWidth || ip.y >= uHeight)
        return;

    uint tile  = uint((ip.y >> 4) * uTilesX + (ip.x >> 4));
    uint first = offWord(tile);
    uint last  = offWord(tile + 1u);
    if (first == last)
        return;

    // ⭐ The destination word and the depth entry live in REGISTERS for the whole record loop.
    // Each pixel belongs to exactly one invocation, so GS's per-pixel ordering is preserved by the
    // loop order alone — no atomics, no inter-invocation visibility, and one store at the end.
    uint colorIdx = ct32Word(ip.x, ip.y);
    uint dstWord  = imageLoad(gColor, dc2048(colorIdx)).r;
    uint zcur     = imageLoad(gDepth, ivec2(ip.x, ip.y)).r;
    bool zDirty = false;

    for (uint ref = first; ref < last; ++ref)
    {
        uint rec  = refWord(ref);
        uint base = rec * 48u;

        uint kind  = recWord(base + 0u);
        uint flags = recWord(base + 1u);
        int  minX  = int(recWord(base + 2u));
        int  maxX  = int(recWord(base + 3u));
        int  minY  = int(recWord(base + 4u));
        int  maxY  = int(recWord(base + 5u));
)GLSL")
            + R"GLSL(        if (ip.x < minX || ip.x > maxX || ip.y < minY || ip.y > maxY)
            continue;

        bool abe = (flags & 32u) != 0u;
        uint blendPack = recWord(base + 32u);

        if (kind == 1u)
        {
            // ---- untextured sprite: flat colour over the clipped rect ----
            uint c = recWord(base + 42u);
            dstWord = commitWord(blendPack, abe, dstWord,
                                 int(c & 255u), int((c >> 8u) & 255u),
                                 int((c >> 16u) & 255u), int((c >> 24u) & 255u));
            continue;
        }

        // ---- coverage: g609TriScanRowT's own expression, decomposed ----
        precise float a0 = recF(base + 6u);
        precise float b0 = recF(base + 7u);
        precise float a1 = recF(base + 8u);
        precise float b1 = recF(base + 9u);
        precise float fx2 = recF(base + 10u);
        precise float fy2 = recF(base + 11u);
        precise float winding  = recF(base + 12u);
        precise float invDenom = recF(base + 13u);
        precise float eps      = recF(base + 14u);

        precise float px  = float(ip.x) + 0.5;
        precise float py  = float(ip.y) + 0.5;
        precise float px2 = px - fx2;
        precise float py2 = py - fy2;

        precise float w0m0 = a0 * px2;
        precise float w0m1 = b0 * py2;
        precise float w0a  = w0m0 + w0m1;
        precise float w0w  = w0a * winding;
        precise float w0   = w0w * invDenom;

        precise float w1m0 = a1 * px2;
        precise float w1m1 = b1 * py2;
        precise float w1a  = w1m0 + w1m1;
        precise float w1w  = w1a * winding;
        precise float w1   = w1w * invDenom;

        precise float oneMinusW0 = 1.0 - w0;
        precise float w2 = oneMinusW0 - w1;

        if (w0 < -eps || w1 < -eps || w2 < -eps)
            continue;

        // ---- depth (ZPRIV only; the DEAD arm has no depth stage at all) ----
        bool zpriv  = (flags & 4u) != 0u;
        bool zwrite = (flags & 8u) != 0u;
        uint zi = 0u;
        if (zpriv)
        {
            precise float z0 = recF(base + 15u);
            precise float z1 = recF(base + 16u);
            precise float z2 = recF(base + 17u);
            precise float zm0 = z0 * w0;
            precise float zm1 = z1 * w1;
            precise float za  = zm0 + zm1;
            precise float zm2 = z2 * w2;
            precise float zf  = za + zm2;
            if (zf < 0.0) zf = 0.0;
            if (zf > 16777215.0) zf = 16777215.0;
            zi = uint(zf);
            if (!(zi >= zcur))
                continue;
        }

        // ---- Gouraud shade ----
        uint c0 = recWord(base + 18u);
        uint c1 = recWord(base + 19u);
        uint c2 = recWord(base + 20u);

        precise float rm0 = float(c0 & 255u) * w0;
        precise float rm1 = float(c1 & 255u) * w1;
        precise float ra  = rm0 + rm1;
        precise float rm2 = float(c2 & 255u) * w2;
        precise float rf  = ra + rm2;
        int shadeR = clampU8i(int(rf));

        precise float gm0 = float((c0 >> 8u) & 255u) * w0;
        precise float gm1 = float((c1 >> 8u) & 255u) * w1;
        precise float ga  = gm0 + gm1;
        precise float gm2 = float((c2 >> 8u) & 255u) * w2;
        precise float gf  = ga + gm2;
        int shadeG = clampU8i(int(gf));

        precise float bm0 = float((c0 >> 16u) & 255u) * w0;
        precise float bm1 = float((c1 >> 16u) & 255u) * w1;
        precise float ba  = bm0 + bm1;
        precise float bm2 = float((c2 >> 16u) & 255u) * w2;
        precise float bf  = ba + bm2;
        int shadeB = clampU8i(int(bf));

        precise float am0 = float((c0 >> 24u) & 255u) * w0;
        precise float am1 = float((c1 >> 24u) & 255u) * w1;
        precise float aa  = am0 + am1;
        precise float am2 = float((c2 >> 24u) & 255u) * w2;
        precise float af  = aa + am2;
        int shadeA = clampU8i(int(af));

        int fr = shadeR, fg = shadeG, fb = shadeB, fa = shadeA;

        if ((flags & 1u) != 0u)
        {
            // ---- the T8 bilinear tap, through the CPU-decoded linear image ----
            uint ti = recWord(base + 33u) * 10u;
            int  tW    = int(descWord(ti + 0u));
            int  tH    = int(descWord(ti + 1u));
            int  wrapU = int(descWord(ti + 2u));
            int  wrapV = int(descWord(ti + 3u));
            int  rminU = int(descWord(ti + 4u));
            int  rmaxU = int(descWord(ti + 5u));
            uint tOff  = descWord(ti + 6u);
            int  rminV = int(descWord(ti + 7u));
            int  rmaxV = int(descWord(ti + 8u));

            precise float texUf;
            precise float texVf;
            if ((flags & 2u) != 0u)
            {
                // FST: u/v are 12.4 fixed point, interpolated then /16
                precise float um0 = float(recWord(base + 36u)) * w0;
                precise float um1 = float(recWord(base + 38u)) * w1;
                precise float ua  = um0 + um1;
                precise float um2 = float(recWord(base + 40u)) * w2;
                precise float uf  = ua + um2;
                precise float vm0 = float(recWord(base + 37u)) * w0;
                precise float vm1 = float(recWord(base + 39u)) * w1;
                precise float va  = vm0 + vm1;
                precise float vm2 = float(recWord(base + 41u)) * w2;
                precise float vf  = va + vm2;
                texUf = float(uint(uf)) / 16.0;
                texVf = float(uint(vf)) / 16.0;
            }
            else
            {
                precise float s0 = recF(base + 21u), t0 = recF(base + 22u), q0 = recF(base + 23u);
                precise float s1 = recF(base + 24u), t1 = recF(base + 25u), q1 = recF(base + 26u);
                precise float s2 = recF(base + 27u), t2 = recF(base + 28u), q2 = recF(base + 29u);
                precise float sm0 = s0 * w0; precise float sm1 = s1 * w1;
                precise float sa2 = sm0 + sm1; precise float sm2 = s2 * w2;
                precise float is = sa2 + sm2;
                precise float tm0 = t0 * w0; precise float tm1 = t1 * w1;
                precise float ta2 = tm0 + tm1; precise float tm2 = t2 * w2;
                precise float it = ta2 + tm2;
                precise float qm0 = q0 * w0; precise float qm1 = q1 * w1;
                precise float qa2 = qm0 + qm1; precise float qm2 = q2 * w2;
                precise float iq = qa2 + qm2;
                // fabsQ: NOT an absolute value — returns q unless |q| <= 1e-8.
                precise float qq = (abs(iq) > 1.0e-8) ? iq : 1.0;
                // GLSL does NOT require correctly-rounded float DIVISION: the spec allows
                // 2.5 ULP and NVIDIA's compiler may emit a reciprocal approximation, while
                // C++'s `1.0f / q` is IEEE correctly-rounded. A 1-ULP invQ moves the
                // interpolated shade across an integer boundary, int() truncates one lower,
                // and MODULATE against a ~255 texel turns that into a 2-LSB channel error.
)GLSL"
            + R"GLSL(                // Evaluating in binary64 and rounding once to binary32 IS correctly rounded
                // for binary32 operands, so this reproduces the CPU divide exactly.
                precise float invQ = float(1.0lf / double(qq));
                precise float su0 = is * invQ;
                precise float sv0 = it * invQ;
                texUf = su0 * float(tW);
                texVf = sv0 * float(tH);
            }

            precise float sampleU = texUf - 0.5;
            precise float sampleV = texVf - 0.5;
            int u0i = int(floor(sampleU));
            int v0i = int(floor(sampleV));
            precise float fxw = sampleU - float(u0i);
            precise float fyw = sampleV - float(v0i);

            int su0 = wrapCoord(u0i,     wrapU, tW, rminU, rmaxU);
            int su1 = wrapCoord(u0i + 1, wrapU, tW, rminU, rmaxU);
            int sv0i = wrapCoord(v0i,     wrapV, tH, rminV, rmaxV);
            int sv1i = wrapCoord(v0i + 1, wrapV, tH, rminV, rmaxV);

            uint t00 = texWord(tOff + uint(sv0i * tW + su0));
            uint t10 = texWord(tOff + uint(sv0i * tW + su1));
            uint t01 = texWord(tOff + uint(sv1i * tW + su0));
            uint t11 = texWord(tOff + uint(sv1i * tW + su1));

            int tr = lerp8(int(t00 & 255u), int(t10 & 255u), int(t01 & 255u), int(t11 & 255u), fxw, fyw);
            int tg = lerp8(int((t00 >> 8u) & 255u), int((t10 >> 8u) & 255u),
                           int((t01 >> 8u) & 255u), int((t11 >> 8u) & 255u), fxw, fyw);
            int tb = lerp8(int((t00 >> 16u) & 255u), int((t10 >> 16u) & 255u),
                           int((t01 >> 16u) & 255u), int((t11 >> 16u) & 255u), fxw, fyw);
            int ta = lerp8(int((t00 >> 24u) & 255u), int((t10 >> 24u) & 255u),
                           int((t01 >> 24u) & 255u), int((t11 >> 24u) & 255u), fxw, fyw);

            // combineTexture, tfx == MODULATE and tcc == 1, both pinned by the admission.
            fr = clampU8i((tr * shadeR) >> 7);
            fg = clampU8i((tg * shadeG) >> 7);
            fb = clampU8i((tb * shadeB) >> 7);
            fa = clampU8i((ta * shadeA) >> 7);
        }

        if ((flags & 16u) != 0u)
        {
            uint fogPack = recWord(base + 30u);
            uint fogCol  = recWord(base + 31u);
            precise float fm0 = float(fogPack & 255u) * w0;
            precise float fm1 = float((fogPack >> 8u) & 255u) * w1;
            precise float faa = fm0 + fm1;
            precise float fm2 = float((fogPack >> 16u) & 255u) * w2;
            precise float ff  = faa + fm2;
            uint fv = uint(clampU8i(int(ff + 0.5)));
            fr = fogLerp(fr, fv, fogCol & 255u);
            fg = fogLerp(fg, fv, (fogCol >> 8u) & 255u);
            fb = fogLerp(fb, fv, (fogCol >> 16u) & 255u);
        }

        dstWord = commitWord(blendPack, abe, dstWord, fr, fg, fb, fa);
        if (zpriv && zwrite)
        {
            zcur = zi;
            zDirty = true;
        }
    }

    imageStore(gColor, dc2048(colorIdx), uvec4(dstWord, 0u, 0u, 0u));
    if (uWriteView != 0)
    {
        vec4 c = vec4(float(dstWord & 255u), float((dstWord >> 8u) & 255u),
                      float((dstWord >> 16u) & 255u), float((dstWord >> 24u) & 255u)) /
                 255.0;
        imageStore(gLinearView, ivec2(ip.x, uViewHeight - 1 - ip.y), c);
    }
    if (zDirty)
        imageStore(gDepth, ivec2(ip.x, ip.y), uvec4(zcur, 0u, 0u, 0u));
}
)GLSL";
        const char *kCS = kCSStr.c_str();

        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G629:gpu] compute compile failed: %s\n", log.c_str());
            return false;
        }
        m_g629Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g629Prog, cs);
        m_f.glLinkProgram_(m_g629Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g629Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g629Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(m_g629Prog, static_cast<GLsizei>(buf.size()), nullptr,
                                     buf.data());
            std::fprintf(stderr, "[G629:gpu] compute link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g629Prog);
            m_g629Prog = 0u;
            return false;
        }
        m_locG629Width     = m_f.glGetUniformLocation_(m_g629Prog, "uWidth");
        m_locG629Height    = m_f.glGetUniformLocation_(m_g629Prog, "uHeight");
        m_locG629TilesX    = m_f.glGetUniformLocation_(m_g629Prog, "uTilesX");
        m_locG629Block     = m_f.glGetUniformLocation_(m_g629Prog, "uBlock");
        m_locG629Fbw       = m_f.glGetUniformLocation_(m_g629Prog, "uFbw");
        m_locG629FirstPage = m_f.glGetUniformLocation_(m_g629Prog, "uFirstPage");
        m_locG629WriteView = m_f.glGetUniformLocation_(m_g629Prog, "uWriteView");
        m_locG629ViewHeight = m_f.glGetUniformLocation_(m_g629Prog, "uViewHeight");
        if (m_locG629Width < 0 || m_locG629Height < 0 || m_locG629TilesX < 0 ||
            m_locG629Block < 0 || m_locG629Fbw < 0 || m_locG629FirstPage < 0 ||
            m_locG629WriteView < 0 || m_locG629ViewHeight < 0)
        {
            std::fprintf(stderr, "[G629:gpu] missing uniform location\n");
            m_f.glDeleteProgram_(m_g629Prog);
            m_g629Prog = 0u;
            return false;
        }
        return setupG630View();
    }

    // Upload helper: pads to a rectangle of `width` words and grows the allocation monotonically,
    // so a steady-state batch re-uses the same texture object (glTexSubImage2D) instead of
    // reallocating -- the G570 pattern, which is why its per-batch upload is a copy and not a
    // driver allocation.
    bool g629Upload(GLuint &tex, uint32_t &allocRows, uint32_t width,
                    const std::vector<uint32_t> &words, std::vector<uint32_t> &pad)
    {
        if (width == 0u)
            return false;
        const uint32_t rows = static_cast<uint32_t>((words.size() + width - 1u) / width);
        const uint32_t need = (rows == 0u) ? 1u : rows;
        pad.assign(static_cast<size_t>(width) * need, 0u);
        if (!words.empty())
            std::copy(words.begin(), words.end(), pad.begin());
        if (tex == 0u)
        {
            glGenTextures(1, &tex);
            if (tex == 0u)
                return false;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        else
            glBindTexture(GL_TEXTURE_2D, tex);
        if (allocRows < need)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                         static_cast<GLsizei>(width), static_cast<GLsizei>(need), 0,
                         GL541_RED_INTEGER, GL_UNSIGNED_INT, pad.data());
            allocRows = need;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(need),
                            GL541_RED_INTEGER, GL_UNSIGNED_INT, pad.data());
        }
        return true;
    }

    bool runG629OnWorker(QueueItem &item)
    {
        if (!m_g629Available || m_g629Prog == 0u)
            return false;
        std::vector<uint32_t> pad;
        while (glGetError() != GL_NO_ERROR) {}

        const bool g629Split = g629SplitOn();
        auto g629Mark = std::chrono::steady_clock::now();
        const auto g629Lap = [&]() -> double {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - g629Mark).count();
            g629Mark = now;
            return ms;
        };
        double msUpload = 0.0, msDispatch = 0.0, msRead = 0.0;

        if (item.g629Resident)
        {
            const G630ReplayPacket &p = *item.g630Replay;
            if (!p.seedVram.empty() &&
                !g629Upload(m_g629ColorTex, m_g629ColorRows, 2048u, p.seedVram, pad))
                return false;
            if (m_g629ColorTex == 0u || m_g629ColorRows != 512u ||
                p.patchWords.size() != p.patchPages.size() * 2048u)
                return false;
            // G631: run-coalesced page patch (was one glTexSubImage2D per page).
            const bool g631Split = g631SplitOn();
            const auto g631T0 = g631Split ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
            uint64_t g631Calls = 0u;
            if (!g631UploadPatchPages(m_g629ColorTex, p.patchPages, p.patchWords, g631Calls))
                return false;
            if (g631Split)
                g631NotePatch(kG631SiteReplay, p.patchPages.size(), g631Calls,
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - g631T0).count(),
                              0.0);
        }
        else if (!g629Upload(m_g629ColorTex, m_g629ColorRows, 2048u, *item.g629Color, pad))
            return false;
        if (!g629Upload(m_g629RecTex,     m_g629RecRows,     1024u, *item.g629Recs,    pad) ||
            !g629Upload(m_g629TexDataTex, m_g629TexDataRows, 2048u, *item.g629TexData, pad) ||
            !g629Upload(m_g629TexDescTex, m_g629TexDescRows, 1024u, *item.g629TexDesc, pad) ||
            !g629Upload(m_g629TileOffTex, m_g629TileOffRows, 1024u, *item.g629TileOff, pad) ||
            !g629Upload(m_g629TileRefTex, m_g629TileRefRows, 1024u, *item.g629TileRef, pad))
            return false;
        // The depth image is bound unconditionally (GL requires every declared image unit to have
        // a valid binding), but the shader only touches it when a record carries ZPRIV.
        static const std::vector<uint32_t> kEmptyDepth(512u * 512u, 0u);
        if (item.g629Resident)
        {
            const G630ReplayPacket &p = *item.g630Replay;
            if (!p.depthSeed.empty() &&
                !g629Upload(m_g629DepthTex, m_g629DepthRows, 512u, p.depthSeed, pad))
                return false;
            if (m_g629DepthTex == 0u &&
                !g629Upload(m_g629DepthTex, m_g629DepthRows, 512u, kEmptyDepth, pad))
                return false;
        }
        else if (!g629Upload(m_g629DepthTex, m_g629DepthRows, 512u,
                             item.g629WantDepth ? *item.g629Depth : kEmptyDepth, pad))
            return false;

        const uint32_t viewKey = item.g629Resident
            ? (static_cast<uint32_t>(item.g629Block) >> 5u) : 0xFFFFFFFCu;
        FboEntry &linearView = ensureFbo(viewKey, item.g629Width, 512);
        if (linearView.colorTex == 0u || linearView.physW != item.g629Width ||
            linearView.physH != 512)
            return false;

        if (g629Split) msUpload = g629Lap();

        m_f.glUseProgram_(m_g629Prog);
        m_e.glBindImageTexture_(0u, m_g629ColorTex,   0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g629DepthTex,   0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
        m_e.glBindImageTexture_(2u, m_g629RecTex,     0, GL_FALSE, 0, GL542_READ_ONLY,  GL541_R32UI);
        m_e.glBindImageTexture_(3u, m_g629TexDataTex, 0, GL_FALSE, 0, GL542_READ_ONLY,  GL541_R32UI);
        m_e.glBindImageTexture_(4u, m_g629TexDescTex, 0, GL_FALSE, 0, GL542_READ_ONLY,  GL541_R32UI);
        m_e.glBindImageTexture_(5u, m_g629TileOffTex, 0, GL_FALSE, 0, GL542_READ_ONLY,  GL541_R32UI);
        m_e.glBindImageTexture_(6u, m_g629TileRefTex, 0, GL_FALSE, 0, GL542_READ_ONLY,  GL541_R32UI);
        m_e.glBindImageTexture_(7u, linearView.colorTex, 0, GL_FALSE, 0,
                                GL178_WRITE_ONLY, GL158_RGBA8);
        m_f.glUniform1i_(m_locG629Width,     item.g629Width);
        m_f.glUniform1i_(m_locG629Height,    item.g629Height);
        m_f.glUniform1i_(m_locG629TilesX,    item.g629TilesX);
        m_f.glUniform1i_(m_locG629Block,     item.g629Block);
        m_f.glUniform1i_(m_locG629Fbw,       item.g629Fbw);
        m_f.glUniform1i_(m_locG629FirstPage, item.g629FirstPage);
        m_f.glUniform1i_(m_locG629WriteView, item.g629Resident ? 1 : 0);
        m_f.glUniform1i_(m_locG629ViewHeight, 512);

        const GLuint gx = static_cast<GLuint>((item.g629Width  + 7) / 8);
        const GLuint gy = static_cast<GLuint>((item.g629Height + 7) / 8);
        m_e.glDispatchCompute_(gx, gy, 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT);
        if (g629Split) msDispatch = g629Lap();

        // ⛔⛔ `glGetTexImage` returns the ENTIRE allocated mip level, not the sub-rectangle that
        // was last uploaded. The allocation grows monotonically across batches, so sizing this
        // buffer to the CURRENT batch's rows overflows the heap by (alloc - need) * 2048 words the
        // moment a smaller batch follows a larger one — and the copy below then took whatever
        // landed there, which is where the `gpu=00000000` words came from. Size to the ALLOCATION.
        if (!item.g629Resident)
        {
        pad.assign(static_cast<size_t>(2048u) *
                       (m_g629ColorRows == 0u ? 1u : m_g629ColorRows), 0u);
        glBindTexture(GL_TEXTURE_2D, m_g629ColorTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, pad.data());
        if (glGetError() != GL_NO_ERROR)
            return false;
        std::copy(pad.begin(), pad.begin() + item.g629Color->size(), item.g629Color->begin());

        if (item.g629WantDepth)
        {
            // Same rule as the colour readback above: size to the ALLOCATION, not to 512 rows.
            pad.assign(static_cast<size_t>(512u) *
                           (m_g629DepthRows == 0u ? 1u : m_g629DepthRows), 0u);
            glBindTexture(GL_TEXTURE_2D, m_g629DepthTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, pad.data());
            if (glGetError() != GL_NO_ERROR)
                return false;
            std::copy(pad.begin(), pad.begin() + item.g629Depth->size(), item.g629Depth->begin());
        }
        }
        if (g629Split)
        {
            msRead = g629Lap();
            static uint64_t s_n = 0ull;
            static double s_up = 0.0, s_di = 0.0, s_rd = 0.0;
            ++s_n; s_up += msUpload; s_di += msDispatch; s_rd += msRead;
            if (s_n <= 4ull || (s_n % 256ull) == 0ull)
                std::fprintf(stderr,
                             "[G629:split] n=%llu upload=%.3f dispatch=%.3f read=%.3f "
                             "| mean up=%.3f di=%.3f rd=%.3f total=%.3f ms/batch "
                             "(colorWords=%zu recs=%zu texels=%zu)\n",
                             (unsigned long long)s_n, msUpload, msDispatch, msRead,
                             s_up / double(s_n), s_di / double(s_n), s_rd / double(s_n),
                             (s_up + s_di + s_rd) / double(s_n),
                             item.g629Resident ? 0u : item.g629Color->size(), item.g629Recs->size(),
                             item.g629TexData->size());
            std::fflush(stderr);
        }
        return true;
    }

    bool runG630ViewOnWorker(const G630ViewPacket &p)
    {
        if (m_g630ViewProg == 0u || m_g629ColorTex == 0u || m_g629ColorRows != 512u ||
            p.patchWords.size() != p.patchPages.size() * 2048u)
            return false;
        while (glGetError() != GL_NO_ERROR) {}
        if (!p.pageWords.empty())
        {
            if (!g629Upload(m_g630ViewMapTex, m_g630ViewMapRows, 2048u,
                            p.pageWords, m_g630ViewMapPad))
                return false;
            m_g630ViewMapReady = true;
        }
        if (!m_g630ViewMapReady) return false;
        // G631: run-coalesced page patch. On the hot 640x416 CT32 surface this is 128 pages, which
        // the per-page form issued as 128 separate 8 KiB sub-uploads — that loop, not the compute
        // dispatch below, is what G630 priced as `630view`.
        const bool g631Split = g631SplitOn();
        auto g631Mark = g631Split ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
        uint64_t g631Calls = 0u;
        if (!g631UploadPatchPages(m_g629ColorTex, p.patchPages, p.patchWords, g631Calls))
            return false;
        double g631PatchMs = 0.0;
        if (g631Split)
        {
            const auto now = std::chrono::steady_clock::now();
            g631PatchMs = std::chrono::duration<double, std::milli>(now - g631Mark).count();
            g631Mark = now;
        }
        FboEntry &dst = ensureFbo(p.fbp, p.width, p.height);
        if (dst.colorTex == 0u || dst.physW != p.width || dst.physH != p.height)
            return false; // exact raw views are logical-resolution only
        m_f.glUseProgram_(m_g630ViewProg);
        m_e.glBindImageTexture_(0u, m_g629ColorTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g630ViewMapTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(2u, dst.colorTex, 0, GL_FALSE, 0,
                                GL178_WRITE_ONLY, GL158_RGBA8);
        m_f.glUniform1i_(m_locG630ViewBlock, static_cast<GLint>(p.fbp << 5u));
        m_f.glUniform1i_(m_locG630ViewFbw, p.fbw);
        m_f.glUniform1i_(m_locG630ViewWidth, p.width);
        m_f.glUniform1i_(m_locG630ViewHeight, p.height);
        m_f.glUniform1i_(m_locG630ViewOriginY, p.originY);
        m_f.glUniform1i_(m_locG630ViewRows, p.rows);
        m_e.glDispatchCompute_(static_cast<GLuint>((p.width + 7) / 8),
                               static_cast<GLuint>((p.rows + 7) / 8), 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT);
        const bool g631Ok = glGetError() == GL_NO_ERROR;
        if (g631Split)
            g631NotePatch(kG631SiteView, p.patchPages.size(), g631Calls, g631PatchMs,
                          std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - g631Mark).count());
        return g631Ok;
    }

    bool runG630UploadOnWorker(const G630UploadPacket &p)
    {
        if (m_g630UploadProg == 0u || m_g629ColorTex == 0u || m_g629ColorRows != 512u ||
            p.pixels.size() != static_cast<size_t>(p.width) * p.height)
            return false;
        while (glGetError() != GL_NO_ERROR) {}
        if (!g629Upload(m_g630UploadTex, m_g630UploadRows, 1024u,
                        p.pixels, m_g630UploadPad))
            return false;
        m_f.glUseProgram_(m_g630UploadProg);
        m_e.glBindImageTexture_(0u, m_g629ColorTex, 0, GL_FALSE, 0,
                                GL178_WRITE_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g630UploadTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_f.glUniform1i_(m_locG630UploadBlock, static_cast<GLint>(p.block));
        m_f.glUniform1i_(m_locG630UploadFbw, p.fbw);
        m_f.glUniform1i_(m_locG630UploadX, p.x);
        m_f.glUniform1i_(m_locG630UploadY, p.y);
        m_f.glUniform1i_(m_locG630UploadWidth, p.width);
        m_f.glUniform1i_(m_locG630UploadHeight, p.height);
        m_e.glDispatchCompute_(static_cast<GLuint>((p.width + 7) / 8),
                               static_cast<GLuint>((p.height + 7) / 8), 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT);
        return glGetError() == GL_NO_ERROR;
    }

    bool runG630T8OnWorker(const G630T8Packet &p)
    {
        if (m_g630T8Prog == 0u || m_g629ColorTex == 0u || m_g629ColorRows != 512u ||
            (!p.mapWords.empty() && p.mapWords.size() != 128u * 64u) || p.tbw == 0u ||
            p.clut.size() != 256u || p.patchWords.size() != p.patchPages.size() * 2048u)
            return false;
        while (glGetError() != GL_NO_ERROR) {}
        {
            // G631: run-coalesced page patch (see g631_page_patch.inc).
            const bool g631Split = g631SplitOn();
            const auto g631T0 = g631Split ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
            uint64_t g631Calls = 0u;
            if (!g631UploadPatchPages(m_g629ColorTex, p.patchPages, p.patchWords, g631Calls))
                return false;
            if (g631Split)
                g631NotePatch(kG631SiteT8, p.patchPages.size(), g631Calls,
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - g631T0).count(),
                              0.0);
        }
        if (!p.mapWords.empty())
        {
            if (!g629Upload(m_g630T8MapTex, m_g630T8MapRows, 1024u,
                            p.mapWords, m_g630T8Pad))
                return false;
            m_g630T8PermReady = true;
        }
        if (!m_g630T8PermReady ||
            !g629Upload(m_g630T8ClutTex, m_g630T8ClutRows, 256u,
                        p.clut, m_g630T8Pad))
            return false;

        TexEntry &out = m_texCache[p.texKey];
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u) { m_texCache.erase(p.texKey); return false; }
        }
        else
            m_texBytes -= std::min(m_texBytes,
                                   static_cast<size_t>(out.w) * out.h * 4u);
        glBindTexture(GL_TEXTURE_2D, out.id);
        if (out.w != p.width || out.h != p.height)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL158_RGBA8),
                         p.width, p.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        }
        out.w = p.width;
        out.h = p.height;
        out.lastUse = m_batchCounter + 1u;
        m_texBytes += static_cast<size_t>(p.width) * p.height * 4u;

        m_f.glUseProgram_(m_g630T8Prog);
        m_e.glBindImageTexture_(0u, m_g629ColorTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g630T8MapTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(2u, m_g630T8ClutTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(3u, out.id, 0, GL_FALSE, 0,
                                GL178_WRITE_ONLY, GL158_RGBA8);
        m_f.glUniform1i_(m_locG630T8Width, p.width);
        m_f.glUniform1i_(m_locG630T8Height, p.height);
        m_f.glUniform1i_(m_locG630T8Tbp, static_cast<GLint>(p.tbp));
        m_f.glUniform1i_(m_locG630T8Tbw, static_cast<GLint>(p.tbw));
        m_e.glDispatchCompute_(static_cast<GLuint>((p.width + 7) / 8),
                               static_cast<GLuint>((p.height + 7) / 8), 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT);
        if (glGetError() != GL_NO_ERROR)
            return false;
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(p.texKey);
        }
        return true;
    }

    // G631: `rowCount != 0` reads only rows [rowLo, rowLo+rowCount) of the persistent raw-VRAM
    // image. Row p IS guest page p, so a materialize that needs a handful of pages no longer drags
    // the whole 4 MiB across the bus. `glGetTexImage` has no sub-rectangle form, so the band read
    // attaches the texture to a scratch FBO and uses glReadPixels: for a texture attachment,
    // framebuffer coordinate (x,y) is texel (x,y), so no flip is involved. Falls back to the
    // whole-image read when the FBO path is unavailable, which is exactly the legacy behaviour.
    bool readG630ImageOnWorker(bool depth, std::vector<uint32_t> &out,
                               uint32_t rowLo = 0u, uint32_t rowCount = 0u)
    {
        const GLuint tex = depth ? m_g629DepthTex : m_g629ColorTex;
        const uint32_t width = depth ? 512u : 2048u;
        const uint32_t rows = depth ? m_g629DepthRows : m_g629ColorRows;
        const uint32_t needRows = depth ? 512u : 512u;
        if (tex == 0u || rows < needRows)
            return false;
        if (rowCount != 0u && rowLo < needRows && rowLo + rowCount <= needRows &&
            m_f.glGenFramebuffers_ != nullptr && m_f.glBindFramebuffer_ != nullptr &&
            m_f.glFramebufferTexture2D_ != nullptr)
        {
            if (m_g631ReadFbo == 0u)
                m_f.glGenFramebuffers_(1, &m_g631ReadFbo);
            if (m_g631ReadFbo != 0u)
            {
                while (glGetError() != GL_NO_ERROR) {}
                out.assign(static_cast<size_t>(width) * rowCount, 0u);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g631ReadFbo);
                m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, tex, 0);
                glReadPixels(0, static_cast<GLint>(rowLo),
                             static_cast<GLsizei>(width), static_cast<GLsizei>(rowCount),
                             GL541_RED_INTEGER, GL_UNSIGNED_INT, out.data());
                const bool ok = glGetError() == GL_NO_ERROR;
                m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, 0u, 0);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
                if (ok && g631VerifyOn())
                {
                    // Oracle: re-read through the legacy whole-image path and compare this band.
                    static std::vector<uint32_t> s_ref;
                    s_ref.assign(static_cast<size_t>(width) * rows, 0u);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT,
                                  s_ref.data());
                    if (glGetError() == GL_NO_ERROR)
                        for (size_t i = 0; i < out.size(); ++i)
                        {
                            ++g_g631ReadChecks;
                            const uint32_t want =
                                s_ref[static_cast<size_t>(rowLo) * width + i];
                            if (out[i] != want && ++g_g631ReadBad <= 8ull)
                                std::fprintf(stderr,
                                             "[G631:readbad] rowLo=%u i=%zu have=%08x want=%08x\n",
                                             rowLo, i, out[i], want);
                        }
                    g631ReportVerify();
                }
                if (ok)
                    return true;
                // Fail closed onto the proven whole-image path rather than publishing a partial
                // band the caller would treat as authoritative.
                while (glGetError() != GL_NO_ERROR) {}
            }
        }
        out.assign(static_cast<size_t>(width) * needRows, 0u);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, out.data());
        return glGetError() == GL_NO_ERROR;
    }

    bool setupG570Shadow139()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        static const char *kCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) uniform uimage2D gColor;
layout(r32ui, binding=1) uniform uimage2D gDepth;
layout(r32ui, binding=2) readonly uniform uimage2D gTris;
layout(r32ui, binding=3) readonly uniform uimage2D gSpans;
layout(r32ui, binding=4) readonly uniform uimage2D gTileOffsets;
layout(r32ui, binding=5) readonly uniform uimage2D gTileTris;

ivec2 dataCoord(uint i) { return ivec2(int(i & 1023u), int(i >> 10u)); }
uint triWord(uint i) { return imageLoad(gTris, dataCoord(i)).r; }
uint spanWord(uint i) { return imageLoad(gSpans, dataCoord(i)).r; }
uint offsetWord(uint i) { return imageLoad(gTileOffsets, dataCoord(i)).r; }
uint tileTriWord(uint i) { return imageLoad(gTileTris, dataCoord(i)).r; }
void main()
{
    ivec2 ip = ivec2(gl_GlobalInvocationID.xy);
    if (ip.x >= 512 || ip.y >= 512)
        return;
    uint tile = uint(ip.y >> 4) * 32u + uint(ip.x >> 4);
    uint first = offsetWord(tile);
    uint last = offsetWord(tile + 1u);
    uint color = imageLoad(gColor, ip).r;
    uint zdst = imageLoad(gDepth, ip).r;
    for (uint ref = first; ref < last; ++ref)
    {
        uint tri = tileTriWord(ref);
        uint b = tri * 20u;
        int minY = int(triWord(b + 2u));
        int maxY = int(triWord(b + 3u));
        if (ip.y < minY || ip.y > maxY)
            continue;
        uint spanBase = (triWord(b + 4u) + uint(ip.y - minY)) * 4u;
        uint packedSpan = spanWord(spanBase);
        int xStart = int(packedSpan << 16u) >> 16;
        int xEnd = int(packedSpan) >> 16;
        if (ip.x < xStart || ip.x > xEnd)
            continue;

        int dx = ip.x - xStart;
        int e0 = int(spanWord(spanBase + 1u)) + int(triWord(b + 5u)) * dx;
        int e1 = int(spanWord(spanBase + 2u)) + int(triWord(b + 6u)) * dx;
        int e2 = int(spanWord(spanBase + 3u)) + int(triWord(b + 7u)) * dx;
        uint mode = triWord(b + 8u);
        if (e0 < 0 || (e0 == 0 && (mode & 1u) == 0u) ||
            e1 < 0 || (e1 == 0 && (mode & 2u) == 0u) ||
            e2 < 0 || (e2 == 0 && (mode & 4u) == 0u))
            continue;

        precise float a0 = uintBitsToFloat(triWord(b + 9u));
        precise float b0 = uintBitsToFloat(triWord(b + 10u));
        precise float a1 = uintBitsToFloat(triWord(b + 11u));
        precise float b1 = uintBitsToFloat(triWord(b + 12u));
        precise float lx2 = uintBitsToFloat(triWord(b + 13u));
        precise float ly2 = uintBitsToFloat(triWord(b + 14u));
        precise float winding = uintBitsToFloat(triWord(b + 15u));
        precise float invDenom = uintBitsToFloat(triWord(b + 16u));
        precise float px = float(ip.x);
        precise float py = float(ip.y);
        precise float px2 = px - lx2;
        precise float py2 = py - ly2;
        precise float w0m0 = a0 * px2;
        precise float w0m1 = b0 * py2;
        precise float w0a = w0m0 + w0m1;
        precise float w0w = w0a * winding;
        precise float w0 = w0w * invDenom;
        precise float w1m0 = a1 * px2;
        precise float w1m1 = b1 * py2;
        precise float w1a = w1m0 + w1m1;
        precise float w1w = w1a * winding;
        precise float w1 = w1w * invDenom;
        precise float oneMinusW0 = 1.0 - w0;
        precise float w2 = oneMinusW0 - w1;
        precise float z0 = uintBitsToFloat(triWord(b + 17u));
        precise float z1 = uintBitsToFloat(triWord(b + 18u));
        precise float z2 = uintBitsToFloat(triWord(b + 19u));
        precise float zm0 = z0 * w0;
        precise float zm1 = z1 * w1;
        precise float za = zm0 + zm1;
        precise float zm2 = z2 * w2;
        precise float zf = za + zm2;
        zf = clamp(zf, 0.0, 16777215.0);
        if (uint(zf) < zdst)
            continue;
        uint r = color & 255u;
        uint g = (color >> 8u) & 255u;
        uint bl = (color >> 16u) & 255u;
        if ((mode & 0x100u) != 0u)
        {
            r = min(r + 1u, 255u);
            g = min(g + 1u, 255u);
            bl = min(bl + 1u, 255u);
        }
        else
        {
            r = r == 0u ? 0u : r - 1u;
            g = g == 0u ? 0u : g - 1u;
            bl = bl == 0u ? 0u : bl - 1u;
        }
        color = 0x40000000u | r | (g << 8u) | (bl << 16u);
    }
    imageStore(gColor, ip, uvec4(color, 0u, 0u, 0u));
}
)GLSL";

        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G570:gpu139] compute compile failed: %s\n", log.c_str());
            return false;
        }
        m_g570Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g570Prog, cs);
        m_f.glLinkProgram_(m_g570Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g570Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g570Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(m_g570Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G570:gpu139] compute link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g570Prog);
            m_g570Prog = 0u;
            return false;
        }
        return true;
    }

    bool g570UploadWords(GLuint &tex, uint32_t &allocatedRows, uint32_t width,
                         const std::vector<uint32_t> &words)
    {
        if (width == 0u || words.empty() || (words.size() % width) != 0u)
            return false;
        const uint32_t rows = static_cast<uint32_t>(words.size() / width);
        if (tex == 0u)
        {
            glGenTextures(1, &tex);
            if (tex == 0u)
                return false;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        else
            glBindTexture(GL_TEXTURE_2D, tex);
        if (allocatedRows < rows)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                         static_cast<GLsizei>(width), static_cast<GLsizei>(rows), 0,
                         GL541_RED_INTEGER, GL_UNSIGNED_INT, words.data());
            allocatedRows = rows;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(rows),
                            GL541_RED_INTEGER, GL_UNSIGNED_INT, words.data());
        }
        return true;
    }

    bool runG570Shadow139OnWorker(const std::vector<uint32_t> &color,
                                  const std::vector<uint32_t> &depth,
                                  const std::vector<uint32_t> &triWords,
                                  const std::vector<uint32_t> &spanWords,
                                  const std::vector<uint32_t> &tileOffsets,
                                  const std::vector<uint32_t> &tileTris,
                                  std::vector<uint32_t> &out)
    {
        if (!m_g570Available || m_g570Prog == 0u)
            return false;
        if (!dc2ShouldExecuteRenderEvent("g570Shadow", 0x139, 0, 0, 0, 0, 512, 512, 0, 0, 0, 0, 0))
            return true;

        const bool g571On = g571ShadowSplitOn();
        auto g571Mark = std::chrono::steady_clock::now();
        const auto g571LapUs = [&]() {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - g571Mark).count());
            g571Mark = now;
            return us;
        };
        uint64_t g571ColorUs = 0u, g571DepthUs = 0u, g571TriUs = 0u;
        uint64_t g571SpanUs = 0u, g571OffsetUs = 0u, g571TileUs = 0u;
        uint64_t g571DispatchUs = 0u, g571ReadUs = 0u;

        if (!g570UploadWords(m_g570ColorTex, m_g570ColorRows, 512u, color))
            return false;
        if (g571On) g571ColorUs = g571LapUs();
        if (!g570UploadWords(m_g570DepthTex, m_g570DepthRows, 512u, depth))
            return false;
        if (g571On) g571DepthUs = g571LapUs();
        if (!g570UploadWords(m_g570TriTex, m_g570TriRows, 1024u, triWords))
            return false;
        if (g571On) g571TriUs = g571LapUs();
        if (!g570UploadWords(m_g570SpanTex, m_g570SpanRows, 1024u, spanWords))
            return false;
        if (g571On) g571SpanUs = g571LapUs();
        if (!g570UploadWords(m_g570TileOffsetTex, m_g570TileOffsetRows, 1024u, tileOffsets))
            return false;
        if (g571On) g571OffsetUs = g571LapUs();
        if (!g570UploadWords(m_g570TileTriTex, m_g570TileTriRows, 1024u, tileTris))
            return false;
        if (g571On) g571TileUs = g571LapUs();

        m_f.glUseProgram_(m_g570Prog);
        m_e.glBindImageTexture_(0u, m_g570ColorTex, 0, GL_FALSE, 0,
                                GL178_READ_WRITE, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g570DepthTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(2u, m_g570TriTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(3u, m_g570SpanTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(4u, m_g570TileOffsetTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(5u, m_g570TileTriTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glDispatchCompute_(64u, 64u, 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT);
        if (g571On) g571DispatchUs = g571LapUs();
        const size_t surfacePixels = 512u * 512u;
        out.resize(surfacePixels);
        glBindTexture(GL_TEXTURE_2D, m_g570ColorTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, out.data());
        if (g571On)
        {
            g571ReadUs = g571LapUs();
            static uint64_t s_g571Batch = 0u;
            std::fprintf(stderr,
                         "[G571:gpuwork] n=%llu color=%.3f depth=%.3f tri=%.3f "
                         "span=%.3f offset=%.3f tile=%.3f dispatch=%.3f read=%.3f total=%.3fms\n",
                         static_cast<unsigned long long>(++s_g571Batch),
                         g571ColorUs / 1000.0, g571DepthUs / 1000.0,
                         g571TriUs / 1000.0, g571SpanUs / 1000.0,
                         g571OffsetUs / 1000.0, g571TileUs / 1000.0,
                         g571DispatchUs / 1000.0, g571ReadUs / 1000.0,
                         (g571ColorUs + g571DepthUs + g571TriUs + g571SpanUs +
                          g571OffsetUs + g571TileUs + g571DispatchUs + g571ReadUs) / 1000.0);
        }
        return true;
    }

    bool setupG541NullCompute()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        static const char *kCS =
            "#version 430 core\n"
            "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;\n"
            "layout(r32ui, binding=7) uniform coherent uimage2D gState;\n"
            "uniform int uKickCount;\n"
            "void main(){\n"
            "  uint v = imageLoad(gState, ivec2(0,0)).r;\n"
            "  imageStore(gState, ivec2(0,0), uvec4(v + uint(uKickCount), 0u, 0u, 0u));\n"
            "}\n";
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G541:null] compute compile failed: %s\n", log.c_str());
            return false;
        }
        m_g541NullProg = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g541NullProg, cs);
        m_f.glLinkProgram_(m_g541NullProg);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g541NullProg, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g541NullProg, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g541NullProg, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G541:null] compute link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }
        m_locG541KickCount = m_f.glGetUniformLocation_(m_g541NullProg, "uKickCount");
        if (m_locG541KickCount < 0)
        {
            std::fprintf(stderr, "[G541:null] missing uKickCount uniform\n");
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }

        while (glGetError() != GL_NO_ERROR) {}
        const uint32_t zero = 0u;
        glGenTextures(1, &m_g541StateTex);
        glBindTexture(GL_TEXTURE_2D, m_g541StateTex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI), 1, 1, 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        const GLenum err = glGetError();
        if (m_g541StateTex == 0u || err != GL_NO_ERROR)
        {
            std::fprintf(stderr, "[G541:null] state image setup failed gl=0x%x\n",
                         static_cast<unsigned>(err));
            if (m_g541StateTex != 0u)
                glDeleteTextures(1, &m_g541StateTex);
            m_g541StateTex = 0u;
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }
        return true;
    }

    bool setupProgram()
    {
        // Vertex positions are already screen pixels; depth is written in the fragment shader
        // from the raw GS Z (noperspective varying) so the tiny title Z values keep full float
        // precision near 0 instead of being crushed by the [-1,1] clip-space round-trip.
        // ALL varyings are `noperspective`: the CPU rasterizer interpolates color, s/|q|, t/|q|
        // and 1/|q| linearly in screen space (barycentric on screen X/Y) — see drawTriangle.
        static const char *kVS =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aSTQ;\n"
            "layout(location=2) in vec4 aColor;\n"
            "layout(location=3) in vec4 aFog;\n"
            "uniform vec2 uFbSize;\n"
            "noperspective out vec3 vSTQ;\n"
            "noperspective out vec4 vColor;\n"
            "noperspective out float vZ;\n"
            "noperspective out float vFog;\n"
            "void main(){\n"
            "    vSTQ = aSTQ;\n"
            "    vColor = aColor;\n"
            "    vFog = aFog.x;\n"
            "    vZ = aPos.z;\n"
            "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
            "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
            "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
            "}\n";
        // uMode bits: 0 = textured, 1..2 = TFX, 3 = TCC, 4 = clamp Z to PSMZ24,
        // 5 = store raw source alpha (G255 RTT second pass), 6 = G265 GEQUAL alpha test,
        // 7..8 = G265 AREF code (0=1, 1=64, 2=127), 9 = G286 exact opaque sampler/combine,
        // 10 = G289 CT24 direct-owner source (force TEXA TA0=128),
        // 11 = G309 direct authoritative-page source selection,
        // 12 = G362 guarded fixed-point POINT fetch (see below),
        // 13 = G364 PRIM.FGE (blend RGB towards uFogCol by the interpolated per-vertex F),
        // 14 = G406 accurate GS 12.4 UV rounding for axis-aligned point sprites,
        //
        // G362: `texture()` with GL_NEAREST decides the texel from floor() of an interpolated
        // float. The front-end shifts FST sprite UVs by -0.5 texel-per-pixel so a fragment's
        // center maps to the CPU sampler's texel coordinate exactly — which puts every sample
        // of an axis-aligned 2D sprite EXACTLY on a texel boundary. Whether floor() then returns
        // the intended texel or the one before it is decided by the last ULP of the raster
        // interpolation, so a whole row/column of a menu sprite could sample the neighbouring
        // atlas patch (Items-menu grey slot panel: brown separator rows bleeding in at the cell
        // seams; per-icon black notch from the atlas row above the icon tile). The CPU path
        // computes the same coordinate in exact 12.4 fixed point and truncates, so it never
        // mis-picks. Bit 12 routes point-sampled draws through sampleExact()'s integer branch,
        // which reconstructs the 12.4 coordinate with the G258 +0.5001 rounding guard and is
        // therefore decided by the intended mapping instead of by float error.
        // PS2 color/alpha scale: 0x80 == 1.0, so MODULATE is (t*c)>>7 -> t*c*255/128 in [0,1]
        // space (1.9921875), clamped — identical to combineTexture(). The written alpha is the
        // BLEND FACTOR encoding (As/128 clamped to 1): glBlendFunc consumes it as SRC_ALPHA. The
        // stored framebuffer alpha therefore differs from the CPU path (which stores raw src A);
        // nothing on the title path reads framebuffer alpha (present + dumps are RGB) — G179
        // must revisit (dual-source blending) before any route that samples an RTT's alpha.
        static const char *kFS =
            "#version 330 core\n"
            "noperspective in vec3 vSTQ;\n"
            "noperspective in vec4 vColor;\n"
            "noperspective in float vZ;\n"
            "noperspective in float vFog;\n"
            "out vec4 o;\n"
            "uniform int uFogCol;\n" // G364: packed 0x00BBGGRR (int keeps to the loaded uniform set)
            "uniform sampler2D uTex;\n"
            "uniform sampler2D uG309Src1;\n"
            "uniform sampler2D uG309Src2;\n"
            "uniform sampler2D uG309Src3;\n"
            "uniform sampler2D uG309Src4;\n"
            "uniform sampler2D uG309Src5;\n"
            "uniform sampler2D uG309Src6;\n"
            "uniform sampler2D uG309Src7;\n"
            "uniform sampler2D uG309PageMap;\n"
            "uniform sampler2D uG309Base;\n"
            "uniform int uMode;\n"
            "uniform int uLinear;\n"
            "uniform int uWrapU;\n"
            "uniform int uWrapV;\n"
            "uniform vec2 uFbSize;\n"
            "uniform vec2 uFbScale;\n"
            "uniform int uUvOriginX;\n"
            "uniform int uUvOriginY;\n"
            "uniform int uUvRoundU;\n"
            "uniform int uUvRoundV;\n"
            // G624: nonzero when this draw samples a producer FBO through `srcFbp`, whose GL row
            // h-1-v holds GS row v (the front end flipped T with `tq = 1 - tq`). See g406RoundUv.
            "uniform int uUvFlipV;\n"
            "int wrapCoord(int p, int n, int mode){\n"
            "    if (mode != 0) return clamp(p, 0, n - 1);\n"
            "    int r = p % n; return r < 0 ? r + n : r;\n"
            "}\n"
            "int floorDiv16(int v){ return v >= 0 ? v / 16 : -((-v + 15) / 16); }\n"
            "vec4 g309Owner(int owner, vec2 p){\n"
            "    if (owner == 0) return texture(uTex, p * uFbScale / vec2(textureSize(uTex, 0)));\n"
            "    if (owner == 1) return texture(uG309Src1, p * uFbScale / vec2(textureSize(uG309Src1, 0)));\n"
            "    if (owner == 2) return texture(uG309Src2, p * uFbScale / vec2(textureSize(uG309Src2, 0)));\n"
            "    if (owner == 3) return texture(uG309Src3, p * uFbScale / vec2(textureSize(uG309Src3, 0)));\n"
            "    if (owner == 4) return texture(uG309Src4, p * uFbScale / vec2(textureSize(uG309Src4, 0)));\n"
            "    if (owner == 5) return texture(uG309Src5, p * uFbScale / vec2(textureSize(uG309Src5, 0)));\n"
            "    if (owner == 6) return texture(uG309Src6, p * uFbScale / vec2(textureSize(uG309Src6, 0)));\n"
            "    return texture(uG309Src7, p * uFbScale / vec2(textureSize(uG309Src7, 0)));\n"
            "}\n"
            "vec4 g309Filtered(int owner, vec2 p){\n"
            "    return g309Owner(owner, p);\n"
            "}\n"
            "vec4 g309Pixel(ivec2 p){\n"
            "    p = clamp(p, ivec2(0), ivec2(511));\n"
            "    int gsY = 511 - p.y;\n"
            "    ivec2 page = ivec2(p.x >> 6, gsY >> 5);\n"
            "    ivec2 local = ivec2(p.x & 63, gsY & 31);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uG309PageMap,page,0)*255.0+0.5)),0,255);\n"
            "    if (m.r == 0) return texture(uG309Base, (vec2(p) + vec2(0.5)) / 512.0);\n"
            "    ivec2 srcGs = ivec2(m.g * 64 + local.x, m.b * 32 + local.y);\n"
            "    return g309Owner(m.r - 1, vec2(srcGs.x, 511 - srcGs.y) + vec2(0.5));\n"
            "}\n"
            "vec4 g309Linear(vec2 uv){\n"
            "    vec2 q = uv * 512.0 - vec2(0.5);\n"
            "    ivec2 p = ivec2(floor(q)); vec2 f = fract(q);\n"
            "    if (all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,ivec2(511))) &&\n"
            "        (p.x >> 6) == ((p.x + 1) >> 6) &&\n"
            "        ((511-p.y) >> 5) == ((510-p.y) >> 5)) {\n"
            "        int gsY = 511-p.y; ivec2 page=ivec2(p.x>>6,gsY>>5);\n"
            "        ivec4 m=clamp(ivec4(floor(texelFetch(uG309PageMap,page,0)*255.0+0.5)),0,255);\n"
            "        if (m.r == 0) return texture(uG309Base, uv);\n"
            "        int owner=m.r-1; ivec2 local=ivec2(p.x&63,gsY&31);\n"
            "        ivec2 srcGs=ivec2(m.g*64+local.x,m.b*32+local.y);\n"
            "        ivec2 srcGl=ivec2(srcGs.x,511-srcGs.y);\n"
            "        return g309Filtered(owner,vec2(srcGl)+f+vec2(0.5));\n"
            "    }\n"
            "    vec4 a = mix(g309Pixel(p), g309Pixel(p + ivec2(1,0)), f.x);\n"
            "    vec4 b = mix(g309Pixel(p + ivec2(0,1)), g309Pixel(p + ivec2(1,1)), f.x);\n"
            "    return mix(a, b, f.y);\n"
            "}\n"
            "ivec4 texel8(ivec2 p){\n"
            "    ivec2 n = textureSize(uTex, 0);\n"
            "    p.x = wrapCoord(p.x, n.x, uWrapU);\n"
            "    p.y = wrapCoord(p.y, n.y, uWrapV);\n"
            "    return clamp(ivec4(floor(texelFetch(uTex, p, 0) * 255.0 + 0.5)), 0, 255);\n"
            "}\n"
            "float g406RoundAxis(float value, int pixel, int origin, int flags){\n"
            "    bool legacy = (flags & 4) != 0;\n"
            "    bool halfBoundary = legacy || uLinear != 0;\n"
            "    float boundary = halfBoundary ? floor(value * 2.0 + 0.5) * 0.5\n"
            "                                  : floor(value + 0.5);\n"
            "    float threshold = legacy ? 0.25 : (1.0 / 64.0);\n"
            "    if (abs(value - boundary) > threshold) return value;\n"
            "    bool down = ((flags & 2) != 0) && pixel != origin;\n"
            "    bool up = ((flags & 1) != 0) || (((flags & 2) != 0) && pixel == origin);\n"
            "    float downNudge = legacy ? 0.25 : (1.0 / 16.0);\n"
            "    float upNudge = legacy ? 0.25 : 0.0;\n"
            "    return down ? boundary - downNudge : (up ? boundary + upNudge : value);\n"
            "}\n"
            // G624: `g406RoundAxis` is a statement about the GS V AXIS. Its boundary is
            // `floor(v + 0.5)` (ties round toward increasing v) and its bias bits nudge by
            // +upNudge / -downNudge along increasing v. A `srcFbp` bind hands this function the
            // FLIPPED coordinate (GL row h-1-v), so both the tie-break and both nudge directions
            // invert — that is G623's pixel-gate failure (5.2% identical against a 77% control
            // band; `DC2_G406_NO_ACCURATE_UV=1` in both arms restored it to 97.5%).
            //
            // ⛔ A FRONT-END BIT SWAP CANNOT EXPRESS THE REPAIR. `flags & 2` means "bias DOWN
            // except on the origin row, where it biases UP", so swapping bits 1 and 2 is not the
            // mirror of the rule — it drops the origin-row exception. Undo the flip, evaluate the
            // IDENTICAL function on the GS-space value, and re-flip the result: exact by
            // construction. `uUvFlipV == 0` reproduces the pre-G624 expression bit for bit.
            "vec2 g406RoundUv(vec2 uvTex){\n"
            "    if ((uMode & 16384) == 0) return uvTex;\n"
            "    int px = int(floor(gl_FragCoord.x / max(uFbScale.x, 1e-4)));\n"
            "    int py = int(floor(uFbSize.y - (gl_FragCoord.y / max(uFbScale.y, 1e-4))));\n"
            "    bool flipV = uUvFlipV != 0;\n"
            "    float flipH = float(textureSize(uTex, 0).y);\n"
            "    float vIn = flipV ? (flipH - uvTex.y) : uvTex.y;\n"
            "    vec2 rounded = vec2(g406RoundAxis(uvTex.x, px, uUvOriginX, uUvRoundU),\n"
            "                        g406RoundAxis(vIn, py, uUvOriginY, uUvRoundV));\n"
            "    if (uLinear != 0) rounded = trunc(rounded * 16.0) * (1.0 / 16.0);\n"
            "    if (flipV) rounded.y = flipH - rounded.y;\n"
            "    return rounded;\n"
            "}\n"
            "ivec4 sampleExact(vec2 uvTex){\n"
            "    ivec2 f16 = clamp(ivec2(floor(uvTex * 16.0 + 0.5001)), ivec2(0), ivec2(65535));\n"
            "    if (uLinear == 0) return texel8(f16 / 16);\n"
            "    ivec2 shifted = f16 - ivec2(8);\n"
            "    ivec2 p0 = ivec2(floorDiv16(shifted.x), floorDiv16(shifted.y));\n"
            "    ivec2 fr = shifted - p0 * 16;\n"
            "    ivec4 c00 = texel8(p0), c10 = texel8(p0 + ivec2(1,0));\n"
            "    ivec4 c01 = texel8(p0 + ivec2(0,1)), c11 = texel8(p0 + ivec2(1,1));\n"
            "    int wx0 = 16 - fr.x, wy0 = 16 - fr.y;\n"
            "    ivec4 sum = c00 * (wx0 * wy0) + c10 * (fr.x * wy0) +\n"
            "                c01 * (wx0 * fr.y) + c11 * (fr.x * fr.y);\n"
            "    return clamp((sum + ivec4(128)) / 256, 0, 255);\n"
            "}\n"
            "ivec4 combineExact(ivec4 vc, ivec4 tc){\n"
            "    int tfx = (uMode >> 1) & 3; bool tcc = (uMode & 8) != 0;\n"
            "    ivec4 c = tc;\n"
            "    if (tfx == 0) {\n"
            "        c.rgb = (tc.rgb * vc.rgb) >> 7; c.a = tcc ? ((tc.a * vc.a) >> 7) : vc.a;\n"
            "    } else if (tfx == 1) { c.a = tcc ? tc.a : vc.a;\n"
            "    } else {\n"
            "        c.rgb = ((tc.rgb * vc.rgb) >> 7) + ivec3(vc.a);\n"
            "        c.a = (tfx == 2) ? (tcc ? tc.a + vc.a : vc.a) : (tcc ? tc.a : vc.a);\n"
            "    }\n"
            "    return clamp(c, 0, 255);\n"
            "}\n"
            // G364: identical integer form to the CPU rasterizer's g364FogLerp, so the GPU and the
            // DC2_G178_NO_GPU=1 oracle agree bit-for-bit. F=255 -> unfogged, F=0 -> pure fog colour.
            "ivec3 fogExact(ivec3 c){\n"
            "    if ((uMode & 8192) == 0) return c;\n"
            "    int f = clamp(int(floor(vFog * 255.0 + 0.5)), 0, 255);\n"
            "    ivec3 fc = ivec3(uFogCol & 255, (uFogCol >> 8) & 255, (uFogCol >> 16) & 255);\n"
            "    return (c * f + fc * (255 - f) + ivec3(127)) / 255;\n"
            "}\n"
            // G584: the exact-sampler branch below wrote the RAW source alpha into o.a and
            // returned, skipping the outA encoding the legacy branch applies at the bottom of
            // main(). o.a is ALSO what glBlendFunc consumes as GL_SRC_ALPHA, so every BLENDED
            // draw on that branch weighted by As/255 while the GS (and the CPU replay) weight is
            // As/128 -- an exact 128/255 = 0.502 halving at As = 0x80, drifting toward 1.0 as Cd
            // grows. Bit 15 selects the blend-factor encoding; the caller sets it only for the
            // exact branch, so the rollback flag reproduces the legacy value bit-for-bit.
            "float g584ExactA(int a){\n"
            "    return ((uMode & 32768) != 0) ? min(float(a) * (1.0 / 128.0), 1.0)\n"
            "                                  : float(a) * (1.0 / 255.0);\n"
            "}\n"
            // G604: which GS blend combo this draw is. Legacy path: bits 17..19 of uMode (the
            // G560 build masks uMode to 16 bits and overrides this body with its own field).
            "int g604BlendMode(){ return (uMode >> 17) & 7; }\n"
            // G604 -- THE EXACTNESS FIX FOR G603's BLEND MODE 6 (Cd - Cs*As/128).
            //
            // The GS evaluates ((A-B)*C)>>7 + D in INTEGERS, and for this combo (A=0, B=Cs) the
            // product is NEGATIVE. `>>7` on a negative int is an ARITHMETIC shift, i.e. FLOOR, so
            // the hardware computes Cd - ceil(Cs*As/128) -- it always rounds the subtraction AWAY
            // from zero. GL's fixed-function blend evaluates in float and rounds the UNORM8 result
            // to NEAREST, so it subtracts 0..1 less on every fragment. On an overdrawn effect that
            // bias accumulates: the same-run oracle measured 22.5% of pixels off by >1 with a
            // uniform +1.6 mean, against a 1.53% floor on the display batches beside them.
            //
            // GL cannot be asked to floor a blend, so do the multiply HERE, exactly, and hand the
            // fixed-function unit a plain subtraction (factors ONE/ONE, reverse-subtract). Integer
            // operands make the final UNORM8 rounding a no-op. `(n+127)/128` is ceil for n >= 0.
            //
            // The 255-clamp on the emitted value is safe rather than approximate: it only engages
            // when Cs*As/128 >= 255, and Cd <= 255, so both arms clamp the difference to 0.
            // (Same shape of repair as G584: fold the weight into the shader output rather than
            // trying to express a PS2 rounding rule in GL blend state.)
            "vec3 g604SubBlendRgb(vec3 rgb, float aa){\n"
            "    ivec3 cs = clamp(ivec3(floor(rgb * 255.0 + 0.5)), 0, 255);\n"
            "    int as = clamp(int(floor(aa * 255.0 + 0.5)), 0, 255);\n"
            "    return vec3(min((cs * as + 127) / 128, ivec3(255))) * (1.0 / 255.0);\n"
            "}\n"
            "void main(){\n"
            "    if ((uMode & 512) != 0) {\n"
            "        ivec4 vc = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255);\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        ivec4 tc = sampleExact(g406RoundUv(uv * vec2(textureSize(uTex, 0))));\n"
            "        if ((uMode & 1024) != 0) tc.a = 128;\n"
            "        ivec4 ec = combineExact(vc, tc);\n"
            "        if ((uMode & 64) != 0) {\n"
            "            int refCode = (uMode >> 7) & 3;\n"
            "            int refA = (refCode == 0) ? 1 : ((refCode == 1) ? 64 : 127);\n"
            "            if (ec.a < refA) discard;\n"
            "        }\n"
            "        ec.rgb = fogExact(ec.rgb);\n"
            "        o = vec4(vec3(ec.rgb) * (1.0 / 255.0),\n"
            "                 ((uMode & 32) != 0) ? float(ec.a) * (1.0 / 255.0)\n"
            "                                     : g584ExactA(ec.a));\n"
            "        float ez = max(vZ, 0.0);\n"
            "        if ((uMode & 16) != 0) ez = floor(min(ez, 16777215.0));\n"
            "        gl_FragDepth = ez * (1.0 / 4294967296.0);\n"
            "        return;\n"
            "    }\n"
            "    vec4 c = vColor;\n"
            "    bool alphaTest = (uMode & 64) != 0;\n"
            "    int alphaByte = 0;\n"
            "    if (alphaTest) alphaByte = int(floor(vColor.a * 255.0 + 0.5));\n"
            "    if ((uMode & 1) != 0) {\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        vec4 t;\n"
            "        if ((uMode & 2048) != 0) t = g309Linear(uv);\n"
            "        else if ((uMode & 4096) != 0)\n"
            "            t = vec4(sampleExact(g406RoundUv(uv * vec2(textureSize(uTex, 0))))) * (1.0 / 255.0);\n"
            "        else t = texture(uTex, uv);\n"
            "        if ((uMode & 1024) != 0) t.a = 128.0 / 255.0;\n"
            "        int tfx = (uMode >> 1) & 3;\n"
            "        bool tcc = (uMode & 8) != 0;\n"
            "        vec3 rgb; float a;\n"
            "        if (tfx == 0) {\n"
            "            rgb = min(t.rgb * c.rgb * 1.9921875, vec3(1.0));\n"
            "            a = tcc ? min(t.a * c.a * 1.9921875, 1.0) : c.a;\n"
            "        } else if (tfx == 1) {\n"
            "            rgb = t.rgb;\n"
            "            a = tcc ? t.a : c.a;\n"
            "        } else {\n"
            "            rgb = min(t.rgb * c.rgb * 1.9921875 + c.aaa, vec3(1.0));\n"
            "            a = (tfx == 2) ? (tcc ? min(t.a + c.a, 1.0) : c.a)\n"
            "                           : (tcc ? t.a : c.a);\n"
            "        }\n"
            "        if (alphaTest) {\n"
            "            int texA = int(floor(t.a * 255.0 + 0.5));\n"
            "            int vertA = alphaByte;\n"
            "            if (tfx == 0)\n"
            "                alphaByte = tcc ? min((texA * vertA) / 128, 255) : vertA;\n"
            "            else if (tfx == 1)\n"
            "                alphaByte = tcc ? texA : vertA;\n"
            "            else\n"
            "                alphaByte = (tfx == 2) ? (tcc ? min(texA + vertA, 255) : vertA)\n"
            "                                       : (tcc ? texA : vertA);\n"
            "        }\n"
            "        c = vec4(rgb, a);\n"
            "    }\n"
            "    if (alphaTest) {\n"
            "        int refCode = (uMode >> 7) & 3;\n"
            "        int refA = (refCode == 0) ? 1 : ((refCode == 1) ? 64 : 127);\n"
            "        if (alphaByte < refA) discard;\n"
            "    }\n"
            "    float outA = ((uMode & 32) != 0) ? c.a : min(c.a * 1.9921875, 1.0);\n"
            "    if ((uMode & 8192) != 0)\n"
            "        c.rgb = vec3(fogExact(clamp(ivec3(floor(c.rgb * 255.0 + 0.5)), 0, 255))) * (1.0 / 255.0);\n"
            // G604: after fog (the GS fogs the source colour before blending it), fold mode 6's
            // As/128 weight into the emitted colour. outA is left alone deliberately: it is also
            // the STORED framebuffer alpha, and the display path's 0..255 alpha convention is a
            // property of every display batch, not of this admission.
            "    if (g604BlendMode() == 6) c.rgb = g604SubBlendRgb(c.rgb, c.a);\n"
            "    o = vec4(c.rgb, outA);\n"
            "    float z = max(vZ, 0.0);\n"
            "    if ((uMode & 16) != 0) z = floor(min(z, 16777215.0));\n"
            "    gl_FragDepth = z * (1.0 / 4294967296.0);\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_prog, vs);
        m_f.glAttachShader_(m_prog, fs);
        m_f.glLinkProgram_(m_prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            std::fprintf(stderr, "[G178:backend] FAIL: program link\n");
            return false;
        }
        m_f.glUseProgram_(m_prog);
        m_locFbSize = m_f.glGetUniformLocation_(m_prog, "uFbSize");
        m_locFbScale = m_f.glGetUniformLocation_(m_prog, "uFbScale");
        m_locMode = m_f.glGetUniformLocation_(m_prog, "uMode");
        m_locFogCol = m_f.glGetUniformLocation_(m_prog, "uFogCol"); // G364
        m_locLinear = m_f.glGetUniformLocation_(m_prog, "uLinear");
        m_locWrapU = m_f.glGetUniformLocation_(m_prog, "uWrapU");
        m_locWrapV = m_f.glGetUniformLocation_(m_prog, "uWrapV");
        m_locUvOriginX = m_f.glGetUniformLocation_(m_prog, "uUvOriginX");
        m_locUvOriginY = m_f.glGetUniformLocation_(m_prog, "uUvOriginY");
        m_locUvRoundU = m_f.glGetUniformLocation_(m_prog, "uUvRoundU");
        m_locUvRoundV = m_f.glGetUniformLocation_(m_prog, "uUvRoundV");
        m_locUvFlipV = m_f.glGetUniformLocation_(m_prog, "uUvFlipV"); // G624
        const GLint locTex = m_f.glGetUniformLocation_(m_prog, "uTex");
        m_f.glUniform1i_(locTex, 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src1"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src2"), 2);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src3"), 3);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src4"), 4);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src5"), 5);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src6"), 6);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src7"), 7);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309PageMap"),
                         static_cast<GLint>(kG309PageMapUnit));
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Base"),
                         static_cast<GLint>(kG309BaseUnit));

        if (g560MultiDrawRequested() || g561TextureRunsRequested() ||
            g570UntexturedRunsRequested())
        {
            // Keep the proven program independent: a driver without dual-source output support
            // simply leaves G560 unavailable and continues on the byte-identical legacy path.
            const auto setupG560 = [&]() -> bool {
                static const char *kG560VS =
                    "#version 330 core\n"
                    "layout(location=0) in vec3 aPos;\n"
                    "layout(location=1) in vec3 aSTQ;\n"
                    "layout(location=2) in vec4 aColor;\n"
                    "layout(location=3) in vec4 aFog;\n"
                    "layout(location=4) in vec4 aDrawState;\n"
                    "uniform vec2 uFbSize;\n"
                    "noperspective out vec3 vSTQ;\n"
                    "noperspective out vec4 vColor;\n"
                    "noperspective out float vZ;\n"
                    "noperspective out float vFog;\n"
                    "flat out ivec4 vG560State;\n"
                    "void main(){\n"
                    "    vSTQ = aSTQ;\n"
                    "    vColor = aColor;\n"
                    "    vFog = aFog.x;\n"
                    "    vG560State = ivec4(aDrawState + vec4(0.5));\n"
                    "    vZ = aPos.z;\n"
                    "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
                    "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
                    "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
                    "}\n";

                std::string g560Fs(kFS);
                const auto replaceOne = [](std::string &s, const char *from, const char *to) {
                    const size_t p = s.find(from);
                    if (p == std::string::npos)
                        return false;
                    s.replace(p, std::strlen(from), to);
                    return true;
                };
                if (!replaceOne(g560Fs,
                                "noperspective in float vFog;\nout vec4 o;\n",
                                "noperspective in float vFog;\n"
                                "flat in ivec4 vG560State;\n"
                                "layout(location=0,index=0) out vec4 o;\n"
                                "layout(location=0,index=1) out vec4 oG560Factor;\n") ||
                    !replaceOne(g560Fs,
                                "int wrapCoord(int p, int n, int mode){\n",
                                "#define uMode (vG560State.x & 65535)\n"
                                "#define uFogCol vG560State.y\n"
                                "#define uUvOriginX (vG560State.z & 65535)\n"
                                "#define uUvRoundU ((vG560State.z >> 16) & 15)\n"
                                "#define uUvOriginY (vG560State.w & 65535)\n"
                                "#define uUvRoundV ((vG560State.w >> 16) & 15)\n"
                                // G624: this build has no per-primitive slot left for the flip
                                // flag, so it keeps the pre-G624 (unflipped) rounding. That is
                                // only correct for draws that do not bind a producer FBO, which
                                // is why `g623ProdSrcOn()` refuses to arm while G560 multidraw
                                // is enabled. Both are opt-in; neither is in the shipped path.
                                "#define uUvFlipV 0\n"
                                "int wrapCoord(int p, int n, int mode){\n") ||
                    !replaceOne(g560Fs,
                                "void main(){\n",
                                "void g560Factors(float blendA){\n"
                                "    int b = (vG560State.x >> 16) & 7;\n"
                                // G604 CORRECTION to G603: mode 6's As/128 weight is applied by
                                // g604SubBlendRgb in the shader (GS floors the negative product;
                                // a GL factor rounds to nearest and drifts). The factor is now a
                                // plain 1.0, exactly like mode 4 whose equation it shares.
                                "    float sf = (b == 1 || b == 2) ? blendA : (b == 5 ? 0.0 : 1.0);\n"
                                "    float df = b == 0 ? 0.0 : ((b == 1 || b == 5) ? 1.0-blendA : 1.0);\n"
                                "    oG560Factor = vec4(sf, sf, sf, df);\n"
                                "}\n"
                                "void main(){\n") ||
                    // G604: this build masks uMode to 16 bits, so the legacy bits-17..19 blend
                    // field is invisible to it. Point g604BlendMode() at its own per-primitive
                    // field instead, or mode 6 would silently keep the drifting GL-factor form.
                    !replaceOne(g560Fs,
                                "int g604BlendMode(){ return (uMode >> 17) & 7; }\n",
                                "int g604BlendMode(){ return (vG560State.x >> 16) & 7; }\n") ||
                    // G584: the dual-source factor for the exact branch takes the same encoding
                    // as the legacy fixed-function one. It fed the RAW As/255 before, which is
                    // the identical halving the legacy path had -- SRC1_COLOR just carried it.
                    !replaceOne(g560Fs,
                                "        o = vec4(vec3(ec.rgb) * (1.0 / 255.0),\n"
                                "                 ((uMode & 32) != 0) ? float(ec.a) * (1.0 / 255.0)\n"
                                "                                     : g584ExactA(ec.a));\n",
                                "        g560Factors(g584ExactA(ec.a));\n"
                                "        o = vec4(vec3(ec.rgb) * (1.0 / 255.0),\n"
                                "                 ((uMode & 32) != 0) ? float(ec.a) * (1.0 / 255.0)\n"
                                "                                     : g584ExactA(ec.a));\n") ||
                    !replaceOne(g560Fs,
                                "    o = vec4(c.rgb, outA);\n",
                                "    g560Factors(min(c.a * 1.9921875, 1.0));\n"
                                "    o = vec4(c.rgb, outA);\n"))
                {
                    std::fprintf(stderr, "[G560:backend] shader transform failed; legacy path kept\n");
                    return false;
                }

                GLuint g560Vs = 0u, g560FsId = 0u;
                std::string g560Log;
                if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kG560VS, g560Vs, g560Log))
                {
                    std::fprintf(stderr, "[G560:backend] VS unavailable: %s\n", g560Log.c_str());
                    return false;
                }
                if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, g560Fs.c_str(),
                                       g560FsId, g560Log))
                {
                    std::fprintf(stderr, "[G560:backend] FS unavailable: %s\n", g560Log.c_str());
                    m_f.glDeleteShader_(g560Vs);
                    return false;
                }
                m_progG560 = m_f.glCreateProgram_();
                m_f.glAttachShader_(m_progG560, g560Vs);
                m_f.glAttachShader_(m_progG560, g560FsId);
                m_f.glLinkProgram_(m_progG560);
                GLint g560Linked = 0;
                m_f.glGetProgramiv_(m_progG560, GL158_LINK_STATUS, &g560Linked);
                m_f.glDeleteShader_(g560Vs);
                m_f.glDeleteShader_(g560FsId);
                if (!g560Linked)
                {
                    std::fprintf(stderr, "[G560:backend] program link unavailable; legacy path kept\n");
                    m_f.glDeleteProgram_(m_progG560);
                    m_progG560 = 0u;
                    return false;
                }

                m_f.glUseProgram_(m_progG560);
                m_locG560FbSize = m_f.glGetUniformLocation_(m_progG560, "uFbSize");
                m_locG560FbScale = m_f.glGetUniformLocation_(m_progG560, "uFbScale");
                m_locG560Mode = m_f.glGetUniformLocation_(m_progG560, "uMode");
                m_locG560FogCol = m_f.glGetUniformLocation_(m_progG560, "uFogCol");
                m_locG560Linear = m_f.glGetUniformLocation_(m_progG560, "uLinear");
                m_locG560WrapU = m_f.glGetUniformLocation_(m_progG560, "uWrapU");
                m_locG560WrapV = m_f.glGetUniformLocation_(m_progG560, "uWrapV");
                m_locG560UvOriginX = m_f.glGetUniformLocation_(m_progG560, "uUvOriginX");
                m_locG560UvOriginY = m_f.glGetUniformLocation_(m_progG560, "uUvOriginY");
                m_locG560UvRoundU = m_f.glGetUniformLocation_(m_progG560, "uUvRoundU");
                m_locG560UvRoundV = m_f.glGetUniformLocation_(m_progG560, "uUvRoundV");
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uTex"), 0);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src1"), 1);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src2"), 2);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src3"), 3);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src4"), 4);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src5"), 5);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src6"), 6);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Src7"), 7);
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309PageMap"),
                                 static_cast<GLint>(kG309PageMapUnit));
                m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progG560, "uG309Base"),
                                 static_cast<GLint>(kG309BaseUnit));
                return true;
            };
            m_g560Available = setupG560();
            std::fprintf(stderr, "[G560:backend] flat-state ordered runs %s mode=%s\n",
                         m_g560Available ? "READY" : "UNAVAILABLE; legacy path kept",
                         g561TextureRunsRequested() ? "same-texture" :
                         (g570UntexturedRunsRequested() ? "untextured" : "blend-only"));
        }
        return true;
    }

    bool setupT8ViewProgram()
    {
        // Full-screen triangle generated from gl_VertexID. The RGBA8 map texture carries the
        // front-end's exact AddressP8 -> CT32 inverse result, so this shader performs no guessed
        // PSM layout math: it only selects the mapped byte lane and applies the captured CLUT.
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform sampler2D uMap;\n"
            "uniform sampler2D uClut;\n"
            "uniform vec2 uFbScale;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uMap,p,0)*255.0+0.5)),0,255);\n"
            "    ivec2 sp = ivec2(m.r | ((m.b & 8) << 5), m.g | ((m.b & 1) << 8));\n"
            "    ivec2 physSp = ivec2(floor(vec2(sp) * uFbScale));\n"
            "    int lane = (m.b >> 1) & 3;\n"
            "    ivec4 raw = clamp(ivec4(floor(texelFetch(uSrc,physSp,0)*255.0+0.5)),0,255);\n"
            "    int ix = lane==0 ? raw.r : (lane==1 ? raw.g : (lane==2 ? raw.b : raw.a));\n"
            "    o = texelFetch(uClut, ivec2(ix,0), 0);\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G281:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G281:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g281Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g281Prog, vs);
        m_f.glAttachShader_(m_g281Prog, fs);
        m_f.glLinkProgram_(m_g281Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g281Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g281Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g281Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G281:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g281Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uSrc"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uMap"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uClut"), 2);
        m_locG281FbScale = m_f.glGetUniformLocation_(m_g281Prog, "uFbScale");
        std::fprintf(stderr, "[G281:backend] PASS: resident PSMT8 view program ready\n");
        return true;
    }

    bool setupCt24ViewProgram()
    {
        // G338: resident CT24-alpha view. A CT24 texture whose base/width equal a resident CT32
        // FBO is that FBO's RGB with alpha re-derived per-texel from TEXA (a=(aem&&rgb==0)?0:ta0,
        // exactly GSMem applyTexa). This shader copies RGB straight from the producer FBO and
        // re-writes only alpha, producing a texture byte-identical to the legacy CT24->RGBA8
        // decode. The consumer then does its own GL_LINEAR, so filtering matches the legacy path
        // regardless of scale. Identity map: view texel (x,y)=GS pixel (x,y); the FBO stores GS
        // row y at GL row (uSrcH-1-y), so flip the sampled row here so the OUTPUT texture is
        // top-origin like any decoded texture (consumer samples it without the g261Bound flip).
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform int uSrcH;\n"
            "uniform int uTa0;\n"
            "uniform int uAem;\n"
            "uniform vec2 uFbScale;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec2 sp = ivec2(p.x, (uSrcH - 1) - p.y);\n"
            "    ivec2 physSp = ivec2(floor(vec2(sp) * uFbScale));\n"
            "    ivec4 raw = clamp(ivec4(floor(texelFetch(uSrc, physSp, 0) * 255.0 + 0.5)), 0, 255);\n"
            "    bool rgb0 = (raw.r == 0 && raw.g == 0 && raw.b == 0);\n"
            "    int a = (uAem != 0 && rgb0) ? 0 : uTa0;\n"
            "    o = vec4(vec3(raw.rgb) * (1.0 / 255.0), float(a) * (1.0 / 255.0));\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G338:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G338:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g338Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g338Prog, vs);
        m_f.glAttachShader_(m_g338Prog, fs);
        m_f.glLinkProgram_(m_g338Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g338Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g338Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g338Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G338:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g338Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g338Prog, "uSrc"), 0);
        m_locG338SrcH = m_f.glGetUniformLocation_(m_g338Prog, "uSrcH");
        m_locG338Ta0 = m_f.glGetUniformLocation_(m_g338Prog, "uTa0");
        m_locG338Aem = m_f.glGetUniformLocation_(m_g338Prog, "uAem");
        m_locG338FbScale = m_f.glGetUniformLocation_(m_g338Prog, "uFbScale");
        std::fprintf(stderr, "[G338:backend] PASS: resident CT24-alpha view program ready\n");
        return true;
    }

    // G338: build/refresh the resident CT24-alpha view of a CT32 producer FBO. Runs on the backend
    // worker thread inside the GL context (same contract as prepareT8ViewOnWorker). Returns true
    // when m_texCache[texKey] holds the exact CT24->RGBA8 decode of srcFbp's current pixels.
    bool prepareCt24ViewOnWorker(uint64_t texKey, uint32_t srcFbp, int srcFbW, int texW, int texH,
                                 int ta0, int aem, bool verify, uint64_t &verifyBad)
    {
        if (!dc2ShouldExecuteRenderEvent("prepareCt24", 0, texKey, srcFbp, 0, 0, static_cast<float>(texW), static_cast<float>(texH), 0, 0, 0, 0, 0))
            return true;
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G338:backend-fail] n=%llu stage=%s src=%03x fbw=%d tex=%dx%d\n",
                             static_cast<unsigned long long>(s_failN), stage, srcFbp, srcFbW,
                             texW, texH);
            return false;
        };
        verifyBad = 0u;
        if (m_g338Prog == 0u || texKey == 0u || texW <= 0 || texH <= 0)
            return fail("args");
        const auto is = m_fbos.find(srcFbp);
        if (is == m_fbos.end() || is->second.colorTex == 0u ||
            is->second.w != srcFbW || is->second.h != 512)
            return fail("source");
        const FboEntry &src = is->second;

        auto dropOutput = [&]() {
            const auto it = m_texCache.find(texKey);
            if (it != m_texCache.end())
            {
                if (it->second.id != 0u)
                    glDeleteTextures(1, &it->second.id);
                m_texBytes -= std::min(
                    m_texBytes, static_cast<size_t>(it->second.w) * it->second.h * 4u);
                m_texCache.erase(it);
            }
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.erase(texKey);
        };

        g494DrainErrors(kG494Ct24Drain);
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return fail("objects");

        TexEntry &out = m_texCache[texKey];
        // G492: the full-screen draw below rewrites every texel of this output, so reallocating its
        // storage each build is redundant. Reuse it only when THIS function allocated it at exactly
        // these dimensions (g492Ct24Alloc), which makes the skip bit-exact by construction.
        // See g492_overlap_ceiling.inc.
        const bool g492Reuse = out.id != 0u && out.g492Ct24Alloc != 0u &&
                               out.w == texW && out.h == texH && g492Ct24ReuseActive();
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u)
            {
                dropOutput();
                return fail("output");
            }
        }
        else if (!g492Reuse)
            m_texBytes -= static_cast<size_t>(out.w) * out.h * 4u;
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        if (!g492Reuse)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            // G496b: the ONE site outside the draw loop that parameterises an m_texCache entry.
            // Record what it set so the per-object sampler cache never goes stale.
            out.g496MinF = GL_NEAREST;
            out.g496MagF = GL_NEAREST;
            out.g496WrapS = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.g496WrapT = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.w = texW;
            out.h = texH;
            out.g492Ct24Alloc = 1u;
            m_texBytes += static_cast<size_t>(texW) * texH * 4u;
        }
        out.lastUse = m_batchCounter + 1u;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, out.id, 0);
        if (g494FboStatus(kG494Ct24Fbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            dropOutput();
            return fail("framebuffer");
        }
        glViewport(0, 0, texW, texH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g338Prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_f.glUniform1i_(m_locG338SrcH, src.h);
        m_f.glUniform1i_(m_locG338Ta0, ta0 & 0xFF);
        m_f.glUniform1i_(m_locG338Aem, aem != 0 ? 1 : 0);
        if (m_locG338FbScale >= 0)
            m_e.glUniform2f_(m_locG338FbScale,
                             dc2IsNativeHighResActive() ? dc2GetNativeScaleX() : 1.0f,
                             dc2IsNativeHighResActive() ? dc2GetNativeScaleY() : 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494Ct24Err) != GL_NO_ERROR)
        {
            dropOutput();
            return fail("draw");
        }

        if (verify)
        {
            std::vector<uint32_t> srcPx(static_cast<size_t>(src.w) * src.h);
            std::vector<uint32_t> outPx(static_cast<size_t>(texW) * texH);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, srcPx.data());
            glBindTexture(GL_TEXTURE_2D, out.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outPx.data());
            const uint32_t ta = static_cast<uint32_t>(ta0 & 0xFF);
            for (int yo = 0; yo < texH; ++yo)
                for (int x = 0; x < texW; ++x)
                {
                    // out row yo (top-origin) == GS row yo == src GL row (src.h-1-yo).
                    const int sRow = src.h - 1 - yo;
                    if (x >= src.w || sRow < 0)
                    {
                        ++verifyBad;
                        continue;
                    }
                    const uint32_t raw = srcPx[static_cast<size_t>(sRow) * src.w + x];
                    const bool rgb0 = (raw & 0x00FFFFFFu) == 0u;
                    const uint32_t a = (aem != 0 && rgb0) ? 0u : ta;
                    const uint32_t expected = (raw & 0x00FFFFFFu) | (a << 24u);
                    if (outPx[static_cast<size_t>(yo) * texW + x] != expected)
                        ++verifyBad;
                }
            static uint64_t s_verifyN = 0u;
            ++s_verifyN;
            if (verifyBad != 0u || s_verifyN <= 8u || (s_verifyN % 256u) == 0u)
                std::fprintf(stderr, "[G338:vfy] n=%llu texels=%zu bad=%llu\n",
                             static_cast<unsigned long long>(s_verifyN), outPx.size(),
                             static_cast<unsigned long long>(verifyBad));
        }
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(texKey);
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool setupAuthoritativeCompositeProgram()
    {
        // Full-screen transient composite. gl_FragCoord is in GL bottom-first space; convert
        // it to the strip's GS top-first page/local coordinates, select that page's owner, then
        // map the identical CT32 within-page coordinate into the owner's own FRAME/FBW layout.
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc0;\n"
            "uniform sampler2D uSrc1;\n"
            "uniform sampler2D uSrc2;\n"
            "uniform sampler2D uSrc3;\n"
            "uniform sampler2D uSrc4;\n"
            "uniform sampler2D uSrc5;\n"
            "uniform sampler2D uSrc6;\n"
            "uniform sampler2D uSrc7;\n"
            "uniform sampler2D uPageMap;\n"
            "uniform sampler2D uBase;\n"
            "uniform vec2 uFbScale;\n"
            "vec4 fetchOwner(int owner, vec2 p){\n"
            "    if (owner == 0) return texture(uSrc0, p * uFbScale / vec2(textureSize(uSrc0, 0)));\n"
            "    if (owner == 1) return texture(uSrc1, p * uFbScale / vec2(textureSize(uSrc1, 0)));\n"
            "    if (owner == 2) return texture(uSrc2, p * uFbScale / vec2(textureSize(uSrc2, 0)));\n"
            "    if (owner == 3) return texture(uSrc3, p * uFbScale / vec2(textureSize(uSrc3, 0)));\n"
            "    if (owner == 4) return texture(uSrc4, p * uFbScale / vec2(textureSize(uSrc4, 0)));\n"
            "    if (owner == 5) return texture(uSrc5, p * uFbScale / vec2(textureSize(uSrc5, 0)));\n"
            "    if (owner == 6) return texture(uSrc6, p * uFbScale / vec2(textureSize(uSrc6, 0)));\n"
            "    return texture(uSrc7, p * uFbScale / vec2(textureSize(uSrc7, 0)));\n"
            "}\n"
            "void main(){\n"
            "    vec2 p = gl_FragCoord.xy / uFbScale;\n"
            "    float gsY = 511.0 - p.y;\n"
            "    ivec2 page = ivec2(int(p.x) >> 6, int(gsY) >> 5);\n"
            "    vec2 local = vec2(fract(p.x / 64.0) * 64.0, fract(gsY / 32.0) * 32.0);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uPageMap, page, 0) * 255.0 + 0.5)), 0, 255);\n"
            "    if (m.r == 0) { o = texture(uBase, (p + vec2(0.5)) / 512.0); return; }\n"
            "    int owner = m.r - 1;\n"
            "    vec2 srcGs = vec2(float(m.g) * 64.0 + local.x, float(m.b) * 32.0 + local.y);\n"
            "    vec2 srcGl = vec2(srcGs.x, 511.0 - srcGs.y) + vec2(0.5);\n"
            "    o = fetchOwner(owner, srcGl);\n"
            "}\n";
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G309:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G309:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g309Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g309Prog, vs);
        m_f.glAttachShader_(m_g309Prog, fs);
        m_f.glLinkProgram_(m_g309Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g309Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g309Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g309Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G309:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g309Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc0"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc1"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc2"), 2);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc3"), 3);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc4"), 4);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc5"), 5);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc6"), 6);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc7"), 7);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uPageMap"),
                         static_cast<GLint>(kG309PageMapUnit));
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uBase"),
                         static_cast<GLint>(kG309BaseUnit));
        m_locG309FbScale = m_f.glGetUniformLocation_(m_g309Prog, "uFbScale");
        std::fprintf(stderr, "[G309:backend] PASS: authoritative-page composite program ready\n");
        return true;
    }

    bool setupExactProgram()
    {
        // G256 is sprite-only and color-only by contract.  The vertex path intentionally keeps
        // the already-proven G255 integer rectangle coverage; the fragment path below replaces
        // normalized sampling/combine/fixed-function blending with the CPU oracle's arithmetic.
        static const char *kVS =
            "#version 430 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aSTQ;\n"
            "layout(location=2) in vec4 aColor;\n"
            "uniform vec2 uFbSize;\n"
            "noperspective out vec3 vSTQ;\n"
            "noperspective out vec4 vColor;\n"
            "void main(){\n"
            "    vSTQ = aSTQ; vColor = aColor;\n"
            "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
            "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
            "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 430 core\n"
            "noperspective in vec3 vSTQ;\n"
            "noperspective in vec4 vColor;\n"
            "layout(location=0) out vec4 oCoverage;\n"
            "layout(rgba8, binding=1) uniform coherent image2D uFb;\n"
            "uniform sampler2D uTex;\n"
            "uniform int uMode;\n"
            "uniform int uLinear;\n"
            "uniform int uWrapU;\n"
            "uniform int uWrapV;\n"
            "int wrapCoord(int p, int n, int mode){\n"
            "    if (mode != 0) return clamp(p, 0, n - 1);\n"
            "    int r = p % n; return r < 0 ? r + n : r;\n"
            "}\n"
            "int floorDiv16(int v){ return v >= 0 ? v / 16 : -((-v + 15) / 16); }\n"
            "int floorDiv128(int v){ return v >= 0 ? v / 128 : -((-v + 127) / 128); }\n"
            "ivec4 texel8(ivec2 p){\n"
            "    ivec2 n = textureSize(uTex, 0);\n"
            "    p.x = wrapCoord(p.x, n.x, uWrapU);\n"
            "    p.y = wrapCoord(p.y, n.y, uWrapV);\n"
            "    return clamp(ivec4(floor(texelFetch(uTex, p, 0) * 255.0 + 0.5)), 0, 255);\n"
            "}\n"
            "ivec4 sampleExact(vec2 uvTex){\n"
            // CPU drawSprite reaches exact 12.4 half-steps and std::lround resolves them upward.
            // GPU noperspective interpolation can land a few ulps below the same mathematical
            // tie (G258: 409.5 -> 409), so stabilize only that quantization boundary.
            "    ivec2 f16 = clamp(ivec2(floor(uvTex * 16.0 + 0.5001)), ivec2(0), ivec2(65535));\n"
            "    if (uLinear == 0) return texel8(f16 / 16);\n"
            "    ivec2 shifted = f16 - ivec2(8);\n"
            "    ivec2 p0 = ivec2(floorDiv16(shifted.x), floorDiv16(shifted.y));\n"
            "    ivec2 fr = shifted - p0 * 16;\n"
            "    ivec4 c00 = texel8(p0);\n"
            "    ivec4 c10 = texel8(p0 + ivec2(1,0));\n"
            "    ivec4 c01 = texel8(p0 + ivec2(0,1));\n"
            "    ivec4 c11 = texel8(p0 + ivec2(1,1));\n"
            "    int wx0 = 16 - fr.x, wy0 = 16 - fr.y;\n"
            "    ivec4 sum = c00 * (wx0 * wy0) + c10 * (fr.x * wy0) +\n"
            "                c01 * (wx0 * fr.y) + c11 * (fr.x * fr.y);\n"
            "    return clamp((sum + ivec4(128)) / 256, 0, 255);\n"
            "}\n"
            "ivec4 combineExact(ivec4 vc, ivec4 tc){\n"
            "    int tfx = (uMode >> 1) & 3; bool tcc = (uMode & 8) != 0;\n"
            "    ivec4 c = tc;\n"
            "    if (tfx == 0) {\n"
            "        c.rgb = (tc.rgb * vc.rgb) >> 7; c.a = tcc ? ((tc.a * vc.a) >> 7) : vc.a;\n"
            "    } else if (tfx == 1) { c.a = tcc ? tc.a : vc.a;\n"
            "    } else {\n"
            "        c.rgb = ((tc.rgb * vc.rgb) >> 7) + ivec3(vc.a);\n"
            "        c.a = (tfx == 2) ? (tcc ? tc.a + vc.a : vc.a) : (tcc ? tc.a : vc.a);\n"
            "    }\n"
            "    return clamp(c, 0, 255);\n"
            "}\n"
            "int blendStd(int s, int d, int a){ return clamp(floorDiv128((s-d)*a) + d, 0, 255); }\n"
            "int blendAdd(int s, int d, int a){ return clamp((s*a)/128 + d, 0, 255); }\n"
            "void main(){\n"
            "    ivec4 src = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255);\n"
            "    if ((uMode & 1) != 0) {\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        src = combineExact(src, sampleExact(uv * vec2(textureSize(uTex, 0))));\n"
            "    }\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec4 dst = clamp(ivec4(floor(imageLoad(uFb, p) * 255.0 + 0.5)), 0, 255);\n"
            "    int blend = (uMode >> 8) & 3; ivec4 outc = src;\n"
            "    bool opaque139 = (uMode & 1024) != 0 && (dst.r | dst.g | dst.b) == 0;\n"
            "    if (!opaque139 && blend == 1) {\n"
            "        outc.r = blendStd(src.r,dst.r,src.a); outc.g = blendStd(src.g,dst.g,src.a);\n"
            "        outc.b = blendStd(src.b,dst.b,src.a);\n"
            "    } else if (!opaque139 && blend == 2) {\n"
            "        outc.r = blendAdd(src.r,dst.r,src.a); outc.g = blendAdd(src.g,dst.g,src.a);\n"
            "        outc.b = blendAdd(src.b,dst.b,src.a);\n"
            "    }\n"
            "    outc.a = src.a;\n"
            "    imageStore(uFb, p, vec4(outc) * (1.0 / 255.0));\n"
            "    oCoverage = vec4(1.0);\n"
            "}\n";

        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G256:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G256:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_progExact = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_progExact, vs);
        m_f.glAttachShader_(m_progExact, fs);
        m_f.glLinkProgram_(m_progExact);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_progExact, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_progExact, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_progExact, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
            std::fprintf(stderr, "[G256:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_progExact);
        m_locExactFbSize = m_f.glGetUniformLocation_(m_progExact, "uFbSize");
        m_locExactMode = m_f.glGetUniformLocation_(m_progExact, "uMode");
        m_locExactLinear = m_f.glGetUniformLocation_(m_progExact, "uLinear");
        m_locExactWrapU = m_f.glGetUniformLocation_(m_progExact, "uWrapU");
        m_locExactWrapV = m_f.glGetUniformLocation_(m_progExact, "uWrapV");
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progExact, "uTex"), 0);
        std::fprintf(stderr, "[G256:backend] PASS: exact RTT fragment program ready\n");
        return true;
    }

    bool setupBuffers()
    {
        m_f.glGenVertexArrays_(1, &m_vao);
        m_f.glBindVertexArray_(m_vao);
        m_f.glGenBuffers_(1, &m_vbo);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        static_assert(sizeof(G178Vtx) == 48,
                      "G178Vtx must be tightly packed (6 floats + RGBA/fog + G561 state)");
        m_f.glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(0));
        m_f.glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(12));
        m_f.glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(24));
        // G364: per-vertex fog byte (normalized; .x carries F, .yzw are the struct padding).
        m_f.glVertexAttribPointer_(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(28));
        m_f.glVertexAttribPointer_(4, 4, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(32));
        m_f.glEnableVertexAttribArray_(0);
        m_f.glEnableVertexAttribArray_(1);
        m_f.glEnableVertexAttribArray_(2);
        m_f.glEnableVertexAttribArray_(3);
        m_f.glEnableVertexAttribArray_(4);
        return true;
    }

    bool runG257ImageStoreDiagnostic()
    {
        constexpr int kW = 4;
        constexpr int kH = 4;
        static const char *kVS =
            "#version 430 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "void main(){ gl_Position = vec4(aPos.xy, 0.0, 1.0); }\n";
        static const char *kFS =
            "#version 430 core\n"
            "layout(location=0) out vec4 oCoverage;\n"
            "layout(rgba8, binding=0) uniform writeonly image2D uImage;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    imageStore(uImage, p, vec4(float(p.x + 1), float(p.y + 17), 90.0, 195.0) / 255.0);\n"
            "    oCoverage = vec4(1.0, 0.0, 1.0, 1.0);\n"
            "}\n";

        auto drainErrors = []() {
            unsigned count = 0;
            GLenum first = GL_NO_ERROR;
            for (GLenum e = glGetError(); e != GL_NO_ERROR && count < 32; e = glGetError())
            {
                if (count == 0)
                    first = e;
                ++count;
            }
            return std::pair<GLenum, unsigned>{first, count};
        };
        const auto staleErrors = drainErrors();
        const char *glVersion = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        const char *glslVersion = reinterpret_cast<const char *>(glGetString(0x8B8C));
        GLint imageLoad = 0;
        GLint imageStore = 0;
        m_e.glGetInternalformativ_(GL_TEXTURE_2D, GL158_RGBA8, GL257_SHADER_IMAGE_LOAD, 1,
                                   &imageLoad);
        m_e.glGetInternalformativ_(GL_TEXTURE_2D, GL158_RGBA8, GL257_SHADER_IMAGE_STORE, 1,
                                   &imageStore);
        const auto capabilityErrors = drainErrors();
        std::fprintf(stderr,
                     "[G257:image] caps gl=%s glsl=%s rgba8Load=%d rgba8Store=%d staleErr=0x%x/%u capErr=0x%x/%u\n",
                     glVersion ? glVersion : "?", glslVersion ? glslVersion : "?",
                     imageLoad, imageStore, staleErrors.first, staleErrors.second,
                     capabilityErrors.first, capabilityErrors.second);

        GLuint vs = 0;
        GLuint fs = 0;
        GLuint prog = 0;
        GLuint imageTex = 0;
        GLuint coverageTex = 0;
        GLuint coverageFbo = 0;
        auto cleanup = [&]() {
            m_e.glBindImageTexture_(0, 0, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
            m_f.glUseProgram_(m_prog);
            m_f.glBindVertexArray_(m_vao);
            m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            if (coverageFbo != 0)
                m_f.glDeleteFramebuffers_(1, &coverageFbo);
            if (coverageTex != 0)
                glDeleteTextures(1, &coverageTex);
            if (imageTex != 0)
                glDeleteTextures(1, &imageTex);
            if (prog != 0)
                m_f.glDeleteProgram_(prog);
            if (vs != 0)
                m_f.glDeleteShader_(vs);
            if (fs != 0)
                m_f.glDeleteShader_(fs);
        };

        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G257:image] FAIL: shader compile: %s\n", log.c_str());
            cleanup();
            return false;
        }
        prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(prog, vs);
        m_f.glAttachShader_(prog, fs);
        m_f.glLinkProgram_(prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(prog, GL158_LINK_STATUS, &linked);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> linkLog(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(prog, static_cast<GLsizei>(linkLog.size()), nullptr,
                                     linkLog.data());
            std::fprintf(stderr, "[G257:image] FAIL: program link: %s\n", linkLog.data());
            cleanup();
            return false;
        }

        std::vector<uint8_t> sentinel(static_cast<size_t>(kW * kH * 4));
        for (size_t i = 0; i < sentinel.size(); i += 4)
        {
            sentinel[i + 0] = 7;
            sentinel[i + 1] = 11;
            sentinel[i + 2] = 13;
            sentinel[i + 3] = 17;
        }
        glGenTextures(1, &imageTex);
        glBindTexture(GL_TEXTURE_2D, imageTex);
        // The direct timing path can bilinearly sample a clean guest-VRAM page. The verifier uses
        // texelFetch, so linear filtering preserves both contracts.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     sentinel.data());

        glGenTextures(1, &coverageTex);
        glBindTexture(GL_TEXTURE_2D, coverageTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        m_f.glGenFramebuffers_(1, &coverageFbo);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverageFbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    coverageTex, 0);
        const GLenum fboStatus = m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
        if (fboStatus != GL158_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[G257:image] FAIL: scratch FBO incomplete 0x%x\n", fboStatus);
            cleanup();
            return false;
        }

        std::vector<uint8_t> before(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, before.data());
        const bool sentinelOk = before == sentinel;
        const auto setupErrors = drainErrors();

        const G178Vtx tri[3] = {
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            { 3.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f,  3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
        };
        m_f.glUseProgram_(prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        m_f.glBufferData_(GL158_ARRAY_BUFFER, sizeof(tri), tri, GL178_STREAM_DRAW);
        m_e.glBindImageTexture_(0, imageTex, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
        glViewport(0, 0, kW, kH);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const auto drawErrors = drainErrors();

        std::vector<uint8_t> beforeBarrier(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, beforeBarrier.data());
        const bool preBarrierChanged = beforeBarrier != sentinel;
        const auto preBarrierErrors = drainErrors();

        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT |
                             GL178_FRAMEBUFFER_BARRIER_BIT);
        const auto barrierErrors = drainErrors();
        std::vector<uint8_t> afterBarrier(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, afterBarrier.data());
        std::vector<uint8_t> coverage(sentinel.size());
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverageFbo);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, coverage.data());
        const auto readErrors = drainErrors();

        size_t patternPixels = 0;
        size_t coveragePixels = 0;
        for (int y = 0; y < kH; ++y)
        {
            for (int x = 0; x < kW; ++x)
            {
                const size_t i = static_cast<size_t>((y * kW + x) * 4);
                if (afterBarrier[i + 0] == static_cast<uint8_t>(x + 1) &&
                    afterBarrier[i + 1] == static_cast<uint8_t>(y + 17) &&
                    afterBarrier[i + 2] == 90 && afterBarrier[i + 3] == 195)
                    ++patternPixels;
                if (coverage[i + 0] == 255 && coverage[i + 1] == 0 &&
                    coverage[i + 2] == 255 && coverage[i + 3] == 255)
                    ++coveragePixels;
            }
        }
        const bool pass = imageLoad != 0 && imageStore != 0 && sentinelOk &&
                          setupErrors.second == 0 && drawErrors.second == 0 &&
                          preBarrierErrors.second == 0 && barrierErrors.second == 0 &&
                          readErrors.second == 0 && patternPixels == kW * kH &&
                          coveragePixels == kW * kH;
        std::fprintf(stderr,
                     "[G257:image] %s sentinel=%d preBarrierChanged=%d pattern=%zu/%d coverage=%zu/%d "
                     "errSetup=0x%x/%u errDraw=0x%x/%u errPre=0x%x/%u errBarrier=0x%x/%u errRead=0x%x/%u\n",
                     pass ? "PASS" : "FAIL", sentinelOk ? 1 : 0, preBarrierChanged ? 1 : 0,
                     patternPixels, kW * kH, coveragePixels, kW * kH,
                     setupErrors.first, setupErrors.second, drawErrors.first, drawErrors.second,
                     preBarrierErrors.first, preBarrierErrors.second,
                     barrierErrors.first, barrierErrors.second, readErrors.first, readErrors.second);
        cleanup();
        return pass;
    }

    // G415: destroy only the ACTIVE geometry, leaving any parked spare intact. The create path
    // below uses this so parking survives the immediately following fresh allocation.
    void destroyActiveFbo(FboEntry &e)
    {
        if (e.fbo != 0)
            m_f.glDeleteFramebuffers_(1, &e.fbo);
        if (e.colorTex != 0)
            glDeleteTextures(1, &e.colorTex);
        if (e.depthRb != 0)
            m_e.glDeleteRenderbuffers_(1, &e.depthRb);
        e.fbo = 0;
        e.colorTex = 0;
        e.depthRb = 0;
        e.w = 0;
        e.h = 0;
    }

    void destroySpareFbo(FboEntry &e)
    {
        if (e.spareFbo != 0)
            m_f.glDeleteFramebuffers_(1, &e.spareFbo);
        if (e.spareColorTex != 0)
            glDeleteTextures(1, &e.spareColorTex);
        if (e.spareDepthRb != 0)
            m_e.glDeleteRenderbuffers_(1, &e.spareDepthRb);
        e.spareFbo = 0;
        e.spareColorTex = 0;
        e.spareDepthRb = 0;
        e.spareW = 0;
        e.spareH = 0;
    }

    void destroyFbo(FboEntry &e)
    {
        destroyActiveFbo(e);
        destroySpareFbo(e);
        e = FboEntry{};
    }

    FboEntry &ensureFbo(uint32_t fbp, int w, int h)
    {
        // G349 de-collide: the narrow (w!=512) 0x139 population gets a private sibling FBO key so it
        // stops destroying the persistent 512-wide display FBO (default-off; identity otherwise).
        const uint32_t fboKey = g349FboKey(fbp, w);
        FboEntry &e = m_fbos[fboKey];
        // G349 (behavior-pure, DC2_G349_FBOLIFE=1): the G348 Stale-When prerequisite for the
        // closure-core build — is a family target's persistent FBO DESTROYED between flushes, or
        // merely not-yet-bound? m_fbos is keyed by fbp ALONE, and destroyFbo()+recreate fires on any
        // (w,h) change (the new FBO is cleared, line below). Hypothesis: 0x139 is written at both
        // fbw=8 (512-wide display strip) and fbw=2 (128-wide narrow population, G347 census w2=22%),
        // so the two widths MUTUALLY destroy each other's FBO every frame → the snapInv 100%
        // FBO-read-fail. Trace confirms/refutes before any persistence is built.
        static const bool s_g349FboLife = (std::getenv("DC2_G349_FBOLIFE") != nullptr);
        if (s_g349FboLife &&
            (fbp == 0x139u || fbp == 0x13bu || fbp == 0x143u ||
             fbp == 0x146u || fbp == 0x14au || fbp == 0x155u))
        {
            static const uint32_t s_fam[6] = {0x139u, 0x13bu, 0x143u, 0x146u, 0x14au, 0x155u};
            static std::atomic<uint64_t> s_ev[6][3] = {}; // [fi][0=reuse 1=recreate 2=fresh-create]
            static std::atomic<uint32_t> s_recPrints{0};
            static std::atomic<uint64_t> s_agg{0};
            const bool reuse = (e.fbo != 0 && e.w == w && e.h == h);
            const int cls = reuse ? 0 : (e.fbo != 0 ? 1 : 2);
            int fi = 0;
            for (; fi < 6; ++fi)
                if (s_fam[fi] == fbp)
                    break;
            const uint64_t n = s_ev[fi][cls].fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (cls == 1 && s_recPrints.fetch_add(1u, std::memory_order_relaxed) < 40u)
                std::fprintf(stderr, "[G349:fbolife] RECREATE fbp=0x%x (%dx%d)->(%dx%d) n=%llu\n",
                             fbp, e.w, e.h, w, h, (unsigned long long)n);
            if ((s_agg.fetch_add(1u, std::memory_order_relaxed) % 512u) == 0u)
            {
                char b[288];
                int o = 0;
                b[0] = '\0';
                for (int i = 0; i < 6; ++i)
                {
                    const uint64_t ru = s_ev[i][0].load(std::memory_order_relaxed);
                    const uint64_t rc = s_ev[i][1].load(std::memory_order_relaxed);
                    const uint64_t cr = s_ev[i][2].load(std::memory_order_relaxed);
                    if ((ru | rc | cr) != 0u && o < 236)
                        o += std::snprintf(b + o, sizeof(b) - o,
                                           " %x:re=%llu rec=%llu new=%llu", s_fam[i],
                                           (unsigned long long)ru, (unsigned long long)rc,
                                           (unsigned long long)cr);
                }
                std::fprintf(stderr, "[G349:fbolife-agg]%s\n", b);
            }
        }
        // G356: display-FBO (0x0/0x68) reuse-vs-recreate census. A recreate (destroy+clear below)
        // would wipe the persistent field the Sindain menu circle depends on.
        {
            static const bool s_g356Tr = (std::getenv("DC2_G356_TRACE") != nullptr);
            if (s_g356Tr && (fbp == 0x0u || fbp == 0x68u))
            {
                const bool reuse = (e.fbo != 0 && e.w == w && e.h == h);
                static std::atomic<uint32_t> s_g356F{0};
                const uint32_t n = s_g356F.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (!reuse || n <= 8u || (n % 200u) == 0u)
                    std::fprintf(stderr,
                        "[G356:fbo] n=%u fbp=0x%x %s old(%dx%d fbo=%u) new(%dx%d)\n",
                        n, fbp, reuse ? "REUSE" : "RECREATE", e.w, e.h, e.fbo, w, h);
            }
        }
        const bool isMainTarget = (fbp == 0x000u || fbp == 0x068u || fbp == 0x139u);
        const bool allowScale = !dc2NativeMainOnlyOn() || isMainTarget;
        const int physW = allowScale ? dc2GetNativePhysicalWidth(w) : w;
        const int physH = allowScale ? dc2GetNativePhysicalHeight(h) : h;
        if (e.fbo != 0 && e.w == w && e.h == h && e.physW == physW && e.physH == physH)
            return e;
        // G415: recycle the parked geometry for this key instead of deleting and reallocating a
        // colour texture + depth renderbuffer on every width flip. The revived FBO is put into the
        // exact post-state a fresh create leaves behind: attachments re-bound, completeness
        // re-checked, creation-time texture parameters restored, colour and depth cleared with the
        // same current clear state. Fail-closed: anything unexpected drops both geometries and
        // takes the original create path.
        if (g415FboRecycleOn())
        {
            if (e.spareFbo != 0 && e.spareColorTex != 0 && e.spareDepthRb != 0 &&
                e.spareW == w && e.spareH == h && e.sparePhysW == physW && e.sparePhysH == physH)
            {
                std::swap(e.fbo, e.spareFbo);
                std::swap(e.colorTex, e.spareColorTex);
                std::swap(e.depthRb, e.spareDepthRb);
                std::swap(e.w, e.spareW);
                std::swap(e.h, e.spareH);
                std::swap(e.physW, e.sparePhysW);
                std::swap(e.physH, e.sparePhysH);
                glBindTexture(GL_TEXTURE_2D, e.colorTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                // A fresh GL texture starts at the GL_REPEAT default; sampling sites may have left
                // CLAMP_TO_EDGE on the parked one.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, e.fbo);
                m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, e.colorTex, 0);
                m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                               GL178_RENDERBUFFER, e.depthRb);
                // G494: the REVIVE path re-attaches the same colour texture and the same depth
                // renderbuffer this FBO was created and validated with, so its completeness is
                // invariant. The CREATE path's check below is deliberately NOT elided.
                if (g494FboStatus(kG494EnsureFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                        return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                    }) == GL158_FRAMEBUFFER_COMPLETE)
                {
                    glDisable(GL_SCISSOR_TEST);
                    glDepthMask(GL_TRUE);
                    glClearDepth(0.0);
                    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
                    ++m_g415FboRevives;
                    return e;
                }
                destroyFbo(e);
            }
            else if (e.fbo != 0)
            {
                destroySpareFbo(e);
                e.spareFbo = e.fbo;
                e.spareColorTex = e.colorTex;
                e.spareDepthRb = e.depthRb;
                e.spareW = e.w;
                e.spareH = e.h;
                e.sparePhysW = e.physW;
                e.sparePhysH = e.physH;
                e.fbo = 0;
                e.colorTex = 0;
                e.depthRb = 0;
                e.w = 0;
                e.h = 0;
                e.physW = 0;
                e.physH = 0;
                ++m_g415FboParks;
            }
        }
        ++m_g415FboCreates;
        destroyActiveFbo(e);
        glGenTextures(1, &e.colorTex);
        glBindTexture(GL_TEXTURE_2D, e.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, physW, physH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_e.glGenRenderbuffers_(1, &e.depthRb);
        m_e.glBindRenderbuffer_(GL178_RENDERBUFFER, e.depthRb);
        m_e.glRenderbufferStorage_(GL178_RENDERBUFFER, GL178_DEPTH_COMPONENT32F, physW, physH);
        m_f.glGenFramebuffers_(1, &e.fbo);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, e.fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    e.colorTex, 0);
        m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                       GL178_RENDERBUFFER, e.depthRb);
        const GLenum status = m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
        if (status != GL158_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[G178:backend] FAIL: FBO incomplete 0x%x (fbp=0x%x %dx%d phys=%dx%d)\n",
                         status, fbp, w, h, physW, physH);
            destroyFbo(e);
            return e;
        }
        e.w = w;
        e.h = h;
        e.physW = physW;
        e.physH = physH;
        // New FBO contents are undefined — force a depth clear; color comes from uploadFb (the
        // front-end always uploads on its first batch for an fbp, via the gen-snapshot mismatch).
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(0.0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        return e;
    }

    DepthEntry &ensureDepth(uint64_t key, int w, int h)
    {
        DepthEntry &e = m_depths[key];
        const int physW = dc2GetNativePhysicalWidth(w);
        const int physH = dc2GetNativePhysicalHeight(h);
        if (e.tex != 0 && e.w == w && e.h == h && e.physW == physW && e.physH == physH)
            return e;
        GLuint newTex = 0;
        glGenTextures(1, &newTex);
        if (newTex == 0)
            return e;
        glBindTexture(GL_TEXTURE_2D, newTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL158_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL158_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL178_DEPTH_COMPONENT32F,
                     physW, physH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        if (e.tex != 0)
            glDeleteTextures(1, &e.tex);
        e.tex = newTex;
        e.w = w;
        e.h = h;
        e.physW = physW;
        e.physH = physH;
        e.initialized = false;
        return e;
    }

    // G261: read a row window of a target's persistent FBO color attachment. The FBO must already
    // exist with matching dimensions (a wave rendered into it); anything else is a caller bug and
    // fails loudly so the front-end can drop residency instead of consuming stale VRAM silently.
    bool readColorOnWorker(uint32_t fbp, int w, int h, int glY, int rows,
                           std::vector<uint32_t> &out)
    {
        if (w <= 0 || h <= 0 || rows <= 0 || glY < 0 || glY > h || rows > h - glY)
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, w)); // G349 de-collide (narrow 0x139 → sibling)
        if (it == m_fbos.end() || it->second.fbo == 0 || it->second.w != w || it->second.h != h)
            return false;
        out.resize(static_cast<size_t>(w) * rows);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, it->second.fbo);
        glDisable(GL_SCISSOR_TEST);
        // ===== G433 ceiling probe (default OFF) =====================================================
        // DC2_G433_NO_RB=1 skips ONLY the glReadPixels call. Everything else on both arms is
        // identical: the front thread still queues the job, still blocks on the promise, still
        // resizes the buffer and still runs the full g418 unpack over the same pixel count. So the
        // arms are workload-invariant by construction (same rows, same Mpx — asserted by the
        // counters below), and the frame delta is the exact price of the blocking readback,
        // i.e. the ceiling of any asynchronous-publication scheme. Output is KNOWINGLY WRONG on the
        // probe arm (stale/garbage rows are published into guest VRAM) — measurement only.
        static const bool s_g433NoRb = (std::getenv("DC2_G433_NO_RB") != nullptr);
        static const bool s_g433Stat = (std::getenv("DC2_G433_RBSTAT") != nullptr);
        const auto t0 = s_g433Stat ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        // ===== G495 drain/transfer split (default OFF, DC2_G495_RB=1) ================================
        // [G418:edge] instruments only the ONE render-batch readback; these SIX readColor items have
        // never been split. glFinish changes no result, no GL state and no command order, so the
        // split is exact — it only moves wall time out of the transfer half and names it.
        const bool g495On = g495RbCensusOn();
        uint64_t g495DrainNs = 0ull, g495XferNs = 0ull;
        std::chrono::steady_clock::time_point g495T1;
        if (g495On)
        {
            const auto g495T0 = std::chrono::steady_clock::now();
            glFinish();
            g495T1 = std::chrono::steady_clock::now();
            g495DrainNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g495T1 - g495T0).count());
        }
        if (!s_g433NoRb)
        {
            if (it->second.physW != w || it->second.physH != h)
            {
                const float sy = static_cast<float>(it->second.physH) / static_cast<float>(h);
                const int physGlY = static_cast<int>(std::round(glY * sy));
                const int physRows = std::max(1, static_cast<int>(std::round(rows * sy)));
                const int physW = it->second.physW;
                std::vector<uint32_t> physPx(static_cast<size_t>(physW) * physRows);
                g433DoReadPixels(m_f, physGlY, physW, physRows, physPx.data());
                for (int r = 0; r < rows; ++r)
                {
                    int pr = (r * physRows) / rows;
                    if (pr >= physRows) pr = physRows - 1;
                    const uint32_t *srcRow = physPx.data() + static_cast<size_t>(pr) * physW;
                    uint32_t *dstRow = out.data() + static_cast<size_t>(r) * w;
                    for (int x = 0; x < w; ++x)
                    {
                        int px = (x * physW) / w;
                        if (px >= physW) px = physW - 1;
                        dstRow[x] = srcRow[px];
                    }
                }
            }
            else
            {
                g433DoReadPixels(m_f, glY, w, rows, out.data());
            }
        }
        if (g495On)
        {
            g495XferNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g495T1).count());
            g495NoteRead(fbp, glY, rows, w * rows, g495DrainNs, g495XferNs);
        }
        if (s_g433Stat)
        {
            static uint64_t s_n = 0ull, s_px = 0ull, s_ns = 0ull;
            s_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            ++s_n;
            s_px += static_cast<uint64_t>(w) * static_cast<uint64_t>(rows);
            if ((s_n % 512u) == 0u)
                std::fprintf(stderr,
                             "[G433:rb] norb=%d pbo=%d n=%llu ms=%.1f Mpx=%.2f usPerRead=%.1f "
                             "nsPerPx=%.2f\n",
                             s_g433NoRb ? 1 : 0, g433PboOn() ? 1 : 0,
                             static_cast<unsigned long long>(s_n),
                             (double)s_ns / 1.0e6, (double)s_px / 1.0e6,
                             (double)s_ns / 1.0e3 / (double)s_n,
                             s_px ? (double)s_ns / (double)s_px : 0.0);
        }
        return true;
    }

    // G264: write a row window of RGBA8 pixels into a target's persistent FBO color texture
    // (glTexSubImage2D — no draw, no readback). Same existence/dimension contract as
    // readColorOnWorker; the front-end owns row flipping and byte layout (VRAM CT32 == RGBA8).
    bool writeColorOnWorker(uint32_t fbp, int texW, int texH,
                            int glX, int glY, int w, int rows,
                            const std::vector<uint32_t> &in)
    {
        if (!dc2ShouldExecuteRenderEvent("writeColor", fbp, 0, 0, static_cast<float>(glX), static_cast<float>(glY), static_cast<float>(glX + w), static_cast<float>(glY + rows), 0, 0, 0, 0, 0))
            return true;
        if (texW <= 0 || texH <= 0 || glX < 0 || glY < 0 || w <= 0 || rows <= 0 ||
            glX > texW || w > texW - glX || glY > texH || rows > texH - glY ||
            in.size() != static_cast<size_t>(w) * rows)
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, texW)); // G349 de-collide (narrow 0x139 → sibling)
        if (it == m_fbos.end() || it->second.colorTex == 0 || it->second.w != texW ||
            it->second.h != texH)
            return false;
        glBindTexture(GL_TEXTURE_2D, it->second.colorTex);
        if (it->second.physW != it->second.w || it->second.physH != it->second.h)
        {
            const float sx = static_cast<float>(it->second.physW) / static_cast<float>(it->second.w);
            const float sy = static_cast<float>(it->second.physH) / static_cast<float>(it->second.h);
            const int physGlX = static_cast<int>(std::round(glX * sx));
            const int physGlY = static_cast<int>(std::round(glY * sy));
            const int physW = std::max(1, static_cast<int>(std::round(w * sx)));
            const int physRows = std::max(1, static_cast<int>(std::round(rows * sy)));
            std::vector<uint32_t> scaledIn(static_cast<size_t>(physW) * physRows);
            for (int r = 0; r < physRows; ++r)
            {
                int srcR = (r * rows) / physRows;
                if (srcR >= rows) srcR = rows - 1;
                const uint32_t *srcRow = in.data() + static_cast<size_t>(srcR) * w;
                uint32_t *dstRow = scaledIn.data() + static_cast<size_t>(r) * physW;
                for (int c = 0; c < physW; ++c)
                {
                    int srcC = (c * w) / physW;
                    if (srcC >= w) srcC = w - 1;
                    dstRow[c] = srcRow[srcC];
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, physGlX, physGlY, physW, physRows,
                            GL_RGBA, GL_UNSIGNED_BYTE, scaledIn.data());
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, glX, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                            in.data());
        }
        return g494GetError(kG494WColErr) == GL_NO_ERROR;
    }

    // ===== G627: per-byte-lane masked patch of a resident CT32 target FBO =====
    // `glTexSubImage2D` (G264's mirror) can only replace WHOLE 32-bit words, which is why a PSMT8
    // copy whose y range is not 4-aligned could not be mirrored: its boundary CT32 words mix bytes
    // the copy wrote (VRAM-authoritative) with bytes only the FBO holds. A draw CAN express that:
    // `glColorMask` selects one byte lane and the fragment shader discards where this texel's mask
    // bit for that lane is clear, so untouched lanes keep the FBO's value bit for bit and no
    // destination read is needed anywhere. Four tiny draws over the patch rect per call.
    bool setupG627LaneProgram()
    {
        if (m_g627Prog != 0u)
            return true;
        if (m_g627ProgFailed)
            return false;
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        // uOrigin/uScale map a physical FBO texel back to the logical patch texel, exactly the
        // nearest replication writeColorOnWorker performs for a high-res FBO.
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uVal;\n"
            "uniform sampler2D uMask;\n"
            "uniform vec2 uOrigin;\n"
            "uniform vec2 uScale;\n"
            "uniform int uLane;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(floor((gl_FragCoord.xy - uOrigin) / uScale));\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uMask,p,0)*255.0+0.5)),0,255);\n"
            "    int mm = uLane==0 ? m.r : (uLane==1 ? m.g : (uLane==2 ? m.b : m.a));\n"
            "    if (mm < 128) discard;\n"
            "    o = texelFetch(uVal, p, 0);\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G627:backend] FAIL: VS compile: %s\n", log.c_str());
            m_g627ProgFailed = true;
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G627:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            m_g627ProgFailed = true;
            return false;
        }
        const GLuint prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(prog, vs);
        m_f.glAttachShader_(prog, fs);
        m_f.glLinkProgram_(prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(prog, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
            std::fprintf(stderr, "[G627:backend] FAIL: program link: %s\n", buf.data());
            m_f.glDeleteProgram_(prog);
            m_g627ProgFailed = true;
            return false;
        }
        m_g627Prog = prog;
        m_f.glUseProgram_(m_g627Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g627Prog, "uVal"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g627Prog, "uMask"), 1);
        m_locG627Origin = m_f.glGetUniformLocation_(m_g627Prog, "uOrigin");
        m_locG627Scale = m_f.glGetUniformLocation_(m_g627Prog, "uScale");
        m_locG627Lane = m_f.glGetUniformLocation_(m_g627Prog, "uLane");
        std::fprintf(stderr, "[G627:backend] PASS: lane-masked FBO patch program ready\n");
        return true;
    }

    bool writeColorMaskedOnWorker(uint32_t fbp, int texW, int texH,
                                  int glX, int glY, int w, int rows,
                                  const std::vector<uint32_t> &in,
                                  const std::vector<uint32_t> &mask)
    {
        if (!dc2ShouldExecuteRenderEvent("g627LanePatch", fbp, 0, 0, static_cast<float>(glX),
                                         static_cast<float>(glY), static_cast<float>(glX + w),
                                         static_cast<float>(glY + rows), 0, 0, 0, 0, 0))
            return true;
        if (texW <= 0 || texH <= 0 || glX < 0 || glY < 0 || w <= 0 || rows <= 0 ||
            glX > texW || w > texW - glX || glY > texH || rows > texH - glY ||
            in.size() != static_cast<size_t>(w) * rows || mask.size() != in.size())
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, texW)); // G349 de-collide, same key writeColor uses
        if (it == m_fbos.end() || it->second.fbo == 0 || it->second.colorTex == 0 ||
            it->second.w != texW || it->second.h != texH)
            return false;
        if (!setupG627LaneProgram())
            return false;
        const FboEntry &dst = it->second;
        const float sx = (dst.physW > 0 && dst.w > 0)
                             ? static_cast<float>(dst.physW) / static_cast<float>(dst.w)
                             : 1.0f;
        const float sy = (dst.physH > 0 && dst.h > 0)
                             ? static_cast<float>(dst.physH) / static_cast<float>(dst.h)
                             : 1.0f;
        const int physX = static_cast<int>(std::round(glX * sx));
        const int physY = static_cast<int>(std::round(glY * sy));
        const int physW = std::max(1, static_cast<int>(std::round(w * sx)));
        const int physRows = std::max(1, static_cast<int>(std::round(rows * sy)));

        g494DrainErrors(kG494WColErr);
        if (m_g627ValTex == 0u)
            glGenTextures(1, &m_g627ValTex);
        if (m_g627MaskTex == 0u)
            glGenTextures(1, &m_g627MaskTex);
        if (m_g627ValTex == 0u || m_g627MaskTex == 0u)
            return false;
        const bool fresh = (m_g627TexW != w || m_g627TexH != rows);
        const auto upload = [&](GLuint tex, const std::vector<uint32_t> &px) {
            glBindTexture(GL_TEXTURE_2D, tex);
            if (fresh)
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, w, rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            }
            else
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, rows,
                                GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            }
        };
        m_f.glActiveTexture_(GL158_TEXTURE0);
        upload(m_g627ValTex, in);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        upload(m_g627MaskTex, mask);
        m_g627TexW = w;
        m_g627TexH = rows;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, dst.fbo);
        glViewport(physX, physY, physW, physRows);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        m_f.glUseProgram_(m_g627Prog);
        if (m_locG627Origin >= 0)
            m_e.glUniform2f_(m_locG627Origin, static_cast<float>(physX),
                             static_cast<float>(physY));
        if (m_locG627Scale >= 0)
            m_e.glUniform2f_(m_locG627Scale,
                             static_cast<float>(physW) / static_cast<float>(w),
                             static_cast<float>(physRows) / static_cast<float>(rows));
        m_f.glBindVertexArray_(m_vao);
        for (int lane = 0; lane < 4; ++lane)
        {
            glColorMask(lane == 0 ? GL_TRUE : GL_FALSE, lane == 1 ? GL_TRUE : GL_FALSE,
                        lane == 2 ? GL_TRUE : GL_FALSE, lane == 3 ? GL_TRUE : GL_FALSE);
            m_f.glUniform1i_(m_locG627Lane, lane);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return g494GetError(kG494WColErr) == GL_NO_ERROR;
    }

    // Native High-Res Presentation Read: read active display FBO directly at its native physical resolution.
    bool readDisplayFboOnWorker(uint32_t fbp, uint32_t fallbackFbp, uint32_t &outW, uint32_t &outH,
                                std::vector<uint8_t> &outRgba)
    {
        dc2RenderEventCounter().store(0u, std::memory_order_relaxed);
        auto it = m_fbos.find(g349FboKey(fbp, 512));
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(fbp);
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(fallbackFbp);
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(0x068u);
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(0x000u);
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(0x139u);
        if (it == m_fbos.end() || it->second.fbo == 0)
            it = m_fbos.find(kG309CompositeFbp);
        if (it == m_fbos.end() || it->second.fbo == 0)
            return false;

        const FboEntry &fbo = it->second;
        const int srcW = fbo.physW > 0 ? fbo.physW : fbo.w;
        const int srcH = fbo.physH > 0 ? fbo.physH : fbo.h;
        if (srcW <= 0 || srcH <= 0)
            return false;

        const int dispW = srcW;
        const int dispH = srcH;

        outW = static_cast<uint32_t>(dispW);
        outH = static_cast<uint32_t>(dispH);
        outRgba.resize(static_cast<size_t>(dispW) * dispH * 4u);

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
        glDisable(GL_SCISSOR_TEST);
        std::vector<uint32_t> glPixels(static_cast<size_t>(srcW) * srcH);
        glReadPixels(0, 0, srcW, srcH, GL_RGBA, GL_UNSIGNED_BYTE, glPixels.data());

        // GL is bottom-origin, convert to top-origin presentation scanlines
        for (int y = 0; y < dispH; ++y)
        {
            const int glY = srcH - 1 - (y * srcH) / dispH;
            const uint32_t *srcRow = glPixels.data() + static_cast<size_t>(glY) * srcW;
            uint32_t *dstRow = reinterpret_cast<uint32_t *>(outRgba.data() + static_cast<size_t>(y) * dispW * 4u);
            if (dispW == srcW)
            {
                std::memcpy(dstRow, srcRow, static_cast<size_t>(dispW) * 4u);
            }
            else
            {
                for (int x = 0; x < dispW; ++x)
                {
                    const int srcX = (x * srcW) / dispW;
                    dstRow[x] = srcRow[srcX];
                }
            }
        }
        return true;
    }

    // G280: copy aligned CT32 page tiles from one persistent target FBO into another target's
    // FBO color texture. glCopyTexSubImage2D is a GL 1.1 core entry point (statically linked,
    // no loader) that reads the bound READ framebuffer; binding GL_FRAMEBUFFER sets both READ
    // and DRAW bindings on 3.x core. RGBA8 -> RGBA8 same-size rect copy is bit-exact. Fails
    // closed (no partial validation): every rect is bounds-checked against BOTH live FBOs
    // before the first copy so a rejected job leaves the destination untouched.
    bool copyColorRectsOnWorker(uint32_t srcFbp, uint32_t dstFbp,
                                const std::vector<int32_t> &rects6)
    {
        if (!dc2ShouldExecuteRenderEvent("copyColor", dstFbp, 0, srcFbp, 0, 0, 512, 512, 0, 0, 0, 0, 0))
            return true;
        if (srcFbp == dstFbp || rects6.empty() || (rects6.size() % 6u) != 0u)
            return false;
        auto is = m_fbos.find(srcFbp);
        auto id = m_fbos.find(dstFbp);
        if (is == m_fbos.end() || is->second.fbo == 0 ||
            id == m_fbos.end() || id->second.colorTex == 0)
            return false;
        const FboEntry &src = is->second;
        const FboEntry &dst = id->second;
        for (size_t k = 0; k < rects6.size(); k += 6u)
        {
            const int32_t sx = rects6[k], sy = rects6[k + 1];
            const int32_t dx = rects6[k + 2], dy = rects6[k + 3];
            const int32_t w = rects6[k + 4], h = rects6[k + 5];
            if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
                sx > src.w - w || sy > src.h - h || dx > dst.w - w || dy > dst.h - h)
                return false;
        }
        static uint32_t s_l2lDiagPrints = 0u;
        if (s_l2lDiagPrints++ < 2u)
        {
            std::fprintf(stderr, "\n========== [DC2 RESOLUTION DIAGNOSTIC (FRAMEBUFFER COPY)] ==========\n");
            std::fprintf(stderr, "Logical source target      = 0x%03x (w=%d, h=%d)\n", srcFbp, src.w, src.h);
            std::fprintf(stderr, "Logical destination target = 0x%03x (w=%d, h=%d)\n", dstFbp, dst.w, dst.h);
            std::fprintf(stderr, "Host source resource size  = %d x %d\n", src.physW > 0 ? src.physW : src.w, src.physH > 0 ? src.physH : src.h);
            std::fprintf(stderr, "Host destination resource size = %d x %d\n", dst.physW > 0 ? dst.physW : dst.w, dst.physH > 0 ? dst.physH : dst.h);
            if (!rects6.empty())
            {
                const float sx = static_cast<float>(src.physW > 0 ? src.physW : src.w) / static_cast<float>(src.w);
                const float sy = static_cast<float>(src.physH > 0 ? src.physH : src.h) / static_cast<float>(src.h);
                const float dx = static_cast<float>(dst.physW > 0 ? dst.physW : dst.w) / static_cast<float>(dst.w);
                const float dy = static_cast<float>(dst.physH > 0 ? dst.physH : dst.h) / static_cast<float>(dst.h);
                std::fprintf(stderr, "GS source region           = (x=%d, y=%d, w=%d, h=%d)\n", rects6[0], rects6[1], rects6[4], rects6[5]);
                std::fprintf(stderr, "GS destination region      = (x=%d, y=%d, w=%d, h=%d)\n", rects6[2], rects6[3], rects6[4], rects6[5]);
                std::fprintf(stderr, "Mapped host source rect    = (x=%d, y=%d, w=%d, h=%d)\n",
                             static_cast<int>(std::round(rects6[0] * sx)), static_cast<int>(std::round(rects6[1] * sy)),
                             static_cast<int>(std::round(rects6[4] * sx)), static_cast<int>(std::round(rects6[5] * sy)));
                std::fprintf(stderr, "Mapped host destination rect = (x=%d, y=%d, w=%d, h=%d)\n",
                             static_cast<int>(std::round(rects6[2] * dx)), static_cast<int>(std::round(rects6[3] * dy)),
                             static_cast<int>(std::round(rects6[4] * dx)), static_cast<int>(std::round(rects6[5] * dy)));
                std::fprintf(stderr, "Texture coordinates        = [u0=%.4f, v0=%.4f, u1=%.4f, v1=%.4f]\n",
                             static_cast<float>(rects6[0]) / static_cast<float>(src.w), static_cast<float>(rects6[1]) / static_cast<float>(src.h),
                             static_cast<float>(rects6[0] + rects6[4]) / static_cast<float>(src.w), static_cast<float>(rects6[1] + rects6[5]) / static_cast<float>(src.h));
            }
            std::fprintf(stderr, "======================================================================\n\n");
            std::fflush(stderr);
        }

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, src.fbo);
        glBindTexture(GL_TEXTURE_2D, dst.colorTex);
        if (src.physW != src.w || dst.physW != dst.w)
        {
            const float sx = static_cast<float>(src.physW) / static_cast<float>(src.w);
            const float sy = static_cast<float>(src.physH) / static_cast<float>(src.h);
            const float dx = static_cast<float>(dst.physW) / static_cast<float>(dst.w);
            const float dy = static_cast<float>(dst.physH) / static_cast<float>(dst.h);
            for (size_t k = 0; k < rects6.size(); k += 6u)
            {
                const int psx = static_cast<int>(std::round(rects6[k] * sx));
                const int psy = static_cast<int>(std::round(rects6[k + 1] * sy));
                const int pdx = static_cast<int>(std::round(rects6[k + 2] * dx));
                const int pdy = static_cast<int>(std::round(rects6[k + 3] * dy));
                const int pw = static_cast<int>(std::round(rects6[k + 4] * sx));
                const int ph = static_cast<int>(std::round(rects6[k + 5] * sy));
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, pdx, pdy, psx, psy, pw, ph);
            }
        }
        else
        {
            for (size_t k = 0; k < rects6.size(); k += 6u)
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, rects6[k + 2], rects6[k + 3],
                                    rects6[k], rects6[k + 1], rects6[k + 4], rects6[k + 5]);
        }
        return g494GetError(kG494CopyErr) == GL_NO_ERROR;
    }

    bool buildAuthoritativeCompositeOnWorker(
        const std::vector<uint32_t> &pageMap,
        const std::vector<uint32_t> &basePixels,
        std::vector<uint32_t> *readback,
        bool forceDraw = false,
        uint32_t outputFbp = kG309CompositeFbp)
    {
        if (!dc2ShouldExecuteRenderEvent("composite", outputFbp, 0, 0, 0, 0, 512, 512, 0, 0, 0, 0, 0))
            return true;
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G309:backend-fail] n=%llu stage=%s\n",
                             static_cast<unsigned long long>(s_failN), stage);
            return false;
        };
        if (m_g309Prog == 0u || pageMap.size() != 128u ||
            basePixels.size() != 512u * 512u)
            return fail("args");

        GLuint sourceTex[kG309SourceCount] = {};
        int sourceW[kG309SourceCount] = {};
        bool sourceUsed[kG309SourceCount] = {};
        for (const uint32_t m : pageMap)
        {
            const uint32_t owner1 = m & 0xFFu;
            if (owner1 > kG309SourceCount)
                return fail("map-owner");
            if (owner1 != 0u)
                sourceUsed[owner1 - 1u] = true;
        }
        for (uint32_t t = 0u; t < kG309SourceCount; ++t)
        {
            if (!sourceUsed[t])
                continue;
            const auto it = m_fbos.find(kG309Sources[t]);
            if (it == m_fbos.end() || it->second.colorTex == 0u ||
                it->second.w <= 0 || it->second.h != 512)
                return fail("source");
            sourceTex[t] = it->second.colorTex;
            sourceW[t] = it->second.w;
        }
        for (const uint32_t m : pageMap)
        {
            const uint32_t owner1 = m & 0xFFu;
            const uint32_t srcCol = (m >> 8u) & 0xFFu;
            const uint32_t srcRow = (m >> 16u) & 0xFFu;
            if (owner1 == 0u)
                continue;
            if (owner1 > kG309SourceCount || srcRow >= 16u ||
                srcCol * 64u + 64u > static_cast<uint32_t>(sourceW[owner1 - 1u]))
                return fail("map");
        }

        g494DrainErrors(kG494CompDrain);
        if (m_g309MapTex == 0u)
            glGenTextures(1, &m_g309MapTex);
        if (m_g309MapTex == 0u)
            return fail("map-object");
        m_f.glActiveTexture_(GL158_TEXTURE0 + kG309PageMapUnit);
        glBindTexture(GL_TEXTURE_2D, m_g309MapTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 8, 16, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pageMap.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        const bool allocateBase = m_g309BaseTex == 0u;
        if (allocateBase)
            glGenTextures(1, &m_g309BaseTex);
        if (m_g309BaseTex == 0u)
            return fail("base-object");
        m_f.glActiveTexture_(GL158_TEXTURE0 + kG309BaseUnit);
        glBindTexture(GL_TEXTURE_2D, m_g309BaseTex);
        if (allocateBase)
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 512, 512, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        // Upload only contiguous clean-page runs. GL_UNPACK_ROW_LENGTH keeps the front-end's
        // 512-pixel pitch, avoiding both a 1 MiB upload and a repack allocation.
        static constexpr GLenum kG309UnpackRowLength = 0x0CF2u;
        glPixelStorei(kG309UnpackRowLength, 512);
        for (uint32_t yp = 0u; yp < 16u; ++yp)
        {
            uint32_t xp = 0u;
            while (xp < 8u)
            {
                while (xp < 8u &&
                       (pageMap[static_cast<size_t>(yp) * 8u + xp] & 0xFFu) != 0u)
                    ++xp;
                const uint32_t runLo = xp;
                while (xp < 8u &&
                       (pageMap[static_cast<size_t>(yp) * 8u + xp] & 0xFFu) == 0u)
                    ++xp;
                if (xp == runLo)
                    continue;
                const uint32_t glY = 512u - (yp + 1u) * 32u;
                const uint32_t x = runLo * 64u;
                glTexSubImage2D(
                    GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(glY),
                    static_cast<GLsizei>((xp - runLo) * 64u), 32,
                    GL_RGBA, GL_UNSIGNED_BYTE,
                    basePixels.data() + static_cast<size_t>(glY) * 512u + x);
            }
        }
        glPixelStorei(kG309UnpackRowLength, 0);

        GLuint fallbackTex = m_g309BaseTex;
        for (uint32_t t = 0u; t < kG309SourceCount; ++t)
            if (sourceTex[t] != 0u)
            {
                fallbackTex = sourceTex[t];
                break;
            }
        for (uint32_t t = 0u; t < kG309SourceCount; ++t)
        {
            m_f.glActiveTexture_(GL158_TEXTURE0 + t);
            glBindTexture(GL_TEXTURE_2D, sourceTex[t] != 0u ? sourceTex[t] : fallbackTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        FboEntry &out = ensureFbo(outputFbp, 512, 512);
        if (out.fbo == 0u || out.colorTex == 0u)
            return fail("output");
        if (readback == nullptr && !forceDraw)
        {
            // Timing path samples the prepared sources/map directly in the display shader.
            // Keep the transient FBO only as the verifier's exact reference surface.
            m_f.glActiveTexture_(GL158_TEXTURE0);
            return g494GetError(kG494CompErr) == GL_NO_ERROR;
        }
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, out.fbo);
        glViewport(0, 0, out.physW > 0 ? out.physW : 512, out.physH > 0 ? out.physH : 512);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g309Prog);
        if (m_locG309FbScale >= 0)
            m_e.glUniform2f_(m_locG309FbScale, dc2IsNativeHighResActive() ? dc2GetNativeScaleX() : 1.0f,
                                               dc2IsNativeHighResActive() ? dc2GetNativeScaleY() : 1.0f);
        m_f.glBindVertexArray_(m_vao);
        for (uint32_t t = 0u; t < kG309SourceCount; ++t)
        {
            m_f.glActiveTexture_(GL158_TEXTURE0 + t);
            glBindTexture(GL_TEXTURE_2D, sourceTex[t] != 0u ? sourceTex[t] : fallbackTex);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494CompErr) != GL_NO_ERROR)
            return fail("draw");

        if (readback != nullptr)
        {
            readback->resize(512u * 512u);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, out.colorTex);
            if (out.physW > 0 && (out.physW != 512 || out.physH != 512))
            {
                std::vector<uint32_t> physPx(static_cast<size_t>(out.physW) * out.physH);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, physPx.data());
                for (int y = 0; y < 512; ++y)
                {
                    int py = (y * out.physH) / 512;
                    if (py >= out.physH) py = out.physH - 1;
                    const uint32_t *srcRow = physPx.data() + static_cast<size_t>(py) * out.physW;
                    uint32_t *dstRow = readback->data() + static_cast<size_t>(y) * 512;
                    for (int x = 0; x < 512; ++x)
                    {
                        int px = (x * out.physW) / 512;
                        if (px >= out.physW) px = out.physW - 1;
                        dstRow[x] = srcRow[px];
                    }
                }
            }
            else
            {
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              readback->data());
            }
            if (g494GetError(kG494CompErr) != GL_NO_ERROR)
            {
                readback->clear();
                return fail("readback");
            }
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool prepareT8ViewOnWorker(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                               int texW, int texH,
                               const std::vector<uint32_t> &mapRgba,
                               const std::vector<uint32_t> &clutRgba,
                               bool verify, uint64_t &verifyBad)
    {
        if (!dc2ShouldExecuteRenderEvent("prepareT8", 0, texKey, srcFbp, 0, 0, static_cast<float>(texW), static_cast<float>(texH), 0, 0, 0, 0, 0))
            return true;
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G281:backend-fail] n=%llu stage=%s src=%03x fbw=%d "
                             "tex=%dx%d\n",
                             static_cast<unsigned long long>(s_failN), stage,
                             srcFbp, srcFbW, texW, texH);
            return false;
        };
        verifyBad = 0u;
        if (m_g281Prog == 0u || texKey == 0u || texW <= 0 || texH <= 0 ||
            mapRgba.size() != static_cast<size_t>(texW) * texH ||
            clutRgba.size() != 256u)
            return fail("args");
        const auto is = m_fbos.find(srcFbp);
        if (is == m_fbos.end() || is->second.colorTex == 0u ||
            is->second.w != srcFbW || is->second.h != 512)
            return fail("source");
        const FboEntry &src = is->second;

        auto dropOutput = [&]() {
            const auto it = m_texCache.find(texKey);
            if (it != m_texCache.end())
            {
                if (it->second.id != 0u)
                    glDeleteTextures(1, &it->second.id);
                m_texBytes -= std::min(
                    m_texBytes, static_cast<size_t>(it->second.w) * it->second.h * 4u);
                m_texCache.erase(it);
            }
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.erase(texKey);
        };

        g494DrainErrors(kG494T8Drain);
        if (m_g281MapTex == 0u)
            glGenTextures(1, &m_g281MapTex);
        if (m_g281ClutTex == 0u)
            glGenTextures(1, &m_g281ClutTex);
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281MapTex == 0u || m_g281ClutTex == 0u || m_g281Fbo == 0u)
            return fail("objects");

        // G431: this job runs ~8.7 times per frame on MAP-0 and the GS front thread BLOCKS on it
        // (0.18 ms per job, measured by [G324:census] t8Wall). Three of those milliseconds were
        // pure GL storage churn: glTexImage2D REALLOCATES the texture every build even though the
        // map/CLUT/destination geometry is identical build after build (the G415 lesson, applied
        // to the T8 view path). Reuse the allocation with glTexSubImage2D and set the immutable
        // sampler state only when the object is first created. Pixel content, filtering, wrap and
        // draw order are unchanged, so the produced view is bit-identical.
        //
        // MEASURED NEUTRAL (G431): pooled 2v2 lean MAP-0, both orders — 48.65 ON vs 48.89 OFF,
        // arms fully overlapping. The blocking cost is the worker round-trip itself (~0.05 ms
        // wall vs 0.005 ms CPU), not the storage churn. Kept OPT-IN (DC2_G431_VIEW_REUSE=1) as
        // an exact instrument; do not quote it as a win.
        static const bool s_g431Reuse = g158EnvFlag("DC2_G431_VIEW_REUSE");
        const bool reuseStorage = s_g431Reuse;
        static GLuint s_g431MapId = 0u, s_g431ClutId = 0u;
        static int s_g431MapW = -1, s_g431MapH = -1;

        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g281MapTex);
        const bool mapFresh = m_g281MapTex != s_g431MapId ||
                              texW != s_g431MapW || texH != s_g431MapH;
        if (!reuseStorage || mapFresh)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, mapRgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            s_g431MapId = m_g281MapTex;
            s_g431MapW = texW;
            s_g431MapH = texH;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW, texH,
                            GL_RGBA, GL_UNSIGNED_BYTE, mapRgba.data());
        }

        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, m_g281ClutTex);
        const bool clutFresh = m_g281ClutTex != s_g431ClutId;
        if (!reuseStorage || clutFresh)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 256, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, clutRgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            s_g431ClutId = m_g281ClutTex;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, clutRgba.data());
        }

        TexEntry &out = m_texCache[texKey];
        bool g431OutFresh = false;
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u)
            {
                dropOutput();
                return fail("output");
            }
            g431OutFresh = true;
        }
        else
            m_texBytes -= static_cast<size_t>(out.w) * out.h * 4u;
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        // The destination is fully rewritten by the full-viewport draw below, so when the
        // existing allocation already has these exact dimensions nothing needs re-specifying.
        if (!reuseStorage || g431OutFresh || out.w != texW || out.h != texH)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        }
        out.w = texW;
        out.h = texH;
        out.lastUse = m_batchCounter + 1u;
        m_texBytes += static_cast<size_t>(texW) * texH * 4u;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, out.id, 0);
        if (g494FboStatus(kG494T8Fbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            dropOutput();
            return fail("framebuffer");
        }
        glViewport(0, 0, texW, texH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g281Prog);
        if (m_locG281FbScale >= 0)
            m_e.glUniform2f_(m_locG281FbScale,
                             dc2IsNativeHighResActive() ? dc2GetNativeScaleX() : 1.0f,
                             dc2IsNativeHighResActive() ? dc2GetNativeScaleY() : 1.0f);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494T8Err) != GL_NO_ERROR)
        {
            dropOutput();
            return fail("draw");
        }

        if (verify)
        {
            std::vector<uint32_t> srcPx(static_cast<size_t>(src.w) * src.h);
            std::vector<uint32_t> outPx(static_cast<size_t>(texW) * texH);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, srcPx.data());
            glBindTexture(GL_TEXTURE_2D, out.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outPx.data());
            for (size_t k = 0; k < outPx.size(); ++k)
            {
                const uint32_t m = mapRgba[k];
                const uint32_t sx = (m & 0xFFu) |
                                    ((((m >> 16u) & 8u) >> 3u) << 8u);
                const uint32_t sy = ((m >> 8u) & 0xFFu) |
                                    (((m >> 16u) & 1u) << 8u);
                const uint32_t lane = (m >> 17u) & 3u;
                if (sx >= static_cast<uint32_t>(src.w) ||
                    sy >= static_cast<uint32_t>(src.h))
                {
                    ++verifyBad;
                    continue;
                }
                const uint32_t raw = srcPx[static_cast<size_t>(sy) * src.w + sx];
                const uint32_t index = (raw >> (lane * 8u)) & 0xFFu;
                if (outPx[k] != clutRgba[index])
                    ++verifyBad;
            }
            static uint64_t s_verifyN = 0u;
            ++s_verifyN;
            if (verifyBad != 0u || s_verifyN <= 8u || (s_verifyN % 256u) == 0u)
                std::fprintf(stderr, "[G281:vfy] n=%llu texels=%zu bad=%llu\n",
                             static_cast<unsigned long long>(s_verifyN), outPx.size(),
                             static_cast<unsigned long long>(verifyBad));
        }
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(texKey);
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool readDepthOnWorker(uint64_t depthKey, int w, int h, int depthY, int depthRows,
                           std::vector<float> &out)
    {
        if (depthKey == 0u || w <= 0 || h <= 0 || depthRows <= 0 || depthY < 0 ||
            depthY > h || depthRows > h - depthY)
            return false;
        auto it = m_depths.find(depthKey);
        if (it == m_depths.end() || it->second.tex == 0 ||
            it->second.w != w || it->second.h != h || !it->second.initialized)
            return false;
        out.resize(static_cast<size_t>(w) * depthRows);
        FboEntry &scratch = ensureFbo(0xFFFFFFFFu, w, h);
        if (scratch.fbo == 0)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, scratch.fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                    GL_TEXTURE_2D, it->second.tex, 0);
        if (g494FboStatus(kG494DepthFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, scratch.depthRb);
            return false;
        }
        if (it->second.physW != w || it->second.physH != h)
        {
            const float sy = static_cast<float>(it->second.physH) / static_cast<float>(h);
            const int physDepthY = static_cast<int>(std::round(depthY * sy));
            const int physDepthRows = std::max(1, static_cast<int>(std::round(depthRows * sy)));
            const int physW = it->second.physW;
            std::vector<float> physDepth(static_cast<size_t>(physW) * physDepthRows);
            glReadPixels(0, physDepthY, physW, physDepthRows, GL_DEPTH_COMPONENT, GL_FLOAT, physDepth.data());
            for (int r = 0; r < depthRows; ++r)
            {
                int pr = (r * physDepthRows) / depthRows;
                if (pr >= physDepthRows) pr = physDepthRows - 1;
                const float *srcRow = physDepth.data() + static_cast<size_t>(pr) * physW;
                float *dstRow = out.data() + static_cast<size_t>(r) * w;
                for (int x = 0; x < w; ++x)
                {
                    int px = (x * physW) / w;
                    if (px >= physW) px = physW - 1;
                    dstRow[x] = srcRow[px];
                }
            }
        }
        else
        {
            glReadPixels(0, depthY, w, depthRows, GL_DEPTH_COMPONENT, GL_FLOAT, out.data());
        }
        m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                       GL178_RENDERBUFFER, scratch.depthRb);
        return true;
    }

    void evictIfNeeded()
    {
        while (m_texBytes > kG178TexCacheCapBytes && !m_texCache.empty())
        {
            uint64_t oldestKey = 0;
            uint64_t oldestUse = ~0ull;
            for (const auto &kv : m_texCache)
            {
                if (kv.second.lastUse < oldestUse && kv.second.lastUse != m_batchCounter)
                {
                    oldestUse = kv.second.lastUse;
                    oldestKey = kv.first;
                }
            }
            if (oldestUse == ~0ull)
                break; // everything is in use by the current batch — can't evict
            TexEntry &t = m_texCache[oldestKey];
            m_texBytes -= static_cast<size_t>(t.w) * t.h * 4u;
            glDeleteTextures(1, &t.id);
            m_texCache.erase(oldestKey);
            {
                std::lock_guard<std::mutex> lk(m_residentMtx);
                m_resident.erase(oldestKey);
            }
        }
    }

    // Validate every condition that can make a submission fail before the first draw. In
    // particular, a late missing-texture failure would be unsafe for G242: earlier draws may
    // already have changed a persistent guest-depth texture even though the front-end is about
    // to replay the whole batch on the CPU.
    bool validateBatchInputs(const G178Batch &b, uint64_t depthKey,
                             const std::vector<float> *depthUpload,
                             int depthY, int depthRows)
    {
        if (b.fbW <= 0 || b.fbH <= 0)
            return false;
        const uint64_t fbPixelCount = static_cast<uint64_t>(b.fbW) *
                                      static_cast<uint64_t>(b.fbH);
        if (b.uploadFb && static_cast<uint64_t>(b.fbPixels.size()) != fbPixelCount)
            return false;

        const bool guestDepth = depthKey != 0u;
        if (guestDepth)
        {
            if (depthRows <= 0 || depthY < 0 || depthY > b.fbH ||
                depthRows > b.fbH - depthY)
                return false;
            if (depthUpload != nullptr &&
                static_cast<uint64_t>(depthUpload->size()) !=
                    static_cast<uint64_t>(b.fbW) * static_cast<uint64_t>(depthRows))
                return false;
            const auto it = m_depths.find(depthKey);
            const bool initialized = it != m_depths.end() && it->second.tex != 0 &&
                                     it->second.w == b.fbW && it->second.h == b.fbH &&
                                     it->second.initialized;
            // A new/recreated depth texture has undefined rows. Its first upload must therefore
            // cover the entire target; later submissions may omit it or update a row subset.
            if (!initialized &&
                (depthUpload == nullptr || depthY != 0 || depthRows != b.fbH))
                return false;
        }
        else if (depthUpload != nullptr || depthY != 0 || depthRows != 0)
        {
            return false;
        }

        std::unordered_set<uint64_t> uploadedKeys;
        uploadedKeys.reserve(b.texUploads.size());
        for (const G178TexUpload &up : b.texUploads)
        {
            if (up.key == 0u || up.w <= 0 || up.h <= 0 ||
                static_cast<uint64_t>(up.px.size()) !=
                    static_cast<uint64_t>(up.w) * static_cast<uint64_t>(up.h))
                return false;
            uploadedKeys.insert(up.key);
        }

        for (const G178Draw &d : b.draws)
        {
            if (d.firstVtx < 0 || d.vtxCount <= 0 || (d.firstVtx % 3) != 0 ||
                (d.vtxCount % 3) != 0 ||
                static_cast<size_t>(d.firstVtx) > b.verts.size() ||
                static_cast<size_t>(d.vtxCount) >
                    b.verts.size() - static_cast<size_t>(d.firstVtx))
                return false;
            // G603: mode 6 is the display buffer's source-alpha subtractive combo
            // ((0-Cs)*As>>7 + Cd). It is admitted by the classifier only for fbp 0x0/0x68 under the
            // g265 display-wave contract; this bound is the batch-level validation that would
            // otherwise reject the whole batch and put it straight back on the CPU band replay —
            // exactly the second admission question g534 warns a lever forgets to ask.
            if (d.scX0 > d.scX1 || d.scY0 > d.scY1 || d.blend > 6u || d.tfx > 3u ||
                d.wrapU > 1u || d.wrapV > 1u ||
                (d.depthFunc & 0x0Fu) > 3u)
                return false;
            if (d.srcFbp != 0u)
            {
                // G261 FBO-texture source: no texKey, never the batch's own target (feedback),
                // and the source FBO must already exist (a wave rendered into it).
                const uint32_t srcFbp = g289RealSourceFbp(d.srcFbp);
                if (d.texKey != 0u || srcFbp == b.fbp)
                    return false;
                const auto it = m_fbos.find(srcFbp);
                if (it == m_fbos.end() || it->second.fbo == 0 || it->second.colorTex == 0)
                    return false;
            }
            else if (d.texKey != 0u && uploadedKeys.count(d.texKey) == 0u)
            {
                const auto it = m_texCache.find(d.texKey);
                if (it == m_texCache.end() || it->second.id == 0)
                    return false;
            }
        }
        return true;
    }

    // G415 stage census bookkeeping. Keyed by (fbp, skipReadback, guest-depth) so the G299
    // command-zero family (readback, no guest depth) is directly comparable with the skip-readback
    // families that share the same target. Periodic aggregate print, never an early-capped one.
    // Stage marks written by renderBatch when the census is armed. kG415Marks-1 intervals:
    // 0->1 texture uploads, 1->2 FBO ensure/bind, 2->3 framebuffer upload, 3->4 depth setup,
    // 4->5 program/VBO/state, 5->6 draw loop, 6->7 pre-readback drain probe, 7->8 colour readback.
    static constexpr int kG415Marks = 9;
    struct G415Stage
    {
        uint64_t n = 0, draws = 0, rows = 0, winRows = 0, upRows = 0;
        uint64_t ns[kG415Marks - 1]{};
    };
    void g415NoteStages(const G178Batch &b, uint64_t depthKey,
                        const std::chrono::steady_clock::time_point *marks,
                        int readRows, int winRows, int upRows)
    {
        const uint64_t key = (static_cast<uint64_t>(b.fbp) << 8) |
                             (b.skipReadback ? 2u : 0u) | (depthKey != 0u ? 1u : 0u);
        G415Stage &s = m_g415Stages[key];
        ++s.n;
        for (int i = 0; i + 1 < kG415Marks; ++i)
            s.ns[i] += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    marks[i + 1] - marks[i]).count());
        s.draws += b.draws.size();
        s.rows += static_cast<uint64_t>(readRows);
        s.winRows += static_cast<uint64_t>(winRows);
        s.upRows += static_cast<uint64_t>(upRows);
        if ((++m_g415Total % 600u) != 0u)
            return;
        static const char *const kNames[kG415Marks - 1] = {
            "tex", "fbo", "up", "z", "state", "draw", "drain", "rb"};
        std::fprintf(stderr, "[G415:stage] total=%llu",
                     static_cast<unsigned long long>(m_g415Total));
        for (const auto &kv : m_g415Stages)
        {
            const G415Stage &e = kv.second;
            if (e.n == 0u)
                continue;
            std::fprintf(stderr, " fbp=0x%x/%s%s(n=%llu",
                         static_cast<unsigned>(kv.first >> 8),
                         (kv.first & 2u) ? "skip" : "read", (kv.first & 1u) ? "+z" : "",
                         static_cast<unsigned long long>(e.n));
            for (int i = 0; i + 1 < kG415Marks; ++i)
                std::fprintf(stderr, " %s=%.3f", kNames[i],
                             static_cast<double>(e.ns[i]) / 1.0e6 / e.n);
            std::fprintf(stderr, " rows=%.1f win=%.1f upRows=%.1f draws=%.1f)",
                         static_cast<double>(e.rows) / e.n,
                         static_cast<double>(e.winRows) / e.n,
                         static_cast<double>(e.upRows) / e.n,
                         static_cast<double>(e.draws) / e.n);
        }
        std::fprintf(stderr, "\n");
    }

    // G417 diagnostic-only split of the framebuffer upload GL edge. `drainNs` is work queued
    // before the upload, `submitNs` is the host call after that queue is empty, and `waitNs` is
    // completion of this upload itself. Key by target and geometry because width-flipping 0x139
    // and the narrow work targets have materially different byte counts.
    struct G417UploadStage
    {
        uint64_t n = 0, bytes = 0;
        uint64_t drainNs = 0, submitNs = 0, waitNs = 0;
    };
    void g417NoteUpload(const G178Batch &b, int rows, uint64_t drainNs,
                        uint64_t submitNs, uint64_t waitNs)
    {
        const uint64_t key = (static_cast<uint64_t>(b.fbp) << 32) |
                             (static_cast<uint64_t>(static_cast<uint32_t>(b.fbW)) << 16) |
                             static_cast<uint32_t>(b.fbH);
        G417UploadStage &s = m_g417Uploads[key];
        ++s.n;
        s.bytes += static_cast<uint64_t>(b.fbW) *
                   static_cast<uint64_t>(rows) * sizeof(uint32_t);
        s.drainNs += drainNs;
        s.submitNs += submitNs;
        s.waitNs += waitNs;
        if ((++m_g417UploadTotal % 512u) != 0u)
            return;
        std::fprintf(stderr, "[G417:backend] total=%llu",
                     static_cast<unsigned long long>(m_g417UploadTotal));
        for (const auto &kv : m_g417Uploads)
        {
            const G417UploadStage &e = kv.second;
            if (e.n == 0u)
                continue;
            std::fprintf(
                stderr,
                " fbp=0x%x/%ux%u(n=%llu KiB=%.1f drain=%.3f submit=%.3f wait=%.3f)",
                static_cast<unsigned>(kv.first >> 32),
                static_cast<unsigned>((kv.first >> 16) & 0xffffu),
                static_cast<unsigned>(kv.first & 0xffffu),
                static_cast<unsigned long long>(e.n),
                static_cast<double>(e.bytes) / 1024.0 / e.n,
                static_cast<double>(e.drainNs) / 1.0e6 / e.n,
                static_cast<double>(e.submitNs) / 1.0e6 / e.n,
                static_cast<double>(e.waitNs) / 1.0e6 / e.n);
        }
        std::fprintf(stderr, "\n");
    }

    // G418 diagnostic-only split of the colour-readback GL edge. `drainNs` is the glFinish that
    // empties GL work queued by earlier skip-readback batches; `rbNs` is this readback's own
    // transfer and completion once that queue is empty. `kickNs` is the cost of the early-kick
    // glFlush on the skip-readback batches that produce that queue, so the drain saving and the
    // kick cost are directly comparable inside one run. Keyed by target.
    struct G418ReadEdge
    {
        uint64_t n = 0, rows = 0, drainNs = 0, rbNs = 0;
        uint64_t kicks = 0, kickNs = 0;
    };
    void g418NoteReadEdge(uint32_t fbp, int rows, uint64_t drainNs, uint64_t rbNs)
    {
        G418ReadEdge &s = m_g418Edges[fbp];
        ++s.n;
        s.rows += static_cast<uint64_t>(rows);
        s.drainNs += drainNs;
        s.rbNs += rbNs;
        g418ReportEdges();
    }
    void g418NoteKick(uint32_t fbp, uint64_t kickNs)
    {
        G418ReadEdge &s = m_g418Edges[fbp];
        ++s.kicks;
        s.kickNs += kickNs;
        g418ReportEdges();
    }
    void g418ReportEdges()
    {
        if ((++m_g418Total % 600u) != 0u)
            return;
        std::fprintf(stderr, "[G418:edge] total=%llu",
                     static_cast<unsigned long long>(m_g418Total));
        for (const auto &kv : m_g418Edges)
        {
            const G418ReadEdge &e = kv.second;
            std::fprintf(stderr, " fbp=0x%x(", static_cast<unsigned>(kv.first));
            if (e.n != 0u)
                std::fprintf(stderr, "rb=%llu rows=%.1f drain=%.3f xfer=%.3f",
                             static_cast<unsigned long long>(e.n),
                             static_cast<double>(e.rows) / e.n,
                             static_cast<double>(e.drainNs) / 1.0e6 / e.n,
                             static_cast<double>(e.rbNs) / 1.0e6 / e.n);
            if (e.kicks != 0u)
                std::fprintf(stderr, "%skick=%llu kickMs=%.4f",
                             e.n != 0u ? " " : "",
                             static_cast<unsigned long long>(e.kicks),
                             static_cast<double>(e.kickNs) / 1.0e6 / e.kicks);
            std::fprintf(stderr, ")");
        }
        std::fprintf(stderr, "\n");
    }

    bool renderBatch(G178Batch &b, uint64_t depthKey, const std::vector<float> *depthUpload,
                     int depthY, int depthRows,
                     std::vector<float> *fusedDepthReadback = nullptr,
                     int depthReadY = 0, int depthReadRows = 0,
                     const G415ColorWindow &g415Window = G415ColorWindow{})
    {
        if (!validateBatchInputs(b, depthKey, depthUpload, depthY, depthRows))
            return false;
        // G415 stage census: attribute this batch's backend work to texture upload / FBO bind /
        // framebuffer upload / depth setup / state / draw loop / drain / colour readback.
        // Behaviour-pure, default-off (DC2_G415_CENSUS=1).
        const bool g415Census = g415CensusOn();
        std::chrono::steady_clock::time_point g415Marks[kG415Marks]{};
        int g415Mark = 0;
        const auto g415Note = [&] {
            if (g415Census && g415Mark < kG415Marks)
                g415Marks[g415Mark] = std::chrono::steady_clock::now();
            ++g415Mark;
        };
        g415Note(); // 0: entry
        // Validate the fused read before uploads or draws: a rejected combined job must leave
        // persistent color/depth state untouched so the front-end can safely replay on the CPU.
        if (fusedDepthReadback != nullptr &&
            (depthKey == 0u || depthReadY < 0 || depthReadRows <= 0 ||
             depthReadY > b.fbH || depthReadRows > b.fbH - depthReadY))
            return false;
        ++m_batchCounter;

        // Texture (re)uploads: replace-in-place keyed by the front-end's descriptor hash.
        for (G178TexUpload &up : b.texUploads)
        {
            TexEntry &t = m_texCache[up.key];
            bool fresh = false;
            if (t.id == 0)
            {
                glGenTextures(1, &t.id);
                if (t.id == 0)
                    return false;
                fresh = true;
            }
            else
                m_texBytes -= static_cast<size_t>(t.w) * t.h * 4u;
            glBindTexture(GL_TEXTURE_2D, t.id);
            // G620: a re-upload at the SAME dimensions does not need new storage. `glTexImage2D`
            // reallocates the level every time — the driver frees and re-creates the mip-0 image,
            // re-validates the format and drops any residency it had. DC2 re-uploads the same
            // descriptor at the same size all frame long (the front-end key `up.key` is the whole
            // TEX0 descriptor, so `w`/`h` cannot change without the key changing except through
            // HD texture replacement, which is checked explicitly). `glTexSubImage2D` writes the
            // identical texels into the identical storage.
            //
            // Exactness: the internal format is unconditionally `GL158_RGBA8` on both arms, mip 0
            // is the only level this cache ever defines, and the sub-image covers the whole
            // rectangle `[0,w) x [0,h)`. A first upload, or any size change, still takes
            // `glTexImage2D`. Rollback: `DC2_G620_NO_SUBIMAGE=1`.
            static const bool s_g620NoSubImage = g158EnvFlag("DC2_G620_NO_SUBIMAGE");
            if (!s_g620NoSubImage && !fresh && t.w == up.w && t.h == up.h && up.w > 0 && up.h > 0)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, up.w, up.h, GL_RGBA, GL_UNSIGNED_BYTE,
                                up.px.data());
            else
                glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, up.w, up.h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, up.px.data());
            t.w = up.w;
            t.h = up.h;
            t.lastUse = m_batchCounter;
            m_texBytes += static_cast<size_t>(up.w) * up.h * 4u;
            {
                std::lock_guard<std::mutex> lk(m_residentMtx);
                m_resident.insert(up.key);
            }
        }

        // Protect every texture used by this batch from LRU eviction, then resolve all handles.
        // No texture-cache operation occurs after this point, so draw submission has no late
        // lookup failure path and pointers into the unordered_map remain stable.
        //
        // ===== G620: ONE hash lookup per DISTINCT texture, not two per DRAW =====
        // `[G415:stage]` prices this mark (`tex`) at ~0.030 ms on the 0x0/0x68 skip populations,
        // which upload almost nothing and carry ~40 draws — i.e. it is not the upload, it is
        // **two `unordered_map::find` per draw**, ~4,000 lookups/frame across 45 flushes, ~0.89 ms/f.
        //
        // Three exact changes, none of which alters what is bound to any draw:
        //   1. the two passes merge into one. The separation existed so `lastUse` was stamped
        //      BEFORE `evictIfNeeded()` and the pointers taken AFTER it. Merging is still safe:
        //      eviction never erases an entry whose `lastUse == m_batchCounter` (its own guard),
        //      it performs no insertion, and erasing OTHER elements of an `unordered_map` does not
        //      invalidate references to the ones that remain. So a pointer taken before the evict
        //      is still valid after it — and it is a pointer to an entry eviction cannot touch.
        //   2. the handle vector becomes a reused member instead of a per-flush heap allocation.
        //   3. consecutive draws overwhelmingly share one texture (the front end coalesces on
        //      `texKey`, see rasterizer_vram_materialization.inc), so a ONE-ENTRY memo answers most
        //      draws with a compare instead of a hash probe.
        // The old first pass also dereferenced `find()` without testing `end()`; the merged form
        // takes the second pass's check, so it is strictly safer as well.
        // Rollback: `DC2_G620_NO_TEXRESOLVE=1`.
        static const bool s_g620NoTexResolve = g158EnvFlag("DC2_G620_NO_TEXRESOLVE");
        m_g620DrawTex.assign(b.draws.size(), nullptr);
        std::vector<TexEntry *> &drawTextures = m_g620DrawTex;
        if (s_g620NoTexResolve)
        {
            for (const G178Draw &d : b.draws)
                if (d.texKey != 0u)
                    m_texCache.find(d.texKey)->second.lastUse = m_batchCounter;
            evictIfNeeded();
            for (size_t i = 0; i < b.draws.size(); ++i)
            {
                if (b.draws[i].texKey == 0u)
                    continue;
                const auto it = m_texCache.find(b.draws[i].texKey);
                if (it == m_texCache.end() || it->second.id == 0)
                    return false;
                drawTextures[i] = &it->second;
            }
        }
        else
        {
            uint64_t memoKey = 0u;
            TexEntry *memoEnt = nullptr;
            for (size_t i = 0; i < b.draws.size(); ++i)
            {
                const uint64_t k = b.draws[i].texKey;
                if (k == 0u)
                    continue;
                if (k != memoKey)
                {
                    const auto it = m_texCache.find(k);
                    if (it == m_texCache.end() || it->second.id == 0)
                        return false;
                    memoKey = k;
                    memoEnt = &it->second;
                }
                memoEnt->lastUse = m_batchCounter;
                drawTextures[i] = memoEnt;
            }
            evictIfNeeded();
        }

        // Allocate all caller-visible output storage before drawing. A bad_alloc is caught by the
        // worker and reported as a clean failure while persistent color/depth are still untouched.
        // G261 skipReadback waves leave the result GPU-resident: no output storage, no readback.
        if (b.skipReadback)
            b.readback.clear();
        else
            b.readback.resize(static_cast<size_t>(b.fbW) * b.fbH);
        g415Note(); // 1: texture uploads + eviction + handle resolve + output alloc

        // G351: an explicit surface key (Owner-B sprite → sibling FBO) overrides the fbp key.
        FboEntry &fbo = ensureFbo(b.fboKey != 0u ? b.fboKey : b.fbp, b.fbW, b.fbH);
        if (fbo.fbo == 0)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
        const int viewportW = fbo.physW > 0 ? fbo.physW : b.fbW;
        const int viewportH = fbo.physH > 0 ? fbo.physH : b.fbH;
        glViewport(0, 0, viewportW, viewportH);
        g415Note(); // 2: FBO ensure/bind

        static uint32_t s_3dScenePrints = 0u;
        static uint32_t s_dispScenePrints = 0u;
        const bool is3dWorldBatch = (b.fbp == 0x139u || (b.verts.size() > 50u && !b.draws.empty() && (b.draws[0].depthFunc & 0x0Fu) != 0));
        const bool isDispBatch = (b.fbp == 0x068u || b.fbp == 0x000u);
        if ((is3dWorldBatch && s_3dScenePrints++ < 2u) || (isDispBatch && s_dispScenePrints++ < 2u))
        {
            auto dIt = (depthKey != 0u) ? m_depths.find(depthKey) : m_depths.end();
            const DepthEntry *depthPtr = (dIt != m_depths.end()) ? &dIt->second : nullptr;
            std::fprintf(stderr, "\n========== [DC2 RESOLUTION DIAGNOSTIC (GS -> HOST RASTER)] ==========\n");
            std::fprintf(stderr, "Batch FBP                   = 0x%03x (fboKey=0x%x, draws=%zu, verts=%zu, %s)\n",
                         b.fbp, b.fboKey, b.draws.size(), b.verts.size(), is3dWorldBatch ? "3D_WORLD_SCENE" : "DISPLAY_COMPOSITE");
            std::fprintf(stderr, "[GS TARGET] logical=%d x %d, physical=%d x %d, GL FBO=%u, ColorTex=%u\n",
                         b.fbW, b.fbH, fbo.physW > 0 ? fbo.physW : fbo.w, fbo.physH > 0 ? fbo.physH : fbo.h, fbo.fbo, fbo.colorTex);
            if (depthPtr && depthPtr->tex != 0)
            {
                std::fprintf(stderr, "[DEPTH TARGET] logical=%d x %d, physical=%d x %d, DepthTex=%u\n",
                             depthPtr->w, depthPtr->h, depthPtr->physW > 0 ? depthPtr->physW : depthPtr->w, depthPtr->physH > 0 ? depthPtr->physH : depthPtr->h, depthPtr->tex);
            }
            else
            {
                std::fprintf(stderr, "[DEPTH TARGET] logical=%d x %d, physical=%d x %d (FBO depthRb=%u)\n",
                             b.fbW, b.fbH, fbo.physW > 0 ? fbo.physW : fbo.w, fbo.physH > 0 ? fbo.physH : fbo.h, fbo.depthRb);
            }
            if (!b.draws.empty())
            {
                const G178Draw &d0 = b.draws[0];
                const int scH = d0.scY1 - d0.scY0 + 1;
                const float sx = static_cast<float>(fbo.physW > 0 ? fbo.physW : b.fbW) / static_cast<float>(b.fbW);
                const float sy = static_cast<float>(fbo.physH > 0 ? fbo.physH : b.fbH) / static_cast<float>(b.fbH);
                std::fprintf(stderr, "Logical GS scissor            = (x0=%d, y0=%d, x1=%d, y1=%d) [W=%d, H=%d]\n",
                             d0.scX0, d0.scY0, d0.scX1, d0.scY1, d0.scX1 - d0.scX0 + 1, scH);
                std::fprintf(stderr, "Physical host scissor         = (x0=%d, y0=%d, W=%d, H=%d)\n",
                             static_cast<int>(std::round(d0.scX0 * sx)),
                             static_cast<int>(std::round((b.fbH - 1 - d0.scY1) * sy)),
                             static_cast<int>(std::round((d0.scX1 - d0.scX0 + 1) * sx)),
                             static_cast<int>(std::round(scH * sy)));
            }
            std::fprintf(stderr, "Logical drawable area         = %d x %d\n", b.fbW, b.fbH);
            std::fprintf(stderr, "Physical host viewport/drawable area = %d x %d\n", viewportW, viewportH);

            if (b.verts.size() >= 3u)
            {
                std::fprintf(stderr, "--- [3D TRIANGLE TRACE (First Triangle)] ---\n");
                for (size_t vi = 0; vi < 3u; ++vi)
                {
                    const G178Vtx &vtx = b.verts[vi];
                    const float ndcX = vtx.x / static_cast<float>(b.fbW) * 2.0f - 1.0f;
                    const float ndcY = 1.0f - vtx.y / static_cast<float>(b.fbH) * 2.0f;
                    const float rasterPxX = (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportW);
                    const float rasterPxY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(viewportH);
                    std::fprintf(stderr, "  Vertex %u: GS raw=(x=%.2f, y=%.2f, z=%.4f) -> logical screen=(%.2f, %.2f) -> NDC=(%.4f, %.4f) -> host raster pixel=(%.1f, %.1f) in target(%dx%d)\n",
                                 static_cast<unsigned>(vi), vtx.x, vtx.y, vtx.z, vtx.x, vtx.y, ndcX, ndcY, rasterPxX, rasterPxY, viewportW, viewportH);
                }
                std::fprintf(stderr, "--------------------------------------------\n");
            }
            std::fprintf(stderr, "--- [BATCH DRAWS SUMMARY (draws=%zu)] ---\n", b.draws.size());
            for (size_t di = 0; di < std::min<size_t>(b.draws.size(), 20u); ++di)
            {
                const G178Draw &dr = b.draws[di];
                float minX = 1e6f, minY = 1e6f, maxX = -1e6f, maxY = -1e6f;
                for (int vi = 0; vi < dr.vtxCount; ++vi)
                {
                    const G178Vtx &vx = b.verts[static_cast<size_t>(dr.firstVtx + vi)];
                    minX = std::min(minX, vx.x);
                    minY = std::min(minY, vx.y);
                    maxX = std::max(maxX, vx.x);
                    maxY = std::max(maxY, vx.y);
                }
                std::fprintf(stderr, "  Draw %zu: verts=%d, texKey=0x%llx, srcFbp=0x%x, blend=%u, ztst=%u, zwrite=%u, sc=[%d,%d..%d,%d], bounds=[(%.1f,%.1f)..(%.1f,%.1f)]\n",
                             di, dr.vtxCount, static_cast<unsigned long long>(dr.texKey), dr.srcFbp, dr.blend,
                             dr.depthFunc & 0x0Fu, dr.depthWrite, dr.scX0, dr.scY0, dr.scX1, dr.scY1, minX, minY, maxX, maxY);
            }
            std::fprintf(stderr, "=======================================================================\n\n");
            std::fflush(stderr);
        }

        const int g417UploadRowsThis = b.uploadFb ? b.fbH : 0;
        if (b.uploadFb && dc2ShouldExecuteRenderEvent("uploadFb", b.fbp, 0, 0, 0, 0, static_cast<float>(b.fbW), static_cast<float>(b.fbH), 0, 0, 0, 0, 0))
        {
            glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
            if (fbo.physW != b.fbW || fbo.physH != b.fbH)
            {
                std::vector<uint32_t> scaledUpload(static_cast<size_t>(fbo.physW) * fbo.physH);
                for (int py = 0; py < fbo.physH; ++py)
                {
                    int srcY = (py * b.fbH) / fbo.physH;
                    if (srcY >= b.fbH) srcY = b.fbH - 1;
                    const uint32_t *srcRow = b.fbPixels.data() + static_cast<size_t>(srcY) * b.fbW;
                    uint32_t *dstRow = scaledUpload.data() + static_cast<size_t>(py) * fbo.physW;
                    for (int px = 0; px < fbo.physW; ++px)
                    {
                        int srcX = (px * b.fbW) / fbo.physW;
                        if (srcX >= b.fbW) srcX = b.fbW - 1;
                        dstRow[px] = srcRow[srcX];
                    }
                }
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fbo.physW, fbo.physH,
                                GL_RGBA, GL_UNSIGNED_BYTE, scaledUpload.data());
            }
            else if (g417ProfileOn())
            {
                const auto drainT0 = std::chrono::steady_clock::now();
                glFinish();
                const auto submitT0 = std::chrono::steady_clock::now();
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, b.fbW, b.fbH,
                                GL_RGBA, GL_UNSIGNED_BYTE, b.fbPixels.data());
                const auto waitT0 = std::chrono::steady_clock::now();
                glFinish();
                const auto done = std::chrono::steady_clock::now();
                const auto ns = [](const auto &a, const auto &z) {
                    return static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count());
                };
                g417NoteUpload(b, b.fbH, ns(drainT0, submitT0),
                               ns(submitT0, waitT0), ns(waitT0, done));
            }
            else
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, b.fbW, b.fbH,
                                GL_RGBA, GL_UNSIGNED_BYTE, b.fbPixels.data());
            }
        }
        g415Note(); // 3: framebuffer upload

        // G242: the real guest Z target owns one persistent texture keyed by ZBP/ZBW/ZPSM, not by
        // the alternating color FBP. The front-end uploads only when guest VRAM changed and reads
        // back only at a CPU/transfer/frame boundary.
        const bool guestDepth = depthKey != 0u;
        if (guestDepth)
        {
            DepthEntry &depth = ensureDepth(depthKey, b.fbW, b.fbH);
            if (depth.tex == 0 || depth.w != b.fbW || depth.h != b.fbH)
                return false;
            if (depthUpload != nullptr)
            {
                glBindTexture(GL_TEXTURE_2D, depth.tex);
                if (depth.physW != b.fbW || depth.physH != b.fbH)
                {
                    const float sy = static_cast<float>(depth.physH) / static_cast<float>(b.fbH);
                    const int physDepthY = static_cast<int>(std::round(depthY * sy));
                    const int physDepthRows = std::max(1, static_cast<int>(std::round(depthRows * sy)));
                    std::vector<float> scaledDepth(static_cast<size_t>(depth.physW) * physDepthRows);
                    for (int py = 0; py < physDepthRows; ++py)
                    {
                        int srcY = (py * depthRows) / physDepthRows;
                        if (srcY >= depthRows) srcY = depthRows - 1;
                        const float *srcRow = depthUpload->data() + static_cast<size_t>(srcY) * b.fbW;
                        float *dstRow = scaledDepth.data() + static_cast<size_t>(py) * depth.physW;
                        for (int px = 0; px < depth.physW; ++px)
                        {
                            int srcX = (px * b.fbW) / depth.physW;
                            if (srcX >= b.fbW) srcX = b.fbW - 1;
                            dstRow[px] = srcRow[srcX];
                        }
                    }
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, physDepthY, depth.physW, physDepthRows,
                                    GL_DEPTH_COMPONENT, GL_FLOAT, scaledDepth.data());
                }
                else
                {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, depthY, b.fbW, depthRows,
                                    GL_DEPTH_COMPONENT, GL_FLOAT, depthUpload->data());
                }
                depth.initialized = true;
            }
            m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                        GL_TEXTURE_2D, depth.tex, 0);
            // G494: `depth` was just validated to be exactly b.fbW x b.fbH against the same
            // colour attachment, so completeness here is a property of an invariant attachment set.
            if (g494FboStatus(kG494RenderFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                    return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                }) != GL158_FRAMEBUFFER_COMPLETE)
            {
                m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                               GL178_RENDERBUFFER, fbo.depthRb);
                return false;
            }
        }
        else
        {
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
            // G494: this attaches the FBO's OWN depth renderbuffer — the same object, at the same
            // size, that ensureFbo already validated when it built this FBO.
            if (g494FboStatus(kG494RenderFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                    return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                }) != GL158_FRAMEBUFFER_COMPLETE)
                return false;
        }
        if (b.clearDepth && !guestDepth)
        {
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        g415Note(); // 4: depth attach / clear

        // G268: was a raw getenv() per submitted batch; env is process-constant
        // (DC2_G268_LIVE_ENVREAD=1 restores the per-call read for A/B).
        static const bool s_g255GpuRttEnvLive = g158EnvFlag("DC2_G268_LIVE_ENVREAD");
        static const bool s_g255GpuRttEnv = (std::getenv("DC2_G255_GPU_RTT") != nullptr);
        const bool g255KnownRtt =
            b.fbp == 0x139u || b.fbp == 0x13cu || b.fbp == 0x143u ||
            b.fbp == 0x146u || b.fbp == 0x155u;
        const bool g255RttAlpha =
            ((b.rttRawAlpha || (s_g255GpuRttEnvLive
                                    ? (std::getenv("DC2_G255_GPU_RTT") != nullptr)
                                    : s_g255GpuRttEnv)) && g255KnownRtt) ||
            // G570's transient 0x13d batch sets the explicit per-batch contract. Do not widen the
            // legacy environment switch to arbitrary targets; only this tagged path stores PS2's
            // raw 0..128 framebuffer alpha after fixed-function RGB blending.
            // G584 adds 0x13b, the OTHER target that already asserts rttRawAlpha (g286Transient in
            // rasterizer_vram_materialization.inc). Its flag was set and then ignored here, so its
            // blended draws had no pass that could carry the As/128 weight at all.
            (b.rttRawAlpha &&
             (b.fbp == 0x13du || b.fbp == 0x141u || b.fbp == 0x15du ||
              (g584ExactBlendAOn() && b.fbp == 0x13bu) ||
              // G604: the discovered targets are selected by PROPERTY, so they cannot be spelled
              // as an address here. They are render targets that get sampled with TCC=1 later, so
              // they need PS2's raw 0..128 framebuffer alpha exactly as 0x13d/0x141/0x15d do.
              // The front-end publishes the property as a bit set (see above), which keeps
              // it in ONE place instead of duplicating a second address list here.
              g604RawAlphaTarget(b.fbp)));
        const bool g256Exact = g256ExactRttOn() && m_progExact != 0u &&
            (b.fbp == 0x139u || b.fbp == 0x13cu || b.fbp == 0x143u ||
             b.fbp == 0x146u || b.fbp == 0x155u) && !guestDepth;
        const bool g561TextureRuns = g561TextureRunsRequested() && m_g560Available && !g256Exact;
        const bool g570UntexturedRuns =
            g570UntexturedRunsRequested() && m_g560Available && !g256Exact;
        const bool g560MultiDraw =
            (g560MultiDrawRequested() || g561TextureRuns || g570UntexturedRuns) &&
            m_g560Available && !g256Exact;

        // G561: snapshot every shader-varying piece of G178Draw into four exactly-representable
        // packed floats.  Texture/sampler, scissor, low depth state, and blend equation remain fixed
        // across a run; everything else is flat per triangle.  This duplicates the mode construction
        // in the legacy loop deliberately so its program and rollback remain independent.
        if (g560MultiDraw)
        {
            static const bool s_g561NoGuard = g158EnvFlag("DC2_G362_NO_UVGUARD");
            const auto packMode = [&](const G178Draw &d) {
                int mode = 0;
                const bool g286Exact = b.fbp == 0x13bu && d.texKey != 0u;
                const bool pointGuard = !s_g561NoGuard && !g286Exact && !d.bilinear;
                if ((d.depthFunc & 0x10u) != 0u)
                    mode |= 16;
                if ((d.depthFunc & 0x20u) != 0u)
                    mode |= 64;
                mode |= (static_cast<int>((d.depthFunc >> 6u) & 3u) << 7u);
                if (d.srcFbp != 0u)
                {
                    if (d.srcFbp == kG309CompositeToken)
                        mode |= 2048;
                    mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                    if (g289SourceIsCt24(d.srcFbp))
                        mode |= 1024;
                    if (pointGuard && (mode & 2048) == 0)
                        mode |= 4096;
                }
                if (g286Exact)
                {
                    mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0) | 512;
                    if (g584ExactBlendAOn())
                        mode |= 32768; // G584: encode o.a / the SRC1 factor as As/128.
                }
                else if (d.texKey != 0u)
                {
                    mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                    if (pointGuard)
                        mode |= 4096;
                }
                const bool accurateUv = ((d.uvRoundU | d.uvRoundV) & 8u) != 0u;
                if (accurateUv && (mode & 1) != 0 && (mode & 512) == 0)
                    mode |= 4096;
                // (G604's exact-tap routing for mode 6 measured null and was not kept; see the
                // refutation note at the matching site in the legacy loop below.)
                if (d.fge != 0u)
                    mode |= 8192;
                if (d.uvRoundU != 0u || d.uvRoundV != 0u)
                    mode |= 16384;
                if (g255RttAlpha)
                    mode |= 32;
                return static_cast<uint32_t>(mode);
            };
            for (const G178Draw &d : b.draws)
            {
                const uint32_t state0 = packMode(d) | (static_cast<uint32_t>(d.blend & 7u) << 16u);
                const uint32_t state1 = d.fogCol & 0x00FFFFFFu;
                const uint32_t state2 = static_cast<uint32_t>(d.uvOriginX) |
                                        (static_cast<uint32_t>(d.uvRoundU & 15u) << 16u);
                const uint32_t state3 = static_cast<uint32_t>(d.uvOriginY) |
                                        (static_cast<uint32_t>(d.uvRoundV & 15u) << 16u);
                for (int v = 0; v < d.vtxCount; ++v)
                {
                    G178Vtx &out = b.verts[static_cast<size_t>(d.firstVtx + v)];
                    out.drawState0 = static_cast<float>(state0);
                    out.drawState1 = static_cast<float>(state1);
                    out.drawState2 = static_cast<float>(state2);
                    out.drawState3 = static_cast<float>(state3);
                }
            }
        }

        m_f.glUseProgram_(g560MultiDraw ? m_progG560 : m_prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        m_f.glBufferData_(GL158_ARRAY_BUFFER,
                          static_cast<GLsizeiptrLocal>(b.verts.size() * sizeof(G178Vtx)),
                          b.verts.data(), GL178_STREAM_DRAW);
        const GLint locFbSize = g560MultiDraw ? m_locG560FbSize : m_locFbSize;
        const GLint locFbScale = g560MultiDraw ? m_locG560FbScale : m_locFbScale;
        const GLint locMode = g560MultiDraw ? m_locG560Mode : m_locMode;
        const GLint locFogCol = g560MultiDraw ? m_locG560FogCol : m_locFogCol;
        const GLint locLinear = g560MultiDraw ? m_locG560Linear : m_locLinear;
        const GLint locWrapU = g560MultiDraw ? m_locG560WrapU : m_locWrapU;
        const GLint locWrapV = g560MultiDraw ? m_locG560WrapV : m_locWrapV;
        const GLint locUvOriginX = g560MultiDraw ? m_locG560UvOriginX : m_locUvOriginX;
        const GLint locUvOriginY = g560MultiDraw ? m_locG560UvOriginY : m_locUvOriginY;
        const GLint locUvRoundU = g560MultiDraw ? m_locG560UvRoundU : m_locUvRoundU;
        const GLint locUvRoundV = g560MultiDraw ? m_locG560UvRoundV : m_locUvRoundV;
        m_e.glUniform2f_(locFbSize, static_cast<GLfloat>(b.fbW), static_cast<GLfloat>(b.fbH));
        if (locFbScale >= 0)
            m_e.glUniform2f_(locFbScale, dc2IsNativeHighResActive() ? dc2GetNativeScaleX() : 1.0f,
                                         dc2IsNativeHighResActive() ? dc2GetNativeScaleY() : 1.0f);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glEnable(GL_SCISSOR_TEST);
        const bool g257RealDiagnostic = g256Exact && g257ImageStoreTestOn() &&
                                        m_g257RealBatches < 32u;
        const bool g258Trace = g256Exact && g258TraceOn() && b.fbp == 0x146u &&
                               m_g258TraceBatches < 4u;
        const uint64_t g258TraceBatch = g258Trace ? ++m_g258TraceBatches : 0u;
        std::vector<uint32_t> g257ImageBefore;
        GLuint g257CoverageFbo = 0;
        GLenum g257PreError = GL_NO_ERROR;
        unsigned g257PreErrorCount = 0;
        auto g257DrainErrors = [](GLenum &first, unsigned &count) {
            first = GL_NO_ERROR;
            count = 0;
            for (GLenum e = glGetError(); e != GL_NO_ERROR && count < 32; e = glGetError())
            {
                if (count == 0)
                    first = e;
                ++count;
            }
        };
        if (g256Exact)
        {
            // Do not rasterize with the image-load/store target attached to the active FBO.
            // That is undefined feedback even with color writes masked off.  A persistent
            // same-sized scratch FBO supplies fragment coverage; the guest color texture is
            // updated exclusively through uFb and remains attached to its own inactive FBO.
            FboEntry &coverage = ensureFbo(0xFFFFFFFEu, b.fbW, b.fbH);
            if (coverage.fbo == 0)
                return false;
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverage.fbo);
            g257CoverageFbo = coverage.fbo;
            glViewport(0, 0, b.fbW, b.fbH);
            if (g257RealDiagnostic)
            {
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                g257ImageBefore.resize(static_cast<size_t>(b.fbW) * b.fbH);
                glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              g257ImageBefore.data());
                g257DrainErrors(g257PreError, g257PreErrorCount);
            }
            m_f.glUseProgram_(m_progExact);
            m_e.glUniform2f_(m_locExactFbSize, static_cast<GLfloat>(b.fbW),
                             static_cast<GLfloat>(b.fbH));
            m_e.glBindImageTexture_(1u, fbo.colorTex, 0, GL_FALSE, 0, GL178_READ_WRITE,
                                    GL158_RGBA8);
            m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                 GL178_FRAMEBUFFER_BARRIER_BIT);
            // Keep a real framebuffer output live in the scratch attachment.  Some Windows GL
            // drivers elide a fragment invocation whose only framebuffer output is fully masked,
            // even though imageStore is a side effect.  The scratch result is deliberately ignored.
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_BLEND);
        }
        g415Note(); // 5: program / VBO / uniform / exact-path state
        // G496b: redundant GL state elision. The arm is read ONCE per batch (thread-level, block
        // held) and the non-texture cache is reset here, because the composite / T8 / CT24 / copy /
        // writecolor items that run between batches own all of this state. Both arms leave GL
        // holding the same state vector — the cache is written on every path, elision only decides
        // whether the call that would set the value already there is issued.
        const int g496GlArm = g496GlStateArm(); // -1 unless DC2_G419_AB=glstate
        const bool g496GlCache =
            (g496GlArm >= 0) ? (g496GlArm == 1) : g496GlCacheDefaultOn();
        g496GlReset();
        const auto g496Uni = [&](GLint loc, int value) { m_f.glUniform1i_(loc, value); };

        const auto g560StateEqual = [](const G178Draw &a, const G178Draw &z) {
            return a.firstVtx + a.vtxCount == z.firstVtx &&
                   g603RevSubBlend(a.blend) == g603RevSubBlend(z.blend) &&
                   a.texKey == z.texKey && a.srcFbp == z.srcFbp &&
                   a.tfx == z.tfx && a.tcc == z.tcc && a.fge == z.fge &&
                   a.fogCol == z.fogCol && a.depthFunc == z.depthFunc &&
                   a.depthWrite == z.depthWrite && a.bilinear == z.bilinear &&
                   a.wrapU == z.wrapU && a.wrapV == z.wrapV &&
                   a.uvOriginX == z.uvOriginX && a.uvOriginY == z.uvOriginY &&
                   a.uvRoundU == z.uvRoundU && a.uvRoundV == z.uvRoundV &&
                   a.scX0 == z.scX0 && a.scY0 == z.scY0 &&
                   a.scX1 == z.scX1 && a.scY1 == z.scY1;
        };
        const auto g561StateEqual = [](const G178Draw &a, const G178Draw &z) {
            return a.firstVtx + a.vtxCount == z.firstVtx &&
                   (a.depthFunc & 0x0fu) == (z.depthFunc & 0x0fu) &&
                   a.depthWrite == z.depthWrite &&
                   g603RevSubBlend(a.blend) == g603RevSubBlend(z.blend) &&
                   a.scX0 == z.scX0 && a.scY0 == z.scY0 &&
                   a.scX1 == z.scX1 && a.scY1 == z.scY1 &&
                   a.texKey == z.texKey && a.srcFbp == z.srcFbp &&
                   a.bilinear == z.bilinear && a.wrapU == z.wrapU && a.wrapV == z.wrapV;
        };
        const auto g570StateEqual = [&](const G178Draw &a, const G178Draw &z) {
            if (g560StateEqual(a, z))
                return true;
            const bool bothUntextured =
                a.texKey == 0u && a.srcFbp == 0u && z.texKey == 0u && z.srcFbp == 0u;
            return bothUntextured && a.firstVtx + a.vtxCount == z.firstVtx &&
                   (a.depthFunc & 0x0fu) == (z.depthFunc & 0x0fu) &&
                   a.depthWrite == z.depthWrite &&
                   g603RevSubBlend(a.blend) == g603RevSubBlend(z.blend) &&
                   a.scX0 == z.scX0 && a.scY0 == z.scY0 &&
                   a.scX1 == z.scX1 && a.scY1 == z.scY1;
        };
        const auto g570RunEqual = [&](const G178Draw &a, const G178Draw &z) {
            if (g561TextureRuns && g561StateEqual(a, z))
                return true;
            if (g570UntexturedRuns && g570StateEqual(a, z))
                return true;
            return g560StateEqual(a, z);
        };

        // G559 premise gate (DC2_G559_DRAW_RUN_CENSUS=1, diagnostic only): price the draw-call
        // reduction available from a future multi-draw shader before building it.  The existing
        // front end already merges fully-identical adjacent draws; these progressively ignore
        // state that can travel through gl_DrawID while retaining state that must remain fixed for
        // one ordered multi-draw command:
        //   noblend  - exact current state except blend (dual-source blending can select 0/1/2/3/5)
        //   mdtex    - same bound texture/sampler + fixed scissor/depth/equation
        //   bindless - fixed scissor/depth/equation only (texture handle also travels per draw)
        // Blend mode 4 remains its own reverse-subtract equation class.  `physical` includes the
        // second alpha-only draw currently issued by raw-alpha RTT waves.  No GL state or output is
        // changed by this census.
        static const bool s_g559DrawRuns = g158EnvFlag("DC2_G559_DRAW_RUN_CENSUS");
        if (s_g559DrawRuns && !g256Exact && !b.draws.empty())
        {
            struct G559RunAcc
            {
                uint64_t batches = 0u, logical = 0u, physical = 0u;
                uint64_t noblend = 0u, mdtex = 0u, bindless = 0u;
            };
            static G559RunAcc s_acc{}, s_last{};
            auto eqFixed = [](const G178Draw &a, const G178Draw &z) {
                return (a.depthFunc & 0x0fu) == (z.depthFunc & 0x0fu) &&
                       a.depthWrite == z.depthWrite &&
                       g603RevSubBlend(a.blend) == g603RevSubBlend(z.blend) &&
                       a.scX0 == z.scX0 && a.scY0 == z.scY0 &&
                       a.scX1 == z.scX1 && a.scY1 == z.scY1;
            };
            auto eqSampler = [](const G178Draw &a, const G178Draw &z) {
                return a.texKey == z.texKey && a.srcFbp == z.srcFbp &&
                       a.bilinear == z.bilinear && a.wrapU == z.wrapU && a.wrapV == z.wrapV;
            };
            uint64_t noBlendRuns = 1u, mdTexRuns = 1u, bindlessRuns = 1u;
            uint64_t physicalDraws = b.draws.size();
            for (size_t i = 0; i < b.draws.size(); ++i)
            {
                const G178Draw &d = b.draws[i];
                if (g255RttAlpha && d.blend != 0u)
                    ++physicalDraws;
                if (i == 0u)
                    continue;
                const G178Draw &p = b.draws[i - 1u];
                if (!g560StateEqual(p, d))
                    ++noBlendRuns;
                if (!eqFixed(p, d) || !eqSampler(p, d))
                    ++mdTexRuns;
                if (!eqFixed(p, d))
                    ++bindlessRuns;
            }
            ++s_acc.batches;
            s_acc.logical += b.draws.size();
            s_acc.physical += physicalDraws;
            s_acc.noblend += noBlendRuns;
            s_acc.mdtex += mdTexRuns;
            s_acc.bindless += bindlessRuns;
            if ((s_acc.batches % 512u) == 0u)
            {
                const G559RunAcc d{
                    s_acc.batches - s_last.batches, s_acc.logical - s_last.logical,
                    s_acc.physical - s_last.physical, s_acc.noblend - s_last.noblend,
                    s_acc.mdtex - s_last.mdtex, s_acc.bindless - s_last.bindless};
                std::fprintf(stderr,
                             "[G559:runs] batches=%llu logical=%llu physical=%llu "
                             "noblend=%llu(%.1f%%) mdtex=%llu(%.1f%%) bindless=%llu(%.1f%%)\n",
                             (unsigned long long)d.batches, (unsigned long long)d.logical,
                             (unsigned long long)d.physical, (unsigned long long)d.noblend,
                             d.physical ? 100.0 * (double)d.noblend / (double)d.physical : 0.0,
                             (unsigned long long)d.mdtex,
                             d.physical ? 100.0 * (double)d.mdtex / (double)d.physical : 0.0,
                             (unsigned long long)d.bindless,
                             d.physical ? 100.0 * (double)d.bindless / (double)d.physical : 0.0);
                std::fflush(stderr);
                s_last = s_acc;
            }
        }
        static uint64_t s_globalDrawCount = 0;
        const int drawLimit = dc2DrawLimit();
        for (size_t drawIndex = 0; drawIndex < b.draws.size(); ++drawIndex)
        {
            if (drawLimit >= 0 && s_globalDrawCount >= static_cast<uint64_t>(drawLimit))
                break;
            ++s_globalDrawCount;
            const G178Draw &d = b.draws[drawIndex];
            size_t g560RunEnd = drawIndex + 1u;
            if (g560MultiDraw)
                while (g560RunEnd < b.draws.size() &&
                       g570RunEqual(b.draws[g560RunEnd - 1u], b.draws[g560RunEnd]))
                    ++g560RunEnd;
            const int g560VtxCount = g560MultiDraw
                                         ? (b.draws[g560RunEnd - 1u].firstVtx +
                                            b.draws[g560RunEnd - 1u].vtxCount - d.firstVtx)
                                         : d.vtxCount;
            // GS scissor is inclusive, top-origin; GL is bottom-origin.
            const int scH = d.scY1 - d.scY0 + 1;
            if (fbo.physW > 0 && (fbo.physW != b.fbW || fbo.physH != b.fbH))
            {
                const float sx = static_cast<float>(fbo.physW) / static_cast<float>(b.fbW);
                const float sy = static_cast<float>(fbo.physH) / static_cast<float>(b.fbH);
                const int physScX0 = static_cast<int>(std::round(d.scX0 * sx));
                const int physScY0 = static_cast<int>(std::round((b.fbH - 1 - d.scY1) * sy));
                const int physScW  = static_cast<int>(std::round((d.scX1 - d.scX0 + 1) * sx));
                const int physScH  = static_cast<int>(std::round(scH * sy));
                g496Scissor(physScX0, physScY0, physScW, physScH, g496GlCache);
            }
            else
            {
                g496Scissor(d.scX0, b.fbH - 1 - d.scY1, d.scX1 - d.scX0 + 1, scH, g496GlCache);
            }
            const uint8_t depthFunc = d.depthFunc & 0x0Fu;
            if (depthFunc == 0)
            {
                g496DepthOff(g496GlCache);
            }
            else
            {
                g496DepthOn(depthFunc == 1 ? GL_ALWAYS :
                                (depthFunc == 2 ? GL_GEQUAL : GL_GREATER),
                            d.depthWrite != 0u, g496GlCache);
            }
            if (g256Exact)
            {
                int mode = (static_cast<int>(d.blend & 3u) << 8);
                if (b.fbp == 0x139u)
                    mode |= 1024; // CPU writePixel's established opaque-over-black RTT rule.
                if (d.texKey != 0u)
                {
                    TexEntry *const texture = drawTextures[drawIndex];
                    glBindTexture(GL_TEXTURE_2D, texture->id);
                    // A newly uploaded GL texture defaults to a mipmapped MIN filter, but the
                    // cache intentionally provides level 0 only.  Even texelFetch returns the
                    // incomplete-texture value (0,0,0,1) in that state.  G256 performs its own
                    // point/bilinear taps, so NEAREST here only completes the sampler object.
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                }
                m_f.glUniform1i_(m_locExactMode, mode);
                m_f.glUniform1i_(m_locExactLinear, d.bilinear ? 1 : 0);
                m_f.glUniform1i_(m_locExactWrapU, static_cast<GLint>(d.wrapU));
                m_f.glUniform1i_(m_locExactWrapV, static_cast<GLint>(d.wrapV));
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                // G256 keeps one captured sprite per draw.  Publish its image stores before the
                // next sprite reads the same destination, preserving CPU submission/blend order.
                m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                     GL178_TEXTURE_FETCH_BARRIER_BIT |
                                     GL178_FRAMEBUFFER_BARRIER_BIT);
                if (g258Trace && (drawIndex == 0u || drawIndex == 6u))
                {
                    std::vector<uint32_t> afterDraw(static_cast<size_t>(b.fbW) * b.fbH);
                    glFinish();
                    glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, afterDraw.data());
                    const size_t watch = static_cast<size_t>(b.fbH - 2) * b.fbW + 52u;
                    std::fprintf(stderr,
                                 "[G258:gpu.draw] batch=%llu draw=%zu rgba=%08x\n",
                                 static_cast<unsigned long long>(g258TraceBatch),
                                 drawIndex, watch < afterDraw.size() ? afterDraw[watch] : 0u);
                }
                continue;
            }
            if (g560MultiDraw)
            {
                // Every primitive supplies its own factors through the secondary fragment output.
                // Only reverse-subtract remains fixed GL state, so G560's run splitter keeps mode 4
                // separate while modes 0/1/2/3/5 can coexist in submission order.
                g496BlendOn([&](GLenum eqRgb, GLenum eqA)
                            { m_e.glBlendEquationSeparate_(eqRgb, eqA); },
                            [&](GLenum sRgb, GLenum dRgb, GLenum sA, GLenum dA)
                            { m_e.glBlendFuncSeparate_(sRgb, dRgb, sA, dA); },
                            g603RevSubBlend(d.blend) ? GL178_FUNC_REVERSE_SUBTRACT : GL178_FUNC_ADD,
                            GL178_FUNC_ADD, GL178_SRC1_COLOR, GL178_SRC1_ALPHA,
                            GL_ONE, GL_ZERO, g496GlCache);
            }
            else if (d.blend == 0)
            {
                g496BlendOff(g496GlCache);
            }
            else
            {
                // Alpha channel: keep dest alpha = written src value (CPU writePixel stores the
                // source alpha unblended). blend==3 (G262): (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd
                // additive; blend==4 (G262): (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs subtractive
                // (GL_FUNC_REVERSE_SUBTRACT = dst - src, both clamp at the [0,1] rail like GS
                // COLCLAMP). The RGB equation is persistent GL state, so it must be re-set whenever
                // it is not already the one this draw wants — which is exactly what the cache
                // tracks; a run of same-mode blended draws now sets it once.
                // G603 blend==6: (0-Cs)*As>>7 + Cd == Cd - Cs*As/128.
                // ⛔ G604 CORRECTION: it does NOT want GL_SRC_ALPHA. GS floors the negative
                // product; GL rounds to nearest, and the resulting sub-LSB shortfall accumulates
                // over an overdrawn effect (measured: 22.5% of pixels off by >1). The shader now
                // emits ceil(Cs*As/128) itself (g604SubBlendRgb), so mode 6 takes mode 4's plain
                // GL_ONE source factor and the fixed-function unit only has to subtract.
                g496BlendOn([&](GLenum eqRgb, GLenum eqA)
                            { m_e.glBlendEquationSeparate_(eqRgb, eqA); },
                            [&](GLenum sRgb, GLenum dRgb, GLenum sA, GLenum dA)
                            { m_e.glBlendFuncSeparate_(sRgb, dRgb, sA, dA); },
                            g603RevSubBlend(d.blend) ? GL178_FUNC_REVERSE_SUBTRACT : GL178_FUNC_ADD,
                            GL178_FUNC_ADD,
                            d.blend == 5 ? GL_ZERO
                                         : ((d.blend == 3 || g603RevSubBlend(d.blend))
                                                ? GL_ONE
                                                : GL_SRC_ALPHA),
                            (d.blend == 1 || d.blend == 5) ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE,
                            GL_ONE, GL_ZERO, g496GlCache);
            }
            int mode = 0;
            // G604: carry the blend combo into the shader (bits 17..19), so mode 6 can apply the
            // GS's integer-floor weighting itself. The field sits above bit 16 so the G560 build's
            // `uMode & 65535` mask cannot see it — that build overrides g604BlendMode() with its
            // own per-primitive field instead.
            //
            // ⚠️ Set ONLY for mode 6, deliberately. `uMode` is a cached uniform (g496Uniform1i), so
            // encoding the field unconditionally would make two otherwise-identical draws that
            // differ only in blend mode a cache MISS and add a glUniform1i to the shipped path for
            // a value nothing reads. Mode 6 is unreachable unless DC2_G603_SUBBLEND=1, so the
            // default binary's uMode stream is bit-identical to the pre-G604 one.
            if (d.blend == 6u)
                mode |= (6 << 17);
            // G558: 0x13b also carries an opaque SOLID terminal sprite.  G286's integer sampler
            // is a property of its textured batches, not of the framebuffer address itself;
            // selecting it for TME=0 made the backend demand a texture and reject the batch.
            const bool g286Exact = b.fbp == 0x13bu && d.texKey != 0u;
            // G362: point-sampled draws on the legacy shader path take the guarded fixed-point
            // fetch (uMode bit 12) instead of GL_NEAREST's float floor(). Default-ON; roll back
            // with DC2_G362_NO_UVGUARD=1. Bilinear draws keep texture()'s filtered path, and the
            // G256/G286 exact programs already reconstruct the coordinate themselves.
            static const bool s_g362NoGuard = g158EnvFlag("DC2_G362_NO_UVGUARD");
            const bool g362PointGuard = !s_g362NoGuard && !g256Exact && !g286Exact && !d.bilinear;
            // Bit 4 asks the fragment shader to clamp interpolated GS Z to PSMZ24 before
            // normalization, matching the CPU path's post-interpolation uint24 clamp.
            if ((d.depthFunc & 0x10u) != 0u)
                mode |= 16;
            // G265: the admitted nontrivial alpha-test shapes are GEQUAL with AFAIL=KEEP.
            // Fragment discard suppresses color and depth together, exactly matching KEEP.
            if ((d.depthFunc & 0x20u) != 0u)
                mode |= 64;
            mode |= (static_cast<int>((d.depthFunc >> 6u) & 3u) << 7u);
            if (d.srcFbp != 0u)
            {
                // G261: sample the producer target's persistent FBO color texture directly. The
                // front-end already remapped UVs into the FBO's full fbW x fbH bottom-first space,
                // and only admits in-range UVs — CLAMP here is inert insurance, never wrap.
                if (d.srcFbp == kG309CompositeToken)
                {
                    for (uint32_t t = 0u; t < kG309SourceCount; ++t)
                    {
                        m_f.glActiveTexture_(GL158_TEXTURE0 + t);
                        const auto it = m_fbos.find(kG309Sources[t]);
                        const GLuint g496Src =
                            it != m_fbos.end() && it->second.colorTex != 0u
                                ? it->second.colorTex : m_g309BaseTex;
                        // G496b: only unit 0 is cached - it is the unit every other draw in this
                        // loop binds through, and this branch leaves kSources[0] resident on it.
                        if (t == 0u)
                            g496BindTex(g496Src, g496GlCache);
                        else
                            glBindTexture(GL_TEXTURE_2D, g496Src);
                    }
                    m_f.glActiveTexture_(GL158_TEXTURE0 + kG309PageMapUnit);
                    glBindTexture(GL_TEXTURE_2D, m_g309MapTex);
                    m_f.glActiveTexture_(GL158_TEXTURE0 + kG309BaseUnit);
                    glBindTexture(GL_TEXTURE_2D, m_g309BaseTex);
                    m_f.glActiveTexture_(GL158_TEXTURE0);
                    mode |= 2048;
                }
                else
                {
                    const uint32_t realSrcFbp = g289RealSourceFbp(d.srcFbp);
                    const FboEntry &src =
                        m_fbos.find(realSrcFbp)->second;
                    // G496b: the BIND is cached; the four sampler params are NOT. An FboEntry
                    // colour texture is re-parameterised by the T8, CT24, composite-source and
                    // spare-swap paths, so a per-object record here could go stale.
                    if (realSrcFbp == 0x139u)
                    {
                        static uint32_t s_dc2_139_sample_count = 0u;
                        if (s_dc2_139_sample_count < 16u)
                            std::fprintf(stderr, "[DC2_139_SAMPLE] n=%u batchFbp=0x%03x draw=%zu srcFbo=%u srcTex=%u blend=%u verts=%d\n",
                                         s_dc2_139_sample_count, b.fbp, drawIndex, src.fbo, src.colorTex, d.blend, d.vtxCount);
                        ++s_dc2_139_sample_count;
                    }
                    g496BindTex(src.colorTex, g496GlCache);
                    const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    const bool ownerSource = g289SourceIsOwner(d.srcFbp);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                    ownerSource && d.wrapU == 0u
                                        ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                    ownerSource && d.wrapV == 0u
                                        ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                }
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                if (g289SourceIsCt24(d.srcFbp))
                    mode |= 1024;
                // G362: the G309 composite branch (bit 11) does its own page-mapped fetch.
                if (g362PointGuard && (mode & 2048) == 0)
                    mode |= 4096;
            }
            if (g286Exact)
            {
                // G287: G286's exact branch selected integer sampling/combine but skipped the
                // ordinary texture-bind branch below. It therefore sampled whichever texture a
                // previous draw had left bound, and uMode silently described TFX=MODULATE/TCC=0.
                // Bind this draw's decoded PSMT8+CLUT texture explicitly and carry its real
                // combine state into the exact shader. G287 promotes the narrowly admitted
                // fbp=0x13b shape after exact-oracle and downstream-presentation gates.
                TexEntry *const texture = drawTextures[drawIndex];
                if (texture == nullptr)
                    return false;
                g496BindTex(texture->id, g496GlCache);
                // Newly created cache textures default to a mipmapped MIN filter while this
                // cache intentionally uploads level 0 only. The first G287 batch therefore
                // sampled the GL incomplete-texture value until another path happened to make
                // the object complete. Integer taps do not need filtering, but the sampler
                // object must still be complete from first use.
                g496TexFilter(texture->g496MinF, texture->g496MagF, GL_NEAREST, GL_NEAREST,
                              g496GlCache);
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                mode |= 512;
                if (g584ExactBlendAOn())
                    mode |= 32768; // G584: encode o.a as the As/128 blend factor.
                g496Uniform1i(g496Uni, kG496ULinear, locLinear, d.bilinear ? 1 : 0, g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapU, locWrapU, static_cast<int>(d.wrapU),
                              g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapV, locWrapV, static_cast<int>(d.wrapV),
                              g496GlCache);
            }
            else if (d.texKey != 0)
            {
                TexEntry *const texture = drawTextures[drawIndex];
                g496BindTex(texture->id, g496GlCache);
                const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                g496TexFilter(texture->g496MinF, texture->g496MagF, filter, filter, g496GlCache);
                g496TexWrap(texture->g496WrapS, texture->g496WrapT,
                            d.wrapU == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE),
                            d.wrapV == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE),
                            g496GlCache);
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                if (g362PointGuard)
                    mode |= 4096;
            }
            const bool g407AccurateUv =
                ((d.uvRoundU | d.uvRoundV) & 8u) != 0u;
            if (g407AccurateUv && (mode & 1) != 0 && (mode & 512) == 0)
                mode |= 4096;
            // ⛔ G604 REFUTED HYPOTHESIS (do not re-try without new evidence): routing mode 6
            // through the exact 12.4 tap (`mode |= 4096`, which also implements GS's 4-bit
            // bilinear weights) to explain its divergence. The mode-6 draws are all bilinear
            // PSMT8/MODULATE, so the sampler looked like the obvious suspect. It measured NULL:
            // 27.72% -> 28.27% of in-footprint pixels off by >1. Caveat for whoever revisits —
            // that null was taken while mode 6 was still diverging for the unfound reason, so it
            // bounds the sampler's contribution as small, not as zero.
            if ((mode & 4096) != 0)
            {
                // Accurate UV uses exact 12.4 point/bilinear taps. The wrap mode travels as a
                // uniform because integer taps do not use the GL sampler's addressing state.
                g496Uniform1i(g496Uni, kG496ULinear, locLinear, d.bilinear ? 1 : 0, g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapU, locWrapU, static_cast<int>(d.wrapU),
                              g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapV, locWrapV, static_cast<int>(d.wrapV),
                              g496GlCache);
                m_g362GuardDraws += 1u;
            }
            else if ((mode & 1) != 0)
            {
                m_g362PlainDraws += 1u;
            }
            // G364: GS per-pixel fog. Bit 13 + the packed FOGCOL travel per draw (FOGCOL is global
            // GS state; the front-end snapshots it into each G178Draw so a mid-frame change splits
            // the batch through g178StateEqual).
            if (d.fge != 0u)
            {
                mode |= 8192;
                g496Uniform1i(g496Uni, kG496UFogCol, locFogCol,
                              static_cast<int>(d.fogCol & 0x00FFFFFFu), g496GlCache);
            }
            if (d.uvRoundU != 0u || d.uvRoundV != 0u)
            {
                mode |= 16384;
                // G624: a `srcFbp` bind samples the producer FBO bottom-first, so the front end
                // flipped this draw's T. Tell the rounding rule, which is expressed in GS V.
                // The G309 composite branch (bit 11) never reaches g406RoundUv; it is excluded
                // for clarity, not for correctness.
                g496Uniform1i(g496Uni, kG496UFlipV, m_locUvFlipV,
                              (!s_g624NoUvFlip && d.srcFbp != 0u && (mode & 2048) == 0) ? 1 : 0,
                              g496GlCache);
                g496Uniform1i(g496Uni, kG496UOriginX, locUvOriginX,
                              static_cast<int>(d.uvOriginX), g496GlCache);
                g496Uniform1i(g496Uni, kG496UOriginY, locUvOriginY,
                              static_cast<int>(d.uvOriginY), g496GlCache);
                g496Uniform1i(g496Uni, kG496URoundU, locUvRoundU,
                              static_cast<int>(d.uvRoundU), g496GlCache);
                g496Uniform1i(g496Uni, kG496URoundV, locUvRoundV,
                              static_cast<int>(d.uvRoundV), g496GlCache);
            }
            float minX = 1e6f, minY = 1e6f, maxX = -1e6f, maxY = -1e6f;
            for (int vi = 0; vi < d.vtxCount; ++vi)
            {
                const G178Vtx &vx = b.verts[static_cast<size_t>(d.firstVtx + vi)];
                minX = std::min(minX, vx.x);
                minY = std::min(minY, vx.y);
                maxX = std::max(maxX, vx.x);
                maxY = std::max(maxY, vx.y);
            }
            if (dc2IsNativeHighResActive() && b.fbp == 0x139u && d.blend == 0u && (d.depthFunc & 0x0Fu) == 0u && d.texKey == 0u && d.srcFbp == 0u)
            {
                static uint32_t s_dc2_139_clear_count = 0u;
                if (s_dc2_139_clear_count < 16u)
                    std::fprintf(stderr, "[DC2_139_CLEAR] n=%u draw=%d verts=%d bounds=(%.0f,%.0f..%.0f,%.0f) physW=%d physH=%d\n",
                                 s_dc2_139_clear_count, drawIndex, d.vtxCount, minX, minY, maxX, maxY, fbo.physW, fbo.physH);
                ++s_dc2_139_clear_count;
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
            }
            const bool execDraw = dc2ShouldExecuteRenderEvent("draw", b.fbp, d.texKey, d.srcFbp, minX, minY, maxX, maxY, d.vtxCount, d.blend, d.depthFunc & 0x0Fu, d.depthWrite, mode);
            if (execDraw)
            {
                if (!g560MultiDraw && g255RttAlpha && d.blend != 0u)
                {
                    // Fixed-function blending needs As/128 in the shader alpha channel, while PS2
                    // framebuffer alpha stores the unblended source A byte. Preserve RGB blending in
                    // pass one with alpha masked, then overwrite alpha only with the raw source value.
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                    g496Uniform1i(g496Uni, kG496UMode, locMode, mode, g496GlCache);
                    glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                    // Routed through the cache so the record tracks the real GL state: this branch
                    // disables blending after g496BlendOn enabled it, and the next draw must not
                    // believe blending is still on.
                    g496BlendOff(g496GlCache);
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                    g496Uniform1i(g496Uni, kG496UMode, locMode, mode | 32, g496GlCache);
                    glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                }
                else
                {
                    if (g255RttAlpha)
                        mode |= 32;
                    g496Uniform1i(g496Uni, kG496UMode, locMode, mode, g496GlCache);
                    glDrawArrays(GL_TRIANGLES, d.firstVtx, g560VtxCount);
                }
            }
            static uint32_t s_auditBatchCounter = 0;
            if (m_batchCounter <= 300 && (m_batchCounter % 60 == 0))
            {
                float minX = 1e6f, minY = 1e6f, maxX = -1e6f, maxY = -1e6f;
                for (int vi = 0; vi < d.vtxCount; ++vi)
                {
                    const G178Vtx &vx = b.verts[static_cast<size_t>(d.firstVtx + vi)];
                    minX = std::min(minX, vx.x);
                    minY = std::min(minY, vx.y);
                    maxX = std::max(maxX, vx.x);
                    maxY = std::max(maxY, vx.y);
                }
                std::fprintf(stderr, "[DRAW_AUDIT] batch=%llu FBP=0x%03x draw=%zu verts=%d mode=%d blend=%u texKey=0x%llx srcFbp=0x%x ztst=%u zw=%u bounds=[(%.1f,%.1f)..(%.1f,%.1f)] col0=(%u,%u,%u,%u)\n",
                             static_cast<unsigned long long>(m_batchCounter), b.fbp, drawIndex, d.vtxCount, mode, d.blend,
                             static_cast<unsigned long long>(d.texKey), d.srcFbp, d.depthFunc & 0x0Fu, d.depthWrite,
                             minX, minY, maxX, maxY, b.verts[d.firstVtx].r, b.verts[d.firstVtx].g, b.verts[d.firstVtx].b, b.verts[d.firstVtx].a);
            }
            if (g560MultiDraw)
                drawIndex = g560RunEnd - 1u;
        }

        // G496 GPU DERIVATIVE PROBE (diagnostic only — DC2_G419_AB=gpuslow / DC2_G496_FORCE=1).
        // Re-issue this batch's whole vertex range with an IDENTITY BLEND: dst = 0*src + 1*dst, with
        // the depth test disabled and depth writes masked. Every fragment is rasterized, shaded,
        // blended and written, so no driver can elide it (the colour-mask form the G495 roadmap
        // proposed is elidable on this platform — see the note above the exact program's scratch
        // attachment), and the byte written is the byte that was already there. One draw call covers
        // the whole batch, so the CPU cost added to the GL worker is one call per batch.
        // Excluded on the G256 exact path: that shader's imageStore is a side effect no blend can
        // undo. g256ExactRttOn() needs two env flags and is off on every default/perf run.
        const int g496Reps = g256Exact ? 0 : g496BallastForBatch(m_batchCounter);
        if (g496Reps > 0 && b.verts.size() >= 3u)
        {
            g496CalBegin();
            glScissor(0, 0, b.fbW, b.fbH);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_BLEND);
            m_e.glBlendEquationSeparate_(GL178_FUNC_ADD, GL178_FUNC_ADD);
            m_e.glBlendFuncSeparate_(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
            const GLsizei g496Vtx = static_cast<GLsizei>((b.verts.size() / 3u) * 3u);
            for (int rep = 0; rep < g496Reps; ++rep)
                glDrawArrays(GL_TRIANGLES, 0, g496Vtx);
            glDisable(GL_BLEND);
            g496CalEnd(static_cast<uint64_t>(g496Reps),
                       static_cast<uint64_t>(g496Vtx) * static_cast<uint64_t>(g496Reps));
        }

        if (g256Exact)
        {
            m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                 GL178_TEXTURE_FETCH_BARRIER_BIT |
                                 GL178_FRAMEBUFFER_BARRIER_BIT);
            std::vector<uint32_t> g257ImageAfter;
            std::vector<uint32_t> g257Coverage;
            GLenum g257PostError = GL_NO_ERROR;
            unsigned g257PostErrorCount = 0;
            if (g257RealDiagnostic)
            {
                g257ImageAfter.resize(static_cast<size_t>(b.fbW) * b.fbH);
                glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              g257ImageAfter.data());
                g257Coverage.resize(static_cast<size_t>(b.fbW) * b.fbH);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, g257CoverageFbo);
                glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE,
                             g257Coverage.data());
                g257DrainErrors(g257PostError, g257PostErrorCount);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_e.glBindImageTexture_(1u, 0u, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
            m_f.glUseProgram_(m_prog);

            if (g257RealDiagnostic)
            {
                size_t changed = 0;
                size_t covered = 0;
                for (size_t i = 0; i < g257ImageAfter.size(); ++i)
                {
                    if (g257ImageAfter[i] != g257ImageBefore[i])
                        ++changed;
                    if (g257Coverage[i] != 0u)
                        ++covered;
                }
                // Preserve the direct texture snapshot until after the normal FBO readback below.
                m_g257ImageAfter.swap(g257ImageAfter);
                m_g257Changed = changed;
                m_g257Covered = covered;
                m_g257PreError = g257PreError;
                m_g257PreErrorCount = g257PreErrorCount;
                m_g257PostError = g257PostError;
                m_g257PostErrorCount = g257PostErrorCount;
            }
        }

        glDisable(GL_SCISSOR_TEST);
        if (dc2NativeResDiagOn())
        {
            static uint32_t s_diagDumpCount = 0;
            if (m_batchCounter >= 60 && m_batchCounter <= 300 && s_diagDumpCount < 10)
            {
                if (b.fbp == 0x000u || b.fbp == 0x068u || b.fbp == 0x139u)
                {
                    ++s_diagDumpCount;
                    std::vector<uint32_t> px(static_cast<size_t>(fbo.physW) * fbo.physH);
                    glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    char fname[256];
                    std::snprintf(fname, sizeof(fname), "d:/ps2r/dc2/captures/surface_batch_%llu_fbp_0x%03x.ppm",
                                  static_cast<unsigned long long>(m_batchCounter), b.fbp);
                    dc2DumpPpm(fname, fbo.physW, fbo.physH, px.data(), true);
                }
            }
        }
        g415Note(); // 6: draw loop (+ exact-path epilogue)
        // G418 early kick: this batch leaves its GL work queued and never fences, so without a kick
        // the frame's one readback batch pays the whole backlog as drain. glFlush only guarantees
        // that already-issued commands finish in finite time - it changes no result, no GL state,
        // no command order, and no ownership - so the arm is exact by construction.
        const bool g418Profile = g418ProfileOn();
        if (b.skipReadback && g445EarlyKickActive())
        {
            const auto kickT0 = g418Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            glFlush();
            if (g418Profile)
                g418NoteKick(b.fbp,
                             static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - kickT0).count()));
        }
        // G415 fence attribution: drain everything already queued on this context BEFORE the
        // readback, so the census can tell "waiting for prior GL work" apart from "this transfer".
        // Diagnostic only (DC2_G415_CENSUS=1 + DC2_G415_FENCE_PROBE=1).
        // G418 takes the same split without the nine-mark stage census riding along; only one of the
        // two probes ever issues the finish.
        uint64_t g418DrainNs = 0u;
        std::chrono::steady_clock::time_point g418RbT0;
        if (!b.skipReadback && (g415FenceProbeOn() || g418Profile))
        {
            const auto drainT0 = std::chrono::steady_clock::now();
            glFinish();
            g418RbT0 = std::chrono::steady_clock::now();
            g418DrainNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g418RbT0 - drainT0).count());
        }
        g415Note(); // 7: pre-readback drain probe
        int g415ReadRows = 0;
        if (!b.skipReadback)
        {
            // G496 calibration frame clock on the GL worker: exactly one render batch per frame
            // reads back (G495 §2 measured rb/frames = 0.99), so this is the divisor that turns the
            // ballast's accumulated GPU nanoseconds into a real ms/FRAME. No-op unless DC2_G496_CAL.
            g496NoteFrame();
            g496GlNoteFrame();
            // G415: read only the GS row window this batch's caller actually consumes, into that
            // window's existing offsets in the full-height buffer. Buffer row index == GL row, so
            // the destination pointer offset is glY*fbW and every consumer index is unchanged.
            // Any missing/odd window falls back to the full-height read (fail-closed).
            // G419: evaluate the arm FIRST and unconditionally. Inside a short-circuiting && chain
            // the instrument's consumer record would be skipped whenever an earlier term failed,
            // and the tick would drop that frame period instead of charging it.
            const bool narrowArm = g415NarrowReadbackOn();
            // G420 mechanism probe: evaluated here, unconditionally, for the same short-circuit
            // reason as narrowArm above.
            const bool g420Touch = g420NarrowTouchArm();
            const bool narrow =
                narrowArm && g415Window.valid && g415Window.rows > 0 &&
                g415Window.glY >= 0 && g415Window.rows <= b.fbH - g415Window.glY &&
                b.readback.size() == static_cast<size_t>(b.fbW) * b.fbH;
            // G420 shape probe: widen the read to a forced row count. Exact for any count >= the
            // published window — the read still covers every consumed row at its existing offset,
            // and rows outside the consumed window are discarded either way (see
            // g420NarrowRowsOverride).
            int narrowY = g415Window.glY;
            int narrowRows = g415Window.rows;
            if (narrow)
            {
                const int forced = std::min(g420NarrowRowsOverride(), b.fbH);
                if (forced > narrowRows)
                {
                    narrowRows = forced;
                    narrowY = std::max(0, std::min(narrowY, b.fbH - forced));
                }
            }
            if (fbo.physW > 0 && (fbo.physW != b.fbW || fbo.physH != b.fbH))
            {
                const float sy = static_cast<float>(fbo.physH) / static_cast<float>(b.fbH);
                if (narrow)
                {
                    const int physY = static_cast<int>(std::round(narrowY * sy));
                    const int physRows = std::max(1, static_cast<int>(std::round(narrowRows * sy)));
                    std::vector<uint32_t> physPx(static_cast<size_t>(fbo.physW) * physRows);
                    glReadPixels(0, physY, fbo.physW, physRows, GL_RGBA, GL_UNSIGNED_BYTE, physPx.data());
                    for (int r = 0; r < narrowRows; ++r)
                    {
                        int pr = (r * physRows) / narrowRows;
                        if (pr >= physRows) pr = physRows - 1;
                        const uint32_t *srcRow = physPx.data() + static_cast<size_t>(pr) * fbo.physW;
                        uint32_t *dstRow = b.readback.data() + static_cast<size_t>(narrowY + r) * b.fbW;
                        for (int x = 0; x < b.fbW; ++x)
                        {
                            int px = (x * fbo.physW) / b.fbW;
                            if (px >= fbo.physW) px = fbo.physW - 1;
                            dstRow[x] = srcRow[px];
                        }
                    }
                    g415ReadRows = narrowRows;
                }
                else
                {
                    std::vector<uint32_t> physPx(static_cast<size_t>(fbo.physW) * fbo.physH);
                    glReadPixels(0, 0, fbo.physW, fbo.physH, GL_RGBA, GL_UNSIGNED_BYTE, physPx.data());
                    for (int r = 0; r < b.fbH; ++r)
                    {
                        int pr = (r * fbo.physH) / b.fbH;
                        if (pr >= fbo.physH) pr = fbo.physH - 1;
                        const uint32_t *srcRow = physPx.data() + static_cast<size_t>(pr) * fbo.physW;
                        uint32_t *dstRow = b.readback.data() + static_cast<size_t>(r) * b.fbW;
                        for (int x = 0; x < b.fbW; ++x)
                        {
                            int px = (x * fbo.physW) / b.fbW;
                            if (px >= fbo.physW) px = fbo.physW - 1;
                            dstRow[x] = srcRow[px];
                        }
                    }
                    g415ReadRows = b.fbH;
                }
            }
            else if (narrow)
            {
                glReadPixels(0, narrowY, b.fbW, narrowRows,
                             GL_RGBA, GL_UNSIGNED_BYTE,
                             b.readback.data() +
                                 static_cast<size_t>(narrowY) * b.fbW);
                g415ReadRows = narrowRows;
                if (g415StatOn())
                {
                    g_g415NarrowBatches.fetch_add(1u, std::memory_order_relaxed);
                    g_g415NarrowRows.fetch_add(static_cast<uint64_t>(narrowRows),
                                               std::memory_order_relaxed);
                }
            }
            else
            {
                glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE, b.readback.data());
                g415ReadRows = b.fbH;
                if (g415StatOn())
                {
                    g_g415FullBatches.fetch_add(1u, std::memory_order_relaxed);
                    g_g415FullRows.fetch_add(static_cast<uint64_t>(b.fbH),
                                             std::memory_order_relaxed);
                }
            }
            // G419 ballast (diagnostic only, DC2_G419_BALLAST with DC2_G419_AB=narrow): re-read the
            // SAME rect into the SAME destination N extra times. Byte-idempotent, so the measured
            // frame-time delta divided by the known added transfer cost is this site's critical-path
            // sensitivity - the multiplier a removed-row saving has to be scaled by.
            for (int rep = g419NarrowBallastReps(); rep > 0; --rep)
            {
                if (narrow)
                    glReadPixels(0, narrowY, b.fbW, narrowRows,
                                 GL_RGBA, GL_UNSIGNED_BYTE,
                                 b.readback.data() +
                                     static_cast<size_t>(narrowY) * b.fbW);
                else
                    glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE,
                                 b.readback.data());
            }
            // G420 mechanism probe (see g420NarrowTouchArm): add the DESTINATION-side write volume
            // of the rows the full read would have produced and the consumer discards, into an
            // inert backend-private scratch buffer. No GL call, no observable byte.
            if (g420Touch && g415ReadRows > 0 && g415ReadRows < b.fbH)
            {
                const size_t words = static_cast<size_t>(b.fbW) *
                                     static_cast<size_t>(b.fbH - g415ReadRows);
                if (m_g420TouchScratch.size() < words)
                    m_g420TouchScratch.resize(words);
                std::memset(m_g420TouchScratch.data(), 0, words * sizeof(uint32_t));
            }
            // G621 (default-off, DC2_G621_SPAN=1): of the rows this read transfers, how many could
            // this batch's draws have WRITTEN? GS row y is GL row fbH-1-y, so the GL window
            // [rdGlY, rdGlY+g415ReadRows) is the GS span below.
            if (g621SpanCensusOn() && g415ReadRows > 0)
            {
                const int rdGlY = narrow ? narrowY : 0;
                const int rdGsHi = b.fbH - 1 - rdGlY;
                const int rdGsLo = rdGsHi - g415ReadRows + 1;
                g621NoteReadback(b, rdGsLo < 0 ? 0 : rdGsLo, rdGsHi, depthKey != 0u);
            }
            if (g415StatOn() && (++m_g415StatN % 300u) == 0u)
            {
                const uint64_t nn = g_g415NarrowBatches.load(std::memory_order_relaxed);
                const uint64_t nr = g_g415NarrowRows.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[G415:narrow] narrow=%llu rows=%llu avgRows=%.1f full=%llu "
                             "fullRows=%llu fboCreate=%llu fboPark=%llu fboRevive=%llu\n",
                             static_cast<unsigned long long>(nn),
                             static_cast<unsigned long long>(nr),
                             nn ? static_cast<double>(nr) / nn : 0.0,
                             static_cast<unsigned long long>(
                                 g_g415FullBatches.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 g_g415FullRows.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(m_g415FboCreates),
                             static_cast<unsigned long long>(m_g415FboParks),
                             static_cast<unsigned long long>(m_g415FboRevives));
            }
        }
        g415Note(); // 8: colour readback
        if (!b.skipReadback && g418Profile)
            g418NoteReadEdge(b.fbp, g415ReadRows, g418DrainNs,
                             static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - g418RbT0).count()));
        if (g415Census && g415Mark == kG415Marks)
            g415NoteStages(b, depthKey, g415Marks, g415ReadRows,
                           g415Window.valid ? g415Window.rows : 0,
                           g417UploadRowsThis);
        if (fusedDepthReadback != nullptr)
        {
            fusedDepthReadback->resize(static_cast<size_t>(b.fbW) * depthReadRows);
            if (fbo.physW > 0 && (fbo.physW != b.fbW || fbo.physH != b.fbH))
            {
                const float sy = static_cast<float>(fbo.physH) / static_cast<float>(b.fbH);
                const int physReadY = static_cast<int>(std::round(depthReadY * sy));
                const int physReadRows = std::max(1, static_cast<int>(std::round(depthReadRows * sy)));
                std::vector<float> physDepth(static_cast<size_t>(fbo.physW) * physReadRows);
                glReadPixels(0, physReadY, fbo.physW, physReadRows,
                             GL_DEPTH_COMPONENT, GL_FLOAT, physDepth.data());
                for (int r = 0; r < depthReadRows; ++r)
                {
                    int pr = (r * physReadRows) / depthReadRows;
                    if (pr >= physReadRows) pr = physReadRows - 1;
                    const float *srcRow = physDepth.data() + static_cast<size_t>(pr) * fbo.physW;
                    float *dstRow = fusedDepthReadback->data() + static_cast<size_t>(r) * b.fbW;
                    for (int x = 0; x < b.fbW; ++x)
                    {
                        int px = (x * fbo.physW) / b.fbW;
                        if (px >= fbo.physW) px = fbo.physW - 1;
                        dstRow[x] = srcRow[px];
                    }
                }
            }
            else
            {
                glReadPixels(0, depthReadY, b.fbW, depthReadRows,
                             GL_DEPTH_COMPONENT, GL_FLOAT, fusedDepthReadback->data());
            }
        }
        if (g257RealDiagnostic)
        {
            GLenum fboReadError = GL_NO_ERROR;
            unsigned fboReadErrorCount = 0;
            g257DrainErrors(fboReadError, fboReadErrorCount);
            size_t directFboBad = 0;
            for (size_t i = 0; i < b.readback.size(); ++i)
                if (b.readback[i] != m_g257ImageAfter[i])
                    ++directFboBad;
            ++m_g257RealBatches;
            std::fprintf(stderr,
                         "[G257:real] n=%llu fbp=0x%x draws=%zu changed=%zu/%zu coverage=%zu/%zu "
                         "directFboBad=%zu preErr=0x%x/%u postErr=0x%x/%u fboErr=0x%x/%u\n",
                         static_cast<unsigned long long>(m_g257RealBatches), b.fbp, b.draws.size(),
                         m_g257Changed, b.readback.size(), m_g257Covered, b.readback.size(),
                         directFboBad, m_g257PreError, m_g257PreErrorCount,
                         m_g257PostError, m_g257PostErrorCount,
                         fboReadError, fboReadErrorCount);
            m_g257ImageAfter.clear();
        }
        if (guestDepth)
        {
            // Keep the upload-safe post-state: a later glTexSubImage2D on a guest depth texture
            // must not target storage still attached to the bound FBO.
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
        }
        // G362 census (default OFF, DC2_G362_CENSUS=1): how much of the textured legacy path is
        // actually served by the guarded fetch. Periodic aggregate, not an early-capped print.
        {
            static const bool s_g362Census = g158EnvFlag("DC2_G362_CENSUS");
            if (s_g362Census)
            {
                static uint64_t s_next = 0;
                const uint64_t total = m_g362GuardDraws + m_g362PlainDraws;
                if (total >= s_next)
                {
                    s_next = total + 20000u;
                    std::fprintf(stderr, "[G362:pt] guarded=%llu plain=%llu\n",
                                 static_cast<unsigned long long>(m_g362GuardDraws),
                                 static_cast<unsigned long long>(m_g362PlainDraws));
                }
            }
        }
        return true;
    }

    GLFns m_f;
    G178GLExt m_e;
    GLuint m_prog = 0, m_progG560 = 0, m_progExact = 0, m_g281Prog = 0, m_g309Prog = 0;
    bool m_g560Available = false;
    GLuint m_g541NullProg = 0, m_g541StateTex = 0;
    GLint m_locG541KickCount = -1;
    bool m_g541Available = false;
    GLuint m_g629Prog = 0;
    GLuint m_g629ColorTex = 0, m_g629DepthTex = 0, m_g629RecTex = 0;
    GLuint m_g629TexDataTex = 0, m_g629TexDescTex = 0;
    GLuint m_g629TileOffTex = 0, m_g629TileRefTex = 0;
    uint32_t m_g629ColorRows = 0, m_g629DepthRows = 0, m_g629RecRows = 0;
    uint32_t m_g629TexDataRows = 0, m_g629TexDescRows = 0;
    uint32_t m_g629TileOffRows = 0, m_g629TileRefRows = 0;
    GLint m_locG629Width = -1, m_locG629Height = -1, m_locG629TilesX = -1;
    GLint m_locG629Block = -1, m_locG629Fbw = -1, m_locG629FirstPage = -1;
    GLint m_locG629WriteView = -1, m_locG629ViewHeight = -1;
    bool m_g629Available = false;
    std::deque<std::future<bool>> m_g630Futures; // front-thread completion order == GL FIFO order
    GLuint m_g630ViewProg = 0u, m_g630ViewMapTex = 0u;
    // G631: scratch FBO used only to read a ROW BAND of the persistent raw-VRAM image back.
    GLuint m_g631ReadFbo = 0u;
    uint32_t m_g630ViewMapRows = 0u;
    GLint m_locG630ViewBlock = -1, m_locG630ViewFbw = -1;
    GLint m_locG630ViewWidth = -1, m_locG630ViewHeight = -1;
    GLint m_locG630ViewOriginY = -1, m_locG630ViewRows = -1;
    bool m_g630ViewMapReady = false;
    std::vector<uint32_t> m_g630ViewMapPad;
    GLuint m_g630UploadProg = 0u, m_g630UploadTex = 0u;
    uint32_t m_g630UploadRows = 0u;
    GLint m_locG630UploadBlock = -1, m_locG630UploadFbw = -1;
    GLint m_locG630UploadX = -1, m_locG630UploadY = -1;
    GLint m_locG630UploadWidth = -1, m_locG630UploadHeight = -1;
    std::vector<uint32_t> m_g630UploadPad;
    GLuint m_g630T8Prog = 0u, m_g630T8MapTex = 0u, m_g630T8ClutTex = 0u;
    uint32_t m_g630T8MapRows = 0u, m_g630T8ClutRows = 0u;
    GLint m_locG630T8Width = -1, m_locG630T8Height = -1;
    GLint m_locG630T8Tbp = -1, m_locG630T8Tbw = -1;
    bool m_g630T8PermReady = false;
    std::vector<uint32_t> m_g630T8Pad;
    GLuint m_g570Prog = 0;
    GLuint m_g570ColorTex = 0, m_g570DepthTex = 0, m_g570TriTex = 0;
    GLuint m_g570SpanTex = 0, m_g570TileOffsetTex = 0, m_g570TileTriTex = 0;
    uint32_t m_g570ColorRows = 0, m_g570DepthRows = 0, m_g570TriRows = 0;
    uint32_t m_g570SpanRows = 0, m_g570TileOffsetRows = 0, m_g570TileTriRows = 0;
    bool m_g570Available = false;
    std::weak_ptr<G541NullBatch> m_g541QueuedBatch;
    std::atomic<uint64_t> m_g541Submitted{0ull};
    std::atomic<uint64_t> m_g541Executed{0ull};
    std::atomic<uint64_t> m_g541Batches{0ull};
    std::atomic<uint64_t> m_g541MaxBatch{0ull};
    std::atomic<uint64_t> m_g541Rejected{0ull};
    std::atomic<uint64_t> m_g541HighWater{0ull};
    uint64_t m_g541NextReport = 65536ull; // GL-worker only
    GLuint m_g545Prog = 0, m_g545CommandTex = 0, m_g545PayloadTex = 0;
    GLuint m_g545WorkspaceTex = 0, m_g566OutputTex = 0;
    GLint m_locG545CommandCount = -1;
    uint32_t m_g545CommandRows = 0u, m_g545PayloadRows = 0u;
    bool m_g545Available = false;
    std::atomic<bool> m_g545Disabled{false};
    std::weak_ptr<G545LiveBatch> m_g545QueuedBatch;
    std::atomic<uint64_t> m_g545Submitted{0ull};
    std::atomic<uint64_t> m_g545Executed{0ull};
    std::atomic<uint64_t> m_g545Rejected{0ull};
    std::atomic<uint64_t> m_g545HighWater{0ull};
    uint64_t m_g545Batches = 0ull, m_g545MaxBatch = 0ull;
    uint64_t m_g545CheckInterval = 16384ull, m_g545NextCheck = 16384ull;
    uint64_t m_g545Readbacks = 0ull, m_g545Verified = 0ull;
    std::vector<uint32_t> m_g566Readback;
    uint64_t m_g566Packets = 0ull, m_g566PacketBytes = 0ull;
    GLuint m_g338Prog = 0;
    GLint m_locG338SrcH = -1, m_locG338Ta0 = -1, m_locG338Aem = -1, m_locG338FbScale = -1;
    GLint m_locG281FbScale = -1;
    GLuint m_vao = 0, m_vbo = 0;
    GLuint m_g281MapTex = 0, m_g281ClutTex = 0, m_g281Fbo = 0;
    // G627: per-byte-lane masked FBO patch (PSMT8-in-CT32 l2l destinations).
    GLuint m_g627Prog = 0, m_g627ValTex = 0, m_g627MaskTex = 0;
    GLint m_locG627Origin = -1, m_locG627Scale = -1, m_locG627Lane = -1;
    int m_g627TexW = -1, m_g627TexH = -1;
    bool m_g627ProgFailed = false;
    GLuint m_g309MapTex = 0, m_g309BaseTex = 0;
    GLint m_locFbSize = -1, m_locFbScale = -1, m_locMode = -1, m_locLinear = -1, m_locFogCol = -1;
    GLint m_locWrapU = -1, m_locWrapV = -1;
    GLint m_locG560FbSize = -1, m_locG560FbScale = -1, m_locG560Mode = -1, m_locG560Linear = -1;
    GLint m_locG560FogCol = -1, m_locG560WrapU = -1, m_locG560WrapV = -1;
    GLint m_locG309FbScale = -1;
    GLint m_locG560UvOriginX = -1, m_locG560UvOriginY = -1;
    GLint m_locG560UvRoundU = -1, m_locG560UvRoundV = -1;
    GLint m_locExactFbSize = -1, m_locExactMode = -1, m_locExactLinear = -1;
    GLint m_locExactWrapU = -1, m_locExactWrapV = -1;
    GLint m_locUvOriginX = -1, m_locUvOriginY = -1;
    GLint m_locUvRoundU = -1, m_locUvRoundV = -1;
    GLint m_locUvFlipV = -1; // G624
    std::map<uint32_t, FboEntry> m_fbos;
    std::unordered_map<uint64_t, DepthEntry> m_depths;
    std::unordered_map<uint64_t, TexEntry> m_texCache;
    // G620: the per-draw texture-handle vector, hoisted to a member so the submit path stops
    // heap-allocating one per flush (45/frame). Owned by the submitting worker only, exactly like
    // m_texCache.
    std::vector<TexEntry *> m_g620DrawTex;
    size_t m_texBytes = 0;
    uint64_t m_batchCounter = 0;
    uint64_t m_g257RealBatches = 0;
    uint64_t m_g258TraceBatches = 0;
    // G362 census: textured legacy-path draws taking the guarded fixed-point point fetch vs the
    // float floor()/texture() path. Reported by [G362:pt] under DC2_G362_CENSUS=1.
    uint64_t m_g362GuardDraws = 0;
    uint64_t m_g362PlainDraws = 0;
    std::vector<uint32_t> m_g257ImageAfter;
    size_t m_g257Changed = 0, m_g257Covered = 0;
    GLenum m_g257PreError = GL_NO_ERROR, m_g257PostError = GL_NO_ERROR;
    unsigned m_g257PreErrorCount = 0, m_g257PostErrorCount = 0;

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cvWork;
    // G447: monotone submit counter — a lock-free hint the worker spins on before sleeping.
    std::atomic<uint64_t> m_submitSeq{0ull};
    std::condition_variable m_cvReady;
    std::deque<QueueItem> m_queue;
    bool m_stop = false;
    bool m_started = false;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};

    struct AsyncSlot
    {
        G178Batch batch;
        std::future<bool> future;
    };
    // G452: stable deque storage keeps QueueItem::batch pointers valid while up to eight packets
    // are in flight. Legacy G305 still drains at every flush and therefore uses this as one slot.
    std::deque<AsyncSlot> m_asyncSlots;
    G178Batch m_asyncRecycle;

    std::mutex m_residentMtx;
    std::unordered_set<uint64_t> m_resident;
    uint64_t m_g299Total = 0u;
    uint64_t m_g299Count[4]{};
    uint64_t m_g299QueueNs[4]{};
    uint64_t m_g299WorkNs[4]{};
    uint64_t m_g299PacketBytes[4]{};
    uint64_t m_g299Draws[4]{};
    uint64_t m_g324Total = 0u;
    uint64_t m_g324Count[3]{};
    uint64_t m_g324QueueNs[3]{};
    uint64_t m_g324WorkNs[3]{};
    // G415: worker-thread-only stage census state (see g415NoteStages).
    std::map<uint64_t, G415Stage> m_g415Stages;
    uint64_t m_g415Total = 0u;
    uint64_t m_g415StatN = 0u;
    // G417 diagnostic-only framebuffer-upload split (worker thread only).
    std::map<uint64_t, G417UploadStage> m_g417Uploads;
    uint64_t m_g417UploadTotal = 0u;
    // G418 diagnostic-only colour-readback drain/transfer split (worker thread only).
    std::map<uint32_t, G418ReadEdge> m_g418Edges;
    uint64_t m_g418Total = 0u;
    // G415 FBO geometry recycling counters (worker thread only).
    uint64_t m_g415FboCreates = 0u;
    uint64_t m_g415FboParks = 0u;
    uint64_t m_g415FboRevives = 0u;
    // G420 narrow-readback mechanism probe: inert destination-side write target (worker thread
    // only, allocated only when DC2_G419_AB=narrowtouch is armed).
    std::vector<uint32_t> m_g420TouchScratch;
};
} // namespace

// G332: global-scope accessor (external linkage) so ps2_gif_arbiter.cpp's frameDrain print can read
// the per-type backend GL-busy accumulators. Anon-namespace names above remain file-visible here.
void g332_backend_snapshot(uint64_t nsOut[4], uint64_t cntOut[4])
{
    for (int i = 0; i < 4; ++i)
    {
        nsOut[i] = g_g332BackendNs[i].load(std::memory_order_relaxed);
        cntOut[i] = g_g332BackendCount[i].load(std::memory_order_relaxed);
    }
}

// G453: global-scope accessor for the 30-frame GS-worker boundary report.
void g453_backend_other_snapshot(uint64_t nsOut[4], uint64_t cntOut[4])
{
    for (int i = 0; i < 4; ++i)
    {
        nsOut[i] = g_g453BackendOtherNs[i].load(std::memory_order_relaxed);
        cntOut[i] = g_g453BackendOtherCount[i].load(std::memory_order_relaxed);
    }
}

// G494: global-scope accessor for the driver-round-trip census, read from the same 30-frame GS-worker
// boundary that prints [G453:other]. Returns the site count so the printer never has to hard-code it.
int g494_backend_sync_snapshot(uint64_t nsOut[], uint64_t nOut[], uint64_t badOut[], int cap,
                               const char *const **namesOut)
{
    const int n = cap < kG494SyncSites ? cap : kG494SyncSites;
    for (int i = 0; i < n; ++i)
    {
        nsOut[i] = g_g494SyncNs[i].load(std::memory_order_relaxed);
        nOut[i] = g_g494SyncN[i].load(std::memory_order_relaxed);
        badOut[i] = g_g494SyncBad[i].load(std::memory_order_relaxed);
    }
    if (namesOut != nullptr)
        *namesOut = kG494SyncName;
    return n;
}
