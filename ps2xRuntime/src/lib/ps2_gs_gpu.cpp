// G397: reserved A+D display-alias fix plus presentation diagnostics live in the parts below.
// G412: force-recompile marker for immutable depth-two frame-boundary presentation snapshots.
// G399: DC2_G399_SURFDUMP raw render-target surface probe (diagnostic, default-off).
// G414: force-recompile marker for exact GS local-to-local self-copy retirement.
#include "ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc"
#include "ps2_gs_gpu_parts/gpu_g399_surface_probe.inc"
#include "ps2_gs_gpu_parts/gpu_display_and_snapshot.inc"
#include "ps2_gs_gpu_parts/gpu_gif_and_registers.inc"
#include "ps2_gs_gpu_parts/gpu_transfers_and_kick.inc"
