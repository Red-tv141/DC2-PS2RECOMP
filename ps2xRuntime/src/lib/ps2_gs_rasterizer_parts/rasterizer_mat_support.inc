// ===== G662: split out of rasterizer_vram_materialization.inc (was 7,737 lines) =================
//
// WHY THIS FILE EXISTS. That include was 7.7k lines / 444 KB, and lines 1402-7682 of it are a
// SINGLE function - `g178TryFlushGpu`, 6,280 lines. Everything before that function is support:
// the G340 census, the G261 materialize-for-ranges entry points, the G285 local-to-local consume,
// the G264/G326 upload-mirror planner, the G661/G662 probes, the CPU-replay prologue, the G582
// dedup, the G305 async drain and the G581 snapshot invalidation. None of it is the flush, and all
// of it had to be scrolled past to reach the flush.
//
// ⛔ THIS IS A PURE TEXT MOVE AND MUST STAY ONE - see the header of lle_gpu_raster_prelude.inc for
// the full argument. Both halves are `#include`d into `ps2_gs_rasterizer.cpp` consecutively and in
// the original order, so preprocessor output is byte-identical.
//
// ⚠️ The flush function itself was deliberately NOT cut. Splitting a function body across includes
// is legal but makes the code harder to follow, not easier, and this one is the hottest path in
// the renderer.

// G340: classify one about-to-materialize target's dirty rows (the readback unit) by whether the
// triggering consumer actually READS them (src dirty-overlap => lazy cannot skip), only WRITES over
// them (dst dirty-overlap, no src => skippable if the write is total), or neither (materialize fired
// on the coarse full-target range only => pure conservative barrier, dead readback). Behavior-pure;
// reads r before g261Materialize clears it. See g340CensusOn() header in gpu_alias_page_view.inc.
static void g340CensusMaterialize(uint32_t ti, const G261Res &r, int cause,
                                  const G144BlockRange &src, const G144BlockRange &dst)
{
    if (!g340CensusOn() || !r.dirty || r.fbW <= 0)
        return;
    const int rows = r.dirtyHi - r.dirtyLo + 1;
    if (rows <= 0)
        return;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[ti]);
    const G144BlockRange dirtyRange = g144RangeForRect(
        fbBlock, GS_PSM_CT32, r.fbw, 0u, static_cast<uint32_t>(r.dirtyLo),
        static_cast<uint32_t>(r.fbW), static_cast<uint32_t>(rows));
    const uint64_t nrows = static_cast<uint64_t>(rows);
    g_g340Mats.fetch_add(1u, std::memory_order_relaxed);
    g_g340RowsTotal.fetch_add(nrows, std::memory_order_relaxed);
    g_g340CauseTotal[cause & 7].fetch_add(nrows, std::memory_order_relaxed);
    uint64_t skippable = 0u;
    if (!dirtyRange.valid)
        g_g340RowsUnknown.fetch_add(nrows, std::memory_order_relaxed);
    else if (src.valid && g144RangeOverlaps(dirtyRange, src))
        g_g340RowsConsumed.fetch_add(nrows, std::memory_order_relaxed);
    else if (dst.valid && g144RangeOverlaps(dirtyRange, dst))
    {
        g_g340RowsOverwrite.fetch_add(nrows, std::memory_order_relaxed);
        skippable = nrows;
    }
    else
    {
        g_g340RowsConserv.fetch_add(nrows, std::memory_order_relaxed);
        skippable = nrows;
    }
    g_g340CauseSkippable[cause & 7].fetch_add(skippable, std::memory_order_relaxed);
}
// G340: the fail-closed unknown-range path materializes EVERY dirty target; those rows are
// conservatively "needed" (we cannot prove non-consumption), counted in the unknown bucket.
static void g340CensusUnknownAllDirty(int cause)
{
    if (!g340CensusOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty || r.fbW <= 0)
            continue;
        const int rows = r.dirtyHi - r.dirtyLo + 1;
        if (rows <= 0)
            continue;
        const uint64_t nrows = static_cast<uint64_t>(rows);
        g_g340Mats.fetch_add(1u, std::memory_order_relaxed);
        g_g340RowsTotal.fetch_add(nrows, std::memory_order_relaxed);
        g_g340RowsUnknown.fetch_add(nrows, std::memory_order_relaxed);
        g_g340CauseTotal[cause & 7].fetch_add(nrows, std::memory_order_relaxed);
    }
}

static void g261MaterializeAllDirty(int cause)
{
    if (!g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        if (g_g261Res[i].dirty)
            g261Materialize(static_cast<int>(i), cause);
    // Bring-up observability: this route rarely reaches the 512-materialize / 1024-wave report
    // triggers in a short capture, so emit a periodic report at a drain-boundary cadence when a
    // stat lever is on. Default runs never call this branch (gate returns false).
    if (g280StatOn() || g283StatOn() || g309StatOn())
    {
        static std::atomic<uint64_t> s_n{0};
        if ((s_n.fetch_add(1u, std::memory_order_relaxed) % 240u) == 0u)
            g261Report();
    }
}

// A VRAM transfer is about to read `src` and/or write `dst`: materialize any resident target it
// touches BEFORE the caller mutates/reads guest bytes. Unknown ranges fail closed (all targets).
static void g261MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause,
                                     uint32_t g264SkipMask = 0u,
                                     bool g630GpuUploadPatch = false)
{
    // G630 owns the same physical 4 MiB address space. Resolve its raw-image authority before
    // any legacy range gate can early-return on "no G261 target dirty".
    if (!g630GpuUploadPatch)
        g630MaterializeForRanges(src, dst, srcUnknown, dstUnknown, cause);
    if (!g261WaveOn() || !g261AnyDirty())
        return;
    if (srcUnknown || dstUnknown)
    {
        g_g261MatUnknownRange.fetch_add(1u, std::memory_order_relaxed);
        g340CensusUnknownAllDirty(cause); // census before AllDirty clears r.dirty
        g261MaterializeAllDirty(cause);
        return;
    }
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        if (g264SkipMask & (1u << i))
            continue; // G264: this overlap was deferred to an upload mirror instead
        if (!r.range.valid ||
            (src.valid && g144RangeOverlaps(r.range, src)) ||
            (dst.valid && g144RangeOverlaps(r.range, dst)))
        {
            g340CensusMaterialize(i, r, cause, src, dst); // census before materialize clears r.dirty
            g261Materialize(static_cast<int>(i), cause);
        }
    }
}

// G264: examine a host→local upload rect BEFORE it writes VRAM. Any dirty resident target it
// overlaps either gets the rect deferred as a mirror (returned in the mask — the caller skips
// its materialize) or falls through to the normal materialize. Fail-closed eligibility: CT32,
// matching buffer width, page-row-aligned base inside the target, rows inside 0..511, and
// full-width whenever the rows intersect DIRTY rows (a column-partial rect inside wave-rendered
// rows cannot be reproduced from VRAM alone — VRAM lacks the wave output in the other columns).
// Rows outside the dirty interval are VRAM-authoritative, so whole-row mirrors are exact there.
// G285: a full-page CT32 local copy can destructively consume a dirty resident destination.
// Publish the one partially covered page-row, then let the caller replace every remaining dirty
// page. Admission is generic GS-page geometry, never screen/game state. Source overlap fails
// closed because source bytes must be published before the copy reads them. G280 overlays also
// fail closed because their independent page authority is a separate mechanism.
static uint32_t g285PrepareL2lConsume(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                     uint32_t ssax, uint32_t ssay,
                                     uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                     uint32_t dsax, uint32_t dsay,
                                     uint32_t rrw, uint32_t rrh,
                                     const G144BlockRange &src,
                                     const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown)
{
    static const bool s_on = !envFlagEnabled("DC2_G285_NO_L2L_PAGE_COPY") &&
                             !envFlagDisabled("DC2_G285_L2L_PAGE_COPY");
    static const bool s_stat = envFlagEnabled("DC2_G285_STAT");
    if (!s_on || !g261WaveOn() || g280AliasOn() || !g261AnyDirty() ||
        srcUnknown || dstUnknown || !src.valid || !dst.valid ||
        spsm != GS_PSM_CT32 || dpsm != GS_PSM_CT32 ||
        sbw == 0u || dbw == 0u || sbw != dbw ||
        (sbp & 31u) != 0u || (dbp & 31u) != 0u ||
        ssax != 0u || ssay != 0u || dsax != 0u || dsay != 0u ||
        rrw != static_cast<uint32_t>(dbw) * 64u ||
        rrh == 0u || (rrh & 31u) != 0u)
        return 0u;

    constexpr uint64_t kPageBytes = 8192ull;
    const uint64_t copyBytes = static_cast<uint64_t>(rrw) * rrh * 4ull;
    const uint64_t srcBase = static_cast<uint64_t>(sbp) * 256ull;
    const uint64_t dstBase = static_cast<uint64_t>(dbp) * 256ull;
    const uint64_t srcEnd = srcBase + copyBytes;
    const uint64_t dstEnd = dstBase + copyBytes;
    if (srcEnd > g_g261VramSize || dstEnd > g_g261VramSize ||
        !(srcEnd <= dstBase || dstEnd <= srcBase))
        return 0u;

    uint32_t consumedMask = 0u;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty || !r.range.valid || r.fbw != dbw ||
            r.fbW != static_cast<int>(rrw) ||
            (r.dirtyLo & 31) != 0 || ((r.dirtyHi + 1) & 31) != 0 ||
            r.dirtyHi - r.dirtyLo + 1 < 64 ||
            g144RangeOverlaps(r.range, src) || !g144RangeOverlaps(r.range, dst))
            continue;

        const uint64_t targetBase =
            static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[i])) * 256ull;
        const uint64_t pageRowBytes = static_cast<uint64_t>(r.fbw) * kPageBytes;
        const uint64_t dirtyStart =
            targetBase + static_cast<uint64_t>(r.dirtyLo / 32) * pageRowBytes;
        const uint64_t dirtyEnd =
            targetBase + static_cast<uint64_t>((r.dirtyHi + 1) / 32) * pageRowBytes;
        // Begin page-aligned inside the first dirty row, cover the rest of that row, and reach
        // through the dirty suffix. Publishing the whole first row is safe bounded over-work.
        // G343 (DC2_G343_L2L_EXACT, default-off): also admit a dst beginning at any page-aligned
        // offset at/after the dirty start — publish every page-row the copy does not cover
        // (rounding a mid-page-row start UP to whole page-rows, same bounded over-publish rule),
        // consume the covered suffix. dstEnd must still reach through the dirty end.
        uint64_t pubPageRows = 1u;
        if (g343L2lExactOn())
        {
            if (dstBase < dirtyStart || dstBase >= dirtyEnd ||
                ((dstBase - dirtyStart) % kPageBytes) != 0u || dstEnd < dirtyEnd)
                continue;
            const uint64_t off = dstBase - dirtyStart;
            pubPageRows = (off + pageRowBytes - 1u) / pageRowBytes;
            if (pubPageRows == 0u)
                pubPageRows = 1u; // conservative floor: always publish the first page-row
        }
        else if (dstBase <= dirtyStart || dstBase >= dirtyStart + pageRowBytes ||
                 ((dstBase - dirtyStart) % kPageBytes) != 0u || dstEnd < dirtyEnd)
            continue;

        const int originalRows = r.dirtyHi - r.dirtyLo + 1;
        const int pubRows = static_cast<int>(pubPageRows * 32u);
        if (pubRows >= originalRows)
            continue; // nothing left to consume
        if (g343L2lExactOn())
            g_g343ConsumeAdmits.fetch_add(1u, std::memory_order_relaxed);
        // G346 consume-DEFER: REFUTED BY CENSUS (2026-07-23, kept as the record — do not rebuild).
        // Deferring the 32-row prefix publication (leave it dirty, publish nothing at this edge)
        // fired 0.5/frame on MAP-0 but REGRESSED the family readback side +13% rows: the deferred
        // dirty span regrows before the next forced publication edge, which then publishes ~97
        // rows instead of the eager 32 (famRb 381984R -> 432544R at matched grabs, famUp
        // unchanged). The eager prefix publish is the cheaper side of this edge. Behavior branch
        // removed; the [G345:escape] defer counters remain (read 0) as the census record.
        r.dirtyHi = r.dirtyLo + pubRows - 1;
        g261Materialize(static_cast<int>(i), kG261MatLocal);
        // Remaining FBO rows are deliberately stale until the caller copies. Never let the
        // partial publication masquerade as a whole-target upload snapshot.
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
        const bool prefixPublished = snap.valid;
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        if (!prefixPublished)
            continue; // g261Materialize already emitted its loud bounded failure and dropped state
        consumedMask |= 1u << i;
        const uint64_t n =
            g_g285L2lConsumes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g285L2lRowsPublished.fetch_add(static_cast<uint64_t>(pubRows),
                                         std::memory_order_relaxed);
        g_g285L2lRowsElided.fetch_add(
            static_cast<uint64_t>(originalRows - pubRows), std::memory_order_relaxed);
        if (s_stat && (n <= 8u || (n % 128u) == 0u))
            std::fprintf(stderr,
                         "[G285:consume] n=%llu fbp=%03x publishRows=%d elidedRows=%d "
                         "dst=0x%x bytes=%llu\n",
                         static_cast<unsigned long long>(n), kG261Fbp[i],
                         pubRows, originalRows - pubRows, dbp,
                         static_cast<unsigned long long>(copyBytes));
    }
    return consumedMask;
}

struct G264InvPage
{
    std::array<uint16_t, 2048> x{};
    std::array<uint16_t, 2048> y{};
    bool valid = false;
};
static const G264InvPage &g264InvPage()
{
    static const G264InvPage s_inv = []() {
        G264InvPage out;
        out.x.fill(0xffffu);
        out.y.fill(0xffffu);
        out.valid = true;
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, y);
                if ((off & 3u) != 0u || off >= 8192u)
                {
                    out.valid = false;
                    continue;
                }
                const uint32_t word = off >> 2u;
                if (out.x[word] != 0xffffu)
                {
                    out.valid = false;
                    continue;
                }
                out.x[word] = static_cast<uint16_t>(x);
                out.y[word] = static_cast<uint16_t>(y);
            }
        for (uint32_t word = 0; word < 2048u; ++word)
            out.valid = out.valid && out.x[word] != 0xffffu;
        return out;
    }();
    return s_inv;
}

struct G326G264Key
{
    uint32_t dbp = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint8_t dbw = 0u, dpsm = 0u;
    std::array<uint8_t, kG248TargetCount> targetFbw{};
    std::array<int16_t, kG248TargetCount> targetFbW{};
};
struct G326G264Row
{
    uint16_t y = 0u;
    std::array<uint64_t, 8> bits{};
};
struct G326G264TargetPlan
{
    std::vector<G326G264Row> rows;
    uint32_t hits = 0u;
};
struct G326G264Plan
{
    G326G264Key key{};
    std::array<G326G264TargetPlan, kG248TargetCount> targets{};
    uint64_t stamp = 0u;
};
static G326G264Key g326G264Key(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                               uint32_t dsax, uint32_t dsay,
                               uint32_t rrw, uint32_t rrh)
{
    G326G264Key key{};
    key.dbp = dbp;
    key.dbw = dbw;
    key.dpsm = dpsm;
    key.dsax = dsax;
    key.dsay = dsay;
    key.rrw = rrw;
    key.rrh = rrh;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        key.targetFbw[t] = g_g261Res[t].fbw;
        key.targetFbW[t] = static_cast<int16_t>(g_g261Res[t].fbW);
    }
    return key;
}
static bool g326G264KeyEqual(const G326G264Key &a, const G326G264Key &b)
{
    return a.dbp == b.dbp && a.dbw == b.dbw && a.dpsm == b.dpsm &&
           a.dsax == b.dsax && a.dsay == b.dsay && a.rrw == b.rrw && a.rrh == b.rrh &&
           a.targetFbw == b.targetFbw && a.targetFbW == b.targetFbW;
}
static bool g326BuildG264Plan(const G326G264Key &key, G326G264Plan &out)
{
    const G264InvPage &inv = g264InvPage();
    const uint64_t pixels64 = static_cast<uint64_t>(key.rrw) * key.rrh;
    if (!inv.valid || key.dpsm != GS_PSM_CT32 || key.dbw == 0u ||
        pixels64 == 0u || pixels64 > 512u * 512u)
        return false;
    out = G326G264Plan{};
    out.key = key;
    std::vector<uint32_t> offsets(static_cast<size_t>(pixels64));
    size_t op = 0u;
    for (uint32_t sy = 0u; sy < key.rrh; ++sy)
        for (uint32_t sx = 0u; sx < key.rrw; ++sx)
            offsets[op++] = GSPSMCT32::addrPSMCT32(
                key.dbp, key.dbw, key.dsax + sx, key.dsay + sy);

    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const uint32_t fbw = key.targetFbw[t];
        const int fbW = key.targetFbW[t];
        if (fbw == 0u || fbW <= 0 || fbW > 512)
            continue;
        std::array<std::array<uint64_t, 8>, 512> masks{};
        const uint64_t targetBase =
            static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[t])) * 256u;
        const uint64_t targetBytes = static_cast<uint64_t>(fbw) * 16u * 8192u;
        G326G264TargetPlan &target = out.targets[t];
        for (const uint32_t off : offsets)
        {
            const uint64_t off64 = off;
            if (off64 < targetBase || off64 + 4u > targetBase + targetBytes)
                continue;
            const uint64_t rel = off64 - targetBase;
            const uint32_t page = static_cast<uint32_t>(rel / 8192u);
            const uint32_t word = static_cast<uint32_t>((rel % 8192u) >> 2u);
            const uint32_t tx = (page % fbw) * 64u + inv.x[word];
            const uint32_t ty = (page / fbw) * 32u + inv.y[word];
            if (tx >= static_cast<uint32_t>(fbW) || ty >= 512u)
                continue;
            masks[ty][tx >> 6u] |= uint64_t{1} << (tx & 63u);
            ++target.hits;
        }
        for (uint32_t y = 0u; y < 512u; ++y)
        {
            bool any = false;
            for (uint32_t w = 0u; w < 8u; ++w)
                any = any || masks[y][w] != 0u;
            if (any)
                target.rows.push_back(G326G264Row{static_cast<uint16_t>(y), masks[y]});
        }
    }
    return true;
}
static bool g326G264PlanEqual(const G326G264Plan &a, const G326G264Plan &b)
{
    if (!g326G264KeyEqual(a.key, b.key))
        return false;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        if (a.targets[t].hits != b.targets[t].hits ||
            a.targets[t].rows.size() != b.targets[t].rows.size())
            return false;
        for (size_t r = 0u; r < a.targets[t].rows.size(); ++r)
            if (a.targets[t].rows[r].y != b.targets[t].rows[r].y ||
                a.targets[t].rows[r].bits != b.targets[t].rows[r].bits)
                return false;
    }
    return true;
}
static G326G264Plan *g326GetG264Plan(const G326G264Key &key)
{
    static std::vector<G326G264Plan> s_cache;
    static uint64_t s_stamp = 0u;
    G326G264Plan *hit = nullptr;
    if (!g326ForceColdOn())
        for (G326G264Plan &plan : s_cache)
            if (g326G264KeyEqual(plan.key, key))
            {
                hit = &plan;
                break;
            }
    if (hit != nullptr)
    {
        hit->stamp = ++s_stamp;
        if (g326DiagOn())
            ++g_g326.g264Hit;
        if (g326VerifyOn())
        {
            G326G264Plan cold;
            bool equal = false;
            try
            {
                equal = g326BuildG264Plan(key, cold) && g326G264PlanEqual(*hit, cold);
            }
            catch (...)
            {
                equal = false;
            }
            if (!equal)
            {
                ++g_g326.g264Bad;
                static uint32_t s_log = 0u;
                if (s_log++ < 16u)
                    std::fprintf(stderr,
                                 "[G326:bad] product=g264 dbp=0x%x rect=%ux%u\n",
                                 key.dbp, key.rrw, key.rrh);
                return nullptr; // caller executes the unchanged cold behavior
            }
        }
        return hit;
    }
    if (g326DiagOn())
        ++g_g326.g264Miss;
    if (g326ForceColdOn())
        return nullptr;
    try
    {
        G326G264Plan plan;
        if (!g326BuildG264Plan(key, plan))
            return nullptr;
        plan.stamp = ++s_stamp;
        if (s_cache.size() < 32u)
        {
            s_cache.push_back(std::move(plan));
            return &s_cache.back();
        }
        auto oldest = std::min_element(
            s_cache.begin(), s_cache.end(),
            [](const G326G264Plan &a, const G326G264Plan &b) {
                return a.stamp < b.stamp;
            });
        *oldest = std::move(plan);
        return &*oldest;
    }
    catch (...)
    {
        return nullptr;
    }
}
static uint32_t g326Popcount64(uint64_t v)
{
    uint32_t n = 0u;
    while (v != 0u)
    {
        v &= v - 1u;
        ++n;
    }
    return n;
}

static uint32_t g264NoteUploadForMirror(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                         uint32_t dsax, uint32_t dsay,
                                         uint32_t rrw, uint32_t rrh)
{
    if (!g264On() || !g261WaveOn() || rrw == 0u || rrh == 0u)
        return 0u;
    const G144BlockRange up = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!up.valid)
        return 0u;
    // Only exact CT32 byte replacement is supported. The common DC2 upload is 128x128 (16K
    // pixels); cap the mapper at one full 512x512 surface so malformed state fails closed.
    const uint64_t uploadPixels64 = static_cast<uint64_t>(rrw) * rrh;
    auto logReject = [&](const char *why, uint32_t ti, const G261Res &r, uint32_t hits) {
        if (!g262CensusOn())
            return;
        static std::atomic<uint32_t> s_g264RejectLog{0};
        if (s_g264RejectLog.fetch_add(1u, std::memory_order_relaxed) >= 32u)
            return;
        std::fprintf(stderr,
                     "[G264:rej] why=%s dbp=0x%x dbw=%u psm=0x%x sax=%u say=%u rect=%ux%u "
                     "blocks=%x..%x -> t=%03x fbw=%u fbW=%d hits=%u "
                     "dirty=%d..%d cov=%d..%d\n",
                     why, dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                     up.first, up.last, kG261Fbp[ti], r.fbw, r.fbW, hits,
                     r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    };

    // Invert the CT32 swizzle inside one 64x32 page once. Surface page selection itself is
    // linear in bytes; this table converts an uploaded byte address into the coordinates each
    // resident FRAME/FBW interpretation assigns to it.
    const G264InvPage &s_inv = g264InvPage();

    if (dpsm != GS_PSM_CT32 || dbw == 0u || uploadPixels64 > 512u * 512u || !s_inv.valid)
    {
        for (uint32_t i = 0; i < kG248TargetCount; ++i)
        {
            const G261Res &r = g_g261Res[i];
            if (r.dirty && r.range.valid && g144RangeOverlaps(r.range, up))
                logReject("shape", i, r, 0u);
        }
        return 0u;
    }

    if (g326G264On())
    {
        bool anyTarget = false;
        for (uint32_t i = 0u; i < kG248TargetCount; ++i)
        {
            const G261Res &r = g_g261Res[i];
            anyTarget = anyTarget ||
                        (r.dirty && r.range.valid && g144RangeOverlaps(r.range, up));
        }
        // The legacy loop builds every source offset before discovering that no resident target
        // observes it. This proof is the same live dirty/range predicate used by that loop.
        if (!anyTarget)
            return 0u;
        G326G264Plan *plan = g326GetG264Plan(
            g326G264Key(dbp, dbw, dpsm, dsax, dsay, rrw, rrh));
        g326MaybeReport();
        if (plan != nullptr)
        {
            uint32_t cachedMask = 0u;
            for (uint32_t i = 0u; i < kG248TargetCount; ++i)
            {
                G261Res &r = g_g261Res[i];
                if (!r.dirty || !r.range.valid || !g144RangeOverlaps(r.range, up))
                    continue;
                if (r.fbw == 0u || r.fbW <= 0 || r.fbW > 512)
                {
                    logReject("target-shape", i, r, 0u);
                    continue;
                }
                const G326G264TargetPlan &target = plan->targets[i];
                if (target.hits == 0u || target.rows.empty())
                {
                    logReject("alias-none", i, r, 0u);
                    continue;
                }
                const bool g291On = g291AtlasOn();
                if (g291On && r.g291CovFbw != r.fbw)
                {
                    r.g291Cov = {};
                    r.g291CovFbw = r.fbw;
                    g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
                }
                uint32_t newHits = 0u;
                const bool countNewHits = g291StatOn() || g262CensusOn();
                for (const G326G264Row &cachedRow : target.rows)
                {
                    const uint32_t y = cachedRow.y;
                    for (uint32_t word = 0u; word < 8u; ++word)
                    {
                        const uint64_t bits = cachedRow.bits[word];
                        if (bits == 0u)
                            continue;
                        if (countNewHits)
                            newHits += g326Popcount64(bits & ~r.mirBits[y][word]);
                        r.mirBits[y][word] |= bits;
                        if (g291On)
                            r.g291Cov[y][word] |= bits;
                    }
                }
                if (g291On)
                    g_g291CurPagesSet.fetch_add(target.hits, std::memory_order_relaxed);
                const int hitLo = target.rows.front().y;
                const int hitHi = target.rows.back().y;
                r.mirPending = true;
                if (hitLo < r.mirLo) r.mirLo = hitLo;
                if (hitHi > r.mirHi) r.mirHi = hitHi;
                cachedMask |= (1u << i);
                g_g264MirNoted.fetch_add(1u, std::memory_order_relaxed);
                {
                    static std::atomic<uint32_t> s_g291HiLog{0};
                    if (g291StatOn() && dbp >= 0x3400u && kG261Fbp[i] == 0x139u &&
                        s_g291HiLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                        std::fprintf(
                            stderr,
                            "[G291:upnote] dbp=0x%x dbw=%u rect=%ux%u rows=%d..%d px=%u new=%u\n",
                            dbp, static_cast<unsigned>(dbw), rrw, rrh, hitLo, hitHi,
                            target.hits, newHits);
                }
                if (g262CensusOn())
                {
                    static std::atomic<uint32_t> s_g264UpLog{0};
                    if (s_g264UpLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
                        std::fprintf(
                            stderr,
                            "[G264:up] dbp=0x%x dbw=%u psm=0x%x sax=%u say=%u rect=%ux%u -> "
                            "t=%03x aliasPx=%u new=%u rows=%d..%d dirty=%d..%d cov=%d..%d\n",
                            dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                            kG261Fbp[i], target.hits, newHits, hitLo, hitHi,
                            r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
                }
            }
            return cachedMask;
        }
    }

    // Compute the exact set of byte addresses the CT32 IMAGE transfer replaces. This is the
    // stable bridge across different DBP/DBW and FRAME/FBW views of the same VRAM pages.
    static std::vector<uint32_t> s_uploadOffsets; // single flush-owner thread by G144 contract
    s_uploadOffsets.resize(static_cast<size_t>(uploadPixels64));
    size_t op = 0;
    for (uint32_t sy = 0; sy < rrh; ++sy)
        for (uint32_t sx = 0; sx < rrw; ++sx)
            s_uploadOffsets[op++] = GSPSMCT32::addrPSMCT32(
                dbp, dbw, dsax + sx, dsay + sy);

    uint32_t mask = 0u;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty || !r.range.valid || !g144RangeOverlaps(r.range, up))
            continue;
        if (r.fbw == 0u || r.fbW <= 0 || r.fbW > 512)
        {
            logReject("target-shape", i, r, 0u);
            continue;
        }
        const uint64_t targetBase = static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[i])) * 256u;
        const uint64_t targetBytes = static_cast<uint64_t>(r.fbw) * 16u * 8192u;
        const bool g291On = g291AtlasOn();
        if (g291On && r.g291CovFbw != r.fbw)
        {
            r.g291Cov = {};
            r.g291CovFbw = r.fbw;
            g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        }
        uint32_t hits = 0u;
        uint32_t newHits = 0u;
        int hitLo = 512, hitHi = -1;
        for (uint32_t off : s_uploadOffsets)
        {
            const uint64_t off64 = off;
            if (off64 < targetBase || off64 + 4u > targetBase + targetBytes)
                continue;
            const uint64_t rel = off64 - targetBase;
            const uint32_t page = static_cast<uint32_t>(rel / 8192u);
            const uint32_t word = static_cast<uint32_t>((rel % 8192u) >> 2u);
            const uint32_t tx = (page % r.fbw) * 64u + s_inv.x[word];
            const uint32_t ty = (page / r.fbw) * 32u + s_inv.y[word];
            if (tx >= static_cast<uint32_t>(r.fbW) || ty >= 512u)
                continue;
            ++hits;
            auto &row = r.mirBits[ty];
            const uint64_t bit = uint64_t{1} << (tx & 63u);
            uint64_t &wordRef = row[tx >> 6u];
            if ((wordRef & bit) == 0u)
            {
                wordRef |= bit;
                ++newHits;
            }
            if (g291On)
            {
                // G291: cumulative coverage — the upload just wrote this pixel's bytes to
                // guest VRAM, so VRAM is current for it until the next wave render of the row.
                r.g291Cov[ty][tx >> 6u] |= bit;
                g_g291CurPagesSet.fetch_add(1u, std::memory_order_relaxed);
            }
            hitLo = std::min(hitLo, static_cast<int>(ty));
            hitHi = std::max(hitHi, static_cast<int>(ty));
        }
        if (hits == 0u)
        {
            logReject("alias-none", i, r, hits);
            continue;
        }
        r.mirPending = true;
        if (hitLo < r.mirLo) r.mirLo = hitLo;
        if (hitHi > r.mirHi) r.mirHi = hitHi;
        mask |= (1u << i);
        g_g264MirNoted.fetch_add(1u, std::memory_order_relaxed);
        {
            // G291 bring-up diag: which uploads land in the seeded batch's row-13 texture band.
            static std::atomic<uint32_t> s_g291HiLog{0};
            if (g291StatOn() && dbp >= 0x3400u && kG261Fbp[i] == 0x139u &&
                s_g291HiLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                std::fprintf(stderr,
                             "[G291:upnote] dbp=0x%x dbw=%u rect=%ux%u rows=%d..%d px=%u new=%u\n",
                             dbp, static_cast<unsigned>(dbw), rrw, rrh, hitLo, hitHi,
                             hits, newHits);
        }
        if (g262CensusOn())
        {
            static std::atomic<uint32_t> s_g264UpLog{0};
            if (s_g264UpLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
                std::fprintf(stderr,
                             "[G264:up] dbp=0x%x dbw=%u psm=0x%x sax=%u say=%u rect=%ux%u -> "
                             "t=%03x aliasPx=%u new=%u rows=%d..%d dirty=%d..%d cov=%d..%d\n",
                             dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                             kG261Fbp[i], hits, newHits, hitLo, hitHi,
                             r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
        }
    }
    return mask;
}

#if defined(PS2X_G662_DIAG)
// G662 [G662:colspan]: the current flush's SCISSOR COLUMN UNION, accumulated in g178TryFlushGpu's
// pass 1 beside the row union G621 already folds, and read at the publication in g261Materialize.
// thread_local for the same reason every other per-flush accumulator here is: the G144 flush-owner
// contract is per thread. Diagnostic-only — nothing narrows on it yet.
static thread_local int t_g662ColLo = 1 << 24;
static thread_local int t_g662ColHi = -1;
#endif

// The flush closures are about to run the CPU band replay for g_g144List (the GPU attempt
// failed or was ineligible). Any entry whose target/Z/texture/CLUT pages touch a resident
// target needs those pages materialized first — the replay reads and writes guest VRAM directly.
#if defined(PS2X_G661_DIAG)
// ===== G661 P11: THE CONSUMER-AWARE RESIDENCY CEILING PROBE =====================================
//
// P11 proposes materializing a GPU-produced surface only when a CPU-visible consumer genuinely
// needs it. This publication is already consumer-TRIGGERED (it runs because an entry below is about
// to replay on the CPU over those pages), so the only prize left in it is consumer-SCOPED: it
// publishes the target's whole dirty span `r.dirtyLo..r.dirtyHi` regardless of which rows the
// consuming entry actually touches. Rule 11 (producer-bounded transfers) applied to the other end.
//
// This census prices that prize before anything is built. Per materialization it accumulates the
// rows PUBLISHED against the rows the triggering entry could possibly READ (its scissor span,
// which is an OVER-estimate of what a draw touches — so the measured prize is a LOWER bound).
//
//   published >> consumed  =>  a windowed publication is worth building
//   published ~= consumed  =>  the lever is dead and P11 must be refuted, not attempted
//
// Diagnostic-only, `#if`-excluded (rule 12c): its call site is inside the flush prologue of the
// hottest TU in the tree.
static std::atomic<uint64_t> g_g661PubRows{0}, g_g661ConRows{0}, g_g661PubN{0};

// ===== G661 P15: THE SINGLE-AUTHORITY DEPTH FEASIBILITY PROBE ===================================
//
// P15 wants the G403/G411 shared private depth mirror owned exclusively by the GPU for the duration
// of a depth-writing discovered-target sequence, so that admitting those families needs no depth
// PUBLICATION — the 1.491 ms/f of CPU packing G656 P21-P23 priced and G660 P27 refused on.
//
// Exclusive ownership is only possible if, while it is held, NOTHING ELSE writes that mirror on the
// CPU. That is a factual question about this corpus and it is answerable without building anything:
// count the CPU-replay entries that touch the private mirror (ZBP page 0xd0 / PSMZ24) and split
// them by whether their own target is a DISCOVERED target (which the single-authority design would
// move to the GPU) or something else (which it would NOT, and which therefore blocks it).
//
//   otherWriters ~ 0   =>  exclusive GPU depth ownership is available, P15 is buildable
//   otherWriters large =>  a second CPU writer shares the mirror; P15 needs arbitration, not
//                          ownership, and inherits G656's publication price after all
// ⛔ DO NOT CLASSIFY WITH `g604DiscovTransientTarget()`. That predicate is gated on
// `g604DiscovTransientOn()`, which requires `DC2_G604_DISCOV_TRANSIENT` to be explicitly set and is
// therefore FALSE FOR EVERY FBP in a default run — a census keyed to it would have reported 100%
// "other writer" on every route and refuted P15 on an artifact.
//
// Report a per-FBP HISTOGRAM instead. It is self-validating: the reader sees which addresses write
// the mirror and can check them against the known families (`ridepod` 0x142, `combat` 0x13d,
// `s03` 0x15c, `dragon` 0x15b) rather than trusting a predicate's arming state.
enum { kG661ZSlots = 12 };
static std::atomic<uint32_t> g_g661ZFbp[kG661ZSlots]{};
static std::atomic<uint64_t> g_g661ZCnt[kG661ZSlots]{};
static std::atomic<uint64_t> g_g661ZOverflow{0};
static std::atomic<uint64_t> g_g661ZTotal{0}, g_g661ZSeen{0};

static void g661ZBump(uint32_t fbp)
{
    for (int i = 0; i < kG661ZSlots; ++i)
    {
        uint32_t cur = g_g661ZFbp[i].load(std::memory_order_relaxed);
        if (cur == fbp + 1u)
        {
            g_g661ZCnt[i].fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        if (cur == 0u)
        {
            uint32_t expect = 0u;
            if (g_g661ZFbp[i].compare_exchange_strong(expect, fbp + 1u,
                                                      std::memory_order_relaxed))
            {
                g_g661ZCnt[i].fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            --i; // slot was taken between the load and the CAS; re-examine it
        }
    }
    g_g661ZOverflow.fetch_add(1u, std::memory_order_relaxed);
}

static void g661NoteDepthWriter(const G144Entry &e)
{
    // The mirror is ZBP page 0xd0 with PSMZ24 — the same tuple every admission predicate in this
    // tree tests (`guestZbpPage == 0x0d0u && guestZpsm == 1u`).
    // ZBUF is a raw 64-bit register here: ZBP in bits 0..8 (page units), PSM in 24..27, ZMSK in 32.
    // Same decode as g603_admission_census.inc and g605_tri_span.inc.
    // ⭐ THE DENOMINATOR IS COUNTED FIRST AND UNCONDITIONALLY, and the report is keyed to IT, not
    // to the hit count. A hit count of ZERO is this probe's single most important possible answer —
    // it is exactly what makes exclusive GPU depth ownership available — and a hit-keyed reporter
    // can never print it. The first version of this census was hit-keyed and returned nothing at
    // all on `lavaboss` and `combat`, which is G657 measurement law 10's third form reproduced by
    // the very phase that documented it.
    const uint64_t seen = g_g661ZSeen.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool zmsk = ((e.ctx.zbuf >> 32) & 1ull) != 0ull;
    const uint32_t zbpPage = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu);
    const uint32_t zpsm = static_cast<uint32_t>((e.ctx.zbuf >> 24) & 0xFu);
    if (!zmsk && zbpPage == 0x0d0u && zpsm == 1u)
    {
        g_g661ZTotal.fetch_add(1u, std::memory_order_relaxed);
        g661ZBump(e.ctx.frame.fbp);
    }
    if (seen == 1u || (seen % 65536u) == 0u)
    {
        const uint64_t n = g_g661ZTotal.load(std::memory_order_relaxed);
        char buf[512];
        int off = std::snprintf(buf, sizeof(buf),
                                "[G661:zauth] cpuEntriesSeen=%llu mirrorWrites=%llu overflow=%llu",
                                (unsigned long long)seen, (unsigned long long)n,
                                (unsigned long long)g_g661ZOverflow.load(
                                    std::memory_order_relaxed));
        for (int i = 0; i < kG661ZSlots && off > 0 && off < (int)sizeof(buf); ++i)
        {
            const uint32_t f = g_g661ZFbp[i].load(std::memory_order_relaxed);
            if (f == 0u)
                continue;
            const uint64_t c = g_g661ZCnt[i].load(std::memory_order_relaxed);
            off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " fbp=%03x:%llu(%.1f%%)",
                                 f - 1u, (unsigned long long)c,
                                 n ? 100.0 * (double)c / (double)n : 0.0);
        }
        std::fprintf(stderr, "%s\n", buf);
    }
}

static void g661NoteConsumerScope(const G144Entry &e, const G261Res &r)
{
    if (r.dirtyHi < r.dirtyLo)
        return;
    const int64_t pub = static_cast<int64_t>(r.dirtyHi - r.dirtyLo + 1);
    // Intersect the published span with the consuming entry's scissor rows.
    const int64_t cLo = std::max<int64_t>(r.dirtyLo, static_cast<int64_t>(e.ctx.scissor.y0));
    const int64_t cHi = std::min<int64_t>(r.dirtyHi, static_cast<int64_t>(e.ctx.scissor.y1));
    const int64_t con = (cHi >= cLo) ? (cHi - cLo + 1) : 0;
    g_g661PubRows.fetch_add(static_cast<uint64_t>(pub), std::memory_order_relaxed);
    g_g661ConRows.fetch_add(static_cast<uint64_t>(con), std::memory_order_relaxed);
    const uint64_t n = g_g661PubN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    // ⛔ Print at n==1 as well as periodically (G657 measurement law 10). A period-only reporter
    // cannot distinguish "this route has ZERO of these" from "this route has fewer than one
    // period's worth", and the first version of this census hit exactly that on `lavaboss`.
    if (n == 1u || (n % 2048u) == 0u)
    {
        const uint64_t p = g_g661PubRows.load(std::memory_order_relaxed);
        const uint64_t c = g_g661ConRows.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G661:conscope] mats=%llu pubRows=%llu conRows=%llu consumed=%.2f%% "
                     "ceiling=%.2f%%\n",
                     (unsigned long long)n, (unsigned long long)p, (unsigned long long)c,
                     p ? 100.0 * (double)c / (double)p : 0.0,
                     p ? 100.0 * (double)(p - c) / (double)p : 0.0);
    }
}
#endif

static void g261PrepareCpuReplay()
{
    if (!g261WaveOn() || !g261AnyDirty() || g_g144List.empty())
        return;
    for (const G144Entry &e : g_g144List)
    {
#if defined(PS2X_G661_DIAG)
        // P15: every entry here is about to REPLAY ON THE CPU, so an entry that writes the private
        // depth mirror is exactly a CPU writer of it. Sampled before the early-out below, because
        // the question is about the population, not about what this flush publishes.
        g661NoteDepthWriter(e);
#endif
        if (!g261AnyDirty())
            return;
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        const bool tme = e.prim.tme;
        const G144BlockRange tex = tme ? g144TextureRange(e) : G144BlockRange{};
        const G144BlockRange clut =
            (tme && g144IsPalettedPsm(e.ctx.tex0.psm)) ? g144ClutRange(e) : G144BlockRange{};
        const bool anyUnknown = !color.valid || (depth.active && !depth.range.valid) ||
                                (tme && !tex.valid);
        for (uint32_t i = 0; i < kG248TargetCount; ++i)
        {
            G261Res &r = g_g261Res[i];
            if (!r.dirty)
                continue;
            const bool hitUnknown = anyUnknown || !r.range.valid;
            const bool hitColor = g144RangeOverlaps(r.range, color);
            const bool hitDepth = depth.active && g144RangeOverlaps(r.range, depth.range);
            const bool hitTex = tme && g144RangeOverlaps(r.range, tex);
            const bool hitClut = clut.valid && g144RangeOverlaps(r.range, clut);
            if (hitUnknown || hitColor || hitDepth || hitTex || hitClut)
            {
                g266NoteCpuMat(e, i, (hitColor ? 1u : 0u) | (hitDepth ? 2u : 0u) |
                                         (hitTex ? 4u : 0u) | (hitClut ? 8u : 0u) |
                                         (hitUnknown ? 16u : 0u));
#if defined(PS2X_G661_DIAG)
                // Sample BEFORE the publication: g261Materialize clears r.dirty and collapses the
                // span, so reading it afterwards measures nothing (G657 measurement law 10).
                g661NoteConsumerScope(e, r);
#endif
                g261Materialize(static_cast<int>(i), kG261MatCpuReplay);
            }
        }
    }
}

// ===== G582: duplicate page-generation bump elision in the CPU-replay publication loops =====
//
// PREMISE (measured before the lever was written). The G446 PC sampler resolved against the
// linker /MAP puts `g178BumpRectImpl+0x5b5` — the `lock inc qword ptr [g_g178PageGen + rax*8]`
// inner loop — at 9.9% of every GS-worker sample on the Ridepod boss, about half of all
// non-waiting in-exe samples on the pole thread. `DC2_G582_BUMPCENSUS=1` then ranked the walk by
// PAGE VISITS (one `lock inc` each), not by calls, over the boss window n=1441..1861:
//
//   total                     1,063,518 visits/f over 11,429 calls/f
//   src=14 g286trncol           379,611/f (35.7%)  3,650 calls x 104 pages
//   src=15 g286trndep           378,882/f (35.6%)  3,643 calls x 104 pages
//   src=2  cpurttcol            163,524/f (15.4%)  2,539 calls x  64 pages
//   src=3  cpurttdep            138,106/f (13.0%)  1,328 calls x 104 pages
//   everything else               2,395/f ( 0.3%)
//
// ROOT. Both publication loops walk `g_g144List` PER ENTRY and bump the entry's whole SCISSOR
// rect. Thousands of entries in one flush share a handful of distinct (target, rect) tuples, so
// 99.7% of the million atomic RMWs per frame re-bump a page that the same loop already bumped.
//
// EXACTNESS. No consumer runs between two iterations of these loops, and every consumer of
// `g_g178PageGen` compares a stored value or SUM against a fresh one for INEQUALITY
// (g178GenSumRect's callers, `r.g536PageGen[p]`, the T8 atlas `gen`). None reads the magnitude of
// the delta. N bumps of one page are therefore indistinguishable from 1 for every reader; the
// only observable is WHICH pages moved, which is unchanged. `g178NoteVramWriteRectTagged`'s other
// side effect (`g242PrepareVramReadRect` on an overlapping dirty depth) is idempotent for an
// identical rect — the first call makes it clean — so eliding exact duplicates elides nothing.
//
// Oracle: DC2_G582_VERIFY=1 accumulates, per loop, the page mask the LEGACY per-entry walk would
// have touched and the mask the deduped walk actually touched, and reports any difference. It
// asserts the invariant (same page set), not the absence of work.
// Rollback: DC2_G582_NO_DEDUP_BUMP=1.  Counters: DC2_G582_STAT=1.
static bool g582DedupBumpOn()
{
    static const bool on = !envFlagEnabled("DC2_G582_NO_DEDUP_BUMP") &&
                           !envFlagDisabled("DC2_G582_DEDUP_BUMP");
    return on;
}

static bool g582DedupStatOn()
{
    static const bool on = envFlagEnabled("DC2_G582_STAT");
    return on;
}

static bool g582DedupVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G582_VERIFY");
    return on;
}

static std::atomic<uint64_t> g_g582DedupLoops{0u};
static std::atomic<uint64_t> g_g582DedupBumps{0u};
static std::atomic<uint64_t> g_g582DedupSkips{0u};
static std::atomic<uint64_t> g_g582DedupOverflow{0u};
static std::atomic<uint64_t> g_g582VerifyChecks{0u};
static std::atomic<uint64_t> g_g582VerifyBad{0u};

struct G582BumpRect
{
    uint32_t bp, bw, psm, x, y, w, h;
};

// Fixed capacity, linear scan: these loops resolve to a handful of distinct tuples, so a hash set
// would cost more than it saves. On overflow the set stops admitting and every later rect bumps
// unconditionally — fail-open to the exact legacy behaviour.
struct G582BumpSet
{
    static constexpr uint32_t kCap = 32u;
    G582BumpRect r[kCap];
    uint32_t n = 0u;

    // Returns true when the caller must perform the bump (first sighting of this tuple, or full).
    bool admit(const G582BumpRect &q)
    {
        for (uint32_t i = 0u; i < n; ++i)
            if (r[i].bp == q.bp && r[i].bw == q.bw && r[i].psm == q.psm &&
                r[i].x == q.x && r[i].y == q.y && r[i].w == q.w && r[i].h == q.h)
                return false;
        if (n < kCap)
            r[n++] = q;
        else
            g_g582DedupOverflow.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }
};

// One loop's accounting: the two masks under DC2_G582_VERIFY, plus the shipped counters.
struct G582DedupScope
{
    bool dedup;
    bool verify;
    uint64_t bumps = 0u, skips = 0u;
    G582PageMask legacyMask{};
    G582PageMask dedupMask{};

    G582DedupScope()
        : dedup(g582DedupBumpOn()), verify(g582DedupVerifyOn()) {}

    // Single decision point for every call site, so the arms can never diverge in shape.
    bool take(G582BumpSet &set, uint32_t bp, uint32_t bw, uint32_t psm,
              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (verify)
            g582MarkRectPages(legacyMask, bp, bw, psm, x, y, w, h);
        const bool doBump = !dedup || set.admit(G582BumpRect{bp, bw, psm, x, y, w, h});
        if (doBump)
        {
            ++bumps;
            if (verify)
                g582MarkRectPages(dedupMask, bp, bw, psm, x, y, w, h);
        }
        else
            ++skips;
        return doBump;
    }

    void finish(const char *what)
    {
        if (bumps == 0u && skips == 0u)
            return;
        g_g582DedupLoops.fetch_add(1u, std::memory_order_relaxed);
        g_g582DedupBumps.fetch_add(bumps, std::memory_order_relaxed);
        g_g582DedupSkips.fetch_add(skips, std::memory_order_relaxed);
        if (verify)
        {
            bool bad = false;
            for (size_t i = 0u; i < legacyMask.size(); ++i)
                if (legacyMask[i] != dedupMask[i])
                    bad = true;
            const uint64_t n = g_g582VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (bad)
            {
                const uint64_t b = g_g582VerifyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (b <= 32u)
                    std::fprintf(stderr,
                                 "[G582:dedup-BAD] %s check=%llu bumps=%llu skips=%llu\n",
                                 what, static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(bumps),
                                 static_cast<unsigned long long>(skips));
            }
            else if (n <= 2ull || (n % 20000ull) == 0ull)
                std::fprintf(stderr, "[G582:dedup-vfy] %s checks=%llu bad=%llu\n",
                             what, static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(
                                 g_g582VerifyBad.load(std::memory_order_relaxed)));
        }
        if (!g582DedupStatOn())
            return;
        const uint64_t loops = g_g582DedupLoops.load(std::memory_order_relaxed);
        if (loops <= 4ull || (loops % 4096ull) == 0ull)
            std::fprintf(stderr,
                         "[G582:dedup] loops=%llu bumps=%llu skips=%llu overflow=%llu last=%s "
                         "(%llu/%llu)\n",
                         static_cast<unsigned long long>(loops),
                         static_cast<unsigned long long>(
                             g_g582DedupBumps.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_g582DedupSkips.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_g582DedupOverflow.load(std::memory_order_relaxed)),
                         what, static_cast<unsigned long long>(bumps),
                         static_cast<unsigned long long>(skips));
    }
};

// A CPU band replay just wrote g_g144List into guest VRAM. Under waves the G260 pre-execution
// generation bump is disabled (it would poison GPU-resident targets with stale-VRAM uploads),
// so publish the RTT-family write generations HERE instead — the exact bump an inline draw
// would have done, after the replay actually changed the bytes.
static void g261NoteCpuRttWrites(uint8_t *vram)
{
    if (!g261WaveOn() || vram == nullptr)
        return;
    G582DedupScope g582;
    G582BumpSet g582Color, g582Depth;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        // G527 (publication half of the registry — MUST stay the same predicate as the admission
        // site in rasterizer_tilebin_capture.inc). A discovered target's deferred writes need the
        // identical page-generation bump an inline draw performs at the end of drawPrimitive;
        // without it a GPU-cached texture decoded from those pages stays stale — wrong pixels,
        // silently. The scissor rect here is a superset of the inline path's clamped bbox, so this
        // is conservative in the safe direction.
        if (!g527DeferTarget(c.frame.fbp))
            continue;
        if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
        {
            const uint32_t w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
            const uint32_t h = static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1);
            const uint32_t fbBlock = framePageBaseToBlock(c.frame.fbp);
            if (g582.take(g582Color, fbBlock, c.frame.fbw, c.frame.psm,
                          c.scissor.x0, c.scissor.y0, w, h))
                g178NoteVramWriteRectTagged(vram, fbBlock,
                                            c.frame.fbw, c.frame.psm,
                                            c.scissor.x0, c.scissor.y0, w, h,
                                            2u); // G570 source 2: post-CPU-replay RTT color write
            if (((c.zbuf >> 32u) & 1u) == 0u)
            {
                const uint32_t zbpBlock = static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u;
                const uint32_t zpsm = static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu);
                if (g582.take(g582Depth, zbpBlock, c.frame.fbw, zpsm,
                              c.scissor.x0, c.scissor.y0, w, h))
                    g178NoteVramWriteRectTagged(
                        vram, zbpBlock, c.frame.fbw, zpsm,
                        c.scissor.x0, c.scissor.y0, w, h,
                        3u); // G570 source 3: post-CPU-replay RTT depth write
            }
        }
    }
    g582.finish("g261rtt");
}

// G286 transient targets deliberately do not join the persistent G261 ownership table, but a
// rejected GPU batch still falls back to the deferred CPU replay. Publish those exact CPU writes
// too, so a later texture/readback consumer cannot retain a generation from before the fallback.
static void g286NoteCpuTransientWrites(uint8_t *vram)
{
    if (!g286TransientTargetOn() || vram == nullptr)
        return;
    G582DedupScope g582;
    G582BumpSet g582Color, g582Depth;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        if (!g286TransientTarget(c.frame.fbp) ||
            c.scissor.x1 < c.scissor.x0 || c.scissor.y1 < c.scissor.y0)
            continue;
        const uint32_t w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
        const uint32_t h = static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1);
        const uint32_t fbBlock = framePageBaseToBlock(c.frame.fbp);
        if (g582.take(g582Color, fbBlock, c.frame.fbw, c.frame.psm,
                      c.scissor.x0, c.scissor.y0, w, h))
            g178NoteVramWriteRectTagged(vram, fbBlock,
                              c.frame.fbw, c.frame.psm,
                              c.scissor.x0, c.scissor.y0, w, h,
                              14u); // G582 source 14: g286 transient publish, color
        if (((c.zbuf >> 32u) & 1u) == 0u)
        {
            const uint32_t zbpBlock = static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u;
            const uint32_t zpsm = static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu);
            if (g582.take(g582Depth, zbpBlock, c.frame.fbw, zpsm,
                          c.scissor.x0, c.scissor.y0, w, h))
                g178NoteVramWriteRectTagged(vram, zbpBlock,
                                  c.frame.fbw, zpsm,
                                  c.scissor.x0, c.scissor.y0, w, h,
                                  15u); // G582 source 15: g286 transient publish, depth
        }
    }
    g582.finish("g286trn");
}

// An inline (non-recordable) primitive is about to rasterize on the CPU with the LIVE GS state:
// materialize any resident target its frame/Z/texture/CLUT pages touch. Reuses the exact G254
// range helpers via a candidate entry so PSM/scissor semantics can never diverge from the
// recorded-entry tests.
static void g261PrepareInline(const GSContext &ctx, const GSPrimReg &prim,
                              const GSTexClutReg &texclut)
{
    G144Entry cand{};
    cand.ctx = ctx;
    cand.prim = prim;
    cand.texclut = texclut;
    const G144BlockRange color = g144TargetRange(cand);
    const G254Surface depth = g254DepthSurface(cand);
    const bool tme = prim.tme;
    const G144BlockRange tex = tme ? g144TextureRange(cand) : G144BlockRange{};
    const G144BlockRange clut =
        (tme && g144IsPalettedPsm(ctx.tex0.psm)) ? g144ClutRange(cand) : G144BlockRange{};
    const bool unknown = (!color.valid &&
                          ctx.scissor.x1 >= ctx.scissor.x0 && ctx.scissor.y1 >= ctx.scissor.y0) ||
                         (depth.active && !depth.range.valid) || (tme && !tex.valid);
    g630MaterializeForRanges(tex, color, unknown, unknown, kG261MatInline);
    if (depth.active)
        g630MaterializeForRanges(G144BlockRange{}, depth.range, false, !depth.range.valid,
                                 kG261MatInline);
    if (clut.valid)
        g630MaterializeForRanges(clut, G144BlockRange{}, false, false, kG261MatInline);
    g630MaterializeDepth();
    if (!g261WaveOn() || !g261AnyDirty())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        if (unknown || !r.range.valid ||
            (color.valid && g144RangeOverlaps(r.range, color)) ||
            (depth.active && depth.range.valid && g144RangeOverlaps(r.range, depth.range)) ||
            (tex.valid && g144RangeOverlaps(r.range, tex)) ||
            (clut.valid && g144RangeOverlaps(r.range, clut)))
            g261Materialize(static_cast<int>(i), kG261MatInline);
    }
}

// The whole-flush GPU attempt. Called from BOTH G144 flush closures (worker thread mid-frame /
// EE thread at the frame barrier — never concurrently, see the barrier contract in
// dc2_game_override.cpp). Returns true when the batch was rendered on the GPU AND its pixels are
// already written back into guest VRAM — the caller must then skip the CPU replay and clear the
// list. Any unsupported entry → false (caller runs the proven CPU replay for the WHOLE flush).
// ---------------------------------------------------------------------------------------------
// G305: one-slot move-owned ASYNC native submit (default-off DC2_G305_ASYNC=1).
//
// The pole (G303/G304) is the GS worker's TOTAL synchronous cost; ~5% of it is the per-flush
// backend `fut.get()` wait (G299). For a skip-readback, no-guest-depth color wave the whole
// post-submit epilogue is FRONT-END-derived bookkeeping (residency/gen/snapshot) — nothing reads
// the backend's output (readback is empty). So such a wave can be submitted WITHOUT blocking: the
// batch is moved into a backend-owned slot, the epilogue commits immediately, and the render
// overlaps the GS worker's next-flush register-replay + prep. Because the backend is a single
// FIFO worker, GL command order is preserved automatically; any later synchronous backend call
// self-fences on that FIFO. The only contract the async break needs to honour explicitly is the
// delayed-failure fallback: today a synchronous submit==false drives the whole-list CPU replay
// (g_g144List still intact). So on async submit we RETAIN g_g144List in a pending record and,
// before the next flush prepares anything (drain at every flush top) or a materialization
// consumer reads a target, we drain the packet and — only if the backend actually FAILED — re-run
// the proven sequential closure (sync GPU re-attempt -> CPU band replay) on the retained batch.
// At most one packet is in flight (drain precedes every submit), so a single owned slot suffices.
static bool g305AsyncOn() { static const bool v = envFlagEnabled("DC2_G305_ASYNC"); return v; }
static bool g305StatOn()  { static const bool v = envFlagEnabled("DC2_G305_STAT");  return v; }

// G444 CEILING PROBE (default-off, DC2_G444_ASYNC=1). G305's async break excludes every G310 flush
// (`!g310On()`), which on this route is every flush — so the mechanism has never been priced. G444
// measured the GS worker at 45.0 ms/f = front 23.0 + backend 22.0 fully SERIALIZED across the
// front/GL-thread boundary, so the overlap this probe buys is bounded above by ~20 ms/f. The probe
// changes only WHICH gate admits a flush to the existing move-owned one-slot path; submission,
// drain, ordering and the delayed-failure fallback are byte-unchanged. Output correctness under
// G310's completion-ordered freeze is exactly what is being tested — treat a pass as a lever and a
// fail as a priced ceiling. Never default-on without the full route matrix.
static bool g444AsyncOn() { static const bool v = envFlagEnabled("DC2_G444_ASYNC"); return v; }

// G640: defined below in g640_async_pipeline.inc (which needs g_g305Pending / g493LateDrainOne and
// therefore cannot be included above this point). A static function may be declared here and
// defined later in the same TU.
static bool g640AsyncOn();

// Every drain stays armed for the whole paired run. This is deliberately selected-by-lever,
// not selected-by-current-arm: a candidate packet may still be pending when the tick changes to
// control, and that packet must be retired before any consumer or next submission proceeds.
static bool g305DrainRequired()
{
    return g305AsyncOn() || g444AsyncOn() || g452AsyncSelected() || g640AsyncOn();
}

struct G305Pending
{
    std::vector<G144Entry> list;
    GS *gs = nullptr;
};
static std::deque<G305Pending> g_g305Pending;       // FIFO matches backend async-slot order
static bool g_g305ForceSync = false;                // set during failure-replay: suppress re-async + re-drain
static std::atomic<uint64_t> g_g305Submitted{0};    // async packets submitted
static std::atomic<uint64_t> g_g305Drained{0};      // packets drained (== submitted at steady state)
static std::atomic<uint64_t> g_g305Failed{0};       // backend render failures caught at drain (expect 0)

// G493: the late-drain lever, its predicates and its [G493:ovl] census. Included here because it
// needs g_g305Pending / g_g305ForceSync / g_g305Drained above and must be visible at the flush's
// submit point below.
#include "g493_late_drain.inc"

// G640: the asynchronous pipeline proper — the entry-storage recycling defect G305 introduced, the
// configurable ring depth, and the drain-to-depth policy. MUST follow g493_late_drain.inc
// (g640LateDrainToDepth calls g493LateDrainOne) and rasterizer_command_graph.inc (g_g260Pool).
#include "g640_async_pipeline.inc"

// Drain the in-flight async packet (if any) and resolve the delayed-failure contract. Cheap no-op
// when nothing is pending; safe to call at every consumer edge. Runs only on the front-end thread.
static bool g305DrainOne()
{
    if (g_g305Pending.empty() || g_g305ForceSync)
        return false;
    G305Pending pending = std::move(g_g305Pending.front());
    g_g305Pending.pop_front();
    bool ok = true;
    // G493: when the late drain already waited on this packet and saw it FAIL, the backend slot is
    // gone — do not wait again, just run the replay here, where s_batch is not live.
    if (g_g493FailPending)
    {
        g_g493FailPending = false;
        ok = false;
    }
    else
    {
        const auto g493T0 = g493DrainBegin();
        g178_backend_drain_async(ok);
        g493DrainEnd(g493T0);
    }
    const uint64_t nDrained = g_g305Drained.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g305StatOn() && (nDrained % 1000u) == 0u)
        std::fprintf(stderr, "[G305:async] submitted=%llu drained=%llu failed=%llu\n",
                     (unsigned long long)g_g305Submitted.load(std::memory_order_relaxed),
                     (unsigned long long)nDrained,
                     (unsigned long long)g_g305Failed.load(std::memory_order_relaxed));
    if (ok)
    {
        // G640: the retained record's entry storage goes BACK to the executor's pool instead of
        // being destroyed here. Exact — the list is already consumed; only its allocation survives.
        g640ReturnPendingStorage(std::move(pending.list));
        g640StatReport();
        return true;
    }
    // Rare: the backend render/readback failed AFTER the optimistic front-end commit. Void the
    // optimistic snapshot and re-run the proven sequential drain (which itself re-attempts the GPU
    // synchronously and, on failure, runs the whole materialize-chain + CPU band replay) on the
    // retained batch, saving/restoring whatever list the current caller had already staged.
    g_g305Failed.fetch_add(1u, std::memory_order_relaxed);
    g_g178FbSnap.clear();
    std::vector<G144Entry> savedList;
    savedList.swap(g_g144List);
    GS *savedGs = g_g144LastGs;
    g_g144List.swap(pending.list);
    g_g144LastGs = pending.gs;
    g_g305ForceSync = true;
    if (g_g144FlushSeqFn)
        g_g144FlushSeqFn();
    g_g305ForceSync = false;
    g_g144List.swap(savedList);
    g_g144LastGs = savedGs;
    // G640: after the replay `savedList` holds the (now consumed) buffer the record carried; recycle
    // that one, not `pending.list`, which the swaps above left empty.
    g640ReturnPendingStorage(std::move(savedList));
    g640StatReport();
    if (g305StatOn())
        std::fprintf(stderr, "[G305:async] backend FAILURE recovered via CPU replay (n=%llu)\n",
                     (unsigned long long)g_g305Failed.load(std::memory_order_relaxed));
    return true;
}

static void g305DrainPending()
{
    while (g305DrainOne()) {}
}

// G581 candidate: a CPU fallback only changes the color/depth surfaces replayed from g_g144List.
// The legacy blanket clear invalidates unrelated persistent FBO snapshots too; on alternating
// display targets that turns every later display batch into a full readback + VRAM pack + upload.
// Preserve the map here, then publish conservative per-surface page-generation bumps after the
// proven CPU replay (g242NoteCpuReplayWrites below). Any overlapping or wrapped alias therefore
// still fails its next gen comparison, while disjoint targets remain resident.
//
// G582 PROMOTED THIS DEFAULT-ON. Evidence carried from G581 and re-confirmed on the promoted
// binary: 3,378 forced full-display publications in the Ridepod boss interval fall to zero, the
// `fbz` bucket drops ~7.4 -> ~2.5 ms/frame, and four whole-run arms measured -6.383 +/- 0.262 ms/f
// (-12.06%, t=-24.36) on matched boss windows. MAP-15 is the gate route for this mechanism
// (GPU-residency publication / page ownership) and passed with defectPerSky=0.0%.
// `DC2_G581_NO_SCOPED_CPU_SNAP=1` is the rollback; `DC2_G581_SCOPED_CPU_SNAP=0` also forces off.
static bool g581ScopedCpuSnapOn()
{
    static const bool on = !envFlagEnabled("DC2_G581_NO_SCOPED_CPU_SNAP") &&
                           !envFlagDisabled("DC2_G581_SCOPED_CPU_SNAP");
    return on;
}

// G582: the lever is default-ON now, so its trace must be opt-in or every run prints on the
// replay path. The COUNTERS stay unconditional -- a null paired reading on this lever is worthless
// without something proving the arms diverged (g538_null_ab_needs_a_path_counter).
static bool g581StatOn()
{
    static const bool on = envFlagEnabled("DC2_G581_STAT");
    return on;
}

static std::atomic<uint64_t> g_g581PreservedClears{0u};
static std::atomic<uint64_t> g_g581CpuReplays{0u};
static std::atomic<uint64_t> g_g581ColorBumps{0u};
static std::atomic<uint64_t> g_g581DepthBumps{0u};

static void g581InvalidateCpuFallbackSnapshots()
{
    if (g581ScopedCpuSnapOn())
        g_g581PreservedClears.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g178FbSnap.clear();
    // G591: this is the single hook every CPU-fallback path in this unit already calls, so it is
    // where the private depth mirror's authority claim dies. A fallback means the batch did NOT
    // complete the upload/render/readback round trip, so nothing proves the resident depth texture
    // still equals the mirror over the claimed rows. Fail closed here rather than enumerating the
    // ~20 return-false sites.
    g591PrivZDrop();
}

