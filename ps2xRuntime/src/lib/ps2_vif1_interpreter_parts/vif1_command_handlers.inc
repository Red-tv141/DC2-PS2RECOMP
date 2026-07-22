            // G28: latch TOP = current TOPS (the buffer the preceding UNPACK filled) BEFORE
            // toggling the double-buffer flag. The VU microprogram reads its vertex/count input
            // via XTOP (= this TOP). Without it XTOP returned ITOP (0 for the model) so the
            // transform read vertices from VU addr 0 -> garbage loop bound -> runaway to maxcycles.
            vif1_regs.top = vif1_regs.tops & 0x3FFu;
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            uint32_t startPC = (uint32_t)imm * 8u;
            g39_seq("MSCAL", startPC, vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu);
            if (startPC == 0x10u && s_g39Armed.exchange(1u, std::memory_order_relaxed) == 0u)
                s_g39UnpkLog.store(600u, std::memory_order_relaxed);
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_f517Mscal.fetch_add(1u, std::memory_order_relaxed);
                if (n < 48u)
                    std::fprintf(stderr, "[F51.7:mscal] n=%u imm=0x%x startPc=0x%x itop=0x%x cbHooked=%u\n",
                                 n, imm, startPC, vif1_regs.itop, m_vu1MscalCallback ? 1u : 0u);
            }
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscal] idx=" << kickIndex
                                                << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                                << " imm=0x" << imm
                                                << " startPc=0x" << startPC
                                                << " itop=0x" << vif1_regs.itop
                                                << std::dec << std::endl);
            }
            g124_note_mscal(startPC, "MSCAL"); // [G124] interleave marker (pre-kick)
            if (g136Active)
            {
                std::fprintf(stderr,
                             "[G136:mscal] seq=%u cmdOff=0x%x startPc=0x%x imm=0x%x itop=0x%x top=0x%x tops=0x%x cb=%u\n",
                             g136Seq, pos - 4u, startPC, imm, vif1_regs.itop & 0x3FFu,
                             vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu,
                             m_vu1MscalCallback ? 1u : 0u);
            }
            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, vif1_regs.itop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            g317_note_gif_fence(kG317VifMscnt);
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_f517Mscnt.fetch_add(1u, std::memory_order_relaxed);
                if (n < 16u)
                    std::fprintf(stderr, "[F51.7:mscnt] n=%u itop=0x%x cbHooked=%u\n",
                                 n, vif1_regs.itop, m_vu1MscntCallback ? 1u : 0u);
            }
            vif1_regs.top = vif1_regs.tops & 0x3FFu; // G28: latch TOP before DBF toggle (XTOP source)
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscnt] idx=" << kickIndex
                                                << " itop=0x" << std::hex << vif1_regs.itop
                                                << " pc=resume"
                                                << std::dec << std::endl);
            }
            g124_note_mscal(0xFFFFu, "MSCNT"); // [G124] interleave marker (pre-kick)
            if (g136Active)
            {
                std::fprintf(stderr,
                             "[G136:mscnt] seq=%u cmdOff=0x%x itop=0x%x top=0x%x tops=0x%x cb=%u\n",
                             g136Seq, pos - 4u, vif1_regs.itop & 0x3FFu,
                             vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu,
                             m_vu1MscntCallback ? 1u : 0u);
            }
            if (m_vu1MscntCallback)
                m_vu1MscntCallback(vif1_regs.itop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            if (vu1_trace_enabled())
                s_f517Mpg.fetch_add(1u, std::memory_order_relaxed);
            uint32_t destAddr = (uint32_t)imm * 8u;
            if (destAddr == kDc2ResidentVu1Pc)
                ++g22Mpg320Uploads;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            const uint32_t availableMpgBytes = (pos < sizeBytes) ? std::min<uint32_t>(mpgBytes, sizeBytes - pos) : 0u;
            const G22XgkickScan xgkickScan = g22_scan_xgkick_ops(data + pos, availableMpgBytes, destAddr);
            if (vu1_trace_enabled() && (destAddr == kDc2ResidentVu1Pc || xgkickScan.count != 0u))
            {
                const uint32_t traceIndex = s_g22MpgTraceCount.fetch_add(1u, std::memory_order_relaxed);
                if (traceIndex < 64u)
                {
                    uint32_t firstLower = 0u;
                    uint32_t firstUpper = 0u;
                    if (availableMpgBytes >= 8u)
                    {
                        std::memcpy(&firstLower, data + pos, sizeof(firstLower));
                        std::memcpy(&firstUpper, data + pos + 4u, sizeof(firstUpper));
                    }
                    std::fprintf(stderr,
                                 "[G22:mpg] n=%u streamSize=%u cmdPos=0x%x dest=0x%x num=%u instr=%u bytes=%u avail=%u xgkick=%u firstPc=0x%x firstIs=%u firstLower=0x%08x firstUpper=0x%08x headLower=0x%08x headUpper=0x%08x\n",
                                 traceIndex, sizeBytes, g17LastOpcodePos, destAddr,
                                 static_cast<uint32_t>(num), instructionCount, mpgBytes, availableMpgBytes,
                                 xgkickScan.count, xgkickScan.firstPc, xgkickScan.firstIs,
                                 xgkickScan.firstLower, xgkickScan.firstUpper,
                                 firstLower, firstUpper);
                }
            }
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    g297NoteVuWrite(true, destAddr, data + pos, copyBytes); // G301 MTVU ordered event
                }
            }
            if (g22KickMode == G22Vu1KickMode::Upload &&
                xgkickScan.count != 0u)
            {
                const uint32_t startPc = g22_vu1_start_pc_override(destAddr);
                g22KickResidentVu1("mpg-upload", g17LastOpcodePos, opcode, instructionCount, destAddr, startPc);
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            ++g22DirectCommands;
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (vu1_trace_enabled() && imm >= 0x100u && sizeBytes > 0x10000u)
            {
                static std::atomic<uint32_t> s_g17big{0};
                if (s_g17big.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    std::fprintf(stderr, "[G17:bigdirect] cmdPos=0x%x imm=0x%04x pending=%u gifdata@0x%x:",
                                 pos - 4u, imm, m_vif1PendingPath2ImageQwc, pos);
                    for (uint32_t k = 0; k < 16u && pos + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + pos + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                const uint32_t originalQwCount = qwCount;
                if (g136Active && g136LoggedPayloads < g136_command_limit())
                {
                    ++g136LoggedPayloads;
                    uint64_t gifLo = 0u;
                    uint64_t gifHi = 0u;
                    if (pos + 8u <= sizeBytes)
                        std::memcpy(&gifLo, data + pos, sizeof(gifLo));
                    if (pos + 16u <= sizeBytes)
                        std::memcpy(&gifHi, data + pos + 8u, sizeof(gifHi));
                    const uint32_t nloop = static_cast<uint32_t>(gifLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((gifLo >> 58u) & 0x3u);
                    const uint32_t nreg = static_cast<uint32_t>((gifLo >> 60u) & 0xFu);
                    const uint32_t prim = static_cast<uint32_t>((gifLo >> 47u) & 0x7FFu);
                    const uint32_t pre = static_cast<uint32_t>((gifLo >> 46u) & 0x1u);
                    std::fprintf(stderr,
                                 "[G136:direct] seq=%u cmdOff=0x%x payloadOff=0x%x qwc=%u imm=0x%04x hl=%u truncated=%u pendingImage=%u gif(nloop=%u flg=%u nreg=%u pre=%u prim=0x%x regs=0x%016llx)\n",
                                 g136Seq, pos - 4u, pos, qwCount, imm, directHl ? 1u : 0u,
                                 truncated ? 1u : 0u, m_vif1PendingPath2ImageQwc,
                                 nloop, flg, nreg, pre, prim,
                                 static_cast<unsigned long long>(gifHi));
                    g136_log_qwords("directqw", g136Seq, pos, data + pos,
                                    (sizeBytes > pos) ? (sizeBytes - pos) : 0u, qwCount);
                }
                // PHASE F30: path2submit probe, retained only for explicit phase tracing.
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f30Path2{0};
                    const uint32_t n = s_f30Path2.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (n <= 12u || (n % 64u) == 0u)
                    {
                        uint64_t firstDw = 0u;
                        std::memcpy(&firstDw, data + pos, sizeof(firstDw));
                        std::fprintf(stderr,
                                     "[F30:path2submit] n=%u qwc=%u hl=%u firstDw=0x%016llx\n",
                                     n, qwCount, directHl ? 1u : 0u,
                                     static_cast<unsigned long long>(firstDw));
                    }
                }
                if (f50_12_trace_enabled())
                {
                    static std::atomic<uint32_t> s_f512VifDirect{0};
                    const uint32_t n = s_f512VifDirect.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 192u || (n % 1024u) == 0u)
                    {
                        uint64_t gifLo = 0u;
                        uint64_t gifHi = 0u;
                        if (pos + 8u <= sizeBytes)
                            std::memcpy(&gifLo, data + pos, sizeof(gifLo));
                        if (pos + 16u <= sizeBytes)
                            std::memcpy(&gifHi, data + pos + 8u, sizeof(gifHi));
                        std::fprintf(stderr,
                                     "[F512:vifdirect] n=%u streamSize=%u cmdOff=0x%x payloadOff=0x%x qwc=%u availQw=%u hl=%u truncated=%u pendingImage=%u gifLo=0x%016llx gifHi=0x%016llx\n",
                                     n, sizeBytes, pos - 4u, pos, qwCount, availableQw,
                                     directHl ? 1u : 0u, truncated ? 1u : 0u,
                                     m_vif1PendingPath2ImageQwc,
                                     static_cast<unsigned long long>(gifLo),
                                     static_cast<unsigned long long>(gifHi));
                    }
                }

                uint32_t payloadPos = pos;
                uint32_t payloadQw = qwCount;
                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, payloadQw);
                    submitPath2ImageChunk(data + payloadPos, chunkQw, m_vif1PendingPath2DirectHl);
                    payloadPos += chunkQw * 16u;
                    payloadQw -= chunkQw;
                    m_vif1PendingPath2ImageQwc -= chunkQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                    {
                        m_vif1PendingPath2DirectHl = false;
                    }
                }

                if (payloadQw > 0u)
                {
                    submitGifPacket(GifPathId::Path2, data + payloadPos, payloadQw * 16u, !g95_defer_gif_enabled(), directHl);

                    const uint32_t imageQw = gifImageQwcFromTag(data + payloadPos, payloadQw * 16u);
                    if (imageQw != 0u)
                    {
                        const uint32_t inlineImageQw = (payloadQw > 0u) ? (payloadQw - 1u) : 0u;
                        if (imageQw > inlineImageQw)
                        {
                            if (vu1_trace_enabled() && sizeBytes > 0x10000u)
                            {
                                static std::atomic<uint32_t> s_g17setp{0};
                                if (s_g17setp.fetch_add(1u, std::memory_order_relaxed) < 8u)
                                {
                                    uint64_t glo = 0u, ghi = 0u;
                                    std::memcpy(&glo, data + payloadPos, 8);
                                    if (payloadQw >= 1u) std::memcpy(&ghi, data + payloadPos + 8u, 8);
                                    const uint8_t flg = static_cast<uint8_t>((glo >> 58) & 0x3u);
                                    const uint8_t nreg = static_cast<uint8_t>((glo >> 60) & 0xFu);
                                    std::fprintf(stderr,
                                                 "[G17:setpending] cmdPos=0x%x payloadQw=%u imageQw=%u flg=%u nreg=%u gifLo=0x%016llx gifHi=0x%016llx newPending=%u\n",
                                                 g17LastOpcodePos, payloadQw, imageQw, flg, nreg,
                                                 static_cast<unsigned long long>(glo),
                                                 static_cast<unsigned long long>(ghi),
                                                 imageQw - inlineImageQw);
                                }
                            }
                            m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                            m_vif1PendingPath2DirectHl = directHl;
                        }
                    }
                }
                qwCount = originalQwCount;
            }

            pos += qwCount * 16;
            if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscntOff && g17FirstMscntOff < pos)
            {
                static std::atomic<uint32_t> s_g17dir{0};
                if (s_g17dir.fetch_add(1u, std::memory_order_relaxed) < 4u)
                {
                    std::fprintf(stderr,
                                 "[G17:swallow] DIRECT cmdPos=0x%x imm=0x%04x qwc=%u span=[0x%x,0x%x) mscntOff=0x%x hl=%u\n",
                                 g17LastOpcodePos, imm, qwCount, g17LastOpcodePos, pos,
                                 g17FirstMscntOff, (opcode == VIF_DIRECTHL) ? 1u : 0u);
                    // dump the DIRECT cmd word + first GIFtag (16 words from cmdPos)
                    std::fprintf(stderr, "[G17:dump@cmd]");
                    for (uint32_t k = 0; k < 16u && g17LastOpcodePos + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + g17LastOpcodePos + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump 12 words around the MSCNT site
                    const uint32_t base = (g17FirstMscntOff >= 16u) ? (g17FirstMscntOff - 16u) : 0u;
                    std::fprintf(stderr, "[G17:dump@mscnt base=0x%x]", base);
                    for (uint32_t k = 0; k < 12u && base + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + base + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump the head of the stream (should be clean STCYCL/MPG/UNPACK setup)
                    std::fprintf(stderr, "[G17:head]");
                    for (uint32_t k = 0; k < 48u && k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump the ring of preceding commands (oldest→newest)
                    std::fprintf(stderr, "[G17:ring]\n");
                    for (uint32_t k = 0; k < kG17Ring; ++k)
                    {
                        const uint32_t ri = (g17RingIdx + k) % kG17Ring;
                        std::fprintf(stderr, "   pos=0x%06x cmd=0x%08x op=0x%02x imm=0x%04x consumed=%u\n",
                                     g17RingPos[ri], g17RingCmd[ri],
                                     static_cast<uint32_t>((g17RingCmd[ri] >> 24) & 0x7Fu),
                                     g17RingCmd[ri] & 0xFFFFu, g17RingConsumed[ri]);
                    }
                }
            }
            // G25: the model kick is MSCAL — detect a DIRECT whose span swallows it.
            if (vu1_trace_enabled() && g17FirstMscalOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscalOff && g17FirstMscalOff < pos)
            {
                static std::atomic<uint32_t> s_g25dir{0};
                if (s_g25dir.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    uint32_t mw = 0u; std::memcpy(&mw, data + g17FirstMscalOff, 4);
                    std::fprintf(stderr,
                                 "[G25:mscalswallow] DIRECT cmdPos=0x%x imm=0x%04x qwc=%u span=[0x%x,0x%x) mscalOff=0x%x mscalWord=0x%08x size=%u hl=%u\n",
                                 g17LastOpcodePos, imm, qwCount, g17LastOpcodePos, pos,
                                 g17FirstMscalOff, mw, sizeBytes, (opcode == VIF_DIRECTHL) ? 1u : 0u);
                    // Bytes leading INTO the swallowing DIRECT command (is it a real cmd or
                    // a data word the parser landed on after an upstream desync?).
                    const uint32_t base2 = (g17LastOpcodePos >= 64u) ? (g17LastOpcodePos - 64u) : 0u;
                    std::fprintf(stderr, "[G25:cmdctx base=0x%x]", base2);
                    for (uint32_t k = 0; k < 20u && base2 + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + base2 + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    // The 16-command ring leading up to the swallow (oldest -> newest): the
                    // op/consumed sequence shows where the parser sized a command wrong.
                    std::fprintf(stderr, "\n[G25:ring]\n");
                    for (uint32_t k = 0; k < kG17Ring; ++k)
                    {
                        const uint32_t ri = (g17RingIdx + k) % kG17Ring;
                        std::fprintf(stderr, "   pos=0x%06x cmd=0x%08x op=0x%02x imm=0x%04x consumed=%u\n",
                                     g17RingPos[ri], g17RingCmd[ri],
                                     static_cast<uint32_t>((g17RingCmd[ri] >> 24) & 0x7Fu),
                                     g17RingCmd[ri] & 0xFFFFu, g17RingConsumed[ri]);
                    }
                }
            }
            if (truncated)
            {
                g17BreakReason = "direct-truncated";
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            if (vu1_trace_enabled())
                s_f517Unpack.fetch_add(1u, std::memory_order_relaxed);
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
