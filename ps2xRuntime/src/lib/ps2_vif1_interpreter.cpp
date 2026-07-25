#include "ps2_vif1_interpreter_parts/vif1_probes_and_helpers.inc"
// G371: EE-side mgRENDER_INFO census (DC2_G371_RI=1). Must precede the parser: the census is
// called from processVIF1Data.
#include "ps2_vif1_interpreter_parts/vif1_g371_renderinfo_census.inc"
#include "ps2_vif1_interpreter_parts/vif1_dma_and_parser.inc"
#include "ps2_vif1_interpreter_parts/vif1_command_handlers.inc"
#include "ps2_vif1_interpreter_parts/vif1_unpack_engine.inc"
