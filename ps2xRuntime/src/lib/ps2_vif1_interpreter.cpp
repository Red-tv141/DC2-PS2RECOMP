#include <chrono>
// G654 P16/P17: exclusive, thread-keyed layer timer (empty unless -DPS2X_G654_DIAG=ON).
#include "ps2_g654_layer_api.inc"
#include "ps2_vif1_interpreter_parts/vif1_probes_and_helpers.inc"
// G371: EE-side mgRENDER_INFO census (DC2_G371_RI=1). Must precede the parser: the census is
// called from processVIF1Data.
#include "ps2_vif1_interpreter_parts/vif1_g371_renderinfo_census.inc"
// G650 (ROADMAP P4): compiled UNPACK kernels. File scope, and it must precede the parser —
// vif1_unpack_engine.inc is a textual continuation of processVIF1Data and calls into it.
#include "ps2_vif1_interpreter_parts/vif1_g650_fast_unpack.inc"
#include "ps2_vif1_interpreter_parts/vif1_dma_and_parser.inc"
#include "ps2_vif1_interpreter_parts/vif1_command_handlers.inc"
#include "ps2_vif1_interpreter_parts/vif1_unpack_engine.inc"
// G410: precise VIF MPG invalidation hook added (force recompile).
