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
