// G411 promoted: force MSBuild to consume default shared GPU-depth ownership .inc changes.
// G415: publishes the exact colour-readback row window to the GPU backend (see
// rasterizer_vram_materialization.inc). Touching this file forces the .inc edit to be consumed.
// G417: diagnostic-only CPU framebuffer-pack split lives in that same part.
// G423: promotes G326's exact compiled mapping-plan cache; rollback DC2_G423_NO_PLAN_CACHE=1.
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

// G343: arc#4 slice-1 — per-shape TIME census of the l2l consumer-floor edge (default-off)
#include "ps2_gs_rasterizer_parts/g343_l2l_floor_census.inc"

// G418: colour-readback VRAM unpack (CPU-copy component of the 0x13b readback edge) — the exact
// table/paired/lane arm plus its profile and oracle. Included before vram_materialization.inc so
// g418UnpackColorRows is visible at the publication site.
#include "ps2_gs_rasterizer_parts/g418_readback_unpack.inc"

// G419: the generalized within-process randomized A/B instrument (DC2_G419_AB) plus the exact CT32
// upload fast pack. Included before vram_materialization.inc so g419AbTick/g419PackFbRows are
// visible at the once-per-frame tick point and at the framebuffer staging pack.
#include "ps2_gs_rasterizer_parts/g419_ab_instrument.inc"

// G420: the two staging-pack residues G419 left untested — the exact zero-fill elision and the
// row-pool lane count — plus the GSRowPool dispatch narrowing the lane-count mechanism turned out
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
