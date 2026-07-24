// ===== G341 parallel-VIF-decode premise-gate census (DIAGNOSIS ONLY, default OFF, DC2_G341_CENSUS=1) =====
// Arc #2 (plans/arc-native-renderer.md §Unexplored #2) proposes splitting the VIF1 DMA chain into
// independent parallel chunk-decoders to cut the "~13 ms serial GIF/VIF parse + G144Entry batch
// build" that G335 attributed to the GS-worker front-end. Per the §2 Optimization Premise Gate
// (G340 discipline: census the ceiling BEFORE building the machinery), this measures WHAT the VIF
// chain the worker replays is actually MADE OF — so we can tell whether the parallelizable term
// (geometry decode → G144Entry) is large enough to move the pole, or whether the front-end wall is
// dominated by work that is NOT parallelizable-decode (IMAGE upload → readback/deswizzle family).
//
// Reconciles against the existing G316 path split (p1/p2/p3 wall ms/f) and G332 front-end/backend.
// It classifies every packet the worker dispatches, per GIF path, by CONTENT:
//   - IMAGE : carries flg==IMAGE payload (or a pending IMAGE continuation) → processImageData work
//             = the CT32 pack + backend readback + swizzle-back deswizzle cascade (G313/G314/G340,
//             a proven local minimum, NOT parallelizable chunk-decode).
//   - DRAW  : carries vertex-kick registers (XYZ(F)2/3) → drawPrimitive + G144Entry batch build
//             = the ONLY term arc #2 could parallelize.
//   - REG   : register-only setup (TEX0/FRAME/TEST/...). Cheap; and it is the cross-packet state
//             prefix a parallel decoder would have to replay SERIALLY as a checkpoint (DC2 draw
//             packets inherit context — Path1 XGKICK submits raw geometry, see vu1 g96Submit).
//
// Pure byte counting, no GS mutation, no per-packet timing (G312 trap). Worker-thread only (same
// single-thread contract as processWindow + the isFrameBoundary branch that prints it). No-op unless
// DC2_G341_CENSUS=1. See plans/phase-G341-fix-log.md.

namespace
{
    bool g341CensusOn()
    {
        static const bool s_on = f50_12_env_flag("DC2_G341_CENSUS");
        return s_on;
    }

    // Cumulative worker-thread counters (windowed deltas printed every 30 boundaries). Index 0/1/2
    // = Path1/Path2/Path3. No atomics: written only on the worker thread (processWindow, boundary).
    uint64_t g_g341Windows = 0u;
    uint64_t g_g341Pkts = 0u;
    uint64_t g_g341ImgPkt[3] = {0, 0, 0};
    uint64_t g_g341DrawPkt[3] = {0, 0, 0};
    uint64_t g_g341RegPkt[3] = {0, 0, 0};
    uint64_t g_g341ImgBytes[3] = {0, 0, 0};
    uint64_t g_g341DrawVerts[3] = {0, 0, 0};
    uint64_t g_g341RegWrites[3] = {0, 0, 0};
    uint64_t g_g341PktBytes[3] = {0, 0, 0};
    // IMAGE continuation carried across packets/windows within the run (mirrors GS::s_pendingImageBytes).
    uint32_t g_g341PendingImageBytes = 0u;

    inline bool g341IsVertexReg(uint8_t r)
    {
        // GS_REG_XYZF2=0x04, XYZ2=0x05, XYZF3=0x0C, XYZ3=0x0D — same codes for PACKED descriptor and
        // A+D register address (ps2_gs_gpu.h).
        return r == 0x04u || r == 0x05u || r == 0x0Cu || r == 0x0Du;
    }

    // Classify ONE packet by content and accumulate per-path counters. Pure read of pkt.data bytes.
    void g341ClassifyPacket(const GifArbiterPacket &pkt)
    {
        const uint32_t pathIdx = static_cast<uint32_t>(static_cast<uint8_t>(pkt.pathId)) - 1u;
        if (pathIdx > 2u || pkt.data.empty())
            return;
        ++g_g341Pkts;
        const uint8_t *data = pkt.data.data();
        const uint32_t size = static_cast<uint32_t>(pkt.data.size());
        g_g341PktBytes[pathIdx] += size;

        uint64_t imgBytes = 0u;
        uint64_t drawVerts = 0u;
        uint64_t regWrites = 0u;
        uint32_t offset = 0u;

        // Leading IMAGE continuation (no tag) — a split PATH2/PATH3 IMAGE transfer (G6).
        if (g_g341PendingImageBytes != 0u)
        {
            const uint32_t consume = std::min<uint32_t>(g_g341PendingImageBytes, size);
            imgBytes += consume;
            offset = consume;
            g_g341PendingImageBytes -= consume;
        }

        while (offset + 16u <= size)
        {
            const uint64_t tagLo = f50_12_load64(data + offset);
            const uint64_t tagHi = f50_12_load64(data + offset + 8u);
            offset += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            const bool pre = ((tagLo >> 46u) & 1u) != 0u;
            if (pre)
                ++regWrites; // PRE writes PRIM

            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i)
                regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            if (flg == GIF_FMT_PACKED)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 16u > size)
                            goto done;
                        const uint64_t hi = f50_12_load64(data + offset + 8u);
                        offset += 16u;
                        const uint8_t desc = regs[r];
                        if (g341IsVertexReg(desc))
                            ++drawVerts;
                        else if (desc == 0x0Eu) // A+D: register selected by hi[7:0]
                        {
                            if (g341IsVertexReg(static_cast<uint8_t>(hi & 0xFFu)))
                                ++drawVerts;
                            else
                                ++regWrites;
                        }
                        else if (desc != 0x0Fu) // 0x0F = NOP
                            ++regWrites;
                    }
                }
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 8u > size)
                            goto done;
                        offset += 8u;
                        if (g341IsVertexReg(regs[r]))
                            ++drawVerts;
                        else
                            ++regWrites;
                    }
                }
                if (((nloop * nreg) & 1u) != 0u)
                    offset += 8u;
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint32_t expected = nloop * 16u;
                uint32_t actual = expected;
                if (offset + actual > size)
                    actual = size - offset;
                imgBytes += actual;
                offset += actual;
                if (actual < expected)
                {
                    g_g341PendingImageBytes = expected - actual;
                    goto done;
                }
            }
            else
            {
                goto done;
            }
        }
    done:
        if (imgBytes != 0u)
        {
            ++g_g341ImgPkt[pathIdx];
            g_g341ImgBytes[pathIdx] += imgBytes;
        }
        else if (drawVerts != 0u)
        {
            ++g_g341DrawPkt[pathIdx];
        }
        else
        {
            ++g_g341RegPkt[pathIdx];
        }
        g_g341DrawVerts[pathIdx] += drawVerts;
        g_g341RegWrites[pathIdx] += regWrites;
    }

    // Called per draw window (after the MTVU merge + path sort) — counts every packet the worker
    // will dispatch this window. Behavior-pure.
    void g341ScanWindow(const std::vector<GifArbiterPacket> &q)
    {
        if (!g341CensusOn())
            return;
        ++g_g341Windows;
        for (const auto &pkt : q)
            g341ClassifyPacket(pkt);
    }

    void g341ReportBoundary()
    {
        if (!g341CensusOn())
            return;
        static uint64_t s_frame = 0u;
        static uint64_t s_win = 0u, s_pkt = 0u;
        static uint64_t s_imgPkt[3] = {0, 0, 0}, s_drawPkt[3] = {0, 0, 0}, s_regPkt[3] = {0, 0, 0};
        static uint64_t s_imgBytes[3] = {0, 0, 0}, s_drawV[3] = {0, 0, 0}, s_regW[3] = {0, 0, 0};
        static uint64_t s_pktBytes[3] = {0, 0, 0};
        ++s_frame;
        if ((s_frame % 30u) != 0u)
            return;
        const double f = 30.0;
        auto d = [](uint64_t cur, uint64_t &last) { uint64_t v = cur - last; last = cur; return v; };
        const double win = d(g_g341Windows, s_win) / f;
        const double pkt = d(g_g341Pkts, s_pkt) / f;
        double imgPkt[3], drawPkt[3], regPkt[3], imgKB[3], drawV[3], regW[3], pktKB[3];
        for (int i = 0; i < 3; ++i)
        {
            imgPkt[i] = d(g_g341ImgPkt[i], s_imgPkt[i]) / f;
            drawPkt[i] = d(g_g341DrawPkt[i], s_drawPkt[i]) / f;
            regPkt[i] = d(g_g341RegPkt[i], s_regPkt[i]) / f;
            imgKB[i] = d(g_g341ImgBytes[i], s_imgBytes[i]) / f / 1024.0;
            drawV[i] = d(g_g341DrawVerts[i], s_drawV[i]) / f;
            regW[i] = d(g_g341RegWrites[i], s_regW[i]) / f;
            pktKB[i] = d(g_g341PktBytes[i], s_pktBytes[i]) / f / 1024.0;
        }
        std::fprintf(stderr,
                     "[G341:vifcensus] frames=30 windows/f=%.0f pkts/f=%.0f | "
                     "P1[img=%.0f/%.0fKB draw=%.0f/%.0fv reg=%.0f/%.0fw bytes=%.0fKB] "
                     "P2[img=%.0f/%.0fKB draw=%.0f/%.0fv reg=%.0f/%.0fw bytes=%.0fKB] "
                     "P3[img=%.0f/%.0fKB draw=%.0f reg=%.0f bytes=%.0fKB]\n",
                     win, pkt,
                     imgPkt[0], imgKB[0], drawPkt[0], drawV[0], regPkt[0], regW[0], pktKB[0],
                     imgPkt[1], imgKB[1], drawPkt[1], drawV[1], regPkt[1], regW[1], pktKB[1],
                     imgPkt[2], imgKB[2], drawPkt[2], regPkt[2], pktKB[2]);
    }
} // namespace
