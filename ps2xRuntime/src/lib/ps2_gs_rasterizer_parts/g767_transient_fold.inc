// ===== G767: TRANSIENT -> RESIDENT FOLD — a transient target is a temporary GPU VIEW of shared
// =====       GS pages; its result is folded GPU->GPU into the resident authority, never read back =====
//
// THE WALL (plans/phase-G767-ledger.md §1). After G766, the dominant remaining GPU->CPU crossing on
// every uncapped route is the TRANSIENT contract's own colour readback. G766 already removed its
// prologue (the seed comes from GPU page pulls, g766_mask_mirror.inc) but kept its epilogue:
//
//   render transient -> glReadPixels (drains the whole GL queue) -> unpack into guest VRAM ->
//   bump page generations -> VRAM->FBO mirror of the SAME rows into every dirty resident alias
//
// i.e. GPU -> CPU -> GPU for bytes whose only consumers are other GPU surfaces.
//
// THE FOLD. For a GPU-seeded transient (G766 armed it: every dirty resident overlapping it passed the
// shape test and its seed is exact), every page of the transient surface inside the rows the batch
// can WRITE (G621's write span) is copied GPU->GPU, as page-tile (or row-band) rects, from the
// transient FBO into EVERY dirty resident FBO whose layout holds that page over FBO-defined rows. The
// render item applies the copies right after its last draw (g767_post_draw_fold.inc), and the batch
// reads nothing back. Front-end bookkeeping is exactly a resident WAVE's for those rows: the resident
// stays dirty (its FBO is newer than guest VRAM over them), the page-owner protocol records it as the
// page's newest owner, G291's upload coverage and G280's overlays / G310's page generations see a
// producer write. Guest VRAM is NOT written and page generations do NOT move: the obligation is
// carried by the residency record and discharged only at a real CPU observer (g261Materialize at a
// CPU replay, an l2h transfer, a CPU-visible texture decode), exactly as for a wave.
//
// The transient itself retains nothing: its snapshot is invalidated, so its next flush re-seeds from
// guest VRAM + owner pulls (a temporary view of the pages, re-created each time).
//
// EXACT BY CONSTRUCTION (same argument as G766's pulls): CT32 page geometry is layout independent,
// every colour FBO carries the same integer scale S, transients and residents both store raw-alpha
// RGBA8 (G255). The copied texels are exactly the ones the legacy path read back, unpacked, and
// mirrored — minus the S>1 resolve/replicate the CPU round trip used to impose.
//
// Fail-closed. A page with NO eligible resident home, a transient with guest depth (its post-submit
// Z paths can still fall back to the CPU, which must not find the result already folded), a G289
// owner consume, an oracle-armed readback, or a fire-and-forget submit: the legacy readback runs.
// A fold that does not land (the backend validates every rect before issuing any copy, so nothing
// lands) sends the batch to its CPU fallback with every resident untouched.
//
//   DC2_G767_NO_FOLD=1       rollback: every transient reads back (G766 behaviour)
//   DC2_G767_NO_NARROWTRN=1  rollback of the G604 width widening that the fold makes profitable
//   DC2_G766_STAT=1 / DC2_G767_STAT=1   [G767:fold] census

extern bool g767_backend_arm_folds(uint32_t srcKey, const std::vector<int32_t> &rects7);
extern bool g767_backend_last_folds_applied();
extern void g767_backend_report();

static bool g767FoldOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_NO_FOLD") && !envFlagDisabled("DC2_G767_FOLD");
    return on;
}

static bool g767FoldDepthOn()
{
    static const bool on = g767FoldOn() && !envFlagEnabled("DC2_G767_NO_FOLDZ") &&
                           !envFlagDisabled("DC2_G767_FOLDZ");
    return on;
}

static bool g767StatOn()
{
    static const bool on = envFlagEnabled("DC2_G767_STAT") || g766StatOn();
    return on;
}

struct G767FoldHit
{
    uint8_t slot = 0u;
    uint16_t page = 0u;
    int16_t y0 = 0, y1 = -1; // GS rows in the SLOT's layout
    uint8_t col = 0u;        // page column in the slot's layout
    bool primary = false;    // the hit that becomes the page's owner
    bool clean = false;      // the home was CLEAN: the fold opens its dirty epoch
    bool seed = false;       // G769 tier 3: the clean home's page row is re-seeded from VRAM first
};

struct G767FoldState
{
    bool armed = false;
    uint32_t fbp = 0u, fbw = 0u;
    int lo = 512, hi = -1;
    std::vector<int32_t> rects7;
    std::vector<G767FoldHit> hits;
    uint16_t seedRows[kG248TargetCount] = {}; // G769 tier 3: page rows to re-seed, per slot
};
static G767FoldState s_g767Fold; // flush-owner thread only

static uint64_t g_g767Arms = 0u, g_g767Commits = 0u, g_g767Fail = 0u, g_g767Pages = 0u,
    g_g767Hits = 0u, g_g767Rows = 0u, g_g767RefUncovered = 0u, g_g767RefMirror = 0u,
    g_g767RefGuard = 0u, g_g767RetireViews = 0u;
static uint64_t g_g767GpuSeeds = 0u, g_g767GpuSeedMiss = 0u;
static uint64_t g_g767CleanHomes = 0u, g_g767CleanRefused = 0u;
static uint64_t g_g769SeedHomes = 0u, g_g769SeedRows = 0u, g_g769SeedRefused = 0u,
    g_g769SeedFail = 0u;

// ===== ORACLE (DC2_G767_FOLD_VERIFY=1, perturbing, never timed) =====
// After a committed fold, read every folded band back from BOTH the transient FBO and each home FBO
// and compare bytewise. Proves the page geometry (transient page (pr,pc) -> slot page (row,col)),
// the row-band arithmetic and the all-or-nothing application.
static bool g767FoldVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G767_FOLD_VERIFY");
    return on;
}
static uint64_t g_g767VerifyBands = 0u, g_g767VerifyBad = 0u, g_g767VerifyReadFail = 0u;
static void g767VerifyFold(uint32_t fbp, uint32_t fbw, const std::vector<G767FoldHit> &hits)
{
    static std::vector<uint32_t> s_a, s_b;
    const int fbW = static_cast<int>(fbw) * 64;
    for (const G767FoldHit &h : hits)
    {
        const uint32_t t = h.slot;
        const G261Res &r = g_g261Res[t];
        const uint32_t off = h.page - fbp;
        const uint32_t pr = off / fbw, pc = off % fbw;
        const int rows = h.y1 - h.y0 + 1;
        const int inPage = h.y0 % 32; // the same within-page offset on both sides
        const int srcGsRow = static_cast<int>(pr) * 32 + inPage;
        const int srcGlY = 512 - (srcGsRow + rows);
        const int dstGlY = 512 - (h.y1 + 1);
        if (!g178_backend_read_color(fbp, fbW, 512, srcGlY, rows, s_a) ||
            !g178_backend_read_color(g766KeyReadFbp(g766SlotKey(t)), r.fbW, 512, dstGlY, rows,
                                     s_b))
        {
            ++g_g767VerifyReadFail;
            continue;
        }
        uint64_t bad = 0u;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < 64; ++x)
            {
                const size_t ia = static_cast<size_t>(y) * fbW + pc * 64u + static_cast<size_t>(x);
                const size_t ib =
                    static_cast<size_t>(y) * r.fbW + h.col * 64u + static_cast<size_t>(x);
                if (ia >= s_a.size() || ib >= s_b.size() || s_a[ia] != s_b[ib])
                    ++bad;
            }
        ++g_g767VerifyBands;
        if (bad != 0u)
        {
            g_g767VerifyBad += bad;
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr,
                             "[G767:verify-BAD] trn=0x%03x fbw=%u page=0x%x -> slot %u (0x%03x "
                             "fbw=%u rows %d..%d col %u) bad=%llu\n",
                             fbp, fbw, h.page, t, kG261Fbp[t], r.fbw, h.y0, h.y1, h.col,
                             (unsigned long long)bad);
        }
    }
    static uint32_t s_n = 0u;
    if ((++s_n % 64u) == 1u)
        std::fprintf(stderr, "[G767:verify] bands=%llu bad=%llu readFail=%llu\n",
                     (unsigned long long)g_g767VerifyBands, (unsigned long long)g_g767VerifyBad,
                     (unsigned long long)g_g767VerifyReadFail);
}

// ===== GPU-ONLY SEED: a transient view whose every rendered page has a resident owner =====
// G766 seeds a transient by uploading its rows from guest VRAM and then pulling the pages a valid
// resident owner holds newer. After a fold every page the transient rendered is OWNED by a resident
// (the fold made it so), so on the transient's next flush the upload is pure overwrite inside the
// rendered page rows: every byte it writes there is replaced by a pull. Skip it — no CPU pack, no
// texture upload — when every page of the batch's page rows has a valid owner the pulls can locate.
//   DC2_G767_NO_GPUSEED=1  rollback: the seed upload always runs
static bool g767GpuSeedOn()
{
    static const bool on = g767FoldOn() && !envFlagEnabled("DC2_G767_NO_GPUSEED") &&
                           !envFlagDisabled("DC2_G767_GPUSEED");
    return on;
}
static bool g767GpuSeedCovers(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi)
{
    if (!g767GpuSeedOn() || !g766TransientArmed(fbp) || fbw == 0u || fbw > 8u || rowHi < rowLo ||
        rowLo < 0 || rowHi > 511)
        return false;
    for (int pr = rowLo / 32; pr <= rowHi / 32; ++pr)
        for (uint32_t pc = 0u; pc < fbw; ++pc)
        {
            const uint32_t page = fbp + static_cast<uint32_t>(pr) * fbw + pc;
            uint32_t srow = 0u, scol = 0u;
            if (page >= kG178PageCount)
            {
                ++g_g767GpuSeedMiss;
                return false;
            }
            const int o = g766ValidOwner(page);
            if (o >= 0 && kG261Fbp[o] != fbp &&
                g766SlotPagePos(static_cast<uint32_t>(o), page, srow, scol))
                continue; // the owner pull seeds it
            if (o >= 0)
            {
                ++g_g767GpuSeedMiss;
                return false;
            }
            // No FBO owner: guest VRAM is newest; a dirty resident whose copy is current seeds it.
            bool held = false;
            for (uint32_t t = 0u; t < kG248TargetCount && !held; ++t)
                held = g_g261Res[t].dirty && kG261Fbp[t] != fbp && g767UnownedPageCurrent(t, page) &&
                       g766SlotPagePos(t, page, srow, scol);
            if (!held)
            {
                ++g_g767GpuSeedMiss;
                return false;
            }
        }
    ++g_g767GpuSeeds;
    s_g766Trn.gpuSeed = true;
    return true;
}

static void g767Report(bool force)
{
    if (!g767StatOn())
        return;
    static uint64_t s_n = 0u;
    if (!force && (++s_n % 256u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G767:fold] arms=%llu commits=%llu fail=%llu pages=%llu hits=%llu rows=%llu "
                 "refuse(uncovered=%llu mirror=%llu guard=%llu) retired=%llu gpuSeed=%llu/%llu "
                 "unowned(ok=%llu ref=%llu) clean(homes=%llu refused=%llu) "
                 "seed(homes=%llu rows=%llu refused=%llu fail=%llu)\n",
                 (unsigned long long)g_g767Arms, (unsigned long long)g_g767Commits,
                 (unsigned long long)g_g767Fail, (unsigned long long)g_g767Pages,
                 (unsigned long long)g_g767Hits, (unsigned long long)g_g767Rows,
                 (unsigned long long)g_g767RefUncovered, (unsigned long long)g_g767RefMirror,
                 (unsigned long long)g_g767RefGuard, (unsigned long long)g_g767RetireViews,
                 (unsigned long long)g_g767GpuSeeds,
                 (unsigned long long)(g_g767GpuSeeds + g_g767GpuSeedMiss),
                 (unsigned long long)g_g767UnownedOk, (unsigned long long)g_g767UnownedRef,
                 (unsigned long long)g_g767CleanHomes, (unsigned long long)g_g767CleanRefused,
                 (unsigned long long)g_g769SeedHomes, (unsigned long long)g_g769SeedRows,
                 (unsigned long long)g_g769SeedRefused, (unsigned long long)g_g769SeedFail);
    g767_backend_report();
}

static void g767FoldReset()
{
    s_g767Fold.armed = false;
    s_g767Fold.rects7.clear();
    s_g767Fold.hits.clear();
    for (uint16_t &m : s_g767Fold.seedRows)
        m = 0u;
}

static bool g767FoldArmed(uint32_t fbp)
{
    return s_g767Fold.armed && s_g767Fold.fbp == fbp;
}

// Append {dstKey, sx, sy, dx, dy, w, h}, merging a horizontal continuation of the previous run.
static inline void g767AppendRect(std::vector<int32_t> &rects7, uint32_t dstKey, int32_t sx,
                                  int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h)
{
    const size_t n = rects7.size();
    if (n >= 7u && static_cast<uint32_t>(rects7[n - 7u]) == dstKey && rects7[n - 5u] == sy &&
        rects7[n - 3u] == dy && rects7[n - 1u] == h && rects7[n - 6u] + rects7[n - 2u] == sx &&
        rects7[n - 4u] + rects7[n - 2u] == dx)
    {
        rects7[n - 2u] += w;
        return;
    }
    rects7.insert(rects7.end(), {static_cast<int32_t>(dstKey), sx, sy, dx, dy, w, h});
}

// ===== Tier 2: a CLEAN resident as a fold home =====
// When no dirty resident holds a page (dungeon1: 0x13b / 0x13f / 0x14b render inside 0x139's pages
// while 0x139 itself is clean), the fold may land in a CLEAN resident whose FBO is PROVABLY equal to
// guest VRAM over every row it defines: its colour snapshot is valid over its whole coverage and no
// page of that coverage has been written since the snapshot was taken (G591's per-page generations).
// The fold then opens the slot's dirty epoch exactly as a wave would (the wave's upload decision
// establishes the same "FBO current over cov" invariant that dirty residents are trusted with).
//   DC2_G767_NO_CLEANHOME=1  rollback: clean residents are never fold homes
static bool g767CleanHomeOn()
{
    static const bool on = g767FoldOn() && !envFlagEnabled("DC2_G767_NO_CLEANHOME") &&
                           !envFlagDisabled("DC2_G767_CLEANHOME");
    return on;
}
static bool g767CleanHomeCurrent(uint32_t t)
{
    if (!g767CleanHomeOn() || t >= kG248TargetCount || !g591ColTracked())
        return false;
    const G261Res &r = g_g261Res[t];
    if (r.dirty || r.fbw == 0u || r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64 ||
        r.covHi < r.covLo || r.covLo < 0 || r.covHi > 511)
        return false;
    const uint32_t key = g766SlotSnapKey(t);
    const auto sit = g_g178FbSnap.find(key);
    if (sit == g_g178FbSnap.end() || !sit->second.valid || sit->second.rowLo > r.covLo ||
        sit->second.rowHi < r.covHi)
    {
        ++g_g767CleanRefused;
        return false;
    }
    const auto ait = g_g591ColAuth.find(key);
    if (ait == g_g591ColAuth.end() || !ait->second.valid)
    {
        ++g_g767CleanRefused;
        return false;
    }
    const G591ColAuth &a = ait->second;
    if (a.page != kG261Fbp[t] || a.fbw != r.fbw || a.rowLo > r.covLo || a.rowHi < r.covHi)
    {
        ++g_g767CleanRefused;
        return false;
    }
    for (int pr = r.covLo / 32; pr <= r.covHi / 32; ++pr)
        for (uint32_t pc = 0u; pc < r.fbw; ++pc)
        {
            const uint32_t sIdx = static_cast<uint32_t>(pr) * r.fbw + pc;
            if (sIdx >= kG591ColPages ||
                a.gen[sIdx] != g_g178PageGen[(a.page + sIdx) & (kG178PageCount - 1u)].load(
                                   std::memory_order_relaxed))
            {
                ++g_g767CleanRefused;
                return false;
            }
        }
    return true;
}

// Is resident slot t a valid fold home for page `page`, GS rows [pageY0+y0, pageY0+y1] of it?
// ===== G769 TIER 3: a clean resident that is NOT current, re-seeded from guest VRAM =====
// ridepod: every "no home" refusal (2,513 / 60 s, each a synchronous transient read-back) was a page
// the CLEAN wide 0x139 slot holds but whose colour snapshot had gone stale (CPU replays and uploads
// wrote other pages of its window since). Tier 2 needs the whole coverage current; the fold only
// needs the PAGE ROWS it lands in. So re-seed exactly those page rows (full slot width) from guest
// VRAM - an upload, never a read-back - shrink the slot's coverage to them, and fold. Admitted only
// where guest VRAM IS the newest copy of every page of those rows: no valid FBO owner (after any
// pending resident mirror is applied), no pending display / deferred depth / G289 owner over them,
// no GPU-resident guest depth aliasing them. One seeded home per page (the first admissible slot).
//   DC2_G769_SEEDHOME=1      opt-in (default OFF, see g769SeedHomeOn)
static bool g767OtherAuthorityHits(const G144BlockRange &r);
static bool g769SeedHomeOn()
{
    // ⛔ DEFAULT OFF (opt-in DC2_G769_SEEDHOME=1): on ridepod it traded 2,513 transient read-backs
    //    for 214 escaped writers on the seeded wide 0x139 and a slower frame (g769c/d, ledger §3):
    //    a dirty slot whose coverage is one seeded band forces publish-before-wave on its next
    //    full-height render. The page-store form (no resident coverage change) is the successor.
    static const bool on = g767CleanHomeOn() && envFlagEnabled("DC2_G769_SEEDHOME");
    return on;
}
// Page row `pr` of clean slot t can be re-seeded from guest VRAM.
static bool g769SeedRowAdmits(uint32_t t, uint32_t pr)
{
    const G261Res &r = g_g261Res[t];
    if (r.dirty || r.fbw == 0u || r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64 || pr >= 16u)
        return false;
    const uint32_t first = kG261Fbp[t] + pr * r.fbw;
    if (first + r.fbw > kG178PageCount)
        return false;
    for (uint32_t pc = 0u; pc < r.fbw; ++pc)
    {
        const uint32_t page = first + pc;
        uint32_t row = 0u, col = 0u;
        for (uint32_t u = 0u; u < kG248TargetCount; ++u)
            if (g_g261Res[u].dirty && g_g261Res[u].mirPending && g766SlotPagePos(u, page, row, col))
                g766MirrorFlushOrDrop(static_cast<int>(u));
        if (g766ValidOwner(page) >= 0)
            return false;
    }
    const G144BlockRange rg{first * 32u, (first + r.fbw) * 32u - 1u, true};
    if (g767OtherAuthorityHits(rg))
        return false;
    if (g242WriteOverlapsDirtyDepth(first * 32u, r.fbw, GS_PSM_CT32, 0u, 0u,
                                    static_cast<uint32_t>(r.fbW), 32u))
        return false;
    return true;
}
// Seed every recorded page-row band of every tier-3 home (their hull, so the slot's coverage stays
// one contiguous row range). False = nothing usable: the caller refuses the fold.
static bool g769SeedHomes()
{
    uint8_t *vram = g_g144LastVram;
    const G765InvPage &inv = g765InvPage();
    static std::vector<uint32_t> s_px; // flush-owner thread only
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const uint16_t m = s_g767Fold.seedRows[t];
        if (m == 0u)
            continue;
        G261Res &r = g_g261Res[t];
        int prLo = 16, prHi = -1;
        for (int pr = 0; pr < 16; ++pr)
            if ((m >> pr) & 1u)
            {
                prLo = std::min(prLo, pr);
                prHi = std::max(prHi, pr);
            }
        if (vram == nullptr || !inv.valid || prHi < prLo || r.dirty)
        {
            ++g_g769SeedFail;
            return false;
        }
        for (int pr = prLo; pr <= prHi; ++pr)
            if (((m >> pr) & 1u) == 0u && !g769SeedRowAdmits(t, static_cast<uint32_t>(pr)))
            {
                ++g_g769SeedRefused;
                return false; // a row between two seeded rows is not VRAM-current
            }
        const int lo = prLo * 32, hi = prHi * 32 + 31, rows = hi - lo + 1;
        const uint32_t base = kG261Fbp[t];
        if (static_cast<uint64_t>(base + static_cast<uint32_t>(prHi + 1) * r.fbw) * 8192ull >
            g_g261VramSize)
        {
            ++g_g769SeedFail;
            return false;
        }
        s_px.assign(static_cast<size_t>(r.fbW) * rows, 0u);
        for (int pr = prLo; pr <= prHi; ++pr)
            for (uint32_t pc = 0u; pc < r.fbw; ++pc)
            {
                const uint8_t *src =
                    vram + static_cast<size_t>(base + static_cast<uint32_t>(pr) * r.fbw + pc) * 8192u;
                for (uint32_t w = 0u; w < 2048u; ++w)
                {
                    const int gx = static_cast<int>(pc) * 64 + inv.x[w];
                    const int gy = pr * 32 + inv.y[w];
                    std::memcpy(&s_px[static_cast<size_t>(hi - gy) * r.fbW + gx], src + w * 4u, 4u);
                }
            }
        // GL rows are bottom-first: buffer row 0 is GS row `hi`.
        if (g769AsyncMirOn()
                ? !g769_backend_write_color_rect_async(kG261Fbp[t], r.fbW, 512, 0, 511 - hi, r.fbW,
                                                       rows, std::vector<uint32_t>(s_px))
                : !g264_backend_write_color_rect(kG261Fbp[t], r.fbW, 512, 0, 511 - hi, r.fbW, rows,
                                                 s_px))
        {
            ++g_g769SeedFail;
            return false;
        }
        // The slot's FBO now equals guest VRAM over exactly these rows; nothing else is claimed.
        r.covLo = lo;
        r.covHi = hi;
        G178FbSnap &snap = g_g178FbSnap[g766SlotSnapKey(t)];
        snap.valid = false;
        snap.genSum = 0;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        ++g_g769SeedHomes;
        g_g769SeedRows += static_cast<uint64_t>(rows);
    }
    return true;
}

static bool g767SlotTakes(uint32_t t, uint32_t page, int y0, int y1, uint32_t transientKey,
                          uint32_t &row, uint32_t &col)
{
    const G261Res &r = g_g261Res[t];
    if (!r.dirty || r.fbw == 0u || r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64)
        return false;
    if (g766SlotKey(t) == transientKey || kG261Fbp[t] == transientKey)
        return false;
    if (!g766SlotPagePos(t, page, row, col))
        return false;
    const int ty0 = static_cast<int>(row) * 32 + y0;
    const int ty1 = static_cast<int>(row) * 32 + y1;
    // Only rows the slot's FBO defines: the copied band then lands in a surface whose every other
    // texel is already current, so claiming the rows dirty publishes nothing stale.
    if (r.covHi < r.covLo || ty0 < r.covLo || ty1 > r.covHi)
        return false;
    // G536: guest VRAM is newer than this FBO for the page (an unmirrored guest write). The slot's
    // publication would preserve those guest bytes over the fold; never claim such a page. A write
    // still sitting in the slot's LAZY mirror reads as guest-owned until the mirror re-anchors the
    // page — apply it first (the fold must land after it anyway).
    if (g536GuestOwnsTargetPage(r, kG261Fbp[t], row, col))
    {
        if (!r.mirPending)
            return false;
        g766MirrorFlushOrDrop(static_cast<int>(t));
        const G261Res &r2 = g_g261Res[t];
        if (!r2.dirty || g536GuestOwnsTargetPage(r2, kG261Fbp[t], row, col))
            return false;
    }
    return true;
}

// Census of a page with no home (first few, DC2_G767_STAT): why each dirty slot declined it.
static void g767LogNoHome(uint32_t fbp, uint32_t fbw, uint32_t page, int y0, int y1,
                          uint32_t transientKey)
{
    if (!g767StatOn())
        return;
    static uint32_t s_n = 0u;
    if (s_n++ >= 24u)
        return;
    char buf[640];
    int off = 0;
    for (uint32_t t = 0u; t < kG248TargetCount && off < 560; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty)
            continue;
        uint32_t row = 0u, col = 0u;
        const bool pos = g766SlotPagePos(t, page, row, col);
        const bool go = pos && g536GuestOwnsTargetPage(r, kG261Fbp[t], row, col);
        off += std::snprintf(buf + off, sizeof(buf) - off,
                             " [s%u 0x%03x fbw=%u fbW=%d pos=%d r%u c%u cov=%d..%d go=%d mir=%d key=%d]",
                             t, kG261Fbp[t], r.fbw, r.fbW, pos ? 1 : 0, row, col, r.covLo, r.covHi,
                             go ? 1 : 0, r.mirPending ? 1 : 0,
                             (g766SlotKey(t) == transientKey || kG261Fbp[t] == transientKey) ? 1 : 0);
    }
    std::fprintf(stderr, "[G767:nohome] trn=0x%03x fbw=%u page=0x%x rows=+%d..%d%s\n", fbp, fbw,
                 page, y0, y1, off > 0 ? buf : " (no dirty slot)");
}

// Plan the fold of transient (fbp, fbw) rows [lo, hi]. Fills s_g767Fold.rects7 / hits.
// Returns 0 = planned, 1 = a page has no home, 2 = guard.
static int g767Plan(uint32_t fbp, uint32_t fbw, uint32_t transientKey, int lo, int hi,
                    uint32_t &slotMask)
{
    s_g767Fold.rects7.clear();
    s_g767Fold.hits.clear();
    slotMask = 0u;
    if (fbw == 0u || fbw > 8u || hi < lo || lo < 0 || hi > 511)
        return 2;
    const int prLo = lo / 32;
    const int prHi = hi / 32;
    for (int pr = prLo; pr <= prHi; ++pr)
    {
        if (pr >= 16)
            return 2;
        const int y0 = std::max(lo, pr * 32) - pr * 32; // within-page rows
        const int y1 = std::min(hi, pr * 32 + 31) - pr * 32;
        const int32_t h = y1 - y0 + 1;
        // Logical GL rows are bottom-first: GS row R lives at GL row 511 - R.
        const int32_t sy = 511 - (pr * 32 + y1);
        for (uint32_t pc = 0u; pc < fbw; ++pc)
        {
            const uint32_t page = fbp + static_cast<uint32_t>(pr) * fbw + pc;
            if (page >= kG178PageCount)
                return 2;
            const int owner = g766ValidOwner(page);
            const size_t firstHit = s_g767Fold.hits.size();
            int primaryIdx = -1;
            for (int pass = 0; pass < 3 && primaryIdx < 0; ++pass)
            for (uint32_t t = 0u; t < kG248TargetCount && !(pass == 2 && primaryIdx >= 0); ++t)
            {
                uint32_t row = 0u, col = 0u;
                bool clean = false;
                bool seed = false;
                if (pass == 0)
                {
                    if (!g767SlotTakes(t, page, y0, y1, transientKey, row, col))
                        continue;
                }
                else if (pass == 2)
                {
                    // G769 tier 3 (only for a page neither a dirty nor a current clean slot holds).
                    const G261Res &rc = g_g261Res[t];
                    if (!g769SeedHomeOn() || rc.dirty || g766SlotKey(t) == transientKey ||
                        kG261Fbp[t] == transientKey || !g766SlotPagePos(t, page, row, col))
                        continue;
                    if ((s_g767Fold.seedRows[t] & (1u << row)) == 0u &&
                        !g769SeedRowAdmits(t, row))
                        continue;
                    clean = true;
                    seed = true;
                    s_g767Fold.seedRows[t] =
                        static_cast<uint16_t>(s_g767Fold.seedRows[t] | (1u << row));
                }
                else
                {
                    // Tier 2 (only for a page no dirty resident holds).
                    const G261Res &rc = g_g261Res[t];
                    if (rc.dirty || g766SlotKey(t) == transientKey || kG261Fbp[t] == transientKey ||
                        !g766SlotPagePos(t, page, row, col))
                        continue;
                    const int cy0 = static_cast<int>(row) * 32 + y0;
                    const int cy1 = static_cast<int>(row) * 32 + y1;
                    if (cy0 < rc.covLo || cy1 > rc.covHi || !g767CleanHomeCurrent(t))
                        continue;
                    clean = true;
                }
                G767FoldHit hit;
                hit.slot = static_cast<uint8_t>(t);
                hit.page = static_cast<uint16_t>(page);
                hit.y0 = static_cast<int16_t>(static_cast<int>(row) * 32 + y0);
                hit.y1 = static_cast<int16_t>(static_cast<int>(row) * 32 + y1);
                hit.col = static_cast<uint8_t>(col);
                hit.clean = clean;
                hit.seed = seed;
                if (static_cast<int>(t) == owner || primaryIdx < 0)
                    primaryIdx = static_cast<int>(s_g767Fold.hits.size());
                s_g767Fold.hits.push_back(hit);
                slotMask |= 1u << t;
                g767AppendRect(s_g767Fold.rects7, g766SlotKey(t),
                               static_cast<int32_t>(pc * 64u), sy,
                               static_cast<int32_t>(col * 64u), 511 - hit.y1, 64, h);
            }
            if (primaryIdx < 0)
            {
                g767LogNoHome(fbp, fbw, page, y0, y1, transientKey);
                return 1; // no resident home for this page: the legacy readback must publish it
            }
            // The current valid owner (if it is a home) keeps ownership; otherwise the first home.
            s_g767Fold.hits[static_cast<size_t>(primaryIdx)].primary = true;
            (void)firstHit;
        }
    }
    return 0;
}

// Before the synchronous submit of a G766-armed transient: plan + arm the fold. True = armed; the
// caller must then make the batch skip its colour readback.
static bool g767FoldArm(uint32_t fbp, uint32_t fbw, uint32_t fboKey, int lo, int hi)
{
    g767FoldReset();
    if (!g767FoldOn() || !g766TransientArmed(fbp) || !g766PullsAllowed())
        return false;
    uint32_t mask = 0u;
    int rc = g767Plan(fbp, fbw, fboKey, lo, hi, mask);
    if (rc == 0 && mask != 0u)
    {
        // Every home must absorb its own deferred guest-write mirror BEFORE the fold lands on it
        // (an older guest write must not be patched over the newer transient bytes later). A
        // failed mirror drops that slot's residency (G264's loud path) — plan again without it.
        bool dropped = false;
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
            if ((mask & (1u << t)) != 0u && g_g261Res[t].mirPending)
            {
                g766MirrorFlushOrDrop(static_cast<int>(t));
                if (!g_g261Res[t].dirty || g_g261Res[t].mirPending)
                    dropped = true;
            }
        if (dropped)
        {
            ++g_g767RefMirror;
            uint32_t mask2 = 0u;
            rc = g767Plan(fbp, fbw, fboKey, lo, hi, mask2);
            for (uint32_t t = 0u; rc == 0 && t < kG248TargetCount; ++t)
                if ((mask2 & (1u << t)) != 0u && g_g261Res[t].mirPending)
                    rc = 2; // a new home appeared with a pending mirror: do not chase it
            mask = mask2;
        }
    }
    if (rc != 0 || mask == 0u || s_g767Fold.rects7.empty())
    {
        if (rc == 1)
            ++g_g767RefUncovered;
        else
            ++g_g767RefGuard;
        g767FoldReset();
        g767Report(false);
        return false;
    }
    // G769 tier 3: the seeded homes' page rows are uploaded from guest VRAM FIRST (FIFO: before the
    // batch whose post-draw fold lands in them). Any failure refuses the whole fold (read-back).
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (s_g767Fold.seedRows[t] != 0u)
        {
            if (!g769SeedHomes())
            {
                ++g_g767RefUncovered;
                g767FoldReset();
                g767Report(false);
                return false;
            }
            break;
        }
    if (!g767_backend_arm_folds(fboKey, s_g767Fold.rects7))
    {
        ++g_g767RefGuard;
        g767FoldReset();
        return false;
    }
    s_g767Fold.armed = true;
    s_g767Fold.fbp = fbp;
    s_g767Fold.fbw = fbw;
    s_g767Fold.lo = lo;
    s_g767Fold.hi = hi;
    ++g_g767Arms;
    return true;
}

static bool g767FoldCommitHits();
// After the submit returned. False = the fold did not land (nothing landed): the caller abandons
// the GPU result and takes the CPU fallback. True = committed: the residents now carry the rows.
static bool g767FoldConfirmCommit()
{
    if (!s_g767Fold.armed)
        return true;
    if (!g767_backend_last_folds_applied())
    {
        ++g_g767Fail;
        static uint32_t s_log = 0u;
        if (s_log++ < 16u)
            std::fprintf(stderr, "[G767:fold-miss] transient 0x%03x fold did not land; CPU fallback\n",
                         s_g767Fold.fbp);
        g767FoldReset();
        return false;
    }
    return g767FoldCommitHits();
}

// The resident bookkeeping of a landed fold (shared by the transient fold and G769's owner fold):
// clean homes open their dirty epoch, every home carries the folded rows exactly as a wave would,
// the primary home becomes each page's owner.
static bool g767FoldCommitHits()
{
    // Tier 2: a clean home opens its dirty epoch first (a wave's clean -> dirty transition), so the
    // ownership stamps below carry the new epoch and the window re-anchors over current bytes.
    uint32_t opened = 0u;
    for (const G767FoldHit &h : s_g767Fold.hits)
    {
        const uint32_t t = h.slot;
        if (!h.clean || (opened & (1u << t)) != 0u)
            continue;
        opened |= 1u << t;
        G261Res &r = g_g261Res[t];
        if (r.dirty)
            continue; // cannot happen between arm and commit; nothing to open
        int lo = 512, hi = -1;
        for (const G767FoldHit &k : s_g767Fold.hits)
            if (k.slot == t)
            {
                lo = std::min(lo, static_cast<int>(k.y0));
                hi = std::max(hi, static_cast<int>(k.y1));
            }
        g716MarkRows(r, lo, hi); // before `dirty` flips, so an epoch starts from empty rows
        ++g_g766Epoch[t];
        r.dirty = true;
        r.dirtyLo = lo;
        r.dirtyHi = hi;
        g261UpdateWindow(static_cast<int>(t)); // FBO == guest VRAM over cov: re-anchor the window
        ++g_g767CleanHomes;
    }
    // Resident bookkeeping, per slot: exactly a wave's for the folded rows.
    uint32_t done = 0u;
    for (const G767FoldHit &h : s_g767Fold.hits)
    {
        const uint32_t t = h.slot;
        G261Res &r = g_g261Res[t];
        g716MarkRows(r, h.y0, h.y1);
        if (h.y0 < r.dirtyLo) r.dirtyLo = h.y0;
        if (h.y1 > r.dirtyHi) r.dirtyHi = h.y1;
        if (g291AtlasOn() && r.g291CovFbw != 0u)
        {
            // G291: the FBO is now newer than VRAM for every texel of the band — no upload covers it.
            for (int y = h.y0; y <= h.y1 && y < 512; ++y)
            {
                if (r.g291CovFbw == r.fbw && h.col < 8u)
                    r.g291Cov[static_cast<size_t>(y)][h.col] = 0u;
                else
                    r.g291Cov[static_cast<size_t>(y)].fill(0u);
            }
        }
        if ((done & (1u << t)) == 0u)
        {
            done |= 1u << t;
            g455NoteResidentWrite(static_cast<int>(t));
        }
        g280NoteFboContentChange(static_cast<int>(t), h.y0, h.y1);
        g310NoteProducerWrite(static_cast<int>(t), h.y0, h.y1);
        ++g_g767Hits;
        g_g767Rows += static_cast<uint64_t>(h.y1 - h.y0 + 1);
    }
    // Page ownership: the primary home becomes the page's newest owner; every other home has seen it.
    for (const G767FoldHit &h : s_g767Fold.hits)
    {
        if (!h.primary)
            continue;
        const uint32_t t = h.slot;
        G766PageOwn &o = g_g766Own[h.page];
        o.slot = static_cast<int8_t>(t);
        o.epoch = g_g766Epoch[t];
        o.stamp = ++g_g766Ctr;
        g766MarkSeen(t, h.page);
        ++g_g766Stamps;
        ++g_g767Pages;
    }
    for (const G767FoldHit &h : s_g767Fold.hits)
        if (!h.primary)
            g766MarkSeen(h.slot, h.page);
    ++g_g767Commits;
    if (g767FoldVerifyOn())
        g767VerifyFold(s_g767Fold.fbp, s_g767Fold.fbw, s_g767Fold.hits);
    g767FoldReset();
    g767Report(false);
    return true;
}

// The transient retains nothing: its FBO is a view that the next flush re-seeds.
static void g767RetireView(G178FbSnap &snap)
{
    snap.valid = false;
    snap.genSum = 0;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    ++g_g767RetireViews;
}

// G604's width contract (fbw == 8, or 2 under G660/G662) exists because the transient's upload /
// readback pair regressed on the wide population. With the readback folded away, every width a
// GPU-seeded transient can be (1..8 pages) is admitted.
static bool g767NarrowTransientOn()
{
    static const bool on = g767FoldOn() && !envFlagEnabled("DC2_G767_NO_NARROWTRN") &&
                           !envFlagDisabled("DC2_G767_NARROWTRN");
    return on;
}

// ===== G767: CT32 / CT24 PAGE VIEWS — a texture read of GPU-owned pages, assembled on the GPU =====
//
// The fold (above) leaves a transient's output where it was produced: in the resident FBOs that
// own its pages. The consumers of that output are texture reads with the TRANSIENT's geometry
// (e.g. lavaboss's display post-process samples 0x13d / 0x13b as CT32 tbw=2 STQ textures), which no
// resident FBO can bind directly (the direct bind needs the resident's own base + width, FST). The
// legacy answer published every overlapping resident — 0x139, 0x13a, 0x13c — to guest VRAM and
// decoded the texture from there: the readback the fold deleted, moved one edge downstream.
//
// A CT32 page is layout independent, so ANY CT32/CT24 texture whose base is page-aligned is the list
// of its pages, and each page is either owned by a resident FBO (valid G766 owner) or current in a
// dirty resident's FBO (G767 unowned-current rule). When every page of the texture is so held and at
// least one is FBO-owned (else guest VRAM is current and the cached decode is exact and cheaper),
// the backend gathers the pages into a logical texture of the texture's own size through the G675
// centre tap: bit-identical to publication + decode, with no GPU->CPU crossing. The consumer then
// samples it as an ordinary decoded texture (its own prim, STQ/FST, REPEAT/CLAMP and filter).
//   DC2_G767_NO_PAGEVIEW=1   rollback: the legacy publication + VRAM decode
extern bool g767_backend_prepare_ct32_view(uint64_t texKey, int texW, int texH, bool ct24,
                                           int ta0, int aem, const std::vector<int32_t> &rects7,
                                           const std::vector<uint32_t> &vramPixels);
// Pages no FBO holds are decoded from guest VRAM into the view (CPU unswizzle of one CT32 page).
//   DC2_G767_NO_VRAMPAGES=1  rollback: such a texture keeps the legacy publication + decode
static bool g767VramPagesOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_NO_VRAMPAGES");
    return on;
}
static bool g767PageViewOn()
{
    static const bool on = g766OwnerOn() && !envFlagEnabled("DC2_G767_NO_PAGEVIEW") &&
                           !envFlagDisabled("DC2_G767_PAGEVIEW");
    return on;
}
static uint64_t g_g767ViewTry = 0u, g_g767ViewBuilt = 0u, g_g767ViewNoOwned = 0u,
    g_g767ViewMixed = 0u, g_g767ViewShape = 0u, g_g767ViewFail = 0u, g_g767ViewPages = 0u,
    g_g767ViewReuse = 0u;
static void g767ViewReport()
{
    if (!g767StatOn())
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 512u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G767:view] try=%llu built=%llu pages=%llu noOwned=%llu mixed=%llu shape=%llu "
                 "fail=%llu reuse=%llu\n",
                 (unsigned long long)g_g767ViewTry, (unsigned long long)g_g767ViewBuilt,
                 (unsigned long long)g_g767ViewPages, (unsigned long long)g_g767ViewNoOwned,
                 (unsigned long long)g_g767ViewMixed, (unsigned long long)g_g767ViewShape,
                 (unsigned long long)g_g767ViewFail, (unsigned long long)g_g767ViewReuse);
}
static uint64_t g767ViewKey(uint32_t tbp0, uint32_t tbw, uint32_t psm, int texW, int texH, int ta0,
                            int aem)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t parts[7] = {tbp0, tbw, psm, static_cast<uint64_t>(texW),
                               static_cast<uint64_t>(texH), static_cast<uint64_t>(ta0 & 0xFF),
                               static_cast<uint64_t>(aem != 0 ? 1 : 0)};
    for (uint64_t p : parts)
    {
        h ^= p;
        h *= 0x100000001b3ull;
    }
    return 0xF767000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
}

// A page region that another GPU authority still owes to guest VRAM (display readback pending,
// deferred depth, a G289 copy owner, a G272 display wave): guest VRAM is not current there.
static bool g767OtherAuthorityHits(const G144BlockRange &r)
{
    if (!r.valid)
        return true;
    if (s_g276Pend.active)
    {
        const G144BlockRange d = g277PendingDisplayRange();
        if (!d.valid || g144RangeOverlaps(d, r))
            return true;
    }
    for (int i = 0; i < kG278PendSlots; ++i)
    {
        const G278PendDepth &p = s_g278PendTab[i];
        if (!p.active || p.privateMirror)
            continue;
        const G144BlockRange z = g278PendingDepthRangeP(p);
        if (!z.valid || g144RangeOverlaps(z, r))
            return true;
    }
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        const G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        if (o.firstOwnedBlock > o.lastOwnedBlock ||
            (r.last >= o.firstOwnedBlock && r.first <= o.lastOwnedBlock))
            return true;
    }
    for (int i = 0; i < 2; ++i)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange w{};
        if (g272DirtyInfo(i, fbp, fbw, w) && (!w.valid || g144RangeOverlaps(w, r)))
            return true;
    }
    return false;
}

// One CT32 page of guest VRAM as the 64x32 logical texel grid an FBO page holds (raw word = RGBA8),
// clipped to w x h; CT24 applies TEXA exactly as GSMem does.
static bool g767DecodeVramPage(uint32_t page, int w, int h, bool ct24, int ta0, int aem,
                               std::vector<uint32_t> &out)
{
    if (g_g144LastVram == nullptr || static_cast<uint64_t>(page + 1u) * 8192ull > g_g261VramSize)
        return false;
    const uint32_t blk = page * 32u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(blk, 1u, static_cast<uint32_t>(x),
                                                        static_cast<uint32_t>(y));
            uint32_t v = 0u;
            std::memcpy(&v, g_g144LastVram + off, 4u);
            if (ct24)
            {
                const uint32_t rgb = v & 0x00FFFFFFu;
                const uint32_t a = (aem != 0 && rgb == 0u) ? 0u : static_cast<uint32_t>(ta0 & 0xFF);
                v = rgb | (a << 24);
            }
            out.push_back(v);
        }
    return true;
}

// ===== The page plan of a view, and the cross-flush view cache =====
// A view is a pure function of, per page: WHICH holder supplies it and the holder's content version
// (the owner's stamp for an FBO-owned page, the page's guest write generation for every page). If
// the plan's signature is unchanged since the view was last built, and the backend still holds the
// texture, the view is reused as is: no gather, no guest-VRAM decode. Everything that changes a
// holder's bytes moves one of those numbers (waves and folds restamp, guest writes bump the page
// generation, a publication ends the owner epoch).
struct G767PagePlan
{
    std::vector<int32_t> rects;     // {srcKey|0, srcX|stageIdx, srcY, dstX, dstY, w, h}
    std::vector<uint32_t> vramPages; // pages to decode from guest VRAM (rects with srcKey 0)
    std::vector<uint64_t> sig;
    uint32_t owners = 0u;
    bool anyOwned = false;
};
extern bool g178_backend_has_tex(uint64_t key);
extern bool g767_backend_queue_view(uint64_t texKey, int texW, int texH, bool ct24, int ta0, int aem,
                                    int t8Kind, std::vector<int32_t> &&rects7,
                                    std::vector<uint32_t> &&pixels, std::vector<uint32_t> &&clut,
                                    const std::vector<uint32_t> *lut);
// Views are queued without a round trip (the batch that samples them follows in the same FIFO).
//   DC2_G767_SYNC_VIEWS=1  every view build waits for the worker (the g767h behaviour)
static bool g767AsyncViewsOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_SYNC_VIEWS");
    return on;
}
static std::unordered_map<uint64_t, std::vector<uint64_t>> g_g767ViewSig; // flush-owner thread only

// Resolve the holder of every page of a (pagesX x pagesY) grid. `pageW/H` is the texel size of one
// page in the texture (CT32 64x32, PSMT8 128x64). False = a page no holder can supply exactly.
static bool g767PlanPages(uint32_t base, uint32_t perRow, int pagesX, int pagesY, int pageW,
                          int pageH, int texW, int texH, G767PagePlan &plan)
{
    plan.rects.clear();
    plan.vramPages.clear();
    plan.sig.clear();
    plan.owners = 0u;
    plan.anyOwned = false;
    int staged = 0;
    for (int py = 0; py < pagesY; ++py)
        for (int px = 0; px < pagesX; ++px)
        {
            const uint32_t page =
                base + static_cast<uint32_t>(py) * perRow + static_cast<uint32_t>(px);
            if (page >= kG178PageCount)
                return false;
            const int w = std::min(pageW, texW - px * pageW);
            const int h = std::min(pageH, texH - py * pageH);
            const uint64_t gen = g766PageGen(page);
            int src = -1;
            uint32_t row = 0u, col = 0u;
            const int o = g766ValidOwner(page);
            if (o >= 0)
            {
                if (!g766SlotPagePos(static_cast<uint32_t>(o), page, row, col))
                    return false;
                src = o;
                plan.anyOwned = true;
                plan.sig.push_back((1ull << 62) | (static_cast<uint64_t>(o) << 56) |
                                   (g_g766Own[page].stamp & 0x00FFFFFFFFFFFFFFull));
            }
            else
            {
                for (uint32_t t = 0u; t < kG248TargetCount && src < 0; ++t)
                {
                    if (!g_g261Res[t].dirty || !g766SlotPagePos(t, page, row, col))
                        continue;
                    if (g767UnownedPageCurrent(t, page))
                        src = static_cast<int>(t);
                }
                if (src >= 0)
                    plan.sig.push_back((2ull << 62) | (static_cast<uint64_t>(src) << 56));
                else
                {
                    const G144BlockRange pr{page * 32u, page * 32u + 31u, true};
                    if (!g767VramPagesOn() || g767OtherAuthorityHits(pr))
                        return false;
                    plan.sig.push_back(3ull << 62);
                    plan.sig.push_back(gen);
                    plan.rects.insert(plan.rects.end(),
                                      {0, staged++, 0, px * pageW, py * pageH, w, h});
                    plan.vramPages.push_back(page);
                    continue;
                }
            }
            plan.sig.push_back(gen);
            plan.rects.insert(plan.rects.end(),
                              {static_cast<int32_t>(g766SlotKey(static_cast<uint32_t>(src))),
                               static_cast<int32_t>(col * 64u), static_cast<int32_t>(row * 32u),
                               px * pageW, py * pageH, w, h});
            plan.owners |= 1u << static_cast<uint32_t>(src);
        }
    return true;
}

// True (and the view is ready for this flush) when an identical plan built this texture before and
// the backend still holds it.
static bool g767ViewCacheHit(uint64_t key, const G767PagePlan &plan)
{
    const auto it = g_g767ViewSig.find(key);
    if (it == g_g767ViewSig.end() || it->second != plan.sig || !g178_backend_has_tex(key))
        return false;
    ++g_g767ViewReuse;
    return true;
}
static void g767ViewCacheStore(uint64_t key, const G767PagePlan &plan)
{
    if (g_g767ViewSig.size() > 4096u)
        g_g767ViewSig.clear(); // bounded; a miss only rebuilds
    g_g767ViewSig[key] = plan.sig;
}

// Returns the view key (built and ready for this flush) or 0 (the legacy path runs).
static uint64_t g767TryPageView(const G144Entry &e, int texW, int texH,
                                std::unordered_set<uint64_t> &builtThisFlush)
{
    if (!g767PageViewOn() || !e.prim.tme)
        return 0u;
    const GSTex0Reg &tex = e.ctx.tex0;
    const bool ct24 = tex.psm == GS_PSM_CT24;
    if ((tex.psm != GS_PSM_CT32 && !ct24) || (tex.tbp0 & 31u) != 0u || tex.tbw == 0u ||
        texW <= 0 || texH <= 0 || texW > 1024 || texH > 1024 || (e.ctx.clamp & 3u) >= 2u ||
        ((e.ctx.clamp >> 2) & 3u) >= 2u)
    {
        ++g_g767ViewShape;
        return 0u;
    }
    const int ta0 = ct24 ? static_cast<int>(e.texa.ta0) : 0;
    const int aem = ct24 ? (e.texa.aem ? 1 : 0) : 0;
    const uint64_t key = g767ViewKey(tex.tbp0, tex.tbw, tex.psm, texW, texH, ta0, aem);
    if (builtThisFlush.count(key) != 0u)
        return key;
    // Per-flush refusal memo: an entry stream samples the same refused texture many times.
    static std::unordered_set<uint64_t> s_refused; // flush-owner thread only
    static uint64_t s_refusedSerial = ~0ull;
    if (s_refusedSerial != g_g767FlushSerial)
    {
        s_refused.clear();
        s_refusedSerial = g_g767FlushSerial;
    }
    if (s_refused.count(key) != 0u)
        return 0u;
    ++g_g767ViewTry;
    static G767PagePlan s_plan; // flush-owner thread only
    const uint32_t base = tex.tbp0 >> 5u;
    const int pagesX = (texW + 63) / 64, pagesY = (texH + 31) / 32;
    const auto refuse = [&](uint64_t &ctr) -> uint64_t {
        ++ctr;
        s_refused.insert(key);
        return 0u;
    };
    if (!g767PlanPages(base, tex.tbw, pagesX, pagesY, 64, 32, texW, texH, s_plan))
        return refuse(g_g767ViewMixed);
    if (!s_plan.anyOwned)
        return refuse(g_g767ViewNoOwned); // guest VRAM is current for every page: cached decode
    if (!g767ViewCacheHit(key, s_plan))
    {
        static std::vector<uint32_t> s_pixels;
        s_pixels.clear();
        size_t vi = 0u;
        for (size_t k = 0u; k + 7u <= s_plan.rects.size(); k += 7u)
            if (s_plan.rects[k] == 0)
            {
                if (!g767DecodeVramPage(s_plan.vramPages[vi++], s_plan.rects[k + 5],
                                        s_plan.rects[k + 6], ct24, ta0, aem, s_pixels))
                    return refuse(g_g767ViewMixed);
            }
        g766FlushOwnerMirrors(s_plan.owners);
        const bool queued =
            g767AsyncViewsOn() &&
            g767_backend_queue_view(key, texW, texH, ct24, ta0, aem, -1,
                                    std::vector<int32_t>(s_plan.rects),
                                    std::vector<uint32_t>(s_pixels), std::vector<uint32_t>{},
                                    nullptr);
        if (!queued &&
            !g767_backend_prepare_ct32_view(key, texW, texH, ct24, ta0, aem, s_plan.rects,
                                            s_pixels))
            return refuse(g_g767ViewFail);
        g767ViewCacheStore(key, s_plan);
    }
    const std::vector<int32_t> &s_rects = s_plan.rects;
    builtThisFlush.insert(key);
    ++g_g767ViewBuilt;
    g_g767ViewPages += s_rects.size() / 7u;
    g767ViewReport();
    return key;
}

// ===== G767: PALETTED PAGE VIEWS (PSMT8 / PSMT8H / PSMT4HL / PSMT4HH) =====
// The CT32 page view's argument, for an indexed read: a paletted texture is the list of its pages,
// each page's index bytes live in fixed CT32 words + byte lanes of that page (one 128x64 table for
// PSMT8; the CT32 geometry with the index in the alpha byte for the H formats), and the CLUT is
// decoded exactly as the legacy decode decodes it. The GPU gathers the indices from each page's
// holder (owner FBO, a dirty resident whose copy is current, or the guest-VRAM page) and looks them
// up. It replaces G281's CPU-built per-texel map and its two hard-coded tuples, and G598's
// default-OFF generalisation of them, for every paletted read of FBO-owned pages.
//   DC2_G767_NO_T8VIEW=1  rollback: the G281/G598 tuple views and the legacy publication
extern bool g767_backend_prepare_t8_view(uint64_t texKey, int texW, int texH, int kind,
                                         const std::vector<int32_t> &rects7,
                                         const std::vector<uint32_t> &vramPages,
                                         const std::vector<uint32_t> &clut256,
                                         const std::vector<uint32_t> &lut);
// ⛔ DEFAULT OFF (g767g, dungeon1 census, same binary): armed, the route read 30.1 ms/f with
// `[G261:invariant] escaped writer fbp=0x139` x1,678 (genNow - genAtSkip = 40 every time); rolled
// back, 17.4 ms/f with inv=0. The views themselves are exact per page, but they stop the T8 reads
// from publishing 0x13c, which moves the 0x141 CPU replay's publication order, and each view is a
// synchronous executor round trip (39k builds / 60 s). Opt in with DC2_G767_T8VIEW=1 for the
// bisection; plans/phase-G767-ledger.md.
static bool g767T8ViewOn()
{
    static const bool on = g767PageViewOn() && envFlagEnabled("DC2_G767_T8VIEW") &&
                           !envFlagEnabled("DC2_G767_NO_T8VIEW");
    return on;
}
static uint64_t g_g767T8Try = 0u, g_g767T8Built = 0u, g_g767T8NoOwned = 0u, g_g767T8Mixed = 0u,
    g_g767T8Clut = 0u, g_g767T8Fail = 0u, g_g767T8Pages = 0u;
static void g767T8Report()
{
    if (!g767StatOn())
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 512u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G767:t8view] try=%llu built=%llu pages=%llu noOwned=%llu mixed=%llu clut=%llu "
                 "fail=%llu\n",
                 (unsigned long long)g_g767T8Try, (unsigned long long)g_g767T8Built,
                 (unsigned long long)g_g767T8Pages, (unsigned long long)g_g767T8NoOwned,
                 (unsigned long long)g_g767T8Mixed, (unsigned long long)g_g767T8Clut,
                 (unsigned long long)g_g767T8Fail);
}
// 128x64 PSMT8 page texel -> {CT32 word x, word y, byte lane} of the same page (RGBA8: r, g, b).
static const std::vector<uint32_t> &g767P8Lut()
{
    static const std::vector<uint32_t> s_lut = [] {
        std::vector<uint32_t> lut(128u * 64u, 0u);
        static const G281Ct32Inverse inv;
        const G618P8Page &pg = g618P8Page();
        if (!inv.ok || !pg.ok)
            return std::vector<uint32_t>{};
        for (uint32_t y = 0u; y < 64u; ++y)
            for (uint32_t x = 0u; x < 128u; ++x)
            {
                const uint32_t off = pg.perm[y * 128u + x];
                const uint32_t word = off >> 2u;
                const uint32_t lane = off & 3u;
                const uint32_t wx = static_cast<uint32_t>(inv.x[word]);
                const uint32_t wy = static_cast<uint32_t>(inv.y[word]);
                lut[y * 128u + x] = wx | (wy << 8) | (lane << 16) | 0xFF000000u;
            }
        return lut;
    }();
    return s_lut;
}

static uint64_t g767TryT8View(const G144Entry &e, int texW, int texH,
                              std::unordered_set<uint64_t> &builtThisFlush)
{
    if (!g767T8ViewOn() || !e.prim.tme || g_g144LastVram == nullptr)
        return 0u;
    const GSTex0Reg &tex = e.ctx.tex0;
    int kind = -1;
    if (tex.psm == GS_PSM_T8)
        kind = 0;
    else if (tex.psm == GS_PSM_T8H)
        kind = 1;
    else if (tex.psm == GS_PSM_T4HL)
        kind = 2;
    else if (tex.psm == GS_PSM_T4HH)
        kind = 3;
    if (kind < 0)
        return 0u;
    const int pageW = kind == 0 ? 128 : 64;
    const int pageH = kind == 0 ? 64 : 32;
    const uint32_t perRow = kind == 0 ? (tex.tbw >> 1u) : tex.tbw;
    if ((tex.tbp0 & 31u) != 0u || perRow == 0u || (kind == 0 && (tex.tbw & 1u) != 0u) ||
        texW <= 0 || texH <= 0 || texW > 1024 || texH > 1024 || (e.ctx.clamp & 3u) >= 2u ||
        ((e.ctx.clamp >> 2) & 3u) >= 2u || (kind == 0 && g767P8Lut().empty()))
        return 0u;
    static std::unordered_set<uint64_t> s_refused; // flush-owner thread only
    // Every register the view depends on -> the view built for them THIS flush. Guest VRAM cannot
    // change inside one flush (a transfer that touches a pending batch's ranges flushes it first),
    // so one flush's views are a pure function of these registers: no page walk, no CLUT decode,
    // no depth sync for the second and later entries that sample the same texture.
    static std::unordered_map<uint64_t, uint64_t> s_built;
    static uint64_t s_refusedSerial = ~0ull;
    if (s_refusedSerial != g_g767FlushSerial)
    {
        s_refused.clear();
        s_built.clear();
        s_refusedSerial = g_g767FlushSerial;
    }
    // Geometry key (without the CLUT): a refused geometry is refused for the whole flush.
    const uint64_t gkey = g767ViewKey(tex.tbp0, tex.tbw, tex.psm, texW, texH, 0x7F, 0);
    if (s_refused.count(gkey) != 0u)
        return 0u;
    uint64_t rkey = gkey;
    {
        const uint64_t regs[8] = {tex.cbp, tex.cpsm, tex.csm, tex.csa, e.texa.ta0,
                                  static_cast<uint64_t>(e.texa.aem ? 1u : 0u) | (uint64_t{e.texa.ta1} << 8),
                                  e.texclut.cbw | (uint64_t{e.texclut.cou} << 8) |
                                      (uint64_t{e.texclut.cov} << 16),
                                  0x5A5Au};
        for (uint64_t r : regs)
            rkey = (rkey ^ r) * 0x100000001b3ull;
    }
    {
        const auto it = s_built.find(rkey);
        if (it != s_built.end())
            return it->second;
    }
    ++g_g767T8Try;
    const auto refuse = [&](uint64_t &ctr) -> uint64_t {
        ++ctr;
        s_refused.insert(gkey);
        return 0u;
    };
    static G767PagePlan s_plan;
    const uint32_t base = tex.tbp0 >> 5u;
    const int pagesX = (texW + pageW - 1) / pageW, pagesY = (texH + pageH - 1) / pageH;
    if (!g767PlanPages(base, perRow, pagesX, pagesY, pageW, pageH, texW, texH, s_plan))
        return refuse(g_g767T8Mixed);
    if (!s_plan.anyOwned)
        return refuse(g_g767T8NoOwned); // guest VRAM current everywhere: the cached decode is exact
    // The CLUT is decoded from guest VRAM exactly as the legacy decode does it: its pages must be
    // current there (no dirty resident owes them, no other GPU authority pending over them).
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid || g767OtherAuthorityHits(clut))
        return refuse(g_g767T8Clut);
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g767ClutBlockedBy(t, clut))
            return refuse(g_g767T8Clut);
    if (!g242SyncOverlappingDepth(g_g144LastVram, tex.cbp, 1u, GS_PSM_CT32, 0u, 0u, 64u, 32u))
        return refuse(g_g767T8Clut);
    static std::vector<uint32_t> s_clut;
    s_clut.assign(256u, 0u);
    const uint32_t entries = (kind == 0 || kind == 1) ? 256u : 16u;
    uint64_t h = 0x84222325cbf29ce4ull;
    for (uint32_t ci = 0u; ci < entries; ++ci)
    {
        s_clut[ci] = g178LookupClutFixed(g_g144LastVram, e.texa, e.texclut, static_cast<uint8_t>(ci),
                                         tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        h = (h ^ s_clut[ci]) * 0x100000001b3ull;
    }
    const uint64_t key = (g767ViewKey(tex.tbp0, tex.tbw, tex.psm, texW, texH, 0x7F, 1) ^
                          (h & 0x0000FFFFFFFFFFFFull)) |
                         0xF768000000000000ull;
    if (builtThisFlush.count(key) != 0u)
    {
        s_built[rkey] = key;
        return key;
    }
    if (!g767ViewCacheHit(key, s_plan))
    {
        static std::vector<uint32_t> s_vram;
        s_vram.clear();
        for (const uint32_t pg : s_plan.vramPages)
            if (!g767DecodeVramPage(pg, 64, 32, false, 0, 0, s_vram))
                return refuse(g_g767T8Mixed);
        g766FlushOwnerMirrors(s_plan.owners);
        const bool queued =
            g767AsyncViewsOn() &&
            g767_backend_queue_view(key, texW, texH, false, 0, 0, kind,
                                    std::vector<int32_t>(s_plan.rects),
                                    std::vector<uint32_t>(s_vram), std::vector<uint32_t>(s_clut),
                                    &g767P8Lut());
        if (!queued &&
            !g767_backend_prepare_t8_view(key, texW, texH, kind, s_plan.rects, s_vram, s_clut,
                                          g767P8Lut()))
            return refuse(g_g767T8Fail);
        g767ViewCacheStore(key, s_plan);
        g_g767T8Pages += s_plan.rects.size() / 7u;
    }
    builtThisFlush.insert(key);
    s_built[rkey] = key;
    ++g_g767T8Built;
    g767T8Report();
    return key;
}

// ===== G769 E: A G289 COPY OWNER IS FOLDED INTO THE RESIDENTS, NOT PUBLISHED =====
//
// dungeon1's last per-frame read-backs (`[G495:rb]` 0x13b@96/416 and 0x14a@96/416, ~2.3 + ~1.0 ms
// of GPU drain each, `[G447:edge]` backend-readcolor ~5 ms/f of the executor) are G289 display-grab
// OWNER publications, 46 of 50 sampled with cause 10 = "owner-conflict": the next batch RENDERS
// wide 0x139 (color range 0x2720..0x351f) over pages the 0x14a grab owner holds. The legacy edge
// reads the owner FBO back into guest VRAM so 0x139 can re-seed from it.
//
// The owner is a GPU surface holding the NEWEST bytes of whole CT32 pages - exactly what a G767
// transient is after its draws. So fold it the same way: copy each owned page GPU->GPU into every
// resident home whose FBO holds that page over defined rows (dirty homes; a clean home only when its
// FBO is provably current, G767 tier 2), commit the rows as a wave would (the primary home becomes
// the page owner, the others have seen it), and retire the owner. Guest VRAM stays stale exactly as
// it does for any dirty resident: the obligation now rides the residency record and is discharged
// at a real CPU observer. A page no resident can take keeps the legacy publication for the owner.
//   DC2_G769_NO_OWNERFOLD=1   rollback: owner-conflict publishes the owner (G768 behaviour)
static bool g769OwnerFoldOn()
{
    static const bool on = g767FoldOn() && g766PullsAllowed() &&
                           !envFlagEnabled("DC2_G769_NO_OWNERFOLD");
    return on;
}
static uint64_t g_g769OwnerFolds = 0u, g_g769OwnerFoldPages = 0u, g_g769OwnerFoldRefuse = 0u,
    g_g769OwnerFoldFail = 0u;
static bool g769FoldOwnerSlot(G289CopyOwner &o, int cause)
{
    if (!o.active || o.dstFbp >= kG178PageCount)
        return false;
    const uint32_t D = o.dstFbp;
    // Whole pages only, inside the owner FBO's 512 x 416 surface (fbw 8, rows 0..415).
    if (o.firstOwnedBlock > o.lastOwnedBlock || (o.firstOwnedBlock & 31u) != 0u ||
        ((o.lastOwnedBlock + 1u) & 31u) != 0u)
    {
        ++g_g769OwnerFoldRefuse;
        return false;
    }
    const uint32_t p0 = o.firstOwnedBlock >> 5u, p1 = o.lastOwnedBlock >> 5u;
    if (p0 < D || (p1 - D) / 8u >= 13u || p1 >= kG178PageCount)
    {
        ++g_g769OwnerFoldRefuse;
        return false;
    }
    g767FoldReset();
    // Per home: the page-tile pull list {srcKey, sx, sy, dx, dy, w, h} (GL rows bottom-first).
    static std::vector<int32_t> s_homeRects[kG248TargetCount];
    for (auto &v : s_homeRects)
        v.clear();
    uint32_t mask = 0u;
    for (uint32_t page = p0; page <= p1; ++page)
    {
        const uint32_t srow = (page - D) / 8u, scol = (page - D) % 8u;
        int primaryIdx = -1;
        for (int pass = 0; pass < 2 && primaryIdx < 0; ++pass)
            for (uint32_t t = 0u; t < kG248TargetCount; ++t)
            {
                uint32_t row = 0u, col = 0u;
                bool clean = false;
                if (pass == 0)
                {
                    if (!g767SlotTakes(t, page, 0, 31, D, row, col))
                        continue;
                }
                else
                {
                    const G261Res &rc = g_g261Res[t];
                    if (rc.dirty || kG261Fbp[t] == D || !g766SlotPagePos(t, page, row, col))
                        continue;
                    const int cy0 = static_cast<int>(row) * 32;
                    if (cy0 < rc.covLo || cy0 + 31 > rc.covHi || !g767CleanHomeCurrent(t))
                        continue;
                    clean = true;
                }
                G767FoldHit hit;
                hit.slot = static_cast<uint8_t>(t);
                hit.page = static_cast<uint16_t>(page);
                hit.y0 = static_cast<int16_t>(row * 32u);
                hit.y1 = static_cast<int16_t>(row * 32u + 31u);
                hit.col = static_cast<uint8_t>(col);
                hit.clean = clean;
                if (primaryIdx < 0)
                    primaryIdx = static_cast<int>(s_g767Fold.hits.size());
                s_g767Fold.hits.push_back(hit);
                mask |= 1u << t;
                g766AppendRect(s_homeRects[t], D, srow, scol, row, col);
            }
        if (primaryIdx < 0)
        {
            ++g_g769OwnerFoldRefuse;
            g767FoldReset();
            return false;
        }
        s_g767Fold.hits[static_cast<size_t>(primaryIdx)].primary = true;
    }
    // Every home absorbs its own deferred guest-write mirror BEFORE the owner's bytes land on it.
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if ((mask & (1u << t)) != 0u && g_g261Res[t].mirPending)
        {
            bool wasDirty = g_g261Res[t].dirty;
            g766MirrorFlushOrDrop(static_cast<int>(t));
            if (wasDirty && !g_g261Res[t].dirty)
            {
                ++g_g769OwnerFoldFail; // a dirty home was dropped by its mirror: fail closed
                g767FoldReset();
                return false;
            }
        }
    // Apply: one GPU->GPU copy list per home.
    bool ok = true;
    for (uint32_t t = 0u; t < kG248TargetCount && ok; ++t)
        if ((mask & (1u << t)) != 0u)
            ok = !s_homeRects[t].empty() && g766PullNow(t, s_homeRects[t]);
    if (!ok)
    {
        // Homes that took the copy hold the owner's (newest) bytes, so the legacy publication that
        // follows only makes guest VRAM agree with them.
        ++g_g769OwnerFoldFail;
        g767FoldReset();
        return false;
    }
    s_g767Fold.fbp = D;
    s_g767Fold.fbw = 8u;
    s_g767Fold.rects7.clear();
    (void)g767FoldCommitHits();
    // Retire the owner: its bytes now live in the residents (owners of its pages).
    o.active = false;
    if (&o == &s_g289Owners[0])
        s_g289PendingUpload.active = false;
    G178FbSnap &snap = g_g178FbSnap[D];
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    ++g_g769OwnerFolds;
    g_g769OwnerFoldPages += static_cast<uint64_t>(p1 - p0 + 1u);
    if (g767StatOn())
    {
        static uint64_t s_n = 0u;
        if ((++s_n % 128u) == 1u)
            std::fprintf(stderr,
                         "[G769:ownerfold] folds=%llu pages=%llu refuse=%llu fail=%llu cause=%d "
                         "dst=0x%03x\n",
                         (unsigned long long)g_g769OwnerFolds,
                         (unsigned long long)g_g769OwnerFoldPages,
                         (unsigned long long)g_g769OwnerFoldRefuse,
                         (unsigned long long)g_g769OwnerFoldFail, cause, D);
    }
    return true;
}
// Fold every active owner; any owner that cannot be folded is published the legacy way.
static void g769FoldOrMaterializeOwners(int cause)
{
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        if (g769OwnerFoldOn() && g769FoldOwnerSlot(o, cause))
            continue;
        (void)g345MaterializeSlot(o, cause);
    }
}

// The same fold at a guest READ/WRITE edge whose ranges touch an owner (a host->local IMAGE upload,
// cause 7; a local->local transfer, cause 8). After the fold the owner's pages are ordinary resident
// pages, so the edge's own G261 handling applies to them: an upload or l2l destination is patched by
// the G264/G766 mirror and the resident stays resident; a source read publishes the resident at the
// real observer. Unknown ranges keep the legacy whole publication.
static void g769FoldOrMaterializeOwnersForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                                 bool srcUnknown, bool dstUnknown, int cause)
{
    if (srcUnknown || dstUnknown || !g769OwnerFoldOn())
    {
        g289MaterializeForRanges(src, dst, srcUnknown, dstUnknown, cause);
        return;
    }
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        const G144BlockRange own{o.firstOwnedBlock, o.lastOwnedBlock,
                                 o.firstOwnedBlock <= o.lastOwnedBlock};
        if (own.valid && !(src.valid && g144RangeOverlaps(own, src)) &&
            !(dst.valid && g144RangeOverlaps(own, dst)))
            continue;
        if (g769FoldOwnerSlot(o, cause))
            continue;
        (void)g345MaterializeSlot(o, cause);
    }
}
