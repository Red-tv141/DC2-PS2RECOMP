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

// ⭐ G718: row-scoped publication. The shipped G700 surrendered `dirtyLo..dirtyHi` — the WHOLE
// surface — whenever any reader's block range touched any page of it. On `dungeon1` that measured
// `mats == renders` (33,061 each) at 306 rows per publication, i.e. the persistent model paid the
// transient model's readback anyway. Rollback: DC2_G718_NO_ROWSCOPE=1.
// ⛔ MEASURED NULL, kept OPT-IN. `[G718:prep]` reported `saved=0.0%` on `dungeon1`: the reader
// covers the whole surface, so no row window is narrower than the debt. Exact and strictly
// narrower by construction, so it stays available — but it is not part of the promoted
// configuration, which is G700 alone.
static bool g718RowScopeOn()
{
    static const bool on =
        envFlagEnabled("DC2_G718_ROWSCOPE") && !envFlagEnabled("DC2_G718_NO_ROWSCOPE");
    return on;
}

// ⭐⭐⭐ G718 TARGET-ALIAS PUBLICATION — a batch's framebuffer/depth UPLOAD reads guest VRAM for
// rows that routinely alias another persistent surface. Correctness fix for the promoted
// configuration, so DEFAULT-ON. Rollback: DC2_G718_NO_TGTPUB=1.
static bool g718TargetPubOn()
{
    static const bool on = !envFlagEnabled("DC2_G718_NO_TGTPUB");
    return on;
}
static std::atomic<uint64_t> g_g718TgtPubs{0};

// A scissor permits writes; it does not prove those rows were written. Match the
// production G621 publication footprint when establishing persistent authority.
//
// ⛔⛔⛔ MEASURED: DEFAULT-ON WAS A MECHANISM-KILLER AND A PIXEL NULL. Narrowing the CLAIM to
// `g621PubLo..g621PubHi` without narrowing the QUERY (`g700NeedUpload` still asks about the whole
// batch row window) leaves the rows between the write span and the scissor unowned, so the very
// next batch uploads. `dungeon1`, `dc2_runner_g718ship3.exe`, same route, same census:
//
//     writespan ON   uploads=37,329  skipped=52     (0.1%)
//     writespan OFF  uploads=23,162  skipped=14,219 (38.0%)
//
// i.e. it deleted 99.7% of the upload skipping the persistent surface exists to produce. And it
// bought nothing: the whole-frame pixel table is IDENTICAL to fifteen significant digits with it
// on or off (`g718v3_dungeon1` vs `g718v4_nows`, sf1892 mean 0.010448042168674699 / 226 px in
// both). ⭐ That pair is also the proof that the surviving defect is NOT stale FBO content: at
// 0.1% skipping the FBO is re-uploaded from guest VRAM on essentially every render and the wrong
// pixels do not move. Rolled back to OPT-IN; the code is retained because it is exact.
static bool g718WriteSpanOn()
{
    static const bool on = envFlagEnabled("DC2_G718_WRITESPAN") &&
                           !envFlagEnabled("DC2_G718_NO_WRITESPAN");
    return on;
}

// ⭐⭐⭐ G718 PAGE ARBITRATION. `g700PageIsGpu` asks `s.owned[p]` and the generation anchor, and
// NEITHER answers "is this surface still the one the reverse index names for that VRAM page".
// On `dungeon1` the four live surfaces are 0x13b (fbw=8, 128 pages: 0x13b..0x1ba) and 0x13f /
// 0x141 / 0x14b (fbw=2, 32 pages each) — all three of the small ones live INSIDE 0x13b's page span,
// and they overlap each other. A GPU render bumps no guest generation, so after A and then B render
// the same VRAM page both hold `owned[p]=1` with an intact anchor and both answer "GPU has the
// newer copy" for one page whose two FBOs disagree. Ownership must be exclusive: the reverse index
// is the arbiter, a claim steals the page from the previous owner, and a surface that lost a page
// falls back to guest VRAM (an upload), which is the fail-closed direction.
// Rollback: DC2_G718_NO_ARB=1.
static bool g718ArbOn()
{
    static const bool on = envFlagEnabled("DC2_G718_ARB") && !envFlagEnabled("DC2_G718_NO_ARB");
    return on;
}
static std::atomic<uint64_t> g_g718Steals{0}, g_g718ArbRejects{0};

// ⭐ G718 TRANSIENT ORDERING PROBES — both DEFAULT OFF, both diagnostics for the one surviving
// whole-frame defect (dungeon1 sf1892: 226 px, bottom two rows, |d|<=30, present in every
// transient-takeover configuration measured so far and absent from every colour-only one).
//   TRNPUB: under takeover, still run the legacy `g261PrepareCpuReplay()` the takeover deletes,
//           restoring the sibling-publication ORDER the four hardcoded contracts imposed.
//   TRNRB:  under takeover, publish the surface's whole covered span immediately after the render,
//           which is what the per-flush readback used to do. If this closes the defect, the defect
//           is the publication FOOTPRINT, not authority; if it does not, publication is exonerated.
static bool g718TrnPubOn()
{
    static const bool on = envFlagEnabled("DC2_G718_TRNPUB");
    return on;
}
static bool g718TrnRbOn()
{
    static const bool on = envFlagEnabled("DC2_G718_TRNRB");
    return on;
}
static std::atomic<uint64_t> g_g718TrnPubs{0}, g_g718TrnRbs{0}, g_g718TrnRbRows{0};

// The four hardcoded transient FRAME bases the takeover replaces. Used by the TRNRB probe, which
// runs at the render site where `g700TrnTakeover` itself is out of scope. The predicates come from
// rasterizer_command_graph.inc, included well before this file in ps2_gs_rasterizer.cpp.
static bool g718IsTakeoverFbp(uint32_t fbp)
{
    return g286TransientTarget(fbp) || g570Transient13dTarget(fbp) ||
           g586Transient141Target(fbp) || g587Transient15dTarget(fbp);
}

// ⭐⭐⭐ G718 LATE PREPARE — publish only for readers that did NOT get a GPU source. See the block
// above `g718PrepareUnbound` near the end of this file for the full argument.
static bool g718LatePrepOn()
{
    static const bool on =
        envFlagEnabled("DC2_G718_LATEPREP") && !envFlagEnabled("DC2_G718_NO_LATEPREP");
    return on;
}

// Census for the consumer edge, so a publication's CAUSE can be told from its COST. Only ever
// reached under DC2_G700_STAT=1 (the caller tests it), so the shipped path pays nothing.
static std::atomic<uint64_t> g_g718PrepUnknown{0}, g_g718PrepTex{0}, g_g718PrepClut{0},
    g_g718PrepRows{0}, g_g718PrepDebtRows{0}, g_g718Preps{0};
static std::atomic<uint32_t> g_g718PrepSampleTbp{0}, g_g718PrepSampleTbw{0},
    g_g718PrepSamplePsm{0}, g_g718PrepSampleFbp{0}, g_g718PrepSampleFbw{0};

static void g718NotePrepare(bool unknown, bool texHit, uint32_t rows, uint32_t debtRows,
                            const G144Entry &e, uint32_t sfbp, uint32_t sfbw)
{
    g_g718Preps.fetch_add(1u, std::memory_order_relaxed);
    if (unknown)
        g_g718PrepUnknown.fetch_add(1u, std::memory_order_relaxed);
    else if (texHit)
        g_g718PrepTex.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g718PrepClut.fetch_add(1u, std::memory_order_relaxed);
    g_g718PrepRows.fetch_add(rows, std::memory_order_relaxed);
    g_g718PrepDebtRows.fetch_add(debtRows, std::memory_order_relaxed);
    // One sampled offender, refreshed cheaply: which texture is reading which surface.
    g_g718PrepSampleTbp.store(e.ctx.tex0.tbp0, std::memory_order_relaxed);
    g_g718PrepSampleTbw.store(e.ctx.tex0.tbw, std::memory_order_relaxed);
    g_g718PrepSamplePsm.store(e.ctx.tex0.psm, std::memory_order_relaxed);
    g_g718PrepSampleFbp.store(sfbp, std::memory_order_relaxed);
    g_g718PrepSampleFbw.store(sfbw, std::memory_order_relaxed);
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

    // ⭐ G718: per-PAGE-ROW block ranges, computed once per reset. A publication triggered by a
    // reader only has to surrender the rows that reader actually touches — the shipped code
    // published `dirtyLo..dirtyHi` (the whole surface) for a texture that reads 64 of 416 rows,
    // which is why `matRows/mat` measured 306 on `dungeon1` with `mats == renders`.
    // 512 rows / (1 << pageHShift) => 16 entries for CT32/CT24, 8 for CT16.
    static constexpr uint32_t kMaxPageRows = kG700FbH >> 5;
    uint32_t pageRowCount = 0u;
    uint32_t pageRowFirst[kMaxPageRows]{};
    uint32_t pageRowLast[kMaxPageRows]{};
    uint8_t pageRowValid[kMaxPageRows]{};
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
    // G718: tabulate this geometry's page-row block ranges once, so a reader-triggered publication
    // can be scoped to the rows the reader overlaps instead of the whole outstanding debt.
    const uint32_t rowsPerPageRow = 1u << s.pageHShift;
    s.pageRowCount = std::min<uint32_t>(G700Surface::kMaxPageRows,
                                        static_cast<uint32_t>(kG700FbH) >> s.pageHShift);
    for (uint32_t pr = 0; pr < s.pageRowCount; ++pr)
    {
        const G144BlockRange r =
            g144RangeForRect(framePageBaseToBlock(s.fbp), static_cast<uint8_t>(s.psm), s.fbw, 0u,
                             pr * rowsPerPageRow, static_cast<uint32_t>(s.fbW), rowsPerPageRow);
        s.pageRowValid[pr] = r.valid ? 1u : 0u;
        s.pageRowFirst[pr] = r.first;
        s.pageRowLast[pr] = r.last;
    }
    g_g700Resets.fetch_add(1u, std::memory_order_relaxed);
}

// ⭐ G718: the row window of `s` that a reader of `a` (and optionally `b`) actually observes.
// Returns false when nothing overlaps. An invalid/unknown range fails closed to the whole debt,
// exactly as the shipped code did unconditionally.
static bool g700OverlapRows(const G700Surface &s, const G144BlockRange &a, const G144BlockRange &b,
                            bool unknown, int &outLo, int &outHi)
{
    outLo = s.dirtyLo;
    outHi = s.dirtyHi;
    if (unknown || s.pageRowCount == 0u)
        return true;
    const uint32_t rowsPerPageRow = 1u << s.pageHShift;
    int lo = kG700FbH, hi = -1;
    for (uint32_t pr = 0; pr < s.pageRowCount; ++pr)
    {
        if (!s.pageRowValid[pr])
            return true; // this geometry cannot be reasoned about; keep the shipped behaviour
        const uint32_t f = s.pageRowFirst[pr], l = s.pageRowLast[pr];
        const bool hit = (a.valid && a.first <= l && f <= a.last) ||
                         (b.valid && b.first <= l && f <= b.last);
        if (!hit)
            continue;
        lo = std::min<int>(lo, static_cast<int>(pr * rowsPerPageRow));
        hi = std::max<int>(hi, static_cast<int>((pr + 1u) * rowsPerPageRow) - 1);
    }
    if (hi < lo)
        return false;
    // Never publish rows the surface does not owe.
    outLo = std::max(lo, s.dirtyLo);
    outHi = std::min(hi, s.dirtyHi);
    return outHi >= outLo;
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
    // I6 (G718): ownership is EXCLUSIVE. An overlapping surface that claimed this page later is the
    // owner; this one holds a stale claim over bytes another FBO now defines.
    if (g718ArbOn() && g_g700PageOwner[vp] != static_cast<uint8_t>(slot + 1))
    {
        s.owned[p] = 0u;
        g_g718ArbRejects.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
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
            // I6 (G718): claiming a page STEALS it. Drop the previous owner's claim explicitly
            // rather than leaving two surfaces both believing their FBO defines these bytes.
            if (g718ArbOn())
            {
                const uint8_t prev = g_g700PageOwner[vp];
                if (prev != 0u && prev != static_cast<uint8_t>(slot + 1))
                {
                    G700Surface &o = g_g700Surf[prev - 1u];
                    const uint32_t op = (vp - o.basePage) & (kG700VramPages - 1u);
                    if (op < o.pageCount && o.owned[op])
                    {
                        o.owned[op] = 0u;
                        g_g718Steals.fetch_add(1u, std::memory_order_relaxed);
                    }
                }
            }
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

// ⭐ G718: does ANY persistent surface exist? The flush's bind pre-pass — which owns every
// FBO-source decision, including `g700DirectSampleSrc` and the G310 atlas refresh — was gated on
// `g261AnyDirty()`, i.e. on the fourteen-address legacy table alone. One cached bool when unarmed.
static bool g700AnyLive()
{
    if (!g700On())
        return false;
    for (uint32_t i = 0; i < kG700Slots; ++i)
        if (g_g700Surf[i].live)
            return true;
    return false;
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
        {
            // ⭐ G718: surrender only the rows the transfer observes. `!r.valid` still fails closed.
            int lo = s.dirtyLo, hi = s.dirtyHi;
            if (g718RowScopeOn() && r.valid &&
                !g700OverlapRows(s, src, dst, false, lo, hi))
                continue; // the debt and the transfer share pages but not page-rows
            (void)g700Materialize(static_cast<int>(i), lo, hi, cause);
        }
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
    // ⭐ G718: under the late-prepare arm this pass is replaced wholesale by `g718PrepareUnbound`,
    // which runs after the bind pre-pass and therefore knows which readers already have a GPU
    // source. Publishing here — before any bind decision exists — is what made `mats == renders`.
    if (g718LatePrepOn())
        return;
    if (!g700On() || g_g700DebtLo > g_g700DebtHi || entries.empty())
        return;
    // ==========================================================================================
    // ⭐⭐⭐ G735 — THE AGGREGATE CONSUMER GATE, AND THE PER-BATCH MEMO
    // ==========================================================================================
    //
    // MEASURED ON THE SHIPPED BINARY (`DC2_G720_PROFILE`, a 1/32-sampled production timer),
    // `dungeon1`, 44.4 flushes/frame, 278 entries/flush:
    //
    //     submit 4.959   residb 2.061   deps 1.818   verts 1.251   fbz 1.000   preflight 0.748 …
    //
    // `residb` is the second-largest stage of the whole flush and the largest one containing no
    // draw. It is NOT the g261 publication scan — `g261PrepareCpuReplay` returns at its
    // `!g261AnyDirty()` early-out — it is THIS FUNCTION, reached through it. The corrupting
    // ceiling probe `DC2_G524_NO_TRNMAT=1`, which deletes that one call, takes `residb`
    // **2.264 → 0.026 ms/f**, which is the attribution.
    //
    // And it publishes nothing: `[G261:stat] mat(cpu=0 inl=0 up=0 l2l=0 l2h=0 tex=0 rsync=0
    // fail=0)` over 28,672 flushes. What the cost actually is: TWO full walks of the entry list
    // (the G718 target-alias pass and the texture/CLUT pass below), each re-deriving a block range
    // per entry, run TWICE per flush — once from the flush preflight and once from
    // `g261PrepareCpuReplay` — i.e. ~1,100 range derivations per flush to prove a negative.
    //
    // Both walks are decided by one question: does anything this batch reads or writes intersect
    // the union of unpublished surface debt? `g_g700DebtLo..g_g700DebtHi` IS that union, already
    // maintained by `g700RecomputeDebtBounds`, and the batch's aggregate `wr`/`rd` sets already
    // hold every entry's colour+depth and texture+CLUT ranges (accumulated on the PARSE thread by
    // `g260NoteAppend`). `g735BatchCannotTouch` is the containment test and FAILS CLOSED on a
    // poisoned, degenerate or unavailable aggregate — exactly the cases where the walks' own
    // `!tgt.valid` / `!tex.valid` arms force a publication.
    //
    // The MEMO covers the second call. The two calls per flush see the same entries and, unless
    // the first one published something, the same surface state — so the second can only reach the
    // same decision. The key is (batch epoch, entry buffer) and the digest covers every field the
    // two walks read, so a changed surface always re-runs the walk.
    //
    // ⛔ ROLLBACK: DC2_G735_NO_PUBGATE=1 (shared with the other G735 gates).
    if (g735PubGateOn() && &entries == &g_g144List)
    {
        // ⛔⛔ MEASURED: THE GLOBAL DEBT UNION IS USELESS AS A GATE. The first form of this test
        // used `{g_g700DebtLo, g_g700DebtHi}` — the min/max over every live surface — and
        // `residb` moved 2.155 → 2.113 ms/f, i.e. **the gate never fired**. With dozens of live
        // surfaces that union spans most of the addresses a batch touches, so a containment test
        // against it can only ever say "maybe". The union is fine as the walks' own per-entry
        // PRE-FILTER (it is one compare against a range already in a register); it is not a
        // decision procedure. Test the surfaces INDIVIDUALLY instead: 48 slots, of which only the
        // live-and-dirty ones are examined, against 278 entries × 2 walks × 2 calls.
        //
        // ⭐ Rule (G735-3): an aggregate gate must be tested against the SAME granularity the scan
        // decides at. Gating a per-surface decision on the union of all surfaces is not a
        // containment argument, it is a coarser question with a different answer.
        uint64_t g735Digest = 0xcbf29ce484222325ull;
        const auto mix = [&g735Digest](uint64_t v) {
            g735Digest = (g735Digest ^ v) * 0x100000001b3ull;
        };
        mix(static_cast<uint64_t>(g_g700DebtLo) << 32 | g_g700DebtHi);
        for (uint32_t i = 0; i < kG700Slots; ++i)
        {
            const G700Surface &s = g_g700Surf[i];
            if (!s.live)
                continue;
            mix((static_cast<uint64_t>(i) << 56) ^ (static_cast<uint64_t>(s.fbp) << 40) ^
                (static_cast<uint64_t>(s.fbw) << 32) ^
                (static_cast<uint64_t>(static_cast<uint32_t>(s.dirtyLo)) << 16) ^
                static_cast<uint64_t>(static_cast<uint32_t>(s.dirtyHi)));
        }
        static const void *s_g735PrepKey = nullptr;
        static uint64_t s_g735PrepEpoch = ~0ull;
        static uint64_t s_g735PrepDigest = 0u;
        const uint64_t g735Epoch = g_g260BatchesExecuted;
        if (s_g735PrepKey == static_cast<const void *>(entries.data()) &&
            s_g735PrepEpoch == g735Epoch && s_g735PrepDigest == g735Digest)
        {
            g735NoteG700Memo();
            return; // already walked this exact batch against this exact surface state
        }
        s_g735PrepKey = static_cast<const void *>(entries.data());
        s_g735PrepEpoch = g735Epoch;
        s_g735PrepDigest = g735Digest;
    }
    // ⭐⭐⭐ G718 — THE TARGET-ALIAS EDGE. This pass tested only what a batch READS AS A TEXTURE.
    // It never tested what the batch WRITES: a flush uploads its framebuffer (and depth) out of
    // guest VRAM for the rows it is about to render, and on this game those rows routinely ALIAS a
    // persistent surface. On `s05` the two surfaces are `0x153` (fbw=1) and `0x13f` (fbw=2), and
    // both sit INSIDE the page span of the 512-wide `0x139` work target — so a `0x139` batch's
    // upload read pages the surfaces still owned. Symptom: `s05` sf 5896, **207,290 differing
    // pixels**, a diffuse low-amplitude error over the whole scene (`max=36`, no 8x8 tile over 20).
    // ⛔ The identical 207,290-pixel signature is recorded in `plans/phase-G717-ledger.md` for a
    // COMPLETELY DIFFERENT GPU-authority mechanism — which is what says the hole is in the edge
    // list, not in either mechanism.
    // ==========================================================================================
    // ⭐⭐⭐ G735 — SURFACE-LIST PRUNING (this is the lever; the whole-union gate above was not)
    // ==========================================================================================
    //
    // Both walks below used to iterate ALL kG700Slots = 48 slots FOR EVERY ENTRY, re-testing
    // `s.live`, `s.dirtyHi < s.dirtyLo` and re-deriving `g700DirtyRange(s)` each time. On
    // `dungeon1` that is 278 entries × 48 = 13,344 inner iterations per walk, two walks per call,
    // two calls per flush, 44.4 flushes/frame — 2.4 million inner iterations per frame, and
    // `[G720:cpu] residb = 2.06–2.26 ms/f` is what they cost.
    //
    // The slot table is the OUTER loop's natural home, not the inner one: whether a surface is
    // live and dirty does not depend on the entry. Build the candidate list ONCE, and prune it
    // with the batch's aggregate range sets — a surface the whole batch provably cannot read or
    // write can never be hit by any single entry of it (`g735BatchCannotTouch` fails closed on a
    // null, poisoned or degenerate aggregate, which is exactly where the walks' own
    // `!tgt.valid` / `!tex.valid` arms force a publication anyway).
    //
    // ⚠️ `g700Materialize` MUTATES `s.dirtyLo/Hi`, so the per-entry body still re-reads liveness
    // and re-derives the range — the candidate list prunes WHICH slots are visited, it does not
    // cache their state. That keeps the transformation exact under publication.
    uint32_t g735Cand[kG700Slots];
    uint32_t g735NCand = 0u;
    {
        const bool g735Prune = g735PubGateOn() && &entries == &g_g144List;
        uint32_t g735Reason = 0u; // 1=poisoned aggregate 2=surface overlap 4=underivable range
        uint32_t g735LiveDirty = 0u;
        for (uint32_t i = 0; i < kG700Slots; ++i)
        {
            const G700Surface &s = g_g700Surf[i];
            if (!s.live || s.dirtyHi < s.dirtyLo)
                continue;
            ++g735LiveDirty;
            if (g735Prune)
            {
                const G144BlockRange r = g700DirtyRange(s);
                if (!r.valid)
                    g735Reason |= 4u;
                else if (g735BatchCannotTouch(r))
                    continue; // no entry of this batch can touch it — drop the slot
                else
                    g735Reason |= (g_g528Wr == nullptr || g_g528Rd == nullptr ||
                                   g_g528Wr->unknown || g_g528Rd->unknown || g_g528Wr->degen)
                                      ? 1u : 2u;
            }
            g735Cand[g735NCand++] = i;
        }
        g735NoteG700Gate(g735NCand == 0u, g735Reason, entries.size(), g735LiveDirty, g735NCand);
        if (g735NCand == 0u)
            return; // nothing this batch can reach is holding unpublished bytes
    }
    // Rollback: DC2_G718_NO_TGTPUB=1.
    if (g718TargetPubOn())
    {
        for (const G144Entry &e : entries)
        {
            const G144BlockRange tgt = g144TargetRange(e);
            if (tgt.valid && !g700DebtTouches(tgt))
                continue;
            for (uint32_t ci = 0; ci < g735NCand; ++ci)
            {
                const uint32_t i = g735Cand[ci];
                G700Surface &s = g_g700Surf[i];
                if (!s.live || s.dirtyHi < s.dirtyLo || s.fbp == e.ctx.frame.fbp)
                    continue; // a surface never has to publish for its OWN batch
                const G144BlockRange r = g700DirtyRange(s);
                if (!tgt.valid || !r.valid || g144RangeOverlaps(r, tgt))
                {
                    g_g718TgtPubs.fetch_add(1u, std::memory_order_relaxed);
                    (void)g700Materialize(static_cast<int>(i), s.dirtyLo, s.dirtyHi,
                                          0 /* kG261MatCpuReplay */);
                }
            }
            if (g_g700DebtLo > g_g700DebtHi)
                return; // everything published
        }
    }
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
        for (uint32_t ci = 0; ci < g735NCand; ++ci)
        {
            const uint32_t i = g735Cand[ci];
            G700Surface &s = g_g700Surf[i];
            if (!s.live || s.dirtyHi < s.dirtyLo)
                continue;
            const G144BlockRange r = g700DirtyRange(s);
            if (unknown || !r.valid || g144RangeOverlaps(r, tex) ||
                (clut.valid && g144RangeOverlaps(r, clut)))
            {
                // ⭐ G718: a reader that samples 64 rows of a 416-row surface must not force the
                // whole surface back to guest VRAM. `unknown` and `!r.valid` still fail closed.
                int lo = s.dirtyLo, hi = s.dirtyHi;
                if (g718RowScopeOn() && !unknown && r.valid &&
                    !g700OverlapRows(s, tex, clut, false, lo, hi))
                    continue;
                if (g700StatOn())
                    g718NotePrepare(unknown, tex.valid && g144RangeOverlaps(r, tex),
                                    static_cast<uint32_t>(hi - lo + 1),
                                    static_cast<uint32_t>(s.dirtyHi - s.dirtyLo + 1), e, s.fbp,
                                    s.fbw);
                (void)g700Materialize(static_cast<int>(i), lo, hi, 0 /* kG261MatCpuReplay */);
            }
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
// ⭐⭐⭐ G718 — LATE PREPARE: publish only for readers that did NOT get a GPU source
// ---------------------------------------------------------------------------------------------
//
// `g700PrepareForBatch` runs at the TOP of `g178TryFlushGpu`, before any bind decision exists, so
// it publishes for EVERY entry whose texture or CLUT range touches a surface's debt — including
// the entries the bind pre-pass is about to serve straight from that surface's FBO. That is the
// whole of `[G700:cause] mats cpuReplay`, and it is why `mats == renders`.
//
// The late pass runs where `s_g261FboSrc[]` is final: an entry with an FBO source never reads
// guest VRAM for its texture, so it owes no publication. Everything else is published exactly as
// before, row-scoped. An empty `fboSrc` (the pre-pass did not run for this flush) means NO entry
// got a source, so every one is treated as unbound — identical to the old behaviour.
//
// ⚠️ CLUTs are always published: a paletted texture reads its CLUT out of guest VRAM even when the
// texels come from an FBO.
//
// Arm: DC2_G718_LATEPREP=1 (bring-up default OFF). Rollback once promoted: DC2_G718_NO_LATEPREP=1.
// (`g718LatePrepOn` is declared with the other arming predicates at the top of this file, because
// `g700PrepareForBatch` — defined above — has to consult it.)

static std::atomic<uint64_t> g_g718LateEntries{0}, g_g718LateBound{0}, g_g718LatePublished{0};

static void g718PrepareUnbound(const std::vector<G144Entry> &entries,
                               const std::vector<uint32_t> &fboSrc)
{
    if (!g700On() || g_g700DebtLo > g_g700DebtHi || entries.empty())
        return;
    const bool haveSrc = fboSrc.size() == entries.size();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const G144Entry &e = entries[i];
        if (!e.prim.tme)
            continue;
        g_g718LateEntries.fetch_add(1u, std::memory_order_relaxed);
        const bool bound = haveSrc && fboSrc[i] != 0u;
        const G144BlockRange clut =
            g144IsPalettedPsm(e.ctx.tex0.psm) ? g144ClutRange(e) : G144BlockRange{};
        // A bound entry owes nothing for its TEXELS. Its CLUT is still a guest-VRAM read.
        const G144BlockRange tex = bound ? G144BlockRange{} : g144TextureRange(e);
        if (bound)
            g_g718LateBound.fetch_add(1u, std::memory_order_relaxed);
        const bool unknown = !bound && !tex.valid;
        if (!unknown && !g700DebtTouches(tex) && !g700DebtTouches(clut))
            continue;
        for (uint32_t s = 0; s < kG700Slots; ++s)
        {
            G700Surface &surf = g_g700Surf[s];
            if (!surf.live || surf.dirtyHi < surf.dirtyLo)
                continue;
            const G144BlockRange r = g700DirtyRange(surf);
            if (!(unknown || !r.valid || (tex.valid && g144RangeOverlaps(r, tex)) ||
                  (clut.valid && g144RangeOverlaps(r, clut))))
                continue;
            int lo = surf.dirtyLo, hi = surf.dirtyHi;
            if (g718RowScopeOn() && !unknown && r.valid &&
                !g700OverlapRows(surf, tex, clut, false, lo, hi))
                continue;
            g_g718LatePublished.fetch_add(1u, std::memory_order_relaxed);
            (void)g700Materialize(static_cast<int>(s), lo, hi, 0 /* kG261MatCpuReplay */);
        }
        if (g_g700DebtLo > g_g700DebtHi)
            return; // everything published
    }
}

// ⛔⛔ THE HOLE THE LATE PASS OPENS, AND ITS CLOSURE.
//
// The shipped `g700PrepareForBatch` ran at the TOP of the flush, so guest VRAM was whole before
// ANY of the flush's ~15 `return false` sites could hand the batch to the CPU band replay — and
// that replay reads guest VRAM for every entry, including the ones the pre-pass bound to an FBO.
// Moving the publication after the pre-pass removes that guarantee twice over: an EARLY rejection
// (before the pre-pass) now publishes nothing at all, and a LATE one (`kG266FrTexAliasZ`,
// `kG266FrSubmit`, `kG266FrZReadback`) published only for the unbound entries.
//
// Closed at the CALLER, which is the one place every rejection converges: when
// `g178TryFlushGpu` returns false the batch is about to be rasterized on the CPU out of guest
// VRAM, so every surface surrenders its whole debt first. Two call sites, both in
// rasterizer_tilebin_capture.inc. Costs nothing on the success path and nothing when unarmed.
static void g718PublishBeforeCpuReplay()
{
    if (!g700On() || !g718LatePrepOn())
        return;
    g700MaterializeAll(0 /* kG261MatCpuReplay */);
}

// ---------------------------------------------------------------------------------------------
// ⭐⭐⭐ G718 — THE G310 LOGICAL ATLAS SOURCES FROM A PERSISTENT SURFACE
// ---------------------------------------------------------------------------------------------
//
// THE DEFECT. `g310ApplyDesired` picks each of the atlas's 128 pages from one of two places: a
// GPU blit out of a resident target's FBO (`kG261Fbp[t]`, the fourteen-address allowlist), or a
// guest path that decodes 8 KiB out of VRAM and uploads it. A G700 surface is at neither, so every
// page it owns took the guest path — and that is what obliged `g700PrepareForBatch` to publish the
// whole surface on every flush that sampled it. `[G718:prep]` named the exact pair on `dungeon1`:
// texture `tbp=0x2760 tbw=8 psm=CT24` reading surface `fbp=0x13b fbw=8`, 1,198 publications at 240
// rows each, `saved=0.0%` — the reader covers the surface, so no row scoping can help. PCSX2
// resolves that read to the target's own texture (`LookupSource` returning a target-backed source).
//
// EXACTNESS. A page is offered only when the surface still OWNS it with an intact generation
// anchor, i.e. exactly when the FBO is the newer copy. The atlas grid and a CT32/CT24 surface share
// the same 64x32 page geometry, so the blit is a page-for-page copy with no format conversion —
// the identical operation the legacy `kG261Fbp[t]` loop already performs. Anything else (CT16's
// 64x64 pages, a surface wider than the atlas, a disowned page) returns -1 and keeps the guest
// path, which `g718PublishForAtlasGuestRun` now makes correct on demand.
//
// Rollback: DC2_G718_NO_ATLASGPU=1.

// ⚠️ OPT-IN. On `dungeon1` this fires zero times (`[G718:atlas] gpuSrcPages=0`) — the readers bind
// the surface directly instead of going through the atlas — so it is not part of the promoted
// configuration and has never been exercised by a pixel gate. Retained because the population is
// route-dependent and the mechanism is exact where it does fire.
static bool g718AtlasGpuSourceOn()
{
    static const bool on =
        envFlagEnabled("DC2_G718_ATLASGPU") && !envFlagEnabled("DC2_G718_NO_ATLASGPU");
    return on;
}

static_assert(kG718AtlasSlots == kG700Slots,
              "G718: the atlas's slot-count mirror must match the G700 surface table");

static std::atomic<uint64_t> g_g718AtlasPages{0}, g_g718AtlasRejGeom{0}, g_g718AtlasRejOwn{0},
    g_g718AtlasSyncs{0}, g_g718AtlasSyncRows{0};

// Which G700 surface holds atlas page `pageBlock`, and where inside that surface it lives.
// Returns the slot, or -1 to leave the page on the guest path.
static int g718AtlasSourceForPage(uint32_t pageBlock, uint32_t *outRow, uint32_t *outCol,
                                  uint64_t *outGen)
{
    if (!g700On() || !g718AtlasGpuSourceOn())
        return -1;
    const uint32_t vp = (pageBlock >> 5u) & (kG700VramPages - 1u);
    const uint8_t owner = g_g700PageOwner[vp];
    if (owner == 0u)
        return -1;
    const int slot = static_cast<int>(owner) - 1;
    G700Surface &s = g_g700Surf[slot];
    // The atlas grid is CT32 64x32 pages, at most 8 columns and 16 rows.
    if (!s.live || s.widthPages == 0u || s.widthPages > 8u || s.pageHShift != 5u ||
        (s.psm != GS_PSM_CT32 && s.psm != GS_PSM_CT24))
    {
        g_g718AtlasRejGeom.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const uint32_t p = (vp - s.basePage) & (kG700VramPages - 1u);
    if (p >= s.pageCount || !g700PageIsGpu(s, slot, p))
    {
        g_g718AtlasRejOwn.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const uint32_t row = p / s.widthPages;
    const uint32_t col = p % s.widthPages;
    if (row >= 16u || col >= 8u)
    {
        g_g718AtlasRejGeom.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    // The FBO must actually define the rows this page covers.
    const int y0 = static_cast<int>(row) * 32;
    if (s.covHi < s.covLo || y0 < s.covLo || y0 + 31 > s.covHi)
    {
        g_g718AtlasRejGeom.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    if (outRow) *outRow = row;
    if (outCol) *outCol = col;
    // Content stamp: every successful batch, including its optional upload, calls
    // g700NoteRender. An atlas page is re-copied whenever the surface can have changed.
    if (outGen) *outGen = s.renders;
    g_g718AtlasPages.fetch_add(1u, std::memory_order_relaxed);
    return slot;
}

// Everything about the G700 surfaces that can change which page the atlas copies, or what that
// page contains. Folded into `g589AtlasKeyNow()` so the drain-transaction memo cannot serve a
// stale atlas across a batch that rendered into a sourced surface.
static uint64_t g718AtlasStateKey()
{
    if (!g700On() || !g718AtlasGpuSourceOn())
        return 0u;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < kG700Slots; ++i)
    {
        const G700Surface &s = g_g700Surf[i];
        if (!s.live)
            continue;
        const uint64_t v = (static_cast<uint64_t>(s.fbp) << 48) ^
                           (static_cast<uint64_t>(static_cast<uint32_t>(s.covLo)) << 32) ^
                           static_cast<uint64_t>(static_cast<uint32_t>(s.covHi));
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= s.renders + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= (static_cast<uint64_t>(static_cast<uint32_t>(s.dirtyLo)) << 32 |
              static_cast<uint64_t>(static_cast<uint32_t>(s.dirtyHi))) +
             0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h;
}

static uint32_t g718AtlasSourceFbp(int slot)
{
    if (slot < 0 || static_cast<uint32_t>(slot) >= kG700Slots || !g_g700Surf[slot].live)
        return 0u;
    return g_g700Surf[slot].fbp;
}

// The atlas is about to DECODE these pages out of guest VRAM. Any surface still holding them on
// the GPU must publish first — scoped to the page-rows actually read. This is the backstop that
// makes the guest fallback correct without a blanket publication at the top of the flush.
static void g718PublishForAtlasGuestRun(uint32_t pageY, uint32_t pageX0, uint32_t pageCount)
{
    if (!g700On() || g_g700DebtLo > g_g700DebtHi)
        return;
    for (uint32_t k = 0; k < pageCount; ++k)
    {
        const uint32_t pageBlock = 0x2720u + ((pageY * 8u) + pageX0 + k) * 32u;
        const uint32_t vp = (pageBlock >> 5u) & (kG700VramPages - 1u);
        const uint8_t owner = g_g700PageOwner[vp];
        if (owner == 0u)
            continue;
        const int slot = static_cast<int>(owner) - 1;
        G700Surface &s = g_g700Surf[slot];
        if (!s.live || s.dirtyHi < s.dirtyLo || s.widthPages == 0u)
            continue;
        const uint32_t p = (vp - s.basePage) & (kG700VramPages - 1u);
        if (p >= s.pageCount)
            continue;
        const uint32_t row = p / s.widthPages;
        const int lo = std::max(static_cast<int>(row) << s.pageHShift, s.dirtyLo);
        const int hi = std::min((static_cast<int>(row + 1u) << s.pageHShift) - 1, s.dirtyHi);
        if (hi < lo)
            continue;
        g_g718AtlasSyncs.fetch_add(1u, std::memory_order_relaxed);
        g_g718AtlasSyncRows.fetch_add(static_cast<uint64_t>(hi - lo + 1),
                                      std::memory_order_relaxed);
        (void)g700Materialize(slot, lo, hi, 0 /* kG261MatCpuReplay */);
    }
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
    // Exercise proof for the G718 ordering/arbitration arms. `arb=1 steals=0` is a REFUTATION of
    // the aliasing hypothesis on this route; `arb=0` beside a verdict means the arm never ran.
    std::fprintf(stderr,
                 "[G718:arb] arb=%u steals=%llu rejects=%llu | trnpub=%u pubs=%llu | trnrb=%u "
                 "rbs=%llu rbRows=%llu\n",
                 g718ArbOn() ? 1u : 0u,
                 (unsigned long long)g_g718Steals.load(std::memory_order_relaxed),
                 (unsigned long long)g_g718ArbRejects.load(std::memory_order_relaxed),
                 g718TrnPubOn() ? 1u : 0u,
                 (unsigned long long)g_g718TrnPubs.load(std::memory_order_relaxed),
                 g718TrnRbOn() ? 1u : 0u,
                 (unsigned long long)g_g718TrnRbs.load(std::memory_order_relaxed),
                 (unsigned long long)g_g718TrnRbRows.load(std::memory_order_relaxed));
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
    // ⭐ G718: the consumer edge, priced. `rows` vs `debtRows` is exactly what row scoping saves;
    // `tex`/`clut`/`unknown` says WHY a reader forced a publication, and the sample names the
    // texture that did it so an RT-as-texture bind can be aimed at a real address.
    {
        const uint64_t preps = g_g718Preps.load(std::memory_order_relaxed);
        const uint64_t rows = g_g718PrepRows.load(std::memory_order_relaxed);
        const uint64_t debt = g_g718PrepDebtRows.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G718:prep] preps=%llu tex=%llu clut=%llu unknown=%llu | rows=%llu "
                     "debtRows=%llu saved=%.1f%% | sample tex(tbp=0x%x tbw=%u psm=%u) "
                     "surf(fbp=0x%x fbw=%u) rowscope=%u\n",
                     (unsigned long long)preps,
                     (unsigned long long)g_g718PrepTex.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718PrepClut.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718PrepUnknown.load(std::memory_order_relaxed),
                     (unsigned long long)rows, (unsigned long long)debt,
                     debt ? 100.0 * (double)(debt - rows) / (double)debt : 0.0,
                     g_g718PrepSampleTbp.load(std::memory_order_relaxed),
                     g_g718PrepSampleTbw.load(std::memory_order_relaxed),
                     g_g718PrepSamplePsm.load(std::memory_order_relaxed),
                     g_g718PrepSampleFbp.load(std::memory_order_relaxed),
                     g_g718PrepSampleFbw.load(std::memory_order_relaxed),
                     g718RowScopeOn() ? 1u : 0u);
        std::fprintf(stderr,
                     "[G718:atlas] gpuSrcPages=%llu copies=%llu copiedPages=%llu | rejGeom=%llu "
                     "rejOwn=%llu | guestSyncs=%llu guestSyncRows=%llu armed=%u\n",
                     (unsigned long long)g_g718AtlasPages.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasCopies.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasCopiedPages.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasRejGeom.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasRejOwn.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasSyncs.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718AtlasSyncRows.load(std::memory_order_relaxed),
                     g718AtlasGpuSourceOn() ? 1u : 0u);
        std::fprintf(stderr,
                     "[G718:late] armed=%u entries=%llu bound=%llu (%.1f%%) published=%llu\n",
                     g718LatePrepOn() ? 1u : 0u,
                     (unsigned long long)g_g718LateEntries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g718LateBound.load(std::memory_order_relaxed),
                     g_g718LateEntries.load(std::memory_order_relaxed)
                         ? 100.0 *
                               (double)g_g718LateBound.load(std::memory_order_relaxed) /
                               (double)g_g718LateEntries.load(std::memory_order_relaxed)
                         : 0.0,
                     (unsigned long long)g_g718LatePublished.load(std::memory_order_relaxed));
        std::fprintf(stderr, "[G718:tgtpub] armed=%u pubs=%llu\n", g718TargetPubOn() ? 1u : 0u,
                     (unsigned long long)g_g718TgtPubs.load(std::memory_order_relaxed));
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
