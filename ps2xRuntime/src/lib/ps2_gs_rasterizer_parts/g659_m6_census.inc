// ==============================================================================================
// G659 — REWRITE-DEPTH / ORDERED-DESTINATION CENSUS  (ROADMAP priorities 3,4,5,6,7,10 + 1,2)
//
// WHY THIS EXISTS.  G658 measured the mode-6 stage ladder and found the error survives every
// SOURCE-side repair: with a CONSTANT source and the ordinary integer blend (stage 3) the oracle
// still reads 11.07% `bigRgb` against a ~1.4% region control.  In that arm both arms evaluate the
// SAME arithmetic on the SAME operands — `g604SubBlendRgb` already folds `ceil(Cs*As/128)` into
// the fragment and hands GL a plain ONE/ONE reverse subtract — so whatever remains cannot be the
// source, the sampler, the shade, or the per-write rounding.  Three candidates survive:
//
//    (a) the DESTINATION the two arms start the batch from is not the same,
//    (b) the NUMBER of mode-6 applications at a pixel is not the same (coverage / order),
//    (c) one arm CLAMPS at 0 where the other does not, which destroys linearity.
//
// This census separates all three WITHOUT a per-draw GPU readback, by recording on the CPU arm,
// per pixel and per batch: how many writes it committed, how many of those carried the mode-6
// blend, the exact pre-clamp GREEN subtrahend they applied, and how many of them clamped.  The
// compare arm then reads the three questions straight off:
//
//    (a) `depth == 0` pixels.  The CPU wrote NOTHING there, so `cpu == before` by construction and
//        `gpu != before` can ONLY mean the GPU's framebuffer did not equal guest VRAM at batch
//        entry.  That is Oracle 2 (P4) with no new GL work at all, and on `dungeon1` it doubles as
//        the FBW=2 ADDRESS oracle (P1): a stride/offset error shows up as `gpu[x,y]` matching
//        `before[]` at a CONSTANT shift, which g659ShiftProbe searches for.
//    (b) `resid = (before.g - gpu.g) - cpuSubG`, divided by the pixel's own mean subtrahend.  A
//        residual of +-1 unit is a WHOLE missing or extra mode-6 application; a residual under a
//        unit is arithmetic.  That is Oracles 3/4/5 (P5/P6/P7) in aggregate.
//    (c) the `clamp` split.
//
// ⛔ The counters below are only correct when every committed pixel passes through `writePixel`.
// The G605/G608/G609/G614/G615/G617/G530 span kernels commit directly, so a census run MUST also
// set their rollbacks (tools/g659_m6.ps1 does).  They are exact-equivalent, so the POPULATION is
// unchanged and only the wall clock moves (rule: a census that changes the population is void).
//
// Diagnostic build only (`-DPS2X_G659_DIAG=ON`, rule 12c).  Arm with DC2_G659_M6CENSUS=1.
// ==============================================================================================
#if defined(PS2X_G659_DIAG)

struct G659PixelState
{
    uint16_t depth = 0;   // committed CPU writes to this pixel in this batch
    uint16_t m6Depth = 0; // ... of which carried the mode-6 blend (A=0 B=Cs C=As D=Cd)
    uint16_t m6Clamp = 0; // ... of which clamped the GREEN channel at 0 pre-store
    int32_t m6SubG = 0;   // sum of the PRE-CLAMP green subtrahend those writes applied
};

static std::vector<G659PixelState> g_g659Px;

static bool g659CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G659_M6CENSUS");
    return on;
}

// The census only reaches its population when the span kernels are rolled back. Report the fact
// once instead of letting a silently-partial count be read as a result (rule: print `checks` and
// the selected arm unconditionally).
static void g659ReportArm()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    std::fprintf(stderr,
                 "[G659:arm] census=%d trispan=%d trifst=%d triuntex=%d triscan=%d "
                 "spritefast=%d privz=%d blendfill=%d sprspan=%d\n",
                 g659CensusOn() ? 1 : 0,
                 envFlagEnabled("DC2_G605_NO_TRI_SPAN") ? 0 : 1,
                 envFlagEnabled("DC2_G608_NO_TRI_FST") ? 0 : 1,
                 envFlagEnabled("DC2_G628_NO_TRI_UNTEX") ? 0 : 1,
                 envFlagEnabled("DC2_G609_NO_TRI_SCAN") ? 0 : 1,
                 envFlagEnabled("DC2_G614_NO_SPRITE_FAST") ? 0 : 1,
                 envFlagEnabled("DC2_G615_NO_PRIVZ_SPAN") ? 0 : 1,
                 envFlagEnabled("DC2_G617_NO_BLEND_FILL") ? 0 : 1,
                 envFlagEnabled("DC2_G530_NO_SPAN") ? 0 : 1);
}

static void g659BeginBatch(size_t count)
{
    if (!g659CensusOn())
        return;
    g659ReportArm();
    g_g659Px.assign(count, G659PixelState{});
}

// Called from writePixel's commit point, i.e. after every test the pixel could still fail.
static void g659NoteCpuWrite(uint32_t fbp, int x, int y, bool m6, int subG, bool clampedG)
{
    if (!g659CensusOn())
        return;
    const G255VerifyState &s = g_g255Verify;
    if (!s.pending || fbp != s.fbp || y < s.rowLo || y > s.rowHi || x < 0 || x >= s.fbW)
        return;
    const size_t i = static_cast<size_t>(y - s.rowLo) * static_cast<size_t>(s.fbW) +
                     static_cast<size_t>(x);
    if (i >= g_g659Px.size())
        return;
    // The G637 cooperative replay partitions the batch into CONTIGUOUS DISJOINT row bands, so no
    // two workers can ever address the same `i`. Plain increments are correct here; atomics would
    // only cost time.
    G659PixelState &p = g_g659Px[i];
    if (p.depth != 0xFFFFu)
        ++p.depth;
    if (m6)
    {
        if (p.m6Depth != 0xFFFFu)
            ++p.m6Depth;
        p.m6SubG += subG;
        if (clampedG && p.m6Clamp != 0xFFFFu)
            ++p.m6Clamp;
    }
}

// ---- compare-arm accumulation -----------------------------------------------------------------

static constexpr int kG659Buckets = 9; // 0,1,2,3,4,5-8,9-16,17-32,33+
static const char *const kG659BucketName[kG659Buckets] = {
    "d0", "d1", "d2", "d3", "d4", "d5-8", "d9-16", "d17-32", "d33+"
};

static int g659Bucket(unsigned d)
{
    if (d <= 4u)
        return static_cast<int>(d);
    if (d <= 8u)
        return 5;
    if (d <= 16u)
        return 6;
    if (d <= 32u)
        return 7;
    return 8;
}

struct G659Bucket
{
    uint64_t px = 0, big = 0, cpuOnly = 0, gpuOnly = 0, both = 0, same = 0;
    int64_t sumSignedG = 0;
    uint64_t sumAbsG = 0;
    uint64_t maxAbs = 0;
};

static G659Bucket g_g659Sub[kG659Buckets];      // batches carrying >=1 mode-6 draw
static G659Bucket g_g659NoSub[kG659Buckets];    // the same-run control
static G659Bucket g_g659InBox[kG659Buckets];    // SUB batches, inside the mode-6 footprint
static G659Bucket g_g659OutBox[kG659Buckets];   // SUB batches, outside it  (P10)
// ⭐ THE ONLY ARM THE RESIDUAL ARITHMETIC IS VALID ON. `before - gpu` only equals the mode-6
// subtrahend when NOTHING ELSE wrote the pixel in this batch; an ordinary draw landing on top of
// the particle makes the difference a statement about that draw instead. `depth == m6Depth` is
// exactly that condition and it is free — both counters already exist.
static G659Bucket g_g659Pure[kG659Buckets];
static uint64_t g_g659PurePx = 0, g_g659MixedPx = 0;
// ⭐⭐ THE MATCHED-OVERDRAW CONTROL. The PURE arm shows the mode-6 draws agree with the CPU to a
// couple of LSBs, so the in-footprint divergence has to come from the ORDINARY draws that share
// those pixels — and the mode-6 footprint is the densest overdraw in the frame, so comparing it
// against a region control is comparing a 20-deep stack against a 1-deep one. Bucket BOTH arms by
// the ORDINARY (non-mode-6) write depth instead: if SUB and NOSUB agree row for row, the admission
// contributes nothing and the 41.3x in-footprint figure is an overdraw artifact.
static G659Bucket g_g659SubOrd[kG659Buckets];
static G659Bucket g_g659NoSubOrd[kG659Buckets];
// The third row of that comparison: ORDINARY-ONLY pixels INSIDE a mode-6-carrying batch. A NOSUB
// batch has m6Depth == 0 everywhere by construction, so NOSUBORD is already the ordinary-only
// control; this is its same-batch twin. If SUBORDM60 tracks NOSUBORD, the ordinary draws behave
// identically whether or not the batch also carries mode 6, and any excess in SUBORD is the
// recurrence carrying a PRE-EXISTING ordinary-draw error, not manufacturing a new one.
static G659Bucket g_g659SubOrdM60[kG659Buckets];

// resid histogram, in units of the pixel's own mean mode-6 subtrahend:
// 0:|r|<0.25u  1:0.25-0.75u  2:~1u  3:~2u  4:~3u  5:>3u  (sign folded into 6/7 counters)
// ⛔ Only CLAMP-FREE pixels are admitted: a write that clamped at 0 is not invertible, so its
// residual is not a statement about how many applications either arm performed.
static uint64_t g_g659Resid[6]{};
static uint64_t g_g659ResidPos = 0, g_g659ResidNeg = 0;
static uint64_t g_g659ClampPx = 0, g_g659ClampBig = 0;
static uint64_t g_g659NoClampPx = 0, g_g659NoClampBig = 0;
// The same residual as a RAW integer histogram. At the constant-source stage the unit is a known
// exact constant (83 green LSBs), so these bins read directly as "how many whole mode-6
// applications did the GPU miss or add".
static uint64_t g_g659RawResid[9]{}; // 0, |1|, |2|, 3..10, 11..50, 51..90, 91..170, 171..300, >300
static const char *const kG659RawName[9] = {
    "r0", "r1", "r2", "r3-10", "r11-50", "r51-90", "r91-170", "r171-300", "rgt300"
};
static uint64_t g_g659UnitSum = 0, g_g659UnitN = 0, g_g659DepthSum = 0;

static void g659Add(G659Bucket &b, bool big, bool cpuChanged, bool gpuChanged, int dG)
{
    ++b.px;
    if (big)
        ++b.big;
    if (cpuChanged && !gpuChanged)
        ++b.cpuOnly;
    else if (gpuChanged && !cpuChanged)
        ++b.gpuOnly;
    else if (cpuChanged && gpuChanged)
        ++b.both;
    else
        ++b.same;
    b.sumSignedG += dG;
    const uint64_t a = static_cast<uint64_t>(dG < 0 ? -dG : dG);
    b.sumAbsG += a;
    if (a > b.maxAbs)
        b.maxAbs = a;
}

// One pixel of the compare loop. `big` is the oracle's own RGB>1 predicate so the census and the
// gate always share a denominator.
static void g659ScorePixel(size_t i, uint32_t before, uint32_t cpu, uint32_t gpu,
                           bool big, bool isSub, bool inBox)
{
    if (!g659CensusOn() || i >= g_g659Px.size())
        return;
    const G659PixelState &p = g_g659Px[i];
    const int bg = static_cast<int>((before >> 8) & 0xFFu);
    const int cg = static_cast<int>((cpu >> 8) & 0xFFu);
    const int gg = static_cast<int>((gpu >> 8) & 0xFFu);
    const bool cpuChanged = (cpu & 0x00FFFFFFu) != (before & 0x00FFFFFFu);
    const bool gpuChanged = (gpu & 0x00FFFFFFu) != (before & 0x00FFFFFFu);
    const int dG = gg - cg;
    const int bucket = g659Bucket(p.m6Depth);
    g659Add(isSub ? g_g659Sub[bucket] : g_g659NoSub[bucket], big, cpuChanged, gpuChanged, dG);
    if (isSub)
        g659Add(inBox ? g_g659InBox[bucket] : g_g659OutBox[bucket], big, cpuChanged,
                gpuChanged, dG);

    const unsigned ordDepth =
        p.depth > p.m6Depth ? static_cast<unsigned>(p.depth - p.m6Depth) : 0u;
    const int ordBucket = g659Bucket(ordDepth);
    g659Add(isSub ? g_g659SubOrd[ordBucket] : g_g659NoSubOrd[ordBucket], big, cpuChanged,
            gpuChanged, dG);
    if (isSub && p.m6Depth == 0u)
        g659Add(g_g659SubOrdM60[ordBucket], big, cpuChanged, gpuChanged, dG);

    const bool pure = p.depth == p.m6Depth;
    if (pure)
    {
        ++g_g659PurePx;
        g659Add(g_g659Pure[bucket], big, cpuChanged, gpuChanged, dG);
    }
    else
    {
        ++g_g659MixedPx;
    }

    if (p.m6Depth != 0u)
    {
        if (p.m6Clamp != 0u)
        {
            ++g_g659ClampPx;
            if (big)
                ++g_g659ClampBig;
        }
        else
        {
            ++g_g659NoClampPx;
            if (big)
                ++g_g659NoClampBig;
        }
        // How much of (before - gpu) is NOT explained by the subtrahend the CPU actually applied?
        const double unit =
            static_cast<double>(p.m6SubG) / static_cast<double>(p.m6Depth);
        const int residI = (bg - gg) - p.m6SubG;
        const double resid = static_cast<double>(residI);
        if (pure && p.m6Clamp == 0u && unit >= 1.0)
        {
            const double k = resid / unit;
            const double ak = k < 0.0 ? -k : k;
            int slot = 5;
            if (ak < 0.25) slot = 0;
            else if (ak < 0.75) slot = 1;
            else if (ak < 1.5) slot = 2;
            else if (ak < 2.5) slot = 3;
            else if (ak < 3.5) slot = 4;
            ++g_g659Resid[slot];
            if (slot >= 2)
            {
                if (k > 0.0) ++g_g659ResidPos; else ++g_g659ResidNeg;
            }
            g_g659UnitSum += static_cast<uint64_t>(unit + 0.5);
            g_g659DepthSum += p.m6Depth;
            ++g_g659UnitN;
            const int ar = residI < 0 ? -residI : residI;
            int raw = 8;
            if (ar == 0) raw = 0;
            else if (ar == 1) raw = 1;
            else if (ar == 2) raw = 2;
            else if (ar <= 10) raw = 3;
            else if (ar <= 50) raw = 4;
            else if (ar <= 90) raw = 5;
            else if (ar <= 170) raw = 6;
            else if (ar <= 300) raw = 7;
            ++g_g659RawResid[raw];
        }
    }
}

// ---- P1: the FBW=2 / discovered-target ADDRESS oracle ------------------------------------------
// For every pixel the CPU replay never wrote, `before` IS the CPU value. If the GPU's value for
// that pixel equals `before` at a CONSTANT (dx,dy) shift far more often than at (0,0), the two
// arms disagree about where the pixel LIVES, not about what colour it should be.
static uint64_t g_g659ShiftHits[17][17]{};
static uint64_t g_g659ShiftProbes = 0, g_g659ShiftNoMatch = 0;

static void g659ShiftProbe(const G255VerifyState &s, int x, int y, uint32_t gpuVal)
{
    if (!g659CensusOn())
        return;
    ++g_g659ShiftProbes;
    bool any = false;
    for (int dy = -8; dy <= 8; ++dy)
    {
        const int sy = y + dy;
        if (sy < s.rowLo || sy > s.rowHi)
            continue;
        for (int dx = -8; dx <= 8; ++dx)
        {
            const int sx = x + dx;
            if (sx < 0 || sx >= s.fbW)
                continue;
            const size_t j = static_cast<size_t>(sy - s.rowLo) * static_cast<size_t>(s.fbW) +
                             static_cast<size_t>(sx);
            if (j < s.before.size() &&
                (s.before[j] & 0x00FFFFFFu) == (gpuVal & 0x00FFFFFFu))
            {
                ++g_g659ShiftHits[dy + 8][dx + 8];
                any = true;
            }
        }
    }
    if (!any)
        ++g_g659ShiftNoMatch;
}

static void g659ReportBuckets(const char *tag, const G659Bucket *rows)
{
    for (int i = 0; i < kG659Buckets; ++i)
    {
        const G659Bucket &b = rows[i];
        if (b.px == 0u)
            continue;
        std::fprintf(stderr,
                     "[G659:m6depth] arm=%s bucket=%s px=%llu big=%llu %.4f%% "
                     "cpuOnly=%llu gpuOnly=%llu both=%llu same=%llu "
                     "meanSignedG=%.3f meanAbsG=%.3f maxAbs=%llu\n",
                     tag, kG659BucketName[i],
                     static_cast<unsigned long long>(b.px),
                     static_cast<unsigned long long>(b.big),
                     100.0 * static_cast<double>(b.big) / static_cast<double>(b.px),
                     static_cast<unsigned long long>(b.cpuOnly),
                     static_cast<unsigned long long>(b.gpuOnly),
                     static_cast<unsigned long long>(b.both),
                     static_cast<unsigned long long>(b.same),
                     static_cast<double>(b.sumSignedG) / static_cast<double>(b.px),
                     static_cast<double>(b.sumAbsG) / static_cast<double>(b.px),
                     static_cast<unsigned long long>(b.maxAbs));
    }
}

static void g659Report(uint64_t n)
{
    if (!g659CensusOn())
        return;
    g659ReportBuckets("SUB", g_g659Sub);
    g659ReportBuckets("NOSUB", g_g659NoSub);
    g659ReportBuckets("INBOX", g_g659InBox);
    g659ReportBuckets("OUTBOX", g_g659OutBox);
    g659ReportBuckets("PURE", g_g659Pure);
    g659ReportBuckets("SUBORD", g_g659SubOrd);
    g659ReportBuckets("NOSUBORD", g_g659NoSubOrd);
    g659ReportBuckets("SUBORDM60", g_g659SubOrdM60);
    std::fprintf(stderr, "[G659:purity] purePx=%llu mixedPx=%llu\n",
                 static_cast<unsigned long long>(g_g659PurePx),
                 static_cast<unsigned long long>(g_g659MixedPx));
    std::fprintf(stderr,
                 "[G659:m6resid] n=%llu lt0.25u=%llu 0.25-0.75u=%llu ~1u=%llu ~2u=%llu ~3u=%llu "
                 "gt3u=%llu pos=%llu neg=%llu | clamp(px=%llu big=%llu) "
                 "noclamp(px=%llu big=%llu)\n",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_g659Resid[0]),
                 static_cast<unsigned long long>(g_g659Resid[1]),
                 static_cast<unsigned long long>(g_g659Resid[2]),
                 static_cast<unsigned long long>(g_g659Resid[3]),
                 static_cast<unsigned long long>(g_g659Resid[4]),
                 static_cast<unsigned long long>(g_g659Resid[5]),
                 static_cast<unsigned long long>(g_g659ResidPos),
                 static_cast<unsigned long long>(g_g659ResidNeg),
                 static_cast<unsigned long long>(g_g659ClampPx),
                 static_cast<unsigned long long>(g_g659ClampBig),
                 static_cast<unsigned long long>(g_g659NoClampPx),
                 static_cast<unsigned long long>(g_g659NoClampBig));
    std::fprintf(stderr, "[G659:m6raw] n=%llu meanUnit=%.2f meanDepth=%.2f",
                 static_cast<unsigned long long>(n),
                 g_g659UnitN ? static_cast<double>(g_g659UnitSum) /
                                   static_cast<double>(g_g659UnitN) : 0.0,
                 g_g659UnitN ? static_cast<double>(g_g659DepthSum) /
                                   static_cast<double>(g_g659UnitN) : 0.0);
    for (int i = 0; i < 9; ++i)
        std::fprintf(stderr, " %s=%llu", kG659RawName[i],
                     static_cast<unsigned long long>(g_g659RawResid[i]));
    std::fprintf(stderr, "\n");
    if (g_g659ShiftProbes != 0u)
    {
        int bestDy = 0, bestDx = 0;
        uint64_t best = 0;
        for (int dy = 0; dy < 17; ++dy)
            for (int dx = 0; dx < 17; ++dx)
                if (g_g659ShiftHits[dy][dx] > best)
                {
                    best = g_g659ShiftHits[dy][dx];
                    bestDy = dy - 8;
                    bestDx = dx - 8;
                }
        std::fprintf(stderr,
                     "[G659:shift] probes=%llu noMatch=%llu zero=%llu bestShift=(%d,%d) hits=%llu\n",
                     static_cast<unsigned long long>(g_g659ShiftProbes),
                     static_cast<unsigned long long>(g_g659ShiftNoMatch),
                     static_cast<unsigned long long>(g_g659ShiftHits[8][8]),
                     bestDx, bestDy, static_cast<unsigned long long>(best));
    }
}

#endif // PS2X_G659_DIAG
// The default build defines NOTHING here and every call site is `#if`-excluded, so the hot TU
// carries zero G659 bytes (rule 12c / the G653 P25 +0.484 ms/f code-layout precedent).
