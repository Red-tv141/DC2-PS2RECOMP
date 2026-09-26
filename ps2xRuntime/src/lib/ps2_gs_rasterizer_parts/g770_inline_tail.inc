// ==============================================================================================
// G770-A — THE INLINE (NON-DEFERRABLE) TAIL OF drawPrimitive, as one captureless lambda.
//
// #included at the TOP of GSRasterizer::drawPrimitive (rasterizer_setup_and_perf_census.inc), so
// the lambda is lexically inside a member of GS's friend class and keeps private access to both
// GSRasterizer (drawSprite / drawTriangle / drawLine / writePixel) and GS (m_prim, m_vtxQueue, ...).
//
// The body is the shipped code VERBATIM, moved, not rewritten:
//   `edge == true`  the tile-bin inline publication/materialisation edge that used to close the
//                   capture block (G260 prefix drain, G278/G289/G272/G276/G261/G242 edges),
//                   followed by the CPU rasterisation;
//   `edge == false` the CPU rasterisation alone (the path that never entered the capture block).
// Only three things changed: member calls take `self->`, `ctx`/`pt144` are re-derived from `gs`
// (they are the same expressions the prologue used), and the G144 mid-flush helper — a local
// lambda of drawPrimitive — is re-created here from the same two globals.
//
// It is ALSO the exec-thread body of an asynchronously posted inline primitive (G770-A,
// g770_async_inline.inc): `s_g770RunJob` below loads a job's snapshot into a thread-local scratch
// GS — the band replay's `s_seq` technique — and calls this with `edge == true`.
// ==============================================================================================
static void (*const s_g770InlineTail)(GSRasterizer *, GS *, bool) = +[](GSRasterizer *self, GS *gs, bool edge) {
    const auto &ctx = gs->activeContext();
    const uint32_t pt144 = static_cast<uint32_t>(gs->m_prim.type);
    (void)pt144;
    if (edge)
    {
        static const bool s_g144Seq = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &g144DoFlush = s_g144Seq ? g_g144FlushSeqFn : g_g144FlushFn;
        auto g144TimedMidFlush = [&]() {
            G146PerfScope g146FlushScope(g_g146G144FlushMidNs, g_g146G144FlushMidCount);
            g144DoFlush();
        };
        (void)g144TimedMidFlush;
        if (g260NrOn())
        {
            // G260: an inline (non-recordable) primitive is about to read/write VRAM directly.
            // Execute the whole pending graph first so it observes the same VRAM evolution
            // order as the immediate-drain baseline. Covers both the open batch and any closed
            // batches (the legacy branch below only ever has the open list).
            if (g260HasPending())
            {
                if (!g_g144List.empty())
                    g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                                       ctx.frame.fbp, g_g144List.size(), pt144,
                                       gs->m_prim.tme, gs->m_prim.abe);
                g260ExecuteAll(kG260NonDef, true);
            }
        }
        else if (!g_g144List.empty())
        {
            g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                               ctx.frame.fbp, g_g144List.size(), pt144,
                               gs->m_prim.tme, gs->m_prim.abe);
            g144TimedMidFlush();
        }
        g526Lap(kG526Drain); // G526: inline path — pending-graph drain before this primitive
        // This primitive will execute on the CPU (RTT/line/point/diagnostic path).  The preceding
        // drain may have left guest Z resident on the GPU, so restore authoritative bytes before
        // any CPU depth test, texture sample, or Z write.
        // G261: same for GPU-resident RTT rows — materialize any resident target this
        // primitive's frame/Z/texture/CLUT pages touch before the CPU reads or writes them.
        // G278 range-exact edge: culled or disjoint inline primitives do not break a same-key Z
        // run; real target/Z/texture/CLUT aliases publish before resident color materialization.
        (void)g278FlushPendingDepthForInline(
            gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        (void)g278FlushPendingDepthForDisplayOwners(4);
        const G289InlineAudit g289Inline =
            g345AnyOwnerActive()
                ? g289AuditInline(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut)
                : G289InlineAudit{};
        if (g345AnyOwnerActive() && g289StatOn())
        {
            static std::atomic<uint32_t> s_g289InlineLog{0};
            const uint32_t n = s_g289InlineLog.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(
                    stderr,
                    "[G289:inline-edge] n=%u owner=%x..%x carry=%u empty=%u unknown=%u "
                    "prim=%u tme=%u abe=%u fst=%u "
                    "frame=%03x/%u/%x sc=%u,%u..%u,%u tex=%x/%u/%x/%u/%u "
                    "zbuf=%llx test=%llx box=%d,%d..%d,%d "
                    "ranges(c=%x..%x z=%x..%x t=%x..%x p=%x..%x) "
                    "v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                    n + 1u, s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock,
                    g289Inline.carry ? 1u : 0u, g289Inline.empty ? 1u : 0u,
                    g289Inline.unknown ? 1u : 0u,
                    static_cast<unsigned>(gs->m_prim.type), gs->m_prim.tme ? 1u : 0u,
                    gs->m_prim.abe ? 1u : 0u, gs->m_prim.fst ? 1u : 0u,
                    ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw), ctx.frame.psm,
                    ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                    ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), ctx.tex0.psm,
                    static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                    static_cast<unsigned long long>(ctx.zbuf),
                    static_cast<unsigned long long>(ctx.test),
                    g289Inline.x0, g289Inline.y0, g289Inline.x1, g289Inline.y1,
                    g289Inline.color.first, g289Inline.color.last,
                    g289Inline.depth.first, g289Inline.depth.last,
                    g289Inline.texture.first, g289Inline.texture.last,
                    g289Inline.clut.first, g289Inline.clut.last,
                    gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                    gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                    gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y);
        }
        if (g289Inline.carry)
            g_g289InlineCarries.fetch_add(1u, std::memory_order_relaxed);
        else
            (void)g289MaterializeOwner(4);
        // ⭐ G769: the inline edge published EVERY display record (live + carried) and every
        // display wave for any inline CPU primitive, whatever pages it touches - map15: 2,634
        // carried display publications + ~1.4 display read-backs/f (0x0 / 0x68 @96/416). A display
        // record is a consumer edge only for a primitive whose colour/depth/texture/CLUT pages
        // reach it; the same scissor-clipped audit G289 already trusts for its owner answers that.
        // Unknown ranges keep the whole publication. Rollback DC2_G769_NO_INLINEDISP=1.
        static const bool s_g769InlineDisp = !envFlagEnabled("DC2_G769_NO_INLINEDISP");
        const G289InlineAudit g769Disp =
            !s_g769InlineDisp ? G289InlineAudit{}
            : g345AnyOwnerActive() ? g289Inline
                                   : g289AuditInline(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        const bool g769Ranged = s_g769InlineDisp && !g769Disp.unknown;
        if (!g769Ranged)
            g272MaterializeAll(4); // inline CPU primitive: conservative display-color consumer edge
        else if (!g769Disp.empty)
        {
            g272MaterializeForRanges(g769Disp.texture, g769Disp.color, false, false, 4);
            g272MaterializeForRanges(g769Disp.clut, g769Disp.depth, false, false, 4);
        }
        g277CensusInlineEdge(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        if (!g769Ranged)
            g276FlushPendingDisplay(4); // G276: publish deferred display readbacks first
        else if (!g769Disp.empty)
        {
            g276FlushPendingDisplayForRanges(4, g769Disp.texture, g769Disp.color, false, false);
            g276FlushPendingDisplayForRanges(4, g769Disp.clut, g769Disp.depth, false, false);
        }
        g261PrepareInline(ctx, gs->m_prim, gs->m_texclut);
        g242PrepareVramReadAll(gs->m_vram);
        g526Lap(kG526Edge); // G526: inline path — ownership/materialize/publish edge
    }

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        self->drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        // G156: time inline (non-deferred) tri raster on the calling thread. Under MTGS+G144 only
        // NON-deferrable tris reach here (deferrable ones returned early), so inlineMs pairs with
        // g_g156InlineTri. Replay calls drawTriangle directly (never this switch) → no double count.
        if (g_g156Stat && !t_g144InReplay)
        {
            const auto g156T0 = std::chrono::steady_clock::now();
            self->drawTriangle(gs);
            g_g156InlineTriNs.fetch_add(
                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g156T0)
                    .count(),
                std::memory_order_relaxed);
        }
        else
            self->drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        self->drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        self->writePixel(gs, px, py, v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }
    g526Lap(kG526Raster); // G526: inline path — the actual CPU rasterization of this primitive

    // G178: an INLINE (non-deferred) draw just wrote guest VRAM directly (e.g. the costume RTT at
    // fbp=0x139). Bump the write generations of the pages its scissored frame region can touch so
    // any GPU-resident texture decoded from those pages (RTT-as-texture) is re-decoded, not stale.
    // Conservative (scissor rect ⊇ written pixels); a no-op single boolean read when the lever is
    // off, and skipped during replay (replay is the GPU path's own CPU fallback, whose target is
    // the display fb the front-end re-uploads unconditionally anyway).
    if (g178GpuOn() && !t_g144InReplay)
    {
        const auto &ctx178 = gs->activeContext();
        // G277-R: bump the prim's CLAMPED bbox ∩ scissor, not the whole scissor rect. Writes are
        // confined to that intersection by construction, so this stays conservative — but a fully
        // scissored-out prim (e.g. the offscreen sceGsSwapDBuff sync linestrips at x≈1792..2303)
        // rasterizes NOTHING and must bump NOTHING. The old whole-scissor bump invalidated the
        // full display target ~1×/frame per buffer, forcing a spurious fb re-upload every batch
        // (and, under G276 deferral, the stale-VRAM clobber behind the MAP-0 Max-body flicker).
        // Kill (restores whole-scissor bump): DC2_G277_NO_INLINE_BBOX_BUMP=1.
        static const bool s_g277BboxBump = !envFlagEnabled("DC2_G277_NO_INLINE_BBOX_BUMP");
        int bx0 = ctx178.scissor.x0, bx1 = ctx178.scissor.x1;
        int by0 = ctx178.scissor.y0, by1 = ctx178.scissor.y1;
        bool bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        if (s_g277BboxBump && !bumpEmpty)
        {
            int nv = 3;
            switch (gs->m_prim.type)
            {
            case GS_PRIM_POINT: nv = 1; break;
            case GS_PRIM_LINE:
            case GS_PRIM_LINESTRIP:
            case GS_PRIM_SPRITE: nv = 2; break;
            default: nv = 3; break;
            }
            const int ofx178 = ctx178.xyoffset.ofx >> 4;
            const int ofy178 = ctx178.xyoffset.ofy >> 4;
            float mnx = gs->m_vtxQueue[0].x, mxx = gs->m_vtxQueue[0].x;
            float mny = gs->m_vtxQueue[0].y, mxy = gs->m_vtxQueue[0].y;
            for (int vi = 1; vi < nv; ++vi)
            {
                mnx = std::min(mnx, gs->m_vtxQueue[vi].x);
                mxx = std::max(mxx, gs->m_vtxQueue[vi].x);
                mny = std::min(mny, gs->m_vtxQueue[vi].y);
                mxy = std::max(mxy, gs->m_vtxQueue[vi].y);
            }
            bx0 = std::max(bx0, static_cast<int>(std::floor(mnx)) - ofx178);
            bx1 = std::min(bx1, static_cast<int>(std::ceil(mxx)) - ofx178);
            by0 = std::max(by0, static_cast<int>(std::floor(mny)) - ofy178);
            by1 = std::min(by1, static_cast<int>(std::ceil(mxy)) - ofy178);
            bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        }
        if (!bumpEmpty)
        {
            g178NoteVramWriteRectTagged(gs->m_vram,
                                  GSInternal::framePageBaseToBlock(ctx178.frame.fbp),
                                  ctx178.frame.fbw, ctx178.frame.psm,
                                  static_cast<uint32_t>(std::max(bx0, 0)),
                                  static_cast<uint32_t>(std::max(by0, 0)),
                                  static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                  static_cast<uint32_t>(by1 - std::max(by0, 0) + 1),
                                  6u); // G582 source 6: inline-draw bbox color bump
            if (((ctx178.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRectTagged(gs->m_vram,
                                      static_cast<uint32_t>(ctx178.zbuf & 0x1FFu) << 5u,
                                      ctx178.frame.fbw,
                                      static_cast<uint32_t>((ctx178.zbuf >> 24u) & 0xFu),
                                      static_cast<uint32_t>(std::max(bx0, 0)),
                                      static_cast<uint32_t>(std::max(by0, 0)),
                                      static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                      static_cast<uint32_t>(by1 - std::max(by0, 0) + 1),
                                      7u); // G582 source 7: inline-draw bbox depth bump
            g768InlEscAfterDraw(ctx178, gs->m_prim, bx0, by0, bx1, by1);
        }
    }

};

// G770-A: the EXEC-thread runner for an asynchronously posted inline primitive. Installed into
// `g_g770RunInlineJob` on the first drawPrimitive call (parse thread), before any post exists.
static const bool s_g770RunJobInstalled = [] {
    g_g770RunInlineJob = +[](G770InlineJob *j) {
        static thread_local GS s_g770Gs;
        s_g770Gs.m_vram = j->vram;
        s_g770Gs.m_vramSize = j->vramSize;
        s_g770Gs.m_privRegs = j->privRegs;
        s_g770Gs.m_ctx[0] = j->ctx[0];
        s_g770Gs.m_ctx[1] = j->ctx[1];
        s_g770Gs.m_prim = j->prim;
        s_g770Gs.m_texa = j->texa;
        s_g770Gs.m_texclut = j->texclut;
        s_g770Gs.m_pabe = j->pabe;
        for (int i = 0; i < 6; ++i)
            s_g770Gs.m_vtxQueue[i] = j->vtx[i];
        t_g364FogCol = j->fogcol;
        t_g144TitleRockScopeOverride = true;
        t_g144TitleRockScope = j->titleRockScope;
        t_g144TownDepthScopeOverride = true;
        t_g144TownDepthScope = j->townDepthScope;
        t_g770AsyncTail = true;
        t_g713ExecInline = true;
        s_g770InlineTail(j->self, &s_g770Gs, true);
        t_g713ExecInline = false;
        t_g770AsyncTail = false;
        t_g144TitleRockScopeOverride = false;
        t_g144TownDepthScopeOverride = false;
    };
    return true;
}();
(void)s_g770RunJobInstalled;
