// ===== G767: LOCAL -> LOCAL ON THE GPU WHEN THE SOURCE LIVES IN RESIDENT FBOs =====
//
// THE EDGE. A local->local transfer is a byte move inside GS memory. When its source pages are
// newest in a resident FBO (a page a wave or a fold rendered), the legacy edge publishes every dirty
// target the source overlaps — the whole 448-row 0x139 window — so the CPU pixel body can read real
// bytes, and G766's destination mirror then patches the result back into the FBOs. dungeon1's
// texture-atlas scrolls (PSMT8 64x64 / 63x56 / 1x8 bands inside 0x139 at bw=2, every frame) and
// combat's 0x36a0 -> 0x3420 PSMT8 copies are this: GPU -> CPU -> GPU for bytes no CPU reads.
//
// THE MOVE, ON THE GPU. The copy is decomposed into its byte map — for every texel of the rect and
// every byte of the format, (source page, CT32 word, lane) -> (destination page, word, lane), from the
// SAME address functions the CPU body uses (addrPSMCT32 / addrPSMCT16(S) / addrPSMT8). Every
// page the map touches is gathered from the FBO that holds it (valid owner, or a dirty resident whose
// copy is current) into an atlas; one pass per destination page assembles each destination texel
// lane by lane (mapped lanes from the source bytes, the rest from the page's own current bytes), at
// every physical sample of the scale S; the finished pages are copied into EVERY dirty resident that
// holds them. The residents then carry the pages exactly as a fold leaves them (dirty rows, owner
// stamp, G291 coverage, G280/G310 notes). Guest VRAM is not written: the obligation is carried by the
// residency record and discharged at a real CPU observer.
//
// Admission (fail-closed, the legacy path runs otherwise): same format (CT32 / CT24 / CT16 / CT16S /
// PSMT8), both rects inside their buffer width (the texel -> byte map is then a bijection), source and
// destination byte sets disjoint, no other GPU authority pending over either range, at least one
// source page FBO-owned (else guest VRAM is current and G766's mirror path is cheaper), every touched
// page held by a resident FBO, every destination page with a dirty home.
//   DC2_G767_NO_GPUL2L=1  rollback
extern bool g767_backend_l2l(const std::vector<int32_t> &gather, const std::vector<uint32_t> &map,
                             const std::vector<int32_t> &destTiles,
                             const std::vector<int32_t> &scatter);
static bool g767L2lOn()
{
    static const bool on = g766OwnerOn() && g767FoldOn() && !envFlagEnabled("DC2_G767_NO_GPUL2L") &&
                           !envFlagDisabled("DC2_G767_GPUL2L");
    return on;
}
static uint64_t g_g767L2lTry = 0u, g_g767L2lDone = 0u, g_g767L2lRefShape = 0u, g_g767L2lRefHold = 0u,
    g_g767L2lRefHome = 0u, g_g767L2lRefNoOwn = 0u, g_g767L2lRefAuth = 0u, g_g767L2lFail = 0u,
    g_g767L2lBytes = 0u;
static void g767L2lReport()
{
    if (!g767StatOn())
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 256u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G767:l2l] try=%llu done=%llu bytes=%llu refuse(shape=%llu auth=%llu noOwned=%llu "
                 "holder=%llu home=%llu) fail=%llu\n",
                 (unsigned long long)g_g767L2lTry, (unsigned long long)g_g767L2lDone,
                 (unsigned long long)g_g767L2lBytes, (unsigned long long)g_g767L2lRefShape,
                 (unsigned long long)g_g767L2lRefAuth, (unsigned long long)g_g767L2lRefNoOwn,
                 (unsigned long long)g_g767L2lRefHold, (unsigned long long)g_g767L2lRefHome,
                 (unsigned long long)g_g767L2lFail);
}

static uint32_t g767L2lBytesPerTexel(uint8_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT32: return 4u;
    case GS_PSM_CT24: return 3u;
    case GS_PSM_CT16:
    case GS_PSM_CT16S: return 2u;
    case GS_PSM_T8: return 1u;
    default: return 0u;
    }
}
// ⛔⛔ G771: `bw` is the TRXPOS/BITBLTBUF buffer width in 64-texel units, which is what the CPU body
// (readTransferPixel / writeTransferPixel) passes to every address function. GSMem::AddressP8 takes
// PAGE columns instead (a PSMT8 page is 128 texels wide; its callers pass max(1, bw >> 1)), so the
// original `AddressP8(bp, bw, ...)` placed every texel row >= 64 of a bw >= 2 PSMT8 move `bw >> 1`
// page rows too far down: the copy read the wrong source pages and wrote the wrong destination
// pages (the lava-palace "Right on schedule" cutscene: the 128-wide mouth/eye composites into
// 0x3420 landed in page 0x1a3 instead of 0x1a2 - a missing mouth and a clobbered collar texture).
// The G767 oracle replays the move through this same function, so it could not see the error.
// PSMT8 now uses GSPSMT8::addrPSMT8, the CPU body's own function.
static uint32_t g767L2lAddr(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24: return GSPSMCT32::addrPSMCT32(bp, bw, x, y);
    case GS_PSM_CT16: return GSPSMCT16::addrPSMCT16(bp, bw, x, y);
    case GS_PSM_CT16S: return GSPSMCT16::addrPSMCT16S(bp, bw, x, y);
    case GS_PSM_T8: return GSPSMT8::addrPSMT8(bp, bw, x, y);
    default: return 0xFFFFFFFFu;
    }
}

struct G767L2lHome
{
    uint8_t slot;
    uint16_t page;
    uint8_t row, col;
    bool primary;
};
struct G767L2lPlan
{
    std::vector<uint32_t> tilePage;   // atlas tile -> GS page
    std::vector<int32_t> gather;      // {holderKey, holderX, holderY, tile} per atlas tile
    std::vector<int32_t> destTiles;   // dest tile -> atlas tile of its own current bytes
    std::vector<uint32_t> destPage;   // dest tile -> GS page
    std::vector<uint32_t> map;        // 64 x 32 x nDest texels x 4 lanes (GL-row order per tile)
    std::vector<int32_t> scatter;     // {homeKey, homeX, homeY, destTile}
    std::vector<G767L2lHome> homes;
    uint32_t slots = 0u;              // every slot the move reads or writes (their mirrors apply first)
    uint64_t bytes = 0u;
};

// The page-level holder of page p for a GPU read (owner FBO, else a dirty resident whose copy is
// current). -1 = no FBO holds it.
static int g767L2lHolder(uint32_t page, uint32_t &row, uint32_t &col)
{
    const int o = g766ValidOwner(page);
    if (o >= 0)
        return g766SlotPagePos(static_cast<uint32_t>(o), page, row, col) ? o : -1;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty && g766SlotPagePos(t, page, row, col) && g767UnownedPageCurrent(t, page))
            return static_cast<int>(t);
    return -1;
}

// Build the plan. `full` = also build the byte map (the execution form); without it only the page-level
// admission runs (the barrier's can-take form).
static bool g767L2lPlanBuild(uint32_t sbp, uint8_t sbw, uint8_t spsm, uint32_t ssax, uint32_t ssay,
                             uint32_t dbp, uint8_t dbw, uint8_t dpsm, uint32_t dsax, uint32_t dsay,
                             uint32_t rrw, uint32_t rrh, bool full, G767L2lPlan &p)
{
    p = G767L2lPlan{};
    const uint32_t bpt = g767L2lBytesPerTexel(spsm);
    if (!g767L2lOn() || !g261AnyDirty() || spsm != dpsm || bpt == 0u || rrw == 0u || rrh == 0u ||
        rrw * rrh > 128u * 128u || sbw == 0u || dbw == 0u ||
        ssax + rrw > static_cast<uint32_t>(sbw) * 64u || dsax + rrw > static_cast<uint32_t>(dbw) * 64u ||
        g280AliasOn() || g351SplitOn() || !g766PullsAllowed())
    {
        ++g_g767L2lRefShape;
        return false;
    }
    const G144BlockRange src = g144RangeForRect(sbp, spsm, sbw, ssax, ssay, rrw, rrh);
    const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!src.valid || !dst.valid)
    {
        ++g_g767L2lRefShape;
        return false;
    }
    if (g144RangeOverlaps(src, dst) &&
        !(sbp == dbp && sbw == dbw &&
          (ssax + rrw <= dsax || dsax + rrw <= ssax || ssay + rrh <= dsay || dsay + rrh <= ssay)))
    {
        ++g_g767L2lRefShape;
        return false;
    }
    // Cheap exact pre-check: a page superset of the source (its block range) with no FBO owner means
    // guest VRAM is current for the whole source (G766's mirror path takes it) - skip the texel walk.
    {
        bool anyOwnedSuperset = false;
        for (uint32_t pg = src.first >> 5u; pg <= (src.last >> 5u) && pg < kG178PageCount; ++pg)
            if (g766ValidOwner(pg) >= 0)
            {
                anyOwnedSuperset = true;
                break;
            }
        if (!anyOwnedSuperset)
        {
            ++g_g767L2lRefNoOwn;
            return false;
        }
    }
    if (g767OtherAuthorityHits(src) || g767OtherAuthorityHits(dst))
    {
        ++g_g767L2lRefAuth;
        return false;
    }
    // Pages the move reads (source) and writes (destination).
    static std::vector<uint32_t> s_srcPages, s_dstPages;
    s_srcPages.clear();
    s_dstPages.clear();
    const auto addPage = [](std::vector<uint32_t> &v, uint32_t pg) {
        if (std::find(v.begin(), v.end(), pg) == v.end())
            v.push_back(pg);
    };
    for (uint32_t j = 0u; j < rrh; ++j)
        for (uint32_t i = 0u; i < rrw; ++i)
        {
            const uint32_t sa = g767L2lAddr(spsm, sbp, sbw, ssax + i, ssay + j);
            const uint32_t da = g767L2lAddr(dpsm, dbp, dbw, dsax + i, dsay + j);
            if (sa == 0xFFFFFFFFu || da == 0xFFFFFFFFu || (sa >> 13) >= kG178PageCount ||
                (da >> 13) >= kG178PageCount)
            {
                ++g_g767L2lRefShape;
                return false;
            }
            addPage(s_srcPages, sa >> 13);
            addPage(s_dstPages, da >> 13);
            if (s_srcPages.size() + s_dstPages.size() > 64u)
            {
                ++g_g767L2lRefShape;
                return false;
            }
        }
    bool anyOwned = false;
    for (uint32_t pg : s_srcPages)
        anyOwned = anyOwned || g766ValidOwner(pg) >= 0;
    if (!anyOwned)
    {
        ++g_g767L2lRefNoOwn;
        return false; // guest VRAM is current for the whole source: G766's mirror path takes it
    }
    // Atlas tiles: every source page and every destination page (its own current bytes).
    const auto tileOf = [&p](uint32_t pg) -> int {
        for (size_t k = 0u; k < p.tilePage.size(); ++k)
            if (p.tilePage[k] == pg)
                return static_cast<int>(k);
        return -1;
    };
    const auto addTile = [&](uint32_t pg) -> bool {
        if (tileOf(pg) >= 0)
            return true;
        uint32_t row = 0u, col = 0u;
        const int h = g767L2lHolder(pg, row, col);
        if (h < 0)
            return false;
        p.tilePage.push_back(pg);
        p.gather.insert(p.gather.end(),
                        {static_cast<int32_t>(g766SlotKey(static_cast<uint32_t>(h))),
                         static_cast<int32_t>(col * 64u), static_cast<int32_t>(row * 32u),
                         static_cast<int32_t>(p.tilePage.size() - 1u)});
        p.slots |= 1u << static_cast<uint32_t>(h);
        return true;
    };
    for (uint32_t pg : s_srcPages)
        if (!addTile(pg))
        {
            ++g_g767L2lRefHold;
            return false;
        }
    for (uint32_t pg : s_dstPages)
        if (!addTile(pg))
        {
            ++g_g767L2lRefHold;
            return false;
        }
    if (p.tilePage.size() > 256u)
    {
        ++g_g767L2lRefShape;
        return false;
    }
    // Destination homes: every dirty resident holding the page over FBO-defined rows.
    for (size_t d = 0u; d < s_dstPages.size(); ++d)
    {
        const uint32_t pg = s_dstPages[d];
        const int owner = g766ValidOwner(pg);
        int primary = -1;
        const size_t first = p.homes.size();
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            uint32_t row = 0u, col = 0u;
            if (!g767SlotTakes(t, pg, 0, 31, 0xFFFFFFFFu, row, col))
                continue;
            if (static_cast<int>(t) == owner || primary < 0)
                primary = static_cast<int>(p.homes.size());
            p.homes.push_back(G767L2lHome{static_cast<uint8_t>(t), static_cast<uint16_t>(pg),
                                          static_cast<uint8_t>(row), static_cast<uint8_t>(col), false});
            p.scatter.insert(p.scatter.end(),
                             {static_cast<int32_t>(g766SlotKey(t)), static_cast<int32_t>(col * 64u),
                              static_cast<int32_t>(row * 32u), static_cast<int32_t>(d)});
            p.slots |= 1u << t;
        }
        if (primary < 0)
        {
            ++g_g767L2lRefHome;
            return false; // a destination page no dirty resident holds: guest VRAM must take it
        }
        p.homes[static_cast<size_t>(primary)].primary = true;
        (void)first;
        p.destTiles.push_back(tileOf(pg));
        p.destPage.push_back(pg);
    }
    if (!full)
        return true;
    // The byte map. Rows are stored in the atlas's GL orientation: in-page GS row y -> row 31 - y.
    static const G281Ct32Inverse inv;
    if (!inv.ok)
        return false;
    p.map.assign(static_cast<size_t>(64u * 32u) * p.destPage.size() * 4u, 0u);
    for (uint32_t j = 0u; j < rrh; ++j)
        for (uint32_t i = 0u; i < rrw; ++i)
        {
            const uint32_t sa0 = g767L2lAddr(spsm, sbp, sbw, ssax + i, ssay + j);
            const uint32_t da0 = g767L2lAddr(dpsm, dbp, dbw, dsax + i, dsay + j);
            for (uint32_t b = 0u; b < bpt; ++b)
            {
                const uint32_t sa = sa0 + b, da = da0 + b;
                const int st = tileOf(sa >> 13);
                int dt = -1;
                for (size_t k = 0u; k < p.destPage.size(); ++k)
                    if (p.destPage[k] == (da >> 13))
                        dt = static_cast<int>(k);
                if (st < 0 || dt < 0)
                    return false;
                const uint32_t sw = (sa & 8191u) >> 2u, dw = (da & 8191u) >> 2u;
                const uint32_t sx = static_cast<uint32_t>(inv.x[sw]);
                const uint32_t sy = 31u - static_cast<uint32_t>(inv.y[sw]);
                const uint32_t dx = static_cast<uint32_t>(inv.x[dw]);
                const uint32_t dy = 31u - static_cast<uint32_t>(inv.y[dw]);
                const size_t at = ((static_cast<size_t>(dt) * 32u + dy) * 64u + dx) * 4u + (da & 3u);
                p.map[at] = 0x80000000u | (static_cast<uint32_t>(st) << 13) | ((sa & 3u) << 11) |
                            (sy << 6) | sx;
            }
            p.bytes += bpt;
        }
    return true;
}

// ===== ORACLE (DC2_G767_L2L_VERIFY=1, perturbing, never timed) =====
// Read every atlas page back from its holder (the G675 centre tap - one physical sample per texel),
// replay the byte move on the CPU with the same address functions the CPU body uses, and after the GPU
// move compare every home's copy of every destination page byte for byte.
static bool g767L2lVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G767_L2L_VERIFY");
    return on;
}
static uint64_t g_g767L2lVfyPages = 0u, g_g767L2lVfyBad = 0u, g_g767L2lVfyReadFail = 0u;
// Read GS page `pg` as held by slot key/pos into 8192 raw bytes.
static bool g767L2lReadPage(uint32_t key, uint32_t col, uint32_t row, std::vector<uint8_t> &out)
{
    int w = 0;
    for (uint32_t u = 0u; u < kG248TargetCount; ++u)
        if (g766SlotKey(u) == key)
            w = g_g261Res[u].fbW;
    static std::vector<uint32_t> s_rows;
    if (w <= 0 || !g178_backend_read_color(g766KeyReadFbp(key), w, 512,
                                           480 - static_cast<int>(row) * 32, 32, s_rows))
        return false;
    out.assign(8192u, 0u);
    for (uint32_t ry = 0u; ry < 32u; ++ry)
        for (uint32_t x = 0u; x < 64u; ++x)
        {
            const uint32_t v = s_rows[static_cast<size_t>(31u - ry) * static_cast<size_t>(w) + col * 64u + x];
            const uint32_t word = (GSPSMCT32::addrPSMCT32(0u, 1u, x, ry) & 8191u) >> 2u;
            std::memcpy(out.data() + word * 4u, &v, 4u);
        }
    return true;
}

// The barrier's question: will this transfer run on the GPU (so the source publication is moot)?
static bool g767CanTakeL2lImpl(uint32_t sbp, uint8_t sbw, uint8_t spsm, uint32_t ssax, uint32_t ssay,
                               uint32_t dbp, uint8_t dbw, uint8_t dpsm, uint32_t dsax, uint32_t dsay,
                               uint32_t rrw, uint32_t rrh)
{
    static G767L2lPlan s_p; // flush-owner thread only
    return g767L2lPlanBuild(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, false,
                            s_p);
}

// Execute the move on the GPU and commit the residents. False = nothing was changed; the caller
// replays the skipped publication and runs the CPU body.
static bool g767TryExecuteL2lImpl(uint32_t sbp, uint8_t sbw, uint8_t spsm, uint32_t ssax,
                                  uint32_t ssay, uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    static G767L2lPlan s_p; // flush-owner thread only
    ++g_g767L2lTry;
    if (!g767L2lPlanBuild(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, true, s_p))
    {
        g767L2lReport();
        return false;
    }
    // Every slot the move reads or writes absorbs its deferred guest-write mirror first; a mirror
    // that drops its slot invalidates the plan.
    bool dropped = false;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if ((s_p.slots & (1u << t)) != 0u && g_g261Res[t].mirPending)
        {
            g766MirrorFlushOrDrop(static_cast<int>(t));
            dropped = dropped || !g_g261Res[t].dirty;
        }
    if (dropped &&
        !g767L2lPlanBuild(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, true, s_p))
        return false;
    // Oracle, part 1: the pre-move bytes of every atlas page, and the CPU replay of the move.
    std::vector<std::vector<uint8_t>> vfyPages;
    bool vfy = g767L2lVerifyOn();
    if (vfy)
    {
        vfyPages.resize(s_p.tilePage.size());
        for (size_t k = 0u; vfy && k < s_p.tilePage.size(); ++k)
            vfy = g767L2lReadPage(static_cast<uint32_t>(s_p.gather[k * 4u]),
                                  static_cast<uint32_t>(s_p.gather[k * 4u + 1]) / 64u,
                                  static_cast<uint32_t>(s_p.gather[k * 4u + 2]) / 32u, vfyPages[k]);
        if (!vfy)
            ++g_g767L2lVfyReadFail;
    }
    std::vector<std::vector<uint8_t>> vfyDest;
    if (vfy)
    {
        vfyDest.resize(s_p.destPage.size());
        for (size_t d = 0u; d < s_p.destPage.size(); ++d)
            vfyDest[d] = vfyPages[static_cast<size_t>(s_p.destTiles[d])];
        const uint32_t bpt = g767L2lBytesPerTexel(spsm);
        for (uint32_t j = 0u; j < rrh; ++j)
            for (uint32_t i = 0u; i < rrw; ++i)
            {
                const uint32_t sa0 = g767L2lAddr(spsm, sbp, sbw, ssax + i, ssay + j);
                const uint32_t da0 = g767L2lAddr(dpsm, dbp, dbw, dsax + i, dsay + j);
                for (uint32_t b = 0u; b < bpt; ++b)
                {
                    const uint32_t sa = sa0 + b, da = da0 + b;
                    size_t st = 0u, dt = 0u;
                    for (size_t k = 0u; k < s_p.tilePage.size(); ++k)
                        if (s_p.tilePage[k] == (sa >> 13))
                            st = k;
                    for (size_t k = 0u; k < s_p.destPage.size(); ++k)
                        if (s_p.destPage[k] == (da >> 13))
                            dt = k;
                    vfyDest[dt][da & 8191u] = vfyPages[st][sa & 8191u];
                }
            }
    }
    if (!g767_backend_l2l(s_p.gather, s_p.map, s_p.destTiles, s_p.scatter))
    {
        ++g_g767L2lFail;
        std::fprintf(stderr, "[G767:l2l-fail] backend refused sbp=0x%x dbp=0x%x %ux%u psm=0x%x\n", sbp,
                     dbp, rrw, rrh, spsm);
        return false;
    }
    // Residents: exactly a fold's bookkeeping, per destination page and home.
    uint32_t noted = 0u;
    for (const G767L2lHome &h : s_p.homes)
    {
        G261Res &r = g_g261Res[h.slot];
        const int y0 = static_cast<int>(h.row) * 32, y1 = y0 + 31;
        g716MarkRows(r, y0, y1);
        if (y0 < r.dirtyLo) r.dirtyLo = y0;
        if (y1 > r.dirtyHi) r.dirtyHi = y1;
        if (g291AtlasOn() && r.g291CovFbw != 0u)
            for (int y = y0; y <= y1; ++y)
            {
                if (r.g291CovFbw == r.fbw && h.col < 8u)
                    r.g291Cov[static_cast<size_t>(y)][h.col] = 0u;
                else
                    r.g291Cov[static_cast<size_t>(y)].fill(0u);
            }
        if ((noted & (1u << h.slot)) == 0u)
        {
            noted |= 1u << h.slot;
            g455NoteResidentWrite(h.slot);
        }
        g280NoteFboContentChange(h.slot, y0, y1);
        g310NoteProducerWrite(h.slot, y0, y1);
    }
    for (const G767L2lHome &h : s_p.homes)
        if (h.primary)
        {
            G766PageOwn &o = g_g766Own[h.page];
            o.slot = static_cast<int8_t>(h.slot);
            o.epoch = g_g766Epoch[h.slot];
            o.stamp = ++g_g766Ctr;
            g766MarkSeen(h.slot, h.page);
            ++g_g766Stamps;
        }
    for (const G767L2lHome &h : s_p.homes)
        if (!h.primary)
            g766MarkSeen(h.slot, h.page);
    // Oracle, part 2: every home's copy of every destination page, byte for byte.
    if (vfy)
    {
        static std::vector<uint8_t> s_got;
        for (const G767L2lHome &h : s_p.homes)
        {
            size_t dt = 0u;
            for (size_t k = 0u; k < s_p.destPage.size(); ++k)
                if (s_p.destPage[k] == h.page)
                    dt = k;
            if (!g767L2lReadPage(g766SlotKey(h.slot), h.col, h.row, s_got))
            {
                ++g_g767L2lVfyReadFail;
                continue;
            }
            uint64_t bad = 0u;
            for (size_t k = 0u; k < 8192u; ++k)
                bad += s_got[k] != vfyDest[dt][k] ? 1u : 0u;
            ++g_g767L2lVfyPages;
            if (bad != 0u)
            {
                g_g767L2lVfyBad += bad;
                static uint32_t s_log = 0u;
                if (s_log++ < 16u)
                    std::fprintf(stderr,
                                 "[G767:l2l-verify-BAD] s(%x/%u/%02x @%u,%u) d(%x/%u/%02x @%u,%u) %ux%u "
                                 "page=0x%x slot=%u bad=%llu\n",
                                 sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                                 h.page, h.slot, (unsigned long long)bad);
            }
        }
        static uint32_t s_n = 0u;
        if ((++s_n % 64u) == 1u)
            std::fprintf(stderr, "[G767:l2l-verify] pages=%llu bad=%llu readFail=%llu\n",
                         (unsigned long long)g_g767L2lVfyPages, (unsigned long long)g_g767L2lVfyBad,
                         (unsigned long long)g_g767L2lVfyReadFail);
    }
    ++g_g767L2lDone;
    g_g767L2lBytes += s_p.bytes;
    g767L2lReport();
    return true;
}
