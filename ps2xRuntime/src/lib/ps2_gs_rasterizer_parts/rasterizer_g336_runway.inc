// ===== G336 boundary-publication runway census (DIAGNOSIS ONLY, default OFF, DC2_G336_RUNWAY=1) =====
// The LAST premise gate of the native-renderer arc (plans/arc-native-renderer.md, G335 follow-up).
//
// PROPOSED MECHANISM (not built here): the frame-boundary closure's display-color readback
// (~10.4 ms/f backend, G332) publishes guest VRAM, then runs SERIAL before the next frame's
// front-end parse on the single GS worker. A boundary-overlap arm would run that readback
// CONCURRENTLY with the next frame's front-end parse (G305 drain machinery), draining (waiting for
// the readback) only at the FIRST next-frame consumer that touches the published state (existing
// G144 block-range conflict detection, fail-closed). Payoff = min(readback ~10.4 ms, RUNWAY).
//
// THIS CENSUS measures the RUNWAY: worker wall-time from the boundary closure's END (when the
// worker begins the next frame's front-end) to the FIRST next-frame captured draw whose read/write
// block-range overlaps the boundary's published set. Gate: runway >= ~8 ms funds the arm; a short
// runway (cf. G324's ~1.1 ms first-observer runway) means the arc is capped at ~13.5 fps.
//
// PUBLISHED SET = the display-color readback VRAM ranges (g276 publish site) captured during the
// boundary window — the dominant readback the mechanism targets. Color-only ⇒ the published set is
// a SUBSET of everything readback touches, so the measured runway is an UPPER BOUND (a Z-page
// overwrite next frame is not counted as a consumer). Therefore a SHORT color-only runway is a
// CONCLUSIVE no-go; only a long one would require adding the g278 depth set to tighten.
//
// No behavior change. All hooks are no-ops unless DC2_G336_RUNWAY=1. Reconciles against
// [G332:gsw]/[G316:worker] (same worker, same window). Per-draw consumer scan stops at the first
// hit each frame, so steady per-frame self-cost is bounded to the conflict-free prefix.

namespace
{
    bool g336RunwayOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G336_RUNWAY");
        return s_on;
    }

    // Published set + arming state (flush-owner / GS-worker thread only, same single-thread contract
    // as s_g276Pend and the g144 capture path — no cross-thread access).
    G260RangeSet g_g336PublishedSet;
    bool g_g336InBoundary = false; // capture window: true while the isFrameBoundary closure runs
    bool g_g336Armed = false;      // true after boundary end until the first next-frame consumer hit
    uint64_t g_g336PublishRanges = 0u;
    std::chrono::steady_clock::time_point g_g336BoundaryStartT{};
    std::chrono::steady_clock::time_point g_g336BoundaryEndT{};

    // Cumulative aggregates (windowed deltas printed every 30 boundaries).
    uint64_t g_g336Frames = 0u;         // boundaries observed
    uint64_t g_g336Hits = 0u;           // frames with a first-consumer hit (finite runway)
    uint64_t g_g336RunwaySumNs = 0u;    // Σ runway over hit frames
    uint64_t g_g336RunwayMinNs = ~0ull;
    uint64_t g_g336RunwayMaxNs = 0u;
    uint64_t g_g336NoConsumer = 0u;     // frames where no consumer hit before the next boundary (fully hideable)
    uint64_t g_g336EmptyPub = 0u;       // frames where the readback published nothing (no constraint)
    uint64_t g_g336BoundarySumNs = 0u;  // Σ boundary-closure duration (incl. readback)
    uint64_t g_g336PubRangeSum = 0u;    // Σ published ranges captured

    // Capture one successful display-color readback publication into the current boundary's set.
    // Called from the g276 display readback publish site.
    void g336NotePublish(uint32_t baseBlock, uint32_t bw, int fbW, int rowLo, int rowHi)
    {
        if (!g336RunwayOn() || !g_g336InBoundary || fbW <= 0 || rowHi < rowLo)
            return;
        const G144BlockRange rg = g144RangeForRect(baseBlock, GS_PSM_CT32, bw, 0u,
                                                   static_cast<uint32_t>(rowLo),
                                                   static_cast<uint32_t>(fbW),
                                                   static_cast<uint32_t>(rowHi - rowLo + 1));
        if (rg.valid)
        {
            g_g336PublishedSet.add(rg);
            ++g_g336PublishRanges;
        }
        else
        {
            g_g336PublishedSet.markUnknown(); // fail-closed: unresolved publish poisons the set
        }
    }

    // Same, for the g278 depth (Z24) readback publication — completes the boundary published set so
    // a Z-only boundary lump cannot be missed.
    void g336NotePublishZ(uint32_t zbpBlock, uint32_t bw, int fbW, int rowLo, int rowHi)
    {
        if (!g336RunwayOn() || !g_g336InBoundary || fbW <= 0 || rowHi < rowLo)
            return;
        const G144BlockRange rg = g144RangeForRect(zbpBlock, GS_PSM_Z24, bw, 0u,
                                                   static_cast<uint32_t>(rowLo),
                                                   static_cast<uint32_t>(fbW),
                                                   static_cast<uint32_t>(rowHi - rowLo + 1));
        if (rg.valid)
        {
            g_g336PublishedSet.add(rg);
            ++g_g336PublishRanges;
        }
        else
        {
            g_g336PublishedSet.markUnknown();
        }
    }

    // Per captured draw-entry on the NEXT frame; records the runway at the first overlapping entry.
    // Read/write classification mirrors g260NoteAppend (color write, Z write, tex/CLUT read).
    void g336NoteConsumer(const G144Entry &e)
    {
        if (!g_g336Armed) // fast path when disabled or already found this frame
            return;
        bool hit = false;
        const G144BlockRange color = g144TargetRange(e);
        if (g_g336PublishedSet.overlaps(color)) // overlaps() returns true when the set is unknown
            hit = true;
        if (!hit)
        {
            const G254Surface depth = g254DepthSurface(e);
            if (depth.active && g_g336PublishedSet.overlaps(depth.range))
                hit = true;
        }
        if (!hit && e.prim.tme)
        {
            if (g_g336PublishedSet.overlaps(g144TextureRange(e)))
                hit = true;
            else if (g144IsPalettedPsm(e.ctx.tex0.psm) &&
                     g_g336PublishedSet.overlaps(g144ClutRange(e)))
                hit = true;
        }
        if (!hit)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g_g336BoundaryEndT).count());
        ++g_g336Hits;
        g_g336RunwaySumNs += ns;
        if (ns < g_g336RunwayMinNs) g_g336RunwayMinNs = ns;
        if (ns > g_g336RunwayMaxNs) g_g336RunwayMaxNs = ns;
        g_g336Armed = false; // stop scanning until the next boundary re-arms
    }

    void g336Report()
    {
        static uint64_t s_frames = 0, s_hits = 0, s_runwaySum = 0, s_noCons = 0,
                        s_emptyPub = 0, s_boundarySum = 0, s_pubRangeSum = 0;
        const uint64_t dFrames = g_g336Frames - s_frames;
        if (dFrames == 0u)
            return;
        const uint64_t dHits = g_g336Hits - s_hits;
        const uint64_t dRunway = g_g336RunwaySumNs - s_runwaySum;
        const uint64_t dNoCons = g_g336NoConsumer - s_noCons;
        const uint64_t dEmpty = g_g336EmptyPub - s_emptyPub;
        const uint64_t dBoundary = g_g336BoundarySumNs - s_boundarySum;
        const uint64_t dPubRange = g_g336PubRangeSum - s_pubRangeSum;
        const double avgRunwayMs = dHits ? (double)dRunway / dHits / 1e6 : 0.0;
        std::fprintf(stderr,
            "[G336:runway] frames=%llu | hits=%llu avgRunway=%.2fms min=%.2f max=%.2f | "
            "noConsumer=%llu emptyPub=%llu | avgBoundary=%.2fms pubRanges/f=%.2f\n",
            (unsigned long long)dFrames, (unsigned long long)dHits, avgRunwayMs,
            g_g336RunwayMinNs == ~0ull ? 0.0 : (double)g_g336RunwayMinNs / 1e6,
            (double)g_g336RunwayMaxNs / 1e6,
            (unsigned long long)dNoCons, (unsigned long long)dEmpty,
            (double)dBoundary / dFrames / 1e6, (double)dPubRange / dFrames);
        s_frames = g_g336Frames; s_hits = g_g336Hits; s_runwaySum = g_g336RunwaySumNs;
        s_noCons = g_g336NoConsumer; s_emptyPub = g_g336EmptyPub;
        s_boundarySum = g_g336BoundarySumNs; s_pubRangeSum = g_g336PubRangeSum;
        g_g336RunwayMinNs = ~0ull;
        g_g336RunwayMaxNs = 0u;
    }
} // g336 nested anonymous namespace

// This .inc is included INSIDE the file-level anonymous namespace that
// rasterizer_headers_and_diagnostics.inc opens and never closes, so a plain definition here would
// get INTERNAL linkage. The two boundary hooks are called from ps2_gif_arbiter.cpp (another TU) and
// must be GLOBAL. Temporarily close the outer anonymous namespace, define them at true global scope
// (they can still name the anon-namespace state above — anonymous-namespace members are visible in
// the enclosing global namespace), then REOPEN it so the remaining rasterizer includes stay balanced.
} // close the file-level anonymous namespace (see note above)

// GLOBAL scope (external linkage): called by the GS worker's isFrameBoundary branch in
// ps2_gif_arbiter.cpp — forward-declared there. Bracket the boundary closure so the census knows
// the capture window and the moment next-frame front-end begins.
void g336_boundary_begin()
{
    if (!g336RunwayOn())
        return;
    // If the prior frame's next-frame parse produced NO consumer before this boundary, the whole
    // frame was conflict-free ⇒ the readback was fully hideable.
    if (g_g336Armed)
    {
        ++g_g336NoConsumer;
        g_g336Armed = false;
    }
    g_g336PublishedSet.reset();
    g_g336PublishRanges = 0u;
    g_g336InBoundary = true;
    g_g336BoundaryStartT = std::chrono::steady_clock::now();
}

void g336_boundary_end()
{
    if (!g336RunwayOn())
        return;
    g_g336InBoundary = false;
    const auto now = std::chrono::steady_clock::now();
    g_g336BoundarySumNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_g336BoundaryStartT).count());
    g_g336BoundaryEndT = now;
    g_g336PubRangeSum += g_g336PublishRanges;
    ++g_g336Frames;
    if (g_g336PublishRanges == 0u && !g_g336PublishedSet.unknown)
    {
        ++g_g336EmptyPub; // readback published nothing this frame ⇒ no runway constraint
        g_g336Armed = false;
    }
    else
    {
        g_g336Armed = true; // arm the next frame's capture scan for the first consumer
    }
    if ((g_g336Frames % 30u) == 0u)
        g336Report();
}

// Reopen the file-level anonymous namespace closed above, so the remaining rasterizer_*.inc
// includes see exactly the same scope depth they did before this file.
namespace
{
