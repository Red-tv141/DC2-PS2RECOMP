// G418 — colour-readback VRAM unpack (the CPU-copy component of the 0x13b readback edge).
//
// The one readback batch per frame publishes its GL rows into guest VRAM with a serial per-pixel
// `addrPSMCT32` + 4-byte memcpy loop. The mirror-image UPLOAD pack (VRAM -> fbPixels) was already
// row-parallelised by G298; the readback direction never was, so it is a single-threaded 213k-pixel
// scatter on the flush-owner thread. This part supplies:
//
//   * DC2_G418_PROFILE=1  — time the unpack, keyed by target, with rows/pixels (behaviour-pure).
//   * DC2_G418_FAST_UNPACK=1 / DC2_G418_NO_FAST_UNPACK=1 — the exact fast unpack arm.
//   * DC2_G418_VERIFY=1  — same-run exact oracle: re-derive every published pixel through the
//     original serial address path and compare the VRAM bytes.
//
// EXACTNESS OF THE ADDRESS TABLE. addrPSMCT32(block, width, x, y) is
//     page       = (block>>5) + (y>>5)*width + (x>>6)
//     blockId    = (block&31) + blockTable32[(y>>3)&3][(x>>3)&7]
//     result     = (page<<13) + ((blockId>>5)<<13) + (blockId&31)*256 + columnTable32[y&7][x&7]*4
// so the ONLY term that depends on x above bit 5 is the linear `(x>>6)` page step, and the only
// terms that depend on y are `(y>>5)` (linear page step) and bits 3..4 / 0..2 of y (the two table
// indices). Therefore, for a fixed (block, width):
//     addr(x,y) == (((block>>5) + (y>>5)*width) << 13) + ((x>>6) << 13) + tbl[y&31][x&63]
// with tbl[yl][xl] := addrPSMCT32(block, width, xl, yl) - ((block>>5)<<13)  for xl<64, yl<32.
// The table is literally the function restricted to one page, so nothing is approximated.
//
// PAIRED STORES. columnTable32[y&7][x&7] for even x and x+1 always differ by exactly 1 word, and
// x and x+1 share (x>>3) and (x>>6), so two horizontally adjacent pixels starting at an even x are
// contiguous in VRAM. An even-width row therefore packs as 8-byte stores. The out-of-range tail
// falls back to the original per-pixel bound check so clipping behaviour is byte-identical.
//
// ROW PARALLELISM. Distinct (x,y) map to distinct words, so distinct rows own disjoint VRAM words:
// lanes over disjoint guest rows are exact by construction. The source readback buffer is read-only
// and GSRowPool::run joins before anything observes VRAM (same contract G298 already relies on for
// the upload pack, on this same call path and thread).

static bool g418ProfileOn()
{
    static const bool on = envFlagEnabled("DC2_G418_PROFILE");
    return on;
}

// G418 PROMOTED to default-ON (2026-07-30). Exact by construction (the table IS addrPSMCT32 restricted
// to one page) and by oracle (109,051,904 pixels, bad=0). Within-process randomized A/B over ~11,000
// MAP-0 frames measured -0.31 ms/frame at seDelta 0.12, matching the isolated 0.344 ms saving scaled
// by this site's measured 0.93 critical-path sensitivity.
// Kill with DC2_G418_NO_FAST_UNPACK=1 or DC2_G418_FAST_UNPACK=0.
static bool g418FastUnpackOn()
{
    static const bool on = !envFlagEnabled("DC2_G418_NO_FAST_UNPACK") &&
                           !envFlagDisabled("DC2_G418_FAST_UNPACK");
    return on;
}

// Row parallelism inside the fast arm, so the table/paired-store term and the lane term can be
// attributed apart without a rebuild.
static bool g418ParUnpackOn()
{
    static const bool on = !envFlagEnabled("DC2_G418_NO_PAR_UNPACK");
    return on;
}

static int g418UnpackLanes()
{
    static const int lanes = [] {
        const char *value = std::getenv("DC2_G418_UNPACK_LANES");
        if (value == nullptr)
            return 8;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    return lanes;
}

static bool g418VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G418_VERIFY");
    return on;
}

// DC2_G418_ALTERNATE=<frames> — within-process A/B. Separate processes on this route carry a
// +-1.5 ms run-level offset that swamps a sub-millisecond mechanism (the G417 outcome). Exactly one
// readback unpack happens per frame, so the interval between consecutive unpacks IS a frame period
// measured at a fixed point in the frame. Alternating the arm every <frames> frames inside one
// process and bucketing those periods by the arm that produced them removes process-level drift
// entirely and yields hundreds of paired samples per run. Diagnostic instrument: it deliberately
// runs both arms, so it is never a shipping mode.
static int g418AlternateBlock()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G418_ALTERNATE");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(3600, std::atoi(value)));
    }();
    return n;
}

// DC2_G418_ALT_INVERT=1 swaps which block parity gets which arm. A fixed parity assignment aliases
// with any periodic workload phase in the route, so a result is only the arm's if it KEEPS ITS SIGN
// under inversion; if the sign follows the parity instead, the delta is positional bias.
static bool g418AltInvertOn()
{
    static const bool on = envFlagEnabled("DC2_G418_ALT_INVERT");
    return on;
}

// The first blocks after the route loads are the MAP-0 warm-up and are systematically slower, so
// whichever parity owns block 0 inherits that cost as a fixed bias (measured: about +0.2 ms at
// block=10, i.e. the entire apparent effect). Drop a warm-up prefix before accumulating anything;
// DC2_G418_ALT_WARM overrides the default 120 frames.
static uint64_t g418AltWarmFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G418_ALT_WARM");
        if (value == nullptr)
            return 120;
        return std::max(0, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Frame periods are wall-clock, so a disc read or a scene transition can inject an outlier that no
// mean survives. Bucket both a raw mean and a mean clipped at 200 ms (well above this route's ~67 ms
// steady frame) and report both.
static uint64_t s_g418AltNs[2]{}, s_g418AltN[2]{}, s_g418AltClipNs[2]{}, s_g418AltClipN[2]{};
static double s_g418AltClipSq[2]{};

// DC2_G418_ALT_RANDOM=1 assigns the arm per frame from a fixed LCG instead of alternating blocks.
// Block alternation aliases with any periodic workload whose period is a multiple of 2*block (a
// residual +0.2 ms parity nuisance survived even a 120-frame warm-up skip). Independent per-frame
// randomisation cannot alias with anything, needs no parity-inversion control, and makes the printed
// standard errors meaningful.
static bool g418AltRandomOn()
{
    static const bool on = envFlagEnabled("DC2_G418_ALT_RANDOM");
    return on;
}

// DC2_G418_ALT_BALLAST=<repeats> — critical-path sensitivity probe. Both buckets run the LEGACY
// unpack; bucket A additionally repeats it <repeats> times (idempotent - it rewrites the identical
// bytes). The measured delta is therefore how much of a KNOWN added cost at this exact site reaches
// the frame. That ratio is the payoff ceiling for any optimization here: if adding 0.46 ms only moves
// the frame 0.1 ms, the site sits behind slack and removing 0.34 ms cannot pay.
static int g418AltBallast()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G418_ALT_BALLAST");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(16, std::atoi(value)));
    }();
    return n;
}

// Returns the arm this frame's unpack should use, and charges the previous frame's period to the arm
// that actually served it. The first two frames of each block are dropped so a switch's cache and
// branch-predictor warm-up cannot land in either bucket.
static bool g418AlternatePick(bool defaultFast)
{
    const int block = g418AlternateBlock();
    if (block <= 0 && !g418AltRandomOn())
        return defaultFast;
    static std::chrono::steady_clock::time_point s_last{};
    static uint64_t s_frame = 0u;
    static bool s_lastFast = false;
    static bool s_lastValid = false;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t idx = s_frame++;
    const bool random = g418AltRandomOn();
    static uint64_t s_lcg = 0x9E3779B97F4A7C15ull;
    uint64_t pos = 2u; // random mode keeps every sample
    bool fast;
    if (random)
    {
        s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull;
        fast = ((s_lcg >> 33u) & 1ull) != 0ull;
    }
    else
    {
        pos = idx % static_cast<uint64_t>(block);
        fast = (((idx / static_cast<uint64_t>(block)) & 1ull) == 0ull) != g418AltInvertOn();
    }
    if (s_lastValid && pos > 1u && idx >= g418AltWarmFrames())
    {
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_last).count());
        const int a = s_lastFast ? 0 : 1;
        s_g418AltNs[a] += ns;
        ++s_g418AltN[a];
        if (ns < 200000000ull)
        {
            const double ms = static_cast<double>(ns) / 1.0e6;
            s_g418AltClipNs[a] += ns;
            ++s_g418AltClipN[a];
            s_g418AltClipSq[a] += ms * ms;
        }
    }
    s_last = now;
    s_lastFast = fast;
    s_lastValid = true;
    if (idx != 0u && (idx % 600u) == 0u)
    {
        const auto mean = [](uint64_t ns, uint64_t n) {
            return n ? static_cast<double>(ns) / 1.0e6 / n : 0.0;
        };
        const double f = mean(s_g418AltClipNs[0], s_g418AltClipN[0]);
        const double l = mean(s_g418AltClipNs[1], s_g418AltClipN[1]);
        // Standard error of each arm mean, and of their difference (independent samples).
        const auto sem = [&](int a, double m) {
            const uint64_t n = s_g418AltClipN[a];
            if (n < 2u)
                return 0.0;
            const double var = (s_g418AltClipSq[a] - static_cast<double>(n) * m * m) /
                               static_cast<double>(n - 1u);
            return var > 0.0 ? std::sqrt(var / static_cast<double>(n)) : 0.0;
        };
        const double sf = sem(0, f), sl = sem(1, l);
        std::fprintf(stderr,
                     "[G418:alt] mode=%s block=%d inv=%d frames=%llu "
                     "fast(n=%llu ms=%.3f se=%.3f) legacy(n=%llu ms=%.3f se=%.3f) "
                     "delta=%.3f (%.2f%%) seDelta=%.3f\n",
                     g418AltRandomOn() ? "rand" : "block", block,
                     g418AltInvertOn() ? 1 : 0,
                     static_cast<unsigned long long>(idx),
                     static_cast<unsigned long long>(s_g418AltClipN[0]), f, sf,
                     static_cast<unsigned long long>(s_g418AltClipN[1]), l, sl,
                     f - l, l != 0.0 ? 100.0 * (f - l) / l : 0.0,
                     std::sqrt(sf * sf + sl * sl));
    }
    return fast;
}

// One-page address table for the current (fbBlock, fbw). Flush-owner thread only; the lanes read it
// after it is fully built (GSRowPool::run publishes it through the dispatch).
struct G418Ct32Table
{
    uint32_t block = 0xFFFFFFFFu;
    uint32_t width = 0xFFFFFFFFu;
    bool valid = false;
    uint32_t off[32][64]{};
};
static G418Ct32Table s_g418Ct32;

static const G418Ct32Table &g418Ct32Table(uint32_t block, uint32_t width)
{
    const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
    if (s_g418Ct32.valid && s_g418Ct32.block == block && s_g418Ct32.width == pagesPerRow)
        return s_g418Ct32;
    const uint32_t base = (block >> 5u) << 13u;
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 64u; ++x)
            s_g418Ct32.off[y][x] =
                GSPSMCT32::addrPSMCT32(block, pagesPerRow, x, y) - base;
    s_g418Ct32.block = block;
    s_g418Ct32.width = pagesPerRow;
    s_g418Ct32.valid = true;
    return s_g418Ct32;
}

// Publish GS rows [rowLo,rowHi] of a bottom-first colour readback into guest VRAM. `legacy` runs the
// original serial per-pixel path verbatim; the fast path is the table/paired/lane form above.
// G498 added `lanesOverride`: 0 keeps G418's own promoted lane policy verbatim (its call site passes
// nothing), a positive value pins the lane count. The owner-publish sites pass 1 because on this
// 6-physical-core host the row pool spends more OTHER-core time than it saves on the caller, and the
// pole is no longer this thread (G498 §1: gsslow 0.23, vu1slow 0.81).
static void g418UnpackColorRows(uint8_t *vram, uint32_t vramSize,
                                const std::vector<uint32_t> &src,
                                uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                                int rowLo, int rowHi, bool legacy, int lanesOverride)
{
    if (vram == nullptr || rowHi < rowLo || fbW <= 0)
        return;
    const auto serialRows = [&](int laneLo, int laneHi) {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(fbH - 1 - y) * fbW;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                            static_cast<uint32_t>(x),
                                                            static_cast<uint32_t>(y));
                if (off + 4u <= vramSize)
                    std::memcpy(vram + off, &src[srcRow + x], 4);
            }
        }
    };
    if (legacy || (fbW & 1) != 0)
    {
        serialRows(rowLo, rowHi);
        return;
    }
    const G418Ct32Table &tbl = g418Ct32Table(fbBlock, fbw);
    const uint32_t pagesPerRow = (fbw != 0u) ? fbw : 1u;
    const uint32_t basePage = fbBlock >> 5u;
    // G672: the arm is read ONCE per call here, never inside a row or group loop (G502/G645 law).
    // `rowPair` additionally requires an 8-multiple width, which is what makes an aligned x group
    // exist at all; every measured DC2 target is 512/128/64 wide.
    const bool g672RowPair = g672RowPairOn() && (fbW & 7) == 0;
    const bool g672Stat = g672StatOn();
    const bool g672Verify = g672VerifyOn();
    // `fastRows` is dispatched to several GSRowPool lanes concurrently, so the census counters must
    // be atomic. They are touched ONLY behind the cached `g672Stat` bool, so the default path pays
    // one predicted-not-taken compare per group, never an atomic (rule 12c).
    std::atomic<uint64_t> g672PairGrp{0ull}, g672ScalarGrp{0ull}, g672OddRows{0ull};
    // One row of the shipped table/paired form, verbatim.
    const auto singleRow = [&](int y) {
        const size_t srcRow = static_cast<size_t>(fbH - 1 - y) * fbW;
        const uint32_t *rowOff = tbl.off[static_cast<uint32_t>(y) & 31u];
        const uint32_t yBase =
            (basePage + (static_cast<uint32_t>(y) >> 5u) * pagesPerRow) << 13u;
        const uint32_t *srcPx = src.data() + srcRow;
        for (int x = 0; x < fbW; x += 2)
        {
            const uint32_t off = yBase +
                                 (static_cast<uint32_t>(x >> 6) << 13u) +
                                 rowOff[static_cast<uint32_t>(x) & 63u];
            if (off + 8u <= vramSize)
                std::memcpy(vram + off, srcPx + x, 8);
            else if (off + 4u <= vramSize)
                // The second pixel of the pair sits at off+4 and needs off+8 <= vramSize, which
                // just failed, so the original loop would have dropped it too.
                std::memcpy(vram + off, srcPx + x, 4);
        }
    };
    // G672: rows y (EVEN) and y+1 in 8-pixel groups. `groupBase + 64 <= vramSize` implies all eight
    // of the pairs' original `off + 8 <= vramSize` tests, so a group that passes writes exactly the
    // bytes the scalar form would; a group that fails falls back to it for both rows.
    const auto pairRows = [&](int y) {
        const uint32_t *srcA = src.data() + static_cast<size_t>(fbH - 1 - y) * fbW;
        const uint32_t *srcB = src.data() + static_cast<size_t>(fbH - 2 - y) * fbW;
        const uint32_t *rowOffA = tbl.off[static_cast<uint32_t>(y) & 31u];
        const uint32_t *rowOffB = tbl.off[static_cast<uint32_t>(y + 1) & 31u];
        // y is even, so (y+1)>>5 == y>>5: both rows share this page base by construction.
        const uint32_t yBase =
            (basePage + (static_cast<uint32_t>(y) >> 5u) * pagesPerRow) << 13u;
        for (int x = 0; x < fbW; x += 8)
        {
            const uint32_t groupBase = yBase +
                                       (static_cast<uint32_t>(x >> 6) << 13u) +
                                       rowOffA[static_cast<uint32_t>(x) & 63u];
            if (groupBase + 64u <= vramSize)
            {
                g672StoreGroup(vram + groupBase, srcA + x, srcB + x);
                if (g672Verify)
                    g672VerifyGroup(vram, vramSize, srcA + x, srcB + x, fbBlock, pagesPerRow, x, y);
                if (g672Stat)
                    g672PairGrp.fetch_add(1ull, std::memory_order_relaxed);
                continue;
            }
            if (g672Stat)
                g672ScalarGrp.fetch_add(1ull, std::memory_order_relaxed);
            for (int k = 0; k < 8; k += 2)
            {
                const uint32_t offA = yBase + (static_cast<uint32_t>((x + k) >> 6) << 13u) +
                                      rowOffA[static_cast<uint32_t>(x + k) & 63u];
                if (offA + 8u <= vramSize)
                    std::memcpy(vram + offA, srcA + x + k, 8);
                else if (offA + 4u <= vramSize)
                    std::memcpy(vram + offA, srcA + x + k, 4);
                const uint32_t offB = yBase + (static_cast<uint32_t>((x + k) >> 6) << 13u) +
                                      rowOffB[static_cast<uint32_t>(x + k) & 63u];
                if (offB + 8u <= vramSize)
                    std::memcpy(vram + offB, srcB + x + k, 8);
                else if (offB + 4u <= vramSize)
                    std::memcpy(vram + offB, srcB + x + k, 4);
            }
        }
    };
    const auto fastRows = [&](int laneLo, int laneHi) {
        int y = laneLo;
        if (g672RowPair)
        {
            if ((y & 1) != 0 && y <= laneHi)
            {
                singleRow(y);
                ++y;
                if (g672Stat)
                    g672OddRows.fetch_add(1ull, std::memory_order_relaxed);
            }
            for (; y + 1 <= laneHi; y += 2)
                pairRows(y);
        }
        for (; y <= laneHi; ++y)
            singleRow(y);
    };
    const int rows = rowHi - rowLo + 1;
    const int lanes = lanesOverride > 0
                          ? lanesOverride
                          : ((g418ParUnpackOn() && rows >= 8) ? g418UnpackLanes() : 1);
    if (lanes <= 1)
        fastRows(rowLo, rowHi); // no dispatch at all — GSRowPool::run would still cost its setup
    else
        GSRowPool::instance().run(rowLo, rowHi, lanes, fastRows);
    // G672 census, folded ONCE per call after every lane has joined. `lanesOverride > 0` is exactly
    // G498's owner-publish dispatch (its two call sites pin one lane); G418's own publication site
    // passes nothing. Behaviour-pure and default-off.
    if (g672Stat)
        g672Note(lanesOverride > 0 ? 2 : 0,
                 g672PairGrp.load(std::memory_order_relaxed),
                 g672ScalarGrp.load(std::memory_order_relaxed),
                 g672OddRows.load(std::memory_order_relaxed));
}

// DC2_G418_PROFILE aggregate for the CPU-copy component, keyed by target.
struct G418UnpackStat
{
    uint64_t n = 0, rows = 0, pixels = 0, ns = 0;
};
static std::map<uint32_t, G418UnpackStat> s_g418Unpacks; // flush-owner thread only
static uint64_t s_g418UnpackTotal = 0u;

static void g418NoteUnpack(uint32_t fbp, int rows, int fbW, uint64_t ns)
{
    G418UnpackStat &s = s_g418Unpacks[fbp];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.pixels += static_cast<uint64_t>(rows) * static_cast<uint64_t>(fbW);
    s.ns += ns;
    if ((++s_g418UnpackTotal % 300u) != 0u)
        return;
    std::fprintf(stderr, "[G418:unpack] total=%llu fast=%d par=%d lanes=%d",
                 static_cast<unsigned long long>(s_g418UnpackTotal),
                 g418FastUnpackOn() ? 1 : 0, g418ParUnpackOn() ? 1 : 0,
                 g418UnpackLanes());
    for (const auto &kv : s_g418Unpacks)
    {
        const G418UnpackStat &e = kv.second;
        if (e.n == 0u)
            continue;
        std::fprintf(stderr, " fbp=0x%x(n=%llu rows=%.1f Mpx=%.3f cpu=%.3f)",
                     static_cast<unsigned>(kv.first),
                     static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.rows) / e.n,
                     static_cast<double>(e.pixels) / 1.0e6 / e.n,
                     static_cast<double>(e.ns) / 1.0e6 / e.n);
    }
    std::fprintf(stderr, "\n");
}

// DC2_G418_VERIFY: re-derive every published pixel through the untouched serial address path and
// compare the bytes actually in VRAM. Diagnostic-only; it repeats the address work deliberately.
static void g418VerifyUnpack(const uint8_t *vram, uint32_t vramSize,
                             const std::vector<uint32_t> &src,
                             uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                             int rowLo, int rowHi)
{
    uint64_t bad = 0u;
    for (int y = rowLo; y <= rowHi; ++y)
    {
        const size_t srcRow = static_cast<size_t>(fbH - 1 - y) * fbW;
        for (int x = 0; x < fbW; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                        static_cast<uint32_t>(x),
                                                        static_cast<uint32_t>(y));
            if (off + 4u > vramSize)
                continue;
            uint32_t got = 0u;
            std::memcpy(&got, vram + off, 4);
            if (got != src[srcRow + x])
                ++bad;
        }
    }
    static std::atomic<uint64_t> s_n{0u}, s_px{0u}, s_bad{0u};
    const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px = static_cast<uint64_t>(rowHi - rowLo + 1) *
                        static_cast<uint64_t>(fbW);
    const uint64_t totalPx = s_px.fetch_add(px, std::memory_order_relaxed) + px;
    const uint64_t totalBad = s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || n <= 8u || (n % 256u) == 0u)
        std::fprintf(stderr,
                     "[G418:verify] unpacks=%llu pixels=%llu bad=%llu last(rows=%d bad=%llu)\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(totalPx),
                     static_cast<unsigned long long>(totalBad),
                     rowHi - rowLo + 1, static_cast<unsigned long long>(bad));
}
