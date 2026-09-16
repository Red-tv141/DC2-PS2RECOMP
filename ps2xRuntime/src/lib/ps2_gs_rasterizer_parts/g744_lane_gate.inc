// ===== G744: ONE PIXEL GATE FOR EVERY REMAINING GSRowPool FAN-OUT ==============================
//
// WHY. G419 established, with a measured table, that **a ROW count cannot express the work** a
// row-pool fan-out is being asked to do:
//
//     target      geometry    8 lanes   1 lane
//     0x139/w8    448x512     0.102     0.152   <- fan-out pays
//     0x139/w2    128x128     0.083     0.012   <- fan-out costs 7x the whole loop
//     0x143/w1     64x64      0.079     0.004
//
// `GSRowPool` dispatch is a fixed ~0.085 ms (8 lane wake-ups plus a join), so an 8-lane split only
// pays above ~154k pixels of serial work; G419 promoted `g419PackLanes` with the admission
// threshold at 65,536 pixels, safely inside the gap of DC2's bimodal population. G743 SS3 found the
// readback UNPACK -- the other half of the same round trip, on the same thread -- still gated on
// `rows >= 8` and fixed it the same way.
//
// ⛔ THE DEFECT SURVIVES AT EVERY REMAINING SITE. A whole-tree enumeration of `GSRowPool::run`
// admissions on the shipping path leaves four that never got G419's law:
//
//     rasterizer_gpu_alias_page_view.inc   unpackDepthRows   rows      >= 8
//     rasterizer_vram_materialization.inc  packDepthRows     depthRows >= 8
//     rasterizer_vram_materialization.inc  unpackDepthRows   rbRows    >= 8
//     rasterizer_command_graph.inc         joinedDecode      texH >= 8 && pixels >= 4096
//
// At the shipped 512-pixel frame width, `rows >= 8` admits at **4,096 pixels -- 16x below G419's
// own threshold** -- so every narrow Z window is fanned out 8 ways to do less work than the fan-out
// costs. The texture decode reaches the same 4,096 by an explicit constant instead of by accident.
//
// ⭐ WHY IT MATTERS MORE NOW THAN WHEN G419 WAS WRITTEN. G721 moved the flush onto the G713
// executor, and G743 SS2.1's PC sample of that thread measures `GSRowPool::run`'s wake/join
// syscalls at **6.07 % of every executor sample** -- on the thread the frame is serialised on.
// G498 measured the direction from the other side: its 8-lane split of the owner-publish unpack
// read **+0.023 ms/f (null)** while the identical mechanism at `lanes = 1` read **-0.213 ms/f,
// 4/4 blocks favourable**. G743 SS6.1 forced all five sites serial at once and got 2/2 negative
// (-0.135, -0.366) but with 1.545 ms/f of control drift, so Rule 16 refused it: the direction is
// credible and the BLANKET form is unmeasurable. This is the targeted form.
//
// EXACT BY CONSTRUCTION, and not merely by argument. `lanes <= 1` runs the caller's closure over
// the whole row span on the calling thread -- which is already the path every one of these sites
// takes whenever its row test fails. Disjoint guest rows own disjoint VRAM words (G298's
// contract), so the lane count cannot change a single stored byte; it only chooses how many
// threads store them. Nothing new is reachable. An existing path is chosen more often.
//
// ⚠️ THE THRESHOLD IS PER-SITE BECAUSE ns/PIXEL IS PER-SITE. Break-even pixels = dispatch cost /
// (ns per pixel), and a Z swizzle (swizzled address + 24-bit pack) is not a texture decode (CLUT
// indirection + format expand). Both default to G419's measured 65,536 -- the only dispatch-cost
// constant this project has ever measured -- and both are env-tunable so the knee can be swept
// without a relink. `PS2X_G744_LANECENSUS` compiles the population census that sizes them.
//
// ⛔ THE CENSUS IS COMPILE-GATED, NOT RUNTIME-GATED, AND THAT IS DELIBERATE. FINAL SS7: G710's
// probe was `#if`-excluded with `constexpr` stubs, every one of its 78 use sites folded, and it
// still measured **+0.362 ms/f pooled (t = 4.48)** in this exact TU. Keep diagnostics out of a hot
// TU by compile-time exclusion, never by a runtime flag.
//
// Levers:
//   DC2_G744_NO_LANE_PXGATE=1   rollback -- restores the pre-G744 row-only admission exactly
//   DC2_G744_ZLANE_MIN_PX=<n>   Z swizzle threshold in pixels (default 65536)
//   DC2_G744_TEXLANE_MIN_PX=<n> texture decode threshold in pixels (default 65536)
//   DC2_G744_LANE_STAT=1        print [G744:lane] (requires -DPS2X_G744_LANECENSUS=ON)

static bool g744LanePxGateOn()
{
    static const bool on = !envFlagEnabled("DC2_G744_NO_LANE_PXGATE");
    return on;
}

static uint64_t g744EnvPixels(const char *name, uint64_t fallback)
{
    const char *const value = std::getenv(name);
    if (value == nullptr)
        return fallback;
    const long long v = std::atoll(value);
    return v <= 0 ? 0ull : static_cast<uint64_t>(v);
}

static uint64_t g744ZLaneMinPixels()
{
    static const uint64_t n = g744EnvPixels("DC2_G744_ZLANE_MIN_PX", 65536ull);
    return n;
}

// ⛔ 4,096, NOT G419's 65,536, AND THAT IS A DELIBERATE NON-CHANGE. The pre-existing predicate at
// the decode site is `texH >= 8 && pixels >= 4096`, so this default makes the new pixel test
// unreachable and the site behaves EXACTLY as it did before G744. Two reasons, both measured:
//
//   1. G419's 65,536 is a PACK-LOOP constant (~0.55 ns/px). Break-even = dispatch cost / ns-per-px,
//      and a CLUT-indirect decode is not a linear staging copy. G529 measured the other heavy
//      per-pixel site at **137 ns/px**, where the same 8-lane split breaks even at ~620 px and
//      forcing serial costs **3.9x**. Transplanting 65,536 here is not supported by any
//      measurement of THIS loop. (Rule G744-3b.)
//   2. G744's own screen could not resolve the difference: two behaviourally identical arms in it
//      disagreed by **0.573 ms/f, 8x the control drift** (see plans/phase-G744-plan.md §2.4), so
//      the 65,536 arm's apparent -0.075 against the 4,096 arm is inside that noise.
//
// ⇒ The gate ships as an EXPLICIT, TUNABLE, PIXEL-DENOMINATED predicate replacing an accidental
// one, with NO CLAIMED VALUE. Raise this knob only behind a gate that can actually resolve it.
static uint64_t g744TexLaneMinPixels()
{
    static const uint64_t n = g744EnvPixels("DC2_G744_TEXLANE_MIN_PX", 4096ull);
    return n;
}

// The census sites, named so a row can be read back to the line that produced it.
enum : unsigned
{
    kG744SiteZUnpackView = 0u,  // rasterizer_gpu_alias_page_view.inc  unpackDepthRows
    kG744SiteZPack = 1u,        // rasterizer_vram_materialization.inc packDepthRows
    kG744SiteZUnpackRb = 2u,    // rasterizer_vram_materialization.inc unpackDepthRows
    kG744SiteTexDecode = 3u,    // rasterizer_command_graph.inc        joinedDecode
    kG744SiteCount = 4u
};

#if defined(PS2X_G744_LANECENSUS)
static bool g744LaneStatOn()
{
    static const bool on = envFlagEnabled("DC2_G744_LANE_STAT");
    return on;
}

static const char *const kG744SiteName[kG744SiteCount] = {"zUnpackView", "zPack", "zUnpackRb",
                                                          "texDecode"};
static std::atomic<uint64_t> g_g744Calls[kG744SiteCount];
static std::atomic<uint64_t> g_g744RowAdmit[kG744SiteCount];
static std::atomic<uint64_t> g_g744PxRefused[kG744SiteCount];
static std::atomic<uint64_t> g_g744Dispatched[kG744SiteCount];
static std::atomic<uint64_t> g_g744Pixels[kG744SiteCount];
static std::atomic<uint64_t> g_g744RefusedPixels[kG744SiteCount];
static std::atomic<uint64_t> g_g744Reports{0};

// Print the split BEFORE and AFTER the pixel test, so "the row gate admitted a dispatch the pixel
// gate then refused" is a printed fact rather than an inference -- the separation G741 SS1 and
// G736 SS2 were both written about. `meanRefusedPx` is what sizes the threshold: it is the mean
// serial work the fan-out was being paid ~0.085 ms to split.
static void g744LaneReport()
{
    const uint64_t n = g_g744Reports.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n != 1ull && (n % 2048ull) != 0ull)
        return;
    for (unsigned s = 0u; s < kG744SiteCount; ++s)
    {
        const uint64_t calls = g_g744Calls[s].load(std::memory_order_relaxed);
        if (calls == 0ull)
            continue;
        const uint64_t rowAdmit = g_g744RowAdmit[s].load(std::memory_order_relaxed);
        const uint64_t refused = g_g744PxRefused[s].load(std::memory_order_relaxed);
        const uint64_t disp = g_g744Dispatched[s].load(std::memory_order_relaxed);
        const uint64_t px = g_g744Pixels[s].load(std::memory_order_relaxed);
        const uint64_t rpx = g_g744RefusedPixels[s].load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G744:lane] site=%s calls=%llu rowAdmit=%llu pxRefused=%llu dispatched=%llu "
                     "meanPx=%.0f meanRefusedPx=%.0f gate=%s\n",
                     kG744SiteName[s], static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(rowAdmit),
                     static_cast<unsigned long long>(refused),
                     static_cast<unsigned long long>(disp),
                     static_cast<double>(px) / static_cast<double>(calls),
                     refused ? static_cast<double>(rpx) / static_cast<double>(refused) : 0.0,
                     g744LanePxGateOn() ? "on" : "OFF");
    }
}

static void g744LaneNote(unsigned site, uint64_t pixels, bool rowAdmit, bool dispatched)
{
    if (!g744LaneStatOn() || site >= kG744SiteCount)
        return;
    g_g744Calls[site].fetch_add(1ull, std::memory_order_relaxed);
    g_g744Pixels[site].fetch_add(pixels, std::memory_order_relaxed);
    if (rowAdmit)
        g_g744RowAdmit[site].fetch_add(1ull, std::memory_order_relaxed);
    if (rowAdmit && !dispatched)
    {
        g_g744PxRefused[site].fetch_add(1ull, std::memory_order_relaxed);
        g_g744RefusedPixels[site].fetch_add(pixels, std::memory_order_relaxed);
    }
    if (dispatched)
        g_g744Dispatched[site].fetch_add(1ull, std::memory_order_relaxed);
    g744LaneReport();
}
#else
static inline void g744LaneNote(unsigned, uint64_t, bool, bool) {}
#endif

// The gate itself. `rowAdmitLanes` is whatever the site's existing row test already decided, so a
// site that was going to run serial anyway stays serial and is still counted. Returns the lane
// count to pass to `GSRowPool::run`.
static int g744LaneGate(unsigned site, uint64_t pixels, int rowAdmitLanes, uint64_t minPixels)
{
    const bool rowAdmit = rowAdmitLanes > 1;
    int lanes = rowAdmitLanes;
    if (rowAdmit && g744LanePxGateOn() && pixels < minPixels)
        lanes = 1;
    g744LaneNote(site, pixels, rowAdmit, lanes > 1);
    return lanes;
}
