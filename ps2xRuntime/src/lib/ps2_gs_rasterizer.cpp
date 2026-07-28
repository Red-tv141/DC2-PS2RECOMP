// G407: force MSBuild to consume accurate UV and ordered speck provenance .inc changes.
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
