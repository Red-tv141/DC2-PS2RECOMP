#include "ps2_vu1_parts/vu1_helpers_and_tables.inc"
#include "ps2_vu1_parts/vu1_g370_store_watch.inc"
#include "ps2_vu1_parts/vu1_core_execution.inc"
#include "ps2_vu1_parts/vu1_g422_fast_lower.inc"
#include "ps2_vu1_parts/vu1_g410_compiled_execution.inc"
#include "ps2_vu1_parts/vu1_g421_fast_upper.inc"
#include "ps2_vu1_parts/vu1_g426_fused_upper.inc"
#include "ps2_vu1_parts/vu1_g421_census.inc"
#include "ps2_vu1_parts/vu1_upper_opcodes.inc"
#include "ps2_vu1_parts/vu1_lower_opcodes.inc"
#include "ps2_vu1_parts/vu1_dispatch_and_sync.inc"
// G360: item-slot UV-loss XGKICK probe added in vu1_dispatch_and_sync.inc (force recompile v2).
// G370: VU1 store-watch probe added (force recompile).
// G410: default-on cached compiled VU1 execution (force recompile v16).
// G413: default-on G328 circular MAC delay + G330 fused MAC classifier (force recompile).
// G421: default-on inline SIMD upper slot + hot-loop census (kill DC2_G421_NO_FAST_UPPER, v3).
// G422: default-on inline lower slot (kill DC2_G422_NO_FAST_LOWER, force recompile v1).
// G425: zero-work pair run census + run-collapsing lever (force recompile v1).
// G426: branchless fused upper FMAC dispatch (DC2_G426_FUSED_UPPER, force recompile v1).
// G427: default-on lazy MAC/STATUS delay-line publication (kill DC2_G427_NO_LAZY_FLAGS, v1).
// G428: fast-upper SIMD-op census + dest-shape store + deferred MAC classification (v2).
// G430: DC2_G430_SKIP ceiling probe + default-on stamped MAC history (force recompile v2).
// G435: pair loop split into vu1_g435_pair_loop.inc (included twice); three OPT-IN levers
//       (DC2_G435_LEAN_LOOP / _WIDE_VF0 / _HAZ_REG) + the DC2_G435_SKIP ceiling probe (v5).
// G436: default-on lean hot-path addressing — constexpr G421 desc/mask tables (no init guard, so
//       no `call g421Desc` per covered upper), one G430HistBlock thread_local hoisted once per
//       run(), and the per-pair s_g106UseLowerFlagRead store gated on G106 parallel flags.
//       Exact and frame-neutral; kill DC2_G436_LEGACY_ADDR=1 (force recompile v1).
// G437: VU1 register-file alignment premise probe DC2_G437_ALIGN=1 (force recompile v1).
// G438: whole-tree alignment sweep probe DC2_G438_ALIGN=1 (force recompile v1).
// G442: subnormal census (compile-time DC2_G442_DENORM_CENSUS), rejected register-resident PC,
//       and the branch-delay loop-carried-local fold (force recompile v3).



