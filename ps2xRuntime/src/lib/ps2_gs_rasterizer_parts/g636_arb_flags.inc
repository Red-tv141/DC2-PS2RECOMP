// ===== G636: ARMS AND COUNTERS FOR THE TWO-AUTHORITY ARBITRATION ==============================
//
// Split out of `g636_authority_arbitration.inc` because two of the four repair sites live in
// `rasterizer_gpu_alias_page_view.inc` / `rasterizer_vram_materialization.inc`, which are included
// BEFORE `g630_gpu_domain.inc`. The predicates need `g630AppendGuestPages` and stay there; only the
// arms and counters need to be visible early, so only they are here.
//
// Full rationale: `g636_authority_arbitration.inc`.

// Namespace-scope caches (PS2_PROJECT_STATE.md diagnostic-hygiene rule: a function-local
// `static const bool` is an MSVC per-access TLS guard, and these sit on per-draw / per-batch paths).
static const bool s_g636NoArb = envFlagEnabled("DC2_G636_NO_ARBITRATE");
static const bool s_g636NoBandPatch = envFlagEnabled("DC2_G636_NO_BANDPATCH");
static const bool s_g636NoFbSeed = envFlagEnabled("DC2_G636_NO_FBSEED");
static const bool s_g636NoMatPub = envFlagEnabled("DC2_G636_NO_MATPUB");
static const bool s_g636NoCt32View = envFlagEnabled("DC2_G636_NO_CT32VIEW");
static const bool s_g636NoT8View = envFlagEnabled("DC2_G636_NO_T8VIEW");
static const bool s_g636NoRawView = envFlagEnabled("DC2_G636_NO_RAWVIEW");
// ⭐⭐⭐ RETIRED BY DEFAULT (this is defect A's actual mechanism — see
// g636_authority_arbitration.inc). `DC2_G636_LINVIEW=1` restores the pre-G636 resident linear
// FBO mirror and the `g630TryBind` CT32 direct view that consumes it, verbatim, defect included.
static const bool s_g636LinView = envFlagEnabled("DC2_G636_LINVIEW");
// G636 BISECT ARMS FOR THE DOMAIN CORE. `_SYNCPUB` publishes the whole replay window to guest VRAM
// immediately after every resident submit, and `_SYNCPUBZ` does the same for the private depth
// mirror — together they make the resident domain OBSERVATIONALLY IDENTICAL to the non-resident
// G629 arm (which reads its result back per batch). If a defect survives both, it is not a
// coherence hole at all; if it dies under one of them, that half names the unpublished authority.
// Diagnostic only — they delete the entire point of residency.
static const bool s_g636SyncPub = envFlagEnabled("DC2_G636_SYNCPUB");
static const bool s_g636SyncPubZ = envFlagEnabled("DC2_G636_SYNCPUBZ");
// ⭐ THE ORACLE G630 NEVER HAD, POINTED AT THE AXIS THAT CAN FAIL (G635's law). For every page the
// GPU does NOT own, guest VRAM is by definition the authority and the raw image is supposed to
// track it. Read the image back at each resident submit and compare those pages byte for byte; a
// mismatch is a page the domain believes it has synced and has not. Run it with _SYNCPUB so the
// comparable population is every page, and read `[G636:rawvfy]`.
static const bool s_g636RawVfy = envFlagEnabled("DC2_G636_RAWVFY");
// ⭐⭐⭐ DEFECT A. The private DEPTH mirror is seeded from `g403DisplayZBuf()` ONCE and then kept
// GPU-side until `g630NoteCpuReplay()` invalidates it — which only fires after a CPU BAND REPLAY.
// Any other writer of that Z buffer leaves the GPU mirror stale, and the resident replay then
// DEPTH-TESTS AGAINST STALE Z: polygons are rejected that should have been drawn. That is the
// `ridepod` chassis rendering black, and it is why the defect survived deleting every direct view,
// retiring the linear FBO mirror, and publishing colour+depth after every batch (`_SYNCPUB`).
//   `_ZRESEED`   brute-force diagnostic: re-seed the mirror on every resident submit (proved it).
//   the default: compare the CPU Z buffer against a shadow of what the mirror was last
//                synchronised to, and re-seed only when it moved. Exact, and it costs one 1 MiB
//                memcmp per resident submit instead of a 1 MiB upload.
//   `_NO_ZSHADOW` rolls the shadow off and restores the pre-G636 "seed once" behaviour.
static const bool s_g636ZReseed = envFlagEnabled("DC2_G636_ZRESEED");
static const bool s_g636NoZShadow = envFlagEnabled("DC2_G636_NO_ZSHADOW");
// ⭐⭐⭐ AND THE DISPOSITION. `[G636:zwho]` names the writer by its footprint: ~12,000 words in
// rows 161..401 — the ridepod's own silhouette — written by the CPU rasterizer between two
// resident replays, with the GPU mirror still unpublished EVERY time (1,082 of 1,082). Both sides
// hold real depth writes, so no after-the-fact detection can merge them; the mirror would have to
// be published before each CPU writer and re-seeded after it, i.e. a 1 MiB upload AND a 1 MiB
// synchronous readback per depth batch (48% of all resident batches — 2,456 of 5,120).
// A persistent GPU depth mirror is therefore REMOVED: a batch that needs the private depth mirror
// is refused by the resident domain and takes the shipped CPU band replay, exactly as it does with
// the prototype disarmed. `DC2_G636_RESIDENT_Z=1` restores the persistent mirror (defect included).
static const bool s_g636ResidentZ = envFlagEnabled("DC2_G636_RESIDENT_Z");
// `g630UploadCompleteInternal` and `g634IngestUpload` mark a whole PAGE synced after writing only
// the transferred RECT into the raw image. Every other byte of that page then never gets patched
// again. `DC2_G636_NO_UPSYNCFIX=1` restores that.
static const bool s_g636NoUpSyncFix = envFlagEnabled("DC2_G636_NO_UPSYNCFIX");
static const bool s_g636Stat = envFlagEnabled("DC2_G636_STAT");

static std::atomic<uint64_t> g_g636ArbCt32{0u}, g_g636ArbT8{0u}, g_g636ArbRaw{0u};
static std::atomic<uint64_t> g_g636ArbTests{0u}, g_g636ArbHits{0u};
static std::atomic<uint64_t> g_g636BandPages{0u}, g_g636BandCalls{0u};
static std::atomic<uint64_t> g_g636FbSeed{0u}, g_g636MatPub{0u};
static std::atomic<uint64_t> g_g636ZStale{0u}, g_g636ZLost{0u}, g_g636ZSeeds{0u};
static std::atomic<uint64_t> g_g636ZRefused{0u};
// ⭐ THE CLOSURE ARITHMETIC. The private-depth population cannot be served by this kernel, so the
// question that decides the whole arc is: how much of the CPU band replay's POLE-THREAD POOL WALL
// does the remaining population carry? Set by the refusal, read where the batch's wall is booked.
static bool s_g636BatchRefusedZ = false;
static std::atomic<uint64_t> g_g636ZRefusedWallNs{0u}, g_g636ZKeptWallNs{0u};

// Defined in g630_gpu_domain.inc; the two early repair sites call them.
static void g630MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause);
static bool g630DomainOn();
static bool g630AnyDirty();
