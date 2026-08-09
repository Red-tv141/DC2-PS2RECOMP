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
                                     uint32_t g264SkipMask = 0u)
{
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

// The flush closures are about to run the CPU band replay for g_g144List (the GPU attempt
// failed or was ineligible). Any entry whose target/Z/texture/CLUT pages touch a resident
// target needs those pages materialized first — the replay reads and writes guest VRAM directly.
static void g261PrepareCpuReplay()
{
    if (!g261WaveOn() || !g261AnyDirty() || g_g144List.empty())
        return;
    for (const G144Entry &e : g_g144List)
    {
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
                g261Materialize(static_cast<int>(i), kG261MatCpuReplay);
            }
        }
    }
}

// A CPU band replay just wrote g_g144List into guest VRAM. Under waves the G260 pre-execution
// generation bump is disabled (it would poison GPU-resident targets with stale-VRAM uploads),
// so publish the RTT-family write generations HERE instead — the exact bump an inline draw
// would have done, after the replay actually changed the bytes.
static void g261NoteCpuRttWrites(uint8_t *vram)
{
    if (!g261WaveOn() || vram == nullptr)
        return;
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
            g178NoteVramWriteRect(vram, framePageBaseToBlock(c.frame.fbp),
                                  c.frame.fbw, c.frame.psm,
                                  c.scissor.x0, c.scissor.y0, w, h);
            if (((c.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRect(vram,
                                      static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                      c.frame.fbw,
                                      static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                      c.scissor.x0, c.scissor.y0, w, h);
        }
    }
}

// G286 transient targets deliberately do not join the persistent G261 ownership table, but a
// rejected GPU batch still falls back to the deferred CPU replay. Publish those exact CPU writes
// too, so a later texture/readback consumer cannot retain a generation from before the fallback.
static void g286NoteCpuTransientWrites(uint8_t *vram)
{
    if (!g286TransientTargetOn() || vram == nullptr)
        return;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        if (!g286TransientTarget(c.frame.fbp) ||
            c.scissor.x1 < c.scissor.x0 || c.scissor.y1 < c.scissor.y0)
            continue;
        const uint32_t w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
        const uint32_t h = static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1);
        g178NoteVramWriteRect(vram, framePageBaseToBlock(c.frame.fbp),
                              c.frame.fbw, c.frame.psm,
                              c.scissor.x0, c.scissor.y0, w, h);
        if (((c.zbuf >> 32u) & 1u) == 0u)
            g178NoteVramWriteRect(vram,
                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                  c.frame.fbw,
                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                  c.scissor.x0, c.scissor.y0, w, h);
    }
}

// An inline (non-recordable) primitive is about to rasterize on the CPU with the LIVE GS state:
// materialize any resident target its frame/Z/texture/CLUT pages touch. Reuses the exact G254
// range helpers via a candidate entry so PSM/scissor semantics can never diverge from the
// recorded-entry tests.
static void g261PrepareInline(const GSContext &ctx, const GSPrimReg &prim,
                              const GSTexClutReg &texclut)
{
    if (!g261WaveOn() || !g261AnyDirty())
        return;
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

// Every drain stays armed for the whole paired run. This is deliberately selected-by-lever,
// not selected-by-current-arm: a candidate packet may still be pending when the tick changes to
// control, and that packet must be retired before any consumer or next submission proceeds.
static bool g305DrainRequired()
{
    return g305AsyncOn() || g444AsyncOn() || g452AsyncSelected();
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
        return true;
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
    if (g305StatOn())
        std::fprintf(stderr, "[G305:async] backend FAILURE recovered via CPU replay (n=%llu)\n",
                     (unsigned long long)g_g305Failed.load(std::memory_order_relaxed));
    return true;
}

static void g305DrainPending()
{
    while (g305DrainOne()) {}
}

static bool g178TryFlushGpu(GSRasterizer *self, GS *gs, uint8_t *vram, uint32_t vramSize)
{
    g324NoteObserver(0u); // next native flush is a G310/FBO state observer
    G324FlushDiag g324Diag{};
    const bool g324IsArmed = g324Armed();
    G324DiagScope g324Scope(g324IsArmed ? &g324Diag : nullptr);
    // G493: under the late-drain lever this ONE drain moves to just before this flush's own submit,
    // so the pending render overlaps everything below instead of nothing. It still runs here when a
    // late drain deferred a FAILURE to us — that replay re-enters the flush sequence and must not
    // happen once s_batch is live. Every other g305DrainPending() call site is untouched.
    if (g305DrainRequired() && (!g493LateDrainActive() || g_g493FailPending))
        g305DrainPending();
    if (!g178GpuOn() || g_g144List.empty() || vram == nullptr)
    {
        g_g266FlushRej = kG266FrOff;
        return false;
    }
    if (!g178_backend_ready())
    {
        g_g266FlushRej = kG266FrOff;
        return false;
    }
    g_g266FlushRej = kG266FrNone;
    G310BatchFreeze g310Freeze;
    g_g178Flushes.fetch_add(1u, std::memory_order_relaxed);
    // Resolve the exact 0x14a alias suffix before the generic owner-conflict audit: this batch
    // itself is the proven destructive prefix writer.
    const uint32_t g289DirectDisplaySrc = g289DirectDisplayToken();
    bool g289ConsumeOwner = g289CanConsumeOwner(vram, vramSize);
    bool g289TryOwnerView = false;
    if (g345AnyOwnerActive() && !g289ConsumeOwner)
    {
        if (g289CanTryOwnerView(vram, vramSize))
            g289TryOwnerView = true;
        else
        {
            uint32_t reason = 0u;
            size_t hitEntry = 0u;
            if (g289BatchDisjointOwner(reason, hitEntry))
            {
                const uint64_t n =
                    g_g289Carries.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 8u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:carry] n=%llu version=%llu entries=%zu\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(s_g289Owner.version),
                                 g_g144List.size());
            }
            else
            {
                const uint64_t n =
                    g_g289Unexpected.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 8u || (n % 256u) == 0u))
                {
                    const G144Entry &e =
                        g_g144List[std::min(hitEntry, g_g144List.size() - 1u)];
                    const G144BlockRange color = g144TargetRange(e);
                    const G144BlockRange tex =
                        e.prim.tme ? g144TextureRange(e) : G144BlockRange{};
                    const G144BlockRange clut =
                        (e.prim.tme && g144IsPalettedPsm(e.ctx.tex0.psm))
                            ? g144ClutRange(e) : G144BlockRange{};
                    std::fprintf(
                        stderr,
                        "[G289:owner-conflict] n=%llu version=%llu reason=0x%x "
                        "entry=%zu/%zu draw(fbp=%03x/%u/%x sc=%u,%u..%u,%u "
                        "prim=%u tme=%u abe=%u fst=%u tex=%x/%u/%x/%u/%u "
                        "tfx=%u tcc=%u clut=%x/%x texa=%u/%u/%u "
                        "uv=(%u,%u)(%u,%u)(%u,%u)) "
                        "ranges(color=%x..%x tex=%x..%x clut=%x..%x)\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(s_g289Owner.version), reason,
                        hitEntry, g_g144List.size(), e.ctx.frame.fbp,
                        static_cast<unsigned>(e.ctx.frame.fbw), e.ctx.frame.psm,
                        e.ctx.scissor.x0, e.ctx.scissor.y0, e.ctx.scissor.x1,
                        e.ctx.scissor.y1, static_cast<unsigned>(e.prim.type),
                        e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                        e.prim.fst ? 1u : 0u, e.ctx.tex0.tbp0,
                        static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm,
                        static_cast<unsigned>(e.ctx.tex0.tw),
                        static_cast<unsigned>(e.ctx.tex0.th),
                        static_cast<unsigned>(e.ctx.tex0.tfx),
                        static_cast<unsigned>(e.ctx.tex0.tcc), e.ctx.tex0.cbp,
                        e.ctx.tex0.cpsm, static_cast<unsigned>(e.texa.ta0),
                        e.texa.aem ? 1u : 0u, static_cast<unsigned>(e.texa.ta1),
                        static_cast<unsigned>(e.v[0].u), static_cast<unsigned>(e.v[0].v),
                        static_cast<unsigned>(e.v[1].u), static_cast<unsigned>(e.v[1].v),
                        static_cast<unsigned>(e.v[2].u), static_cast<unsigned>(e.v[2].v),
                        color.first, color.last, tex.first, tex.last,
                        clut.first, clut.last);
                }
                (void)g289MaterializeOwner(10);
            }
        }
    }
    G289ConsumeGuard g289Guard{g289ConsumeOwner};
    G289OwnerViewGuard g289OwnerViewGuard{g289TryOwnerView};
    G289DirectGuard g289DirectGuard{g289DirectDisplaySrc != 0u};
    // G288: a G284-deferred display owner must be published before this native batch can read
    // overlapping guest-VRAM bytes through framebuffer upload, depth, texture, or CLUT paths.
    // G289's one exact display-grab sprite consumes that same owner directly from its FBO.
    if (g289DirectDisplaySrc == 0u)
        g288PublishDisplayForGpuBatch();
    // G273 diagnostic: split the proven GPU flush into front-end preparation, synchronous backend
    // submit, and post-submit VRAM/depth publication. Quiet unless DC2_G273_PROFILE=1.
    static const bool s_g273Profile = envFlagEnabled("DC2_G273_PROFILE");
    // G298 diagnosis: split G273's large front-end "pre" bucket at architecture boundaries.
    // Aggregate-only, cached, and behavior-inert unless explicitly requested.
    // G523: FIXED-per-flush vs PER-ENTRY decomposition of the five `pre` passes. G429's law says an
    // inclusive per-flush timer cannot say whether its bucket is conserved work or per-flush
    // overhead, and the roadmap has carried "33.35 GPU flushes/frame x pre 0.140 ms = 4.67 ms/f,
    // fixed share never separated" since G521. Entry counts per flush vary over three orders of
    // magnitude on this route (`[G178:stat] lastBatch` reads 1, 8 and 17 draws on consecutive
    // reports against a 298.6 mean), so a least-squares fit of each pass's per-flush nanoseconds
    // against that flush's ENTRY COUNT recovers both numbers with no behaviour change:
    //   intercept = the cost a flush pays carrying zero entries  => recoverable ONLY by having
    //               fewer flushes;
    //   slope     = the marginal cost per entry                  => conserved across any
    //               re-batching, recoverable only by making the per-entry work cheaper.
    // Reuses G298's seven timestamps and adds two laps inside `class` (whose 0.064 ms/flush is the
    // largest single pass) so its per-flush PROLOGUE, its per-entry LOOP and its EPILOGUE are
    // separated too. Behaviour-pure; implies G298's timestamps. Armed by DC2_G523_FIT=1.
    static const bool s_g523Fit = envFlagEnabled("DC2_G523_FIT");
    static const bool s_g298Profile = envFlagEnabled("DC2_G298_PROFILE") || s_g523Fit;
    // G323: split G298 T2->T3 dependency preparation into exclusive complete-operation families.
    // The empty arm takes the same cached scope timestamps but retains no counters, bounding timer
    // self-cost without changing renderer behavior or dependency ordering.
    static const bool s_g323Census = envFlagEnabled("DC2_G323_CENSUS");
    static const bool s_g323Empty = !s_g323Census && envFlagEnabled("DC2_G323_EMPTY");
    const bool g323Armed = s_g323Census || s_g323Empty;
    const auto g273T0 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto g298T0 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};

    const uint32_t fbp = g_g144List[0].ctx.frame.fbp;
    const bool g286Transient = g286TransientTarget(fbp);
    if (g286Transient)
    {
        g_g286GpuAttempts.fetch_add(1u, std::memory_order_relaxed);
        if (g289CensusOn())
        {
            static std::atomic<uint64_t> s_g289ConsumerN{0};
            const uint64_t n =
                s_g289ConsumerN.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                const G144BlockRange copied = g144RangeForRect(
                    framePageBaseToBlock(0x13bu), GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
                std::fprintf(stderr,
                             "[G289:consumer] n=%llu entries=%zu copied=%x..%x pend=%u\n",
                             static_cast<unsigned long long>(n), g_g144List.size(),
                             copied.first, copied.last, s_g276Pend.active ? 1u : 0u);
                for (size_t i = 0; i < g_g144List.size(); ++i)
                {
                    const G144Entry &e = g_g144List[i];
                    const G144BlockRange tex =
                        e.prim.tme ? g144TextureRange(e) : G144BlockRange{};
                    const G144BlockRange clut =
                        (e.prim.tme && g144IsPalettedPsm(e.ctx.tex0.psm))
                            ? g144ClutRange(e) : G144BlockRange{};
                    const G144BlockRange target = g144TargetRange(e);
                    std::fprintf(
                        stderr,
                        "[G289:entry] n=%llu i=%zu prim=%u tme=%u "
                        "tex(tbp=%x tbw=%u psm=%x wh=%ux%u range=%x..%x hit=%u) "
                        "clut(cbp=%x cpsm=%x range=%x..%x hit=%u) "
                        "target=%x..%x hit=%u\n",
                        static_cast<unsigned long long>(n), i,
                        static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                        e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.tbw),
                        e.ctx.tex0.psm,
                        1u << std::min<uint8_t>(e.ctx.tex0.tw, 15u),
                        1u << std::min<uint8_t>(e.ctx.tex0.th, 15u),
                        tex.first, tex.last, g144RangeOverlaps(copied, tex) ? 1u : 0u,
                        e.ctx.tex0.cbp, e.ctx.tex0.cpsm, clut.first, clut.last,
                        g144RangeOverlaps(copied, clut) ? 1u : 0u,
                        target.first, target.last,
                        g144RangeOverlaps(copied, target) ? 1u : 0u);
                }
            }
        }
        // 0x13b begins inside the physical 0x139 family. Match the existing CPU-replay
        // ownership edge before any FBO upload or texture decode can observe stale VRAM.
        //
        // G524 CEILING PROBE (DC2_G524_NO_TRNMAT=1, default-off, knowingly WRONG output): delete
        // this one call. It is a CONSUMER edge (G492's law) — it publishes GPU-resident 0x139 rows
        // into guest VRAM so the transient sprite's texture decode can read them — so deleting it
        // yields stale texels, not a crash, and prices "what if this readback were free".
        // [G279:mat] c0 = 1.031 ms per materialize x ~1.0/frame = 1.03 ms/f, and [G523:fit]
        // pro0trn = 0.0323 ms/flush x 33.4 = 1.08 ms/f: the same number from two instruments.
        static const bool s_g524NoTrnMat = envFlagEnabled("DC2_G524_NO_TRNMAT");
        if (!s_g524NoTrnMat)
            g261PrepareCpuReplay();
    }
    // G523 prologue lap P0: the g286 transient-target admission (and its g261PrepareCpuReplay).
    const auto g523P0 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const bool g255Rtt = g255GpuRttOn() && g248TargetIndex(fbp) >= 0;
    // G261: GPU-resident RTT wave — this RTT batch renders with readback deferred to a real
    // CPU-consumer edge instead of the per-flush readPixels+swizzle-writeback round-trip.
    const bool g261Wave = g255Rtt && g261WaveOn();
    if (g261WaveOn())
    {
        g_g261VramSize = vramSize; // materialize needs the size; GS::m_vramSize is private
        // G264: bring resident FBOs current with any deferred upload mirrors before the
        // invariant/uploadFb/FBO-bind logic below reasons about them.
        g264FlushMirrorsAtFlush();
    }
    // G523 prologue lap P1: the G261 wave setup, i.e. g264FlushMirrorsAtFlush's deferred
    // VRAM->FBO mirror uploads. The only prologue stage that can do GL work.
    const auto g523P1 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (g255Rtt)
        g255NoteAttempt(fbp);
    // G252: RTT band deferral is intentionally CPU-only during bring-up.  The CPU replay keeps
    // the proven sampler/blend/Z implementation and merely partitions disjoint rows.  A future
    // GPU RTT phase must opt in separately after explicit write->sample barrier verification.
    static const bool s_g252GpuRttEnv = (std::getenv("DC2_G252_GPU_RTT") != nullptr);
    const bool g252GpuRttEnv = g268LiveEnvRead()
                                   ? (std::getenv("DC2_G252_GPU_RTT") != nullptr)
                                   : s_g252GpuRttEnv;
    if (g248TargetIndex(fbp) >= 0 && !g252GpuRttEnv && !g255Rtt)
    {
        g_g266FlushRej = kG266FrCpuOnlyRtt;
        return false;
    }
    // G527: a DISCOVERED render target has no residency slot, no g_g310TargetPageGen row and no
    // FBO — g255Rtt above is false for it (it keys on g248TargetIndex), and so is the legacy
    // CPU-only return, so without this it would fall through the rest of this function as if it
    // were a DISPLAY batch: framebuffer upload, alias/generation reasoning and readback all tuned
    // for 0x0/0x68, applied to an arbitrary address. The value this lever buys is the 8-lane
    // band-parallel CPU replay below, not the GPU. Route it there explicitly.
    if (g527DiscoveredTarget(fbp))
    {
        g_g266FlushRej = kG266FrCpuOnlyRtt;
        return false;
    }
    const uint32_t fbw = g_g144List[0].ctx.frame.fbw;
    const int fbW = static_cast<int>(fbw) * 64;
    constexpr int kFbH = 512;
    if (fbW <= 0 || fbW > 1024)
    {
        g_g266FlushRej = kG266FrFbW;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }
    const bool g272Wave = g272DisplayColorWaveOn() && t_g272DisplayBatch &&
                          g272DisplayIndex(fbp) >= 0;

    // G266: FBW is part of the RESIDENCY identity too. DC2 draws fbp=0x139 through both fbw=2
    // (character work blocks) and fbw=8 (display-grab sprite) every frame, and the backend
    // recreates the FBO on any dimension change — nothing defined survives a width flip.
    // Publish dirty rows under the OLD layout first (materialize reads back with r.fbw), then
    // drop coverage/window/snapshot so the new-width wave starts from a fresh VRAM upload.
    // Pending upload mirrors are dropped after flushing under the old geometry: their rows are
    // VRAM-authoritative by construction and the redefined FBO re-uploads from VRAM anyway.
    if (g261Wave)
    {
        const int g266Ti = g248TargetIndex(fbp);
        G261Res &g266R = g_g261Res[g266Ti];
        if (g266R.fbW > 0 && g266R.fbw != fbw && (g266R.dirty || g266R.covHi >= g266R.covLo))
        {
            if (g266R.dirty)
                g261Materialize(g266Ti, kG261MatResync); // flushes the mirror itself first
            else if (g266R.mirPending)
                g264FlushMirror(g266Ti);
            g266R.mirPending = false;
            g266R.mirLo = 512;
            g266R.mirHi = -1;
            g266R.mirBits = {};
            g266R.covLo = 512;
            g266R.covHi = -1;
            g266R.fbw = fbw;
            g266R.fbW = fbW;
            g261UpdateWindow(g266Ti);
            G178FbSnap &g266Snap = g_g178FbSnap[fbp];
            g266Snap.valid = false;
            g266Snap.genSum = 0;
            g266Snap.rowLo = 1 << 30;
            g266Snap.rowHi = -1;
        }
    }

    // G523 prologue lap P2: the g252/g255 RTT admission, the fbW bounds check, the g272 display
    // -wave predicates and the G266 FBW residency reset (which can run a blocking g261Materialize).
    const auto g523P2 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // Pass 1: validate every entry + derive draw state. All-or-nothing.
    static std::vector<G178EntryState> s_states;
    s_states.clear();
    s_states.reserve(g_g144List.size());
    int rowLo = kFbH, rowHi = -1;
    int depthRowLo = kFbH, depthRowHi = -1;
    int g282RowLo = kFbH, g282RowHi = -1;
    int g282DepthLo = kFbH, g282DepthHi = -1;
    const bool g282Candidate =
        g282TightRowsOn() && fbp == 0x139u && fbw == 8u;
    // G290: accumulate the same exact per-entry row unions WITHOUT engaging the default-off
    // G282 tight/seeding/fan-out machinery (g282MeasuredFamily stays gated on g282Candidate).
    const bool g290Candidate =
        !g282Candidate && g290ExactRowsOn() && fbp == 0x139u && fbw == 8u;
    bool guestDepth = false;
    bool guestDepthWrites = false;
    uint32_t guestZbpPage = 0u;
    uint32_t guestZpsm = 0u;
    // G510: consecutive graph entries almost always carry byte-identical STATE (a mesh's triangles
    // differ only in their vertices), and G178EntryState is a pure function of that state — the
    // ctx/prim/texa/texclut registers, pabe, fogcol and the two scope flags, plus static env gates
    // and per-target predicates that are themselves functions of ctx.frame.fbp. When entry i's state
    // compares byte-equal to entry i-1's, its classification IS entry i-1's, so reuse s_states[i-1]
    // instead of re-deriving it. Exact by construction, and CONSERVATIVE in the safe direction: the
    // comparison is over raw bytes, so struct padding can only ever cause a false MISS (an extra
    // classify call), never a false HIT. Only accepted states are ever reused — the loop returns
    // false on the first rejection, so a rejected entry can never be entry i-1.
    // The two seen-counters inside classify (g262/g265) are census-only; the memo stands down
    // whenever either census is armed so their totals stay exact.
    const bool g510Memo = g510ClsMemoActive() && !g262CensusOn() && !g265CensusOn();
    const bool g510Census = g510CensusOn();
    // G511 INFLATION probe #2 (DC2_G419_AB=clsprobe, arm 1 only): repeat the state-identity test
    // once more per entry and fold it into a sink that reaches an atomic once per FLUSH. `class`
    // reads 1.00 ms/f (§2) with the G510 memo already serving 94.4% of the classifications, so the
    // four memcmps + four scalar compares the memo pays on EVERY entry are the one per-entry price
    // in `pre` that has never been measured. delta / entries-per-frame is that price.
    const bool g511ClsProbe = g511ClsProbeActive();
    uint64_t g511ClsSink = 0u;
    uint64_t g510Eligible = 0u;
    // G523 lap A: everything above is the `class` pass's per-flush PROLOGUE (transient/RTT/wave
    // admission, the G266 FBW residency reset, the s_states reserve). Below is the per-entry loop.
    const auto g523ClsA = s_g298Profile ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    for (size_t g510i = 0; g510i < g_g144List.size(); ++g510i)
    {
        const G144Entry &e = g_g144List[g510i];
        if (e.ctx.frame.fbp != fbp || e.ctx.frame.fbw != fbw)
        {
            static std::atomic<uint32_t> s_aliasColorLog{0};
            const uint32_t n = s_aliasColorLog.fetch_add(1u, std::memory_order_relaxed);
            if (g242StatEnabled() && n < 24u)
                std::fprintf(stderr,
                             "[G242:alias] color-depth n=%u fbp=0x%x zbp=0x%x fbRows=%d..%d zRows=%d..%d fbw=%u\n",
                             n + 1u, fbp, guestZbpPage, rowLo, rowHi,
                             depthRowLo, depthRowHi, fbw);
            g_g266FlushRej = kG266FrMixedTgt;
            // G266: the batch boundary splits on FBP only, so this is expected to be a same-fbp
            // FBW mix — capture both tuples to pin the exact mix shape.
            if (g266CensusOn())
            {
                static std::atomic<uint32_t> s_g266MixLog{0};
                if (s_g266MixLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
                    std::fprintf(stderr,
                                 "[G266:mix] first(fbp=%03x fbw=%u) entry(fbp=%03x fbw=%u prim=%u "
                                 "tme=%u tbp=0x%x tpsm=0x%x) idx=%zu n=%zu\n",
                                 fbp, fbw, e.ctx.frame.fbp, e.ctx.frame.fbw,
                                 static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                                 e.ctx.tex0.tbp0, e.ctx.tex0.psm,
                                 static_cast<size_t>(&e - g_g144List.data()), g_g144List.size());
            }
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false; // mixed targets in one flush — shouldn't happen (block flush on fbp change)
        }
        G178EntryState st;
        // G510: state-identity test against the immediately preceding entry. Each sub-struct is
        // compared whole because every one of them is written by a single struct assignment at
        // capture time (`e.ctx = ctx` etc), so their internal padding travels with their content.
        const bool g510Same =
            (g510Memo || g510Census) && g510i > 0u &&
            std::memcmp(&e.ctx, &g_g144List[g510i - 1u].ctx, sizeof(GSContext)) == 0 &&
            std::memcmp(&e.prim, &g_g144List[g510i - 1u].prim, sizeof(GSPrimReg)) == 0 &&
            std::memcmp(&e.texa, &g_g144List[g510i - 1u].texa, sizeof(GSTexaReg)) == 0 &&
            std::memcmp(&e.texclut, &g_g144List[g510i - 1u].texclut, sizeof(GSTexClutReg)) == 0 &&
            e.pabe == g_g144List[g510i - 1u].pabe &&
            e.fogcol == g_g144List[g510i - 1u].fogcol &&
            e.titleRockScope == g_g144List[g510i - 1u].titleRockScope &&
            e.townDepthScope == g_g144List[g510i - 1u].townDepthScope;
        // G511: the extra comparison sits on the SAME operands as the real one, so it measures the
        // marginal cost of the identity test itself (the second run is cache-warm exactly as a
        // hypothetical replacement test would be). It cannot change behaviour — the result only
        // reaches g511ClsSink.
        if (g511ClsProbe && g510i > 0u)
        {
            const G144Entry &p = g_g144List[g510i - 1u];
            g511ClsSink +=
                (std::memcmp(&e.ctx, &p.ctx, sizeof(GSContext)) == 0 &&
                 std::memcmp(&e.prim, &p.prim, sizeof(GSPrimReg)) == 0 &&
                 std::memcmp(&e.texa, &p.texa, sizeof(GSTexaReg)) == 0 &&
                 std::memcmp(&e.texclut, &p.texclut, sizeof(GSTexClutReg)) == 0 &&
                 e.pabe == p.pabe && e.fogcol == p.fogcol &&
                 e.titleRockScope == p.titleRockScope &&
                 e.townDepthScope == p.townDepthScope)
                    ? 1u : 0u;
        }
        if (g510Same)
            ++g510Eligible;
        if (g510Memo && g510Same)
        {
            st = s_states[g510i - 1u]; // identical state in ⇒ identical classification out
        }
        else if (!g178ClassifyEntry(e, st))
        {
            g_g266FlushRej = kG266FrClassify; // g_g266ClassifySite set inside g262NoteReject
            if (g261Wave)
                g_g261RttClassifyRej.fetch_add(1u, std::memory_order_relaxed);
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear(); // the CPU replay about to run writes VRAM the gens don't track
            if (g510Census)
            {
                g_g510ClsEntries.fetch_add(g510i + 1u, std::memory_order_relaxed);
                g_g510ClsEligible.fetch_add(g510Eligible, std::memory_order_relaxed);
                g_g510Flushes.fetch_add(1u, std::memory_order_relaxed);
            }
            return false;
        }
        // G282: G144 capture already computed conservative primitive rows with the same
        // floor/ceil/scissor rules as drawTriangle/drawSprite. Keep a parallel tight union for
        // the measured alias batch; the legacy scissor union stays intact until all entries pass.
        const int g282EntryLo = std::max(static_cast<int>(st.scY0), e.minY);
        const int g282EntryHi = std::min(static_cast<int>(st.scY1), e.maxY);
        if (st.guestDepth)
        {
            if (!guestDepth)
            {
                guestDepth = true;
                guestZbpPage = st.zbpPage;
                guestZpsm = st.zpsm;
            }
            else if (guestZbpPage != st.zbpPage || guestZpsm != st.zpsm)
            {
                // One GL attachment cannot represent two guest Z targets in one all-or-nothing
                // flush. Preserve exact ordering by sending the whole list to the CPU replay.
                g_g266FlushRej = kG266FrMixedZ;
                g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                g_g178FbSnap.clear();
                return false;
            }
            guestDepthWrites = guestDepthWrites || st.depthWrite;
            if (!st.skip)
            {
                depthRowLo = std::min(depthRowLo, static_cast<int>(st.scY0));
                depthRowHi = std::max(depthRowHi, static_cast<int>(st.scY1));
                if ((g282Candidate || g290Candidate) && g282EntryHi >= g282EntryLo)
                {
                    g282DepthLo = std::min(g282DepthLo, g282EntryLo);
                    g282DepthHi = std::max(g282DepthHi, g282EntryHi);
                }
            }
        }
        s_states.push_back(st);
        if (!st.skip)
        {
            rowLo = std::min(rowLo, static_cast<int>(st.scY0));
            rowHi = std::max(rowHi, static_cast<int>(st.scY1));
            if ((g282Candidate || g290Candidate) && g282EntryHi >= g282EntryLo)
            {
                g282RowLo = std::min(g282RowLo, g282EntryLo);
                g282RowHi = std::max(g282RowHi, g282EntryHi);
            }
        }
    }
    // G523 lap B: the per-entry loop ends here. What follows (the G510/G511 census tails and the
    // G289 owner-view build) is per-flush EPILOGUE.
    const auto g523ClsB = s_g298Profile ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    // G511: one relaxed add per FLUSH (33.4/frame), never per entry (G502).
    if (g511ClsProbe)
        g_g511Sink.fetch_add(g511ClsSink, std::memory_order_relaxed);
    if (g510Census)
    {
        g_g510ClsEntries.fetch_add(g_g144List.size(), std::memory_order_relaxed);
        g_g510ClsEligible.fetch_add(g510Eligible, std::memory_order_relaxed);
        if (g510Memo)
            g_g510ClsServed.fetch_add(g510Eligible, std::memory_order_relaxed);
        const uint64_t n = g_g510Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 2000u) == 0u)
            std::fprintf(stderr,
                         "[G510:cls] flushes=%llu entries=%llu eligible=%llu (%.1f%%) served=%llu "
                         "entriesPerFlush=%.1f\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(
                             g_g510ClsEntries.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_g510ClsEligible.load(std::memory_order_relaxed)),
                         100.0 * static_cast<double>(
                                     g_g510ClsEligible.load(std::memory_order_relaxed)) /
                             static_cast<double>(std::max<uint64_t>(
                                 1u, g_g510ClsEntries.load(std::memory_order_relaxed))),
                         static_cast<unsigned long long>(
                             g_g510ClsServed.load(std::memory_order_relaxed)),
                         static_cast<double>(
                             g_g510ClsEntries.load(std::memory_order_relaxed)) /
                             static_cast<double>(n));
    }
    static std::vector<uint32_t> s_g289OwnerSrc;
    bool g289OwnerViewReady = false;
    if (g289TryOwnerView)
    {
        uint64_t binds = 0u;
        bool builtAlias = false;
        if (g289PrepareOwnerViewSources(s_states, s_g289OwnerSrc, binds, builtAlias))
        {
            g289OwnerViewReady = true;
            const uint64_t n =
                g_g289OwnerViews.fetch_add(1u, std::memory_order_relaxed) + 1u;
            g_g289OwnerBinds.fetch_add(binds, std::memory_order_relaxed);
            if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                std::fprintf(stderr,
                             "[G289:owner-view] n=%llu version=%llu entries=%zu binds=%llu "
                             "alias=%u\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(s_g289Owner.version),
                             g_g144List.size(), static_cast<unsigned long long>(binds),
                             builtAlias ? 1u : 0u);
        }
        else
        {
            s_g289OwnerSrc.clear();
            g289TryOwnerView = false;
            g289OwnerViewGuard.armed = false;
            const uint64_t n =
                g_g289OwnerViewFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                std::fprintf(stderr,
                             "[G289:owner-view-reject] n=%llu version=%llu entries=%zu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(s_g289Owner.version),
                             g_g144List.size());
            (void)g289MaterializeOwner(11);
        }
    }
    else
        s_g289OwnerSrc.clear();
    if (rowHi < rowLo)
        return true; // everything skipped — nothing to draw, nothing to read back
    const auto g298T1 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const int g282LegacyRowLo = rowLo, g282LegacyRowHi = rowHi;
    const int g282LegacyDepthLo = depthRowLo, g282LegacyDepthHi = depthRowHi;
    static std::vector<G282Fanout> s_g282Fanout;
    s_g282Fanout.clear();
    const bool g282MeasuredFamily =
        g282Candidate && guestDepth && !guestDepthWrites &&
        guestZbpPage == 0x0d0u && guestZpsm == 1u;
    if (g282MeasuredFamily)
        g_g282Candidates.fetch_add(1u, std::memory_order_relaxed);
    const bool g282Prepared =
        g282MeasuredFamily &&
        g282PrepareFanout(s_states, guestDepth, guestDepthWrites,
                          guestZbpPage, guestZpsm, s_g282Fanout);
    if (g282MeasuredFamily && !g282Prepared)
        g_g282RejShape.fetch_add(1u, std::memory_order_relaxed);
    const bool g282Tight =
        g282Prepared && g282RowHi >= g282RowLo &&
        (!guestDepth || g282DepthHi >= g282DepthLo);
    if (g282Tight)
    {
        rowLo = g282RowLo;
        rowHi = g282RowHi;
        if (guestDepth)
        {
            depthRowLo = g282DepthLo;
            depthRowHi = g282DepthHi;
        }
        g_g282TightBatches.fetch_add(1u, std::memory_order_relaxed);
    }
    // G290: standalone seeded admission for the measured 0x139 batch (fbw=8, CT32, zbp=0xd0
    // Z24, READ-ONLY Z — every depth entry has ZMSK=1, so no Z readback can ever publish the
    // seeded rows). The G282 structural proof (g282PrepareFanout, fan-out list must stay empty
    // without the alias-view stack) establishes: entry 0 is an exact opaque zero 512x416 clear,
    // every later draw writes color only at rows 32..447 inside the clear, and depth is never
    // written. The physical color/Z page alias (color rows 0..31 == Z rows 416..447, x>=64)
    // therefore holds exactly the post-clear zero bytes at every Z read, which the seeded depth
    // upload below reproduces independent of prior VRAM content (first use included). Exact
    // vertex-bound rows are adopted so upload/readback/residency windows match real coverage.
    bool g290Seeded = false;
    if (g290Candidate && !g282Tight &&
        g_g144List[0].ctx.frame.psm == 0u && guestDepth && !guestDepthWrites &&
        guestZbpPage == 0x0d0u && guestZpsm == 1u &&
        g282RowHi >= g282RowLo && g282DepthHi >= g282DepthLo &&
        g282RowLo == 0 && g282RowHi <= 447 && g282DepthLo >= 32 && g282DepthHi <= 447)
    {
        static std::vector<G282Fanout> s_emptyPlan;
        s_emptyPlan.clear();
        const bool g310StructureReady =
            !g310AliasRetireOn() ||
            (g282PrepareFanout(s_states, guestDepth, guestDepthWrites,
                               guestZbpPage, guestZpsm, s_emptyPlan, false) &&
             g310RetireSeededAliases());
        s_emptyPlan.clear();
        if (g310StructureReady &&
            g282PrepareFanout(s_states, guestDepth, guestDepthWrites,
                              guestZbpPage, guestZpsm, s_emptyPlan, true) &&
            s_emptyPlan.empty())
        {
            g290Seeded = true;
            rowLo = g282RowLo;
            rowHi = g282RowHi;
            depthRowLo = g282DepthLo;
            depthRowHi = g282DepthHi;
        }
        else
            g_g290ExactRejShape.fetch_add(1u, std::memory_order_relaxed);
    }
    if (g255Rtt && guestDepth && !g262WideOn())
    {
        // G255 owns RTT color only. Real-Z batches retain the proven CPU replay until a separate
        // persistent-depth design can satisfy the G249 dirty-generation invariant. G262 widening
        // lifts this via the NON-persistent per-wave Z round-trip below (no ownership to break).
        g_g266FlushRej = kG266FrRttDepth;
        if (g261Wave)
            g_g261RttDepthRej.fetch_add(1u, std::memory_order_relaxed);
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }

    const uint32_t fbBlock = GSInternal::framePageBaseToBlock(fbp);
    if (guestDepth)
    {
        const uint32_t zbpBlock = guestZbpPage << 5u;
        // Separate GL attachments cannot emulate simultaneous color/depth access to the same GS
        // storage.  Keep this rare alias configuration on the proven CPU path.
        const bool conservativeAlias = g242RectsOverlap(
            fbBlock, fbw, GS_PSM_CT32, 0u, static_cast<uint32_t>(rowLo),
            static_cast<uint32_t>(fbW), static_cast<uint32_t>(rowHi - rowLo + 1),
            zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
            static_cast<uint32_t>(fbW), static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
        // G273 promotion: the only relaxed tuple is the measured page-aligned display pair below.
        // The exact marker proves color pages 104..207 and Z pages 208+ are disjoint; every other
        // layout retains G242's conservative slack. Ride the promoted native display-Z stack so
        // its master kill/oracle declines remain authoritative. Per-phase rollback accepts either
        // DC2_G273_NO_EXACT_COLORZ_ALIAS=1 or DC2_G273_EXACT_COLORZ_ALIAS=0.
        // G534 RETIREMENT (2026-08-09) — ⛔ REVERSED BY G539, AND THE REASONING WAS WRONG.
        // G534 made this relaxation default-OFF, blaming it for missing environment game-wide: the
        // theory was that admitting fbp=0x68 puts BOTH display buffers on the GPU wave path against
        // the shared G403/G411 private ZBP=0xd0 mirror that CPU draws also write, and that the
        // mixed depth authority let sky win the depth test over the world.
        //
        // G535 refuted that: the defect was present with this flag BOTH ON (39.0%) and OFF (29.3%),
        // so G273 was never the cause. The real cause was g411PrivateDepth deferring the mirror's
        // publication past the CPU band replay that consumes it, which G535 fixed (default-ON) and
        // G536 completed with page-ownership-aware publication.
        //
        // G534 also measured the retirement as FREE (51.82 vs 51.89 ms/f, null). That measurement
        // was wrong, and it cost two phases to find: with the relaxation off, the display batch is
        // rejected from the GPU flush as `rej=colorzalias` and its ~78 entries per batch fall to the
        // CPU band replay — 32.7 ms/f over 2,078 entries/f on lean MAP-0 (G538 §3). Restoring it
        // measures 18.13-19.28 ms/f against 49.4, i.e. the retirement was the entire ~3x lean MAP-0
        // level regression carried from G534 to G538.
        //
        // ⭐ The lesson (G538): a NULL paired A/B is worthless without a counter proving the two arms
        // took different code paths. `[G290:gpufail] rej=` is that counter here and it is free.
        //
        // G539 PROMOTION (2026-08-09): DEFAULT-ON, kill DC2_G273_NO_EXACT_COLORZ_ALIAS=1.
        // Gated same-binary against the shipped default on: held golden title (211650 at every tick
        // vs a 211648-211650 control, occupancy identical); map_4 adjacent-frame distribution
        // (p90 386 vs 535, better on every quantile) and visual vs ref/dumps/map_4.png; map_4_zoom
        // vs ref/dumps/map_4_zoom.png; MAP-15 sky 0 defective in 99 sky frames (p ~ 4e-8 against
        // G536's 15.9% base rate); inventory character RTT intact on every frame.
        // ⚠️ The `211646±4` frame_001500 smoke gate CANNOT gate this lever - it indexes a frame
        // number, not a scene, so a 2.7x speed change lands the arms on different screens (G539 §1).
        static const bool s_g273ExactAlias = !envFlagEnabled("DC2_G273_NO_EXACT_COLORZ_ALIAS") &&
                                              !envFlagDisabled("DC2_G273_EXACT_COLORZ_ALIAS") &&
                                              g265DisplayZWaveOn();
        const bool measuredPair = s_g273ExactAlias && fbp == 0x68u && fbBlock == 0x0d00u &&
                                  zbpBlock == 0x1a00u && fbw == 8u;
        const bool rawColorZAlias = measuredPair
            ? g273AlignedRectsOverlapExact(
                  fbBlock, fbw, GS_PSM_CT32, 0u, static_cast<uint32_t>(rowLo),
                  static_cast<uint32_t>(fbW), static_cast<uint32_t>(rowHi - rowLo + 1),
                  zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                  static_cast<uint32_t>(fbW), static_cast<uint32_t>(depthRowHi - depthRowLo + 1))
            : conservativeAlias;
        // G290: the structurally-proven seeded batch represents the color/Z page alias exactly
        // (post-clear zeros seeded into the depth upload below), so the alias is not a reject.
        if (g290Seeded)
        {
            const uint64_t n =
                g_g290ExactAdmit.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g290StatOn() && (n <= 8u || (n % 120u) == 0u))
                std::fprintf(stderr,
                             "[G290:seeded] admit=%llu alias=%u rows(c=%d..%d z=%d..%d) "
                             "entries=%zu rejShape=%llu\n",
                             (unsigned long long)n, rawColorZAlias ? 1u : 0u, rowLo, rowHi,
                             depthRowLo, depthRowHi, g_g144List.size(),
                             (unsigned long long)g_g290ExactRejShape.load(
                                 std::memory_order_relaxed));
        }
        // G282's only real overlap is color page row 0 (blocks 313..319) aliasing Z page row
        // 13, cols 1..7. The structurally-proven leading black clear fills those whole CT32
        // pages before any depth-tested draw, and every later color write starts at row 32, so
        // pre-seeding those Z upload pages to zero exactly represents the intra-batch GS alias.
        const bool g282SeededColorZ =
            g282Tight && rawColorZAlias && rowLo == 0 && rowHi <= 447 &&
            depthRowLo >= 32 && depthRowHi <= 447;
        const bool colorZAlias = rawColorZAlias && !g282SeededColorZ && !g290Seeded;
        const bool g282LegacyAlias =
            g282Tight && g242RectsOverlap(
                fbBlock, fbw, GS_PSM_CT32, 0u,
                static_cast<uint32_t>(g282LegacyRowLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(g282LegacyRowHi - g282LegacyRowLo + 1),
                zbpBlock, fbw, GS_PSM_Z24, 0u,
                static_cast<uint32_t>(g282LegacyDepthLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(g282LegacyDepthHi - g282LegacyDepthLo + 1));
        if (g282LegacyAlias && !colorZAlias)
            g_g282AliasSaved.fetch_add(1u, std::memory_order_relaxed);
        if (measuredPair && conservativeAlias && !colorZAlias)
        {
            static std::atomic<uint64_t> s_admit{0};
            const uint64_t n = s_admit.fetch_add(1u, std::memory_order_relaxed) + 1u;
            static const bool s_stat = envFlagEnabled("DC2_G273_STAT");
            if (s_stat && (n <= 16u || (n % 120u) == 0u))
                std::fprintf(stderr,
                             "[G273:alias] admit=%llu colorPages<=207 zPages>=208 rows(c=%d..%d z=%d..%d)\n",
                             static_cast<unsigned long long>(n), rowLo, rowHi,
                             depthRowLo, depthRowHi);
        }
        if (colorZAlias)
        {
            // G282: G266 attributed the one-per-frame fbp=0x139/fbw=8 CPU publication to this
            // branch, while the drain-wide G178 census showed only ZTST=ALWAYS + ZMSK=1 entries
            // for that target (a depth no-op). Log at the rejecting branch itself so concurrent
            // drain diagnostics cannot misattribute another thread's shared reject code.
            if (g282CensusOn())
            {
                static std::atomic<uint64_t> s_aliasN{0};
                const uint64_t n = s_aliasN.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 8u || (n % 120u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G282:colorz] n=%llu fbp=%03x fbw=%u rows=%d..%d "
                                 "zbp=%03x zpsm=%u zrows=%d..%d entries=%zu "
                                 "legacy(c=%d..%d z=%d..%d) tight=%u\n",
                                 (unsigned long long)n, fbp, fbw, rowLo, rowHi,
                                 guestZbpPage, guestZpsm, depthRowLo, depthRowHi,
                                 g_g144List.size(), g282LegacyRowLo, g282LegacyRowHi,
                                 g282LegacyDepthLo, g282LegacyDepthHi, g282Tight ? 1u : 0u);
                    if (n <= 2u)
                    {
                        for (size_t i = 0; i < std::min<size_t>(g_g144List.size(), 16u); ++i)
                        {
                            const G144Entry &e = g_g144List[i];
                            const G178EntryState &st = s_states[i];
                            const uint64_t ts = e.ctx.test;
                            std::fprintf(
                                stderr,
                                "[G282:entry] n=%llu i=%zu prim=%u tme=%u abe=%u "
                                "test=%llx(zte=%u ztst=%u) zbuf=%llx(zmsk=%u) "
                                "guestZ=%u zwrite=%u skip=%u sc=%u,%u..%u,%u "
                                "v0=(%.1f,%.1f,z=%.0f) v1=(%.1f,%.1f,z=%.0f)\n",
                                (unsigned long long)n, i,
                                static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                                e.prim.abe ? 1u : 0u, (unsigned long long)ts,
                                static_cast<unsigned>((ts >> 16u) & 1u),
                                static_cast<unsigned>((ts >> 17u) & 3u),
                                (unsigned long long)e.ctx.zbuf,
                                static_cast<unsigned>((e.ctx.zbuf >> 32u) & 1u),
                                st.guestDepth ? 1u : 0u, st.depthWrite ? 1u : 0u,
                                st.skip ? 1u : 0u, st.scX0, st.scY0, st.scX1, st.scY1,
                                e.v[0].x, e.v[0].y, e.v[0].z,
                                e.v[1].x, e.v[1].y, e.v[1].z);
                        }
                    }
                }
            }
            g_g266FlushRej = kG266FrColorZAlias;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
    }

    // G278 pending-depth consumer audit before ANY pass-2 VRAM reads. A same-key guest-depth
    // continuation consumes the resident backend texture directly and is safe; a cross-key wave
    // must first publish because its upload reads VRAM Z. Texture/CLUT reads are tested against
    // the unpublished row union; an exact disjoint color write may continue, then marks its depth
    // rows backend-stale when it reaches VRAM. Unknown layouts fail closed to publication.
    // CPU-fallback returns below publish again at cause 3.
    bool g278AnyPend = false;
    for (int s = 0; s < kG278PendSlots && !g278AnyPend; ++s)
        g278AnyPend = s_g278PendTab[s].active;
    if (g278AnyPend)
    {
        const bool g337On = g337ZAuthOn();
        // The wave publishes no VRAM color this flush when it rides a skip-readback class; the
        // color-vs-dirty ordering test below is then vacuous for THIS wave (any later VRAM color
        // publication route — g261Materialize / g276 flush — already publishes overlapping
        // pending Z first).
        const bool waveSkipsColorRb = g261Wave || g272Wave;
        const uint64_t nextKey =
            guestDepth ? g242DepthKey(guestZbpPage, guestZpsm, fbw) : 0u;
        const G144BlockRange waveZWhole =
            guestDepth ? g144RangeForRect(guestZbpPage << 5u, GS_PSM_Z24, fbw,
                                          0u, 0u, static_cast<uint32_t>(fbW), 512u)
                       : G144BlockRange{};
        const G144BlockRange target = g144RangeForRect(
            fbBlock, GS_PSM_CT32, fbw, 0u,
            static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
            static_cast<uint32_t>(rowHi - rowLo + 1));
        // Pass 1: per-slot structural decisions (everything except the g_g144List texture scan).
        bool candidate[kG278PendSlots] = {};
        G144BlockRange slotDirty[kG278PendSlots] = {};
        bool anyCandidate = false;
        for (int s = 0; s < kG278PendSlots; ++s)
        {
            G278PendDepth &p = s_g278PendTab[s];
            if (!p.active)
                continue;
            bool mustPublish = p.vram != vram || p.vramSize != vramSize;
            // G290: the seeded batch is owner-INELIGIBLE (its color write range physically
            // overlaps its Z read range), so it must never fall through to a full VRAM Z upload
            // while a same-key pending owner holds newer backend rows. A pending union confined
            // to rows 0..415 is safe to CONTINUE (the seeded rows 416..447 are re-established by
            // the partial upload below and row 416+ is never in the union); any wider pending
            // union publishes.
            if (!p.privateMirror && g290Seeded && p.rowHi > 415)
                mustPublish = true;
            const G144BlockRange dirty = g278PendingDepthRangeP(p);
            const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
            if (!mustPublish && (!dirty.valid || !whole.valid))
                mustPublish = true;
            if (!mustPublish && guestDepth)
            {
                if (nextKey == p.key)
                {
                    mustPublish = g242GpuDepthOn();
                    if (!mustPublish)
                    {
                        const int prospectiveLo = std::min(p.rowLo, depthRowLo);
                        const int prospectiveHi = std::max(p.rowHi, depthRowHi);
                        mustPublish =
                            g278StaleRowsOverlapP(p, prospectiveLo, prospectiveHi);
                    }
                }
                else if (!g337On)
                    mustPublish = true; // legacy: any cross-key Z wave publishes THE owner
                else if (!waveZWhole.valid || g144RangeOverlaps(whole, waveZWhole))
                {
                    // G337: a cross-key wave only consumes this owner when its own Z window
                    // (upload reads VRAM Z; may be a whole-target first-init) can touch the
                    // owner's physical range (cross-fbw same-zbp aliasing).
                    mustPublish = true;
                    g_g337AuditCrossKey.fetch_add(1u, std::memory_order_relaxed);
                }
            }
            if (!mustPublish && !p.privateMirror &&
                !(g337On && waveSkipsColorRb))
            {
                const bool hit = !target.valid || g144RangeOverlaps(dirty, target);
                if (hit)
                {
                    mustPublish = true;
                    if (g337On)
                        g_g337AuditTarget.fetch_add(1u, std::memory_order_relaxed);
                }
            }
            if (!mustPublish && !p.privateMirror && s_g276Pend.active)
            {
                const G144BlockRange display = g277PendingDisplayRange();
                mustPublish = !display.valid || g144RangeOverlaps(whole, display);
            }
            if (mustPublish)
            {
                if (!g278FlushPendSlot(p, 0))
                {
                    g_g266FlushRej = kG266FrZReadback;
                    g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                    g_g178FbSnap.clear();
                    return false;
                }
            }
            else
            {
                // G411 private Z has no physical texture/CLUT read range. Same-key depth consumers
                // continue through the backend attachment; all CPU consumers publish elsewhere.
                if (!p.privateMirror)
                {
                    candidate[s] = true;
                    slotDirty[s] = dirty;
                    anyCandidate = true;
                }
            }
        }
        // Pass 2: one walk over the recorded draws tests texture/CLUT reads against every
        // surviving owner's unpublished rows (ranges are built once per entry).
        if (anyCandidate)
        {
            for (size_t i = 0; i < g_g144List.size(); ++i)
            {
                const G144Entry &e = g_g144List[i];
                if (s_states[i].skip || !e.prim.tme)
                    continue;
                const G144BlockRange tex = g144TextureRange(e);
                G144BlockRange clut{};
                bool haveClut = false;
                for (int s = 0; s < kG278PendSlots; ++s)
                {
                    if (!candidate[s])
                        continue;
                    bool hit = !tex.valid || g144RangeOverlaps(slotDirty[s], tex);
                    if (!hit && g144IsPalettedPsm(e.ctx.tex0.psm))
                    {
                        if (!haveClut)
                        {
                            clut = g144ClutRange(e);
                            haveClut = true;
                        }
                        hit = !clut.valid || g144RangeOverlaps(slotDirty[s], clut);
                    }
                    if (hit)
                    {
                        candidate[s] = false;
                        if (g337ZAuthOn())
                            g_g337AuditTex.fetch_add(1u, std::memory_order_relaxed);
                        if (!g278FlushPendSlot(s_g278PendTab[s], 0))
                        {
                            g_g266FlushRej = kG266FrZReadback;
                            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                            g_g178FbSnap.clear();
                            return false;
                        }
                    }
                }
            }
        }
    }

    // The CPU framebuffer upload below must not observe stale bytes from an aliased persistent Z
    // resource.  This is normally a no-op (display color and Z occupy disjoint GS pages).
    if (!g242SyncOverlappingDepth(vram, fbBlock, fbw, GS_PSM_CT32, 0u,
                                  static_cast<uint32_t>(rowLo),
                                  static_cast<uint32_t>(fbW),
                                  static_cast<uint32_t>(rowHi - rowLo + 1)))
    {
        g_g266FlushRej = kG266FrDepthSetup;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }

    // Pass 2: build the batch.
    // G351 surface classification (used by the census AND, when DC2_G351_SPLIT=1, to route the
    // Owner-B copy-sprite to its private sibling FBO). Row-extent primary key (G350 F1). Routing
    // is stricter than the census class: ONLY the exact single-draw full-screen sprite is routed
    // — merged frame-end batches (scene + sprite in one list) must stay on the fbp key, or the
    // scene draws would land in the sibling FBO and every scene consumer would go stale (G272).
    const bool g351Wide = fbp == 0x139u && fbw == 8u && fbW == 512 && !g_g144List.empty();
    const int g351Cls =
        g351Wide ? g351ClassifyWide139(
                       rowHi, static_cast<int>(g_g144List[0].ctx.scissor.y1))
                 : kG351WNone;
    const bool g351RouteB = g351SplitOn() && g351Cls == kG351WSprite && rowLo == 0 &&
                            g_g144List.size() == 1u &&
                            g_g144List[0].prim.type == GS_PRIM_SPRITE;
    static G178Batch s_batch; // reused across flushes (keeps vector capacity)
    s_batch.fbp = fbp;
    s_batch.fboKey = g351RouteB ? kG351Sprite139 : 0u;
    s_batch.fbW = fbW;
    s_batch.fbH = kFbH;
    s_batch.texUploads.clear();
    s_batch.verts.clear();
    s_batch.draws.clear();
    // G276: coalesce consecutive same-target display-color readbacks. A display batch (0x0/0x68)
    // that is not already a G261/G272 wave defers its color readback: skip it here (the draw still
    // lands in the persistent FBO), and publish the pending union at the next different target or a
    // consumer edge. If a pending readback exists for a DIFFERENT target/width, publish it first so
    // that stale target is never read by this batch.
    const bool g276Coalesce = g276DisplayRbOn() && g265DisplayTarget(fbp) &&
                              !g261Wave && !g272Wave && !g255Rtt;
    if (g276Coalesce && s_g276Pend.active &&
        (s_g276Pend.fbp != fbp || s_g276Pend.fbw != fbw))
        g276FlushPendingDisplay(0);
    // G261: waves leave the result in the FBO (no readPixels, no swizzle-writeback); the raw-
    // alpha RTT store rides the batch flag so the backend does not depend on the G255 env var.
    s_batch.skipReadback = g261Wave || g272Wave || g276Coalesce;
    s_batch.rttRawAlpha = g255Rtt || g286Transient;

    // Depth-clear on target change: mirror g106TitleZClearIfNeeded's fbp-transition semantics and
    // keep the CPU private-Z state machine in sync for any CPU-fallback flush in between.
    {
        static uint32_t s_lastZFbp = 0xFFFFFFFFu;
        // Guest depth already contains the game's clears and persists independently of color-FBP
        // changes.  Synthetic GL clears are only valid for the legacy private-depth path.
        s_batch.clearDepth = !guestDepth && (s_lastZFbp != fbp);
        s_lastZFbp = fbp;
        g106TitleZClearIfNeeded(fbp);
    }

    // Framebuffer upload, GEN-GATED: skipped when (a) the fb pages' write-gen sum is unchanged
    // since our last readback for this fbp (no CPU upload/inline draw touched them — GPU inline
    // writes bump via the drawPrimitive hook, BITBLTs via the ps2_gs_gpu.cpp hooks, and a CPU-
    // fallback flush invalidates the snapshot wholesale below), AND (b) this batch's row window
    // is inside the rows we have ever uploaded/read back (FBO rows outside that are undefined).
    // GL row order = bottom-first: buffer row r = GS row (kFbH-1-r).
    const int rows = rowHi - rowLo + 1;
    // G351: the Owner-B sprite tracks FBO↔VRAM equality for ITS sibling FBO, not the scene's.
    G178FbSnap &snap = g_g178FbSnap[g351RouteB ? kG351Sprite139 : fbp];
    // G277-R exact snapshot gen window: for the G273-measured display tuple, color rows 0..415 map
    // to pages 104..207, provably disjoint from zbp 0xd0's Z pages 208+ (which the full 512-row
    // window falsely included — every Z-wave writeback bump then clobbered G276-deferred FBO rows
    // with a stale-VRAM re-upload: the MAP-0 Max body flicker). Adopt the exact bound only while
    // every row this snapshot reasons about stays <=415; otherwise conservative full height.
    int g277GenRows = kFbH;
    if (g277ExactDisplayGenOn() && fbp == 0x68u && fbBlock == 0x0d00u && fbw == 8u &&
        rowHi <= 415 && (!snap.valid || snap.rowHi <= 415))
    {
        g277GenRows = 416;
        g_g277ExactGenWin.fetch_add(1u, std::memory_order_relaxed);
    }
    // Exact tuple: slack-free aligned sum (g178GenSumRect's +1 slack page would still read Z page
    // 208 through color page 207 and defeat the window — see g277GenSumAligned).
    uint64_t fbGenNow = (g277GenRows == 416)
        ? g277GenSumAligned(fbBlock, fbw, 416u)
        : g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0, 0,
                         static_cast<uint32_t>(fbW), kFbH);
    // G261: a GPU-dirty target must NEVER be overwritten by an uploadFb from (stale) VRAM. If
    // the pages' generations moved while dirty, a CPU writer escaped the materialize hooks —
    // drop residency loudly and let VRAM stay authoritative (the G242/G249 fail-loud pattern).
    // If this batch renders outside the FBO-defined rows, materialize first (VRAM is current
    // for non-dirty rows) and take the normal upload path.
    bool g261ForceNoUpload = false;
    if (g261Wave)
    {
        G261Res &r = g_g261Res[g248TargetIndex(fbp)];
        if (r.dirty)
        {
            const uint64_t g261WinNow = g261WindowGenNow(r, fbp);
            if (r.genAtSkip != g261WinNow)
            {
                const uint64_t n = g_g261Invariants.fetch_add(1u, std::memory_order_relaxed) + 1u;
                std::fprintf(stderr,
                             "[G261:invariant] escaped writer fbp=0x%x genAtSkip=%llu genNow=%llu n=%llu\n",
                             fbp, (unsigned long long)r.genAtSkip,
                             (unsigned long long)g261WinNow, (unsigned long long)n);
                g280DropOverlays(g248TargetIndex(fbp)); // residency dies; nothing publishes them
                r.dirty = false;
                r.dirtyLo = 512;
                r.dirtyHi = -1;
                // Unknown FBO↔VRAM relation — force a fresh upload. Reset IN PLACE (a `snap`
                // reference into the map is live in this scope; erase would invalidate it).
                snap.valid = false;
                snap.genSum = 0;
                snap.rowLo = 1 << 30;
                snap.rowHi = -1;
            }
            else if (rowLo >= r.covLo && rowHi <= r.covHi)
            {
                g261ForceNoUpload = true;
                if (!snap.valid)
                {
                    // A CPU-fallback elsewhere cleared the snapshot map. The ownership hooks
                    // guarantee no CPU write touched THESE pages while dirty, so the FBO is
                    // still the newest content — restore the snapshot instead of re-uploading.
                    snap.valid = true;
                    snap.genSum = fbGenNow;
                    snap.rowLo = r.covLo;
                    snap.rowHi = r.covHi;
                }
            }
            else
            {
                g261Materialize(g248TargetIndex(fbp), kG261MatResync);
                fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0, 0,
                                          static_cast<uint32_t>(fbW), kFbH);
            }
        }
    }
    s_batch.uploadFb = !g261ForceNoUpload && !g289ConsumeOwner &&
                       !(snap.valid && snap.genSum == fbGenNow &&
                         rowLo >= snap.rowLo && rowHi <= snap.rowHi);
    if (s_batch.uploadFb && g345StatOn() && g345FamilyFbp(fbp))
        g345NoteFamUploadFbp(fbp, static_cast<uint64_t>(rowHi - rowLo + 1));
    // G347: classify WHY this 0x139 uploadFb fired, captured before the g277 publish can mutate
    // snap. Paired with the width/pack note at the pack site below.
    int g347Cause139 = -1;
    if (s_batch.uploadFb && fbp == 0x139u && g345StatOn())
        g347Cause139 = !snap.valid ? kG347CSnapInv
                                   : (snap.genSum != fbGenNow ? kG347CGenMoved : kG347CRowsOut);
    // G350 slice-1: bucket EVERY wide 0x139 write by its (uploadFb-cause × scissor-Y × row-window ×
    // draw-count) signature so the multi-owner census can enumerate distinct logical surfaces (the
    // wave flag is trivially true here — see g345_closure.inc). Behavior-pure.
    if ((g345StatOn() || g351SplitOn()) && g351Wide)
    {
        if (g345StatOn())
        {
            int g350cause;
            if (!s_batch.uploadFb)
                g350cause = kG350CauseNoUp;
            else if (!snap.valid)
                g350cause = kG350CauseSnapInv;
            else if (snap.genSum != fbGenNow)
                g350cause = kG350CauseGenMoved;
            else
                g350cause = kG350CauseRowsOut;
            const auto &g350sc = g_g144List[0].ctx.scissor;
            g350NoteWrite139(g350cause, rowLo, rowHi, static_cast<int>(g350sc.y0),
                             static_cast<int>(g350sc.y1),
                             static_cast<uint32_t>(g_g144List.size()));
            // G351: the uploadFb about to fire overwrites FBO content belonging to the PREVIOUS
            // wide writer — note it against that surface, THEN advance the last-writer latch.
            if (s_batch.uploadFb)
                g351NoteCon(kG351SiteUp, g350cause,
                            static_cast<uint64_t>(rowHi - rowLo + 1));
        }
        g351NoteWide139Write(g351Cls);
        if (g351SplitOn())
            g351NoteWide139Routed(g351RouteB, rowLo, rowHi);
    }
    // G356 (behavior-pure, DC2_G356_TRACE=1): Sindain inventory circle-noise diagnosis. For the
    // display targets (0x0/0x68) log the batch decision AND a circle-region noise metric read
    // straight from guest VRAM at batch time (before this batch's own upload/pack). The circle
    // interior sits ~x300..380,y200 in the 512x415 display. meanAbsD(green) is low for the smooth
    // forest field, high for the horizontal-band garbage — so this pins whether the field is still
    // resident in THIS target's VRAM across the field->menu transition, and what uploadFb/skip does.
    {
        static const bool s_g356Tr = (std::getenv("DC2_G356_TRACE") != nullptr);
        if (s_g356Tr && (fbp == 0x0u || fbp == 0x68u))
        {
            static std::atomic<uint32_t> s_g356N{0};
            const uint32_t n = s_g356N.fetch_add(1u, std::memory_order_relaxed) + 1u;
            uint64_t accG = 0, sumG = 0; uint32_t cnt = 0;
            int prevG = -1;
            for (int x = 300; x <= 380; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                        static_cast<uint32_t>(x), 200u);
                uint32_t px = 0;
                if (off + 4u <= vramSize) std::memcpy(&px, vram + off, 4);
                const int gch = static_cast<int>((px >> 8) & 0xFFu);
                sumG += static_cast<uint32_t>(gch);
                if (prevG >= 0) accG += static_cast<uint32_t>(std::abs(gch - prevG));
                prevG = gch; ++cnt;
            }
            const double meanG = cnt ? (double)sumG / cnt : 0.0;
            const double meanD = cnt > 1 ? (double)accG / (cnt - 1) : 0.0;
            std::fprintf(stderr,
                "[G356:disp] n=%u fbp=0x%x fbW=%d fbw=%u rows=%d..%d nDraws=%zu uploadFb=%d skipRb=%d "
                "snapV=%d genMatch=%d circleMeanG=%.1f circleMeanAbsD=%.1f\n",
                n, fbp, fbW, fbw, rowLo, rowHi, g_g144List.size(),
                s_batch.uploadFb ? 1 : 0, s_batch.skipReadback ? 1 : 0,
                snap.valid ? 1 : 0,
                (snap.valid && snap.genSum == fbGenNow) ? 1 : 0,
                meanG, meanD);
            // Per-draw circle-coverage dump: which draws (if any) touch the circle interior
            // (screen row ~200, cols ~250..450) this batch, and from what texture. Capped.
            if (fbp == 0x68u && g_g144List.size() <= 40u)  // chrome batches only (skip 126-glyph text)
            {
                static std::atomic<uint32_t> s_g356D{0};
                for (const G144Entry &e : g_g144List)
                {
                    if ((e.maxY - e.minY) < 80) continue;   // large vertical span only (span is convention-independent)
                    const int ofx = static_cast<int>(e.ctx.xyoffset.ofx);
                    int xmin = 1 << 30, xmax = -(1 << 30);
                    const int nv = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
                    for (int k = 0; k < nv; ++k)
                    {
                        const int xs = (static_cast<int>(e.v[k].x) - ofx) >> 4;
                        if (xs < xmin) xmin = xs;
                        if (xs > xmax) xmax = xs;
                    }
                    if (s_g356D.fetch_add(1u, std::memory_order_relaxed) < 80u)
                        std::fprintf(stderr,
                            "[G356:cdraw] batchN=%u nD=%zu prim=%u tme=%u abe=%u tbp=0x%x psm=0x%x "
                            "cbp=0x%x rows=%d..%d cols=%d..%d rgba=(%u,%u,%u,%u) alpha=0x%llx\n",
                            n, g_g144List.size(), e.prim.type, e.prim.tme, e.prim.abe,
                            e.ctx.tex0.tbp0, static_cast<uint32_t>(e.ctx.tex0.psm),
                            e.ctx.tex0.cbp, e.minY, e.maxY, xmin, xmax,
                            e.v[0].r, e.v[0].g, e.v[0].b, e.v[0].a,
                            static_cast<unsigned long long>(e.ctx.alpha));
                }
            }
        }
    }
    if (s_batch.uploadFb && s_g276Pend.active && s_g276Pend.fbp == fbp)
    {
        // G277-R fail-safe: the FBO holds G276-deferred rows that exist NOWHERE else. Re-uploading
        // (stale) VRAM now would erase them (the Max-body flicker mechanism). Publish the pending
        // union first so the upload below reads current bytes; correctness identical to the
        // pre-G276 per-batch readback ordering. Loud counter — with the exact gen window this
        // should be rare; a hot count means a new unhandled generation mover.
        if (g277GenMoveOn() && fbp == 0x68u && s_g277ChunkValid)
        {
            char buf[160];
            int off = 0;
            for (int i = 0; i < 13; ++i)
            {
                const uint64_t cur = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u,
                                                    static_cast<uint32_t>(i * 32),
                                                    static_cast<uint32_t>(fbW), 32u);
                if (cur != s_g277Chunk[i] && off < 140)
                    off += std::snprintf(buf + off, sizeof(buf) - off, " %d(+%lld)", i,
                                         static_cast<long long>(cur - s_g277Chunk[i]));
            }
            static std::atomic<uint32_t> s_gmLog{0};
            if (s_gmLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                std::fprintf(stderr, "[G277:genmove] movedChunks:%s (rows=chunk*32..+31)\n",
                             off ? buf : " none");
        }
        g276FlushPendingDisplay(2);
        const uint64_t n = g_g277UploadPublish.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 8u)
            std::fprintf(stderr,
                         "[G277:upload-publish] n=%llu fbp=0x%x rows=%d..%d snap(v=%d gen%s)\n",
                         static_cast<unsigned long long>(n), fbp, rowLo, rowHi,
                         snap.valid ? 1 : 0,
                         (snap.valid && snap.genSum != fbGenNow) ? "MOVED" : "=");
    }
    if (s_batch.uploadFb)
    {
        g_g178FbUploads.fetch_add(1u, std::memory_order_relaxed);
        // G417 diagnostic-only CPU split. Keep the allocation/zero, metadata/setup, and joined
        // CT32 row pack separate so the backend GL edge is not blamed for front-end preparation.
        static const bool s_g417Profile = envFlagEnabled("DC2_G417_PROFILE");
        const auto g417PrepT0 = s_g417Profile ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
        // Upload the union of this batch's window and everything previously covered, so the
        // covered-row invariant stays true after a single upload.
        const int upLo = snap.valid ? std::min(rowLo, snap.rowLo) : rowLo;
        const int upHi = snap.valid ? std::max(rowHi, snap.rowHi) : rowHi;
        // G290: the structurally-proven leading 512x416 opaque clear (fbmsk=0, no blend)
        // overwrites every FBO pixel in rows 0..415 before any draw reads destination color,
        // so uploading those rows from VRAM is pure churn — only rows the clear does not cover
        // need real VRAM bytes. Coverage invariant unchanged: rows 0..415 are valid post-clear.
        const int upLoEff = g290Seeded ? std::max(upLo, 416) : upLo;
        // G298 promotion (default on): CT32 maps each logical pixel to one independent 32-bit
        // staging element. Split disjoint guest-row ranges across the existing joined row pool;
        // VRAM is read-only here and the pool joins before backend submission, snapshot
        // publication, or any ownership transition. Keep combined and stage-local kill switches.
        static const bool s_g298ParFbPack =
            !envFlagEnabled("DC2_G298_NO_PAR_PREP") &&
            !envFlagEnabled("DC2_G298_NO_PAR_FBPACK") &&
            !envFlagDisabled("DC2_G298_PAR_FBPACK");
        static const int s_g298FbPackLanes = [] {
            const char *value = std::getenv("DC2_G298_FBPACK_LANES");
            if (value == nullptr)
                return 8;
            return std::max(1, std::min(16, std::atoi(value)));
        }();
        const int packRows = upHi >= upLoEff ? (upHi - upLoEff + 1) : 0;
        // G420: the pack overwrites EVERY staging element of guest rows [upLoEff,upHi], so zeroing
        // that span is pure churn — 1 MiB per 512-wide upload, of which 81-88% is packed over.
        // Zero only the complement. Exact by construction: each element is still written exactly
        // once, and the complement keeps the same zero value it had. The arm is evaluated
        // unconditionally so the instrument's consumer record is never skipped (G419).
        const bool g420ZeroSkip = g419AbArmFor(kG420LeverZero, g420ZeroSkipOn());
        const bool s_g420Profile = g419ProfileOn();
        const auto g417ZeroT0 = (s_g417Profile || s_g420Profile)
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        uint64_t g420ZeroFilled = 0u;
        g420PrepStaging(s_batch.fbPixels, fbW, kFbH, upLoEff, upHi, g420ZeroSkip, &g420ZeroFilled);
        const auto g417ZeroT1 = (s_g417Profile || s_g420Profile)
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        if (s_g420Profile)
            g420NoteZero(fbp, fbw, static_cast<uint64_t>(fbW) * kFbH, g420ZeroFilled,
                         static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 g417ZeroT1 - g417ZeroT0).count()));
        // G419: the CT32 staging pack is the mirror image of G418's readback unpack, so the same
        // exact one-page addrPSMCT32 decomposition applies with paired 8-byte LOADS instead of
        // stores. `g419PackLegacy` selects the original serial per-pixel path verbatim as the
        // control arm; the arm may come from the within-process instrument (DC2_G419_AB=pack).
        const bool g419PackFast = g419AbArmFor(kG419LeverPack, g419FastPackOn());
        // Both front-end levers act at this one call site, so one ballast probe measures the site's
        // critical-path sensitivity for either of them.
        const int g419PackBallast = std::max(g419AbBallastReps(kG419LeverPack),
                                            g419AbBallastReps(kG419LeverLanes));
        const auto packFbRows = [&](int laneLo, int laneHi) {
            g419PackFbRows(s_batch.fbPixels.data(), s_batch.fbPixels.size(),
                           vram, vramSize, fbBlock, fbw, fbW, kFbH,
                           laneLo, laneHi, !g419PackFast);
            // Ballast (diagnostic): repeat the control form N extra times. Packing the same rows out
            // of read-only VRAM into the same staging words is idempotent, so this adds a KNOWN cost
            // at this exact site without changing any result.
            for (int rep = 0; rep < g419PackBallast; ++rep)
                g419PackFbRows(s_batch.fbPixels.data(), s_batch.fbPixels.size(),
                               vram, vramSize, fbBlock, fbw, fbW, kFbH,
                               laneLo, laneHi, true);
        };
        // G347 census: the REAL rows this uploadFb packs into VRAM (post-G290 upLoEff redraw-skip),
        // vs the window rows famUp counts at the gate. If pack << window, the "560 up rows/f"
        // scoping slice-2 is a census-window artifact and the upload is already minimized (G290).
        if (g345StatOn() && g345FamilyFbp(fbp))
        {
            g345NoteFamUploadPackFbp(fbp, static_cast<uint64_t>(packRows));
            if (g347Cause139 >= 0)
            {
                const int wcls = (fbw == 8u) ? (g290Seeded ? kG347W8Seed : kG347W8Plain)
                                             : (fbw == 2u ? kG347W2 : kG347WOther);
                g347NoteUp139(wcls, g347Cause139, fbw, static_cast<uint64_t>(packRows));
            }
        }
        if (g308UploadCensusOn())
        {
            const uint64_t n =
                g_g308Uploads.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint64_t rows =
                g_g308Rows.fetch_add(static_cast<uint64_t>(packRows),
                                     std::memory_order_relaxed) +
                static_cast<uint64_t>(packRows);
            const uint64_t fullRows =
                g_g308FullRows.fetch_add(static_cast<uint64_t>(kFbH),
                                         std::memory_order_relaxed) +
                static_cast<uint64_t>(kFbH);
            if (n <= 8u || (n % 256u) == 0u)
                std::fprintf(stderr,
                             "[G308:census] uploads=%llu rows=%llu/%llu potential=%.1f%% "
                             "last(fbp=%03x fbw=%u rows=%d)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(rows),
                             static_cast<unsigned long long>(fullRows),
                             fullRows != 0u
                                 ? 100.0 * static_cast<double>(fullRows - rows) /
                                       static_cast<double>(fullRows)
                                 : 0.0,
                             fbp, fbw, packRows);
        }
        // G419: G298's row gate lets 4k-16k-pixel packs pay a fixed ~0.085 ms row-pool dispatch that
        // is several times their entire loop. Re-gate on pixels (see g419PackLanes).
        // G420 re-adjudicates the lane COUNT on top of G419's pixel gate. Called unconditionally
        // (consumer-charged attribution) and clamped by g419PackLanes' result, so it can only ever
        // narrow a split the pixel gate already approved.
        const int packLanes = g420PackLaneCount(g419PackLanes(
            packRows, fbW, s_g298ParFbPack && packRows >= 8 ? s_g298FbPackLanes : 1));
        const auto g417PrepT2 = s_g417Profile ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
        // G419: behaviour-pure isolated pack timer. Unlike DC2_G417_PROFILE this arms nothing in the
        // GL backend, so it inserts no finish and can be read alongside an otherwise normal run.
        const bool s_g419Profile = g419ProfileOn();
        const auto g419PackT0 = s_g419Profile ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
        GSRowPool::instance().run(upLoEff, upHi, packLanes, packFbRows);
        if (s_g419Profile && packRows > 0)
            g419NotePack(fbp, fbw, packRows,
                         static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - g419PackT0).count()));
        if (s_g417Profile)
        {
            const auto g417PrepT3 = std::chrono::steady_clock::now();
            struct G417PrepStage
            {
                uint64_t n = 0, rows = 0, bytes = 0;
                uint64_t zeroNs = 0, setupNs = 0, packNs = 0;
            };
            static std::map<uint64_t, G417PrepStage> s_g417Prep;
            static uint64_t s_g417PrepTotal = 0u;
            const uint64_t key = (static_cast<uint64_t>(fbp) << 32) |
                                 (static_cast<uint64_t>(fbW) << 16) |
                                 static_cast<uint32_t>(kFbH);
            G417PrepStage &st = s_g417Prep[key];
            const auto ns = [](const auto &a, const auto &z) {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count());
            };
            ++st.n;
            st.rows += static_cast<uint64_t>(packRows);
            st.bytes += static_cast<uint64_t>(s_batch.fbPixels.size()) * sizeof(uint32_t);
            st.zeroNs += ns(g417ZeroT0, g417ZeroT1);
            st.setupNs += ns(g417PrepT0, g417ZeroT0) +
                          ns(g417ZeroT1, g417PrepT2);
            st.packNs += ns(g417PrepT2, g417PrepT3);
            if ((++s_g417PrepTotal % 512u) == 0u)
            {
                std::fprintf(stderr, "[G417:prep] total=%llu",
                             static_cast<unsigned long long>(s_g417PrepTotal));
                for (const auto &kv : s_g417Prep)
                {
                    const G417PrepStage &e = kv.second;
                    if (e.n == 0u)
                        continue;
                    std::fprintf(
                        stderr,
                        " fbp=0x%x/%ux%u(n=%llu rows=%.1f KiB=%.1f zero=%.3f setup=%.3f pack=%.3f)",
                        static_cast<unsigned>(kv.first >> 32),
                        static_cast<unsigned>((kv.first >> 16) & 0xffffu),
                        static_cast<unsigned>(kv.first & 0xffffu),
                        static_cast<unsigned long long>(e.n),
                        static_cast<double>(e.rows) / e.n,
                        static_cast<double>(e.bytes) / 1024.0 / e.n,
                        static_cast<double>(e.zeroNs) / 1.0e6 / e.n,
                        static_cast<double>(e.setupNs) / 1.0e6 / e.n,
                        static_cast<double>(e.packNs) / 1.0e6 / e.n);
                }
                std::fprintf(stderr, "\n");
            }
        }

        // G347 shadow oracle (census, DC2_G347_SHADOW=1, default-off, PERTURBING GPU readback):
        // read the 0x139 FBO rows this uploadFb is about to overwrite and compare to the just-
        // packed VRAM bytes (s_batch.fbPixels). If FBO already == VRAM the invalidation was
        // spurious and the upload dead; if not, the FBO is stale and the upload live. Split by the
        // captured uploadFb cause so the crossW/snapInv thrash fraction is separable.
        static const bool s_g347Shadow = envFlagEnabled("DC2_G347_SHADOW");
        if (s_g347Shadow && fbp == 0x139u && g347Cause139 >= 0 && packRows > 0)
        {
            static std::vector<uint32_t> s_g347FboPx; // flush-owner thread only
            const int glY = kFbH - 1 - upHi;
            const bool readOk =
                g178_backend_read_color(g351RouteB ? kG351Sprite139 : fbp, fbW, kFbH,
                                        glY, packRows, s_g347FboPx) &&
                s_g347FboPx.size() == static_cast<size_t>(fbW) * packRows;
            uint64_t eqPix = 0u, totPix = 0u;
            if (readOk)
            {
                for (int y = upLoEff; y <= upHi; ++y)
                {
                    const size_t fboRow = static_cast<size_t>(upHi - y) * fbW;
                    const size_t vramRow =
                        static_cast<size_t>(kFbH - 1 - y) * fbW;
                    for (int x = 0; x < fbW; ++x)
                    {
                        ++totPix;
                        if (s_g347FboPx[fboRow + x] == s_batch.fbPixels[vramRow + x])
                            ++eqPix;
                    }
                }
            }
            // Does the g261 residency already cover these rows (dirty + rows inside covLo..covHi)?
            // High cov ⇒ an unrelated .clear() wiped a still-resident snapshot (bounded protect fix);
            // low cov ⇒ genuinely non-resident (needs new residency).
            bool g261Cover = false;
            {
                const int ti = g248TargetIndex(fbp);
                if (ti >= 0 && ti < static_cast<int>(kG248TargetCount))
                {
                    const G261Res &rr = g_g261Res[ti];
                    g261Cover = rr.dirty && upLoEff >= rr.covLo && upHi <= rr.covHi;
                }
            }
            g347NoteShadow(g347Cause139, fbw == 8u ? 0 : 1, readOk, eqPix, totPix, g261Cover);
        }

        // Exact same-run oracle for bring-up: independently reread every packed CT32 pixel using
        // the serial address path after the joined dispatch. This is diagnostic-only because it
        // deliberately repeats the staging work.
        // G419 reuses this oracle for the exact CT32 fast pack (DC2_G419_VERIFY is an alias), and the
        // s_g298ParFbPack gate is dropped so a single-lane fast pack is checked too.
        static const bool s_g298Verify =
            envFlagEnabled("DC2_G298_VERIFY_FBPACK") || envFlagEnabled("DC2_G419_VERIFY");
        if (s_g298Verify && packRows > 0)
        {
            uint64_t bad = 0u;
            for (int y = upLoEff; y <= upHi; ++y)
            {
                const size_t dstRow =
                    static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, fbw, static_cast<uint32_t>(x),
                        static_cast<uint32_t>(y));
                    uint32_t expected = 0u;
                    if (off + 4u <= vramSize)
                        std::memcpy(&expected, vram + off, 4);
                    bad += s_batch.fbPixels[dstRow + x] != expected ? 1u : 0u;
                }
            }
            static std::atomic<uint64_t> s_packs{0u}, s_pixels{0u}, s_bad{0u};
            const uint64_t n =
                s_packs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint64_t pixels = static_cast<uint64_t>(packRows) *
                                    static_cast<uint64_t>(fbW);
            const uint64_t totalPixels =
                s_pixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
            const uint64_t totalBad =
                s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
            if (bad != 0u || n <= 8u || (n % 256u) == 0u)
                std::fprintf(stderr,
                             "[G298:verify] packs=%llu pixels=%llu bad=%llu "
                             "last(fbp=0x%x fbw=%u rows=%d lanes=%d fast=%d bad=%llu)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(totalPixels),
                             static_cast<unsigned long long>(totalBad),
                             fbp, fbw, packRows, packLanes, g419PackFast ? 1 : 0,
                             static_cast<unsigned long long>(bad));
        }
        snap.rowLo = upLo;
        snap.rowHi = upHi;
    }
    else
    {
        g_g178FbUploadSkips.fetch_add(1u, std::memory_order_relaxed);
        // G420: the zero-fill elision needs a live buffer to elide against, and clear() drops
        // size() to 0 so the next uploadFb would value-initialise the whole thing again. Nothing
        // outside uploadFb reads fbPixels (every backend use is gated on b.uploadFb, and
        // validateBatchInputs only size-checks it when uploadFb), so retaining stale bytes here is
        // inert. Retain only when the elision is armable this run, so an instrument arm can never
        // measure "candidate had a warm buffer, control did not".
        if (!g420RetainStagingOn())
            s_batch.fbPixels.clear();
    }

    // G242: keep guest Z resident across GPU flushes. Upload the full validated 512-row target only
    // on first use or after an overlapping guest write; CPU/transfer/frame boundaries materialize
    // just the dirty row union back to VRAM. Float(raw * 2^-32) is exact for uint24.
    static std::vector<float> s_depthUpload;
    int depthGlY = 0;
    int depthRows = 0;
    uint64_t depthKey = 0u;
    uint64_t stagedGuestGen = 0u;
    bool stagedDepthUpload = false;
    const std::vector<float> *depthUpload = nullptr;
    G242DepthState *depthState = nullptr;
    bool g262WaveDepth = false;
    bool g411PrivateDepth = false;
    bool g278PendingSameKey = false;
    bool g278OwnerEligible = false;
    // G535: restores the pre-G535 unconditional deferral of the G411 private-mirror depth owner.
    // DIAGNOSTIC ONLY — setting it reproduces the game-wide missing-environment defect. The
    // reasoning is at the eligibility site below.
    static const bool s_g535PrivZDefer = envFlagEnabled("DC2_G535_PRIVZ_DEFER");
    G144BlockRange g278ColorWrite = g144RangeForRect(
        fbBlock, GS_PSM_CT32, fbw, 0u,
        static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
        static_cast<uint32_t>(rowHi - rowLo + 1));
    if (guestDepth)
    {
        if (guestZpsm != 0x1u || depthRowHi < depthRowLo)
        {
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        depthKey = g242DepthKey(guestZbpPage, guestZpsm, fbw);
        g262WaveDepth = !g242GpuDepthOn(); // classify admits guestDepth without G242 only via G262
        g411PrivateDepth =
            g262WaveDepth && g411SharedGpuDepthOn() &&
            g404SharedZScope(fbp, fbw, guestZbpPage, guestZpsm);
        if (g262WaveDepth)
        {
            const G144BlockRange depthWrite = g144RangeForRect(
                guestZbpPage << 5u, GS_PSM_Z24, fbw, 0u,
                static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            // Separate color/depth backend resources cannot preserve GS physical aliasing across
            // a simultaneous physical write. Exact disjoint row windows are safe: an immediate
            // color publication marks the other depth rows stale below, while retained color is
            // ordered by the G261/G272/G276 materialization barriers.
            g278OwnerEligible =
                g278ColorWrite.valid && depthWrite.valid &&
                !g144RangeOverlaps(g278ColorWrite, depthWrite);
            // G411: color and the shared depth mirror are separate authorities. Their historical
            // GS-page alias is intentionally absent, so deferring the Z publication cannot race
            // an immediate color writeback.
            //
            // G535: that proof is true but covers only the COLOUR/Z page-alias race, and this
            // admission shares more than one authority. The private mirror has a second consumer
            // the deferral never accounted for: CPU-rasterized draws read AND write it directly
            // via g403DisplayZRead/g403DisplayZWrite, gated on g203Uni && g404SharedZScope and
            // therefore INDEPENDENT of G411. While a private-mirror owner is pending, the backend
            // depth texture is the only current copy, yet those draws still depth-test against the
            // stale mirror — and every ranged publication path deliberately skips privateMirror
            // owners on the belief that they have no consumer ("no VRAM transfer consumer",
            // g528_flush_publish.inc). The result is mixed CPU/GPU depth authority: environment
            // geometry alternately wins and loses the depth test, so on ~25% of frames the near
            // ground is replaced by the sky (map_4) and interior floors/walls by unrelated
            // surfaces (game-wide). The unconditional override is retired; g278DeferDepth below
            // refuses the private mirror outright, because g337WideEligible is a SECOND deferral
            // path that this eligibility flag does not gate.
            if (g411PrivateDepth && s_g535PrivZDefer)
                g278OwnerEligible = true;
        }
        // G337: VRAM staleness is a property of the pending owner, not of this wave's own
        // eligibility — a same-key wave must never upload stale VRAM Z over the resident rows,
        // whether or not it will itself defer. Arm off: slot 0 only, original condition.
        for (int s = 0; s < kG278PendSlots && !g278PendingSameKey; ++s)
        {
            const G278PendDepth &pk = s_g278PendTab[s];
            g278PendingSameKey =
                g278DepthRbOn() && (g278OwnerEligible || g337ZAuthOn()) &&
                pk.active && pk.key == depthKey && pk.vram == vram &&
                pk.vramSize == vramSize &&
                pk.privateMirror == g411PrivateDepth;
        }
        // G277 census: a cross-key Z wave re-reads VRAM Z for its own window upload — it is a real
        // consumer of the previous key's deferred readback (conservative lower bound: G274 may
        // actually skip that upload when the other key's gen is unchanged).
        if (g277CensusOn() && s_g277ZRunLive && depthKey != s_g277ZRunKey)
            s_g277ZRunLive = false;
        if (g262WaveDepth)
        {
            // G262 per-wave NON-PERSISTENT guest depth: upload exactly the depth row window from
            // VRAM every wave and read it back immediately after execution (below). VRAM stays
            // authoritative between waves — no G242DepthState, no dirty flag, no generation
            // invariant, no boundary-sync obligation. This is the wave-granular barrier the arc
            // prescribes (coarser than G256's refuted per-draw barriers by the whole batch).
            // A fresh backend depth texture has undefined rows and its first upload must cover
            // the whole target (backend contract). Track first-use per key front-end-side; if
            // the backend ever recreates a texture behind our back the submit fail-closes to
            // the CPU replay (validateBatchInputs), which is safe.
            const bool fullInit = g_g262DepthInit.count(depthKey) == 0u;
            const uint32_t zbpBlock = guestZbpPage << 5;
            // G274: whole-target Z gen. If unchanged since our last writeback for this key, no
            // writer touched VRAM Z between waves, so the resident backend depth texture already
            // equals VRAM Z (last readback wrote GPU->VRAM; VRAM unchanged; GPU untouched) and the
            // re-upload is pure churn. Computed when the census OR the residency lever is active.
            uint64_t zGenNow = 0u;
            bool zGenValid = false;
            if (!g411PrivateDepth && (g274ZTimeOn() || g274ZResidentOn()))
            {
                zGenNow = g178GenSumRect(zbpBlock, fbw, GS_PSM_Z24, 0u, 0u,
                                         static_cast<uint32_t>(fbW), 512u);
                zGenValid = true;
            }
            bool g274SkipUpload = false;
            bool g290PartialSeed = false;
            {
                auto itPrev = g_g274LastZGen.find(depthKey);
                const bool prevKnown = zGenValid && itPrev != g_g274LastZGen.end();
                const bool unchanged = prevKnown && itPrev->second == zGenNow;
                if (g274ZTimeOn() && prevKnown)
                {
                    if (unchanged) g_g274ZRedundant.fetch_add(1u, std::memory_order_relaxed);
                    else           g_g274ZNecessary.fetch_add(1u, std::memory_order_relaxed);
                }
                // Skip only when NOT a first-init (the resident texture already holds every row for
                // this key) AND VRAM Z is provably unchanged. Bit-exact by construction.
                // G534 probe (default OFF): never skip the depth upload under the G411 private
                // mirror. Isolates "the wave renders against a retained depth texture another
                // target left behind" from the windowed-upload contents.
                static const bool s_g534NoSkip = envFlagEnabled("DC2_G534_NOSKIP");
                if (g278PendingSameKey && !(s_g534NoSkip && g411PrivateDepth))
                {
                    // VRAM is intentionally stale while G278 owns this key. The backend texture
                    // is the sole current copy, so an upload would clobber newer depth. A pending
                    // owner can exist only after a successful submit/full-init for this key.
                    g274SkipUpload = true;
                    g_g278UploadSkips.fetch_add(1u, std::memory_order_relaxed);
                    // G290: the seeded batch may CONTINUE on the resident rows (the owner union
                    // is confined to rows 0..415 — the wider case published above), but its
                    // aliased rows 416..447 must be re-established to the post-clear state with
                    // a partial upload; the resident copy holds a previous batch's seed window.
                    if (g290Seeded && !g411PrivateDepth)
                        g290PartialSeed = true;
                }
                else if (g274ZResidentOn() && !fullInit && unchanged && !g282Tight &&
                         !g290Seeded)
                    g274SkipUpload = true;
            }
            // G534 sequence probe (default OFF): one line per guest-depth wave — which colour
            // target, which depth key, whether this wave uploads or rides the retained texture.
            static const bool s_g534ZSeq = envFlagEnabled("DC2_G534_ZSEQ");
            if (s_g534ZSeq)
            {
                static std::atomic<uint64_t> s_n{0};
                const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 60u || (n % 500u) == 0u)
                    std::fprintf(stderr,
                                 "[G534:zseq] n=%llu fbp=%03x key=%llx priv=%u full=%u skip=%u "
                                 "pend=%u rows=%d..%d drows=%d..%d\n",
                                 (unsigned long long)n, fbp, (unsigned long long)depthKey,
                                 g411PrivateDepth ? 1u : 0u, fullInit ? 1u : 0u,
                                 g274SkipUpload ? 1u : 0u, g278PendingSameKey ? 1u : 0u,
                                 rowLo, rowHi, depthRowLo, depthRowHi);
            }
            if (g290PartialSeed)
            {
                // Rows 416..447 only: x0..63 (physical page 312, VRAM-authoritative — outside
                // the 0x139 color family and never in a pending display-Z row union) from VRAM,
                // x64..511 the structural post-clear zero seed. Rows 0..415 stay resident.
                constexpr int kSeedLo = 416, kSeedHi = 447;
                depthRows = kSeedHi - kSeedLo + 1;
                depthGlY = kFbH - 1 - kSeedHi;
                s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
                constexpr float kZToDepth = 1.0f / 4294967296.0f;
                const uint32_t zbpBlockSeed = guestZbpPage << 5;
                for (int y = kSeedLo; y <= kSeedHi; ++y)
                {
                    const size_t dstRow = static_cast<size_t>(kSeedHi - y) * fbW;
                    for (int x = 0; x < 64; ++x)
                    {
                        const uint32_t z = GSMem::ReadZ24(vram, zbpBlockSeed, fbw,
                                                          static_cast<uint32_t>(x),
                                                          static_cast<uint32_t>(y));
                        s_depthUpload[dstRow + x] = static_cast<float>(z) * kZToDepth;
                    }
                    for (int x = 64; x < fbW; ++x)
                        s_depthUpload[dstRow + static_cast<size_t>(x)] = 0.0f;
                }
                depthUpload = &s_depthUpload;
            }
            else if (g274SkipUpload)
            {
                // renderBatch skips glTexSubImage2D when depthUpload==nullptr and renders against
                // the persistent depth texture as-is. depthRows/depthGlY are then unused.
                depthUpload = nullptr;
                depthRows = depthRowHi - depthRowLo + 1;
                depthGlY = kFbH - 1 - depthRowHi;
                g_g274ZUploadsSkipped.fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                // G534 probe (default OFF): force a whole-target depth upload on every wave.
                // If the missing-environment defect disappears under this, the GPU depth texture
                // was carrying rows the windowed upload never refreshed.
                static const bool s_g534FullZ = envFlagEnabled("DC2_G534_FULLZ");
                const bool forceFull = fullInit || s_g534FullZ;
                const int upLoZ = forceFull ? 0 : depthRowLo;
                const int upHiZ = forceFull ? (kFbH - 1) : depthRowHi;
                depthRows = upHiZ - upLoZ + 1;
                depthGlY = kFbH - 1 - upHiZ;
                s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
                constexpr float kZToDepth = 1.0f / 4294967296.0f;
                const auto g274ZUpT0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                // G534 probes (default OFF).
                //   DC2_G534_ZSRC_VRAM=1 — upload guest VRAM Z even under the G411 private mirror,
                //     leaving the readback/CPU authority untouched. Separates "the mirror is stale"
                //     from "the readback is wrong".
                //   DC2_G534_ZDIFF=1 — census only: report how far the mirror and guest VRAM Z
                //     diverge over the uploaded window.
                static const bool s_g534ZSrcVram = envFlagEnabled("DC2_G534_ZSRC_VRAM");
                static const bool s_g534ZDiff = envFlagEnabled("DC2_G534_ZDIFF");
                const bool g534ProbeArm =
                    g411PrivateDepth && (s_g534ZSrcVram || s_g534ZDiff);
                std::atomic<uint64_t> g534Diff{0}, g534Total{0};
                const auto packDepthRows = [&](int laneLo, int laneHi) {
                    if (g534ProbeArm)
                    {
                        uint64_t diff = 0u, total = 0u;
                        for (int y = laneLo; y <= laneHi; ++y)
                        {
                            const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                            for (int x = 0; x < fbW; ++x)
                            {
                                const uint32_t zv = GSMem::ReadZ24(
                                    vram, zbpBlock, fbw,
                                    static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                                const uint32_t zm = g403DisplayZRead(x, y, fbp);
                                ++total;
                                if (zm != zv)
                                    ++diff;
                                s_depthUpload[dstRow + x] =
                                    static_cast<float>(s_g534ZSrcVram ? zv : zm) * kZToDepth;
                            }
                        }
                        g534Diff.fetch_add(diff, std::memory_order_relaxed);
                        g534Total.fetch_add(total, std::memory_order_relaxed);
                        return;
                    }
                    for (int y = laneLo; y <= laneHi; ++y)
                    {
                        const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                        for (int x = 0; x < fbW; ++x)
                        {
                            const uint32_t z = g411PrivateDepth
                                ? g403DisplayZRead(x, y, fbp)
                                : GSMem::ReadZ24(
                                      vram, zbpBlock, fbw,
                                      static_cast<uint32_t>(x),
                                      static_cast<uint32_t>(y));
                            s_depthUpload[dstRow + x] =
                                static_cast<float>(z) * kZToDepth;
                        }
                    }
                };
                const int swizzleLanes =
                    depthRows >= 8 ? g275ZSwizzleLanes() : 1;
                GSRowPool::instance().run(upLoZ, upHiZ, swizzleLanes,
                                          packDepthRows);
                if (s_g534ZDiff && g534Total.load(std::memory_order_relaxed) != 0u)
                {
                    static std::atomic<uint64_t> s_n{0};
                    const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 8u || (n % 240u) == 0u)
                        std::fprintf(stderr,
                                     "[G534:zdiff] n=%llu fbp=%03x rows=%d..%d diff=%llu/%llu\n",
                                     (unsigned long long)n, fbp, upLoZ, upHiZ,
                                     (unsigned long long)g534Diff.load(std::memory_order_relaxed),
                                     (unsigned long long)g534Total.load(std::memory_order_relaxed));
                }
                if (!g411PrivateDepth && (g282Tight || g290Seeded))
                {
                    // The batch's leading black CT32 clear writes physical blocks 313..319,
                    // which are this Z layout's y=416..447, x=64..511 pages. Seed the separate
                    // backend depth attachment with the post-clear value before later draws.
                    const int seedLo = std::max(upLoZ, 416);
                    const int seedHi = std::min(upHiZ, 447);
                    for (int y = seedLo; y <= seedHi; ++y)
                    {
                        const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                        for (int x = 64; x < fbW; ++x)
                            s_depthUpload[dstRow + static_cast<size_t>(x)] = 0.0f;
                    }
                }
                if (g274ZTimeOn())
                    g_g274ZUpNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - g274ZUpT0).count(),
                                          std::memory_order_relaxed);
                depthUpload = &s_depthUpload;
            }
            g_g262DepthWaves.fetch_add(1u, std::memory_order_relaxed);
            if (g411PrivateDepth)
            {
                g_g411DepthBatches.fetch_add(1u, std::memory_order_relaxed);
                if (depthUpload != nullptr)
                    g_g411DepthUploadRows.fetch_add(
                        static_cast<uint64_t>(depthRows), std::memory_order_relaxed);
            }
            if (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp))
                g_g265DepthBatches.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
        G242DepthState &state = g_g242DepthStates[depthKey];
        state.key = depthKey;
        state.zbpPage = guestZbpPage;
        state.zpsm = guestZpsm;
        state.fbw = fbw;
        state.fbW = fbW;
        depthState = &state;
        // A different persistent Z tuple may map the same physical GS pages.  Publish that older
        // owner before this tuple becomes active; never allow two overlapping dirty owners.
        if (!g242SyncOverlappingDepth(vram, guestZbpPage << 5u, fbw, GS_PSM_Z24,
                                      0u, 0u, static_cast<uint32_t>(fbW), kFbH, depthKey))
        {
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        uint64_t guestGenNow = g242GuestGen(state);
        if (state.dirty && state.guestGen != guestGenNow)
        {
            // A CPU writer escaped its mandatory pre-write barrier.  Reading the GPU copy back now
            // would overwrite newer guest bytes, so discard ownership and fall back loudly.
            const uint64_t n = g_g242InvariantFails.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::fprintf(stderr,
                         "[G242:invariant] dirty generation mismatch n=%llu key=0x%llx old=%llu new=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(depthKey),
                         static_cast<unsigned long long>(state.guestGen),
                         static_cast<unsigned long long>(guestGenNow));
            state.valid = false;
            state.dirty = false;
            state.dirtyLo = kFbH;
            state.dirtyHi = -1;
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        const bool needUpload = !state.valid || state.guestGen != guestGenNow;
        depthRows = kFbH;
        depthGlY = 0;
        if (needUpload)
        {
            s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
            const uint32_t zbpBlock = guestZbpPage << 5;
            constexpr float kZToDepth = 1.0f / 4294967296.0f;
            for (int y = 0; y < kFbH; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t z = GSMem::ReadZ24(vram, zbpBlock, fbw,
                                                       static_cast<uint32_t>(x),
                                                       static_cast<uint32_t>(y));
                    s_depthUpload[dstRow + x] = static_cast<float>(z) * kZToDepth;
                }
            }
            depthUpload = &s_depthUpload;
            stagedDepthUpload = true;
            stagedGuestGen = guestGenNow;
            g_g242DepthUploads.fetch_add(1u, std::memory_order_relaxed);
        }
        } // end G242 persistent-depth path (G262 per-wave path above)
    }

    const auto g298T2 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto g323T2 = g323Armed ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    uint64_t g323HygieneNs = 0u, g323LogicalNs = 0u, g323CompositeNs = 0u;
    uint64_t g323BindNs = 0u, g323MaterializeNs = 0u, g323OtherNs = 0u;
    uint64_t g323HygieneCalls = 0u, g323LogicalCalls = 0u, g323CompositeCalls = 0u;
    uint64_t g323BindEntries = 0u, g323MaterializeCalls = 0u;
    const auto g323Ns = [](const auto &begin, const auto &end) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    };
    // G261 FBO-bind pre-pass: entries that sample a GPU-dirty RTT target either bind the
    // producer's FBO color texture directly (skip VRAM decode entirely — the pixels never
    // round-tripped through VRAM) or, when the access shape is not provably in-range/aligned,
    // force that target's materialization and fall through to the normal decode path.
    static std::vector<uint32_t> s_g261FboSrc;
    static std::vector<uint64_t> s_g281ViewKey;
    static std::vector<int8_t> s_g281ViewSrc;
    static std::unordered_set<uint64_t> s_g310BuiltViews;
    bool g261AnyBind = false;
    if ((g261WaveOn() && g261AnyDirty()) || g289DirectDisplaySrc != 0u ||
        g289OwnerViewReady)
    {
        s_g261FboSrc.assign(g_g144List.size(), 0u);
        s_g281ViewKey.assign(g_g144List.size(), 0u);
        s_g281ViewSrc.assign(g_g144List.size(), -1);
        s_g310BuiltViews.clear();
        // G280: re-sync stale/orphaned alias-overlay tiles from VRAM before any bind decision
        // this flush makes (including legacy-path binds) can sample them.
        if (g280AliasOn())
        {
            const auto g323ScopeT0 = g323Armed ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            g280HygienePass();
            if (g323Armed)
            {
                const uint64_t elapsed = g323Ns(g323ScopeT0, std::chrono::steady_clock::now());
                if (s_g323Census)
                {
                    g323HygieneNs += elapsed;
                    ++g323HygieneCalls;
                }
            }
        }
        // G310: build every distinct self-atlas T8 snapshot before this pre-pass can publish
        // 0x139 for a later unsupported consumer. The legacy path likewise decodes textures
        // from the pre-batch image; doing this first preserves that command boundary while
        // keeping the producer FBO private. Entries sharing a texture key share one snapshot.
        if (g310T8ViewOn())
        {
            const auto g323ScopeT0 = g323Armed ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            const auto g324T8T0 = g_g324ActiveDiag != nullptr
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            uint64_t g324T8BackendNs = 0u;
            uint64_t g324T8Jobs = 0u;
            static std::vector<uint32_t> s_g310Map, s_g310Clut;
            for (size_t i = 0; i < g_g144List.size(); ++i)
            {
                const G144Entry &e = g_g144List[i];
                const G178EntryState &st = s_states[i];
                const bool triType =
                    e.prim.type == GS_PRIM_TRIANGLE ||
                    e.prim.type == GS_PRIM_TRISTRIP;
                if (st.texKey == 0u || st.skip || e.ctx.frame.fbp != 0x139u ||
                    !e.prim.tme || e.prim.fst || !triType || !st.bilinear ||
                    e.ctx.tex0.psm != GS_PSM_T8 || !g310T8Base(e.ctx.tex0.tbp0) ||
                    e.ctx.tex0.tbw != 2u || st.texW != 128 || st.texH != 128)
                    continue;
                const uint64_t viewKey = g281ViewKey(st.texKey);
                if (s_g310BuiltViews.count(viewKey) != 0u)
                {
                    s_g281ViewKey[i] = viewKey;
                    s_g281ViewSrc[i] = static_cast<int8_t>(g248TargetIndex(0x139u));
                    continue;
                }
                const int srcTi = g310TryT8Entry(0x139u, e, st);
                if (srcTi < 0)
                    continue;
                const bool syncOk =
                    g242SyncOverlappingDepth(vram, e.ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                             0u, 0u, 64u, 32u);
                const bool mapOk =
                    syncOk && g281BuildT8Map(e, srcTi, st.texW, st.texH, s_g310Map);
                bool ok = mapOk;
                if (ok)
                {
                    s_g310Clut.resize(256u);
                    for (uint32_t ci = 0; ci < 256u; ++ci)
                        s_g310Clut[ci] = g178LookupClutFixed(
                            vram, e.texa, e.texclut, static_cast<uint8_t>(ci),
                            e.ctx.tex0.cbp, e.ctx.tex0.cpsm, e.ctx.tex0.csm,
                            e.ctx.tex0.csa, e.ctx.tex0.psm);
                    uint64_t bad = 0u;
                    // G431 (census only, DC2_G431_T8CENSUS=1): the resident T8 view is rebuilt
                    // once per FLUSH because s_g310BuiltViews is per-flush state, but the view is
                    // a pure function of (producer FBO contents, byte map, CLUT). Count how many
                    // of these ~0.18 ms blocking backend round-trips would be skippable if the
                    // set survived the flush boundary: same viewKey, no producer write to that
                    // target since the last build, and byte-identical map+CLUT. Ceiling first —
                    // do not build the cache until this says the redundancy is real.
                    {
                        static const bool s_g431T8Census = envFlagEnabled("DC2_G431_T8CENSUS");
                        if (s_g431T8Census)
                        {
                            struct G431T8Rec { uint64_t prodN, mapHash, clutHash; int srcTi; };
                            static std::unordered_map<uint64_t, G431T8Rec> s_seen;
                            static uint64_t s_red = 0u, s_nec = 0u, s_reasonProd = 0u,
                                            s_reasonMap = 0u, s_reasonClut = 0u, s_reasonNew = 0u;
                            const auto fnv = [](const uint32_t *p, size_t n) {
                                uint64_t h = 1469598103934665603ull;
                                for (size_t k = 0; k < n; ++k)
                                { h ^= p[k]; h *= 1099511628211ull; }
                                return h;
                            };
                            // Page-granular producer signature: the whole-target write counter
                            // bumps on any write anywhere in the target, so it can never show a
                            // redundant rebuild. g_g310TargetPageGen is per 64x32 page.
                            const auto &pageGen =
                                g_g310TargetPageGen[static_cast<size_t>(srcTi)];
                            uint64_t prodN = 1469598103934665603ull;
                            for (size_t pg = 0; pg < pageGen.size(); ++pg)
                            { prodN ^= pageGen[pg]; prodN *= 1099511628211ull; }
                            const uint64_t mh = fnv(s_g310Map.data(), s_g310Map.size());
                            const uint64_t ch = fnv(s_g310Clut.data(), s_g310Clut.size());
                            auto it = s_seen.find(viewKey);
                            if (it == s_seen.end())
                            { ++s_nec; ++s_reasonNew; }
                            else if (it->second.srcTi == srcTi && it->second.prodN == prodN &&
                                     it->second.mapHash == mh && it->second.clutHash == ch)
                                ++s_red;
                            else
                            {
                                ++s_nec;
                                if (it->second.prodN != prodN) ++s_reasonProd;
                                else if (it->second.mapHash != mh) ++s_reasonMap;
                                else if (it->second.clutHash != ch) ++s_reasonClut;
                            }
                            s_seen[viewKey] = G431T8Rec{prodN, mh, ch, srcTi};
                            if (((s_red + s_nec) % 2000ull) == 0ull)
                                std::fprintf(stderr,
                                             "[G431:t8census] builds=%llu redundant=%llu (%.1f%%) "
                                             "necessary=%llu why(new=%llu prod=%llu map=%llu clut=%llu) keys=%zu\n",
                                             (unsigned long long)(s_red + s_nec),
                                             (unsigned long long)s_red,
                                             100.0 * (double)s_red /
                                                 (double)std::max<uint64_t>(1u, s_red + s_nec),
                                             (unsigned long long)s_nec,
                                             (unsigned long long)s_reasonNew,
                                             (unsigned long long)s_reasonProd,
                                             (unsigned long long)s_reasonMap,
                                             (unsigned long long)s_reasonClut,
                                             s_seen.size());
                        }
                    }
                    const auto g324BackendT0 = g_g324ActiveDiag != nullptr
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    ok = g281_backend_prepare_t8_view(
                        viewKey, kG261Fbp[static_cast<uint32_t>(srcTi)],
                        g_g261Res[srcTi].fbW, st.texW, st.texH,
                        s_g310Map, s_g310Clut, g281VerifyOn(), bad);
                    if (g_g324ActiveDiag != nullptr)
                    {
                        const auto done = std::chrono::steady_clock::now();
                        g324T8BackendNs += g324ElapsedNs(g324BackendT0, done);
                        ++g324T8Jobs;
                        g_g324ActiveDiag->hadProducer = true;
                        g_g324ActiveDiag->lastProducerDone = done;
                    }
                    if (bad != 0u)
                        g_g281VerifyBad.fetch_add(bad, std::memory_order_relaxed);
                }
                if (!ok)
                {
                    g_g310T8BuildFail.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
                s_g310BuiltViews.insert(viewKey);
                s_g281ViewKey[i] = viewKey;
                s_g281ViewSrc[i] = static_cast<int8_t>(srcTi);
                g_g310T8Views.fetch_add(1u, std::memory_order_relaxed);
            }
            if (g_g324ActiveDiag != nullptr)
            {
                const uint64_t totalNs =
                    g324ElapsedNs(g324T8T0, std::chrono::steady_clock::now());
                g_g324ActiveDiag->t8BackendWallNs += g324T8BackendNs;
                g_g324ActiveDiag->t8CpuNs +=
                    totalNs > g324T8BackendNs ? totalNs - g324T8BackendNs : 0u;
                g_g324ActiveDiag->t8Jobs += g324T8Jobs;
            }
            if (g323Armed)
            {
                const uint64_t elapsed = g323Ns(g323ScopeT0, std::chrono::steady_clock::now());
                if (s_g323Census)
                {
                    g323LogicalNs += elapsed;
                    ++g323LogicalCalls;
                }
            }
        }
        bool g310Ready = false;
        bool g309Ready = false;
        // Snapshot before walking the consumer list. Earlier entries in this same pre-pass can
        // force legacy texture-alias publication and clear dirty ownership before the dominant
        // strip entry is reached; waiting until that entry therefore loses the residency state
        // G309/G310 must consume. G310 freezes this one-texture snapshot until submission ends.
        if (g310On())
        {
            const auto g323ScopeT0 = g323Armed ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            for (size_t i = 0; i < g_g144List.size(); ++i)
                if (g310EntryShape(fbp, g_g144List[i], s_states[i]))
                {
                    (void)g310PrepareLogical(g310Ready);
                    if (g310Ready)
                        g310Freeze.begin();
                    break;
                }
            if (g323Armed)
            {
                const uint64_t elapsed = g323Ns(g323ScopeT0, std::chrono::steady_clock::now());
                if (s_g323Census)
                {
                    g323LogicalNs += elapsed;
                    ++g323LogicalCalls;
                }
            }
        }
        else if (g310StatOn())
        {
            const auto g323ScopeT0 = g323Armed ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            for (size_t i = 0; i < g_g144List.size(); ++i)
                if (g310EntryShape(fbp, g_g144List[i], s_states[i]))
                {
                    std::array<G310DesiredPage, 128> census{};
                    int seedTi = -1;
                    (void)g310BuildDesired(census, seedTi);
                    break;
                }
            if (g323Armed)
            {
                const uint64_t elapsed = g323Ns(g323ScopeT0, std::chrono::steady_clock::now());
                if (s_g323Census)
                {
                    g323LogicalNs += elapsed;
                    ++g323LogicalCalls;
                }
            }
        }
        if (!g310On() && g309On())
        {
            const auto g323ScopeT0 = g323Armed ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            for (size_t i = 0; i < g_g144List.size(); ++i)
                if (g309EntryShape(fbp, g_g144List[i], s_states[i]))
                {
                    (void)g309PrepareComposite(g309Ready);
                    break;
                }
            if (g323Armed)
            {
                const uint64_t elapsed = g323Ns(g323ScopeT0, std::chrono::steady_clock::now());
                if (s_g323Census)
                {
                    g323CompositeNs += elapsed;
                    ++g323CompositeCalls;
                }
            }
        }
        const auto g323BindT0 = g323Armed ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
        if (s_g323Census)
            g323BindEntries += static_cast<uint64_t>(g_g144List.size());
        for (size_t i = 0; i < g_g144List.size(); ++i)
        {
            const G178EntryState &st = s_states[i];
            const G144Entry &e = g_g144List[i];
            if (g289DirectDisplaySrc != 0u && i == 0u)
            {
                s_g261FboSrc[i] = g289DirectDisplaySrc;
                g261AnyBind = true;
                const uint64_t n =
                    g_g289Direct.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:direct] n=%llu src=0x%x dst=0x139 rows=0..415 "
                                 "clampHalo=1\n",
                                 static_cast<unsigned long long>(n),
                                 g289DirectDisplaySrc == kG289ZeroFbpToken
                                     ? 0u : g289DirectDisplaySrc);
                continue;
            }
            if (g289OwnerViewReady && s_g289OwnerSrc[i] != 0u)
            {
                s_g261FboSrc[i] = s_g289OwnerSrc[i];
                g261AnyBind = true;
                continue;
            }
            const bool g310Shape =
                (g310On() || g310StatOn()) && g310EntryShape(fbp, e, st);
            const bool g309Shape = g309On() && g309EntryShape(fbp, e, st);
            if ((g310On() || g310StatOn()) && g310Shape)
                g_g310Candidates.fetch_add(1u, std::memory_order_relaxed);
            const bool g310Candidate = g310On() && g310Shape;
            if (g310Candidate)
            {
                if (g310Ready && g310Freeze.active && g310FrozenUsable())
                {
                    s_g261FboSrc[i] =
                        e.ctx.tex0.psm == GS_PSM_CT24
                            ? kG310LogicalCt24Token : kG310LogicalFbp;
                    g261AnyBind = true;
                    g_g310Binds.fetch_add(1u, std::memory_order_relaxed);
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
                // A failed or invalidated logical snapshot takes the frozen legacy path below.
            }
            const bool g309Candidate =
                !g310On() && g309Shape;
            if (g309Candidate)
            {
                g_g309Candidates.fetch_add(1u, std::memory_order_relaxed);
                if (g309Ready)
                {
                    s_g261FboSrc[i] = kG309CompositeToken;
                    g261AnyBind = true;
                    g_g309Binds.fetch_add(1u, std::memory_order_relaxed);
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
                // Verifier and every failed/stale/missing-authority case fall through to the
                // frozen legacy materialize/decode path. Do not let the implied G280 stack turn
                // a G309 failure into the older page-copy behavior.
            }
            if (st.texKey == 0 || st.skip || !e.prim.tme)
                continue;
            if (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                continue; // G310 decoded this exact self-atlas snapshot before publication.

            // G281: close the measured format-crossing consumer before the generic overlap loop
            // materializes 0x13c. Admission proves the exact PSMT8 byte map belongs solely to
            // that dirty FBO and that the CLUT remains VRAM-authoritative. The decoded GPU view
            // is built later with the other texture work.
            if (g281ViewOn() && fbp == 0x143u &&
                e.ctx.tex0.psm == GS_PSM_T8 && e.ctx.tex0.tbp0 == 0x2820u)
            {
                const int srcTi = g281TryEntry(fbp, e, st);
                if (srcTi >= 0)
                {
                    s_g281ViewKey[i] = g281ViewKey(st.texKey);
                    s_g281ViewSrc[i] = static_cast<int8_t>(srcTi);
                    continue;
                }
            }

            // G267 diagnostic: characterize the stable display TRISTRIP consumer with exact
            // bilinear taps and exact CT32 sampled pages. Behavior is intentionally unchanged:
            // the repaired prototype was neutral and was removed at phase close.
            if ((g267CensusOn() || g342Ct32BindOn()) && g265DisplayTarget(fbp) &&
                e.prim.type == GS_PRIM_TRISTRIP && e.prim.fst && st.bilinear &&
                e.ctx.tex0.psm == GS_PSM_CT32)
            {
                int srcTi = -1;
                for (uint32_t t = 0; t < kG248TargetCount; ++t)
                {
                    const G261Res &r = g_g261Res[t];
                    if (r.dirty && e.ctx.tex0.tbp0 == framePageBaseToBlock(kG261Fbp[t]) &&
                        e.ctx.tex0.tbw == r.fbw)
                    {
                        srcTi = static_cast<int>(t);
                        break;
                    }
                }
                if (srcTi >= 0)
                {
                    g_g267TriCandidates.fetch_add(1u, std::memory_order_relaxed);
                    const G261Res &src = g_g261Res[srcTi];
                    G267TriFootprint fp;
                    const bool footprintOk = g267BilinearTriFootprint(e, st, src, fp);
                    uint32_t aliasMask = 0u;
                    if (footprintOk)
                    {
                        for (uint32_t t = 0; t < kG248TargetCount; ++t)
                        {
                            if (static_cast<int>(t) == srcTi || !g_g261Res[t].dirty)
                                continue;
                            if (g267SamplePagesOverlap(e, fp, g_g261Res[t].range))
                                aliasMask |= 1u << t;
                        }
                    }

                    if (!footprintOk)
                        g_g267TriFootReject.fetch_add(1u, std::memory_order_relaxed);
                    else if (aliasMask != 0u)
                        g_g267TriAliasReject.fetch_add(1u, std::memory_order_relaxed);
                    else
                        g_g267TriEligible.fetch_add(1u, std::memory_order_relaxed);

                    // G342 arc#6 slice-1 (default-off): when the tap window is provably inside the
                    // source FBO and no dirty sibling contributes, bind the resident FBO directly
                    // (backend bilinear-samples it) and skip the readback+materialize — exactly the
                    // G267-verified prototype, now re-armed to measure net on the current stack.
                    if (footprintOk && aliasMask == 0u && g342Ct32BindOn())
                    {
                        s_g261FboSrc[i] = kG261Fbp[srcTi];
                        g261AnyBind = true;
                        g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                        const uint64_t nb =
                            g_g342Ct32Binds.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if ((nb % 512u) == 0u)
                            std::fprintf(stderr, "[G342:ct32bind] binds=%llu\n",
                                         static_cast<unsigned long long>(nb));
                        continue;
                    }

                    static std::atomic<uint32_t> s_g267Log{0};
                    if (s_g267Log.fetch_add(1u, std::memory_order_relaxed) < 24u)
                        std::fprintf(stderr,
                                     "[G267:tri] dst=%03x src=%03x ok=%u alias=0x%x "
                                     "uv=(%u,%u)(%u,%u)(%u,%u) taps=%d..%d,%d..%d "
                                     "range=%04x..%04x cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u\n",
                                     fbp, kG261Fbp[srcTi], footprintOk ? 1u : 0u,
                                     aliasMask, e.v[0].u, e.v[0].v, e.v[1].u, e.v[1].v,
                                     e.v[2].u, e.v[2].v, fp.tapLoU, fp.tapHiU,
                                     fp.tapLoV, fp.tapHiV,
                                     fp.sampleRange.valid ? fp.sampleRange.first : 0u,
                                     fp.sampleRange.valid ? fp.sampleRange.last : 0u,
                                     src.covLo, src.covHi, src.fbW, st.texW, st.texH,
                                     static_cast<unsigned>(st.wrapU),
                                     static_cast<unsigned>(st.wrapV));
                }
            }
            // G280: fully handle the entry when the bind is provable AND every dirty-sibling
            // physical-page alias resolves with a GPU->GPU page-tile copy (the G267 family).
            // Success replaces the whole legacy per-target loop below for this entry — the
            // sibling's newer pages are already IN the bound FBO, so no materialization is
            // needed. Any rejection falls through to the legacy loop unchanged.
            if (g280AliasOn() && !g309Candidate)
            {
                const uint32_t g280Fbp = g280TryEntry(fbp, e, st);
                if (g280Fbp != 0u)
                {
                    s_g261FboSrc[i] = g280Fbp;
                    g261AnyBind = true;
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
            }
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                G261Res &r = g_g261Res[t];
                if (!r.dirty)
                    continue;
                const uint32_t tBlock = framePageBaseToBlock(kG261Fbp[t]);
                bool bind = false;
                if (e.ctx.tex0.psm == GS_PSM_CT32 && e.ctx.tex0.tbp0 == tBlock &&
                    e.ctx.tex0.tbw == r.fbw && e.prim.fst &&
                    (e.prim.type == GS_PRIM_SPRITE || e.prim.type == GS_PRIM_TRIANGLE ||
                     e.prim.type == GS_PRIM_TRISTRIP || e.prim.type == GS_PRIM_TRIFAN))
                {
                    if (!st.bilinear)
                    {
                    // FST texel extent across the primitive's vertices must lie inside the
                    // FBO-defined row/column window (NEAREST taps only touch [u0..u1) texels).
                    const int nv = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
                    int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
                    for (int k = 0; k < nv; ++k)
                    {
                        const int uu = static_cast<int>(e.v[k].u >> 4);
                        const int vv = static_cast<int>(e.v[k].v >> 4);
                        u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                        v0 = std::min(v0, vv); v1 = std::max(v1, vv);
                    }
                    if (e.prim.type == GS_PRIM_SPRITE)
                    {
                        // Sprite endpoints are exclusive right/bottom (max tap = v1-1 texels).
                        bind = u0 >= 0 && u1 <= r.fbW && v0 >= r.covLo && v1 <= r.covHi + 1;
                    }
                    else
                    {
                        // Triangle interpolation can reach the vertex texel itself — require it
                        // to be inside the defined window outright.
                        bind = u0 >= 0 && u1 <= r.fbW - 1 && v0 >= r.covLo && v1 <= r.covHi;
                    }
                    }
                    else if (e.prim.type == GS_PRIM_SPRITE && g248TargetIndex(fbp) < 0)
                    {
                        // G263: the display composite sprite samples the character RTT with
                        // bilinear filtering — G262 measured it as the ONLY bind rejector
                        // (2728 mat(tex)/run, solely on bilin=1). Admit it when the exact tap
                        // footprint stays FBO-defined; the backend already selects GL_LINEAR
                        // from d.bilinear on the srcFbp path. RTT-family consumers keep the
                        // NEAREST-only rule until a census shape demands more.
                        static const bool s_g263Off = envFlagEnabled("DC2_G263_NO_BILIN_BIND");
                        if (!s_g263Off)
                        {
                            bind = g263BilinearSpriteBind(e, st, r);
                            if (bind)
                                g_g263FboBindBi.fetch_add(1u, std::memory_order_relaxed);
                        }
                    }
                }
                if (bind)
                {
                    s_g261FboSrc[i] = kG261Fbp[t];
                    g261AnyBind = true;
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                }
                else
                {
                    // Any other overlap of this texture/CLUT with the resident target's pages
                    // needs real VRAM bytes — materialize before the decode loop reads them.
                    const G144BlockRange tex = g144TextureRange(e);
                    const G144BlockRange clut = g144IsPalettedPsm(e.ctx.tex0.psm)
                                                    ? g144ClutRange(e)
                                                    : G144BlockRange{};
                    if (!tex.valid || !r.range.valid ||
                        g144RangeOverlaps(r.range, tex) ||
                        (clut.valid && g144RangeOverlaps(r.range, clut)))
                    {
                        // G291: the read needs VRAM bytes, but if every overlapped 64x32 page
                        // of this resident target was FULLY re-covered by noted uploads since
                        // its last wave render, VRAM is already current for exactly the pages
                        // the read touches — the materialize round-trip would be the identity
                        // there. CLUT reads use paletted PSMs whose range test is the same
                        // block interval. Fails closed on any partial page.
                        if (g291AtlasOn() && tex.valid && r.range.valid &&
                            g291PagesCurrent(r, kG261Fbp[t], tex) &&
                            (!clut.valid || g291PagesCurrent(r, kG261Fbp[t], clut)))
                        {
                            const uint64_t n = g_g291TexSkips.fetch_add(
                                                   1u, std::memory_order_relaxed) + 1u;
                            if (g291StatOn() && (n <= 8u || (n % 4096u) == 0u))
                                std::fprintf(stderr,
                                             "[G291:skip] n=%llu t=%03x tex=0x%x..0x%x "
                                             "mats=%llu\n",
                                             (unsigned long long)n, kG261Fbp[t],
                                             tex.first, tex.last,
                                             (unsigned long long)g_g291TexMats.load(
                                                 std::memory_order_relaxed));
                            continue;
                        }
                        if (g291AtlasOn())
                            g_g291TexMats.fetch_add(1u, std::memory_order_relaxed);
                        // G262 diagnosis: WHY did the direct-bind test fail for a real consumer?
                        static std::atomic<uint32_t> s_g262BindRejLog{0};
                        if (g262CensusOn() &&
                            s_g262BindRejLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                            std::fprintf(stderr,
                                         "[G262:bindrej] dstFbp=0x%x src=%03x psm=0x%x tbp=0x%x "
                                         "tbw=%u rfbw=%u fst=%u bilin=%u prim=%u cov=%d..%d "
                                         "wrap=%u%u tex=%dx%d tcc=%u tfx=%u abe=%u ta0=%u aem=%u\n",
                                         fbp, kG261Fbp[t], e.ctx.tex0.psm, e.ctx.tex0.tbp0,
                                         e.ctx.tex0.tbw, r.fbw, e.prim.fst ? 1u : 0u,
                                         st.bilinear ? 1u : 0u,
                                         static_cast<unsigned>(e.prim.type), r.covLo, r.covHi,
                                         static_cast<unsigned>(st.wrapU),
                                         static_cast<unsigned>(st.wrapV), st.texW, st.texH,
                                         static_cast<unsigned>(e.ctx.tex0.tcc),
                                         static_cast<unsigned>(e.ctx.tex0.tfx),
                                         e.prim.abe ? 1u : 0u,
                                         static_cast<unsigned>(e.texa.ta0),
                                         e.texa.aem ? 1u : 0u);
                        // G338: a CT24 consumer that reads this producer at the SAME base/width is
                        // that FBO's RGB with per-texel TEXA alpha. Bind a resident CT24-alpha view
                        // instead of the readback+swizzle+re-decode materialize. Exact by
                        // construction (the view == legacy CT24->RGBA8 decode, consumer keeps its
                        // own GL_LINEAR). Any build failure or verifier mismatch falls through to
                        // the proven materialize, so the default path is never left incorrect.
                        if (g338Ct24ViewOn() && e.ctx.tex0.psm == GS_PSM_CT24 &&
                            e.ctx.tex0.tbp0 == tBlock && e.ctx.tex0.tbw == r.fbw &&
                            g248TargetIndex(fbp) < 0 && st.texKey != 0u &&
                            st.texW > 0 && st.texH > 0 && st.texW <= r.fbW && r.fbW > 0)
                        {
                            const uint64_t viewKey = g338ViewKey(st.texKey);
                            uint64_t bad = 0u;
                            const bool ok = g338_backend_prepare_ct24_view(
                                viewKey, kG261Fbp[t], r.fbW, st.texW, st.texH,
                                static_cast<int>(e.texa.ta0), e.texa.aem ? 1 : 0,
                                g338VerifyOn(), bad);
                            if (bad != 0u)
                                g_g338VerifyBad.fetch_add(bad, std::memory_order_relaxed);
                            if (ok && bad == 0u)
                            {
                                s_g281ViewKey[i] = viewKey;
                                s_g281ViewSrc[i] = static_cast<int8_t>(t);
                                s_g310BuiltViews.insert(viewKey);
                                g_g338Views.fetch_add(1u, std::memory_order_relaxed);
                                break; // entry handled by the resident view; do not materialize
                            }
                            g_g338BuildFail.fetch_add(1u, std::memory_order_relaxed);
                        }
                        g_g261FboRejects.fetch_add(1u, std::memory_order_relaxed);
                        g266NoteTexBindMat(fbp, t, e, st);
                        // G342 (default-off): time THIS tex-alias materialize and attribute it to
                        // the consumer's shape. rows must be read BEFORE materialize clears dirty.
                        const bool g342On = g342CensusOn();
                        const uint64_t g342Rows =
                            g342On ? static_cast<uint64_t>(std::max(0, r.dirtyHi - r.dirtyLo + 1))
                                   : 0u;
                        const auto g342T0 = g342On ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
                        const auto g323MatT0 = g323Armed ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
                        g261Materialize(static_cast<int>(t), kG261MatTexAlias);
                        if (g342On)
                            g342NoteTexAlias(
                                fbp, t, tBlock, e, st, r.fbw, g342Rows,
                                static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - g342T0)
                                        .count()));
                        if (g323Armed)
                        {
                            const uint64_t elapsed =
                                g323Ns(g323MatT0, std::chrono::steady_clock::now());
                            if (s_g323Census)
                            {
                                g323MaterializeNs += elapsed;
                                ++g323MaterializeCalls;
                            }
                        }
                        // G338 AEM census (default-off): the CT24 display-composite consumer of
                        // 0x139 (tbp==FBO base, tbw==r.fbw) can only bind the resident CT32 FBO
                        // bit-exactly under BILINEAR if AEM never fires (a pure-black CT24 texel
                        // gets alpha=0, which a constant-TA0 shader override cannot reproduce once
                        // filtered). Scan the just-materialized sampled region for RGB==0 words to
                        // decide tractability. Behavior-pure; caps its own log volume.
                        static const bool s_g338AemCensus =
                            envFlagEnabled("DC2_G338_AEM_CENSUS");
                        if (s_g338AemCensus && vram != nullptr &&
                            e.ctx.tex0.psm == GS_PSM_CT24 &&
                            e.ctx.tex0.tbp0 == framePageBaseToBlock(kG261Fbp[t]) &&
                            e.ctx.tex0.tbw == r.fbw && r.covHi >= r.covLo && r.fbW > 0)
                        {
                            const uint32_t base = e.ctx.tex0.tbp0;
                            const uint32_t tbw = e.ctx.tex0.tbw;
                            uint64_t zero = 0u, total = 0u;
                            for (int y = r.covLo; y <= r.covHi; ++y)
                                for (int x = 0; x < r.fbW; ++x)
                                {
                                    const uint32_t w = GSMem::ReadCT32(
                                        vram, base, tbw, static_cast<uint32_t>(x),
                                        static_cast<uint32_t>(y));
                                    if ((w & 0x00FFFFFFu) == 0u)
                                        ++zero;
                                    ++total;
                                }
                            static std::atomic<uint64_t> s_g338AemZero{0}, s_g338AemTot{0},
                                s_g338AemN{0};
                            s_g338AemZero.fetch_add(zero, std::memory_order_relaxed);
                            s_g338AemTot.fetch_add(total, std::memory_order_relaxed);
                            const uint64_t n =
                                s_g338AemN.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (n <= 8u || (n % 128u) == 0u)
                                std::fprintf(stderr,
                                             "[G338:aem] n=%llu dst=%03x rgb0=%llu/%llu "
                                             "cumRgb0=%llu/%llu rows=%d..%d aem=%u ta0=%u\n",
                                             (unsigned long long)n, fbp,
                                             (unsigned long long)zero,
                                             (unsigned long long)total,
                                             (unsigned long long)s_g338AemZero.load(
                                                 std::memory_order_relaxed),
                                             (unsigned long long)s_g338AemTot.load(
                                                 std::memory_order_relaxed),
                                             r.covLo, r.covHi,
                                             e.texa.aem ? 1u : 0u,
                                             static_cast<unsigned>(e.texa.ta0));
                        }
                    }
                }
            }
        }
        if (g323Armed)
        {
            const uint64_t elapsed = g323Ns(g323BindT0, std::chrono::steady_clock::now());
            if (s_g323Census)
                g323BindNs += elapsed > g323MaterializeNs
                    ? elapsed - g323MaterializeNs : 0u;
        }
    }
    else
    {
        s_g261FboSrc.clear();
        s_g281ViewKey.clear();
        s_g281ViewSrc.clear();
        s_g310BuiltViews.clear();
    }
    const bool g261HaveFboSrc = g261AnyBind; // s_g261FboSrc sized to the list only when true

    const auto g298T3 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    uint64_t g323DepsNs = 0u;
    if (g323Armed)
    {
        g323DepsNs = g323Ns(g323T2, std::chrono::steady_clock::now());
        if (s_g323Census)
        {
            const uint64_t known = g323HygieneNs + g323LogicalNs + g323CompositeNs +
                                   g323BindNs + g323MaterializeNs;
            g323OtherNs = g323DepsNs > known ? g323DepsNs - known : 0u;
        }
    }
    // Textures: decode + upload anything not resident or invalidated by page-gen changes.
    static std::unordered_set<uint64_t> s_batchKeys;
    s_batchKeys.clear();
    // G511: this pass walks every graph entry (~9,900/frame) and asks s_batchKeys whether the
    // entry's effective texture key was already collected in THIS flush. G510 measured 94.4% of
    // entries carrying byte-identical state to their predecessor, and the texture key is a function
    // of that state, so the key is overwhelmingly the same key the previous iteration just resolved.
    // `g511Present` remembers exactly one key that has been PROVEN resident in s_batchKeys, which
    // turns the repeat's hash probe into a 64-bit compare.
    //
    // Exact by outcome, not merely by likelihood: the original test is
    //   `effectiveKey == 0 || st.skip || s_batchKeys.count(effectiveKey)` -> continue
    // and ALL THREE arms continue, so once the key is known-present the iteration continues whatever
    // st.skip says. `g511Present` is only ever set where the set membership was actually
    // established — after an insert, or after a count() that returned nonzero (never on the
    // short-circuited st.skip arm, where count() was not evaluated) — and it is cleared on the one
    // path that ERASES a key from the set (the G281 view-build failure below).
    const bool g511Run = g511TexRunActive();
    const bool g511Probe = g511TexProbeActive();
    const bool g511Census = g511CensusOn();
    const bool g511Split = g511TexSplitOn();
    uint64_t g511Present = 0u;
    uint64_t g511Entries = 0u, g511Probes = 0u, g511RunRep = 0u, g511ProbeSink = 0u;
    uint64_t g511HashNs = 0u, g511HashN = 0u, g511DecodeNs = 0u, g511DecodeN = 0u;
    uint64_t g511GenNs = 0u, g511GenN = 0u, g511HashPx = 0u, g511DecodePx = 0u;
    const auto g511LoopT0 = g511Split ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    for (size_t i = 0; i < g_g144List.size(); ++i)
    {
        const G178EntryState &st = s_states[i];
        if (g511Census)
            ++g511Entries;
        if (g261HaveFboSrc && s_g261FboSrc[i] != 0u)
            continue; // G261: sampled straight from the producer's FBO — no VRAM decode
        const uint64_t effectiveKey =
            (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                ? s_g281ViewKey[i] : st.texKey;
        if (effectiveKey == 0u)
            continue;
        if (g511Census)
        {
            ++g511Probes;
            if (effectiveKey == g511Present)
                ++g511RunRep;
        }
        // G511 INFLATION probe (DC2_G419_AB=texprobe, arm 1 only): one EXTRA set probe per PROBING
        // entry, summed into a local that reaches an atomic once per flush so it cannot be deleted.
        // It sits ABOVE the run-length shortcut deliberately: the population it prices must be the
        // whole pre-G511 probe population (`probes/f` in the census), not the ~6% of entries that
        // still miss the memo — otherwise the price would be divided by the wrong count, which is
        // exactly how G508 mis-priced a lever 7.8x.
        if (g511Probe)
            g511ProbeSink += s_batchKeys.count(effectiveKey);
        if (g511Run && effectiveKey == g511Present)
            continue; // known present ⇒ the original test continues on its third arm
        if (st.skip || s_batchKeys.count(effectiveKey))
        {
            if (!st.skip)
                g511Present = effectiveKey; // count() was evaluated and said present
            continue;
        }
        s_batchKeys.insert(effectiveKey);
        g511Present = effectiveKey;
        const G144Entry &e = g_g144List[i];
        const GSContext &ctx = e.ctx;
        const bool paletted = ctx.tex0.psm == GS_PSM_T8 || ctx.tex0.psm == GS_PSM_T4 ||
                              ctx.tex0.psm == GS_PSM_T4HL || ctx.tex0.psm == GS_PSM_T4HH;

        if (s_g310BuiltViews.count(effectiveKey) != 0u)
            continue; // Synchronously decoded from the pre-batch 0x139 image above.

        if (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
        {
            const int srcTi = s_g281ViewSrc[i];
            static std::vector<uint32_t> s_map, s_clut;
            const bool srcOk = srcTi >= 0;
            const bool syncOk =
                srcOk && g242SyncOverlappingDepth(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                                  0u, 0u, 64u, 32u);
            const bool mapOk =
                syncOk && g281BuildT8Map(e, srcTi, st.texW, st.texH, s_map);
            bool ok = mapOk;
            if (ok)
            {
                s_clut.resize(256u);
                for (uint32_t ci = 0; ci < 256u; ++ci)
                    s_clut[ci] = g178LookupClutFixed(
                        vram, e.texa, e.texclut, static_cast<uint8_t>(ci),
                        ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa,
                        ctx.tex0.psm);
                uint64_t bad = 0u;
                ok = g281_backend_prepare_t8_view(
                    s_g281ViewKey[i], kG261Fbp[static_cast<uint32_t>(srcTi)],
                    g_g261Res[srcTi].fbW, st.texW, st.texH, s_map, s_clut,
                    g281VerifyOn(), bad);
                if (bad != 0u)
                    g_g281VerifyBad.fetch_add(bad, std::memory_order_relaxed);
            }
            if (ok)
            {
                g_g281Views.fetch_add(1u, std::memory_order_relaxed);
                continue;
            }

            // Fail closed before any CPU decode/replay can observe stale guest bytes. Publishing
            // the source restores the exact legacy path; clear every entry sharing this view.
            const uint64_t buildFail =
                g_g281BuildFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g282StatOn() && buildFail <= 8u)
                std::fprintf(stderr,
                             "[G282:g281-fail] n=%llu src=%d srcOk=%d sync=%d map=%d "
                             "dirty=%d range=%d win=%d..%d cov=%d..%d\n",
                             static_cast<unsigned long long>(buildFail), srcTi,
                             srcOk ? 1 : 0, syncOk ? 1 : 0, mapOk ? 1 : 0,
                             srcOk && g_g261Res[srcTi].dirty ? 1 : 0,
                             srcOk && g_g261Res[srcTi].range.valid ? 1 : 0,
                             srcOk ? g_g261Res[srcTi].dirtyLo : -1,
                             srcOk ? g_g261Res[srcTi].dirtyHi : -1,
                             srcOk ? g_g261Res[srcTi].covLo : -1,
                             srcOk ? g_g261Res[srcTi].covHi : -1);
            if (srcTi >= 0)
                g261Materialize(srcTi, kG261MatTexAlias);
            const uint64_t failedKey = s_g281ViewKey[i];
            for (size_t j = 0; j < s_g281ViewKey.size(); ++j)
                if (s_g281ViewKey[j] == failedKey)
                {
                    s_g281ViewKey[j] = 0u;
                    s_g281ViewSrc[j] = -1;
                }
            s_batchKeys.erase(failedKey);
            s_batchKeys.insert(st.texKey);
            // G511: this is the ONLY path that removes a key from s_batchKeys, so the run-length
            // memo's invariant ("g511Present is resident") would break here. Clear it — the next
            // entry pays one ordinary hash probe. Costs nothing measurable: G281 build failures are
            // rare by construction (they fall back to the legacy decode).
            g511Present = 0u;
            // Continue below through the ordinary guest-VRAM texture registry/decode.
        }

        if (guestDepth)
        {
            const uint32_t zbpBlock = guestZbpPage << 5u;
            const bool texAliasesZ = g242RectsOverlap(
                ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u, 0u,
                static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH),
                zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            const bool clutAliasesZ = paletted && g242RectsOverlap(
                ctx.tex0.cbp, 1u, GS_PSM_CT32, 0u, 0u, 64u, 32u,
                zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            if (texAliasesZ || clutAliasesZ)
            {
                static std::atomic<uint32_t> s_aliasTexLog{0};
                const uint32_t n = s_aliasTexLog.fetch_add(1u, std::memory_order_relaxed);
                if (g242StatEnabled() && n < 24u)
                    std::fprintf(stderr,
                                 "[G242:alias] texture-depth n=%u tbp=0x%x psm=0x%x cbp=0x%x zbp=0x%x zRows=%d..%d tex=%dx%d texHit=%u clutHit=%u\n",
                                 n + 1u, ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.cbp,
                                 guestZbpPage, depthRowLo, depthRowHi, st.texW, st.texH,
                                 texAliasesZ ? 1u : 0u, clutAliasesZ ? 1u : 0u);
                g_g266FlushRej = kG266FrTexAliasZ;
                g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                g_g178RejectTex.fetch_add(1u, std::memory_order_relaxed);
                g_g178FbSnap.clear();
                return false;
            }
        }
        if (!g242SyncOverlappingDepth(vram, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                                      0u, 0u, static_cast<uint32_t>(st.texW),
                                      static_cast<uint32_t>(st.texH)) ||
            (paletted && !g242SyncOverlappingDepth(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                                   0u, 0u, 64u, 32u)))
        {
            g_g266FlushRej = kG266FrTexAliasZ;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        const auto g511GenT0 = g511Split ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
        const uint64_t genNow =
            g178GenSumRect(ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0, 0,
                           static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH)) +
            g178GenSumRect(ctx.tex0.cbp, 1, GS_PSM_CT32, 0, 0, 64, 32); // CLUT page(s)
        auto it = g_g178TexReg.find(st.texKey);
        const bool backendHas = g178_backend_has_tex(st.texKey);
        if (g511Split)
        {
            g511GenNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g511GenT0).count());
            ++g511GenN;
        }
        static const bool s_g401Ct24Redecode =
            envFlagEnabled("DC2_G401_CT24_REDECODE");
        const bool g401ForceDecode =
            s_g401Ct24Redecode && ctx.tex0.psm == GS_PSM_CT24;
        if (!g401ForceDecode && it != g_g178TexReg.end() && backendHas &&
            it->second.genSum == genNow)
        {
            g_g178TexHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        // Gen mismatch (or unknown): DC2 re-uploads texture data every frame, usually byte-
        // identical — hash the source pages before paying the swizzle+CLUT decode. Hash-equal +
        // still resident → refresh the gen stamp and keep the GPU copy.
        const auto g511HashT0 = g511Split ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
        // G513: `fast` selects the byte loop. Read ONCE per call (~23/frame) against ~9,200 loop
        // iterations, so the accessor cannot pollute the loop it gates (G502).
        const bool g513Fast = g513HashLaneActive();
        uint64_t g513Bytes = 0u;
        const uint64_t hashNow = g178HashPages(vram, vramSize, ctx.tex0.tbp0, ctx.tex0.tbw,
                                               ctx.tex0.psm, static_cast<uint32_t>(st.texW),
                                               static_cast<uint32_t>(st.texH), ctx.tex0.cbp,
                                               g513Fast, &g513Bytes);
        if (g511Split)
        {
            g511HashNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g511HashT0).count());
            ++g511HashN;
            g511HashPx += static_cast<uint64_t>(st.texW) * static_cast<uint64_t>(st.texH);
            g_g513HashBytes.fetch_add(g513Bytes, std::memory_order_relaxed);
        }
        // G513 EXACTNESS ORACLE (DC2_G513_VERIFY=1, and NEVER inside a timed run — it runs the
        // other body as well, i.e. it doubles the very bucket the phase is measuring). The two
        // bodies return different values by construction, so value equality is not the property
        // under test; DECISION equality is. For each texture key this compares "did the digest
        // change since this key's previous call" under both bodies. A disagreement in which the
        // FAST body says unchanged while the reference says changed is the false HIT that would
        // leave a stale GPU texture on screen — the one failure mode the roadmap flags. A
        // disagreement the other way is only a wasted decode. Both are counted separately, and
        // the whole block is confined to the GS worker thread, which is the only thread that ever
        // walks this pass (g_g178TexReg itself is an unsynchronised std::unordered_map).
        if (g513VerifyOn())
        {
            static std::unordered_map<uint64_t, uint64_t> s_g513PrevRef;
            static std::unordered_map<uint64_t, uint64_t> s_g513PrevFast;
            uint64_t sinkBytes = 0u;
            const uint64_t otherNow =
                g178HashPages(vram, vramSize, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                              static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH),
                              ctx.tex0.cbp, !g513Fast, &sinkBytes);
            const uint64_t refNow = g513Fast ? otherNow : hashNow;
            const uint64_t fastNow = g513Fast ? hashNow : otherNow;
            auto rIt = s_g513PrevRef.find(st.texKey);
            auto fIt = s_g513PrevFast.find(st.texKey);
            if (rIt != s_g513PrevRef.end() && fIt != s_g513PrevFast.end())
            {
                const bool refEq = (rIt->second == refNow);
                const bool fastEq = (fIt->second == fastNow);
                const uint64_t n =
                    g_g513VerifyN.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (refEq == fastEq)
                    g_g513VerifyAgree.fetch_add(1u, std::memory_order_relaxed);
                else if (fastEq)
                    g_g513VerifyFalseHit.fetch_add(1u, std::memory_order_relaxed);
                else
                    g_g513VerifyFalseMiss.fetch_add(1u, std::memory_order_relaxed);
                if (n <= 4u || (n % 20000u) == 0u)
                    std::fprintf(stderr,
                                 "[G513:verify] n=%llu agree=%llu falseHit=%llu falseMiss=%llu\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(
                                     g_g513VerifyAgree.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(
                                     g_g513VerifyFalseHit.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(
                                     g_g513VerifyFalseMiss.load(std::memory_order_relaxed)));
            }
            s_g513PrevRef[st.texKey] = refNow;
            s_g513PrevFast[st.texKey] = fastNow;
        }
        // G513: the stamp must have been written by the SAME byte loop that just produced
        // `hashNow`. Without this the paired A/B's arm switch would compare a reference digest
        // against an 8-lane one — always a MISS, and on a collision a stale texture.
        if (!g401ForceDecode && it != g_g178TexReg.end() && backendHas &&
            it->second.hashVar == static_cast<uint8_t>(g513Fast ? 1u : 0u) &&
            it->second.contentHash == hashNow)
        {
            it->second.genSum = genNow;
            g_g178TexHashHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        const auto g511DecT0 = g511Split ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
        G178TexUpload up;
        up.key = st.texKey;
        up.w = st.texW;
        up.h = st.texH;
        up.px.resize(static_cast<size_t>(st.texW) * st.texH);
        g178DecodeWhole(self, gs, vram, ctx, e.texa, e.texclut, st.texW, st.texH, up.px.data());
        if (g511Split)
        {
            g511DecodeNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g511DecT0).count());
            ++g511DecodeN;
            g511DecodePx += static_cast<uint64_t>(st.texW) * static_cast<uint64_t>(st.texH);
        }
        if (g401CompositeOn() && ctx.tex0.psm == GS_PSM_CT24 &&
            ctx.tex0.tbp0 == 0x2720u)
        {
            extern std::atomic<uint64_t> g_dc2PresentTick;
            const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
            const uint64_t from = g400EnvU64("DC2_G401_FROM", 0ull);
            const uint64_t to = g400EnvU64("DC2_G401_TO", from + 8ull);
            if (tick >= from && tick <= to)
            {
                uint64_t sumA = 0u;
                uint32_t minA = 255u, maxA = 0u;
                for (uint32_t c : up.px)
                {
                    const uint32_t a = c >> 24u;
                    sumA += a;
                    minA = std::min(minA, a);
                    maxA = std::max(maxA, a);
                }
                std::fprintf(stderr,
                             "[G401:ct24-decode] tick=%llu tex=%dx%d ta0=%u aem=%u "
                             "A=%.2f/%u/%u view=%llu fbo=%03x\n",
                             static_cast<unsigned long long>(tick), st.texW, st.texH,
                             static_cast<unsigned>(e.texa.ta0), e.texa.aem ? 1u : 0u,
                             up.px.empty() ? 0.0 : static_cast<double>(sumA) / up.px.size(),
                             minA, maxA,
                             static_cast<unsigned long long>(
                                 s_g281ViewKey.empty() ? 0u : s_g281ViewKey[i]),
                             (g261HaveFboSrc && !s_g261FboSrc.empty())
                                 ? s_g261FboSrc[i] : 0u);
            }
        }
        s_batch.texUploads.push_back(std::move(up));
        g_g178TexReg[st.texKey] =
            G178TexReg{genNow, hashNow, st.texW, st.texH,
                       static_cast<uint8_t>(g513Fast ? 1u : 0u)};
        g_g178TexDecodes.fetch_add(1u, std::memory_order_relaxed);
    }
    // G511: one relaxed add per counter per FLUSH (33.4/frame), never per entry — a per-entry
    // counter would inflate the very loop it prices (G502).
    if (g511Probe)
        g_g511Sink.fetch_add(g511ProbeSink, std::memory_order_relaxed);
    if (g511Split)
    {
        g_g511TexLoopNs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g511LoopT0).count()),
            std::memory_order_relaxed);
        g_g511TexHashNs.fetch_add(g511HashNs, std::memory_order_relaxed);
        g_g511TexHashN.fetch_add(g511HashN, std::memory_order_relaxed);
        g_g511TexDecodeNs.fetch_add(g511DecodeNs, std::memory_order_relaxed);
        g_g511TexDecodeN.fetch_add(g511DecodeN, std::memory_order_relaxed);
        g_g511TexGenNs.fetch_add(g511GenNs, std::memory_order_relaxed);
        g_g511TexGenN.fetch_add(g511GenN, std::memory_order_relaxed);
        g_g511TexHashPx.fetch_add(g511HashPx, std::memory_order_relaxed);
        g_g511TexDecodePx.fetch_add(g511DecodePx, std::memory_order_relaxed);
        const uint64_t nf = g_g511TexSplitFlushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((nf % 2000u) == 0u)
        {
            // Per-FLUSH means; multiply by flushes/frame (33.4 on lean MAP-0) for ms/f. `rest` is
            // a stated RESIDUAL — loop minus the three directly-timed sites — and must be read as
            // one (G505/G507: never RANK with a residual; quoting one inside its own bucket, with
            // the subtrahends measured directly, is what this is).
            const double n = static_cast<double>(nf);
            const double loop = static_cast<double>(
                g_g511TexLoopNs.load(std::memory_order_relaxed)) / n / 1.0e6;
            const double hash = static_cast<double>(
                g_g511TexHashNs.load(std::memory_order_relaxed)) / n / 1.0e6;
            const double dec = static_cast<double>(
                g_g511TexDecodeNs.load(std::memory_order_relaxed)) / n / 1.0e6;
            const double gen = static_cast<double>(
                g_g511TexGenNs.load(std::memory_order_relaxed)) / n / 1.0e6;
            const double hashN = static_cast<double>(
                g_g511TexHashN.load(std::memory_order_relaxed));
            const double decN = static_cast<double>(
                g_g511TexDecodeN.load(std::memory_order_relaxed));
            const double hashPx = static_cast<double>(
                g_g511TexHashPx.load(std::memory_order_relaxed));
            const double decPx = static_cast<double>(
                g_g511TexDecodePx.load(std::memory_order_relaxed));
            std::fprintf(stderr,
                         "[G511:texsplit] flushes=%llu msPerFlush(loop=%.4f gen=%.4f hash=%.4f "
                         "decode=%.4f rest=%.4f) perFlush(gen=%.2f hash=%.2f decode=%.2f) "
                         "nsEach(gen=%.0f hash=%.0f decode=%.0f) "
                         "avgPx(hash=%.0f decode=%.0f) nsPerPx(hash=%.2f decode=%.2f) "
                         "hashKBEach=%.1f nsPerHashByte=%.3f\n",
                         static_cast<unsigned long long>(nf), loop, gen, hash, dec,
                         loop - gen - hash - dec,
                         static_cast<double>(g_g511TexGenN.load(std::memory_order_relaxed)) / n,
                         hashN / n, decN / n,
                         gen * 1.0e6 / std::max(1.0, static_cast<double>(
                             g_g511TexGenN.load(std::memory_order_relaxed)) / n),
                         hash * 1.0e6 / std::max(1.0, hashN / n),
                         dec * 1.0e6 / std::max(1.0, decN / n),
                         hashPx / std::max(1.0, hashN), decPx / std::max(1.0, decN),
                         hash * n * 1.0e6 / std::max(1.0, hashPx),
                         dec * n * 1.0e6 / std::max(1.0, decPx),
                         // G494's law applied to the hash: its item is a PAGE, not a texel, so
                         // nsPerPx above divides by the wrong denominator. These two columns are
                         // the rate the byte loop is actually paid at, and the only ones a
                         // before/after on this phase's lever may be quoted from.
                         static_cast<double>(
                             g_g513HashBytes.load(std::memory_order_relaxed)) /
                             std::max(1.0, hashN) / 1024.0,
                         hash * n * 1.0e6 /
                             std::max(1.0, static_cast<double>(
                                 g_g513HashBytes.load(std::memory_order_relaxed))));
        }
    }
    if (g511Census)
    {
        g_g511TexEntries.fetch_add(g511Entries, std::memory_order_relaxed);
        g_g511TexProbes.fetch_add(g511Probes, std::memory_order_relaxed);
        g_g511TexRunRep.fetch_add(g511RunRep, std::memory_order_relaxed);
        const uint64_t n = g_g511TexFlushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 2000u) == 0u)
        {
            const uint64_t ent = g_g511TexEntries.load(std::memory_order_relaxed);
            const uint64_t prb = g_g511TexProbes.load(std::memory_order_relaxed);
            const uint64_t rep = g_g511TexRunRep.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G511:tex] flushes=%llu entries=%llu probes=%llu (%.1f%% of entries) "
                         "runRepeat=%llu (%.1f%% of probes) entriesPerFlush=%.1f "
                         "probesPerFlush=%.1f\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(ent),
                         static_cast<unsigned long long>(prb),
                         100.0 * static_cast<double>(prb) /
                             static_cast<double>(std::max<uint64_t>(1u, ent)),
                         static_cast<unsigned long long>(rep),
                         100.0 * static_cast<double>(rep) /
                             static_cast<double>(std::max<uint64_t>(1u, prb)),
                         static_cast<double>(ent) / static_cast<double>(n),
                         static_cast<double>(prb) / static_cast<double>(n));
        }
    }

    const auto g298T4 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // Vertices + state-batched draw runs (submission order preserved — no sorting; blending
    // depends on order).
    for (size_t i = 0; i < g_g144List.size(); ++i)
    {
        const G178EntryState &st = s_states[i];
        if (st.skip)
            continue;
        const G144Entry &e = g_g144List[i];
        const int ofx = e.ctx.xyoffset.ofx >> 4;
        const int ofy = e.ctx.xyoffset.ofy >> 4;
        const int first = static_cast<int>(s_batch.verts.size());
        G406UvRoundInfo g406Round;
        // G261 FBO-bound entry: normalize FST UVs against the PRODUCER target's full fbW x 512
        // space instead of the declared texture dims; the V axis is flipped after the push
        // (GS row y lives at GL row 511-y in the persistent FBO).
        const bool g261Bound = g261HaveFboSrc && s_g261FboSrc[i] != 0u;
        const uint64_t drawTexKey =
            (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                ? s_g281ViewKey[i] : st.texKey;
        int pushTexW = st.texW, pushTexH = st.texH;
        if (g261Bound)
        {
            if (s_g261FboSrc[i] == kG289ZeroFbpToken ||
                g289IsOwnerSourceToken(s_g261FboSrc[i]) ||
                s_g261FboSrc[i] == 0x68u ||
                s_g261FboSrc[i] == kG309CompositeToken ||
                s_g261FboSrc[i] == kG310LogicalFbp ||
                s_g261FboSrc[i] == kG310LogicalCt24Token)
            {
                pushTexW = 512;
                pushTexH = 512;
            }
            else
            {
                const int srcTi = g248TargetIndex(s_g261FboSrc[i]);
                pushTexW = g_g261Res[srcTi].fbW;
                pushTexH = 512;
            }
        }
        if (g270LineType(static_cast<uint32_t>(e.prim.type)))
        {
            g270AppendLinePixels(s_batch.verts, e);
        }
        else if (e.prim.type == GS_PRIM_SPRITE)
        {
            // drawSprite: rect [min..max) exclusive right/bottom, FLAT color from v1, UV linearly
            // interpolated with the G8 LEFT-EDGE (0.0) bias — GL samples at pixel centers (+0.5),
            // so shift the UVs by -0.5 * (texel step per pixel) to reproduce the CPU convention.
            const GSVertex &a = e.v[0];
            const GSVertex &b = e.v[1];
            GSVertex q0 = e.v[0], q1 = e.v[1];
            g406Round = g406SpriteUvRoundInfo(
                a, b, ofx, ofy, !st.bilinear, e.prim.tme != 0u, e.prim.fst != 0u);
            if (g255Rtt || g286Transient)
            {
                // drawSprite first truncates screen positions, sorts the bounds, and expands a
                // zero span to one pixel.  The RTT effects deliberately submit full rectangles
                // plus degenerate edge/corner sprites; raw GL triangles silently drop the latter.
                //
                // G365 coverage rule, GPU half — DEFAULT-ON again since G368 (kill:
                // DC2_G365_NO_SPRITE_EDGE=1). See the long note in drawSprite: G366/G367 blamed
                // this rule for the MAP-160 page-row tear, but G368 measured the tear to be the
                // presentation black-fallback reading a 512-wide display buffer at fbw=10, and
                // this rule only perturbed which context that fallback picked.
                static const bool s_g365EdgeGpu = !envFlagEnabled("DC2_G365_NO_SPRITE_EDGE");
                // G367 bisect selector — see the matching note in drawSprite.
                static const int s_g367SelGpu = []() -> int {
                    const char *e = std::getenv("DC2_G367_SEL");
                    return e ? std::atoi(e) : 7;
                }();
                const uint32_t g367Tbp = e.ctx.tex0.tbp0;
                const bool g367IsBlur = (g367Tbp == 0x2860u || g367Tbp == 0x28a0u || g367Tbp == 0x28e0u);
                const int g367Bit = (g367Tbp == 0x0d00u) ? 1 : (g367IsBlur ? 2 : 4);
                int sx0, sy0, sx1, sy1;
                if (s_g365EdgeGpu && (s_g367SelGpu & g367Bit))
                {
                    float fx0 = a.x - static_cast<float>(ofx);
                    float fy0 = a.y - static_cast<float>(ofy);
                    float fx1 = b.x - static_cast<float>(ofx);
                    float fy1 = b.y - static_cast<float>(ofy);
                    if (fx0 > fx1) std::swap(fx0, fx1);
                    if (fy0 > fy1) std::swap(fy0, fy1);
                    sx0 = static_cast<int>(std::ceil(fx0));
                    sy0 = static_cast<int>(std::ceil(fy0));
                    sx1 = static_cast<int>(std::ceil(fx1));
                    sy1 = static_cast<int>(std::ceil(fy1));
                    // G367: zero span under the ceil rule = covers nothing on hardware. Drop the
                    // sprite instead of letting the legacy `max(1,·)` expansion below force it to
                    // one pixel at the wrong place (that combination caused G366's page-row tear).
                    if (sx1 <= sx0 || sy1 <= sy0)
                        continue;
                }
                else
                {
                    sx0 = static_cast<int>(a.x) - ofx;
                    sy0 = static_cast<int>(a.y) - ofy;
                    sx1 = static_cast<int>(b.x) - ofx;
                    sy1 = static_cast<int>(b.y) - ofy;
                    if (sx0 > sx1) std::swap(sx0, sx1);
                    if (sy0 > sy1) std::swap(sy0, sy1);
                }
                // G366 (diagnostic, default-off): every RTT/transient sprite whose ceil-rect
                // differs from the legacy trunc-rect — i.e. exactly the draws G365's GPU-side
                // change moves. Deduped by the rect pair so the log stays short.
                {
                    static const bool s_g366 = (std::getenv("DC2_G366_RECT") != nullptr);
                    if (s_g366)
                    {
                        int tx0 = static_cast<int>(a.x) - ofx, ty0 = static_cast<int>(a.y) - ofy;
                        int tx1 = static_cast<int>(b.x) - ofx, ty1 = static_cast<int>(b.y) - ofy;
                        if (tx0 > tx1) std::swap(tx0, tx1);
                        if (ty0 > ty1) std::swap(ty0, ty1);
                        if (tx0 != sx0 || ty0 != sy0 || tx1 != sx1 || ty1 != sy1)
                        {
                            char key[256];
                            std::snprintf(key, sizeof(key),
                                "fbp=0x%x tbp=0x%x tme=%u trunc=(%d,%d)-(%d,%d) ceil=(%d,%d)-(%d,%d) "
                                "raw=(%.2f,%.2f)-(%.2f,%.2f)",
                                fbp, e.ctx.tex0.tbp0, (unsigned)e.prim.tme,
                                tx0, ty0, tx1, ty1, sx0, sy0, sx1, sy1,
                                a.x - (float)ofx, a.y - (float)ofy,
                                b.x - (float)ofx, b.y - (float)ofy);
                            static std::mutex s_m;
                            static std::unordered_set<std::string> s_seen;
                            std::lock_guard<std::mutex> lk(s_m);
                            if (s_seen.size() < 64u && s_seen.insert(key).second)
                                std::fprintf(stderr, "[G366:rect] %s\n", key);
                        }
                    }
                }
                q0.x = static_cast<float>(sx0 + ofx);
                q0.y = static_cast<float>(sy0 + ofy);
                q1.x = static_cast<float>(sx0 + std::max(1, sx1 - sx0) + ofx);
                q1.y = static_cast<float>(sy0 + std::max(1, sy1 - sy0) + ofy);
                if (e.prim.fst && !g406Round.active)
                {
                    // CPU FST sprites derive endpoints with integer `u >> 4` / `v >> 4` before
                    // interpolation. Preserve that truncation instead of feeding fractional 12.4
                    // endpoints into GL.
                    q0.u = static_cast<uint16_t>((q0.u >> 4u) << 4u);
                    q0.v = static_cast<uint16_t>((q0.v >> 4u) << 4u);
                    q1.u = static_cast<uint16_t>((q1.u >> 4u) << 4u);
                    q1.v = static_cast<uint16_t>((q1.v >> 4u) << 4u);
                }
            }
            float uvShiftU0 = 0.0f, uvShiftU1 = 0.0f;
            float uvShiftV0 = 0.0f, uvShiftV1 = 0.0f;
            if (e.prim.tme && e.prim.fst)
            {
                const float dxp = q1.x - q0.x; // positions are already pixel-unit floats (kick /16)
                const float dyp = q1.y - q0.y;
                const bool g407SubpixelPhase =
                    g406Round.active && g407SpriteSubpixelPhaseEnabled();
                const float rawX0 = a.x - static_cast<float>(ofx);
                const float rawY0 = a.y - static_cast<float>(ofy);
                const float rawX1 = b.x - static_cast<float>(ofx);
                const float rawY1 = b.y - static_cast<float>(ofy);
                const float rawDx = rawX1 - rawX0;
                const float rawDy = rawY1 - rawY0;
                if (g407SubpixelPhase && std::fabs(rawDx) >= (1.0f / 16.0f))
                {
                    const float step =
                        (static_cast<float>(b.u) - static_cast<float>(a.u)) /
                        16.0f / rawDx;
                    uvShiftU0 =
                        ((q0.x - static_cast<float>(ofx)) - rawX0 - 0.5f) * step;
                    uvShiftU1 =
                        ((q1.x - static_cast<float>(ofx)) - rawX1 - 0.5f) * step;
                }
                else if (dxp != 0.0f)
                {
                    uvShiftU0 = uvShiftU1 =
                        -0.5f * (static_cast<float>(q1.u) - static_cast<float>(q0.u)) /
                        16.0f / dxp;
                }
                if (g407SubpixelPhase && std::fabs(rawDy) >= (1.0f / 16.0f))
                {
                    const float step =
                        (static_cast<float>(b.v) - static_cast<float>(a.v)) /
                        16.0f / rawDy;
                    uvShiftV0 =
                        ((q0.y - static_cast<float>(ofy)) - rawY0 - 0.5f) * step;
                    uvShiftV1 =
                        ((q1.y - static_cast<float>(ofy)) - rawY1 - 0.5f) * step;
                }
                else if (dyp != 0.0f)
                {
                    uvShiftV0 = uvShiftV1 =
                        -0.5f * (static_cast<float>(q1.v) - static_cast<float>(q0.v)) /
                        16.0f / dyp;
                }
            }
            // Two triangles covering the rect; UVs pair with the ORIGINAL corners (axis-aligned).
            GSVertex c00 = q0, c11 = q1, c10 = q0, c01 = q0;
            c10.x = q1.x; c10.u = q1.u; // (x1, y0)
            c01.y = q1.y; c01.v = q1.v; // (x0, y1)
            // The GS uses the second kick's Z as one flat value for a sprite. Interpolating the
            // two endpoint Z values would turn a clear sprite into a depth ramp.
            c00.z = c10.z = c11.z = c01.z = b.z;
            c00.fog = c10.fog = c11.fog = c01.fog = b.fog; // G364: sprite F is flat (2nd vertex)
            const bool tme = e.prim.tme, fst = e.prim.fst;
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU0, uvShiftV0);
            g178PushVertex(s_batch.verts, c10, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU1, uvShiftV0);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU1, uvShiftV1);
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU0, uvShiftV0);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU1, uvShiftV1);
            g178PushVertex(s_batch.verts, c01, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU0, uvShiftV1);
        }
        else
        {
            // Triangle: gouraud (iip=1) uses per-vertex color; flat uses v2's (drawTriangle).
            const bool tme = e.prim.tme, fst = e.prim.fst, iip = e.prim.iip;
            for (int k = 0; k < 3; ++k)
                g178PushVertex(s_batch.verts, e.v[k], ofx, ofy, iip ? e.v[k] : e.v[2],
                               tme, fst, pushTexW, pushTexH, 0.0f, 0.0f);
        }
        const int count = static_cast<int>(s_batch.verts.size()) - first;
        if (g261Bound)
        {
            // GS texel row v ↔ FBO GL row 511-v: flip the normalized T coordinate. Linear in
            // screen space, so a per-vertex flip is exact under noperspective interpolation.
            for (int k = first; k < first + count; ++k)
                s_batch.verts[static_cast<size_t>(k)].tq =
                    1.0f - s_batch.verts[static_cast<size_t>(k)].tq;
        }
        if (!g256ExactRttOn() && !s_batch.draws.empty() &&
            g178StateEqual(s_states[i], s_states[i - 1]) &&
            (!g261HaveFboSrc || s_g261FboSrc[i] == s_g261FboSrc[i - 1]) &&
            s_batch.draws.back().texKey == (g261Bound ? 0u : drawTexKey) &&
            s_batch.draws.back().uvOriginX == g406Round.originX &&
            s_batch.draws.back().uvOriginY == g406Round.originY &&
            s_batch.draws.back().uvRoundU == g406Round.flagsU &&
            s_batch.draws.back().uvRoundV == g406Round.flagsV &&
            s_batch.draws.back().firstVtx + s_batch.draws.back().vtxCount == first)
        {
            s_batch.draws.back().vtxCount += count;
        }
        else
        {
            G178Draw d;
            d.firstVtx = first;
            d.vtxCount = count;
            d.texKey = g261Bound ? 0u : drawTexKey;
            d.srcFbp = g261Bound ? s_g261FboSrc[i] : 0u;
            d.blend = st.blend;
            d.tfx = st.tfx;
            d.tcc = st.tcc;
            d.fge = st.fge;       // G364
            d.fogCol = st.fogCol; // G364
            d.depthFunc = st.depthFunc;
            d.depthWrite = st.depthWrite;
            d.bilinear = st.bilinear;
            d.wrapU = st.wrapU;
            d.wrapV = st.wrapV;
            d.uvOriginX = static_cast<uint16_t>(g406Round.originX);
            d.uvOriginY = static_cast<uint16_t>(g406Round.originY);
            d.uvRoundU = g406Round.flagsU;
            d.uvRoundV = g406Round.flagsV;
            d.scX0 = st.scX0; d.scY0 = st.scY0; d.scX1 = st.scX1; d.scY1 = st.scY1;
            s_batch.draws.push_back(d);
        }
    }

    const auto g298T5 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // G255 verification is intentionally color-only. Any batch that needs real guest depth has
    // already failed closed unless the separately opt-in G242 ownership model is active; do not
    // combine those two experiments in one oracle.
    if ((g255Rtt || (g286Transient && g286VerifyOn())) && !guestDepth)
        g255BeginVerify(vram, vramSize, fbp, fbw, fbW, rowLo, rowHi);

    const auto g273T1 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const int g275ReadRows = g262WaveDepth ? (depthRowHi - depthRowLo + 1) : 0;
    const int g275ReadGlY = g262WaveDepth ? (kFbH - 1 - depthRowHi) : 0;
    // G337 widening: a skip-readback wave writes no VRAM color this flush, so the disjointness
    // requirement (which protects the immediate color writeback ordering) is vacuous for it.
    const bool g337WideEligible =
        g337ZAuthOn() && s_batch.skipReadback && !g290Seeded && !g282Tight;
    const bool g278DeferDepth =
        g278DepthRbOn() && g262WaveDepth &&
        (g278OwnerEligible || g337WideEligible) && guestDepthWrites &&
        !g275FusedZReadOn() &&
        // G535: a private-mirror owner has a CPU-rasterizer consumer that no publication edge
        // covers, so it must publish in-wave instead of deferring. Gating g278OwnerEligible alone
        // would leave g337WideEligible as an open second path. See the note at that site.
        // g535EdgePubOn() is the cheaper candidate: it restores the deferral and instead closes the
        // consumer edge itself in g528PublishScoped.
        (!g411PrivateDepth || s_g535PrivZDefer || g535EdgePubOn());
    if (g278DeferDepth && !g278OwnerEligible)
        g_g337WideDefer.fetch_add(1u, std::memory_order_relaxed);
    if (g337ZAuthOn() && g262WaveDepth && guestDepthWrites && !g278DeferDepth)
    {
        if (g290Seeded)               g_g337DirSeeded.fetch_add(1u, std::memory_order_relaxed);
        else if (g282Tight)           g_g337DirTight.fetch_add(1u, std::memory_order_relaxed);
        else if (!s_batch.skipReadback) g_g337DirNoSkipRb.fetch_add(1u, std::memory_order_relaxed);
        else                          g_g337DirOther.fetch_add(1u, std::memory_order_relaxed);
    }
    const bool g275FuseReadback =
        g262WaveDepth && guestDepthWrites && g275FusedZReadOn() &&
        !g278DeferDepth;
    static std::vector<float> s_g262DepthReadback;
    if (g262WaveDepth && guestDepthWrites && !g278DeferDepth)
        s_g262DepthReadback.clear();
    // G274: time EVERY synchronous backend submit (RTT color/depth waves AND display batches),
    // not only G273's display profile. This is the blocking future to the GPU worker; the
    // hypothesis is that per-wave submit/sync latency dominates the pending-graph drain.
    // G314 needs only the submits executed inside one real upload edge.  Reuse these proven
    // category counters while that edge scope is active; no global probe or behavior arm turns on.
    const bool g276Probe = g276ProbeOn() || g314EdgeActive();
    const auto g324SubmitT0 = g324IsArmed && g324Diag.hadProducer
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    if (g324IsArmed && g324Diag.hadProducer)
        g324Diag.tailPrepNs +=
            g324ElapsedNs(g324Diag.lastProducerDone, g324SubmitT0);
    const auto g274SubT0 = (g274ZTimeOn() || g276Probe) ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const auto g275FusedT0 = g275FuseReadback ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    // G305 (default-off DC2_G305_ASYNC=1): async-eligible = a skip-readback color wave with no
    // guest depth and no readback-consuming oracle/transient/fanout/view path. Its post-submit
    // epilogue is pure front-end bookkeeping (readback is empty), so it commits immediately and the
    // render overlaps the next flush's prep. Everything else stays on the synchronous path.
    // Eligible = skip-readback color wave whose post-submit epilogue is pure front-end bookkeeping.
    // s_g281ViewSrc (T8-view sources) is NOT excluded: it is per-flush state used only to BUILD the
    // batch and, on failure, to materialize sources — and the drain-time failure re-run rebuilds it
    // from the retained list, so an async success never needs it.
    if (g310Freeze.active && !g310FrozenUsable())
    {
        g_g266FlushRej = kG266FrSubmit;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }
    // G444: `|| g444AsyncOn()` relaxes ONLY the G310 exclusion (and only when the probe is armed);
    // every other eligibility term is unchanged, so a non-armed run takes the identical decision.
    const bool g452Async = g452AsyncArm();
    const bool g305Async =
        (g305AsyncOn() || g444AsyncOn() || g452Async) && !g_g305ForceSync && s_batch.skipReadback && !guestDepth &&
        !g275FuseReadback && !g262WaveDepth && !g286Transient && !g282Tight &&
        !(g255Rtt && g255VerifyOn()) &&
        (!g310On() || g444AsyncOn() || g452Async);
    // G324 premise only: this is the existing G305 pure-epilogue contract with G310 admitted as
    // the missing completion-ordered state family. No submission behavior changes here.
    const bool g324TxnEligible =
        g324Diag.hadProducer && g310Freeze.active && s_batch.skipReadback && !guestDepth &&
        !g275FuseReadback && !g262WaveDepth && !g286Transient && !g282Tight &&
        !(g255Rtt && g255VerifyOn());
    if (g305StatOn())
    {
        static std::atomic<uint64_t> nF{0}, nSkip{0}, nNoD{0}, nElig{0};
        const uint64_t f = nF.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (s_batch.skipReadback) nSkip.fetch_add(1u, std::memory_order_relaxed);
        if (!guestDepth) nNoD.fetch_add(1u, std::memory_order_relaxed);
        if (g305Async) nElig.fetch_add(1u, std::memory_order_relaxed);
        if ((f % 4096u) == 0u)
            std::fprintf(stderr,
                "[G305:elig] flush=%llu skipRb=%llu noDepth=%llu elig=%llu "
                "(this: skipRb=%d noDepth=%d g262wd=%d g275fr=%d g286=%d g282=%d g281v=%d)\n",
                (unsigned long long)f, (unsigned long long)nSkip.load(std::memory_order_relaxed),
                (unsigned long long)nNoD.load(std::memory_order_relaxed),
                (unsigned long long)nElig.load(std::memory_order_relaxed),
                s_batch.skipReadback ? 1 : 0, guestDepth ? 0 : 1, g262WaveDepth ? 1 : 0,
                g275FuseReadback ? 1 : 0, g286Transient ? 1 : 0, g282Tight ? 1 : 0,
                s_g281ViewSrc.empty() ? 0 : 1);
    }
    // G415: every consumer of s_batch.readback reads exactly GS rows rowLo..rowHi — the VRAM pack
    // loop below and both g255StageGpuVerify oracles use the same window and the same
    // (kFbH-1-y)*fbW indexing. Publish that window (GL rows are bottom-first, so buffer row index
    // == GL row) so the backend can read back only those rows into their existing offsets in the
    // same full-height buffer. Rows outside the window were already copied out of the FBO and
    // discarded. Published unconditionally: the backend honours it only when its arm is on, and
    // the census needs the window even in the control arm.
    g415_backend_set_color_window(kFbH - 1 - rowHi, rowHi - rowLo + 1);
    // G493 LATE DRAIN. Everything above this line — batch building, materialization, the G310
    // freeze, argument validation and the colour-window publication — is the front-end CPU the
    // previous packet's render was supposed to overlap with, and under the shipped top-of-flush
    // drain it overlapped with none of it. Retire the packet HERE instead: submitAsync's one slot
    // is free by the next statement, GL order is unchanged (single FIFO worker), and every
    // consumer-side drain site is untouched. Failure is deferred, never replayed inline: s_batch is
    // live and the replay rebuilds it.
    if (g305DrainRequired() && g493LateDrainActive())
        g493LateDrainPending();
    bool submitted;
    if (g305Async)
    {
        submitted = g178_backend_submit_async(s_batch);
        if (submitted)
        {
            g493NoteSubmit();
            // Retain fallback authority for the drain-time failure check; leave g_g144List empty so
            // the closure's post-success clear() is a no-op and the next flush accumulates fresh.
            G305Pending pending;
            pending.gs = gs;
            pending.list.swap(g_g144List);
            g_g305Pending.push_back(std::move(pending));
            g_g305Submitted.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    else
    {
        submitted = g275FuseReadback
            ? g275_backend_submit_depth_readback(
                  s_batch, depthKey, depthUpload, depthGlY, depthRows,
                  g275ReadGlY, g275ReadRows, s_g262DepthReadback)
            : (guestDepth
                   ? g242_backend_submit_depth(s_batch, depthKey, depthUpload,
                                               depthGlY, depthRows)
                   : g178_backend_submit(s_batch));
    }
    if (g324IsArmed && g324Diag.hadProducer)
        g324Diag.submitNs +=
            g324ElapsedNs(g324SubmitT0, std::chrono::steady_clock::now());
    if (submitted && g324TxnEligible)
        // Starts the independent front-end interval a future non-blocking transaction could hide
        // behind. g324NoteObserver consumes it at the first next flush/materialize/frame boundary.
        g324ArmObserver();
    if (g275FuseReadback)
    {
        g_g275FusedNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g275FusedT0).count(),
            std::memory_order_relaxed);
        if (submitted)
        {
            g_g275FusedJobs.fetch_add(1u, std::memory_order_relaxed);
            g_g275ReadWaitsElided.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    if (g274ZTimeOn() || g276Probe)
    {
        const uint64_t subNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - g274SubT0).count();
        g_g274SubNs.fetch_add(subNs, std::memory_order_relaxed);
        g_g274SubN.fetch_add(1u, std::memory_order_relaxed);
        if (g276Probe)
        {
            const int si = s_batch.skipReadback ? 1 : 0;
            const int di = guestDepthWrites ? 1 : 0;
            g_g276SubNs[si][di].fetch_add(subNs, std::memory_order_relaxed);
            g_g276SubN[si][di].fetch_add(1u, std::memory_order_relaxed);
        }
    }
    const auto g273T2 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto g298T6 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (!submitted)
    {
        // A G281 view kept its producer newer in the FBO. If the consumer batch cannot submit
        // and will replay on the CPU, publish that producer first so CPU sampling sees the same
        // bytes. This is a failure-only completion edge; successful batches remain readback-free.
        if (!s_g281ViewSrc.empty())
        {
            bool done[kG248TargetCount] = {};
            for (int8_t srcTi : s_g281ViewSrc)
                if (srcTi >= 0 && srcTi < static_cast<int8_t>(kG248TargetCount) &&
                    !done[static_cast<uint8_t>(srcTi)])
                {
                    done[static_cast<uint8_t>(srcTi)] = true;
                    g261Materialize(srcTi, kG261MatTexAlias);
                }
        }
        g_g266FlushRej = kG266FrSubmit;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear(); // the CPU replay about to run writes VRAM the gens don't track
        return false;
    }
    // The synchronous worker has consumed the frozen logical texture. It may now follow any
    // successful materializations that occurred while this batch was being prepared.
    g310Freeze.complete();
    if (g282Tight)
    {
        // The leading clear made every planned source page independent of its old contents.
        // Publish the completed GPU result to the resident sibling views now, while command
        // order is exact and before any of them can materialize stale bytes.
        if (!g282ExecuteFanout(s_g282Fanout))
        {
            g282DropResidencyAfterFanoutFailure();
            g_g266FlushRej = kG266FrSubmit;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        g282CommitFanout(s_g282Fanout);
    }
    if (depthState && stagedDepthUpload)
    {
        // Commit ownership only after the backend accepted and completed the whole batch.
        depthState->valid = true;
        depthState->guestGen = stagedGuestGen;
    }
    if (g262WaveDepth)
        g_g262DepthInit.insert(depthKey); // full-init (or window update) reached the texture
    if (g262WaveDepth && guestDepthWrites && !g278DeferDepth)
    {
        // G262: materialize the wave's Z writes into guest VRAM immediately (the wave-granular
        // barrier). Between waves VRAM Z stays authoritative, so nothing persists on the GPU and
        // there is no ownership invariant to trip. Same float→uint24 packing as g242SyncDepthState.
        // Readback covers only the depth ROW WINDOW (scissor confines Z writes to it), even when
        // the upload was a full first-init.
        const int rbRows = g275ReadRows;
        const int rbGlY = g275ReadGlY;
        const auto g274ZRbT0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
        if ((!g275FuseReadback &&
             !g242_backend_read_depth(depthKey, fbW, kFbH, rbGlY, rbRows,
                                      s_g262DepthReadback)) ||
            s_g262DepthReadback.size() != static_cast<size_t>(fbW) * rbRows)
        {
            // Fail loudly and fall back: the FBO result is orphaned (snapshot cleared → next
            // flush re-uploads from VRAM) and the CPU replay re-renders color AND Z into VRAM,
            // so guest state stays coherent.
            std::fprintf(stderr, "[G262:depth] Z readback FAIL fbp=0x%x rows=%d..%d\n",
                         fbp, depthRowLo, depthRowHi);
            g_g266FlushRej = kG266FrZReadback;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        constexpr double kDepthToZ = 4294967296.0;
        const uint32_t zbpBlock = guestZbpPage << 5u;
        const auto unpackDepthRows = [&](int laneLo, int laneHi) {
            for (int y = laneLo; y <= laneHi; ++y)
            {
                const size_t srcRow = static_cast<size_t>(depthRowHi - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    double scaled =
                        static_cast<double>(s_g262DepthReadback[srcRow + x]) *
                        kDepthToZ;
                    if (scaled < 0.0) scaled = 0.0;
                    if (scaled > 16777215.0) scaled = 16777215.0;
                    if (g411PrivateDepth)
                        g403DisplayZWrite(x, y, fbp, static_cast<uint32_t>(scaled));
                    else
                        GSMem::WriteZ24(vram, zbpBlock, fbw,
                                        static_cast<uint32_t>(x),
                                        static_cast<uint32_t>(y),
                                        static_cast<uint32_t>(scaled));
                }
            }
        };
        const int swizzleLanes =
            rbRows >= 8 ? g275ZSwizzleLanes() : 1;
        GSRowPool::instance().run(depthRowLo, depthRowHi, swizzleLanes,
                                  unpackDepthRows);
        // Publish the Z bytes exactly as an inline CPU Z writer would (the G260 ownership rule):
        // page write-generations for range/gen consumers, plus the coarse decoded-texture gen
        // (a Z page can alias texture/CLUT storage).
        if (!g411PrivateDepth)
        {
            g178BumpRectImpl(zbpBlock, fbw, GS_PSM_Z24, 0u,
                             static_cast<uint32_t>(depthRowLo), static_cast<uint32_t>(fbW),
                             static_cast<uint32_t>(rbRows));
            g149BumpVramGen();
        }
        if (g274ZTimeOn())
            g_g274ZRbNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - g274ZRbT0).count(),
                                  std::memory_order_relaxed);
        if (!g411PrivateDepth && (g274ZTimeOn() || g274ZResidentOn()))
            // Baseline the whole-target Z gen AFTER our own writeback bump so the next same-key
            // upload can tell whether any OTHER writer touched VRAM Z in between. This readback
            // just synced GPU->VRAM, so the resident texture now equals VRAM at this gen.
            g_g274LastZGen[depthKey] = g178GenSumRect(zbpBlock, fbw, GS_PSM_Z24, 0u, 0u,
                                                      static_cast<uint32_t>(fbW), 512u);
        g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
        if (g411PrivateDepth)
        {
            g_g411DepthPublishes.fetch_add(1u, std::memory_order_relaxed);
            g_g411DepthPublishRows.fetch_add(
                static_cast<uint64_t>(rbRows), std::memory_order_relaxed);
        }
        if (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp))
            g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
        if (g277CensusOn())
        {
            // Simulated within-drain Z-readback coalescing: this readback is coalescable when the
            // previous same-key readback's run is still live (no real VRAM-Z consumer edge and no
            // cross-key Z wave in between — a different-key wave re-reads VRAM Z for its upload).
            g_g277ZRb.fetch_add(1u, std::memory_order_relaxed);
            if (s_g277ZRunLive && s_g277ZRunKey == depthKey)
                g_g277ZRbCoalescable.fetch_add(1u, std::memory_order_relaxed);
            s_g277ZRunLive = true;
            s_g277ZRunKey = depthKey;
            s_g277ZRunRange = g144RangeForRect(zbpBlock, GS_PSM_Z24, fbw, 0u, 0u,
                                               static_cast<uint32_t>(fbW), 512u);
        }
    }
    else if (g278DeferDepth)
    {
        G278PendDepth *slotP = nullptr;
        if (!g337ZAuthOn())
        {
            // Legacy single-slot owner: a cross-key arrival is an invariant break and publishes.
            const bool continuing = s_g278Pend.active;
            if (continuing &&
                (s_g278Pend.key != depthKey || s_g278Pend.vram != vram ||
                 s_g278Pend.vramSize != vramSize ||
                 s_g278Pend.privateMirror != g411PrivateDepth))
            {
                const uint64_t n =
                    g_g278InvariantFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                std::fprintf(stderr,
                             "[G278:invariant] pending owner changed after submit n=%llu "
                             "old=0x%llx new=0x%llx\n",
                             (unsigned long long)n,
                             (unsigned long long)s_g278Pend.key,
                             (unsigned long long)depthKey);
                if (!g278FlushPendingDepth(0))
                {
                    g_g266FlushRej = kG266FrZReadback;
                    g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                    g_g178FbSnap.clear();
                    return false;
                }
            }
            slotP = &s_g278Pend;
        }
        else
        {
            // G337 slot table: same-key slot continues; otherwise take a free slot, evicting
            // (publishing) an existing owner only when the table is full. A new owner whose
            // physical Z range can alias another active owner's range (cross-fbw same-zbp)
            // publishes that other owner first — publication order across aliased owners is
            // arrival order, exactly as the per-wave path interleaved them.
            for (int s = 0; s < kG278PendSlots; ++s)
            {
                G278PendDepth &p = s_g278PendTab[s];
                if (p.active && p.key == depthKey && p.vram == vram &&
                    p.vramSize == vramSize &&
                    p.privateMirror == g411PrivateDepth)
                {
                    slotP = &p;
                    break;
                }
            }
            if (slotP == nullptr)
                for (int s = 0; s < kG278PendSlots; ++s)
                    if (!s_g278PendTab[s].active)
                    {
                        slotP = &s_g278PendTab[s];
                        break;
                    }
            if (slotP == nullptr)
            {
                g_g337SlotEvict.fetch_add(1u, std::memory_order_relaxed);
                if (!g278FlushPendSlot(s_g278PendTab[0], 0))
                {
                    g_g266FlushRej = kG266FrZReadback;
                    g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                    g_g178FbSnap.clear();
                    return false;
                }
                slotP = &s_g278PendTab[0];
            }
            const G144BlockRange wholeThis =
                g144RangeForRect(guestZbpPage << 5u, GS_PSM_Z24, fbw, 0u, 0u,
                                 static_cast<uint32_t>(fbW), 512u);
            for (int s = 0; s < kG278PendSlots; ++s)
            {
                G278PendDepth &p = s_g278PendTab[s];
                if (!p.active || &p == slotP)
                    continue;
                if (g411PrivateDepth || p.privateMirror)
                    continue; // private and physical owners have no storage overlap
                const G144BlockRange wholeOther = g278PendingDepthWholeRangeP(p);
                if (!wholeThis.valid || !wholeOther.valid ||
                    g144RangeOverlaps(wholeOther, wholeThis))
                {
                    g_g337OvlFlush.fetch_add(1u, std::memory_order_relaxed);
                    if (!g278FlushPendSlot(p, 0))
                    {
                        g_g266FlushRej = kG266FrZReadback;
                        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                        g_g178FbSnap.clear();
                        return false;
                    }
                }
            }
        }
        if (!slotP->active)
        {
            slotP->active = true;
            slotP->privateMirror = g411PrivateDepth;
            slotP->key = depthKey;
            slotP->vram = vram;
            slotP->vramSize = vramSize;
            slotP->zbpPage = guestZbpPage;
            slotP->fbw = fbw;
            slotP->fbW = fbW;
            slotP->rowLo = depthRowLo;
            slotP->rowHi = depthRowHi;
        }
        else
        {
            slotP->rowLo = std::min(slotP->rowLo, depthRowLo);
            slotP->rowHi = std::max(slotP->rowHi, depthRowHi);
            g_g278Saved.fetch_add(1u, std::memory_order_relaxed);
        }
        slotP->display =
            slotP->display ||
            (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp));
        const uint64_t deferred =
            g_g278Deferred.fetch_add(1u, std::memory_order_relaxed) + 1u;
        // Timeout-driven harnesses do not run process-exit reporting. Keep the stat useful even
        // when coalescing holds the flush count below its normal 256-event print cadence.
        if (g278StatOn() && (deferred % 256u) == 0u)
            g278Report(0u);
    }

    // Write the rendered rows back into guest VRAM (same addressing as writePixel) so the
    // present latch / frame dumps / any guest read sees the GPU result.
    if (s_batch.readback.size() == static_cast<size_t>(fbW) * kFbH)
    {
        if (g276ProbeOn())
        {
            g_g276ColorRbN.fetch_add(1u, std::memory_order_relaxed);
            g_g276ColorRbWinRows.fetch_add(
                static_cast<uint64_t>(rowHi - rowLo + 1), std::memory_order_relaxed);
            g_g276ColorRbFullRows.fetch_add(static_cast<uint64_t>(kFbH),
                                            std::memory_order_relaxed);
        }
        // G418: the CPU-copy component of the colour-readback edge. `legacy` is the original serial
        // per-pixel address loop, kept byte-identical as the control arm; the fast arm is the exact
        // one-page address table with paired stores over disjoint guest rows.
        {
            const bool g418Profile = g418ProfileOn();
            const int g418Ballast = g418AltBallast();
            const bool g418Pick = g418AlternatePick(g418FastUnpackOn());
            const bool g418Fast = g418Ballast > 0 ? false : g418Pick;
            const auto g418T0 = g418Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            g418UnpackColorRows(vram, vramSize, s_batch.readback, fbBlock, fbw, fbW, kFbH,
                                rowLo, rowHi, !g418Fast);
            if (g418Ballast > 0 && g418Pick)
                for (int i = 0; i < g418Ballast; ++i)
                    g418UnpackColorRows(vram, vramSize, s_batch.readback, fbBlock, fbw, fbW,
                                        kFbH, rowLo, rowHi, true);
            if (g418Profile)
                g418NoteUnpack(fbp, rowHi - rowLo + 1, fbW,
                               static_cast<uint64_t>(
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - g418T0).count()));
            if (g418VerifyOn())
                g418VerifyUnpack(vram, vramSize, s_batch.readback, fbBlock, fbw, fbW, kFbH,
                                 rowLo, rowHi);
        }
        // The color and depth backends are distinct even when their GS storage aliases. The
        // admitted write windows are disjoint, so this cannot overwrite pending dirty Z rows;
        // remember any other affected Z rows so a later same-key wave cannot use stale depth.
        if (g278ColorWrite.valid)
            g278MarkStaleRowsForRange(g278ColorWrite); // marks every overlapped pending owner
        if (g286Transient)
        {
            // Unlike display color, this work target is consumed later as ordinary VRAM. Publish
            // the same page generations an inline draw would advance, after bytes really changed.
            g178BumpRectImpl(fbBlock, fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                             static_cast<uint32_t>(rowHi - rowLo + 1));
            fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                                      static_cast<uint32_t>(fbW), kFbH);
            const uint64_t done =
                g_g286GpuSuccess.fetch_add(1u, std::memory_order_relaxed) + 1u;
            g_g286RowsPublished.fetch_add(
                static_cast<uint64_t>(rowHi - rowLo + 1), std::memory_order_relaxed);
            if (g286StatOn() && (done <= 8u || (done % 120u) == 0u))
                std::fprintf(stderr,
                             "[G287:transient] gpu=%llu attempts=%llu captured=%llu "
                             "rows=%llu fbp=%03x fbw=%u win=%d..%d entries=%zu\n",
                             (unsigned long long)done,
                             (unsigned long long)g_g286GpuAttempts.load(std::memory_order_relaxed),
                             (unsigned long long)g_g286Captured.load(std::memory_order_relaxed),
                             (unsigned long long)g_g286RowsPublished.load(std::memory_order_relaxed),
                             fbp, fbw, rowLo, rowHi, g_g144List.size());
            if (g289ConsumeOwner)
            {
                G289CopyOwner *o13b = g345OwnerFor(0x13bu);
                const uint64_t version = o13b != nullptr ? o13b->version : 0u;
                if (o13b != nullptr)
                    o13b->active = false;
                if (o13b == &s_g289Owners[0])
                    s_g289PendingUpload.active = false;
                g289Guard.armed = false;
                const uint64_t n =
                    g_g289Consumes.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:consume] n=%llu version=%llu fbp=0x13b rows=0..415\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(version));
            }
        }
    }
    if (g272Wave)
    {
        // Display color remains in the persistent FBO until an actual VRAM consumer or the
        // presentation boundary. Page generations advance only when materialization changes VRAM.
        g272MarkDirty(fbp, fbw, fbW, rowLo, rowHi, vramSize);
    }
    if (g255Rtt)
    {
        const int ti = g248TargetIndex(fbp);
        if (g261Wave)
        {
            // G261 wave: VRAM did NOT change (readback skipped), so the page generations stay
            // untouched — the ownership edge is carried by the residency record instead. The
            // generations advance when (and only when) the rows materialize at a real CPU
            // consumer edge, which is when the VRAM bytes actually change.
            G261Res &r = g_g261Res[ti];
            g455NoteResidentWrite(ti);
            r.fbw = fbw;
            r.fbW = fbW;
            r.dirty = true;
            if (rowLo < r.dirtyLo) r.dirtyLo = rowLo;
            if (rowHi > r.dirtyHi) r.dirtyHi = rowHi;
            if (g291AtlasOn() && r.g291CovFbw != 0u)
            {
                // G291: the wave just rendered these rows on the GPU — the FBO is newer than
                // VRAM for every pixel it may have written. Conservative whole-row clear
                // (scissor columns ignored, fail-closed direction).
                for (int y = rowLo; y <= rowHi && y < 512; ++y)
                    r.g291Cov[static_cast<size_t>(y)].fill(0u);
            }
            // G280: this wave changed the target's FBO content — stale any alias copies made
            // FROM it and drop any of its OWN overlay tiles the rendered rows overlap (the
            // draw layered over the resolved page state exactly as GS command order would).
            if (s_batch.uploadFb)
                g280NoteFboContentChange(ti, 0, 511);
            g280NoteFboContentChange(ti, rowLo, rowHi);
            if (g282Tight)
                g282CommitSourceOverlays(s_g282Fanout);
            // window/range/gen baseline refresh happens after the snapshot row update below
            // (cov must include this flush's rendered rows first).
            g_g261SkippedRb.fetch_add(1u, std::memory_order_relaxed);
            const uint64_t nWaves =
                g_g261Waves.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((nWaves % 1024u) == 0u ||
                (g283StatOn() && (nWaves % 128u) == 0u)) // G283 bring-up observability
                g261Report();
        }
        else
        {
            // The RTT color write is a texture-producer event. Advance the same page generations
            // used by G178's texture registry before a later display-target composite asks
            // whether its GPU copy is still resident. This is the ownership edge the dormant
            // G252 GPU switch lacked.
            g178BumpRectImpl(fbBlock, fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                             static_cast<uint32_t>(rows));
            fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                                      static_cast<uint32_t>(fbW), kFbH);
        }
        if (ti >= 0)
            g_g255Gpu[ti].fetch_add(1u, std::memory_order_relaxed);
    }
    if (depthState)
    {
        if (guestDepthWrites)
        {
            depthState->dirty = true;
            depthState->dirtyLo = std::min(depthState->dirtyLo, depthRowLo);
            depthState->dirtyHi = std::max(depthState->dirtyHi, depthRowHi);
        }
        static std::atomic<uint64_t> s_g242DepthBatches{0};
        static std::atomic<uint64_t> s_g242DepthPixels{0};
        const uint64_t nDepth = s_g242DepthBatches.fetch_add(1u, std::memory_order_relaxed) + 1u;
        s_g242DepthPixels.fetch_add(static_cast<uint64_t>(fbW) *
                                        static_cast<uint64_t>(depthRowHi - depthRowLo + 1),
                                    std::memory_order_relaxed);
        static const bool s_g242Stat = envFlagEnabled("DC2_G242_STAT");
        if (s_g242Stat && (nDepth % 120u) == 0u)
            std::fprintf(stderr,
                "[G242:depth] batches=%llu pixels=%llu uploads=%llu syncs=%llu "
                "last(zbp=0x%x rows=%d..%d write=%u dirty=%u)\n",
                static_cast<unsigned long long>(nDepth),
                static_cast<unsigned long long>(s_g242DepthPixels.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_g242DepthUploads.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_g242DepthSyncs.load(std::memory_order_relaxed)),
                guestZbpPage, depthRowLo, depthRowHi, guestDepthWrites ? 1u : 0u,
                 depthState->dirty ? 1u : 0u);
    }
    if (g272Wave || (g276Coalesce && g262WaveDepth && guestDepthWrites))
    {
        // G265 can publish its non-persistent Z result after fbGenNow was sampled for the upload
        // decision. Normal color readback is written after that Z publication and is therefore the
        // effective winner for any conservative page alias; retained FBO color has the same order.
        // Re-anchor the snapshot now so the next display batch cannot upload pre-wave VRAM over it.
        // G277-R: G276-deferred batches need the same re-anchor for their OWN Z writeback bump —
        // without it the next same-target batch re-uploads stale VRAM over unpublished FBO rows
        // (the Max-body flicker). Same-thread flush ownership makes the re-sample exact: only this
        // flush's own writeback bumped between the sample above and here. Uses the same gen window
        // as the upload decision so the two sums stay comparable.
        const uint64_t postDepthGen = (g277GenRows == 416)
            ? g277GenSumAligned(fbBlock, fbw, 416u)
            : g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                             static_cast<uint32_t>(fbW), kFbH);
        if (postDepthGen != fbGenNow)
            g_g272GenMoves.fetch_add(1u, std::memory_order_relaxed);
        fbGenNow = postDepthGen;
    }
    // Color FBO == VRAM for this fbp now (depth may remain GPU-owned until the next boundary).
    // G261/G272 wave: FBO is AHEAD of VRAM instead of equal — snap.genSum == current VRAM gens
    // still means "do not upload stale VRAM over the FBO"; the residency record carries the
    // dirty-row obligation until a consumer edge materializes it.
    snap.genSum = fbGenNow;
    snap.valid = true;
    if (rowLo < snap.rowLo) snap.rowLo = rowLo;
    if (rowHi > snap.rowHi) snap.rowHi = rowHi;
    if (g277GenMoveOn() && fbp == 0x68u)
    {
        for (int i = 0; i < 13; ++i)
            s_g277Chunk[i] = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u,
                                            static_cast<uint32_t>(i * 32),
                                            static_cast<uint32_t>(fbW), 32u);
        s_g277ChunkValid = true;
    }
    if (g261Wave)
    {
        const int ti = g248TargetIndex(fbp);
        G261Res &r = g_g261Res[ti];
        r.covLo = snap.rowLo;
        r.covHi = snap.rowHi;
        g261UpdateWindow(ti); // re-anchor claimed rows + gen baseline for the new residency state
        g310NoteProducerWrite(
            ti, s_batch.uploadFb ? 0 : rowLo,
            s_batch.uploadFb ? 511 : rowHi);
    }
    if (g276Coalesce)
    {
        // The batch's draws are now in the persistent FBO but its color readback was deferred.
        // Extend the pending union (same target, guaranteed by the different-target flush above).
        s_g276Pend.active = true;
        s_g276Pend.fbp = fbp;
        s_g276Pend.fbw = fbw;
        s_g276Pend.fbBlock = fbBlock;
        s_g276Pend.fbW = fbW;
        if (rowLo < s_g276Pend.rowLo) s_g276Pend.rowLo = rowLo;
        if (rowHi > s_g276Pend.rowHi) s_g276Pend.rowHi = rowHi;
        g455NoteDisplayWrite();
        g_g276Deferred.fetch_add(1u, std::memory_order_relaxed);
    }

    const uint64_t nGpu = g_g178GpuFlushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (s_g323Census)
    {
        static uint64_t s_depsNs = 0u, s_hygieneNs = 0u, s_logicalNs = 0u,
                        s_compositeNs = 0u, s_bindNs = 0u, s_materializeNs = 0u,
                        s_otherNs = 0u;
        static uint64_t s_hygieneCalls = 0u, s_logicalCalls = 0u,
                        s_compositeCalls = 0u, s_bindEntries = 0u,
                        s_materializeCalls = 0u;
        s_depsNs += g323DepsNs;
        s_hygieneNs += g323HygieneNs;
        s_logicalNs += g323LogicalNs;
        s_compositeNs += g323CompositeNs;
        s_bindNs += g323BindNs;
        s_materializeNs += g323MaterializeNs;
        s_otherNs += g323OtherNs;
        s_hygieneCalls += g323HygieneCalls;
        s_logicalCalls += g323LogicalCalls;
        s_compositeCalls += g323CompositeCalls;
        s_bindEntries += g323BindEntries;
        s_materializeCalls += g323MaterializeCalls;
        if ((nGpu % 300u) == 0u)
        {
            const uint64_t sumNs = s_hygieneNs + s_logicalNs + s_compositeNs +
                                   s_bindNs + s_materializeNs + s_otherNs;
            const double denom = 1.0e6 * static_cast<double>(nGpu);
            std::fprintf(
                stderr,
                "[G323:census] gpu=%llu deps=%.3f sum=%.3f deltaNs=%lld "
                "avgMs(hygiene=%.3f logical=%.3f g309=%.3f bind=%.3f mat=%.3f other=%.3f) "
                "calls(hygiene=%llu logical=%llu g309=%llu bindEntries=%llu mat=%llu)\n",
                static_cast<unsigned long long>(nGpu),
                static_cast<double>(s_depsNs) / denom,
                static_cast<double>(sumNs) / denom,
                static_cast<long long>(s_depsNs) - static_cast<long long>(sumNs),
                static_cast<double>(s_hygieneNs) / denom,
                static_cast<double>(s_logicalNs) / denom,
                static_cast<double>(s_compositeNs) / denom,
                static_cast<double>(s_bindNs) / denom,
                static_cast<double>(s_materializeNs) / denom,
                static_cast<double>(s_otherNs) / denom,
                static_cast<unsigned long long>(s_hygieneCalls),
                static_cast<unsigned long long>(s_logicalCalls),
                static_cast<unsigned long long>(s_compositeCalls),
                static_cast<unsigned long long>(s_bindEntries),
                static_cast<unsigned long long>(s_materializeCalls));
        }
    }
    if (g324CensusOn())
    {
        static uint64_t s_desiredCpuNs = 0u, s_logicalCpuNs = 0u,
                        s_logicalBackendWallNs = 0u, s_t8CpuNs = 0u,
                        s_t8BackendWallNs = 0u, s_tailPrepNs = 0u,
                        s_submitNs = 0u;
        static uint64_t s_producerFlushes = 0u, s_logicalJobs = 0u,
                        s_t8Jobs = 0u, s_skipReadback = 0u,
                        s_noGuestDepth = 0u, s_txnEligible = 0u;
        s_desiredCpuNs += g324Diag.desiredCpuNs;
        s_logicalCpuNs += g324Diag.logicalCpuNs;
        s_logicalBackendWallNs += g324Diag.logicalBackendWallNs;
        s_t8CpuNs += g324Diag.t8CpuNs;
        s_t8BackendWallNs += g324Diag.t8BackendWallNs;
        s_tailPrepNs += g324Diag.tailPrepNs;
        s_submitNs += g324Diag.submitNs;
        s_logicalJobs += g324Diag.logicalJobs;
        s_t8Jobs += g324Diag.t8Jobs;
        if (g324Diag.hadProducer)
        {
            ++s_producerFlushes;
            if (s_batch.skipReadback)
                ++s_skipReadback;
            if (!guestDepth)
                ++s_noGuestDepth;
            if (g324TxnEligible)
                ++s_txnEligible;
        }
        if ((nGpu % 300u) == 0u)
        {
            const double denom = 1.0e6 * static_cast<double>(nGpu);
            const uint64_t observerN =
                g_g324ObserverN[0] + g_g324ObserverN[1] + g_g324ObserverN[2];
            const uint64_t observerNs =
                g_g324ObserverNs[0] + g_g324ObserverNs[1] + g_g324ObserverNs[2];
            std::fprintf(
                stderr,
                "[G324:census] gpu=%llu producer=%llu jobs(t8=%llu logical=%llu) "
                "avgMs(desiredCpu=%.3f t8Cpu=%.3f t8Wall=%.3f logicalCpu=%.3f "
                "logicalWall=%.3f tailPrep=%.3f submit=%.3f) "
                "shape(skipRb=%llu noDepth=%llu txnElig=%llu) "
                "observer(n=%llu avgMs=%.3f flush=%llu mat=%llu frame=%llu)\n",
                static_cast<unsigned long long>(nGpu),
                static_cast<unsigned long long>(s_producerFlushes),
                static_cast<unsigned long long>(s_t8Jobs),
                static_cast<unsigned long long>(s_logicalJobs),
                static_cast<double>(s_desiredCpuNs) / denom,
                static_cast<double>(s_t8CpuNs) / denom,
                static_cast<double>(s_t8BackendWallNs) / denom,
                static_cast<double>(s_logicalCpuNs) / denom,
                static_cast<double>(s_logicalBackendWallNs) / denom,
                static_cast<double>(s_tailPrepNs) / denom,
                static_cast<double>(s_submitNs) / denom,
                static_cast<unsigned long long>(s_skipReadback),
                static_cast<unsigned long long>(s_noGuestDepth),
                static_cast<unsigned long long>(s_txnEligible),
                static_cast<unsigned long long>(observerN),
                observerN != 0u
                    ? static_cast<double>(observerNs) / 1.0e6 / observerN : 0.0,
                static_cast<unsigned long long>(g_g324ObserverN[0]),
                static_cast<unsigned long long>(g_g324ObserverN[1]),
                static_cast<unsigned long long>(g_g324ObserverN[2]));
        }
    }
    if (s_g298Profile)
    {
        static uint64_t s_classNs = 0u, s_fbzNs = 0u, s_depsNs = 0u, s_texNs = 0u,
                        s_vertsNs = 0u, s_submitNs = 0u, s_postNs = 0u;
        const auto g298T7 = std::chrono::steady_clock::now();
        const auto ns = [](auto a, auto b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        s_classNs += ns(g298T0, g298T1);
        s_fbzNs += ns(g298T1, g298T2);
        s_depsNs += ns(g298T2, g298T3);
        s_texNs += ns(g298T3, g298T4);
        s_vertsNs += ns(g298T4, g298T5);
        s_submitNs += ns(g298T5, g298T6);
        s_postNs += ns(g298T6, g298T7);
        if ((nGpu % 300u) == 0u)
        {
            const double denom = 1.0e6 * static_cast<double>(nGpu);
            std::fprintf(stderr,
                         "[G298:profile] gpu=%llu avgMs(class=%.3f fbz=%.3f deps=%.3f "
                         "tex=%.3f verts=%.3f submit=%.3f post=%.3f)\n",
                         static_cast<unsigned long long>(nGpu),
                         static_cast<double>(s_classNs) / denom,
                         static_cast<double>(s_fbzNs) / denom,
                         static_cast<double>(s_depsNs) / denom,
                         static_cast<double>(s_texNs) / denom,
                         static_cast<double>(s_vertsNs) / denom,
                         static_cast<double>(s_submitNs) / denom,
                         static_cast<double>(s_postNs) / denom);
        }
    }
    if (s_g523Fit)
    {
        // y = a + b*n over the flushes seen so far, n = this flush's entry count.
        //   b = (N*Sxy - Sx*Sy) / (N*Sxx - Sx*Sx)   marginal ns per entry
        //   a = (Sy - b*Sx) / N                     fixed ns per flush
        // `submit` is additionally fit against DRAW count, because a per-draw cost is conserved
        // under re-batching in exactly the way a per-entry cost is and must not be read as fixed.
        static constexpr int kG523N = 14;
        static const char *const kG523Name[kG523N] = {
            "pro0trn", "pro1mir", "pro2fbw", "pro3set",
            "clsPro", "clsLoop", "clsEpi", "class", "fbz",
            "deps",   "tex",     "verts",  "submit", "post"};
        static double s_g523Sy[kG523N] = {}, s_g523Sxy[kG523N] = {};
        static double s_g523N = 0.0, s_g523Sx = 0.0, s_g523Sxx = 0.0;
        static double s_g523Sd = 0.0, s_g523Sdd = 0.0, s_g523Sdy = 0.0;
        const auto g523T7 = std::chrono::steady_clock::now();
        const auto g523ns = [](auto a, auto b) {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        const double g523Y[kG523N] = {
            g523ns(g298T0, g523P0),   g523ns(g523P0, g523P1),     g523ns(g523P1, g523P2),
            g523ns(g523P2, g523ClsA),
            g523ns(g298T0, g523ClsA), g523ns(g523ClsA, g523ClsB), g523ns(g523ClsB, g298T1),
            g523ns(g298T0, g298T1),   g523ns(g298T1, g298T2),     g523ns(g298T2, g298T3),
            g523ns(g298T3, g298T4),   g523ns(g298T4, g298T5),     g523ns(g298T5, g298T6),
            g523ns(g298T6, g523T7)};
        const double g523Entries = static_cast<double>(g_g144List.size());
        const double g523Draws = static_cast<double>(s_batch.draws.size());
        s_g523N += 1.0;
        s_g523Sx += g523Entries;
        s_g523Sxx += g523Entries * g523Entries;
        s_g523Sd += g523Draws;
        s_g523Sdd += g523Draws * g523Draws;
        static constexpr int kG523Submit = 12; // index of "submit" in kG523Name
        s_g523Sdy += g523Draws * g523Y[kG523Submit];
        for (int i = 0; i < kG523N; ++i)
        {
            s_g523Sy[i] += g523Y[i];
            s_g523Sxy[i] += g523Entries * g523Y[i];
        }
        if ((nGpu % 600u) == 0u)
        {
            const double den = s_g523N * s_g523Sxx - s_g523Sx * s_g523Sx;
            std::fprintf(stderr,
                         "[G523:fit] gpu=%llu flushes=%.0f entries/flush=%.1f draws/flush=%.1f\n",
                         static_cast<unsigned long long>(nGpu), s_g523N,
                         s_g523Sx / s_g523N, s_g523Sd / s_g523N);
            for (int i = 0; i < kG523N; ++i)
            {
                const double b =
                    (den != 0.0) ? (s_g523N * s_g523Sxy[i] - s_g523Sx * s_g523Sy[i]) / den : 0.0;
                const double a = (s_g523Sy[i] - b * s_g523Sx) / s_g523N;
                const double mean = s_g523Sy[i] / s_g523N;
                std::fprintf(stderr,
                             "[G523:fit] %-7s meanMs=%.4f fixedMs=%.4f perEntryNs=%.1f "
                             "fixedShare=%.0f%%\n",
                             kG523Name[i], mean / 1.0e6, a / 1.0e6, b,
                             (mean > 0.0) ? (100.0 * a / mean) : 0.0);
            }
            const double dden = s_g523N * s_g523Sdd - s_g523Sd * s_g523Sd;
            const double bd =
                (dden != 0.0)
                    ? (s_g523N * s_g523Sdy - s_g523Sd * s_g523Sy[kG523Submit]) / dden
                    : 0.0;
            std::fprintf(stderr,
                         "[G523:fit] submit-vs-draws fixedMs=%.4f perDrawNs=%.1f\n",
                         (s_g523Sy[kG523Submit] - bd * s_g523Sd) / s_g523N / 1.0e6, bd);
        }
    }
    if (s_g273Profile)
    {
        static uint64_t s_preNs = 0u, s_submitNs = 0u, s_postNs = 0u;
        static uint64_t s_entries = 0u, s_verts = 0u, s_draws = 0u;
        const auto g273T3 = std::chrono::steady_clock::now();
        s_preNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T1 - g273T0).count());
        s_submitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T2 - g273T1).count());
        s_postNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T3 - g273T2).count());
        s_entries += g_g144List.size();
        s_verts += s_batch.verts.size();
        s_draws += s_batch.draws.size();
        if ((nGpu % 300u) == 0u)
            std::fprintf(stderr,
                         "[G273:profile] gpu=%llu avgMs(pre=%.3f submit=%.3f post=%.3f) avg(entries=%.1f verts=%.1f draws=%.1f)\n",
                         static_cast<unsigned long long>(nGpu),
                         static_cast<double>(s_preNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_submitNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_postNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_entries) / nGpu,
                         static_cast<double>(s_verts) / nGpu,
                         static_cast<double>(s_draws) / nGpu);
    }
    static const bool s_stat = (std::getenv("DC2_G178_STAT") != nullptr);
    if (s_stat && (nGpu % 300u) == 0u)
        std::fprintf(stderr,
            "[G178:stat] flushes=%llu gpu=%llu fallback=%llu texDecodes=%llu texHits=%llu "
            "texHashHits=%llu fbUp=%llu fbSkip=%llu rej(state=%llu blend=%llu tex=%llu) "
            "lastBatch(verts=%zu draws=%zu texUps=%zu rows=%d)\n",
            (unsigned long long)g_g178Flushes.load(std::memory_order_relaxed),
            (unsigned long long)nGpu,
            (unsigned long long)g_g178Fallbacks.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexDecodes.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexHits.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexHashHits.load(std::memory_order_relaxed),
            (unsigned long long)g_g178FbUploads.load(std::memory_order_relaxed),
            (unsigned long long)g_g178FbUploadSkips.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectState.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectBlend.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectTex.load(std::memory_order_relaxed),
            s_batch.verts.size(), s_batch.draws.size(), s_batch.texUploads.size(), rows);
    if (g255Rtt && !guestDepth && g255VerifyOn())
    {
        // Leave the proven CPU replay authoritative in verification mode. The GPU result is kept
        // only as the comparison oracle; invalidate this FBO snapshot so the next GPU submission
        // uploads the CPU result rather than assuming the restored target is still resident.
        g255StageGpuVerify(vram, vramSize, s_batch.readback, kFbH);
        g_g178FbSnap.erase(fbp);
        return false;
    }
    if (g286Transient && !guestDepth && g286VerifyOn())
    {
        // Same-run exact oracle: keep GPU pixels only as the comparison arm, restore the
        // pre-batch target, then let the established CPU band replay remain authoritative.
        g255StageGpuVerify(vram, vramSize, s_batch.readback, kFbH);
        g_g178FbSnap.erase(fbp);
        return false;
    }
    g289OwnerViewGuard.armed = false;
    g289DirectGuard.armed = false;
    return true;
}

// A whole-flush CPU replay bypasses drawPrimitive's normal post-draw notification.  Publish one
// conservative full-target generation bump per distinct color/Z tuple after replay, so persistent
// G242 depth and G178 textures can never reuse bytes the CPU just changed.
static void g242NoteCpuReplayWrites(uint8_t *vram)
{
    if (!g242GpuDepthOn() || vram == nullptr || g_g144List.empty())
        return;
    std::unordered_set<uint64_t> colorTargets;
    std::unordered_set<uint64_t> depthTargets;
    for (const G144Entry &e : g_g144List)
    {
        const uint32_t fbBlock = GSInternal::framePageBaseToBlock(e.ctx.frame.fbp);
        const uint32_t fbw = e.ctx.frame.fbw;
        const uint32_t fpsm = e.ctx.frame.psm;
        const uint64_t colorKey = static_cast<uint64_t>(fbBlock) |
                                  (static_cast<uint64_t>(fbw) << 16u) |
                                  (static_cast<uint64_t>(fpsm) << 24u);
        if (colorTargets.insert(colorKey).second)
            g178NoteVramWriteRect(vram, fbBlock, fbw, fpsm, 0u, 0u,
                                  std::max(1u, fbw) * 64u, 512u);

        const bool zmsk = ((e.ctx.zbuf >> 32u) & 1u) != 0u;
        if (!zmsk)
        {
            const uint32_t zbpBlock = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu) << 5u;
            const uint32_t zpsm = static_cast<uint32_t>((e.ctx.zbuf >> 24u) & 0xFu);
            const uint64_t depthKey = static_cast<uint64_t>(zbpBlock) |
                                      (static_cast<uint64_t>(fbw) << 16u) |
                                      (static_cast<uint64_t>(zpsm) << 24u);
            if (depthTargets.insert(depthKey).second)
                g178NoteVramWriteRect(vram, zbpBlock, fbw, zpsm, 0u, 0u,
                                      std::max(1u, fbw) * 64u, 512u);
        }
    }
}
