// G475: held paired arm for the VU1 compact VNOP trace; force-consume g419 instrument edit.
// G434: DC2_G434_NO_L2LPIN l2l-pin deletion ceiling probe in rasterizer_draw_sprite.inc ג€”
// content edit here forces MSBuild to consume it.
// G431: T8 resident-view rebuild redundancy census (DC2_G431_T8CENSUS=1) in
// rasterizer_vram_materialization.inc ג€” content edit here forces the .inc to be consumed.
// G411 promoted: force MSBuild to consume default shared GPU-depth ownership .inc changes.
// G415: publishes the exact colour-readback row window to the GPU backend (see
// rasterizer_vram_materialization.inc). Touching this file forces the .inc edit to be consumed.
// G417: diagnostic-only CPU framebuffer-pack split lives in that same part.
// G423: promotes G326's exact compiled mapping-plan cache; rollback DC2_G423_NO_PLAN_CACHE=1.
// G429: opt-in ceiling probe DC2_G429_NO_UPLOAD_EDGE=1 in rasterizer_draw_sprite.inc (default-off,
// knowingly incorrect) prices the upload-forced deferred-graph execute.
// G432: local->local flush-edge split census (DC2_G432_CENSUS=1) in rasterizer_draw_sprite.inc ג€”
// content edit here forces the .inc to be consumed. (rev 3: per-stage lap timers + the exact G418
// unpack at the SECOND publication site, g261Materialize; rollback DC2_G432_NO_MAT_UNPACK=1,
// A/B gate DC2_G419_AB=matunpack)
// G447: host core-count probe. Defined at global scope in ps2_gif_arbiter.cpp
// (g447_edge_census.inc), which is the TU that already pulls in <windows.h>. Declared here outside
// the parts' anonymous namespaces so DC2_G447_HOSTCHECK links against the real cross-TU symbol.
// G493: NEW part g493_late_drain.inc, included from rasterizer_vram_materialization.inc — moves the
// async break's drain from the flush TOP to just before that flush's submit, so the pending render
// overlaps the flush's own preparation instead of nothing (force recompile v1).
void g447HostShape(int *logicalOut, int *physicalOut);
int g456UvPredicateArm();

#include <deque>

#include "ps2_gs_rasterizer_parts/rasterizer_headers_and_diagnostics.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_g214_cap_trace.inc"

    #include "ps2_gs_rasterizer_parts/rasterizer_clipping_and_tex_checks.inc"

    #include "ps2_gs_rasterizer_parts/rasterizer_row_pool.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_tilebinning_and_probes.inc"

// G400/G401: raw surface sampling must be visible at command-graph execution time. G400's
// draw-entry hook remains below; G401 calls the helper before the composite batch executes.
#include "ps2_gs_rasterizer_parts/rasterizer_g400_stage_probe.inc"
#include "ps2_gs_rasterizer_parts/rasterizer_g401_composite_probe.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_draw_prim_probe.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_command_graph.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_g336_runway.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_rtt_census_and_waves.inc"

#include "ps2_gs_rasterizer_parts/g345_closure.inc"

// G399: DC2_G399_SURFDUMP joins the raw-VRAM diag flag set in the part below.
#include "ps2_gs_rasterizer_parts/rasterizer_gpu_alias_page_view.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_alias_page_state.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_page_authority.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_page_source_maps.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_target_page_authority.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_t8_maps_and_atlas.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_t8_map_resolver.inc"

// G342 arc#6 slice-1 premise-gate: per-consumer-shape TIME census of c5 tex-alias materialize.
// Included before vram_materialization.inc so g342NoteTexAlias is visible at the trigger site.
#include "ps2_gs_rasterizer_parts/g342_texalias_time_census.inc"

// G343: arc#4 slice-1 ג€” per-shape TIME census of the l2l consumer-floor edge (default-off)
#include "ps2_gs_rasterizer_parts/g343_l2l_floor_census.inc"

// G418: colour-readback VRAM unpack (CPU-copy component of the 0x13b readback edge) ג€” the exact
// table/paired/lane arm plus its profile and oracle. Included before vram_materialization.inc so
// g418UnpackColorRows is visible at the publication site.
#include "ps2_gs_rasterizer_parts/g418_readback_unpack.inc"

// G419: the generalized within-process randomized A/B instrument (DC2_G419_AB) plus the exact CT32
// upload fast pack. Included before vram_materialization.inc so g419AbTick/g419PackFbRows are
// visible at the once-per-frame tick point and at the framebuffer staging pack.
#include "ps2_gs_rasterizer_parts/g419_ab_instrument.inc"

// G420: the two staging-pack residues G419 left untested ג€” the exact zero-fill elision and the
// row-pool lane count ג€” plus the GSRowPool dispatch narrowing the lane-count mechanism turned out
// to require. Included before vram_materialization.inc so g420PrepStaging/g420PackLaneCount are
// visible at the framebuffer staging pack; rasterizer_row_pool.inc forward-declares
// g420PoolNarrowArm because it is included far earlier in this TU.
#include "ps2_gs_rasterizer_parts/g420_pack_residue.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_vram_materialization.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_setup_and_perf_census.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_tilebin_capture.inc"


// G369 cutscene gray-screen census (default-off: DC2_G369_CENSUS=1). Must sit AFTER the
// anonymous namespace opened in rasterizer_headers_and_diagnostics.inc has closed, because it
// declares the externally-linked g_dc2PresentTick from ps2_runtime.cpp.
#include "ps2_gs_rasterizer_parts/rasterizer_g369_scene_census.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_write_pixel.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_sprite.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_triangle.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_line.inc"
// G401: composite probe also hooks direct drawSprite execution (force recompile).
// G438: DC2_G419_AB=vu1slow|gsslow derivative probes + DC2_G438_HOLD block-held arms (force recompile v3, DC2_G447_HOSTCHECK).
// G439: DC2_G419_AB=coalesce window-count ceiling probe, g439CoalesceArm() (force recompile v2).
// G447: DC2_G419_AB=spinwait lever + kG447LeverSpinWait in g419_ab_instrument.inc (force recompile v1).
// G481: DC2_G419_AB=hotlower lever + g481HotLowerArm in g419_ab_instrument.inc (force recompile v1).
// G482: DC2_G419_AB=memohash lever + g482MemoHashArm in g419_ab_instrument.inc (force recompile v1).
// G484: DC2_G419_AB=tlsblock lever + g484TlsBlockArm in g419_ab_instrument.inc (force recompile v1).
// G488: DC2_G419_AB=fastop lever + g488FastOpArm in g419_ab_instrument.inc; added to the hold-16
//       default set because an arm flip re-decodes the whole G410 pair cache (force recompile v1).
// G454: readback dependency-edge A/B probes + callsite selection (force recompile v1).
// G455: DC2_G455_RUNWAY last-writer/readback runway census (force recompile v4).
// G456: paired exact G407 UV-predicate short circuit + shadow verifier (force recompile v4, promoted).
// G457: rejected narrow-boundary fast path removed; A/B used-state census retained (force recompile v4).
// G458: exact fully-scissor-clipped primitive rejection promoted default-on (force recompile v2).
// G459: rejected triangle/scissor behavior removed; diagnostic census retained (force recompile v2).
// G471: rejected spinclock/dirty-count levers removed; exact page-shift walks + bitset page-overlap
//       + G369 call-site gate hoist retained (force recompile v5).
// G473: DC2_G419_AB=simdmerge paired presentation-latch gate, promoted (force recompile v2).
// G474: frame-boundary g144FlushPending six-stage census (force recompile v1).
// G471: exact power-of-two SHIFT form of the page-range walks (g178PageShifts / g144PsmPageShifts)
//       in rasterizer_command_graph.inc + rasterizer_draw_prim_probe.inc (force recompile v2).
// G444: DC2_G444_ASYNC=1 ceiling probe ג€” admits G310 flushes to the G305 async slot (force recompile v1).
// G445: DC2_G419_AB=earlykick behaviour lever, g445KickArm() (force recompile v1).
// G445: DC2_G419_AB=kickall extension probe, g445KickAllArm() (force recompile v2).
// G446: cache g294OwnerTokenRequested / g283AuthorityRequested (per-page getenv on the pole thread) (force recompile v1).
// G446: cache DC2_DUMP_FONT in the per-texel T4HH sampler (force recompile v2).
// G452: DC2_G419_AB=async1 corrected one-slot G305/G310 async gate (force recompile v1).
// G452: rejected deeper FIFO removed; corrected one-slot diagnostic retained (force recompile v4).
// G453: rejected behavior levers removed (force recompile v6).
// G476: exact run-weighted page-generation walk (g178BumpRectImpl / g178GenSumRect) + visit-ceiling
//       census. Verified exact but PRICED OUT (~940 atomic RMWs/f, ~6 us/f); kept DEFAULT-OFF via
//       DC2_G476_PAGERUN=1, oracle DC2_G476_VERIFY=1, paired gate DC2_G419_AB=pagebump; the
//       lever-selected test is hoisted to a static so the default path pays no per-call query
//       (force recompile v6).
// G477: DC2_G419_AB=runwrite paired gate for the swizzle-row hoisted IMAGE run writers
//       (GSMem::WriteRunCT32/Z32/P4 in ps2_gs_memory.cpp) (force recompile v1).
