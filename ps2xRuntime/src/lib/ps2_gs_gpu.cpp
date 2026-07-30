// G397: reserved A+D display-alias fix plus presentation diagnostics live in the parts below.
// G412: force-recompile marker for immutable depth-two frame-boundary presentation snapshots.
// G399: DC2_G399_SURFDUMP raw render-target surface probe (diagnostic, default-off).
// G414 diagnostic: force-recompile marker for default-off exact GS l2l self-copy retirement.
#include "ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc"
#include "ps2_gs_gpu_parts/gpu_g399_surface_probe.inc"
#include "ps2_gs_gpu_parts/gpu_display_and_snapshot.inc"
#include "ps2_gs_gpu_parts/gpu_gif_and_registers.inc"
// G424: run-based host->local IMAGE upload writer, PROMOTED default-on
// (-2.45 ms/f, -4.69%); kill DC2_G424_NO_FAST_IMAGE=1.
// Must precede gpu_transfers_and_kick.inc, which calls into it from GS::processImageData.
// g424_fast_image_upload.inc revision: 4 (G359 .inc rebuild marker — touch on every edit)
#include "ps2_gs_gpu_parts/g424_fast_image_upload.inc"
#include "ps2_gs_gpu_parts/gpu_transfers_and_kick.inc"
