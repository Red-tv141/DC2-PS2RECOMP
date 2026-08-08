// ===== G530: the replayed SPRITE PIXEL, decomposed and then rewritten as a SPAN =====
// revision: 1
//
// WHERE THIS PHASE STARTS.  G529 closed with a FLAT ranking (largest attributable item 16.6% of a
// 34.4 ms/f Georama frame) and one surviving lead: `[G529:spr]` prices the replayed sprite pixel at
// **397.8 ns (~1,250 host cycles for ONE pixel)**, 86% of all replay CPU, 100% bilinear.  Two
// mechanisms were already refuted there — the arithmetic (`DC2_G529_NOLERP` deletes the entire
// bilinear lerp: ceiling 0.00 ms/f) and the dispatch (`rt` 0.41 of 4.83; `lanes=1` is 3.91x worse).
// G529's remaining hypothesis was "every per-pixel VRAM touch is a scattered uncached walk through
// the PS2 swizzle".
//
// ⚠️ THAT HYPOTHESIS IS PARTLY WRONG ON ITS FACE, AND THE ADDRESS TABLE SAYS SO.
// `GSPSMCT32::addrPSMCT32` puts eight consecutive pixels of one scanline at word offsets
// {0,1,4,5,8,9,12,13} (even rows) / {2,3,6,7,10,11,14,15} (odd rows) inside a 256-byte-ALIGNED
// block — i.e. bytes 0..63, ONE cache line.  A bilinear quad's four taps (u0,u1 adjacent, v0,v1
// adjacent) land in the same line too.  So the swizzle is not scattering the accesses; what it is
// doing is forcing a full page/block/column ADDRESS SYNTHESIS per access, ~6 times per pixel, plus
// everything else the generic per-pixel path re-derives.
//
// ⛔⛔ SO: SPLIT THE BUCKET BEFORE DESIGNING (g528_split_the_bucket_before_designing), with CEILING
// PROBES on the frame — the only instrument G529 found honest at this magnitude.  Each probe below
// deletes exactly one class of per-pixel work from the textured-sprite loop and is measured as a
// paired frame delta.  ALL are DEFAULT-OFF and produce KNOWINGLY INCORRECT PIXELS; none may ever be
// promoted.  They exist so the lever is priced before it is built.
//
//   DC2_G530_NOZ=1     delete spriteZPass' Z read AND spriteZStore's Z write   (2 VRAM touches/px)
//   DC2_G530_NOWR=1    delete the writePixel call                             (dest addr + RMW + calls)
//   DC2_G530_NOUV=1    hoist the per-pixel UV interpolation to the row         (1 divide + 2 lround/px)
//   DC2_G530_NOTEX=1   replace the sampler with a constant texel               (4 taps + lerp + wrap)
//   DC2_G530_NOCOMB=1  delete combineTexture + classifyAlphaTest               (per-pixel switch pair)
//
// The probes are read ONCE per drawSprite into plain `const bool`s hoisted above the loop nest, so
// the default binary's inner loop is unchanged apart from five loop-invariant branches that MSVC
// hoists (verified by the frame gate: the probe build must reproduce the baseline with no flag set).

namespace
{

// ---- G530 decomposition probes (default-off, knowingly wrong output, never promote) ----
inline bool g530ProbeNoZ()    { static const bool v = envFlagEnabled("DC2_G530_NOZ");    return v; }
inline bool g530ProbeNoWr()   { static const bool v = envFlagEnabled("DC2_G530_NOWR");   return v; }
inline bool g530ProbeNoUv()   { static const bool v = envFlagEnabled("DC2_G530_NOUV");   return v; }
inline bool g530ProbeNoTex()  { static const bool v = envFlagEnabled("DC2_G530_NOTEX");  return v; }
inline bool g530ProbeNoComb() { static const bool v = envFlagEnabled("DC2_G530_NOCOMB"); return v; }
// ⛔ The WHOLE-BODY ceiling: skip every per-pixel statement of the textured sprite loop. Bounds
// everything the five probes above can ever sum to, and — crucially — separates "the pixels are
// slow" from "the per-CALL path around the pixels is slow". If this reads ~0 the loop is not the
// subject at all and no per-pixel lever can pay (g511_probe_bounds_the_gate).
inline bool g530ProbeNoPx()   { static const bool v = envFlagEnabled("DC2_G530_NOPX");   return v; }

// True when ANY G530 probe is armed — used to keep the probes and the shipped span kernel from
// ever running at the same time (a probe deletes work the kernel is supposed to do exactly).
inline bool g530AnyProbe()
{
    static const bool v = g530ProbeNoZ() || g530ProbeNoWr() || g530ProbeNoUv() ||
                          g530ProbeNoTex() || g530ProbeNoComb() || g530ProbeNoPx();
    return v;
}

// =========== [G530:cyc] — IS THE 429 us DRAWSPRITE CALL CPU, AND IS IT THE PIXELS? ===========
//
// ⚠️⚠️ WHAT G529'S HEADLINE NUMBER ACTUALLY MEASURES. `[G529:spr] ns/px=397.8` is
// `G529SprScope`'s bracket, and that scope is constructed at the TOP of drawSprite and destroyed at
// the END — it is the WHOLE CALL, prologue included, measured with steady_clock, i.e. WALL time on
// a lane. Dividing it by the call's pixel count and calling the quotient "the cost of a pixel"
// silently assumes (a) the prologue is negligible and (b) the lane was running. Neither is
// established, and `144,387 ms / 336,642 calls` says each call is **429 us for ~1,078 pixels**,
// which no per-pixel code path in this rasterizer can produce.
//
// This census settles it with the two measurements that discriminate (G524's conversion law and
// G526's `cyc/ns` rule — "do not read a flat profile as blocked, check retired cycles"):
//
//   cyc/ns   near the core clock (~3.15 here) = the lane really was executing; near 0 = parked
//   pro/loop the SAME bracket split at the entry of the pixel loop nest
//
// DC2_G530_CYC=1, default-off; QueryThreadCycleTime is ~50 ns so it must never ship armed.
// (QueryThreadCycleTime / GetCurrentThread are already declared at GLOBAL scope by
// g526_draw_census.inc, which this TU includes first — do not re-declare them here: inside an
// anonymous namespace they would be a different, internally-linked entity.)

inline bool g530CycOn()
{
    static const bool v = envFlagEnabled("DC2_G530_CYC");
    return v;
}

inline uint64_t g530Cycles()
{
    unsigned long long c = 0ull;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return static_cast<uint64_t>(c);
}

enum { kG530CycFast = 0, kG530CycOther = 1, kG530CycBuckets = 2 };
std::atomic<uint64_t> g_g530CycN[kG530CycBuckets];
std::atomic<uint64_t> g_g530CycPx[kG530CycBuckets];
std::atomic<uint64_t> g_g530CycProNs[kG530CycBuckets];
std::atomic<uint64_t> g_g530CycLoopNs[kG530CycBuckets];
std::atomic<uint64_t> g_g530CycProCyc[kG530CycBuckets];
std::atomic<uint64_t> g_g530CycLoopCyc[kG530CycBuckets];

inline void g530CycReport()
{
    static std::atomic<uint64_t> s_next{200000};
    const uint64_t n = g_g530CycN[kG530CycFast].load(std::memory_order_relaxed) +
                       g_g530CycN[kG530CycOther].load(std::memory_order_relaxed);
    uint64_t want = s_next.load(std::memory_order_relaxed);
    if (n < want || !s_next.compare_exchange_strong(want, want + 200000u))
        return;
    static const char *kName[kG530CycBuckets] = {"fast", "other"};
    for (int i = 0; i < kG530CycBuckets; ++i)
    {
        const double cn = static_cast<double>(g_g530CycN[i].load(std::memory_order_relaxed));
        if (cn <= 0.0)
            continue;
        const double px = static_cast<double>(g_g530CycPx[i].load(std::memory_order_relaxed));
        const double pns = static_cast<double>(g_g530CycProNs[i].load(std::memory_order_relaxed));
        const double lns = static_cast<double>(g_g530CycLoopNs[i].load(std::memory_order_relaxed));
        const double pcy = static_cast<double>(g_g530CycProCyc[i].load(std::memory_order_relaxed));
        const double lcy = static_cast<double>(g_g530CycLoopCyc[i].load(std::memory_order_relaxed));
        std::fprintf(stderr,
            "[G530:cyc] %-5s n=%.0f px=%.0f pxPerCall=%.1f | PRO ms=%.1f us/call=%.2f cyc/ns=%.2f"
            " | LOOP ms=%.1f us/call=%.2f cyc/ns=%.2f ns/px=%.1f cyc/px=%.1f | pro%%=%.1f\n",
            kName[i], cn, px, px / cn,
            pns / 1e6, pns / cn / 1e3, (pns > 0.0) ? pcy / pns : 0.0,
            lns / 1e6, lns / cn / 1e3, (lns > 0.0) ? lcy / lns : 0.0,
            (px > 0.0) ? lns / px : 0.0, (px > 0.0) ? lcy / px : 0.0,
            100.0 * pns / (pns + lns + 1e-9));
    }
    std::fflush(stderr);
}

// Only replayed calls are booked — that is the population `replay` is made of.
struct G530CycScope
{
    bool on;
    int bucket;
    uint64_t px;
    std::chrono::steady_clock::time_point t0, tMark;
    uint64_t c0, cMark;
    bool marked;
    G530CycScope()
        : on(g530CycOn() && t_g144InReplay), bucket(kG530CycOther), px(0u), c0(0u), cMark(0u),
          marked(false)
    {
        if (on)
        {
            t0 = std::chrono::steady_clock::now();
            c0 = g530Cycles();
            tMark = t0;
            cMark = c0;
        }
    }
    void setFast(bool fast) { if (on) bucket = fast ? kG530CycFast : kG530CycOther; }
    void setPx(uint64_t p) { if (on) px = p; }
    // Called at the entry of the pixel loop nest: everything before is the PROLOGUE.
    void markLoop()
    {
        if (!on || marked)
            return;
        marked = true;
        tMark = std::chrono::steady_clock::now();
        cMark = g530Cycles();
        g_g530CycProNs[bucket].fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tMark - t0).count()),
            std::memory_order_relaxed);
        g_g530CycProCyc[bucket].fetch_add(cMark - c0, std::memory_order_relaxed);
    }
    ~G530CycScope()
    {
        if (!on)
            return;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t c1 = g530Cycles();
        if (!marked)
        {
            // Returned before reaching the loop (culled/untextured): all of it is prologue.
            g_g530CycProNs[bucket].fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
                std::memory_order_relaxed);
            g_g530CycProCyc[bucket].fetch_add(c1 - c0, std::memory_order_relaxed);
        }
        else
        {
            g_g530CycLoopNs[bucket].fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - tMark).count()),
                std::memory_order_relaxed);
            g_g530CycLoopCyc[bucket].fetch_add(c1 - cMark, std::memory_order_relaxed);
        }
        g_g530CycN[bucket].fetch_add(1u, std::memory_order_relaxed);
        g_g530CycPx[bucket].fetch_add(px, std::memory_order_relaxed);
        g530CycReport();
    }
};

// ===================== THE LEVER: the axis-aligned textured-sprite SPAN =====================
//
// ⭐⭐⭐ THE ARCHITECTURE, IN ONE SENTENCE: the replayed sprite pixel pays for GENERALITY IT DOES NOT
// HAVE.  Everything the generic per-pixel path re-derives — the interpolated UV, the wrap, the
// page/block/column address of four texture taps, of the Z word and of the destination word, the
// alpha-test classification, the scissor test, the blend selectors, and roughly ten function calls
// of which several cross a translation unit — is either constant for the whole draw or constant for
// the scanline.  The kernel below LOWERS THE LOOP NEST ONE LEVEL: per-DRAW tables, per-ROW bases,
// per-PIXEL nothing but the taps, the filter and the commit.
//
//   per DRAW   the U schedule (sampleU / wrapped u0,u1 / fx per screen column — U DOES NOT DEPEND
//              ON Y, so the divide + std::lround the old loop ran once per PIXEL runs once per
//              COLUMN), the alpha-test gate as an interval, the blend selectors, the fbmsk/FBA
//              decision, the destination page row base
//   per ROW    sampleV, the two wrapped source rows v0/v1 and fy, the destination/Z row bases
//   per PIXEL  four taps, the lerp, combine, the Z compare, the commit
//
// ⭐ EVERY REDUCTION IS BIT-EXACT BY CONSTRUCTION, NOT BY APPROXIMATION.  The U schedule evaluates
// the IDENTICAL float expression the old inner loop evaluated (`u0f + (u1f-u0f)*((x - sampleX0 +
// bias)/spriteW)` then `clampInt(lround(*16),0,0xFFFF)`); it is only evaluated once per column
// instead of once per column per row.  No reciprocal is substituted for the divide, no rounding
// identity is assumed — G529 already proved that class of lever is worth 0.00 ms/f anyway, so the
// kernel deliberately takes NO arithmetic risk.  The destination address uses the SAME
// `GSPSMCT32::addrPSMCT32` expression `writePixel` uses, and the commit reproduces `writePixel`'s
// 32-bit tail statement for statement.
//
//   DC2_G530_SPAN=1     arm the kernel        (bring-up default: OFF)
//   DC2_G530_NO_SPAN=1  rollback once promoted
//   DC2_G530_VERIFY=1   ORACLE — see below. `bad` must be 0.
//
// ⭐⭐ THE ORACLE COMPARES THE COMMITTED WORD, NOT AN INTERMEDIATE (g513_oracle_compares_decisions).
// Under DC2_G530_VERIFY the kernel computes its own destination word and Z decision, then hands the
// pixel to the ORIGINAL `writePixel` / `spriteZPass` / `spriteZStore` and reads VRAM back: the
// authority does the store, so a kernel defect can never reach a pixel while the oracle is on, and
// any disagreement is reported with its coordinates. That is the only form that survives
// g528_oracle_asserts_invariant_not_absence_of_work — it asserts the INVARIANT ("the byte the fast
// path would have written is the byte the proven path did write"), not the absence of later work.

// PROMOTED TO DEFAULT-ON (G530). Rollback: DC2_G530_NO_SPAN=1. DC2_G530_SPAN=1 stays accepted as a
// compatibility no-op so the bring-up harnesses keep working (same convention as G248's
// DC2_G248_FASTSPRITE). Gate: Georama -1.588 ms/f (-4.38%), 4/4 paired, per-pair -0.43%..-8.32%;
// subject -45.1% (1,310.5 -> 721.5 cyc/px) on an identical population; oracle 238,866,074 px bad=0.
inline bool g530SpanOn()
{
    static const bool on = !envFlagEnabled("DC2_G530_NO_SPAN");
    return on;
}
inline bool g530VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G530_VERIFY");
    return on;
}

std::atomic<uint64_t> g_g530Px{0}, g_g530Bad{0}, g_g530Draws{0}, g_g530Reject{0};

// ⚠️ AN ORACLE THAT REPORTS `bad=0` PROVES NOTHING UNLESS THE LEVER RAN. This prints the kernel's
// own coverage next to its defect count, so "0 bad" can never be read off a path that was never
// taken (the failure mode g501_shipped_link_false_promotion and G528's oracle rewrite both hit).
inline void g530SpanReport()
{
    static std::atomic<uint64_t> s_next{1};
    const uint64_t d = g_g530Draws.load(std::memory_order_relaxed);
    uint64_t want = s_next.load(std::memory_order_relaxed);
    if (d < want || !s_next.compare_exchange_strong(want, (want < 20000u) ? 20000u : want + 20000u))
        return;
    std::fprintf(stderr, "[G530:span] draws=%llu px=%llu bad=%llu gateReject=%llu\n",
                 static_cast<unsigned long long>(d),
                 static_cast<unsigned long long>(g_g530Px.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g530Bad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g530Reject.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

// Every per-pixel diagnostic the GENERIC loop can service and the kernel deliberately cannot.
// One cached AND; a set flag simply keeps that draw on the proven path.
inline bool g530Quiet()
{
    static const bool quiet =
        !envFlagEnabled("DC2_PHASE_TRACE") &&
        !envFlagEnabled("DC2_TRACE_RENDER_QUALITY") &&
        !envFlagEnabled("DC2_G195_FG_TRACE") &&
        !envFlagEnabled("DC2_G195_DOF_TRACE") &&
        !envFlagEnabled("DC2_TRACE_G43_FACE_PIXEL") &&
        !envFlagEnabled("DC2_G407_AUTOSCAN") &&
        !envFlagEnabled("DC2_G219_SKYPX") &&
        !envFlagEnabled("DC2_G365_WATCH") &&
        !envFlagEnabled("DC2_G365_ROW") &&
        !envFlagEnabled("DC2_G356_WATCH") &&
        !envFlagEnabled("DC2_G357_ZTRACE") &&
        !envFlagEnabled("DC2_G248_VERIFY") &&
        !envFlagEnabled("DC2_G529_NOLERP") &&
        !envFlagEnabled("DC2_G529_LERPVFY") &&
        !envFlagEnabled("DC2_G529_POINTONLY") &&
        !envFlagEnabled("DC2_TRACE_F50_12") &&
        !envFlagEnabled("DC2_G258_TRACE") &&
        !envFlagEnabled("DC2_G255_VERIFY") &&
        !envFlagEnabled("DC2_TRACE_COSTUME") &&
        !envFlagEnabled("DC2_G43_FACE_UV") &&
        std::getenv("DC2_G407_PIXEL") == nullptr &&
        std::getenv("DC2_DUMP_FONT") == nullptr &&
        std::getenv("DC2_DUMP_VRAMRGN") == nullptr;
    return quiet;
}

// ---- the alpha test, hoisted from a per-pixel switch pair to a per-draw interval ----
// EXACTLY mirrors passesAlphaTest() + classifyAlphaTest() in rasterizer_row_pool.inc: every ATST
// admits an interval of alpha values (NOTEQUAL is the complement of a point, hence `inv`), and the
// two AlphaTestResult triples are constants of TEST.AFAIL. The per-pixel cost becomes two compares.
struct G530AlphaGate
{
    int lo = 0, hi = 255;
    bool inv = false;
    // {writeFramebuffer, writeDepth, preserveDestinationAlpha}
    bool passFb = true, passZ = true, passPa = false;
    bool failFb = false, failZ = false, failPa = false;
    inline bool pass(uint8_t a) const
    {
        const int v = static_cast<int>(a);
        return ((v >= lo) && (v <= hi)) != inv;
    }
};

inline G530AlphaGate g530BuildAlphaGate(uint64_t testReg)
{
    G530AlphaGate gate;
    if ((testReg & 0x1u) != 0u)
    {
        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const int aref = static_cast<int>((testReg >> 4) & 0xFFu);
        switch (atst)
        {
        case 0: gate.lo = 1;    gate.hi = 0;        break; // NEVER  -> empty interval
        case 1: gate.lo = 0;    gate.hi = 255;      break; // ALWAYS
        case 2: gate.lo = 0;    gate.hi = aref - 1; break; // LESS    (aref==0 -> empty)
        case 3: gate.lo = 0;    gate.hi = aref;     break; // LEQUAL
        case 4: gate.lo = aref; gate.hi = aref;     break; // EQUAL
        case 5: gate.lo = aref; gate.hi = 255;      break; // GEQUAL
        case 6: gate.lo = aref + 1; gate.hi = 255;  break; // GREATER (aref==255 -> empty)
        default: gate.lo = aref; gate.hi = aref; gate.inv = true; break; // NOTEQUAL
        }
    }
    switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
    {
    case 1: gate.failFb = true;  gate.failZ = false; gate.failPa = false; break; // FB_ONLY
    case 2: gate.failFb = false; gate.failZ = true;  gate.failPa = false; break; // ZB_ONLY
    case 3: gate.failFb = true;  gate.failZ = false; gate.failPa = true;  break; // RGB_ONLY
    default: gate.failFb = false; gate.failZ = false; gate.failPa = false; break; // KEEP
    }
    return gate;
}

// Self-test: the interval form must agree with the switch form on the WHOLE domain. Runs once, on
// the first eligible draw, and is what lets the gate be trusted without a per-pixel oracle.
inline bool g530AlphaGateAgrees(uint64_t testReg)
{
    const G530AlphaGate gate = g530BuildAlphaGate(testReg);
    for (int a = 0; a <= 255; ++a)
    {
        const AlphaTestResult ref = classifyAlphaTest(testReg, static_cast<uint8_t>(a));
        const bool p = gate.pass(static_cast<uint8_t>(a));
        const bool fb = p ? gate.passFb : gate.failFb;
        const bool zw = p ? gate.passZ : gate.failZ;
        const bool pa = p ? gate.passPa : gate.failPa;
        if (ref.writeFramebuffer != fb || ref.writeDepth != zw ||
            ref.preserveDestinationAlpha != pa)
            return false;
    }
    return true;
}

// ---- the destination address, split into the part that changes per ROW and per 8-PIXEL GROUP ----
// GSPSMCT32::addrPSMCT32 is (page << 13) + ((blockId >> 5) << 13) + (blockId & 31) * 256 +
// columnTable32[y & 7][x & 7] * 4, with `page` advancing every 64 px of x and `blockId` every 8.
// The kernel walks x in groups of 8 aligned to (x & ~7), so the first three terms are computed once
// per group and the last is a table load. Identical arithmetic, one eighth of it.
struct G530RowAddr
{
    uint32_t pageRow = 0;       // (block >> 5) + (y >> 5) * pagesPerRow
    uint32_t blockLo = 0;       // block & 31
    const uint8_t *bt = nullptr; // blockTable32[(y >> 3) & 3]
    const uint8_t *ct = nullptr; // columnTable32[y & 7]
};

inline G530RowAddr g530RowAddr(uint32_t block, uint32_t width, uint32_t y)
{
    G530RowAddr ra;
    const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
    ra.pageRow = (block >> 5u) + (y >> 5u) * pagesPerRow;
    ra.blockLo = block & 0x1Fu;
    ra.bt = GSPSMCT32::blockTable32[(y >> 3u) & 3u];
    ra.ct = GSPSMCT32::columnTable32[y & 0x7u];
    return ra;
}

// Byte offset of the 8-pixel group that contains x, EXCLUDING the column term.
inline uint32_t g530GroupBase(const G530RowAddr &ra, uint32_t x)
{
    const uint32_t page = ra.pageRow + (x >> 6u);
    const uint32_t blockId = ra.blockLo + ra.bt[(x >> 3u) & 7u];
    return (page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 0x1Fu) * 256u;
}

// ---- the per-draw U schedule ----
// sampleU / the wrapped bilinear column pair / the horizontal filter weight all depend ONLY on the
// screen column, so the old loop recomputed them once per row for nothing. Thread-local because a
// band replay runs eight of these concurrently.
struct G530UTable
{
    std::vector<uint16_t> sampleU;
    std::vector<int32_t> u0;
    std::vector<int32_t> u1;
    std::vector<float> fx;
};

inline G530UTable &g530UTable()
{
    static thread_local G530UTable t;
    return t;
}

} // namespace
