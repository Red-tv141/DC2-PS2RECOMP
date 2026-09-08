// ===== G700: ADDRESS-GENERIC PERSISTENT SURFACE AUTHORITY =====
//
// THE DEFECT THIS REPLACES.  GPU residency in this port is an ALLOWLIST OF FOURTEEN ADDRESSES.
// `g248TargetIndex` is a `switch` over {0x139, 0x13c, 0x143, 0x146, 0x155, 0x159, 0x181, 0x13a,
// 0x13d, 0x157, 0x14b, 0x151, 0x13f, 0x141}; every array that carries residency state
// (`g_g261Res`, `g_g280ContentGen`, `g_g310TargetPageGen`, `g_g261MatByTarget`) is sized
// `kG248TargetCount`.  A render target at any other address gets one of two non-persistent
// contracts:
//
//   * the TRANSIENT contract (G286/G570/G586/G587/G604) — upload the target from guest VRAM into
//     the FBO, render, read the FBO back, swizzle it into guest VRAM.  Every flush.  `[G523:fit]`
//     books that prologue at `pro0trn` 0.0323 ms/flush x 33.4 flushes = **1.08 ms/f**, and it is
//     the reason a *target switch* costs 0.184 ms here and ~0.02 ms in PCSX2.
//   * the CPU BAND REPLAY — `g178TryFlushGpu` returns `kG266FrCpuOnlyRtt` and the batch is
//     rasterized scalar on the GS worker.  This is where the whole `guestDepth` class of
//     discovered targets still lives (G660/G663: the split is `guestDepth` vs `!guestDepth`,
//     because a CPU replay is the only path that keeps guest Z authoritative).
//
// The precedent for the size of the prize is G569, which did nothing but ADD SIX ADDRESSES to
// that switch: route `s05` went 39.74 -> 31.01 ms/f, **-8.73 ms/f = -22.0%**, non-overlapping
// per-run means.  The list is not a design; it is the subset somebody had time to measure.
//
// THE MODEL.  A surface (colour target, and its depth) is a PERSISTENT FIRST-CLASS OBJECT keyed
// by (fbp, fbw, psm).  Guest VRAM is a materialised VIEW of it, produced only when a real CPU
// consumer demands those bytes.  The authority question — "for these rows, is the FBO or guest
// VRAM the newer copy?" — is answered by an explicit PER-PAGE OWNERSHIP TOKEN, not by
// reconstructing and comparing generation SUMS over a whole surface on every flush.
//
//   g_g700PageOwner[page]   0 = guest VRAM owns this page; s+1 = surface slot s owns it.
//   surf.pageAnchor[p]      the value of g_g178PageGen at the instant the surface claimed page p.
//
// A page is GPU-authoritative iff the surface owns it AND `g_g178PageGen[p] == pageAnchor[p]`,
// i.e. no guest write has landed on it since the claim.  Both are O(1) per page over the pages a
// batch actually covers (104 for a 512x416 CT32 target at fbw=8) and involve no allocation, no
// map lookup, no snapshot record and no 64 KB per-target pixel bitmap.  Compare with the shipped
// path, which per flush computes `g178GenSumRect` over the WHOLE target, compares it against a
// `snap.genSum` recorded under different row coverage, and then repairs the mismatch with
// `g536CollectGuestOwnedPages` / `g536RestoreGuestOwnedPages` page saves.
//
// THE INVARIANTS (all fail-closed — every uncertainty resolves to "upload from VRAM" or
// "materialise to VRAM", never to "trust the FBO"):
//
//   I1  A surface may only claim pages it just RENDERED.  Claiming is `g700NoteRender`.
//   I2  A claimed page stays claimed until either (a) a guest write bumps its generation, or
//       (b) it is materialised, or (c) the surface is evicted.  All three release ownership.
//   I3  `g700NeedUpload` is true if ANY page of the requested window is not owned by this
//       surface with an intact anchor.  A partially-owned window uploads in full: the upload is
//       exact for the disowned pages and re-establishes the owned ones from VRAM bytes this
//       surface itself published, so it is never wrong, only occasionally redundant.
//   I4  Materialisation happens at the SAME transfer edges the proven G261 residency uses
//       (`g700MaterializeForRanges` is called beside `g261MaterializeForRanges`), plus at the
//       frame/presentation boundary.  The page-generation anchor is the BACKSTOP for any write
//       path that reaches VRAM without passing one of those edges: such a write does not corrupt
//       anything, it merely disowns the page and forces the next batch to re-upload it.
//   I5  A surface whose fbw or psm changes is RESET (ownership dropped, full upload) — the FBO's
//       physical geometry changed, so no page mapping survives.
//
// DEPTH.  `depthLive` records that this surface has a persistent GPU depth attachment whose
// contents were produced by GPU rendering and have never been read back.  That is the mechanism
// the `guestDepth` discovered-target class lacks today: it is forced onto the CPU band replay
// because guest Z must stay authoritative, and guest Z can only stay authoritative if somebody
// reads the depth buffer back.  A surface that OWNS its depth pages does not need to: the same
// ownership token answers "who has the newer Z" for the depth range exactly as it does for
// colour, and the depth attachment simply stays on the GPU across flushes.
//
// LEVERS
//   DC2_G700=1              arm the architecture (bring-up default: OFF)
//   DC2_G700_NO_RESIDENT=1  rollback once promoted default-ON
//   DC2_G700_NO_DEPTH=1     colour residency only; depth-carrying targets keep their old path
//   DC2_G700_STAT=1         [G700:auth] periodic census (never in a timing arm)
//   DC2_G700_VERIFY=1       oracle: materialise on EVERY edge and compare against the claim

static constexpr uint32_t kG700Slots = 48u;      // live persistent surfaces
static constexpr uint32_t kG700VramPages = 512u; // 4 MiB / 8 KiB GS page
static constexpr uint32_t kG700MaxPages = 256u;  // 16 page-rows x fbw<=16 page-cols
static constexpr int kG700FbH = 512;             // every GS colour target is walked 0..511
// G582 write-generation source tag for this file's own FBO->VRAM publication. Sources 0..25 are
// taken (see the `G582 source N` comments); the guest-write backstop below recognises this one so
// a publication cannot disown the pages it just brought into sync.
static constexpr uint32_t kG700BumpSource = 26u;

// ---------------------------------------------------------------------------------------------
// Arming
// ---------------------------------------------------------------------------------------------

static bool g700On()
{
    static const bool on = (envFlagEnabled("DC2_G700") ||
                            !envFlagDisabled("DC2_G700_DEFAULT_OFF")) &&
                           !envFlagEnabled("DC2_G700_NO_RESIDENT") &&
                           !envFlagDisabled("DC2_G700");
    // Bring-up: opt-in only. The `DC2_G700_DEFAULT_OFF` spelling exists so the promotion edit is
    // one line here and the rollback flag is already registered in every harness clear-list.
    static const bool armed = envFlagEnabled("DC2_G700") &&
                              !envFlagEnabled("DC2_G700_NO_RESIDENT");
    (void)on;
    return armed;
}

// ⭐ THE TRANSIENT TAKEOVER. `g286` (0x13b), `g570` (0x13d), `g586` (0x141) and `g587` (0x15d) are
// four hand-written copies of the same non-persistent contract: publish every resident sibling
// into guest VRAM, upload the target from VRAM into the FBO, render, read the FBO back, swizzle
// it into VRAM — every flush. `[G523:fit]` books just the publication half of that (`pro0trn`) at
// 0.0323 ms per flush x 33.4 flushes = 1.08 ms/f, and the upload and readback halves are on top.
// Nothing about those four addresses needs a transient contract; they were transient because no
// persistent one existed for them. Armed separately from the discovered-target half because each
// carries its own measured pixel gate (G680's fish icons, G659's Ridepod floor shadow).
static bool g700TransientTakeoverOn()
{
    static const bool on = envFlagEnabled("DC2_G700_TRANSIENT") &&
                           !envFlagEnabled("DC2_G700_NO_TRANSIENT");
    return on;
}

// The depth half is a separate architectural claim from the colour half and is armed on its own,
// so a measurement can attribute to one or the other. Promotion flips the default here.
static bool g700DepthOn()
{
    static const bool on = envFlagEnabled("DC2_G700_DEPTH") &&
                           !envFlagEnabled("DC2_G700_NO_DEPTH");
    return on;
}

static bool g700StatOn()
{
    static const bool on = envFlagEnabled("DC2_G700_STAT");
    return on;
}

static bool g700VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G700_VERIFY");
    return on;
}

// ---------------------------------------------------------------------------------------------
// The surface object
// ---------------------------------------------------------------------------------------------

struct G700Surface
{
    bool live = false;
    uint32_t fbp = 0;   // FRAME.FBP, page units — also the backend FBO key
    uint32_t fbw = 0;   // FRAME.FBW, 64-pixel units
    uint32_t psm = 0;   // FRAME.PSM
    int fbW = 0;        // fbw * 64
    uint32_t basePage = 0;   // fbp (a FRAME base is a page base by construction)
    uint32_t widthPages = 0; // pages per page-row for this psm/fbw
    uint32_t pageHShift = 5; // log2(page height in rows) — 5 for CT32/CT24, 6 for CT16
    uint32_t pageCount = 0;  // widthPages * (512 >> pageHShift)

    // Rows the FBO holds content for at all (upload | render union).
    int covLo = kG700FbH, covHi = -1;
    // Rows the FBO holds that guest VRAM does NOT (the materialisation debt).
    int dirtyLo = kG700FbH, dirtyHi = -1;

    // Per-page authority. `owned[p]` is this surface's claim; `anchor[p]` is the global page
    // generation at the instant of the claim. Ownership survives only while they agree.
    std::array<uint8_t, kG700MaxPages> owned{};
    std::array<uint64_t, kG700MaxPages> anchor{};

    // Persistent GPU depth. `depthKey` is the backend's depth texture key (g242DepthKey), so a
    // surface and its depth stay attached across every flush that renders to it.
    uint64_t depthKey = 0u;
    uint32_t zbp = 0u, zpsm = 0u;
    bool depthLive = false; // GPU depth content exists and has never been read back
    int depthLo = kG700FbH, depthHi = -1;

    uint64_t lastUse = 0u;
    uint64_t renders = 0u;
};

static G700Surface g_g700Surf[kG700Slots];
// page -> owning slot + 1 (0 = guest VRAM owns). The reverse index that lets a guest write find
// its affected surfaces in O(pages touched) instead of O(slots).
static uint8_t g_g700PageOwner[kG700VramPages]{};
// FRAME base page -> slot + 1. The complete key space (a FRAME base is 9 bits), so the per-entry
// "is this texture a live surface" question is one indexed byte load rather than a slot scan.
static uint8_t g_g700BaseToSlot[kG700VramPages]{};
static uint64_t g_g700Clock = 0u;

static std::atomic<uint64_t> g_g700Acquires{0}, g_g700Evictions{0}, g_g700Renders{0},
    g_g700Uploads{0}, g_g700UploadSkips{0}, g_g700Mats{0}, g_g700MatRows{0},
    g_g700Disowns{0}, g_g700DepthKeeps{0}, g_g700Resets{0}, g_g700DirectBinds{0};
// Publications bucketed by the G261MatCause the caller passed: 0 cpuReplay, 1 inline, 2 upload,
// 3 local, 4 l2h, 5 texAlias/boundary, 6 resync/evict, 7 fail.
static std::atomic<uint64_t> g_g700MatByCause[8];
// Guest writes that disowned a page, bucketed by the G582 write-source tag of the writer. Without
// this, `disowns=9392` names a number and no culprit.
static std::atomic<uint64_t> g_g700DisownBySrc[32];
// Direct-bind rejects, by conjunct: 0 unaligned base, 1 base names no surface, 2 self/empty,
// 3 width, 4 format, 5 sampled rows outside the covered span. `directBinds=0` is a number with no
// culprit without this.
static std::atomic<uint64_t> g_g700BindRej[6];

// ---------------------------------------------------------------------------------------------
// Page geometry
// ---------------------------------------------------------------------------------------------

// Page height shift for a colour PSM. Matches g178PageShifts' `phSh`; kept local so this file is
// self-contained and can be reasoned about without the bump walker.
static inline uint32_t g700PageHShift(uint32_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return 6u; // 64 x 64
    default:
        return 5u; // 64 x 32 (CT32/CT24/Z32/Z24)
    }
}

static inline uint32_t g700PageWShift(uint32_t psm)
{
    switch (psm)
    {
    case GS_PSM_T8:
        return 7u;
    case GS_PSM_T4:
        return 7u;
    default:
        return 6u;
    }
}

// Index of the page holding GS row `y`, column page `px`, within this surface.
static inline uint32_t g700PageIndex(const G700Surface &s, uint32_t px, int y)
{
    return (static_cast<uint32_t>(y) >> s.pageHShift) * s.widthPages + px;
}

// ---------------------------------------------------------------------------------------------
// Slot lookup / lifetime
// ---------------------------------------------------------------------------------------------

// Forward declarations: eviction must publish before it drops a claim, the first acquire installs
// the guest-write backstop, and every claim/publication moves the debt bound.
static bool g700Materialize(int slot, int rowLo, int rowHi, int cause);
static void g700EnsureHook();
static void g700RecomputeDebtBounds();

static void g700ReleaseAll(G700Surface &s, int slot)
{
    for (uint32_t p = 0; p < s.pageCount && p < kG700MaxPages; ++p)
    {
        if (!s.owned[p])
            continue;
        s.owned[p] = 0u;
        const uint32_t vp = (s.basePage + p) & (kG700VramPages - 1u);
        if (g_g700PageOwner[vp] == static_cast<uint8_t>(slot + 1))
            g_g700PageOwner[vp] = 0u;
    }
    s.dirtyLo = kG700FbH;
    s.dirtyHi = -1;
    g700RecomputeDebtBounds();
}

static int g700Find(uint32_t fbp)
{
    if (!g700On() || fbp >= kG700VramPages)
        return -1;
    const uint8_t slot1 = g_g700BaseToSlot[fbp];
    if (slot1 == 0u || !g_g700Surf[slot1 - 1u].live)
        return -1;
    return static_cast<int>(slot1) - 1;
}

static void g700Reset(int slot, uint32_t fbw, uint32_t psm)
{
    G700Surface &s = g_g700Surf[slot];
    g700ReleaseAll(s, slot);
    s.fbw = fbw;
    s.psm = psm;
    s.fbW = static_cast<int>(fbw) * 64;
    s.pageHShift = g700PageHShift(psm);
    const uint32_t pwSh = g700PageWShift(psm);
    s.widthPages = std::max(1u, (std::max(1u, fbw) << 6u) >> pwSh);
    s.pageCount = std::min(kG700MaxPages,
                           s.widthPages * (static_cast<uint32_t>(kG700FbH) >> s.pageHShift));
    s.covLo = kG700FbH;
    s.covHi = -1;
    s.depthLive = false;
    s.depthLo = kG700FbH;
    s.depthHi = -1;
    g_g700Resets.fetch_add(1u, std::memory_order_relaxed);
}

// Acquire (or create) the persistent surface for this target. Returns -1 when the geometry is
// outside the model's domain, in which case the caller keeps its existing path.
static int g700Acquire(uint32_t fbp, uint32_t fbw, uint32_t psm)
{
    if (!g700On() || fbw == 0u || fbw > 16u || fbp >= kG700VramPages)
        return -1;
    ++g_g700Clock;
    int slot = g700Find(fbp);
    if (slot >= 0)
    {
        G700Surface &s = g_g700Surf[slot];
        // I5: a geometry change invalidates every page mapping this surface holds.
        if (s.fbw != fbw || s.psm != psm)
        {
            (void)g700Materialize(slot, s.dirtyLo, s.dirtyHi, 6 /* kG261MatResync */);
            g700Reset(slot, fbw, psm);
        }
        s.lastUse = g_g700Clock;
        return slot;
    }
    // Free slot, else evict the least recently used one (publishing its debt first).
    int free = -1, lru = -1;
    uint64_t lruUse = ~0ull;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        if (!g_g700Surf[i].live)
        {
            free = static_cast<int>(i);
            break;
        }
        if (g_g700Surf[i].lastUse < lruUse)
        {
            lruUse = g_g700Surf[i].lastUse;
            lru = static_cast<int>(i);
        }
    }
    if (free < 0)
    {
        if (lru < 0)
            return -1;
        G700Surface &v = g_g700Surf[lru];
        (void)g700Materialize(lru, v.dirtyLo, v.dirtyHi, 6);
        g700ReleaseAll(v, lru);
        if (v.fbp < kG700VramPages &&
            g_g700BaseToSlot[v.fbp] == static_cast<uint8_t>(lru + 1))
            g_g700BaseToSlot[v.fbp] = 0u;
        v.live = false;
        g_g700Evictions.fetch_add(1u, std::memory_order_relaxed);
        free = lru;
    }
    g700EnsureHook();
    G700Surface &s = g_g700Surf[free];
    s = G700Surface{};
    s.live = true;
    s.fbp = fbp;
    s.basePage = fbp;
    g_g700BaseToSlot[fbp] = static_cast<uint8_t>(free + 1);
    g700Reset(free, fbw, psm);
    s.lastUse = g_g700Clock;
    g_g700Acquires.fetch_add(1u, std::memory_order_relaxed);
    if (g700StatOn())
        std::fprintf(stderr, "[G700:auth] acquire slot=%d fbp=0x%x fbw=%u psm=%u pages=%u\n",
                     free, fbp, fbw, psm, s.pageCount);
    return free;
}

// ---------------------------------------------------------------------------------------------
// Authority queries
// ---------------------------------------------------------------------------------------------

// Is page `p` of this surface still GPU-authoritative? Ownership plus an intact generation
// anchor. A disowned page is recorded here (rather than at the write) because the write path
// cannot publish — see I4.
static inline bool g700PageIsGpu(G700Surface &s, int slot, uint32_t p)
{
    if (p >= s.pageCount || !s.owned[p])
        return false;
    const uint32_t vp = (s.basePage + p) & (kG700VramPages - 1u);
    if (g_g178PageGen[vp].load(std::memory_order_relaxed) != s.anchor[p])
    {
        // A guest write landed on a claimed page. Guest VRAM is now the newer copy for it; drop
        // the claim so the next batch uploads it back. The GPU content for that page is
        // deliberately discarded — the guest just wrote over it.
        s.owned[p] = 0u;
        if (g_g700PageOwner[vp] == static_cast<uint8_t>(slot + 1))
            g_g700PageOwner[vp] = 0u;
        g_g700Disowns.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// I3: does the FBO need refreshing from guest VRAM for rows [rowLo, rowHi]?
static bool g700NeedUpload(int slot, int rowLo, int rowHi)
{
    if (slot < 0)
        return true;
    G700Surface &s = g_g700Surf[slot];
    if (rowHi < rowLo)
        return false;
    if (rowLo < s.covLo || rowHi > s.covHi)
        return true; // rows the FBO has never held
    const uint32_t pLo = static_cast<uint32_t>(std::max(0, rowLo)) >> s.pageHShift;
    const uint32_t pHi = static_cast<uint32_t>(std::min(kG700FbH - 1, rowHi)) >> s.pageHShift;
    for (uint32_t py = pLo; py <= pHi; ++py)
        for (uint32_t px = 0; px < s.widthPages; ++px)
        {
            const uint32_t p = py * s.widthPages + px;
            if (!g700PageIsGpu(s, slot, p))
                return true;
        }
    return false;
}

// I1: the GPU just rendered rows [rowLo, rowHi] of this surface. Claim their pages and anchor
// them at the current global generation, so any later guest write is detectable.
static void g700NoteRender(int slot, int rowLo, int rowHi)
{
    if (slot < 0 || rowHi < rowLo)
        return;
    G700Surface &s = g_g700Surf[slot];
    const uint32_t pLo = static_cast<uint32_t>(std::max(0, rowLo)) >> s.pageHShift;
    const uint32_t pHi = static_cast<uint32_t>(std::min(kG700FbH - 1, rowHi)) >> s.pageHShift;
    for (uint32_t py = pLo; py <= pHi; ++py)
        for (uint32_t px = 0; px < s.widthPages; ++px)
        {
            const uint32_t p = py * s.widthPages + px;
            if (p >= s.pageCount)
                continue;
            const uint32_t vp = (s.basePage + p) & (kG700VramPages - 1u);
            s.owned[p] = 1u;
            s.anchor[p] = g_g178PageGen[vp].load(std::memory_order_relaxed);
            g_g700PageOwner[vp] = static_cast<uint8_t>(slot + 1);
        }
    s.covLo = std::min(s.covLo, rowLo);
    s.covHi = std::max(s.covHi, rowHi);
    s.dirtyLo = std::min(s.dirtyLo, rowLo);
    s.dirtyHi = std::max(s.dirtyHi, rowHi);
    s.lastUse = ++g_g700Clock;
    ++s.renders;
    g_g700Renders.fetch_add(1u, std::memory_order_relaxed);
    g700RecomputeDebtBounds();
}

// The FBO was just refreshed from guest VRAM over rows [rowLo, rowHi]: for those rows the two
// copies agree, so the surface may claim them without owing a publication.
static void g700NoteUpload(int slot, int rowLo, int rowHi)
{
    if (slot < 0 || rowHi < rowLo)
        return;
    G700Surface &s = g_g700Surf[slot];
    const int dLo = s.dirtyLo, dHi = s.dirtyHi;
    g700NoteRender(slot, rowLo, rowHi);
    // An upload is not a debt: restore the pre-call dirty span, widened only where the surface
    // already owed a publication.
    s.dirtyLo = dLo;
    s.dirtyHi = dHi;
    g_g700Uploads.fetch_add(1u, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Materialisation — the only edge at which GPU authority is surrendered to guest VRAM
// ---------------------------------------------------------------------------------------------

static bool g700Materialize(int slot, int rowLo, int rowHi, int cause)
{
    if (slot < 0)
        return true;
    G700Surface &s = g_g700Surf[slot];
    if (rowHi < rowLo || s.fbW <= 0 || g_g144LastVram == nullptr || g_g261VramSize == 0u)
        return true;
    rowLo = std::max(rowLo, 0);
    rowHi = std::min(rowHi, kG700FbH - 1);
    if (rowHi < rowLo)
        return true;
    const int rows = rowHi - rowLo + 1;
    static std::vector<uint32_t> s_px; // flush-owner thread only, exactly as g261Materialize
    const int glY = kG700FbH - 1 - rowHi;
    if (!g178_backend_read_color(s.fbp, s.fbW, kG700FbH, glY, rows, s_px) ||
        s_px.size() != static_cast<size_t>(s.fbW) * static_cast<size_t>(rows))
    {
        // Fail closed: the surface keeps its debt and its claim, so a later edge retries. Nothing
        // in guest VRAM was touched.
        return false;
    }
    const uint32_t fbBlock = framePageBaseToBlock(s.fbp);
    g418UnpackColorRows(g_g144LastVram, g_g261VramSize, s_px, fbBlock, s.fbw, s.fbW,
                        rowHi + 1, rowLo, rowHi, /*legacy=*/false, /*lanesOverride=*/0);
    // Guest VRAM now holds these rows, so publish the write generations for every downstream
    // consumer (texture caches, CLUT reads, the display path) exactly as g261Materialize does,
    // then re-anchor: the pages are in sync, and the surface still owns them.
    g178BumpRectImpl(fbBlock, s.fbw, s.psm, 0u, static_cast<uint32_t>(rowLo),
                     static_cast<uint32_t>(s.fbW), static_cast<uint32_t>(rows),
                     kG700BumpSource | (static_cast<uint32_t>(cause & 0xff) << 8u));
    const uint32_t pLo = static_cast<uint32_t>(rowLo) >> s.pageHShift;
    const uint32_t pHi = static_cast<uint32_t>(rowHi) >> s.pageHShift;
    for (uint32_t py = pLo; py <= pHi; ++py)
        for (uint32_t px = 0; px < s.widthPages; ++px)
        {
            const uint32_t p = py * s.widthPages + px;
            if (p >= s.pageCount)
                continue;
            const uint32_t vp = (s.basePage + p) & (kG700VramPages - 1u);
            s.anchor[p] = g_g178PageGen[vp].load(std::memory_order_relaxed);
        }
    // The debt is settled for the published span. Anything outside it survives.
    if (rowLo <= s.dirtyLo && rowHi >= s.dirtyHi)
    {
        s.dirtyLo = kG700FbH;
        s.dirtyHi = -1;
    }
    else if (rowLo <= s.dirtyLo)
        s.dirtyLo = std::max(s.dirtyLo, rowHi + 1);
    else if (rowHi >= s.dirtyHi)
        s.dirtyHi = std::min(s.dirtyHi, rowLo - 1);
    g_g700Mats.fetch_add(1u, std::memory_order_relaxed);
    g_g700MatRows.fetch_add(static_cast<uint64_t>(rows), std::memory_order_relaxed);
    // ⛔ WITHOUT THIS THE CENSUS CANNOT BE ACTED ON. The first armed run read `renders=1174
    // mats=1174` — one publication per render, i.e. the residency was defeated on every single
    // batch — and a single `mats` total cannot say WHICH consumer edge demanded it. Bucket by the
    // cause the caller passed (the G261MatCause space, shared so the two models are comparable).
    g_g700MatByCause[cause & 7].fetch_add(1u, std::memory_order_relaxed);
    g700RecomputeDebtBounds();
    (void)cause;
    return true;
}

static bool g700AnyDirty()
{
    if (!g700On())
        return false;
    for (uint32_t i = 0; i < kG700Slots; ++i)
        if (g_g700Surf[i].live && g_g700Surf[i].dirtyHi >= g_g700Surf[i].dirtyLo)
            return true;
    return false;
}

// The block range guest VRAM would see for this surface's outstanding debt.
static G144BlockRange g700DirtyRange(const G700Surface &s)
{
    if (s.dirtyHi < s.dirtyLo || s.fbW <= 0)
        return G144BlockRange{};
    return g144RangeForRect(framePageBaseToBlock(s.fbp), static_cast<uint8_t>(s.psm), s.fbw, 0u,
                            static_cast<uint32_t>(s.dirtyLo), static_cast<uint32_t>(s.fbW),
                            static_cast<uint32_t>(s.dirtyHi - s.dirtyLo + 1));
}

// I4: a VRAM transfer is about to read `src` and/or write `dst`. Publish every surface whose
// outstanding debt those bytes intersect, BEFORE the caller mutates or reads them. Unknown
// ranges fail closed (publish everything). Mirrors g261MaterializeForRanges' contract exactly,
// which is why it is called from the same sites.
static void g700MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (!g700On() || !g700AnyDirty())
        return;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        G700Surface &s = g_g700Surf[i];
        if (!s.live || s.dirtyHi < s.dirtyLo)
            continue;
        if (srcUnknown || dstUnknown)
        {
            (void)g700Materialize(static_cast<int>(i), s.dirtyLo, s.dirtyHi, cause);
            continue;
        }
        const G144BlockRange r = g700DirtyRange(s);
        if (!r.valid || (src.valid && g144RangeOverlaps(r, src)) ||
            (dst.valid && g144RangeOverlaps(r, dst)))
            (void)g700Materialize(static_cast<int>(i), s.dirtyLo, s.dirtyHi, cause);
    }
}

// The union of every surface's outstanding debt, maintained as one block range so the common
// case — a batch whose textures and CLUTs touch nothing any surface owes — costs two compares
// instead of a walk over 48 slots x N entries. Recomputed whenever a claim or a publication moves
// it; conservative (a superset), so a false positive costs an unnecessary range walk and a false
// negative is impossible.
static uint32_t g_g700DebtLo = 0xFFFFFFFFu, g_g700DebtHi = 0u;

static void g700RecomputeDebtBounds()
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0u;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        const G700Surface &s = g_g700Surf[i];
        if (!s.live || s.dirtyHi < s.dirtyLo)
            continue;
        const G144BlockRange r = g700DirtyRange(s);
        if (!r.valid)
        {
            lo = 0u;
            hi = 0x3FFFu;
            break;
        }
        lo = std::min(lo, r.first);
        hi = std::max(hi, r.last);
    }
    g_g700DebtLo = lo;
    g_g700DebtHi = hi;
}

static inline bool g700DebtTouches(const G144BlockRange &r)
{
    return r.valid && g_g700DebtLo <= g_g700DebtHi && r.first <= g_g700DebtHi &&
           g_g700DebtLo <= r.last;
}

// ⭐ THE CONSUMER EDGE. Before this batch's textures, CLUTs and framebuffer are read out of guest
// VRAM, publish any surface whose debt those bytes intersect. This is the analogue of
// `g261PrepareCpuReplay` — but where that one runs unconditionally for every transient target on
// every flush (`[G523:fit] pro0trn` = 1.08 ms/f), this one fires only when a batch genuinely reads
// bytes another surface is still holding on the GPU.
static void g700PrepareForBatch(const std::vector<G144Entry> &entries)
{
    if (!g700On() || g_g700DebtLo > g_g700DebtHi || entries.empty())
        return;
    for (const G144Entry &e : entries)
    {
        if (!e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        const G144BlockRange clut =
            g144IsPalettedPsm(e.ctx.tex0.psm) ? g144ClutRange(e) : G144BlockRange{};
        const bool unknown = !tex.valid;
        if (!unknown && !g700DebtTouches(tex) && !g700DebtTouches(clut))
            continue;
        for (uint32_t i = 0; i < kG700Slots; ++i)
        {
            G700Surface &s = g_g700Surf[i];
            if (!s.live || s.dirtyHi < s.dirtyLo)
                continue;
            const G144BlockRange r = g700DirtyRange(s);
            if (unknown || !r.valid || g144RangeOverlaps(r, tex) ||
                (clut.valid && g144RangeOverlaps(r, clut)))
                (void)g700Materialize(static_cast<int>(i), s.dirtyLo, s.dirtyHi,
                                      0 /* kG261MatCpuReplay */);
        }
        if (g_g700DebtLo > g_g700DebtHi)
            return; // everything published
    }
}

// A fire-and-forget render failed somewhere behind us (G701). The FBO contents are unknown, so
// every claim is void: drop ownership without publishing (publishing would write garbage into
// guest VRAM) and let the next batch re-establish each surface from VRAM.
static void g700DropAllClaims()
{
    if (!g700On())
        return;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        G700Surface &s = g_g700Surf[i];
        if (!s.live)
            continue;
        g700ReleaseAll(s, static_cast<int>(i));
        s.covLo = kG700FbH;
        s.covHi = -1;
        s.depthLive = false;
        s.depthLo = kG700FbH;
        s.depthHi = -1;
    }
    g700RecomputeDebtBounds();
}

static void g700MaterializeAll(int cause)
{
    if (!g700On())
        return;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        G700Surface &s = g_g700Surf[i];
        if (s.live && s.dirtyHi >= s.dirtyLo)
            (void)g700Materialize(static_cast<int>(i), s.dirtyLo, s.dirtyHi, cause);
    }
}

// ---------------------------------------------------------------------------------------------
// Guest-write backstop (I4)
// ---------------------------------------------------------------------------------------------
//
// Installed into the same single-null-branch hook the G633 residency planner uses. It does NOT
// publish (a write path cannot: the bytes may already be half-written). It only drops claims, so
// the affected pages revert to guest-VRAM authority and the next batch uploads them. That is the
// conservative direction in every case.

static G633GuestWriteHook g_g700PrevWriteHook = nullptr;

static void g700GuestWriteHook(uint32_t basePage, uint32_t widthPages, uint32_t py0, uint32_t py1,
                               uint32_t px0, uint32_t px1, bool exactAligned, uint32_t source)
{
    // Chain: G633's residency planner owns this slot when it is armed, and both may be on.
    if (g_g700PrevWriteHook != nullptr)
        g_g700PrevWriteHook(basePage, widthPages, py0, py1, px0, px1, exactAligned, source);
    // g700Materialize's own publication re-anchors explicitly and must not disown the pages it
    // just brought into sync.
    if (source == kG700BumpSource)
        return;
    const uint32_t slack = exactAligned ? 0u : 1u;
    for (uint32_t py = py0; py <= py1; ++py)
    {
        const uint32_t rowBase = basePage + py * widthPages;
        for (uint32_t px = px0; px <= px1 + slack; ++px)
        {
            const uint32_t vp = (rowBase + px) & (kG700VramPages - 1u);
            const uint8_t o = g_g700PageOwner[vp];
            if (o == 0u)
                continue;
            G700Surface &s = g_g700Surf[o - 1u];
            if (!s.live)
            {
                g_g700PageOwner[vp] = 0u;
                continue;
            }
            const uint32_t p = (vp - s.basePage) & (kG700VramPages - 1u);
            if (p < s.pageCount)
                s.owned[p] = 0u;
            g_g700PageOwner[vp] = 0u;
            g_g700Disowns.fetch_add(1u, std::memory_order_relaxed);
            g_g700DisownBySrc[source & 31u].fetch_add(1u, std::memory_order_relaxed);
        }
    }
}

// Installed once, on the first acquire. The shipped (unarmed) build never reaches this, so the
// hot bump walker keeps the single predictable null branch it already ships with.
static void g700EnsureHook()
{
    static bool s_installed = false;
    if (s_installed)
        return;
    s_installed = true;
    g_g700PrevWriteHook = g_g633GuestWriteHook;
    g_g633GuestWriteHook = &g700GuestWriteHook;
}

// ---------------------------------------------------------------------------------------------
// Depth (task 2): a persistent GPU depth attachment that is never read back
// ---------------------------------------------------------------------------------------------
//
// The `guestDepth` discovered-target class is on the CPU band replay because guest Z must stay
// authoritative and only a readback can keep it so. It does not have to be: the same page
// ownership token that answers "who holds the newer colour" answers it for Z. A surface that owns
// its depth pages keeps the depth attachment on the GPU across every flush, uploads guest Z only
// when a page of the depth range has been written behind its back, and materialises only when a
// real consumer reads those bytes.
//
// `depthKey` is the backend's own persistent depth-texture key, so binding is a lookup, not an
// allocation, and the contents survive between flushes by construction.

// Claim this surface's depth identity. Returns true when the GPU may keep the existing depth
// contents (no upload needed), false when guest Z must be uploaded first.
static bool g700DepthAcquire(int slot, uint32_t zbp, uint32_t zpsm, uint64_t depthKey,
                             int rowLo, int rowHi)
{
    if (slot < 0 || !g700DepthOn())
        return false;
    G700Surface &s = g_g700Surf[slot];
    if (s.depthKey != depthKey || s.zbp != zbp || s.zpsm != zpsm)
    {
        // A different Z buffer is now attached: nothing about the resident contents applies.
        s.depthKey = depthKey;
        s.zbp = zbp;
        s.zpsm = zpsm;
        s.depthLive = false;
        s.depthLo = kG700FbH;
        s.depthHi = -1;
        return false;
    }
    if (!s.depthLive || rowLo < s.depthLo || rowHi > s.depthHi)
        return false; // rows this attachment has never held
    // The depth pages must not have moved in guest VRAM behind the GPU's back. Z pages live at
    // `zbp` with this surface's buffer width; ask the same page-generation question the colour
    // path asks, over the Z range.
    const uint32_t zBase = zbp;
    const uint32_t hSh = g700PageHShift(zpsm);
    const uint32_t pLo = static_cast<uint32_t>(std::max(0, rowLo)) >> hSh;
    const uint32_t pHi = static_cast<uint32_t>(std::min(kG700FbH - 1, rowHi)) >> hSh;
    for (uint32_t py = pLo; py <= pHi; ++py)
        for (uint32_t px = 0; px < s.widthPages; ++px)
        {
            const uint32_t vp = (zBase + py * s.widthPages + px) & (kG700VramPages - 1u);
            if (g_g700PageOwner[vp] != static_cast<uint8_t>(slot + 1))
                return false; // somebody else owns it, or guest VRAM does
        }
    g_g700DepthKeeps.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

// The GPU just rendered depth for rows [rowLo, rowHi]. Claim the Z pages exactly as the colour
// path claims colour pages, so a guest write to them is detected and forces the next upload.
static void g700DepthNoteRender(int slot, int rowLo, int rowHi)
{
    if (slot < 0 || !g700DepthOn() || rowHi < rowLo)
        return;
    G700Surface &s = g_g700Surf[slot];
    if (s.depthKey == 0u)
        return;
    const uint32_t hSh = g700PageHShift(s.zpsm);
    const uint32_t pLo = static_cast<uint32_t>(std::max(0, rowLo)) >> hSh;
    const uint32_t pHi = static_cast<uint32_t>(std::min(kG700FbH - 1, rowHi)) >> hSh;
    for (uint32_t py = pLo; py <= pHi; ++py)
        for (uint32_t px = 0; px < s.widthPages; ++px)
        {
            const uint32_t vp = (s.zbp + py * s.widthPages + px) & (kG700VramPages - 1u);
            g_g700PageOwner[vp] = static_cast<uint8_t>(slot + 1);
        }
    s.depthLive = true;
    s.depthLo = std::min(s.depthLo, rowLo);
    s.depthHi = std::max(s.depthHi, rowHi);
}

// ---------------------------------------------------------------------------------------------
// Direct RTT consumption — the half that turns "defer the readback" into "never read back"
// ---------------------------------------------------------------------------------------------
//
// A surface whose pixels a later batch SAMPLES is the case the whole model exists for. Publishing
// it into guest VRAM so the texture pass can decode it back out is a full GPU->CPU->GPU round trip
// to move bytes that never had to leave the device. `G178Draw::srcFbp` already expresses the
// alternative — "sample the persistent FBO colour texture of this target instead of a texKey
// upload" — and G342 built exactly one narrow consumer for it (display TRISTRIP + FST + bilinear
// + CT32, `g342Ct32BindOn`, default-off). This generalises it to any G700 surface.
//
// Admission is geometric identity, not heuristics: the texture must name this surface's own base
// and width, in a format whose in-FBO pixel layout is the surface's own, and the sampled rows must
// be rows the surface actually holds. Anything else materialises as before.

// Width in pixels of the live surface at `fbp`, or 0 when there is none. Used by the UV
// normalisation, which must divide by the PRODUCER's width rather than the declared texture dims.
static int g700SurfaceFbW(uint32_t fbp)
{
    const int slot = g700Find(fbp);
    return slot >= 0 ? g_g700Surf[slot].fbW : 0;
}

// Can this entry sample a G700 surface's FBO colour texture directly? Returns the surface's fbp
// (a valid backend FBO key) or 0.
static inline uint32_t g700DirectSampleSrc(const G144Entry &e, uint32_t dstFbp)
{
    // ⚠️ THIS RUNS PER ENTRY IN THE BIND LOOP — ~273 entries x 16.7 flushes = 4,560 calls/frame on
    // `dragon`. A linear scan over 48 slots would put a quarter of a million loads a frame on the
    // pole thread to answer "no" almost every time (G502: never let a gate cost more than the
    // thing it gates). `g_g700BaseToSlot` is the complete key space — a FRAME base is a 9-bit page
    // number — so the answer is one indexed byte load.
    if (!g700On() || !e.prim.tme)
        return 0u;
    const uint32_t tbp = e.ctx.tex0.tbp0;
    if ((tbp & 31u) != 0u) // a surface base is page-aligned by construction
    {
        if (g700StatOn())
            g_g700BindRej[0].fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    const uint32_t page = tbp >> 5u;
    if (page >= kG700VramPages)
        return 0u;
    const uint8_t slot1 = g_g700BaseToSlot[page];
    if (slot1 == 0u)
    {
        if (g700StatOn())
            g_g700BindRej[1].fetch_add(1u, std::memory_order_relaxed);
        return 0u; // the texture does not name any live surface's base
    }
    G700Surface &s = g_g700Surf[slot1 - 1u];
    if (!s.live || s.fbp == dstFbp || s.covHi < s.covLo)
    {
        if (g700StatOn())
            g_g700BindRej[2].fetch_add(1u, std::memory_order_relaxed);
        return 0u; // never feed a target from itself
    }
    if (e.ctx.tex0.tbw != s.fbw)
    {
        if (g700StatOn())
            g_g700BindRej[3].fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    // The FBO holds the surface's own pixel layout, so only a same-format read is exact.
    const uint32_t tpsm = e.ctx.tex0.psm;
    if (s.psm != tpsm && !(s.psm == GS_PSM_CT32 && tpsm == GS_PSM_CT24))
    {
        if (g700StatOn())
            g_g700BindRej[4].fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    // The rows this entry SAMPLES must be rows the FBO defines. The declared TH is a log2 size
    // and games routinely over-declare it, so use the entry's own V extent when the entry carries
    // one (FST addresses texels directly in 12.4 fixed point) and fall back to the declaration
    // otherwise. Sampling above `covHi` would read FBO rows this surface never wrote.
    int maxV = 1 << std::min<uint8_t>(e.ctx.tex0.th, 15u);
    if (e.prim.fst)
    {
        const int n = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
        int hi = 0;
        for (int k = 0; k < n; ++k)
            hi = std::max(hi, static_cast<int>(e.v[k].v) >> 4);
        maxV = hi + 1;
    }
    if (s.covLo > 0 || s.covHi + 1 < maxV)
    {
        if (g700StatOn())
            g_g700BindRej[5].fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    s.lastUse = ++g_g700Clock;
    return s.fbp;
}

// ---------------------------------------------------------------------------------------------
// Census
// ---------------------------------------------------------------------------------------------

static void g700Report()
{
    if (!g700StatOn())
        return;
    uint32_t liveN = 0u;
    for (uint32_t i = 0; i < kG700Slots; ++i)
        liveN += g_g700Surf[i].live ? 1u : 0u;
    const uint64_t up = g_g700Uploads.load(std::memory_order_relaxed);
    const uint64_t skip = g_g700UploadSkips.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G700:auth] live=%u acquires=%llu evict=%llu resets=%llu renders=%llu "
                 "uploads=%llu skipped=%llu (%.1f%%) mats=%llu matRows=%llu disowns=%llu "
                 "depthKeeps=%llu directBinds=%llu\n",
                 liveN,
                 (unsigned long long)g_g700Acquires.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700Evictions.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700Resets.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700Renders.load(std::memory_order_relaxed),
                 (unsigned long long)up, (unsigned long long)skip,
                 (up + skip) ? 100.0 * (double)skip / (double)(up + skip) : 0.0,
                 (unsigned long long)g_g700Mats.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700MatRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700Disowns.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700DepthKeeps.load(std::memory_order_relaxed),
                 (unsigned long long)g_g700DirectBinds.load(std::memory_order_relaxed));
    {
        static const char *const kCause[8] = {"cpuReplay", "inline", "upload", "local",
                                              "l2h",       "bound",  "resync", "fail"};
        char buf[512];
        int off = std::snprintf(buf, sizeof(buf), "[G700:cause] mats");
        for (int c = 0; c < 8 && off > 0 && off < (int)sizeof(buf); ++c)
        {
            const uint64_t v = g_g700MatByCause[c].load(std::memory_order_relaxed);
            if (v != 0u)
                off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%llu",
                                     kCause[c], (unsigned long long)v);
        }
        static const char *const kBindRej[6] = {"unaligned", "noSurface", "self",
                                                "width",     "format",    "rows"};
        if (off > 0 && off < (int)sizeof(buf))
            off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " | bindRej");
        for (int r = 0; r < 6 && off > 0 && off < (int)sizeof(buf); ++r)
        {
            const uint64_t v = g_g700BindRej[r].load(std::memory_order_relaxed);
            if (v != 0u)
                off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%llu",
                                     kBindRej[r], (unsigned long long)v);
        }
        if (off > 0 && off < (int)sizeof(buf))
            off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " | disownSrc");
        for (int s = 0; s < 32 && off > 0 && off < (int)sizeof(buf); ++s)
        {
            const uint64_t v = g_g700DisownBySrc[s].load(std::memory_order_relaxed);
            if (v != 0u)
                off += std::snprintf(buf + off, sizeof(buf) - (size_t)off, " %d=%llu", s,
                                     (unsigned long long)v);
        }
        std::fprintf(stderr, "%s\n", buf);
    }
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        const G700Surface &s = g_g700Surf[i];
        if (!s.live || s.renders == 0u)
            continue;
        std::fprintf(stderr,
                     "[G700:surf] slot=%u fbp=0x%x fbw=%u psm=%u cov=%d..%d dirty=%d..%d "
                     "renders=%llu depth=%u/%d..%d\n",
                     i, s.fbp, s.fbw, s.psm, s.covLo, s.covHi, s.dirtyLo, s.dirtyHi,
                     (unsigned long long)s.renders, s.depthLive ? 1u : 0u, s.depthLo, s.depthHi);
    }
}
