// ==============================================================================================
// G713 — the parse/exec pipeline's PUBLIC SURFACE.
//
// Included by ps2_g713_pipeline.cpp (the cold definition TU), ps2_gs_gpu.cpp (the transfer/image
// producer and its exec-side bodies), ps2_gs_rasterizer.cpp (the capture/batch producer and the
// batch executor) and ps2_gif_arbiter.cpp (the frame boundary).
//
// ⚠️ EVERY cross-TU edge is a FUNCTION POINTER registered into the cold TU, never a direct call
// into another TU's anonymous namespace. `ps2_gs_rasterizer.cpp` is one long anonymous namespace,
// so its helpers have internal linkage and cannot be named from `ps2_gs_gpu.cpp`; hook
// registration is what makes this a four-TU mechanism with no header surgery and no rebuild of
// the 30,000-TU generated target.
//
// ⚠️ Rule 12b: DECLARATIONS ONLY in the hot TUs. Every definition lives in the cold TU, and no
// std:: type appears in this interface.
// ==============================================================================================
#ifndef DC2_G713_PIPELINE_API_INC
#define DC2_G713_PIPELINE_API_INC 1

#include <cstdint>

// The GS transfer registers, snapshotted at POST time on the parse thread and applied by exec
// immediately before the operation runs. These are the ONLY GS registers whose owner moves:
// everything else the GIF stream writes (PRIM/RGBAQ/ST/UV/XYZ/TEX0/FRAME/ZBUF/TEXA/...) stays with
// the parse thread, and everything the flush reads lives inside the captured entries.
struct G713XferSnap
{
    uint64_t bitbltbuf, trxpos, trxreg, trxdir; // the four RAW register words, replayed by exec
    uint32_t resetProgress;                     // 1 = also write TRXDIR (resets m_hwregX/Y)
    uint32_t pad;
};

// ---- hooks, registered once by each producing TU --------------------------------------------
struct G713RasterHooks
{
    // PARSE thread. "Must the open capture batch be closed and posted before this VRAM operation,
    // so entries captured BEFORE it cannot execute after it?" Exactly the g260 range test applied
    // to the open batch alone — every closed batch is already ahead of the node in the ring.
    bool (*closeForUpload)(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                           uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh) = nullptr;
    bool (*closeForLocalCopy)(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax, uint32_t ssay,
                              uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t dsax, uint32_t dsay,
                              uint32_t rrw, uint32_t rrh) = nullptr;
    void (*flushOpen)() = nullptr;       // PARSE: close the open capture batch and post it
    void (*execBatch)(void *) = nullptr; // EXEC:  adopt a closed batch into g_g260Graph
};
struct G713GsHooks
{
    void (*xfer)(const G713XferSnap *s) = nullptr;                                 // EXEC
    void (*image)(const G713XferSnap *s, const uint8_t *data, uint32_t n) = nullptr; // EXEC
};

void g713SetRasterHooks(const G713RasterHooks &h);
void g713SetGsHooks(const G713GsHooks &h);
void g713SetThreadInit(void (*threadInit)());

// ---- arms / state ---------------------------------------------------------------------------
bool g713PipeEnabled();     // DC2_G713_PIPE  (cached; false in every shipped run)
bool g713PipeSerialMode();  // DC2_G713_SERIAL — oracle: drain inline, never cross a thread
bool g713PipeStatOn();      // DC2_G713_PIPE_STAT
bool g713PipeImageOn();     // !DC2_G713_NO_IMAGE  (bisect, diagnostic only)
bool g713PipeBatchOn();     // !DC2_G713_NO_BATCH  (bisect, diagnostic only)
bool g713PipeArmed();       // the ONLY predicate the hot paths read
bool g713OnExecThread();    // true only on the exec thread
bool g713PipeStart();       // arms once every required hook is registered
void g713PipeStop();
void g713PipeReport();      // once per frame boundary; quiet unless PIPE_STAT

// ---- posts (parse thread) --------------------------------------------------------------------
void g713PostXfer(const G713XferSnap &s);
void g713PostImage(const G713XferSnap &s, const uint8_t *data, uint32_t bytes);
void g713PostBatch(void *closedBatch);

// ⭐ THE BYPASS. Run `fn(arg)` on the EXEC thread, in ring order, and block until it has returned.
// Every operation this phase has NOT moved — the non-deferrable inline primitive, the frame
// boundary closure, the G177 apply closure, any diagnostic that touches GL or guest VRAM — uses
// this instead of being reimplemented. Because the caller blocks, the callee may dereference live
// parse-thread state (`GS*`, `GSRasterizer*`, the vertex queue): the producer is stopped inside
// this call, so nothing it owns can change. Correct by construction, at the price of one round
// trip. `[G713:pipe] calls/f` is the census that says whether that price matters.
void g713CallOnExec(void (*fn)(void *), void *arg);
void g713Sync();            // block until every posted node has executed

// ---- parse-side bridges the GS TU calls (thin forwarders onto G713RasterHooks) ---------------
bool g713RasterCloseForUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                              uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh);
bool g713RasterCloseForLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax,
                                 uint32_t ssay, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                 uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh);
void g713RasterFlushOpen();

#endif // DC2_G713_PIPELINE_API_INC
