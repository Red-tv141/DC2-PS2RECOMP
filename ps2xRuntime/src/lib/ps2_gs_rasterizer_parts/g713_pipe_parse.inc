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
// ---------------------------------------------------------------------------------------------
static void g713ExecBatchImpl(void *p)
{
    G260Batch *b = static_cast<G260Batch *>(p);
    if (b->entries.empty())
    {
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
// EXEC thread bring-up. `g650PinThread` gives the exec thread the GS worker's own affinity role:
// it IS the GS worker now, as far as every core-scheduling promotion since G650 is concerned, and
// the parse thread becomes the new front end.
// ---------------------------------------------------------------------------------------------
static void g713ExecThreadInit()
{
    ::g650PinThread(2 /* G650_ROLE_GS */); // declared at global scope in ps2_gs_rasterizer.cpp
}

// ---------------------------------------------------------------------------------------------
// One-shot arm, from the first captured primitive (so the GS is fully constructed and the runtime
// has read its environment). Re-points the two open-batch indirections and starts the exec thread.
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
