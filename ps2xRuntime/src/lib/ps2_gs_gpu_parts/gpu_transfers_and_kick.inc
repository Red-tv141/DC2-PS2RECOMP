// G144/G145: defined in ps2_gs_rasterizer.cpp; drains pending tile-binning deferred triangles
// before transfers that overlap VRAM they depend on. Unknown ranges fall back to flushing.
// G149: bump the de-swizzle texture-cache VRAM generation so stale decoded buffers are invalidated.
// G260: native-renderer command graph (opt-in DC2_G260_NR=1) — when active, transfers must take
// the RANGE-tested barrier path (the graph executes only on real dependency edges).
extern bool g260NrEnabled();
extern void g149BumpVramGen();
extern void g144FlushPendingUpload();
extern void g144FlushPendingUploadRange(uint32_t dbp,
                                        uint8_t dbw,
                                        uint8_t dpsm,
                                        uint32_t dsax,
                                        uint32_t dsay,
                                        uint32_t rrw,
                                        uint32_t rrh);
extern void g144FlushPendingLocalTransferRange(uint32_t sbp,
                                               uint8_t sbw,
                                               uint8_t spsm,
                                               uint32_t ssax,
                                               uint32_t ssay,
                                               uint32_t dbp,
                                               uint8_t dbw,
                                               uint8_t dpsm,
                                               uint32_t dsax,
                                               uint32_t dsay,
                                               uint32_t rrw,
                                               uint32_t rrh);
// G242: when opt-in GPU guest-depth ownership is active, materialize an overlapping
// GPU-resident depth surface before a transfer reads its guest-VRAM source. No-op otherwise.
extern void g242PrepareVramReadRect(uint8_t *vram,
                                    uint32_t bpBlocks,
                                    uint32_t bw64,
                                    uint32_t psm,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t w,
                                    uint32_t h);
extern bool g242GuestDepthEnabled();
extern bool g289TryGpuLocalCopy(uint8_t *vram, uint32_t vramSize,
                                uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                uint32_t ssax, uint32_t ssay,
                                uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                uint32_t dsax, uint32_t dsay,
                                uint32_t rrw, uint32_t rrh);
extern void g289NoteUploadComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                   uint32_t dsax, uint32_t dsay,
                                   uint32_t rrw, uint32_t rrh);

void GS::performLocalToLocalTransfer()
{
    G146PerfScope g146Scope(g_g146GsLocalNs);
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on)
        {
            static thread_local char s_g219tag[80];
            std::snprintf(s_g219tag, sizeof(s_g219tag), "l2l(sbp=0x%x dbp=0x%x rr=%ux%u)",
                          m_bitbltbuf.sbp, m_bitbltbuf.dbp, m_trxreg.rrw, m_trxreg.rrh);
            g219Sentinel(m_vram, m_vramSize, s_g219tag);
        }
    }

    static const bool s_g145EnvDirtyUpload = envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                             !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD");
    const bool g145DirtyUpload = (g268LiveEnvRead()
                                      ? (envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                         !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD"))
                                      : s_g145EnvDirtyUpload) ||
                                 g260NrEnabled();
    if (!g145DirtyUpload)
        g144FlushPendingUpload();

    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = m_bitbltbuf.sbw;
    uint8_t spsm = m_bitbltbuf.spsm;
    uint32_t dbp = m_bitbltbuf.dbp;
    uint8_t dbw = m_bitbltbuf.dbw;
    uint8_t dpsm = m_bitbltbuf.dpsm;

    if (sbw == 0)
        sbw = 1;
    if (dbw == 0)
        dbw = 1;

    const uint32_t rrw = m_trxreg.rrw;
    const uint32_t rrh = m_trxreg.rrh;
    const uint32_t ssax = m_trxpos.ssax;
    const uint32_t ssay = m_trxpos.ssay;
    const uint32_t dsax = m_trxpos.dsax;
    const uint32_t dsay = m_trxpos.dsay;
    const GSTransferTraversal traversal = decodeTransferTraversal(m_trxpos.dir);
    const bool formatAware = (spsm == dpsm) && supportsFormatAwareLocalCopy(spsm);

    if (rrw == 0u || rrh == 0u)
    {
        return;
    }

    if (g145DirtyUpload)
    {
        g144FlushPendingLocalTransferRange(sbp, sbw, spsm, ssax, ssay,
                                           dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    }

    // G242: the range/full G144 barrier above must run first so every deferred draw
    // that can feed this copy has reached its GPU depth surface before we read VRAM.
    g242PrepareVramReadRect(m_vram, sbp, sbw, spsm, ssax, ssay, rrw, rrh);

    const bool g289GpuCopy =
        g289TryGpuLocalCopy(m_vram, m_vramSize,
                            sbp, sbw, spsm, ssax, ssay,
                            dbp, dbw, dpsm, dsax, dsay, rrw, rrh);

    g149BumpVramGen(); // VRAM about to change: invalidate the de-swizzle texture cache
    // G178/G242: notify only after pending draws and source depth have been resolved.
    // The hook materializes an overlapping GPU-owned destination before the CPU copy writes it.
    {
        extern void g178NoteVramWriteRect(uint8_t *, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t, uint32_t);
        g178NoteVramWriteRect(m_vram, dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    }
    if (g289GpuCopy)
        return; // exact logical write is versioned in fbp=0x13b's FBO

    PS2_IF_AGRESSIVE_LOGS({
        if ((spsm == GS_PSM_T4 || dpsm == GS_PSM_T4) &&
            s_debugLocalCopyCount.fetch_add(1u, std::memory_order_relaxed) < 96u)
        {
            RUNTIME_LOG("[gs:l2l] sbp=" << sbp
                                        << " dbp=" << dbp
                                        << " sbw=" << static_cast<uint32_t>(sbw)
                                        << " dbw=" << static_cast<uint32_t>(dbw)
                                        << " spsm=0x" << std::hex << static_cast<uint32_t>(spsm)
                                        << " dpsm=0x" << static_cast<uint32_t>(dpsm) << std::dec
                                        << " ss=(" << ssax << "," << ssay << ")"
                                        << " ds=(" << dsax << "," << dsay << ")"
                                        << " rr=(" << rrw << "," << rrh << ")"
                                        << " dir=" << static_cast<uint32_t>(m_trxpos.dir)
                                        << " formatAware=" << (formatAware ? 1 : 0) << std::endl);
        }
    });

    // G285 PROMOTION (2026-07-17): DEFAULT-ON with the native-renderer stack. A CT32 transfer
    // whose source and destination both cover whole GS pages in the same FBW layout has an
    // address-preserving one-to-one mapping: every 64x32 logical page occupies one contiguous
    // 8192-byte physical page. When the physical spans are disjoint, traversal order is
    // irrelevant, so page-sized memcpy calls are exactly equivalent to the per-pixel copy.
    //
    // All graph barriers, generation bumps, and downstream presentation hooks above still run.
    // The rasterizer hook may prove a destructive destination and publish only its uncovered
    // page-row; all other resident-target materialization remains unchanged.
    // Fail closed on partial pages, shifted origins, unlike layouts, overlap, or VRAM wrap.
    // Kill: DC2_G285_NO_L2L_PAGE_COPY=1. Positive flag remains accepted but is redundant.
    static const bool s_g285PageCopyOn =
        !envFlagEnabled("DC2_G285_NO_L2L_PAGE_COPY");
    static const bool s_g285StatOn = envFlagEnabled("DC2_G285_STAT");
    const uint64_t g285Bytes = static_cast<uint64_t>(rrw) *
                               static_cast<uint64_t>(rrh) * 4ull;
    const uint64_t g285SrcBase = static_cast<uint64_t>(sbp) * 256ull;
    const uint64_t g285DstBase = static_cast<uint64_t>(dbp) * 256ull;
    const uint64_t g285SrcEnd = g285SrcBase + g285Bytes;
    const uint64_t g285DstEnd = g285DstBase + g285Bytes;
    const bool g285Disjoint = g285SrcEnd <= g285DstBase || g285DstEnd <= g285SrcBase;
    const bool g285Eligible =
        formatAware &&
        spsm == GS_PSM_CT32 &&
        sbw == dbw &&
        (sbp & 31u) == 0u &&
        (dbp & 31u) == 0u &&
        ssax == 0u && ssay == 0u &&
        dsax == 0u && dsay == 0u &&
        rrw == static_cast<uint32_t>(sbw) * 64u &&
        (rrh & 31u) == 0u &&
        g285SrcEnd <= static_cast<uint64_t>(m_vramSize) &&
        g285DstEnd <= static_cast<uint64_t>(m_vramSize) &&
        g285Disjoint;
    const bool g285UsePageCopy = g260NrEnabled() && s_g285PageCopyOn && g285Eligible;
    const auto g285BodyT0 = (s_g285StatOn && g285Eligible)
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    if (g285UsePageCopy)
    {
        // Keep each copy at one physical GS page. Besides mirroring the proof boundary, this
        // avoids the CRT's large-copy streaming-store path: the destination is consumed again
        // immediately by the renderer, so evicting/bypassing its cache lines can erase the
        // transfer-body win in end-to-end timing.
        constexpr size_t kG285PageBytes = 8192u;
        const size_t pages = static_cast<size_t>(g285Bytes) / kG285PageBytes;
        for (size_t page = 0; page < pages; ++page)
        {
            const size_t pageOff = page * kG285PageBytes;
            std::memcpy(m_vram + static_cast<size_t>(g285DstBase) + pageOff,
                        m_vram + static_cast<size_t>(g285SrcBase) + pageOff,
                        kG285PageBytes);
        }
    }
    else if (formatAware)
    {
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t srcY = transferCoord(ssay, rrh, row, traversal.reverseY);
            const uint32_t dstY = transferCoord(dsay, rrh, row, traversal.reverseY);
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t srcX = transferCoord(ssax, rrw, col, traversal.reverseX);
                const uint32_t dstX = transferCoord(dsax, rrw, col, traversal.reverseX);
                const uint32_t pixel =
                    readTransferPixel(m_vram, m_vramSize, sbp, sbw, spsm, srcX, srcY);
                writeTransferPixel(m_vram, m_vramSize, dbp, dbw, dpsm, dstX, dstY, pixel);
            }
        }
    }
    else
    {
        const uint32_t srcBase = sbp * 256u;
        const uint32_t dstBase = dbp * 256u;
        uint32_t srcBpp = bitsPerPixel(spsm) / 8u;
        uint32_t dstBpp = bitsPerPixel(dpsm) / 8u;
        if (srcBpp == 0)
            srcBpp = 4;
        if (dstBpp == 0)
            dstBpp = 4;
        const uint32_t srcStride = static_cast<uint32_t>(sbw) * 64u * srcBpp;
        const uint32_t dstStride = static_cast<uint32_t>(dbw) * 64u * dstBpp;
        const uint32_t copyBpp = (srcBpp < dstBpp) ? srcBpp : dstBpp;

        uint8_t pixelBytes[4] = {};
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t srcY = transferCoord(ssay, rrh, row, traversal.reverseY);
            const uint32_t dstY = transferCoord(dsay, rrh, row, traversal.reverseY);
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t srcX = transferCoord(ssax, rrw, col, traversal.reverseX);
                const uint32_t dstX = transferCoord(dsax, rrw, col, traversal.reverseX);
                const uint32_t srcOff = srcBase + srcY * srcStride + srcX * srcBpp;
                const uint32_t dstOff = dstBase + dstY * dstStride + dstX * dstBpp;
                if (srcOff + copyBpp > m_vramSize || dstOff + copyBpp > m_vramSize)
                {
                    continue;
                }

                std::memcpy(pixelBytes, m_vram + srcOff, copyBpp);
                std::memcpy(m_vram + dstOff, pixelBytes, copyBpp);
            }
        }
    }

    if (s_g285StatOn)
    {
        const uint64_t calls =
            g_g285L2lCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (g285Eligible)
        {
            const uint64_t bodyNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g285BodyT0).count());
            g_g285L2lEligible.fetch_add(1u, std::memory_order_relaxed);
            g_g285L2lBytes.fetch_add(g285Bytes, std::memory_order_relaxed);
            if (g285UsePageCopy)
            {
                g_g285L2lFast.fetch_add(1u, std::memory_order_relaxed);
                g_g285L2lFastNs.fetch_add(bodyNs, std::memory_order_relaxed);
            }
            else
            {
                g_g285L2lLegacyNs.fetch_add(bodyNs, std::memory_order_relaxed);
            }
        }
        if ((calls & 127u) == 0u)
        {
            std::fprintf(stderr,
                         "[G285:l2l] calls=%llu eligible=%llu fast=%llu bytes=%llu "
                         "legacyMs=%.3f fastMs=%.3f\n",
                         static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(
                             g_g285L2lEligible.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_g285L2lFast.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_g285L2lBytes.load(std::memory_order_relaxed)),
                         static_cast<double>(
                             g_g285L2lLegacyNs.load(std::memory_order_relaxed)) / 1.0e6,
                         static_cast<double>(
                             g_g285L2lFastNs.load(std::memory_order_relaxed)) / 1.0e6);
        }
    }

    if (sbp == 0u && (dbp == 0u || dbp == 0x20u) && rrw >= 640u && rrh >= 512u)
    {
        m_lastDisplayBaseBytes = (dbp == 0x20u) ? 8192u : 0u;
        snapshotVRAM();
    }
}

void GS::vertexKick(bool drawing)
{
    // G196: raw per-kick ADC/drawing trace for tbp=0x2a20 (the G195 foreground-sheet suspect),
    // scoped identically to the existing G195:fgtri draw-time trace, so the two can be diffed
    // directly against the real map_0.gs freeze-dump's vertex/ADC stream. Default-off.
    // G268: this ran getenv() on EVERY vertex kick (~2.4us/vertex, ~66ms/frame on MAP-0).
    static const bool s_g196AdcTrace = envFlagEnabled("DC2_G196_ADC_TRACE");
    if (g268LiveEnvRead() ? envFlagEnabled("DC2_G196_ADC_TRACE") : s_g196AdcTrace)
    {
        const GSContext &ctx196 = m_ctx[m_prim.ctxt & 1u];
        if (m_prim.tme && ctx196.tex0.tbp0 == 0x2a20u &&
            (ctx196.frame.fbp == 0u || ctx196.frame.fbp == 0x68u))
        {
            static std::atomic<uint32_t> s_g196{0};
            const uint32_t n = s_g196.fetch_add(1u, std::memory_order_relaxed);
            if (n < 512u)
            {
                // G200: also log the freshly-written vertex position so the runner's output
                // coords can be compared against the real map_0.gs freeze's XYZ2 stream for the
                // same tbp (positions-diverge vs pure-gate-diverge discriminator).
                const GSVertex &v196 = m_vtxQueue[m_vtxCount % kMaxVerts];
                std::fprintf(stderr,
                    "[G196:kick] n=%u prim=%u drawing=%u vtxCount=%u fbp=0x%x x=%.3f y=%.3f z=%.1f q=%g\n",
                    n, static_cast<uint32_t>(m_prim.type),
                    static_cast<uint32_t>(drawing ? 1u : 0u), m_vtxCount, ctx196.frame.fbp,
                    v196.x, v196.y, v196.z, v196.q);
            }
        }
    }

    // G107 A/B: ADC is "drawing kick disabled". For title-rock strips, test the stricter
    // interpretation that an ADC vertex must not advance the primitive queue either.
    if (!drawing)
    {
        extern std::atomic<bool> g_dc2TitleRockScope;
        static const bool s_g107NoAdvance = envFlagEnabled("DC2_G107_ADC_NO_ADVANCE");
        if (s_g107NoAdvance &&
            g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
            m_prim.type == GS_PRIM_TRISTRIP)
        {
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (m_prim.tme && ctx.frame.fbp != 0x139u && g105TitleRockTbp(ctx.tex0.tbp0))
            {
                static const bool s_stat = envFlagEnabled("DC2_G107_ADC_STAT");
                if (s_stat)
                {
                    static std::atomic<uint64_t> s_skip{0};
                    const uint64_t n = s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 32u || (n & 0x3FFull) == 0ull)
                        std::fprintf(stderr,
                                     "[G107:adcskip] n=%llu prim=%u tbp=0x%x vtxCount=%u\n",
                                     static_cast<unsigned long long>(n),
                                     static_cast<uint32_t>(m_prim.type),
                                     ctx.tex0.tbp0, m_vtxCount);
                }
                return;
            }
        }
    }

    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    // [G123] ordered interleave: accumulate this tri-class kick into the current draw-run keyed
    // by the bound TEX0; a tbp change flushes the run. Pairs with the BIND events above.
    if (g123SeqEnabled())
    {
        const uint32_t tt123 = (uint32_t)m_prim.type & 7u;
        const bool triClass123 = (tt123 == GS_PRIM_TRIANGLE || tt123 == GS_PRIM_TRISTRIP ||
                                  tt123 == GS_PRIM_TRIFAN);
        extern std::atomic<bool> g_dc2TitleRockScope;
        if (triClass123 && m_prim.tme &&
            g_dc2TitleRockScope.load(std::memory_order_relaxed))
        {
            const GSContext &ctx123 = m_ctx[m_prim.ctxt & 1u];
            if (g105TitleRockTbp(ctx123.tex0.tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                if (ctx123.tex0.tbp0 != s_g123RunTbp || tt123 != s_g123RunPrim)
                    g123FlushRun();
                s_g123RunTbp = ctx123.tex0.tbp0;
                s_g123RunPrim = tt123;
                if (drawing)
                    ++s_g123RunDraw;
                else
                    ++s_g123RunNodraw;
            }
        }
    }

    if (m_vtxCount < needed)
        return;

    // G65: tally EVERY vertexKick by prim type AND by drawing flag, so we can tell whether
    // the title's tristrips reach the GS at all (and with drawing=true) vs are lost upstream.
    if (g14TraceEnabled())
    {
        static std::atomic<uint64_t> s_all[8]{}, s_nodraw[8]{};
        const uint32_t tt = (uint32_t)m_prim.type & 7u;
        s_all[tt].fetch_add(1u, std::memory_order_relaxed);
        if (!drawing) s_nodraw[tt].fetch_add(1u, std::memory_order_relaxed);

        // G66: per-tbp draw/nodraw tally for tri-class prims. Distinguishes "rock geometry
        // (tbp=0x3960) never emitted" (H1) vs "emitted but ADC=1 / texture missing / occluded"
        // (H2). HW new_game.gs: rock = tbp=0x3960 PSMT8, 608 tstrip+184 tfan+128 tri / frame.
        {
            static std::mutex s_g66Mtx;
            struct TbpRec { uint32_t tbp; uint32_t tme; uint64_t draw; uint64_t nodraw; };
            static TbpRec s_g66[64]{};
            static uint32_t s_g66N = 0;
            const bool triClass = (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP || tt == GS_PRIM_TRIFAN);
            if (triClass)
            {
                const uint32_t tbp = m_prim.tme ? m_ctx[m_prim.ctxt & 1u].tex0.tbp0 : 0xFFFFFFFFu;
                std::lock_guard<std::mutex> lk(s_g66Mtx);
                uint32_t idx = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < s_g66N; ++i)
                    if (s_g66[i].tbp == tbp) { idx = i; break; }
                if (idx == 0xFFFFFFFFu && s_g66N < 64u) { idx = s_g66N++; s_g66[idx].tbp = tbp; s_g66[idx].tme = m_prim.tme; }
                if (idx != 0xFFFFFFFFu) { if (drawing) s_g66[idx].draw++; else s_g66[idx].nodraw++; }
                static uint64_t s_g66Tick = 0;
                if ((++s_g66Tick % 20000ull) == 0ull)
                {
                    for (uint32_t i = 0; i < s_g66N; ++i)
                        std::fprintf(stderr, "[G66:tbp] tbp=0x%x tme=%u draw=%llu nodraw=%llu\n",
                            s_g66[i].tbp, s_g66[i].tme,
                            (unsigned long long)s_g66[i].draw, (unsigned long long)s_g66[i].nodraw);
                }
            }
        }
        static std::atomic<uint64_t> s_n{0};
        const uint64_t vkn = s_n.fetch_add(1u, std::memory_order_relaxed);
        // G65: steady-state (late) sample of triangle-class coords + tme, skipping the early
        // boot/intro geometry that contaminates the capped per-sample traces. Window at high
        // vkn so we see the actual title menu rock geometry, not boot frames.
        if (vkn >= 80000u && vkn < 80360u &&
            (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP) && m_vtxCount >= 3u)
        {
            const GSVertex& a = m_vtxQueue[(m_vtxCount-3) % kMaxVerts];
            const GSVertex& b = m_vtxQueue[(m_vtxCount-2) % kMaxVerts];
            const GSVertex& c = m_vtxQueue[(m_vtxCount-1) % kMaxVerts];
            std::fprintf(stderr, "[G65:late] t=%u draw=%u tme=%u tbp=0x%x v0=(%.0f,%.0f,%.0f) v1=(%.0f,%.0f,%.0f) v2=(%.0f,%.0f,%.0f)\n",
                tt, (unsigned)(drawing?1u:0u), (unsigned)m_prim.tme, m_ctx[m_prim.ctxt&1u].tex0.tbp0,
                a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z);
        }
        if ((vkn % 20000u) == 0u)
            std::fprintf(stderr, "[G65:vk] ALL t0..7= %llu %llu %llu %llu %llu %llu %llu %llu | NODRAW tri/tstrip/tfan= %llu %llu %llu\n",
                (unsigned long long)s_all[0].load(),(unsigned long long)s_all[1].load(),(unsigned long long)s_all[2].load(),
                (unsigned long long)s_all[3].load(),(unsigned long long)s_all[4].load(),(unsigned long long)s_all[5].load(),
                (unsigned long long)s_all[6].load(),(unsigned long long)s_all[7].load(),
                (unsigned long long)s_nodraw[3].load(),(unsigned long long)s_nodraw[4].load(),(unsigned long long)s_nodraw[5].load());
    }

    if (drawing && g14TraceEnabled())
    {
        static std::atomic<uint32_t> s_pt[8]{};
        const uint32_t t = (uint32_t)m_prim.type & 7u;
        const uint32_t c = s_pt[t].fetch_add(1u, std::memory_order_relaxed);
        const bool tri = (t == GS_PRIM_TRIANGLE || t == GS_PRIM_TRISTRIP || t == GS_PRIM_TRIFAN);
        if (tri && c < 40u)
        {
            std::fprintf(stderr, "[G14:tri] type=%u n=%u tme=%u v0=(%.1f,%.1f,%.1f) v1=(%.1f,%.1f,%.1f) v2=(%.1f,%.1f,%.1f)\n",
                         t, c, (uint32_t)m_prim.tme,
                         m_vtxQueue[0].x, m_vtxQueue[0].y, m_vtxQueue[0].z,
                         m_vtxQueue[1].x, m_vtxQueue[1].y, m_vtxQueue[1].z,
                         m_vtxQueue[2].x, m_vtxQueue[2].y, m_vtxQueue[2].z);
            std::fflush(stderr);
        }
        if (((c + 1u) % 5000u) == 0u || (tri && c == 0u))
        {
            std::fprintf(stderr, "[G14:primsum] point=%u line=%u lstrip=%u tri=%u tstrip=%u tfan=%u sprite=%u\n",
                         s_pt[GS_PRIM_POINT].load(), s_pt[GS_PRIM_LINE].load(), s_pt[GS_PRIM_LINESTRIP].load(),
                         s_pt[GS_PRIM_TRIANGLE].load(), s_pt[GS_PRIM_TRISTRIP].load(), s_pt[GS_PRIM_TRIFAN].load(),
                         s_pt[GS_PRIM_SPRITE].load());
            std::fflush(stderr);
        }
    }

    if (drawing)
    {
        G147CountedPerfScope g147DrawScope(g_g147DrawPrimitiveNs, g_g147DrawPrimitiveCount);
        m_rasterizer.drawPrimitive(this);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    G146PerfScope g146Scope(g_g146GsImageNs);
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on)
        {
            static thread_local char s_g219tag[64];
            std::snprintf(s_g219tag, sizeof(s_g219tag), "upload(dbp=0x%x dpsm=0x%x)",
                          m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dpsm);
            g219Sentinel(m_vram, m_vramSize, s_g219tag);
        }
    }

    static const bool s_g145EnvDirtyUpload = envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                             !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD");
    const bool g145DirtyUpload = (g268LiveEnvRead()
                                      ? (envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                         !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD"))
                                      : s_g145EnvDirtyUpload) ||
                                 g260NrEnabled();
    if (!g145DirtyUpload)
        g144FlushPendingUpload();

    // G145 opt-in: the deferred-triangle upload barrier is range-checked after host-to-local is confirmed.
    if (renderQualityTraceEnabled())
    {
        static std::atomic<int> s_f37PidEntry{0};
        const int n = s_f37PidEntry.fetch_add(1, std::memory_order_relaxed);
        const bool isT4HH = (m_bitbltbuf.dpsm == GS_PSM_T4HH || m_bitbltbuf.dpsm == GS_PSM_T4HL);
        // F50.9: also surface the dungeon paletted-texture/CLUT uploads (PSMT4=0x14,
        // PSMT8=0x13, and PSMCT32 CLUTs with dbw==1) which arrive after the first 24
        // (all-T4HH/title) calls, so we can confirm processImageData actually runs for them.
        const bool isDungeonShaped = (m_bitbltbuf.dpsm == GS_PSM_T4 ||
                                      m_bitbltbuf.dpsm == GS_PSM_T8 ||
                                      (m_bitbltbuf.dpsm == GS_PSM_CT32 && m_bitbltbuf.dbw == 1u));
        static std::atomic<int> s_f37PidDng{0};
        const int dng = isDungeonShaped ? s_f37PidDng.fetch_add(1, std::memory_order_relaxed) : 0;
        if (n < 24 || isT4HH || (isDungeonShaped && dng < 48))
        {
            uint32_t nz = 0;
            const uint32_t cap = std::min<uint32_t>(sizeBytes, 256u);
            for (uint32_t i = 0; i < cap; ++i) if (data[i]) ++nz;
            std::fprintf(stderr, "[F37:pid] n=%d trxdir=%u dpsm=0x%x dbp=0x%x dbw=%u rr=(%u,%u) bytes=%u nz=%u pending=%u\n",
                n, m_trxdir, (unsigned)m_bitbltbuf.dpsm, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw,
                m_trxreg.rrw, m_trxreg.rrh, sizeBytes, nz, s_pendingImageBytes);
        }
    }
    if (m_trxdir != 0 || !m_vram)
        return;

    // G145: flush only if this upload overlaps a pending deferred draw's texture/CLUT/target.
    if (g145DirtyUpload)
    {
        g144FlushPendingUploadRange(m_bitbltbuf.dbp,
                                    m_bitbltbuf.dbw,
                                    m_bitbltbuf.dpsm,
                                    m_trxpos.dsax,
                                    m_trxpos.dsay,
                                    m_trxreg.rrw,
                                    m_trxreg.rrh);
    }

    g149BumpVramGen(); // host-to-local upload about to write VRAM: invalidate the texture cache
    // G178/G242: notify only after the pending-draw barrier above has established GS order.
    {
        extern void g178NoteVramWriteRect(uint8_t *, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t, uint32_t);
        g178NoteVramWriteRect(m_vram, m_bitbltbuf.dbp, m_bitbltbuf.dbw, m_bitbltbuf.dpsm,
                              m_trxpos.dsax, m_trxpos.dsay, m_trxreg.rrw, m_trxreg.rrh);
    }

    // [G37:seq] GS-order trace of writes to the 0x2720 work page (uploads), shared with the
    // rasterizer's RTT-draw + composite events, to pin what clobbers the model RTT before the
    // display composite samples it. Gated by DC2_G37_SEQ.
    if (g_g37_seq_trace && g_g37_armed.load(std::memory_order_relaxed) == 1 &&
        m_bitbltbuf.dbp >= 0x2700u && m_bitbltbuf.dbp <= 0x2740u)
    {
        const uint32_t s = g_g37_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (s <= 120u)
        {
            std::fprintf(stderr, "[G37:seq] s=%u UPLOAD dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) bytes=%u\n",
                         s, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm,
                         m_trxreg.rrw, m_trxreg.rrh, sizeBytes);
            std::fflush(stderr);
        }
    }

    uint32_t dbp = m_bitbltbuf.dbp;
    uint8_t dbw = m_bitbltbuf.dbw;
    uint8_t dpsm = m_bitbltbuf.dpsm;

    // G2: capture the raw host->local source bytes for the font/banner pages so the
    // CT32-alias upload + T8 read swizzle can be solved offline. DC2_DUMP_FONTSRC=1.
    {
        static const bool s_dumpSrc = []() {
            const char *e = std::getenv("DC2_DUMP_FONTSRC");
            return e && e[0] != '0';
        }();
        if (s_dumpSrc && (dbp == 0x2720u || dbp == 0x2f20u || dbp == 0x3220u) && sizeBytes != 0u)
        {
            static std::atomic<int> s_n2720{0}, s_n2f20{0}, s_n3220{0};
            std::atomic<int> *cnt = (dbp == 0x2720u) ? &s_n2720 : (dbp == 0x2f20u) ? &s_n2f20 : &s_n3220;
            int idx = cnt->fetch_add(1, std::memory_order_relaxed);
            if (idx < 8)
            {
                char p[128];
                std::snprintf(p, sizeof(p), "captures/src_%x_%d_dbw%u_rr%ux%u_ds%ux%u.bin",
                              dbp, idx, (unsigned)dbw, m_trxreg.rrw, m_trxreg.rrh, m_trxpos.dsax, m_trxpos.dsay);
                FILE *fp = std::fopen(p, "wb");
                if (fp) { std::fwrite(data, 1, sizeBytes, fp); std::fclose(fp); }
                std::fprintf(stderr, "[G2:srcdump] %s bytes=%u dpsm=0x%x\n", p, sizeBytes, (unsigned)dpsm);
            }
        }
    }

    if (dbw == 0)
        dbw = 1;
    uint32_t base = dbp * 256u;
    uint32_t bpp = bitsPerPixel(dpsm);
    uint32_t stridePixels = static_cast<uint32_t>(dbw) * 64u;

    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t dsax = m_trxpos.dsax;
    uint32_t dsay = m_trxpos.dsay;

    // G155 (perf, bit-exact): the upload-preview candidate below (this full-buffer nonzero scan
    // plus, for CT* uploads, a 512x448x4 RGBA assign+copy) is consumed ONLY by the default-off host
    // upload-preview fallback (uploadPresentationFallbackEnabled -> the [F31:upload-present] latch)
    // and by DC2_PHASE_TRACE traces. It never touches VRAM. In the normal render path it is pure
    // dead work — the title streams ~1.9 MB of host->local image data/frame, so this scan alone is a
    // ~1.9M byte compares/frame tax on the (MTGS worker) upload budget for no visible effect. Skip
    // the scan unless a consumer is active; nonzeroBytes then stays 0 so the candidate block below is
    // naturally skipped (bit-exact). Kill switch DC2_G155_NO_SKIP_PREVIEW forces the old scan.
    uint32_t nonzeroBytes = 0u;
    static const bool s_g155PreviewNeeded =
        uploadPresentationFallbackEnabled() || phaseDiagnosticsEnabled() ||
        envFlagEnabled("DC2_G155_NO_SKIP_PREVIEW");
    if (s_g155PreviewNeeded)
    {
        for (uint32_t i = 0; i < sizeBytes; ++i)
        {
            if (data[i] != 0u)
                ++nonzeroBytes;
        }
    }

    if (nonzeroBytes != 0u && isHostPresentableUploadPsm(dpsm) && rrw != 0u && rrh != 0u)
    {
        const uint32_t candidateWidth = std::min<uint32_t>(rrw, kHostFrameWidth);
        const uint32_t candidateHeight = std::min<uint32_t>(rrh, kHostFrameHeight);
        const uint64_t candidateScore =
            static_cast<uint64_t>(candidateWidth) *
            static_cast<uint64_t>(candidateHeight) *
            static_cast<uint64_t>(nonzeroBytes);
        if (!s_f31UploadPresentationCandidate.valid ||
            candidateScore > s_f31UploadPresentationCandidate.score)
        {
            s_f31UploadPresentationCandidate.valid = true;
            s_f31UploadPresentationCandidate.frame.fbp = dbp;
            s_f31UploadPresentationCandidate.frame.fbw = dbw;
            s_f31UploadPresentationCandidate.frame.psm = dpsm;
            s_f31UploadPresentationCandidate.frame.fbmsk = 0u;
            s_f31UploadPresentationCandidate.width = candidateWidth;
            s_f31UploadPresentationCandidate.height = candidateHeight;
            s_f31UploadPresentationCandidate.sourceOriginX = dsax;
            s_f31UploadPresentationCandidate.sourceOriginY = dsay;
            s_f31UploadPresentationCandidate.nonzeroBytes = nonzeroBytes;
            s_f31UploadPresentationCandidate.score = candidateScore;
            s_f31UploadPresentationCandidate.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);

            if (dpsm == GS_PSM_CT32)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 4u;
                        if (srcOff + 4u > sizeBytes)
                            break;
                        dstRow[x * 4u + 0u] = data[srcOff + 0u];
                        dstRow[x * 4u + 1u] = data[srcOff + 1u];
                        dstRow[x * 4u + 2u] = data[srcOff + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }
            else if (dpsm == GS_PSM_CT24)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 3u;
                        if (srcOff + 3u > sizeBytes)
                            break;
                        dstRow[x * 4u + 0u] = data[srcOff + 0u];
                        dstRow[x * 4u + 1u] = data[srcOff + 1u];
                        dstRow[x * 4u + 2u] = data[srcOff + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }
            else if (dpsm == GS_PSM_CT16 || dpsm == GS_PSM_CT16S)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 2u;
                        if (srcOff + sizeof(uint16_t) > sizeBytes)
                            break;
                        uint16_t pixel = 0u;
                        std::memcpy(&pixel, data + srcOff, sizeof(pixel));
                        const uint32_t r = pixel & 31u;
                        const uint32_t g = (pixel >> 5) & 31u;
                        const uint32_t b = (pixel >> 10) & 31u;
                        dstRow[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                        dstRow[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                        dstRow[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }

            if (phaseDiagnosticsEnabled())
            {
                static std::atomic<uint32_t> s_f31UploadCandidateCount{0};
                const uint32_t n = s_f31UploadCandidateCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u || (n % 64u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[F31:upload-candidate] n=%u dbp=0x%x dbw=%u psm=0x%x size=(%u,%u) ds=(%u,%u) nonzero=%u score=%llu\n",
                                 n,
                                 dbp,
                                 static_cast<uint32_t>(dbw),
                                 static_cast<uint32_t>(dpsm),
                                 candidateWidth,
                                 candidateHeight,
                                 dsax,
                                 dsay,
                                 nonzeroBytes,
                                 static_cast<unsigned long long>(candidateScore));
                }
            }
        }
    }

    if (phaseDiagnosticsEnabled() && sizeBytes != 0u)
    {
        static std::atomic<uint32_t> s_f31ImageWriteCount{0};
        const uint32_t n = s_f31ImageWriteCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:imgwrite] n=%u bytes=%u nonzero=%u dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) hw=(%u,%u)\n",
                         n, sizeBytes, nonzeroBytes, dbp, static_cast<uint32_t>(dbw),
                         static_cast<uint32_t>(dpsm), rrw, rrh, dsax, dsay,
                         m_hwregX, m_hwregY);
        }
    }

    if (dpsm == GS_PSM_T4HL || dpsm == GS_PSM_T4HH)
    {
        if (dpsm == GS_PSM_T4HH && dbp == 0x1A00u && envFlagEnabled("DC2_DUMP_FONT"))
        {
            static uint32_t s_g5SourceBytes = 0u;
            const uint32_t sourceBytes = (rrw * rrh + 1u) / 2u;
            if (s_g5SourceBytes < sourceBytes)
            {
                const uint32_t bytesToWrite = std::min<uint32_t>(sizeBytes, sourceBytes - s_g5SourceBytes);
                FILE *fp = std::fopen("captures/font4hh_source.bin",
                                      (s_g5SourceBytes == 0u) ? "wb" : "ab");
                if (fp)
                {
                    std::fwrite(data, 1, bytesToWrite, fp);
                    std::fclose(fp);
                }
                s_g5SourceBytes += bytesToWrite;
                std::fprintf(stderr, "[G5:font4hh-src] wrote=%u total=%u expected=%u\n",
                             bytesToWrite, s_g5SourceBytes, sourceBytes);
            }
        }

        if (renderQualityTraceEnabled())
        {
            static std::atomic<int> s_f37T4hhCount{0};
            const int n = s_f37T4hhCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 16)
            {
                uint32_t nz = 0;
                for (uint32_t i = 0; i < sizeBytes; ++i) if (data[i]) ++nz;
                std::fprintf(stderr, "[F37:t4hh-write] call=%d dpsm=0x%x dbp=0x%x dbw=%u bytes=%u nonzero=%u hwx=%u hwy=%u rrw=%u rrh=%u\n",
                    n, static_cast<uint32_t>(dpsm), dbp, static_cast<uint32_t>(dbw),
                    sizeBytes, nz, m_hwregX, m_hwregY, rrw, rrh);
            }
        }
        // Host-to-local transfers use the format's transfer bpp. T4HL/T4HH are packed
        // 4-bit streams even though their destination nibble aliases a CT32 word.
        auto writeT4Nibble = [&](uint8_t nibble)
        {
            if (m_hwregY >= rrh)
                return;

            if (dpsm == GS_PSM_T4HH)
            {
                GSMem::WriteP4HH(m_vram, dbp, dbw, dsax + m_hwregX, dsay + m_hwregY,
                                 static_cast<uint32_t>(nibble & 0x0Fu));
            }
            else
            {
                GSMem::WriteP4HL(m_vram, dbp, dbw, dsax + m_hwregX, dsay + m_hwregY,
                                 static_cast<uint32_t>(nibble & 0x0Fu));
            }

            ++m_hwregX;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        };

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            const uint8_t srcByte = data[offset++];
            const uint32_t xBefore = m_hwregX;

            writeT4Nibble(static_cast<uint8_t>(srcByte & 0x0Fu));
            if ((xBefore + 1u) < rrw && m_hwregY < rrh)
            {
                writeT4Nibble(static_cast<uint8_t>((srcByte >> 4u) & 0x0Fu));
            }
        }
    }
    else if (bpp == 4)
    {
        // F62 (PR #132): PSMT4 upload via the GSMem swizzle module. GSMem takes the
        // buffer width in PAGES; PSMT4 pages are 128 wide, so pass dbw>>1 (the same
        // halving the local GSPSMT4::addrPSMT4 did internally) — byte-identical.
        const uint32_t pageBwT4 = ((static_cast<uint32_t>(dbw) >> 1u) != 0u)
                                      ? (static_cast<uint32_t>(dbw) >> 1u)
                                      : 1u;
        auto writeT4Nibble = [&](uint8_t nibble)
        {
            if (m_hwregY >= rrh)
                return;
            GSMem::WriteP4(m_vram, dbp, pageBwT4, dsax + m_hwregX, dsay + m_hwregY,
                           static_cast<uint32_t>(nibble & 0x0Fu));
            ++m_hwregX;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        };

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            const uint8_t srcByte = data[offset++];
            const uint32_t srcLo = srcByte & 0x0Fu;
            const uint32_t srcHi = (srcByte >> 4) & 0x0Fu;
            const uint32_t xBefore = m_hwregX;

            writeT4Nibble(static_cast<uint8_t>(srcLo));
            if ((xBefore + 1u) < rrw && m_hwregY < rrh)
            {
                writeT4Nibble(static_cast<uint8_t>(srcHi));
            }
        }
    }
    else if (dpsm == GS_PSM_T8)
    {
        // F62 (PR #132): PSMT8 upload via GSMem. PSMT8 pages are 128 wide → dbw>>1
        // (same halving GSPSMT8::addrPSMT8 did internally) — byte-identical.
        const uint32_t pageBwT8 = ((static_cast<uint32_t>(dbw) >> 1u) != 0u)
                                      ? (static_cast<uint32_t>(dbw) >> 1u)
                                      : 1u;
        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t pixelsToCopy = std::min<uint32_t>(pixelsLeft, sizeBytes - offset);
            if (pixelsToCopy == 0)
            {
                break;
            }

            for (uint32_t i = 0; i < pixelsToCopy; ++i)
            {
                const uint32_t vx = dsax + m_hwregX + i;
                const uint32_t vy = dsay + m_hwregY;
                GSMem::WriteP8(m_vram, dbp, pageBwT8, vx, vy,
                               static_cast<uint32_t>(data[offset + i]));
            }

            offset += pixelsToCopy;
            m_hwregX += pixelsToCopy;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        }
    }
    else if (dpsm == GS_PSM_CT24 || dpsm == GS_PSM_Z24)
    {
        // F62 (PR #132): 24-bit upload via GSMem. CT24/Z24 share the 32-bit page
        // layout, so write through WriteCT32 (GSMem CT32 == local addrPSMCT32) and
        // keep the pre-F62 quirk of storing alpha=0x80 in the unused upper byte.
        const uint32_t pageBw24 = (static_cast<uint32_t>(dbw) != 0u)
                                      ? static_cast<uint32_t>(dbw)
                                      : 1u;
        uint32_t transferBpp = 3;

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t srcBytesLeft = pixelsLeft * transferBpp;
            uint32_t bytesAvail = sizeBytes - offset;
            uint32_t pixelsToCopy = pixelsLeft;
            if (srcBytesLeft > bytesAvail)
                pixelsToCopy = bytesAvail / transferBpp;

            if (pixelsToCopy == 0)
                break;

            for (uint32_t p = 0; p < pixelsToCopy; ++p)
            {
                const uint32_t vx = dsax + m_hwregX + p;
                const uint32_t vy = dsay + m_hwregY;
                const uint32_t c = static_cast<uint32_t>(data[offset + p * 3u + 0u]) |
                                   (static_cast<uint32_t>(data[offset + p * 3u + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + p * 3u + 2u]) << 16u) |
                                   (0x80u << 24u);
                GSMem::WriteCT32(m_vram, dbp, pageBw24, vx, vy, c);
            }

            offset += pixelsToCopy * transferBpp;
            m_hwregX += pixelsToCopy;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        }
    }
    else
    {
        // F62 (PR #132): remaining formats via GSMem. CT32/Z32 were already swizzled
        // (GSMem CT32/Z32 == local addrPSMCT32, byte-identical). The 16-bit family
        // (CT16/CT16S/Z16/Z16S) was previously written LINEARLY here while the sampler
        // reads it SWIZZLED — GSMem now swizzles the upload so the two agree. T8H stores
        // the index in the upper byte of a 32-bit word. Unknown psm keeps the old linear
        // copy as a safety fallback.
        const uint32_t pageBwOther = (static_cast<uint32_t>(dbw) != 0u)
                                         ? static_cast<uint32_t>(dbw)
                                         : 1u;
        uint32_t bytesPerPixel = bpp / 8u;
        if (bytesPerPixel == 0)
            bytesPerPixel = 4;
        uint32_t strideBytes = stridePixels * bytesPerPixel;

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t dstY = dsay + m_hwregY;
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t bytesLeft = pixelsLeft * bytesPerPixel;
            uint32_t bytesAvail = sizeBytes - offset;
            if (bytesLeft > bytesAvail)
                bytesLeft = (bytesAvail / bytesPerPixel) * bytesPerPixel;

            uint32_t pixelsCopied = bytesLeft / bytesPerPixel;

            for (uint32_t p = 0; p < pixelsCopied; ++p)
            {
                const uint32_t vx = dsax + m_hwregX + p;
                const uint32_t vy = dstY;
                const uint8_t *src = data + offset + p * bytesPerPixel;
                switch (dpsm)
                {
                case GS_PSM_CT32:
                {
                    uint32_t c; std::memcpy(&c, src, 4u);
                    GSMem::WriteCT32(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z32:
                {
                    uint32_t c; std::memcpy(&c, src, 4u);
                    GSMem::WriteZ32(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_CT16:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteCT16(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_CT16S:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteCT16S(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z16:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteZ16(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z16S:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteZ16S(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_T8H:
                    GSMem::WriteP8H(m_vram, dbp, pageBwOther, vx, vy,
                                    static_cast<uint32_t>(src[0]));
                    break;
                default:
                {
                    const uint32_t dstOff = base + vy * strideBytes +
                                            (dsax + m_hwregX + p) * bytesPerPixel;
                    if (dstOff + bytesPerPixel <= m_vramSize)
                        std::memcpy(m_vram + dstOff, src, bytesPerPixel);
                    break;
                }
                }
            }

            offset += bytesLeft;
            m_hwregX += pixelsCopied;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }

            if (bytesLeft == 0)
                break;
        }
    }

    // G289 commits its exact partial-owner overwrite only after the complete IMAGE transfer
    // has landed. Chunked IMAGE packets keep the owner conservative until the last chunk.
    if (m_hwregY >= rrh)
        g289NoteUploadComplete(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);

    if (t8UploadTraceEnabled() &&
        dpsm == GS_PSM_CT32 &&
        (dbp == 0x2720u || dbp == 0x3220u) &&
        dbw != 0u &&
        rrw != 0u &&
        rrh != 0u &&
        sizeBytes != 0u &&
        f51T8AliasShouldLog())
    {
        const uint32_t t8Tbw = static_cast<uint32_t>(dbw) * 2u;
        const uint32_t sampleW = std::min<uint32_t>(rrw * 2u, 256u);
        const uint32_t sampleH = std::min<uint32_t>(rrh * 2u, 256u);
        uint32_t nzT8 = 0u;
        uint32_t firstOff = 0u;
        uint32_t firstX = 0u;
        uint32_t firstY = 0u;
        uint8_t firstByte = 0u;
        bool haveFirst = false;

        for (uint32_t y = 0u; y < sampleH; ++y)
        {
            for (uint32_t x = 0u; x < sampleW; ++x)
            {
                const uint32_t off = GSPSMT8::addrPSMT8(dbp, t8Tbw, dsax * 2u + x, dsay * 2u + y);
                if (off < m_vramSize && m_vram[off] != 0u)
                {
                    ++nzT8;
                    if (!haveFirst)
                    {
                        firstOff = off;
                        firstX = x;
                        firstY = y;
                        firstByte = m_vram[off];
                        haveFirst = true;
                    }
                }
            }
        }

        std::fprintf(stderr,
                     "[F51:t8alias] dbp=0x%x ct32dbw=%u t8tbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) bytes=%u srcNz=%u t8Sample=%ux%u nzT8=%u first=(%u,%u off=0x%x byte=0x%02x)\n",
                     dbp, static_cast<uint32_t>(dbw), t8Tbw, static_cast<uint32_t>(dpsm),
                     rrw, rrh, dsax, dsay, sizeBytes, nonzeroBytes, sampleW, sampleH,
                     nzT8, firstX, firstY, firstOff, firstByte);
    }

    // G3: raw VRAM page-region snapshot immediately after this upload completes.
    // Pairs with the at-sample snapshot in the rasterizer to detect a later overwrite
    // of the font page (0x2720 == RTT fbp=0x139). DC2_DUMP_VRAMRGN=1.
    {
        static const bool s_dumpRgn = []() {
            const char *e = std::getenv("DC2_DUMP_VRAMRGN");
            return e && e[0] != '0';
        }();
        if (s_dumpRgn && (dbp == 0x2720u || dbp == 0x2f20u || dbp == 0x3220u) && sizeBytes != 0u)
        {
            static std::atomic<int> s_rc2720{0}, s_rc2f20{0}, s_rc3220{0};
            std::atomic<int> *rc = (dbp == 0x2720u) ? &s_rc2720 : (dbp == 0x2f20u) ? &s_rc2f20 : &s_rc3220;
            const int idx = rc->fetch_add(1, std::memory_order_relaxed);
            if (idx < 6)
            {
                const uint32_t rbase = dbp * 256u;
                const uint32_t rlen = 0x40000u;
                if (rbase + rlen <= m_vramSize)
                {
                    char p[128];
                    std::snprintf(p, sizeof(p), "captures/vram_postup_%x_%d.bin", dbp, idx);
                    FILE *fp = std::fopen(p, "wb");
                    if (fp) { std::fwrite(m_vram + rbase, 1, rlen, fp); std::fclose(fp); }
                    std::fprintf(stderr, "[G3:postup] %s rr=(%u,%u)\n", p, rrw, rrh);
                }
            }
        }
    }

    // F50.10: immediately read VRAM back through the SAME addressing the sampler uses,
    // to prove whether the write landed (independent of any later overwrite/timing).
    if (renderQualityTraceEnabled() && rrw != 0u && rrh != 0u)
    {
        const bool isPT4 = (dpsm == GS_PSM_T4);
        const bool isCT32clut = (dpsm == GS_PSM_CT32 && dbw == 1u);
        if (isPT4 || isCT32clut)
        {
            static std::atomic<int> s_f510rb{0};
            const int rb = s_f510rb.fetch_add(1, std::memory_order_relaxed);
            if (rb < 40)
            {
                const uint32_t sampW = std::min<uint32_t>(rrw, 64u);
                const uint32_t sampH = std::min<uint32_t>(rrh, 64u);
                uint32_t nzBack = 0u;
                if (isPT4)
                {
                    for (uint32_t y = 0; y < sampH; ++y)
                        for (uint32_t x = 0; x < sampW; ++x)
                        {
                            const uint32_t na = GSPSMT4::addrPSMT4(dbp, (dbw != 0 ? dbw : 1u), dsax + x, dsay + y);
                            const uint32_t bo = na >> 1;
                            if (bo < m_vramSize && m_vram[bo] != 0u) ++nzBack;
                        }
                }
                else
                {
                    for (uint32_t y = 0; y < sampH; ++y)
                        for (uint32_t x = 0; x < sampW; ++x)
                        {
                            const uint32_t off = GSPSMCT32::addrPSMCT32(dbp, dbw, dsax + x, dsay + y);
                            if (off + 4u <= m_vramSize)
                            {
                                uint32_t w; std::memcpy(&w, m_vram + off, 4);
                                if (w != 0u) ++nzBack;
                            }
                        }
                }
                std::fprintf(stderr,
                             "[F510:readback] rb=%d dpsm=0x%x dbp=0x%x dbw=%u ds=(%u,%u) rr=(%u,%u) sampled=%ux%u nzInVram=%u\n",
                             rb, (unsigned)dpsm, dbp, (unsigned)dbw, dsax, dsay, rrw, rrh, sampW, sampH, nzBack);
            }
        }
    }
}

void GS::performLocalToHostToBuffer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = m_bitbltbuf.sbw;
    uint8_t spsm = m_bitbltbuf.spsm;

    if (sbw == 0)
        sbw = 1;
    uint32_t base = sbp * 256u;
    uint32_t bpp = bitsPerPixel(spsm);
    uint32_t stridePixels = static_cast<uint32_t>(sbw) * 64u;

    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t ssax = m_trxpos.ssax;
    uint32_t ssay = m_trxpos.ssay;

    // G242: establish GS submission order first, then publish any persistent depth aliased by the
    // source.  Keep the historical default-off path byte-for-byte unchanged.
    if (g242GuestDepthEnabled())
        g144FlushPendingUpload();
    g242PrepareVramReadRect(m_vram, sbp, sbw, spsm, ssax, ssay, rrw, rrh);

    if (bpp == 4)
    {
        uint32_t rowBytes = (rrw + 1u) / 2u;
        if (rowBytes == 0)
            rowBytes = 1;
        m_localToHostBuffer.reserve(rowBytes * rrh);
        uint32_t widthBlocks = static_cast<uint32_t>(sbw);
        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                uint32_t vx = ssax + x;
                uint32_t vy = ssay + y;
                uint32_t nibbleAddr = GSPSMT4::addrPSMT4(sbp, widthBlocks, vx, vy);
                uint32_t byteOff = nibbleAddr >> 1;
                uint8_t nibble = 0;
                if (byteOff < m_vramSize)
                {
                    int shift = static_cast<int>((nibbleAddr & 1u) << 2);
                    nibble = static_cast<uint8_t>((m_vram[byteOff] >> shift) & 0x0Fu);
                }
                if (x & 1u)
                    m_localToHostBuffer.back() = static_cast<uint8_t>((m_localToHostBuffer.back() & 0x0Fu) | (nibble << 4));
                else
                    m_localToHostBuffer.push_back(nibble);
            }
        }
    }
    else if (spsm == GS_PSM_T8)
    {
        m_localToHostBuffer.reserve(rrw * rrh);
        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                const uint32_t src = GSPSMT8::addrPSMT8(sbp, sbw, ssax + x, ssay + y);
                m_localToHostBuffer.push_back((src < m_vramSize) ? m_vram[src] : 0u);
            }
        }
    }
    else if (spsm == GS_PSM_CT24 || spsm == GS_PSM_Z24)
    {
        uint32_t transferBpp = 3;
        m_localToHostBuffer.reserve(rrw * rrh * transferBpp);

        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                uint32_t srcOff = GSPSMCT32::addrPSMCT32(sbp, sbw, ssax + x, ssay + y);
                if (srcOff + 4 <= m_vramSize)
                {
                    m_localToHostBuffer.push_back(m_vram[srcOff + 0]);
                    m_localToHostBuffer.push_back(m_vram[srcOff + 1]);
                    m_localToHostBuffer.push_back(m_vram[srcOff + 2]);
                }
            }
        }
    }
    else
    {
        uint32_t bytesPerPixel = bpp / 8u;
        if (bytesPerPixel == 0)
            bytesPerPixel = 4;
        uint32_t strideBytes = stridePixels * bytesPerPixel;
        uint32_t rowBytes = rrw * bytesPerPixel;
        m_localToHostBuffer.reserve(rowBytes * rrh);

        for (uint32_t y = 0; y < rrh; ++y)
        {
            if (spsm == GS_PSM_CT32 || spsm == GS_PSM_Z32)
            {
                for (uint32_t x = 0; x < rrw; ++x)
                {
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(sbp, sbw, ssax + x, ssay + y);
                    if (srcOff + 4u <= m_vramSize)
                    {
                        m_localToHostBuffer.push_back(m_vram[srcOff + 0u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 1u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 2u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 3u]);
                    }
                }
            }
            else
            {
                uint32_t srcOff = base + (ssay + y) * strideBytes + ssax * bytesPerPixel;
                if (srcOff + rowBytes <= m_vramSize)
                {
                    for (uint32_t i = 0; i < rowBytes; ++i)
                        m_localToHostBuffer.push_back(m_vram[srcOff + i]);
                }
            }
        }
    }
}

bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(m_vram, m_vramSize, m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(m_vram, m_vramSize, activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!dst || maxBytes == 0)
        return 0;
    size_t avail = m_localToHostBuffer.size() - m_localToHostReadPos;
    if (avail == 0)
        return 0;
    size_t toCopy = (avail < maxBytes) ? avail : static_cast<size_t>(maxBytes);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, toCopy);
    m_localToHostReadPos += toCopy;
    return static_cast<uint32_t>(toCopy);
}
