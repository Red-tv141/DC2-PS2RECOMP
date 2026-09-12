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
    // G721: resolved transfer command. No replay of parser register side effects.
    uint32_t sbp = 0, sbw = 0, spsm = 0, dbp = 0, dbw = 0, dpsm = 0;
    uint32_t ssax = 0, ssay = 0, dsax = 0, dsay = 0, dir = 0;
    uint32_t rrw = 0, rrh = 0, trxdir = 3, resetProgress = 0;
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
void g713_raster_arm(); // first packet, after transfer hooks and before any GS operation

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
// ⭐ G734: MONOTONIC executor totals for the acceptance board, always accumulated. The board's
// `gsOwn` is `g303_gs_worker_busy_ns()`, which is the PARSE thread's window branch only — since
// G726 it has not been able to see this thread at all. Deltas are the caller's job, exactly like
// `g303_gs_worker_busy_ns()`.
uint64_t g713_exec_busy_ns();
uint64_t g713_exec_idle_ns();
// ⭐⭐⭐ G735: the executor's REAL CPU (GetThreadTimes kernel+user), monotonic, so the acceptance
// board can compare it against `[G182:ee] cpuMs/f` in the SAME UNIT. `g713_exec_busy_ns` above is
// OCCUPANCY — it brackets a whole drain cycle, so the blocking readback and every lock inside it
// count as busy — and ranking that column against EE's work column is what made G734 call the
// executor the pole on 12 of 18 windows. Returns 0 when the pipeline is unarmed or the handle
// could not be duplicated; the reporter treats 0 as "no column".
uint64_t g713_exec_cpu_ns();
// Called once from the executor thread's own entry: GetCurrentThread() is a pseudo-handle valid
// only on the calling thread, so the real CPU accessor needs a duplicated one.
void g735ExecCaptureThreadHandle();

// ⭐ The generalised form. A board that gives only the executor a work column still ranks it
// against `[G303:vu1w] busyMs/f`, which is OCCUPANCY (G434/G476) — the VU1 worker's own entry
// comment records that it "has never had a sample distribution, only occupancy timers". One slot
// per worker thread closes that.
enum
{
    kG735SlotExec = 0,  // the G713/G721 executor
    kG735SlotVu1 = 1,   // the VU1 (MTVU) worker
    kG735SlotParse = 2, // the GS parse worker
    kG735ThreadSlots = 3
};
// Call once from the owning thread's entry. Cheap, idempotent-enough (a second call leaks one
// handle and is otherwise harmless), and a no-op off Windows.
void g735CaptureThreadHandle(int slot);
// Monotonic kernel+user CPU for that thread, or 0 if the slot was never captured. Deltas are the
// caller's job, exactly like `g713_exec_busy_ns()`.
uint64_t g735ThreadCpuNs(int slot);
// G726: once per frame boundary, ALWAYS. Latches the pipeline into inline-drain (SERIAL) mode on a
// route whose blocking-call rate makes threading a loss - `map15` measures 1060 calls/frame against
// `dungeon1`'s 1.0. Rollback DC2_G726_NO_ADAPT=1, threshold DC2_G726_ADAPT_CALLS (default 64).
void g713PipeAdaptTick();

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
// ⭐ G740: the same ordered node WITHOUT the round trip. The callee runs on the exec thread at its
// FIFO position and the caller returns immediately, so the callee must own everything it touches —
// it may not dereference live parse-thread state. Built for the frame-boundary closure, whose
// presentation inputs are already an immutable G412 snapshot. See ps2_g713_pipeline.cpp.
void g713PostCall(void (*fn)(void *), void *arg);
void g713Sync();            // block until every posted node has executed

// ---- parse-side bridges the GS TU calls (thin forwarders onto G713RasterHooks) ---------------
bool g713RasterCloseForUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                              uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh);
bool g713RasterCloseForLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax,
                                 uint32_t ssay, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                 uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh);
void g713RasterFlushOpen();

#endif // DC2_G713_PIPELINE_API_INC
