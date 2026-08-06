int g473SimdMergeArm();
// G508: A/B arm for the m_stateMutex lock-skip lever, read once per GIF packet in
// GS::processGIFPacket. Defined at global scope in g419_ab_instrument.inc (rasterizer TU) — declared
// HERE, above every anonymous namespace, or it would bind to an internal-linkage symbol and fail to
// link (the appendix's anon-namespace trap).
int g508LockSkipArm();
// G508: force-recompile marker — .inc edits do not trigger MSBuild (G359). revision: 1

// G397: reserved A+D display-alias fix plus presentation diagnostics live in the parts below.
// G412: force-recompile marker for immutable depth-two frame-boundary presentation snapshots.
// G399: DC2_G399_SURFDUMP raw render-target surface probe (diagnostic, default-off).
// G414 diagnostic: force-recompile marker for default-off exact GS l2l self-copy retirement.
// G434 probe: force-recompile marker for DC2_G434_NO_DRAW (front-thread draw deletion ceiling).
// G440: present-latch component profile (DC2_G440_LATCH=1, behaviour-pure). Must precede the
// helpers/display parts so both can use G440LatchScope.
// g440_latch_profile.inc revision: 3 (merge loop restored to the vectorisable three-pass form)
// G473: exact SSE2 dual-CRT merge promoted + scalar byte oracle (force recompile v2).
#include "ps2_gs_gpu_parts/g440_latch_profile.inc"
#include "ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc"
// G432: TRXDIR cost census (DC2_G432_CENSUS=1, behaviour-pure). Must precede
// gpu_gif_and_registers.inc (the TRXDIR dispatch site) and gpu_transfers_and_kick.inc.
// g432_trxdir_census.inc revision: 2 (added dir=2 internal stage split)
#include "ps2_gs_gpu_parts/g432_trxdir_census.inc"
#include "ps2_gs_gpu_parts/gpu_g399_surface_probe.inc"
#include "ps2_gs_gpu_parts/gpu_display_and_snapshot.inc"
#include "ps2_gs_gpu_parts/gpu_gif_and_registers.inc"
// G424: run-based host->local IMAGE upload writer, PROMOTED default-on
// (-2.45 ms/f, -4.69%); kill DC2_G424_NO_FAST_IMAGE=1.
// Must precede gpu_transfers_and_kick.inc, which calls into it from GS::processImageData.
// g424_fast_image_upload.inc revision: 4 (G359 .inc rebuild marker — touch on every edit)
#include "ps2_gs_gpu_parts/g424_fast_image_upload.inc"
#include "ps2_gs_gpu_parts/gpu_transfers_and_kick.inc"
// G446: env-read census hook in envFlagEnabled (force recompile v1).
// G446: cache DC2_DUMP_FONT at the T4HH upload site (force recompile v2).
