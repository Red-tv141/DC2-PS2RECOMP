// ===== G769: ONE AUTHORITY RULE FOR EVERY READER AND WRITER OF A RESIDENT PAGE =====
//
// G766 gave every GS page an owner (the resident FBO holding its newest bytes, or none = guest VRAM
// is newest) and G767/G768 taught the GPU consumers to resolve pages through it. The CPU-side edges
// still decided by WINDOW OVERLAP: "this read/write touches a dirty resident's claimed window, so
// publish that resident" — a blocking glReadPixels of ~430 rows whose bytes, for a page no FBO
// owns, are already in guest VRAM. `[G766:mat]` on g768c:
//   fight    texBind 0x139 1,899 / 60 s   cpuReplay 0x139 1,466 + 0x13c 1,186
//   georama  texBind 0x139 2,194 / 60 s   (T8 FST sprite reading 0x139's own base)
//   s03      cpuReplay 0x139 229 + 0x13a 121
// This file holds the two rules that replace overlap with authority at those edges:
//
//   READ   (texture bind, legacy decode loop): publish t only when some page the read touches
//          inside t's dirty interval has t's FBO as its authority (g766SrcPagesCurrent == false).
//          Rollback DC2_G769_NO_TEXAUTH=1.
//
//   WRITE  (CPU band replay prologue): a replay entry whose only contact with dirty resident t is
//          WRITING colour into t's pages (no depth, texture/CLUT pages current) does not publish t
//          when guest VRAM already holds every page of the entry's written rectangle that t holds.
//          The write lands in guest VRAM as the replay performs it and t's FBO is patched over the
//          same rectangle at t's next edge by the G264/G766 upload mirror — exactly the contract a
//          host->local IMAGE upload already has (note before the write, patch at the next edge).
//          Before the write VRAM == t's FBO over the rectangle (every page current), so the patch
//          is the identity on texels the replay did not write and the truth on texels it did.
//          A target the mirror refuses is published the legacy way before the replay runs.
//          Opt-in DC2_G769_CPUWRITEMIR=1 (default OFF, see g769CpuWriteMirOn).
//
//   DC2_G769_STAT=1   [G769:auth] census (also printed with DC2_G768_STAT / DC2_G767_STAT)

// Defined in g766_mask_mirror.inc (included after rasterizer_vram_materialization.inc).
static uint32_t g766WriterBitsPerTexel(uint8_t psm);
static uint32_t g766NoteUploadMaskMirror(uint32_t dbp, uint8_t dbw, uint8_t dpsm, uint32_t dsax,
                                         uint32_t dsay, uint32_t rrw, uint32_t rrh,
                                         uint32_t skipMask);

// G769 E (g767_transient_fold.inc): fold a G289 copy owner into its resident homes.
static void g769FoldOrMaterializeOwners(int cause);
static void g769FoldOrMaterializeOwnersForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                                 bool srcUnknown, bool dstUnknown, int cause);

static bool g769StatOn()
{
    static const bool on = envFlagEnabled("DC2_G769_STAT") || envFlagEnabled("DC2_G768_STAT") ||
                           envFlagEnabled("DC2_G767_STAT");
    return on;
}
static bool g769TexAuthOn()
{
    static const bool on = g766OwnerOn() && g767OwnerVramOn() && !envFlagEnabled("DC2_G769_NO_TEXAUTH");
    return on;
}
// ⛔ DEFAULT OFF (opt-in DC2_G769_CPUWRITEMIR=1). The oracle (DC2_G769_VERIFY=1) finds resident
// pages whose FBO kept pre-replay bytes under a current anchor ONLY when this rule is on (s03,
// g769e bisect: 313/313 compared pages bad with it, no skip reaches the oracle without it). The
// replay's write and the mirror's consumption are not ordered by construction here; the rule
// needs the page-store form (the replay's bytes enter the GPU authority directly) to be safe.
static bool g769CpuWriteMirOn()
{
    static const bool on = g766OwnerOn() && g767OwnerVramOn() &&
                           envFlagEnabled("DC2_G769_CPUWRITEMIR");
    return on;
}

// Set only around g528FlushPrologue's g261PrepareCpuReplay: the transient contracts call the same
// prologue before a GPU batch, whose writes never reach guest VRAM, so they keep publication.
static bool s_g769ReplayEdge = false; // flush-owner thread only
struct G769ReplayEdgeScope
{
    G769ReplayEdgeScope() { s_g769ReplayEdge = true; }
    ~G769ReplayEdgeScope() { s_g769ReplayEdge = false; }
    G769ReplayEdgeScope(const G769ReplayEdgeScope &) = delete;
    G769ReplayEdgeScope &operator=(const G769ReplayEdgeScope &) = delete;
};

static uint64_t g_g769TexAuthSkips = 0u, g_g769WrDefer = 0u, g_g769WrTaken = 0u,
    g_g769WrRefPages = 0u, g_g769WrRefMirror = 0u, g_g769WrClean = 0u, g_g769WrOverflow = 0u,
    g_g769WrMiss = 0u;

static void g769Report(bool force)
{
    if (!g769StatOn())
        return;
    static uint64_t s_n = 0u;
    if (!force && (++s_n % 2048u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G769:auth] texSkips=%llu cpuWrite(defer=%llu taken=%llu miss=%llu clean=%llu "
                 "refPages=%llu refMirror=%llu overflow=%llu)\n",
                 (unsigned long long)g_g769TexAuthSkips, (unsigned long long)g_g769WrDefer,
                 (unsigned long long)g_g769WrTaken, (unsigned long long)g_g769WrMiss,
                 (unsigned long long)g_g769WrClean,
                 (unsigned long long)g_g769WrRefPages, (unsigned long long)g_g769WrRefMirror,
                 (unsigned long long)g_g769WrOverflow);
}

// ---- ORACLE (DC2_G769_VERIFY=1, perturbing, never timed) -----------------------------------------
// Every skip claims "guest VRAM == t's FBO for every page of `rd` that t holds, over the rows the
// FBO defines". Read t's FBO back (logical centre tap) and compare it word for word with guest VRAM
// through the CT32 page inverse. t's pending upload mirror is applied first (VRAM-newer rows).
extern bool g178_backend_read_color(uint32_t fbp, int fbW, int fbH, int glY, int rows,
                                    std::vector<uint32_t> &out);
static void g766MirrorFlushOrDrop(int ti);
static bool g769VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G769_VERIFY");
    return on;
}
static uint64_t g_g769VerN = 0u, g_g769VerPages = 0u, g_g769VerBadPages = 0u, g_g769VerFail = 0u;
static void g769VerifyPagesEqual(uint32_t t, const G144BlockRange &rd, const char *what)
{
    if (!g769VerifyOn() || t >= kG248TargetCount || !rd.valid || g_g144LastVram == nullptr)
        return;
    if (g_g261Res[t].mirPending)
        g766MirrorFlushOrDrop(static_cast<int>(t));
    const G261Res &r = g_g261Res[t];
    if (!r.dirty || r.fbW <= 0 || r.fbw == 0u || r.fbw > 8u || r.covHi < r.covLo)
        return;
    ++g_g769VerN;
    std::vector<uint32_t> px;
    if (!g178_backend_read_color(g766SlotKey(t), r.fbW, 512, 0, 512, px) ||
        px.size() != static_cast<size_t>(r.fbW) * 512u)
    {
        ++g_g769VerFail;
        return;
    }
    // CT32 in-page inverse from the shipped address function (word -> x, y).
    static const struct Inv
    {
        uint16_t x[2048], y[2048];
        Inv()
        {
            for (uint32_t yy = 0u; yy < 32u; ++yy)
                for (uint32_t xx = 0u; xx < 64u; ++xx)
                {
                    const uint32_t w = (GSPSMCT32::addrPSMCT32(0u, 1u, xx, yy) >> 2u) & 2047u;
                    x[w] = static_cast<uint16_t>(xx);
                    y[w] = static_cast<uint16_t>(yy);
                }
        }
    } inv;
    for (uint32_t page = rd.first >> 5u; page <= (rd.last >> 5u) && page < kG178PageCount; ++page)
    {
        uint32_t row = 0u, col = 0u;
        if (!g766SlotPagePos(t, page, row, col))
            continue;
        const int y0 = static_cast<int>(row) * 32;
        if (y0 + 31 < r.covLo || y0 > r.covHi || y0 + 31 < r.dirtyLo || y0 > r.dirtyHi)
            continue;
        if (static_cast<uint64_t>(page + 1u) * 8192ull > g_g261VramSize)
            continue;
        // A guest-owned page (G536) is newer in VRAM by definition; the FBO copy is not claimed.
        if (g536GuestOwnsTargetPage(r, kG261Fbp[t], row, col))
            continue;
        ++g_g769VerPages;
        uint32_t bad = 0u, first = ~0u, got = 0u, want = 0u;
        for (uint32_t w = 0u; w < 2048u; ++w)
        {
            const int gy = y0 + inv.y[w];
            const int gx = static_cast<int>(col) * 64 + inv.x[w];
            if (gy < r.covLo || gy > r.covHi || gx >= r.fbW)
                continue;
            uint32_t v = 0u;
            std::memcpy(&v, g_g144LastVram + static_cast<size_t>(page) * 8192u + w * 4u, 4u);
            const uint32_t f = px[static_cast<size_t>(511 - gy) * r.fbW + gx];
            if (f != v)
            {
                if (bad++ == 0u)
                {
                    first = w;
                    got = f;
                    want = v;
                }
            }
        }
        if (bad != 0u)
        {
            ++g_g769VerBadPages;
            if (g_g769VerBadPages <= 24u)
            {
                const G766PageOwn &o = g_g766Own[page];
                const int os = o.slot;
                const bool osOk = os >= 0 && static_cast<uint32_t>(os) < kG248TargetCount;
                const G570BumpTrace &b = g_g570LastBump[page];
                std::fprintf(stderr,
                             "[G769:verify-BAD] %s t=0x%03x(s%u) page=0x%03x bad=%u word=%u fbo=%08x "
                             "vram=%08x owner=%d dirty=%d..%d cov=%d..%d | rec(slot=%d ep=%u/%u "
                             "stamp=%llu dirty=%d) seen(t)=%llu/%llu gen=%llu | bump(seq=%llu "
                             "src=%u bp=%x bw=%u psm=%x rect=%u,%u %ux%u)\n",
                             what, kG261Fbp[t], t, page, bad, first, got, want, g766ValidOwner(page),
                             r.dirtyLo, r.dirtyHi, r.covLo, r.covHi, os, o.epoch,
                             osOk ? g_g766Epoch[os] : 0u, (unsigned long long)o.stamp,
                             osOk ? (g_g261Res[os].dirty ? 1 : 0) : -1,
                             (unsigned long long)g_g766SeenStamp[t][page],
                             (unsigned long long)g_g766SeenGen[t][page],
                             (unsigned long long)g766PageGen(page), (unsigned long long)b.seq,
                             b.source, b.bpBlocks, b.bw64, b.psm, b.x, b.y, b.w, b.h);
            }
        }
    }
    if ((g_g769VerN % 256u) == 1u)
        std::fprintf(stderr, "[G769:verify] n=%llu pages=%llu badPages=%llu fail=%llu\n",
                     (unsigned long long)g_g769VerN, (unsigned long long)g_g769VerPages,
                     (unsigned long long)g_g769VerBadPages, (unsigned long long)g_g769VerFail);
}

// ---- the WRITE rule: deferred colour-write decisions of one CPU-replay prologue ------------------
// One slot per (target, frame column key). Two row unions per slot:
//   y0..y1   the entries' own clamped row extents (G708's publication rows: what the replay can
//            write and what g261NoteCpuRttWrites normally bumps) -> checked for currency, mirrored;
//   sy0..sy1 their SCISSOR rows (what the bump covers on G708's overflow fallback and on the G286
//            transient path) -> checked for ownership only: a page bumped there without being
//            mirrored becomes G536 "guest-owned" in t, which is harmless exactly when no FBO owns it
//            (guest VRAM is then the newest copy anyway).
// Only DISCOVERED-target frames (G527) that are not resident slots take the rule: their writes are
// the ones g261NoteCpuRttWrites / g286NoteCpuTransientWrites bump with these rectangles.
struct G769WriteSlot
{
    uint32_t target = 0u;
    uint32_t block = 0u, bw = 0u, psm = 0u, x0 = 0u, w = 0u;
    int y0 = 0, y1 = -1, sy0 = 0, sy1 = -1;
    bool used = false;
};
static constexpr int kG769WriteSlots = 16;
static G769WriteSlot s_g769Write[kG769WriteSlots]; // flush-owner thread only

static void g769WriteReset()
{
    for (G769WriteSlot &s : s_g769Write)
        s.used = false;
}

// The rectangle a CPU replay of `e` can write: scissor columns x its own clamped row extent, plus
// the scissor rows. False when it cannot be bounded or the frame is not a discovered target.
static bool g769EntryWriteRect(const G144Entry &e, uint32_t &block, uint32_t &bw, uint32_t &psm,
                               uint32_t &x0, uint32_t &w, int &y0, int &y1, int &sy0, int &sy1)
{
    const GSContext &c = e.ctx;
    if (c.scissor.x1 < c.scissor.x0 || c.scissor.y1 < c.scissor.y0 || e.maxY < e.minY)
        return false;
    if (!g527DeferTarget(c.frame.fbp) || g248TargetIndex(c.frame.fbp) >= 0)
        return false;
    block = framePageBaseToBlock(c.frame.fbp);
    bw = c.frame.fbw;
    psm = c.frame.psm;
    x0 = static_cast<uint32_t>(c.scissor.x0);
    w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
    y0 = std::max(e.minY, static_cast<int>(c.scissor.y0));
    y1 = std::min(e.maxY, static_cast<int>(c.scissor.y1));
    sy0 = static_cast<int>(c.scissor.y0);
    sy1 = static_cast<int>(c.scissor.y1);
    // A write past the buffer width lands in other page columns; the mirror plan would refuse it.
    if (bw == 0u || x0 + w > bw * 64u || sy1 > 2047)
        return false;
    return y1 >= y0 && g766WriterBitsPerTexel(static_cast<uint8_t>(psm)) != 0u;
}

// Record that entry `e` writes colour into dirty target t. False on overflow (the caller publishes).
static bool g769DeferWrite(uint32_t t, const G144Entry &e)
{
    uint32_t block = 0u, bw = 0u, psm = 0u, x0 = 0u, w = 0u;
    int y0 = 0, y1 = -1, sy0 = 0, sy1 = -1;
    if (!g769EntryWriteRect(e, block, bw, psm, x0, w, y0, y1, sy0, sy1))
        return false;
    for (G769WriteSlot &s : s_g769Write)
    {
        if (!s.used)
        {
            s = G769WriteSlot{t, block, bw, psm, x0, w, y0, y1, sy0, sy1, true};
            ++g_g769WrDefer;
            return true;
        }
        if (s.target == t && s.block == block && s.bw == bw && s.psm == psm && s.x0 == x0 && s.w == w)
        {
            s.y0 = std::min(s.y0, y0);
            s.y1 = std::max(s.y1, y1);
            s.sy0 = std::min(s.sy0, sy0);
            s.sy1 = std::max(s.sy1, sy1);
            ++g_g769WrDefer;
            return true;
        }
    }
    ++g_g769WrOverflow;
    return false;
}

// No page of `rg` that t's FBO represents has a valid FBO owner.
static bool g769NoOwnedPages(uint32_t t, const G144BlockRange &rg)
{
    if (!rg.valid)
        return false;
    for (uint32_t page = rg.first >> 5u; page <= (rg.last >> 5u); ++page)
    {
        if (page >= kG178PageCount)
            return false;
        uint32_t row = 0u, col = 0u;
        if (g766SlotPagePos(t, page, row, col) && g766ValidOwner(page) >= 0)
            return false;
    }
    return true;
}

// The WRITE currency test, stricter than the read test (g766SrcPagesCurrent): the mirror patch
// rewrites t's FBO over the whole rectangle from guest VRAM and then re-anchors those pages, so t's
// copy of every page the rectangle touches must ALREADY equal guest VRAM — no valid owner (so VRAM
// is newest), not guest-owned in t (t's copy is not stale), and inside the rows t's FBO defines.
static bool g769WritePagesCurrent(uint32_t t, const G144BlockRange &rg)
{
    if (t >= kG248TargetCount || !rg.valid)
        return false;
    const G261Res &r = g_g261Res[t];
    if (!r.dirty || r.fbw == 0u || r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64 ||
        r.covHi < r.covLo)
        return false;
    for (uint32_t page = rg.first >> 5u; page <= (rg.last >> 5u); ++page)
    {
        if (page >= kG178PageCount)
            return false;
        uint32_t row = 0u, col = 0u;
        if (!g766SlotPagePos(t, page, row, col))
            continue; // t's FBO does not represent this page: nothing to keep coherent
        if (g766ValidOwner(page) >= 0)
            return false;
        const int y0 = static_cast<int>(row) * 32;
        if (y0 < r.covLo || y0 + 31 > r.covHi)
            return false;
        if (g536GuestOwnsTargetPage(r, kG261Fbp[t], row, col))
            return false;
    }
    return true;
}

// Rectangles the prologue mirrored, re-noted AFTER the replay has written guest VRAM: any edge that
// flushes the target's mirror between the prologue and the write (a coherent pull into it while
// another deferred target is published, the depth barrier) consumes the patch with PRE-write bytes
// and re-anchors the pages, and the replay's bump would then leave the FBO stale under a current
// anchor (`[G769:verify-BAD]` s03, page 0x19c of 0x13a: last bump = the replay, FBO = pre-write).
// The re-note is idempotent: still pending -> nothing changes; already flushed -> patched again,
// this time from the written bytes.
struct G769WriteNote
{
    uint32_t target, block, bw, psm, x0, w, y0, rows;
};
static std::vector<G769WriteNote> s_g769PostNotes; // flush-owner thread only
static uint64_t g_g769PostNotes = 0u, g_g769PostLost = 0u;
static void g769PostReplayWriteNotes()
{
    for (const G769WriteNote &n : s_g769PostNotes)
    {
        if (n.target >= kG248TargetCount || !g_g261Res[n.target].dirty)
            continue;
        const uint32_t taken = g766NoteUploadMaskMirror(
            n.block, static_cast<uint8_t>(n.bw), static_cast<uint8_t>(n.psm), n.x0, n.y0, n.w,
            n.rows, ~(1u << n.target));
        if ((taken & (1u << n.target)) != 0u)
            ++g_g769PostNotes;
        else
        {
            ++g_g769PostLost;
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr, "[G769:postnote-LOST] t=0x%03x rect=%u,%u %ux%u\n",
                             kG261Fbp[n.target], n.x0, n.y0, n.w, n.rows);
        }
    }
    s_g769PostNotes.clear();
}

// The post-replay bump of this flush's discovered-target colour writes is exactly G708's row union
// per column key (the rows the entries write) unless G708 overflows its 12 slots (then an entry
// bumps its whole scissor) or a frame is a G286 transient (whole-scissor bump). Same key and
// predicate as g261NoteCpuRttWrites / g286NoteCpuTransientWrites.
static bool g769BumpIsRowUnion()
{
    static const bool s_noRowPub = envFlagEnabled("DC2_G708_NO_ROWPUB");
    if (s_noRowPub)
        return false;
    uint32_t keys[12][5];
    int n = 0;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        if (g286TransientTargetOn() && g286TransientTarget(c.frame.fbp))
            return false;
        if (!g527DeferTarget(c.frame.fbp) || c.scissor.x1 < c.scissor.x0 ||
            c.scissor.y1 < c.scissor.y0)
            continue;
        if (e.maxY < e.minY)
            return false; // G708 falls back to the scissor bump for an empty extent
        const uint32_t k[5] = {framePageBaseToBlock(c.frame.fbp), c.frame.fbw, c.frame.psm,
                               static_cast<uint32_t>(c.scissor.x0),
                               static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1)};
        bool found = false;
        for (int i = 0; i < n && !found; ++i)
            found = std::memcmp(keys[i], k, sizeof(k)) == 0;
        if (found)
            continue;
        if (n >= 12)
            return false;
        std::memcpy(keys[n++], k, sizeof(k));
    }
    return true;
}

static void g261Materialize(int ti, int cause);
// Resolve every deferred write: mirror it (target stays resident) or publish the target now, before
// the replay writes guest VRAM.
static void g769ResolveWrites()
{
    bool anyUsed = false;
    for (const G769WriteSlot &s : s_g769Write)
        anyUsed = anyUsed || s.used;
    const bool rowUnionBump = anyUsed && g769BumpIsRowUnion();
    for (G769WriteSlot &s : s_g769Write)
    {
        if (!s.used)
            continue;
        s.used = false;
        const uint32_t t = s.target;
        if (t >= kG248TargetCount || !g_g261Res[t].dirty)
        {
            ++g_g769WrClean; // another entry of this flush published t already
            continue;
        }
        const uint32_t rows = static_cast<uint32_t>(s.y1 - s.y0 + 1);
        const G144BlockRange rg = g144RangeForRect(s.block, s.psm, s.bw, s.x0,
                                                   static_cast<uint32_t>(s.y0), s.w, rows);
        const G144BlockRange bump =
            rowUnionBump ? rg
                         : g144RangeForRect(s.block, s.psm, s.bw, s.x0,
                                            static_cast<uint32_t>(s.sy0), s.w,
                                            static_cast<uint32_t>(s.sy1 - s.sy0 + 1));
        if (bump.valid && g_g261Res[t].range.valid && !g144RangeOverlaps(g_g261Res[t].range, bump))
        {
            ++g_g769WrMiss; // nothing the replay writes or bumps reaches t's window: no edge at all
            continue;
        }
        bool ok = rg.valid && g769NoOwnedPages(t, bump) && g769WritePagesCurrent(t, rg);
        if (!ok)
            ++g_g769WrRefPages;
        else
        {
            g769VerifyPagesEqual(t, rg, "write");
            const uint32_t taken = g766NoteUploadMaskMirror(
                s.block, static_cast<uint8_t>(s.bw), static_cast<uint8_t>(s.psm), s.x0,
                static_cast<uint32_t>(s.y0), s.w, rows, ~(1u << t));
            ok = (taken & (1u << t)) != 0u;
            if (!ok)
                ++g_g769WrRefMirror;
        }
        if (ok)
        {
            ++g_g769WrTaken;
            s_g769PostNotes.push_back(G769WriteNote{t, s.block, s.bw, s.psm, s.x0, s.w,
                                                    static_cast<uint32_t>(s.y0), rows});
            continue;
        }
        if (g_g261Res[t].dirty)
        {
            G766MatSite g766s(kG766SiteCpuReplay);
            g261Materialize(static_cast<int>(t), kG261MatCpuReplay);
        }
    }
    g769Report(false);
}
