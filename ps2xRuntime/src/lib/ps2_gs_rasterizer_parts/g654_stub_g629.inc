// ===== G654 P5: THE G629 GPU-RESIDENT BAND REPLAY, COMPILED OUT ===============================
//
// Replaces `g629_gpu_band_replay.inc` + `g629_gpu_kernel.inc` (25,394 bytes / 174 COMDATs of
// `ps2_gs_rasterizer.obj`). Same disposition and same reasoning as `g654_stub_gpudomain.inc`:
// the marshalling arm needs `DC2_G629_GPU`, the census needs `DC2_G629_STAT` and the ceiling probe
// needs `DC2_G629_CEIL` — all default-off, all closed negative — so every entry point below is a
// constant in a shipped run, and rule 12c says a constant that is still COMPILED is not free.
//
//   real                     domain-off value        (source)
//   g629StatOn()             false                   g629_gpu_band_replay:103 (DC2_G629_STAT)
//   g629CeilSkipBatch()      false                   g629_gpu_band_replay:595 (!g629CeilOn())
//   g629TryReplayGpu()       false                   g629_gpu_kernel:564 (!g629GpuRequested())
//   g629VerifyAfterCpu()     no-op                   g629_gpu_kernel:773 (!g_g629VerifyArmed)
//   g629NoteBatch()          two relaxed adds into counters nothing reads with the census off
//
// ⚠️ `g629NoteBatch` is the one entry point whose real body is UNCONDITIONAL: it books the batch's
// pole-thread wall into `g_g636ZRefusedWallNs` / `g_g636ZKeptWallNs`. Those two counters exist only
// to answer the G636 closure question ("how much of the replay wall does the private-depth
// population carry?"), which is answered and archived; nothing reads them in a shipped run, and
// `s_g636BatchRefusedZ` is only ever set by the excluded resident admission. Dropping the adds is
// therefore observationally inert AND removes two atomic RMWs per replayed batch from the GS
// worker.
static constexpr bool g629StatOn() { return false; }
static constexpr bool g629CeilSkipBatch(const std::vector<G144Entry> &) { return false; }
static inline void g629NoteBatch(const std::vector<G144Entry> &, uint64_t, uint64_t) {}
static inline void g629VerifyAfterCpu(uint8_t *) {}
static inline bool g629TryReplayGpu(std::vector<G144Entry> &, GS *, uint8_t *, uint32_t,
                                    const G580ClutFillFn &)
{
    return false;
}
