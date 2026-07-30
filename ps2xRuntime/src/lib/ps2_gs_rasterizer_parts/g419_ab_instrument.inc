// G419 — generalized within-process randomized A/B instrument + the exact CT32 upload fast pack.
//
// WHY THIS EXISTS. G418 proved that separate-process lean pairs on this route carry a +-1.5 ms
// run-level offset, which cannot adjudicate a sub-millisecond mechanism: five alternating pairs
// reported the two arms of the SAME mechanism with opposite signs. That is what forced G417 to
// retire an exact optimization it could not separate, and what made G414/G415 shelve exact levers as
// "below the timing floor". G418's fix was an instrument bolted onto one call site. This part lifts
// it into a lever-agnostic instrument so any shelved lever can be re-adjudicated:
//
//   * ONE fixed sample point per frame. Exactly one colour-readback unpack happens per frame, so the
//     interval between consecutive unpacks IS a frame period sampled at a fixed point in the frame.
//     g419AbTick() is called there and nowhere else.
//   * THE ARM IS CHOSEN AT THE TICK, but CHARGED BY THE CONSUMER. A consumer records which arm it
//     actually used (g_g419AbUsed); the next tick charges the elapsed interval to that arm and
//     clears the record. An interval with no consumer, or with both arms observed, is dropped. This
//     makes attribution correct no matter which thread hosts the lever or how deep the host pipeline
//     is - the period is charged to the arm that demonstrably served it. G418's design charged the
//     arm it PICKED, which is only equivalent for a same-thread, zero-lag consumer.
//   * PER-FRAME RANDOMISATION, not block alternation. Fixed-parity block alternation aliases with the
//     route's own periodicity: G418 measured its promoted arm as 0.79 ms SLOWER at block=60 and the
//     sign flipped under parity inversion. The arm comes from a fixed LCG, which cannot alias.
//   * WARM-UP SKIP. Whichever bucket owns the first frames inherits the route's load-in cost.
//   * BLOCKED ESTIMATOR IN-PROCESS. Both arm means drift together, so the difference of per-window
//     means is far tighter than the pooled SE (G418: 0.077 vs 0.120 ms). G418 computed this by hand
//     from the periodic prints; it is now printed directly, with its t statistic.
//   * BALLAST. Both buckets run the CONTROL form and one bucket repeats it N extra times
//     (idempotent). measured_delta / known_added_cost is the site's critical-path sensitivity.
//     Multiply an isolated saving by it BEFORE predicting a payoff - this is the step G414's and
//     G417's inclusive-timer deletions were missing.
//
// Diagnostic-only: it deliberately runs both arms, so DC2_G419_AB is never a shipping mode.
//
// -----------------------------------------------------------------------------------------------
// THE EXACT CT32 UPLOAD FAST PACK (the lever this phase re-adjudicates on the upload side).
//
// The framebuffer-upload staging pack is the mirror image of G418's readback unpack: for every
// guest row it walks x with a full addrPSMCT32() call plus a 4-byte memcpy out of VRAM. G418's
// decomposition applies unchanged, because it is a property of addrPSMCT32 and not of a direction:
//
//     addr(x,y) == (((block>>5) + (y>>5)*width) << 13) + ((x>>6) << 13) + tbl[y&31][x&63]
//
// with tbl[yl][xl] := addrPSMCT32(block, width, xl, yl) - ((block>>5)<<13). The only x term above
// bit 5 is the linear (x>>6) page step; the only y terms are the linear (y>>5) page step and bits
// 0..4 feeding blockTable32/columnTable32, all of which the 32x64 table covers exhaustively. The
// table is literally the function restricted to one page, so nothing is approximated - including a
// non-zero (block&31), whose page carry is already baked into every table entry.
//
// PAIRED LOADS. columnTable32[y&7][x&7] for an even x and x+1 differ by exactly one word and share
// (x>>3)/(x>>6), so the two source pixels are contiguous in VRAM: an even-width row reads as 8-byte
// loads. The partial-bound cases reproduce the original per-pixel behaviour exactly (see below).
//
// WHY A MULTI-ENTRY TABLE CACHE. G418's single-entry cache is correct for the readback, which only
// ever sees fbp=0x13b at one width. Uploads visit fbp=0x139 at BOTH fbw=8 and fbw=2 plus 0x0, 0x68
// and several singletons in the same frame, so a single-entry cache would rebuild 2048 addresses per
// upload and eat the win. A small direct cache keyed by (block,width) makes every steady-state
// lookup a hit. Flush-owner thread only, exactly like G418's - the lanes only read it, after
// GSRowPool::run has published it through the dispatch.

// The shared arm register (g_g419AbArm / g_g419AbUsed) is defined in
// ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc and declared at global scope in
// rasterizer_headers_and_diagnostics.inc:
//   g_g419AbArm  : 0 = control, 1 = candidate. Written only by the tick.
//   g_g419AbUsed : bit0 = a consumer ran the control form, bit1 = the candidate form. Written by
//                  consumers, cleared by the tick.

enum G419Lever
{
    kG419LeverNone = 0,
    kG419LeverNarrow = 1, // DC2_G415_NARROW_READBACK  (GL backend, colour readback rows)
    kG419LeverPack = 2,   // the exact CT32 upload pack (GS front-end, this file)
    kG419LeverLanes = 3,  // pixel-gated row-pool fan-out for the upload pack (this file)
    kG419LeverAll = 4,    // G419's three together == the G419 shipping stack (its promotion gate)
    // G420 levers. `all420` is their combined arm; `all` deliberately still means G419's stack, so
    // the two phases' promotion gates never blur into one another.
    kG420LeverZero = 5,   // exact staging zero-fill elision (g420_pack_residue.inc)
    kG420LeverPLanes = 6, // pack row-pool LANE COUNT (alt vs G298's 8)
    kG420LeverPool = 7,   // GSRowPool dispatch narrowing (wait only for lanes that took work)
    kG420LeverAll = 8,    // the three G420 BEHAVIOUR levers (narrowtouch is diagnostic, excluded)
    // MECHANISM PROBE, never a behaviour lever and never in a combined arm: the candidate bucket
    // writes the complement rows the FULL readback would have written, into an inert scratch
    // buffer. It adds the full read's destination-side memory traffic WITHOUT its GL transfer, so
    // it measures the leading hypothesis for G419's unaccounted narrow-readback payoff directly.
    kG420LeverNTouch = 9,
};

// DC2_G419_AB=<lever> selects the ONE lever the instrument alternates. Exactly one at a time:
// a phase must keep one architectural variable per arm, and a shared arm register cannot attribute
// two levers from one period series.
static int g419AbLever()
{
    static const int lever = [] {
        const char *value = std::getenv("DC2_G419_AB");
        if (value == nullptr || value[0] == '\0')
            return static_cast<int>(kG419LeverNone);
        if (std::strcmp(value, "narrow") == 0)
            return static_cast<int>(kG419LeverNarrow);
        if (std::strcmp(value, "pack") == 0)
            return static_cast<int>(kG419LeverPack);
        if (std::strcmp(value, "lanes") == 0)
            return static_cast<int>(kG419LeverLanes);
        if (std::strcmp(value, "all") == 0)
            return static_cast<int>(kG419LeverAll);
        if (std::strcmp(value, "zero") == 0)
            return static_cast<int>(kG420LeverZero);
        if (std::strcmp(value, "packlanes") == 0)
            return static_cast<int>(kG420LeverPLanes);
        if (std::strcmp(value, "pool") == 0)
            return static_cast<int>(kG420LeverPool);
        if (std::strcmp(value, "all420") == 0)
            return static_cast<int>(kG420LeverAll);
        if (std::strcmp(value, "narrowtouch") == 0)
            return static_cast<int>(kG420LeverNTouch);
        std::fprintf(stderr,
                     "[G419:ab] unknown DC2_G419_AB='%s' (expected "
                     "narrow|pack|lanes|all|zero|packlanes|pool|all420|narrowtouch)\n", value);
        return static_cast<int>(kG419LeverNone);
    }();
    return lever;
}

static const char *g419AbLeverName(int lever)
{
    switch (lever)
    {
    case kG419LeverNarrow: return "narrow";
    case kG419LeverPack: return "pack";
    case kG419LeverLanes: return "lanes";
    case kG419LeverAll: return "all";
    case kG420LeverZero: return "zero";
    case kG420LeverPLanes: return "packlanes";
    case kG420LeverPool: return "pool";
    case kG420LeverAll: return "all420";
    case kG420LeverNTouch: return "narrowtouch";
    default: return "none";
    }
}

// DC2_G419_BALLAST=<repeats> — critical-path sensitivity probe. BOTH buckets run the control form;
// the candidate bucket additionally repeats the site's work <repeats> times. The repeat must be
// idempotent (it rewrites identical bytes), so the only thing measured is how much of a KNOWN added
// cost at that exact site reaches the frame.
static int g419AbBallast()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G419_BALLAST");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(16, std::atoi(value)));
    }();
    return n;
}

static uint64_t g419AbWarmFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WARM");
        if (value == nullptr)
            return 120;
        return std::max(0, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Blocked-estimator window length in ticks. Both arm means drift together over a run, so a window
// short enough that the drift is common to both buckets differences it away.
static uint64_t g419AbWindow()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WINDOW");
        if (value == nullptr)
            return 600;
        return std::max(60, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Consumer-side arm selector. Returns the behaviour this call should use and records which bucket
// actually ran so the next tick can charge the interval to it.
//   * not the selected lever -> the promoted default, and nothing is recorded (zero instrument cost
//     on the shipping path beyond one cached-int compare);
//   * ballast mode -> the CONTROL form for both buckets (the extra work is added by the site).
// `all` alternates every lever at once, which is the promotion gate for the shipping stack: the three
// levers act on different threads, so only a combined arm answers "does the promoted default pay".
static bool g419AbLeverMatches(int lever)
{
    const int sel = g419AbLever();
    if (sel == lever)
        return true;
    // The two combined arms are each restricted to their OWN phase's levers. `all` must keep
    // meaning "the G419 shipping stack" so its promotion gate stays reproducible.
    if (sel == kG419LeverAll)
        return lever >= kG419LeverNarrow && lever <= kG419LeverLanes;
    if (sel == kG420LeverAll)
        return lever >= kG420LeverZero && lever <= kG420LeverPool;
    return false;
}

static bool g419AbArmFor(int lever, bool defaultOn)
{
    if (!g419AbLeverMatches(lever))
        return defaultOn;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    if (g419AbBallast() > 0)
        return false;
    return arm != 0u;
}

// How many extra idempotent repetitions this frame's site should perform (ballast mode only).
static int g419AbBallastReps(int lever)
{
    const int n = g419AbBallast();
    if (n <= 0 || !g419AbLeverMatches(lever))
        return 0;
    return g_g419AbArm.load(std::memory_order_relaxed) != 0u ? n : 0;
}

// Frames observed at the instrument's sample point. Counted unconditionally (one increment per
// frame) so every G419 per-batch census can be normalised to a real per-frame figure. Guessing the
// divisor is how a per-batch cost turns into a wrong payoff prediction: DC2 issues ~1.7 uploads of
// fbp=0x139 per frame, not one, so a plausible-looking "one per frame" assumption was off by 70%.
static uint64_t s_g419FrameTicks = 0u;

static void g419NoteFrameTick()
{
    ++s_g419FrameTicks;
}

// Once-per-frame tick at the colour-readback unpack. Charges the elapsed interval to whichever arm a
// consumer reported, then draws the next frame's arm.
static void g419AbTick()
{
    if (g419AbLever() == kG419LeverNone)
        return;
    static std::chrono::steady_clock::time_point s_last{};
    static bool s_haveLast = false;
    static uint64_t s_frame = 0u;
    static uint64_t s_lcg = 0x9E3779B97F4A7C15ull;
    // [0] = candidate, [1] = control.
    static double s_sum[2]{}, s_sq[2]{};
    static uint64_t s_n[2]{};
    static double s_winSum[2]{};
    static uint64_t s_winN[2]{};
    static double s_blkSum = 0.0, s_blkSq = 0.0;
    static uint64_t s_blkN = 0u, s_blkFav = 0u, s_dropped = 0u;

    const auto now = std::chrono::steady_clock::now();
    const uint32_t used = g_g419AbUsed.exchange(0u, std::memory_order_relaxed);
    const uint64_t idx = s_frame++;
    if (s_haveLast && idx >= g419AbWarmFrames())
    {
        const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
        // An interval with no consumer (used==0) or with both arms observed (used==3) cannot be
        // attributed. Frame periods are wall-clock, so clip the disc-read / transition outliers that
        // no mean survives (same 200 ms clip G418 used against this route's ~66 ms steady frame).
        if ((used == 1u || used == 2u) && ms < 200.0)
        {
            const int a = (used == 2u) ? 0 : 1;
            s_sum[a] += ms;
            s_sq[a] += ms * ms;
            ++s_n[a];
            s_winSum[a] += ms;
            ++s_winN[a];
        }
        else
        {
            ++s_dropped;
        }
    }
    s_last = now;
    s_haveLast = true;

    s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull;
    g_g419AbArm.store(((s_lcg >> 33u) & 1ull) != 0ull ? 1u : 0u, std::memory_order_relaxed);

    const uint64_t window = g419AbWindow();
    if (idx == 0u || (idx % window) != 0u)
        return;
    // Close the blocked window. Require a real paired sample count in both buckets so a window the
    // route spent loading or stalled in cannot contribute a spurious difference.
    if (s_winN[0] >= 8u && s_winN[1] >= 8u)
    {
        const double d = s_winSum[0] / static_cast<double>(s_winN[0]) -
                         s_winSum[1] / static_cast<double>(s_winN[1]);
        s_blkSum += d;
        s_blkSq += d * d;
        ++s_blkN;
        if (d < 0.0)
            ++s_blkFav;
    }
    s_winSum[0] = s_winSum[1] = 0.0;
    s_winN[0] = s_winN[1] = 0u;

    const auto mean = [](double sum, uint64_t n) {
        return n != 0u ? sum / static_cast<double>(n) : 0.0;
    };
    const double c = mean(s_sum[0], s_n[0]), k = mean(s_sum[1], s_n[1]);
    const auto sem = [&](int a, double m) {
        const uint64_t n = s_n[a];
        if (n < 2u)
            return 0.0;
        const double var =
            (s_sq[a] - static_cast<double>(n) * m * m) / static_cast<double>(n - 1u);
        return var > 0.0 ? std::sqrt(var / static_cast<double>(n)) : 0.0;
    };
    const double sc = sem(0, c), sk = sem(1, k);
    double blkMean = 0.0, blkSe = 0.0, blkT = 0.0;
    if (s_blkN >= 2u)
    {
        blkMean = s_blkSum / static_cast<double>(s_blkN);
        const double var = (s_blkSq - static_cast<double>(s_blkN) * blkMean * blkMean) /
                           static_cast<double>(s_blkN - 1u);
        blkSe = var > 0.0 ? std::sqrt(var / static_cast<double>(s_blkN)) : 0.0;
        blkT = blkSe > 0.0 ? blkMean / blkSe : 0.0;
    }
    std::fprintf(stderr,
                 "[G419:ab] lever=%s ballast=%d frames=%llu drop=%llu "
                 "cand(n=%llu ms=%.3f se=%.3f) ctrl(n=%llu ms=%.3f se=%.3f) "
                 "delta=%.3f (%.2f%%) seDelta=%.3f "
                 "blocked(w=%llu mean=%.3f se=%.3f t=%.2f fav=%llu)\n",
                 g419AbLeverName(g419AbLever()),
                 g419AbBallast(),
                 static_cast<unsigned long long>(idx),
                 static_cast<unsigned long long>(s_dropped),
                 static_cast<unsigned long long>(s_n[0]), c, sc,
                 static_cast<unsigned long long>(s_n[1]), k, sk,
                 c - k, k != 0.0 ? 100.0 * (c - k) / k : 0.0,
                 std::sqrt(sc * sc + sk * sk),
                 static_cast<unsigned long long>(s_blkN), blkMean, blkSe, blkT,
                 static_cast<unsigned long long>(s_blkFav));
}

// ---------------------------------------------------------------------------------------------
// Exact CT32 upload fast pack.

// G419 PROMOTED to default-ON (2026-07-30). Exact by construction (the table IS addrPSMCT32
// restricted to one page, paired loads rest on the one-word columnTable32 adjacency) and by oracle
// (643,149,824 pixels, bad=0). Isolated cost 1.032 -> 0.767 ms/frame against G298's row-gated lanes
// and 0.730 -> 0.371 against the pixel gate; within-process randomized A/B over 6,000 MAP-0 frames
// measured -0.426 ms/frame (blocked -0.394, se 0.169, t = -2.33, 7/10 windows favouring).
// Kill with DC2_G419_NO_FAST_PACK=1 or DC2_G419_FAST_PACK=0.
static bool g419FastPackOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_FAST_PACK") &&
                           !envFlagDisabled("DC2_G419_FAST_PACK");
    return on;
}

// G419 pixel-gated row-pool fan-out for the upload pack. PROMOTED to default-ON (2026-07-30):
// isolated 1.032 -> 0.730 ms/frame against the legacy pack and 0.767 -> 0.371 against the fast pack,
// and within-process randomized A/B over 5,400 MAP-0 frames measured -0.320 ms/frame
// (blocked -0.303, se 0.137, t = -2.22, 6/9 windows favouring).
// Kill with DC2_G419_NO_PACK_LANE_GATE=1 or DC2_G419_PACK_LANE_GATE=0.
//
// G298 gates the pack's row-pool fan-out on `packRows >= 8`. Rows cannot express the work: DC2's
// small upload targets pack 128 rows of a 128-pixel target (16k pixels) or 64 rows of a 64-pixel one
// (4k pixels), which clear a row gate while carrying 1/13th to 1/53rd of a 416x512 pack. Measured on
// the current binary (DC2_G419_PROFILE, exact fast pack, same call counts):
//
//     target      geometry    8 lanes   1 lane
//     0x139/w8    448x512     0.102     0.152   <- fan-out pays
//     0x0/w8      416x512     0.102     0.134   <- pays
//     0x68/w8     416x512     0.096     0.137   <- pays
//     0x139/w2    128x128     0.083     0.012   <- fan-out costs 7x the whole loop
//     0x13c/w2    128x128     0.076     0.011
//     0x146/w2    128x128     0.077     0.013
//     0x143/w1     64x64      0.079     0.004
//     0x155/w1     64x64      0.076     0.004
//
// So GSRowPool dispatch is a fixed ~0.085 ms on this route (8 lane wake-ups + join). Break-even for
// an 8-lane split is therefore W > D*8/7, i.e. about 0.097 ms of serial work, which at the measured
// 0.63 ns/pixel is about 154k pixels. The DC2 population is bimodal (213k-229k vs 4k-16k), so any
// threshold in the gap is safe; 65,536 stays well clear of both clusters.
//
// EXACT BY CONSTRUCTION: lane count never changes results. G298's contract is that disjoint guest
// row ranges own disjoint VRAM/staging words, so one lane is simply the serial reference. Verified
// by DC2_G298_VERIFY_FBPACK / DC2_G419_VERIFY, which re-derive every packed pixel serially.
static bool g419PackLaneGateOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_PACK_LANE_GATE") &&
                           !envFlagDisabled("DC2_G419_PACK_LANE_GATE");
    return on;
}

static uint64_t g419PackLaneMinPixels()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_LANE_MIN_PX");
        if (value == nullptr)
            return 65536;
        return std::max(0, std::atoi(value));
    }();
    return n;
}

// `requested` is G298's existing lane count (already row-gated). Returns the lane count to dispatch.
static int g419PackLanes(int packRows, int fbW, int requested)
{
    if (requested <= 1 || packRows <= 0 || fbW <= 0)
        return requested;
    if (!g419AbArmFor(kG419LeverLanes, g419PackLaneGateOn()))
        return requested;
    const uint64_t pixels = static_cast<uint64_t>(packRows) * static_cast<uint64_t>(fbW);
    return pixels >= g419PackLaneMinPixels() ? requested : 1;
}

// Behaviour-pure isolated timer for the pack loop, keyed by target/width. Unlike DC2_G417_PROFILE
// this arms NOTHING in the GL backend, so it does not insert a finish and does not perturb
// scheduling - it is safe to read alongside a normal run.
static bool g419ProfileOn()
{
    static const bool on = envFlagEnabled("DC2_G419_PROFILE");
    return on;
}

struct G419Ct32Page
{
    uint32_t block = 0u;
    uint32_t width = 0u;
    bool valid = false;
    uint32_t off[32][64]{};
};

// Uploads visit several (block,width) pairs per frame (fbp=0x139 at fbw=8 AND fbw=2, plus 0x0,
// 0x68 and singletons), so a single-entry cache would rebuild 2048 addresses per upload. Twelve
// slots cover the observed DC2 population with round-robin replacement as the fail-safe.
static G419Ct32Page s_g419Ct32[12];
static uint32_t s_g419Ct32Next = 0u;

static const G419Ct32Page &g419Ct32Table(uint32_t block, uint32_t width)
{
    const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
    for (G419Ct32Page &e : s_g419Ct32)
        if (e.valid && e.block == block && e.width == pagesPerRow)
            return e;
    G419Ct32Page &e = s_g419Ct32[s_g419Ct32Next % 12u];
    ++s_g419Ct32Next;
    const uint32_t base = (block >> 5u) << 13u;
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 64u; ++x)
            e.off[y][x] = GSPSMCT32::addrPSMCT32(block, pagesPerRow, x, y) - base;
    e.block = block;
    e.width = pagesPerRow;
    e.valid = true;
    return e;
}

// Pack guest rows [laneLo,laneHi] of a CT32 target out of VRAM into the bottom-first staging buffer.
// `legacy` runs the original serial per-pixel path verbatim as the control arm.
static void g419PackFbRows(uint32_t *dst, size_t dstCount,
                           const uint8_t *vram, uint32_t vramSize,
                           uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                           int laneLo, int laneHi, bool legacy)
{
    if (dst == nullptr || vram == nullptr || laneHi < laneLo || fbW <= 0)
        return;
    if (legacy || (fbW & 1) != 0)
    {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
            if (dstRow + static_cast<size_t>(fbW) > dstCount)
                continue;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                           static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y));
                uint32_t px = 0u;
                if (off + 4u <= vramSize)
                    std::memcpy(&px, vram + off, 4);
                dst[dstRow + x] = px;
            }
        }
        return;
    }
    const G419Ct32Page &tbl = g419Ct32Table(fbBlock, fbw);
    const uint32_t pagesPerRow = (fbw != 0u) ? fbw : 1u;
    const uint32_t basePage = fbBlock >> 5u;
    for (int y = laneLo; y <= laneHi; ++y)
    {
        const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
        if (dstRow + static_cast<size_t>(fbW) > dstCount)
            continue;
        const uint32_t *rowOff = tbl.off[static_cast<uint32_t>(y) & 31u];
        const uint32_t yBase =
            (basePage + (static_cast<uint32_t>(y) >> 5u) * pagesPerRow) << 13u;
        uint32_t *dstPx = dst + dstRow;
        for (int x = 0; x < fbW; x += 2)
        {
            const uint32_t off = yBase +
                                 (static_cast<uint32_t>(x >> 6) << 13u) +
                                 rowOff[static_cast<uint32_t>(x) & 63u];
            // Pixel x sits at off and pixel x+1 at off+4 (one-word columnTable32 adjacency), so the
            // original loop's per-pixel bound test maps onto exactly these three cases.
            if (off + 8u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 8);
            }
            else if (off + 4u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 4);
                dstPx[x + 1] = 0u; // the original wrote 0 when off+4 (here off+8) exceeded VRAM
            }
            else
            {
                dstPx[x] = 0u;
                dstPx[x + 1] = 0u;
            }
        }
    }
}

// DC2_G419_PROFILE aggregate for the isolated pack cost, keyed by target and staging geometry.
struct G419PackStat
{
    uint64_t n = 0, rows = 0, ns = 0;
};
static std::map<uint64_t, G419PackStat> s_g419Packs; // flush-owner thread only
static uint64_t s_g419PackTotal = 0u;

static void g419NotePack(uint32_t fbp, uint32_t fbw, int rows, uint64_t ns)
{
    const uint64_t key = (static_cast<uint64_t>(fbp) << 8) | (fbw & 0xffu);
    G419PackStat &s = s_g419Packs[key];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.ns += ns;
    if ((++s_g419PackTotal % 512u) != 0u)
        return;
    // `perF` is calls per FRAME and `msF` is this target's whole contribution to a frame. msF is the
    // only number a payoff prediction may use; per-call cpu is a mechanism figure.
    const double frames = s_g419FrameTicks != 0u ? static_cast<double>(s_g419FrameTicks) : 1.0;
    double sumMsF = 0.0;
    std::fprintf(stderr, "[G419:pack] total=%llu frames=%llu fast=%d",
                 static_cast<unsigned long long>(s_g419PackTotal),
                 static_cast<unsigned long long>(s_g419FrameTicks),
                 g419FastPackOn() ? 1 : 0);
    for (const auto &kv : s_g419Packs)
    {
        const G419PackStat &e = kv.second;
        if (e.n == 0u)
            continue;
        const double msF = static_cast<double>(e.ns) / 1.0e6 / frames;
        sumMsF += msF;
        std::fprintf(stderr, " fbp=0x%x/w%u(n=%llu perF=%.2f rows=%.1f cpu=%.3f msF=%.3f)",
                     static_cast<unsigned>(kv.first >> 8),
                     static_cast<unsigned>(kv.first & 0xffu),
                     static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.n) / frames,
                     static_cast<double>(e.rows) / e.n,
                     static_cast<double>(e.ns) / 1.0e6 / e.n,
                     msF);
    }
    std::fprintf(stderr, " TOTmsF=%.3f\n", sumMsF);
}
