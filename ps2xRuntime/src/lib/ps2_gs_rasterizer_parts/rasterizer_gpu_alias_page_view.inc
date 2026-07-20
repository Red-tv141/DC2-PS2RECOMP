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
    uint64_t key = 0u;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
    uint32_t zbpPage = 0u, fbw = 0u;
    int fbW = 0;
    int rowLo = 512, rowHi = -1;
    std::array<uint64_t, 8> staleRows{}; // CPU-written rows outside dirty union; backend is stale
};
static G278PendDepth s_g278Pend; // flush-owner thread only (same contract as s_g276Pend)
static std::atomic<uint64_t> g_g278Deferred{0}, g_g278Flushes{0}, g_g278Saved{0},
    g_g278FlushRows{0}, g_g278UploadSkips{0}, g_g278FlushFail{0},
    g_g278InvariantFail{0}, g_g278StaleMarks{0}, g_g278Reinits{0};
static std::atomic<uint64_t> g_g278FlushByCause[9] = {};
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
        envFlagEnabled("DC2_G232_RTTDUMP");
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
static G289CopyOwner s_g289Owner;
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
static G144BlockRange g278PendingDepthWholeRange();
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
    if (!g261StatOn() && !g279ProfileOn() && !g310StatOn())
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
            s_g264Px.assign(static_cast<size_t>(width) * rows, 0u);
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
            if (!packed ||
                !g264_backend_write_color_rect(fbp, r.fbW, 512, x0, glY,
                                                width, rows, s_g264Px))
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
static void g305DrainPending();

static void g261Materialize(int ti, int cause)
{
    if (g305AsyncOn())
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
        const bool readOk = g178_backend_read_color(fbp, r.fbW, 512, glY, rows, s_g261Px);
        if (g279Profile)
            g_g279MatReadNs[cause & 7].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g261Px.size() == static_cast<size_t>(r.fbW) * rows;
        if (ok)
        {
            for (int y = r.dirtyLo; y <= r.dirtyHi; ++y)
            {
                // s_g261Px rows are GL window rows starting at glY (bottom-first):
                // GS row y ↔ GL row (511-y) ↔ buffer row (511-y-glY).
                const size_t srcRow = static_cast<size_t>(511 - y - glY) * r.fbW;
                for (int x = 0; x < r.fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw,
                                                                static_cast<uint32_t>(x),
                                                                static_cast<uint32_t>(y));
                    if (off + 4u <= g_g261VramSize)
                        std::memcpy(vram + off, &s_g261Px[srcRow + x], 4);
                }
            }
            g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW), static_cast<uint32_t>(rows));
            // FBO still equals VRAM for this target: refresh the snapshot past our own bump.
            G178FbSnap &snap = g_g178FbSnap[fbp];
            snap.valid = true;
            if (r.covLo < snap.rowLo) snap.rowLo = r.covLo;
            if (r.covHi > snap.rowHi) snap.rowHi = r.covHi;
            snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                         static_cast<uint32_t>(r.fbW), 512u);
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
        // hold live references into g_g178FbSnap; erase would invalidate them).
        G178FbSnap &failSnap = g_g178FbSnap[fbp];
        failSnap.valid = false;
        failSnap.genSum = 0;
        failSnap.rowLo = 1 << 30;
        failSnap.rowHi = -1;
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
    if (s_g278Pend.active)
    {
        const G144BlockRange depthWhole = g278PendingDepthWholeRange();
        if (!depthWhole.valid || !displayRange.valid ||
            g144RangeOverlaps(depthWhole, displayRange))
        {
            if (!g278FlushPendingDepth(cause))
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
        const bool readOk = g178_backend_read_color(p.fbp, p.fbW, 512, glY, rows, s_g276Px);
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
    if (s_g289Owner.active && !g289Resolve14aBeforeDirect())
        return 0u;
    return s_g276Pend.fbp == 0u ? kG289ZeroFbpToken : s_g276Pend.fbp;
}

static G144BlockRange g289OwnerRange()
{
    G144BlockRange out;
    out.first = s_g289Owner.firstOwnedBlock;
    out.last = s_g289Owner.lastOwnedBlock;
    out.valid = s_g289Owner.active && out.first <= out.last;
    return out;
}

static bool g289MaterializeOwner(int cause)
{
    if (!s_g289Owner.active)
        return true;
    G289CopyOwner owner = s_g289Owner;
    s_g289Owner.active = false;
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
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
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
    if (!s_g289Owner.active || s_g289Owner.dstFbp != 0x14au ||
        (!exactT4 && !exactCt32 && !exactCt32Tail) ||
        !dst.valid || dst.first > s_g289Owner.firstOwnedBlock ||
        dst.last < s_g289Owner.firstOwnedBlock ||
        s_g289Owner.lastOwnedBlock != 0x363fu)
        return false;
    s_g289PendingUpload.active = true;
    s_g289PendingUpload.ownerVersion = s_g289Owner.version;
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
    const bool exact =
        s_g289Owner.active &&
        s_g289Owner.version == s_g289PendingUpload.ownerVersion &&
        s_g289Owner.dstFbp == 0x14au &&
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
    s_g289Owner.firstOwnedBlock =
        std::max(s_g289Owner.firstOwnedBlock,
                 s_g289PendingUpload.replacedThroughBlock + 1u);
    s_g289PendingUpload.active = false;
    const bool fullyReplaced =
        s_g289Owner.firstOwnedBlock > s_g289Owner.lastOwnedBlock;
    if (fullyReplaced)
    {
        s_g289Owner.active = false;
        G178FbSnap &snap = g_g178FbSnap[s_g289Owner.dstFbp];
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
                     static_cast<unsigned long long>(s_g289Owner.version),
                     dbp, s_g289PendingUpload.replacedThroughBlock,
                     s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock,
                     fullyReplaced ? 1u : 0u);
}

// The following exact fbp=0x139 display sprite overwrites physical pages 0x2720..0x341f.
// For the partially overwritten 0x14a owner, only page 0x3420 onward can survive. Publishing
// GS rows 320..415 covers that complete suffix (plus harmless bounded over-work), after which
// the normal direct sprite may execute with no unresolved alias owner.
static bool g289Resolve14aBeforeDirect()
{
    if (!s_g289Owner.active || s_g289Owner.dstFbp != 0x14au ||
        s_g289Owner.firstOwnedBlock < 0x29a0u ||
        s_g289Owner.firstOwnedBlock > s_g289Owner.lastOwnedBlock ||
        s_g289Owner.lastOwnedBlock != 0x363fu ||
        s_g289Owner.vram == nullptr || s_g289Owner.vramSize == 0u)
        return false;

    G289CopyOwner owner = s_g289Owner;
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
    s_g289Owner.active = false;
    s_g289PendingUpload.active = false;
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
    if (!s_g289Owner.active)
        return;
    const G144BlockRange owner = g289OwnerRange();
    if (srcUnknown || dstUnknown || !owner.valid ||
        (src.valid && g144RangeOverlaps(owner, src)) ||
        (dst.valid && g144RangeOverlaps(owner, dst)))
        (void)g289MaterializeOwner(cause);
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
    if (!s_g289Owner.active || w == 0u || h == 0u)
        return;
    if (vram != s_g289Owner.vram || !useLocalMemoryLayout)
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

static bool g289CanDeferLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                  uint32_t ssax, uint32_t ssay,
                                  uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay,
                                  uint32_t rrw, uint32_t rrh)
{
    const bool can = g289LocalCopyOn() && !s_g289Owner.active && s_g276Pend.active &&
           s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
           s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
           framePageBaseToBlock(s_g276Pend.fbp) == sbp &&
           sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
           (dbp == framePageBaseToBlock(0x13bu) ||
            dbp == framePageBaseToBlock(0x14au)) &&
           dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
           rrw == 512u && rrh == 416u;
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
    if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                               dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
        return false;

    const uint32_t sourceFbp = s_g276Pend.fbp;
    const uint32_t destFbp = dbp >> 5u;
    if (!g289_backend_copy_display_to_work(sourceFbp, destFbp))
    {
        const uint64_t n =
            g_g289CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        std::fprintf(stderr,
                     "[G289:copy-fail] n=%llu src=0x%x dst=0x%x; legacy fallback\n",
                     static_cast<unsigned long long>(n), sourceFbp, destFbp);
        g276FlushPendingDisplay(8);
        return false;
    }

    s_g289Owner.active = true;
    ++s_g289Owner.version;
    s_g289Owner.srcFbp = sourceFbp;
    s_g289Owner.dstFbp = destFbp;
    const G144BlockRange ownerRange =
        g144RangeForRect(dbp, GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
    s_g289Owner.firstOwnedBlock = ownerRange.first;
    s_g289Owner.lastOwnedBlock = ownerRange.last;
    s_g289Owner.vram = vram;
    s_g289Owner.vramSize = vramSize;
    s_g289PendingUpload.active = false;
    const uint64_t copyN =
        g_g289Copies.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (copyN <= 16u || (copyN % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:copy] n=%llu version=%llu src=0x%x dst=0x%x rows=0..415\n",
                     static_cast<unsigned long long>(copyN),
                     static_cast<unsigned long long>(s_g289Owner.version), sourceFbp,
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
    if (!s_g289Owner.active || s_g289Owner.vram != vram ||
        s_g289Owner.vramSize != vramSize || s_g289Owner.dstFbp != 0x13bu ||
        g_g144List.size() != 1u)
    {
        if (s_g289Owner.active && g289StatOn())
        {
            static std::atomic<uint32_t> s_earlyRejectLogs{0};
            const uint32_t n = s_earlyRejectLogs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 8u)
                std::fprintf(
                    stderr,
                    "[G289:consume-early-reject] n=%u ownerVram=%p/%u vram=%p/%u entries=%zu\n",
                    n + 1u, static_cast<void *>(s_g289Owner.vram), s_g289Owner.vramSize,
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
    if (!s_g289Owner.active || s_g289Owner.vram != vram ||
        s_g289Owner.dstFbp != 0x13bu ||
        s_g289Owner.vramSize != vramSize || g_g144List.empty())
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
        if (armed && s_g289Owner.active)
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
        if (armed && s_g289Owner.active)
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
static G144BlockRange g278PendingDepthRange()
{
    if (!s_g278Pend.active || s_g278Pend.fbW <= 0 ||
        s_g278Pend.rowHi < s_g278Pend.rowLo)
        return {};
    return g144RangeForRect(s_g278Pend.zbpPage << 5u, GS_PSM_Z24,
                            s_g278Pend.fbw, 0u,
                            static_cast<uint32_t>(s_g278Pend.rowLo),
                            static_cast<uint32_t>(s_g278Pend.fbW),
                            static_cast<uint32_t>(
                                s_g278Pend.rowHi - s_g278Pend.rowLo + 1));
}

static G144BlockRange g278PendingDepthWholeRange()
{
    if (!s_g278Pend.active || s_g278Pend.fbW <= 0)
        return {};
    return g144RangeForRect(s_g278Pend.zbpPage << 5u, GS_PSM_Z24,
                            s_g278Pend.fbw, 0u, 0u,
                            static_cast<uint32_t>(s_g278Pend.fbW), 512u);
}

static bool g278StaleRowsAny(const G278PendDepth &p)
{
    for (uint64_t bits : p.staleRows)
        if (bits != 0u)
            return true;
    return false;
}

static bool g278StaleRowsOverlap(int lo, int hi)
{
    if (!s_g278Pend.active || hi < lo)
        return false;
    lo = std::max(0, lo);
    hi = std::min(511, hi);
    for (int y = lo; y <= hi; ++y)
        if ((s_g278Pend.staleRows[static_cast<size_t>(y >> 6)] >>
             static_cast<uint32_t>(y & 63)) & 1u)
            return true;
    return false;
}

static void g278MarkStaleRowsForRange(const G144BlockRange &write)
{
    if (!s_g278Pend.active || !write.valid)
        return;
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!whole.valid || !g144RangeOverlaps(whole, write))
        return;
    const uint32_t firstBlock = std::max(whole.first, write.first);
    const uint32_t lastBlock = std::min(whole.last, write.last);
    const uint32_t firstPage = (firstBlock - whole.first) >> 5u;
    const uint32_t lastPage = (lastBlock - whole.first) >> 5u;
    const uint32_t pagesPerRow = std::max(1u, s_g278Pend.fbw);
    const int lo = static_cast<int>(firstPage / pagesPerRow) * 32;
    const int hi = std::min(
        511, static_cast<int>(lastPage / pagesPerRow) * 32 + 31);
    for (int y = lo; y <= hi; ++y)
        s_g278Pend.staleRows[static_cast<size_t>(y >> 6)] |=
            1ull << static_cast<uint32_t>(y & 63);
    g_g278StaleMarks.fetch_add(1u, std::memory_order_relaxed);
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
}

static bool g278FlushPendingDepth(int cause)
{
    if (!s_g278Pend.active)
        return true;
    const G278PendDepth p = s_g278Pend;
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
    g178BumpRectImpl(zbpBlock, p.fbw, GS_PSM_Z24, 0u,
                     static_cast<uint32_t>(p.rowLo),
                     static_cast<uint32_t>(p.fbW),
                     static_cast<uint32_t>(rows));
    g149BumpVramGen();
    if (g274ZTimeOn())
        g_g274ZRbNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count(),
            std::memory_order_relaxed);
    if (g278StaleRowsAny(p))
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
    if (p.display)
        g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);

    s_g278Pend = {};
    s_g278Pend.rowLo = 512;
    s_g278Pend.rowHi = -1;
    const uint64_t n =
        g_g278Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g278FlushRows.fetch_add(static_cast<uint64_t>(rows),
                              std::memory_order_relaxed);
    if (cause >= 0 && cause < 9)
        g_g278FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    g278Report(n);
    return true;
}

static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown)
{
    if (!s_g278Pend.active)
        return true;
    if (srcUnknown || dstUnknown)
        return g278FlushPendingDepth(cause);
    const G144BlockRange dirty = g278PendingDepthRange();
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!dirty.valid || !whole.valid)
        return g278FlushPendingDepth(cause); // unknown owner window fails closed
    // Reads only require unpublished rows. A write that touches dirty rows must preserve the
    // historical GPU-Z-before-CPU-write order, so publish first. A write elsewhere in the same
    // target can proceed without forcing a readback: remember its rows as backend-stale and force
    // a full re-init when this owner ends (or publish before a same-key wave reaches those rows).
    if ((src.valid && g144RangeOverlaps(dirty, src)) ||
        (dst.valid && g144RangeOverlaps(dirty, dst)))
        return g278FlushPendingDepth(cause);
    if (dst.valid && g144RangeOverlaps(whole, dst))
        g278MarkStaleRowsForRange(dst);
    return true;
}

static bool g278FlushPendingDepthForDisplayOwners(int cause)
{
    if (!s_g278Pend.active)
        return true;
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!whole.valid)
        return g278FlushPendingDepth(cause);
    for (int di = 0; di < 2; ++di)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange dirty{};
        if (g272DirtyInfo(di, fbp, fbw, dirty) &&
            (!dirty.valid || g144RangeOverlaps(whole, dirty)))
            return g278FlushPendingDepth(cause);
    }
    if (s_g276Pend.active)
    {
        const G144BlockRange display = g277PendingDisplayRange();
        if (!display.valid || g144RangeOverlaps(whole, display))
            return g278FlushPendingDepth(cause);
    }
    return true;
}

// Exact inline edge: fully clipped primitives consume nothing. Otherwise publish only when the
// CPU primitive's target, active Z, texture, or CLUT range can touch unpublished Z bytes.
static bool g278FlushPendingDepthForInline(const GSPrimReg &prim,
                                           const GSVertex *verts,
                                           const GSContext &ctx,
                                           const GSTexClutReg &texclut)
{
    if (!s_g278Pend.active)
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

    const G144BlockRange dirty = g278PendingDepthRange();
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!dirty.valid || !whole.valid)
        return g278FlushPendingDepth(4);
    const uint32_t x = static_cast<uint32_t>(cx0);
    const uint32_t y = static_cast<uint32_t>(cy0);
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    const auto readsPending = [&](const G144BlockRange &r) {
        return !r.valid || g144RangeOverlaps(dirty, r);
    };
    const auto writesOwner = [&](const G144BlockRange &r) {
        return !r.valid || g144RangeOverlaps(whole, r);
    };

    const G144BlockRange target =
        g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                         ctx.frame.fbw, x, y, w, h);
    bool mustPublish = writesOwner(target);
    const uint64_t test = ctx.test;
    const bool zte = ((test >> 16u) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17u) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32u) & 1u) != 0u;
    const bool depthReads = zte && ztst >= 2u;
    const bool depthWrites = !zmsk;
    if (!mustPublish && (depthReads || depthWrites))
    {
        const uint32_t zbpBlock =
            (static_cast<uint32_t>(ctx.zbuf) & 0x1FFu) << 5u;
        const uint32_t zpsm =
            static_cast<uint32_t>((ctx.zbuf >> 24u) & 0xFu);
        const G144BlockRange zr = g144RangeForRect(
            zbpBlock, zpsm, ctx.frame.fbw, x, y, w, h);
        mustPublish = (depthReads && readsPending(zr)) ||
                      (depthWrites && writesOwner(zr));
    }
    if (!mustPublish && prim.tme)
    {
        const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
        const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
        mustPublish = readsPending(g144RangeForRect(
            ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw,
            0u, 0u, texW, texH));
        if (!mustPublish && g144IsPalettedPsm(ctx.tex0.psm))
        {
            const uint32_t clutBw =
                texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            mustPublish = readsPending(g144RangeForRect(
                ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                static_cast<uint32_t>(texclut.cou),
                static_cast<uint32_t>(texclut.cov), 16u, 16u));
        }
    }
    return !mustPublish || g278FlushPendingDepth(4);
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
static bool g294OwnerTokenRequested()
{
    if (envFlagEnabled("DC2_G294_NO_OWNER_TOKEN") || envFlagDisabled("DC2_G294_OWNER_TOKEN"))
        return false;
    return envFlagEnabled("DC2_G294_OWNER_TOKEN");
}