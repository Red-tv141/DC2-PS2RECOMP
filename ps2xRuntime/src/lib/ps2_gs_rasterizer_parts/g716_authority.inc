// G716: GPU memory transactions. Included after the G714 address tables.
static bool g_g716LogicalWrite = false;
template<class Packet>
static void g716AppendGuestPage(uint32_t page, Packet &p)
{
    if (page >= 512 || g_g716Raw[page] || !g_g144LastVram ||
        (uint64_t(page) + 1u) * 8192u > g_g261VramSize) return;
    if (std::find(p.patchPages.begin(), p.patchPages.end(), page) != p.patchPages.end()) return;
    p.patchPages.push_back(page);
    const size_t old = p.patchWords.size();
    p.patchWords.resize(old + 2048u);
    std::memcpy(p.patchWords.data() + old, g_g144LastVram + page * 8192u, 8192u);
}
template<class Packet>
static void g716AppendGuestRange(const G144BlockRange &r, Packet &p)
{
    if (!r.valid) return;
    for (uint32_t page = r.first >> 5u; page <= std::min(511u, r.last >> 5u); ++page)
        g716AppendGuestPage(page, p);
}
static void g716PageMap(G714L2lPacket &p)
{
    // Small immutable map; send on handoffs too, so no queue-order-dependent init latch.
    p.ct32PageWords.resize(2048);
    const auto &c = g714Geom(GS_PSM_CT32);
    for (uint32_t i = 0; i < 2048; ++i) p.ct32PageWords[i] = c.fwd[i] >> 2u;
}

static std::vector<uint8_t> g716ShadowMemory(const G144BlockRange &range)
{
    g716AuditRange(range, 8);
    std::vector<uint8_t> shadow(g_g144LastVram, g_g144LastVram + g_g261VramSize);
    if (g716RangeHasRaw(range))
    {
        const uint32_t lo = range.first >> 5u, hi = std::min(511u, range.last >> 5u);
        std::vector<uint32_t> raw;
        if (!g630_backend_read_vram_rows(lo, hi - lo + 1, raw)) std::abort();
        for (uint32_t pg = lo; pg <= hi; ++pg)
            if (g_g716Raw[pg])
                std::memcpy(shadow.data() + pg * 8192u, raw.data() + (pg - lo) * 2048u, 8192u);
    }
    return shadow;
}

static void g716MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool su, bool du, int cause)
{
    if (!g716AnyRaw() || g_g716GpuObserver) return;
    std::array<uint8_t, 512> wanted{};
    uint32_t lo = 512, hi = 0;
    for (uint32_t p = 0; p < 512; ++p)
    {
        if (!g_g716Raw[p]) continue;
        const G144BlockRange page{p * 32u, p * 32u + 31u, true};
        if (su || du || (src.valid && g144RangeOverlaps(page, src)) ||
            (dst.valid && g144RangeOverlaps(page, dst)))
        { wanted[p] = 1; lo = std::min(lo, p); hi = p; }
    }
    if (lo == 512) return;
    for (uint32_t p = lo; p <= hi; ++p) if (wanted[p]) g716AuditPage(p, 7);
    // One wait for the union, with holes excluded from publication. Guest writes in holes survive.
    std::vector<uint32_t> words;
    if (!g630_backend_read_vram_rows(lo, hi - lo + 1, words) ||
        words.size() != size_t(hi - lo + 1) * 2048u)
    {
        std::fprintf(stderr, "[G716:FAIL] raw CPU publication cause=%d pages=%u..%u\n", cause, lo, hi);
        std::abort(); // Never surrender authority after a failed read.
    }
    ++g_g716CpuBreaks;
    ++g_g716CpuCause[std::min(8, cause)];
    for (uint32_t p = lo; p <= hi; ++p)
        if (wanted[p])
        {
            std::memcpy(g_g144LastVram + p * 8192u, words.data() + (p - lo) * 2048u, 8192u);
            g716SetRaw(p, false);
            ++g_g716CpuPages;
        }
}

// See DC2_G716_MIRROR in the state header. Publishes raw -> guest for every raw page and KEEPS
// the claim, so only an unrouted guest-VRAM reader can observe the difference.
static void g716MirrorRawToGuest()
{
    if (!s_g716Mirror || !g716AnyRaw() || g_g716GpuObserver) return;
    uint32_t lo = 512, hi = 0;
    for (uint32_t p = 0; p < 512u; ++p)
        if (g_g716Raw[p]) { lo = std::min(lo, p); hi = p; }
    if (lo == 512) return;
    std::vector<uint32_t> words;
    if (!g630_backend_read_vram_rows(lo, hi - lo + 1, words) ||
        words.size() != size_t(hi - lo + 1) * 2048u)
    { std::fprintf(stderr, "[G716:FAIL] mirror read %u..%u\n", lo, hi); std::abort(); }
    for (uint32_t p = lo; p <= hi; ++p)
        if (g_g716Raw[p] && (uint64_t(p) + 1u) * 8192u <= g_g261VramSize)
        {
            std::memcpy(g_g144LastVram + p * 8192u, words.data() + (p - lo) * 2048u, 8192u);
            g716StampPage(p); // keep the writer audit's baseline consistent with what we wrote
            ++g_g716MirrorPages;
        }
    if (++g_g716Mirrors % 4096 == 0)
        std::fprintf(stderr, "[G716:mirror] flushes=%llu pages=%llu span=%u..%u\n",
                     (unsigned long long)g_g716Mirrors, (unsigned long long)g_g716MirrorPages,
                     lo, hi);
}

static bool g716RetireTarget(int ti, int cause)
{
    if (!s_g716Authority || !g_g716GpuObserver || s_g716NoRetire) return false;
    G261Res &r = g_g261Res[ti];
    if (!r.dirty) return true;
    if (dc2GetNativeScale() != 1 || r.fbw == 0 || r.fbw > 8 || r.fbW <= 0 ||
        r.fbW > 512 || (g351SplitOn() && kG261Fbp[ti] == 0x139u) ||
        !g714_backend_available() || !g714Geom(GS_PSM_CT32).ok)
    { ++g_g716Fallbacks; return false; }
    if (!g280PrepareMaterialize(ti) || !g264FlushMirror(ti))
    { ++g_g716Fallbacks; return false; }
    auto p = std::make_shared<G714L2lPacket>();
    p->g716Mode = 1;
    g716PageMap(*p);
    const int lo = r.dirtyLo, hi = r.dirtyHi;
    const G144BlockRange verifyRange = g144RangeForRect(framePageBaseToBlock(kG261Fbp[ti]),
        GS_PSM_CT32, r.fbw, 0, lo, r.fbW, hi - lo + 1);
    std::vector<uint8_t> expected;
    if (s_g716Verify && (g_g716Retires % 64u) == 0u)
    {
        expected = g716ShadowMemory(verifyRange);
        std::vector<uint32_t> colors;
        if (!g178_backend_read_color(kG261Fbp[ti], r.fbW, 512, 511 - hi, hi - lo + 1, colors))
            std::abort();
        std::vector<G536PageSave> saved;
        g536CollectGuestOwnedPages(r, kG261Fbp[ti], lo, hi, expected.data(), g_g261VramSize, saved);
        g418UnpackColorRows(expected.data(), g_g261VramSize, colors,
            framePageBaseToBlock(kG261Fbp[ti]), r.fbw, r.fbW, hi + 1, lo, hi, false);
        g536RestoreGuestOwnedPages(saved, expected.data(), g_g261VramSize);
    }
    std::array<uint8_t, 512> produced{};
    for (int pr = lo / 32; pr <= hi / 32; ++pr)
        for (uint32_t pc = 0; pc < (uint32_t(r.fbW) + 63u) / 64u; ++pc)
        {
            const uint32_t pg = kG261Fbp[ti] + uint32_t(pr) * r.fbw + pc;
            if (pg >= 512) { ++g_g716Fallbacks; return false; }
            g716AppendGuestPage(pg, *p);
            if (g536GuestOwnsTargetPage(r, kG261Fbp[ti], pr, pc))
                p->g716Protected.push_back(pg);
            else if (s_g716FullSpan || g716HasRows(r, pr * 32, pr * 32 + 31)) produced[pg] = 1;
        }
    // Price the coverage difference against the publication this handoff replaces: every row in
    // lo..hi without an exact bit is a row the publication WOULD have written and this ingest
    // does not, while g716ClearRows retires its debt anyway.
    {
        uint32_t gap = 0;
        for (int y = lo; y <= hi; ++y) if (!g716HasRows(r, y, y)) ++gap;
        if (gap) { ++g_g716GapRetires; g_g716GapRows += gap; }
        if (s_g716Stat && (g_g716Retires < 4 || (g_g716Retires % 8192) == 0))
            std::fprintf(stderr, "[G716:rowgap] retires=%llu gapRetires=%llu gapRows=%llu "
                         "thisGap=%u span=%d..%d\n",
                         (unsigned long long)g_g716Retires, (unsigned long long)g_g716GapRetires,
                         (unsigned long long)g_g716GapRows, gap, lo, hi);
    }
    if (s_g716FullSpan)
    {
        p->fboIn.insert(p->fboIn.end(), {int32_t(kG261Fbp[ti]), int32_t(r.fbw), r.fbW, lo, hi});
        g_g716Rows += hi - lo + 1;
    }
    else
    // Each run is exact; interior holes remain representable and are not ingested.
    for (int y = lo; y <= hi; )
    {
        if (!g716HasRows(r, y, y)) { ++y; continue; }
        const int first = y++;
        while (y <= hi && g716HasRows(r, y, y)) ++y;
        p->fboIn.insert(p->fboIn.end(), {int32_t(kG261Fbp[ti]), int32_t(r.fbw),
                                       r.fbW, first, y - 1});
        g_g716Rows += y - first;
    }
    if (!g714_backend_submit_l2l(p)) { ++g_g716Fallbacks; return false; }
    if (!expected.empty())
    {
        const uint32_t first = verifyRange.first >> 5u, last = std::min(511u, verifyRange.last >> 5u);
        std::vector<uint32_t> actual;
        if (!g630_backend_read_vram_rows(first, last - first + 1, actual)) std::abort();
        uint64_t bad = 0;
        uint32_t firstBad = 0;
        for (uint32_t pg = first; pg <= last; ++pg)
        {
            if (!produced[pg] && std::find(p->patchPages.begin(), p->patchPages.end(), pg) == p->patchPages.end()
                && !g_g716Raw[pg]) continue;
            for (uint32_t w = 0; w < 2048; ++w)
            {
                uint32_t want; std::memcpy(&want, expected.data() + pg * 8192u + w * 4u, 4u);
                if (want != actual[(pg - first) * 2048u + w])
                { if (!bad) firstBad = pg * 8192u + w * 4u; ++bad; }
            }
        }
        static uint64_t checks = 0, totalBad = 0;
        ++checks; totalBad += bad;
        if (bad || checks <= 4 || checks % 128 == 0)
            std::fprintf(stderr, "[G716:handoff-vfy] checks=%llu bad=%llu thisBad=%llu fbp=%x rows=%d..%d first=%x\n",
                (unsigned long long)checks, (unsigned long long)totalBad, (unsigned long long)bad,
                kG261Fbp[ti], lo, hi, firstBad);
    }
    for (uint32_t pg = 0; pg < 512; ++pg) if (produced[pg]) g716SetRaw(pg, true);
    // Mirror the pages this handoff just claimed, so guest VRAM is correct CONTINUOUSLY and not
    // only at flush boundaries -- otherwise a consumer running later in this same flush would
    // still read stale guest bytes and the reader test would be weaker than it looks.
    g716MirrorRawToGuest();
    // The logical write and all its invalidations occur now. The eventual byte readback is silent.
    g_g716LogicalWrite = true;
    g178BumpRectImpl(framePageBaseToBlock(kG261Fbp[ti]), r.fbw, GS_PSM_CT32,
                     0u, lo, r.fbW, hi - lo + 1,
                     1u | (static_cast<uint32_t>(cause & 255) << 8u));
    g_g716LogicalWrite = false;
    G178FbSnap &snap = g_g178FbSnap[kG261Fbp[ti]];
    snap.valid = true;
    snap.rowLo = std::min(snap.rowLo, r.covLo);
    snap.rowHi = std::max(snap.rowHi, r.covHi);
    snap.genSum = g178GenSumRect(framePageBaseToBlock(kG261Fbp[ti]), r.fbw,
                                 GS_PSM_CT32, 0, 0, r.fbW, 512);
    g716ClearRows(r, lo, hi);
    g261UpdateWindow(ti);
    g310NoteTargetClean(true);
    // Exercise proof must exist on EVERY arm, including ones that decline the copy: the census
    // used to print only from g716ExecuteL2l, so a retire-only arm emitted nothing at all and its
    // clean pixel table was indistinguishable from an inert arm.
    if (++g_g716Retires % 8192 == 0) g716Report();
    return true;
}

static bool g716CanL2l(uint32_t sbp, uint8_t sbw, uint8_t psm, uint32_t sx, uint32_t sy,
                       uint32_t dbp, uint8_t dbw, uint8_t dpsm, uint32_t dx, uint32_t dy,
                       uint32_t w, uint32_t h)
{
    if (!s_g716Authority || !g714_backend_available() || !g261WaveOn() ||
        dc2GetNativeScale() != 1 || psm != dpsm || psm != GS_PSM_T8 ||
        !w || !h || (sbp & 31u) || (dbp & 31u)) return false;
    const auto &g = g714Geom(psm);
    if (!g.ok || sbw < g.bwProbe || dbw < g.bwProbe) return false;
    uint64_t sl, sh, dl, dh;
    g714ByteSpan(g, sbp, g714PagesPerRow(g, sbw), sx, sy, w, h, sl, sh);
    g714ByteSpan(g, dbp, g714PagesPerRow(g, dbw), dx, dy, w, h, dl, dh);
    if (sh >= std::min(4194304u, g_g261VramSize) || dh >= std::min(4194304u, g_g261VramSize) ||
        !(sh < dl || dh < sl)) return false;
    return !g242WriteOverlapsDirtyDepth(sbp, sbw, psm, sx, sy, w, h) &&
           !g242WriteOverlapsDirtyDepth(dbp, dbw, dpsm, dx, dy, w, h);
}
static bool g716ExecuteL2l(uint8_t *vram, uint32_t size,
                           uint32_t sbp, uint8_t sbw, uint8_t psm, uint32_t sx, uint32_t sy,
                           uint32_t dbp, uint8_t dbw, uint8_t dpsm, uint32_t dx, uint32_t dy,
                           uint32_t w, uint32_t h, bool rx, bool ry)
{
    if (!vram || !g716CanL2l(sbp, sbw, psm, sx, sy, dbp, dbw, dpsm, dx, dy, w, h)) return false;
    // Replay the pin's exact state obligations, with a GPU observer. Each affected surface closes
    // its render transaction into raw memory instead of publishing to guest VRAM.
    extern void g715LatePinPart(int);
    g_g716GpuObserver = true;
    g715LatePinPart(3);
    g_g716GpuObserver = false;
    // NOL2L exercises the handoff, then lets the real CPU transfer run. g715LatePinPart already
    // DISARMED the deferred pin; the caller's g144L2lLatePin therefore cannot publish these pages.
    // This diagnostic CPU observer must explicitly acquire its bytes before declining.
    if (s_g716NoL2l)
    {
        const auto src = g144RangeForRect(sbp, psm, sbw, sx, sy, w, h);
        const auto dst = g144RangeForRect(dbp, dpsm, dbw, dx, dy, w, h);
        g716MaterializeForRanges(src, dst, !src.valid, !dst.valid, kG261MatLocal);
        if (++g_g716Copies <= 4 || g_g716Copies % 4096 == 0) g716Report();
        return false;
    }
    const auto &g = g714Geom(psm);
    auto p = std::make_shared<G714L2lPacket>();
    p->g716Mode = 2;
    p->fwd = g.fwd; p->inv = g.inv; p->tableKey = psm;
    g716PageMap(*p);
    p->sbp = sbp; p->dbp = dbp;
    p->sPagesPerRow = g714PagesPerRow(g, sbw); p->dPagesPerRow = g714PagesPerRow(g, dbw);
    p->pw = g.pw; p->ph = g.ph; p->bpp = g.bpp;
    p->ssax = sx; p->ssay = sy; p->dsax = dx; p->dsay = dy;
    p->rrw = w; p->rrh = h; p->revX = rx; p->revY = ry;
    p->dPgX0 = dx / g.pw; p->dPgY0 = dy / g.ph;
    p->dPgNx = (dx + w - 1) / g.pw - p->dPgX0 + 1;
    p->dPgNy = (dy + h - 1) / g.ph - p->dPgY0 + 1;
    for (uint32_t y = sy / g.ph; y <= (sy + h - 1) / g.ph; ++y)
        for (uint32_t x = sx / g.pw; x <= (sx + w - 1) / g.pw; ++x)
            g716AppendGuestPage((sbp >> 5u) + y * p->sPagesPerRow + x, *p);
    for (uint32_t y = p->dPgY0; y < p->dPgY0 + p->dPgNy; ++y)
        for (uint32_t x = p->dPgX0; x < p->dPgX0 + p->dPgNx; ++x)
            g716AppendGuestPage((dbp >> 5u) + y * p->dPagesPerRow + x, *p);
    p->noFboOut = 1;
    if (!g714_backend_submit_l2l(p))
    {
        g716MaterializeForRanges({}, {}, true, true, kG261MatLocal);
        return false;
    }
    for (uint32_t y = p->dPgY0; y < p->dPgY0 + p->dPgNy; ++y)
        for (uint32_t x = p->dPgX0; x < p->dPgX0 + p->dPgNx; ++x)
            g716SetRaw((dbp >> 5u) + y * p->dPagesPerRow + x, true);
    g716MirrorRawToGuest();
    g_g716LogicalWrite = true;
    g178BumpRectImpl(dbp, dbw, psm, dx, dy, w, h, 8u);
    g_g716LogicalWrite = false;
    if (++g_g716Copies <= 4 || g_g716Copies % 4096 == 0) g716Report();
    return true;
}

static void g716PrepareCpuBatch()
{
    if (!g716AnyRaw()) return;
    G144BlockRange wanted{};
    bool unknown = false;
    const auto add = [&](const G144BlockRange &r) {
        if (!r.valid) { unknown = true; return; }
        if (!wanted.valid) wanted = r;
        else { wanted.first = std::min(wanted.first, r.first); wanted.last = std::max(wanted.last, r.last); }
    };
    for (const auto &e : g_g144List)
    {
        add(g144TargetRange(e));
        const auto depth = g254DepthSurface(e);
        if (depth.active) add(depth.range);
        if (e.prim.tme)
        { add(g144TextureRange(e)); if (g144IsPalettedPsm(e.ctx.tex0.psm)) add(g144ClutRange(e)); }
    }
    // Sweep only when the batch's own demand is unresolvable; otherwise audit exactly what this
    // batch is about to read. A full 512-page digest sweep per CPU batch costs ~390 ms/f and
    // stops the route reaching the scored window.
    if (unknown) g716AuditSweep(4); else g716AuditRange(wanted, 4);
    g716MaterializeForRanges(wanted, {}, unknown, false, kG261MatCpuReplay);
}

// The content stamp for a raw-decoded view: the generations of every page the decode reads.
// Both raw producers (g716RetireTarget and g716ExecuteL2l) already declare their writes through
// g178BumpRectImpl, so these move exactly when the source bytes change.
static uint64_t g716SourceGen(const G144BlockRange &tex, const G144BlockRange &clut)
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (const G144BlockRange *r : {&tex, &clut})
    {
        if (!r->valid) { mix(0xD1CEull); continue; }
        for (uint32_t p = r->first >> 5u; p <= std::min(511u, r->last >> 5u); ++p)
            mix(g_g178PageGen[p & (kG178PageCount - 1u)].load(std::memory_order_relaxed));
    }
    return h;
}

static uint64_t g716TryTexture(const G144Entry &e, const G178EntryState &st)
{
    if (!(s_g716ConsumeMask & 9u) || !g716AnyRaw() || !e.prim.tme || st.skip || !st.texKey ||
        st.texW <= 0 || st.texH <= 0)
        return 0;
    const auto tex = g144TextureRange(e);
    if (!g716RangeHasRaw(tex)) return 0;
    g716AuditRange(tex, 5);
    const uint32_t psm = e.ctx.tex0.psm;
    if (psm != GS_PSM_T8 && psm != GS_PSM_CT32 && psm != GS_PSM_CT24) return 0;
    // Split the texture consumer by decoder: bit0 = the T8+CLUT decode (G630), bit3 = the direct
    // CT32/CT24 raw view (G634). They share nothing but this entry point.
    if (!g716Consumes(psm == GS_PSM_T8 ? 1u : 8u)) return 0;
    // ⭐ OWNERSHIP: for a LIVE RENDER TARGET the FBO is the authority, not raw and not guest VRAM.
    // The shipped renderer binds such a texture's FBO directly -- no conversion at all. Routing it
    // through raw substitutes a decoded copy for that bind, and the copy cannot carry what the
    // direct bind carries (the target's own colour/alpha contract). All three bases the oracle
    // sampled -- 0x2720, 0x2b20, 0x2740 -- are exactly these surfaces, and every content oracle
    // passes on them because the bytes are not what differs.
    if (s_g716NoRtt && !s_g717LateTexture)
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            if (g_g261Res[t].range.valid && g144RangeOverlaps(g_g261Res[t].range, tex))
                return 0;
    // Merge newer native render rows before interpreting the physical bytes through TEX0.
    const bool prior = g_g716GpuObserver;
    g_g716GpuObserver = true;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty && g144RangeOverlaps(g_g261Res[t].range, tex))
            g261Materialize(t, kG261MatTexAlias);
    g_g716GpuObserver = prior;
    if (s_g716CpuView)
    {
        g716MaterializeForRanges(tex, g144ClutRange(e), false, false, kG261MatTexAlias);
        return 0;
    }
    // ⭐ The view key must name everything BAKED INTO the decoded RGBA image, not just where the
    // texels live. `st.texKey` is (tbp0, tbw, tw, th) alone -- no PSM and no CLUT component -- so
    // two draws that share texture geometry but differ in palette (the normal case for character
    // textures, which is exactly where the damage was) hash to one key, and the second draw binds
    // the first draw's decoded view with the wrong palette. The shipped G629 slot matcher pairs
    // `texKey` AND `clutKey` for this reason; the raw view dropped the second half.
    // Content oracles cannot see this: every individual decode IS correct, the BINDING is wrong.
    uint64_t ident = st.texKey;
    const auto identMix = [&ident](uint64_t v) { ident ^= v; ident *= 1099511628211ull; };
    identMix(psm);
    identMix(uint64_t(st.texW) << 32 | uint32_t(st.texH));
    if (g144IsPalettedPsm(psm))
    {
        identMix((uint64_t(e.ctx.tex0.cbp) << 32) | (uint64_t(e.ctx.tex0.csa) << 24) |
                 (uint64_t(e.ctx.tex0.cpsm) << 16) | (uint64_t(e.ctx.tex0.csm) << 8));
        identMix((uint64_t(e.texclut.cbw) << 16) | (uint64_t(e.texclut.cou) << 8) |
                 uint64_t(e.texclut.cov));
    }
    else
        identMix((uint64_t(e.texa.ta0) << 8) | uint64_t(e.texa.aem));
    const uint64_t key = g281ViewKey(ident) ^ 0x0016000000000000ull;
    // Stamp AFTER the resident-row merge above, so the generations named here are the ones the
    // decode below will actually read.
    const uint64_t gen = g716SourceGen(tex, g144ClutRange(e));
    if (!s_g716NoViewCache)
    {
        const auto it = g_g716Built.find(key);
        if (it != g_g716Built.end())
        {
            if (it->second == gen) { ++g_g716ViewHits; return key; }
            ++g_g716ViewStale;
        }
    }
    bool ok = false;
    if (psm == GS_PSM_T8)
    {
        const auto clut = g144ClutRange(e);
        // The established CPU CLUT helper is itself a real byte observer.
        g716MaterializeForRanges(clut, {}, !clut.valid, false, 8);
        const auto &map = g618P8Page();
        if (!map.ok) return 0;
        auto p = std::make_shared<G630T8Packet>();
        p->texKey = key; p->tbp = e.ctx.tex0.tbp0;
        p->tbw = std::max(1u, uint32_t(e.ctx.tex0.tbw) >> 1u);
        p->width = st.texW; p->height = st.texH;
        p->mapWords.assign(map.perm.begin(), map.perm.end());
        p->clut.resize(256);
        for (uint32_t c = 0; c < 256; ++c)
            p->clut[c] = g178LookupClutFixed(g_g144LastVram, e.texa, e.texclut, uint8_t(c),
                e.ctx.tex0.cbp, e.ctx.tex0.cpsm, e.ctx.tex0.csm, e.ctx.tex0.csa, e.ctx.tex0.psm);
        g716AppendGuestRange(tex, *p);
        if (s_g716Verify && (g_g716Views % 64u) == 0u)
        {
            auto shadow = g716ShadowMemory(tex);
            p->expected.resize(size_t(st.texW) * st.texH);
            for (int y = 0; y < st.texH; ++y)
                for (int x = 0; x < st.texW; ++x)
                    p->expected[size_t(y) * st.texW + x] = p->clut[
                        GSMem::ReadP8(shadow.data(), p->tbp, p->tbw, x, y) & 255u];
        }
        ok = g630_backend_prepare_t8_view(p);
    }
    else
    {
        auto p = std::make_shared<G634RawViewPacket>();
        p->texKey = key; p->tbp = e.ctx.tex0.tbp0; p->tbw = std::max(1u, uint32_t(e.ctx.tex0.tbw));
        p->width = st.texW; p->height = st.texH; p->psm = psm;
        p->ta0 = e.texa.ta0; p->aem = e.texa.aem;
        const auto &map = g714Geom(GS_PSM_CT32);
        p->pageWords.resize(2048);
        for (uint32_t i = 0; i < 2048; ++i) p->pageWords[i] = map.fwd[i] >> 2;
        // Drain and re-equalise immediately before snapshotting the reference, so a disagreement
        // cannot simply mean "raw moved on between submit and execute". Without this the oracle
        // reads 27.8% bad; with the per-flush mirror alone it reads 2.8%; what survives HERE is
        // the part that is not the instrument's own timing.
        if (s_g716Verify && (g_g716Views % 64u) == 0u) g716MirrorRawToGuest();
        g716AppendGuestRange(tex, *p);
        // The CT32/CT24 raw view was the ONE G716 consumer with no oracle at all, and the
        // consumer bisect showed it carries the whole systematic error (meanOfMeans 1.3440 alone
        // vs 1.3436 for every consumer together). This is G634's own reference, verbatim: the
        // SHIPPED row reader over guest VRAM plus the shipped CT24/TEXA rule. Run it with
        // DC2_G716_MIRROR=1, which holds guest VRAM equal to raw, and it is a genuine end-to-end
        // check of the TEX0 -> address -> TEXA chain rather than the raw image against itself.
        if (s_g716Verify && g_g144LastVram != nullptr && (g_g716Views % 64u) == 0u)
        {
            // G717 validates without altering live guest bytes or authority. The private shadow
            // drains prior raw work and overlays exactly those raw pages onto the guest snapshot.
            // The old MIRROR-based experiment remains available for the frozen G716 arm.
            const auto shadow = s_g717LateTexture ? g716ShadowMemory(tex) : std::vector<uint8_t>{};
            const uint8_t *reference = s_g717LateTexture ? shadow.data() : g_g144LastVram;
            p->expected.assign(size_t(st.texW) * st.texH, 0u);
            const uint32_t w = uint32_t(st.texW);
            for (int v = 0; v < st.texH; ++v)
            {
                uint32_t *const row = p->expected.data() + size_t(v) * w;
                // G718: the shipped row reader takes a non-const `u8 *` but only reads. The oracle
                // holds either a private shadow copy or the live VRAM base; neither is written here.
                GSMem::G512ReadRowCT32(const_cast<uint8_t *>(reference), p->tbp, p->tbw,
                                       uint32_t(v), w, row);
                if (p->psm == GS_PSM_CT24)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const bool rgbZero = (row[x] & 0x00FFFFFFu) == 0u;
                        const uint32_t a = (p->aem != 0u && rgbZero) ? 0u : (p->ta0 & 0xFFu);
                        row[x] = (row[x] & 0x00FFFFFFu) | (a << 24u);
                    }
            }
            p->verify = true;
        }
        ok = g634_backend_prepare_raw_view(p);
    }
    if (!ok) return 0;
    // Pin this decode to its submit position: the read below drains the queue, so the view is
    // decoded from the raw state the draw actually implies, not from a state later producers
    // have already advanced past.
    if (s_g716SyncView)
    {
        std::vector<uint32_t> drain;
        (void)g630_backend_read_vram_rows(0u, 1u, drain);
    }
    g_g716Built[key] = gen; ++g_g716Views;
    return key;
}

static void g716PrepareSeed(G178Batch &b, uint32_t fbw)
{
    b.g716SeedPages.clear(); b.g716SeedFbw = 0;
    if (!g716Consumes(2u) || s_g716CpuSeed || !g716AnyRaw() || !b.uploadFb || b.fbH != 512 ||
        b.g716SeedHi < b.g716SeedLo || dc2GetNativeScale() != 1) return;
    const auto range = g144RangeForRect(framePageBaseToBlock(b.fbp), GS_PSM_CT32,
                                        fbw, 0, b.g716SeedLo, b.fbW, b.g716SeedHi - b.g716SeedLo + 1);
    if (!g716RangeHasRaw(range)) return;
    for (uint32_t p = range.first >> 5u; p <= std::min(511u, range.last >> 5u); ++p)
        if (g_g716Raw[p]) { g716AuditPage(p, 6); b.g716SeedPages.push_back(p); }
    b.g716SeedFbw = fbw; ++g_g716Seeds;
}

static bool g716RestoreRawPage(int ti, uint32_t page)
{
    auto p = std::make_shared<G714L2lPacket>();
    p->g716Mode = 3;
    p->g716Protected.push_back(page);
    const G261Res &r = g_g261Res[ti];
    const int y = int((page - kG261Fbp[ti]) / r.fbw) * 32;
    g716PageMap(*p);
    p->fboOut = {int32_t(kG261Fbp[ti]), int32_t(r.fbw), r.fbW, y, y + 31};
    return g714_backend_submit_l2l(p);
}

static bool g716CanUpload(uint32_t block, uint32_t bw, uint32_t psm,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool unknown)
{
    if (!s_g716RawUpload || !g716AnyRaw() || unknown || !w || !h ||
        uint64_t(w) * h > 1048576u || !g714_backend_available()) return false;
    uint32_t word, mask;
    if (!g634UploadTexelBits(block, std::max(1u, bw), psm, x, y, word, mask)) return false;
    return g716RangeHasRaw(g144RangeForRect(block, psm, bw, x, y, w, h));
}
static void g716UploadComplete(uint8_t *vram, uint32_t block, uint32_t bw, uint32_t psm,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g716CanUpload(block, bw, psm, x, y, w, h, false)) return;
    // Only written bits cross to the GPU. Unchanged lanes never make a CPU round trip,
    // including the other nibble of PSMT4, CT24 alpha, and partial-page uploads.
    static std::vector<uint32_t> masks(1048576u, 0u);
    static std::vector<uint32_t> touched;
    touched.clear();
    for (uint32_t py = 0; py < h; ++py)
        for (uint32_t px = 0; px < w; ++px)
        {
            uint32_t word = 0, mask = 0;
            if (!g634UploadTexelBits(block, std::max(1u, bw), psm, x + px, y + py, word, mask) ||
                word >= 1048576u)
            { std::fprintf(stderr, "[G716:FAIL] upload address\n"); std::abort(); }
            if (!g_g716Raw[word >> 11u]) continue;
            if (!masks[word]) touched.push_back(word);
            masks[word] |= mask;
        }
    if (touched.empty()) return;
    auto p = std::make_shared<G634RawUploadPacket>();
    p->psm = psm;
    p->verify = s_g716Verify && ((g_g716Uploads % 64u) == 0u);
    p->ops.reserve(touched.size() * 3u);
    for (uint32_t word : touched)
    {
        uint32_t value; std::memcpy(&value, vram + word * 4u, 4u);
        p->ops.insert(p->ops.end(), {word, value, masks[word]});
        masks[word] = 0;
    }
    if (!g634_backend_submit_raw_upload(p))
    { std::fprintf(stderr, "[G716:FAIL] upload submit\n"); std::abort(); }
    ++g_g716Uploads; g_g716UploadWords += touched.size();
}
static void g716WriteEvent(uint32_t block, uint32_t bw, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t source)
{
    if (g_g716LogicalWrite || !g716AnyRaw()) return;
    const auto range = g144RangeForRect(block, static_cast<uint8_t>(psm), bw, x, y, w, h);
    if (!g716RangeHasRaw(range)) return;
    const uint32_t tag = std::min(31u, source & 255u);
    const uint64_t n = ++g_g716WriterCounts[tag];
    uint32_t rawPages = 0;
    for (uint32_t pg = range.first >> 5u; pg <= std::min(511u, range.last >> 5u); ++pg)
        rawPages += g_g716Raw[pg] != 0;
    g_g716WriterRawPages[tag] += rawPages;
    if (n <= 12 || n % 4096 == 0)
        std::fprintf(stderr, "[G716:writer] source=%u n=%llu block=%x bw=%u psm=%x xy=%u,%u wh=%u,%u raw=%u observer=%d\n",
            source, (unsigned long long)n, block, bw, psm, x, y, w, h, rawPages, int(g_g716GpuObserver));
}
static struct G716InstallUpload
{
    G716InstallUpload()
    {
        if (s_g716Authority && (s_g716Verify || s_g716Audit)) g_g716WriteHook = g716WriteEvent;
        if (s_g716Authority && s_g716RawUpload)
            g_g630UploadCompleteHook = g716UploadComplete;
    }
} g_g716InstallUpload;
