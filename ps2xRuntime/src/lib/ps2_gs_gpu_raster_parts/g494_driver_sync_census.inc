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
// real query every 64th item (~1.1 per frame) only while the elision is active; costs one predicted
// branch otherwise.
static inline void g494SentinelTick()
{
    if (!g494NoSyncActive() || g_g494Failsafe.load(std::memory_order_relaxed))
        return;
    if ((g_g494SentinelN.fetch_add(1ull, std::memory_order_relaxed) % 64ull) != 0ull)
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
