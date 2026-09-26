// ===== G768: GENERAL GPU PAGE VIEWS — the paletted path rebuilt on page AUTHORITY, not overlap =====
//
// G767 proved the page-view idea for CT32/CT24 and shipped its paletted form DEFAULT OFF. Two things
// were wrong with that form, and neither was the idea:
//   * it REFUSED a read whose CLUT lived in an FBO ("CLUT pages must be current in guest VRAM"), and
//     that is exactly dungeon1's last texBind publication: the display HUD sprite reads a PSMT8
//     texture (tbp 0x1a00, current in VRAM) through a CLUT that 0x13c RENDERS (`[G737:texrej]`
//     2,822 / 60 s, 320 rows each — the route's whole remaining tex publication population);
//   * its regression (escaped writer 0x139) was never the view's: it was the G289 owner
//     publication the 0x13c publication used to hide (g768_owner_mirror.inc).
//
// The G768 view (g768_view_packet.inc for the packet, g768_page_view_members.inc for the worker):
//   INDEX PAGES   each page of the texture is resolved by AUTHORITY: its valid G766 owner FBO, a dirty
//                 resident whose copy is current (G767 unowned rule), or guest VRAM — read on the
//                 GPU from a persistent MIRROR of guest VRAM that uploads a page only when its write
//                 generation moved (a static index texture uploads once, not once per build);
//   PAGE LUT      one per storage format: texel -> {CT32 word x/y, bit} of its page, composed from
//                 PixelStorageTraits<psm>::Read's own arithmetic (GSMem::G768BitAddress) and the CT32
//                 inverse; self-tested against GSMem::Read* before first use;
//   CLUT          per entry, exactly g178LookupClutFixed's address (resolveClutIndex + TEXCLUT
//                 window): entries whose page is current in guest VRAM are CPU-decoded constants
//                 (the oracle's own value), entries whose page an FBO owns are gathered on the GPU
//                 and converted there (CT32 / CT24+TEXA / CT16 / CT16S + TEXA);
//   CACHE         per view: the per-page holder signatures + the CLUT signature. Unchanged -> reuse
//                 (no work). Only some pages changed and the CLUT did not -> PARTIAL rebuild of just
//                 those pages into the persistent view. Otherwise a full rebuild;
//   ASYNC         every build is a FIFO job on the GL worker; no front-end wait, no readback.
//
//   DC2_G768_NO_VIEW=1      rollback: the G767 paletted path (itself default OFF) and legacy decode
//   DC2_G768_NO_CLUTGPU=1   rollback: a CLUT held by an FBO refuses the view (G767's rule)
//   DC2_G768_NO_PARTIAL=1   rollback: every changed view is rebuilt whole
//   DC2_G768_STAT=1         [G768:view] census
extern bool g768_backend_queue_view(std::shared_ptr<G768ViewPacket> pk);
extern uint64_t g768_backend_mirror_epoch();
extern uint64_t g768_backend_exec_serial();
extern void g768_backend_report();

static bool g768StatOn()
{
    static const bool on = envFlagEnabled("DC2_G768_STAT") || envFlagEnabled("DC2_G767_STAT");
    return on;
}
static bool g768ViewOn()
{
    static const bool on = g766OwnerOn() && !envFlagEnabled("DC2_G768_NO_VIEW") &&
                           !envFlagDisabled("DC2_G768_VIEW");
    return on;
}
static bool g768ClutGpuOn()
{
    static const bool on = !envFlagEnabled("DC2_G768_NO_CLUTGPU");
    return on;
}
static bool g768PartialOn()
{
    static const bool on = !envFlagEnabled("DC2_G768_NO_PARTIAL");
    return on;
}

// ---- the storage formats -------------------------------------------------------------------------
struct G768Fmt
{
    uint8_t psm;
    int fmt;
    int lutId;
    int pageW, pageH;
    bool halfBw; // the decode passes max(1, tbw >> 1) as the page stride (T8 / T4)
};
static const G768Fmt *g768FmtFor(uint8_t psm)
{
    // PSMT8H is not in the GPU classifier's texture list (g178DecodeWhole would decode it as T4),
    // and PSMCT16/16S read through readTexelPSMCT16's LIVE-psm dispatch; both stay on the legacy path.
    static const G768Fmt k[] = {
        {GS_PSM_T8, kG768FmtIdx8, 2, 128, 64, true},
        {GS_PSM_T4, kG768FmtIdx4, 4, 128, 128, true},
        {GS_PSM_T4HL, kG768FmtIdx4, 5, 64, 32, false},
        {GS_PSM_T4HH, kG768FmtIdx4, 6, 64, 32, false},
        // Direct colour (reached only through g768TryDirectView, after the FBO-direct binds).
        {GS_PSM_CT32, kG768FmtCt32, 0, 64, 32, false},
        {GS_PSM_CT24, kG768FmtCt24, 0, 64, 32, false},
    };
    for (const G768Fmt &f : k)
        if (f.psm == psm)
            return &f;
    return nullptr;
}

// The CT32 inverse from the SAME address function the LUT uses (word index -> x, y in the page).
struct G768Ct32Inv
{
    int16_t x[2048];
    int16_t y[2048];
    bool ok = true;
    G768Ct32Inv()
    {
        for (int i = 0; i < 2048; ++i)
            x[i] = y[i] = -1;
        for (uint32_t yy = 0u; yy < 32u; ++yy)
            for (uint32_t xx = 0u; xx < 64u; ++xx)
            {
                const uint32_t bits = GSMem::G768BitAddress(GS_PSM_CT32, 0u, 1u, xx, yy);
                const uint32_t word = bits >> 5u;
                if ((bits & 31u) != 0u || word >= 2048u || x[word] >= 0)
                {
                    ok = false;
                    continue;
                }
                x[word] = static_cast<int16_t>(xx);
                y[word] = static_cast<int16_t>(yy);
            }
        for (int i = 0; i < 2048; ++i)
            if (x[i] < 0)
                ok = false;
    }
};
static const G768Ct32Inv &g768Inv()
{
    static const G768Ct32Inv s_inv;
    return s_inv;
}

// Bit position of a texel inside its page -> {word x, word y, bit} of the page's CT32 layout.
static inline bool g768MapBits(uint32_t pageBits, uint32_t &wx, uint32_t &wy, uint32_t &bit)
{
    const uint32_t word = pageBits >> 5u;
    if (word >= 2048u)
        return false;
    const G768Ct32Inv &inv = g768Inv();
    wx = static_cast<uint32_t>(inv.x[word]);
    wy = static_cast<uint32_t>(inv.y[word]);
    bit = pageBits & 31u;
    return true;
}

// Self-test of one format's page LUT against GSMem's own readers on a synthetic 4 MB image.
static bool g768SelfTestLut(const G768Fmt &f, const std::vector<uint32_t> &lut)
{
    static std::vector<uint8_t> s_mem;
    if (s_mem.empty())
    {
        s_mem.assign(4u * 1024u * 1024u, 0u);
        uint32_t h = 0x9E3779B9u;
        for (size_t i = 0u; i < 16384u; ++i) // pages 0 and 1: enough for one page of any format
        {
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            s_mem[i] = static_cast<uint8_t>(h);
        }
    }
    uint8_t *mem = s_mem.data();
    for (int y = 0; y < f.pageH; ++y)
        for (int x = 0; x < f.pageW; ++x)
        {
            const uint32_t l = lut[static_cast<size_t>(y) * f.pageW + x];
            const uint32_t wx = l & 0xFFu, wy = (l >> 8) & 0xFFu, bit = (l >> 16) & 0xFFu;
            const uint32_t word = GSMem::ReadCT32(mem, 0u, 1u, wx, wy);
            uint32_t got = 0u, want = 0u;
            switch (f.psm)
            {
            case GS_PSM_T8:
                got = (word >> bit) & 0xFFu;
                want = GSMem::ReadP8(mem, 0u, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                break;
            case GS_PSM_T4:
                got = (word >> bit) & 0xFu;
                want = GSMem::ReadP4(mem, 0u, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                break;
            case GS_PSM_T4HL:
                got = (word >> bit) & 0xFu;
                want = GSMem::ReadP4HL(mem, 0u, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                break;
            case GS_PSM_T4HH:
                got = (word >> bit) & 0xFu;
                want = GSMem::ReadP4HH(mem, 0u, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                break;
            case GS_PSM_CT32:
            case GS_PSM_CT24:
                got = (bit == 0u) ? word : ~0u;
                want = GSMem::ReadCT32(mem, 0u, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                break;
            default:
                return false;
            }
            if (got != want)
            {
                std::fprintf(stderr, "[G768:selftest] FAIL psm=0x%x x=%d y=%d got=%u want=%u\n", f.psm,
                             x, y, got, want);
                return false;
            }
        }
    return true;
}

// The page LUT of one format (built, self-tested, and kept for the process). Empty = unusable.
static const std::vector<uint32_t> &g768Lut(const G768Fmt &f)
{
    static std::vector<uint32_t> s_luts[8];
    static bool s_done[8] = {};
    std::vector<uint32_t> &lut = s_luts[f.lutId];
    if (s_done[f.lutId])
        return lut;
    s_done[f.lutId] = true;
    if (!g768Inv().ok)
        return lut;
    std::vector<uint32_t> tmp(static_cast<size_t>(f.pageW) * f.pageH);
    for (int y = 0; y < f.pageH; ++y)
        for (int x = 0; x < f.pageW; ++x)
        {
            const uint32_t bits = GSMem::G768BitAddress(f.psm, 0u, 1u, static_cast<uint32_t>(x),
                                                        static_cast<uint32_t>(y));
            uint32_t wx = 0u, wy = 0u, bit = 0u;
            if (bits >= 65536u || !g768MapBits(bits, wx, wy, bit))
                return lut;
            tmp[static_cast<size_t>(y) * f.pageW + x] = wx | (wy << 8) | (bit << 16) | 0xFF000000u;
        }
    if (!g768SelfTestLut(f, tmp))
        return lut;
    std::fprintf(stderr, "[G768:selftest] PASS psm=0x%x page=%dx%d\n", f.psm, f.pageW, f.pageH);
    lut = std::move(tmp);
    return lut;
}

// ---- the GPU mirror of guest VRAM (front-end bookkeeping) -----------------------------------------
static uint64_t g_g768MirGen[512];
static bool g_g768MirValid[512];
static uint64_t g_g768MirEpoch = 0u;
static uint64_t g_g768MirUploads = 0u;
static void g768MirSync()
{
    const uint64_t e = g768_backend_mirror_epoch();
    if (e != g_g768MirEpoch)
    {
        g_g768MirEpoch = e;
        std::fill(std::begin(g_g768MirValid), std::end(g_g768MirValid), false);
    }
}
// Ensure page `page` is current in the mirror as of this packet (appending its upload if needed).
static bool g768MirEnsure(uint32_t page, G768ViewPacket &pk)
{
    if (page >= 512u || g_g144LastVram == nullptr ||
        static_cast<uint64_t>(page + 1u) * 8192ull > g_g261VramSize)
        return false;
    const uint64_t gen = g766PageGen(page);
    if (g_g768MirValid[page] && g_g768MirGen[page] == gen)
        return true;
    for (const uint32_t p : pk.mirPages)
        if (p == page)
            return true;
    const G768Ct32Inv &inv = g768Inv();
    const size_t base = pk.mirPixels.size();
    pk.mirPixels.resize(base + 2048u);
    const uint8_t *src = g_g144LastVram + static_cast<size_t>(page) * 8192u;
    for (uint32_t w = 0u; w < 2048u; ++w)
    {
        uint32_t v = 0u;
        std::memcpy(&v, src + w * 4u, 4u);
        pk.mirPixels[base + static_cast<size_t>(inv.x[w]) + 64u * static_cast<size_t>(inv.y[w])] = v;
    }
    pk.mirPages.push_back(page);
    g_g768MirValid[page] = true;
    g_g768MirGen[page] = gen;
    ++g_g768MirUploads;
    return true;
}
static void g768MirForget(const G768ViewPacket &pk)
{
    for (const uint32_t p : pk.mirPages)
        if (p < 512u)
            g_g768MirValid[p] = false;
}

// ---- page authority --------------------------------------------------------------------------------
struct G768Holder
{
    int kind = kG768SrcMirror;
    int key = 0;
    int ox = 0, oy = 0;  // GS logical origin of the page in the source
    uint64_t sig0 = 0u, sig1 = 0u;
    int ownerSlot = -1;  // a valid G766 owner (its lazy mirror must be flushed)
    int slot = -1;       // the resident slot whose FBO supplies the page (FBO kind)
};
// Where page `page`'s current bytes are. `allowVramCopy`: an unowned page current in a dirty
// resident may be read from that FBO (free) instead of the mirror (an upload when stale).
static bool g768ResolvePage(uint32_t page, bool allowResidentCopy, G768Holder &h)
{
    if (page >= kG178PageCount)
        return false;
    uint32_t row = 0u, col = 0u;
    // ⭐ G769: a PARTIAL guest write pending in a resident's lazy mirror makes the page read as
    // guest-owned (no valid owner) while that FBO still holds its other bytes. Apply such mirrors
    // first so the owner below is the truth (see g766SrcPagesCurrent).
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty && g_g261Res[t].mirPending && g766SlotPagePos(t, page, row, col))
            g766MirrorFlushOrDrop(static_cast<int>(t));
    const uint64_t gen = g766PageGen(page);
    const int o = g766ValidOwner(page);
    if (o >= 0)
    {
        if (!g766SlotPagePos(static_cast<uint32_t>(o), page, row, col))
            return false;
        h.kind = kG768SrcFbo;
        h.key = static_cast<int>(g766SlotKey(static_cast<uint32_t>(o)));
        h.ox = static_cast<int>(col * 64u);
        h.oy = static_cast<int>(row * 32u);
        h.sig0 = (1ull << 62) | (static_cast<uint64_t>(o) << 56) |
                 (g_g766Own[page].stamp & 0x00FFFFFFFFFFFFFFull);
        h.sig1 = gen;
        h.ownerSlot = o;
        h.slot = o;
        return true;
    }
    if (allowResidentCopy)
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            if (!g_g261Res[t].dirty || !g766SlotPagePos(t, page, row, col))
                continue;
            if (!g767UnownedPageCurrent(t, page))
                continue;
            h.kind = kG768SrcFbo;
            h.key = static_cast<int>(g766SlotKey(t));
            h.ox = static_cast<int>(col * 64u);
            h.oy = static_cast<int>(row * 32u);
            h.sig0 = (2ull << 62) | (static_cast<uint64_t>(t) << 56);
            h.sig1 = gen;
            h.ownerSlot = -1;
            h.slot = static_cast<int>(t);
            return true;
        }
    // Guest VRAM: nothing else may owe it bytes (pending display / deferred depth / G289 owner /
    // G272 display wave), and GPU-resident guest depth over it is synced first.
    const G144BlockRange pr{page * 32u, page * 32u + 31u, true};
    if (g767OtherAuthorityHits(pr))
        return false;
    if (!g242SyncOverlappingDepth(g_g144LastVram, page * 32u, 1u, GS_PSM_CT32, 0u, 0u, 64u, 32u))
        return false;
    h.kind = kG768SrcMirror;
    h.key = 0;
    h.ox = static_cast<int>((page & 15u) * 64u);
    h.oy = static_cast<int>((page >> 4u) * 32u);
    h.sig0 = 3ull << 62;
    h.sig1 = g766PageGen(page); // re-read: the depth sync may have written the page
    h.ownerSlot = -1;
    return true;
}
// A page inside some dirty resident's claimed window: the legacy path may publish for it.
static bool g768InDirtyWindow(uint32_t page)
{
    const G144BlockRange pr{page * 32u, page * 32u + 31u, true};
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (r.dirty && (!r.range.valid || g144RangeOverlaps(r.range, pr)))
            return true;
    }
    return false;
}

// ---- census ----------------------------------------------------------------------------------------
static uint64_t g_g768Try = 0u, g_g768Full = 0u, g_g768Partial = 0u, g_g768Reuse = 0u,
    g_g768NoNeed = 0u, g_g768RefShape = 0u, g_g768RefPage = 0u, g_g768RefClut = 0u,
    g_g768RefQueue = 0u, g_g768ClutGpu = 0u, g_g768ClutConst = 0u, g_g768Pages = 0u,
    g_g768MemoHits = 0u;
static void g768Report(bool force)
{
    if (!g768StatOn())
        return;
    static uint64_t s_n = 0u;
    if (!force && (++s_n % 1024u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G768:view] try=%llu full=%llu partial=%llu reuse=%llu memo=%llu noNeed=%llu "
                 "pages=%llu clut(gpu=%llu const=%llu) mirUploads=%llu ref(shape=%llu page=%llu "
                 "clut=%llu queue=%llu)\n",
                 (unsigned long long)g_g768Try, (unsigned long long)g_g768Full,
                 (unsigned long long)g_g768Partial, (unsigned long long)g_g768Reuse,
                 (unsigned long long)g_g768MemoHits, (unsigned long long)g_g768NoNeed,
                 (unsigned long long)g_g768Pages, (unsigned long long)g_g768ClutGpu,
                 (unsigned long long)g_g768ClutConst, (unsigned long long)g_g768MirUploads,
                 (unsigned long long)g_g768RefShape, (unsigned long long)g_g768RefPage,
                 (unsigned long long)g_g768RefClut, (unsigned long long)g_g768RefQueue);
    if ((s_n % 8192u) == 1u)
        g768_backend_report();
}

// ---- the end-to-end oracle (DC2_G768_VERIFY=1, perturbing, never timed) ---------------------------
// Rebuild the texture the way the LEGACY path would see it after publication: guest VRAM, with every
// FBO-held page this view used replaced by that FBO's own (read-back, centre-tap resolved) words,
// then decode it with GSMem's readers + g178LookupClutFixed and compare every texel of the view.
extern bool g768_backend_read_view(uint64_t key, int w, int h, std::vector<uint32_t> &out);
static bool g768VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G768_VERIFY");
    return on;
}
static uint64_t g_g768VerN = 0u, g_g768VerTexels = 0u, g_g768VerBad = 0u, g_g768VerFail = 0u,
    g_g768VerFboPages = 0u;
struct G768Fmt;
static void g768Verify(const G144Entry &e, const G768Fmt &f, uint64_t key, int texW, int texH,
                       uint32_t perRow, const std::vector<std::pair<uint32_t, G768Holder>> &used);
// ---- the view cache ---------------------------------------------------------------------------------
struct G768ViewState
{
    std::vector<uint64_t> pageSig; // 2 per page
    uint64_t clutSig = 0u;
    uint64_t serial = 0u;          // the packet that last built / patched it
    bool valid = false;
};
static std::unordered_map<uint64_t, G768ViewState> g_g768Views; // flush-owner thread only
static uint64_t g_g768Serial = 0u;

static inline uint64_t g768Mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0x100000001B3ull;
}
static inline uint32_t g768ClutIndex(uint32_t index, uint8_t csm, uint8_t csa, uint8_t psm)
{
    // Exactly GSRasterizer::resolveClutIndex (rasterizer_row_pool.inc).
    const auto swz = [](uint32_t i) { return (i & 0xE7u) | ((i & 0x08u) << 1u) | ((i & 0x10u) >> 1u); };
    uint32_t ci = index;
    if (psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
    {
        ci = (static_cast<uint32_t>(csa) << 4u) | (ci & 0x0Fu);
        if (csm == 0u)
            ci = swz(ci);
    }
    else if ((psm == GS_PSM_T8 || psm == GS_PSM_T8H) && csm == 0u)
        ci = swz(ci);
    return ci;
}

// CT32 / CT24 through this system (per-page partial rebuild, GPU VRAM mirror) instead of G767's
// whole-view gather. Rollback DC2_G768_NO_CT32=1 (G767's page view serves them again).
static bool g768Ct32On()
{
    static const bool on = !envFlagEnabled("DC2_G768_NO_CT32");
    return on;
}
// Returns the view key (built / queued / reused for this flush) or 0 (the caller's legacy path).
// `direct`: the CT32 / CT24 call site (after the FBO-direct binds); otherwise the paletted site.
static uint64_t g768TryViewImpl(const G144Entry &e, int texW, int texH,
                                std::unordered_set<uint64_t> &builtThisFlush, bool direct)
{
    if (!g768ViewOn() || !e.prim.tme || g_g144LastVram == nullptr)
        return 0u;
    const GSTex0Reg &tex = e.ctx.tex0;
    const G768Fmt *fp = g768FmtFor(tex.psm);
    if (fp == nullptr)
        return 0u;
    const G768Fmt &f = *fp;
    const bool indexed = f.fmt == kG768FmtIdx8 || f.fmt == kG768FmtIdx4;
    if (indexed == direct || (direct && !g768Ct32On()))
        return 0u;
    const bool ct24 = f.fmt == kG768FmtCt24;
    // Per-flush memo of every register tuple: guest VRAM cannot change inside one flush.
    static std::unordered_map<uint64_t, uint64_t> s_memo; // tuple -> key (0 = refused)
    static uint64_t s_memoSerial = ~0ull;
    if (s_memoSerial != g_g767FlushSerial)
    {
        s_memo.clear();
        s_memoSerial = g_g767FlushSerial;
    }
    // Only the registers the view is a function of: no CLUT / TEXA for CT32, TA0 + AEM for CT24.
    const uint64_t regs[16] = {tex.psm, tex.tbp0, tex.tbw, static_cast<uint64_t>(texW),
                               static_cast<uint64_t>(texH), indexed ? tex.cbp : 0u,
                               indexed ? tex.cpsm : 0u, indexed ? tex.csm : 0u,
                               indexed ? tex.csa : 0u, indexed ? e.texclut.cbw : 0u,
                               indexed ? e.texclut.cou : 0u, indexed ? e.texclut.cov : 0u,
                               (indexed || ct24) ? e.texa.ta0 : 0u, indexed ? e.texa.ta1 : 0u,
                               static_cast<uint64_t>(((indexed || ct24) && e.texa.aem) ? 1u : 0u),
                               static_cast<uint64_t>(e.ctx.clamp & 0xFu)};
    // Level 0: consecutive entries overwhelmingly repeat the previous entry's texture registers.
    static uint64_t s_lastRegs[16];
    static uint64_t s_lastSerial = ~0ull, s_lastResult = 0u;
    if (s_lastSerial == g_g767FlushSerial && std::memcmp(s_lastRegs, regs, sizeof(regs)) == 0)
    {
        ++g_g768MemoHits;
        if (s_lastResult != 0u)
            builtThisFlush.insert(s_lastResult);
        return s_lastResult;
    }
    uint64_t rkey = 0xF768ull;
    for (uint64_t r : regs)
        rkey = g768Mix(rkey, r);
    struct LastScope
    {
        const uint64_t *regs;
        uint64_t &result;
        ~LastScope()
        {
            std::memcpy(s_lastRegs, regs, sizeof(s_lastRegs));
            s_lastSerial = g_g767FlushSerial;
            s_lastResult = result;
        }
    };
    uint64_t result = 0u;
    LastScope lastScope{regs, result};
    {
        const auto it = s_memo.find(rkey);
        if (it != s_memo.end())
        {
            ++g_g768MemoHits;
            if (it->second != 0u)
                builtThisFlush.insert(it->second);
            return result = it->second;
        }
    }
    const auto refuse = [&](uint64_t &ctr) -> uint64_t {
        ++ctr;
        s_memo[rkey] = 0u;
        return 0u;
    };
    ++g_g768Try;
    // Cheap admission first: the view is only worth building when some page it reads (index or
    // CLUT) has a valid FBO OWNER — every other read is exact and publication-free on the legacy
    // path (guest VRAM current: G291 / G766 / G767 page rules), and its decode is cached there.
    // ⭐ G769: a page whose owner is hidden by a PARTIAL guest write pending in a resident's lazy
    // mirror (G536 "guest-owned" until the mirror lands) counts as owned: after the mirror the FBO
    // holds bytes guest VRAM does not, and g768ResolvePage applies the mirror before resolving.
    // Without this the read fell to the legacy loop, which (correctly, since G769 D) publishes.
    const auto g769HiddenOwner = [](uint32_t page) -> bool {
        uint32_t row = 0u, col = 0u;
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            const G261Res &r = g_g261Res[t];
            if (!r.dirty || !r.mirPending || r.mirHi < r.mirLo || !g766SlotPagePos(t, page, row, col))
                continue;
            const int y0 = static_cast<int>(row) * 32;
            if (y0 + 31 >= r.mirLo && y0 <= r.mirHi && y0 + 31 >= r.dirtyLo && y0 <= r.dirtyHi)
                return true;
        }
        return false;
    };
    {
        bool anyOwner = false;
        const uint32_t base0 = tex.tbp0 >> 5u;
        const uint32_t perRow0 = f.halfBw ? std::max<uint32_t>(1u, static_cast<uint32_t>(tex.tbw) >> 1u)
                                          : static_cast<uint32_t>(tex.tbw);
        const int px0 = (texW + f.pageW - 1) / f.pageW, py0 = (texH + f.pageH - 1) / f.pageH;
        for (int py = 0; py < py0 && !anyOwner; ++py)
            for (int px = 0; px < px0 && !anyOwner; ++px)
            {
                const uint32_t pg0 = base0 + static_cast<uint32_t>(py) * perRow0 +
                                     static_cast<uint32_t>(px);
                anyOwner = g766ValidOwner(pg0) >= 0 || g769HiddenOwner(pg0);
            }
        if (!anyOwner && indexed)
        {
            const uint8_t sp = (tex.cpsm == GS_PSM_CT24) ? GS_PSM_CT32 : tex.cpsm;
            const uint32_t cw = e.texclut.cbw != 0u ? static_cast<uint32_t>(e.texclut.cbw) : 1u;
            const uint32_t n = (f.fmt == kG768FmtIdx8) ? 256u : 16u;
            uint32_t last = ~0u;
            for (uint32_t i = 0u; i < n && !anyOwner; ++i)
            {
                const uint32_t ci = g768ClutIndex(i, tex.csm, tex.csa, tex.psm);
                const uint32_t pg = (GSMem::G768BitAddress(sp, tex.cbp, cw,
                                                          static_cast<uint32_t>(e.texclut.cou) + (ci & 15u),
                                                          static_cast<uint32_t>(e.texclut.cov) + (ci >> 4u)) &
                                     0x1FFFFFFu) >> 16u;
                if (pg != last)
                {
                    last = pg;
                    anyOwner = g766ValidOwner(pg) >= 0 || g769HiddenOwner(pg);
                }
            }
        }
        if (!anyOwner)
            return result = refuse(g_g768NoNeed);
    }
    const uint32_t perRow = f.halfBw ? std::max<uint32_t>(1u, static_cast<uint32_t>(tex.tbw) >> 1u)
                                     : static_cast<uint32_t>(tex.tbw);
    if ((tex.tbp0 & 31u) != 0u || perRow == 0u || texW <= 0 || texH <= 0 || texW > 1024 ||
        texH > 1024 || (e.ctx.clamp & 3u) >= 2u || ((e.ctx.clamp >> 2) & 3u) >= 2u ||
        (indexed && tex.cpsm != GS_PSM_CT32 && tex.cpsm != GS_PSM_CT24 &&
         tex.cpsm != GS_PSM_CT16 && tex.cpsm != GS_PSM_CT16S))
        return result = refuse(g_g768RefShape);
    const std::vector<uint32_t> &lut = g768Lut(f);
    if (lut.empty())
        return result = refuse(g_g768RefShape);
    g768MirSync();

    auto pk = std::make_shared<G768ViewPacket>();
    pk->texW = texW;
    pk->texH = texH;
    pk->fmt = f.fmt;
    pk->lutId = f.lutId;
    pk->lutW = f.pageW;
    pk->lutH = f.pageH;
    pk->lut = &lut;
    pk->ta0 = e.texa.ta0;
    pk->ta1 = e.texa.ta1;
    pk->aem = e.texa.aem ? 1 : 0;

    // (1) index pages
    const uint32_t base = tex.tbp0 >> 5u;
    const int pagesX = (texW + f.pageW - 1) / f.pageW, pagesY = (texH + f.pageH - 1) / f.pageH;
    static std::vector<uint64_t> s_sig;
    s_sig.clear();
    static std::vector<int32_t> s_rects;
    s_rects.clear();
    uint32_t owners = 0u;
    bool anyFbo = false;
    static std::vector<std::pair<uint32_t, G768Holder>> s_used; // oracle: every page and its holder
    s_used.clear();
    for (int py = 0; py < pagesY; ++py)
        for (int px = 0; px < pagesX; ++px)
        {
            const uint32_t page = base + static_cast<uint32_t>(py) * perRow + static_cast<uint32_t>(px);
            G768Holder h;
            if (page >= 512u || !g768ResolvePage(page, true, h))
            {
                pk->mirPages.clear(); // nothing was recorded as mirrored yet for a refused plan
                return result = refuse(g_g768RefPage);
            }
            s_used.emplace_back(page, h);
            if (h.kind == kG768SrcFbo)
                anyFbo = true;
            if (h.ownerSlot >= 0)
                owners |= 1u << static_cast<uint32_t>(h.ownerSlot);
            s_sig.push_back(h.sig0 | static_cast<uint64_t>(h.kind));
            s_sig.push_back(h.sig1);
            const int w = std::min(f.pageW, texW - px * f.pageW);
            const int hh = std::min(f.pageH, texH - py * f.pageH);
            s_rects.insert(s_rects.end(), {h.kind, h.key, h.ox, h.oy, px * f.pageW, py * f.pageH, w, hh});
        }

    // (2) the CLUT, entry by entry, by the authority of each entry's page
    const uint32_t clutN = !indexed ? 0u : ((f.fmt == kG768FmtIdx8) ? 256u : 16u);
    pk->clutN = static_cast<int>(clutN);
    pk->clutConst.assign(clutN, 0u);
    pk->clutEnt.assign(static_cast<size_t>(clutN) * 4u, -1);
    const uint8_t storePsm = (tex.cpsm == GS_PSM_CT24) ? GS_PSM_CT32 : tex.cpsm;
    const uint32_t clutW = e.texclut.cbw != 0u ? static_cast<uint32_t>(e.texclut.cbw) : 1u;
    uint64_t clutSig = g768Mix(0xC1u, tex.cpsm);
    clutSig = g768Mix(clutSig, (uint64_t{e.texa.ta0} << 16) | (uint64_t{e.texa.ta1} << 8) |
                                   (e.texa.aem ? 1u : 0u));
    int nSrc = 0;
    uint32_t lastPage = ~0u;
    G768Holder lastH;
    bool lastOk = false;
    for (uint32_t i = 0u; i < clutN; ++i)
    {
        const uint32_t ci = g768ClutIndex(i, tex.csm, tex.csa, tex.psm);
        const uint32_t cx = static_cast<uint32_t>(e.texclut.cou) + (ci & 0x0Fu);
        const uint32_t cy = static_cast<uint32_t>(e.texclut.cov) + (ci >> 4u);
        const uint32_t bits = GSMem::G768BitAddress(storePsm, tex.cbp, clutW, cx, cy) & 0x1FFFFFFu;
        const uint32_t page = bits >> 16u;
        if (page != lastPage)
        {
            lastPage = page;
            // The CLUT's own pages never take the resident-copy shortcut: guest VRAM (the constant
            // path) is the exact oracle whenever no FBO owns the page.
            lastOk = g768ResolvePage(page, false, lastH);
            if (lastOk)
                s_used.emplace_back(page, lastH);
            if (lastOk && lastH.kind == kG768SrcFbo)
                anyFbo = true;
            if (lastOk && lastH.ownerSlot >= 0)
                owners |= 1u << static_cast<uint32_t>(lastH.ownerSlot);
        }
        if (!lastOk)
            return result = refuse(g_g768RefClut);
        if (lastH.kind != kG768SrcFbo)
        {
            // Guest VRAM holds this entry: the oracle's own value.
            const uint32_t v = g178LookupClutFixed(g_g144LastVram, e.texa, e.texclut,
                                                   static_cast<uint8_t>(i), tex.cbp, tex.cpsm,
                                                   tex.csm, tex.csa, tex.psm);
            pk->clutConst[i] = v;
            clutSig = g768Mix(clutSig, v);
            continue;
        }
        if (!g768ClutGpuOn())
            return result = refuse(g_g768RefClut);
        int src = -1;
        for (int k = 0; k < nSrc; ++k)
            if (pk->clutSrcKind[k] == lastH.kind && pk->clutSrcKey[k] == lastH.key)
                src = k;
        if (src < 0)
        {
            if (nSrc >= 4)
                return result = refuse(g_g768RefClut);
            src = nSrc++;
            pk->clutSrcKind[src] = lastH.kind;
            pk->clutSrcKey[src] = lastH.key;
        }
        uint32_t wx = 0u, wy = 0u, bit = 0u;
        if (!g768MapBits(bits & 0xFFFFu, wx, wy, bit))
            return result = refuse(g_g768RefClut);
        int mode = 0;
        if (tex.cpsm == GS_PSM_CT32)
            mode = 0;
        else if (tex.cpsm == GS_PSM_CT24)
            mode = 1;
        else
            mode = (bit == 0u) ? 2 : 3;
        if ((mode <= 1 && bit != 0u) || (mode >= 2 && bit != 0u && bit != 16u))
            return result = refuse(g_g768RefClut);
        pk->clutEnt[i * 4u + 0u] = src;
        pk->clutEnt[i * 4u + 1u] = lastH.ox + static_cast<int>(wx);
        pk->clutEnt[i * 4u + 2u] = lastH.oy + static_cast<int>(wy);
        pk->clutEnt[i * 4u + 3u] = mode;
        pk->clutGpu = true;
        clutSig = g768Mix(clutSig, (static_cast<uint64_t>(i) << 48) ^ lastH.sig0);
        clutSig = g768Mix(clutSig, lastH.sig1);
    }
    if (!anyFbo)
        return result = refuse(g_g768NoNeed); // no FBO page after all: legacy decode is exact
    if (pk->clutGpu)
        ++g_g768ClutGpu;
    else
        ++g_g768ClutConst;

    // (3) the view key: every register the view is a function of
    const uint64_t key = 0xF76A000000000000ull | (g768Mix(rkey, 0x768u) & 0x0000FFFFFFFFFFFFull);
    if (builtThisFlush.count(key) != 0u)
    {
        s_memo[rkey] = key;
        return result = key;
    }
    pk->texKey = key;
    pk->clutSig = clutSig | 1u;

    // (4) reuse / partial / full
    if (g_g768Views.size() > 4096u)
        g_g768Views.clear(); // bounded; a forgotten view only rebuilds whole
    G768ViewState &vs = g_g768Views[key];
    const bool live = vs.valid && vs.pageSig.size() == s_sig.size() &&
                      (vs.serial > g768_backend_exec_serial() || g178_backend_has_tex(key));
    const bool clutSame = vs.clutSig == pk->clutSig;
    if (live && clutSame && vs.pageSig == s_sig)
    {
        ++g_g768Reuse;
        builtThisFlush.insert(key);
        s_memo[rkey] = key;
        g768Report(false);
        return result = key;
    }
    bool full = true;
    if (live && clutSame && g768PartialOn())
    {
        std::vector<int32_t> part;
        for (size_t p = 0u; p * 2u + 1u < s_sig.size(); ++p)
            if (vs.pageSig[p * 2u] != s_sig[p * 2u] || vs.pageSig[p * 2u + 1u] != s_sig[p * 2u + 1u])
                part.insert(part.end(), s_rects.begin() + static_cast<std::ptrdiff_t>(p * 8u),
                            s_rects.begin() + static_cast<std::ptrdiff_t>(p * 8u + 8u));
        pk->rects = std::move(part);
        full = false;
    }
    else
        pk->rects = s_rects;
    pk->full = full;
    // Mirror uploads for every mirror-sourced page this packet draws.
    for (size_t k = 0u; k + 8u <= pk->rects.size(); k += 8u)
        if (pk->rects[k] == kG768SrcMirror)
        {
            const uint32_t page = static_cast<uint32_t>((pk->rects[k + 3] / 32) * 16 + pk->rects[k + 2] / 64);
            if (!g768MirEnsure(page, *pk))
            {
                g768MirForget(*pk);
                vs.valid = false;
                return result = refuse(g_g768RefPage);
            }
        }
    if (owners != 0u)
        g766FlushOwnerMirrors(owners);
    pk->serial = ++g_g768Serial;
    const size_t nRects = pk->rects.size() / 8u;
    if (!g768_backend_queue_view(pk))
    {
        g768MirForget(*pk);
        vs.valid = false;
        return result = refuse(g_g768RefQueue);
    }
    vs.pageSig = s_sig;
    vs.clutSig = pk->clutSig;
    vs.serial = pk->serial;
    vs.valid = true;
    if (full)
        ++g_g768Full;
    else
        ++g_g768Partial;
    g_g768Pages += nRects;
    builtThisFlush.insert(key);
    s_memo[rkey] = key;
    if (g768VerifyOn())
        g768Verify(e, f, key, texW, texH, perRow, s_used);
    g768Report(false);
    return result = key;
}

static uint64_t g768TryView(const G144Entry &e, int texW, int texH,
                            std::unordered_set<uint64_t> &builtThisFlush)
{
    return g768TryViewImpl(e, texW, texH, builtThisFlush, false);
}
static uint64_t g768TryDirectView(const G144Entry &e, int texW, int texH,
                                  std::unordered_set<uint64_t> &builtThisFlush)
{
    return g768TryViewImpl(e, texW, texH, builtThisFlush, true);
}

static void g768Verify(const G144Entry &e, const G768Fmt &f, uint64_t key, int texW, int texH,
                       uint32_t perRow, const std::vector<std::pair<uint32_t, G768Holder>> &used)
{
    ++g_g768VerN;
    static std::vector<uint8_t> s_mem;
    const size_t vsz = std::min<size_t>(g_g261VramSize, 4u * 1024u * 1024u);
    s_mem.assign(4u * 1024u * 1024u, 0u);
    std::memcpy(s_mem.data(), g_g144LastVram, vsz);
    std::unordered_map<int, std::vector<uint32_t>> fbo;
    const G768Ct32Inv &inv = g768Inv();
    for (const auto &pu : used)
    {
        const uint32_t page = pu.first;
        const G768Holder &h = pu.second;
        if (h.kind != kG768SrcFbo || h.slot < 0)
            continue;
        const G261Res &r = g_g261Res[h.slot];
        auto it = fbo.find(h.key);
        if (it == fbo.end())
        {
            std::vector<uint32_t> px;
            if (r.fbW <= 0 || !g178_backend_read_color(static_cast<uint32_t>(h.key), r.fbW, 512, 0, 512, px) ||
                px.size() != static_cast<size_t>(r.fbW) * 512u)
            {
                ++g_g768VerFail;
                return;
            }
            it = fbo.emplace(h.key, std::move(px)).first;
        }
        for (uint32_t w = 0u; w < 2048u; ++w)
        {
            const int gx = h.ox + inv.x[w];
            const int gy = h.oy + inv.y[w];
            if (gx < 0 || gx >= r.fbW || gy < 0 || gy >= 512)
            {
                ++g_g768VerFail;
                return;
            }
            const uint32_t v = it->second[static_cast<size_t>(511 - gy) * r.fbW + gx];
            std::memcpy(&s_mem[static_cast<size_t>(page) * 8192u + w * 4u], &v, 4u);
        }
        ++g_g768VerFboPages;
    }
    std::vector<uint32_t> view;
    if (!g768_backend_read_view(key, texW, texH, view))
    {
        ++g_g768VerFail;
        return;
    }
    const GSTex0Reg &tex = e.ctx.tex0;
    uint8_t *mem = s_mem.data();
    uint64_t bad = 0u;
    for (int v = 0; v < texH; ++v)
        for (int u = 0; u < texW; ++u)
        {
            uint32_t idx = 0u;
            switch (f.psm)
            {
            case GS_PSM_T8: idx = GSMem::ReadP8(mem, tex.tbp0, perRow, u, v); break;
            case GS_PSM_T4: idx = GSMem::ReadP4(mem, tex.tbp0, perRow, u, v); break;
            case GS_PSM_T4HL: idx = GSMem::ReadP4HL(mem, tex.tbp0, perRow, u, v); break;
            case GS_PSM_T4HH: idx = GSMem::ReadP4HH(mem, tex.tbp0, perRow, u, v); break;
            default: break;
            }
            uint32_t want = 0u;
            if (f.fmt == kG768FmtCt32)
                want = GSMem::ReadCT32(mem, tex.tbp0, perRow, u, v);
            else if (f.fmt == kG768FmtCt24)
            {
                const uint32_t rgb = GSMem::ReadCT32(mem, tex.tbp0, perRow, u, v) & 0x00FFFFFFu;
                want = rgb | ((e.texa.aem && rgb == 0u) ? 0u : (uint32_t{e.texa.ta0} << 24));
            }
            else
                want = g178LookupClutFixed(mem, e.texa, e.texclut, static_cast<uint8_t>(idx),
                                           tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            const uint32_t got = view[static_cast<size_t>(v) * texW + u];
            if (got != want)
            {
                ++bad;
                if (g_g768VerBad + bad <= 12u)
                    std::fprintf(stderr,
                                 "[G768:verify-BAD] psm=0x%x tbp=0x%x tbw=%u cbp=0x%x cpsm=0x%x csm=%u "
                                 "csa=%u u=%d v=%d idx=%u got=%08x want=%08x\n",
                                 tex.psm, tex.tbp0, static_cast<unsigned>(tex.tbw), tex.cbp, tex.cpsm,
                                 static_cast<unsigned>(tex.csm), static_cast<unsigned>(tex.csa), u, v,
                                 idx, got, want);
            }
        }
    g_g768VerTexels += static_cast<uint64_t>(texW) * texH;
    g_g768VerBad += bad;
    if ((g_g768VerN % 64u) == 1u || bad != 0u)
        std::fprintf(stderr, "[G768:verify] n=%llu texels=%llu bad=%llu fail=%llu fboPages=%llu\n",
                     (unsigned long long)g_g768VerN, (unsigned long long)g_g768VerTexels,
                     (unsigned long long)g_g768VerBad, (unsigned long long)g_g768VerFail,
                     (unsigned long long)g_g768VerFboPages);
}
