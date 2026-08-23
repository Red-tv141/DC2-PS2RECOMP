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

    // G508: the outer holder. Nested writeRegister guards skip the already-owned recursive
    // re-acquisition when this raises the depth. The arm is read HERE, once per packet (~4,465/f),
    // and never in writeRegister (~55.8k/f) — the measured path carries no instrument. Gated on the
    // arm VALUE (G504): -1 = not under the A/B (shipped default, raise), 0 = control (pre-G508
    // path), 1 = candidate.
    const int g508Arm = g508LockSkipArm();
    G508StateLock lock(m_stateMutex, g508Arm != 0);
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
        // G401: continuation payload is opaque pixel data. It may coincidentally satisfy the old
        // "PACKED A+D tag" heuristic (Dungeon 6 begins its 64 KiB chunks with
        // 100e0c0e0f0c0c0d/180d0c0c150e0c0e), so never discard a live IMAGE transfer based on
        // payload contents. VIF/arbiter ordering owns the continuation boundary.
        static const bool g401LegacyPendingDrop =
            envFlagEnabled("DC2_G401_LEGACY_PENDING_DROP");
        static const bool g401GifTrace = envFlagEnabled("DC2_G401_GIF_TRACE");
        if (g401LegacyPendingDrop && looksLikePackedAdGifTag(data, sizeBytes))
        {
            if ((t8UploadTraceEnabled() && f51PendingDropShouldLog()) || g401GifTrace)
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

        // G602: the descriptor list IS `tagHi`'s nibbles, so read them where they already are. The
        // old `uint8_t regs[16]` staging array cost 16 stores per tag AND was the only stack array
        // in this function, i.e. the reason MSVC gave it a /GS security cookie.
#if DC2_G602_TAGWALK
#define G602_REGDESC(r) static_cast<uint8_t>((tagHi >> ((r) * 4)) & 0xFu)
#else
        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);
#define G602_REGDESC(r) regs[r]
#endif

        if (flg == GIF_FMT_PACKED)
        {
            m_hwregX = 0;
            m_hwregY = 0;
            const bool g171On = g171PerfEnabled();
            // ===== G593 ceiling probe (DC2_G593_NOREG=1, default-off, knowingly WRONG output) =====
            // The arm selects a DUPLICATED cold loop rather than adding a branch to the hot one.
            // The first form put one loop-invariant predicted branch inside the ~141 k
            // descriptors/frame loop, and `[G147:gif] parserOther` moved 9.12 -> 9.76 ms/f - 4.5 ns
            // per descriptor of standing tax on the POLE THREAD for a diagnostic that ships
            // default-off. G520's law is that arm symmetry is a CODEGEN property; here the
            // requirement is the opposite and stronger - the DEFAULT arm's code must be untouched.
            if (g593NoRegOn())
            {
                for (uint32_t loop = 0; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (offset + 16 > sizeBytes)
                        {
                            g147FlushCounters();
                            return;
                        }
                        const uint64_t lo = loadLE64(data + offset);
                        const uint64_t hi = loadLE64(data + offset + 8);
                        offset += 16;
                        ++g147PackedRegs;
                        // Keep every vertex kick so [G434:inv] kicks/f stays exact; drop every
                        // other descriptor. A+D (0x0E) carries its GS register address in
                        // hi[7:0], and the four vertex codes are the same there as in the PACKED
                        // descriptor table.
                        const uint8_t d593 = G602_REGDESC(r);
                        const uint8_t a593 = static_cast<uint8_t>(hi & 0xFFu);
                        const bool vtx593 =
                            (d593 == 0x04u || d593 == 0x05u || d593 == 0x0Cu || d593 == 0x0Du) ||
                            (d593 == 0x0Eu && (a593 == 0x04u || a593 == 0x05u ||
                                               a593 == 0x0Cu || a593 == 0x0Du));
                        if (vtx593)
                            writeRegisterPacked(d593, lo, hi);
                    }
                }
                continue; // this tag is fully consumed
            }
#if DC2_G602_TAGWALK
            // ===== G602 (default): the descriptor walk, bounds-checked ONCE PER TAG =====
            // Two per-descriptor costs leave the ~141 k/frame loop:
            //   1. `offset + 16 > sizeBytes`, replaced by one `availDesc` computation. Truncation
            //      semantics are identical - process exactly min(nloop*nreg, availDesc)
            //      descriptors, count exactly those, then return if the tag was short.
            //   2. the loop-invariant `if (g171On)` branch, which now selects a duplicated loop
            //      instead - the same remedy G593 applied to its own probe, for the same measured
            //      reason.
            if (!g171On)
            {
                const uint32_t availDesc = (sizeBytes - offset) >> 4;
                const uint64_t wantDesc = static_cast<uint64_t>(nloop) * nreg;
                const bool truncated = wantDesc > static_cast<uint64_t>(availDesc);
                const uint32_t nDesc = truncated ? availDesc : static_cast<uint32_t>(wantDesc);
                const uint32_t fullLoops = nDesc / nreg;
                const uint32_t tailRegs = nDesc - fullLoops * nreg;
                const uint8_t *p = data + offset;
                // ===== ⭐ G650 (ROADMAP P3): the descriptor bodies, INLINE ====================
                // The seven vertex-carrying descriptors execute here instead of through an
                // out-of-line call into `writeRegisterPacked`'s 16-way switch. The bodies are the
                // SAME MACROS that switch's `case` labels expand, so there is no second copy of
                // the arithmetic. Everything else still takes the original call.
                //
                // ⚠️ The arm is a DUPLICATED LOOP, selected once per packet — never an in-loop
                // ternary. G593 measured +0.64 ms/f for one loop-invariant branch inside exactly
                // this loop, and G645 generalised it into a law.
                if (g650GifInlineOn())
                {
                    if (g650GifStatOn())
                    {
                        g_g650GifDesc.fetch_add(nDesc, std::memory_order_relaxed);
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            const uint8_t dd = G602_REGDESC(r);
                            const uint32_t reps = (r < tailRegs) ? (fullLoops + 1u) : fullLoops;
                            g_g650GifByDesc[dd & 0xFu].fetch_add(reps, std::memory_order_relaxed);
                            const bool inl = (dd <= 0x05u && dd >= 0x01u) || dd == 0x0Au || dd == 0x0Fu;
                            (inl ? g_g650GifInlined : g_g650GifCalled)
                                .fetch_add(reps, std::memory_order_relaxed);
                        }
                        g650GifReport();
                    }
                    for (uint32_t loop = 0; loop < fullLoops; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r, p += 16)
                        {
                            const uint8_t g650d = G602_REGDESC(r);
                            const uint64_t g650lo = loadLE64(p);
                            const uint64_t g650hi = loadLE64(p + 8);
                            G650_PK_DISPATCH(g650d, g650lo, g650hi);
                        }
                    }
                    for (uint32_t r = 0; r < tailRegs; ++r, p += 16)
                    {
                        const uint8_t g650d = G602_REGDESC(r);
                        const uint64_t g650lo = loadLE64(p);
                        const uint64_t g650hi = loadLE64(p + 8);
                        G650_PK_DISPATCH(g650d, g650lo, g650hi);
                    }
                }
                else
                {
                    for (uint32_t loop = 0; loop < fullLoops; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r, p += 16)
                            writeRegisterPacked(G602_REGDESC(r), loadLE64(p), loadLE64(p + 8));
                    }
                    for (uint32_t r = 0; r < tailRegs; ++r, p += 16)
                        writeRegisterPacked(G602_REGDESC(r), loadLE64(p), loadLE64(p + 8));
                }
                offset += nDesc * 16u;
                g147PackedRegs += nDesc;
                if (truncated)
                {
                    g147FlushCounters();
                    return;
                }
                continue; // this tag is fully consumed
            }
            // [G171:regstat] census arm - its own loop, so the default walk above carries no branch.
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    const uint64_t lo = loadLE64(data + offset);
                    const uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    const uint8_t g171Desc = G602_REGDESC(r);
                    const auto g171T0 = std::chrono::steady_clock::now();
                    writeRegisterPacked(g171Desc, lo, hi);
                    const auto g171Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g171T0).count();
                    const GSContext &sctx = m_ctx[m_prim.ctxt & 1u];
                    g602ColdG171Note(g171Desc, hi, static_cast<long long>(g171Ns),
                                     static_cast<uint32_t>(m_prim.type), m_prim.tme, m_prim.abe,
                                     sctx.frame, sctx.scissor,
                                     m_vtxQueue[(m_vtxCount + kMaxVerts - 1) % kMaxVerts],
                                     m_vtxQueue[(m_vtxCount + kMaxVerts - 2) % kMaxVerts]);
                    ++g147PackedRegs;
                }
            }
#else
            // Pre-G602 shape: one fused loop, bounds-checked per descriptor, with the
            // loop-invariant `g171On` test inside it.
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    const uint64_t lo = loadLE64(data + offset);
                    const uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    if (g171On)
                    {
                        const uint8_t g171Desc = G602_REGDESC(r);
                        const auto g171T0 = std::chrono::steady_clock::now();
                        writeRegisterPacked(g171Desc, lo, hi);
                        const auto g171Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - g171T0).count();
                        const GSContext &sctx = m_ctx[m_prim.ctxt & 1u];
                        g602ColdG171Note(g171Desc, hi, static_cast<long long>(g171Ns),
                                         static_cast<uint32_t>(m_prim.type), m_prim.tme, m_prim.abe,
                                         sctx.frame, sctx.scissor,
                                         m_vtxQueue[(m_vtxCount + kMaxVerts - 1) % kMaxVerts],
                                         m_vtxQueue[(m_vtxCount + kMaxVerts - 2) % kMaxVerts]);
                    }
                    else
                    {
                        writeRegisterPacked(G602_REGDESC(r), lo, hi);
                    }
                    ++g147PackedRegs;
                }
            }
#endif
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
                    writeRegister(G602_REGDESC(r), loadLE64(data + offset));
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
#undef G602_REGDESC

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegister(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        // G52: the Select-Costume Max model (skinned, fbp=0x139 RTT) occasionally emits a
        // FULLY-ZERO RGBAQ for a skin vertex — a VU1/packet artifact. HW's red_vest.gs NEVER
        // emits a zero RGBAQ for this model (every model vertex has alpha=0x80, color>=0x50),
        // so a zero here is spurious; absent the bogus write the GS color register would simply
        // RETAIN the previous (valid) vertex color. Triangles interpolating from these zero
        // verts produced the long-deferred dark "face/hand speckle". Scope: only the costume
        // model context (fbp=0x139 textured tristrip). Kill switch DC2_G52_HOLDCOLOR=0.
        // G650 (P3): the body is now a macro shared with processGIFPacket's inline walk, so the
        // two dispatch paths cannot drift. [G52:rgba] + [F31:packed-rgbaq] are inside it.
        G650_PK_RGBAQ(lo, hi);
        break;
    case 0x02:
        G650_PK_ST(lo, hi); // G650 (P3): shared body, incl. the Q==0 => 1.0 normalisation
        break;
    case 0x03:
        G650_PK_UV(lo); // G650 (P3): shared body
        break;
    case 0x04:
        // G650 (P3): shared body — carries the G105 synthetic-ADC guard, G602_GATE_PK_FORCEDRAW,
        // the [G52:seq]/[G31:xyzf2] trace and the g105Restart postcondition. The two
        // PS2_IF_AGRESSIVE_LOGS blocks that used to sit here are `((void)0)` in every Release
        // configuration (AGRESSIVE_LOGS is not defined) and are not carried into the macro.
        G650_PK_XYZF2(lo, hi);
        break;
    case 0x05:
        G650_PK_XYZ2(lo, hi); // G650 (P3): shared body
        break;
    case 0x0A:
        G650_PK_FOG(hi); // G650 (P3): shared body
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
        // G602: [F33:ad] moved out of line (g602_gif_cold_outline.inc).
        if (G602_GATE_PK_ADTRACE)
            g602ColdPackedAdTrace(addr, lo);
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
    // G508: ~55.8k calls/frame, ~all of them nested inside processGIFPacket's own hold of this
    // same recursive mutex. See G508StateLock in gpu_bridge_and_latch_helpers.inc.
    G508StateLock lock(m_stateMutex);
    // G602: the register census and the whole PHASE F30/F31 diagnostic preamble (three separate
    // tests of `phaseDiagnosticsEnabled()`, each a magic-static guard, on every one of ~55.8k calls
    // per frame) are now one gate plus one out-of-line call. The `interestingReg` /
    // `isCopyRelevantReg` chains and their PS2_IF_AGRESSIVE_LOGS blocks were dead in every Release
    // build (`ps2_log::agressive_logs_enabled` is `constexpr false`) and are deleted with them.
    if (G602_GATE_RG_CENSUS)
        g508NoteRegisterWrite(regAddr, value);
    if (G602_GATE_RG_PHASE)
        g602ColdRegPreamble(regAddr, value, m_curR, m_curG, m_curB, m_prmodecont);

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
        // G602: [G35:seq] PRIM moved out of line.
        if (G602_GATE_RG_QUALITY)
            g602ColdRegPrimTrace(static_cast<uint32_t>(m_prim.type), m_prim.ctxt, m_prim.tme,
                                 m_prim.abe);
        break;
    }
    case GS_REG_RGBAQ:
    {
        // G52: hold last valid color on a spurious fully-zero model RGBAQ (see writeRegisterPacked
        // case 0x01). Covers the A+D / REGLIST color path. Scope: costume model fbp=0x139 tristrip.
        if (G602_GATE_PK_G52HOLD && (value & 0xFFFFFFFFull) == 0ull &&
            m_prim.type == GS_PRIM_TRISTRIP &&
            m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u)
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
        // G602: [G35:seq] moved out of line.
        if (G602_GATE_RG_QUALITY)
            g602ColdRegVertexTrace(true, vtx, m_ctx[m_prim.ctxt & 1u].tex0, m_prim.ctxt,
                                   m_prim.tme, static_cast<uint32_t>(m_prim.type), m_vtxCount,
                                   m_curU, m_curV);
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
        // G602: [G34:vtx2720ct32] + [G35:seq] moved out of line.
        if (G602_GATE_RG_QUALITY)
            g602ColdRegVertexTrace(false, vtx, m_ctx[m_prim.ctxt & 1u].tex0, m_prim.ctxt,
                                   m_prim.tme, static_cast<uint32_t>(m_prim.type), m_vtxCount,
                                   m_curU, m_curV);
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
        // G602: [G91:tex0] / [G123:seq] BIND / [G34:tex0bind2720] / [G35:seq] / [F509:tex0bind]
        // moved out of line (g602_gif_cold_outline.inc).
        if (G602_GATE_RG_TEX0)
            g602ColdRegTex0(regAddr, ci, t);
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
        // G602: [F49:frame] moved out of line (g602_gif_cold_outline.inc).
        if (G602_GATE_RG_FRAME)
            g602ColdRegFrame(regAddr, ci, value, prev, m_ctx[ci].frame, m_prim.ctxt, m_bitbltbuf);
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
        // G602: [F49:bitblt] / [F37:bitblt] / [F43:t8] / [F509:hit*] moved out of line.
        if (G602_GATE_RG_BITBLT)
            g602ColdRegBitblt(value, m_bitbltbuf, prevDbp, prevDbw, prevDpsm,
                              m_prim.ctxt, m_ctx[0].frame, m_ctx[1].frame);
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
        // G602: [F37:trxreg] moved out of line.
        if (G602_GATE_RG_QUALITY)
            g602ColdRegTrxreg(m_trxreg);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        m_hwregX = 0;
        m_hwregY = 0;
        // G602: [F37:trxdir] moved out of line.
        if (G602_GATE_RG_QUALITY)
            g602ColdRegTrxdir(m_trxdir, m_bitbltbuf, m_trxreg);

        // G432: split the addr=0x53 A+D bucket by the transfer it actually dispatches. The dir=1
        // gather charges itself from inside performLocalToHostToBuffer (prep vs pixel loop).
        if (m_trxdir == 2 && m_vram)
        {
            if (g432CensusOn())
            {
                G432Scope sc(&s_g432L2L,
                             static_cast<uint64_t>(m_trxreg.rrw) * m_trxreg.rrh);
                performLocalToLocalTransfer();
            }
            else
            {
                performLocalToLocalTransfer();
            }
        }
        else if (m_trxdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        else if (g432CensusOn())
        {
            ++s_g432Other.n;
        }
        if (g432CensusOn())
            g432Report();
        break;
    }
    case GS_REG_HWREG:
    {
        // G602: `value` is a by-value uint64_t parameter and the GS consumes HWREG as its eight
        // little-endian bytes, so this is byte-identical to the old `uint8_t buf[8]` + memcpy — and
        // it removes the only stack ARRAY in this function, which is what made MSVC give a
        // 55.8k-calls/frame function a /GS security cookie plus a `__security_check_cookie` call on
        // every return.
        processImageData(reinterpret_cast<const uint8_t *>(&value), 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    // G364: FOGCOL is the second operand of the per-pixel fog blend; it used to be discarded
    // together with the genuinely-unimplemented registers below, which is why DC2's dungeon
    // distance fade never appeared (see plans/phase-G364-fix-log.md).
    case GS_REG_FOGCOL:
        g_g364FogCol = static_cast<uint32_t>(value & 0x00FFFFFFu);
        break;
    case GS_REG_TEXFLUSH:
    case GS_REG_SCANMSK:
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
    // G397: 0x59..0x5f are reserved in the GIF A+D register namespace.
    // Privileged PCRTC registers (DISPFB/DISPLAY/BGCOLOR) are MMIO-only and
    // must never be writable through an A+D packet. The old host-private
    // aliases let ordinary Dungeon 6 packet payloads poison presentation.
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:
        // G602: moved out of line (g602_gif_cold_outline.inc). Behaviour unchanged — the aliases
        // stay off unless DC2_G397_LEGACY_RESERVED_DISPLAY is set.
        g602ColdRegReserved(regAddr, value, m_privRegs);
        break;
    default:
        break;
    }
}
