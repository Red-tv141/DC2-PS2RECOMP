// G420 — the two staging-pack residues G419 left untested, plus the row-pool dispatch mechanism
// they turned out to depend on.
//
// G419 closed the pack's own loop cost (1.032 -> 0.371 ms/frame) and left two things measured but
// untouched:
//
//   1. THE ZERO-FILL. Every framebuffer upload starts with
//        s_batch.fbPixels.assign(fbW * kFbH, 0u)
//      which is 1 MiB for each 512-wide target and 256/128 KiB for the small ones. The pack then
//      immediately overwrites EVERY pixel of the guest rows it covers, so that fraction of the
//      zero-fill is pure churn. Zeroing only the COMPLEMENT of the packed window is exact by
//      construction: each staging element is written exactly once either way, and the complement
//      keeps the same zero value it had before.
//
//      Two implementation facts make this non-trivial:
//        * The !uploadFb path calls s_batch.fbPixels.clear(), which drops size() to 0. A later
//          resize() back to `total` then value-initialises the WHOLE buffer, i.e. the legacy cost
//          reappears. So the elision needs the staging buffer RETAINED across batches.
//        * Targets alternate widths inside one frame (512 / 128 / 64), so one retained buffer would
//          miss on every switch. A small size-keyed park keeps one buffer per geometry; a swap both
//          takes the right one and parks the outgoing one under its own size.
//      Nothing outside `uploadFb` reads fbPixels (the backend gates every use on b.uploadFb; the
//      G299 packet-byte counter is a diagnostic and is gated too), so retaining stale bytes there
//      is inert.
//
//   2. THE LANE COUNT. G419 measured GSRowPool dispatch at a fixed ~0.085 ms for 8 lanes and asked
//      whether 4 lanes beats 8 on the 213k-229k-pixel targets. Reading GSRowPool::run answers the
//      shape of that question before any measurement: the dispatch sets
//        m_pending = m_threads.size()
//      and notify_all()s EVERY worker, and run() then blocks until every one of them has woken,
//      taken the mutex, found no lane left, and reported. The lane count controls how the rows are
//      SPLIT, not how many threads the dispatch wakes or waits for — so lowering it cannot remove
//      dispatch cost, and the ~0.085 ms is a thundering-herd/convoy figure, not a per-lane one.
//      kG420LeverPool is the exact narrowing that follows from that: wait only for the workers that
//      actually took a lane. See the guard note in rasterizer_row_pool.inc.
//      SCOPE: GSRowPool has eight dispatch sites (pack, G418 unpack, T8 decode, tile-bin replay,
//      triangle bands, alias page view, two Z swizzles), so this lever moves ALL of them, not just
//      the pack. That is the right scope for a dispatch-mechanism lever, but it means its A/B is
//      NOT attributable to the pack alone and it must be adjudicated on its own arm.
//
// Both behaviour levers are exact. Bring-up is default-OFF (the shipping path stays the immutable
// control arm); adjudication is the within-process randomized instrument from G419, never a
// separate-process pair.

// ---------------------------------------------------------------------------------------------
// 1. Exact staging zero-fill elision.

// DC2_G420_ZERO_SKIP=1 arms it, DC2_G420_NO_ZERO_SKIP=1 kills it.
static bool g420ZeroSkipOn()
{
    static const bool on = !envFlagEnabled("DC2_G420_NO_ZERO_SKIP") &&
                           !envFlagDisabled("DC2_G420_ZERO_SKIP") &&
                           envFlagEnabled("DC2_G420_ZERO_SKIP");
    return on;
}

// The elision needs a live buffer to elide against, so the !uploadFb path must stop dropping it.
// Retaining is itself cost-neutral for the legacy arm: assign() fills `total` elements whether the
// vector arrives empty-with-capacity or already sized. Retain whenever the elision can be selected
// this run, so an instrument arm never measures "candidate had a warm buffer, control did not".
static bool g420RetainStagingOn()
{
    static const bool armable = g420ZeroSkipOn() || g419AbLeverMatches(kG420LeverZero);
    return armable;
}

// One parked staging buffer per observed geometry. Six slots cover DC2's three upload widths with
// room to spare; a miss simply falls back to resize(), which is correct but pays the full zero.
static std::vector<uint32_t> s_g420Park[6]; // flush-owner thread only

// Prepare `buf` to hold fbW*fbH staging pixels for a pack of GS rows [packLo, packHi].
//
// Postcondition (identical for both arms): every element outside the packed rows is 0, and every
// element inside them is about to be overwritten by g419PackFbRows. GL row order is bottom-first,
// so GS row y is buffer row (fbH-1-y) and the packed window is rows [fbH-1-packHi, fbH-1-packLo].
//
// Returns true when the elision ran; *filledOut reports how many elements were actually zeroed
// (the census normalises the saving against `total`).
static bool g420PrepStaging(std::vector<uint32_t> &buf, int fbW, int fbH,
                            int packLo, int packHi, bool skip, uint64_t *filledOut)
{
    const size_t total = static_cast<size_t>(fbW) * static_cast<size_t>(fbH);
    const bool windowOk = fbW > 0 && fbH > 0 && packHi >= packLo &&
                          packLo >= 0 && packHi <= fbH - 1;
    if (!skip || !windowOk)
    {
        // Legacy, verbatim. Fail-closed: an absent or out-of-range pack window zeroes everything.
        buf.assign(total, 0u);
        if (filledOut != nullptr)
            *filledOut = total;
        return false;
    }
    if (buf.size() != total)
    {
        // Take this geometry's parked buffer if we have one (the swap parks the outgoing buffer
        // under its own size at the same time). Otherwise park the outgoing buffer into a free
        // slot so the NEXT visit to its geometry finds it — without this, DC2's per-frame
        // 512/128/64-wide rotation would resize the one buffer every time and never elide.
        int want = -1, free = -1;
        for (int i = 0; i < 6; ++i)
        {
            if (want < 0 && s_g420Park[i].size() == total)
                want = i;
            if (free < 0 && s_g420Park[i].empty())
                free = i;
        }
        if (want >= 0)
            buf.swap(s_g420Park[want]);
        else if (free >= 0 && !buf.empty())
            buf.swap(s_g420Park[free]); // buf becomes empty; resize() below zeroes all of it
    }
    const size_t oldSize = buf.size();
    if (oldSize != total)
        buf.resize(total); // growth value-initialises [oldSize, total) to zero for us
    // Only elements the vector already held can carry stale bytes; anything resize() appended is
    // already zero and must not be re-filled.
    const size_t stale = std::min(oldSize, total);
    const size_t w = static_cast<size_t>(fbW);
    const size_t headEnd = static_cast<size_t>(fbH - 1 - packHi) * w; // rows before the packed span
    const size_t tailBeg = static_cast<size_t>(fbH - packLo) * w;     // rows after  the packed span
    uint32_t *p = buf.data();
    size_t filled = 0;
    {
        const size_t e = std::min(headEnd, stale);
        if (e > 0)
        {
            std::fill(p, p + e, 0u);
            filled += e;
        }
    }
    if (tailBeg < stale)
    {
        std::fill(p + tailBeg, p + stale, 0u);
        filled += stale - tailBeg;
    }
    if (filledOut != nullptr)
        *filledOut = filled;
    return true;
}

// DC2_G419_PROFILE census for the zero-fill, keyed exactly like [G419:pack] so the two tables line
// up. `msF` is this target's whole per-frame zero cost; `savedF` is the elements the elision did
// NOT have to write, per frame.
struct G420ZeroStat
{
    uint64_t n = 0, total = 0, filled = 0, ns = 0;
};
static std::map<uint64_t, G420ZeroStat> s_g420Zeros; // flush-owner thread only
static uint64_t s_g420ZeroTotal = 0u;

static void g420NoteZero(uint32_t fbp, uint32_t fbw, uint64_t totalPx, uint64_t filledPx,
                         uint64_t ns)
{
    const uint64_t key = (static_cast<uint64_t>(fbp) << 8) | (fbw & 0xffu);
    G420ZeroStat &s = s_g420Zeros[key];
    ++s.n;
    s.total += totalPx;
    s.filled += filledPx;
    s.ns += ns;
    if ((++s_g420ZeroTotal % 512u) != 0u)
        return;
    const double frames = s_g419FrameTicks != 0u ? static_cast<double>(s_g419FrameTicks) : 1.0;
    double sumMsF = 0.0;
    uint64_t sumTotal = 0u, sumFilled = 0u;
    std::fprintf(stderr, "[G420:zero] total=%llu frames=%llu skip=%d",
                 static_cast<unsigned long long>(s_g420ZeroTotal),
                 static_cast<unsigned long long>(s_g419FrameTicks),
                 g420ZeroSkipOn() ? 1 : 0);
    for (const auto &kv : s_g420Zeros)
    {
        const G420ZeroStat &e = kv.second;
        if (e.n == 0u)
            continue;
        const double msF = static_cast<double>(e.ns) / 1.0e6 / frames;
        sumMsF += msF;
        sumTotal += e.total;
        sumFilled += e.filled;
        std::fprintf(stderr, " fbp=0x%x/w%u(n=%llu perF=%.2f KiB=%.1f wrote=%.1f%% msF=%.3f)",
                     static_cast<unsigned>(kv.first >> 8),
                     static_cast<unsigned>(kv.first & 0xffu),
                     static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.n) / frames,
                     static_cast<double>(e.total) * 4.0 / 1024.0 / e.n,
                     e.total != 0u ? 100.0 * static_cast<double>(e.filled) /
                                         static_cast<double>(e.total)
                                   : 0.0,
                     msF);
    }
    std::fprintf(stderr, " TOTmsF=%.3f wrote=%.1f%%\n", sumMsF,
                 sumTotal != 0u
                     ? 100.0 * static_cast<double>(sumFilled) / static_cast<double>(sumTotal)
                     : 0.0);
}

// ---------------------------------------------------------------------------------------------
// 2. Pack row-pool lane count.

// DC2_G420_PACK_LANES=<n> is the candidate lane count (default 4 against G298's 8).
static int g420PackLanesAlt()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G420_PACK_LANES");
        if (value == nullptr)
            return 4;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    return n;
}

// DC2_G420_ALT_LANES=1 arms the alternate count outside the instrument.
static bool g420AltLanesOn()
{
    static const bool on = envFlagEnabled("DC2_G420_ALT_LANES") &&
                           !envFlagEnabled("DC2_G420_NO_ALT_LANES");
    return on;
}

// MUST be called unconditionally at the pack site, never inside a ternary or a short-circuiting
// chain: the instrument charges a frame period to the arm a CONSUMER reported, so a skipped call
// silently drops the interval instead of charging it (G419).
static int g420PackLaneCount(int requested)
{
    const bool alt = g419AbArmFor(kG420LeverPLanes, g420AltLanesOn());
    if (!alt || requested <= 1)
        return requested;
    return std::min(requested, g420PackLanesAlt());
}

// ---------------------------------------------------------------------------------------------
// 3. GSRowPool dispatch narrowing (see rasterizer_row_pool.inc for the implementation and its
//    generation-safety guard). DC2_G420_POOL_NARROW=1 arms it, DC2_G420_NO_POOL_NARROW=1 kills it.
static bool g420PoolNarrowOn()
{
    static const bool on = !envFlagEnabled("DC2_G420_NO_POOL_NARROW") &&
                           !envFlagDisabled("DC2_G420_POOL_NARROW") &&
                           envFlagEnabled("DC2_G420_POOL_NARROW");
    return on;
}

static bool g420PoolNarrowArm()
{
    return g419AbArmFor(kG420LeverPool, g420PoolNarrowOn());
}
