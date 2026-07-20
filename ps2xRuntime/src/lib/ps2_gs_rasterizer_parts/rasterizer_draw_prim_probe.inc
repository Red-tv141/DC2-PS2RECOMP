// ===== G156 probe (DEFAULT OFF, DC2_G156_STAT=1) — attribute the ~78 ms worker drawPrimitive. =====
// G155 measured mid-flush=0, so the 78 ms billed to drawPrimitive under MTGS+G144 is NOT flush time.
// It is either the deferred-snapshot cost (cheap, should be ~ms) OR inline serial raster of tris that
// FAIL deferrable144 (drawTriangle on the single worker thread). This probe counts deferred vs inline
// tris, TIMES the inline raster, and records WHY each inline tri was not deferrable — so G157 knows
// whether the lever is "widen the defer eligibility → parallel 8-band replay" or "the sampler itself".
static const bool g_g156Stat = (std::getenv("DC2_G156_STAT") != nullptr);
static std::atomic<uint64_t> g_g156DeferredTri{0}; // tris deferred to g_g144List (parallel raster)
static std::atomic<uint64_t> g_g156InlineTri{0};   // tris rasterized inline on the calling thread
static std::atomic<uint64_t> g_g156InlineTriNs{0}; // wall-time of that inline drawTriangle
static std::atomic<uint64_t> g_g156CaptureNs{0};   // wall-time of the deferred snapshot+push_back body
static std::atomic<uint64_t> g_g156PrologueNs{0};  // wall-time from drawPrimitive entry to the capture
static std::atomic<uint64_t> g_g156NoTme{0};       // inline reason: untextured (tme=0)
static std::atomic<uint64_t> g_g156BadFbp{0};      // inline reason: fbp not 0x0/0x68 (RTT/other target)
static std::atomic<uint64_t> g_g156Diag{0};        // inline reason: a diagnostic trace forced eager
static void g156Report()
{
    std::fprintf(stderr,
                 "[G156:draw] deferTri=%llu prologueMs=%.1f captureMs=%.1f (%.2f us/tri) inlineTri=%llu inlineMs=%.1f (%.2f us/tri) | notme=%llu badfbp=%llu diag=%llu\n",
                 (unsigned long long)g_g156DeferredTri.load(std::memory_order_relaxed),
                 g_g156PrologueNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156CaptureNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156DeferredTri.load(std::memory_order_relaxed)
                     ? (double)g_g156CaptureNs.load(std::memory_order_relaxed) / 1e3 /
                           (double)g_g156DeferredTri.load(std::memory_order_relaxed)
                     : 0.0,
                 (unsigned long long)g_g156InlineTri.load(std::memory_order_relaxed),
                 g_g156InlineTriNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156InlineTri.load(std::memory_order_relaxed)
                     ? (double)g_g156InlineTriNs.load(std::memory_order_relaxed) / 1e3 /
                           (double)g_g156InlineTri.load(std::memory_order_relaxed)
                     : 0.0,
                 (unsigned long long)g_g156NoTme.load(std::memory_order_relaxed),
                 (unsigned long long)g_g156BadFbp.load(std::memory_order_relaxed),
                 (unsigned long long)g_g156Diag.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

struct G144BlockRange
{
    uint32_t first = 0;
    uint32_t last = 0;
    bool valid = false;
};

static bool g144PsmPageDims(uint8_t psm, uint32_t &pageW, uint32_t &pageH, bool &halfWidthBw)
{
    halfWidthBw = false;
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        pageW = 64u;
        pageH = 32u;
        return true;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        pageW = 64u;
        pageH = 64u;
        return true;
    case GS_PSM_T8:
        pageW = 128u;
        pageH = 64u;
        halfWidthBw = true;
        return true;
    case GS_PSM_T4:
        pageW = 128u;
        pageH = 128u;
        halfWidthBw = true;
        return true;
    default:
        return false;
    }
}

static G144BlockRange g144FullVramRange()
{
    return {0u, 0x3FFFu, true};
}

static G144BlockRange g144RangeForRect(uint32_t baseBlock,
                                       uint8_t psm,
                                       uint32_t bw,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t w,
                                       uint32_t h)
{
    if (w == 0u || h == 0u)
        return {};

    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    if (!g144PsmPageDims(psm, pageW, pageH, halfWidthBw))
        return g144FullVramRange();

    uint32_t pageBw = (bw != 0u) ? bw : 1u;
    if (halfWidthBw)
        pageBw = std::max<uint32_t>((pageBw + 1u) >> 1u, 1u);

    const uint64_t x0Page = static_cast<uint64_t>(x) / pageW;
    const uint64_t x1Page = (static_cast<uint64_t>(x) + static_cast<uint64_t>(w) - 1u) / pageW;
    const uint64_t y0Page = static_cast<uint64_t>(y) / pageH;
    const uint64_t y1Page = (static_cast<uint64_t>(y) + static_cast<uint64_t>(h) - 1u) / pageH;

    const uint64_t firstPage = y0Page * pageBw + x0Page;
    const uint64_t lastPage = y1Page * pageBw + x1Page;
    const uint64_t firstBlock64 = static_cast<uint64_t>(baseBlock) + firstPage * 32u;
    const uint64_t lastBlock64 = static_cast<uint64_t>(baseBlock) + lastPage * 32u + 31u;

    if (firstBlock64 > 0x3FFFu)
        return {};
    return {static_cast<uint32_t>(firstBlock64),
            static_cast<uint32_t>(std::min<uint64_t>(lastBlock64, 0x3FFFu)),
            true};
}

static bool g144RangeOverlaps(const G144BlockRange &a, const G144BlockRange &b)
{
    return a.valid && b.valid && a.first <= b.last && b.first <= a.last;
}

static bool g144IsPalettedPsm(uint8_t psm)
{
    return psm == GS_PSM_T8 || psm == GS_PSM_T8H ||
           psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

static G144BlockRange g144TextureRange(const G144Entry &e)
{
    const GSTex0Reg &tex = e.ctx.tex0;
    const uint32_t texW = 1u << std::min<uint8_t>(tex.tw, 15u);
    const uint32_t texH = 1u << std::min<uint8_t>(tex.th, 15u);
    return g144RangeForRect(tex.tbp0, tex.psm, tex.tbw, 0u, 0u, texW, texH);
}

static G144BlockRange g144ClutRange(const G144Entry &e)
{
    const GSTex0Reg &tex = e.ctx.tex0;
    if (!g144IsPalettedPsm(tex.psm))
        return {};

    const uint32_t clutBw = (e.texclut.cbw != 0u) ? static_cast<uint32_t>(e.texclut.cbw) : 1u;
    // The sampler may swizzle CSM1/CSA indices. Use the whole 16x16 CLUT tile so dirty-checking
    // cannot miss a palette write because of index remapping.
    return g144RangeForRect(tex.cbp, tex.cpsm, clutBw,
                            static_cast<uint32_t>(e.texclut.cou),
                            static_cast<uint32_t>(e.texclut.cov),
                            16u, 16u);
}

static G144BlockRange g144TargetRange(const G144Entry &e)
{
    const GSContext &ctx = e.ctx;
    const uint32_t baseBlock = framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t x0 = static_cast<uint32_t>(ctx.scissor.x0);
    const uint32_t y0 = static_cast<uint32_t>(ctx.scissor.y0);
    const uint32_t w = (ctx.scissor.x1 >= ctx.scissor.x0)
                           ? (static_cast<uint32_t>(ctx.scissor.x1 - ctx.scissor.x0) + 1u)
                           : 0u;
    const uint32_t h = (ctx.scissor.y1 >= ctx.scissor.y0)
                           ? (static_cast<uint32_t>(ctx.scissor.y1 - ctx.scissor.y0) + 1u)
                           : 0u;
    return g144RangeForRect(baseBlock, ctx.frame.psm, ctx.frame.fbw, x0, y0, w, h);
}

static bool g144PendingNeedsFlushForRanges(const G144BlockRange &src, const G144BlockRange &dst)
{
    if (g_g144List.empty())
        return false;

    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange target = g144TargetRange(e);
        if (g144RangeOverlaps(dst, target) || g144RangeOverlaps(src, target))
            return true;

        if (g144RangeOverlaps(dst, g144TextureRange(e)) ||
            g144RangeOverlaps(dst, g144ClutRange(e)))
        {
            return true;
        }
    }

    return false;
}

// G254 (DIAGNOSTIC ONLY, DEFAULT OFF): census candidate cross-target dependencies. Both behavior
// prototypes were rejected: triangle widening caused a temporal model dropout, while isolated
// predicted-safe FBP crossing was performance-neutral and produced large black MAP-0 regions.
// The range model is informative but not a permission oracle; all FBP drains remain.
enum G254Dependency : uint32_t
{
    G254_DEP_NONE = 0u,
    G254_DEP_UNKNOWN_RANGE = 1u << 0,
    G254_DEP_TARGET_ALIAS = 1u << 1,
    G254_DEP_EARLIER_WRITE_READ = 1u << 2,
    G254_DEP_LATER_WRITE_READ = 1u << 3,
    G254_DEP_DEPTH_ALIAS = 1u << 4,
    G254_DEP_LEGACY_Z = 1u << 5,
};

struct G254Surface
{
    uint32_t base = 0u;
    uint32_t bw = 0u;
    uint8_t psm = 0u;
    G144BlockRange range{};
    bool active = false;
    bool writable = false;
};

static G254Surface g254ColorSurface(const G144Entry &e)
{
    G254Surface s;
    s.base = framePageBaseToBlock(e.ctx.frame.fbp);
    s.bw = e.ctx.frame.fbw;
    s.psm = e.ctx.frame.psm;
    s.range = g144TargetRange(e);
    s.active = true;
    s.writable = e.ctx.frame.fbmsk != 0xFFFFFFFFu;
    return s;
}

static G254Surface g254DepthSurface(const G144Entry &e)
{
    G254Surface s;
    const uint32_t zte = static_cast<uint32_t>((e.ctx.test >> 16u) & 1u);
    const uint32_t ztst = static_cast<uint32_t>((e.ctx.test >> 17u) & 3u);
    const bool zmsk = ((e.ctx.zbuf >> 32u) & 1u) != 0u;
    if (zte == 0u)
        return s;
    s.base = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu) << 5u;
    s.bw = e.ctx.frame.fbw;
    s.psm = static_cast<uint8_t>((e.ctx.zbuf >> 24u) & 0xFu);
    const uint32_t x0 = static_cast<uint32_t>(e.ctx.scissor.x0);
    const uint32_t y0 = static_cast<uint32_t>(e.ctx.scissor.y0);
    const uint32_t w = (e.ctx.scissor.x1 >= e.ctx.scissor.x0)
                           ? static_cast<uint32_t>(e.ctx.scissor.x1 - e.ctx.scissor.x0) + 1u
                           : 0u;
    const uint32_t h = (e.ctx.scissor.y1 >= e.ctx.scissor.y0)
                           ? static_cast<uint32_t>(e.ctx.scissor.y1 - e.ctx.scissor.y0) + 1u
                           : 0u;
    s.range = g144RangeForRect(s.base, s.psm, s.bw, x0, y0, w, h);
    s.active = true;
    s.writable = !zmsk && ztst != 0u;
    return s;
}

// ============================================================================
// G307 — Milestone-2 Pillar-1 feasibility census (DEFAULT-OFF, DC2_G307_PIPE=1).
// Measures the CEILING of a 2-stage decode<->raster pipeline (Pillar-1) WITHOUT
// building it. The GS worker alternates DECODE (register replay + drawPrimitive
// build, between upload-edge flushes) and RASTER (the flush body: g178 GPU submit
// + consumer materialize + parallel band replay). A perfect Pillar-1 pipeline
// overlaps raster(N) with decode(N+1), hiding min(raster(N), decode(N+1)) of
// GS-worker time per boundary. Sum over the frame = the hideable time (the ceiling).
// Reported two ways:
//   idealHide - decode(N+1) is free to run fully concurrently with raster(N).
//   realHide  - zeroes any boundary where batch N+1's texture/CLUT INPUT overlaps
//               batch N's target/Z OUTPUT (the game samples what it just drew — an
//               RTT round-trip a simple command pipeline STILL serializes).
// Truth is between. A low idealHide refutes Pillar-1 outright. A high idealHide with
// a near-zero realHide means the RTT dependency (not the serial register replay) is
// the wall -> the answer is Pillar-3 residency, not a command-pipeline split.
// Single GS-worker thread (rasterizer:1549) -> plain statics; all clock work is
// gated on g307On() -> ZERO cost on the shipping default path.
// ============================================================================
static bool g307On() { static const bool v = envFlagEnabled("DC2_G307_PIPE"); return v; }

struct G307RangeSet
{
    G144BlockRange r[24];
    int n = 0;
    void add(const G144BlockRange &x)
    {
        if (!x.valid)
            return;
        for (int i = 0; i < n; ++i)
            if (r[i].first == x.first && r[i].last == x.last)
                return;
        if (n < 24)
            r[n++] = x;
    }
    bool overlapsAny(const G144BlockRange &x) const
    {
        if (!x.valid)
            return false;
        for (int i = 0; i < n; ++i)
            if (g144RangeOverlaps(r[i], x))
                return true;
        return false;
    }
};

// Distinct color-target + Z output block ranges of the batch currently in g_g144List.
static void g307CollectOutput(G307RangeSet &out)
{
    int cap = 0;
    for (const G144Entry &e : g_g144List)
    {
        out.add(g144TargetRange(e));
        const G254Surface z = g254DepthSurface(e);
        if (z.active)
            out.add(z.range);
        if (++cap >= 4096)
            break;
    }
}

// Does the batch currently in g_g144List SAMPLE any page in prevOut (an RTT round-trip
// that a naive command pipeline cannot overlap)?
static bool g307InputsHitPrevOut(const G307RangeSet &prevOut)
{
    if (prevOut.n == 0)
        return false;
    int cap = 0;
    for (const G144Entry &e : g_g144List)
    {
        if (prevOut.overlapsAny(g144TextureRange(e)) ||
            prevOut.overlapsAny(g144ClutRange(e)))
            return true;
        if (++cap >= 4096)
            break;
    }
    return false;
}

// Rolling census state — single GS-worker thread (rasterizer:1549) -> plain statics.
static G307RangeSet g_g307PrevOut;
static bool g_g307HavePrev = false;
static uint64_t g_g307PrevRasterNs = 0;
static std::chrono::steady_clock::time_point g_g307LastFlushEnd;
static uint64_t g_g307SumRasterNs = 0, g_g307SumDecodeNs = 0;
static uint64_t g_g307SumIdealNs = 0, g_g307SumRealNs = 0;
static uint64_t g_g307Flushes = 0, g_g307RttBoundaries = 0;
// Deep-buffer 2-stage pipeline SIMULATION clocks (decode thread ⇄ raster thread). Unlike the
// per-boundary min-model above (a 1-deep buffer), this lets decode run arbitrarily far ahead into
// a queue, BUT stalls the decode of batch N until raster(N-1) completes whenever batch N samples
// batch N-1's output (an RTT round-trip). g_g307SimRasDone at the end = the pipelined wall time;
// compare to (sumRaster+sumDecode) = the serial wall. This collapses the ideal/real bracket into
// one realistic ceiling that accounts for the RTT stalls draining the queue.
static uint64_t g_g307SimDecDone = 0, g_g307SimRasDone = 0;

// RAII bracket around ONE upload-edge flush body (the raster). ctor: close the boundary
// with the PREVIOUS batch (decode gap since last flush + RTT-hazard test), then snapshot
// THIS batch's output BEFORE the closure clears g_g144List. dtor: bank this flush's raster
// wall time and (every 600 flushes) print the aggregate.
struct G307FlushScope
{
    bool on;
    bool mHavePrev = false;   // this flush closes a boundary with a previous batch
    bool mRtt = false;        // this batch samples the previous batch's output (RTT round-trip)
    uint64_t mDecodeNs = 0;   // decode gap before this flush (register replay + draw build)
    std::chrono::steady_clock::time_point t0;
    // Guarded on a non-empty batch so a closure that early-returns (empty list / nothing to
    // raster) is not counted as a flush and does not perturb the boundary state.
    G307FlushScope() : on(g307On() && !g_g144List.empty())
    {
        if (!on)
            return;
        t0 = std::chrono::steady_clock::now();
        if (g_g307HavePrev)
        {
            mHavePrev = true;
            mDecodeNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t0 - g_g307LastFlushEnd)
                            .count();
            mRtt = g307InputsHitPrevOut(g_g307PrevOut);
            const uint64_t hide = std::min<uint64_t>(g_g307PrevRasterNs, mDecodeNs);
            g_g307SumDecodeNs += mDecodeNs;
            g_g307SumIdealNs += hide;
            if (mRtt)
                ++g_g307RttBoundaries;
            else
                g_g307SumRealNs += hide;
        }
        g_g307PrevOut = G307RangeSet{};
        g307CollectOutput(g_g307PrevOut);
        g_g307HavePrev = true;
    }
    ~G307FlushScope()
    {
        if (!on)
            return;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t rasterNs =
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        g_g307SumRasterNs += rasterNs;
        g_g307PrevRasterNs = rasterNs;
        g_g307LastFlushEnd = t1;

        // Deep-buffer pipeline sim step for batch N: decode runs ahead on its own thread, but an
        // RTT boundary blocks decode(N) until raster(N-1) is done; raster(N) needs raster(N-1) done
        // AND batch N decoded.
        uint64_t decStart = g_g307SimDecDone;
        if (mRtt)
            decStart = std::max(decStart, g_g307SimRasDone);
        g_g307SimDecDone = decStart + (mHavePrev ? mDecodeNs : 0u);
        const uint64_t rasStart = std::max(g_g307SimRasDone, g_g307SimDecDone);
        g_g307SimRasDone = rasStart + rasterNs;

        const uint64_t nf = ++g_g307Flushes;
        if ((nf % 600u) == 0u)
        {
            const uint64_t bnd = (nf > 1u) ? (nf - 1u) : 1u; // boundaries = flushes-1
            const double serialNs = (double)(g_g307SumRasterNs + g_g307SumDecodeNs);
            const double idealPct = serialNs > 0.0 ? 100.0 * (double)g_g307SumIdealNs / serialNs : 0.0;
            const double realPct = serialNs > 0.0 ? 100.0 * (double)g_g307SumRealNs / serialNs : 0.0;
            // Deep-buffer pipeline saving = 1 - pipelined_wall / serial_wall.
            const double simPct =
                serialNs > 0.0 ? 100.0 * (1.0 - (double)g_g307SimRasDone / serialNs) : 0.0;
            std::fprintf(stderr,
                         "[G307:pipe] flushes=%llu avgRaster=%.3fms avgDecode=%.3fms "
                         "idealHide=%.1f%% realHide=%.1f%% simPipeSave=%.1f%% rttBoundary=%.0f%% "
                         "(sumRaster=%.1fms sumDecode=%.1fms sumIdeal=%.1fms sumReal=%.1fms "
                         "simWall=%.1fms serialWall=%.1fms)\n",
                         (unsigned long long)nf, (double)g_g307SumRasterNs / 1e6 / (double)nf,
                         (double)g_g307SumDecodeNs / 1e6 / (double)bnd, idealPct, realPct, simPct,
                         100.0 * (double)g_g307RttBoundaries / (double)bnd,
                         (double)g_g307SumRasterNs / 1e6, (double)g_g307SumDecodeNs / 1e6,
                         (double)g_g307SumIdealNs / 1e6, (double)g_g307SumRealNs / 1e6,
                         (double)g_g307SimRasDone / 1e6, serialNs / 1e6);
        }
    }
};

static bool g254SameSurface(const G254Surface &a, const G254Surface &b)
{
    return a.active && b.active && a.base == b.base && a.bw == b.bw && a.psm == b.psm;
}

static bool g254KnownSurface(const G254Surface &s)
{
    if (!s.active)
        return true;
    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    return s.range.valid && g144PsmPageDims(s.psm, pageW, pageH, halfWidthBw);
}

static bool g254KnownTextureReads(const G144Entry &e,
                                  G144BlockRange &texture,
                                  G144BlockRange &clut)
{
    if (!e.prim.tme)
    {
        texture = {};
        clut = {};
        return true;
    }
    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    if (!g144PsmPageDims(e.ctx.tex0.psm, pageW, pageH, halfWidthBw))
        return false;
    texture = g144TextureRange(e);
    if (!texture.valid)
        return false;
    clut = g144ClutRange(e);
    if (g144IsPalettedPsm(e.ctx.tex0.psm) &&
        (!g144PsmPageDims(e.ctx.tex0.cpsm, pageW, pageH, halfWidthBw) || !clut.valid))
    {
        return false;
    }
    return true;
}

static uint32_t g254DependencyMask(const G144Entry &candidate)
{
    if (g_g144List.empty())
        return G254_DEP_NONE;
    if (!g203UniversalZEnabled())
        return G254_DEP_LEGACY_Z;

    const G254Surface nextColor = g254ColorSurface(candidate);
    const G254Surface nextDepth = g254DepthSurface(candidate);
    G144BlockRange nextTexture, nextClut;
    if (!g254KnownSurface(nextColor) || !g254KnownSurface(nextDepth) ||
        !g254KnownTextureReads(candidate, nextTexture, nextClut))
    {
        return G254_DEP_UNKNOWN_RANGE;
    }

    uint32_t mask = G254_DEP_NONE;
    for (const G144Entry &pending : g_g144List)
    {
        const G254Surface prevColor = g254ColorSurface(pending);
        const G254Surface prevDepth = g254DepthSurface(pending);
        G144BlockRange prevTexture, prevClut;
        if (!g254KnownSurface(prevColor) || !g254KnownSurface(prevDepth) ||
            !g254KnownTextureReads(pending, prevTexture, prevClut))
        {
            mask |= G254_DEP_UNKNOWN_RANGE;
            continue;
        }

        if ((prevColor.writable || nextColor.writable) &&
            g144RangeOverlaps(prevColor.range, nextColor.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevColor.writable || nextDepth.writable) &&
            g144RangeOverlaps(prevColor.range, nextDepth.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevDepth.writable || nextColor.writable) &&
            g144RangeOverlaps(prevDepth.range, nextColor.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevDepth.writable || nextDepth.writable) &&
            g144RangeOverlaps(prevDepth.range, nextDepth.range) &&
            !g254SameSurface(prevDepth, nextDepth))
        {
            mask |= G254_DEP_DEPTH_ALIAS;
        }
        if ((prevColor.writable &&
             (g144RangeOverlaps(prevColor.range, nextTexture) ||
              g144RangeOverlaps(prevColor.range, nextClut))) ||
            (prevDepth.writable &&
             (g144RangeOverlaps(prevDepth.range, nextTexture) ||
              g144RangeOverlaps(prevDepth.range, nextClut))))
        {
            mask |= G254_DEP_EARLIER_WRITE_READ;
        }
        if ((nextColor.writable &&
             (g144RangeOverlaps(nextColor.range, prevTexture) ||
              g144RangeOverlaps(nextColor.range, prevClut))) ||
            (nextDepth.writable &&
             (g144RangeOverlaps(nextDepth.range, prevTexture) ||
              g144RangeOverlaps(nextDepth.range, prevClut))))
        {
            mask |= G254_DEP_LATER_WRITE_READ;
        }
    }
    return mask;
}

static bool g254DependencyStatEnabled()
{
    static const bool s_on = envFlagEnabled("DC2_G254_DEP_STAT");
    return s_on;
}

static void g254NoteSwitch(uint32_t mask, uint32_t fromFbp, uint32_t toFbp, size_t pendingEntries)
{
    if (!g254DependencyStatEnabled())
        return;
    static std::atomic<uint64_t> s_seq{0};
    static std::atomic<uint64_t> s_safe{0};
    static std::atomic<uint64_t> s_reason[6]{};
    static std::atomic<uint64_t> s_entries{0};
    static std::atomic<uint64_t> s_transition[8][8]{};

    if (mask == G254_DEP_NONE)
        s_safe.fetch_add(1u, std::memory_order_relaxed);
    for (uint32_t bit = 0; bit < 6u; ++bit)
        if ((mask & (1u << bit)) != 0u)
            s_reason[bit].fetch_add(1u, std::memory_order_relaxed);
    s_entries.fetch_add(static_cast<uint64_t>(pendingEntries), std::memory_order_relaxed);
    s_transition[g253BarrierFbpBucket(fromFbp)][g253BarrierFbpBucket(toFbp)]
        .fetch_add(1u, std::memory_order_relaxed);

    const uint64_t seq = s_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((seq % 512u) != 0u)
        return;
    static constexpr const char *kReason[6] = {
        "unknown", "targetAlias", "earlierWriteRead", "laterWriteRead", "depthAlias", "legacyZ"
    };
    const uint64_t safe = s_safe.exchange(0u, std::memory_order_relaxed);
    const uint64_t entries = s_entries.exchange(0u, std::memory_order_relaxed);
    std::fprintf(stderr, "[G254:dep] n=%llu safe=%llu blocked=%llu entries=%llu avgEntries=%.2f\n",
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned long long>(safe),
                 static_cast<unsigned long long>(512u - safe),
                 static_cast<unsigned long long>(entries),
                 static_cast<double>(entries) / 512.0);
    for (uint32_t bit = 0; bit < 6u; ++bit)
    {
        const uint64_t count = s_reason[bit].exchange(0u, std::memory_order_relaxed);
        if (count != 0u)
            std::fprintf(stderr, "[G254:reason] %s=%llu\n", kReason[bit],
                         static_cast<unsigned long long>(count));
    }
    for (int from = 0; from < 8; ++from)
        for (int to = 0; to < 8; ++to)
        {
            const uint64_t count = s_transition[from][to].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G254:transition] from=%d to=%d count=%llu\n", from, to,
                             static_cast<unsigned long long>(count));
        }
}

static void g144DirtyStat(const char *kind, bool flushed)
{
    static const bool s_stat = (std::getenv("DC2_G145_DIRTY_STAT") != nullptr);
    if (!s_stat)
        return;

    static std::atomic<uint64_t> s_flush{0};
    static std::atomic<uint64_t> s_skip{0};
    const uint64_t f = flushed ? (s_flush.fetch_add(1u, std::memory_order_relaxed) + 1u)
                               : s_flush.load(std::memory_order_relaxed);
    const uint64_t s = flushed ? s_skip.load(std::memory_order_relaxed)
                               : (s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u);
    if ((flushed && f <= 32u) || (!flushed && s <= 32u) || (((f + s) & 0x3FFull) == 0ull))
    {
        std::fprintf(stderr, "[G145:dirty] kind=%s action=%s flush=%llu skip=%llu\n",
                     kind, flushed ? "flush" : "skip",
                     static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(s));
    }
}

// G166: extracted so BOTH the direct (single-texture) path and the batch-fallback path (any GPU
// batch failure) can share the exact same, already-proven CPU decode expression -- never
// duplicated inline. Identical math to the original inline T8 case (ReadP8 -> lookupCLUT).
static void g149DecodeT8Cpu(GSRasterizer *self, GS *gs, uint32_t tbp0, uint32_t pageBwT8,
                            uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t texPsm,
                            uint8_t *vram, int texW, int texH, uint32_t *outPx)
{
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, tbp0, pageBwT8,
                                    static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
            outPx[static_cast<size_t>(vv) * static_cast<size_t>(texW) + static_cast<size_t>(uu)] =
                self->lookupCLUT(gs, idx, cbp, cpsm, csm, csa, texPsm);
        }
}

// G166 (default OFF, needs DC2_G162_GPU_DECODE): a T8 cache-miss with GPU decode enabled defers
// its actual pixel fill to ONE per-frame batch (see g162ResolvePendingT8Batch below) instead of a
// synchronous per-texture GPU round-trip -- G164/G165 isolated cross-thread draw+readback
// synchronization, not buffer management, as the dominant remaining GPU-decode cost, and batching
// amortizes that synchronization over the whole frame's requests instead of paying it ~8x/frame.
// SAFE BY CONSTRUCTION: the placeholder `tex` is inserted into g_g149Map immediately (so later
// triangles in the same frame needing the SAME texture get a normal cache hit, unchanged), but its
// `px` is only filled at RESOLVE time -- which is guaranteed to run before any replay reads it,
// because it is the first statement of both G144 flush closures (g_g144FlushFn/g_g144FlushSeqFn),
// and replay of a frame's G144Entry list can only happen via one of those closures.
struct G162PendingT8
{
    uint32_t tbp0 = 0, pageBwT8 = 0;
    int texW = 0, texH = 0;
    uint32_t clut256[256]{};
    uint32_t cbp = 0; uint8_t cpsm = 0, csm = 0, csa = 0, texPsm = 0;
    GSRasterizer *self = nullptr; GS *gs = nullptr; uint8_t *vram = nullptr; // CPU-fallback params
    G149Buf tex;
};
static std::vector<G162PendingT8> g_g162Pending;

// G242's guest-depth ownership helpers are defined below the G178 front-end.  Keep them as a
// private .cpp-to-.cpp contract: CPU readers must materialize overlapping GPU-owned depth before
// they inspect VRAM bytes, while the default-off path is a single cached flag check.
void g242PrepareVramReadAll(uint8_t *vram);
void g242PrepareVramReadRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Resolves ALL pending T8 batch requests in one shot: tries the GPU batch first; on ANY failure
// (decoder not ready, batch too large for the atlas, etc) falls back to the proven CPU decode for
// every pending entry individually -- correctness never depends on the batch succeeding. Called
// from the START of both G144 flush closures (before they read any e.decoded), so this must
// complete before this function returns.
static void g162ResolvePendingT8Batch()
{
    if (g_g162Pending.empty())
        return;
    // Pending T8 requests dereference their captured VRAM pointer below (GPU upload or CPU
    // fallback).  A prior G242 batch may own aliased Z pages, so resolve that ownership first.
    g242PrepareVramReadAll(g_g162Pending[0].vram);
    extern bool g162DecodeT8Batch(int, const uint32_t *, const uint32_t *, const int *, const int *,
                                  const uint32_t *, const uint8_t *, size_t, uint32_t **);

    const int count = static_cast<int>(g_g162Pending.size());
    std::vector<uint32_t> tbp0Arr(count), tbwArr(count), clutFlat(static_cast<size_t>(count) * 256u);
    std::vector<int> texWArr(count), texHArr(count);
    std::vector<uint32_t *> outPxArr(count);
    for (int i = 0; i < count; ++i)
    {
        const G162PendingT8 &p = g_g162Pending[static_cast<size_t>(i)];
        tbp0Arr[i] = p.tbp0; tbwArr[i] = p.pageBwT8;
        texWArr[i] = p.texW; texHArr[i] = p.texH;
        std::memcpy(&clutFlat[static_cast<size_t>(i) * 256u], p.clut256, sizeof(p.clut256));
        outPxArr[i] = p.tex->px.data();
    }
    const bool ok = g162DecodeT8Batch(count, tbp0Arr.data(), tbwArr.data(), texWArr.data(),
                                      texHArr.data(), clutFlat.data(),
                                      g_g162Pending[0].vram, static_cast<size_t>(GSMem::MEMORY_SIZE),
                                      outPxArr.data());
    if (ok)
    {
        g_g162GpuHits.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
    }
    else
    {
        g_g162GpuFallback.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
        for (const G162PendingT8 &p : g_g162Pending)
            g149DecodeT8Cpu(p.self, p.gs, p.tbp0, p.pageBwT8, p.cbp, p.cpsm, p.csm, p.csa, p.texPsm,
                            p.vram, p.texW, p.texH, p.tex->px.data());
    }
    g_g162Pending.clear();
}

// G149: fetch (or ALLOCATE, lazily) the decoded-texture buffer for the texture bound in `ctx`, at
// DEFER time on the single guest thread. Returns null if the tri is not texcache-eligible (mirrors
// g141FastSampleTri's PSM/tbw gate), uses a REGION wrap mode, or exceeds the cap. The buffer is NOT
// decoded here — only allocated + zero-`valid`; texels are decoded lazily on first sample (see
// g141SamplePoint), so only sampled texels ever cost a swizzle+CLUT read. Memoized in g_g149Map keyed
// on descriptor + VRAM generation; the map is cleared when the generation advances (any VRAM upload).
// Guest-thread-only (defer is pre-replay) ⇒ no mutex. `self`/`vram`/`texa` are unused now (decode is
// deferred to sample time) but kept for signature stability.
static G149Buf g149BuildDecoded(GSRasterizer *self, GS *gs, const GSContext &ctx,
                                const GSTexaReg &texa, uint8_t *vram)
{
    (void)self; (void)gs; (void)vram;
    static const bool s_on = (std::getenv("DC2_G149_TEXCACHE") != nullptr);
    if (!s_on)
        return nullptr;
    static const size_t s_cap = []() {
        const char *e = std::getenv("DC2_G149_CAP");
        const size_t c = e ? static_cast<size_t>(std::strtoull(e, nullptr, 0)) : 262144u;
        return c ? c : 262144u;
    }();
    static const bool s_t8GsmemDefault =
        !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
    static const bool s_t8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);

    const uint8_t psm = ctx.tex0.psm;
    const bool fastPsm =
        psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S ||
        psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH ||
        (psm == GS_PSM_T8 && s_t8GsmemDefault && !s_t8AliasSet);
    if (!fastPsm || ctx.tex0.tbw == 0u)
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }
    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    if (!((wrapU == 0u || wrapU == 1u) && (wrapV == 0u || wrapV == 1u)))
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr; // REGION_CLAMP/REPEAT can index outside [0,texW): let the normal sampler run
    }
    const int texW = 1 << ctx.tex0.tw;
    const int texH = 1 << ctx.tex0.th;
    const size_t texels = static_cast<size_t>(texW) * static_cast<size_t>(texH);
    if (texels == 0 || texels > s_cap)
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }

    // G242: texture storage and CLUT storage can alias real guest Z (notably the 0x1a00 work
    // pages).  Materialize only on an actual overlap before even consulting the decoded cache;
    // the sync bumps g_g149VramGen, so an alias can never return a stale cached decode.
    g242PrepareVramReadRect(vram, ctx.tex0.tbp0, ctx.tex0.tbw, psm, 0u, 0u,
                            static_cast<uint32_t>(texW), static_cast<uint32_t>(texH));
    if (psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
        g242PrepareVramReadRect(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                0u, 0u, 64u, 32u);

    G149Key key;
    key.tbp0 = ctx.tex0.tbp0; key.tbw = ctx.tex0.tbw; key.cbp = ctx.tex0.cbp;
    key.tw = static_cast<uint32_t>(texW); key.th = static_cast<uint32_t>(texH);
    key.psm = psm; key.cpsm = ctx.tex0.cpsm; key.csm = ctx.tex0.csm; key.csa = ctx.tex0.csa;
    key.texa = (static_cast<uint32_t>(texa.aem ? 1u : 0u) << 16) |
               (static_cast<uint32_t>(texa.ta1) << 8) | static_cast<uint32_t>(texa.ta0);
    key.gen = g_g149VramGen.load(std::memory_order_relaxed);

    if (g_g149MapGen != key.gen) { g_g149Map.clear(); g_g149MapGen = key.gen; }
    auto it = g_g149Map.find(key);
    if (it != g_g149Map.end())
    {
        g_g149Hits.fetch_add(1u, std::memory_order_relaxed);
        return it->second;
    }

    // EAGER decode: fill the whole texture ONCE now (guest thread), so band-replay reads are pure
    // read-only linear lookups (no cross-thread writes → stable on the parallel path). Bit-exact:
    // filled with the SAME leaf expression g141SamplePoint uses (readTexel*/ReadP8 → lookupCLUT →
    // applyTexa) over uu,vv in [0,texW)x[0,texH). (A lazy per-texel fill would decode only sampled
    // texels but requires cross-band writes to the shared buffer, which proved unstable — see fix-log.)
    auto tex = std::make_shared<G149Tex>();
    tex->px.resize(texels);
    tex->valid.assign(texels, 1u); // eager: every texel already decoded
    const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;

    // G166 (default OFF, DC2_G162_GPU_DECODE=1, also needs DC2_G158_GPURASTER=1): defer this T8
    // texture's decode to the per-frame batch resolved at G144's flush point (see
    // g162ResolvePendingT8Batch above) instead of a synchronous per-texture GPU round-trip.
    // `pageBwT8` (not the raw ctx.tex0.tbw) is passed through unchanged -- same value ReadP8 uses
    // in the CPU fallback, so both paths address VRAM identically. `tex` is cached NOW (px filled
    // in later, before any replay can read it) so later triangles in this frame needing the same
    // texture get a normal cache hit. Bit-exactness verified by the EXISTING `DC2_G149_TCVERIFY`
    // per-pixel A/B (zero new verify code): if this path is wrong, TCVERIFY's compare shows it.
    if (psm == GS_PSM_T8)
    {
        static const bool s_gpuDecode = (std::getenv("DC2_G162_GPU_DECODE") != nullptr);
        if (s_gpuDecode)
        {
            G162PendingT8 pend;
            pend.tbp0 = ctx.tex0.tbp0; pend.pageBwT8 = pageBwT8;
            pend.texW = texW; pend.texH = texH;
            for (int i = 0; i < 256; ++i)
                pend.clut256[i] = self->lookupCLUT(gs, static_cast<uint8_t>(i), ctx.tex0.cbp, ctx.tex0.cpsm,
                                                   ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            pend.cbp = ctx.tex0.cbp; pend.cpsm = ctx.tex0.cpsm; pend.csm = ctx.tex0.csm;
            pend.csa = ctx.tex0.csa; pend.texPsm = ctx.tex0.psm;
            pend.self = self; pend.gs = gs; pend.vram = vram;
            pend.tex = tex;
            g_g162Pending.push_back(std::move(pend));
            g_g149Builds.fetch_add(1u, std::memory_order_relaxed);
            g_g149Map.emplace(key, tex);
            return tex; // px filled by g162ResolvePendingT8Batch() before replay reads it
        }
    }

    uint32_t *d = tex->px.data();
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            uint32_t c;
            switch (psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_CT24:
                c = applyTexa(texa, psm, self->readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
                c = applyTexa(texa, psm, self->readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_T8:
            {
                const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, ctx.tex0.tbp0, pageBwT8,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
                c = self->lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                break;
            }
            default: // T4 / T4HL / T4HH
            {
                const uint32_t idx = self->readTexelPSMT4(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv);
                c = self->lookupCLUT(gs, static_cast<uint8_t>(idx), ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                break;
            }
            }
            d[static_cast<size_t>(vv) * static_cast<size_t>(texW) + static_cast<size_t>(uu)] = c;
        }
    g_g149Builds.fetch_add(1u, std::memory_order_relaxed);
    g_g149Map.emplace(key, tex);
    return tex;
}

// ============================== G178: LLE GPU rasterizer FRONT-END ==============================
// (default OFF, DC2_G178_GPU=1; backend = ps2_gs_gpu_raster.cpp via ps2_gs_gpu_lle.h)
//
// Owns everything that needs this TU's proven GS semantics: per-entry eligibility validation,
// fixed-point→float vertex translation (exact drawTriangle/drawSprite conventions), eager
// whole-texture decode (the SAME leaf expressions as g149BuildDecoded), the per-page VRAM write
// generations that keep the backend's GPU-resident texture cache honest, and the swizzled VRAM
// read/write for framebuffer upload + readback (the SAME addrPSMCT32 calls writePixel uses).
// A flush either renders ENTIRELY on the GPU (then reads back into VRAM, so presentation/dumps/
// any downstream VRAM reader is unaffected) or falls back ENTIRELY to the proven CPU band replay
// — never mixed within one flush.
namespace
{
bool g178GpuOn()
{
    // G178 promoted to DEFAULT-ON (2026-07-10) — kill via DC2_G178_NO_GPU=1 or DC2_G178_GPU=0.
    static const bool on = !envFlagEnabled("DC2_G178_NO_GPU") && !envFlagDisabled("DC2_G178_GPU");
    return on;
}

// G249: G246's default-on promotion was retired after a multi-frame title test exposed repeated
// dirty-generation invariant failures: CPU VRAM writes can advance the guest Z-page generation
// while G242 still owns newer GPU depth. Keep the bridge available for explicit barrier work,
// but never enter an ownership model whose own invariant is already false on the title route.
// DC2_G242_GPU_DEPTH=1 opts in; DC2_G242_NO_GPU_DEPTH=1 remains the explicit kill switch.
bool g242GpuDepthOn()
{
    static const bool on = envFlagEnabled("DC2_G242_GPU_DEPTH") &&
                           !envFlagEnabled("DC2_G242_NO_GPU_DEPTH");
    return on;
}