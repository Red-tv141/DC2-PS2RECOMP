// ===== G786 — CHARACTER OUTLINE PASS STRUCTURE (plans/phase-G786-ledger.md) ===================
// COutLineDraw renders, per outlined object, a SILHOUETTE batch into a work RTT W (dungeon1: 0x139
// resident / 0x13b transient, the latter ALIASING 0x139 two pages in) and then a COMPOSITE batch on
// another target whose entries are all textured sprites sampling W (tbp0 == W's block base). The
// work RTT is reused by every object, so composite i must run before silhouette i+1 (WAR on W) and
// the parse stream alternates targets: 2 flushes per outlined object.
//
// Executor-side instrument (included by rasterizer_command_graph.inc, run once per graph drain,
// before any batch is consumed). Nothing here is armed in a shipping configuration.
//   DC2_G786_OLCENSUS=1  pair / run census + exact-merge feasibility (rect-precise RAW on W)
//   DC2_G786_OLCLASS=1   tag each executing batch for the G720 per-class stage census:
//                        1 silhouette, 2 composite, 3 silhouette on a G286 transient target (set in
//                        the flush), 0 other. Implies every-flush stage timing.
//   DC2_G786_OLPROBE=n   flush-structure CEILING PROBES (graph rewrite before execution):
//                        1 COMP   silhouettes keep their batches, the run's composites merge into ONE
//                                 batch after the last silhouette (= W-renaming's n+1 flush count).
//                                 ⛔ CORRUPTING where rects overlap: composites sample the final W.
//                        2 FULL   one silhouette batch + one composite batch per run (2 flushes).
//                                 ⛔ CORRUPTING where rects overlap.
//                        3 EXACT  greedy groups: a silhouette joins the open group only if its write
//                                 rect is disjoint from the read rect of every composite already in
//                                 it and no block-level conflict exists. Exact by construction.
//                        4 DROP   the executor skips every detected silhouette + composite (backend
//                                 ceiling; front end untouched). ⛔ CORRUPTING: outlines disappear.
struct G786Rect
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    void add(int ax0, int ay0, int ax1, int ay1)
    {
        x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
        x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
    }
    void add(const G786Rect &o) { if (o.valid()) add(o.x0, o.y0, o.x1, o.y1); }
    bool overlaps(const G786Rect &o) const
    {
        return valid() && o.valid() && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
    }
    long long area() const { return valid() ? (long long)(x1 - x0 + 1) * (y1 - y0 + 1) : 0; }
};

static int g786ProbeMode()
{
    static const int v = [] {
        const char *e = std::getenv("DC2_G786_OLPROBE");
        return e ? std::atoi(e) : 0;
    }();
    return v;
}
static bool g786CensusOn()
{
    static const bool v = envFlagEnabled("DC2_G786_OLCENSUS");
    return v;
}
static bool g786ClassOn()
{
    static const bool v = envFlagEnabled("DC2_G786_OLCLASS");
    return v;
}
// G786: why g766TransientSeedAdmit last refused (0 admitted; codes in g766_mask_mirror.inc), and a
// per-reason census of the 0x13b-class transient prologue (exec thread only).
static unsigned g_g786TrnWhy = 0u;
static uint64_t g_g786TrnWhyN[16]{};
// Class of the batch the graph loop is about to execute (exec thread only; read by the flush).
static unsigned g_g786Class = 0u;

// G786 diagnostic (DC2_G786_OLCLASS): how the backend sees outline batches at submit time — the
// FBO key, readback/upload flags, draw count and, for composites, the texture SOURCE of every draw
// (an FBO key = srcFbp, a decoded texture = texKey). Exec thread only.
struct G786DrawCensus
{
    uint64_t batches = 0, draws = 0, srcDraws = 0, texDraws = 0, noneDraws = 0, skipRb = 0, upload = 0;
    uint32_t src[6]{}; uint64_t srcN[6]{};
    uint32_t fbo[4]{}; uint64_t fboN[4]{};
};
static G786DrawCensus g_g786Dc[4];
static void g786NoteDraws(unsigned cls, uint32_t fbp, uint32_t fboKey, const std::vector<G178Draw> &draws,
                          bool skipRb, bool upload)
{
    G786DrawCensus &k = g_g786Dc[cls & 3u];
    ++k.batches;
    k.skipRb += skipRb ? 1u : 0u;
    k.upload += upload ? 1u : 0u;
    const uint32_t key = (fboKey != 0u ? fboKey : fbp) | (fbp << 20);
    for (uint32_t s = 0; s < 4u; ++s)
        if (k.fbo[s] == key || k.fboN[s] == 0u) { k.fbo[s] = key; ++k.fboN[s]; break; }
    for (const G178Draw &d : draws)
    {
        ++k.draws;
        if (d.srcFbp != 0u)
        {
            ++k.srcDraws;
            for (uint32_t s = 0; s < 6u; ++s)
                if (k.src[s] == d.srcFbp || k.srcN[s] == 0u) { k.src[s] = d.srcFbp; ++k.srcN[s]; break; }
        }
        else if (d.texKey != 0u)
            ++k.texDraws;
        else
            ++k.noneDraws;
    }
    if ((k.batches % 4096u) == 0u)
        std::fprintf(stderr,
                     "[G786:draws] cls=%u batches=%llu draws=%llu src=%llu tex=%llu none=%llu skipRb=%llu upload=%llu "
                     "fbo(key|fbp<<20)=%x:%llu,%x:%llu,%x:%llu,%x:%llu srcFbp=%x:%llu,%x:%llu,%x:%llu,%x:%llu,%x:%llu,%x:%llu\n",
                     cls, (unsigned long long)k.batches, (unsigned long long)k.draws,
                     (unsigned long long)k.srcDraws, (unsigned long long)k.texDraws,
                     (unsigned long long)k.noneDraws, (unsigned long long)k.skipRb,
                     (unsigned long long)k.upload, k.fbo[0], (unsigned long long)k.fboN[0], k.fbo[1],
                     (unsigned long long)k.fboN[1], k.fbo[2], (unsigned long long)k.fboN[2], k.fbo[3],
                     (unsigned long long)k.fboN[3], k.src[0], (unsigned long long)k.srcN[0], k.src[1],
                     (unsigned long long)k.srcN[1], k.src[2], (unsigned long long)k.srcN[2], k.src[3],
                     (unsigned long long)k.srcN[3], k.src[4], (unsigned long long)k.srcN[4], k.src[5],
                     (unsigned long long)k.srcN[5]);
}

// Write rect of a batch's entries on its own target: scissor-clipped vertex bbox, +1 px margin.
static G786Rect g786WriteRect(const std::vector<G144Entry> &list)
{
    G786Rect r;
    for (const G144Entry &e : list)
    {
        const GSContext &c = e.ctx;
        const float ofx = static_cast<float>(c.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(c.xyoffset.ofy >> 4);
        const int nv = e.prim.type == GS_PRIM_SPRITE ? 2 : 3;
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        for (int k = 0; k < nv; ++k)
        {
            mnx = std::min(mnx, e.v[k].x - ofx); mxx = std::max(mxx, e.v[k].x - ofx);
            mny = std::min(mny, e.v[k].y - ofy); mxy = std::max(mxy, e.v[k].y - ofy);
        }
        int ax0 = static_cast<int>(std::floor(mnx)) - 1, ax1 = static_cast<int>(std::ceil(mxx)) + 1;
        int ay0 = static_cast<int>(std::floor(mny)) - 1, ay1 = static_cast<int>(std::ceil(mxy)) + 1;
        ax0 = std::max(ax0, static_cast<int>(c.scissor.x0)); ax1 = std::min(ax1, static_cast<int>(c.scissor.x1));
        ay0 = std::max(ay0, static_cast<int>(c.scissor.y0)); ay1 = std::min(ay1, static_cast<int>(c.scissor.y1));
        if (ax1 >= ax0 && ay1 >= ay0)
            r.add(ax0, ay0, ax1, ay1);
    }
    return r;
}

// Is `c` a composite of work target W (every entry a textured UV sprite sampling W with W's
// layout)? `readRect` receives the union of the texel rects read (+1 texel bilinear margin).
// `zfree` = no entry tests or writes Z. Fails (false) on anything else.
static bool g786IsComposite(const G260Batch &c, uint32_t wFbp, uint32_t wFbw, uint32_t wPsm,
                            G786Rect &readRect, bool &zfree)
{
    if (c.entries.empty() || c.fbp == wFbp)
        return false;
    const uint32_t wBlock = framePageBaseToBlock(wFbp);
    zfree = true;
    for (const G144Entry &e : c.entries)
    {
        if (e.prim.type != GS_PRIM_SPRITE || !e.prim.tme || !e.prim.fst ||
            e.ctx.tex0.tbp0 != wBlock || e.ctx.tex0.tbw != wFbw || e.ctx.tex0.psm != wPsm)
            return false;
        const int u0 = std::min(e.v[0].u, e.v[1].u) >> 4, u1 = (std::max(e.v[0].u, e.v[1].u) + 15) >> 4;
        const int v0 = std::min(e.v[0].v, e.v[1].v) >> 4, v1 = (std::max(e.v[0].v, e.v[1].v) + 15) >> 4;
        // Wrap modes: CLAMP keeps every read inside the UV hull (+margin); REPEAT does too while the
        // hull stays inside the texture; REGION_* (or a wrapping REPEAT) may read anywhere.
        const uint32_t wms = static_cast<uint32_t>(e.ctx.clamp & 3ull);
        const uint32_t wmt = static_cast<uint32_t>((e.ctx.clamp >> 2) & 3ull);
        const int tw = 1 << std::min<int>(e.ctx.tex0.tw, 10), th = 1 << std::min<int>(e.ctx.tex0.th, 10);
        const bool uIn = wms == 1u || (wms == 0u && u0 >= 1 && u1 + 1 < tw);
        const bool vIn = wmt == 1u || (wmt == 0u && v0 >= 1 && v1 + 1 < th);
        if (uIn && vIn)
            readRect.add(u0 - 1, v0 - 1, u1 + 1, v1 + 1);
        else
            readRect.add(0, 0, tw - 1, th - 1);
        const bool zte = ((e.ctx.test >> 16) & 1ull) != 0ull;
        const uint32_t ztst = static_cast<uint32_t>((e.ctx.test >> 17) & 3ull);
        const bool zmsk = ((e.ctx.zbuf >> 32) & 1ull) != 0ull;
        if (zte && !(ztst == 1u && zmsk))
            zfree = false;
    }
    return true;
}

struct G786Pair
{
    size_t s = 0, c = 0;   // graph indices (c == SIZE_MAX: trailing silhouette, no composite)
    G786Rect wr, rd;       // silhouette write rect on W, composite read rect on W
    bool zfree = true;
};

// Census state (exec thread only).
struct G786Census
{
    uint64_t drains = 0, runs = 0, pairs = 0, trailing = 0, sEntries = 0, sTris = 0, sSprites = 0,
             cEntries = 0, zUse = 0, blockConflict = 0, exactGroups = 0, exactFlushes = 0,
             origFlushes = 0, rectOverlapPairs = 0, probeMerged = 0;
    uint64_t runLen[6]{}; // 1,2,3,4-7,8-15,16+
    long long sArea = 0, cArea = 0;
    uint32_t wFbps[4]{}; uint64_t wPairs[4]{};
    uint64_t sigS[4]{}, sigC[4]{}; uint32_t nSigS = 0, nSigC = 0; uint64_t sigSN[4]{}, sigCN[4]{};
    uint32_t lastSf = 0;
};
static G786Census g_g786C;

static uint64_t g786Sig(const G144Entry &e)
{
    // ALPHA (A,B,C,D,FIX), TEST (ATE/ATST/AREF/AFAIL/ZTE/ZTST), ZMSK, TEX1 LCM/MMAG/MMIN, CLAMP wms/wmt,
    // TFX/TCC, FBMSK!=0.
    uint64_t h = e.ctx.alpha * 0x9E3779B97F4A7C15ull;
    h ^= (e.ctx.test & 0x7FFFFull) * 0xC2B2AE3D27D4EB4Full;
    h ^= ((e.ctx.zbuf >> 32) & 1ull) << 63;
    h ^= ((e.ctx.tex1 >> 5) & 0x1Full) << 40;
    h ^= (e.ctx.clamp & 0xFull) << 48;
    h ^= (static_cast<uint64_t>(e.ctx.tex0.tfx) << 52) ^ (static_cast<uint64_t>(e.ctx.tex0.tcc) << 54);
    h ^= (e.ctx.frame.fbmsk != 0u ? 1ull : 0ull) << 55;
    return h;
}
static void g786NoteSig(uint64_t *sig, uint64_t *sigN, uint32_t &n, uint64_t s, const G144Entry &e,
                        const char *what)
{
    for (uint32_t i = 0; i < n; ++i)
        if (sig[i] == s) { ++sigN[i]; return; }
    if (n < 4u)
    {
        sig[n] = s; sigN[n] = 1u; ++n;
        std::fprintf(stderr,
                     "[G786:sig] %s fbp=0x%03x prim=%u alpha=0x%llx test=0x%llx zmsk=%u tex1=0x%llx "
                     "clamp=0x%llx tfx=%u tcc=%u fbmsk=0x%x tex=%x/%u/%02x fst=%u abe=%u\n",
                     what, e.ctx.frame.fbp, static_cast<unsigned>(e.prim.type),
                     static_cast<unsigned long long>(e.ctx.alpha),
                     static_cast<unsigned long long>(e.ctx.test),
                     static_cast<unsigned>((e.ctx.zbuf >> 32) & 1ull),
                     static_cast<unsigned long long>(e.ctx.tex1),
                     static_cast<unsigned long long>(e.ctx.clamp), static_cast<unsigned>(e.ctx.tex0.tfx),
                     static_cast<unsigned>(e.ctx.tex0.tcc), e.ctx.frame.fbmsk, e.ctx.tex0.tbp0,
                     static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm, e.prim.fst ? 1u : 0u,
                     e.prim.abe ? 1u : 0u);
    }
}

static void g786MergeSets(G260RangeSet &dst, const G260RangeSet &src)
{
    if (src.unknown) { dst.markUnknown(); return; }
    dst.degen = dst.degen || src.degen;
    for (int i = 0; i < src.n; ++i)
        dst.add(src.r[i]);
}

static void g786Report()
{
    G786Census &k = g_g786C;
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf / 600u == k.lastSf / 600u)
        return;
    k.lastSf = sf;
    std::fprintf(stderr,
                 "[G786:ol] sf=%u drains=%llu runs=%llu pairs=%llu trailing=%llu run(1/2/3/4-7/8-15/16+)="
                 "%llu/%llu/%llu/%llu/%llu/%llu sEnt=%llu sTri=%llu sSpr=%llu cEnt=%llu sArea=%lld cArea=%lld "
                 "zUse=%llu blockConf=%llu rectOverlapPairs=%llu origFlushes=%llu exactGroups=%llu "
                 "exactFlushes=%llu probe=%d merged=%llu w=%03x:%llu,%03x:%llu,%03x:%llu,%03x:%llu\n",
                 sf, (unsigned long long)k.drains, (unsigned long long)k.runs, (unsigned long long)k.pairs,
                 (unsigned long long)k.trailing, (unsigned long long)k.runLen[0],
                 (unsigned long long)k.runLen[1], (unsigned long long)k.runLen[2],
                 (unsigned long long)k.runLen[3], (unsigned long long)k.runLen[4],
                 (unsigned long long)k.runLen[5], (unsigned long long)k.sEntries,
                 (unsigned long long)k.sTris, (unsigned long long)k.sSprites,
                 (unsigned long long)k.cEntries, k.sArea, k.cArea, (unsigned long long)k.zUse,
                 (unsigned long long)k.blockConflict, (unsigned long long)k.rectOverlapPairs,
                 (unsigned long long)k.origFlushes, (unsigned long long)k.exactGroups,
                 (unsigned long long)k.exactFlushes, g786ProbeMode(), (unsigned long long)k.probeMerged,
                 k.wFbps[0], (unsigned long long)k.wPairs[0], k.wFbps[1], (unsigned long long)k.wPairs[1],
                 k.wFbps[2], (unsigned long long)k.wPairs[2], k.wFbps[3], (unsigned long long)k.wPairs[3]);
}

// Tags for the graph about to execute (index-aligned with g_g260Graph; class per batch).
static std::vector<uint8_t> g_g786Tags;

// ===== G786 VERSIONED OUTLINE MERGE (DC2_G786_VMERGE=1) ========================================
// Exact form of probe 2: a run [S1 C1 … Sn Cn] on work RTT W executes as S* = S1..Sn then
// C* = C1..Cn. Legal because (checked per run, else the run is left alone):
//   * composites never test or write Z, and are pure W-sampling sprites (g786IsComposite);
//   * no hoisted silhouette reads or writes what an earlier composite writes (block range sets);
//   * the one remaining hazard — silhouette m > k overwriting W where composite k reads — is
//     removed by VERSIONING: right after silhouette k the backend snapshots composite k's read
//     rect of W, and composite k's draws sample that snapshot (ps2_g786_outline_api.inc).
// W keeps its exact final contents (S* is S1..Sn in order), so every later consumer of W — the
// G310 atlas, aliases, readbacks — is untouched.
struct G786Plan
{
    bool isS = false;
    int n = 0;
    uint32_t wfbp = 0u;
    uint32_t a[kG786MaxSlots]{}; // S*: entry where silhouette k+1 starts (snapshot point); C*: begin
    uint32_t b[kG786MaxSlots]{}; // C*: end (exclusive) of composite k's entries
    int slot[kG786MaxSlots]{};
    G786Rect rect[kG786MaxSlots];
};
static std::vector<G786Plan> g_g786Plans;   // index-aligned with g_g260Graph for one drain
static std::vector<uint8_t> g_g786HasPlan;
static const G786Plan *g_g786CurPlan = nullptr; // the executing batch's plan (exec thread)
struct G786VmStat
{
    uint64_t runs = 0, merged = 0, pairs = 0, snaps = 0, refW = 0, refZ = 0, refBlock = 0, refSlots = 0,
             armed = 0, unconsumed = 0, splitSelf = 0, refSource = 0;
};
static G786VmStat g_g786Vm;
// G786 isolation variants (diagnostic): ALLSNAP versions every composite; SEPS keeps each silhouette
// its own flush (hoisted, snapshot at its end) and merges only the composites.
static bool g786AllSnapOn()
{
    static const bool v = envFlagEnabled("DC2_G786_VMERGE_ALLSNAP");
    return v;
}
// SNAPONLY (diagnostic): no reordering at all — every composite samples a snapshot of W taken at
// the end of its own silhouette, in the original order. With DC2_G786_VERIFY it proves the snapshot
// equals the texture the legacy composite samples.
static bool g786SnapOnlyOn()
{
    static const bool v = envFlagEnabled("DC2_G786_VMERGE_SNAPONLY");
    return v;
}
static bool g786VerifyRasterOn()
{
    static const bool v = envFlagEnabled("DC2_G786_VERIFY");
    return v;
}
// Mirrors g311On() (rasterizer_page_source_maps.inc, declared later in a different scope).
static bool g786G311On()
{
    static const bool v = envFlagEnabled("DC2_G311_INCREMENTAL") && !envFlagEnabled("DC2_G311_NO_INCREMENTAL") &&
                          !envFlagDisabled("DC2_G311_INCREMENTAL");
    return v;
}
// DC2_G786_VMERGE_W=<hex fbp>[+<hex fbp>...]: restrict the versioned merge to these work RTTs
// (per-class pricing arm; unset = every eligible W).
static bool g786VmergeWAllowed(uint32_t wfbp)
{
    static const std::vector<uint32_t> list = [] {
        std::vector<uint32_t> v;
        const char *e = std::getenv("DC2_G786_VMERGE_W");
        while (e != nullptr && *e != 0)
        {
            char *end = nullptr;
            const unsigned long x = std::strtoul(e, &end, 16);
            if (end == e)
                break;
            v.push_back(static_cast<uint32_t>(x));
            e = (*end == '+' || *end == ',') ? end + 1 : end;
        }
        return v;
    }();
    if (list.empty())
        return true;
    for (const uint32_t f : list)
        if (f == wfbp)
            return true;
    return false;
}
static bool g786SepSOn()
{
    static const bool v = envFlagEnabled("DC2_G786_VMERGE_SEPS");
    return v;
}
static bool g786VmergeOn()
{
    // Default ON (G786). Rollback DC2_G786_NO_VMERGE=1; DC2_G786_VMERGE=0 also disables.
    static const bool v = !envFlagEnabled("DC2_G786_NO_VMERGE") && !envFlagDisabled("DC2_G786_VMERGE");
    return v;
}

// Arms the backend directives for the executing batch and clears them when the flush returns,
// whatever path it takes; a plan the backend never consumed (CPU fallback) is counted.
struct G786DirGuard
{
    bool armed;
    uint64_t consumed0;
    explicit G786DirGuard(bool a) : armed(a), consumed0(g_g786Consumed) {}
    ~G786DirGuard()
    {
        if (!armed)
            return;
        if (g_g786Consumed == consumed0)
            ++g_g786Vm.unconsumed;
        g_g786SnapN = 0;
        g_g786OvrN = 0;
    }
};
// Translate the executing batch's plan to vertex indices (entryVtx[i] = first vertex of entry i).
static void g786ArmDirectives(const G786Plan &p, const std::vector<int> &entryVtx, size_t entries,
                              int totalVtx)
{
    ++g_g786Vm.armed;
    g_g786SnapN = 0;
    g_g786OvrN = 0;
    auto vtxOf = [&](uint32_t e) { return e < entries ? entryVtx[e] : totalVtx; };
    for (int k = 0; k < p.n && k < kG786MaxSlots; ++k)
    {
        if (p.isS)
        {
            G786SnapDir &d = g_g786Snap[g_g786SnapN++];
            d.vtx = vtxOf(p.a[k]);
            d.slot = p.slot[k];
            d.x0 = p.rect[k].x0; d.y0 = p.rect[k].y0; d.x1 = p.rect[k].x1; d.y1 = p.rect[k].y1;
            d.wfbp = p.wfbp;
        }
        else
        {
            G786OvrDir &d = g_g786Ovr[g_g786OvrN++];
            d.vtxBegin = vtxOf(p.a[k]);
            d.vtxEnd = vtxOf(p.b[k]);
            d.slot = p.slot[k];
        }
    }
}

static void g786VmReport()
{
    static uint64_t s_n = 0;
    if ((++s_n % 4096u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G786:vmerge] runs=%llu merged=%llu pairs=%llu snaps=%llu refW=%llu refZ=%llu "
                 "refBlock=%llu refSlots=%llu armed=%llu unconsumed=%llu consumed=%llu copies=%llu "
                 "ovrDraws=%llu ovrMiss=%llu repl=%llu replMiss=%llu stale=%llu splitSelf=%llu refSource=%llu\n",
                 (unsigned long long)g_g786Vm.runs, (unsigned long long)g_g786Vm.merged,
                 (unsigned long long)g_g786Vm.pairs, (unsigned long long)g_g786Vm.snaps,
                 (unsigned long long)g_g786Vm.refW, (unsigned long long)g_g786Vm.refZ,
                 (unsigned long long)g_g786Vm.refBlock, (unsigned long long)g_g786Vm.refSlots,
                 (unsigned long long)g_g786Vm.armed, (unsigned long long)g_g786Vm.unconsumed,
                 (unsigned long long)g_g786Consumed, (unsigned long long)g_g786Copies,
                 (unsigned long long)g_g786OvrDraws, (unsigned long long)g_g786OvrMiss,
                 (unsigned long long)g_g786Repl, (unsigned long long)g_g786ReplMiss,
                 (unsigned long long)g_g786OvrStale, (unsigned long long)g_g786Vm.splitSelf,
                 (unsigned long long)g_g786Vm.refSource);
}

// Rewrite run [i, end) into S* (at i) and C* (at i + 1) with their plans; false = left untouched.
static bool g786VersionedMerge(std::vector<G260Batch> &g, size_t i, size_t end,
                               const std::vector<G786Pair> &run)
{
    ++g_g786Vm.runs;
    // The atlas-layout snapshots replicate the G309 compositor (G311 off: the atlas is rebuilt per
    // consumer). With G311's incremental page copies the legacy atlas has different semantics.
    if (g786G311On())
    {
        ++g_g786Vm.refW;
        return false;
    }
    const uint32_t wfbp = g[run[0].s].fbp;
    if (!g786VmergeWAllowed(wfbp))
    {
        ++g_g786Vm.refW;
        return false;
    }
    const GSContext &w0 = g[run[0].s].entries.front().ctx;
    if (w0.frame.fbw != 8u || w0.frame.psm != GS_PSM_CT32 || wfbp < 0x139u || wfbp - 0x139u >= 128u)
    {
        ++g_g786Vm.refW;
        return false;
    }
    // Partially initialized G700 textures mix direct W and materialized VRAM
    // consumers. The latter decode the declared texture, including inherited
    // padding/history, and bypass G786's srcFbp snapshot override. A per-draw
    // UV bound cannot preserve that decode boundary. Keep original ordering
    // until the complete declared source has initialized coverage.
    // This does not replace G700 page ownership validation or alter the proven
    // legacy resident/transient snapshot paths. Cold G700 sources fail closed.
    static const bool sourceGuard = !envFlagEnabled("DC2_G786_NO_SOURCE_GUARD");
    static const bool sourceTrace = envFlagEnabled("DC2_G786_SOURCE_TRACE");
    if (sourceGuard || sourceTrace)
    {
        const int initializedHi = g700VmergeInitializedRowHi(wfbp, w0.frame.fbw, w0.frame.psm);
        for (const G786Pair &pair : run)
        {
            if (pair.c == SIZE_MAX)
                continue;
            for (const G144Entry &e : g[pair.c].entries)
            {
                const int declaredHi = (1 << std::min<int>(e.ctx.tex0.th, 10)) - 1;
                if (sourceTrace && g770XTraceActive())
                    std::fprintf(stderr, "[G786:source] w=%x dst=%x initializedHi=%d declaredHi=%d guard=%u\n",
                                 wfbp, g[pair.c].fbp, initializedHi, declaredHi, sourceGuard ? 1u : 0u);
                if (sourceGuard && initializedHi != -2 && initializedHi < declaredHi)
                {
                    ++g_g786Vm.refSource;
                    return false;
                }
            }
        }
    }
    // Block-level hoist test against each composite's COLOUR writes only. Its G260 `wr` set also
    // carries the Z surface of every ZTE=1 entry, but a composite is proven Z-free above (ZTST ALWAYS
    // with ZMSK, or ZTE=0), so that Z range is no write and would refuse every run.
    for (size_t q = 0; q < run.size(); ++q)
    {
        const G786Pair &o = run[q];
        if (o.c == SIZE_MAX)
            continue;
        if (!o.zfree)
        {
            ++g_g786Vm.refZ;
            return false;
        }
        G260RangeSet colour;
        for (const G144Entry &e : g[o.c].entries)
            colour.add(g144TargetRange(e));
        for (size_t m = q + 1; m < run.size(); ++m)
            if (g523SetsOverlap(g[run[m].s].rd, colour) || g523SetsOverlap(g[run[m].s].wr, colour))
            {
                ++g_g786Vm.refBlock;
                return false;
            }
    }
    std::vector<int> slotOf(run.size(), -1);
    int slots = 0;
    for (size_t q = 0; q < run.size(); ++q)
    {
        if (run[q].c == SIZE_MAX)
            continue;
        bool need = g786AllSnapOn();
        for (size_t m = q + 1; m < run.size() && !need; ++m)
            need = run[m].wr.overlaps(run[q].rd);
        if (!need)
            continue;
        if (slots >= kG786MaxSlots)
        {
            ++g_g786Vm.refSlots;
            return false;
        }
        slotOf[q] = slots++;
    }
    for (size_t r = i; r < end; ++r)
        g712PrepRelease(g[r].prep); // a worker may hold a prepared class pass of any of them
    auto absorb = [](G260Batch &dst, G260Batch &from) {
        dst.entries.insert(dst.entries.end(), std::make_move_iterator(from.entries.begin()),
                           std::make_move_iterator(from.entries.end()));
        from.entries.clear();
        g786MergeSets(dst.wr, from.wr);
        g786MergeSets(dst.rd, from.rd);
    };
    if (g786SnapOnlyOn())
    {
        int slot = 0;
        for (size_t q = 0; q < run.size() && slot < kG786MaxSlots; ++q)
        {
            if (run[q].c == SIZE_MAX)
                continue;
            G786Plan one;
            one.isS = true;
            one.wfbp = wfbp;
            one.n = 1;
            one.a[0] = static_cast<uint32_t>(g[run[q].s].entries.size());
            one.slot[0] = slot;
            G786Rect r = run[q].rd;
            r.x0 = std::max(r.x0, 0); r.y0 = std::max(r.y0, 0);
            r.x1 = std::min(r.x1, 511); r.y1 = std::min(r.y1, 511);
            one.rect[0] = r;
            g_g786Plans[run[q].s] = one;
            g_g786HasPlan[run[q].s] = 1u;
            G786Plan c;
            c.wfbp = wfbp;
            c.n = 1;
            c.a[0] = 0u;
            c.b[0] = static_cast<uint32_t>(g[run[q].c].entries.size());
            c.slot[0] = slot;
            c.rect[0] = r; // diagnostic only (g786DiagAtlasPages)
            g_g786Plans[run[q].c] = c;
            g_g786HasPlan[run[q].c] = 1u;
            ++slot;
        }
        ++g_g786Vm.merged;
        return true;
    }
    if (g786SepSOn())
    {
        // Silhouettes keep their own batches, in order, hoisted above the run's single composite
        // batch; silhouette q's snapshot is taken after its own last draw.
        std::vector<G260Batch> sb;
        std::vector<G786Plan> spl;
        G260Batch cmS;
        bool haveCS = false;
        G786Plan cpS;
        cpS.wfbp = wfbp;
        for (size_t q = 0; q < run.size(); ++q)
        {
            G786Plan one;
            one.isS = true;
            one.wfbp = wfbp;
            if (slotOf[q] >= 0)
            {
                one.a[0] = static_cast<uint32_t>(g[run[q].s].entries.size());
                one.slot[0] = slotOf[q];
                G786Rect r = run[q].rd;
                r.x0 = std::max(r.x0, 0); r.y0 = std::max(r.y0, 0);
                r.x1 = std::min(r.x1, 511); r.y1 = std::min(r.y1, 511);
                one.rect[0] = r;
                one.n = 1;
            }
            sb.push_back(std::move(g[run[q].s]));
            spl.push_back(one);
            if (run[q].c == SIZE_MAX)
                continue;
            const uint32_t cBegin = haveCS ? static_cast<uint32_t>(cmS.entries.size()) : 0u;
            if (!haveCS) { cmS = std::move(g[run[q].c]); haveCS = true; }
            else absorb(cmS, g[run[q].c]);
            if (slotOf[q] >= 0)
            {
                cpS.a[cpS.n] = cBegin;
                cpS.b[cpS.n] = static_cast<uint32_t>(cmS.entries.size());
                cpS.slot[cpS.n] = slotOf[q];
                ++cpS.n;
            }
        }
        size_t wS = i;
        for (size_t q = 0; q < sb.size(); ++q, ++wS)
        {
            g[wS] = std::move(sb[q]);
            g[wS].prep = nullptr;
            g_g786Tags[wS] = 1u;
            g_g786HasPlan[wS] = spl[q].n > 0 ? 1u : 0u;
            if (spl[q].n > 0)
                g_g786Plans[wS] = spl[q];
        }
        if (haveCS)
        {
            g[wS] = std::move(cmS);
            g[wS].prep = nullptr;
            g_g786Tags[wS] = 2u;
            g_g786HasPlan[wS] = cpS.n > 0 ? 1u : 0u;
            if (cpS.n > 0)
                g_g786Plans[wS] = cpS;
            ++wS;
        }
        for (; wS < end; ++wS)
        {
            g[wS] = G260Batch{};
            g_g786Tags[wS] = 0u;
            g_g786HasPlan[wS] = 0u;
        }
        ++g_g786Vm.merged;
        g_g786Vm.snaps += static_cast<uint64_t>(slots);
        for (const G786Pair &p : run)
            g_g786Vm.pairs += p.c != SIZE_MAX ? 1u : 0u;
        return true;
    }
    G786Plan sp, cp;
    sp.isS = true;
    sp.wfbp = cp.wfbp = wfbp;
    G260Batch sm = std::move(g[run[0].s]);
    G260Batch cm;
    bool haveC = false;
    for (size_t q = 0; q < run.size(); ++q)
    {
        if (q > 0u)
            absorb(sm, g[run[q].s]);
        if (slotOf[q] >= 0)
        {
            sp.a[sp.n] = static_cast<uint32_t>(sm.entries.size()); // silhouette q+1 starts here
            sp.slot[sp.n] = slotOf[q];
            G786Rect r = run[q].rd;
            r.x0 = std::max(r.x0, 0); r.y0 = std::max(r.y0, 0);
            r.x1 = std::min(r.x1, 511); r.y1 = std::min(r.y1, 511);
            sp.rect[sp.n] = r;
            ++sp.n;
        }
        if (run[q].c == SIZE_MAX)
            continue;
        const uint32_t cBegin = haveC ? static_cast<uint32_t>(cm.entries.size()) : 0u;
        if (!haveC)
        {
            cm = std::move(g[run[q].c]);
            haveC = true;
        }
        else
            absorb(cm, g[run[q].c]);
        if (slotOf[q] >= 0)
        {
            cp.a[cp.n] = cBegin;
            cp.b[cp.n] = static_cast<uint32_t>(cm.entries.size());
            cp.slot[cp.n] = slotOf[q];
            ++cp.n;
        }
    }
    size_t w = i;
    g[w] = std::move(sm);
    g[w].prep = nullptr;
    g_g786Tags[w] = 1u;
    if (sp.n > 0)
    {
        g_g786Plans[w] = sp;
        g_g786HasPlan[w] = 1u;
    }
    ++w;
    if (haveC)
    {
        g[w] = std::move(cm);
        g[w].prep = nullptr;
        g_g786Tags[w] = 2u;
        if (cp.n > 0)
        {
            g_g786Plans[w] = cp;
            g_g786HasPlan[w] = 1u;
        }
        ++w;
    }
    for (; w < end; ++w)
    {
        g[w] = G260Batch{};
        g_g786Tags[w] = 0u;
        g_g786HasPlan[w] = 0u;
    }
    ++g_g786Vm.merged;
    g_g786Vm.snaps += static_cast<uint64_t>(slots);
    for (const G786Pair &p : run)
        g_g786Vm.pairs += p.c != SIZE_MAX ? 1u : 0u;
    return true;
}

// Census (DC2_G786_OLCENSUS): batches that LOOK like an outline silhouette (on a work-RTT candidate,
// an untextured sprite then a non-sprite) or an outline composite (every entry a textured FST sprite
// sampling a work-RTT candidate) but were not taken into a run, classified by why. Per W fbp.
struct G786Unm
{
    uint32_t fbp = 0u;
    uint64_t sil = 0, silNextMixed = 0, silNextSameW = 0, silNextOther = 0, silLast = 0, silFbw = 0, comp = 0;
};
static G786Unm g_g786Unm[8];
static uint64_t g_g786UnmDrains = 0;
static G786Unm &g786UnmSlot(uint32_t fbp)
{
    for (G786Unm &u : g_g786Unm)
        if (u.fbp == fbp || u.fbp == 0u)
        {
            u.fbp = fbp;
            return u;
        }
    return g_g786Unm[7];
}
static bool g786WCandidate(uint32_t fbp) { return fbp >= 0x139u && fbp < 0x1b9u; }
static void g786ClassifyUnmatched(const std::vector<G260Batch> &g)
{
    ++g_g786UnmDrains;
    for (size_t i = 0; i < g.size(); ++i)
    {
        if (g[i].entries.empty() || (i < g_g786Tags.size() && g_g786Tags[i] != 0u))
            continue;
        const std::vector<G144Entry> &te = g[i].entries;
        const bool silLike = g786WCandidate(g[i].fbp) && te.size() >= 2u && te.front().prim.type == GS_PRIM_SPRITE &&
                             !te.front().prim.tme && te[1].prim.type != GS_PRIM_SPRITE;
        if (silLike)
        {
            G786Unm &u = g786UnmSlot(g[i].fbp);
            ++u.sil;
            if (te.front().ctx.frame.fbw != 8u)
                ++u.silFbw;
            if (i + 1 >= g.size() || g[i + 1].entries.empty())
                ++u.silLast;
            else if (g[i + 1].fbp == g[i].fbp)
                ++u.silNextSameW;
            else
            {
                const uint32_t wBlock = framePageBaseToBlock(g[i].fbp);
                bool any = false;
                for (const G144Entry &e : g[i + 1].entries)
                    any = any || (e.prim.tme && e.ctx.tex0.tbp0 == wBlock);
                ++(any ? u.silNextMixed : u.silNextOther);
            }
            continue;
        }
        // composite-like: every entry a textured FST sprite on one work-RTT candidate base
        uint32_t base = 0u;
        bool comp = true;
        for (const G144Entry &e : te)
        {
            if (e.prim.type != GS_PRIM_SPRITE || !e.prim.tme || !e.prim.fst) { comp = false; break; }
            if (base == 0u) base = e.ctx.tex0.tbp0;
            else if (base != e.ctx.tex0.tbp0) { comp = false; break; }
        }
        if (comp && base >= 0x2720u && (base & 31u) == 0u && g786WCandidate(base >> 5u) && g[i].fbp != (base >> 5u))
            ++g786UnmSlot(base >> 5u).comp;
    }
    if ((g_g786UnmDrains % 2048u) == 0u)
    {
        std::fprintf(stderr, "[G786:unm] drains=%llu", (unsigned long long)g_g786UnmDrains);
        for (const G786Unm &u : g_g786Unm)
            if (u.fbp != 0u)
                std::fprintf(stderr, " w%x:sil=%llu(fbw!8=%llu next:mixed=%llu sameW=%llu other=%llu last=%llu) comp=%llu",
                             u.fbp, (unsigned long long)u.sil, (unsigned long long)u.silFbw,
                             (unsigned long long)u.silNextMixed, (unsigned long long)u.silNextSameW,
                             (unsigned long long)u.silNextOther, (unsigned long long)u.silLast,
                             (unsigned long long)u.comp);
        std::fputc(10, stderr);
    }
}

static void g786OutlineGraphPass()
{
    const bool census = g786CensusOn();
    const bool cls = g786ClassOn();
    // A ceiling probe (DC2_G786_OLPROBE) replaces the versioned merge for its run; otherwise the
    // default-ON versioned merge (mode 5) applies.
    const int mode = g786ProbeMode() != 0 ? g786ProbeMode() : (g786VmergeOn() ? 5 : 0);
    if (!census && !cls && mode == 0)
        return;
    std::vector<G260Batch> &g = g_g260Graph;
    const size_t nb = g.size();
    g_g786Tags.assign(nb, 0u);
    if (mode == 5)
    {
        if (g_g786Plans.size() < nb)
            g_g786Plans.resize(nb);
        g_g786HasPlan.assign(nb, 0u);
        g786VmReport();
    }
    ++g_g786C.drains;
    size_t i = 0;
    while (i < nb)
    {
        // A run starts at a silhouette batch S on W immediately followed by a composite of W.
        if (g[i].entries.empty() || i + 1 >= nb)
        {
            ++i;
            continue;
        }
        const uint32_t wFbp = g[i].fbp;
        const uint32_t wFbw = g[i].entries.front().ctx.frame.fbw;
        const uint32_t wPsm = g[i].entries.front().ctx.frame.psm;
        G786Rect rd0;
        bool zf0 = true;
        if (!g786IsComposite(g[i + 1], wFbp, wFbw, wPsm, rd0, zf0))
        {
            ++i;
            continue;
        }
        const uint32_t cFbp = g[i + 1].fbp;
        const uint32_t cFbw = g[i + 1].entries.front().ctx.frame.fbw;
        std::vector<G786Pair> run;
        size_t j = i;
        G260RangeSet runSWr; // VMERGE: colour + Z written by the run's silhouettes so far
        while (j < nb && !g[j].entries.empty() && g[j].fbp == wFbp &&
               g[j].entries.front().ctx.frame.fbw == wFbw)
        {
            // A silhouette that READS (texture / CLUT) what an earlier silhouette of the run wrote
            // must see it as of its own flush — e.g. an entry sampling W itself, which its flush
            // binds through a copy or the atlas taken at flush prep. One merged S* flush preps once,
            // before S1, so such a silhouette ends the run (it starts the next one). §7d.
            // Opt-in (DC2_G786_SPLITSELF=1): priced at +0.21 ms/f and it changed no pixel (§7e/§7h).
            static const bool s_g786Split = envFlagEnabled("DC2_G786_SPLITSELF");
            if (mode == 5 && s_g786Split && !run.empty() && g523SetsOverlap(g[j].rd, runSWr))
            {
                ++g_g786Vm.splitSelf;
                break;
            }
            G786Pair p;
            p.s = j;
            p.wr = g786WriteRect(g[j].entries);
            G786Rect rd;
            bool zf = true;
            if (j + 1 < nb && g[j + 1].fbp == cFbp && !g[j + 1].entries.empty() &&
                g[j + 1].entries.front().ctx.frame.fbw == cFbw &&
                g786IsComposite(g[j + 1], wFbp, wFbw, wPsm, rd, zf))
            {
                p.c = j + 1;
                p.rd = rd;
                p.zfree = zf;
                run.push_back(p);
                g786MergeSets(runSWr, g[j].wr);
                j += 2;
                continue;
            }
            // Trailing silhouette (its composite is in the next drain) only when it looks like one:
            // an UNTEXTURED clear sprite followed by at least one triangle. (A textured sprite here is
            // a different pass on W — e.g. dungeon1's display-to-0x139 screen copy — never hoisted.)
            const std::vector<G144Entry> &te = g[j].entries;
            if (!run.empty() && te.size() >= 2u && te.front().prim.type == GS_PRIM_SPRITE &&
                !te.front().prim.tme && te[1].prim.type != GS_PRIM_SPRITE)
            {
                p.c = SIZE_MAX;
                run.push_back(p);
                g786MergeSets(runSWr, g[j].wr);
                ++j;
            }
            break;
        }
        const size_t end = j; // run occupies [i, end)
        size_t nPairs = 0;
        for (const G786Pair &p : run)
            nPairs += p.c != SIZE_MAX ? 1u : 0u;
        for (const G786Pair &p : run)
        {
            g_g786Tags[p.s] = 1u;
            if (p.c != SIZE_MAX)
                g_g786Tags[p.c] = 2u;
        }
        // ---- exact greedy grouping (also the census answer) ----
        std::vector<size_t> groupStart; // index into run where each group starts
        {
            G786Rect groupReads;
            bool open = false;
            for (size_t r = 0; r < run.size(); ++r)
            {
                const G786Pair &p = run[r];
                bool fits = open && !p.wr.overlaps(groupReads);
                if (fits)
                    for (size_t q = groupStart.back(); q < r && fits; ++q)
                    {
                        const G786Pair &o = run[q];
                        if (o.c == SIZE_MAX || !o.zfree || !p.zfree)
                        {
                            fits = false;
                            break;
                        }
                        // block-level: S_r must not read what C_q writes, nor write what C_q writes
                        if (g523SetsOverlap(g[p.s].rd, g[o.c].wr) || g523SetsOverlap(g[p.s].wr, g[o.c].wr))
                            fits = false;
                    }
                if (!fits)
                {
                    groupStart.push_back(r);
                    groupReads = G786Rect{};
                    open = true;
                }
                if (p.c != SIZE_MAX)
                    groupReads.add(p.rd);
            }
        }
        if (census)
        {
            G786Census &k = g_g786C;
            ++k.runs;
            k.pairs += nPairs;
            k.trailing += run.size() - nPairs;
            const size_t L = nPairs;
            k.runLen[L <= 1 ? 0 : L == 2 ? 1 : L == 3 ? 2 : L < 8 ? 3 : L < 16 ? 4 : 5]++;
            uint32_t slot = 0;
            for (; slot < 4u; ++slot)
                if (k.wFbps[slot] == wFbp || k.wPairs[slot] == 0u)
                    break;
            if (slot < 4u) { k.wFbps[slot] = wFbp; k.wPairs[slot] += nPairs; }
            for (size_t r = 0; r < run.size(); ++r)
            {
                const G786Pair &p = run[r];
                for (const G144Entry &e : g[p.s].entries)
                {
                    ++k.sEntries;
                    if (e.prim.type == GS_PRIM_SPRITE) ++k.sSprites; else ++k.sTris;
                    g786NoteSig(k.sigS, k.sigSN, k.nSigS, g786Sig(e), e, "S");
                }
                k.sArea += p.wr.area();
                if (p.c != SIZE_MAX)
                {
                    k.cEntries += g[p.c].entries.size();
                    k.cArea += p.rd.area();
                    if (!p.zfree) ++k.zUse;
                    g786NoteSig(k.sigC, k.sigCN, k.nSigC, g786Sig(g[p.c].entries.front()),
                                g[p.c].entries.front(), "C");
                }
                for (size_t q = 0; q < r; ++q)
                {
                    const G786Pair &o = run[q];
                    if (o.c == SIZE_MAX) continue;
                    if (p.wr.overlaps(o.rd)) ++k.rectOverlapPairs;
                    if (g523SetsOverlap(g[p.s].rd, g[o.c].wr) || g523SetsOverlap(g[p.s].wr, g[o.c].wr))
                        ++k.blockConflict;
                }
            }
            k.origFlushes += run.size() + nPairs;
            k.exactGroups += groupStart.size();
            // each group: one S batch + one C batch (if it has any composite)
            for (size_t gi = 0; gi < groupStart.size(); ++gi)
            {
                const size_t a = groupStart[gi];
                const size_t b = gi + 1 < groupStart.size() ? groupStart[gi + 1] : run.size();
                bool anyC = false;
                for (size_t r = a; r < b; ++r) anyC = anyC || run[r].c != SIZE_MAX;
                k.exactFlushes += 1u + (anyC ? 1u : 0u);
            }
        }
        if (mode == 4)
        {
            // ⛔ BACKEND CEILING PROBE (corrupting: outlines disappear): the executor never runs the
            // run's silhouettes or composites; the whole front end (EE, VU1, parse) still produced them.
            for (const G786Pair &p : run)
            {
                g712PrepRelease(g[p.s].prep);
                g[p.s].entries.clear();
                g_g786Tags[p.s] = 0u;
                if (p.c != SIZE_MAX)
                {
                    g712PrepRelease(g[p.c].prep);
                    g[p.c].entries.clear();
                    g_g786Tags[p.c] = 0u;
                }
                ++g_g786C.probeMerged;
            }
        }
        if (mode == 5 && nPairs >= 1u && run.size() >= 2u)
            (void)g786VersionedMerge(g, i, end, run);
        // G787 CEILING PROBE (corrupting: outlined characters disappear): the executor skips the
        // run's COMPOSITE batches only (after the VMERGE rewrite); silhouettes, snapshots and the
        // whole front end still run. Prices the executor share of the composite.
        static const bool s_g787XSkipC = envFlagEnabled("DC2_G787_XSKIPC");
        if (s_g787XSkipC)
        {
            static uint64_t s_skipped = 0, s_entries = 0;
            for (size_t r = i; r < end && r < g_g786Tags.size(); ++r)
                if (g_g786Tags[r] == 2u)
                {
                    g712PrepRelease(g[r].prep);
                    s_entries += g[r].entries.size();
                    g[r].entries.clear();
                    g_g786Tags[r] = 0u;
                    if (r < g_g786HasPlan.size())
                        g_g786HasPlan[r] = 0u;
                    if ((++s_skipped % 20000u) == 1u)
                        std::fprintf(stderr, "[G787:xskipc] batches=%llu entries=%llu\n",
                                     (unsigned long long)s_skipped, (unsigned long long)s_entries);
                }
        }
        if (mode >= 1 && mode <= 3 && run.size() >= 2u)
        {
            // Rebuild [i, end): release every prepared class pass first (a worker may hold it).
            for (size_t r = i; r < end; ++r)
                g712PrepRelease(g[r].prep);
            std::vector<G260Batch> src;
            src.reserve(end - i);
            for (size_t r = i; r < end; ++r)
                src.push_back(std::move(g[r]));
            std::vector<G260Batch> out;
            std::vector<uint8_t> outTag;
            auto absorb = [](G260Batch &dst, G260Batch &from) {
                dst.entries.insert(dst.entries.end(), std::make_move_iterator(from.entries.begin()),
                                   std::make_move_iterator(from.entries.end()));
                from.entries.clear();
                g786MergeSets(dst.wr, from.wr);
                g786MergeSets(dst.rd, from.rd);
            };
            std::vector<std::pair<size_t, size_t>> groups; // [a,b) in run
            if (mode == 3)
                for (size_t gi = 0; gi < groupStart.size(); ++gi)
                    groups.emplace_back(groupStart[gi],
                                        gi + 1 < groupStart.size() ? groupStart[gi + 1] : run.size());
            else
                groups.emplace_back(0u, run.size());
            for (const auto &gr : groups)
            {
                if (mode == 1)
                {
                    for (size_t r = gr.first; r < gr.second; ++r)
                    {
                        out.push_back(std::move(src[run[r].s - i]));
                        outTag.push_back(1u);
                    }
                }
                else
                {
                    G260Batch sm = std::move(src[run[gr.first].s - i]);
                    for (size_t r = gr.first + 1; r < gr.second; ++r)
                    {
                        absorb(sm, src[run[r].s - i]);
                        ++g_g786C.probeMerged;
                    }
                    out.push_back(std::move(sm));
                    outTag.push_back(1u);
                }
                G260Batch cm;
                bool haveC = false;
                for (size_t r = gr.first; r < gr.second; ++r)
                {
                    if (run[r].c == SIZE_MAX) continue;
                    if (!haveC) { cm = std::move(src[run[r].c - i]); haveC = true; }
                    else { absorb(cm, src[run[r].c - i]); ++g_g786C.probeMerged; }
                }
                if (haveC)
                {
                    out.push_back(std::move(cm));
                    outTag.push_back(2u);
                }
            }
            // Write back; leftover slots become empty batches (the executor skips them).
            size_t w = i;
            for (size_t r = 0; r < out.size(); ++r, ++w)
            {
                g[w] = std::move(out[r]);
                g[w].prep = nullptr;
                g_g786Tags[w] = outTag[r];
            }
            for (; w < end; ++w)
            {
                g[w] = G260Batch{};
                g_g786Tags[w] = 0u;
            }
        }
        i = std::max(end, i + 1);
    }
    if (census)
    {
        g786ClassifyUnmatched(g);
        g786Report();
    }
}
