// G566: exact GPU-authoritative VU1 command producer and asynchronous packet-result handoff (v1).
// G545: incremental CPU-authoritative VU1 worker -> persistent GPU shadow stream (v2 trace).
// G478: runtime_mtvu_and_env.inc now arms the G446 PC sampler on the MTVU worker thread.
// G483: it also splits [G441:kick]'s exec bucket by kick kind ([G483:kind]) and by stop reason
// ([G483:stop]).
#include "ps2_runtime_parts/dc2_logger.inc"
#include "ps2_runtime_parts/runtime_mtvu_and_env.inc"
#include "ps2_runtime_parts/runtime_host_display.inc"
#include "ps2_runtime_parts/runtime_init_and_signals.inc"
#include "ps2_runtime_parts/runtime_guest_heap.inc"
#include "ps2_runtime_parts/dc2_profiler.inc"
#include "ps2_runtime_parts/runtime_dispatch_and_memory.inc"
// G617: DC2 performance profiler and telemetry system (presentation diagnostic once)
// G438: VU1-worker derivative spin gated by the G419 per-frame arm (force recompile v1).
// G482: runtime_g482_memo_census.inc added — memo-key price arm + full-identity census (force
// recompile v2; MSBuild does not track .inc dependencies).
// G541: exact VU1 kick corpus + asynchronous GPU compute transport prototype (force recompile v1).
// G616: RTSS presentation synchronization and frame-gated display loop (force recompile v1).
