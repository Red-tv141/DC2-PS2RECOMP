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
// G309: authoritative-page multi-source atlas consumer. The timing path samples five live target
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
// downstream 0x2720/tbw=8 atlas while the five physical FRAME/FBW views remain producer-owned.
// Default-on after exact, normal-presentation, and >=10% same-binary payoff gates passed.
static constexpr uint32_t kG310LogicalFbp = 0xfffffff5u;
static constexpr uint32_t kG310LogicalCt24Token = 0xfffffff4u;
struct G310PageState
{
    int8_t sourceTi = -1; // -1 guest VRAM; 0..4 resident producer
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

static bool g279ProfileOn()
{
    static const bool s_on = envFlagEnabled("DC2_G279_PROFILE");
    return s_on;
}

static bool g261StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G261_STAT");
    return s_on;
}

// ---- G340: lazy-VRAM payoff-ceiling census (behavior-pure, default-off DC2_G340_CENSUS=1) -----
// The arc's #1 unexplored lever (lazy materialized guest VRAM) proposes deferring the per-target
// materialize glReadPixels until a guest/runtime consumer actually READS the guest-VRAM bytes,
// via page protection. But `g261Materialize` ALREADY defers readback to consumer edges — it fires
// only when a consumer's block range overlaps a dirty resident target. So a page-fault lazy scheme
// can only save the readback for dirty rows that NO consumer actually reads before they are
// overwritten. This census measures that ceiling directly, without any SEH/VirtualProtect risk:
// at each range-triggered materialize, split the target's dirty rows (the readback unit,
// == g_g279MatRows) three ways —
//   consumed : the triggering copy's SRC rect dirty-overlaps them  -> a real read; lazy CANNOT skip.
//   overwrite: only the DST rect dirty-overlaps them (no src)      -> written over; lazy skips IFF
//              the write fully covers (upper-bounded skippable).
//   conserv  : NEITHER src nor dst dirty-overlaps (materialize fired purely on the coarse
//              full-target range) -> pure conservative barrier; 100% skippable by an exact scheme.
// unknown    : srcUnknown/dstUnknown fail-closed (counted as consumed = safe "needed" upper bound).
// Skippable = overwrite + conserv. If consumed dominates, lazy-VRAM is the G280/G304/G339 local
// minimum (deferral just relocates the read); if skippable dominates, a page-exact scheme pays.
static bool g340CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G340_CENSUS");
    return s_on;
}
static std::atomic<uint64_t> g_g340Mats{0};        // range-triggered materializes censused
static std::atomic<uint64_t> g_g340RowsTotal{0};   // Σ dirty rows materialized (== consumed+skippable)
static std::atomic<uint64_t> g_g340RowsConsumed{0};// Σ rows the consumer src-reads (lazy cannot skip)
static std::atomic<uint64_t> g_g340RowsOverwrite{0};// Σ rows only dst-written (skippable if write total)
static std::atomic<uint64_t> g_g340RowsConserv{0}; // Σ rows neither src nor dst dirty-overlap (dead)
static std::atomic<uint64_t> g_g340RowsUnknown{0}; // Σ rows under fail-closed unknown ranges
static std::atomic<uint64_t> g_g340CauseTotal[8]{};    // per-cause total rows
static std::atomic<uint64_t> g_g340CauseSkippable[8]{};// per-cause (overwrite+conserv) rows

static bool g267CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G267_CENSUS");
    return s_on;
}

static void g266Report(); // defined below g261AnyDirty (reads the G261 residency state)

// G291 (bring-up opt-in DC2_G291_ATLAS=1; kills DC2_G291_NO_ATLAS=1 / DC2_G291_ATLAS=0):
// persistent-residency consumer-completeness for the 0x139 work-block atlas. The G290-ON census
// measured the exact relocation: admitting the seeded fbp=0x139/fbw=8 batch keeps the whole
// atlas dirty-resident, and texture consumers (T8 materials at 0x34a0/0x3460/0x34e0, the T8
// 0x143<-0x13c view source) then force kG261MatTexAlias materializations of ~4x more rows
// (c5 3.7s -> 11.8s per run) — even though the game re-UPLOADS those exact texture pages every
// frame (3.9 MB/f) before sampling them, so guest VRAM is already current for the pages the
// consumers read. G291 tracks cumulative per-pixel upload re-coverage: a page whose pixels were
// FULLY re-covered by noted uploads since the last wave write has VRAM >= FBO by construction
// (the G264 mirror map records the exact CT32 byte->target-pixel map of every upload), so a
// texture/CLUT read overlapping ONLY such pages needs no materialization. Writers still publish
// through every existing edge; the skip applies to the READ-triggered materialize only, and
// fails closed on any page not provably current or any layout change. G292 audited the G280
// alias-view write paths and retained exact tile-coverage invalidation diagnostics, but the
// combined behavior remains forbidden: true multi-owner pages need a per-target authority token,
// which G283's page-global epoch does not provide. G291 implies the G290 seeded admission.
static bool g291AtlasOn()
{
    // G294 combined arm: the owner-token makes the g280 alias-view exact on multi-owner pages, so
    // G291's upload-re-coverage retire (excluded from G280 since G292) can now run ALONGSIDE it —
    // this is the consumer-chain retire that makes the atlas residency actually pay. g294 also
    // implies the retire (and, via g290ExactRowsOn, the seeded admission) so the whole paying arm
    // is one behavior lever: DC2_G294_OWNER_TOKEN=1.
    // G310 removes the whole-atlas display publication that made this exact retire neutral.
    // Its subordinate-alias retirement makes G290's seeded 0x139 admission single-owner again,
    // so the proven re-coverage rule completes the remaining T8 consumer class.
    static const bool s_on = (envFlagEnabled("DC2_G291_ATLAS") ||
                              g294OwnerTokenRequested() ||
                              g310AliasRetireOn()) &&
                             !envFlagEnabled("DC2_G291_NO_ATLAS") &&
                             !envFlagDisabled("DC2_G291_ATLAS");
    return s_on && (!g280AliasOn() || g294OwnerTokenOn());
}
static bool g291StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G291_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g291TexSkips{0}, g_g291TexMats{0}, g_g291CurPagesSet{0},
    g_g291CurPagesClr{0}, g_g291LayoutResets{0};

// G292: any GPU-side alias copy makes this target tile potentially newer than guest VRAM.
// Clear only the exact CT32 64x32 page tile; unrelated uploaded pages in the same row retain
// their G291 proof. Overlay restoration later makes FBO==VRAM again, but leaving the tile clear
// is the conservative direction until a noted upload re-covers it.
static void g291ClearCopiedTile(int ti, uint32_t pageRow, uint32_t pageCol)
{
    if (!g291AtlasOn() || ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    G261Res &r = g_g261Res[ti];
    if (r.g291CovFbw == 0u)
        return;
    if (pageRow >= 16u || pageCol >= 8u)
    {
        r.g291Cov = {};
        r.g291CovFbw = 0u;
        g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t y0 = pageRow * 32u;
    for (uint32_t y = y0; y < y0 + 32u; ++y)
        r.g291Cov[y][pageCol] = 0u;
    g_g291CurPagesClr.fetch_add(64u * 32u, std::memory_order_relaxed);
}

static void g261Report()
{
    if (!g261StatOn() && !g279ProfileOn() && !g310StatOn() && !g340CensusOn())
        return;
    if (g291StatOn())
        std::fprintf(stderr,
                     "[G291:stat] skips=%llu mats=%llu covSetPx=%llu covClrPx=%llu "
                     "layoutResets=%llu\n",
                     (unsigned long long)g_g291TexSkips.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291TexMats.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesSet.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesClr.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291LayoutResets.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G261:stat] waves=%llu rbSkip=%llu fboBind=%llu fboBindBi=%llu fboRej=%llu inv=%llu unk=%llu "
                 "mat(cpu=%llu inl=%llu up=%llu l2l=%llu l2h=%llu tex=%llu rsync=%llu fail=%llu) "
                 "mir(n=%llu f=%llu x=%llu) g267(cand=%llu elig=%llu foot=%llu alias=%llu)\n",
                 (unsigned long long)g_g261Waves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261SkippedRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboBinds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g263FboBindBi.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboRejects.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Invariants.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261MatUnknownRange.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirNoted.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFlushed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriCandidates.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriEligible.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriFootReject.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriAliasReject.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G261:res]");
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        std::fprintf(stderr, " %03x(mat=%llu win=%d..%d dirty=%d)", kG261Fbp[i],
                     (unsigned long long)g_g261MatByTarget[i].load(std::memory_order_relaxed),
                     g_g261Res[i].winLo, g_g261Res[i].winHi, g_g261Res[i].dirty ? 1 : 0);
    std::fprintf(stderr, " rttRej(classify=%llu depth=%llu) g262(zwave=%llu zrb=%llu)\n",
                 (unsigned long long)g_g261RttClassifyRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261RttDepthRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262DepthWaves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262ZReadbacks.load(std::memory_order_relaxed));
    g280Report();
    g310Report();
    if (g274ZTimeOn())
        std::fprintf(stderr,
                     "[G274:ztime] zUpMs=%.1f zRbMs=%.1f redundant=%llu necessary=%llu "
                     "(cumulative swizzle round-trip; redundant=elidable by depth residency)\n",
                     (double)g_g274ZUpNs.load(std::memory_order_relaxed) / 1e6,
                     (double)g_g274ZRbNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274ZRedundant.load(std::memory_order_relaxed),
                     (unsigned long long)g_g274ZNecessary.load(std::memory_order_relaxed));
    if (g274ZTimeOn())
        std::fprintf(stderr, "[G274:submit] submitMs=%.1f submitN=%llu (synchronous backend waves)\n",
                     (double)g_g274SubNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274SubN.load(std::memory_order_relaxed));
    if (g274ZTimeOn() || g274ZResidentOn())
        std::fprintf(stderr, "[G274:zresident] uploadsSkipped=%llu (redundant Z re-uploads elided)\n",
                     (unsigned long long)g_g274ZUploadsSkipped.load(std::memory_order_relaxed));
    if (g275StatOn())
        std::fprintf(stderr,
                     "[G275:fused] jobs=%llu readWaitsElided=%llu submitReadMs=%.1f "
                     "zSwizzleLanes=%d\n",
                     (unsigned long long)g_g275FusedJobs.load(std::memory_order_relaxed),
                     (unsigned long long)g_g275ReadWaitsElided.load(std::memory_order_relaxed),
                     (double)g_g275FusedNs.load(std::memory_order_relaxed) / 1e6,
                     g275ZSwizzleLanes());
    if (g276ProbeOn())
    {
        auto ms = [](const std::atomic<uint64_t> &a) {
            return (double)a.load(std::memory_order_relaxed) / 1e6;
        };
        auto cnt = [](const std::atomic<uint64_t> &a) {
            return (unsigned long long)a.load(std::memory_order_relaxed);
        };
        std::fprintf(stderr,
                     "[G276:submit] rttSkip(depthW=%.1fms/%llu noZ=%.1fms/%llu) "
                     "readback(colorZ=%.1fms/%llu color=%.1fms/%llu)\n",
                     ms(g_g276SubNs[1][1]), cnt(g_g276SubN[1][1]),
                     ms(g_g276SubNs[1][0]), cnt(g_g276SubN[1][0]),
                     ms(g_g276SubNs[0][1]), cnt(g_g276SubN[0][1]),
                     ms(g_g276SubNs[0][0]), cnt(g_g276SubN[0][0]));
        const unsigned long long win = cnt(g_g276ColorRbWinRows);
        const unsigned long long full = cnt(g_g276ColorRbFullRows);
        std::fprintf(stderr,
                     "[G276:colorrb] n=%llu winRows=%llu fullRows=%llu span=%.1f%% "
                     "(rows actually swizzled vs full-FBO glReadPixels)\n",
                     cnt(g_g276ColorRbN), win, full,
                     full ? 100.0 * (double)win / (double)full : 0.0);
    }
    if (g276DisplayRbOn() || g276ProbeOn())
    {
        const unsigned long long def = g_g276Deferred.load(std::memory_order_relaxed);
        const unsigned long long fl = g_g276Flushes.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G276:coalesce] deferred=%llu flushes=%llu rows=%llu fail=%llu "
                     "readbacksSaved=%llu uploadPublish=%llu exactGenWin=%llu\n",
                     def, fl, (unsigned long long)g_g276FlushRows.load(std::memory_order_relaxed),
                     (unsigned long long)g_g276FlushFail.load(std::memory_order_relaxed),
                     def > fl ? def - fl : 0ull,
                     (unsigned long long)g_g277UploadPublish.load(std::memory_order_relaxed),
                     (unsigned long long)g_g277ExactGenWin.load(std::memory_order_relaxed));
    }
    if (g279ProfileOn())
    {
        std::fprintf(stderr, "[G279:mat]");
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llur", i,
                         (double)g_g279MatNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279MatReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279MatRows[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/rows)\n[G279:display]");
        for (int i = 0; i < 9; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llu", i,
                         (double)g_g279DisplayNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279DisplayReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279DisplayN[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/count)\n");
    }
    if (g340CensusOn())
    {
        const uint64_t tot = g_g340RowsTotal.load(std::memory_order_relaxed);
        const uint64_t cons = g_g340RowsConsumed.load(std::memory_order_relaxed);
        const uint64_t ovr = g_g340RowsOverwrite.load(std::memory_order_relaxed);
        const uint64_t con = g_g340RowsConserv.load(std::memory_order_relaxed);
        const uint64_t unk = g_g340RowsUnknown.load(std::memory_order_relaxed);
        const uint64_t skip = ovr + con;
        std::fprintf(stderr,
                     "[G340:ceiling] mats=%llu rows=%llu consumed=%llu overwrite=%llu "
                     "conserv=%llu unknown=%llu skippable=%llu (%.1f%% of rows)\n",
                     (unsigned long long)g_g340Mats.load(std::memory_order_relaxed),
                     (unsigned long long)tot, (unsigned long long)cons,
                     (unsigned long long)ovr, (unsigned long long)con,
                     (unsigned long long)unk, (unsigned long long)skip,
                     tot ? 100.0 * (double)skip / (double)tot : 0.0);
        std::fprintf(stderr, "[G340:bycause]");
        for (int i = 0; i < 8; ++i)
        {
            const uint64_t ct = g_g340CauseTotal[i].load(std::memory_order_relaxed);
            if (ct == 0u)
                continue;
            const uint64_t cs = g_g340CauseSkippable[i].load(std::memory_order_relaxed);
            std::fprintf(stderr, " c%d=%llu/%lluskip", i,
                         (unsigned long long)cs, (unsigned long long)ct);
        }
        std::fprintf(stderr, " (skippable/total rows; c2=upload c3=l2l)\n");
    }
    if (g284StatOn())
    {
        std::fprintf(stderr, "[G284:stat] uploadCoalesce=%d skipped=%llu flushed=%llu\n",
                     g284UploadCoalesceOn() ? 1 : 0,
                     (unsigned long long)g_g284UploadSkipped.load(std::memory_order_relaxed),
                     (unsigned long long)g_g284UploadFlushed.load(std::memory_order_relaxed));
    }
    if (g288StatOn())
    {
        std::fprintf(stderr,
                     "[G288:stat] audits=%llu publishes=%llu disjoint=%llu unknown=%llu\n",
                     (unsigned long long)g_g288GpuAudits.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuDisjoint.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuUnknown.load(std::memory_order_relaxed));
    }
    if (g289StatOn())
    {
        std::fprintf(stderr,
                     "[G289:stat] direct=%llu copy=%llu copyFail=%llu consume=%llu carry=%llu "
                     "view=%llu viewFail=%llu bind=%llu mat=%llu matFail=%llu unexpected=%llu "
                     "upOverwrite=%llu inlineCarry=%llu frameCarry=%llu suffixPub=%llu "
                     "verify=%llu bad=%llu "
                     "active=%u version=%llu owned=%x..%x\n",
                     (unsigned long long)g_g289Direct.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Copies.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289CopyFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Consumes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Carries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViews.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViewFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerBinds.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Materializes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289MatFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Unexpected.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289UploadOverwrites.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289InlineCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289FrameCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289SuffixPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyChecks.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyBad.load(std::memory_order_relaxed),
                     s_g289Owner.active ? 1u : 0u,
                     (unsigned long long)s_g289Owner.version,
                     s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock);
    }
    if (g338Ct24ViewOn())
        std::fprintf(stderr, "[G338:stat] views=%llu buildFail=%llu verifyBad=%llu\n",
                     (unsigned long long)g_g338Views.load(std::memory_order_relaxed),
                     (unsigned long long)g_g338BuildFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g338VerifyBad.load(std::memory_order_relaxed));
    g345PrintEscape();
    g347PrintUp139();
    g262Report();
    g265Report();
    g266Report();
}

static bool g261AnyDirty()
{
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        if (g_g261Res[i].dirty)
            return true;
    return false;
}

// G266: a CPU band replay is about to materialize resident target `ti` because entry `e`
// overlaps it. hitBits: 1=color 2=depth 4=texture 8=CLUT 16=unknown-range.
static void g266NoteCpuMat(const G144Entry &e, uint32_t ti, uint32_t hitBits)
{
    if (!g266CensusOn())
        return;
    g_g266CpuN.fetch_add(1u, std::memory_order_relaxed);
    const GSContext &ctx = e.ctx;
    const bool zOn = ((ctx.test >> 16) & 1u) != 0u && ((ctx.zbuf >> 32) & 1ull) == 0ull;
    uint64_t k = 0;
    k |= static_cast<uint64_t>(ctx.frame.fbp & 0x1FFu);                               // 0-8
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 9;                              // 9-11
    k |= static_cast<uint64_t>(e.prim.tme ? 1u : 0u) << 12;                           // 12
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.psm & 0x3Fu) : 0x3Fu) << 13;    // 13-18
    k |= static_cast<uint64_t>(ctx.zbuf & 0x1FFu) << 19;                              // 19-27
    k |= static_cast<uint64_t>(zOn ? 1u : 0u) << 28;                                  // 28
    k |= static_cast<uint64_t>(hitBits & 0x1Fu) << 29;                                // 29-33
    k |= static_cast<uint64_t>(ti & 0x7u) << 34;                                      // 34-36
    k |= static_cast<uint64_t>(g_g266FlushRej & 0xFu) << 37;                          // 37-40
    k |= static_cast<uint64_t>((g_g266FlushRej == kG266FrClassify
                                    ? g_g266ClassifySite : 0xF) & 0xFu) << 41;        // 41-44
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.tbp0 & 0x3FFFu) : 0u) << 45;    // 45-58
    ++g_g266CpuShapes[k];
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
        std::fprintf(stderr,
                     "[G266:cpu] fbp=%03x prim=%u tme=%u tpsm=0x%x tbp=0x%x zbp=%03x zOn=%u "
                     "hit=0x%x t=%03x rej=%s site=%s\n",
                     ctx.frame.fbp, static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                     e.prim.tme ? ctx.tex0.psm : 0u, e.prim.tme ? ctx.tex0.tbp0 : 0u,
                     static_cast<unsigned>(ctx.zbuf & 0x1FFu), zOn ? 1u : 0u, hitBits,
                     kG261Fbp[ti], kG266FrName[g_g266FlushRej],
                     (g_g266FlushRej == kG266FrClassify && g_g266ClassifySite >= 0 &&
                      g_g266ClassifySite < kG262RejSiteCount)
                         ? kG262SiteName[g_g266ClassifySite]
                         : "-");
}

// G266: a local→local copy is about to drain resident targets. Record each target's exact role
// (source/destination/unknown-range) plus both transfer layouts — the G264 lesson says the fix
// (if any) must map addresses under BOTH swizzles, so the census must carry both.
static void g266NoteLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                              uint32_t ssax, uint32_t ssay,
                              uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                              uint32_t dsax, uint32_t dsay,
                              uint32_t rrw, uint32_t rrh,
                              const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown)
{
    if (!g266CensusOn() || !g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        const bool unknown = srcUnknown || dstUnknown || !r.range.valid;
        const bool hitSrc = !unknown && src.valid && g144RangeOverlaps(r.range, src);
        const bool hitDst = !unknown && dst.valid && g144RangeOverlaps(r.range, dst);
        if (!unknown && !hitSrc && !hitDst)
            continue; // this target survives the copy — not a drain
        g_g266L2lN.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t role = unknown ? 4u : ((hitSrc ? 1u : 0u) | (hitDst ? 2u : 0u));
        uint64_t k = 0;
        k |= static_cast<uint64_t>(sbp & 0x3FFFu);       // 0-13
        k |= static_cast<uint64_t>(spsm & 0x3Fu) << 14;  // 14-19
        k |= static_cast<uint64_t>(sbw & 0x3Fu) << 20;   // 20-25
        k |= static_cast<uint64_t>(dbp & 0x3FFFu) << 26; // 26-39
        k |= static_cast<uint64_t>(dpsm & 0x3Fu) << 40;  // 40-45
        k |= static_cast<uint64_t>(dbw & 0x3Fu) << 46;   // 46-51
        k |= static_cast<uint64_t>(i & 0x7u) << 52;      // 52-54
        k |= static_cast<uint64_t>(role & 0x7u) << 55;   // 55-57
        ++g_g266L2lShapes[k];
        static std::atomic<uint32_t> s_log{0};
        if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
            std::fprintf(stderr,
                         "[G266:l2l] s(bp=0x%x bw=%u psm=0x%x at %u,%u) d(bp=0x%x bw=%u psm=0x%x "
                         "at %u,%u) rect=%ux%u t=%03x role=%s dirty=%d..%d cov=%d..%d\n",
                         sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                         dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                         kG261Fbp[i],
                         role == 4u ? "unk" : role == 3u ? "src+dst"
                                            : role == 1u ? "src" : "dst",
                         r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    }
}

// G266: a GPU batch samples a resident target but the FBO-direct bind was rejected — record the
// consumer's exact state shape (the [G262:bindrej] print is capped; this aggregates all of them).
static void g266NoteTexBindMat(uint32_t dstFbp, uint32_t ti, const G144Entry &e,
                               const G178EntryState &st)
{
    if (!g266CensusOn())
        return;
    g_g266TexN.fetch_add(1u, std::memory_order_relaxed);
    uint64_t k = 0;
    k |= static_cast<uint64_t>(dstFbp & 0x1FFu);                 // 0-8
    k |= static_cast<uint64_t>(ti & 0x7u) << 9;                  // 9-11
    k |= static_cast<uint64_t>(e.ctx.tex0.psm & 0x3Fu) << 12;    // 12-17
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 18;        // 18-20
    k |= static_cast<uint64_t>(e.prim.fst ? 1u : 0u) << 21;      // 21
    k |= static_cast<uint64_t>(st.bilinear ? 1u : 0u) << 22;     // 22
    k |= static_cast<uint64_t>(st.wrapU & 0x3u) << 23;           // 23-24
    k |= static_cast<uint64_t>(st.wrapV & 0x3u) << 25;           // 25-26
    k |= static_cast<uint64_t>(e.ctx.tex0.tbp0 & 0x3FFFu) << 27; // 27-40
    k |= static_cast<uint64_t>(e.ctx.tex0.tbw & 0x3Fu) << 41;    // 41-46
    ++g_g266TexShapes[k];
}

static void g266Report()
{
    if (!g266CensusOn())
        return;
    std::fprintf(stderr, "[G266:census] cpu=%llu l2l=%llu tex=%llu shapes(%zu/%zu/%zu)\n",
                 (unsigned long long)g_g266CpuN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266L2lN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266TexN.load(std::memory_order_relaxed),
                 g_g266CpuShapes.size(), g_g266L2lShapes.size(), g_g266TexShapes.size());
    for (const auto &kv : g_g266CpuShapes)
    {
        const uint64_t k = kv.first;
        const int rej = static_cast<int>((k >> 37) & 0xFu);
        const int site = static_cast<int>((k >> 41) & 0xFu);
        std::fprintf(stderr,
                     "[G266:cpuShape] n=%llu fbp=%03x prim=%u tme=%u tpsm=0x%02x tbp=0x%x "
                     "zbp=%03x zOn=%u hit=0x%x t=%03x rej=%s site=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), (unsigned)((k >> 9) & 0x7u),
                     (unsigned)((k >> 12) & 1u), (unsigned)((k >> 13) & 0x3Fu),
                     (unsigned)((k >> 45) & 0x3FFFu), (unsigned)((k >> 19) & 0x1FFu),
                     (unsigned)((k >> 28) & 1u), (unsigned)((k >> 29) & 0x1Fu),
                     kG261Fbp[(k >> 34) & 0x7u],
                     kG266FrName[rej],
                     (rej == kG266FrClassify && site < kG262RejSiteCount)
                         ? kG262SiteName[site] : "-");
    }
    for (const auto &kv : g_g266L2lShapes)
    {
        const uint64_t k = kv.first;
        const uint32_t role = static_cast<uint32_t>((k >> 55) & 0x7u);
        std::fprintf(stderr,
                     "[G266:l2lShape] n=%llu s(bp=0x%x bw=%u psm=0x%02x) d(bp=0x%x bw=%u "
                     "psm=0x%02x) t=%03x role=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x3FFFu), (unsigned)((k >> 20) & 0x3Fu),
                     (unsigned)((k >> 14) & 0x3Fu),
                     (unsigned)((k >> 26) & 0x3FFFu), (unsigned)((k >> 46) & 0x3Fu),
                     (unsigned)((k >> 40) & 0x3Fu),
                     kG261Fbp[(k >> 52) & 0x7u],
                     role == 4u ? "unk" : role == 3u ? "src+dst"
                                        : role == 1u ? "src" : "dst");
    }
    for (const auto &kv : g_g266TexShapes)
    {
        const uint64_t k = kv.first;
        std::fprintf(stderr,
                     "[G266:texShape] n=%llu dstFbp=%03x t=%03x tpsm=0x%02x tbp=0x%x tbw=%u "
                     "prim=%u fst=%u bilin=%u wrap=%u%u\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), kG261Fbp[(k >> 9) & 0x7u],
                     (unsigned)((k >> 12) & 0x3Fu), (unsigned)((k >> 27) & 0x3FFFu),
                     (unsigned)((k >> 41) & 0x3Fu), (unsigned)((k >> 18) & 0x7u),
                     (unsigned)((k >> 21) & 1u), (unsigned)((k >> 22) & 1u),
                     (unsigned)((k >> 23) & 0x3u), (unsigned)((k >> 25) & 0x3u));
    }
}

// Recompute the claimed row window (cov ∪ dirty) and its block range + gen baseline. Rows
// OUTSIDE the window belong to VRAM alone (never uploaded to or read back from the FBO), so
// writes there are free — no materialize, no invariant. The gen baseline is scoped to the same
// window so unrelated same-fbp-page-range traffic cannot trip it falsely.
static void g261UpdateWindow(int ti)
{
    G261Res &r = g_g261Res[ti];
    int lo = r.covLo, hi = r.covHi;
    if (r.dirty)
    {
        lo = std::min(lo, r.dirtyLo);
        hi = std::max(hi, r.dirtyHi);
    }
    r.winLo = lo;
    r.winHi = hi;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[ti]);
    if (hi >= lo && r.fbW > 0)
    {
        r.range = g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                                   static_cast<uint32_t>(lo),
                                   static_cast<uint32_t>(r.fbW),
                                   static_cast<uint32_t>(hi - lo + 1));
        r.genAtSkip = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                                     static_cast<uint32_t>(lo),
                                     static_cast<uint32_t>(r.fbW),
                                     static_cast<uint32_t>(hi - lo + 1));
    }
    else
    {
        r.range = G144BlockRange{};
        r.genAtSkip = 0;
    }
}

static uint64_t g261WindowGenNow(const G261Res &r, uint32_t fbp)
{
    if (r.winHi < r.winLo || r.fbW <= 0)
        return 0;
    return g178GenSumRect(framePageBaseToBlock(fbp), r.fbw, GS_PSM_CT32, 0u,
                          static_cast<uint32_t>(r.winLo),
                          static_cast<uint32_t>(r.fbW),
                          static_cast<uint32_t>(r.winHi - r.winLo + 1));
}

// G264 (default-on since G266): route guest re-uploads INTO the resident FBO instead of
// materializing around them (pillar-3 GPU-resident upload; G261 blocker 2). The upload hook
// defers eligible rects to `mirPending` rows; this flush reads those rows from VRAM (the guest
// upload has completed — the GIF stream is serial) and glTexSubImage2Ds them into the FBO,
// keeping the residency alive across the guest's per-frame scratch-page upload cycle.
static bool g264On()
{
    // G266 promotion: default-on (only consulted on wave paths, so the wave/stack kills gate it
    // upstream). Kill: DC2_G264_NO_UP_FBO=1. DC2_G264_UP_FBO=1 stays accepted; =0 disables.
    static const bool s_on = !envFlagEnabled("DC2_G264_NO_UP_FBO") &&
                             !envFlagDisabled("DC2_G264_UP_FBO");
    return s_on;
}

// Push pending mirror rows VRAM→FBO. Returns false on backend failure, in which case the FBO
// no longer matches guest intent for those rows — the CALLER must drop the residency WITHOUT a
// readback (a readback would clobber the newer VRAM upload bytes with stale FBO content).
static bool g264FlushMirror(int ti)
{
    G261Res &r = g_g261Res[ti];
    if (!r.mirPending)
        return true;
    std::unique_ptr<G327MirrorTimer> g327Timer;
    if (g327CensusOn())
        g327Timer = std::make_unique<G327MirrorTimer>();
    const int lo = r.mirLo, hi = r.mirHi;
    const uint32_t fbp = kG261Fbp[ti];
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange mirrorRange =
        (hi >= lo && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(std::max(0, lo)),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(hi - lo + 1))
            : G144BlockRange{};
    if (!g278FlushPendingDepthForRanges(
            1, mirrorRange, G144BlockRange{},
            hi >= lo && r.fbW > 0 && !mirrorRange.valid, false))
        return false; // deferred Z could not be published; do not decode stale VRAM into the FBO
    r.mirPending = false;
    r.mirLo = 512;
    r.mirHi = -1;
    uint8_t *vram = g_g144LastVram;
    auto clearBits = [&]() {
        if (hi >= lo)
            for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
                r.mirBits[static_cast<size_t>(y)].fill(0u);
    };
    if (vram == nullptr || hi < lo || lo < 0 || hi >= 512 ||
        r.fbW <= 0 || r.fbW > 512 || g_g261VramSize == 0u)
    {
        clearBits();
        return false;
    }
    static std::vector<uint32_t> s_g264Px; // single-threaded (flush-owner threads only)
    const bool g351MirDup = g351SplitOn() && fbp == 0x139u &&
                            (s_g351Band0IsB || s_g351Band1IsB);
    bool anyPatch = false;
    int patchLo = 512, patchHi = -1;
    uint32_t patchRects = 0u;
    for (int y = lo; y <= hi;)
    {
        const auto &rowMask = r.mirBits[static_cast<size_t>(y)];
        bool rowEmpty = true;
        for (uint64_t word : rowMask)
            rowEmpty = rowEmpty && (word == 0u);
        if (rowEmpty)
        {
            ++y;
            continue;
        }

        // Adjacent rows with the same target-pixel mask become one glTexSubImage2D rectangle
        // per horizontal run. This keeps the common full-page aliases to 1-2 queue jobs while
        // remaining exact for arbitrary CT32 swizzle aliases and disjoint pending uploads.
        int groupHi = y;
        while (groupHi < hi &&
               r.mirBits[static_cast<size_t>(groupHi + 1)] == rowMask)
            ++groupHi;
        const int rows = groupHi - y + 1;
        for (int x = 0; x < r.fbW;)
        {
            const auto bitSet = [&](int px) {
                return (rowMask[static_cast<size_t>(px >> 6)] &
                        (uint64_t{1} << (px & 63))) != 0u;
            };
            while (x < r.fbW && !bitSet(x))
                ++x;
            if (x >= r.fbW)
                break;
            const int x0 = x;
            while (x < r.fbW && bitSet(x))
                ++x;
            const int width = x - x0;
            const int glY = 511 - groupHi;
            const size_t pixelCount = static_cast<size_t>(width) * rows;
            s_g264Px.assign(pixelCount, 0u);
            bool packed = true;
            for (int gy = y; gy <= groupHi && packed; ++gy)
            {
                // GL input rows are bottom-first: highest GS y is the first buffer row.
                const size_t dstRow = static_cast<size_t>(groupHi - gy) * width;
                for (int gx = x0; gx < x0 + width; ++gx)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, r.fbw, static_cast<uint32_t>(gx), static_cast<uint32_t>(gy));
                    if (off + 4u > g_g261VramSize)
                    {
                        packed = false;
                        break;
                    }
                    std::memcpy(&s_g264Px[dstRow + static_cast<size_t>(gx - x0)],
                                vram + off, 4);
                }
            }
            // G351: a mirror patch carries VRAM-authoritative bytes — under the surface split
            // the sibling FBO may be the one a later materialize reads, so patch it too
            // (writing newest-VRAM bytes into both FBOs is always coherent).
            if (!packed ||
                     !g264_backend_write_color_rect(fbp, r.fbW, 512, x0, glY,
                                                     width, rows, s_g264Px) ||
                     (g351MirDup &&
                      !g264_backend_write_color_rect(kG351Sprite139, r.fbW, 512, x0, glY,
                                                     width, rows, s_g264Px)))
            {
                clearBits();
                g_g264MirFail.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[G264:mirror-fail] fbp=0x%x rect=%d,%d %dx%d fbW=%d packed=%u\n",
                             fbp, x0, y, width, rows, r.fbW, packed ? 1u : 0u);
                return false;
            }
            anyPatch = true;
            ++patchRects;
            patchLo = std::min(patchLo, y);
            patchHi = std::max(patchHi, groupHi);
        }
        y = groupHi + 1;
    }
    clearBits();
    if (!anyPatch)
        return false;
    if (patchLo < r.covLo) r.covLo = patchLo;
    if (patchHi > r.covHi) r.covHi = patchHi;
    g261UpdateWindow(ti); // re-anchor claimed rows + gen baseline past the upload's own bumps
    // FBO == VRAM again for the mirrored rows; keep the snapshot coherent if one is live.
    G178FbSnap &snap = g_g178FbSnap[fbp];
    if (snap.valid)
    {
        if (lo < snap.rowLo) snap.rowLo = lo;
        if (hi > snap.rowHi) snap.rowHi = hi;
        snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                     static_cast<uint32_t>(r.fbW), 512u);
    }
    if (g262CensusOn())
    {
        static std::atomic<uint32_t> s_g264PatchLog{0};
        if (s_g264PatchLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr, "[G264:patch] fbp=0x%x rows=%d..%d rects=%u fbW=%d\n",
                         fbp, patchLo, patchHi, patchRects, r.fbW);
    }
    g_g264MirFlushed.fetch_add(1u, std::memory_order_relaxed);
    // G280: the mirror wrote VRAM-authoritative bytes into these FBO rows — re-sync + drop any
    // alias-overlay tiles they touch and stale every overlay copied FROM this target.
    g280NoteMirrorPatch(ti, patchLo, patchHi);
    g310NoteProducerWrite(ti, patchLo, patchHi);
    return true;
}

// G264: flush every pending upload mirror before a GPU flush touches classify/invariant/bind
// state. A mirror failure means the FBO is stale for rows VRAM now owns — drop that residency
// WITHOUT a readback (loud, bounded; the fresh VRAM upload bytes must win).
static void g264FlushMirrorsAtFlush()
{
    if (!g264On())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.mirPending)
            continue;
        if (!g264FlushMirror(static_cast<int>(i)))
        {
            g280DropOverlays(static_cast<int>(i)); // residency dies; nothing publishes them
            r.dirty = false;
            r.dirtyLo = 512;
            r.dirtyHi = -1;
            g261UpdateWindow(static_cast<int>(i));
            G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
            snap.valid = false;
            snap.genSum = 0;
            snap.rowLo = 1 << 30;
            snap.rowHi = -1;
            g310NoteTargetClean(false);
        }
    }
}

// Read the dirty row window back from the target's FBO into guest VRAM, advance the page write
// generations (this is when VRAM bytes actually change), and refresh the fb snapshot so the next
// GPU flush of this target does not re-upload bytes the FBO already equals. On ANY failure the
// residency is dropped loudly with VRAM left as-is (bounded stale-pixel glitch, never a hang or
// a silent overwrite of newer guest bytes).
// G305 forward decls (definitions live next to g178TryFlushGpu). g261Materialize is a target-FBO
// read consumer, so it must fence any in-flight async packet first.
static bool g305AsyncOn();
static bool g305DrainRequired();
static void g305DrainPending();

// ===== G432: the SECOND colour-readback unpack site =====
// G418 replaced the serial per-pixel `addrPSMCT32` + 4-byte-memcpy publication loop with an exact
// one-page offset table + paired 8-byte stores (+ optional row lanes) and promoted it — but only at
// the G144 batch publication site in rasterizer_vram_materialization.inc. `g261Materialize` runs the
// IDENTICAL loop shape on the resident-RTT publication path, and the G432 census measured that path
// as the largest single block on the GS front thread (the local->local TRXDIR edge alone spends
// ~5.3 ms/f in g261MaterializeForRanges and ~2.3 ms/f in g285PrepareL2lConsume, both of which end
// here). This part forwards that site to the same proven helper.
//
// EXACTNESS. The legacy loop indexes its source as `srcRow = (511 - y - glY) * fbW` with
// `glY = 512 - 1 - dirtyHi`, i.e. `(dirtyHi - y) * fbW`. g418UnpackColorRows computes
// `(fbH - 1 - y) * fbW`, so passing `fbH = dirtyHi + 1` reproduces the same index for every y in
// [dirtyLo, dirtyHi]. Everything else (address table, paired stores, clipping fallback) is the
// G418 mechanism unchanged, already oracle-verified at 109,051,904 pixels with bad=0.
//
// Declared here because g418_readback_unpack.inc is included later in this TU.
static void g418UnpackColorRows(uint8_t *vram, uint32_t vramSize,
                                const std::vector<uint32_t> &src,
                                uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                                int rowLo, int rowHi, bool legacy);

// DC2_G432_NO_MAT_UNPACK=1 restores the serial per-pixel publication loop (rollback arm).
static bool g432MatUnpackOn()
{
    static const bool on = !envFlagEnabled("DC2_G432_NO_MAT_UNPACK");
    return on;
}

// Sub-millisecond lever ⇒ the within-process randomized A/B (DC2_G419_AB=matunpack) is its only
// admissible gate. g419_ab_instrument.inc is compiled later in this TU, so both the selector and its
// lever id are referenced by forward declaration / by value.
static constexpr int kG432MatUnpackLeverId = 10; // == kG419Lever::kG432LeverMatUnpack
static bool g419AbArmFor(int lever, bool defaultOn);

// G454 dependency-boundary deletion probes. IDs mirror G419Lever, declared later in this TU.
// Candidate output is knowingly stale; downstream publication work remains unchanged so the
// delta prices the synchronous readback edge, not a smaller pixel-copy microbenchmark.
static constexpr int kG454RbAllLeverId = 20;
static constexpr int kG454RbL2lLeverId = 21;
static constexpr int kG454RbResyncLeverId = 22;
static constexpr int kG454RbDisplayLeverId = 23;
static bool g454SkipReadbackAt(int siteLever)
{
    if (g419AbArmFor(kG454RbAllLeverId, false))
        return true;
    return siteLever != 0 && g419AbArmFor(siteLever, false);
}

// Behaviour-pure timing of this publication loop only (DC2_G432_CENSUS=1).
static bool g432MatCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G432_CENSUS");
    return on;
}
static uint64_t g_g432MatUnpackNs = 0, g_g432MatUnpackN = 0, g_g432MatUnpackPx = 0;

static void g261Materialize(int ti, int cause)
{
    g324NoteObserver(1u); // direct resident-FBO/guest-authority consumer
    if (g305DrainRequired())
        g305DrainPending();
    G261Res &r = g_g261Res[ti];
    if (!r.dirty)
    {
        // Not dirty but a mirror may still be pending (e.g. noted just before an invariant
        // drop): flush it so the FBO does not keep stale rows for a later wave.
        if (r.mirPending)
            g264FlushMirror(ti);
        return;
    }
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    const uint32_t fbp = kG261Fbp[ti];
    const int rows = r.dirtyHi - r.dirtyLo + 1;
    if (g345StatOn() && g345FamilyFbp(fbp) && rows > 0)
        g345NoteFamReadbackFbp(fbp, static_cast<uint64_t>(rows));
    // G351: attribute this 0x139 FBO→VRAM publication (and the consumer demand behind its cause)
    // to the surface that last wrote the wide FBO — the split's consumer→surface map.
    if (fbp == 0x139u && rows > 0)
        g351NoteCon(kG351SiteMat, cause, static_cast<uint64_t>(rows));
    if (g310StatOn())
    {
        const int pageLo = std::max(0, r.dirtyLo) / 32;
        const int pageHi = std::min(511, r.dirtyHi) / 32;
        const uint64_t pages =
            pageHi >= pageLo ? static_cast<uint64_t>(pageHi - pageLo + 1) * r.fbw : 0u;
        g_g310MatN[cause & 7].fetch_add(1u, std::memory_order_relaxed);
        g_g310MatPages[cause & 7].fetch_add(pages, std::memory_order_relaxed);
    }
    uint8_t *vram = g_g144LastVram;
    bool ok = false;
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange dirtyRange =
        (rows > 0 && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(r.dirtyLo),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(rows))
            : G144BlockRange{};
    const bool depthPublished = g278FlushPendingDepthForRanges(
        1, G144BlockRange{}, dirtyRange, false,
        rows > 0 && r.fbW > 0 && !dirtyRange.valid);
    static std::vector<uint32_t> s_g261Px; // single-threaded (flush-owner threads only)
    // G264: bring the FBO current with any pending upload mirror FIRST — the readback below
    // must not clobber newer VRAM upload bytes with pre-upload FBO content. Mirror failure ⇒
    // ok stays false ⇒ the loud drop path below runs with VRAM untouched (upload preserved).
    // G280: alias-overlay tiles are re-synced from VRAM (and dropped) BEFORE the readback so
    // this publication never writes alias-page bytes the dst view does not own; a restore
    // failure takes the same loud no-readback drop path as a mirror failure.
    if (depthPublished && g280PrepareMaterialize(ti) && g264FlushMirror(ti) &&
        vram != nullptr && rows > 0 && r.fbW > 0 && g_g261VramSize != 0u)
    {
        const int glY = 512 - 1 - r.dirtyHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const int g455Site = fbp == 0x139u && cause == kG261MatLocal ? kG455Local139
                           : fbp == 0x139u && cause == kG261MatResync ? kG455Resync139
                                                                          : -1;
        const G455Ticket g455Ticket = g455BeginRead(
            g455Site, s_g455ResWriteSeq, fbp, r.fbw, r.fbW, r.dirtyLo, r.dirtyHi);
        // G351: under the surface split the CURRENT wide-0x139 content lives in the per-band
        // routed FBO (scene rows on the fbp key, sprite-only rows possibly on the sibling).
        const bool g351Split139 = g351SplitOn() && fbp == 0x139u;
        const uint32_t g351K0 = g351Split139 ? g351Band0Key139() : fbp; // rows <= 415
        const uint32_t g351K1 = g351Split139 ? g351Band1Key139() : fbp; // rows >= 416
        bool readOk;
        uint32_t g351ReadKey = fbp; // 0 = mixed-band read (no single FBO equals VRAM after)
        const int g454Site = cause == kG261MatLocal ? kG454RbL2lLeverId
                            : cause == kG261MatResync ? kG454RbResyncLeverId
                                                     : 0;
        const bool g454Skip = g454SkipReadbackAt(g454Site);
        if (g454Skip)
        {
            s_g261Px.resize(static_cast<size_t>(r.fbW) * rows);
            readOk = true;
        }
        else if (!g351Split139 || g351K0 == g351K1 || r.dirtyLo >= 416 || r.dirtyHi <= 415)
        {
            const uint32_t k = r.dirtyLo >= 416 ? g351K1 : g351K0;
            g351ReadKey = k;
            readOk = g178_backend_read_color(k, r.fbW, 512, glY, rows, s_g261Px);
        }
        else
        {
            // Two-part read: the dirty span crosses the band boundary and the bands live in
            // different FBOs. GL rows are bottom-first, so band-1 (higher GS rows) fills the
            // buffer head and band-0 the tail — same layout the copy loop below expects.
            ++s_g351MixedMat;
            g351ReadKey = 0u;
            static std::vector<uint32_t> s_g351Hi, s_g351Lo; // flush-owner thread only
            const int rows1 = r.dirtyHi - 416 + 1, rows0 = 415 - r.dirtyLo + 1;
            readOk = g178_backend_read_color(g351K1, r.fbW, 512, glY, rows1, s_g351Hi) &&
                     g178_backend_read_color(g351K0, r.fbW, 512, 512 - 1 - 415, rows0,
                                             s_g351Lo) &&
                     s_g351Hi.size() == static_cast<size_t>(r.fbW) * rows1 &&
                     s_g351Lo.size() == static_cast<size_t>(r.fbW) * rows0;
            if (readOk)
            {
                s_g261Px.resize(static_cast<size_t>(r.fbW) * rows);
                std::memcpy(s_g261Px.data(), s_g351Hi.data(),
                            s_g351Hi.size() * sizeof(uint32_t));
                std::memcpy(s_g261Px.data() + s_g351Hi.size(), s_g351Lo.data(),
                            s_g351Lo.size() * sizeof(uint32_t));
            }
        }
        g455EndRead(g455Ticket);
        if (g279Profile)
            g_g279MatReadNs[cause & 7].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g261Px.size() == static_cast<size_t>(r.fbW) * rows;
        if (ok)
        {
            // G432: exact G418 unpack (table + paired stores) at this second publication site.
            // `fbH = r.dirtyHi + 1` reproduces the legacy `(511 - y - glY)` source row index.
            const bool g432MatCensus = g432MatCensusOn();
            const auto g432T0 = g432MatCensus ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
            const bool g432Fast =
                g419AbArmFor(kG432MatUnpackLeverId, g432MatUnpackOn());
            g418UnpackColorRows(vram, g_g261VramSize, s_g261Px, fbBlock, r.fbw, r.fbW,
                                r.dirtyHi + 1, r.dirtyLo, r.dirtyHi,
                                /*legacy=*/!g432Fast);
            if (g432MatCensus)
            {
                g_g432MatUnpackNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g432T0).count());
                ++g_g432MatUnpackN;
                g_g432MatUnpackPx += static_cast<uint64_t>(rows) *
                                     static_cast<uint64_t>(r.fbW);
                if ((g_g432MatUnpackN % 2000u) == 0u)
                    std::fprintf(stderr,
                                 "[G432:matunpack] fast=%d n=%llu ms=%.1f Mpx=%.1f nsPerPx=%.2f\n",
                                 g432MatUnpackOn() ? 1 : 0,
                                 static_cast<unsigned long long>(g_g432MatUnpackN),
                                 static_cast<double>(g_g432MatUnpackNs) / 1.0e6,
                                 static_cast<double>(g_g432MatUnpackPx) / 1.0e6,
                                 g_g432MatUnpackPx
                                     ? static_cast<double>(g_g432MatUnpackNs) /
                                           static_cast<double>(g_g432MatUnpackPx)
                                     : 0.0);
            }
            g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW), static_cast<uint32_t>(rows));
            // FBO still equals VRAM for this target: refresh the snapshot past our own bump —
            // G351: for the FBO key actually READ; a mixed-band publication leaves neither
            // single FBO equal to VRAM, so both surface snapshots go conservative-invalid.
            if (g351ReadKey != 0u)
            {
                G178FbSnap &snap = g_g178FbSnap[g351ReadKey];
                snap.valid = true;
                if (r.covLo < snap.rowLo) snap.rowLo = r.covLo;
                if (r.covHi > snap.rowHi) snap.rowHi = r.covHi;
                snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                             static_cast<uint32_t>(r.fbW), 512u);
                if (g351ReadKey != fbp)
                {
                    G178FbSnap &phys = g_g178FbSnap[fbp];
                    phys.valid = false;
                    phys.genSum = 0;
                    phys.rowLo = 1 << 30;
                    phys.rowHi = -1;
                }
            }
            else
            {
                for (const uint32_t k : {fbp, kG351Sprite139})
                {
                    G178FbSnap &sn = g_g178FbSnap[k];
                    sn.valid = false;
                    sn.genSum = 0;
                    sn.rowLo = 1 << 30;
                    sn.rowHi = -1;
                }
            }
        }
    }
    if (!ok)
    {
        std::fprintf(stderr,
                     "[G261:materialize-fail] fbp=0x%x rows=%d..%d fbW=%d cause=%d — residency "
                     "dropped, VRAM stays authoritative\n",
                     fbp, r.dirtyLo, r.dirtyHi, r.fbW, cause);
        g_g261Mat[kG261MatFail].fetch_add(1u, std::memory_order_relaxed);
        // FBO↔VRAM equality is unknown now — force a fresh upload. Reset in place (callers may
        // hold live references into g_g178FbSnap; erase would invalidate them). G351: both
        // surface snapshots (the split routes 0x139 across two FBO keys).
        for (const uint32_t k : {fbp, kG351Sprite139})
        {
            G178FbSnap &failSnap = g_g178FbSnap[k];
            failSnap.valid = false;
            failSnap.genSum = 0;
            failSnap.rowLo = 1 << 30;
            failSnap.rowHi = -1;
            if (!(g351SplitOn() && fbp == 0x139u))
                break; // split off (or another target): original single-key behavior
        }
    }
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    g261UpdateWindow(ti); // window shrinks to cov rows; gen baseline re-anchored past our bump
    g310NoteTargetClean(ok);
    g_g261MatByTarget[ti & 7].fetch_add(1u, std::memory_order_relaxed);
    if (g279Profile)
    {
        g_g279MatNs[cause & 7].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279MatRows[cause & 7].fetch_add(
            static_cast<uint64_t>(std::max(rows, 0)), std::memory_order_relaxed);
    }
    const uint64_t n = g_g261Mat[cause & 7].fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 512u) == 0u)
        g261Report();
}

// ---- G276: within-drain display-color readback coalescing -------------------------------------
// Steady MAP-0 pays ~9 full-FBO color glReadPixels/frame (~30 ms/f, ~13% of the frame): each
// display batch (fbp 0x0/0x68) blocks on its own readback even when the very next batch draws into
// the SAME persistent FBO with no intervening VRAM-color consumer. The FBO accumulates across those
// batches, and the gen-snapshot already suppresses a re-upload for a same-target continuation (the
// readback does not bump color gens), so a consecutive same-target run needs exactly ONE readback,
// not K. G276 defers the readback (renderBatch skips the glReadPixels; the draw still lands in the
// FBO), records the pending union, and publishes it (one row-windowed g178_backend_read_color +
// swizzle -> VRAM) at the next different target OR any g272MaterializeAll consumer edge (GPU drain,
// CPU replay, inline primitive, drain end, l2h/l2l upload) — the same edge set G272 established.
// Scope is WITHIN A DRAIN (no persistent cross-frame residency), so the G242/G249/G272 cross-frame
// ownership trap cannot occur; VRAM is authoritative at exactly the boundaries it is today.
// (Lever g276DisplayRbOn(), the G276PendDisplay state, and its counters are declared earlier so
// g261Report can print them; only the flush body lives here where its readback helpers are visible.)

// Publish the pending coalesced display readback (union rows) into guest VRAM. Mirrors the per-batch
// color writeback exactly: row-windowed FBO readback, swizzle to VRAM, and NO color-gen bump (so the
// snapshot for a later same-target batch still equals current gens and re-upload stays suppressed —
// identical to today's per-batch display path). On any failure the snapshot is invalidated so the
// next flush re-uploads from VRAM (bounded stale-pixel, never a silent overwrite of newer bytes).
// Defined at global scope (region 4683..7342), static/internal linkage; called only from sites that
// are lexically after this point (the different-target site in g178TryFlushGpu and each
// g272MaterializeAll consumer-edge call site), so no forward declaration is needed.
static void g276FlushPendingDisplay(int cause)
{
    if (!s_g276Pend.active)
        return;
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    // Preserve the historical per-batch publication order when color and Z physically alias:
    // G262 wrote Z back before color. Range-disjoint owners remain independently coalescable.
    const G144BlockRange displayRange = g144RangeForRect(
        s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
        static_cast<uint32_t>(s_g276Pend.rowLo),
        static_cast<uint32_t>(s_g276Pend.fbW),
        static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &pz = s_g278PendTab[s];
        if (!pz.active)
            continue;
        if (pz.privateMirror)
            continue;
        const G144BlockRange depthWhole = g278PendingDepthWholeRangeP(pz);
        if (!depthWhole.valid || !displayRange.valid ||
            g144RangeOverlaps(depthWhole, displayRange))
        {
            if (!g278FlushPendSlot(pz, cause))
                return;
        }
    }
    if (!g278FlushPendingDepthForRanges(
            cause, G144BlockRange{}, displayRange, false, !displayRange.valid))
        return;
    if (g277CensusOn() && cause >= 0 && cause < 9)
        g_g277FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    G276PendDisplay p = s_g276Pend;
    s_g276Pend.active = false;
    s_g276Pend.rowLo = 1 << 30;
    s_g276Pend.rowHi = -1;
    uint8_t *vram = g_g144LastVram;
    const uint32_t vramSize = g_g261VramSize;
    const int rows = p.rowHi - p.rowLo + 1;
    bool ok = false;
    static std::vector<uint32_t> s_g276Px; // single-threaded flush-owner path (as g261Materialize)
    if (vram != nullptr && vramSize != 0u && p.fbW > 0 && rows > 0)
    {
        const int glY = 512 - 1 - p.rowHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const G455Ticket g455Ticket = g455BeginRead(
            kG455Display, s_g455DisplayWriteSeq, p.fbp, p.fbw, p.fbW, p.rowLo, p.rowHi);
        const bool g454Skip = g454SkipReadbackAt(kG454RbDisplayLeverId);
        if (g454Skip)
            s_g276Px.resize(static_cast<size_t>(p.fbW) * rows);
        const bool readOk = g454Skip ||
            g178_backend_read_color(p.fbp, p.fbW, 512, glY, rows, s_g276Px);
        g455EndRead(g455Ticket);
        if (g279Profile && cause >= 0 && cause < 9)
            g_g279DisplayReadNs[cause].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g276Px.size() == static_cast<size_t>(p.fbW) * rows;
        if (ok)
        {
            for (int y = p.rowLo; y <= p.rowHi; ++y)
            {
                const size_t srcRow = static_cast<size_t>(511 - y - glY) * p.fbW;
                for (int x = 0; x < p.fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(p.fbBlock, p.fbw,
                                                                static_cast<uint32_t>(x),
                                                                static_cast<uint32_t>(y));
                    if (off + 4u <= vramSize)
                        std::memcpy(vram + off, &s_g276Px[srcRow + x], 4);
                }
            }
            // G336: this display-color readback published guest VRAM rows rowLo..rowHi at
            // (fbBlock,fbw) as CT32 — capture it into the boundary's published set (no-op unless
            // DC2_G336_RUNWAY=1 and inside the boundary window).
            g336NotePublish(p.fbBlock, p.fbw, p.fbW, p.rowLo, p.rowHi);
        }
    }
    if (ok)
    {
        g_g276Flushes.fetch_add(1u, std::memory_order_relaxed);
        g_g276FlushRows.fetch_add(static_cast<uint64_t>(rows), std::memory_order_relaxed);
    }
    else
    {
        // FBO<->VRAM relation now unknown for this target — force a fresh upload next flush.
        g_g276FlushFail.fetch_add(1u, std::memory_order_relaxed);
        G178FbSnap &snap = g_g178FbSnap[p.fbp];
        snap.valid = false;
        snap.genSum = 0;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    if (g279Profile && cause >= 0 && cause < 9)
    {
        g_g279DisplayNs[cause].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279DisplayN[cause].fetch_add(1u, std::memory_order_relaxed);
    }
}

// ---- G277 census helpers (counters only, DC2_G277_CENSUS=1; state by s_g276Pend) ---------------
// Block range of the CURRENT pending coalesced display readback (invalid when none).
static G144BlockRange g277PendingDisplayRange()
{
    if (!s_g276Pend.active || s_g276Pend.fbW <= 0 || s_g276Pend.rowHi < s_g276Pend.rowLo)
        return {};
    return g144RangeForRect(s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
                            static_cast<uint32_t>(s_g276Pend.rowLo),
                            static_cast<uint32_t>(s_g276Pend.fbW),
                            static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
}

// G284: publish the pending coalesced display readback at a RANGED transfer edge (cause 7 l2h dst,
// cause 8 l2l src+dst) only if that transfer could observe or overwrite the pending display rows.
// Lever-off, unknown range, degenerate pending, or overlap => the original unconditional G276 flush
// (bit-identical to pre-G284). A provably-disjoint transfer keeps the display readback coalesced for
// the next REAL consumer edge — exactly how G278 already gates the depth readback at this same edge.
static void g276FlushPendingDisplayForRanges(int cause, const G144BlockRange &src,
                                             const G144BlockRange &dst, bool srcUnknown,
                                             bool dstUnknown)
{
    if (!s_g276Pend.active)
        return;
    if (g284UploadCoalesceOn() && !srcUnknown && !dstUnknown)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const bool overlaps = (src.valid && g144RangeOverlaps(pend, src)) ||
                              (dst.valid && g144RangeOverlaps(pend, dst));
        if (pend.valid && !overlaps)
        {
            if (g284StatOn())
                g_g284UploadSkipped.fetch_add(1u, std::memory_order_relaxed);
            return; // disjoint: keep the display readback coalesced
        }
    }
    if (g284StatOn())
        g_g284UploadFlushed.fetch_add(1u, std::memory_order_relaxed);
    g276FlushPendingDisplay(cause);
}

// G288: consumer-complete bridge for a G284-deferred display owner into the next native GPU
// batch. g178TryFlushGpu calls this before framebuffer upload, texture/CLUT decode, or depth setup.
// Same-target color writes deliberately continue in the persistent FBO (the proven G276 path);
// every other physical read/write range is checked against the unpublished display rows.
static void g288PublishDisplayForGpuBatch()
{
    if (!g288GpuConsumerOn() || !s_g276Pend.active || g_g144List.empty())
        return;

    g_g288GpuAudits.fetch_add(1u, std::memory_order_relaxed);
    const G144BlockRange pending = g277PendingDisplayRange();
    bool overlap = false;
    bool unknown = !pending.valid;
    uint32_t reason = unknown ? 0x10u : 0u; // 1=color, 2=depth, 4=texture, 8=CLUT, 16=unknown
    size_t hitEntry = 0u;

    for (size_t i = 0; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool sameDisplay =
            e.ctx.frame.fbp == s_g276Pend.fbp && e.ctx.frame.fbw == s_g276Pend.fbw;

        if (!sameDisplay)
        {
            const G144BlockRange color = g144TargetRange(e);
            const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                                  e.ctx.scissor.y1 >= e.ctx.scissor.y0;
            if (nonEmpty && !color.valid)
            {
                unknown = true;
                reason |= 0x11u;
            }
            else if (color.valid && g144RangeOverlaps(pending, color))
            {
                overlap = true;
                reason |= 0x1u;
            }
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(pending, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(pending, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(pending, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }

    if (!overlap && !unknown)
    {
        g_g288GpuDisjoint.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if (unknown)
        g_g288GpuUnknown.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t n =
        g_g288GpuPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g288StatOn() && (n <= 64u || (n % 256u) == 0u))
    {
        const G144Entry &e = g_g144List[std::min(hitEntry, g_g144List.size() - 1u)];
        std::fprintf(stderr,
                     "[G288:gpu-consumer] n=%llu reason=0x%x entry=%zu/%zu "
                     "pend(fbp=0x%x fbw=%u rows=%d..%d blocks=%x..%x) "
                     "draw(fbp=0x%x fbw=%u tme=%u tbp=0x%x psm=0x%x cbp=0x%x)\n",
                     static_cast<unsigned long long>(n), reason, hitEntry, g_g144List.size(),
                     s_g276Pend.fbp, s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                     pending.first, pending.last, e.ctx.frame.fbp, e.ctx.frame.fbw,
                     e.prim.tme ? 1u : 0u, e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.cbp);
    }
    g276FlushPendingDisplay(0);
}

// Private .cpp-to-.cpp bridge; keeping G289 out of the shared LLE header avoids widening the
// backend surface for one tightly admitted owner.
extern bool g289_backend_copy_display_to_work(uint32_t srcFbp, uint32_t dstFbp);
extern bool g289_backend_build_work_alias_view();
static bool g289Resolve14aBeforeDirect();

// The exact display-grab sprite previously proven by G288's direct-source prototype. The one-row
// bilinear halo is safe because CLAMP repeats GS row 415; no undefined FBO row is sampled.
static uint32_t g289DirectDisplayToken()
{
    if (!g289LocalCopyOn() || !s_g276Pend.active ||
        s_g276Pend.fbW != 512 || s_g276Pend.fbw != 8u ||
        s_g276Pend.rowLo != 0 || s_g276Pend.rowHi != 415 ||
        (s_g276Pend.fbp != 0u && s_g276Pend.fbp != 0x68u) ||
        g_g144List.size() != 1u)
        return 0u;

    const G144Entry &e = g_g144List[0];
    const uint32_t sourceBlock = framePageBaseToBlock(s_g276Pend.fbp);
    const bool exact =
        e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 447u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && e.prim.fst &&
        e.ctx.tex0.tbp0 == sourceBlock && e.ctx.tex0.tbw == 8u &&
        e.ctx.tex0.psm == GS_PSM_CT32 &&
        e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u &&
        tex1UsesLinearFilter(e.ctx.tex1) &&
        e.v[0].u == 0u && e.v[0].v == 0u &&
        e.v[1].u == 512u * 16u && e.v[1].v == 416u * 16u;
    if (!exact)
        return 0u;
    if (g345AnyOwnerActive() && !g289Resolve14aBeforeDirect())
        return 0u;
    if (g345AnyOwnerActive())
        return 0u; // G346: another slot survived the 0x14a resolve — fail closed to normal path
    return s_g276Pend.fbp == 0u ? kG289ZeroFbpToken : s_g276Pend.fbp;
}

// G346: union across every active slot. Overlap consumers become fail-closed conservative for
// multi-slot states; closure-off (single active slot at most) this is the original range.
static G144BlockRange g289OwnerRange()
{
    G144BlockRange out;
    out.valid = false;
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        const G289CopyOwner &o = s_g289Owners[i];
        if (!o.active || o.firstOwnedBlock > o.lastOwnedBlock)
            continue;
        if (!out.valid)
        {
            out.first = o.firstOwnedBlock;
            out.last = o.lastOwnedBlock;
            out.valid = true;
        }
        else
        {
            out.first = std::min(out.first, o.firstOwnedBlock);
            out.last = std::max(out.last, o.lastOwnedBlock);
        }
    }
    return out;
}

static bool g345MaterializeSlot(G289CopyOwner &slot, int cause)
{
    if (!slot.active)
        return true;
    G289CopyOwner owner = slot;
    slot.active = false;
    if (&slot == &s_g289Owners[0])
        s_g289PendingUpload.active = false;

    static std::vector<uint32_t> s_px;
    const bool ok =
        owner.vram != nullptr && owner.vramSize != 0u &&
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 416, s_px) &&
        s_px.size() == 512u * 416u;
    if (ok)
    {
        const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
        for (uint32_t y = 0u; y < 416u; ++y)
        {
            const size_t srcRow = static_cast<size_t>(415u - y) * 512u;
            for (uint32_t x = 0u; x < 512u; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
                const uint32_t block = off >> 8u;
                if (block >= owner.firstOwnedBlock && block <= owner.lastOwnedBlock &&
                    off + 4u <= owner.vramSize)
                    std::memcpy(owner.vram + off, &s_px[srcRow + x], 4u);
            }
        }
        g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 0u, 512u, 416u);
        g_g289Materializes.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        g_g289MatFail.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G289:materialize-fail] version=%llu cause=%d vram=%p size=%u\n",
                     static_cast<unsigned long long>(owner.version), cause,
                     static_cast<void *>(owner.vram), owner.vramSize);
    }
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    if (ok && g345ClosureOn())
    {
        // G346 write-side retirement: the readback above published the FULL 416-row FBO content
        // and bumped those pages' generations, so FBO == VRAM for rows 0..415 of dstFbp right
        // now. Revalidating (the proven G272-materialize pattern) lets the next wave skip the
        // stale re-upload that G343 measured as the conserving edge. Non-closure keeps the
        // original conservative invalidation.
        snap.valid = true;
        snap.genSum = g178GenSumRect(framePageBaseToBlock(owner.dstFbp), 8u, GS_PSM_CT32,
                                     0u, 0u, 512u, 512u);
        snap.rowLo = std::min(snap.rowLo, 0);
        snap.rowHi = std::max(snap.rowHi, 415);
        g_g345SnapRevalidates.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    if (g345ClosureOn())
        g345NoteEscMat(cause, owner.dstFbp, 416u);
    if (g289StatOn())
    {
        const uint64_t n =
            g_g289Materializes.load(std::memory_order_relaxed) +
            g_g289MatFail.load(std::memory_order_relaxed);
        if (n <= 8u || (n % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:materialize] n=%llu version=%llu dst=%03x cause=%d ok=%u\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(owner.version), owner.dstFbp,
                         cause, ok ? 1u : 0u);
    }
    return ok;
}

// Fail-closed edge: resolve EVERY active owner slot (closure-off: only slot 0 can be active).
static bool g289MaterializeOwner(int cause)
{
    bool ok = true;
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
        if (s_g289Owners[i].active)
            ok = g345MaterializeSlot(s_g289Owners[i], cause) && ok;
    return ok;
}

// The measured 512x384 PSMT4 uploads and following CT32 page uploads replace complete physical
// pages at the front of the 0x14a owner. When one intersects that prefix without a gap, guest VRAM
// owns the replaced pages after IMAGE completes and the FBO keeps owning the untouched suffix.
static bool g289PrepareUploadOverwrite(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                       uint32_t dsax, uint32_t dsay,
                                       uint32_t rrw, uint32_t rrh,
                                       const G144BlockRange &dst)
{
    const bool exactT4 =
        dbw == 8u && dpsm == GS_PSM_T4 &&
        dsax == 0u && dsay == 0u && rrw == 512u && rrh == 384u;
    const bool exactCt32 =
        dbw == 2u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 128u && rrh == 128u;
    const bool exactCt32Tail =
        dbw == 1u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 64u && rrh == 64u;
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    if (o14a == nullptr ||
        (!exactT4 && !exactCt32 && !exactCt32Tail) ||
        !dst.valid || dst.first > o14a->firstOwnedBlock ||
        dst.last < o14a->firstOwnedBlock ||
        o14a->lastOwnedBlock != 0x363fu)
        return false;
    s_g289PendingUpload.active = true;
    s_g289PendingUpload.ownerVersion = o14a->version;
    s_g289PendingUpload.dbp = dbp;
    s_g289PendingUpload.dbw = dbw;
    s_g289PendingUpload.dpsm = dpsm;
    s_g289PendingUpload.dsax = dsax;
    s_g289PendingUpload.dsay = dsay;
    s_g289PendingUpload.rrw = rrw;
    s_g289PendingUpload.rrh = rrh;
    s_g289PendingUpload.replacedThroughBlock = dst.last;
    return true;
}

// Called after GS::processImageData has written the complete exact image.
void g289NoteUploadComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                            uint32_t dsax, uint32_t dsay,
                            uint32_t rrw, uint32_t rrh)
{
    if (!s_g289PendingUpload.active)
        return;
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    const bool exact =
        o14a != nullptr &&
        o14a->version == s_g289PendingUpload.ownerVersion &&
        dbp == s_g289PendingUpload.dbp &&
        dbw == s_g289PendingUpload.dbw &&
        dpsm == s_g289PendingUpload.dpsm &&
        dsax == s_g289PendingUpload.dsax &&
        dsay == s_g289PendingUpload.dsay &&
        rrw == s_g289PendingUpload.rrw &&
        rrh == s_g289PendingUpload.rrh;
    if (!exact)
    {
        s_g289PendingUpload.active = false;
        return;
    }
    o14a->firstOwnedBlock =
        std::max(o14a->firstOwnedBlock,
                 s_g289PendingUpload.replacedThroughBlock + 1u);
    s_g289PendingUpload.active = false;
    const bool fullyReplaced =
        o14a->firstOwnedBlock > o14a->lastOwnedBlock;
    if (fullyReplaced)
    {
        o14a->active = false;
        G178FbSnap &snap = g_g178FbSnap[o14a->dstFbp];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    const uint64_t n =
        g_g289UploadOverwrites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:upload-overwrite] n=%llu version=%llu "
                     "upload=%x..%x owner=%x..%x full=%u\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(o14a->version),
                     dbp, s_g289PendingUpload.replacedThroughBlock,
                     o14a->firstOwnedBlock, o14a->lastOwnedBlock,
                     fullyReplaced ? 1u : 0u);
}

// G314 cross-TU bridge from GS::processImageData. Arm first-consumer tracking only after every
// IMAGE chunk has landed; otherwise continuation chunks would be misclassified as consumers.
// This sits beside the proven G289 completion bridge so it has the same external linkage.
void g314NoteImageComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                           uint32_t dsax, uint32_t dsay,
                           uint32_t rrw, uint32_t rrh)
{
    if (!g314CensusOn() || !g_g314Pending.waitingForComplete)
        return;
    const bool exact = g_g314Pending.dbp == dbp && g_g314Pending.dbw == dbw &&
                       g_g314Pending.dpsm == dpsm && g_g314Pending.dsax == dsax &&
                       g_g314Pending.dsay == dsay && g_g314Pending.rrw == rrw &&
                       g_g314Pending.rrh == rrh;
    g_g314Pending.waitingForComplete = false;
    if (!exact || g_g314Pending.shape == nullptr || !g_g314Pending.range.valid)
    {
        g_g314Pending.shape = nullptr;
        return;
    }
    g_g314Pending.armed = true;
}

// The following exact fbp=0x139 display sprite overwrites physical pages 0x2720..0x341f.
// For the partially overwritten 0x14a owner, only page 0x3420 onward can survive. Publishing
// GS rows 320..415 covers that complete suffix (plus harmless bounded over-work), after which
// the normal direct sprite may execute with no unresolved alias owner.
static bool g289Resolve14aBeforeDirect()
{
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    if (o14a == nullptr ||
        o14a->firstOwnedBlock < 0x29a0u ||
        o14a->firstOwnedBlock > o14a->lastOwnedBlock ||
        o14a->lastOwnedBlock != 0x363fu ||
        o14a->vram == nullptr || o14a->vramSize == 0u)
        return false;

    G289CopyOwner owner = *o14a;
    static std::vector<uint32_t> s_px;
    const bool readOk =
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 96, s_px) &&
        s_px.size() == 512u * 96u;
    if (!readOk)
        return g289MaterializeOwner(12);

    const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
    for (uint32_t y = 320u; y < 416u; ++y)
    {
        const size_t srcRow = static_cast<size_t>(415u - y) * 512u;
        for (uint32_t x = 0u; x < 512u; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
            const uint32_t block = off >> 8u;
            if (block >= owner.firstOwnedBlock && block <= owner.lastOwnedBlock &&
                off + 4u <= owner.vramSize)
                std::memcpy(owner.vram + off, &s_px[srcRow + x], 4u);
        }
    }
    g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 320u, 512u, 96u);
    o14a->active = false;
    s_g289PendingUpload.active = false;
    if (g345StatOn())
        g345NoteFamReadbackFbp(owner.dstFbp, 96u);
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    const uint64_t n =
        g_g289SuffixPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:suffix-publish] n=%llu version=%llu dst=14a rows=320..415\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(owner.version));
    return true;
}

static void g289MaterializeForRanges(const G144BlockRange &src,
                                     const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (srcUnknown || dstUnknown)
    {
        if (g345AnyOwnerActive())
            (void)g289MaterializeOwner(cause);
        return;
    }
    // G346 partial escape (contract I3): only the slots the ranges actually touch pay; the rest
    // stay resident. Closure-off at most slot 0 is active, so behavior is unchanged.
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        G144BlockRange range;
        range.first = o.firstOwnedBlock;
        range.last = o.lastOwnedBlock;
        range.valid = o.firstOwnedBlock <= o.lastOwnedBlock;
        if (!range.valid ||
            (src.valid && g144RangeOverlaps(range, src)) ||
            (dst.valid && g144RangeOverlaps(range, dst)))
            (void)g345MaterializeSlot(o, cause);
    }
}

static void g310NotePresentationRead(uint32_t fbp, uint32_t fbw, uint32_t psm,
                                     uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     bool useLocalMemoryLayout, bool frameBaseIsPages)
{
    if (!g310StatOn() || w == 0u || h == 0u)
        return;
    g_g310PresentReads.fetch_add(1u, std::memory_order_relaxed);
    if (!useLocalMemoryLayout)
    {
        g_g310PresentUnknown.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t baseBlock = frameBaseIsPages ? framePageBaseToBlock(fbp) : fbp;
    const G144BlockRange read =
        g144RangeForRect(baseBlock, static_cast<uint8_t>(psm),
                         fbw != 0u ? fbw : 1u, x, y, w, h);
    if (!read.valid)
    {
        g_g310PresentUnknown.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    static constexpr uint32_t kStripFirst = 0x2720u;
    static constexpr uint32_t kStripLast = 0x371fu;
    const uint32_t lo = std::max(read.first, kStripFirst);
    const uint32_t hi = std::min(read.last, kStripLast);
    if (lo <= hi)
    {
        g_g310PresentOverlap.fetch_add(1u, std::memory_order_relaxed);
        g_g310PresentPages.fetch_add(
            static_cast<uint64_t>((hi >> 5u) - (lo >> 5u) + 1u),
            std::memory_order_relaxed);
    }
}

// Private .cpp-to-.cpp presentation bridge. Normal presentation may read the display frames,
// preferred source, or a non-black context fallback. Audit the exact source rectangle at the
// point of read; an overlap or layout we cannot prove materializes before the host copy.
void g289_prepare_presentation_read(uint8_t *vram,
                                    uint32_t fbp, uint32_t fbw, uint32_t psm,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    bool useLocalMemoryLayout, bool frameBaseIsPages)
{
    g310NotePresentationRead(
        fbp, fbw, psm, x, y, w, h, useLocalMemoryLayout, frameBaseIsPages);
    if (!g345AnyOwnerActive() || w == 0u || h == 0u)
        return;
    bool vramMismatch = !useLocalMemoryLayout;
    for (uint32_t i = 0u; i < kG345OwnerSlots && !vramMismatch; ++i)
        if (s_g289Owners[i].active && vram != s_g289Owners[i].vram)
            vramMismatch = true;
    if (vramMismatch)
    {
        (void)g289MaterializeOwner(13);
        return;
    }
    const uint32_t baseBlock = frameBaseIsPages ? framePageBaseToBlock(fbp) : fbp;
    const G144BlockRange read =
        g144RangeForRect(baseBlock, static_cast<uint8_t>(psm),
                         fbw != 0u ? fbw : 1u, x, y, w, h);
    g289MaterializeForRanges(read, G144BlockRange{}, !read.valid, false, 13);
}

struct G289InlineAudit
{
    bool carry = false;
    bool empty = false;
    bool unknown = false;
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    G144BlockRange color{}, depth{}, texture{}, clut{};
};

// Range-exact inline consumer edge. A clipped-out primitive touches nothing; otherwise the
// scissor-clipped target/depth bbox and whole texture/CLUT footprints must all provably miss the
// surviving owner. Unknown ranges and every overlap retain fail-closed cause-4 materialization.
static G289InlineAudit g289AuditInline(const GSPrimReg &prim, const GSVertex *verts,
                                       const GSContext &ctx, const GSTexClutReg &texclut)
{
    G289InlineAudit out;
    int count = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: count = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: count = 2; break;
    default: count = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minX = verts[0].x, maxX = verts[0].x;
    float minY = verts[0].y, maxY = verts[0].y;
    for (int i = 1; i < count; ++i)
    {
        minX = std::min(minX, verts[i].x);
        maxX = std::max(maxX, verts[i].x);
        minY = std::min(minY, verts[i].y);
        maxY = std::max(maxY, verts[i].y);
    }
    out.x0 = std::max(static_cast<int>(std::floor(minX)) - ofx,
                      static_cast<int>(ctx.scissor.x0));
    out.x1 = std::min(static_cast<int>(std::ceil(maxX)) - ofx,
                      static_cast<int>(ctx.scissor.x1));
    out.y0 = std::max(static_cast<int>(std::floor(minY)) - ofy,
                      static_cast<int>(ctx.scissor.y0));
    out.y1 = std::min(static_cast<int>(std::ceil(maxY)) - ofy,
                      static_cast<int>(ctx.scissor.y1));
    if (out.x1 < out.x0 || out.y1 < out.y0)
    {
        out.empty = true;
        out.carry = true;
        return out;
    }

    const uint32_t x = static_cast<uint32_t>(out.x0);
    const uint32_t y = static_cast<uint32_t>(out.y0);
    const uint32_t w = static_cast<uint32_t>(out.x1 - out.x0 + 1);
    const uint32_t h = static_cast<uint32_t>(out.y1 - out.y0 + 1);
    out.color = g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                                 ctx.frame.fbw, x, y, w, h);
    const bool depthActive = ((ctx.test >> 16u) & 1u) != 0u ||
                             ((ctx.zbuf >> 32u) & 1u) == 0u;
    if (depthActive)
        out.depth = g144RangeForRect(
            static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5u,
            static_cast<uint8_t>((ctx.zbuf >> 24u) & 0xFu), ctx.frame.fbw,
            x, y, w, h);
    if (prim.tme)
    {
        G144Entry cand{};
        cand.ctx = ctx;
        cand.prim = prim;
        cand.texclut = texclut;
        out.texture = g144TextureRange(cand);
        if (g144IsPalettedPsm(ctx.tex0.psm))
            out.clut = g144ClutRange(cand);
    }
    out.unknown = !out.color.valid ||
                  (depthActive && !out.depth.valid) ||
                  (prim.tme && !out.texture.valid) ||
                  (prim.tme && g144IsPalettedPsm(ctx.tex0.psm) && !out.clut.valid);
    const G144BlockRange owner = g289OwnerRange();
    out.carry = !out.unknown && owner.valid &&
                !g144RangeOverlaps(owner, out.color) &&
                !g144RangeOverlaps(owner, out.depth) &&
                !g144RangeOverlaps(owner, out.texture) &&
                !g144RangeOverlaps(owner, out.clut);
    return out;
}

// G346: the exact display-grab shape (the l2l family this slice closes), src display fbp derived
// from sbp — never from the pend (the closure arm can admit with the pend already flushed).
static bool g345GrabShape(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                          uint32_t ssax, uint32_t ssay,
                          uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                          uint32_t dsax, uint32_t dsay,
                          uint32_t rrw, uint32_t rrh, uint32_t &srcFbpOut)
{
    srcFbpOut = 0xffffffffu;
    const bool shape =
        sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
        (dbp == framePageBaseToBlock(0x13bu) ||
         dbp == framePageBaseToBlock(0x14au)) &&
        dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
        rrw == 512u && rrh == 416u;
    if (!shape)
        return false;
    if (sbp == framePageBaseToBlock(0u))
        srcFbpOut = 0u;
    else if (sbp == framePageBaseToBlock(0x68u))
        srcFbpOut = 0x68u;
    else
        return false;
    return true;
}

// G346 closure source authority: the src display FBO provably equals guest VRAM for rows 0..415
// (valid snapshot covering the rows whose stored gen sum still matches the CURRENT sum, computed
// with the same window the wave upload gate uses — the G277 416-row aligned window for the 0x68
// tuple). Then the GPU FBO->FBO copy reads exactly the bytes the CPU copy would read from VRAM;
// a pending deferred readback only means the FBO is NEWER, which is still the copy's source of
// truth. Any doubt fails closed to the CPU copy.
static bool g345SnapshotSourceAuthoritative(uint32_t srcFbp, uint32_t sbp)
{
    const auto it = g_g178FbSnap.find(srcFbp);
    if (it == g_g178FbSnap.end())
        return false;
    const G178FbSnap &snap = it->second;
    if (!snap.valid || snap.rowLo > 0 || snap.rowHi < 415)
        return false;
    const uint64_t genNow = (srcFbp == 0x68u && snap.rowHi <= 415)
        ? g277GenSumAligned(sbp, 8u, 416u)
        : g178GenSumRect(sbp, 8u, GS_PSM_CT32, 0u, 0u, 512u, 512u);
    return genNow == snap.genSum;
}

static bool g289CanDeferLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                  uint32_t ssax, uint32_t ssay,
                                  uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay,
                                  uint32_t rrw, uint32_t rrh)
{
    bool can = g289LocalCopyOn() && !s_g289Owner.active && s_g276Pend.active &&
           s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
           s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
           framePageBaseToBlock(s_g276Pend.fbp) == sbp &&
           sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
           (dbp == framePageBaseToBlock(0x13bu) ||
            dbp == framePageBaseToBlock(0x14au)) &&
           dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
           rrw == 512u && rrh == 416u;
    if (!can && g345ClosureOn() && g289LocalCopyOn())
    {
        // G346 widened admission: (a) a secondary slot or a same-dst owner reuse covers the
        // single-slot reject; (b) an exact pend for this src OR snapshot source authority covers
        // the pend-inactive reject. Everything else still falls to the CPU copy (counted escape).
        uint32_t srcFbp = 0xffffffffu;
        if (g345GrabShape(sbp, sbw, spsm, ssax, ssay,
                          dbp, dbw, dpsm, dsax, dsay, rrw, rrh, srcFbp))
        {
            const bool pendSrc = s_g276Pend.active &&
                s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
                s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
                s_g276Pend.fbp == srcFbp;
            const bool snapSrc = !pendSrc && g345SnapshotSourceAuthoritative(srcFbp, sbp);
            const uint32_t dstFbp = dbp >> 5u;
            const bool slotOk =
                g345OwnerFor(dstFbp) != nullptr || g345FreeSlot() != nullptr;
            can = (pendSrc || snapSrc) && slotOk;
        }
    }
    if (!can && g289LocalCopyOn() && s_g276Pend.active && g289StatOn())
    {
        static std::atomic<uint32_t> s_logs{0};
        const uint32_t n = s_logs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 16u)
            std::fprintf(
                stderr,
                "[G289:defer-reject] n=%u owner=%u pend=%x/%u/%d..%d/%d "
                "copy(src=%x/%u/%x/%u,%u dst=%x/%u/%x/%u,%u size=%ux%u)\n",
                n + 1u, s_g289Owner.active ? 1u : 0u, s_g276Pend.fbp,
                s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                s_g276Pend.fbW, sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh);
    }
    return can;
}

// Called by GS::performLocalToLocalTransfer after all graph/range barriers have completed. A false
// result leaves the caller on its unchanged legacy copy. A true result means the exact logical
// write lives in fbp=0x13b's FBO until the immediately measured G287 consumer or a fail-closed
// materialization edge.
bool g289TryGpuLocalCopy(uint8_t *vram, uint32_t vramSize,
                         uint32_t sbp, uint8_t sbw, uint8_t spsm,
                         uint32_t ssax, uint32_t ssay,
                         uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                         uint32_t dsax, uint32_t dsay,
                         uint32_t rrw, uint32_t rrh)
{
    uint32_t g345SrcFbp = 0xffffffffu;
    const bool g345Grab = g345GrabShape(sbp, sbw, spsm, ssax, ssay,
                                        dbp, dbw, dpsm, dsax, dsay, rrw, rrh, g345SrcFbp);
    if (g345Grab)
        g_g345GrabEdges.fetch_add(1u, std::memory_order_relaxed);
    if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                               dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
    {
        if (g345Grab)
        {
            // Aggregate reject-cause census (the G291 rule — never trust the capped early prints).
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
            if (!g289LocalCopyOn())
                g_g345RejOther.fetch_add(1u, std::memory_order_relaxed);
            else if (g345OwnerFor(dbp >> 5u) == nullptr && g345FreeSlot() == nullptr)
                g_g345RejOwnerBusy.fetch_add(1u, std::memory_order_relaxed);
            else if (!s_g276Pend.active)
                g_g345RejPendInactive.fetch_add(1u, std::memory_order_relaxed);
            else
                g_g345RejPendMismatch.fetch_add(1u, std::memory_order_relaxed);
        }
        return false;
    }

    // Source fbp: derived from sbp (grab-shaped whenever admission passed); the legacy pend
    // admission implies the same value it used before (blockOf(pend.fbp) == sbp).
    const uint32_t sourceFbp = g345Grab ? g345SrcFbp : s_g276Pend.fbp;
    const uint32_t destFbp = dbp >> 5u;
    if (!g289_backend_copy_display_to_work(sourceFbp, destFbp))
    {
        const uint64_t n =
            g_g289CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        std::fprintf(stderr,
                     "[G289:copy-fail] n=%llu src=0x%x dst=0x%x; legacy fallback\n",
                     static_cast<unsigned long long>(n), sourceFbp, destFbp);
        g276FlushPendingDisplay(8);
        if (g345Grab)
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    // G346 slot select: reuse the same-dst active owner (its whole range is superseded by this
    // identical-range copy) else take a free slot. Admission guaranteed one exists.
    G289CopyOwner *slot = g345OwnerFor(destFbp);
    const bool reused = slot != nullptr;
    if (slot == nullptr)
        slot = g345FreeSlot();
    if (slot == nullptr)
    {
        // Cannot represent ownership — fail closed: publish everything, let the CPU copy run.
        (void)g289MaterializeOwner(14);
        g276FlushPendingDisplay(8);
        if (g345Grab)
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const G144BlockRange ownerRange =
        g144RangeForRect(dbp, GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
    // G346 owner-supersede (contract I1): this copy destructively overwrites every physical block
    // in ownerRange, so any OTHER active owner's claim on those blocks is dead. A superseded
    // PREFIX trims (the carried 0x14a owner vs a new 0x13b grab); full coverage deactivates; any
    // other overlap shape fails closed to materialization of that slot.
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active || &o == slot)
            continue;
        if (o.firstOwnedBlock > o.lastOwnedBlock ||
            ownerRange.last < o.firstOwnedBlock || ownerRange.first > o.lastOwnedBlock)
            continue; // disjoint
        if (ownerRange.first <= o.firstOwnedBlock && ownerRange.last >= o.lastOwnedBlock)
        {
            o.active = false; // fully superseded
            g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (ownerRange.first <= o.firstOwnedBlock && ownerRange.last < o.lastOwnedBlock)
        {
            o.firstOwnedBlock = ownerRange.last + 1u; // prefix superseded, suffix survives
            g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            (void)g345MaterializeSlot(o, 15); // unsupported overlap shape — fail closed
        }
    }
    if (reused)
        g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
    if (g345Grab)
    {
        g_g345GrabGpu.fetch_add(1u, std::memory_order_relaxed);
        if (slot != &s_g289Owners[0])
            g_g345Copies2nd.fetch_add(1u, std::memory_order_relaxed);
        if (!(s_g276Pend.active && s_g276Pend.fbp == sourceFbp))
            g_g345SnapAdmits.fetch_add(1u, std::memory_order_relaxed);
    }
    slot->active = true;
    ++slot->version;
    slot->srcFbp = sourceFbp;
    slot->dstFbp = destFbp;
    slot->firstOwnedBlock = ownerRange.first;
    slot->lastOwnedBlock = ownerRange.last;
    slot->vram = vram;
    slot->vramSize = vramSize;
    s_g289PendingUpload.active = false; // unconditional, exactly as the single-slot code did
    const uint64_t copyN =
        g_g289Copies.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (copyN <= 16u || (copyN % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:copy] n=%llu version=%llu src=0x%x dst=0x%x rows=0..415\n",
                     static_cast<unsigned long long>(copyN),
                     static_cast<unsigned long long>(slot->version), sourceFbp,
                     destFbp);

    if (g289VerifyOn())
    {
        // Make the legacy source bytes authoritative, materialize the GPU copy, then compare the
        // two exact CT32 transfer views pixel-for-pixel. This deliberately disables the payoff.
        g276FlushPendingDisplay(8);
        const bool matOk = g289MaterializeOwner(9);
        uint64_t bad = matOk ? 0u : 512u * 416u;
        const uint32_t srcBlock = framePageBaseToBlock(sourceFbp);
        const uint32_t dstBlock = framePageBaseToBlock(destFbp);
        if (matOk)
        {
            for (uint32_t y = 0u; y < 416u; ++y)
                for (uint32_t x = 0u; x < 512u; ++x)
                {
                    uint32_t srcPx = 0u, dstPx = 0u;
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(srcBlock, 8u, x, y);
                    const uint32_t dstOff = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
                    if (srcOff + 4u > vramSize || dstOff + 4u > vramSize)
                    {
                        ++bad;
                        continue;
                    }
                    std::memcpy(&srcPx, vram + srcOff, 4u);
                    std::memcpy(&dstPx, vram + dstOff, 4u);
                    if (srcPx != dstPx)
                        ++bad;
                }
        }
        const uint64_t checks =
            g_g289VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g289VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (checks <= 16u || bad != 0u || (checks % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:verify] checks=%llu bad=%llu totalBad=%llu firstUse=%u\n",
                         static_cast<unsigned long long>(checks),
                         static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(
                             g_g289VerifyBad.load(std::memory_order_relaxed)),
                         checks == 1u ? 1u : 0u);
    }
    return true;
}

static bool g289CanConsumeOwner(uint8_t *vram, uint32_t vramSize)
{
    const G289CopyOwner *o13b = g345OwnerFor(0x13bu);
    if (o13b == nullptr || o13b->vram != vram ||
        o13b->vramSize != vramSize ||
        g_g144List.size() != 1u)
    {
        if (g345AnyOwnerActive() && g289StatOn())
        {
            static std::atomic<uint32_t> s_earlyRejectLogs{0};
            const uint32_t n = s_earlyRejectLogs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 8u)
                std::fprintf(
                    stderr,
                    "[G289:consume-early-reject] n=%u ownerVram=%p/%u vram=%p/%u entries=%zu\n",
                    n + 1u,
                    static_cast<void *>(o13b != nullptr ? o13b->vram : nullptr),
                    o13b != nullptr ? o13b->vramSize : 0u,
                    static_cast<void *>(vram), vramSize, g_g144List.size());
        }
        return false;
    }
    const G144Entry &e = g_g144List[0];
    const G144BlockRange tex = g144TextureRange(e);
    const G144BlockRange clut = g144ClutRange(e);
    const G144BlockRange owner = g289OwnerRange();
    const bool exact =
        e.ctx.frame.fbp == 0x13bu && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 415u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && !e.prim.abe &&
        e.ctx.tex0.tbp0 == 0x2720u && e.ctx.tex0.tbw == 2u &&
        e.ctx.tex0.psm == GS_PSM_T8 && e.ctx.tex0.tw == 7u && e.ctx.tex0.th == 7u &&
        e.ctx.tex0.cbp == 0x3fbcu && e.ctx.tex0.cpsm == GS_PSM_CT32 &&
        owner.valid && tex.valid && clut.valid &&
        !g144RangeOverlaps(owner, tex) && !g144RangeOverlaps(owner, clut);
    if (!exact && g289StatOn())
    {
        static std::atomic<uint32_t> s_rejectLogs{0};
        const uint32_t n = s_rejectLogs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(
                stderr,
                "[G289:consume-reject] n=%u frame=%03x/%u/%x sc=%u,%u..%u,%u "
                "prim=%u tme=%u abe=%u tex=%x/%u/%x/%u/%u clut=%x/%x "
                "range(owner=%x..%x tex=%x..%x clut=%x..%x)\n",
                n + 1u, e.ctx.frame.fbp, static_cast<unsigned>(e.ctx.frame.fbw),
                e.ctx.frame.psm, e.ctx.scissor.x0, e.ctx.scissor.y0,
                e.ctx.scissor.x1, e.ctx.scissor.y1, static_cast<unsigned>(e.prim.type),
                e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u, e.ctx.tex0.tbp0,
                static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm,
                static_cast<unsigned>(e.ctx.tex0.tw), static_cast<unsigned>(e.ctx.tex0.th),
                e.ctx.tex0.cbp, e.ctx.tex0.cpsm, owner.first, owner.last,
                tex.first, tex.last, clut.first, clut.last);
    }
    return exact;
}

// A versioned work-page owner may cross arbitrary native batches, not just the immediately next
// flush. Keep it GPU-local only when every color/depth/texture/CLUT range is known and disjoint;
// overlap or an unresolvable range fails closed to guest-VRAM materialization.
static bool g289BatchDisjointOwner(uint32_t &reason, size_t &hitEntry)
{
    const G144BlockRange owner = g289OwnerRange();
    reason = owner.valid ? 0u : 0x10u;
    hitEntry = 0u;
    bool overlap = false;
    bool unknown = !owner.valid;
    for (size_t i = 0u; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                              e.ctx.scissor.y1 >= e.ctx.scissor.y0;
        const G144BlockRange color = g144TargetRange(e);
        if (nonEmpty && !color.valid)
        {
            unknown = true;
            reason |= 0x11u;
        }
        else if (color.valid && g144RangeOverlaps(owner, color))
        {
            overlap = true;
            reason |= 0x1u;
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(owner, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(owner, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(owner, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }
    return !overlap && !unknown;
}

static bool g289OwnerAliasTuple(const G144Entry &e)
{
    const bool triangle =
        e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
        e.prim.type == GS_PRIM_TRIFAN;
    const bool format =
        e.ctx.tex0.psm == GS_PSM_CT32 || e.ctx.tex0.psm == GS_PSM_CT24;
    const bool ct24Alpha =
        e.ctx.tex0.psm != GS_PSM_CT24 ||
        (e.ctx.tex0.tcc == 1u && e.texa.ta0 == 128u &&
         !e.texa.aem && e.texa.ta1 == 128u);
    return triangle && e.prim.tme && e.prim.fst && format && ct24Alpha &&
           (e.ctx.tex0.tbp0 == 0x2720u || e.ctx.tex0.tbp0 == 0x2760u) &&
           e.ctx.tex0.tbw == 8u && e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u;
}

// The measured final-composition batch may read the copied work pages through two CT32-page
// aliases. This coarse pass runs before classification and admits no bytes yet; it only defers
// fail-closed materialization until exact per-entry tap footprints are available.
static bool g289CanTryOwnerView(uint8_t *vram, uint32_t vramSize)
{
    const G289CopyOwner *o13b = g345OwnerFor(0x13bu);
    if (o13b == nullptr || o13b->vram != vram ||
        o13b->vramSize != vramSize || g_g144List.empty())
        return false;
    const uint32_t fbp = g_g144List[0].ctx.frame.fbp;
    if (fbp != 0u && fbp != 0x68u)
        return false;
    const G144BlockRange owner = g289OwnerRange();
    if (!owner.valid)
        return false;
    bool hit = false;
    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        if (!color.valid || g144RangeOverlaps(owner, color) ||
            (depth.active && (!depth.range.valid || g144RangeOverlaps(owner, depth.range))))
            return false;
        if (!e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        if (!tex.valid)
            return false;
        if (g144RangeOverlaps(owner, tex))
        {
            if (!g289OwnerAliasTuple(e))
                return false;
            hit = true;
        }
        if (g144IsPalettedPsm(e.ctx.tex0.psm))
        {
            const G144BlockRange clut = g144ClutRange(e);
            if (!clut.valid || g144RangeOverlaps(owner, clut))
                return false;
        }
    }
    return hit;
}

struct G289ConsumeGuard
{
    bool armed = false;
    ~G289ConsumeGuard()
    {
        if (armed && g345OwnerFor(0x13bu) != nullptr)
        {
            g_g289Unexpected.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(10);
        }
    }
};

struct G289OwnerViewGuard
{
    bool armed = false;
    ~G289OwnerViewGuard()
    {
        if (armed && g345OwnerFor(0x13bu) != nullptr)
        {
            g_g289OwnerViewFail.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(11);
        }
    }
};

struct G289DirectGuard
{
    bool armed = false;
    ~G289DirectGuard()
    {
        if (armed && s_g276Pend.active)
            g276FlushPendingDisplay(0);
    }
};

static void g277CensusPrintMaybe()
{
    static uint64_t s_events = 0; // flush-owner thread only
    if ((++s_events % 4096u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G277:census] g276flush(c0=%llu c3=%llu c4=%llu c5=%llu c6=%llu c7=%llu c8=%llu) "
                 "inl(empty=%llu noovl=%llu ovl=%llu z=%llu) "
                 "e7(n=%llu skip=%llu) e8(n=%llu skip=%llu) "
                 "zrb(n=%llu coalescable=%llu)\n",
                 (unsigned long long)g_g277FlushByCause[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlEmpty.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlNoOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlZ.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRbCoalescable.load(std::memory_order_relaxed));
}

// Hard consumer edge (cause 3 CPU replay, 5 frame boundary, 6 un-ranged upload): always a real
// VRAM consumer — breaks the simulated Z-coalescing run.
static void g277CensusHardEdge(int cause)
{
    if (!g277CensusOn())
        return;
    if (cause >= 0 && cause < 9)
        g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    s_g277ZRunLive = false;
}

// Ranged transfer edge (cause 7 l2h dst, cause 8 l2l src+dst): skippable for the pending display
// flush when the transfer provably misses the pending rows; breaks the Z run only on Z overlap.
static void g277CensusRangeEdge(int cause, const G144BlockRange &src, const G144BlockRange &dst,
                                bool srcUnknown, bool dstUnknown)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    if (srcUnknown || dstUnknown)
    {
        s_g277ZRunLive = false;
        g277CensusPrintMaybe();
        return;
    }
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        if (pend.valid && !g144RangeOverlaps(pend, src) && !g144RangeOverlaps(pend, dst))
            g_g277EdgeSkippable[cause].fetch_add(1u, std::memory_order_relaxed);
    }
    if (s_g277ZRunLive &&
        (g144RangeOverlaps(s_g277ZRunRange, src) || g144RangeOverlaps(s_g277ZRunRange, dst)))
        s_g277ZRunLive = false;
    g277CensusPrintMaybe();
}

// Inline-primitive consumer edge (cause 4). Classifies by the prim's CLIPPED screen bbox: a fully
// scissored-out prim (the offscreen sceGsSwapDBuff sync linestrips, vtx x∈[1792,2303]) rasterizes
// nothing, reads nothing, writes nothing — a phantom consumer. Non-empty prims are classified by
// block-range overlap (target/Z/texture/CLUT vs the pending display rows), and break the simulated
// Z run only when they actually test/write Z.
static void g277CensusInlineEdge(const GSPrimReg &prim, const GSVertex *verts,
                                 const GSContext &ctx, const GSTexClutReg &texclut)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[4].fetch_add(1u, std::memory_order_relaxed);
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x, minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x); maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y); maxYf = std::max(maxYf, verts[i].y);
    }
    const int bx0 = static_cast<int>(std::floor(minXf)) - ofx;
    const int bx1 = static_cast<int>(std::ceil(maxXf)) - ofx;
    const int by0 = static_cast<int>(std::floor(minYf)) - ofy;
    const int by1 = static_cast<int>(std::ceil(maxYf)) - ofy;
    const int cx0 = std::max(bx0, static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(bx1, static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(by0, static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(by1, static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
    {
        g_g277InlEmpty.fetch_add(1u, std::memory_order_relaxed); // phantom: draws nothing
        g277CensusPrintMaybe();
        return;
    }
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    const uint64_t ts = ctx.test;
    const bool zte = ((ts >> 16) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32) & 1u) != 0u;
    if ((zte && ztst >= 2u) || !zmsk)
    {
        g_g277InlZ.fetch_add(1u, std::memory_order_relaxed);
        if (s_g277ZRunLive)
        {
            const uint32_t zbpBlock = (static_cast<uint32_t>(ctx.zbuf) & 0x1FFu) << 5u;
            const G144BlockRange zr = g144RangeForRect(zbpBlock, GS_PSM_Z24, ctx.frame.fbw,
                                                       static_cast<uint32_t>(cx0),
                                                       static_cast<uint32_t>(cy0), w, h);
            if (!zr.valid || g144RangeOverlaps(s_g277ZRunRange, zr))
                s_g277ZRunLive = false;
        }
    }
    bool ovl = false;
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const uint32_t fbBlock = framePageBaseToBlock(ctx.frame.fbp);
        const G144BlockRange tgt = g144RangeForRect(fbBlock, ctx.frame.psm, ctx.frame.fbw,
                                                    static_cast<uint32_t>(cx0),
                                                    static_cast<uint32_t>(cy0), w, h);
        ovl = !pend.valid || !tgt.valid || g144RangeOverlaps(pend, tgt);
        if (!ovl && prim.tme)
        {
            const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
            const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
            const G144BlockRange tex =
                g144RangeForRect(ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw, 0u, 0u, texW, texH);
            ovl = !tex.valid || g144RangeOverlaps(pend, tex);
            if (!ovl && g144IsPalettedPsm(ctx.tex0.psm))
            {
                const uint32_t clutBw = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
                const G144BlockRange clut =
                    g144RangeForRect(ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                                     static_cast<uint32_t>(texclut.cou),
                                     static_cast<uint32_t>(texclut.cov), 16u, 16u);
                ovl = !clut.valid || g144RangeOverlaps(pend, clut);
            }
        }
        if (ovl)
            g_g277InlOvl.fetch_add(1u, std::memory_order_relaxed);
        else
            g_g277InlNoOvl.fetch_add(1u, std::memory_order_relaxed);
    }
    g277CensusPrintMaybe();
}

// ---- G278: within-drain depth-readback coalescing ----------------------------------------------
// A G262 write wave leaves its Z result in the per-key backend depth texture and extends this
// pending row union. Same-key waves render directly against that texture, without uploading stale
// VRAM. The union is published before the first actual VRAM-Z consumer and unconditionally at the
// drain/presentation boundary. This is deliberately NOT G242 ownership: no state crosses a frame.
static G144BlockRange g278PendingDepthRangeP(const G278PendDepth &p)
{
    if (!p.active || p.fbW <= 0 || p.rowHi < p.rowLo)
        return {};
    return g144RangeForRect(p.zbpPage << 5u, GS_PSM_Z24,
                            p.fbw, 0u,
                            static_cast<uint32_t>(p.rowLo),
                            static_cast<uint32_t>(p.fbW),
                            static_cast<uint32_t>(p.rowHi - p.rowLo + 1));
}

static G144BlockRange g278PendingDepthWholeRangeP(const G278PendDepth &p)
{
    if (!p.active || p.fbW <= 0)
        return {};
    return g144RangeForRect(p.zbpPage << 5u, GS_PSM_Z24,
                            p.fbw, 0u, 0u,
                            static_cast<uint32_t>(p.fbW), 512u);
}

static bool g278StaleRowsAny(const G278PendDepth &p)
{
    for (uint64_t bits : p.staleRows)
        if (bits != 0u)
            return true;
    return false;
}

static bool g278StaleRowsOverlapP(const G278PendDepth &p, int lo, int hi)
{
    if (!p.active || hi < lo)
        return false;
    lo = std::max(0, lo);
    hi = std::min(511, hi);
    for (int y = lo; y <= hi; ++y)
        if ((p.staleRows[static_cast<size_t>(y >> 6)] >>
             static_cast<uint32_t>(y & 63)) & 1u)
            return true;
    return false;
}

static void g278MarkStaleRowsForRangeP(G278PendDepth &p, const G144BlockRange &write)
{
    // G411 private depth has no guest-VRAM storage range. GS texture/upload writes to physical
    // ZBP=0xd0 pages are unrelated staging traffic and cannot make the private mirror stale.
    if (!p.active || p.privateMirror || !write.valid)
        return;
    const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
    if (!whole.valid || !g144RangeOverlaps(whole, write))
        return;
    const uint32_t firstBlock = std::max(whole.first, write.first);
    const uint32_t lastBlock = std::min(whole.last, write.last);
    const uint32_t firstPage = (firstBlock - whole.first) >> 5u;
    const uint32_t lastPage = (lastBlock - whole.first) >> 5u;
    const uint32_t pagesPerRow = std::max(1u, p.fbw);
    const int lo = static_cast<int>(firstPage / pagesPerRow) * 32;
    const int hi = std::min(
        511, static_cast<int>(lastPage / pagesPerRow) * 32 + 31);
    for (int y = lo; y <= hi; ++y)
        p.staleRows[static_cast<size_t>(y >> 6)] |=
            1ull << static_cast<uint32_t>(y & 63);
    g_g278StaleMarks.fetch_add(1u, std::memory_order_relaxed);
}

// Mark every active slot whose whole range the write touches (each slot clamps internally).
static void g278MarkStaleRowsForRange(const G144BlockRange &write)
{
    for (int s = 0; s < kG278PendSlots; ++s)
        if (s_g278PendTab[s].active)
            g278MarkStaleRowsForRangeP(s_g278PendTab[s], write);
}

static bool g278StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G278_STAT");
    return s_on;
}

static void g278Report(uint64_t flushes)
{
    if (!g278StatOn() || (flushes > 8u && (flushes % 256u) != 0u))
        return;
    std::fprintf(
        stderr,
        "[G278:zrb] deferred=%llu flushes=%llu saved=%llu rows=%llu uploadSkip=%llu "
        "fail=%llu inv=%llu stale=%llu reinit=%llu "
        "causes(0=%llu 1=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu)\n",
        (unsigned long long)g_g278Deferred.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Flushes.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Saved.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushRows.load(std::memory_order_relaxed),
        (unsigned long long)g_g278UploadSkips.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278InvariantFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278StaleMarks.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Reinits.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[0].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[1].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[3].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[4].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[5].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[6].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[7].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[8].load(std::memory_order_relaxed));
    if (g337ZAuthOn())
        std::fprintf(
            stderr,
            "[G337:zauth] carried=%llu evict=%llu ovlFlush=%llu wideDefer=%llu "
            "auditTex=%llu auditTarget=%llu auditXKey=%llu\n",
            (unsigned long long)g_g337Carried.load(std::memory_order_relaxed),
            (unsigned long long)g_g337SlotEvict.load(std::memory_order_relaxed),
            (unsigned long long)g_g337OvlFlush.load(std::memory_order_relaxed),
            (unsigned long long)g_g337WideDefer.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditTex.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditTarget.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditCrossKey.load(std::memory_order_relaxed));
    if (g337ZAuthOn())
        std::fprintf(
            stderr,
            "[G337:direct] seeded=%llu tight=%llu noSkipRb=%llu other=%llu\n",
            (unsigned long long)g_g337DirSeeded.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirTight.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirNoSkipRb.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirOther.load(std::memory_order_relaxed));
}

static bool g278FlushPendSlot(G278PendDepth &slot, int cause)
{
    if (!slot.active)
        return true;
    const G278PendDepth p = slot;
    const int rows = p.rowHi - p.rowLo + 1;
    const int glY = 512 - 1 - p.rowHi;
    static std::vector<float> s_readback; // flush-owner thread only
    s_readback.clear();
    const auto t0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    if (p.vram == nullptr || p.vramSize == 0u || p.fbW <= 0 || rows <= 0 ||
        !g242_backend_read_depth(p.key, p.fbW, 512, glY, rows, s_readback) ||
        s_readback.size() != static_cast<size_t>(p.fbW) * rows)
    {
        const uint64_t n = g_g278FlushFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
            std::fprintf(stderr,
                         "[G278:depth] PUBLISH FAIL n=%llu cause=%d key=0x%llx "
                         "zbp=0x%x rows=%d..%d fbW=%d\n",
                         (unsigned long long)n, cause,
                         (unsigned long long)p.key, p.zbpPage,
                         p.rowLo, p.rowHi, p.fbW);
        // Keep the pending owner armed so a transient backend failure can be retried before a
        // fallback/consumer proceeds. Any observed failure blocks promotion.
        return false;
    }

    constexpr double kDepthToZ = 4294967296.0;
    const uint32_t zbpBlock = p.zbpPage << 5u;
    const auto unpackDepthRows = [&](int laneLo, int laneHi) {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(p.rowHi - y) * p.fbW;
            for (int x = 0; x < p.fbW; ++x)
            {
                double scaled = static_cast<double>(
                                    s_readback[srcRow + static_cast<size_t>(x)]) *
                                kDepthToZ;
                if (scaled < 0.0) scaled = 0.0;
                if (scaled > 16777215.0) scaled = 16777215.0;
                if (p.privateMirror)
                    g403DisplayZWrite(x, y, 0u, static_cast<uint32_t>(scaled));
                else
                    GSMem::WriteZ24(p.vram, zbpBlock, p.fbw,
                                    static_cast<uint32_t>(x),
                                    static_cast<uint32_t>(y),
                                    static_cast<uint32_t>(scaled));
            }
        }
    };
    GSRowPool::instance().run(p.rowLo, p.rowHi,
                              rows >= 8 ? g275ZSwizzleLanes() : 1,
                              unpackDepthRows);
    if (!p.privateMirror)
    {
        g178BumpRectImpl(zbpBlock, p.fbw, GS_PSM_Z24, 0u,
                         static_cast<uint32_t>(p.rowLo),
                         static_cast<uint32_t>(p.fbW),
                         static_cast<uint32_t>(rows));
        g336NotePublishZ(zbpBlock, p.fbw, p.fbW, p.rowLo, p.rowHi);
        g149BumpVramGen();
    }
    if (g274ZTimeOn())
        g_g274ZRbNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count(),
            std::memory_order_relaxed);
    if (p.privateMirror)
    {
        // G274's equality stamp describes guest VRAM and is meaningless for the private mirror.
        g_g274LastZGen.erase(p.key);
    }
    else if (g278StaleRowsAny(p))
    {
        // An out-of-union CPU write left other backend rows stale. Force the next use to perform
        // the backend contract's full initialization; a whole-gen equality stamp would otherwise
        // let G274 incorrectly skip over those newer VRAM rows.
        g_g274LastZGen.erase(p.key);
        g_g262DepthInit.erase(p.key);
        g_g278Reinits.fetch_add(1u, std::memory_order_relaxed);
    }
    else if (g274ZTimeOn() || g274ZResidentOn() || g278DepthRbOn())
        g_g274LastZGen[p.key] =
            g178GenSumRect(zbpBlock, p.fbw, GS_PSM_Z24, 0u, 0u,
                           static_cast<uint32_t>(p.fbW), 512u);
    g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
    if (p.privateMirror)
    {
        const uint64_t publishes =
            g_g411DepthPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g411DepthPublishRows.fetch_add(
            static_cast<uint64_t>(rows), std::memory_order_relaxed);
        if (g411SharedGpuDepthStatOn() &&
            (publishes <= 8u || (publishes % 128u) == 0u))
        {
            std::fprintf(
                stderr,
                "[G411:depth] batches=%llu uploadRows=%llu publishes=%llu publishRows=%llu "
                "last(cause=%d rows=%d..%d)\n",
                static_cast<unsigned long long>(
                    g_g411DepthBatches.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_g411DepthUploadRows.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(publishes),
                static_cast<unsigned long long>(
                    g_g411DepthPublishRows.load(std::memory_order_relaxed)),
                cause, p.rowLo, p.rowHi);
        }
    }
    if (p.display)
        g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);

    slot = {};
    slot.rowLo = 512;
    slot.rowHi = -1;
    const uint64_t n =
        g_g278Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g278FlushRows.fetch_add(static_cast<uint64_t>(rows),
                              std::memory_order_relaxed);
    if (cause >= 0 && cause < 9)
        g_g278FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    g278Report(n);
    return true;
}

// Conservative edge: publish EVERY pending owner (arm-off: only slot 0 is ever active, so this
// is byte-identical to the historical single-slot flush).
static bool g278FlushPendingDepth(int cause)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
        if (s_g278PendTab[s].active)
            ok = g278FlushPendSlot(s_g278PendTab[s], cause) && ok;
    return ok;
}

static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
            continue; // private shared depth has no VRAM transfer consumer
        if (srcUnknown || dstUnknown)
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        const G144BlockRange dirty = g278PendingDepthRangeP(p);
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!dirty.valid || !whole.valid)
        {
            ok = g278FlushPendSlot(p, cause) && ok; // unknown owner window fails closed
            continue;
        }
        // Reads only require unpublished rows. A write that touches dirty rows must preserve the
        // historical GPU-Z-before-CPU-write order, so publish first. A write elsewhere in the same
        // target can proceed without forcing a readback: remember its rows as backend-stale and
        // force a full re-init when this owner ends (or publish before a same-key wave reaches
        // those rows).
        if ((src.valid && g144RangeOverlaps(dirty, src)) ||
            (dst.valid && g144RangeOverlaps(dirty, dst)))
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        if (dst.valid && g144RangeOverlaps(whole, dst))
            g278MarkStaleRowsForRangeP(p, dst);
    }
    return ok;
}

static bool g278FlushPendingDepthForDisplayOwners(int cause)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
            continue; // display-color owners cannot consume the private Z mirror
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!whole.valid)
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        bool flush = false;
        for (int di = 0; di < 2 && !flush; ++di)
        {
            uint32_t fbp = 0u, fbw = 0u;
            G144BlockRange dirty{};
            if (g272DirtyInfo(di, fbp, fbw, dirty) &&
                (!dirty.valid || g144RangeOverlaps(whole, dirty)))
                flush = true;
        }
        if (!flush && s_g276Pend.active)
        {
            const G144BlockRange display = g277PendingDisplayRange();
            if (!display.valid || g144RangeOverlaps(whole, display))
                flush = true;
        }
        if (flush)
            ok = g278FlushPendSlot(p, cause) && ok;
    }
    return ok;
}

// Exact inline edge: fully clipped primitives consume nothing. Otherwise publish only when the
// CPU primitive's target, active Z, texture, or CLUT range can touch unpublished Z bytes.
static bool g278FlushPendingDepthForInline(const GSPrimReg &prim,
                                           const GSVertex *verts,
                                           const GSContext &ctx,
                                           const GSTexClutReg &texclut)
{
    bool anyActive = false;
    for (int s = 0; s < kG278PendSlots && !anyActive; ++s)
        anyActive = s_g278PendTab[s].active;
    if (!anyActive)
        return true;
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x;
    float minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x);
        maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y);
        maxYf = std::max(maxYf, verts[i].y);
    }
    const int cx0 = std::max(static_cast<int>(std::floor(minXf)) - ofx,
                             static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(static_cast<int>(std::ceil(maxXf)) - ofx,
                             static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(static_cast<int>(std::floor(minYf)) - ofy,
                             static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(static_cast<int>(std::ceil(maxYf)) - ofy,
                             static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
        return true;

    const uint32_t x = static_cast<uint32_t>(cx0);
    const uint32_t y = static_cast<uint32_t>(cy0);
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    // Build the primitive's consumed/produced ranges ONCE, then test each pending owner.
    const G144BlockRange target =
        g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                         ctx.frame.fbw, x, y, w, h);
    const uint64_t test = ctx.test;
    const bool zte = ((test >> 16u) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17u) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32u) & 1u) != 0u;
    const bool depthReads = zte && ztst >= 2u;
    const bool depthWrites = !zmsk;
    G144BlockRange zr{};
    uint64_t inlineDepthKey = 0u;
    bool inlinePrivateDepth = false;
    if (depthReads || depthWrites)
    {
        const uint32_t zbpPage =
            static_cast<uint32_t>(ctx.zbuf) & 0x1FFu;
        const uint32_t zbpBlock = zbpPage << 5u;
        const uint32_t zpsm =
            static_cast<uint32_t>((ctx.zbuf >> 24u) & 0xFu);
        inlineDepthKey = g242DepthKey(zbpPage, zpsm, ctx.frame.fbw);
        inlinePrivateDepth =
            g404SharedZScope(ctx.frame.fbp, ctx.frame.fbw, zbpPage, zpsm);
        zr = g144RangeForRect(zbpBlock, zpsm, ctx.frame.fbw, x, y, w, h);
    }
    G144BlockRange tex{}, clut{};
    bool texValidKnown = false, clutNeeded = false;
    if (prim.tme)
    {
        const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
        const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
        tex = g144RangeForRect(ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw,
                               0u, 0u, texW, texH);
        texValidKnown = true;
        if (g144IsPalettedPsm(ctx.tex0.psm))
        {
            const uint32_t clutBw =
                texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            clut = g144RangeForRect(ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                                    static_cast<uint32_t>(texclut.cou),
                                    static_cast<uint32_t>(texclut.cov), 16u, 16u);
            clutNeeded = true;
        }
    }
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
        {
            // Physical color/texture/CLUT ranges cannot consume the private mirror. An inline CPU
            // primitive using the same logical Z surface can, so publish at that semantic edge.
            if (inlinePrivateDepth && p.key == inlineDepthKey)
                ok = g278FlushPendSlot(p, 4) && ok;
            continue;
        }
        const G144BlockRange dirty = g278PendingDepthRangeP(p);
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!dirty.valid || !whole.valid)
        {
            ok = g278FlushPendSlot(p, 4) && ok;
            continue;
        }
        const auto readsPending = [&](const G144BlockRange &r) {
            return !r.valid || g144RangeOverlaps(dirty, r);
        };
        const auto writesOwner = [&](const G144BlockRange &r) {
            return !r.valid || g144RangeOverlaps(whole, r);
        };
        bool mustPublish = writesOwner(target);
        if (!mustPublish && (depthReads || depthWrites))
            mustPublish = (depthReads && readsPending(zr)) ||
                          (depthWrites && writesOwner(zr));
        if (!mustPublish && texValidKnown)
        {
            mustPublish = readsPending(tex);
            if (!mustPublish && clutNeeded)
                mustPublish = readsPending(clut);
        }
        if (mustPublish)
            ok = g278FlushPendSlot(p, 4) && ok;
    }
    return ok;
}

// G263: exact GL-tap footprint admission for a BILINEAR FST sprite that samples a resident RTT
// target (the G262 census shape: the display composite, prim=6, CT32, tbp=0x2720, bilin=1 was the
// SOLE bind rejector). GL_LINEAR taps floor(s-0.5)/+1 around every sample; samples are the GL
// pixel-center evaluations of the pushed varyings (raw corner UVs, drawSprite's left-edge
// -0.5*gradient shift, linear in screen space) — the same 4-tap convention the CPU bilinear
// sampler applies (sampleU = texUf - 0.5, floor, +1), so GL and the CPU oracle read identical
// taps wherever both are defined. Admission requires every tap that can carry NONZERO weight to
// land on an FBO-DEFINED texel: rows [covLo..covHi], full width. One fold is allowed per axis:
// tap -1 under GS CLAMP folds onto texel 0 on both GL (CLAMP_TO_EDGE) and the CPU sampler, exact
// when row/col 0 is itself defined. Everything else fails closed to the materialize path:
// REPEAT consumers with edge taps (GL would clamp where the CPU wraps), taps past the covered
// rows (FBO content undefined there), taps past the DECLARED texture dims (the CPU folds at
// texW/texH, the fbW×512 FBO space does not).
static bool g263BilinearSpriteBind(const G144Entry &e, const G178EntryState &st,
                                   const G261Res &r, int *outTaps = nullptr)
{
    const GSVertex &q0 = e.v[0];
    const GSVertex &q1 = e.v[1];
    int tapLo[2] = {0, 0}, tapHi[2] = {0, 0};
    for (int a = 0; a < 2; ++a)
    {
        const float p0 = a ? q0.y : q0.x, p1 = a ? q1.y : q1.x;
        const float t0 = static_cast<float>(a ? q0.v : q0.u) / 16.0f;
        const float t1 = static_cast<float>(a ? q1.v : q1.u) / 16.0f;
        const float lo = std::min(p0, p1), hi = std::max(p0, p1);
        // First/last GL pixel centers covered by [lo, hi). XYOFFSET is integer at this level
        // (ofx = reg >> 4), so subtracting it keeps the +0.5 center grid — evaluate in raw coords.
        const float cF = std::ceil(lo - 0.5f) + 0.5f;
        const float cL = std::ceil(hi - 0.5f) - 0.5f;
        if (!(cL >= cF) || p1 == p0)
            return false; // degenerate/empty axis — nothing provable, materialize instead
        const float g = (t1 - t0) / (p1 - p0);
        const float shift = -0.5f * g; // drawSprite left-edge (0.0) bias, exactly as pushed
        const float sA = t0 + (cF - p0) * g + shift;
        const float sB = t0 + (cL - p0) * g + shift;
        const float sLo = std::min(sA, sB), sHi = std::max(sA, sB);
        tapLo[a] = static_cast<int>(std::floor(sLo - 0.5f - 0.001f));
        // ceil() keeps an exactly-integral edge in place (that +1 neighbour has weight 0); the
        // epsilon biases toward INCLUDING a tap, i.e. toward rejection, never toward admission.
        tapHi[a] = static_cast<int>(std::ceil(sHi - 0.5f + 0.001f));
    }
    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (tapLo[0] >= 0 || (clampU && tapLo[0] == -1)) && tapHi[0] <= maxCol;
    const bool vOk = (tapLo[1] >= r.covLo || (clampV && r.covLo == 0 && tapLo[1] == -1)) &&
                     tapHi[1] <= maxRow;
    if (!(uOk && vOk) && g262CensusOn())
    {
        static std::atomic<uint32_t> s_g263RejLog{0};
        if (s_g263RejLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr,
                         "[G263:rej] xy0=(%.2f,%.2f) xy1=(%.2f,%.2f) uv0=(%u,%u) uv1=(%u,%u) "
                         "tapU=%d..%d tapV=%d..%d cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u\n",
                         q0.x, q0.y, q1.x, q1.y, q0.u, q0.v, q1.u, q1.v,
                         tapLo[0], tapHi[0], tapLo[1], tapHi[1], r.covLo, r.covHi, r.fbW,
                         st.texW, st.texH, static_cast<unsigned>(st.wrapU),
                         static_cast<unsigned>(st.wrapV));
    }
    if (uOk && vOk && outTaps != nullptr)
    {
        outTaps[0] = tapLo[0];
        outTaps[1] = tapHi[0];
        outTaps[2] = tapLo[1];
        outTaps[3] = tapHi[1];
    }
    return uOk && vOk;
}

// G267: conservative tap footprint for an affine FST triangle. Every interpolated UV inside a
// triangle lies inside the per-axis convex hull of its three vertex UVs, so expanding that hull
// to the two GL_LINEAR taps is a safe (possibly over-wide) bound. Unlike the sprite helper above,
// this does not try to exploit exclusive right/bottom endpoints. The returned GS block range is
// used to reject a direct bind whenever another dirty resident target could contribute any sampled
// byte. That alias screen is load-bearing: binding one producer FBO while silently ignoring a dirty
// sibling would make an internal residency oracle pass but corrupt final composition.
struct G267TriFootprint
{
    int tapLoU = 0, tapHiU = -1;
    int tapLoV = 0, tapHiV = -1;
    uint32_t sampleX0 = 0, sampleY0 = 0, sampleX1 = 0, sampleY1 = 0;
    G144BlockRange sampleRange{};
};

static bool g267BilinearTriFootprint(const G144Entry &e, const G178EntryState &st,
                                     const G261Res &r, G267TriFootprint &out)
{
    float uvLo[2] = {FLT_MAX, FLT_MAX};
    float uvHi[2] = {-FLT_MAX, -FLT_MAX};
    for (int k = 0; k < 3; ++k)
    {
        const float u = static_cast<float>(e.v[k].u) / 16.0f;
        const float v = static_cast<float>(e.v[k].v) / 16.0f;
        if (!std::isfinite(u) || !std::isfinite(v))
            return false;
        uvLo[0] = std::min(uvLo[0], u); uvHi[0] = std::max(uvHi[0], u);
        uvLo[1] = std::min(uvLo[1], v); uvHi[1] = std::max(uvHi[1], v);
    }

    // GL_LINEAR samples floor(coord - 0.5) and its +1 neighbour. Epsilons only widen the
    // footprint, biasing uncertain half ties toward rejection rather than unsafe admission.
    out.tapLoU = static_cast<int>(std::floor(uvLo[0] - 0.5f - 0.001f));
    out.tapHiU = static_cast<int>(std::ceil (uvHi[0] - 0.5f + 0.001f));
    out.tapLoV = static_cast<int>(std::floor(uvLo[1] - 0.5f - 0.001f));
    out.tapHiV = static_cast<int>(std::ceil (uvHi[1] - 0.5f + 0.001f));

    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (out.tapLoU >= 0 || (clampU && out.tapLoU == -1)) &&
                     out.tapHiU <= maxCol;
    const bool vOk = (out.tapLoV >= r.covLo ||
                      (clampV && r.covLo == 0 && out.tapLoV == -1)) &&
                     out.tapHiV <= maxRow;
    if (!(uOk && vOk))
        return false;

    const uint32_t x0 = static_cast<uint32_t>(std::max(0, out.tapLoU));
    const uint32_t y0 = static_cast<uint32_t>(std::max(0, out.tapLoV));
    const uint32_t x1 = static_cast<uint32_t>(out.tapHiU);
    const uint32_t y1 = static_cast<uint32_t>(out.tapHiV);
    out.sampleX0 = x0; out.sampleY0 = y0; out.sampleX1 = x1; out.sampleY1 = y1;
    out.sampleRange = g144RangeForRect(e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.tbw,
                                       x0, y0, x1 - x0 + 1u, y1 - y0 + 1u);
    return out.sampleRange.valid;
}

// The generic G144BlockRange is intentionally conservative: it collapses a 2-D page footprint
// to one contiguous min/max interval. For G267's narrow U=0..16 strip over many page rows that
// interval falsely includes the seven unsampled CT32 page columns between each real column-0
// page. Enumerate the exact sampled CT32 pages before declaring a dirty sibling alias.
static bool g267SamplePagesOverlap(const G144Entry &e, const G267TriFootprint &fp,
                                   const G144BlockRange &resident)
{
    if (!resident.valid || e.ctx.tex0.psm != GS_PSM_CT32)
        return true; // fail closed for unknown range/format
    const uint32_t pageBw = std::max<uint32_t>(1u, e.ctx.tex0.tbw);
    const uint32_t xp0 = fp.sampleX0 / 64u, xp1 = fp.sampleX1 / 64u;
    const uint32_t yp0 = fp.sampleY0 / 32u, yp1 = fp.sampleY1 / 32u;
    for (uint32_t yp = yp0; yp <= yp1; ++yp)
    {
        for (uint32_t xp = xp0; xp <= xp1; ++xp)
        {
            const uint64_t first64 = static_cast<uint64_t>(e.ctx.tex0.tbp0) +
                                     (static_cast<uint64_t>(yp) * pageBw + xp) * 32u;
            if (first64 > 0x3FFFu)
                return true;
            const G144BlockRange page{static_cast<uint32_t>(first64),
                                      static_cast<uint32_t>(std::min<uint64_t>(first64 + 31u,
                                                                               0x3FFFu)),
                                      true};
            if (g144RangeOverlaps(page, resident))
                return true;
        }
    }
    return false;
}

// G309 (default-off): request the one-pass authoritative-page atlas composite. The exact
// verifier implies the mechanism so first-use validation needs only DC2_G309_VERIFY=1.
static bool g309Requested()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G309_NO_MULTI_SOURCE") ||
            envFlagDisabled("DC2_G309_MULTI_SOURCE"))
            return false;
        return envFlagEnabled("DC2_G309_MULTI_SOURCE") ||
               envFlagEnabled("DC2_G309_VERIFY");
    }();
    return s_on;
}

// G294 (default-off, DC2_G294_OWNER_TOKEN=1; kill DC2_G294_NO_OWNER_TOKEN=1): per-physical-page
// authoritative-owner token. Requesting it implies the whole G280/G281/G282/G283 view+fanout+epoch
// stack so a same-binary A/B needs one behavior lever. Declared free here (env-only, no gate deps)
// so the request chains below and g283AuthorityRequested can all reference it.
// G446: this predicate is consulted from g283AuthorityOn(), which the T8 map resolver and the
// target-page authority call PER PAGE / PER DRAW on the GS worker — and it did three uncached
// getenv() calls per invocation. The G446 host PC sampler measured getenv at 1.40% of ALL GS-worker
// samples with the worker at 98% occupancy and the frame equal to its span (gsslow 0.98), i.e. the
// pole thread was spending ~0.5 ms/f inside the Windows CRT environment lock. This is the exact
// G268 defect, recurring at a site G268 never swept. Caching is exact: DC2_* levers are
// process-constant in every harness (the whole tree's `static const bool` idiom assumes it) and
// envFlagEnabled/envFlagDisabled have no other side effect.
static bool g294OwnerTokenRequested()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G294_NO_OWNER_TOKEN") || envFlagDisabled("DC2_G294_OWNER_TOKEN"))
            return false;
        return envFlagEnabled("DC2_G294_OWNER_TOKEN");
    }();
    return s_on;
}
