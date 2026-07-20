static uint64_t g281ViewKey(uint64_t base)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(base);
    mix(0x4732383154385649ull); // "G281T8VI": distinct from guest-VRAM decoded textures
    return h | (1ull << 63);
}

// Exact physical-page ownership test shared by the G281 read view and G282 write fanout.
// G261's range is an interval around a row window and can include layout holes; map the page
// through the target's real CT32 page grid before calling it resident-owned.
static bool g280DirtyTargetOwnsPage(int ti, uint32_t pageBlock,
                                    uint32_t *pageRow = nullptr,
                                    uint32_t *pageCol = nullptr)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return false;
    const G261Res &r = g_g261Res[ti];
    if (!r.dirty || r.fbw == 0u || r.fbW <= 0 ||
        r.dirtyHi < r.dirtyLo || !r.range.valid)
        return false;
    const uint32_t base = framePageBaseToBlock(kG261Fbp[ti]);
    if (pageBlock < base || ((pageBlock - base) & 31u) != 0u)
        return false;
    const uint32_t off = (pageBlock - base) >> 5u;
    const uint32_t row = off / r.fbw;
    const uint32_t col = off % r.fbw;
    const int y0 = static_cast<int>(row) * 32;
    if (static_cast<int>(col + 1u) * 64 > r.fbW || y0 >= 512 ||
        y0 + 31 < r.dirtyLo || y0 > r.dirtyHi)
        return false;
    const G144BlockRange page{pageBlock, pageBlock + 31u, true};
    if (!g144RangeOverlaps(page, r.range))
        return false;
    if (pageRow) *pageRow = row;
    if (pageCol) *pageCol = col;
    return true;
}

struct G309PageSnapshot
{
    uint32_t pageBlock = 0u;
    int32_t sourceTi = -1;
    uint64_t sourceGen = 0u;
    bool gpuSource = false;
};

// Snapshot the complete measured strip. The final legacy guest bytes are NOT simply G294's most
// recent writer: g261MaterializeAllDirty publishes targets in ascending target order, so the last
// dirty alias covering a physical page is the downstream presentation winner. Select that same
// winner without publishing any of them. Clean pages map to the already-authoritative guest bytes.
// Fail closed when the selected FBO would first need an overlay restore/mirror patch, or a dirty
// window cuts through a page; those need a separate composition mechanism.
static bool g309BuildPageMap(std::vector<uint32_t> &pageMap,
                             std::array<G309PageSnapshot, 128> &snapshot)
{
    if (!g309On())
        return false;
    pageMap.resize(128u);
    uint32_t sourceMask = 0u;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty)
            continue;
        if (r.mirPending || !g_g280Ovl[t].empty() ||
            r.dirtyLo < 0 || r.dirtyHi < r.dirtyLo ||
            (r.dirtyLo & 31) != 0 || ((r.dirtyHi + 1) & 31) != 0)
        {
            static std::atomic<uint32_t> s_stateLog{0};
            if (s_stateLog.fetch_add(1u, std::memory_order_relaxed) < 8u)
                std::fprintf(stderr,
                             "[G309:map-reject] stage=state ti=%u mirror=%d ovl=%zu "
                             "rows=%d..%d\n",
                             t, r.mirPending ? 1 : 0, g_g280Ovl[t].size(),
                             r.dirtyLo, r.dirtyHi);
            g_g309FailLayout.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    }
    for (uint32_t yp = 0u; yp < 16u; ++yp)
        for (uint32_t xp = 0u; xp < 8u; ++xp)
        {
            const size_t index = static_cast<size_t>(yp) * 8u + xp;
            const uint32_t pageBlock = 0x2720u + static_cast<uint32_t>(index) * 32u;
            int sourceTi = -1;
            uint32_t srcRow = 0u, srcCol = 0u;
            // Deliberately do not break: the highest target index is the final legacy publisher.
            for (uint32_t t = 0u; t < kG248TargetCount; ++t)
            {
                uint32_t row = 0u, col = 0u;
                if (g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock, &row, &col))
                {
                    sourceTi = static_cast<int>(t);
                    srcRow = row;
                    srcCol = col;
                }
            }
            const bool gpuSource = sourceTi >= 0;
            if (gpuSource && (srcRow >= 16u || srcCol >= 8u))
            {
                static std::atomic<uint32_t> s_badCoordLog{0};
                if (s_badCoordLog.fetch_add(1u, std::memory_order_relaxed) < 8u)
                    std::fprintf(stderr,
                                 "[G309:map-reject] stage=coord page=0x%x owner=%d "
                                 "row=%u col=%u\n",
                                 pageBlock, sourceTi, srcRow, srcCol);
                g_g309FailLayout.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            if (gpuSource)
                sourceMask |= 1u << static_cast<uint32_t>(sourceTi);
            snapshot[index] = G309PageSnapshot{
                pageBlock, sourceTi,
                gpuSource ? g_g280ContentGen[static_cast<uint32_t>(sourceTi)] : 0u,
                gpuSource};
            pageMap[index] = gpuSource
                ? (static_cast<uint32_t>(sourceTi + 1) |
                   (srcCol << 8u) | (srcRow << 16u) | 0xFF000000u)
                : 0xFF000000u; // source byte 0 selects the decoded guest-VRAM base
        }
    if (sourceMask == 0u)
    {
        static std::atomic<uint32_t> s_maskLog{0};
        if (s_maskLog.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr,
                         "[G309:map-reject] stage=mask sources=0x%x\n", sourceMask);
        g_g309FailLayout.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return true;
}

static bool g309SnapshotCurrent(const std::array<G309PageSnapshot, 128> &snapshot)
{
    for (const G309PageSnapshot &s : snapshot)
    {
        int sourceTi = -1;
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
            if (g280DirtyTargetOwnsPage(static_cast<int>(t), s.pageBlock))
                sourceTi = static_cast<int>(t);
        if ((sourceTi >= 0) != s.gpuSource || sourceTi != s.sourceTi)
            return false;
        if (sourceTi >= 0 &&
            (g_g280ContentGen[static_cast<uint32_t>(sourceTi)] != s.sourceGen ||
             g_g261Res[static_cast<uint32_t>(sourceTi)].mirPending ||
             !g_g280Ovl[static_cast<uint32_t>(sourceTi)].empty()))
            return false;
    }
    return true;
}

// Verifier-only: publish the five dirty targets through the exact legacy materialization
// machinery, then compare every texel of the transient GPU composite to the now-authoritative
// 0x2720/tbw=8 guest-VRAM strip. The verified flush deliberately does NOT consume the transient
// texture; normal decode/composition continues from the materialized bytes.
static void g309VerifyAgainstLegacy(const std::vector<uint32_t> &composite)
{
    if (composite.size() != 512u * 512u)
    {
        g_g309VerifyBad.fetch_add(512u * 512u, std::memory_order_relaxed);
        return;
    }
    const uint64_t priorVerify =
        g_g309VerifyComposites.load(std::memory_order_relaxed);
    if (priorVerify < 2u)
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            const G261Res &r = g_g261Res[t];
            std::fprintf(stderr,
                         "[G309:vfy-state] n=%llu ti=%u fbp=%03x dirty=%d "
                         "rows=%d..%d cov=%d..%d fbw=%u ovl=%zu\n",
                         static_cast<unsigned long long>(priorVerify + 1u), t,
                         kG261Fbp[t], r.dirty ? 1 : 0, r.dirtyLo, r.dirtyHi,
                         r.covLo, r.covHi, r.fbw, g_g280Ovl[t].size());
        }
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty)
            g261Materialize(static_cast<int>(t), kG261MatTexAlias);

    uint64_t bad = 0u;
    std::array<uint32_t, 128> pageBad{};
    std::array<uint32_t, 128> firstGot{};
    std::array<uint32_t, 128> firstExpected{};
    uint8_t *vram = g_g144LastVram;
    if (vram == nullptr || g_g261VramSize < 4u * 1024u * 1024u)
        bad = 512u * 512u;
    else
        for (uint32_t glY = 0u; glY < 512u; ++glY)
        {
            const uint32_t gsY = 511u - glY;
            for (uint32_t x = 0u; x < 512u; ++x)
            {
                const uint32_t expected =
                    GSMem::ReadCT32(vram, 0x2720u, 8u, x, gsY);
                const uint32_t got =
                    composite[static_cast<size_t>(glY) * 512u + x];
                if (got != expected)
                {
                    const size_t page =
                        static_cast<size_t>(gsY >> 5u) * 8u + (x >> 6u);
                    if (pageBad[page]++ == 0u)
                    {
                        firstGot[page] = got;
                        firstExpected[page] = expected;
                    }
                    ++bad;
                }
            }
        }
    const uint64_t n =
        g_g309VerifyComposites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g309VerifyPixels.fetch_add(512u * 512u, std::memory_order_relaxed);
    const uint64_t totalBad =
        g_g309VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || n <= 8u || (n % 128u) == 0u)
        std::fprintf(stderr,
                     "[G309:vfy] n=%llu pixels=262144 bad=%llu totalBad=%llu\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(bad),
                     static_cast<unsigned long long>(totalBad));
    if (bad != 0u && n <= 2u)
        for (size_t page = 0u; page < pageBad.size(); ++page)
            if (pageBad[page] != 0u)
                std::fprintf(stderr,
                             "[G309:vfy-page] n=%llu page=0x%x py=%zu px=%zu "
                             "bad=%u first=%08x/%08x\n",
                             static_cast<unsigned long long>(n),
                             0x2720u + static_cast<uint32_t>(page) * 32u,
                             page / 8u, page % 8u, pageBad[page],
                             firstGot[page], firstExpected[page]);
}

struct G310DesiredPage
{
    int8_t sourceTi = -1;
    uint8_t sourceRow = 0u;
    uint8_t sourceCol = 0u;
    uint64_t sourceGen = 0u;
};

static bool g310GuestPageMeta(size_t pageIndex, uint64_t &gen, uint64_t &hash)
{
    if (pageIndex >= 128u || g_g144LastVram == nullptr)
        return false;
    const uint32_t pageBlock =
        0x2720u + static_cast<uint32_t>(pageIndex) * 32u;
    const size_t byteOffset = static_cast<size_t>(pageBlock) << 8u;
    if (byteOffset > g_g261VramSize ||
        8192u > static_cast<size_t>(g_g261VramSize) - byteOffset)
        return false;
    gen = g_g178PageGen[pageBlock >> 5u].load(std::memory_order_relaxed);
    hash = 1469598103934665603ull;
    const uint8_t *const bytes = g_g144LastVram + byteOffset;
    for (size_t i = 0u; i < 8192u; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return true;
}

static bool g310BuildDesired(std::array<G310DesiredPage, 128> &desired,
                             int &seedTi)
{
    seedTi = -1;
    if (!g310On() && !g310StatOn())
        return false;
    if (g_g144LastVram == nullptr ||
        g_g261VramSize < 4u * 1024u * 1024u)
    {
        g_g310FailMissing.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty)
            continue;
        if (seedTi < 0)
            seedTi = static_cast<int>(t);
        if (r.mirPending || !g_g280Ovl[t].empty() || !r.range.valid ||
            r.fbw == 0u || r.fbw > 8u ||
            r.fbW != static_cast<int>(r.fbw) * 64 ||
            r.dirtyLo < 0 || r.dirtyHi < r.dirtyLo ||
            (r.dirtyLo & 31) != 0 || ((r.dirtyHi + 1) & 31) != 0)
        {
            static std::atomic<uint32_t> s_log{0};
            if (s_log.fetch_add(1u, std::memory_order_relaxed) < 12u)
                std::fprintf(
                    stderr,
                    "[G310:reject] stage=layout ti=%u fbp=%03x dirty=%d "
                    "rows=%d..%d fbw=%u fbW=%d range=%d mirror=%d ovl=%zu\n",
                    t, kG261Fbp[t], r.dirty ? 1 : 0, r.dirtyLo, r.dirtyHi,
                    r.fbw, r.fbW, r.range.valid ? 1 : 0,
                    r.mirPending ? 1 : 0, g_g280Ovl[t].size());
            g_g310FailLayout.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    }

    for (size_t page = 0u; page < desired.size(); ++page)
    {
        const uint32_t pageBlock =
            0x2720u + static_cast<uint32_t>(page) * 32u;
        G310DesiredPage d{};
        // Ascending legacy publication makes the highest still-dirty target the final winner.
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            uint32_t row = 0u, col = 0u;
            if (g280DirtyTargetOwnsPage(
                    static_cast<int>(t), pageBlock, &row, &col))
            {
                if (row >= 16u || col >= 8u)
                {
                    g_g310FailLayout.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                d.sourceTi = static_cast<int8_t>(t);
                d.sourceRow = static_cast<uint8_t>(row);
                d.sourceCol = static_cast<uint8_t>(col);
                d.sourceGen = g_g310TargetPageGen[t][page];
            }
        }
        desired[page] = d;
        if (g310StatOn())
            g_g310SourcePages[
                d.sourceTi >= 0 ? static_cast<uint32_t>(d.sourceTi)
                                : kG248TargetCount]
                .fetch_add(1u, std::memory_order_relaxed);
    }
    return true;
}

static bool g310DecodeGuestRun(uint32_t pageY, uint32_t pageX0,
                               uint32_t pageCount,
                               std::vector<uint32_t> &pixels)
{
    if (pageY >= 16u || pageX0 >= 8u || pageCount == 0u ||
        pageCount > 8u - pageX0 || g_g144LastVram == nullptr)
        return false;
    const uint32_t width = pageCount * 64u;
    pixels.resize(static_cast<size_t>(width) * 32u);
    for (uint32_t localY = 0u; localY < 32u; ++localY)
    {
        const uint32_t gsY = pageY * 32u + localY;
        const size_t dstRow = static_cast<size_t>(31u - localY) * width;
        for (uint32_t localX = 0u; localX < width; ++localX)
        {
            const uint32_t x = pageX0 * 64u + localX;
            pixels[dstRow + localX] =
                GSMem::ReadCT32(g_g144LastVram, 0x2720u, 8u, x, gsY);
        }
    }
    return true;
}

// Count how many of the 128 logical pages differ between two committed page states. A page differs
// when its winning source target changes, when the resident source's generation changes, or when a
// guest-owned page's content hash changes. This is exactly the set the incremental path re-copies,
// so the census histogram of this count is the deletion ceiling of the full-composite cost.
static uint32_t g311DeltaCount(const std::array<G310PageState, 128> &next,
                               const std::array<G310PageState, 128> &prev)
{
    uint32_t changed = 0u;
    for (size_t p = 0u; p < 128u; ++p)
    {
        const G310PageState &n = next[p];
        const G310PageState &o = prev[p];
        const bool same =
            n.sourceTi == o.sourceTi &&
            (n.sourceTi >= 0 ? n.sourceGen == o.sourceGen
                             : n.guestHash == o.guestHash);
        if (!same)
            ++changed;
    }
    return changed;
}

static void g311RecordDelta(uint32_t changed)
{
    size_t bucket;
    if (changed == 0u) bucket = 0u;
    else if (changed <= 4u) bucket = 1u;
    else if (changed <= 16u) bucket = 2u;
    else if (changed <= 32u) bucket = 3u;
    else if (changed <= 64u) bucket = 4u;
    else if (changed <= 96u) bucket = 5u;
    else if (changed <= 127u) bucket = 6u;
    else bucket = 7u;
    g_g311DeltaHist[bucket].fetch_add(1u, std::memory_order_relaxed);
}

static bool g310ApplyDesired(
    const std::array<G310DesiredPage, 128> &desired)
{
    if (!g_g310Initialized || g_g310Frozen)
        return false;
    // Only measure/account the incremental path when it is the active mechanism; a G311-off
    // no-change ApplyDesired must not pollute the census run's timers/histograms.
    const bool census = g311CensusOn() && g311On();
    const auto t0 = census ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    std::array<G310PageState, 128> next = g_g310Pages;
    std::array<bool, 128> guestWrite{};
    std::array<bool, 128> gpuWrite{};
    uint64_t moves = 0u, noops = 0u;

    for (size_t page = 0u; page < desired.size(); ++page)
    {
        const G310DesiredPage &d = desired[page];
        const G310PageState &old = g_g310Pages[page];
        G310PageState state{};
        state.sourceTi = d.sourceTi;
        if (d.sourceTi >= 0)
        {
            state.sourceGen = d.sourceGen;
            gpuWrite[page] =
                old.sourceTi != d.sourceTi || old.sourceGen != d.sourceGen;
            if (!gpuWrite[page])
                ++noops;
        }
        else
        {
            const uint32_t pageBlock =
                0x2720u + static_cast<uint32_t>(page) * 32u;
            const uint64_t guestGen =
                g_g178PageGen[pageBlock >> 5u].load(std::memory_order_relaxed);
            if (old.sourceTi < 0 && old.guestGen == guestGen)
            {
                state = old;
                ++noops;
                next[page] = state;
                continue;
            }
            if (!g310GuestPageMeta(page, state.guestGen, state.guestHash))
            {
                g_g310FailMissing.fetch_add(1u, std::memory_order_relaxed);
                g_g310Initialized = false;
                return false;
            }
            const bool sameGuest =
                old.sourceTi < 0 && old.guestHash == state.guestHash;
            guestWrite[page] = !sameGuest;
            if (sameGuest)
                ++noops;
        }
        if (old.sourceTi != state.sourceTi)
            ++moves;
        next[page] = state;
    }

    static std::vector<uint32_t> s_guestPixels;
    for (uint32_t py = 0u; py < 16u; ++py)
    {
        uint32_t px = 0u;
        while (px < 8u)
        {
            while (px < 8u &&
                   !guestWrite[static_cast<size_t>(py) * 8u + px])
                ++px;
            const uint32_t runLo = px;
            while (px < 8u &&
                   guestWrite[static_cast<size_t>(py) * 8u + px])
                ++px;
            if (runLo == px)
                continue;
            const uint32_t pages = px - runLo;
            if (!g310DecodeGuestRun(py, runLo, pages, s_guestPixels) ||
                !g264_backend_write_color_rect(
                    kG310LogicalFbp, 512, 512,
                    static_cast<int>(runLo * 64u),
                    static_cast<int>(512u - (py + 1u) * 32u),
                    static_cast<int>(pages * 64u), 32, s_guestPixels))
            {
                g310Invalidate("guest-upload", true);
                return false;
            }
            g_g310GuestJobs.fetch_add(1u, std::memory_order_relaxed);
            g_g310GuestPages.fetch_add(pages, std::memory_order_relaxed);
        }
    }

    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        static std::vector<int32_t> s_rects;
        s_rects.clear();
        uint64_t copiedPages = 0u;
        for (uint32_t py = 0u; py < 16u; ++py)
        {
            uint32_t px = 0u;
            while (px < 8u)
            {
                const size_t index = static_cast<size_t>(py) * 8u + px;
                if (!gpuWrite[index] ||
                    desired[index].sourceTi != static_cast<int8_t>(t))
                {
                    ++px;
                    continue;
                }
                const uint32_t runLo = px;
                const uint32_t srcRow = desired[index].sourceRow;
                const uint32_t srcCol = desired[index].sourceCol;
                ++px;
                while (px < 8u)
                {
                    const size_t nextIndex = static_cast<size_t>(py) * 8u + px;
                    const G310DesiredPage &n = desired[nextIndex];
                    if (!gpuWrite[nextIndex] ||
                        n.sourceTi != static_cast<int8_t>(t) ||
                        n.sourceRow != srcRow ||
                        n.sourceCol != srcCol + (px - runLo))
                        break;
                    ++px;
                }
                const uint32_t pages = px - runLo;
                s_rects.push_back(static_cast<int32_t>(srcCol * 64u));
                s_rects.push_back(static_cast<int32_t>(
                    512u - (srcRow + 1u) * 32u));
                s_rects.push_back(static_cast<int32_t>(runLo * 64u));
                s_rects.push_back(static_cast<int32_t>(
                    512u - (py + 1u) * 32u));
                s_rects.push_back(static_cast<int32_t>(pages * 64u));
                s_rects.push_back(32);
                copiedPages += pages;
            }
        }
        if (s_rects.empty())
            continue;
        if (!g310_backend_copy_logical_pages(kG261Fbp[t], s_rects))
        {
            g310Invalidate("gpu-copy", true);
            return false;
        }
        g_g310GpuJobs.fetch_add(1u, std::memory_order_relaxed);
        g_g310GpuPages.fetch_add(copiedPages, std::memory_order_relaxed);
    }

    g_g310Pages = next;
    g_g310AuthorityMoves.fetch_add(moves, std::memory_order_relaxed);
    g_g310NoopPages.fetch_add(noops, std::memory_order_relaxed);
    g_g310Refresh.fetch_add(1u, std::memory_order_relaxed);
    g_g310Stale = false;
    const uint64_t work = noops <= 128u ? 128u - noops : 0u;
    if (g311On())
    {
        g_g311Refreshes.fetch_add(1u, std::memory_order_relaxed);
        g_g311RefreshWork.fetch_add(work, std::memory_order_relaxed);
        if (work == 0u)
            g_g311RefreshNoop.fetch_add(1u, std::memory_order_relaxed);
    }
    if (census)
    {
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        g_g311IncrementalN.fetch_add(1u, std::memory_order_relaxed);
        g_g311IncrementalUs.fetch_add(
            static_cast<uint64_t>(us < 0 ? 0 : us), std::memory_order_relaxed);
        g311RecordDelta(static_cast<uint32_t>(work));
    }
    return true;
}

static bool g310Initialize(
    const std::array<G310DesiredPage, 128> &desired, int seedTi)
{
    if (seedTi < 0 || seedTi >= static_cast<int>(kG248TargetCount))
    {
        g_g310FailMissing.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    static std::vector<uint32_t> s_pageMap;
    static std::vector<uint32_t> s_basePixels;
    s_pageMap.resize(128u);
    s_basePixels.resize(512u * 512u);
    std::array<G310PageState, 128> next{};
    uint64_t gpuPages = 0u, guestPages = 0u;
    for (uint32_t py = 0u; py < 16u; ++py)
    {
        for (uint32_t px = 0u; px < 8u; ++px)
        {
            const size_t page = static_cast<size_t>(py) * 8u + px;
            const G310DesiredPage &d = desired[page];
            G310PageState state{};
            state.sourceTi = d.sourceTi;
            if (d.sourceTi >= 0)
            {
                state.sourceGen = d.sourceGen;
                s_pageMap[page] =
                    static_cast<uint32_t>(d.sourceTi + 1) |
                    (static_cast<uint32_t>(d.sourceCol) << 8u) |
                    (static_cast<uint32_t>(d.sourceRow) << 16u) |
                    0xFF000000u;
                ++gpuPages;
            }
            else
            {
                s_pageMap[page] = 0xFF000000u;
                if (!g310GuestPageMeta(
                        page, state.guestGen, state.guestHash))
                {
                    g_g310FailMissing.fetch_add(1u, std::memory_order_relaxed);
                    g_g310Initialized = false;
                    return false;
                }
                for (uint32_t localY = 0u; localY < 32u; ++localY)
                {
                    const uint32_t gsY = py * 32u + localY;
                    const uint32_t glY = 511u - gsY;
                    for (uint32_t localX = 0u; localX < 64u; ++localX)
                    {
                        const uint32_t x = px * 64u + localX;
                        s_basePixels[static_cast<size_t>(glY) * 512u + x] =
                            GSMem::ReadCT32(
                                g_g144LastVram, 0x2720u, 8u, x, gsY);
                    }
                }
                ++guestPages;
            }
            next[page] = state;
        }
    }
    const bool census = g311CensusOn() && !g_g311InOracle;
    const auto t0 = census ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    if (!g310_backend_build_logical_composite(s_pageMap, s_basePixels))
    {
        g310Invalidate("composite", true);
        return false;
    }
    if (census)
    {
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        g_g311CompositeN.fetch_add(1u, std::memory_order_relaxed);
        g_g311CompositeUs.fetch_add(
            static_cast<uint64_t>(us < 0 ? 0 : us), std::memory_order_relaxed);
        // Changed pages vs the prior committed snapshot = the incremental deletion ceiling.
        g311RecordDelta(g311DeltaCount(next, g_g310Pages));
    }
    g_g310Pages = next;
    g_g310Stale = false;
    g_g310Initialized = true;
    g_g310Init.fetch_add(1u, std::memory_order_relaxed);
    g_g310Refresh.fetch_add(1u, std::memory_order_relaxed);
    g_g310CompositeJobs.fetch_add(1u, std::memory_order_relaxed);
    if (gpuPages != 0u)
    {
        g_g310GpuJobs.fetch_add(1u, std::memory_order_relaxed);
        g_g310GpuPages.fetch_add(gpuPages, std::memory_order_relaxed);
        g_g310AuthorityMoves.fetch_add(gpuPages, std::memory_order_relaxed);
    }
    if (guestPages != 0u)
    {
        g_g310GuestJobs.fetch_add(1u, std::memory_order_relaxed);
        g_g310GuestPages.fetch_add(guestPages, std::memory_order_relaxed);
    }
    return true;
}

// Immediately before G290's structurally-proven full-width 0x139 clear/write, destructively retire
// subordinate dirty aliases whose complete dirty page sets the leading opaque clear replaces
// before any read. This converts a divergent multi-owner set into one exact 0x139 writer without
// either readback publication or G282 fan-out. If a later admission step fails, the immediate CPU
// fallback executes the same leading clear first, so the discarded bytes remain unobservable.
static bool g310RetireSeededAliases()
{
    if (!g310AliasRetireOn())
        return true;
    const uint32_t srcBase = framePageBaseToBlock(0x139u);
    const uint32_t clearLast =
        srcBase + (13u * 8u - 1u) * 32u;
    const auto ownsReplacedPage = [&](int ti) {
        for (uint32_t py = 0u; py < 13u; ++py)
            for (uint32_t px = 0u; px < 8u; ++px)
            {
                const uint32_t pageBlock =
                    srcBase + (py * 8u + px) * 32u;
                if (g280DirtyTargetOwnsPage(ti, pageBlock, nullptr, nullptr))
                    return true;
            }
        return false;
    };

    std::array<bool, kG248TargetCount> retire{};
    std::array<uint64_t, kG248TargetCount> targetPages{};
    uint64_t targets = 0u, pages = 0u;
    for (uint32_t t = 1u; t < kG248TargetCount; ++t)
    {
        G261Res &r = g_g261Res[t];
        if (!r.dirty || !ownsReplacedPage(static_cast<int>(t)))
            continue;
        const int pageLo = std::max(0, r.dirtyLo) / 32;
        const int pageHi = std::min(511, r.dirtyHi) / 32;
        if (r.fbw == 0u || r.fbW != static_cast<int>(r.fbw) * 64 ||
            pageHi < pageLo || r.dirtyLo < 0 || r.dirtyHi >= 512 ||
            (r.dirtyLo & 31) != 0 || ((r.dirtyHi + 1) & 31) != 0 ||
            r.mirPending || !g_g280Ovl[t].empty())
        {
            g_g310RetireFail.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const uint32_t targetBase = framePageBaseToBlock(kG261Fbp[t]);
        for (int py = pageLo; py <= pageHi; ++py)
            for (uint32_t px = 0u; px < r.fbw; ++px)
            {
                const uint64_t block =
                    static_cast<uint64_t>(targetBase) +
                    (static_cast<uint64_t>(py) * r.fbw + px) * 32u;
                if (block < srcBase || block > clearLast)
                {
                    g_g310RetireFail.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
            }
        targetPages[t] =
            static_cast<uint64_t>(pageHi - pageLo + 1) * r.fbw;
        retire[t] = true;
        ++targets;
        pages += targetPages[t];
    }
    for (uint32_t t = 1u; t < kG248TargetCount; ++t)
    {
        if (!retire[t])
            continue;
        G261Res &r = g_g261Res[t];
        r.dirty = false;
        r.dirtyLo = 512;
        r.dirtyHi = -1;
        r.covLo = 512;
        r.covHi = -1;
        r.mirPending = false;
        r.mirLo = 512;
        r.mirHi = -1;
        r.mirBits = {};
        if (r.g291CovFbw != 0u)
        {
            r.g291Cov = {};
            r.g291CovFbw = 0u;
            g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        }
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[t]];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        g261UpdateWindow(static_cast<int>(t));
        g310NoteTargetClean(true);
    }
    for (uint32_t t = 1u; t < kG248TargetCount; ++t)
        if (ownsReplacedPage(static_cast<int>(t)))
        {
            g_g310RetireFail.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    if (targets != 0u)
    {
        const uint64_t n =
            g_g310RetireBatches.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g310RetireTargets.fetch_add(targets, std::memory_order_relaxed);
        g_g310RetirePages.fetch_add(pages, std::memory_order_relaxed);
        if (g310StatOn() && (n <= 8u || (n % 128u) == 0u))
            std::fprintf(stderr,
                         "[G310:retire] n=%llu destructive=1 targets=%llu pages=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(targets),
                         static_cast<unsigned long long>(pages));
    }
    return true;
}

// G311 exact oracle. The logical FBO currently holds the incremental page-delta result. Snapshot
// it, force a full G309 composite of the SAME desired page map into the same FBO, snapshot that,
// and compare all 512x512 pixels. bad==0 proves incremental == full-composite; G310 already proved
// full-composite == legacy publication, so this transitively proves incremental == legacy. The
// full rebuild leaves g_g310Pages/g_g310Initialized consistent (identical desired), and since the
// two results are equal the display binds a correct snapshot either way.
static void g311VerifyIncremental(const std::array<G310DesiredPage, 128> &desired,
                                  int seedTi)
{
    static std::vector<uint32_t> s_inc;
    static std::vector<uint32_t> s_full;
    if (!g178_backend_read_color(kG310LogicalFbp, 512, 512, 0, 512, s_inc) ||
        s_inc.size() != 512u * 512u)
    {
        g_g311VerifyMiss.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_g311InOracle = true;
    g_g310Initialized = false;
    const bool ok =
        g310Initialize(desired, seedTi) &&
        g178_backend_read_color(kG310LogicalFbp, 512, 512, 0, 512, s_full) &&
        s_full.size() == 512u * 512u;
    g_g311InOracle = false;
    if (!ok)
    {
        g_g311VerifyMiss.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t bad = 0u;
    std::array<uint32_t, 128> pageBad{};
    std::array<uint32_t, 128> firstInc{};
    std::array<uint32_t, 128> firstFull{};
    for (uint32_t glY = 0u; glY < 512u; ++glY)
        for (uint32_t x = 0u; x < 512u; ++x)
        {
            const size_t i = static_cast<size_t>(glY) * 512u + x;
            if (s_inc[i] == s_full[i])
                continue;
            const uint32_t gsY = 511u - glY;
            const size_t page =
                static_cast<size_t>(gsY >> 5u) * 8u + (x >> 6u);
            if (pageBad[page]++ == 0u)
            {
                firstInc[page] = s_inc[i];
                firstFull[page] = s_full[i];
            }
            ++bad;
        }
    const uint64_t n =
        g_g311VerifyN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g311VerifyPixels.fetch_add(512u * 512u, std::memory_order_relaxed);
    const uint64_t totalBad =
        g_g311VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || n <= 8u || (n % 128u) == 0u)
        std::fprintf(stderr,
                     "[G311:vfy] n=%llu pixels=262144 bad=%llu totalBad=%llu\n",
                     (unsigned long long)n, (unsigned long long)bad,
                     (unsigned long long)totalBad);
    if (bad != 0u && n <= 4u)
        for (size_t page = 0u; page < pageBad.size(); ++page)
            if (pageBad[page] != 0u)
                std::fprintf(stderr,
                             "[G311:vfy-page] n=%llu page=0x%x py=%zu px=%zu "
                             "bad=%u inc=%08x full=%08x\n",
                             (unsigned long long)n,
                             0x2720u + static_cast<uint32_t>(page) * 32u,
                             page / 8u, page % 8u, pageBad[page],
                             firstInc[page], firstFull[page]);
}

static bool g310EnsureCurrent()
{
    if (!g310On())
        return false;
    if (g_g310Frozen)
        return g310FrozenUsable();
    std::array<G310DesiredPage, 128> desired{};
    int seedTi = -1;
    if (!g310BuildDesired(desired, seedTi))
    {
        g_g310Initialized = false;
        return false;
    }
    if (!g_g310Initialized)
        return g310Initialize(desired, seedTi);
    // G311: incremental page-delta refresh of the persisted snapshot. On any backend failure it
    // fails closed (g310Invalidate → g_g310Initialized=false → full composite next consumer).
    if (!g310ApplyDesired(desired))
        return false;
    if (g311VerifyOn())
        g311VerifyIncremental(desired, seedTi);
    return true;
}

static void g310Invalidate(const char *stage, bool backendFailure)
{
    if (backendFailure)
        g_g310FailBackend.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g310FailLayout.fetch_add(1u, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1u, std::memory_order_relaxed) < 16u)
        std::fprintf(stderr, "[G310:invalidate] stage=%s frozen=%d\n",
                     stage, g_g310Frozen ? 1 : 0);
    g_g310Initialized = false;
    if (g_g310Frozen)
        g_g310FrozenInvalid = true;
}

static void g310NoteProducerWrite(int ti, int rowLo, int rowHi)
{
    if ((!g310StatOn() && !g310On()) ||
        ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    const G261Res &r = g_g261Res[ti];
    uint64_t pages = 0u;
    if (r.fbw != 0u && r.fbW == static_cast<int>(r.fbw) * 64 &&
        rowLo <= rowHi)
    {
        const uint32_t base = framePageBaseToBlock(kG261Fbp[ti]);
        const int pageLo = std::max(0, rowLo) / 32;
        const int pageHi = std::min(511, rowHi) / 32;
        for (int py = pageLo; py <= pageHi; ++py)
            for (uint32_t px = 0u; px < r.fbw; ++px)
            {
                const uint64_t pageBlock64 =
                    static_cast<uint64_t>(base) +
                    (static_cast<uint64_t>(py) * r.fbw + px) * 32u;
                if (pageBlock64 < 0x2720u || pageBlock64 > 0x3700u ||
                    ((pageBlock64 - 0x2720u) & 31u) != 0u)
                    continue;
                const size_t index =
                    static_cast<size_t>((pageBlock64 - 0x2720u) >> 5u);
                ++g_g310TargetPageGen[static_cast<size_t>(ti)][index];
                ++pages;
            }
    }
    g_g310ProducerN[ti].fetch_add(1u, std::memory_order_relaxed);
    g_g310ProducerPages[ti].fetch_add(pages, std::memory_order_relaxed);
    if (!g310On() || !g_g310Initialized || g_g310Verifying)
        return;
    if (g_g310Frozen)
    {
        g_g310RefreshPending = true;
        return;
    }
    // Any producer write changes page generations. Rebuilding each transition immediately caused
    // thousands of synchronous jobs; the next admitted display consumer reconstructs the snapshot.
    // G310 default: full one-pass recomposition. G311: keep the snapshot initialized and mark it
    // page-granular stale so the consumer refreshes only the changed pages (g310ApplyDesired).
    g_g310Invalidations.fetch_add(1u, std::memory_order_relaxed);
    if (g311On())
    {
        g_g310Stale = true;
        g_g311Stales.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_g310Initialized = false;
}

static void g310NoteTargetClean(bool publicationOk)
{
    if (!g310On() || g_g310Verifying || !g_g310Initialized)
        return;
    if (!publicationOk)
    {
        g310Invalidate("materialize", false);
        return;
    }
    if (g_g310Frozen)
    {
        g_g310RefreshPending = true;
        return;
    }
    // Publication changes winner pages from a resident target to guest VRAM. Recompose lazily at
    // the next display consumer instead of issuing per-page guest uploads here. G311 refreshes only
    // the changed pages; the G310 default fully recomposes.
    g_g310Invalidations.fetch_add(1u, std::memory_order_relaxed);
    if (g311On())
    {
        g_g310Stale = true;
        g_g311Stales.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_g310Initialized = false;
}

static void g310BeginFreeze()
{
    g_g310Frozen = true;
    g_g310FrozenInvalid = !g_g310Initialized;
    g_g310RefreshPending = false;
}

static void g310EndFreeze(bool submitted)
{
    if (!g_g310Frozen)
        return;
    const bool usable = submitted && !g_g310FrozenInvalid &&
                        g_g310Initialized;
    const bool invalidate = usable && g_g310RefreshPending;
    g_g310Frozen = false;
    g_g310FrozenInvalid = false;
    g_g310RefreshPending = false;
    if (!usable)
        g_g310Initialized = false;
    else if (invalidate)
    {
        // A producer wrote during the freeze. G311 marks the persisted snapshot stale (page-delta
        // refresh next consumer); the G310 default de-initializes for a full recomposition.
        g_g310Invalidations.fetch_add(1u, std::memory_order_relaxed);
        if (g311On())
        {
            g_g310Stale = true;
            g_g311Stales.fetch_add(1u, std::memory_order_relaxed);
        }
        else
            g_g310Initialized = false;
    }
}

static bool g310FrozenUsable()
{
    return g_g310Frozen && !g_g310FrozenInvalid &&
           g_g310Initialized;
}

static void g310VerifyAgainstLegacy(const std::vector<uint32_t> &logical)
{
    const uint64_t prior = g_g310VerifyN.load(std::memory_order_relaxed);
    if (prior < 2u)
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            const G261Res &r = g_g261Res[t];
            std::fprintf(
                stderr,
                "[G310:vfy-state] n=%llu ti=%u fbp=%03x dirty=%d "
                "rows=%d..%d cov=%d..%d fbw=%u\n",
                (unsigned long long)(prior + 1u), t, kG261Fbp[t],
                r.dirty ? 1 : 0, r.dirtyLo, r.dirtyHi,
                r.covLo, r.covHi, r.fbw);
        }
    g_g310Verifying = true;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty)
            g261Materialize(static_cast<int>(t), kG261MatTexAlias);
    g_g310Verifying = false;
    g_g310Initialized = false; // verifier consumed the frozen pre-publication snapshot

    uint64_t bad = 0u;
    std::array<uint32_t, 128> pageBad{};
    std::array<uint32_t, 128> firstGot{};
    std::array<uint32_t, 128> firstExpected{};
    if (logical.size() != 512u * 512u || g_g144LastVram == nullptr)
        bad = 512u * 512u;
    else
        for (uint32_t glY = 0u; glY < 512u; ++glY)
        {
            const uint32_t gsY = 511u - glY;
            for (uint32_t x = 0u; x < 512u; ++x)
            {
                const uint32_t expected =
                    GSMem::ReadCT32(g_g144LastVram, 0x2720u, 8u, x, gsY);
                const uint32_t got =
                    logical[static_cast<size_t>(glY) * 512u + x];
                if (got == expected)
                    continue;
                const size_t page =
                    static_cast<size_t>(gsY >> 5u) * 8u + (x >> 6u);
                if (pageBad[page]++ == 0u)
                {
                    firstGot[page] = got;
                    firstExpected[page] = expected;
                }
                ++bad;
            }
        }
    const uint64_t n =
        g_g310VerifyN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g310VerifyPixels.fetch_add(512u * 512u, std::memory_order_relaxed);
    const uint64_t totalBad =
        g_g310VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || n <= 8u || (n % 128u) == 0u)
        std::fprintf(
            stderr,
            "[G310:vfy] n=%llu pixels=262144 bad=%llu totalBad=%llu\n",
            (unsigned long long)n, (unsigned long long)bad,
            (unsigned long long)totalBad);
    if (bad != 0u && n <= 2u)
        for (size_t page = 0u; page < pageBad.size(); ++page)
            if (pageBad[page] != 0u)
                std::fprintf(
                    stderr,
                    "[G310:vfy-page] n=%llu page=0x%x py=%zu px=%zu "
                    "bad=%u first=%08x/%08x\n",
                    (unsigned long long)n,
                    0x2720u + static_cast<uint32_t>(page) * 32u,
                    page / 8u, page % 8u, pageBad[page],
                    firstGot[page], firstExpected[page]);
}

static bool g310PrepareLogical(bool &useLogical)
{
    useLogical = false;
    if (!g310EnsureCurrent())
        return false;
    const uint64_t n =
        g_g310Prepares.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g310VerifyOn())
    {
        static std::vector<uint32_t> s_logical;
        if (!g178_backend_read_color(
                kG310LogicalFbp, 512, 512, 0, 512, s_logical))
        {
            g310Invalidate("verify-read", true);
            return false;
        }
        g310VerifyAgainstLegacy(s_logical);
    }
    else
        useLogical = true;
    if ((g310StatOn() || g311CensusOn()) && (n == 1u || (n % 64u) == 0u))
        g310Report();
    return true;
}

// Invert one CT32 page's 2048 word positions once. PSMT8 bytes and CT32 pixels share physical
// GS pages but NOT within-page geometry; the view therefore starts with the real AddressP8
// result, then maps that byte through this exact CT32 inverse instead of assuming tile order.
struct G281Ct32Inverse
{
    std::array<int16_t, 2048> x{};
    std::array<int16_t, 2048> y{};
    bool ok = true;
    G281Ct32Inverse()
    {
        x.fill(-1);
        y.fill(-1);
        for (uint32_t yy = 0; yy < 32u; ++yy)
            for (uint32_t xx = 0; xx < 64u; ++xx)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, xx, yy);
                const uint32_t word = (off & 8191u) >> 2u;
                if (word >= x.size() || x[word] >= 0)
                {
                    ok = false;
                    continue;
                }
                x[word] = static_cast<int16_t>(xx);
                y[word] = static_cast<int16_t>(yy);
            }
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] < 0 || y[i] < 0)
                ok = false;
    }
};