// ==============================================================================================
// G713 — PIPELINE, PART 2 of 2: the parse-side close/conflict tests and the exec-side batch adopt.
//
// Included immediately AFTER rasterizer_command_graph.inc (it needs `G260Batch`, `g_g260Graph`,
// `g_g260Pool`, `g_g260OpenWr/Rd`, `g260RangeSetsConflict`, `g260ExecuteAll`) and after
// rasterizer_draw_prim_probe.inc (it needs `g144RangeForRect`).
// ==============================================================================================

// ---------------------------------------------------------------------------------------------
// Recycled carriers. A closed batch travels parse -> exec by pointer; its (now empty) entry buffer
// travels back with the carrier, so neither side reallocates in steady state. Both mutexes are
// taken ~37x/frame, against a 10,000x/frame capture — they are not on any hot path.
// ---------------------------------------------------------------------------------------------
static std::mutex g_g713CarrierMtx;
static std::vector<G260Batch *> g_g713Carriers;

// G723: open the G712 prep record on the PARSE thread instead of at executor adoption. Default OFF;
// see the post site in g713ParseFlushOpen for why the extra lead time is the point.
static bool g722PrepAtParseOn()
{
    static const bool on = envFlagEnabled("DC2_G722_PREP_AT_PARSE");
    return on;
}

static G260Batch *g713TakeCarrier()
{
    {
        std::lock_guard<std::mutex> lk(g_g713CarrierMtx);
        if (!g_g713Carriers.empty())
        {
            G260Batch *b = g_g713Carriers.back();
            g_g713Carriers.pop_back();
            b->entries.clear();
            b->prep = nullptr;
            return b;
        }
    }
    return new G260Batch();
}

static void g713GiveCarrier(G260Batch *b)
{
    std::lock_guard<std::mutex> lk(g_g713CarrierMtx);
    if (g_g713Carriers.size() < 256u)
        g_g713Carriers.push_back(b);
    else
        delete b;
}

// ---------------------------------------------------------------------------------------------
// PARSE: close the open capture batch and post it. This is `g260CloseOpenBatch`'s body with the
// graph push replaced by a ring post. The G712 prep post is deliberately NOT made: the two
// mechanisms are alternatives, and G712's worker would otherwise race the exec thread for the
// same batch.
// ---------------------------------------------------------------------------------------------
static void g713ParseFlushOpen()
{
    if (g_g713ParseList.empty())
    {
        g_g260OpenWr.reset();
        g_g260OpenRd.reset();
        return;
    }
    G260Batch *b = g713TakeCarrier();
    b->entries.swap(g_g713ParseList);
    b->fbp = g_g713ParseFbp;
    b->wr = g_g260OpenWr;
    b->rd = g_g260OpenRd;
    b->prep = nullptr;
    // ⭐ G723 LEAD TIME. Posting the prep job at ADOPTION gives the worker only the executor's own
    // ring latency, and the census says that is not always enough: `helped=18.3%` of batches had
    // their `class` pass run by the executor because the worker had not started, plus `waited=4.8%`
    // that made the executor block on it. Posting HERE hands the job over one whole executor
    // backlog earlier (`syncMs/f` says parse runs ~10 ms ahead), which is what those two numbers
    // are asking for. Pointer identity survives the handover: both `b->entries.swap()` here and
    // `d.entries.swap(b->entries)` at adoption exchange vector HEADERS, so the heap buffer the
    // record points at — and the flush's own `g_g144List.swap(b.entries)` — is the same allocation
    // throughout, which is exactly what the consumer's identity check compares.
    if (g722PrepAtParseOn() && !b->entries.empty())
        b->prep = g712PrepOpen(b->entries.data(), b->entries.size(),
                               b->entries.front().ctx.frame.fbp,
                               b->entries.front().ctx.frame.fbw);
    g_g260OpenWr.reset();
    g_g260OpenRd.reset();
    g713PostBatch(b);
}

// ---------------------------------------------------------------------------------------------
// PARSE: does this VRAM operation conflict with the OPEN batch? Every CLOSED batch is already in
// the ring ahead of the operation, so only the open one can be reordered. Identical range algebra
// to `g260VramEventNeedsExecute`, restricted to the open sets.
// ---------------------------------------------------------------------------------------------
static bool g713CloseForUploadImpl(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                   uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    if (g_g713ParseList.empty())
        return false;
    const G144BlockRange dst = g144RangeForRect(dbp, static_cast<uint8_t>(dpsm), dbw,
                                                dsax, dsay, rrw, rrh);
    const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
    return g260RangeSetsConflict(g_g260OpenWr, g_g260OpenRd, G144BlockRange{}, dst,
                                 false, dstUnknown);
}

static bool g713CloseForLocalCopyImpl(uint32_t sbp, uint32_t sbw, uint32_t spsm,
                                      uint32_t ssax, uint32_t ssay,
                                      uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                      uint32_t dsax, uint32_t dsay,
                                      uint32_t rrw, uint32_t rrh)
{
    if (g_g713ParseList.empty())
        return false;
    const G144BlockRange src = g144RangeForRect(sbp, static_cast<uint8_t>(spsm), sbw,
                                                ssax, ssay, rrw, rrh);
    const G144BlockRange dst = g144RangeForRect(dbp, static_cast<uint8_t>(dpsm), dbw,
                                                dsax, dsay, rrw, rrh);
    const bool srcUnknown = (rrw != 0u && rrh != 0u && !src.valid);
    const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
    return g260RangeSetsConflict(g_g260OpenWr, g_g260OpenRd, src, dst, srcUnknown, dstUnknown);
}

// ---------------------------------------------------------------------------------------------
// EXEC: adopt one closed batch into the graph. Identical bookkeeping to `g260CloseOpenBatch`'s
// tail, including the fail-safe cap, so a piped run reaches the same drain causes as a shipped one.
//
// ⭐ G722 THIRD STAGE. `g260CloseOpenBatch`'s G712 prep-ahead post site is bypassed by the
// pipeline, so the identical post is made HERE instead. Placing it on adoption rather than on
// parse-side close is not a detail: g712_prep_body.inc's free list is deliberately lock-free
// because "allocation and release happen ONLY on the GS worker thread", and under the pipeline the
// consumer (`g178TryFlushGpu`) and `g712PrepRelease` both run on the EXEC thread. Posting from
// `g713ParseFlushOpen` would have put the free list on two threads; posting from adoption keeps
// open/consume/release on one, so the invariant the mechanism was written against still holds.
//
// The overlap this buys is real for the same reason G712 states: an adopted batch is immutable
// until it drains, and the census shows several batch closes per drain, so `class` (2.279 ms/f in
// the G720 stage census) can run on the prep worker while the executor is still submitting the
// batches ahead of it. Nothing is deleted, so the G304 conservation law does not apply.
// ---------------------------------------------------------------------------------------------
static void g713ExecBatchImpl(void *p)
{
    G260Batch *b = static_cast<G260Batch *>(p);
    if (b->entries.empty())
    {
        // Defensive: a parse-side record is only opened for a non-empty batch, so this cannot
        // normally hold one. Recycling the carrier while a worker still owns its record would be
        // the one unrecoverable shape here, so refuse to depend on that argument.
        g712PrepRelease(b->prep);
        g713GiveCarrier(b);
        return;
    }
    g_g260Graph.emplace_back();
    G260Batch &d = g_g260Graph.back();
    d.fbp = b->fbp;
    d.wr = b->wr;
    d.rd = b->rd;
    d.prep = nullptr;
    d.entries.swap(b->entries);
    // A record opened on the parse thread (G723 lead-time arm) travels with the carrier; adopt it
    // rather than opening a second one for the same buffer.
    d.prep = b->prep;
    b->prep = nullptr;
    // fbp/fbw from entry 0, exactly as the shipped post site derives them, so the consumer's
    // identity check is an equality on the same two derivations.
    if (d.prep == nullptr && !d.entries.empty())
        d.prep = g712PrepOpen(d.entries.data(), d.entries.size(),
                              d.entries.front().ctx.frame.fbp,
                              d.entries.front().ctx.frame.fbw);
    // Hand the carrier a recycled buffer so the parse thread never re-grows one.
    if (!g_g260Pool.empty())
    {
        b->entries = std::move(g_g260Pool.back());
        g_g260Pool.pop_back();
        b->entries.clear();
    }
    ++g_g260Closes;
    if (g_g260Graph.size() > g_g260MaxGraph)
        g_g260MaxGraph = g_g260Graph.size();
    g713GiveCarrier(b);
    if (g_g260Graph.size() > 96u)
        g260ExecuteAll(kG260Runaway, true); // the shipped fail-safe cap, unchanged
}

// ---------------------------------------------------------------------------------------------
// EXEC thread bring-up. The fused backend's former worker core is available for
// execution. Reusing the parser's GS role would pin both CPU stages to one core.
//
// ⚠️ G723: role 3 is free ONLY because G705's fused transport removed the backend worker. With
// DC2_G705_NO_FUSEGL=1 that worker comes back and pins role 3 itself
// (lle_gpu_raster_backend.inc), so the executor and the GL worker would share one core and any
// three-stage measurement would be meaningless. `DC2_G721_EXEC_ROLE=<n>` selects the executor's
// G650 role (default 3, the shipped fused-path choice); set it to 4 (MAIN) when restoring the
// backend worker. Clamped to the declared role range; anything else falls back to 3.
// ---------------------------------------------------------------------------------------------
static void g713ExecThreadInit()
{
    int role = 3; // G650_ROLE_GL
    if (const char *v = std::getenv("DC2_G721_EXEC_ROLE"))
    {
        const int parsed = std::atoi(v);
        if (parsed >= 0 && parsed < 5 /* G650_ROLE_COUNT */)
            role = parsed;
    }
    std::fprintf(stderr, "[G713:pipe] exec role=%d\n", role);
    std::fflush(stderr);
    ::g650PinThread(role); // declared at global scope in ps2_gs_rasterizer.cpp
    // ⭐ G735: arm the G446 PC sampler ON THIS THREAD. `g446RegisterGsWorkerThread` points at the
    // PARSE thread; the executor — 17.9 ms/f at 90% occupancy, the measured pole — has never had a
    // distribution. Default OFF (DC2_G735_EXECPROF=1); singleton, so never arm it together with
    // DC2_G446_GSPROF / DC2_G478_VU1PROF / DC2_G503_EEPROF.
    ::g735RegisterExecThread();
}

// ---------------------------------------------------------------------------------------------
// One-shot arm after first-packet hook registration and before its GS operations.
// Re-points the open-batch indirections and starts the executor before GL adoption.
// ---------------------------------------------------------------------------------------------
static void g713ArmIfRequested(GSRasterizer *self)
{
    (void)self;
    static bool s_tried = false;
    if (s_tried)
        return;
    s_tried = true;
    if (!g713PipeEnabled())
        return;
    G713RasterHooks h;
    h.closeForUpload = &g713CloseForUploadImpl;
    h.closeForLocalCopy = &g713CloseForLocalCopyImpl;
    h.flushOpen = &g713ParseFlushOpen;
    h.execBatch = &g713ExecBatchImpl;
    g713SetRasterHooks(h);
    g713SetThreadInit(&g713ExecThreadInit);
    if (!g713PipeStart())
        return;
    // From here the capture appends to parse-thread storage and `g_g144List` is exec-private.
    g_g713ParseList.reserve(g_g144List.capacity() ? g_g144List.capacity() : 4096u);
    if (g713PipeBatchOn())
    {
        g_g144Open = &g_g713ParseList;
        g_g144OpenFbp = &g_g713ParseFbp;
        g_g713On = true;
    }
}

// ---------------------------------------------------------------------------------------------
// EXEC: run the whole non-deferrable tail of `drawPrimitive` on the exec thread. The parse thread
// is blocked inside `g713CallOnExec` for the duration, so `self`, `gs`, `gs->m_vtxQueue` and every
// register the tail reads are frozen — which is why this needs no entry capture and no second copy
// of the ~200-line inline path. `t_g713ExecInline` steers the re-entry straight past the deferral
// predicate, so the predicate is evaluated exactly ONCE per primitive (on parse) and the address
// registries it mutates cannot be double-noted.
// ---------------------------------------------------------------------------------------------
struct G713InlineArg
{
    GSRasterizer *self;
    GS *gs;
};

static void g713ExecInlineThunk(void *p)
{
    G713InlineArg *a = static_cast<G713InlineArg *>(p);
    t_g713ExecInline = true;
    a->self->drawPrimitive(a->gs);
    t_g713ExecInline = false;
}
