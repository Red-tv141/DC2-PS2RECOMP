// G158a: GPU-raster infrastructure prototype (default-off, DC2_G158_GPURASTER=1).
//
// Scope (deliberate, see plans/phase-G158-fix-log.md): prove the FBO + shader + readback
// round-trip on a SYNTHETIC textured triangle, entirely on the main thread, reusing raylib's
// already-current GL context (no second/shared context, no MTGS worker-thread integration yet
// -- those are separate, harder axes deferred to a later increment once this one is proven).
// This file is NOT wired into drawPrimitive()/drawTriangle() in this phase -- it runs once at
// startup, self-contained, and has zero effect on real title geometry or the shipped default
// (DC2_G158_GPURASTER unset -> a single cached boolean read, nothing else executes).
//
// No header edits (matches the G157 precedent: PS2Recomp/ps2xRuntime/include/runtime/
// ps2_gs_gpu.h is pulled into the generated dc2_game MSVC target -- touching it risks a 30+
// hour full rebuild). g158RunSelfTest()/g158_gpu_raster_enabled() are declared as inline
// `extern` forward declarations at their call site (ps2_runtime.cpp), the same idiom already
// used for g150_pipeline_enabled()/g150_mtgs_enabled().

#include "ps2_gs_gpu_lle.h" // G178: private front-end<->backend interface (both build branches)

#if defined(_WIN32) && !defined(PLATFORM_VITA)

#include "ps2_gs_gpu_raster_parts/gpu_raster_infrastructure.inc"
#include "ps2_gs_gpu_raster_parts/persistent_t8_decoder.inc"
#include "ps2_gs_gpu_raster_parts/lle_gpu_raster_backend.inc"
#include "ps2_gs_gpu_raster_parts/gpu_raster_bridge_and_stubs.inc"
#else // !(_WIN32 && !PLATFORM_VITA)

bool g158_gpu_raster_enabled() { return false; }
void g158RunSelfTest() {}
void g158CaptureMainContext() {}
void g158RunWorkerContextTest() {}
void g158StartDedicatedGpuThread() {}
void g162StartPersistentDecoder() {}
bool g162DecodeT8ToRgba(uint32_t, uint32_t, int, int, const uint32_t *, const uint8_t *, size_t, uint32_t *) { return false; }
bool g162DecodeT8Batch(int, const uint32_t *, const uint32_t *, const int *, const int *,
                       const uint32_t *, const uint8_t *, size_t, uint32_t **) { return false; }
bool g178_backend_ready() { return false; }
bool g178_backend_submit(G178Batch &) { return false; }
bool g178_backend_submit_async(G178Batch &) { return false; }
bool g178_backend_drain_async(bool &ok) { ok = true; return false; }
bool g242_backend_submit_depth(G178Batch &, uint64_t, const std::vector<float> *, int, int) { return false; }
bool g275_backend_submit_depth_readback(G178Batch &, uint64_t, const std::vector<float> *,
                                        int, int, int, int, std::vector<float> &) { return false; }
bool g242_backend_read_depth(uint64_t, int, int, int, int, std::vector<float> &) { return false; }
bool g178_backend_has_tex(uint64_t) { return false; }
bool g178_backend_read_color(uint32_t, int, int, int, int, std::vector<uint32_t> &) { return false; }
bool g178_backend_write_color(uint32_t, int, int, int, int, const std::vector<uint32_t> &) { return false; }
bool g264_backend_write_color_rect(uint32_t, int, int, int, int, int, int,
                                   const std::vector<uint32_t> &) { return false; }
bool g280_backend_copy_color_rects(uint32_t, uint32_t, const std::vector<int32_t> &) { return false; }
bool g309_backend_build_authoritative_composite(
    const std::vector<uint32_t> &, const std::vector<uint32_t> &,
    bool, std::vector<uint32_t> &) { return false; }
bool g310_backend_init_logical(uint32_t, const std::vector<uint32_t> &) { return false; }
bool g310_backend_copy_logical_pages(uint32_t, const std::vector<int32_t> &) { return false; }
bool g310_backend_build_logical_composite(
    const std::vector<uint32_t> &, const std::vector<uint32_t> &) { return false; }
bool g289_backend_copy_display_to_work(uint32_t, uint32_t) { return false; }
bool g289_backend_build_work_alias_view() { return false; }
bool g281_backend_prepare_t8_view(uint64_t, uint32_t, int, int, int,
                                  const std::vector<uint32_t> &,
                                  const std::vector<uint32_t> &,
                                  bool, uint64_t &bad) { bad = 0u; return false; }
void g178StartPersistentBackend() {}

#endif
