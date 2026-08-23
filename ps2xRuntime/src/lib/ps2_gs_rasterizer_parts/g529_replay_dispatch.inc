// ===== G529: the CPU band replay's DISPATCH, priced and sized to the batch it is given =====
// revision: 1
//
// THE MEASUREMENT THAT LED HERE (G528 §1, re-taken this phase). Post-G527 the deferred drain on
// Georama splits into:
//
//   bucket                          count/f   ms/f    us each
//   gpuOk   (GPU flush accepted)      13.96    6.553    469
//   replay  (CPU band replay)          5.01    4.796    957     <- 3.35 entries per batch
//   pub     (publication prologue)     5.01    2.795 -> ~0 after G528's scopedpub
//
// 957 us to replay 3.35 sprite entries. Those entries are 4,160-8,253 px each, so the batch is
// ~20 k pixels: at any sane software-raster rate that is tens of microseconds of work, not a
// millisecond. The number does not describe rasterization.
//
// ⭐⭐ THE STRUCTURAL DEFECT — THE PHASE'S THROUGH-LINE, ONE LEVEL DOWN.
// `GSRowPool::run(y0, y1, g_g144Threads, job)` is called with a CONSTANT lane count for every
// batch. That pool was built (G143/G168) for the DISPLAY batch: `[G528:batch]` measures those at
// 66,186 entries over 283 batches = 234 entries each, covering the whole 512x416 frame. An 8-way
// fork/join amortizes beautifully over 234 full-width entries.
//
// G527 then created a SECOND population that takes the identical path: 5,717 discovered-target
// batches carrying 19,445 entries = 3.4 each, a few thousand pixels, often a handful of rows. The
// dispatch cost is FIXED per call (G419 measured the pool's floor at ~0.085 ms and G420 narrowed
// the join for exactly this reason) while the work being split is ~1/70th of what the constant was
// chosen for. This is the same defect class as G526/G527/G528 — a mechanism gated or sized by
// something NARROWER (here: by nothing at all, a compile-time constant) than the property it needs
// (here: how much parallelizable work this batch actually contains).
//
// ⛔ BEFORE THE LEVER, THE COUNTER FOR ITS CLAIMED MECHANISM (g527_lineclip_refuted_by_its_own_census).
// `[G529:disp]` splits the `replay` bucket into what the lever can move and what it cannot:
//
//   zclr    g106TitleZClearIfNeeded + g202TownZClearIfNeeded   (inside G290's `replay`, not raster)
//   scan    the y0/y1 min-max loop over the entry list
//   wall    GSRowPool::run, caller-observed
//   lane0   the band the CALLER rasterized itself, inside that wall
//   worker  the sum of every OTHER band's rasterization
//   rt      wall - lane0  -- G497's law: caller wait minus the caller's own work IS the round trip
//
// If `rt` is small the mechanism is refuted and the pixels really are slow; only a large `rt` on a
// small `worker` licenses the lever. Split by batch class (display / legacy / discovered / other)
// because the two populations must never be averaged again (g526_per_item_average_hides_a_bimodal_population).
//
// THE LEVER (`inlrep`). Choose the lane count PER DISPATCH from the batch's own parallelizable
// work instead of the constant:
//
//   want  = px / DC2_G529_PXPERLANE          (how many lanes this batch can feed)
//   lanes = clamp(1, min(g_g144Threads, rows), want)
//
// px is the scissor-bounded row-span area summed over the entries -- the same quantity the pool
// splits. `lanes == 1` makes GSRowPool::run take its OWN existing `if (lanes <= 1) job(y0, y1)`
// path: no notify_all, no m_cvDone wait, no worker wake. That is why this is bit-exact by
// CONSTRUCTION rather than by argument -- it is the path the pool already runs on a single-core
// host, and the job body sets t_g144Banded with the FULL row range, which clamps nothing.
//
// Both thresholds are ENV-TUNABLE so the operating point can be swept without a rebuild
// (g507_probe_at_two_magnitudes / g508_the_curve_is_one_session_too).
//
//   DC2_G529_STAT=1        census (above). Two clock reads per dispatch + one per lane.
//   DC2_G529_INLINEREPLAY=1  the lever (bring-up default: OFF)
//   DC2_G529_NO_INLINEREPLAY=1  rollback once promoted
//   DC2_G529_PXPERLANE=<n>   px of row-span area required to justify one lane (default 262144)
//   DC2_G529_MINROWS=<n>     rows required before ANY split is considered (default 8)

namespace
{

// G570 declarations.  The implementation lives after G530's CT32 row-address helpers, but the
// replay dispatcher must bracket each lane so the exact 0x139 triangle kernel can operate on one
// linear color band instead of paying the GS swizzle for every accepted fragment.
static bool g570Cpu139LinearBatchCandidate();
static void g570Cpu139LinearBegin(uint8_t *vram, size_t vramSize, int y0, int y1);
static void g570Cpu139LinearEnd(uint8_t *vram, size_t vramSize);
static bool g570Cpu139BatchOn();
static void g570Cpu139PrepareDepthTiles();
static int g570Cpu139DepthClass(int x0, int y0, int x1, int y1,
                                float z0, float z1, float z2);
static bool g570Batch139Prepare();
static int g570Batch139LastReject();
static uint64_t g570Batch139ReplayBand(uint8_t *vram, size_t vramSize, int y0, int y1);
static uint64_t g570Batch139ReplayTri(uint8_t *vram, size_t vramSize,
                                     int y0, int y1, size_t triIndex);
static size_t g570Batch139TriCount();
static void g570Batch139ReadBand(const uint8_t *vram, size_t vramSize,
                                 int y0, int y1, std::vector<uint32_t> &out);
static void g570Batch139WriteBand(uint8_t *vram, size_t vramSize,
                                  int y0, int y1, const std::vector<uint32_t> &in);
static void g570Batch139NoteVerify(uint64_t pixels, uint64_t bad);
static bool g570Batch139VerifyOn();
static bool g570Batch139GpuOn();
static bool g570Batch139GpuVerifyOn();
static bool g570Batch139ReplayGpu(const std::vector<uint32_t> &color,
                                  std::vector<uint32_t> &out);
static void g570Batch139NoteGpuVerify(uint64_t pixels, uint64_t bad);
static void g570Batch139ResetCpuDebug();
static uint32_t g570Batch139GpuDebugAt(size_t index);
static uint32_t g570Batch139CpuDebugAt(size_t index);
static uint32_t g570Batch139PackedCoverageHash(int x, int y);

// G641 declarations. The 0x139 shadow compute's producer-scoped window is derived from G570's tile
// bins, so its implementation follows g570_cpu_triangle.inc while its only call site — the batch
// dispatch in rasterizer_tilebin_capture.inc — precedes it.
static void g641NoteBatch();
static bool g641WinOn();
static bool g641WinUsable();
static bool g641WinVerifyOn();
static bool g641ShadowRect(int &x, int &y, int &w, int &h);
static void g641ReadShadowBand(const uint8_t *vram, size_t vramSize, int y0, int y1,
                               std::vector<uint32_t> &out);
static void g641WriteShadowBand(uint8_t *vram, size_t vramSize, int y0, int y1,
                                const std::vector<uint32_t> &in);
static void g641NoteWindowUse(bool narrowed);
static void g641RunVerify(const std::vector<uint32_t> &initial,
                          const std::vector<uint32_t> &narrow);

// G580 declarations. The implementation follows G530's swizzle helpers, while the replay
// dispatcher owns the candidate-only serial CLUT preparation and per-entry TLS binding.
using G580ClutFillFn =
    std::function<void(const G144Entry &, std::array<uint32_t, 256> &)>;
static bool g580SharedClutBatchSelected(const std::vector<G144Entry> &entries);
static bool g580PrepareSharedCluts(const std::vector<G144Entry> &entries,
                                   const G580ClutFillFn &fill);
// G605 declarations. The property-admitted twin of the two above: it populates the SAME
// g_g580SharedSamples / g_g580SharedCluts tables (so g580BindSharedSample serves both leaves) but
// selects the wider shape family `[G605:shape]` measured at 70.6% of the replay's cycles.
static bool g605SharedClutBatchSelected(const std::vector<G144Entry> &entries);
static bool g605PrepareSharedCluts(const std::vector<G144Entry> &entries,
                                   const G580ClutFillFn &fill);
static void g580BindSharedSample(size_t entryIndex);
static void g580ClearSharedSample();

static bool g529StatOn()
{
    static const bool on = envFlagEnabled("DC2_G529_STAT");
    return on;
}

// Bring-up default OFF. Promotion flips this to `!envFlagEnabled("DC2_G529_NO_INLINEREPLAY")`.
static bool g529InlineReplayOn()
{
    static const bool on = envFlagEnabled("DC2_G529_INLINEREPLAY") &&
                           !envFlagEnabled("DC2_G529_NO_INLINEREPLAY");
    return on;
}

static uint64_t g529EnvU64(const char *name, uint64_t dflt)
{
    const char *e = std::getenv(name);
    if (!e || !*e)
        return dflt;
    const long long v = std::atoll(e);
    return (v <= 0) ? dflt : static_cast<uint64_t>(v);
}

// 262,144 px = one 512x512 surface's worth of row-span area per lane. Deliberately conservative
// for bring-up: at this setting the whole discovered-target population collapses to lanes=1 and
// the display batch (512x416 x 234 entries) still gets every lane. Swept in-session.
static uint64_t g529PxPerLane()
{
    static const uint64_t v = g529EnvU64("DC2_G529_PXPERLANE", 262144ull);
    return v;
}

static uint64_t g529MinRows()
{
    static const uint64_t v = g529EnvU64("DC2_G529_MINROWS", 8ull);
    return v;
}

// ---------------------------------------------------------------------------------------------
// Census accumulators. thread_local for the dispatch-side laps (the flush closure runs on ONE
// thread -- the G144 flush-owner contract every function in it already relies on), but the LANE
// laps are written by pool workers, so those two are atomics. An atomic RMW per BAND is ~4/dispatch
// at 5 dispatches/frame; an atomic per lap inside the raster loop would be the self-inflating
// census g526_draw_census.inc's rule forbids, and this is not that.
enum G529Cls
{
    kG529ClsDisplay = 0,
    kG529ClsLegacy,
    kG529ClsDiscovered,
    kG529ClsOther,
    kG529ClsN
};
static const char *const kG529ClsName[kG529ClsN] = {"disp", "legacy", "discov", "other"};

struct G529Acc
{
    uint64_t n[kG529ClsN];        // dispatches
    uint64_t wallNs[kG529ClsN];   // GSRowPool::run wall, caller-observed
    uint64_t zclrNs[kG529ClsN];   // the two Z clears that sit inside G290's `replay`
    uint64_t scanNs[kG529ClsN];   // the y0/y1 + px scan
    uint64_t entries[kG529ClsN];
    uint64_t rows[kG529ClsN];
    uint64_t px[kG529ClsN];
    uint64_t lanesAsked[kG529ClsN];
    uint64_t inlined[kG529ClsN];  // dispatches that resolved to lanes == 1
};
static thread_local G529Acc t_g529{};

// Written by whichever thread ran the band. Indexed by the class of the dispatch in flight.
static std::atomic<uint64_t> g_g529Lane0Ns{0};
static std::atomic<uint64_t> g_g529WorkerNs{0};
static std::atomic<uint64_t> g_g529WorkerBands{0};
static std::atomic<uint64_t> g_g529Lane0Bands{0};

// The dispatch in flight: set by the flush closure immediately before GSRowPool::run and cleared
// after it returns. `callerTid` is what separates the caller's own band (lane 0, which the pool
// runs INLINE) from the worker bands -- that difference is the whole point of the census.
// `callerTid` is written before `active` is released and read after it is acquired, so a worker
// woken by the dispatch always sees the matching pair. `run` cannot return while any lane job is
// still executing (the narrow join subtracts only AFTER the lane loop exits), so clearing `active`
// after it returns cannot race a live G529LaneScope.
static std::thread::id g_g529CallerTid{};
static std::atomic<bool> g_g529InFlight{false};

static void g529BeginDispatch()
{
    g_g529CallerTid = std::this_thread::get_id();
    g_g529InFlight.store(true, std::memory_order_release);
}
static void g529EndDispatch() { g_g529InFlight.store(false, std::memory_order_release); }

// RAII lap for one band, constructed inside the pool job. No-op unless the census is armed.
struct G529LaneScope
{
    std::chrono::steady_clock::time_point t0;
    bool on;
    G529LaneScope() : on(g529StatOn() && g_g529InFlight.load(std::memory_order_acquire))
    {
        if (on)
            t0 = std::chrono::steady_clock::now();
    }
    ~G529LaneScope()
    {
        if (!on)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        if (std::this_thread::get_id() == g_g529CallerTid)
        {
            g_g529Lane0Ns.fetch_add(ns, std::memory_order_relaxed);
            g_g529Lane0Bands.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            g_g529WorkerNs.fetch_add(ns, std::memory_order_relaxed);
            g_g529WorkerBands.fetch_add(1u, std::memory_order_relaxed);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// The lane-count policy. Returns g_g144Threads unchanged unless the lever is armed, so the shipped
// default is byte-identical to pre-G529.
//
// `rows` is the union row span the pool will split; `px` is the scissor-bounded row-span area
// summed over the entries -- an upper bound on the pixels any band can touch, and the same
// quantity the split divides. Both are computed by the caller in the loop it already runs.
static int g529ReplayLanes(uint64_t rows, uint64_t px, int constantLanes)
{
    if (!g529InlineReplayOn())
        return constantLanes;
    if (constantLanes <= 1)
        return constantLanes;
    if (rows < g529MinRows())
        return 1;
    const uint64_t want = px / g529PxPerLane();
    if (want <= 1u)
        return 1;
    uint64_t lanes = want;
    if (lanes > static_cast<uint64_t>(constantLanes))
        lanes = static_cast<uint64_t>(constantLanes);
    if (lanes > rows)
        lanes = rows; // a band with no rows is a wake-up that rasterizes nothing
    return static_cast<int>(lanes < 1u ? 1u : lanes);
}

static void g529NoteDispatch(uint32_t cls, uint32_t fbp, uint64_t wallNs,
                             uint64_t zclrNs, uint64_t scanNs, size_t entries,
                             uint64_t rows, uint64_t px, int lanes)
{
    if (!g529StatOn())
        return;
    const uint32_t c = (cls < kG529ClsN) ? cls : static_cast<uint32_t>(kG529ClsOther);
    ++t_g529.n[c];
    t_g529.wallNs[c] += wallNs;
    t_g529.zclrNs[c] += zclrNs;
    t_g529.scanNs[c] += scanNs;
    t_g529.entries[c] += entries;
    t_g529.rows[c] += rows;
    t_g529.px[c] += px;
    t_g529.lanesAsked[c] += static_cast<uint64_t>(lanes);
    if (lanes <= 1)
        ++t_g529.inlined[c];

    // G570: the jump/dash stalls are isolated, while the cumulative class means below mix their
    // 80-180 ms CPU replays with hundreds of sub-millisecond batches. Name the exact framebuffer
    // of each slow replay before changing admission. This remains under DC2_G529_STAT and is
    // therefore silent in normal runs.
    if (wallNs >= 20000000ull)
    {
        static uint64_t s_g570SlowReplay = 0u;
        const uint64_t n = ++s_g570SlowReplay;
        if (n <= 128u)
            std::fprintf(stderr,
                         "[G570:slowreplay] n=%llu fbp=%03x cls=%s wall=%.2fms entries=%zu "
                         "rows=%llu px=%llu lanes=%d zclr=%.3fms scan=%.3fms\n",
                         (unsigned long long)n, fbp, kG529ClsName[c],
                         static_cast<double>(wallNs) / 1e6, entries,
                         (unsigned long long)rows, (unsigned long long)px, lanes,
                         static_cast<double>(zclrNs) / 1e6,
                         static_cast<double>(scanNs) / 1e6);
    }

    uint64_t total = 0;
    for (int i = 0; i < kG529ClsN; ++i)
        total += t_g529.n[i];
    if ((total % 400ull) != 0ull)
        return;

    const double l0 = static_cast<double>(g_g529Lane0Ns.load(std::memory_order_relaxed)) / 1e6;
    const double wk = static_cast<double>(g_g529WorkerNs.load(std::memory_order_relaxed)) / 1e6;
    double wallAll = 0.0;
    for (int i = 0; i < kG529ClsN; ++i)
        wallAll += static_cast<double>(t_g529.wallNs[i]) / 1e6;
    std::fprintf(stderr,
                 "[G529:disp] n=%llu wall=%.1f lane0=%.1f worker=%.1f rt=%.1f "
                 "bands(l0=%llu wk=%llu) (cumMs)\n",
                 (unsigned long long)total, wallAll, l0, wk, wallAll - l0,
                 (unsigned long long)g_g529Lane0Bands.load(std::memory_order_relaxed),
                 (unsigned long long)g_g529WorkerBands.load(std::memory_order_relaxed));
    for (int i = 0; i < kG529ClsN; ++i)
    {
        if (t_g529.n[i] == 0u)
            continue;
        const double nb = static_cast<double>(t_g529.n[i]);
        std::fprintf(stderr,
                     "[G529:cls] %s n=%llu wall=%.1fms(%.0fus ea) zclr=%.1f scan=%.1f "
                     "entries/b=%.2f rows/b=%.0f px/b=%.0f lanes/b=%.2f inlined=%llu\n",
                     kG529ClsName[i], (unsigned long long)t_g529.n[i],
                     static_cast<double>(t_g529.wallNs[i]) / 1e6,
                     static_cast<double>(t_g529.wallNs[i]) / 1e3 / nb,
                     static_cast<double>(t_g529.zclrNs[i]) / 1e6,
                     static_cast<double>(t_g529.scanNs[i]) / 1e6,
                     static_cast<double>(t_g529.entries[i]) / nb,
                     static_cast<double>(t_g529.rows[i]) / nb,
                     static_cast<double>(t_g529.px[i]) / nb,
                     static_cast<double>(t_g529.lanesAsked[i]) / nb,
                     (unsigned long long)t_g529.inlined[i]);
    }
    std::fflush(stderr);
}

// =============================================================================================
// [G529:spr] — WHY IS A REPLAYED RTT SPRITE 41x THE COST PER PIXEL OF A DISPLAY SPRITE?
//
// The two-magnitude lane probe (DC2_G529_PXPERLANE=100000000, i.e. lanes=1 everywhere) removed
// every threading artefact and left the gap intact, measured SERIALLY:
//
//   class    us/batch @1 lane   px/batch     ns per pixel
//   disp        18,933          5,750,440       3.3
//   discov       3,773             27,435     137.0        <- 41x
//
// Same GSRasterizer::drawSprite, same band replay, same host. So the difference is WHICH PATH a
// sprite takes inside drawSprite, and the candidate is the G248/G250 fast sampler: G526 widened
// only its TARGET term (g526FastSpriteTarget), and six other conjuncts remain — `fst`,
// CT32/CT24/T8 psm, `tbw != 0`, the two diagnostic-off terms, and the master rollback.
//
// ⛔ That is a HYPOTHESIS, and this port's standing rule is to build the counter for the claimed
// mechanism BEFORE the lever (g527_lineclip_refuted_by_its_own_census — an entire phase's second
// lever died to `ratio=1.0000` from a census that should have been written first). So this books
// every REPLAYED sprite to fast/slow with its pixel count and its own elapsed time, and names the
// FIRST failing conjunct. `ns/px` per bucket is then a price, not an inference: if `slow` carries
// the pixels and the ns, the admission predicate is the defect; if `fast` does, it is not, and the
// 137 ns/px lives somewhere else entirely.
//
// Scope is `t_g144InReplay` only — the deferred-batch population is the subject, and including
// inline draws would average two populations again (g526_per_item_average_hides_a_bimodal_population).
// Atomics because the eight band lanes all write; ~141 sprite draws/frame, so three atomic RMWs
// each is not a self-inflating census.
enum G529SprB
{
    kG529SprFast = 0, // took the G248/G250 hoisted sampler
    kG529SprSlow,     // fell through to the generic per-pixel sampleTexture()
    kG529SprOther,    // untextured, or returned before the sampler (scissor/degenerate)
    kG529SprBN
};
static const char *const kG529SprBName[kG529SprBN] = {"fast", "slow", "other"};

enum G529SprR
{
    kG529SprROk = 0,
    kG529SprRTarget, // g526FastSpriteTarget said no
    kG529SprRFst,    // STQ sprite, not FST
    kG529SprRPsm,    // texture psm outside CT32 / CT24 / T8-gsmem
    kG529SprRTbw,    // tbw == 0
    kG529SprRTrace,  // a diagnostic trace is armed
    kG529SprRFilter, // a special filter override is armed
    kG529SprROff,    // master rollback, or never reached the predicate
    kG529SprRN
};
static const char *const kG529SprRName[kG529SprRN] = {
    "ok", "target", "fst", "psm", "tbw", "trace", "filter", "off"};

static std::atomic<uint64_t> g_g529SprN[kG529SprBN];
static std::atomic<uint64_t> g_g529SprPx[kG529SprBN];
static std::atomic<uint64_t> g_g529SprNs[kG529SprBN];
static std::atomic<uint64_t> g_g529SprLin[kG529SprBN]; // bilinear => four samples per pixel
static std::atomic<uint64_t> g_g529SprReason[kG529SprRN];
static std::atomic<uint64_t> g_g529SprPsm[16];

static void g529SprReport()
{
    static std::atomic<uint64_t> s_ticks{0};
    if ((s_ticks.fetch_add(1u, std::memory_order_relaxed) % 20000ull) != 0ull)
        return;
    std::fprintf(stderr, "[G529:spr]");
    for (int i = 0; i < kG529SprBN; ++i)
    {
        const double n = (double)g_g529SprN[i].load(std::memory_order_relaxed);
        const double px = (double)g_g529SprPx[i].load(std::memory_order_relaxed);
        const double ns = (double)g_g529SprNs[i].load(std::memory_order_relaxed);
        std::fprintf(stderr, " %s(n=%.0f px=%.0f ms=%.1f ns/px=%.1f lin=%.0f)", kG529SprBName[i],
                     n, px, ns / 1e6, px > 0.0 ? ns / px : 0.0,
                     (double)g_g529SprLin[i].load(std::memory_order_relaxed));
    }
    std::fprintf(stderr, "\n[G529:sprwhy]");
    for (int i = 0; i < kG529SprRN; ++i)
        std::fprintf(stderr, " %s=%llu", kG529SprRName[i],
                     (unsigned long long)g_g529SprReason[i].load(std::memory_order_relaxed));
    std::fprintf(stderr, " psm(");
    for (int i = 0; i < 16; ++i)
    {
        const uint64_t v = g_g529SprPsm[i].load(std::memory_order_relaxed);
        if (v)
            std::fprintf(stderr, "%x:%llu ", i, (unsigned long long)v);
    }
    std::fprintf(stderr, ")\n");
    std::fflush(stderr);
}

struct G529SprScope
{
    std::chrono::steady_clock::time_point t0;
    bool on;
    int bucket;
    int reason;
    uint64_t px;
    bool linear;
    uint8_t psm;
    G529SprScope()
        : on(g529StatOn() && t_g144InReplay), bucket(kG529SprOther), reason(kG529SprROff), px(0u),
          linear(false), psm(0u)
    {
        if (on)
            t0 = std::chrono::steady_clock::now();
    }
    void setPx(uint64_t p)
    {
        if (on)
            px = p;
    }
    void classify(bool fast, int rsn, bool lin, uint8_t p)
    {
        if (!on)
            return;
        bucket = fast ? kG529SprFast : kG529SprSlow;
        reason = rsn;
        linear = lin;
        psm = p;
    }
    ~G529SprScope()
    {
        if (!on)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        g_g529SprN[bucket].fetch_add(1u, std::memory_order_relaxed);
        g_g529SprPx[bucket].fetch_add(px, std::memory_order_relaxed);
        g_g529SprNs[bucket].fetch_add(ns, std::memory_order_relaxed);
        if (linear)
            g_g529SprLin[bucket].fetch_add(1u, std::memory_order_relaxed);
        g_g529SprReason[reason].fetch_add(1u, std::memory_order_relaxed);
        g_g529SprPsm[psm & 0xFu].fetch_add(1u, std::memory_order_relaxed);
        g529SprReport();
    }
};

} // namespace
