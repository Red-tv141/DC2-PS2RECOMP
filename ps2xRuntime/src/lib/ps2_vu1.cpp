#include "ps2_vu1_parts/vu1_helpers_and_tables.inc"
#include "ps2_vu1_parts/vu1_g370_store_watch.inc"
#include "ps2_vu1_parts/vu1_core_execution.inc"
#include "ps2_vu1_parts/vu1_g410_compiled_execution.inc"
#include "ps2_vu1_parts/vu1_upper_opcodes.inc"
#include "ps2_vu1_parts/vu1_lower_opcodes.inc"
#include "ps2_vu1_parts/vu1_dispatch_and_sync.inc"
// G360: item-slot UV-loss XGKICK probe added in vu1_dispatch_and_sync.inc (force recompile v2).
// G370: VU1 store-watch probe added (force recompile).
// G410: default-on cached compiled VU1 execution (force recompile v16).
// G413: default-on G328 circular MAC delay + G330 fused MAC classifier (force recompile).
