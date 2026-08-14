// G566: GPU-authoritative VU1 packet publication and one-readback-per-16-kick prototype (v3).
// G587: include transient 0x15d in the explicit raw-alpha two-pass (force .inc recompile v2).
// G433: blocking colour-readback ceiling probe in lle_gpu_raster_backend.inc
// (DC2_G433_NO_RB=1 skips only glReadPixels; DC2_G433_RBSTAT=1 times it). Content edit here
// forces MSBuild to consume the .inc.
// G546: CPU dead-history MAX/MIN + image-resident live VU code/RAM workspace (v31).
// G545: batched live VU1 GPU shadow, persistent workspace + delayed checkpoints.
// G565: clean rebuild of the resumed G545 exact semantic source.
// G544: one-dispatch persistent VU1 chain + bounded ordered XGKICK packet-ring oracle (v4).
// G543: exact default-off GPU VU1 semantic replay + one-readback corpus oracle (v6 denorm repair).
// G541: coalesced async no-readback VU1 null transport; timed gather refuted/removed (v4).
// G431: T8 resident-view GL storage reuse in lle_gpu_raster_backend.inc — measured NEUTRAL,
// opt-in only (DC2_G431_VIEW_REUSE=1). Content edit here forces MSBuild to consume the .inc.
// G158a: GPU-raster infrastructure prototype (default-off, DC2_G158_GPURASTER=1).
//
// Scope (deliberate, see plans/phase-G158-fix-log.md): prove the FBO + shader + readback
// round-trip on a SYNTHETIC textured triangle, entirely on the main thread, reusing raylib's
// already-current GL context (no second/shared context, no MTGS worker-thread integration yet
// -- those are separate, harder axes deferred to a later increment once this one is proven).
// This file is NOT wired into drawPrimitive()/drawTriangle() in this phase -- it runs once at
// startup, self-contained, and has zero effect on real title geometry or the shipped default
// (DC2_G158_GPURASTER unset -> a single cached boolean read, nothing else executes).
//
// No header edits (matches the G157 precedent: PS2Recomp/ps2xRuntime/include/runtime/
// ps2_gs_gpu.h is pulled into the generated dc2_game MSVC target -- touching it risks a 30+
// hour full rebuild). g158RunSelfTest()/g158_gpu_raster_enabled() are declared as inline
// `extern` forward declarations at their call site (ps2_runtime.cpp), the same idiom already
// used for g150_pipeline_enabled()/g150_mtgs_enabled().

// G362/G407: guarded exact POINT fetch plus point/linear hardware UV rounding -- see
// lle_gpu_raster_backend.inc. Touching this file forces MSBuild to recompile it after an
// .inc-only edit (the G359 stale-link trap).
//
// G415: command-zero submit stage census (DC2_G415_CENSUS=1) plus the narrowed colour-readback
// window (DC2_G415_NARROW_READBACK=1 / DC2_G415_NO_NARROW_READBACK=1) live in the same parts.
// G417: diagnostic-only upload attribution also lives in the same parts.
// G418: colour-readback drain/transfer attribution (DC2_G418_PROFILE=1) and the early command kick
// (DC2_G418_EARLY_KICK=1 / DC2_G418_NO_EARLY_KICK=1) live in the same parts.
// G419: the narrowed colour readback is now switchable per frame by the within-process A/B
// instrument (DC2_G419_AB=narrow) and carries an idempotent re-read ballast (DC2_G419_BALLAST).
// G420: DC2_G419_AB=narrowtouch adds the full read's DESTINATION-side write volume to the narrow
// arm, into an inert backend-private scratch buffer, to close G419's unaccounted narrow-readback
// payoff. Diagnostic only; no GL call and no observable byte change.
// G420: DC2_G420_NARROW_ROWS=<n> widens the narrow read to n rows (exact for any n >= the published
// window) to separate a per-row cost from a full-attachment driver step.
// The instrument contract lives in ps2_gs_rasterizer_parts/g419_ab_instrument.inc.

#include "ps2_gs_gpu_lle.h" // G178: private front-end<->backend interface (both build branches)

#if defined(_WIN32) && !defined(PLATFORM_VITA)

// G566 result ABI must live at global scope in both translation units; including it from a parts
// file inside the backend's anonymous namespace changes the C++ type and therefore the bridge name.
#include "ps2_gs_gpu_raster_parts/g566_vu_authority_shared.inc"

// G445: within-process A/B arm selector for the G418 early command kick. Defined at global scope in
// ps2_gs_rasterizer_parts/g419_ab_instrument.inc; declared here (outside the parts' anonymous
// namespace) so the backend site links against the real cross-TU symbol.
int g445KickArm();
int g445KickAllArm();

// G447: GS-worker blocking-edge census. Defined at global scope in ps2_gif_arbiter.cpp
// (g447_edge_census.inc); declared here — outside the parts' anonymous namespace — so the
// backend's fut.get() sites link against the real cross-TU symbols (the G445 lesson).
bool g447EdgeOn();
bool g447OnGsWorker();
void g447NoteWait(int edge, unsigned long long ns);
// G447 spin-then-block lever arm (DC2_G419_AB=spinwait); defined in g419_ab_instrument.inc.
int g447SpinArm();
int g447WorkerSpinArm();
// G491 arms (DC2_G419_AB=rbfmt / =rbspin); defined at global scope in g419_ab_instrument.inc.
int g491RbFmtArm();
int g491RbSpinArm();
// G492 ceiling-probe arms (DC2_G419_AB=norender / =noother) and the per-arm workload-invariant
// counter that makes them admissible (17a law 22). Same global-scope contract as the G491 arms.
int g492NoRenderArm();
int g492NoOtherArm();
int g492Ct24ReuseArm();
void g492NoteEdge(int edge, unsigned long long draws, unsigned long long skipped);
// G494 sync-query elision arm (DC2_G419_AB=nosync). Same global-scope contract as the G491/G492 arms
// — declared here, ABOVE the anonymous namespace the parts sit inside.
int g494NoSyncArm();
// G496 GPU derivative-probe arm (DC2_G419_AB=gpuslow). Same global-scope contract as the G491/G492
// arms — declared here, ABOVE the anonymous namespace the parts sit inside.
int g496GpuSlowArm();
// G496b redundant-GL-state-elision arm (DC2_G419_AB=glstate). Same global-scope contract.
int g496GlStateArm();

#include "ps2_gs_gpu_raster_parts/gpu_raster_infrastructure.inc"
#include "ps2_gs_gpu_raster_parts/persistent_t8_decoder.inc"
#include "ps2_gs_gpu_raster_parts/g491_readback_format.inc"
#include "ps2_gs_gpu_raster_parts/g492_overlap_ceiling.inc"
#include "ps2_gs_gpu_raster_parts/g433_pbo_readback.inc"
#include "ps2_gs_gpu_raster_parts/g494_driver_sync_census.inc"
#include "ps2_gs_gpu_raster_parts/g495_readback_queue_census.inc"
#include "ps2_gs_gpu_raster_parts/g496_gpu_ballast.inc"
#include "ps2_gs_gpu_raster_parts/g496_gl_state_cache.inc"
#include "ps2_gs_gpu_raster_parts/lle_gpu_raster_backend.inc"
#include "ps2_gs_gpu_raster_parts/gpu_raster_bridge_and_stubs.inc"
// G425: readback-redundancy ceiling census added in lle_gpu_raster_backend.inc (force recompile v1).
// G445: DC2_G419_AB=earlykick arm selector wired into the early-kick site (force recompile v2).
// G447: spin-then-block backend round trip + edge census hooks (force recompile v2, PROMOTED).
// G445: early kick PROMOTED default-ON + DC2_G445_KICK_ALL extension probe (force recompile v3).
// G452: recycle completed async-slot vector capacity to the GS front end (force recompile v1).
// G452: rejected deeper FIFO removed; one-slot capacity recycling retained (force recompile v4).
// G453: backend-other subtype census (DC2_G453_CENSUS=1; force recompile v1).
// G453: rejected behavior levers removed; subtype census retained (force recompile v6).
// G471: both spin-poll reshapes REJECTED and removed; loops restored verbatim (force recompile v3).
// G472: same-FBO synchronous render-round-trip opportunity census (force recompile v3).
// G491: native-format colour readback (g491_readback_format.inc) + the no-spin readback wait
// (force recompile v1).
// G492: backend-render / backend-other ceiling probes + the per-arm workload-invariant census
// (g492_overlap_ceiling.inc; force recompile v1).
// G494: driver-round-trip census + the sync-query elision, PROMOTED default-ON
// (g494_driver_sync_census.inc; census DC2_G494_SYNC=1 — which now needs DC2_G494_NO_ELIDE=1
// alongside it to see anything; rollback DC2_G494_NO_ELIDE=1; arm DC2_G419_AB=nosync;
// force recompile v2).
// G495: queued-ahead census for the SIX colour readbacks (DC2_G495_RB=1, DC2_G495_SEQ=1) in
// g495_readback_queue_census.inc — [G418:edge] only ever instrumented the ONE render-batch
// readback, so backend-readcolor has never had a drain/transfer split (force recompile v1).
// G496: the GPU DERIVATIVE PROBE (g496_gpu_ballast.inc; DC2_G419_AB=gpuslow, calibration
// DC2_G496_FORCE=1 DC2_G496_CAL=1). Identity-blend replay of each render batch's vertex range —
// real fragment work, zero byte change. Diagnostic only, never promotable (force recompile v1).
// G518: readback SUBSUMPTION census inside g495_readback_queue_census.inc (`SUB=` on the
// [G495:rb] line, DC2_G495_RB=1). G495's redundancy clock is keyed on (fbp,glY,rows), so a read
// contained in an earlier WIDER read of the same clean surface was never tested — and this
// route's four 0x139 windows are nested 480/32 c 384/128 c 64/448 (force recompile v1).
#else // !(_WIN32 && !PLATFORM_VITA)

bool g158_gpu_raster_enabled() { return false; }
void g158RunSelfTest() {}
void g158CaptureMainContext() {}
void g158RunWorkerContextTest() {}
void g158StartDedicatedGpuThread() {}
void g162StartPersistentDecoder() {}
bool g162DecodeT8ToRgba(uint32_t, uint32_t, int, int, const uint32_t *, const uint8_t *, size_t, uint32_t *) { return false; }
bool g162DecodeT8Batch(int, const uint32_t *, const uint32_t *, const int *, const int *,
                       const uint32_t *, const uint8_t *, size_t, uint32_t **) { return false; }
bool g178_backend_ready() { return false; }
bool g178_backend_submit(G178Batch &) { return false; }
bool g570_backend_shadow139(const std::vector<uint32_t> &, const std::vector<uint32_t> &,
                            const std::vector<uint32_t> &, const std::vector<uint32_t> &,
                            const std::vector<uint32_t> &, const std::vector<uint32_t> &,
                            std::vector<uint32_t> &) { return false; }
bool g541_backend_null_compute() { return false; }
bool g545_backend_live_submit(std::vector<uint8_t> &&) { return false; }
bool g566_backend_authority_submit(
    std::vector<uint8_t> &&, const std::shared_ptr<G566VuAuthorityResult> &) { return false; }
bool g178_backend_submit_async(G178Batch &) { return false; }
bool g178_backend_drain_async(bool &ok) { ok = true; return false; }
bool g242_backend_submit_depth(G178Batch &, uint64_t, const std::vector<float> *, int, int) { return false; }
bool g275_backend_submit_depth_readback(G178Batch &, uint64_t, const std::vector<float> *,
                                        int, int, int, int, std::vector<float> &) { return false; }
bool g242_backend_read_depth(uint64_t, int, int, int, int, std::vector<float> &) { return false; }
void g415_backend_set_color_window(int, int) {}
bool g178_backend_has_tex(uint64_t) { return false; }
bool g178_backend_read_color(uint32_t, int, int, int, int, std::vector<uint32_t> &) { return false; }
bool g178_backend_write_color(uint32_t, int, int, int, int, const std::vector<uint32_t> &) { return false; }
bool g264_backend_write_color_rect(uint32_t, int, int, int, int, int, int,
                                   const std::vector<uint32_t> &) { return false; }
bool g280_backend_copy_color_rects(uint32_t, uint32_t, const std::vector<int32_t> &) { return false; }
bool g309_backend_build_authoritative_composite(
    const std::vector<uint32_t> &, const std::vector<uint32_t> &,
    bool, std::vector<uint32_t> &) { return false; }
bool g310_backend_init_logical(uint32_t, const std::vector<uint32_t> &) { return false; }
bool g310_backend_copy_logical_pages(uint32_t, const std::vector<int32_t> &) { return false; }
bool g310_backend_build_logical_composite(
    const std::vector<uint32_t> &, const std::vector<uint32_t> &) { return false; }
bool g289_backend_copy_display_to_work(uint32_t, uint32_t) { return false; }
bool g289_backend_build_work_alias_view() { return false; }
bool g281_backend_prepare_t8_view(uint64_t, uint32_t, int, int, int,
                                  const std::vector<uint32_t> &,
                                  const std::vector<uint32_t> &,
                                  bool, uint64_t &bad) { bad = 0u; return false; }
bool g338_backend_prepare_ct24_view(uint64_t, uint32_t, int, int, int, int, int,
                                    bool, uint64_t &bad) { bad = 0u; return false; }
void g178StartPersistentBackend() {}

#endif
