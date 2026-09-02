// ===== G662: split out of rasterizer_gpu_alias_page_view.inc (was 5,116 lines) ==================
//
// WHY THIS FILE EXISTS. That include mixed ~1,000 lines of pure STATE - the G280 physical-alias
// overlay records, the G310 page-authority tables, the G326 plan caches, and the flag predicates
// and counters for G536/G281/G282/G309 - in front of the runtime functions that use them
// (`g261Report`, `g261UpdateWindow`, the G266 censuses, the G536 guest-page guard, `g264FlushMirror`
// and `g261Materialize`). Reading any one of those functions meant paging through the whole
// declaration block first.
//
// ⛔ THIS IS A PURE TEXT MOVE AND MUST STAY ONE - see the header of lle_gpu_raster_prelude.inc.
// Both halves are `#include`d into `ps2_gs_rasterizer.cpp` consecutively and in the original order.
//
// Contents: G280 overlay state - G536 page-ownership flags - G281/G282 fanout - G309 composite
// tokens - G310 page-authority tables and per-target generation maps - G326 plan-cache predicates
// and counters.

// G498: the two owner-publish colour unpacks below (g345MaterializeSlot, g289Resolve14aBeforeDirect)
// go through this helper instead of their own per-pixel addrPSMCT32 + 4-byte memcpy loop. It is
// DEFINED in g498_owner_publish.inc, which this TU includes much later (it needs G418's promoted
// fast unpack), so it is forward-declared here — the same pattern rasterizer_row_pool.inc uses for
// g420PoolNarrowArm.
static void g498PublishOwnedRows(int site, uint8_t *vram, uint32_t vramSize,
                                 const std::vector<uint32_t> &src,
                                 uint32_t dstBlock, uint32_t bw, int fbW, int fbH,
                                 int rowLo, int rowHi,
                                 uint32_t firstBlock, uint32_t lastBlock);

// ===== G280 GPU PHYSICAL-ALIAS PAGE VIEW (default-off bring-up, DC2_G280_ALIAS_VIEW=1) =====
// G267 proved the display TRISTRIP consumer of the 0x139 character RTT samples one physical CT32
// page (0x2920) that is NEWER in the dirty sibling target 0x146 — the same GS page rendered under
// two FRAME/FBW layouts (fbw=8 vs fbw=2). Today that consumer pays cause-5 materializations of
// BOTH targets plus a full 512x512 VRAM texture decode. G280 instead composes the sampled view on
// the GPU: the aliased page tiles are copied FBO->FBO (bit-exact 64x32 rects — CT32 within-page
// pixel order is layout-independent, only page SELECTION depends on FBW), the consumer binds the
// producer FBO directly (existing G261/G263 srcFbp path), and VRAM publication stays exactly on
// today's consumer edges.
//
// Ownership/versioning model:
//  - Physical-page authority is unchanged: a dirty target's FBO owns its pages until its existing
//    materialization edge publishes them; VRAM owns everything else. G280 adds NO publication.
//  - Each target gets a monotonic contentGen, bumped whenever its FBO CONTENT changes (wave
//    render, G264 mirror patch, uploadFb). An overlay entry records the src contentGen at copy
//    time; a moved gen makes the copy stale (re-resolved at the next consumer, restored from
//    VRAM before any dst publication).
//  - Overlay tiles in the dst FBO are a READ CACHE, never dst-owned: before the dst target
//    materializes (ANY cause), every live overlay tile is overwritten with the current guest
//    VRAM bytes and dropped, so dst never publishes alias-page bytes whose authority is the src
//    FBO or VRAM. Restore failure is loud and drops residency without a readback (G264 pattern).
//  - Ordered invalidation: dst waves rendering rows that overlap an overlay tile drop the entry
//    (the blit reproduced the physical page state at resolve time, so later dst draws layer on
//    top exactly as GS command order did); src waves/mirrors bump contentGen (stale -> re-copy);
//    CPU/transfer writers keep their existing materialize/mirror hooks, which land in the
//    restore-and-drop paths above.
//  - Unsupported consumers and presentation are untouched: both targets stay dirty and publish
//    at exactly today's consumer edges; the RTT family is never presented directly.
// Admitted alias tuples are evidence-limited (G267 census): dst 0x139 (ti 0) <- src 0x146 (ti 3).
// Everything else fails closed to the legacy bind/materialize loop.
struct G280Ovl
{
    uint32_t pageBlock = 0u; // absolute GS block address of the physical page (multiple of 32)
    uint8_t srcTi = 0u;
    uint64_t srcGen = 0u;    // src contentGen when the tile was copied
    uint64_t pageEpoch = 0u; // G283: physical-page epoch when resolved (authority-off => 0)
    uint16_t dstRow = 0u, dstCol = 0u; // page coords under the DST layout (row*32 / col*64 px)
    uint16_t srcRow = 0u, srcCol = 0u; // page coords under the SRC layout
};
static std::vector<G280Ovl> g_g280Ovl[kG248TargetCount]; // per DST target; flush-owner thread only
static uint64_t g_g280ContentGen[kG248TargetCount] = {};
static std::atomic<uint64_t> g_g280Binds{0}, g_g280Copies{0}, g_g280CopyFail{0},
    g_g280Restores{0}, g_g280RestoreFail{0}, g_g280Drops{0}, g_g280SibSkips{0},
    g_g280RejFoot{0}, g_g280RejTuple{0}, g_g280RejPage{0}, g_g280VerifyBad{0};

// G536: page-ownership-aware publication — gate and counters. Declared up here because
// g261Report (below) prints them while the implementation sits beside g261UpdateWindow.
static bool g536PageOwnOn()
{
    static const bool on = !envFlagEnabled("DC2_G536_NO_PAGEOWN") &&
                           !envFlagDisabled("DC2_G536_PAGEOWN");
    return on;
}
static std::atomic<uint64_t> g_g536PubTotal{0}, g_g536PubGuarded{0}, g_g536PagesKept{0};

// G455 is included here because its ownership proof needs G261/G276 state declared earlier in
// this translation unit and the G280 overlay state declared immediately above.
#include "rasterizer_g455_readback_runway.inc"

// G281: PSMT8 view over the resident 0x13c CT32 target. The view is a decoded RGBA texture
// produced entirely on the G178 worker from the source FBO + current guest CLUT; no guest-VRAM
// color publication occurs. It is deliberately a child of the G280 alias/view lever.
static std::atomic<uint64_t> g_g281Candidates{0}, g_g281Views{0}, g_g281BuildFail{0},
    g_g281RejShape{0}, g_g281RejPage{0}, g_g281RejClut{0}, g_g281VerifyBad{0};
// G282: exact captured coverage + write-side page ownership for the measured
// fbp=0x139/fbw=8 atlas batch. A page-aligned leading clear makes its touched pages independent
// of their old alias owners; after the GPU batch, exact CT32 page copies fan the new bytes back
// to every resident sibling view before any of those views can publish stale data.
struct G282Fanout
{
    uint32_t pageBlock = 0u;
    uint8_t dstTi = 0u;
    uint16_t srcRow = 0u, srcCol = 0u;
    uint16_t dstRow = 0u, dstCol = 0u;
};
static std::atomic<uint64_t> g_g282Candidates{0}, g_g282TightBatches{0},
    g_g282AliasSaved{0}, g_g282FanoutPages{0}, g_g282FanoutFail{0},
    g_g282RejShape{0}, g_g282RejOwner{0}, g_g282VerifyBad{0};
// G309: authoritative-page multi-source atlas consumer. The timing path samples live target
// textures through an 8x16 page map; the exact verifier alone builds a transient 512x512 reference.
// All behavior stays default-off because the end-to-end payoff gate did not pass.
static constexpr uint32_t kG309CompositeToken = 0xfffffff7u;
static std::atomic<uint64_t> g_g309Candidates{0}, g_g309Composites{0},
    g_g309Binds{0}, g_g309FailMissing{0}, g_g309FailLayout{0},
    g_g309FailBackend{0}, g_g309FailStale{0}, g_g309VerifyComposites{0},
    g_g309VerifyPixels{0}, g_g309VerifyBad{0};
static bool g309Requested();
static bool g309On();
static bool g309VerifyOn();
static bool g309StatOn();
static void g309Report();
// G310: persistent logical-vs-physical framebuffer split. The private logical FBO carries the
// downstream 0x2720/tbw=8 atlas while physical FRAME/FBW views remain producer-owned.
// Default-on after exact, normal-presentation, and >=10% same-binary payoff gates passed.
static constexpr uint32_t kG310LogicalFbp = 0xfffffff5u;
static constexpr uint32_t kG310LogicalCt24Token = 0xfffffff4u;
struct G310PageState
{
    int8_t sourceTi = -1; // -1 guest VRAM; otherwise resident-target index
    uint64_t sourceGen = 0u;
    uint64_t guestGen = 0u;
    uint64_t guestHash = 0u;
};
static std::array<G310PageState, 128> g_g310Pages{};
static std::array<std::array<uint64_t, 128>, kG248TargetCount> g_g310TargetPageGen{};
static bool g_g310Initialized = false;
static bool g_g310Frozen = false;
static bool g_g310FrozenInvalid = false;
static bool g_g310RefreshPending = false;
static bool g_g310Verifying = false;
static std::atomic<uint64_t> g_g310Candidates{0}, g_g310Prepares{0},
    g_g310Binds{0}, g_g310Init{0}, g_g310Refresh{0},
    g_g310Invalidations{0}, g_g310CompositeJobs{0},
    g_g310RetireBatches{0}, g_g310RetireTargets{0},
    g_g310RetirePages{0}, g_g310RetireFail{0},
    g_g310GpuJobs{0}, g_g310GpuPages{0}, g_g310GuestJobs{0},
    g_g310GuestPages{0}, g_g310NoopPages{0}, g_g310AuthorityMoves{0},
    g_g310FailLayout{0}, g_g310FailBackend{0}, g_g310FailMissing{0},
    g_g310VerifyN{0}, g_g310VerifyPixels{0}, g_g310VerifyBad{0},
    g_g310PresentReads{0}, g_g310PresentOverlap{0}, g_g310PresentPages{0},
    g_g310PresentUnknown{0};
static std::atomic<uint64_t> g_g310ProducerN[kG248TargetCount]{};
static std::atomic<uint64_t> g_g310ProducerPages[kG248TargetCount]{};
static std::atomic<uint64_t> g_g310SourcePages[kG248TargetCount + 1u]{};
static std::atomic<uint64_t> g_g310MatN[8]{};
static std::atomic<uint64_t> g_g310MatPages[8]{};
static std::atomic<uint64_t> g_g310T8Candidates{0}, g_g310T8Views{0},
    g_g310T8BuildFail{0}, g_g310T8RejShape{0}, g_g310T8RejPage{0},
    g_g310T8RejClut{0};
// G326/G423: persistent cache for descriptor-derived frontend products. G423 promoted the existing
// exact cache after the current renderer made it worth ~12 ms/frame. The renderer's builders remain
// the cold path and verifier oracle; no authority or ordering state is cached here. Kill the whole
// cache with DC2_G423_NO_PLAN_CACHE=1.
static bool g326PlanCacheOn()
{
    static const bool v = !envFlagEnabled("DC2_G423_NO_PLAN_CACHE");
    return v;
}
static bool g326VerifyOn()
{
    static const bool v = envFlagEnabled("DC2_G326_VERIFY");
    return v;
}
static bool g326StatOn()
{
    static const bool v = envFlagEnabled("DC2_G326_STAT");
    return v;
}
static bool g326ForceColdOn()
{
    static const bool v = envFlagEnabled("DC2_G326_FORCE_COLD");
    return v;
}
static bool g326G264On()
{
    static const bool v = !envFlagEnabled("DC2_G326_NO_G264");
    return g326PlanCacheOn() && v;
}
static bool g326G281On()
{
    static const bool v = !envFlagEnabled("DC2_G326_NO_G281");
    return g326PlanCacheOn() && v;
}
static bool g326G310On()
{
    static const bool v = !envFlagEnabled("DC2_G326_NO_G310");
    return g326PlanCacheOn() && v;
}
static bool g326DiagOn() { return g326VerifyOn() || g326StatOn(); }
struct G326Counters
{
    uint64_t g264Hit = 0u, g264Miss = 0u, g264Bad = 0u;
    uint64_t g281Hit = 0u, g281Miss = 0u, g281Bad = 0u;
    uint64_t g310Hit = 0u, g310Miss = 0u, g310Bad = 0u;
    uint64_t reports = 0u;
};
static G326Counters g_g326{}; // all three products are owned by the single flush/GS worker
static void g326MaybeReport()
{
    if (!g326StatOn())
        return;
    const uint64_t work = g_g326.g264Hit + g_g326.g264Miss;
    if (work == 0u || (work % 128u) != 0u || g_g326.reports == work)
        return;
    g_g326.reports = work;
    std::fprintf(stderr,
                 "[G326:stat] g264=%llu/%llu bad=%llu g281=%llu/%llu bad=%llu "
                 "g310=%llu/%llu bad=%llu cold=%d\n",
                 static_cast<unsigned long long>(g_g326.g264Hit),
                 static_cast<unsigned long long>(g_g326.g264Miss),
                 static_cast<unsigned long long>(g_g326.g264Bad),
                 static_cast<unsigned long long>(g_g326.g281Hit),
                 static_cast<unsigned long long>(g_g326.g281Miss),
                 static_cast<unsigned long long>(g_g326.g281Bad),
                 static_cast<unsigned long long>(g_g326.g310Hit),
                 static_cast<unsigned long long>(g_g326.g310Miss),
                 static_cast<unsigned long long>(g_g326.g310Bad),
                 g326ForceColdOn() ? 1 : 0);
}
// G327 premise census: classify the two sides of one host-upload conflict contract before
// building the physical-byte overlay. Timers exist only under the explicit census flag; the
// default renderer pays one cached false branch at the existing upload/mirror boundaries.
static bool g327CensusOn()
{
    static const bool v = envFlagEnabled("DC2_G327_CENSUS");
    return v;
}
struct G327PsmCensus
{
    uint64_t hooks = 0u, overlapHooks = 0u;
    uint64_t mirrorHooks = 0u, fallbackHooks = 0u;
    uint64_t mirrorTargets = 0u, fallbackTargets = 0u;
    uint64_t noteNs = 0u, materializeNs = 0u;
    uint64_t transfers = 0u, chunks = 0u, bytes = 0u;
    uint64_t mirrorTransfers = 0u, fallbackTransfers = 0u, mixedTransfers = 0u;
};
struct G327TransferWatch
{
    bool active = false;
    uint32_t dbp = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint8_t dbw = 0u, dpsm = 0u;
    uint32_t mirrorMask = 0u, fallbackMask = 0u;
    uint64_t chunks = 0u, bytes = 0u;
};
static std::array<G327PsmCensus, 64> g_g327Psm{};
static G327TransferWatch g_g327Watch{};
static uint64_t g_g327MirrorFlushes = 0u, g_g327MirrorFlushNs = 0u;
static uint64_t g_g327WatchAborts = 0u, g_g327LastReport = 0u;
static uint32_t g327BitCount(uint32_t v)
{
    uint32_t n = 0u;
    while (v != 0u)
    {
        v &= v - 1u;
        ++n;
    }
    return n;
}
static bool g327WatchMatches(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                             uint32_t dsax, uint32_t dsay,
                             uint32_t rrw, uint32_t rrh)
{
    return g_g327Watch.active && g_g327Watch.dbp == dbp && g_g327Watch.dbw == dbw &&
           g_g327Watch.dpsm == dpsm && g_g327Watch.dsax == dsax &&
           g_g327Watch.dsay == dsay && g_g327Watch.rrw == rrw && g_g327Watch.rrh == rrh;
}
static void g327MaybeReport()
{
    if (!g327CensusOn())
        return;
    uint64_t overlap = 0u;
    for (const G327PsmCensus &p : g_g327Psm)
        overlap += p.overlapHooks;
    if (overlap == 0u || (overlap % 1024u) != 0u || overlap == g_g327LastReport)
        return;
    g_g327LastReport = overlap;
    std::fprintf(stderr,
                 "[G327:total] overlap=%llu mirrorFlush=%llu/%.3fms watchAbort=%llu\n",
                 static_cast<unsigned long long>(overlap),
                 static_cast<unsigned long long>(g_g327MirrorFlushes),
                 static_cast<double>(g_g327MirrorFlushNs) / 1000000.0,
                 static_cast<unsigned long long>(g_g327WatchAborts));
    for (uint32_t psm = 0u; psm < g_g327Psm.size(); ++psm)
    {
        const G327PsmCensus &p = g_g327Psm[psm];
        if (p.overlapHooks == 0u)
            continue;
        std::fprintf(stderr,
                     "[G327:psm] psm=0x%02x hook=%llu overlap=%llu mirror=%llu/%llu "
                     "fallback=%llu/%llu note=%.3fms mat=%.3fms "
                     "xfer=%llu chunks=%llu bytes=%llu kind=%llu/%llu/%llu\n",
                     psm, static_cast<unsigned long long>(p.hooks),
                     static_cast<unsigned long long>(p.overlapHooks),
                     static_cast<unsigned long long>(p.mirrorHooks),
                     static_cast<unsigned long long>(p.mirrorTargets),
                     static_cast<unsigned long long>(p.fallbackHooks),
                     static_cast<unsigned long long>(p.fallbackTargets),
                     static_cast<double>(p.noteNs) / 1000000.0,
                     static_cast<double>(p.materializeNs) / 1000000.0,
                     static_cast<unsigned long long>(p.transfers),
                     static_cast<unsigned long long>(p.chunks),
                     static_cast<unsigned long long>(p.bytes),
                     static_cast<unsigned long long>(p.mirrorTransfers),
                     static_cast<unsigned long long>(p.fallbackTransfers),
                     static_cast<unsigned long long>(p.mixedTransfers));
    }
}
static void g327RecordUpload(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                             uint32_t dsax, uint32_t dsay,
                             uint32_t rrw, uint32_t rrh,
                             uint32_t overlapMask, uint32_t mirrorMask,
                             uint64_t noteNs, uint64_t materializeNs)
{
    if (!g327CensusOn())
        return;
    G327PsmCensus &p = g_g327Psm[dpsm & 0x3fu];
    ++p.hooks;
    if (overlapMask == 0u)
        return;
    const uint32_t fallbackMask = overlapMask & ~mirrorMask;
    ++p.overlapHooks;
    p.noteNs += noteNs;
    p.materializeNs += materializeNs;
    if (mirrorMask != 0u)
    {
        ++p.mirrorHooks;
        p.mirrorTargets += g327BitCount(mirrorMask);
    }
    if (fallbackMask != 0u)
    {
        ++p.fallbackHooks;
        p.fallbackTargets += g327BitCount(fallbackMask);
    }
    if (g_g327Watch.active && !g327WatchMatches(dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
    {
        ++g_g327WatchAborts;
        g_g327Watch = G327TransferWatch{};
    }
    if (!g_g327Watch.active)
    {
        g_g327Watch.active = true;
        g_g327Watch.dbp = dbp;
        g_g327Watch.dbw = dbw;
        g_g327Watch.dpsm = dpsm;
        g_g327Watch.dsax = dsax;
        g_g327Watch.dsay = dsay;
        g_g327Watch.rrw = rrw;
        g_g327Watch.rrh = rrh;
        g_g327Watch.mirrorMask = mirrorMask;
        g_g327Watch.fallbackMask = fallbackMask;
        static uint32_t s_logs = 0u;
        if (s_logs++ < 48u)
            std::fprintf(stderr,
                         "[G327:edge] n=%u dbp=0x%x dbw=%u psm=0x%x xy=%u,%u "
                         "size=%ux%u overlap=0x%x mirror=0x%x fallback=0x%x\n",
                         s_logs, dbp, static_cast<unsigned>(dbw),
                         static_cast<unsigned>(dpsm), dsax, dsay, rrw, rrh,
                         overlapMask, mirrorMask, fallbackMask);
    }
    else
    {
        g_g327Watch.mirrorMask |= mirrorMask;
        g_g327Watch.fallbackMask |= fallbackMask;
    }
    g327MaybeReport();
}
// Cross-TU completion bridge from GS::processImageData. It records only transfers armed by an
// actual dirty-resident overlap, and never retains or copies IMAGE bytes.
void g327NoteImageChunk(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                        uint32_t dsax, uint32_t dsay,
                        uint32_t rrw, uint32_t rrh,
                        uint32_t sizeBytes, bool complete)
{
    if (!g327CensusOn() || !g327WatchMatches(dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
        return;
    ++g_g327Watch.chunks;
    g_g327Watch.bytes += sizeBytes;
    if (!complete)
        return;
    G327PsmCensus &p = g_g327Psm[dpsm & 0x3fu];
    ++p.transfers;
    p.chunks += g_g327Watch.chunks;
    p.bytes += g_g327Watch.bytes;
    if (g_g327Watch.mirrorMask != 0u && g_g327Watch.fallbackMask != 0u)
        ++p.mixedTransfers;
    else if (g_g327Watch.mirrorMask != 0u)
        ++p.mirrorTransfers;
    else
        ++p.fallbackTransfers;
    g_g327Watch = G327TransferWatch{};
}
struct G327MirrorTimer
{
    G327MirrorTimer() : start(std::chrono::steady_clock::now()) {}
    ~G327MirrorTimer()
    {
        ++g_g327MirrorFlushes;
        g_g327MirrorFlushNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
    }
    std::chrono::steady_clock::time_point start;
};
// G324 diagnostics: split G310 producer preparation into CPU staging versus synchronous backend
// wall time, then measure how much ordinary front-end work exists before the first external
// observer. The active pointer is scoped to one g178TryFlushGpu call; all gates are cached and the
// default path executes no clocks or counter updates.
static bool g324CensusOn()
{
    static const bool v = envFlagEnabled("DC2_G324_CENSUS");
    return v;
}
static bool g324EmptyOn()
{
    static const bool v = !g324CensusOn() && envFlagEnabled("DC2_G324_EMPTY");
    return v;
}
static bool g324Armed() { return g324CensusOn() || g324EmptyOn(); }
static uint64_t g324ElapsedNs(const std::chrono::steady_clock::time_point &a,
                              const std::chrono::steady_clock::time_point &b)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}
struct G324FlushDiag
{
    uint64_t desiredCpuNs = 0u;
    uint64_t logicalCpuNs = 0u;
    uint64_t logicalBackendWallNs = 0u;
    uint64_t t8CpuNs = 0u;
    uint64_t t8BackendWallNs = 0u;
    uint64_t tailPrepNs = 0u;
    uint64_t submitNs = 0u;
    uint64_t logicalJobs = 0u;
    uint64_t t8Jobs = 0u;
    bool hadProducer = false;
    std::chrono::steady_clock::time_point lastProducerDone{};
};
static thread_local G324FlushDiag *g_g324ActiveDiag = nullptr;
struct G324DiagScope
{
    explicit G324DiagScope(G324FlushDiag *diag) : previous(g_g324ActiveDiag)
    {
        g_g324ActiveDiag = diag;
    }
    ~G324DiagScope() { g_g324ActiveDiag = previous; }
    G324FlushDiag *previous = nullptr;
};
static bool g_g324ObserverPending = false;
static std::chrono::steady_clock::time_point g_g324ObserverReady{};
static uint64_t g_g324ObserverNs[3]{};
static uint64_t g_g324ObserverN[3]{};
static void g324NoteObserver(unsigned kind)
{
    if (!g324Armed() || !g_g324ObserverPending)
        return;
    const auto now = std::chrono::steady_clock::now();
    g_g324ObserverPending = false;
    if (g324CensusOn())
    {
        kind = std::min(kind, 2u);
        g_g324ObserverNs[kind] += g324ElapsedNs(g_g324ObserverReady, now);
        ++g_g324ObserverN[kind];
    }
}
static void g324ArmObserver()
{
    if (!g324Armed())
        return;
    g_g324ObserverReady = std::chrono::steady_clock::now();
    g_g324ObserverPending = true;
}
// G311: batched page-delta maintenance of the G310 logical atlas. The full G309 compositor is
// retained as fail-closed recovery + exact oracle; when enabled, producer transitions mark the
// snapshot page-granular stale instead of de-initializing it, and the next display consumer
// refreshes only changed 64x32 pages through the pre-built g310ApplyDesired incremental path
// (GPU-to-GPU copies for resident winners, guest uploads for changed guest winners) rather than a
// full 512x512 recomposition. Page state commits only after the backend update succeeds.
static bool g_g310Stale = false;
static bool g_g311InOracle = false; // suppress census timing/histogram during the verify rebuild
static std::atomic<uint64_t> g_g311Refreshes{0}, g_g311RefreshWork{0},
    g_g311RefreshNoop{0}, g_g311Stales{0}, g_g311CompositeN{0},
    g_g311CompositeUs{0}, g_g311IncrementalN{0}, g_g311IncrementalUs{0},
    g_g311VerifyN{0}, g_g311VerifyPixels{0}, g_g311VerifyBad{0},
    g_g311VerifyMiss{0};
static std::atomic<uint64_t> g_g311DeltaHist[8]{};
static bool g310Requested();
static bool g310On();
static bool g310VerifyOn();
static bool g310StatOn();
static bool g310AliasRetireOn();
static bool g310T8ViewOn();
static bool g310EnsureCurrent();
static bool g310RetireSeededAliases();
static void g310NoteProducerWrite(int ti, int rowLo, int rowHi);
static void g310NoteTargetClean(bool publicationOk);
static void g310Invalidate(const char *stage, bool backendFailure);
static void g310BeginFreeze();
static void g310EndFreeze(bool submitted);
static bool g310FrozenUsable();
static void g310Report();
static bool g311On();
static bool g311CensusOn();
static bool g311VerifyOn();
static void g311Report();
// G598: defined in rasterizer_t8_map_resolver.inc, which is included AFTER
// rasterizer_page_source_maps.inc where g310Report() calls it.
static void g598T8Report();
struct G310BatchFreeze
{
    bool active = false;
    void begin()
    {
        if (!active)
        {
            g310BeginFreeze();
            active = true;
        }
    }
    void complete()
    {
        if (active)
        {
            g310EndFreeze(true);
            active = false;
        }
    }
    ~G310BatchFreeze()
    {
        if (active)
            g310EndFreeze(false);
    }
};
static bool g280AliasOn();
static bool g281ViewOn();
static bool g282TightRowsOn();
static bool g294OwnerTokenRequested(); // G294 combined-arm request (defined near g280 bodies)
static bool g294OwnerTokenOn();         // G294 owner-token active (rides g280 alias-view)
static bool g280PrepareMaterialize(int ti); // restore+drop overlays before dst publication
static void g280DropOverlays(int ti, uint64_t *dropCounter = nullptr);
static void g280NoteFboContentChange(int ti, int rowLo, int rowHi);
static void g280NoteMirrorPatch(int ti, int lo, int hi);
static void g280HygienePass();
static void g280Report();
// Why RTT batches fall to the CPU under waves (the residency-killing consumers): entry classify
// rejects vs the color-only guest-depth fail-close. Pins the follow-up phase's target.
static std::atomic<uint64_t> g_g261RttClassifyRej{0}, g_g261RttDepthRej{0};
// G262 widened-wave telemetry: waves carrying per-wave guest depth, and how many of those
// actually paid the immediate Z readback (write-enabled batches only).
static std::atomic<uint64_t> g_g262DepthWaves{0}, g_g262ZReadbacks{0};
// G274 measurement: wall-time spent in the per-wave Z round-trip swizzle loops (upload:
// ReadZ24->float; readback: backend glReadPixels + WriteZ24). Diagnostics only, cached gate.
static std::atomic<uint64_t> g_g274ZUpNs{0}, g_g274ZRbNs{0};
// G274 residency-ceiling census: was the previous same-key Z readback->VRAM->reupload redundant
// (no intervening VRAM-Z writer)? If the whole-target Z gen is unchanged since our last writeback,
// nobody consumed VRAM Z between waves and a resident GPU depth would have elided the round-trip.
static std::unordered_map<uint64_t, uint64_t> g_g274LastZGen; // depthKey -> gen after last writeback
static std::atomic<uint64_t> g_g274ZRedundant{0}, g_g274ZNecessary{0};
// G278: within-drain Z-readback coalescing. The backend depth texture is authoritative only until
// the next proven VRAM-Z consumer (or the drain/presentation boundary); it never crosses a frame.
// The pending union stores enough layout identity to publish without consulting mutable GS state.
struct G278PendDepth
{
    bool active = false;
    bool display = false;
    // G411: this owner materializes into G405's private shared-Z mirror, never aliased GS VRAM.
    bool privateMirror = false;
    uint64_t key = 0u;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
    uint32_t zbpPage = 0u, fbw = 0u;
    int fbW = 0;
    int rowLo = 512, rowHi = -1;
    std::array<uint64_t, 8> staleRows{}; // CPU-written rows outside dirty union; backend is stale
};
// G337 (Pillar-4 slice 1, default-off DC2_G337_ZAUTH=1): persistent cross-wave/cross-frame GPU
// depth authority. The G278 pending owner generalizes to a small per-depthKey slot table and is
// allowed to (a) cover alias-family waves that skip their color readback (no VRAM color write can
// race the deferred Z), and (b) survive the frame boundary (G336 proved the boundary Z publish is
// WAR-dead: the next frame's Z-clear overwrites the published pages in ~0.04 ms). Guest VRAM Z is
// materialized lazily at the existing fail-closed consumer edges only. Arm off = slot 0 only,
// byte-identical legacy behavior.
static constexpr int kG278PendSlots = 6;
static G278PendDepth s_g278PendTab[kG278PendSlots]; // flush-owner thread only (as s_g276Pend)
static G278PendDepth &s_g278Pend = s_g278PendTab[0]; // legacy single-slot alias (arm-off path)
static std::atomic<uint64_t> g_g278Deferred{0}, g_g278Flushes{0}, g_g278Saved{0},
    g_g278FlushRows{0}, g_g278UploadSkips{0}, g_g278FlushFail{0},
    g_g278InvariantFail{0}, g_g278StaleMarks{0}, g_g278Reinits{0};
static std::atomic<uint64_t> g_g411DepthBatches{0}, g_g411DepthUploadRows{0},
    g_g411DepthPublishes{0}, g_g411DepthPublishRows{0};
static std::atomic<uint64_t> g_g278FlushByCause[9] = {};
// G337 telemetry: boundary carries, cross-key slot evictions, cross-slot physical-overlap
// flushes, widened (alias-family) deferrals, audit-forced publications by class.
static std::atomic<uint64_t> g_g337Carried{0}, g_g337SlotEvict{0}, g_g337OvlFlush{0},
    g_g337WideDefer{0}, g_g337AuditTex{0}, g_g337AuditTarget{0}, g_g337AuditCrossKey{0};
// Why a Z-writing wave still took the DIRECT per-wave readback under the arm (attribution).
static std::atomic<uint64_t> g_g337DirSeeded{0}, g_g337DirTight{0}, g_g337DirNoSkipRb{0},
    g_g337DirOther{0};
// G274: total wall-time inside the synchronous backend submit (all waves) + count.
static std::atomic<uint64_t> g_g274SubNs{0}, g_g274SubN{0};
// G275: one synchronous worker job now owns the render and its mandatory immediate Z readback.
// The CPU swizzle/publication remains before this flush returns; only the redundant queue/future
// boundary and scratch-FBO reattachment are removed.
static std::atomic<uint64_t> g_g275FusedJobs{0}, g_g275ReadWaitsElided{0};
static std::atomic<uint64_t> g_g275FusedNs{0};
// G276 measure-first probe (default-off, DC2_G276_PROBE=1): split the synchronous submit time by
// batch category and measure the color-readback window span, to decide whether the full-FBO color
// glReadPixels (renderBatch reads 0..fbH but only rowLo..rowHi is swizzled back) is a real cost.
// Buckets indexed [skipReadback][guestDepthWrites].
static std::atomic<uint64_t> g_g276SubNs[2][2] = {};
static std::atomic<uint64_t> g_g276SubN[2][2] = {};
static std::atomic<uint64_t> g_g276ColorRbN{0}, g_g276ColorRbWinRows{0}, g_g276ColorRbFullRows{0};
static bool g276ProbeOn()
{
    static const bool s_on = envFlagEnabled("DC2_G276_PROBE");
    return s_on;
}
// G276 within-drain display-color readback coalescing lever + pending state + counters (defined
// here so g261Report can print them; the flush body is defined after g261Materialize).
static bool g276DisplayRbOn()
{
    // G276 PROMOTION (2026-07-17): DEFAULT-ON. Coalesces consecutive same-target display-color
    // readbacks within a drain (the persistent FBO accumulates; the gen-snapshot already suppresses
    // re-upload for a same-target continuation, and the deferred readback bumps no color gens), so a
    // K-batch same-target run pays ONE full-FBO glReadPixels instead of K. VRAM is authoritative at
    // exactly today's boundaries (each g272MaterializeAll consumer edge + drain end/present + any
    // different-target flush), so the cross-frame G242/G249/G272 ownership trap cannot occur.
    // Rides the native wave stack (g261WaveOn already declines under every exact oracle, the
    // G242/G256 ownership arms, the tile-bin/GPU kills, and the VRAM-dump diagnostics). Kill:
    // DC2_G276_NO_DISPLAY_RB_COALESCE=1 (or DC2_G276_DISPLAY_RB_COALESCE=0); the whole-stack kill
    // DC2_G26X_NO_NATIVE=1 also disables it.
    static const bool s_on = !envFlagEnabled("DC2_G276_NO_DISPLAY_RB_COALESCE") &&
                             !envFlagDisabled("DC2_G276_DISPLAY_RB_COALESCE");
    return s_on && g261WaveOn();
}
// G284 experiment: extends the G276 within-drain display-color readback
// coalescing ACROSS the ranged transfer edges (cause 7 host->local upload dst, cause 8 local->local
// src+dst) when the transfer provably misses the pending display rows. The upload edge is NOT a
// display-color consumer; before G284 it force-flushed the pending display readback unconditionally,
// so every streaming-texture upload (~3.9 MB/frame on MAP-0) paid a full-FBO display glReadPixels.
// The DEPTH readback already skips this exact edge when disjoint (g278FlushPendingDepthForRanges);
// G284 is the display twin. Bit-exact by the same disjointness proof G273/G278 use: if the transfer
// dst/src does not overlap the pending display rows, publishing the display readback now vs. at the
// next real consumer edge (drain-end / present / different-target / CPU-replay / inline / an
// OVERLAPPING l2l display->work-page copy) yields identical guest VRAM. Fail-closed: unknown ranges,
// any overlap, or degenerate pending take the original unconditional flush. Rides G276 (so it
// auto-declines under every oracle/kill G276 already declines under, incl. DC2_G26X_NO_NATIVE=1).
// G289 PROMOTION (2026-07-18): DEFAULT-ON only as an atomic pair with G289 ownership. G284 alone
// lost MAP-2's left cliff because its range-local proof did not cover the later physical-alias
// consumer. G289 now owns every skipped display->work copy or fails closed to publication, so
// either kill disables both halves and restores the G287-safe baseline.
static bool g289PolicyOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G289_NO_GPU_LOCAL_COPY") &&
        !envFlagDisabled("DC2_G289_GPU_LOCAL_COPY");
    return s_on;
}
static bool g284UploadCoalesceOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G284_NO_UPLOAD_COALESCE") &&
        !envFlagDisabled("DC2_G284_UPLOAD_COALESCE");
    return s_on && g289PolicyOn() && g276DisplayRbOn();
}
static bool g284StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G284_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g284UploadSkipped{0}, g_g284UploadFlushed{0};
// G289: one versioned GPU-local-copy owner for the measured display CT32 -> work-page transfers.
// It is deliberately coupled to G284 because the source must still be authoritative in the
// display FBO. Default-on after exact oracle, hardware-reference route matrix, and timing gates.
static bool g289LocalCopyOn()
{
    return g289PolicyOn() && g284UploadCoalesceOn();
}
static bool g289VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_VERIFY");
    return s_on && g289LocalCopyOn();
}
static bool g289StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_STAT") || g289VerifyOn();
    return s_on;
}
static bool g289RawVramDiagOn()
{
    static const bool s_on =
        envFlagEnabled("DC2_DUMP_VRAMRGN") ||
        envFlagEnabled("DC2_G37_RTTDUMP") ||
        envFlagEnabled("DC2_G38_VRAMDUMP") ||
        envFlagEnabled("DC2_G86_TEXDUMP") ||
        envFlagEnabled("DC2_G194_VRAM_DUMP") ||
        envFlagEnabled("DC2_G231_GEMDUMP") ||
        envFlagEnabled("DC2_G232_RTTDUMP") ||
        // G399: the Dungeon 6 surface probe reads raw VRAM, so it needs the same
        // "keep VRAM authoritative" treatment as the older dump levers.
        envFlagEnabled("DC2_G399_SURFDUMP");
    return s_on;
}
static constexpr uint32_t kG289ZeroFbpToken = 0xfffffffdu;
static constexpr uint32_t kG289AliasCt32Token = 0xfffffffcu;
static constexpr uint32_t kG289AliasCt24Token = 0xfffffffau;
static constexpr uint32_t kG289WorkCt24Token = 0xfffffff9u;
static constexpr uint32_t kG289WorkCt32Token = 0xfffffff8u;
struct G289CopyOwner
{
    bool active = false;
    uint64_t version = 0u;
    uint32_t srcFbp = 0u;
    uint32_t dstFbp = 0u;
    uint32_t firstOwnedBlock = 0u;
    uint32_t lastOwnedBlock = 0u;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
};
// G346: N-slot versioned owner table. Slot 0 is the legacy single owner (every pre-G346 site
// keeps its exact semantics through the s_g289Owner reference); slots 1..N-1 are populated ONLY
// under DC2_G345_CLOSURE (g345ClosureOn), so the default path is byte-identical single-slot.
static constexpr uint32_t kG345OwnerSlots = 4u;
static G289CopyOwner s_g289Owners[kG345OwnerSlots];
static G289CopyOwner &s_g289Owner = s_g289Owners[0];
static bool g345AnyOwnerActive()
{
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
        if (s_g289Owners[i].active)
            return true;
    return false;
}
// Active owner for a destination fbp (slot 0 first). Closure-off this degenerates to slot 0.
static G289CopyOwner *g345OwnerFor(uint32_t dstFbp)
{
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
        if (s_g289Owners[i].active && s_g289Owners[i].dstFbp == dstFbp)
            return &s_g289Owners[i];
    return nullptr;
}
static G289CopyOwner *g345FreeSlot()
{
    if (!s_g289Owners[0].active)
        return &s_g289Owners[0];
    if (!g345ClosureOn())
        return nullptr; // legacy single-slot admission
    for (uint32_t i = 1u; i < kG345OwnerSlots; ++i)
        if (!s_g289Owners[i].active)
            return &s_g289Owners[i];
    return nullptr;
}
struct G289PendingUploadOverwrite
{
    bool active = false;
    uint64_t ownerVersion = 0u;
    uint32_t dbp = 0u;
    uint8_t dbw = 0u;
    uint8_t dpsm = 0u;
    uint32_t dsax = 0u;
    uint32_t dsay = 0u;
    uint32_t rrw = 0u;
    uint32_t rrh = 0u;
    uint32_t replacedThroughBlock = 0u;
};
static G289PendingUploadOverwrite s_g289PendingUpload;
static std::atomic<uint64_t> g_g289Direct{0}, g_g289Copies{0}, g_g289CopyFail{0},
    g_g289Consumes{0}, g_g289Carries{0}, g_g289OwnerViews{0}, g_g289OwnerViewFail{0},
    g_g289OwnerBinds{0}, g_g289Materializes{0}, g_g289MatFail{0},
    g_g289UploadOverwrites{0}, g_g289SuffixPublishes{0},
    g_g289InlineCarries{0}, g_g289FrameCarries{0},
    g_g289VerifyChecks{0}, g_g289VerifyBad{0},
    g_g289Unexpected{0};
// G288 bring-up: G284 can leave display color newer in the persistent FBO while guest VRAM stays
// stale across a disjoint upload. CPU replay already publishes at cause 3 before reading VRAM, but
// a later GPU batch can read target/Z/texture/CLUT bytes directly without crossing any G276 edge.
// Audit that whole batch before its first VRAM read and publish only when its physical block ranges
// overlap the pending display owner (unknown ranges fail closed). Opt-in until MAP-2 reference
// composition and the G284 payoff both pass.
static bool g288GpuConsumerOn()
{
    static const bool s_on = envFlagEnabled("DC2_G288_GPU_CONSUMER") &&
                             !envFlagEnabled("DC2_G288_NO_GPU_CONSUMER");
    return s_on && g284UploadCoalesceOn();
}
static bool g288StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G288_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g288GpuAudits{0}, g_g288GpuPublishes{0},
    g_g288GpuDisjoint{0}, g_g288GpuUnknown{0};
// (G276PendDisplay struct + s_g276Pend moved above g178BumpRectImpl for the G277 mover probe.)
static std::atomic<uint64_t> g_g276Deferred{0}, g_g276Flushes{0}, g_g276FlushRows{0},
    g_g276FlushFail{0};
// G277-R repair counters (MAP-0 Max body flicker, user-reported on the G276 default):
// publish-before-upload fail-safe fires + exact-window adoptions.
static std::atomic<uint64_t> g_g277UploadPublish{0}, g_g277ExactGenWin{0};
// G277-R mover probe (default-off, DC2_G277_GENMOVE=1): per-32-row-chunk gen sums for fbp 0x68
// captured at snapshot anchor, compared at each publish-before-upload event to pin WHICH rows'
// pages a mystery writer bumped between same-target deferred batches.
static bool g277GenMoveOn()
{
    static const bool s_on = envFlagEnabled("DC2_G277_GENMOVE");
    return s_on;
}
static uint64_t s_g277Chunk[13] = {};   // rows 32i..32i+31, i=0..12 (rows 0..415)
static bool s_g277ChunkValid = false;   // flush-owner thread only
static bool g277ExactDisplayGenOn()
{
    // G277-R (2026-07-17): the display fb snapshot's FULL-HEIGHT CT32 gen window for fbp 0x68
    // (pages 104..231) overlaps zbp 0xd0's Z pages (208..231), so every per-wave Z readback
    // writeback bump spuriously invalidated the snapshot. Pre-G276 that only re-uploaded identical
    // bytes; with G276's deferred readback it re-uploaded STALE VRAM over unpublished FBO rows and
    // erased Max's body (dense MAP-0: 484/1009 frames body-absent). G273 proved color rows 0..415
    // occupy pages 104..207, disjoint from Z pages 208+; the snapshot window now uses that exact
    // row bound for the measured tuple whenever all coverage stays <=415 (anything taller falls
    // back to the conservative full height). A/B kill: DC2_G277_NO_EXACT_DISPLAY_GEN=1.
    static const bool s_on = !envFlagEnabled("DC2_G277_NO_EXACT_DISPLAY_GEN");
    return s_on;
}
// G277 measure-first census (default-off, DC2_G277_CENSUS=1): sizes range-exact consumer edges.
// Hypothesis: most G276 pending-display flushes are PHANTOM consumers — fully-scissored offscreen
// inline prims (the sceGsSwapDBuff sync linestrips draw nothing) and range-skipped l2h upload
// edges whose destination never touches the pending display rows — and the same exactness would
// make within-drain Z-readback coalescing viable (G274's 44% redundancy counted writers only;
// reads don't bump gens, so the READ-side consumer census is this one). Counters only, no
// behavior change; all runs on the flush-owner thread (same single-thread contract as s_g276Pend).
static bool g277CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G277_CENSUS");
    return s_on;
}
static bool g278DepthRbOn()
{
    // G278 PROMOTION CANDIDATE (2026-07-17): default-on while riding the already-oracle-gated
    // native wave stack. The G277 census remains behavior-pure by disabling this lever whenever
    // it is requested. Kill: DC2_G278_NO_Z_RB_COALESCE=1 (or DC2_G278_Z_RB_COALESCE=0).
    static const bool s_on = !envFlagEnabled("DC2_G278_NO_Z_RB_COALESCE") &&
                             !envFlagDisabled("DC2_G278_Z_RB_COALESCE");
    return s_on && g261WaveOn() && !g277CensusOn();
}
// G535 PROMOTION (2026-08-09): DEFAULT-ON. Publishes a pending G411 private-mirror depth owner at
// the CPU band replay's semantic consumer edge (g528PublishScoped) instead of refusing the deferral
// outright. That replay reads AND writes the mirror through g403DisplayZRead/g403DisplayZWrite, so
// it is a consumer — but every ranged publication path skipped privateMirror owners because they
// carry no VRAM transfer range. The INLINE edge (g278FlushPendingDepthForInline) already published
// them for exactly this reason; the batched replay is the same consumer one level up, and it was
// the last one left uncovered.
//
// Measured on lean MAP-0, same binary, 8 windows per arm:
//   pre-G535 (broken)          58.74 ms/f   24.8% of map_4 frames defective
//   this arm  (correct)        60.80 ms/f   +3.51%, 0/671 defective, 0 alternation events
//   in-wave publish (correct)  84.12 ms/f   +42.9%  — the conservative fallback below
// Rollback to the conservative form: DC2_G535_NO_EDGEPUB=1 (private-mirror owners never defer, so
// they publish in-wave). DC2_G535_PRIVZ_DEFER=1 restores the pre-G535 defect for diagnosis and
// therefore also disables this edge publication.
static bool g535EdgePubOn()
{
    static const bool s_on = !envFlagEnabled("DC2_G535_NO_EDGEPUB") &&
                             !envFlagDisabled("DC2_G535_EDGEPUB") &&
                             !envFlagEnabled("DC2_G535_PRIVZ_DEFER");
    return s_on;
}
// G337 persistent depth authority. G474 PROMOTION (2026-08-02): default-on after the current
// uncapped binary repriced the frame-boundary publication at 0.74-1.14 ms/f, with zero owner
// failures/evictions/overlap flushes and the affected route matrix clean. Rides G278, so every
// oracle/census/kill that disables G278 disables this too. Roll back with
// DC2_G337_NO_ZAUTH=1 (or DC2_G337_ZAUTH=0).
static bool g337ZAuthOn()
{
    static const bool s_on = !envFlagEnabled("DC2_G337_NO_ZAUTH") &&
                             !envFlagDisabled("DC2_G337_ZAUTH");
    return s_on && g278DepthRbOn();
}
// G338: retire the CT24 display-composite consumer that re-reads a resident CT32 producer FBO at
// the SAME base/width by binding a resident CT24-alpha view (RGB copied from the FBO, alpha
// re-derived per-texel from TEXA) instead of materializing the producer. The view is byte-identical
// to the legacy CT24->RGBA8 decode, so the consumer's own GL_LINEAR matches exactly. Rides the
// native wave stack. Verify with DC2_G338_VERIFY.
// G352 PROMOTION (2026-07-23): DEFAULT-ON — fork option 2, the accept-floor close of
// arc-total-closure (the ≥10% closure stack is unreachable: both bounded 0x139 composition forms
// are G304 local minima, G349/G351). Standalone bit-exact (DC2_G338_VERIFY bad=0), tex-alias
// 448->0, GS worker -3.8 ms/f, frame -2.6% (VU1-capped), G339 route-matrix PASS.
// Rollback: DC2_G338_NO_CT24_VIEW=1 (or DC2_G338_CT24_VIEW=0, or the whole-stack
// DC2_G26X_NO_NATIVE=1 via g261WaveOn) restores the legacy CPU CT24 display composite.
// DC2_G338_CT24_VIEW=1 stays accepted (now redundant).
static bool g338Ct24ViewOn()
{
    static const bool s_on = !envFlagEnabled("DC2_G338_NO_CT24_VIEW") &&
                             !envFlagDisabled("DC2_G338_CT24_VIEW");
    return s_on && g261WaveOn();
}
static bool g338VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G338_VERIFY");
    return s_on && g338Ct24ViewOn();
}
static uint64_t g338ViewKey(uint64_t base)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(base);
    mix(0x4732333843543234ull); // "G338CT24": distinct from T8 views and guest-VRAM textures
    return h | (1ull << 63);
}
static std::atomic<uint64_t> g_g338Views{0}, g_g338BuildFail{0}, g_g338VerifyBad{0};
static G144BlockRange g278PendingDepthWholeRangeP(const G278PendDepth &p);
static G144BlockRange g278PendingDepthRangeP(const G278PendDepth &p);
static bool g278FlushPendSlot(G278PendDepth &p, int cause);
static bool g278FlushPendingDepth(int cause);
static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown);
static std::atomic<uint64_t> g_g277FlushByCause[9] = {};          // g276 flushes that had pending
static std::atomic<uint64_t> g_g277EdgeCalls[9] = {}, g_g277EdgeSkippable[9] = {};
static std::atomic<uint64_t> g_g277InlEmpty{0}, g_g277InlNoOvl{0}, g_g277InlOvl{0}, g_g277InlZ{0};
static std::atomic<uint64_t> g_g277ZRb{0}, g_g277ZRbCoalescable{0};
static bool s_g277ZRunLive = false;   // flush-owner thread only
static uint64_t s_g277ZRunKey = 0u;
static G144BlockRange s_g277ZRunRange; // whole-target block range of the live run's Z key
static bool g274ZTimeOn()
{
    static const bool s_on = envFlagEnabled("DC2_G274_ZTIME");
    return s_on;
}
// G274 behavior lever (default-off): skip the redundant per-wave Z RE-UPLOAD when the persistent
// backend depth texture for this key is provably current (whole-target Z gen unchanged since our
// last writeback => resident GPU depth == VRAM Z, bit-exact by construction). VRAM stays
// authoritative (the readback still runs every write-wave), so there is NO deferred-ownership
// mechanism and the G242/G249 trap cannot occur. Rides the native stack; auto-off under the
// oracles that require the per-flush readback contract.
static std::atomic<uint64_t> g_g274ZUploadsSkipped{0};
static bool g274ZResidentOn()
{
    // G274 PROMOTION (2026-07-17): DEFAULT-ON. Elides the redundant per-wave Z RE-UPLOAD when the
    // whole-target Z gen is unchanged since our last writeback (resident backend depth == VRAM Z,
    // bit-exact). VRAM stays authoritative (the readback still runs every write-wave), so there is
    // NO deferred-ownership mechanism and the G242/G249 trap cannot occur. Requires the native
    // wave stack (g261WaveOn already declines under every exact oracle, the G242/G256 ownership
    // arms, the tile-bin/GPU kills, and the VRAM-dump diagnostics). Kill: DC2_G274_NO_ZRESIDENT=1
    // (or DC2_G274_ZRESIDENT=0); the whole-stack kill DC2_G26X_NO_NATIVE=1 also disables it.
    static const bool s_on = !envFlagEnabled("DC2_G274_NO_ZRESIDENT") &&
                             !envFlagDisabled("DC2_G274_ZRESIDENT");
    return s_on && g261WaveOn();
}

static bool g275FusedZReadOn()
{
    static const bool s_on = g261WaveOn() && envFlagEnabled("DC2_G275_FUSED_ZREAD");
    return s_on;
}

static bool g275StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G275_STAT");
    return s_on;
}

static int g275ZSwizzleLanes()
{
    static const int s_lanes = [] {
        // G275 PROMOTION (2026-07-17): DEFAULT 8. PSMZ24 is a one-to-one mapping from logical
        // pixels to independent 32-bit VRAM words, so disjoint guest-row lanes can pack/unpack in
        // parallel without changing RMW semantics or ownership timing. The existing persistent
        // row pool is joined before upload/submission or generation publication proceeds.
        if (envFlagEnabled("DC2_G275_NO_PAR_ZSWIZZLE"))
            return 1;
        const char *value = std::getenv("DC2_G275_ZSWIZZLE_LANES");
        if (value == nullptr)
            return 8;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    return g261WaveOn() ? s_lanes : 1;
}
// Per-key "backend depth texture has been fully uploaded once" (backend contract: a fresh
// texture's first upload must cover the whole target). Committed only after a successful submit.
static std::unordered_set<uint64_t> g_g262DepthInit;
static std::atomic<uint64_t> g_g261Mat[8] = {};
static std::atomic<uint64_t> g_g261MatByTarget[kG248TargetCount] = {};
// G279 diagnosis (default-off, DC2_G279_PROFILE=1): wall-time split for the remaining
// GPU-residency publication paths. Counts/rows are bucketed by the existing G261 cause enum;
// display publication uses the existing 0..8 consumer-edge causes. This is behavior-pure and
// cached so the normal renderer pays one predictable branch at each real materialization.
static std::atomic<uint64_t> g_g279MatNs[8] = {}, g_g279MatReadNs[8] = {},
    g_g279MatRows[8] = {};
static std::atomic<uint64_t> g_g279DisplayNs[9] = {}, g_g279DisplayReadNs[9] = {},
    g_g279DisplayN[9] = {};

#if defined(PS2X_G684_HOT_DIAG)
static bool g279ProfileOn()
{
    static const bool s_on = envFlagEnabled("DC2_G279_PROFILE");
    return s_on;
}
#else
static constexpr bool g279ProfileOn() { return false; }
#endif

static bool g261StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G261_STAT");
    return s_on;
}
