// ===== G494: the DRIVER ROUND TRIP — census + elision lever ====================================
//
// PREMISE (re-taken on the current binary before a line was written — the G476 law).
//
//   [G447:edge] windowMs/f=23.6-24.3 | backend-render=3.51-3.70/34.0
//               backend-readcolor=3.12-3.36/6.0 backend-writecolor=0.41-0.44/11.0
//               backend-copy=0.10-0.11/2.0 backend-other=3.00-3.16/20.0
//               | blocked=10.84-11.24 cpu=12.68-13.31 (45-47% blocked)
//   [G453:other] t8=1.05-1.07/10.0 ct24=0.92-1.05/1.0 composite=0.99-1.00/9.0 upload=0.38-0.41/11.0
//   gsslow sensitivity 0.91 (t=25.0, 0/5) · vu1slow 0.00 n.s. (t=0.14, 3/6)
//
// THE OBSERVATION THIS PART EXISTS TO TEST. The per-item cost of the GS worker's blocking backend
// items is very nearly CONSTANT across items that do wildly different amounts of GPU work:
//
//   render      3.6 ms / 34 = 105 us   (a batch of ~418 draws + its texture uploads)
//   t8 view     1.07 ms / 10 = 107 us  (one full-screen triangle into a 512x512 view)
//   composite   1.00 ms /  9 = 111 us  (one full-screen triangle + a page-map/base upload)
//   writecolor  0.41 ms / 11 =  37 us  (one glTexSubImage2D)
//
// A batch of 418 draws costing the same as one triangle is not a GPU-work signature; it is a FIXED
// PER-ITEM OVERHEAD. This host is an NVIDIA GTX 1060 whose GL driver runs the multi-threaded
// ("Threaded Optimization") command path, and on that path every GL entry point that must RETURN A
// SERVER-SIDE VALUE to the caller — glGetError, glCheckFramebufferStatus — cannot be queued: the
// calling thread has to wait for the driver's own worker to drain its command stream first. The
// backend issues ~110 such queries per frame, and every one of them is pure validation: none of them
// draws anything, uploads anything, or changes a single output byte.
//
// That is the exact shape the arc has been looking for since G493 closed the overlap vein: work that
// can be REMOVED rather than rescheduled. It also retro-explains why overlap never composed on this
// host — the shared resource that serialises the two threads is the driver's internal worker, and
// each of these queries is a rendezvous with it.
//
// ---------------------------------------------------------------------------------------------
// 1. THE CENSUS (DC2_G494_SYNC=1, default OFF, behaviour-pure).
//
// Times every sync-class query on the GL worker, keyed by site, and counts how many of them ever
// report anything other than the success value. `[G494:sync]` prints per-frame ms and counts from
// the same 30-frame GS-worker boundary that `[G453:other]` uses, so the two are directly comparable.
// One steady_clock pair per query (~25 ns) against a query the census exists to prove costs 10^3
// times that; the instrument cannot manufacture its own result (17e law 4).
//
// MEASURED, on the binary this part was written against:
//
//   [G494:sync] total=4.56-4.67 ms/f over 100.0 queries/f  bad=0
//     rendfbo 1.40-1.59/34  ct24drain 0.69-0.83/1  t8fbo 0.49-0.50/10  comperr 0.45-0.46/9
//     wcolerr 0.31-0.33/11  compdrain 0.32-0.34/9  t8drain 0.30-0.34/10  t8err 0.16/10
//     copyerr 0.08-0.12/2   ensfbo 0.09-0.10/2     ct24fbo 0.04/1       ct24err 0.03/1
//
// 4.6 ms/f — 43% of the pole thread's entire 10.6-11.0 ms/f blocked half — inside queries that
// produce no output byte, and NOT ONE of them has ever reported a problem. `ct24drain` at 0.69-0.83
// ms for a SINGLE call is the whole of `[G453:other] ct24`: the item G492 spent a phase optimising
// was never the view build at all, it was one stale-error drain sitting where the frame's heaviest
// pending GPU work lands.
//
// 2. THE LEVER (DC2_G419_AB=nosync paired; PROMOTED default-ON, rollback DC2_G494_NO_ELIDE=1).
//
// Elides the per-item validation queries. EXACTNESS ARGUMENT, site by site:
//
//   * The pre-build drain loops (`while (glGetError() != GL_NO_ERROR) {}`) exist ONLY so the
//     post-draw check below can attribute an error to this build rather than to a previous one.
//     With the post-draw check gone they have no consumer at all.
//   * The post-draw `glGetError()` checks select between "publish the produced view" and "fail ->
//     the front end's CPU replay". The census reports how many times they have ever selected the
//     second branch; on this route that count is the thing to read before promoting anything.
//   * `glCheckFramebufferStatus` in renderBatch attaches either the FBO's OWN depth renderbuffer
//     (same object, same size, already complete) or a depth TEXTURE whose dimensions the caller has
//     already validated against the colour attachment two lines above. Completeness is a property of
//     the attachment set, and the attachment set is invariant.
//   * `glCheckFramebufferStatus` on the ensureFbo REVIVE path re-attaches the same colour texture
//     and the same depth renderbuffer that were validated when that FBO was created. The CREATE
//     path's check is NOT elided — it runs ~12 times in a whole run and is the only one that
//     validates a genuinely new attachment set.
//
// FAILSAFE, so this is not a one-way door. Every 64th backend item (~1.1 per frame, against ~110
// elided) the worker runs ONE `glGetError()`. Drawing into an incomplete framebuffer raises
// GL_INVALID_FRAMEBUFFER_OPERATION, so the sentinel covers the elided framebuffer-status checks as
// well as the elided error checks. If it ever trips, `g_g494Failsafe` latches for the rest of the
// process, every elided query comes back, and `[G494:sync-fail]` says so. The failure mode of the
// lever is therefore "one frame of undetected GL error, then the shipped behaviour", not "silent
// corruption forever".
//
// NOTE (the appendix's anonymous-namespace linkage trap): g494NoSyncArm() is defined at global scope
// in g419_ab_instrument.inc and declared in ps2_gs_gpu_raster.cpp ABOVE the anonymous namespace this
// part sits inside. Do not re-declare it here.

// Sites. Ordered by the bucket they belong to in [G447:edge] / [G453:other].
static constexpr int kG494SyncSites = 13;
static constexpr int kG494T8Drain = 0;    // prepareT8ViewOnWorker      stale-error drain
static constexpr int kG494T8Fbo = 1;      //                            framebuffer status
static constexpr int kG494T8Err = 2;      //                            post-draw error check
static constexpr int kG494Ct24Drain = 3;  // prepareCt24ViewOnWorker    stale-error drain
static constexpr int kG494Ct24Fbo = 4;    //                            framebuffer status
static constexpr int kG494Ct24Err = 5;    //                            post-draw error check
static constexpr int kG494CompDrain = 6;  // buildAuthoritativeComposite stale-error drain
static constexpr int kG494CompErr = 7;    //                            post-draw / readback checks
static constexpr int kG494WColErr = 8;    // writeColorOnWorker         post-upload error check
static constexpr int kG494CopyErr = 9;    // copyColorRectsOnWorker     post-copy error check
static constexpr int kG494RenderFbo = 10; // renderBatch                depth-attach status
static constexpr int kG494EnsureFbo = 11; // ensureFbo REVIVE path      status (create path excluded)
static constexpr int kG494DepthFbo = 12;  // readDepthOnWorker          status

static const char *const kG494SyncName[kG494SyncSites] = {
    "t8drain", "t8fbo", "t8err", "ct24drain", "ct24fbo", "ct24err",
    "compdrain", "comperr", "wcolerr", "copyerr", "rendfbo", "ensfbo", "dpthfbo"};

static std::atomic<uint64_t> g_g494SyncNs[kG494SyncSites];
static std::atomic<uint64_t> g_g494SyncN[kG494SyncSites];
static std::atomic<uint64_t> g_g494SyncBad[kG494SyncSites];
static std::atomic<uint64_t> g_g494SentinelN{0ull};
static std::atomic<bool> g_g494Failsafe{false};

static bool g494SyncCensusOn()
{
    static const bool on = (std::getenv("DC2_G494_SYNC") != nullptr);
    return on;
}

// G494 PROMOTED to default-ON (2026-08-03). Four paired within-process runs, hold=16, 3,600 frames
// each: -1.526 / -1.534 / -1.524 / -1.356 ms/f raw, blocked mean -1.456 / -1.502 / -1.532 / -1.298,
// t = -26.8 / -36.9 / -10.9 / -6.2, **24 of 24 windows favouring**, with `[G492:inv]` holding on
// every column (render 34.0, rdcol 6.0, wrcol 11.0, copy 2.0, other 20.0, flush 34.0/34.1).
// Kill with DC2_G494_NO_ELIDE=1.
//
// READ THIS BEFORE QUOTING THE CENSUS (the G493 §2 trap, one level up). Now that the elision is the
// default, `DC2_G494_SYNC=1` ALONE reports almost nothing — the queries it times are gone. To see
// the pre-promotion cost again you must run `DC2_G494_SYNC=1 DC2_G494_NO_ELIDE=1`.
static bool g494NoSyncActive()
{
    const int arm = g494NoSyncArm(); // -1 unless DC2_G419_AB=nosync
    if (arm >= 0)
        return arm == 1;
    static const bool on = (std::getenv("DC2_G494_NO_ELIDE") == nullptr);
    return on;
}

static inline bool g494Elide()
{
    return !g_g494Failsafe.load(std::memory_order_relaxed) && g494NoSyncActive();
}

static inline void g494SyncNote(int site, std::chrono::steady_clock::time_point t0, bool bad)
{
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    g_g494SyncNs[site].fetch_add(ns, std::memory_order_relaxed);
    g_g494SyncN[site].fetch_add(1u, std::memory_order_relaxed);
    if (bad)
        g_g494SyncBad[site].fetch_add(1u, std::memory_order_relaxed);
}

// Stale-error drain. Byte-identical to the `while (glGetError() != GL_NO_ERROR) {}` it replaces on
// the default path — the loop is unbounded there and stays unbounded here.
static inline void g494DrainErrors(int site)
{
    if (g494Elide())
        return;
    if (!g494SyncCensusOn())
    {
        while (glGetError() != GL_NO_ERROR)
        {
        }
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t found = 0u;
    while (glGetError() != GL_NO_ERROR)
        ++found;
    g494SyncNote(site, t0, found != 0u);
}

// Post-draw / post-upload error check. Returns GL_NO_ERROR when elided, i.e. the caller takes the
// success branch it already takes on every frame of this route.
static inline GLenum g494GetError(int site)
{
    if (g494Elide())
        return GL_NO_ERROR;
    if (!g494SyncCensusOn())
        return glGetError();
    const auto t0 = std::chrono::steady_clock::now();
    const GLenum e = glGetError();
    g494SyncNote(site, t0, e != GL_NO_ERROR);
    return e;
}

// Framebuffer completeness query. `okValue` is passed in rather than referenced by name so this part
// does not depend on the GL158_* constants being declared before its include point.
template <typename F>
static inline GLenum g494FboStatus(int site, GLenum okValue, F &&query)
{
    if (g494Elide())
        return okValue;
    if (!g494SyncCensusOn())
        return query();
    const auto t0 = std::chrono::steady_clock::now();
    const GLenum s = query();
    g494SyncNote(site, t0, s != okValue);
    return s;
}

// The failsafe sentinel, called once per backend item at the worker's dispatch choke point. Runs a
// real query every Nth item only while the elision is active; costs one predicted branch otherwise.
//
// ⛔⛔⛔ G743 — THE PERIOD WAS 64, AND THAT IS ~1.1 `glGetError()` PER FRAME ON THE POLE THREAD.
//
// `glGetError` is not a cheap query on this driver: it is a synchronisation point, and the G446
// executor PC sampler with the G733 RtlVirtualUnwind caller (`DC2_G735_EXECPROF=1
// DC2_G737_PAIRS=1 DC2_G733_UNWIND=1`, `dungeon1`) measured it directly —
//
//     [G737:execprofpair] 3.36% n=420 ZwWaitForSingleObject <= g494SentinelTick+0x35
//
// and the return address resolves, out of the shipped image's own import thunk, to
// `__imp_glGetError`. 3.36 % of the G713 executor — the thread the frame is serialised on — is
// **~0.6 ms/f**, larger than most levers this project has ever shipped, paid by a failsafe for an
// elision that has never fired.
//
// FIX: keep the period at 64 for the first `kG494DenseItems` items, where a driver/entry-point
// problem would actually show up, then back off to `kG494SparsePeriod`. Detection latency for a
// steady-state error goes from ~20 ms to ~1 s — irrelevant for a failsafe whose only job is to
// notice that the elision is unsafe on some unknown driver, and which restores every elided query
// for the rest of the process the moment it does. `DC2_G494_SENTINEL_PERIOD=<n>` overrides the
// sparse period (0 or 1 = check every item; 64 reproduces the pre-G743 behaviour exactly).
// ⛔⛔⛔ GATED AND NULL — SO THE DEFAULT STAYS AT THE PRE-G743 RATE. `dungeon1`, ONE binary, the
// only difference the period: `g708_abba_blocks.ps1 -Blocks 2 -TimingOnly`, control = `=64`,
// candidate = `=4096`, pooled **-0.02 ms/f, t = -0.05**, blocks +0.356 / -0.396 (sign disagreement),
// 0/2 Rule-16 admissible. The mechanism CONVERTED and the frame did not, and §2.4 of the plan says
// why: the joint leaf x caller table splits this query's 3.80 % of executor samples into
// **3.36 % `ZwWaitForSingleObject` (BLOCKED in the driver) + 0.44 % `NtQueryInformationThread`
// (driver CPU)**. 88 % of it is a wait for the GPU to drain, and the next `glReadPixels` — 40.1 %
// of this same thread — waits for exactly that work instead. G304 conservation.
//
// The backoff therefore ships REACHABLE but not ARMED: `kG494SparsePeriod` is 64, identical to the
// pre-G743 constant, so current source reproduces the accepted behaviour byte for byte, and
// `DC2_G494_SENTINEL_PERIOD=4096` arms the 60x reduction for anyone re-testing it on a route where
// the executor is NOT blocked behind a readback.
static constexpr uint64_t kG494DenseItems = 4096ull;  // ~55 frames of dense checking at startup
static constexpr uint64_t kG494DensePeriod = 64ull;   // the pre-G743 rate
static constexpr uint64_t kG494SparsePeriod = 64ull;  // == dense: NO default change (gated null)

static uint64_t g494SparsePeriod()
{
    static const uint64_t p = [] () -> uint64_t {
        const char *v = std::getenv("DC2_G494_SENTINEL_PERIOD");
        if (v == nullptr || v[0] == '\0')
            return kG494SparsePeriod;
        const long long n = std::atoll(v);
        return n <= 1 ? 1ull : static_cast<uint64_t>(n);
    }();
    return p;
}

static inline void g494SentinelTick()
{
    if (!g494NoSyncActive() || g_g494Failsafe.load(std::memory_order_relaxed))
        return;
    const uint64_t n = g_g494SentinelN.fetch_add(1ull, std::memory_order_relaxed);
    const uint64_t period = (n < kG494DenseItems) ? kG494DensePeriod : g494SparsePeriod();
    if ((n % period) != 0ull)
        return;
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR)
    {
        g_g494Failsafe.store(true, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G494:sync-fail] sentinel err=0x%x after n=%llu items; every elided query is "
                     "restored for the rest of this process\n",
                     static_cast<unsigned>(e),
                     static_cast<unsigned long long>(
                         g_g494SentinelN.load(std::memory_order_relaxed)));
    }
}
