#include <chrono>
// G654 P16/P17: exclusive, thread-keyed layer timer. Empty struct unless
// -DPS2X_G654_DIAG=ON (rule 12c: a diagnostic in a hot TU is a compile-time build mode).
#include "ps2_g654_layer_api.inc"
#include "ps2_memory_parts/memory_mmu_and_scratchpad.inc"
#include "ps2_memory_parts/memory_bus_and_io.inc"
#include "ps2_memory_parts/memory_dma_and_transfers.inc"
#include "ps2_memory_parts/memory_page_table_and_translate.inc"
// G360: cell-bg submit-path probe added in memory_page_table_and_translate.inc (force recompile).
