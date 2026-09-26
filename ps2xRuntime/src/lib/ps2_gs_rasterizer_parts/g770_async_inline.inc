// ==============================================================================================
// G770-A — ASYNCHRONOUS INLINE (NON-DEFERRABLE) PRIMITIVES: the parse -> exec round trip removed.
//
// Included immediately AFTER g713_pipe_parse.inc (it needs `g713ParseFlushOpen` and the G713 API).
//
// THE WALL, MEASURED (g769m, uncapped `DC2_G769_VBLANK_US=8333`, map15, clean, stack sampler over
// 950 samples per thread, `tools/g770_waits.py` + `tools/g770_callers.py`):
//
//     parse  41.6 %  wait<> <- g713Sync <- g713CallOnExec <- GSRasterizer::drawPrimitive
//     exec   28.6 %  wait<> <- g713ExecLoop                   (ring EMPTY: starved)
//     VU1    36.5 %  idle
//
// A two-stage ping-pong, exactly the G740 boundary defect one level down: every non-deferrable
// primitive (lines, points, CPU-only RTT sprites/triangles, ...) made the parse thread post a
// node, then BLOCK until the executor had drained its whole backlog AND run the primitive — and
// while it was blocked the parse thread produced nothing, so the executor then starved. G726
// measured this population at ~1,060 round trips per frame on map15.
//
// WHY THE BLOCK WAS THERE. `g713ExecInlineThunk` re-enters `drawPrimitive(gs)` on the exec thread
// with the REAL `gs`, and that re-entry reads the live GS registers (m_ctx, m_prim, m_vtxQueue,
// TEXA, TEXCLUT, PABE, FOGCOL) — which the parse thread would be overwriting with the next
// primitive the instant it was released. The block froze them. It was never an ordering
// requirement: the ring is FIFO, so the primitive runs at its correct position either way.
//
// WHAT THIS DOES. The parse thread snapshots exactly the state a deferred G144 entry carries (the
// band replay's `s_seq` contract, rasterizer_tilebin_capture.inc — ctx, prim, texa, texclut,
// pabe, fogcol, the two scope bits, the vertices) into a pooled job and posts it with
// `g713PostCall` — non-blocking. The executor loads the job into a thread-local SCRATCH GS (the
// same technique the replay uses) and runs ONLY the inline tail: the publication/materialisation
// edge and the CPU rasterisation, factored into `s_g770InlineTail` inside drawPrimitive. The
// drawPrimitive PROLOGUE is not re-run on the exec thread any more (it mutates parse-side statics:
// the costume-Z fbp edge, the G687 fast-capture run, the G527 registry), which the old blocking
// re-entry only tolerated because the parse thread was parked.
//
// WHAT STAYS BLOCKING (fallback to g713CallOnExec, counted):
//   * a sprite with TME&ABE&FST&CTXT — drawSprite's G172 display-copy hoist writes
//     `gs->m_preferredDisplaySource*`, which the parse thread snapshots at the frame boundary;
//     that write must land on the REAL GS before the parse thread moves on.
//   * local->host (TRXDIR=1) is untouched: its result is CPU-visible to the EE.
//
// SERIAL mode (`DC2_G713_SERIAL`) is unchanged by construction: g713PostCall drains inline, so
// the job runs before the post returns, on the same thread, from the same snapshot.
//
// Flags:  DC2_G770_NO_ASYNC_INLINE=1   rollback (restores the blocking round trip)
//         DC2_G770_STAT=1               `[G770:inline]` posted / blocking / pool census
// ==============================================================================================

struct G770InlineJob
{
    GSRasterizer *self = nullptr;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
    struct GSRegisters *privRegs = nullptr;
    GSContext ctx[2]{};
    GSPrimReg prim{};
    GSVertex vtx[6]{};
    GSTexaReg texa{0u, false, 0u};
    GSTexClutReg texclut{0u, 0u, 0u};
    bool pabe = false;
    uint32_t fogcol = 0u;
    bool titleRockScope = false;
    bool townDepthScope = false;
};

// Installed by drawPrimitive (it needs GSRasterizer/GS private access, which only code lexically
// inside a member of the friend class has). Written once on the parse thread before the first
// post; the ring's release/acquire publication orders it before the exec thread's first read.
static void (*g_g770RunInlineJob)(G770InlineJob *) = nullptr;

static bool g770AsyncInlineOn()
{
    static const bool on = !envFlagEnabled("DC2_G770_NO_ASYNC_INLINE");
    return on;
}

static bool g770StatOn()
{
    static const bool on = envFlagEnabled("DC2_G770_STAT");
    return on;
}

static std::atomic<uint64_t> g_g770Posted{0}, g_g770Blocking{0}, g_g770Ran{0};

// Parse acquires, exec releases: a mutex-guarded free list (~1k/frame, never contended in
// practice — the two sides touch it at different ends of a multi-microsecond node).
static std::mutex g_g770PoolMtx;
static std::vector<G770InlineJob *> g_g770Pool;

static G770InlineJob *g770AcquireJob()
{
    {
        std::lock_guard<std::mutex> lk(g_g770PoolMtx);
        if (!g_g770Pool.empty())
        {
            G770InlineJob *j = g_g770Pool.back();
            g_g770Pool.pop_back();
            return j;
        }
    }
    return new G770InlineJob();
}

static void g770ReleaseJob(G770InlineJob *j)
{
    std::lock_guard<std::mutex> lk(g_g770PoolMtx);
    if (g_g770Pool.size() < 4096u)
        g_g770Pool.push_back(j);
    else
        delete j;
}

static void g770Report()
{
    if (!g770StatOn())
        return;
    const uint64_t p = g_g770Posted.load(std::memory_order_relaxed);
    const uint64_t b = g_g770Blocking.load(std::memory_order_relaxed);
    if (((p + b) & 0x3FFFull) != 0ull)
        return;
    std::fprintf(stderr, "[G770:inline] posted=%llu ran=%llu blocking=%llu (async %.1f%%)\n",
                 static_cast<unsigned long long>(p),
                 static_cast<unsigned long long>(g_g770Ran.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(b),
                 (p + b) ? 100.0 * static_cast<double>(p) / static_cast<double>(p + b) : 0.0);
    std::fflush(stderr);
}

// EXEC (or SERIAL inline drain): run one posted inline primitive, then recycle its job.
static void g770ExecInlineThunk(void *p)
{
    G770InlineJob *j = static_cast<G770InlineJob *>(p);
    const uint64_t g770T0 = g770XTraceActive() ? g770XTraceNow() : 0u;
    if (g_g770RunInlineJob != nullptr)
        g_g770RunInlineJob(j);
    if (g770T0 != 0u)
        g770XTrace('I', static_cast<uint32_t>(j->prim.type), j->ctx[j->prim.ctxt ? 1 : 0].frame.fbp, 0u, g770T0);
    g_g770Ran.fetch_add(1u, std::memory_order_relaxed);
    g770ReleaseJob(j);
}
