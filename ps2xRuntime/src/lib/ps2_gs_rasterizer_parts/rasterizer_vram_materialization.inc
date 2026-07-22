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
            g261Materialize(static_cast<int>(i), cause);
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
        if (dstBase <= dirtyStart || dstBase >= dirtyStart + pageRowBytes ||
            ((dstBase - dirtyStart) % kPageBytes) != 0u || dstEnd < dirtyEnd)
            continue;

        const int originalRows = r.dirtyHi - r.dirtyLo + 1;
        r.dirtyHi = r.dirtyLo + 31;
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
        g_g285L2lRowsPublished.fetch_add(32u, std::memory_order_relaxed);
        g_g285L2lRowsElided.fetch_add(
            static_cast<uint64_t>(originalRows - 32), std::memory_order_relaxed);
        if (s_stat && (n <= 8u || (n % 128u) == 0u))
            std::fprintf(stderr,
                         "[G285:consume] n=%llu fbp=%03x publishRows=32 elidedRows=%d "
                         "dst=0x%x bytes=%llu\n",
                         static_cast<unsigned long long>(n), kG261Fbp[i],
                         originalRows - 32, dbp,
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
        if (g248TargetIndex(c.frame.fbp) < 0)
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

static bool g_g305PendActive = false;               // a packet is in flight + its fallback retained
static std::vector<G144Entry> g_g305PendList;       // retained fallback authority for the in-flight batch
static GS *g_g305PendGs = nullptr;
static bool g_g305ForceSync = false;                // set during failure-replay: suppress re-async + re-drain
static std::atomic<uint64_t> g_g305Submitted{0};    // async packets submitted
static std::atomic<uint64_t> g_g305Drained{0};      // packets drained (== submitted at steady state)
static std::atomic<uint64_t> g_g305Failed{0};       // backend render failures caught at drain (expect 0)

// Drain the in-flight async packet (if any) and resolve the delayed-failure contract. Cheap no-op
// when nothing is pending; safe to call at every consumer edge. Runs only on the front-end thread.
static void g305DrainPending()
{
    if (!g_g305PendActive || g_g305ForceSync)
        return;
    g_g305PendActive = false;
    bool ok = true;
    g178_backend_drain_async(ok);
    const uint64_t nDrained = g_g305Drained.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g305StatOn() && (nDrained % 1000u) == 0u)
        std::fprintf(stderr, "[G305:async] submitted=%llu drained=%llu failed=%llu\n",
                     (unsigned long long)g_g305Submitted.load(std::memory_order_relaxed),
                     (unsigned long long)nDrained,
                     (unsigned long long)g_g305Failed.load(std::memory_order_relaxed));
    if (ok)
    {
        g_g305PendList.clear();
        return;
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
    g_g144List.swap(g_g305PendList);
    g_g144LastGs = g_g305PendGs;
    g_g305ForceSync = true;
    if (g_g144FlushSeqFn)
        g_g144FlushSeqFn();
    g_g305ForceSync = false;
    g_g144List.swap(savedList);
    g_g144LastGs = savedGs;
    g_g305PendList.clear();
    if (g305StatOn())
        std::fprintf(stderr, "[G305:async] backend FAILURE recovered via CPU replay (n=%llu)\n",
                     (unsigned long long)g_g305Failed.load(std::memory_order_relaxed));
}

static bool g178TryFlushGpu(GSRasterizer *self, GS *gs, uint8_t *vram, uint32_t vramSize)
{
    g324NoteObserver(0u); // next native flush is a G310/FBO state observer
    G324FlushDiag g324Diag{};
    const bool g324IsArmed = g324Armed();
    G324DiagScope g324Scope(g324IsArmed ? &g324Diag : nullptr);
    // G305: fence the previous async packet before this flush reads any FBO/snapshot/residency
    // state or reuses s_batch. Catches a (rare) delayed backend failure before the next prep.
    if (g305AsyncOn())
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
    if (s_g289Owner.active && !g289ConsumeOwner)
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
    static const bool s_g298Profile = envFlagEnabled("DC2_G298_PROFILE");
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
        g261PrepareCpuReplay();
    }
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
    for (const G144Entry &e : g_g144List)
    {
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
        if (!g178ClassifyEntry(e, st))
        {
            g_g266FlushRej = kG266FrClassify; // g_g266ClassifySite set inside g262NoteReject
            if (g261Wave)
                g_g261RttClassifyRej.fetch_add(1u, std::memory_order_relaxed);
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear(); // the CPU replay about to run writes VRAM the gens don't track
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
    if (s_g278Pend.active)
    {
        bool mustPublish = s_g278Pend.vram != vram ||
                           s_g278Pend.vramSize != vramSize;
        // G290: the seeded batch is owner-INELIGIBLE (its color write range physically overlaps
        // its Z read range), so it must never fall through to a full VRAM Z upload while a
        // same-key pending owner holds newer backend rows. A pending union confined to rows
        // 0..415 is safe to CONTINUE (the seeded rows 416..447 are re-established by the partial
        // upload below and row 416+ is never in the union); any wider pending union publishes.
        if (g290Seeded && s_g278Pend.rowHi > 415)
            mustPublish = true;
        if (!mustPublish && guestDepth)
        {
            const uint64_t nextKey = g242DepthKey(guestZbpPage, guestZpsm, fbw);
            mustPublish = g242GpuDepthOn() || nextKey != s_g278Pend.key;
            if (!mustPublish)
            {
                const int prospectiveLo = std::min(s_g278Pend.rowLo, depthRowLo);
                const int prospectiveHi = std::max(s_g278Pend.rowHi, depthRowHi);
                mustPublish = g278StaleRowsOverlap(prospectiveLo, prospectiveHi);
            }
        }
        const G144BlockRange dirty = g278PendingDepthRange();
        const G144BlockRange whole = g278PendingDepthWholeRange();
        if (!mustPublish && (!dirty.valid || !whole.valid))
            mustPublish = true;
        if (!mustPublish)
        {
            const G144BlockRange target = g144RangeForRect(
                fbBlock, GS_PSM_CT32, fbw, 0u,
                static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(rowHi - rowLo + 1));
            mustPublish = !target.valid || g144RangeOverlaps(dirty, target);
        }
        if (!mustPublish)
        {
            for (size_t i = 0; i < g_g144List.size(); ++i)
            {
                const G144Entry &e = g_g144List[i];
                if (s_states[i].skip || !e.prim.tme)
                    continue;
                const G144BlockRange tex = g144TextureRange(e);
                if (!tex.valid || g144RangeOverlaps(dirty, tex))
                {
                    mustPublish = true;
                    break;
                }
                if (g144IsPalettedPsm(e.ctx.tex0.psm))
                {
                    const G144BlockRange clut = g144ClutRange(e);
                    if (!clut.valid || g144RangeOverlaps(dirty, clut))
                    {
                        mustPublish = true;
                        break;
                    }
                }
            }
        }
        if (!mustPublish && s_g276Pend.active)
        {
            const G144BlockRange display = g277PendingDisplayRange();
            mustPublish = !display.valid || g144RangeOverlaps(whole, display);
        }
        if (mustPublish && !g278FlushPendingDepth(0))
        {
            g_g266FlushRej = kG266FrZReadback;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
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
    static G178Batch s_batch; // reused across flushes (keeps vector capacity)
    s_batch.fbp = fbp;
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
    G178FbSnap &snap = g_g178FbSnap[fbp];
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
        s_batch.fbPixels.assign(static_cast<size_t>(fbW) * kFbH, 0u);
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
        const auto packFbRows = [&](int laneLo, int laneHi) {
            for (int y = laneLo; y <= laneHi; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, fbw, static_cast<uint32_t>(x),
                        static_cast<uint32_t>(y));
                    uint32_t px = 0;
                    if (off + 4u <= vramSize)
                        std::memcpy(&px, vram + off, 4);
                    s_batch.fbPixels[dstRow + x] = px;
                }
            }
        };
        const int packRows = upHi >= upLoEff ? (upHi - upLoEff + 1) : 0;
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
        const int packLanes =
            s_g298ParFbPack && packRows >= 8 ? s_g298FbPackLanes : 1;
        GSRowPool::instance().run(upLoEff, upHi, packLanes, packFbRows);

        // Exact same-run oracle for bring-up: independently reread every packed CT32 pixel using
        // the serial address path after the joined dispatch. This is diagnostic-only because it
        // deliberately repeats the staging work.
        static const bool s_g298Verify =
            envFlagEnabled("DC2_G298_VERIFY_FBPACK");
        if (s_g298ParFbPack && s_g298Verify && packRows > 0)
        {
            uint64_t bad = 0u;
            for (int y = upLoEff; y <= upHi; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
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
                             "last(rows=%d lanes=%d bad=%llu)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(totalPixels),
                             static_cast<unsigned long long>(totalBad),
                             packRows, packLanes,
                             static_cast<unsigned long long>(bad));
        }
        snap.rowLo = upLo;
        snap.rowHi = upHi;
    }
    else
    {
        g_g178FbUploadSkips.fetch_add(1u, std::memory_order_relaxed);
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
    bool g278PendingSameKey = false;
    bool g278OwnerEligible = false;
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
        }
        g278PendingSameKey =
            g278DepthRbOn() && g278OwnerEligible && s_g278Pend.active &&
            s_g278Pend.key == depthKey && s_g278Pend.vram == vram &&
            s_g278Pend.vramSize == vramSize;
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
            if (g274ZTimeOn() || g274ZResidentOn())
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
                if (g278PendingSameKey)
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
                    if (g290Seeded)
                        g290PartialSeed = true;
                }
                else if (g274ZResidentOn() && !fullInit && unchanged && !g282Tight &&
                         !g290Seeded)
                    g274SkipUpload = true;
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
                const int upLoZ = fullInit ? 0 : depthRowLo;
                const int upHiZ = fullInit ? (kFbH - 1) : depthRowHi;
                depthRows = upHiZ - upLoZ + 1;
                depthGlY = kFbH - 1 - upHiZ;
                s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
                constexpr float kZToDepth = 1.0f / 4294967296.0f;
                const auto g274ZUpT0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                const auto packDepthRows = [&](int laneLo, int laneHi) {
                    for (int y = laneLo; y <= laneHi; ++y)
                    {
                        const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                        for (int x = 0; x < fbW; ++x)
                        {
                            const uint32_t z = GSMem::ReadZ24(
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
                if (g282Tight || g290Seeded)
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
            if (g267CensusOn() && g265DisplayTarget(fbp) &&
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
                                         "wrap=%u%u tex=%dx%d\n",
                                         fbp, kG261Fbp[t], e.ctx.tex0.psm, e.ctx.tex0.tbp0,
                                         e.ctx.tex0.tbw, r.fbw, e.prim.fst ? 1u : 0u,
                                         st.bilinear ? 1u : 0u,
                                         static_cast<unsigned>(e.prim.type), r.covLo, r.covHi,
                                         static_cast<unsigned>(st.wrapU),
                                         static_cast<unsigned>(st.wrapV), st.texW, st.texH);
                        g_g261FboRejects.fetch_add(1u, std::memory_order_relaxed);
                        g266NoteTexBindMat(fbp, t, e, st);
                        const auto g323MatT0 = g323Armed ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
                        g261Materialize(static_cast<int>(t), kG261MatTexAlias);
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
    for (size_t i = 0; i < g_g144List.size(); ++i)
    {
        const G178EntryState &st = s_states[i];
        if (g261HaveFboSrc && s_g261FboSrc[i] != 0u)
            continue; // G261: sampled straight from the producer's FBO — no VRAM decode
        const uint64_t effectiveKey =
            (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                ? s_g281ViewKey[i] : st.texKey;
        if (effectiveKey == 0u || st.skip || s_batchKeys.count(effectiveKey))
            continue;
        s_batchKeys.insert(effectiveKey);
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
        const uint64_t genNow =
            g178GenSumRect(ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0, 0,
                           static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH)) +
            g178GenSumRect(ctx.tex0.cbp, 1, GS_PSM_CT32, 0, 0, 64, 32); // CLUT page(s)
        auto it = g_g178TexReg.find(st.texKey);
        const bool backendHas = g178_backend_has_tex(st.texKey);
        if (it != g_g178TexReg.end() && backendHas && it->second.genSum == genNow)
        {
            g_g178TexHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        // Gen mismatch (or unknown): DC2 re-uploads texture data every frame, usually byte-
        // identical — hash the source pages before paying the swizzle+CLUT decode. Hash-equal +
        // still resident → refresh the gen stamp and keep the GPU copy.
        const uint64_t hashNow = g178HashPages(vram, vramSize, ctx.tex0.tbp0, ctx.tex0.tbw,
                                               ctx.tex0.psm, static_cast<uint32_t>(st.texW),
                                               static_cast<uint32_t>(st.texH), ctx.tex0.cbp);
        if (it != g_g178TexReg.end() && backendHas && it->second.contentHash == hashNow)
        {
            it->second.genSum = genNow;
            g_g178TexHashHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        G178TexUpload up;
        up.key = st.texKey;
        up.w = st.texW;
        up.h = st.texH;
        up.px.resize(static_cast<size_t>(st.texW) * st.texH);
        g178DecodeWhole(self, gs, vram, ctx, e.texa, e.texclut, st.texW, st.texH, up.px.data());
        s_batch.texUploads.push_back(std::move(up));
        g_g178TexReg[st.texKey] = G178TexReg{genNow, hashNow, st.texW, st.texH};
        g_g178TexDecodes.fetch_add(1u, std::memory_order_relaxed);
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
            if (g255Rtt || g286Transient)
            {
                // drawSprite first truncates screen positions, sorts the bounds, and expands a
                // zero span to one pixel.  The RTT effects deliberately submit full rectangles
                // plus degenerate edge/corner sprites; raw GL triangles silently drop the latter.
                int sx0 = static_cast<int>(a.x) - ofx;
                int sy0 = static_cast<int>(a.y) - ofy;
                int sx1 = static_cast<int>(b.x) - ofx;
                int sy1 = static_cast<int>(b.y) - ofy;
                if (sx0 > sx1) std::swap(sx0, sx1);
                if (sy0 > sy1) std::swap(sy0, sy1);
                q0.x = static_cast<float>(sx0 + ofx);
                q0.y = static_cast<float>(sy0 + ofy);
                q1.x = static_cast<float>(sx0 + std::max(1, sx1 - sx0) + ofx);
                q1.y = static_cast<float>(sy0 + std::max(1, sy1 - sy0) + ofy);
                if (e.prim.fst)
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
            float uvShiftU = 0.0f, uvShiftV = 0.0f;
            if (e.prim.tme && e.prim.fst)
            {
                const float dxp = q1.x - q0.x; // positions are already pixel-unit floats (kick /16)
                const float dyp = q1.y - q0.y;
                if (dxp != 0.0f)
                    uvShiftU = -0.5f * (static_cast<float>(q1.u) - static_cast<float>(q0.u)) / 16.0f / dxp;
                if (dyp != 0.0f)
                    uvShiftV = -0.5f * (static_cast<float>(q1.v) - static_cast<float>(q0.v)) / 16.0f / dyp;
            }
            // Two triangles covering the rect; UVs pair with the ORIGINAL corners (axis-aligned).
            GSVertex c00 = q0, c11 = q1, c10 = q0, c01 = q0;
            c10.x = q1.x; c10.u = q1.u; // (x1, y0)
            c01.y = q1.y; c01.v = q1.v; // (x0, y1)
            // The GS uses the second kick's Z as one flat value for a sprite. Interpolating the
            // two endpoint Z values would turn a clear sprite into a depth ramp.
            c00.z = c10.z = c11.z = c01.z = b.z;
            const bool tme = e.prim.tme, fst = e.prim.fst;
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c10, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c01, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
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
            d.depthFunc = st.depthFunc;
            d.depthWrite = st.depthWrite;
            d.bilinear = st.bilinear;
            d.wrapU = st.wrapU;
            d.wrapV = st.wrapV;
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
    const bool g278DeferDepth =
        g278DepthRbOn() && g262WaveDepth && g278OwnerEligible && guestDepthWrites &&
        !g275FusedZReadOn();
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
    const bool g305Async =
        g305AsyncOn() && !g_g305ForceSync && s_batch.skipReadback && !guestDepth &&
        !g275FuseReadback && !g262WaveDepth && !g286Transient && !g282Tight &&
        !(g255Rtt && g255VerifyOn()) && !g310On();
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
    bool submitted;
    if (g305Async)
    {
        submitted = g178_backend_submit_async(s_batch);
        if (submitted)
        {
            // Retain fallback authority for the drain-time failure check; leave g_g144List empty so
            // the closure's post-success clear() is a no-op and the next flush accumulates fresh.
            g_g305PendGs = gs;
            g_g305PendList.clear();
            g_g305PendList.swap(g_g144List);
            g_g305PendActive = true;
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
        g178BumpRectImpl(zbpBlock, fbw, GS_PSM_Z24, 0u,
                         static_cast<uint32_t>(depthRowLo), static_cast<uint32_t>(fbW),
                         static_cast<uint32_t>(rbRows));
        g149BumpVramGen();
        if (g274ZTimeOn())
            g_g274ZRbNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - g274ZRbT0).count(),
                                  std::memory_order_relaxed);
        if (g274ZTimeOn() || g274ZResidentOn())
            // Baseline the whole-target Z gen AFTER our own writeback bump so the next same-key
            // upload can tell whether any OTHER writer touched VRAM Z in between. This readback
            // just synced GPU->VRAM, so the resident texture now equals VRAM at this gen.
            g_g274LastZGen[depthKey] = g178GenSumRect(zbpBlock, fbw, GS_PSM_Z24, 0u, 0u,
                                                      static_cast<uint32_t>(fbW), 512u);
        g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
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
        const bool continuing = s_g278Pend.active;
        if (continuing &&
            (s_g278Pend.key != depthKey || s_g278Pend.vram != vram ||
             s_g278Pend.vramSize != vramSize))
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
        if (!s_g278Pend.active)
        {
            s_g278Pend.active = true;
            s_g278Pend.key = depthKey;
            s_g278Pend.vram = vram;
            s_g278Pend.vramSize = vramSize;
            s_g278Pend.zbpPage = guestZbpPage;
            s_g278Pend.fbw = fbw;
            s_g278Pend.fbW = fbW;
            s_g278Pend.rowLo = depthRowLo;
            s_g278Pend.rowHi = depthRowHi;
        }
        else
        {
            s_g278Pend.rowLo = std::min(s_g278Pend.rowLo, depthRowLo);
            s_g278Pend.rowHi = std::max(s_g278Pend.rowHi, depthRowHi);
            g_g278Saved.fetch_add(1u, std::memory_order_relaxed);
        }
        s_g278Pend.display =
            s_g278Pend.display ||
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
        for (int y = rowLo; y <= rowHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                            static_cast<uint32_t>(x),
                                                            static_cast<uint32_t>(y));
                if (off + 4u <= vramSize)
                    std::memcpy(vram + off, &s_batch.readback[srcRow + x], 4);
            }
        }
        // The color and depth backends are distinct even when their GS storage aliases. The
        // admitted write windows are disjoint, so this cannot overwrite pending dirty Z rows;
        // remember any other affected Z rows so a later same-key wave cannot use stale depth.
        if (s_g278Pend.active && g278ColorWrite.valid)
            g278MarkStaleRowsForRange(g278ColorWrite);
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
                const uint64_t version = s_g289Owner.version;
                s_g289Owner.active = false;
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
