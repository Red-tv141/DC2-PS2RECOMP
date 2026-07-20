void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    G147CountedPerfScope g147GifScope(g_g147GsGifPacketNs, g_g147GsGifPacketCount);
    const bool g147On = g147PerfEnabled();
    uint64_t g147Tags = 0ull;
    uint64_t g147PackedRegs = 0ull;
    uint64_t g147ReglistRegs = 0ull;
    uint64_t g147ImageBytesLocal = 0ull;
    auto g147FlushCounters = [&]() {
        if (!g147On)
            return;
        if (g147Tags)
            g_g147GifTags.fetch_add(g147Tags, std::memory_order_relaxed);
        if (g147PackedRegs)
            g_g147PackedRegs.fetch_add(g147PackedRegs, std::memory_order_relaxed);
        if (g147ReglistRegs)
            g_g147ReglistRegs.fetch_add(g147ReglistRegs, std::memory_order_relaxed);
        if (g147ImageBytesLocal)
            g_g147ImageBytes.fetch_add(g147ImageBytesLocal, std::memory_order_relaxed);
    };

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_vram)
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    if (s_pendingImageBytes == 0u && sizeBytes >= 16)
    {
        const uint64_t tagLo = loadLE64(data);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        if (flg == GIF_FMT_PACKED)
        {
            m_hwregX = 0;
            m_hwregY = 0;
        }
    }

    uint32_t offset = 0;
    if (s_pendingImageBytes != 0u)
    {
        if (looksLikePackedAdGifTag(data, sizeBytes))
        {
            if (t8UploadTraceEnabled() && f51PendingDropShouldLog())
            {
                uint64_t tagLo = 0u;
                uint64_t tagHi = 0u;
                std::memcpy(&tagLo, data, sizeof(tagLo));
                std::memcpy(&tagHi, data + 8u, sizeof(tagHi));
                std::fprintf(stderr,
                             "[F51:pendingdrop] pending=%u size=%u tagLo=0x%016llx tagHi=0x%016llx bitblt(dbp=0x%x dbw=%u dpsm=0x%x)\n",
                             s_pendingImageBytes, sizeBytes,
                             static_cast<unsigned long long>(tagLo),
                             static_cast<unsigned long long>(tagHi),
                             m_bitbltbuf.dbp,
                             static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
            }
            s_pendingImageBytes = 0u;
        }
    }

    if (s_pendingImageBytes != 0u)
    {
        const uint32_t imageBytes = std::min<uint32_t>(s_pendingImageBytes, sizeBytes);
        {
            static const bool g12up = envFlagEnabled("DC2_TRACE_G12_UPLOAD");
            const uint32_t db = m_bitbltbuf.dbp;
            if (g12up && (db == 0x2aa0u || (db >= 0x2700u && db <= 0x3400u)))
                std::fprintf(stderr, "[G12:cont] dbp=0x%x consume=%u remain=%u\n",
                             db, imageBytes, s_pendingImageBytes - imageBytes);
        }
        processImageData(data, imageBytes);
        g147ImageBytesLocal += imageBytes;
        s_pendingImageBytes -= imageBytes;
        offset = imageBytes;
        if (s_pendingImageBytes != 0u)
        {
            g147FlushCounters();
            return;
        }
    }

    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        ++g147Tags;

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegister(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            m_hwregX = 0;
            m_hwregY = 0;
            const bool g171On = g171PerfEnabled();
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    if (g171On)
                    {
                        const auto g171T0 = std::chrono::steady_clock::now();
                        writeRegisterPacked(regs[r], lo, hi);
                        const auto g171Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - g171T0).count();
                        const uint8_t g171Desc = regs[r] & 0xFu;
                        g_g171RegNs[g171Desc].fetch_add(static_cast<uint64_t>(g171Ns), std::memory_order_relaxed);
                        g_g171RegCount[g171Desc].fetch_add(1u, std::memory_order_relaxed);
                        if (g171Desc == 0x0Eu)
                        {
                            const uint8_t g171AdAddr = static_cast<uint8_t>(hi & 0xFFu);
                            g_g171AdNs[g171AdAddr].fetch_add(static_cast<uint64_t>(g171Ns), std::memory_order_relaxed);
                            g_g171AdCount[g171AdAddr].fetch_add(1u, std::memory_order_relaxed);
                            // G171 follow-up: identify WHAT is being drawn on the slow A+D XYZ2/XYZF2
                            // vertex-kick path (candidate: non-deferred 2D sprite raster, since G144
                            // only defers textured TRIS). Rate-limited trace on expensive calls only.
                            if ((g171AdAddr == 0x04u || g171AdAddr == 0x05u) && g171Ns > 5000)
                            {
                                static std::atomic<uint32_t> s_g171SlowCount{0};
                                const uint32_t sn = s_g171SlowCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (sn <= 40u || (sn % 200u) == 0u)
                                {
                                    const GSContext &sctx = m_ctx[m_prim.ctxt & 1u];
                                    const GSVertex &vA = m_vtxQueue[(m_vtxCount + kMaxVerts - 1) % kMaxVerts];
                                    const GSVertex &vB = m_vtxQueue[(m_vtxCount + kMaxVerts - 2) % kMaxVerts];
                                    std::fprintf(stderr,
                                                 "[G171:slowad] sn=%u addr=0x%02x ns=%lld prim=%u tme=%u abe=%u fbp=0x%x fbw=%u psm=0x%x scissor=(%d,%d)-(%d,%d) vtx=(%.1f,%.1f)-(%.1f,%.1f)\n",
                                                 sn, g171AdAddr, static_cast<long long>(g171Ns),
                                                 static_cast<uint32_t>(m_prim.type),
                                                 (unsigned)m_prim.tme, (unsigned)m_prim.abe,
                                                 sctx.frame.fbp, sctx.frame.fbw, static_cast<uint32_t>(sctx.frame.psm),
                                                 sctx.scissor.x0, sctx.scissor.y0, sctx.scissor.x1, sctx.scissor.y1,
                                                 vA.x, vA.y, vB.x, vB.y);
                                }
                            }
                        }
                    }
                    else
                    {
                        writeRegisterPacked(regs[r], lo, hi);
                    }
                    ++g147PackedRegs;
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    writeRegister(regs[r], loadLE64(data + offset));
                    offset += 8;
                    ++g147ReglistRegs;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            const uint32_t expectedImageBytes = nloop * 16u;
            uint32_t imageBytes = expectedImageBytes;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            if (phaseDiagnosticsEnabled())
            {
                static std::atomic<uint32_t> s_f31ImageCount{0};
                const uint32_t n = s_f31ImageCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u || (n % 128u) == 0u)
                {
                    uint32_t nonzeroBytes = 0u;
                    for (uint32_t i = 0; i < imageBytes; ++i)
                    {
                        if (data[offset + i] != 0u)
                            ++nonzeroBytes;
                    }
                    std::fprintf(stderr,
                                 "[F31:image] n=%u nloop=%u bytes=%u nonzero=%u trxdir=%u dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u)\n",
                                 n, nloop, imageBytes, nonzeroBytes, m_trxdir,
                                 m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                                 static_cast<uint32_t>(m_bitbltbuf.dpsm),
                                 static_cast<uint32_t>(m_trxreg.rrw),
                                 static_cast<uint32_t>(m_trxreg.rrh),
                                 static_cast<uint32_t>(m_trxpos.dsax),
                                 static_cast<uint32_t>(m_trxpos.dsay));
                }
            }
            // G12: uncapped marker for uploads into the costume common-menu-frame /
            // fukusel region (block 0x2700-0x3400). Confirms whether block 0x2aa0
            // (the row-bar atlas, slot[64], src=0xde7890 on HW) ever receives pixels.
            static const bool g12up = envFlagEnabled("DC2_TRACE_G12_UPLOAD");
            if (g12up)
            {
                const uint32_t db = m_bitbltbuf.dbp;
                if (db == 0x2aa0u || (db >= 0x2700u && db <= 0x3400u))
                {
                    uint32_t nz = 0u;
                    for (uint32_t i = 0; i < imageBytes; ++i) if (data[offset + i]) ++nz;
                    std::fprintf(stderr, "[G12:upload] dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) nloop=%u expect=%u bytes=%u nz=%u sizeLeft=%u\n",
                                 db, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm,
                                 (unsigned)m_trxreg.rrw, (unsigned)m_trxreg.rrh,
                                 (unsigned)m_trxpos.dsax, (unsigned)m_trxpos.dsay,
                                 nloop, expectedImageBytes, imageBytes, nz, sizeBytes - offset);
                }
            }
            // G66: tally distinct upload destination blocks so we can tell whether the rock
            // texture (HW tbp=0x3960 PSMT8) is ever uploaded to VRAM on the runner.
            if (g14TraceEnabled())
            {
                static std::mutex s_g66UpMtx;
                struct UpRec { uint32_t dbp; uint32_t dpsm; uint64_t n; uint64_t nz; };
                static UpRec s_up[64]{}; static uint32_t s_upN = 0;
                uint32_t nz = 0u; for (uint32_t i = 0; i < imageBytes; ++i) if (data[offset + i]) ++nz;
                std::lock_guard<std::mutex> lk(s_g66UpMtx);
                uint32_t idx = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < s_upN; ++i) if (s_up[i].dbp == m_bitbltbuf.dbp && s_up[i].dpsm == m_bitbltbuf.dpsm) { idx = i; break; }
                if (idx == 0xFFFFFFFFu && s_upN < 64u) { idx = s_upN++; s_up[idx].dbp = m_bitbltbuf.dbp; s_up[idx].dpsm = m_bitbltbuf.dpsm; }
                if (idx != 0xFFFFFFFFu) { s_up[idx].n++; s_up[idx].nz += nz; }
                static uint64_t s_upTick = 0;
                if ((++s_upTick % 4000ull) == 0ull)
                    for (uint32_t i = 0; i < s_upN; ++i)
                        std::fprintf(stderr, "[G66:up] dbp=0x%x dpsm=0x%x n=%llu nz=%llu\n",
                            s_up[i].dbp, s_up[i].dpsm, (unsigned long long)s_up[i].n, (unsigned long long)s_up[i].nz);
            }
            processImageData(data + offset, imageBytes);
            g147ImageBytesLocal += imageBytes;
            offset += imageBytes;
            if (imageBytes < expectedImageBytes)
            {
                s_pendingImageBytes = expectedImageBytes - imageBytes;
                g147FlushCounters();
                return;
            }
        }
    }
    g147FlushCounters();
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegister(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
    {
        // G52: the Select-Costume Max model (skinned, fbp=0x139 RTT) occasionally emits a
        // FULLY-ZERO RGBAQ for a skin vertex — a VU1/packet artifact. HW's red_vest.gs NEVER
        // emits a zero RGBAQ for this model (every model vertex has alpha=0x80, color>=0x50),
        // so a zero here is spurious; absent the bogus write the GS color register would simply
        // RETAIN the previous (valid) vertex color. Triangles interpolating from these zero
        // verts produced the long-deferred dark "face/hand speckle". Scope: only the costume
        // model context (fbp=0x139 textured tristrip). Kill switch DC2_G52_HOLDCOLOR=0.
        static const bool g52holdOff = envFlagEnabled("DC2_G52_NO_HOLDCOLOR");
        const bool g52ZeroRgbaq = (lo == 0u && hi == 0u);
        const bool g52ModelCtx = (m_prim.type == GS_PRIM_TRISTRIP &&
                                  m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u);
        if (!(!g52holdOff && g52ZeroRgbaq && g52ModelCtx))
        {
            m_curR = static_cast<uint8_t>(lo & 0xFF);
            m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
            m_curB = static_cast<uint8_t>(hi & 0xFF);
            m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        }
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && m_prim.type == GS_PRIM_TRISTRIP &&
                m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52c{0};
                const uint32_t n = s_g52c.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr, "[G52:rgba] n=%u col=(%u,%u,%u,%u) lo=0x%016llx hi=0x%016llx\n",
                                 n, (unsigned)m_curR, (unsigned)m_curG, (unsigned)m_curB, (unsigned)m_curA,
                                 static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
            }
        }
        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31PackedRgbaCount{0};
            const uint32_t n = s_f31PackedRgbaCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool rgbNz = (m_curR | m_curG | m_curB) != 0u;
            if (n <= 32u || rgbNz || (n % 1024u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:packed-rgbaq] n=%u rgba=%02x,%02x,%02x,%02x lo=0x%016llx hi=0x%016llx\n",
                             n, m_curR, m_curG, m_curB, m_curA,
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
        break;
    }
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0xFFFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0xFFFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        { static const bool s_fd = envFlagEnabled("DC2_G65_FORCEDRAW"); if (s_fd) adk = false; }
        bool g105Restart = false;
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (!adk && g105SynthAdcEnabled() &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                m_prim.type == GS_PRIM_TRISTRIP &&
                m_prim.tme &&
                ctx.frame.fbp != 0x139u &&
                g105TitleRockTbp(ctx.tex0.tbp0))
            {
                const float guard = g105AdcGuard();
                const float sx = static_cast<float>(x) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofx >> 4);
                const float sy = static_cast<float>(y) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofy >> 4);
                const bool outside =
                    sx < static_cast<float>(ctx.scissor.x0) - guard ||
                    sx > static_cast<float>(ctx.scissor.x1) + guard ||
                    sy < static_cast<float>(ctx.scissor.y0) - guard ||
                    sy > static_cast<float>(ctx.scissor.y1) + guard;
                if (outside)
                {
                    adk = true;
                    g105Restart = g105ResetStripEnabled();
                    static const bool s_stat = envFlagEnabled("DC2_G105_ADC_STAT");
                    if (s_stat)
                    {
                        static std::atomic<uint64_t> s_forced{0}, s_seen{0};
                        const uint64_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t fcnt = s_forced.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if ((n & 0x1FFFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G105:adc] seen=%llu forced=%llu guard=%.0f tbp=0x%x xy=(%.1f,%.1f)\n",
                                         static_cast<unsigned long long>(n),
                                         static_cast<unsigned long long>(fcnt),
                                         guard, ctx.tex0.tbp0, sx, sy);
                    }
                }
            }
        }
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && m_prim.type == GS_PRIM_TRISTRIP &&
                m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52{0};
                const uint32_t n = s_g52.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr,
                                 "[G52:seq] n=%u adc=%u xy=(%.1f,%.1f) col=(%u,%u,%u,%u) tbp=0x%x hi=0x%016llx\n",
                                 n, (unsigned)(adk ? 1u : 0u), x / 16.0f, y / 16.0f,
                                 (unsigned)m_curR, (unsigned)m_curG, (unsigned)m_curB, (unsigned)m_curA,
                                 m_ctx[m_prim.ctxt & 1u].tex0.tbp0,
                                 static_cast<unsigned long long>(hi));
            }
        }
        if (g31TraceEnabled() &&
            (m_prim.type == GS_PRIM_TRIANGLE ||
             m_prim.type == GS_PRIM_TRISTRIP ||
             m_prim.type == GS_PRIM_TRIFAN))
        {
            static std::atomic<uint32_t> s_g31PackedXyzf2Count{0};
            const uint32_t n = s_g31PackedXyzf2Count.fetch_add(1u, std::memory_order_relaxed);
            if (n < 160u)
            {
                std::fprintf(stderr,
                             "[G31:xyzf2] n=%u prim=%u slot=%u countBefore=%u adc=%u x=%u(%.1f) y=%u(%.1f) z=0x%x f=%u lo=0x%016llx hi=0x%016llx\n",
                             n,
                             static_cast<uint32_t>(m_prim.type),
                             static_cast<uint32_t>(m_vtxCount % kMaxVerts),
                             m_vtxCount,
                             static_cast<uint32_t>(adk ? 1u : 0u),
                             static_cast<uint32_t>(x), x / 16.0f,
                             static_cast<uint32_t>(y), y / 16.0f,
                             z,
                             static_cast<uint32_t>(f),
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        if (g105Restart)
            m_vtxCount = 0;
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        { static const bool s_fd = envFlagEnabled("DC2_G65_FORCEDRAW"); if (s_fd) adk = false; }
        bool g105Restart = false;
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (!adk && g105SynthAdcEnabled() &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                m_prim.type == GS_PRIM_TRISTRIP &&
                m_prim.tme &&
                ctx.frame.fbp != 0x139u &&
                g105TitleRockTbp(ctx.tex0.tbp0))
            {
                const float guard = g105AdcGuard();
                const float sx = static_cast<float>(x) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofx >> 4);
                const float sy = static_cast<float>(y) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofy >> 4);
                const bool outside =
                    sx < static_cast<float>(ctx.scissor.x0) - guard ||
                    sx > static_cast<float>(ctx.scissor.x1) + guard ||
                    sy < static_cast<float>(ctx.scissor.y0) - guard ||
                    sy > static_cast<float>(ctx.scissor.y1) + guard;
                if (outside)
                {
                    adk = true;
                    g105Restart = g105ResetStripEnabled();
                    static const bool s_stat = envFlagEnabled("DC2_G105_ADC_STAT");
                    if (s_stat)
                    {
                        static std::atomic<uint64_t> s_forced{0}, s_seen{0};
                        const uint64_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t fcnt = s_forced.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if ((n & 0x1FFFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G105:adc] seen=%llu forced=%llu guard=%.0f tbp=0x%x xy=(%.1f,%.1f)\n",
                                         static_cast<unsigned long long>(n),
                                         static_cast<unsigned long long>(fcnt),
                                         guard, ctx.tex0.tbp0, sx, sy);
                    }
                }
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        if (g105Restart)
            m_vtxCount = 0;
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        if (f33AdTraceEnabled() && f33TraceAdRegister(addr))
        {
            static std::atomic<uint32_t> s_f33AdCount{0};
            const uint32_t n = s_f33AdCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 1024u)
                f33TraceAdWrite(n, addr, lo);
        }
        writeRegister(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegister(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // PHASE F30: gsreg probe — track RGBAQ separately to catch color sets
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30GsReg{0};
        static std::atomic<uint32_t> s_f30Rgbaq{0};
        static std::atomic<uint32_t> s_f30RgbaqNz{0};
        const uint32_t n = s_f30GsReg.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (regAddr == 0x01u)
        {
            const uint32_t k = s_f30Rgbaq.fetch_add(1, std::memory_order_relaxed) + 1u;
            const uint32_t lo = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            const bool rgbNz = (lo & 0x00FFFFFFu) != 0u;
            if (rgbNz)
                s_f30RgbaqNz.fetch_add(1, std::memory_order_relaxed);
            if (k <= 8u || (k & 0x3FFu) == 0u || rgbNz)
            {
                std::fprintf(stderr,
                             "[F30:rgbaq] k=%u value=0x%016llx rgbNz=%u nzTotal=%u\n",
                             k, static_cast<unsigned long long>(value),
                             rgbNz ? 1u : 0u, s_f30RgbaqNz.load());
            }
        }
        if (regAddr >= 0x50u && regAddr <= 0x53u)
        {
            static std::atomic<uint32_t> s_f31UploadRegCount{0};
            const uint32_t k = s_f31UploadRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (k <= 64u || (k % 256u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:upload-reg] k=%u reg=0x%02x value=0x%016llx\n",
                             k, regAddr, static_cast<unsigned long long>(value));
            }
        }
        if (n <= 12u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                         "[F30:gsreg] n=%u reg=0x%02x lo=0x%08x\n",
                         n, regAddr, static_cast<uint32_t>(value & 0xFFFFFFFFu));
        }
    }
    if (phaseDiagnosticsEnabled() && regAddr == GS_REG_PRIM)
    {
        static std::atomic<uint32_t> s_f31PrimRegCount{0};
        const uint32_t n = s_f31PrimRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 4096u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:prim-reg] n=%u value=0x%llx prmodecont=%u type=%u iip=%u tme=%u abe=%u fst=%u ctxt=%u\n",
                         n, static_cast<unsigned long long>(value),
                         m_prmodecont ? 1u : 0u,
                         static_cast<uint32_t>(value & 0x7u),
                         static_cast<uint32_t>((value >> 3u) & 1u),
                         static_cast<uint32_t>((value >> 4u) & 1u),
                         static_cast<uint32_t>((value >> 6u) & 1u),
                         static_cast<uint32_t>((value >> 8u) & 1u),
                         static_cast<uint32_t>((value >> 9u) & 1u));
        }
    }
    else if (phaseDiagnosticsEnabled() && (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2))
    {
        static std::atomic<uint32_t> s_f31Tex0RegCount{0};
        const uint32_t n = s_f31Tex0RegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 1024u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:tex0-reg] n=%u reg=0x%02x value=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u\n",
                         n, regAddr, static_cast<unsigned long long>(value),
                         static_cast<uint32_t>(value & 0x3FFFu),
                         static_cast<uint32_t>((value >> 14u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 20u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 26u) & 0xFu),
                         static_cast<uint32_t>((value >> 30u) & 0xFu),
                         static_cast<uint32_t>((value >> 34u) & 0x1u),
                         static_cast<uint32_t>((value >> 35u) & 0x3u),
                         static_cast<uint32_t>((value >> 37u) & 0x3FFFu),
                         static_cast<uint32_t>((value >> 51u) & 0xFu),
                         static_cast<uint32_t>((value >> 55u) & 0x1u),
                         static_cast<uint32_t>((value >> 56u) & 0x1Fu),
                         static_cast<uint32_t>((value >> 61u) & 0x7u));
        }
    }
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_prim.type = static_cast<GSPrimType>(value & 0x7);
        if (m_prmodecont)
        {
            m_prim.iip = ((value >> 3) & 1) != 0;
            m_prim.tme = ((value >> 4) & 1) != 0;
            m_prim.fge = ((value >> 5) & 1) != 0;
            m_prim.abe = ((value >> 6) & 1) != 0;
            m_prim.aa1 = ((value >> 7) & 1) != 0;
            m_prim.fst = ((value >> 8) & 1) != 0;
            m_prim.ctxt = ((value >> 9) & 1) != 0;
            m_prim.fix = ((value >> 10) & 1) != 0;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        if (renderQualityTraceEnabled() && g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
            g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
            g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G35:seq] PRIM type=%u ctxt=%u tme=%u abe=%u\n",
                         (unsigned)m_prim.type, (unsigned)m_prim.ctxt,
                         (unsigned)m_prim.tme, (unsigned)m_prim.abe);
        }
        break;
    }
    case GS_REG_RGBAQ:
    {
        // G52: hold last valid color on a spurious fully-zero model RGBAQ (see writeRegisterPacked
        // case 0x01). Covers the A+D / REGLIST color path. Scope: costume model fbp=0x139 tristrip.
        static const bool g52holdOff = envFlagEnabled("DC2_G52_NO_HOLDCOLOR");
        const bool g52ModelCtx = (m_prim.type == GS_PRIM_TRISTRIP &&
                                  m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u);
        if (!g52holdOff && g52ModelCtx && (value & 0xFFFFFFFFull) == 0ull)
            break;
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0xFFFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0xFFFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        if (renderQualityTraceEnabled() && g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
            g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            const auto &sc = m_ctx[m_prim.ctxt & 1u].tex0;
            g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
            g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G35:seq] XYZF2 xy=(%.1f,%.1f) curTex0(tbp=0x%x psm=0x%x) ctxt=%u%s\n",
                         vtx.x, vtx.y, sc.tbp0, (unsigned)sc.psm, (unsigned)m_prim.ctxt,
                         (sc.tbp0 == 0x2720u) ? ((sc.psm == 0u) ? "  <-- verts under CT32 (GOOD)" : "  <-- verts under T8 (BUG)") : "");
        }
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        // [G34] Does the costume composite's sprite geometry reach the GS? Log vertices
        // kicked while the active context binds the CT32 0x2720 RTT (the composite).
        if (renderQualityTraceEnabled())
        {
            const auto &ct = m_ctx[m_prim.ctxt & 1u].tex0;
            if (m_prim.tme && ct.tbp0 == 0x2720u && ct.psm == 0u)
            {
                static std::atomic<uint32_t> s_g34v{0};
                const uint32_t vn = s_g34v.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (vn <= 64u)
                    std::fprintf(stderr, "[G34:vtx2720ct32] vn=%u xyz2 vtxCount=%u xy=(%.1f,%.1f) prim=%u ctxt=%u uv=(%u,%u)\n",
                                 vn, m_vtxCount, vtx.x, vtx.y,
                                 (unsigned)m_prim.type, (unsigned)m_prim.ctxt,
                                 (unsigned)m_curU, (unsigned)m_curV);
            }
            if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
                g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
            {
                const auto &sc = m_ctx[m_prim.ctxt & 1u].tex0;
                g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] XYZ2 xy=(%.1f,%.1f) curTex0(tbp=0x%x psm=0x%x) ctxt=%u%s\n",
                             vtx.x, vtx.y, sc.tbp0, (unsigned)sc.psm, (unsigned)m_prim.ctxt,
                             (sc.tbp0 == 0x2720u) ? ((sc.psm == 0u) ? "  <-- verts under CT32 (GOOD)" : "  <-- verts under T8 (BUG)") : "");
            }
        }
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        // [G91] Ground-truth TEX0 register-write stream for the title rock. Logs every rock-range
        // TEX0 bind in DMA/GIF stream order (title scope) so we can tell whether the wall pages
        // (0x2b20/0x2f20/0x3320) are ever WRITTEN to the GS context at all (vs the geometry just
        // inheriting a stale block-0 late texture). DC2_G91_TEX0.
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            static const bool s_g91tex0 = (std::getenv("DC2_G91_TEX0") != nullptr);
            if (s_g91tex0 && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                t.tbp0 >= 0x2720u && t.tbp0 <= 0x3960u)
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (k <= 4000u)
                    std::fprintf(stderr, "[G91:tex0] k=%u ci=%d tbp=0x%x psm=0x%x cbp=0x%x tw=%u th=%u\n",
                        k, ci, t.tbp0, (unsigned)t.psm, t.cbp, (unsigned)t.tw, (unsigned)t.th);
            }
        }
        // [G123] ordered interleave: emit the rock-range BIND event (and flush the pending
        // draw-run that preceded it) so the bind/kick pairing is visible vs the .gs ground truth.
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            if (g123SeqEnabled() && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                g105TitleRockTbp(t.tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                g123FlushRun();
                if (s_g123Events < g123Cap())
                {
                    const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G123:seq] %u BIND tbp=0x%x psm=0x%x ci=%d\n",
                                 seq, t.tbp0, (unsigned)t.psm, ci);
                    ++s_g123Events;
                }
            }
        }
        // [G34] Count every TEX0 register bind of block 0x2720 (the model-RTT / brick
        // time-share) by psm. Tells definitively whether the costume model composite ever
        // emits a CT32 (psm=0) bind to GS, vs only the T8 brick (psm=0x13). ct32==0 after
        // a costume run => the composite's CT32 sprite never reaches the GS at all.
        if (renderQualityTraceEnabled() && t.tbp0 == 0x2720u)
        {
            static std::atomic<uint32_t> s_g34ct32{0};
            static std::atomic<uint32_t> s_g34t8{0};
            static std::atomic<uint32_t> s_g34oth{0};
            std::atomic<uint32_t> *c = (t.psm == 0u) ? &s_g34ct32
                                       : ((t.psm == GS_PSM_T8) ? &s_g34t8 : &s_g34oth);
            const uint32_t bn = c->fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((t.psm == 0u && bn <= 32u) || (bn % 8192u) == 0u)
                std::fprintf(stderr,
                    "[G34:tex0bind2720] psm=0x%x tbw=%u tw=%u th=%u bn=%u (ct32=%u t8=%u oth=%u)\n",
                    (unsigned)t.psm, (unsigned)t.tbw, (unsigned)t.tw, (unsigned)t.th, bn,
                    s_g34ct32.load(std::memory_order_relaxed),
                    s_g34t8.load(std::memory_order_relaxed),
                    s_g34oth.load(std::memory_order_relaxed));
        }
        // [G35] GIF register-sequence capture: arm on a CT32 0x2720 bind, then log every
        // following TEX0 (catching the T8 0x2720 clobber), bounded by the window + global cap.
        if (renderQualityTraceEnabled() && g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            const int seqCi = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
            if (t.tbp0 == 0x2720u && t.psm == 0u)
            {
                g_g34SeqWindow.store(70, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x2720 psm=0x0 tbw=%u  (CT32 ARM)\n",
                             seqCi, (unsigned)t.tbw);
            }
            else if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0)
            {
                g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x%x psm=0x%x tbw=%u%s\n",
                             seqCi, t.tbp0, (unsigned)t.psm, (unsigned)t.tbw,
                             (t.tbp0 == 0x2720u && t.psm != 0u) ? "   <-- T8 0x2720 CLOBBER" : "");
            }
        }
        // F50.9: uncapped histogram of paletted (PSMT4/T8) TEX0 binds per context, to see
        // whether materials ever bind the uploaded pages (0x2820/0x2f20/...) or only the
        // empty 0x2580/0x2980 the dungeon draw samples.
        if (renderQualityTraceEnabled() &&
            (t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH))
        {
            // Skip the title 4HH spam (tbp=0x1a00) so the cap captures dungeon-steady-state binds.
            if (t.tbp0 != 0x1a00u)
            {
                static std::atomic<uint32_t> s_f509Tex0Bind{0};
                const uint32_t n = s_f509Tex0Bind.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 300u)
                    std::fprintf(stderr, "[F509:tex0bind] n=%u ci=%d tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u\n",
                                 n, ci, t.tbp0, (unsigned)t.tbw, (unsigned)t.psm,
                                 t.cbp, (unsigned)t.cpsm, (unsigned)t.cld);
            }
        }
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
        m_prmodecont = (value & 1) != 0;
        break;
    case GS_REG_PRMODE:
        if (!m_prmodecont)
        {
            m_prim.iip = ((value >> 3) & 1) != 0;
            m_prim.tme = ((value >> 4) & 1) != 0;
            m_prim.fge = ((value >> 5) & 1) != 0;
            m_prim.abe = ((value >> 6) & 1) != 0;
            m_prim.aa1 = ((value >> 7) & 1) != 0;
            m_prim.fst = ((value >> 8) & 1) != 0;
            m_prim.ctxt = ((value >> 9) & 1) != 0;
            m_prim.fix = ((value >> 10) & 1) != 0;
        }
        break;
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        const GSFrameReg prev = m_ctx[ci].frame;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        if (frameRegisterTraceEnabled())
        {
            static std::atomic<uint32_t> s_f49FrameRegCount{0};
            const uint32_t n = s_f49FrameRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool zeroed = (m_ctx[ci].frame.fbp == 0u || m_ctx[ci].frame.fbw == 0u);
            const bool changed = prev.fbp != m_ctx[ci].frame.fbp ||
                                 prev.fbw != m_ctx[ci].frame.fbw ||
                                 prev.psm != m_ctx[ci].frame.psm ||
                                 prev.fbmsk != m_ctx[ci].frame.fbmsk;
            if ((changed && (n <= 256u || (n % 512u) == 0u)) || zeroed)
            {
                std::fprintf(stderr,
                             "[F49:frame] n=%u reg=0x%02x ctx=%d raw=0x%016llx prev(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) new(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) zero=%u primCtxt=%u bitblt(dbp=0x%x dbw=%u dpsm=0x%x)\n",
                             n, regAddr, ci, static_cast<unsigned long long>(value),
                             prev.fbp, prev.fbw, static_cast<uint32_t>(prev.psm), prev.fbmsk,
                             m_ctx[ci].frame.fbp, m_ctx[ci].frame.fbw, static_cast<uint32_t>(m_ctx[ci].frame.psm), m_ctx[ci].frame.fbmsk,
                             zeroed ? 1u : 0u,
                             m_prim.ctxt ? 1u : 0u,
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw), static_cast<uint32_t>(m_bitbltbuf.dpsm));
            }
        }
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf = value;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        const uint32_t prevDbp = m_bitbltbuf.dbp;
        const uint8_t prevDbw = m_bitbltbuf.dbw;
        const uint8_t prevDpsm = m_bitbltbuf.dpsm;
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        if (frameRegisterTraceEnabled())
        {
            static std::atomic<uint32_t> s_f49BitbltRegCount{0};
            const uint32_t n = s_f49BitbltRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool changed = prevDbp != m_bitbltbuf.dbp ||
                                 prevDbw != m_bitbltbuf.dbw ||
                                 prevDpsm != m_bitbltbuf.dpsm;
            if (changed && (n <= 128u || (n % 256u) == 0u))
            {
                std::fprintf(stderr,
                             "[F49:bitblt] n=%u raw=0x%016llx dst=0x%x/%u/0x%x src=0x%x/%u/0x%x primCtxt=%u frame0=0x%x/%u frame1=0x%x/%u\n",
                             n, static_cast<unsigned long long>(value),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw), static_cast<uint32_t>(m_bitbltbuf.dpsm),
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw), static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_prim.ctxt ? 1u : 0u,
                             m_ctx[0].frame.fbp, m_ctx[0].frame.fbw,
                             m_ctx[1].frame.fbp, m_ctx[1].frame.fbw);
            }
        }
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37BitbltCount{0};
            const uint32_t n = s_f37BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:bitblt] n=%u raw=0x%016llx sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n, static_cast<unsigned long long>(value),
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw),
                             static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
        }
        if (t8UploadTraceEnabled())
        {
            static std::atomic<uint32_t> s_f43BitbltCount{0};
            const uint32_t n = s_f43BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 128u)
                std::fprintf(stderr, "[F43:t8] n=%u sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n,
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw),
                             static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
        }
        // F50.9: UNCAPPED check — does ANY BITBLTBUF ever target the pages the dungeon
        // paletted draws actually sample (tbp=0x2580 texture, cbp=0x2980 CLUT)? If never,
        // the upload-destination vs draw-reference addressing genuinely diverges.
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f509Tex2580{0};
            static std::atomic<uint32_t> s_f509Clut2980{0};
            static std::atomic<uint32_t> s_f509Total{0};
            const uint32_t tot = s_f509Total.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (m_bitbltbuf.dbp == 0x2580u)
            {
                const uint32_t h = s_f509Tex2580.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2580] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm);
            }
            if (m_bitbltbuf.dbp == 0x2980u)
            {
                const uint32_t h = s_f509Clut2980.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2980] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm);
            }
            if ((tot % 256u) == 0u)
                std::fprintf(stderr, "[F509:bbtally] total=%u hit2580=%u hit2980=%u\n",
                             tot, s_f509Tex2580.load(), s_f509Clut2980.load());
        }
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37TrxregCount{0};
            const uint32_t n = s_f37TrxregCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:trxreg] n=%u rrw=%u rrh=%u\n", n, m_trxreg.rrw, m_trxreg.rrh);
        }
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        m_hwregX = 0;
        m_hwregY = 0;
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37TrxdirCount{0};
            const uint32_t n = s_f37TrxdirCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:trxdir] n=%u dir=%u dbp=0x%x dpsm=0x%x rr=(%u,%u)\n",
                             n, m_trxdir, m_bitbltbuf.dbp,
                             static_cast<uint32_t>(m_bitbltbuf.dpsm),
                             m_trxreg.rrw, m_trxreg.rrh);
        }

        if (m_trxdir == 2 && m_vram)
        {
            performLocalToLocalTransfer();
        }
        else if (m_trxdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_TEXFLUSH:
    case GS_REG_SCANMSK:
    case GS_REG_FOGCOL:
    case GS_REG_DIMX:
    case GS_REG_DTHE:
    case GS_REG_COLCLAMP:
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
        break;
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr |= 0x1;
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_privRegs)
            m_privRegs->csr |= 0x2;
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }
}

