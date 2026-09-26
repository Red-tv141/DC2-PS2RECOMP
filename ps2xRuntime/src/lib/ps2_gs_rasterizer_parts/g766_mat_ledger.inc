// ===== G766: THE MATERIALISATION LEDGER — WHO FORCES GPU->CPU, AT WHICH SITE =====
//
// `[G261:stat] mat(cpu l2l tex rsync ...)` books a publication by CAUSE, and a cause is shared by
// several call sites that have nothing in common (three `kG261MatResync` sites, six
// `kG261MatTexAlias` sites, the l2l pin, the l2l consume prefix and G714). G766's job is to delete
// publications, and a publication can only be deleted where its CALLER is, so the ledger keys on
// the call site as well: every g261Materialize caller sets `t_g766MatSite` through `G766MatSite`.
//
// Behaviour-pure: the tag is one thread-local store per publication (a path that already performs
// a blocking glReadPixels), and the ledger itself only runs under DC2_G766_STAT=1.
//
//   DC2_G766_STAT=1   [G766:mat] site x target x cause, with rows, every 1024 publications
//                     [G766:matd] the first 6 publications of every site, with the dirty window

enum G766MatSiteId : int
{
    kG766SiteUnknown = 0,
    kG766SiteCpuReplay,      // g261PrepareCpuReplay
    kG766SiteInline,         // g261PrepareInline
    kG766SiteAllDirty,       // g261MaterializeAllDirty (fail-closed / unknown range)
    kG766SiteRanges,         // g261MaterializeForRanges (transfer / upload / pin edges)
    kG766SiteL2lConsume,     // g285PrepareL2lConsume prefix publication
    kG766SiteG714,           // g714 raw l2l (default OFF)
    kG766SiteG716,           // g716 authority retire
    kG766SiteT8Atlas,        // T8 atlas builders
    kG766SiteT8Map,          // T8 map resolver
    kG766SiteRsyncOracle,    // G553/G557 heavy-verify boundary
    kG766SiteRsyncWidth,     // width flip of a resident target
    kG766SiteRsyncWindow,    // wave renders outside the FBO-defined rows
    kG766SiteTexBind,        // rejected FBO-direct texture bind
    kG766SiteT8ViewFail,     // G281 view build failure
    kG766SiteTexOther,       // the remaining texture-alias site
    kG766SiteOwnerFallback,  // G766: page owner published because pulls are unavailable
    kG766SiteCount
};
static const char *const kG766SiteName[kG766SiteCount] = {
    "unknown", "cpuReplay", "inline", "allDirty", "ranges", "l2lConsume", "g714", "g716",
    "t8atlas", "t8map", "rsyncOracle", "rsyncWidth", "rsyncWindow", "texBind", "t8viewFail",
    "texOther", "ownerFallback"};

static thread_local int t_g766MatSite = kG766SiteUnknown;
struct G766MatSite
{
    int prev;
    explicit G766MatSite(int site) : prev(t_g766MatSite) { t_g766MatSite = site; }
    ~G766MatSite() { t_g766MatSite = prev; }
    G766MatSite(const G766MatSite &) = delete;
    G766MatSite &operator=(const G766MatSite &) = delete;
};

static bool g766StatOn()
{
    static const bool on = envFlagEnabled("DC2_G766_STAT");
    return on;
}

struct G766MatCell
{
    uint64_t n = 0u, rows = 0u;
};
// Flush-owner threads only (the same contract as every G261 structure).
static G766MatCell g_g766Mat[kG766SiteCount][kG248TargetCount][8];
static uint64_t g_g766MatTotal = 0u;
static uint32_t g_g766MatDetail[kG766SiteCount] = {};

static void g766MatReport()
{
    std::fprintf(stderr, "[G766:matsum] total=%llu\n",
                 static_cast<unsigned long long>(g_g766MatTotal));
    for (int s = 0; s < kG766SiteCount; ++s)
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            for (int c = 0; c < 8; ++c)
            {
                const G766MatCell &m = g_g766Mat[s][t][c];
                if (m.n == 0u)
                    continue;
                std::fprintf(stderr,
                             "[G766:mat] site=%s fbp=0x%03x cause=%d n=%llu rows=%llu "
                             "rowsPer=%.1f\n",
                             kG766SiteName[s], kG261Fbp[t], c,
                             static_cast<unsigned long long>(m.n),
                             static_cast<unsigned long long>(m.rows),
                             static_cast<double>(m.rows) / static_cast<double>(m.n));
            }
}

static bool g766SeqWindow();
static uint64_t g_g766SeqN = 0u;
// Called at the top of g261Materialize for a DIRTY target (before any state changes).
static void g766NoteMat(int ti, int cause, const G261Res &r)
{
    if (g766SeqWindow() && ti >= 0 && static_cast<uint32_t>(ti) < kG248TargetCount)
        std::fprintf(stderr,
                     "[G766:seq] sf=%u i=%llu   MAT site=%s fbp=0x%03x cause=%d fbw=%u "
                     "dirty=%d..%d cov=%d..%d\n",
                     g_dc2ScriptFrame.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(++g_g766SeqN),
                     kG766SiteName[(t_g766MatSite >= 0 && t_g766MatSite < kG766SiteCount) ? t_g766MatSite : 0],
                     kG261Fbp[ti], cause, r.fbw, r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    if (!g766StatOn() || ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    int site = t_g766MatSite;
    if (site < 0 || site >= kG766SiteCount)
        site = kG766SiteUnknown;
    const int rows = r.dirtyHi >= r.dirtyLo ? r.dirtyHi - r.dirtyLo + 1 : 0;
    G766MatCell &m = g_g766Mat[site][ti][cause & 7];
    ++m.n;
    m.rows += static_cast<uint64_t>(rows);
    if (g_g766MatDetail[site] < 6u)
    {
        ++g_g766MatDetail[site];
        std::fprintf(stderr,
                     "[G766:matd] site=%s fbp=0x%03x cause=%d fbw=%u dirty=%d..%d cov=%d..%d "
                     "win=%d..%d sf=%u\n",
                     kG766SiteName[site], kG261Fbp[ti], cause, r.fbw, r.dirtyLo, r.dirtyHi,
                     r.covLo, r.covHi, r.winLo, r.winHi,
                     g_dc2ScriptFrame.load(std::memory_order_relaxed));
    }
    if ((++g_g766MatTotal % 1024u) == 0u)
        g766MatReport();
}

// ===== G766 ORDERED FRAME TRACE (DC2_G766_SEQ=1, window DC2_G766_SEQ_LO/_HI script frames) =====
// One line per GPU flush attempt, per transfer edge, per publication and per batch decision, in
// program order, so the authority chain of one frame (who renders a page, who reads it, where it
// crosses to the CPU) can be read directly instead of being inferred from bucketed censuses.
static bool g766SeqWindow()
{
    static const bool on = envFlagEnabled("DC2_G766_SEQ");
    if (!on)
        return false;
    static const uint32_t lo = [] {
        const char *e = std::getenv("DC2_G766_SEQ_LO");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    static const uint32_t hi = [] {
        const char *e = std::getenv("DC2_G766_SEQ_HI");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    return sf >= lo && (hi == 0u || sf <= hi);
}

static void g766SeqBatch(const std::vector<G144Entry> &list, const char *cls)
{
    if (list.empty() || !g766SeqWindow())
        return;
    const G144Entry &e0 = list[0];
    uint32_t nTri = 0u, nSpr = 0u, nOther = 0u, nTme = 0u, nAbe = 0u, nZte = 0u;
    int yLo = 1 << 20, yHi = -1;
    uint32_t tex[3][3] = {};
    uint32_t nTex = 0u;
    for (const G144Entry &e : list)
    {
        if (e.prim.type == GS_PRIM_SPRITE) ++nSpr;
        else if (e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
                 e.prim.type == GS_PRIM_TRIFAN) ++nTri;
        else ++nOther;
        if (e.prim.abe) ++nAbe;
        if (((e.ctx.test >> 16) & 1ull) != 0ull) ++nZte;
        yLo = std::min(yLo, e.minY);
        yHi = std::max(yHi, e.maxY);
        if (e.prim.tme)
        {
            ++nTme;
            bool seen = false;
            for (uint32_t k = 0; k < nTex; ++k)
                if (tex[k][0] == e.ctx.tex0.tbp0 && tex[k][1] == e.ctx.tex0.tbw &&
                    tex[k][2] == e.ctx.tex0.psm)
                    seen = true;
            if (!seen && nTex < 3u)
            {
                tex[nTex][0] = e.ctx.tex0.tbp0;
                tex[nTex][1] = e.ctx.tex0.tbw;
                tex[nTex][2] = e.ctx.tex0.psm;
                ++nTex;
            }
        }
    }
    char tb[160];
    int o = 0;
    tb[0] = '\0';
    for (uint32_t k = 0; k < nTex; ++k)
        o += std::snprintf(tb + o, sizeof(tb) - o, " t(%x/%u/%02x)", tex[k][0], tex[k][1],
                           tex[k][2]);
    std::fprintf(stderr,
                 "[G766:seq] sf=%u i=%llu BATCH %s fbp=0x%03x fbw=%u psm=%02x zbp=%03x n=%zu "
                 "tri=%u spr=%u oth=%u tme=%u abe=%u zte=%u y=%d..%d sc=%u..%u,%u..%u%s\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(++g_g766SeqN), cls, e0.ctx.frame.fbp,
                 e0.ctx.frame.fbw, e0.ctx.frame.psm,
                 static_cast<unsigned>(e0.ctx.zbuf & 0x1FFu), list.size(), nTri, nSpr, nOther,
                 nTme, nAbe, nZte, yLo, yHi, e0.ctx.scissor.x0, e0.ctx.scissor.x1,
                 e0.ctx.scissor.y0, e0.ctx.scissor.y1, tb);
}

static void g766SeqDecision(uint32_t fbp, bool uploadFb, bool skipReadback, int rowLo, int rowHi,
                            int upLo, int upHi)
{
    if (!g766SeqWindow())
        return;
    std::fprintf(stderr,
                 "[G766:seq] sf=%u i=%llu   decide fbp=0x%03x upload=%d skipRb=%d rows=%d..%d "
                 "up=%d..%d\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(++g_g766SeqN), fbp, uploadFb ? 1 : 0,
                 skipReadback ? 1 : 0, rowLo, rowHi, upLo, upHi);
}

static void g766SeqXfer(const char *kind, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax,
                        uint32_t ssay, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t dsax,
                        uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    if (!g766SeqWindow())
        return;
    std::fprintf(stderr,
                 "[G766:seq] sf=%u i=%llu XFER %s s(%x/%u/%02x @%u,%u) d(%x/%u/%02x @%u,%u) "
                 "%ux%u\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(++g_g766SeqN), kind, sbp, sbw, spsm, ssax, ssay,
                 dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
}

static void g766SeqNote(const char *what, uint32_t a, int b, int c)
{
    if (!g766SeqWindow())
        return;
    std::fprintf(stderr, "[G766:seq] sf=%u i=%llu   %s a=0x%x b=%d c=%d\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(++g_g766SeqN), what, a, b, c);
}
