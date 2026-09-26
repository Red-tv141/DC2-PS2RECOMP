// ===== G765: A WRITE-ONLY FILL CONSUMES THE RESIDENT PAGES IT COVERS — NO READBACK =====
//
// THE ROUND TRIP. `dragon` clears a 512x416 CT32 surface at 0x15b (and 0x151) with one opaque
// untextured SPRITE every frame or two. 0x15b is a discovered target (`cpuonlyrtt`), so the fill is
// a CPU band replay, and `g261PrepareCpuReplay` first materialises every dirty resident target its
// colour range overlaps: 0x15b lies 34 pages inside 0x139 and 2 pages inside 0x159, so the clear
// costs two synchronous full-window readbacks (448 + 416 rows) that drain the whole GPU queue —
// 15.1 % of the executor's wall in G765's stack profile — and then the next 0x139/0x159 batch
// re-uploads the whole target because the replay moved its page generations.
//
// WHY THE READBACK IS DEAD. A fill reads nothing from its destination: no texture, no blending, no
// alpha/destination-alpha test, no depth test, full FBMSK, and a sprite that covers its whole
// scissor. Every page it FULLY covers is overwritten in guest VRAM whatever the FBO held, so
// publishing the FBO into those pages first is a write-after-write with no reader in between. The
// only pages that need the FBO's bytes are pages the fill touches PARTIALLY — then this lever
// declines and the shipped materialise runs.
//
// THE MODEL. For a resident target whose overlapped dirty pages are all fully covered, skip the
// materialise and arm the existing G264 mirror (pixel-exact, bitmask per row) over exactly those
// pages. The replay writes guest VRAM; the mirror pushes those VRAM bytes into the FBO at the next
// flush edge (g264FlushMirrorsAtFlush, and g261Materialize flushes a pending mirror first). The
// target stays resident and dirty for every page the fill did not touch, so nothing is read back
// and the whole-target re-upload disappears too. G536/G597 page ownership treats the mirrored pages
// exactly as an upload mirror's.
//
// Rollback DC2_G765_NO_FILL_CONSUME=1. Census DC2_G765_STAT=1 ([G765:fill]).

static std::atomic<uint64_t> g_g765FillEntries{0}, g_g765FillConsumed{0}, g_g765FillDeclined{0},
    g_g765FillMirPages{0};

static bool g765FillConsumeOn()
{
    static const bool on = !envFlagEnabled("DC2_G765_NO_FILL_CONSUME") &&
                           !envFlagDisabled("DC2_G765_FILL_CONSUME");
    return on && g264On() && g261WaveOn();
}

// The pixel rectangle an entry certainly writes and never reads, or false. Every conjunct fails
// closed: anything that can discard a pixel, read the destination, or leave part of the scissor
// uncovered disqualifies the entry.
// The alpha test of a FLAT untextured sprite sees one fragment alpha — the kicking (second)
// vertex's — so its outcome is the same for every pixel and can be evaluated exactly.
static bool g765FlatAlphaTestPasses(uint32_t atst, uint32_t aref, uint32_t a)
{
    switch (atst)
    {
    case 1u: return true;           // ALWAYS
    case 2u: return a < aref;       // LESS
    case 3u: return a <= aref;      // LEQUAL
    case 4u: return a == aref;      // EQUAL
    case 5u: return a >= aref;      // GEQUAL
    case 6u: return a > aref;       // GREATER
    case 7u: return a != aref;      // NOTEQUAL
    default: return false;          // NEVER (or unknown): not a full write
    }
}

// drawSprite takes the ceil rule unless DC2_G365_NO_SPRITE_EDGE is set or DC2_G367_SEL excludes
// bit 2 ("everything else", which is every untextured sprite). Mirror that decision exactly.
static bool g765SpriteCeilRuleActive()
{
    static const bool on = []() {
        if (envFlagEnabled("DC2_G365_NO_SPRITE_EDGE"))
            return false;
        const char *sel = std::getenv("DC2_G367_SEL");
        return sel == nullptr || (std::atoi(sel) & 4) != 0;
    }();
    return on;
}

static void g765NoteFillReject(const G144Entry &e, const char *why)
{
    if (!g765StatOn())
        return;
    static uint32_t s_n = 0u; // flush-owner thread only
    if (s_n++ >= 12u)
        return;
    const GSContext &c = e.ctx;
    std::fprintf(stderr,
                 "[G765:fillrej] why=%s fbp=0x%x fbw=%u psm=0x%x fbmsk=0x%x test=0x%llx "
                 "prim(t=%u tme=%u abe=%u fge=%u aa1=%u) v0=(%.2f,%.2f a=%u) v1=(%.2f,%.2f a=%u) "
                 "of=(%u,%u) sc=%u,%u..%u,%u\n",
                 why, c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk,
                 static_cast<unsigned long long>(c.test), static_cast<unsigned>(e.prim.type),
                 e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u, e.prim.fge ? 1u : 0u,
                 e.prim.aa1 ? 1u : 0u, e.v[0].x, e.v[0].y, e.v[0].a, e.v[1].x, e.v[1].y,
                 e.v[1].a, c.xyoffset.ofx >> 4, c.xyoffset.ofy >> 4, c.scissor.x0,
                 c.scissor.y0, c.scissor.x1, c.scissor.y1);
}

static bool g765PureFillRect(const G144Entry &e, int &x0, int &y0, int &x1, int &y1)
{
    const GSContext &c = e.ctx;
    if (e.prim.type != GS_PRIM_SPRITE || e.prim.tme)
        return false; // not the population; silent
    if (e.prim.abe || e.prim.fge || e.prim.aa1)
    {
        g765NoteFillReject(e, "prim");
        return false;
    }
    if (c.frame.psm != GS_PSM_CT32 || c.frame.fbmsk != 0u || c.frame.fbw == 0u ||
        c.frame.fbw > 8u)
    {
        g765NoteFillReject(e, "frame");
        return false;
    }
    const uint64_t test = c.test;
    const bool ate = (test & 1ull) != 0ull;
    const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7ull);
    const uint32_t aref = static_cast<uint32_t>((test >> 4) & 0xFFull);
    const bool date = ((test >> 14) & 1ull) != 0ull;
    const bool zte = ((test >> 16) & 1ull) != 0ull;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
    if ((ate && !g765FlatAlphaTestPasses(atst, aref, e.v[1].a)) || date ||
        (zte && ztst != 1u))
    {
        g765NoteFillReject(e, "test");
        return false;
    }
    if (c.scissor.x1 < c.scissor.x0 || c.scissor.y1 < c.scissor.y0)
        return false;
    // The EXACT pixel rectangle drawSprite writes under the G365 ceil rule (its default path for an
    // untextured sprite): covered [ceil(x0), ceil(x1) - 1], nothing when that span is empty, then
    // clipped to the scissor. Every pixel inside is written (no test above can discard one).
    if (!g765SpriteCeilRuleActive())
    {
        g765NoteFillReject(e, "rule");
        return false;
    }
    const int ofx = static_cast<int>(c.xyoffset.ofx >> 4);
    const int ofy = static_cast<int>(c.xyoffset.ofy >> 4);
    float fx0 = e.v[0].x - static_cast<float>(ofx), fx1 = e.v[1].x - static_cast<float>(ofx);
    float fy0 = e.v[0].y - static_cast<float>(ofy), fy1 = e.v[1].y - static_cast<float>(ofy);
    if (fx0 > fx1)
        std::swap(fx0, fx1);
    if (fy0 > fy1)
        std::swap(fy0, fy1);
    const int cx0 = static_cast<int>(std::ceil(fx0)), cx1 = static_cast<int>(std::ceil(fx1));
    const int cy0 = static_cast<int>(std::ceil(fy0)), cy1 = static_cast<int>(std::ceil(fy1));
    if (cx1 <= cx0 || cy1 <= cy0)
    {
        g765NoteFillReject(e, "empty");
        return false;
    }
    x0 = std::max<int>(cx0, c.scissor.x0);
    y0 = std::max<int>(cy0, c.scissor.y0);
    x1 = std::min<int>(cx1 - 1, c.scissor.x1);
    y1 = std::min<int>(cy1 - 1, c.scissor.y1);
    x1 = std::min<int>(x1, static_cast<int>(c.frame.fbw) * 64 - 1);
    y1 = std::min<int>(y1, 2047);
    if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0)
    {
        g765NoteFillReject(e, "clip");
        return false;
    }
    return true;
}

static void g765NoteDecline(uint32_t ti, const char *why, uint32_t page)
{
    if (!g765StatOn())
        return;
    static uint32_t s_n = 0u; // flush-owner thread only
    if (s_n++ >= 12u)
        return;
    const G261Res &r = g_g261Res[ti];
    std::fprintf(stderr,
                 "[G765:filldecline] why=%s t=0x%x page=0x%x fbw=%u fbW=%d dirty=%d..%d "
                 "cov=%d..%d\n",
                 why, kG261Fbp[ti], page, r.fbw, r.fbW, r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
}

// Decide for one (write-only sprite, dirty resident target) pair. PIXEL-EXACT: every pixel the
// sprite writes is mapped through the GS page geometry into the target's surface and mirrored
// (VRAM -> FBO) after the replay; no pixel it does not write is ever mirrored. The untouched pixels of
// a partially written page stay FBO-authoritative: G536 would keep a gen-moved page as guest-owned
// at publication, but the mirror flush (which g261Materialize runs first) re-anchors the patched page
// rows (G597), so the page publishes whole from an FBO that is exact everywhere.
static bool g765TryConsumeTargetByFill(const G144Entry &e, int fx0, int fy0, int fx1, int fy1,
                                       uint32_t ti)
{
    G261Res &r = g_g261Res[ti];
    if (!r.dirty || !r.range.valid || r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || r.fbW > 512 ||
        r.dirtyHi < r.dirtyLo || r.dirtyLo < 0 || r.dirtyHi > 511)
    {
        g765NoteDecline(ti, "state", 0u);
        return false;
    }
    const uint32_t tcols = static_cast<uint32_t>(r.fbW) / 64u;
    if (tcols == 0u || tcols > r.fbw || static_cast<uint32_t>(r.fbW) != tcols * 64u)
    {
        g765NoteDecline(ti, "cols", 0u);
        return false;
    }
    const uint32_t F = e.ctx.frame.fbw;
    const uint32_t fillPage0 = e.ctx.frame.fbp; // FBP is in pages
    const uint32_t targetPage0 = kG261Fbp[ti];
    const uint32_t tPages = 16u * r.fbw;         // the target surface is walked rows 0..511
    struct Patch { uint32_t tx0, tx1, ty0, ty1; };
    Patch patches[64];
    uint32_t nPatch = 0u;
    const uint32_t pr0 = static_cast<uint32_t>(fy0) / 32u, pr1 = static_cast<uint32_t>(fy1) / 32u;
    const uint32_t pc0 = static_cast<uint32_t>(fx0) / 64u, pc1 = static_cast<uint32_t>(fx1) / 64u;
    for (uint32_t pr = pr0; pr <= pr1; ++pr)
        for (uint32_t pc = pc0; pc <= pc1; ++pc)
        {
            const uint32_t page = (fillPage0 + pr * F + pc) & 511u;
            const uint32_t q = (page - targetPage0) & 511u;
            if (q >= tPages)
                continue; // this page is not part of the target's surface
            const uint32_t tr = q / r.fbw, tc = q % r.fbw;
            const uint32_t sx0 = std::max<uint32_t>(static_cast<uint32_t>(fx0), pc * 64u) - pc * 64u;
            const uint32_t sx1 = std::min<uint32_t>(static_cast<uint32_t>(fx1), pc * 64u + 63u) - pc * 64u;
            const uint32_t sy0 = std::max<uint32_t>(static_cast<uint32_t>(fy0), pr * 32u) - pr * 32u;
            const uint32_t sy1 = std::min<uint32_t>(static_cast<uint32_t>(fy1), pr * 32u + 31u) - pr * 32u;
            const int rowLo = static_cast<int>(tr * 32u + sy0), rowHi = static_cast<int>(tr * 32u + sy1);
            const bool dirtyRows = rowHi >= r.dirtyLo && rowLo <= r.dirtyHi;
            if (tc >= tcols)
            {
                if (dirtyRows)
                {
                    g765NoteDecline(ti, "outcols", page);
                    return false; // the FBO lacks these columns but claims the rows: publish
                }
                continue;
            }
            if (!dirtyRows && !(rowHi >= r.covLo - 1 && rowLo <= r.covHi + 1))
                continue; // clean and outside the claimed coverage: nothing to keep current
            if (nPatch >= 64u)
            {
                g765NoteDecline(ti, "many", page);
                return false;
            }
            patches[nPatch++] = Patch{tc * 64u + sx0, tc * 64u + sx1,
                                      static_cast<uint32_t>(rowLo), static_cast<uint32_t>(rowHi)};
        }
    if (nPatch == 0u)
    {
        g765NoteDecline(ti, "nohit", 0u);
        return false; // the shipped path decides
    }
    const bool g291On = g291AtlasOn();
    if (g291On && r.g291CovFbw != r.fbw)
    {
        r.g291Cov = {};
        r.g291CovFbw = r.fbw;
        g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
    }
    int lo = 512, hi = -1;
    uint64_t px = 0u;
    for (uint32_t i = 0u; i < nPatch; ++i)
    {
        const Patch &pt = patches[i];
        for (uint32_t y = pt.ty0; y <= pt.ty1 && y < 512u; ++y)
            for (uint32_t x = pt.tx0; x <= pt.tx1; ++x)
            {
                r.mirBits[y][x >> 6] |= 1ull << (x & 63u);
                if (g291On)
                    r.g291Cov[y][x >> 6] |= 1ull << (x & 63u);
            }
        px += static_cast<uint64_t>(pt.tx1 - pt.tx0 + 1u) * (pt.ty1 - pt.ty0 + 1u);
        lo = std::min(lo, static_cast<int>(pt.ty0));
        hi = std::max(hi, static_cast<int>(std::min(511u, pt.ty1)));
    }
    r.mirPending = true;
    if (lo < r.mirLo) r.mirLo = lo;
    if (hi > r.mirHi) r.mirHi = hi;
    g_g264MirNoted.fetch_add(1u, std::memory_order_relaxed);
    g_g765FillMirPages.fetch_add(px, std::memory_order_relaxed);
    return true;
}

static void g765FillReport()
{
    if (!g765StatOn())
        return;
    static uint64_t s_n = 0u; // flush-owner thread only
    if ((++s_n % 256u) != 1u)
        return;
    std::fprintf(stderr, "[G765:fill] entries=%llu consumed=%llu declined=%llu mirPx=%llu\n",
                 (unsigned long long)g_g765FillEntries.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765FillConsumed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765FillDeclined.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765FillMirPages.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G765:t8memo] hits=%llu\n",
                 (unsigned long long)g_g765T8MemoHits.load(std::memory_order_relaxed));
}
