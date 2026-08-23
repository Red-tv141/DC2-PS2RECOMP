int g473SimdMergeArm();
// G508: A/B arm for the m_stateMutex lock-skip lever, read once per GIF packet in
// GS::processGIFPacket. Defined at global scope in g419_ab_instrument.inc (rasterizer TU) - declared
// HERE, above every anonymous namespace, or it would bind to an internal-linkage symbol and fail to
// link (the appendix's anon-namespace trap).
int g508LockSkipArm();
// G596: A/B arm for the table-driven same-format local->local transfer body, read once per
// GS::performLocalToLocalTransfer call (~15/frame). Defined at global scope in
// g419_ab_instrument.inc (rasterizer TU) - declared HERE, above every anonymous namespace, for the
// same reason g508LockSkipArm is (the appendix's anon-namespace trap).
int g596CopyArm();
// G508: force-recompile marker - .inc edits do not trigger MSBuild (G359). revision: 2
// G596: force-recompile marker for the table-driven l2l copy. revision: 1
// G630: replay co-writes raw authority and linear FBO mirror. revision: 5
// G522: `g144L2lLatePin` - the fail-safe replay of the local->local guest-VRAM publication that the
// range barrier skipped when g289CanDeferLocalCopy promised an FBO->FBO blit. Declared with the
// other g144* externs at the top of gpu_transfers_and_kick.inc, called from
// Native High-Res Presentation Latch support v1

// G397: reserved A+D display-alias fix plus presentation diagnostics live in the parts below.
// G412: force-recompile marker for immutable depth-two frame-boundary presentation snapshots.
// G399: DC2_G399_SURFDUMP raw render-target surface probe (diagnostic, default-off).
// G414 diagnostic: force-recompile marker for default-off exact GS l2l self-copy retirement.
// G434 probe: force-recompile marker for DC2_G434_NO_DRAW (front-thread draw deletion ceiling).
// G440: present-latch component profile (DC2_G440_LATCH=1, behaviour-pure). Must precede the
// helpers/display parts so both can use G440LatchScope.
// g440_latch_profile.inc revision: 3 (merge loop restored to the vectorisable three-pass form)
// G473: exact SSE2 dual-CRT merge promoted + scalar byte oracle (force recompile v2).
// G599: DC2_G599_VRAMMAP=<every> — whole-VRAM page-hash map at the presentation latch
// (gpu_display_and_snapshot.inc), keyed on the SCRIPT clock. The presented image is a VRAM
// deswizzle read, so every visible pixel is in m_vram at that instant; hashing all 512 pages and
// diffing two arms localizes a visual defect to its pages without a hypothesis. `g_dc2ScriptFrame`
// is defined in ps2_memory.cpp and declared HERE, above every anonymous namespace, or it would bind
// G616: RTSS presentation synchronization support (force recompile v1).
#include <atomic>
#include <condition_variable>
#include <chrono>
extern std::atomic<uint32_t> g_dc2ScriptFrame;
// G602: Layer-4 (GIF decode) hot-path SHAPE. Every default-off diagnostic that used to sit inline in
// processGIFPacket / writeRegisterPacked / writeRegister / vertexKick now lives behind a
// `__declspec(noinline)` helper there, so MSVC stops giving those three hot members prologues of
// 5-8 GPR pushes, a 256-328 byte frame, 2-9 nonvolatile XMM spills and a /GS cookie (17d §2.1's
// measured call-boundary class). Compile-time rollbacks: DC2_G602_COLD_OUTLINE / DC2_G602_FLAG_GLOBALS.
// Must follow gpu_bridge_and_latch_helpers.inc (it uses that file's accessors and g123/g34 statics)
// and precede gpu_gif_and_registers.inc + gpu_transfers_and_kick.inc, which call into it.
// g602_gif_cold_outline.inc revision: 1  (G359 .inc rebuild marker — touch on every edit)
#include "ps2_gs_gpu_parts/g440_latch_profile.inc"
#include "ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc"
#include "ps2_gs_gpu_parts/g602_gif_cold_outline.inc"
// G432: TRXDIR cost census (DC2_G432_CENSUS=1, behaviour-pure). Must precede
// gpu_gif_and_registers.inc (the TRXDIR dispatch site) and gpu_transfers_and_kick.inc.
// g432_trxdir_census.inc revision: 2 (added dir=2 internal stage split)
#include "ps2_gs_gpu_parts/g432_trxdir_census.inc"
#include "ps2_gs_gpu_parts/gpu_g399_surface_probe.inc"
#include "ps2_gs_gpu_parts/gpu_display_and_snapshot.inc"
// G650 (ROADMAP P3): the PACKED descriptor bodies as macros, shared by writeRegisterPacked's
// switch and processGIFPacket's inline walk. Must precede both.
#include "ps2_gs_gpu_parts/g650_gif_packed_inline.inc"
#include "ps2_gs_gpu_parts/gpu_gif_and_registers.inc"
// G424: run-based host->local IMAGE upload writer, PROMOTED default-on
// (-2.45 ms/f, -4.69%); kill DC2_G424_NO_FAST_IMAGE=1.
// Must precede gpu_transfers_and_kick.inc, which calls into it from GS::processImageData.
// g424_fast_image_upload.inc revision: 4 (G359 .inc rebuild marker - touch on every edit)
#include "ps2_gs_gpu_parts/g424_fast_image_upload.inc"
// G596: table-driven same-format LOCAL->LOCAL transfer body (rollback DC2_G596_NO_FASTCOPY=1;
// bring-up DC2_G596_FASTCOPY=1 is a no-op now the body is default-ON). Must precede
// gpu_transfers_and_kick.inc, which calls it from GS::performLocalToLocalTransfer's format-aware
// branch.
// g596_fast_local_copy.inc revision: 3 (BYTE oracle replaces the address oracle; G359 marker)
#include "ps2_gs_gpu_parts/g596_fast_local_copy.inc"
#include "ps2_gs_gpu_parts/gpu_transfers_and_kick.inc"
// G446: env-read census hook in envFlagEnabled (force recompile v1).
// G446: cache DC2_DUMP_FONT at the T4HH upload site (force recompile v2).
