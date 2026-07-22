void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (!m_rdram || !m_gsVRAM || sizeBytes == 0u)
        return;
    if (srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    G146PerfScope g146Scope(g_g146Vif1Ns);

    if (!data || !m_gsVRAM || sizeBytes == 0u)
        return;

    g124_note_stream(sizeBytes); // [G124] DMA-transfer boundary in the ordered interleave log
    const uint32_t g136Seq = g136_trace_enabled()
        ? g_dc2G136TraceActive.load(std::memory_order_relaxed)
        : 0u;
    const bool g136Active = g136Seq != 0u;
    uint32_t g136LoggedCommands = 0u;
    uint32_t g136LoggedPayloads = 0u;
    if (g136Active)
    {
        std::fprintf(stderr, "[G136:vifstart] seq=%u size=%u\n", g136Seq, sizeBytes);
        g136_log_qwords("vifhead", g136Seq, 0u, data, sizeBytes, g136_qword_limit());
        if (g137_trace_enabled())
            g137_clear_origins();
    }

    auto g95DrainDeferredGif = [&](const char *reason)
    {
        if (!g95_defer_gif_enabled() || !m_gifArbiter)
            return;
        if (g95_trace_enabled())
        {
            static std::atomic<uint32_t> s_g95Drain{0};
            const uint32_t n = s_g95Drain.fetch_add(1u, std::memory_order_relaxed);
            if (n < 128u || (n % 1024u) == 0u)
                std::fprintf(stderr, "[G95:gifdrain] n=%u reason=%s\n", n, reason);
        }
        m_gifArbiter->drain();
    };

    // G17: first clean-MSCNT offset in this stream (filled by the trace scan, used to log the
    // parser's command boundaries around the kick so we can see what consumes it).
    uint32_t g17FirstMscntOff = 0xFFFFFFFFu;
    // G25: first clean-MSCAL offset (the costume/character model kick is MSCAL, not MSCNT).
    uint32_t g17FirstMscalOff = 0xFFFFFFFFu;

    if (vu1_trace_enabled())
    {
        const uint32_t n = s_f517Vif1Calls.fetch_add(1u, std::memory_order_relaxed);
        // Crude raw scan: count 4-byte words whose top opcode byte is MSCAL/MSCALF.
        // Over-counts (payload bytes may match) but a 0 means MSCAL is nowhere in the stream.
        for (uint32_t p = 0; p + 4u <= sizeBytes; p += 4u)
        {
            uint32_t w = 0u;
            std::memcpy(&w, data + p, 4);
            const uint8_t op = static_cast<uint8_t>((w >> 24) & 0x7Fu);
            if (op == 0x14u || op == 0x15u)
            {
                s_f517RawMscalBytes.fetch_add(1u, std::memory_order_relaxed);
                // G25: a clean MSCAL VIFcode = num field (23:16)==0 and a plausible micro
                // start address (imm < 0x800 instructions). The model's per-draw kick.
                if (g17FirstMscalOff == 0xFFFFFFFFu &&
                    ((w >> 16) & 0xFFu) == 0u && (w & 0xFFFFu) < 0x800u)
                    g17FirstMscalOff = p;
            }
            // A genuine MSCNT VIFcode has num==0 and imm==0 (kick continues resident prog).
            // Match the clean word to avoid payload noise that merely has 0x17 in the top byte.
            if (op == 0x17u && (w & 0x00FFFFFFu) == 0u)
            {
                if (g17FirstMscntOff == 0xFFFFFFFFu)
                    g17FirstMscntOff = p;
                const uint32_t r = s_f517RawMscntBytes.fetch_add(1u, std::memory_order_relaxed);
                if (r < 24u)
                {
                    uint32_t prev = 0u, prev2 = 0u;
                    if (p >= 4u) std::memcpy(&prev, data + p - 4u, 4);
                    if (p >= 8u) std::memcpy(&prev2, data + p - 8u, 4);
                    std::fprintf(stderr, "[G17:rawmscnt] call=%u off=0x%x word=0x%08x prev=0x%08x prev2=0x%08x size=%u\n",
                                 n, p, w, prev, prev2, sizeBytes);
                }
            }
        }
        // G25: announce streams that DO contain a clean MSCAL kick, so we can confirm the
        // offset exists (and whether it is later swallowed / reached).
        if (g17FirstMscalOff != 0xFFFFFFFFu && sizeBytes > 0x1000u)
        {
            static std::atomic<uint32_t> s_g25found{0};
            if (s_g25found.fetch_add(1u, std::memory_order_relaxed) < 8u)
            {
                uint32_t mw = 0u; std::memcpy(&mw, data + g17FirstMscalOff, 4);
                std::fprintf(stderr, "[G25:mscalfound] call=%u mscalOff=0x%x word=0x%08x size=%u\n",
                             n, g17FirstMscalOff, mw, sizeBytes);
            }
        }
        if (n < 64u)
        {
            uint64_t firstQw = 0u;
            if (sizeBytes >= 8u)
                std::memcpy(&firstQw, data, sizeof(firstQw));
            std::fprintf(stderr, "[F51.7:vif1] n=%u size=%u firstQw=0x%016llx\n",
                         n, sizeBytes, static_cast<unsigned long long>(firstQw));
        }
    }

    // PHASE F30: vif1cmd probe, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30Vif1Cmd{0};
        const uint32_t n = s_f30Vif1Cmd.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (n <= 12u || (n % 64u) == 0u)
        {
            uint64_t firstQw = 0u;
            if (sizeBytes >= 8u)
                std::memcpy(&firstQw, data, sizeof(firstQw));
            std::fprintf(stderr,
                         "[F30:vif1cmd] n=%u size=%u firstQw=0x%016llx\n",
                         n, sizeBytes, static_cast<unsigned long long>(firstQw));
        }
    }

    auto recomputeVif1Tops = [&]()
    {
        const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
        const uint32_t base = vif1_regs.base & 0x3FFu;
        const uint32_t ofst = vif1_regs.ofst & 0x3FFu;
        vif1_regs.tops = dbf ? ((base + ofst) & 0x3FFu) : base;
    };

    uint32_t pos = 0;
    // G27: window dump following a model BASE (small imm framing) — logs the exact command
    // sequence after the known-aligned model section-1 so we can see where the cursor desyncs
    // before the section-2 MSCAL. Gated on DC2_TRACE_VU1; capped to 8 windows.
    uint32_t g27Win = 0u;
    static std::atomic<uint32_t> s_g27Starts{0};
    // G17: track the last command parsed + where the loop ultimately stops, so we can tell
    // whether the parser desyncs / breaks early before reaching a MSCNT kick deep in the chain.
    uint8_t g17LastOpcode = 0xFF;
    uint32_t g17LastOpcodePos = 0;
    const char *g17BreakReason = "loop-end";
    // G17: ring buffer of the last commands parsed, dumped when a bogus over-long DIRECT
    // (the symptom of an upstream desync) swallows the kick.
    static constexpr uint32_t kG17Ring = 16u;
    uint32_t g17RingPos[kG17Ring] = {0};
    uint32_t g17RingCmd[kG17Ring] = {0};
    uint32_t g17RingConsumed[kG17Ring] = {0};
    uint32_t g17RingIdx = 0;
    uint32_t g17PrevPosForConsumed = 0;
    const G22Vu1KickMode g22KickMode = g22_vu1_kick_mode();
    uint32_t g22DirectCommands = 0u;
    uint32_t g22V432Unpacks = 0u;
    uint32_t g22LargeUnpacks = 0u;
    uint32_t g22Mpg320Uploads = 0u;
    auto g22KickResidentVu1 = [&](const char *reason, uint32_t cmdPos, uint8_t sourceOpcode,
                                  uint32_t writeCount, uint32_t vuAddr, uint32_t startPc)
    {
        if (g22KickMode == G22Vu1KickMode::Off || !m_vu1MscalCallback ||
            !g22_vu1_program_ready(m_vu1Code))
        {
            return;
        }

        const uint32_t kickIndex = s_g22SynthVu1Kick.fetch_add(1u, std::memory_order_relaxed);
        const bool resumeResident = (g22KickMode == G22Vu1KickMode::Resume) &&
                                    (kickIndex > 0u) &&
                                    static_cast<bool>(m_vu1MscntCallback);
        if (vu1_trace_enabled() && kickIndex < 128u)
        {
            std::fprintf(stderr,
                         "[G22:synthkick] n=%u kind=%s reason=%s size=%u cmdPos=0x%x op=0x%02x count=%u vuAddr=0x%x startPc=0x%x itop=0x%x tops=0x%x base=0x%x ofst=0x%x direct=%u v432=%u large=%u mpg320=%u\n",
                         kickIndex, resumeResident ? "resume" : "execute", reason, sizeBytes, cmdPos, static_cast<uint32_t>(sourceOpcode),
                         writeCount, vuAddr, startPc, vif1_regs.itop & 0x3FFu, vif1_regs.tops & 0x3FFu,
                         vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu,
                         g22DirectCommands, g22V432Unpacks, g22LargeUnpacks, g22Mpg320Uploads);
        }

        if (resumeResident)
            m_vu1MscntCallback(vif1_regs.itop & 0x3FFu);
        else
            m_vu1MscalCallback(startPc, vif1_regs.itop & 0x3FFu);
    };
    auto submitPath2ImageChunk = [&](const uint8_t *payload, uint32_t chunkQw, bool directHl)
    {
        if (!payload || chunkQw == 0u)
            return;

        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31VifImageDirect{0};
            const uint32_t n = s_f31VifImageDirect.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                uint64_t firstDw = 0u;
                std::memcpy(&firstDw, payload, sizeof(firstDw));
                std::fprintf(stderr,
                             "[F31:vif-image-direct] n=%u qwc=%u pendingBefore=%u hl=%u firstDw=0x%016llx\n",
                             n, chunkQw, m_vif1PendingPath2ImageQwc, directHl ? 1u : 0u,
                             static_cast<unsigned long long>(firstDw));
            }
        }

        // The GS already consumed the IMAGE tag that established its pending transfer.
        // A second tag here would be consumed as the first qword of pixel data.
        submitGifPacket(GifPathId::Path2,
                        payload,
                        chunkQw * 16u,
                        !g95_defer_gif_enabled(),
                        directHl);
    };

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            uint32_t nextCmd = 0u;
            std::memcpy(&nextCmd, data + pos, sizeof(nextCmd));
            const uint8_t nextOpcode = static_cast<uint8_t>((nextCmd >> 24) & 0x7Fu);
            uint32_t nextCmd2 = 0u;
            if (pos + 8u <= sizeBytes)
                std::memcpy(&nextCmd2, data + pos + 4u, sizeof(nextCmd2));
            const uint8_t nextOpcode2 = static_cast<uint8_t>((nextCmd2 >> 24) & 0x7Fu);
            const bool nextLooksLikeDirectWrapper =
                nextOpcode == VIF_DIRECT ||
                nextOpcode == VIF_DIRECTHL ||
                (nextOpcode == VIF_NOP && (nextOpcode2 == VIF_DIRECT || nextOpcode2 == VIF_DIRECTHL));
            if (nextLooksLikeDirectWrapper)
            {
                // Some compact chains put the next image chunk behind another VIF DIRECT.
                // Let the command parser see that wrapper instead of treating it as pixels.
                if (vu1_trace_enabled() && sizeBytes > 0x10000u)
                {
                    static std::atomic<uint32_t> s_g17wrap{0};
                    if (s_g17wrap.fetch_add(1u, std::memory_order_relaxed) < 8u)
                        std::fprintf(stderr,
                                     "[G17:wrapbail] pos=0x%x pending=%u nextCmd=0x%08x nextCmd2=0x%08x\n",
                                     pos, m_vif1PendingPath2ImageQwc, nextCmd, nextCmd2);
                }
            }
            else
            {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            submitPath2ImageChunk(data + pos, chunkQw, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
            }
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        g17LastOpcode = opcode;
        g17LastOpcodePos = pos - 4u;
        if (vu1_trace_enabled())
        {
            // backfill the consumed-span of the previous ring entry
            const uint32_t prevIdx = (g17RingIdx + kG17Ring - 1u) % kG17Ring;
            g17RingConsumed[prevIdx] = (pos - 4u) - g17PrevPosForConsumed;
            g17RingPos[g17RingIdx] = pos - 4u;
            g17RingCmd[g17RingIdx] = cmd;
            g17RingConsumed[g17RingIdx] = 0u;
            g17RingIdx = (g17RingIdx + 1u) % kG17Ring;
            g17PrevPosForConsumed = pos - 4u;
        }
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;

        if (g136Active && opcode != VIF_NOP && g136LoggedCommands < g136_command_limit())
        {
            ++g136LoggedCommands;
            std::fprintf(stderr,
                         "[G136:vifcmd] seq=%u off=0x%x op=0x%02x imm=0x%04x num=%u tops=0x%x top=0x%x base=0x%x ofst=0x%x itop=0x%x cycle=0x%04x\n",
                         g136Seq, pos - 4u, static_cast<uint32_t>(opcode), imm,
                         static_cast<uint32_t>(num), vif1_regs.tops & 0x3FFu,
                         vif1_regs.top & 0x3FFu, vif1_regs.base & 0x3FFu,
                         vif1_regs.ofst & 0x3FFu, vif1_regs.itop & 0x3FFu,
                         vif1_regs.cycle & 0xFFFFu);
        }

        if (vu1_trace_enabled() && g27Win > 0u)
        {
            std::fprintf(stderr, "[G27] pos=0x%x op=0x%02x imm=0x%04x num=%u cl=%u wl=%u\n",
                         pos - 4u, static_cast<uint32_t>(opcode), imm,
                         static_cast<uint32_t>(num),
                         vif1_regs.cycle & 0xFFu, (vif1_regs.cycle >> 8) & 0xFFu);
            --g27Win;
        }

        if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu)
        {
            const uint32_t cmdPos = pos - 4u;
            if (cmdPos + 80u >= g17FirstMscntOff && cmdPos <= g17FirstMscntOff + 16u)
            {
                static std::atomic<uint32_t> s_g17win{0};
                const uint32_t w = s_g17win.fetch_add(1u, std::memory_order_relaxed);
                if (w < 80u)
                    std::fprintf(stderr,
                                 "[G17:win] cmdPos=0x%x op=0x%02x imm=0x%04x num=%u (mscntOff=0x%x)\n",
                                 cmdPos, static_cast<uint32_t>(opcode), imm,
                                 static_cast<uint32_t>(num), g17FirstMscntOff);
            }
        }
        const bool irq = (cmd & 0x80000000u) != 0u;
        if (vu1_trace_enabled())
        {
            s_f517Ops.fetch_add(1u, std::memory_order_relaxed);
            if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
                s_f517Direct.fetch_add(1u, std::memory_order_relaxed);
        }

        const uint32_t opcodeIndex = s_debugVif1OpcodeCount.fetch_add(1, std::memory_order_relaxed);
        if (opcodeIndex < 160u)
        {
            RUNTIME_LOG("[vif1:cmd] idx=" << opcodeIndex
                                          << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                          << " imm=0x" << imm
                                          << std::dec
                                          << " num=" << static_cast<uint32_t>(num)
                                          << " irq=" << static_cast<uint32_t>(irq ? 1u : 0u)
                                          << std::endl);
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // HW/PCSX2: OFFSET sets OFST=imm, clears DBF, and resets TOPS=BASE. It does NOT
            // modify BASE. The old code set base=oldTops, which made the double-buffer TOPS
            // MARCH upward each mesh (0x3c->0xf0->0x1a4...) instead of ping-ponging, so the
            // model's VU input buffer eventually collided with its XGKICK output buffer
            // (0x1a4 qw == 0x1a40 byte) -> the transform read its own giftags -> -inf verts. (G28)
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            recomputeVif1Tops();          // tops = base (DBF now 0)
            g39_seq("OFST", imm & 0x3FFu, vif1_regs.base & 0x3FFu, vif1_regs.tops & 0x3FFu);
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g25ofs{0};
                if (s_g25ofs.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr, "[G25:base/ofst] OFFSET imm=0x%x base=0x%x size=%u\n",
                                 imm & 0x3FFu, vif1_regs.base & 0x3FFu, sizeBytes);
            }
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            vif1_regs.base = imm & 0x3FFu;
            recomputeVif1Tops();
            if (vu1_trace_enabled() && (imm & 0x3FFu) == 0x3cu &&
                s_g39SeqArmed.load(std::memory_order_relaxed) == 0)
            {
                s_g39SeqN.store(0u, std::memory_order_relaxed);
                s_g39SeqArmed.store(1, std::memory_order_relaxed);
            }
            g39_seq("BASE", imm & 0x3FFu, 0u, 0u);
            if (vu1_trace_enabled() && g27Win == 0u &&
                (imm & 0x3FFu) != 0u && (imm & 0x3FFu) < 0x100u)
            {
                const uint32_t s = s_g27Starts.fetch_add(1u, std::memory_order_relaxed);
                if (s < 8u)
                {
                    g27Win = 48u;
                    std::fprintf(stderr, "[G27:start] BASE imm=0x%x pos=0x%x size=%u\n",
                                 imm & 0x3FFu, pos - 4u, sizeBytes);
                }
            }
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g25base{0};
                if (s_g25base.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr, "[G25:base/ofst] BASE imm=0x%x size=%u\n",
                                 imm & 0x3FFu, sizeBytes);
            }
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif1_regs.itop = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            if (opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
            {
                g317_note_gif_fence((opcode == VIF_FLUSH) ? kG317VifFlush : kG317VifFlusha);
                g95DrainDeferredGif((opcode == VIF_FLUSH) ? "FLUSH" : "FLUSHA");
            }
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            g317_note_gif_fence((opcode == VIF_MSCAL) ? kG317VifMscal : kG317VifMscalf);
            if (opcode == VIF_MSCALF)
                g95DrainDeferredGif("MSCALF-pre");
