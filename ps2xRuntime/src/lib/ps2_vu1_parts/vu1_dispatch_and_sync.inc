void VU1Interpreter::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    (void)upperInstr;
    if (instr == 0x00000000 || instr == 0x8000033C) // NOP
        return;

    uint8_t opHi = (instr >> 25) & 0x7F;

    // G133: copy-packer behind-camera SKIP. The copy packer (0x1b68) emits the wall tristrips with
    // ADC = the FOG clamp (structurally 0, G132), so it DRAWS the behind-camera spanning triangles HW
    // SKIPS (G133 drawn-vs-skipped extent A/B: HW draws only <1024px tris, skips the >512px/full-screen
    // ones). The copy input VF16 = (screenX, screenY, screenZ, 1/W) carries the q sign — VF16.w<0 =
    // behind camera (DC2_G132_COPYW range VF16.w=[-0.0034,128]). Capture the kicking vert's 1/W at the
    // copy loop's `MOVE VF26.xyz<-VF16` (pc 0x1c10), and at the XYZF2 store (pc 0x1c28) set the ADC bit
    // (word3 bit15) when 1/W<=0 so the GS skips the triangle ending at that vert = HW's near-plane
    // strip-restart, on the NATURAL copy route (vs the G100 forced 0x1ff0). 1-iter software-pipeline:
    // the store at 0x1c28 emits the VF26 set at the PREVIOUS 0x1c10, so consume the PREVIOUS stash;
    // the pipeline-edge first verts are strip primers (complete no triangle) so an off-by-one there is
    // harmless. Title-rock-scoped; opt-in DC2_G133_COPY_QSKIP (default off) to PROVE the diagnosis.
    static const bool s_g133qskip = (std::getenv("DC2_G133_COPY_QSKIP") != nullptr);
    static float s_g133qPrev = 1.0f, s_g133qCur = 1.0f;
    static std::atomic<uint64_t> s_g133skipN{0}, s_g133keepN{0};
    if (s_g133qskip && m_state.pc == 0x1c10u &&
        g_dc2TitleRockScope.load(std::memory_order_relaxed))
    {
        s_g133qPrev = s_g133qCur;
        s_g133qCur = m_state.vf[16][3];
    }

    // G32: watch the GIF-packing loop (model packs ST/RGBAQ/XYZF2 into the PATH1 output
    // buffer here). G31 showed varied transformed verts (LQI) collapse to an identical
    // packed XYZF2. Log every integer/store op in the packing PC window with the VI
    // pointer/value state so we can see whether the output pointer fails to advance or the
    // packed value is constant. Capped + env-gated (DC2_TRACE_G32).
    if (g32_vu_trace_enabled() && m_state.pc >= 0x1b00u && m_state.pc <= 0x1c80u)
    {
        const uint32_t n = s_g32Count.fetch_add(1u, std::memory_order_relaxed);
        if (n < 20000u)
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dst = (instr >> 21) & 0xF;
            const uint8_t f6 = instr & 0x3F;
            const bool isSpecial2 = (opHi == 0x40u) && (f6 >= 0x3Cu);
            const uint8_t s2 = isSpecial2 ? S2_FUNC(instr) : 0xFFu;
            // SQI(0x35): store VF[is] -> (VI[it]++). Dump the VALUE being packed + dest addr.
            if (s2 == 0x35u && m_state.pc == 0x1c28u) // the XYZF2 store
            {
                int32_t ix, iy, iz;
                std::memcpy(&ix, &m_state.vf[is][0], 4);
                std::memcpy(&iy, &m_state.vf[is][1], 4);
                std::memcpy(&iz, &m_state.vf[is][2], 4);
                std::fprintf(stderr, "[G32:xyz] n=%u px=(%.2f,%.2f) z=%d ix=%d iy=%d\n",
                             n, ix / 16.0f, iy / 16.0f, iz, ix, iy);
            }
            else if (s2 == 0x34u) // LQI: load VF[it] <- (VI[is]++)
            {
                const uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                std::fprintf(stderr, "[G32:lqi] n=%u pc=0x%x LQI VF%u <- addr=0x%x (VI%u)\n",
                             n, m_state.pc, (uint32_t)it, addr, (uint32_t)is);
            }
            else if (s2 == 0x3Fu) // ISWR
            {
                const uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                std::fprintf(stderr, "[G32:iswr] n=%u pc=0x%x ISWR dst=0x%x VI%u=0x%x -> addr=0x%x (VI%u)\n",
                             n, m_state.pc, (uint32_t)dst, (uint32_t)it,
                             (uint32_t)(uint16_t)m_state.vi[it], addr, (uint32_t)is);
            }
        }
    }

    // G39: trace the loads / top reads that feed the model transform so the vertex / matrix /
    // translation source address is visible across consecutive kicks (n=1 good, n=3+ bad).
    if (s_g39LoadTrace.load(std::memory_order_relaxed) > 0)
    {
        const uint8_t it = LIT(instr);
        const uint8_t is = LIS(instr);
        const uint8_t funct = instr & 0x3Fu;
        const uint8_t s2 = (opHi == 0x40u && funct >= 0x3Cu) ? S2_FUNC(instr) : 0xFFu;
        const char *name = nullptr;
        uint32_t addr = 0xFFFFFFFFu;
        float fv[4] = {0, 0, 0, 0};
        int32_t iv = 0;
        bool haveQuad = false, haveInt = false;
        auto readQuad = [&](uint32_t a)
        { a &= (dataSize - 1u); if (a + 16u <= dataSize) { std::memcpy(fv, vuData + a, 16); haveQuad = true; } addr = a; };
        if (opHi == 0x00u) { name = "LQ"; readQuad(((uint32_t)(int32_t)(m_state.vi[is] + IMM11(instr))) * 16u); }
        else if (opHi == 0x04u) { name = "ILW"; readQuad(((uint32_t)(int32_t)(m_state.vi[is] + IMM11(instr))) * 16u); }
        else if (s2 == 0x34u) { name = "LQI"; readQuad(((uint32_t)(uint16_t)m_state.vi[is]) * 16u); }
        else if (s2 == 0x36u) { name = "LQD"; readQuad(((uint32_t)(uint16_t)(m_state.vi[is] - 1)) * 16u); }
        else if (s2 == 0x3Eu) { name = "ILWR"; readQuad(((uint32_t)(uint16_t)m_state.vi[is]) * 16u); }
        else if (s2 == 0x3Cu) { name = "MTIR"; std::memcpy(&iv, &m_state.vf[is][(instr >> 21) & 0x3], 4); haveInt = true; }
        else if (s2 == 0x68u) { name = "XTOP"; iv = (int32_t)(memory ? (memory->vif1_regs.top & 0x3FFu) : 0u); haveInt = true; }
        else if (s2 == 0x69u) { name = "XITOP"; iv = (int32_t)(m_state.itop & 0x3FFu); haveInt = true; }
        if (name)
        {
            s_g39LoadTrace.fetch_sub(1, std::memory_order_relaxed);
            if (haveQuad)
                std::fprintf(stderr, "[G39:ld] k=%u pc=0x%x %s VF%u<-addr=0x%x (VI%u=0x%x) = % .4g % .4g % .4g % .4g\n",
                             s_g39KickIdx.load(std::memory_order_relaxed), m_state.pc, name, (unsigned)it,
                             addr, (unsigned)is, (unsigned)(uint16_t)m_state.vi[is], fv[0], fv[1], fv[2], fv[3]);
            else if (haveInt)
                std::fprintf(stderr, "[G39:ld] k=%u pc=0x%x %s VI%u<-0x%x (VI%u=0x%x)\n",
                             s_g39KickIdx.load(std::memory_order_relaxed), m_state.pc, name, (unsigned)it,
                             (unsigned)iv, (unsigned)is, (unsigned)(uint16_t)m_state.vi[is]);
        }
    }

    // The lower instruction encoding uses bits 31:25 for the primary opcode
    switch (opHi)
    {
    case 0x00: // LQ (Load Quadword from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(m_state.vf[it], tmp, dest);
        }
        return;
    }
    case 0x01: // SQ (Store Quadword to VU data memory)
    {
        uint8_t is = LIS(instr);
        uint8_t it = LIT(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[it] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            if (dest & 0x8)
                tmp[0] = m_state.vf[is][0];
            if (dest & 0x4)
                tmp[1] = m_state.vf[is][1];
            if (dest & 0x2)
                tmp[2] = m_state.vf[is][2];
            if (dest & 0x1)
                tmp[3] = m_state.vf[is][3];
            std::memcpy(vuData + addr, tmp, 16);
        }
        return;
    }
    case 0x04: // ILW (Integer Load Word from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            int comp = 0;
            if (dest & 0x8)
                comp = 0;
            else if (dest & 0x4)
                comp = 1;
            else if (dest & 0x2)
                comp = 2;
            else
                comp = 3;
            uint32_t v;
            std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (it != 0)
                m_state.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return;
    }
    case 0x05: // ISW (Integer Store Word to VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t val = (uint32_t)(uint16_t)(m_state.vi[it] & 0xFFFF);
            if (dest & 0x8)
                std::memcpy(vuData + addr + 0, &val, 4);
            if (dest & 0x4)
                std::memcpy(vuData + addr + 4, &val, 4);
            if (dest & 0x2)
                std::memcpy(vuData + addr + 8, &val, 4);
            if (dest & 0x1)
                std::memcpy(vuData + addr + 12, &val, 4);
        }
        return;
    }
    case 0x08: // IADDIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] + imm);
        return;
    }
    case 0x09: // ISUBIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] - imm);
        return;
    }
    case 0x10: // FCEQ
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & 0xFFFFFF) == imm24) ? 1 : 0;
        return;
    }
    case 0x11: // FCSET
    {
        m_state.clip = instr & 0xFFFFFF;
        return;
    }
    case 0x12: // FCAND
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & imm24) != 0) ? 1 : 0;
        return;
    }
    case 0x13: // FCOR
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip | imm24) == 0xFFFFFF) ? 1 : 0;
        return;
    }
    case 0x14: // FSEQ
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = ((statusRead & 0xFFF) == imm12) ? 1 : 0;
        return;
    }
    case 0x15: // FSSET
    {
        m_state.status = (instr >> 6) & 0xFC0;
        return;
    }
    case 0x16: // FSAND
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = (int32_t)(statusRead & imm12);
        return;
    }
    case 0x17: // FSOR
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = ((statusRead | imm12) == 0xFFF) ? 1 : 0;
        return;
    }
    // G138: the MAC-flag lower opcodes were mis-mapped vs the real VU lower-opcode table
    // (PCSX2 VUops.cpp _LOWER_OPCODE[128]): 0x18=FMEQ, 0x1A=FMAND, 0x1B=FMOR, 0x1C=FCGET.
    // The old runner had 0x18=FMAND / 0x1A=FMEQ (SWAPPED) and FMOR parked on 0x1C. Every
    // "FMEQ" in the G70..G137 title-gate disassemblies was therefore really FMAND: the
    // transform packers' draw gate is an FMAND mask cascade (VI = mac & 0xD0 accumulated
    // over the guard-plane SUBs, winding bit 0x20 IORed in, IBEQ against 0xD0|qw30 = draw
    // iff inside-guard AND front-facing) — impossible under FMEQ's 0/1 results, which is
    // why the gate never fired and the whole cavern emitted ADC=1 (blue void / G100 band-
    // aid). Fix is global (all VU1 programs decoded through the swapped slots). Kill-switch
    // DC2_VU1_NO_FMSWAPFIX restores the old mapping for A/B.
    case 0x18: // real VU: FMEQ (old runner: FMAND)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (it != 0)
        {
            if (s_fmFixed)
                m_state.vi[it] = ((macRead & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
            else
                m_state.vi[it] = (int32_t)(macRead & (uint32_t)(uint16_t)m_state.vi[is]);
        }
        return;
    }
    case 0x1A: // real VU: FMAND (old runner: FMEQ)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (it != 0)
        {
            if (s_fmFixed)
                m_state.vi[it] = (int32_t)(macRead & (uint32_t)(uint16_t)m_state.vi[is]);
            else
                m_state.vi[it] = ((macRead & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
        }
        return;
    }
    case 0x1B: // real VU: FMOR (old runner: unmapped)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (s_fmFixed && it != 0)
            m_state.vi[it] = (int32_t)(macRead | (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    case 0x1C: // real VU: FCGET (old runner: FMOR)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        if (s_fmFixed)
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(m_state.clip & 0xFFFu);
        }
        else
        {
            const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                     : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
            if (it != 0)
                m_state.vi[it] = (int32_t)(macRead | (uint32_t)(uint16_t)m_state.vi[is]);
        }
        return;
    }
    case 0x20: // B (unconditional branch)
    {
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        // The run loop detects this target and applies it after the delay slot.
        m_state.pc = target - 8;
        return;
    }
    case 0x21: // BAL (Branch and link)
    {
        uint8_t it = LIT(instr);
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.pc = target - 8;
        return;
    }
    case 0x24: // JR
    {
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & 0x3FFF;
        m_state.pc = target - 8;
        return;
    }
    case 0x25: // JALR
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & 0x3FFF;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.pc = target - 8;
        return;
    }
    case 0x28: // IBEQ
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] == (int16_t)m_state.vi[it])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x29: // IBNE
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] != (int16_t)m_state.vi[it])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2C: // IBLTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] < 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2D: // IBGTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] > 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2E: // IBLEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] <= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2F: // IBGEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] >= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }

    case 0x40: // Lower special (opcode in bits 5:0)
    {
        uint8_t funct = instr & 0x3F;
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t id = LID(instr);
        uint8_t dest = (instr >> 21) & 0xF;

        switch (funct)
        {
        case 0x30: // IADD
            if (id != 0)
                m_state.vi[id] = (int16_t)(m_state.vi[is] + m_state.vi[it]);
            return;
        case 0x31: // ISUB
            if (id != 0)
                m_state.vi[id] = (int16_t)(m_state.vi[is] - m_state.vi[it]);
            return;
        case 0x32: // IADDI
        {
            int16_t imm5 = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27);
            if (it != 0)
                m_state.vi[it] = (int16_t)(m_state.vi[is] + imm5);
            return;
        }
        case 0x34: // IAND
        {
            // G64: the title-map rock batches gate on IAND VI4,VI1,VI5 (high enable
            // 0x80/0x40/0x200/0x100) @0x30d8 and IAND VI4,VI1,VI6 (low enable 2/1/8/4)
            // @0x30f8/0x3168, inside the per-strip emit subroutine 0x3088. VI1 = the
            // per-mesh batch-enable flag from the VIF-unpacked control header
            // (ILWR.z VI1,(VI14) @0x2fc8) -- NOT qw0x26 (that gates a different post-kick
            // op @0x2e98). If VI1 lacks the high bits the full tristrip emit is skipped.
            // G64: repair the title/costume MAP VU1 program's per-mesh prim-type ENABLE flag.
            // The map render program emits geometry in 6 flag-gated batches via the emit
            // subroutine at 0x3088: `IAND VI4,VI1,VI5` @0x30d8 (high enable, VI5 masks
            // 0x40..0x800) and `IAND VI4,VI1,VI6` @0x30f8/0x3168 (low enable, VI6 2..0x20).
            // VI1 = the per-mesh enable mask (loaded by the header loader from the VIF-unpacked
            // control header). Headless it is 0 for EVERY mesh -> every IAND is 0 -> all batches
            // gated off -> only the degenerate trifan/flush path reaches the GS (the green swirl;
            // the rock tristrips/tris vanish). Repair: at each gate, OR the batch's own tested
            // mask (vi[it]) into VI1 so the batch passes its gate. The per-batch COUNT word VI11
            // (`IBEQ VI11,VI0` preamble check) still skips empty batches, so this only un-gates
            // the tristrip/tri batches the EE packet already carries (G62: 52 tristrip + 10 tri
            // templates present) -- it cannot synthesise geometry. Scoped to the 3 exact gate PCs
            // inside the map emit subroutine. Kill: DC2_G64_NO_ENABLE_FIX. Diagnostic (pre-repair
            // VI1): DC2_G63_GATE -> [G63:gate].
            // G140 (2026-07-06): G64 RETIRED BY DEFAULT — the premise was wrong. 0x3088 is NOT a
            // "per-mesh enable gate": it is the polygon CLIPPER's per-edge inside/outside test
            // (VI1 = FCGET clip flags of edge verts A/B; VI5/VI6 = the plane bit for A/B). ORing
            // the mask into VI1 marks EVERY vertex OUTSIDE every plane -> the Sutherland-Hodgman
            // passes always end with 0 verts -> the fan emitters (XGKICK 780 @0x2d88/0x2f38)
            // NEVER fire -> the clipped bottom strip + water pool trifans (HW 1012 verts/frame)
            // were missing from the title. With G64 off the runner emits the same alternating
            // empty/real trifan giftags as HW (00008000/0000800N 302ec000). The geometry G64
            // was credited with (rock tristrips) is actually gated by the transform packers'
            // FMAND cascade, fixed at the root by G138/G139. Re-enable: DC2_G64_FORCE_ENABLE_FIX.
            if (m_state.pc == 0x30d8u || m_state.pc == 0x30f8u || m_state.pc == 0x3168u)
            {
                static const bool s_g63gate = (std::getenv("DC2_G63_GATE") != nullptr);
                static const bool s_g64noEnable = (std::getenv("DC2_G64_FORCE_ENABLE_FIX") == nullptr);
                if (s_g63gate)
                {
                    static std::atomic<uint32_t> s_g63n{0};
                    if (s_g63n.fetch_add(1u, std::memory_order_relaxed) < 96u)
                        std::fprintf(stderr,
                            "[G63:gate] pc=0x%x VI1=0x%x VI5=0x%x VI6=0x%x VI11=0x%x\n",
                            m_state.pc,
                            (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[5],
                            (uint32_t)(uint16_t)m_state.vi[6], (uint32_t)(uint16_t)m_state.vi[11]);
                    // G65: aggregate across ALL gate hits (not just first 96) so we can tell
                    // whether the emit subroutine EVER sees a nonzero per-batch count VI11.
                    static std::atomic<uint32_t> s_tot{0}, s_nz{0}, s_max{0};
                    const uint32_t v11 = (uint32_t)(uint16_t)m_state.vi[11];
                    s_tot.fetch_add(1u, std::memory_order_relaxed);
                    if (v11) s_nz.fetch_add(1u, std::memory_order_relaxed);
                    uint32_t pm = s_max.load(std::memory_order_relaxed);
                    while (v11 > pm && !s_max.compare_exchange_weak(pm, v11)) {}
                    const uint32_t tot = s_tot.load(std::memory_order_relaxed);
                    if ((tot % 2000u) == 0u)
                        std::fprintf(stderr, "[G65:gateagg] totalGateHits=%u nonzeroVI11=%u maxVI11=0x%x\n",
                                     tot, s_nz.load(std::memory_order_relaxed), s_max.load(std::memory_order_relaxed));
                }
                if (!s_g64noEnable && is == 1u)
                    m_state.vi[1] = (int32_t)(uint16_t)((uint16_t)m_state.vi[1] | (uint16_t)m_state.vi[it]);
            }
            if (id != 0)
                m_state.vi[id] = m_state.vi[is] & m_state.vi[it];
            return;
        }
        case 0x35: // IOR
            if (id != 0)
                m_state.vi[id] = m_state.vi[is] | m_state.vi[it];
            return;

        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F: // Lower special2 subtables
        {
            const uint8_t vuFunc = S2_FUNC(instr);
            switch (vuFunc)
            {
            case 0x30: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[is], 16);
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x31: // MR32 (xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[is][1], m_state.vf[is][2], m_state.vf[is][3], m_state.vf[is][0]};
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x34: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (g31_vu_trace_enabled())
                    {
                        const uint32_t n = s_g31VuLqiCount.fetch_add(1u, std::memory_order_relaxed);
                        if (n < 192u)
                        {
                            std::fprintf(stderr,
                                         "[G31:lqi] n=%u pc=0x%x fn=0x34 it=VF%u is=VI%u viBefore=0x%x addr=0x%x dest=0x%x v=(% .4g,% .4g,% .4g,% .4g)\n",
                                         n, m_state.pc, static_cast<uint32_t>(it), static_cast<uint32_t>(is),
                                         static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                         addr, static_cast<uint32_t>(dest),
                                         tmp[0], tmp[1], tmp[2], tmp[3]);
                        }
                    }
                    applyDest(m_state.vf[it], tmp, dest);
                }
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] + 1);
                return;
            }
            case 0x35: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    // G133: copy-packer behind-camera skip — the copy XYZF2 store is this SQI at pc
                    // 0x1c28 (S2_FUNC==0x35). Set ADC=word3 bit15 when the kicking vert's 1/W<=0
                    // (captured at 0x1c10). Skips the spanning connector triangle HW omits.
                    if (s_g133qskip && m_state.pc == 0x1c28u && (dest & 0x1) &&
                        g_dc2TitleRockScope.load(std::memory_order_relaxed))
                    {
                        if (s_g133qPrev <= 0.0f)
                        {
                            int32_t w3; std::memcpy(&w3, &tmp[3], 4);
                            w3 |= 0x8000; std::memcpy(&tmp[3], &w3, 4);
                            s_g133skipN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        else
                            s_g133keepN.fetch_add(1u, std::memory_order_relaxed);
                        static std::atomic<uint64_t> s_t{0};
                        if ((s_t.fetch_add(1u, std::memory_order_relaxed) % 200000u) == 0u)
                            std::fprintf(stderr, "[G133:qskip] skip=%llu keep=%llu\n",
                                         (unsigned long long)s_g133skipN.load(),
                                         (unsigned long long)s_g133keepN.load());
                    }
                    std::memcpy(vuData + addr, tmp, 16);
                }
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] + 1);
                return;
            }
            case 0x36: // LQD (Load Quadword, pre-decrement)
            {
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[it], tmp, dest);
                }
                return;
            }
            case 0x37: // SQD (Store Quadword, pre-decrement)
            {
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                return;
            }
            case 0x38: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = m_state.vf[it][ftf];
                float qv = (den != 0.0f)
                    ? (num / den)
                    : ((num >= 0.0f) ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max());
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                if (s_g29DivTrace.load(std::memory_order_relaxed) > 0)
                {
                    s_g29DivTrace.fetch_sub(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G29:div] op=0x38 pc=0x%x num=% .4g den=% .4g q=% .4g  it=VF%d ftf=%d vf[it]=(% .4g % .4g % .4g % .4g)\n",
                                 m_state.pc, num, den, qv, it, ftf,
                                 m_state.vf[it][0], m_state.vf[it][1], m_state.vf[it][2], m_state.vf[it][3]);
                    // G38: one-shot full VF dump at first GOOD (den!=0) and first BAD (den==0)
                    // divide, so the matrix regs zeroed in the bad case stand out.
                    static std::atomic<int> s_dumpGood{0}, s_dumpBad{0};
                    const bool bad = (den == 0.0f);
                    if ((bad ? s_dumpBad : s_dumpGood).exchange(1) == 0)
                    {
                        char b[1200]; int p = 0;
                        for (int r = 0; r < 32; ++r)
                            p += std::snprintf(b + p, sizeof(b) - p, "VF%d(% .3g,% .3g,% .3g,% .3g) ",
                                               r, m_state.vf[r][0], m_state.vf[r][1], m_state.vf[r][2], m_state.vf[r][3]);
                        std::fprintf(stderr, "[G38:vfdiv %s] %s\n", bad ? "BAD" : "GOOD", b);
                    }
                }
                return;
            }
            case 0x39: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                float val = m_state.vf[it][ftf];
                float qv = std::sqrt(std::fabs(val));
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x3A: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = std::sqrt(std::fabs(m_state.vf[it][ftf]));
                float qv = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::max();
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 13; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x3B: // WAITQ
                if (g87_q_latency_enabled() && s_vuQDelay > 0) { m_state.q = s_vuQPending; s_vuQDelay = 0; }
                return;
            case 0x3C: // MTIR
            {
                int comp = (instr >> 21) & 0x3;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[is][comp], 4);
                if (it != 0)
                    m_state.vi[it] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x3D: // MFIR
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[is] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x3E: // ILWR
            {
                if (it == 0)
                    return;
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t value = 0u;
                    if (dest & 0x8)
                        std::memcpy(&value, vuData + addr + 0u, 4);
                    if (dest & 0x4)
                        std::memcpy(&value, vuData + addr + 4u, 4);
                    if (dest & 0x2)
                        std::memcpy(&value, vuData + addr + 8u, 4);
                    if (dest & 0x1)
                        std::memcpy(&value, vuData + addr + 12u, 4);
                    m_state.vi[it] = (int32_t)(int16_t)(value & 0xFFFFu);
                }
                return;
            }
            case 0x3F: // ISWR
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t value = (uint32_t)(uint16_t)(m_state.vi[it] & 0xFFFF);
                    if (dest & 0x8)
                        std::memcpy(vuData + addr + 0u, &value, 4);
                    if (dest & 0x4)
                        std::memcpy(vuData + addr + 4u, &value, 4);
                    if (dest & 0x2)
                        std::memcpy(vuData + addr + 8u, &value, 4);
                    if (dest & 0x1)
                        std::memcpy(vuData + addr + 12u, &value, 4);
                }
                return;
            }
            case 0x40: // RNEXT
            case 0x41: // RGET
            case 0x42: // RINIT
            case 0x43: // RXOR
                return;
            case 0x64: // MFP
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x68: // XTOP - double-buffer DATA top (VIF1 TOPS latched at MSCAL); the model
                       // transform reads its vertex/count buffer relative to this. G28: was
                       // returning ITOP (0) -> reads from VU addr 0 -> runaway loop.
                if (it != 0)
                    m_state.vi[it] = (int32_t)(g_g297KickCapture // G297: captured top on the worker
                                                   ? g_g297KickTop
                                                   : (memory ? (memory->vif1_regs.top & 0x3FFu)
                                                             : (m_state.itop & 0x3FFu)));
                return;
            case 0x69: // XITOP - INTEGER top (VIF1 ITOP)
                if (it != 0)
                    m_state.vi[it] = (int32_t)(m_state.itop & 0x3FFu);
                return;
            case 0x6C: // XGKICK
                goto legacy_xgkick;
            case 0x70: // ESADD
                g239_stage_p(m_state.p,
                             m_state.vf[is][0] * m_state.vf[is][0] +
                             m_state.vf[is][1] * m_state.vf[is][1] +
                             m_state.vf[is][2] * m_state.vf[is][2],
                             11);
                return;
            case 0x71: // ERSADD
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                g239_stage_p(m_state.p, (s != 0.0f) ? (1.0f / s) : 0.0f, 18);
                return;
            }
            case 0x72: // ELENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                g239_stage_p(m_state.p, std::sqrt(std::fabs(s)), 18);
                return;
            }
            case 0x73: // ERLENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                float len = std::sqrt(std::fabs(s));
                g239_stage_p(m_state.p, (len != 0.0f) ? (1.0f / len) : 0.0f, 24);
                return;
            }
            case 0x76: // ESUM
                g239_stage_p(m_state.p,
                             m_state.vf[is][0] + m_state.vf[is][1] +
                             m_state.vf[is][2] + m_state.vf[is][3],
                             12);
                return;
            case 0x78: // ESQRT
            {
                int fsf = (instr >> 21) & 0x3;
                g239_stage_p(m_state.p, std::sqrt(std::fabs(m_state.vf[is][fsf])), 12);
                return;
            }
            case 0x79: // ERSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                float root = std::sqrt(std::fabs(m_state.vf[is][fsf]));
                g239_stage_p(m_state.p, (root != 0.0f) ? (1.0f / root) : 0.0f, 18);
                return;
            }
            case 0x7A: // ERCPR
            {
                int fsf = (instr >> 21) & 0x3;
                float val = m_state.vf[is][fsf];
                g239_stage_p(m_state.p, (val != 0.0f) ? (1.0f / val) : 0.0f, 12);
                return;
            }
            case 0x7B: // WAITP
                g239_wait_p(m_state.p);
                return;
            case 0x74: // EATANxy
            case 0x75: // EATANxz
            case 0x77: // reserved
            case 0x7C: // ESIN
            case 0x7D: // EATAN
            case 0x7E: // EEXP
                return;
            default:
                g22_log_unknown_lower(m_state.pc, instr, "lower-special2", opHi, vuFunc);
                return;
            }

            uint8_t funct2 = (instr >> 6) & 0x1F;
            switch (funct2)
            {
            case 0x00: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[is], 16);
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x01: // MR32 (rotate right by 32 bits = shift xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[is][1], m_state.vf[is][2], m_state.vf[is][3], m_state.vf[is][0]};
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x03: // MFIR (Move From Integer Register)
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[is] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x04: // MTIR (Move To Integer Register)
            {
                int comp = 0;
                if (dest & 0x8)
                    comp = 0;
                else if (dest & 0x4)
                    comp = 1;
                else if (dest & 0x2)
                    comp = 2;
                else
                    comp = 3;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[is][comp], 4);
                if (it != 0)
                    m_state.vi[it] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x05: // RNEXT
                return;
            case 0x06: // RGET
                return;
            case 0x07: // RINIT
                return;
            case 0x10: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (g31_vu_trace_enabled())
                    {
                        const uint32_t n = s_g31VuLqiCount.fetch_add(1u, std::memory_order_relaxed);
                        if (n < 192u)
                        {
                            std::fprintf(stderr,
                                         "[G31:lqi] n=%u pc=0x%x fn=0x10 it=VF%u is=VI%u viBefore=0x%x addr=0x%x dest=0x%x v=(% .4g,% .4g,% .4g,% .4g)\n",
                                         n, m_state.pc, static_cast<uint32_t>(it), static_cast<uint32_t>(is),
                                         static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                         addr, static_cast<uint32_t>(dest),
                                         tmp[0], tmp[1], tmp[2], tmp[3]);
                        }
                    }
                    applyDest(m_state.vf[it], tmp, dest);
                }
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] + 1);
                return;
            }
            case 0x11: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    // G133: copy-packer behind-camera skip — at the XYZF2 store (pc 0x1c28, VF17),
                    // set ADC=word3 bit15 when the kicking vert's 1/W<=0 (behind camera). Skips the
                    // spanning connector triangle HW omits. Title-rock-scoped + opt-in.
                    if (s_g133qskip && m_state.pc == 0x1c28u && is == 17u && (dest & 0x1) &&
                        g_dc2TitleRockScope.load(std::memory_order_relaxed))
                    {
                        if (s_g133qPrev <= 0.0f)
                        {
                            int32_t w3; std::memcpy(&w3, &tmp[3], 4);
                            w3 |= 0x8000; std::memcpy(&tmp[3], &w3, 4);
                            s_g133skipN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        else
                            s_g133keepN.fetch_add(1u, std::memory_order_relaxed);
                        static std::atomic<uint64_t> s_t{0};
                        if ((s_t.fetch_add(1u, std::memory_order_relaxed) % 200000u) == 0u)
                            std::fprintf(stderr, "[G133:qskip] skip=%llu keep=%llu\n",
                                         (unsigned long long)s_g133skipN.load(),
                                         (unsigned long long)s_g133keepN.load());
                    }
                    std::memcpy(vuData + addr, tmp, 16);
                }
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] + 1);
                return;
            }
            case 0x12: // LQD (Load Quadword, pre-decrement)
            {
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[it], tmp, dest);
                }
                return;
            }
            case 0x13: // SQD (Store Quadword, pre-decrement)
            {
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                return;
            }
            case 0x14: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = m_state.vf[it][ftf];
                float qv = (den != 0.0f)
                    ? (num / den)
                    : ((num >= 0.0f) ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max());
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                if (s_g29DivTrace.load(std::memory_order_relaxed) > 0)
                {
                    s_g29DivTrace.fetch_sub(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G29:div] op=0x14 pc=0x%x num=% .4g den=% .4g q=% .4g\n",
                                 m_state.pc, num, den, qv);
                }
                return;
            }
            case 0x15: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                float val = m_state.vf[it][ftf];
                float qv = std::sqrt(std::fabs(val));
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x16: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = std::sqrt(std::fabs(m_state.vf[it][ftf]));
                float qv = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::max();
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 13; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x17: // WAITQ
                if (g87_q_latency_enabled() && s_vuQDelay > 0) { m_state.q = s_vuQPending; s_vuQDelay = 0; }
                return;
            case 0x18: // ESADD
                return;
            case 0x19: // ERSADD
                return;
            case 0x1B: // ELENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                m_state.p = std::sqrt(s);
                return;
            }
            case 0x1C: // ERCPR
            {
                int fsf = (instr >> 21) & 0x3;
                float val = m_state.vf[is][fsf];
                m_state.p = (val != 0.0f) ? (1.0f / val) : std::numeric_limits<float>::max();
                return;
            }
            case 0x1D: // ERLENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                float len = std::sqrt(s);
                m_state.p = (len != 0.0f) ? (1.0f / len) : std::numeric_limits<float>::max();
                return;
            }
            case 0x1E: // WAITP
                return;
            case 0x1A: // EATAN / EATANxy / EATANxz
                return;
            case 0x1F: // MFP (Move From P register)
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            default:
                g22_log_unknown_lower(m_state.pc, instr, "lower-special2", opHi, funct2);
                return;
            }
        }
        case 0x7D: // legacy-unreachable XGKICK body; real dispatch is full function 0x6C.
        legacy_xgkick:
        {
            if (!vuData || dataSize < 16u)
                return;

            auto wrapOffset = [&](uint32_t off) -> uint32_t
            {
                return off % dataSize;
            };

            auto read64Wrap = [&](uint32_t off) -> uint64_t
            {
                uint8_t bytes[8];
                for (uint32_t i = 0; i < 8u; ++i)
                {
                    bytes[i] = vuData[wrapOffset(off + i)];
                }
                uint64_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                return value;
            };

            uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
            addr = wrapOffset(addr);
            // G62: at each XGKICK dump the GIFtag PRIM currently in VU memory at the candidate
            // template qwords (0x35c/0x398 = what the map per-strip copy subroutine reads;
            // 0x3c/0xf0 = where the EE templates were UNPACKED). If 0x35c/0x398 hold trifan(5)
            // while 0x3c/0xf0 hold the correct tristrip(4), the EE template never reached the
            // address VU1 reads (a VIF dest / addressing bug). Env DC2_G62_VUTPL.
            static const bool s_g62vutpl = (std::getenv("DC2_G62_VUTPL") != nullptr);
            if (s_g62vutpl)
            {
                static std::atomic<uint32_t> s_g62v{0};
                if (s_g62v.fetch_add(1u, std::memory_order_relaxed) < 24u)
                {
                    const uint32_t tplQw[4] = {0x35cu, 0x398u, 0x3cu, 0xf0u};
                    char buf[256]; int bp = 0;
                    bp += std::snprintf(buf + bp, sizeof(buf) - bp, "[G62:vutpl] kickAddr=0x%x ", addr);
                    for (int t = 0; t < 4; ++t)
                    {
                        const uint64_t lo = read64Wrap(wrapOffset(tplQw[t] * 16u));
                        const uint64_t hi = read64Wrap(wrapOffset(tplQw[t] * 16u + 8u));
                        const uint32_t prim = (uint32_t)((lo >> 47) & 0x7FFu);
                        bp += std::snprintf(buf + bp, sizeof(buf) - bp,
                                            "qw0x%x:prim=0x%x(t%u,pre%u,nl%u,hi=0x%llx) ",
                                            tplQw[t], prim, prim & 7u, (unsigned)((lo >> 46) & 1u),
                                            (unsigned)(lo & 0x7FFFu),
                                            (unsigned long long)(hi & 0xFFFFu));
                    }
                    // G63: also dump the batch-gating flag word (VU qw 0x26, read by the title-map
                    // program as `ILW.x VI1,38(VI0)`) and count words (qw 0x1e/0x1d), which gate
                    // which prim-type batches render (IAND VI1,{0x80,0x40,0x200,0x100}).
                    const uint64_t f26lo = read64Wrap(wrapOffset(0x26u * 16u));
                    const uint64_t f26hi = read64Wrap(wrapOffset(0x26u * 16u + 8u));
                    const uint64_t f1elo = read64Wrap(wrapOffset(0x1eu * 16u));
                    const uint64_t f1dlo = read64Wrap(wrapOffset(0x1du * 16u));
                    std::fprintf(stderr,
                                 "[G63:flags] qw0x26=%08x %08x %08x %08x  qw0x1e.x=%u qw0x1d.x=%u  &0x80=%u &0x40=%u &0x200=%u &0x100=%u\n",
                                 (uint32_t)(f26lo & 0xFFFFFFFFu), (uint32_t)(f26lo >> 32),
                                 (uint32_t)(f26hi & 0xFFFFFFFFu), (uint32_t)(f26hi >> 32),
                                 (uint32_t)(f1elo & 0xFFFFFFFFu), (uint32_t)(f1dlo & 0xFFFFFFFFu),
                                 (unsigned)((f26lo & 0x80u) != 0u), (unsigned)((f26lo & 0x40u) != 0u),
                                 (unsigned)((f26lo & 0x200u) != 0u), (unsigned)((f26lo & 0x100u) != 0u));
                }
            }
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_g22Vu1XgkickPacketCount.fetch_add(1u, std::memory_order_relaxed);
                if (n < 96u)
                {
                    uint64_t tagLo = 0u;
                    uint64_t tagHi = 0u;
                    if (dataSize >= 16u)
                    {
                        tagLo = read64Wrap(addr);
                        tagHi = read64Wrap(addr + 8u);
                    }
                    std::fprintf(stderr,
                                 "[G22:xgkick] n=%u pc=0x%x is=VI%u vi=0x%x addr=0x%x tagLo=0x%016llx tagHi=0x%016llx itop=0x%x\n",
                                 n, m_state.pc, static_cast<uint32_t>(is),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                 addr,
                                 static_cast<unsigned long long>(tagLo),
                                 static_cast<unsigned long long>(tagHi),
                                 m_state.itop);

                    // G28: decode the first PACKED tristrip vertex (ST,RGBAQ,XYZF2 => XYZF2 at
                    // tag+48) screen XY (12.4 fixed) to confirm the transform lands on-screen.
                    {
                        const uint8_t flg0 = (uint8_t)((tagLo >> 58) & 0x3u);
                        const uint32_t nreg0 = (uint32_t)((tagLo >> 60) & 0xFu);
                        if (flg0 == 0u && nreg0 == 3u && dataSize >= 64u && n < 4u)
                        {
                            // Raw dump of vert0's 3 regs (ST,RGBAQ,XYZF2) lo/hi to find the
                            // real XYZF2 layout / whether positions are genuinely zero.
                            std::fprintf(stderr,
                                "[G28:raw] n=%u ST=%016llx_%016llx RGBAQ=%016llx_%016llx XYZF2=%016llx_%016llx\n",
                                n,
                                (unsigned long long)read64Wrap(wrapOffset(addr + 24u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 16u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 40u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 32u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 56u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 48u)));
                        }
                    }

                    // G24: map where real data sits in VU1 data mem (16-byte qwords) so we can
                    // tell "no input delivered" from "input present but transformed to NaN".
                    uint32_t nzQw = 0u, firstNz[6] = {0,0,0,0,0,0}, nFirst = 0u;
                    for (uint32_t off = 0u; off + 16u <= dataSize; off += 16u)
                    {
                        bool nz = false;
                        for (uint32_t b = 0u; b < 16u; ++b) { if (vuData[off + b] != 0u) { nz = true; break; } }
                        if (nz) { if (nFirst < 6u) firstNz[nFirst++] = off; ++nzQw; }
                    }
                    std::fprintf(stderr,
                                 "[G24:vumap] nzQw=%u/%u firstNz=0x%x,0x%x,0x%x,0x%x,0x%x,0x%x readAddr=0x%x\n",
                                 nzQw, dataSize / 16u, firstNz[0], firstNz[1], firstNz[2],
                                 firstNz[3], firstNz[4], firstNz[5], addr);
                }
            }
            // G65: attribute the prim types emitted by THIS XGKICK to the program that built
            // the packet (s_g65CurStartPC). Walk the giftag chain, tally NLOOP per PRIM type
            // into a per-startPC bucket, print periodically. Reveals which VU1 program emits
            // the rock tristrips (PRIM type 4) and whether the title ever runs it.
            static const bool s_g65kickagg = (std::getenv("DC2_G65_KICKAGG") != nullptr);
            if (s_g65kickagg)
            {
                struct Bucket { uint32_t pc; uint64_t prim[8]; uint64_t kicks; };
                static Bucket s_b[24]; static uint32_t s_bn = 0; static uint64_t s_kc = 0;
                Bucket* bk = nullptr;
                for (uint32_t i = 0; i < s_bn; ++i) if (s_b[i].pc == s_g65CurStartPC) { bk = &s_b[i]; break; }
                if (!bk && s_bn < 24u) { bk = &s_b[s_bn++]; bk->pc = s_g65CurStartPC; for (int j=0;j<8;++j) bk->prim[j]=0; bk->kicks=0; }
                if (bk)
                {
                    bk->kicks++;
                    uint32_t off = addr; bool d2 = false;
                    for (int s2 = 0; s2 < 256 && !d2; ++s2)
                    {
                        uint64_t lo = read64Wrap(off);
                        uint32_t nl = (uint32_t)(lo & 0x7FFFu);
                        uint8_t fg = (uint8_t)((lo >> 58) & 0x3u);
                        uint32_t nr = (uint32_t)((lo >> 60) & 0xFu); if (nr == 0u) nr = 16u;
                        bool pre = ((lo >> 46) & 1u) != 0u;
                        uint32_t prim = (uint32_t)((lo >> 47) & 0x7u);
                        bool eop2 = ((lo >> 15) & 1u) != 0u;
                        if (pre) bk->prim[prim] += nl;
                        uint32_t ps = 16u;
                        if (fg == 0u) ps += nl * nr * 16u;
                        else if (fg == 1u) { uint32_t rg = nl*nr; ps += rg*8u; if (rg&1u) ps += 8u; }
                        else if (fg == 2u) ps += nl * 16u;
                        if (ps == 0u) break;
                        off = wrapOffset(off + ps);
                        if (eop2) d2 = true;
                    }
                }
                if ((++s_kc % 4000u) == 0u)
                {
                    for (uint32_t i = 0; i < s_bn; ++i)
                        std::fprintf(stderr, "[G65:kickagg] startPC=0x%x kicks=%llu prim[pt0..7]= %llu %llu %llu %llu %llu %llu %llu %llu\n",
                                     s_b[i].pc, (unsigned long long)s_b[i].kicks,
                                     (unsigned long long)s_b[i].prim[0],(unsigned long long)s_b[i].prim[1],
                                     (unsigned long long)s_b[i].prim[2],(unsigned long long)s_b[i].prim[3],
                                     (unsigned long long)s_b[i].prim[4],(unsigned long long)s_b[i].prim[5],
                                     (unsigned long long)s_b[i].prim[6],(unsigned long long)s_b[i].prim[7]);
                }
            }

            // G118: live per-PRIM per-ADC tally of THIS XGKICK's emitted packet — the
            // runner-side equivalent of tools/g69_hw_adc.py on ref/dumps/new_game.gs. Walks the
            // kicked GIFtag chain in VU memory and counts XYZF2/XYZ2 vertices by (prim, ADC bit
            // 111 = hi>>47&1). Answers the decisive G118 question: does the runner's NATURAL
            // (DC2_G100_NO_FORCEDRAW=1) tri packer (0x1dc0) reproduce HW's ~85%-draw "0,0,1"
            // pattern and the tristrip ~60%-nodraw primer pattern, and is trifan(pt5) genuinely
            // zero? Env DC2_G118_ADC; startPc==0x10 only; quiet by default.
            static const bool s_g118adc = (std::getenv("DC2_G118_ADC") != nullptr);
            if (s_g118adc && s_g65CurStartPC == 0x10u)
            {
                static std::atomic<uint64_t> s_adcSet[8] = {};
                static std::atomic<uint64_t> s_adcClr[8] = {};
                static std::atomic<uint64_t> s_g118kc{0};
                // Per-PACKER tally keyed by the XGKICK pc: 0x2180=tristrip(0x1ff0),
                // 0x1fd0=tri(0x1dc0), 0x1da0=trifan(0x1c50), 0x1c30=copy(0x1b68). Resolves the
                // copy-vs-transform question: which packer's verts actually DRAW (ADC=0) on the
                // runner, and is the 0x1ff0 transform-tristrip 100% nodraw (= the cavern walls
                // invisible) while the drawn tristrips come from the copy packer.
                static std::atomic<uint64_t> s_pkSet[4] = {}; // [tristrip,tri,trifan,copy]
                static std::atomic<uint64_t> s_pkClr[4] = {};
                // G119: per-packer screen-position stats of DRAWING (ADC=0) verts. XYZF2 packed:
                // X=lo[0:15], Y=lo[16:31] in 12.4 fixed (px = raw/16, screen centred ~XYOFFSET).
                // "on-screen" window (generous): raw X in [0x4000,0xD000], Y in [0x4000,0xC000].
                // Resolves the copy-vs-transform crux: which packer's drawn verts are visible.
                static std::atomic<uint64_t> s_pkOn[4] = {};   // drawing verts on-screen
                static std::atomic<uint64_t> s_pkOff[4] = {};  // drawing verts off-screen
                static std::atomic<uint64_t> s_pkSX[4] = {};   // sum X (drawing verts, for centroid)
                static std::atomic<uint64_t> s_pkSY[4] = {};   // sum Y
                static std::atomic<uint64_t> s_pkAX[4] = {};   // sum X over ALL verts (incl nodraw)
                static std::atomic<uint64_t> s_pkAY[4] = {};   // sum Y over ALL verts
                static std::atomic<uint64_t> s_pkAN[4] = {};   // count ALL verts
                // G132: per-(packer x GS-prim) ADC tally. Resolves which packer emits which GS prim
                // (e.g. does the copy packer 0x1b68 emit prim=tri(3) all-draw, while the drawing
                // tristrips(4) come from a different packer?). [packer][prim] -> draw(ADC0)/nodraw.
                static std::atomic<uint64_t> s_pkPrimDraw[4][8] = {};
                static std::atomic<uint64_t> s_pkPrimNo[4][8] = {};
                // G119: per-STRIP structure of the tristrip packer (pkIdx==0, pc=0x2180). For each
                // tristrip GIFtag, nloop = strip length and the per-vertex loop index l = strip
                // position. Histogram strip-length and ADC-by-position so the runner's 0x1ff0 output
                // is DIRECTLY comparable to tools/g117_strip_adc.py on ref/dumps/new_game.gs (HW:
                // 1296 strips len 4-7, pos0/1 92% nodraw primers, pos2+ ~45%). Settles whether the
                // runner builds HW's short-strip structure (=> fix is per-position ADC in 0x1ff0) or
                // a divergent grouping (=> EE packet bug). Env DC2_G119_STRIP; quiet by default.
                static const bool s_g119strip = (std::getenv("DC2_G119_STRIP") != nullptr);
                static std::atomic<uint64_t> s_posSet[32] = {}; // ADC=1 (nodraw) by strip position
                static std::atomic<uint64_t> s_posClr[32] = {}; // ADC=0 (draw)   by strip position
                static std::atomic<uint64_t> s_lenHist[33] = {}; // strip-length histogram (cap 32)
                static std::atomic<uint64_t> s_stripN{0};       // total tristrip GIFtags seen
                static std::atomic<uint64_t> s_copyLenHist[33] = {}; // G132 copy-packer tristrip lens
                static std::atomic<uint64_t> s_copyStripN{0};        // G132 copy tristrip GIFtags
                static std::atomic<uint64_t> s_copyExt[7] = {};      // G132 copy tristrip extent buckets px
                int pkIdx = -1;
                switch (m_state.pc) { case 0x2180u: pkIdx=0; break; case 0x1fd0u: pkIdx=1; break;
                                      case 0x1da0u: pkIdx=2; break; case 0x1c30u: pkIdx=3; break; }
                uint32_t off = addr; bool eopDone = false; uint32_t curPrim = 0u;
                for (int s2 = 0; s2 < 256 && !eopDone; ++s2)
                {
                    uint64_t lo = read64Wrap(off);
                    uint32_t nl = (uint32_t)(lo & 0x7FFFu);
                    uint8_t fg = (uint8_t)((lo >> 58) & 0x3u);
                    uint32_t nr = (uint32_t)((lo >> 60) & 0xFu); if (nr == 0u) nr = 16u;
                    bool pre = ((lo >> 46) & 1u) != 0u;
                    bool eop2 = ((lo >> 15) & 1u) != 0u;
                    uint64_t rd = read64Wrap(off + 8u); // REGS descriptor (bits 64..127)
                    if (pre) curPrim = (uint32_t)((lo >> 47) & 0x7u);
                    uint32_t voff = off + 16u;
                    if (fg == 0u)
                    {
                        // G119: this GIFtag's vertex count == strip length for the tristrip packer.
                        if (s_g119strip && pkIdx == 0 && nl >= 3u)
                        {
                            s_lenHist[nl < 32u ? nl : 32u].fetch_add(1u, std::memory_order_relaxed);
                            s_stripN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        // G132: COPY-packer tristrip GIFtag length histogram. If the copy packer
                        // (pkIdx==3) emits ONE long tristrip GIFtag = many strips concatenated, every
                        // strip boundary (no restart ADC, copy ADC structurally 0) draws a spanning
                        // connector triangle = the green streaks. Short (4-7) = one real strip each.
                        if (s_g119strip && pkIdx == 3 && curPrim == 4u && nl >= 3u)
                        {
                            s_copyLenHist[nl < 32u ? nl : 32u].fetch_add(1u, std::memory_order_relaxed);
                            s_copyStripN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        // G132: per-copy-tristrip-GIFtag screen bbox (spanning vs local test).
                        const uint32_t gifPrim = curPrim;
                        int cgx0 = 0x7fffffff, cgx1 = -1, cgy0 = 0x7fffffff, cgy1 = -1;
                        for (uint32_t l = 0; l < nl; ++l)
                            for (uint32_t r = 0; r < nr; ++r)
                            {
                                uint32_t rr = (uint32_t)((rd >> (r * 4u)) & 0xFu);
                                if (rr == 0x00u) curPrim = (uint32_t)(read64Wrap(voff) & 0x7u);
                                else if (rr == 0x04u || rr == 0x05u)
                                {
                                    uint64_t hi = read64Wrap(voff + 8u);
                                    uint32_t a = (uint32_t)((hi >> 47) & 1u);
                                    if (s_g119strip && pkIdx == 3 && gifPrim == 4u)
                                    {
                                        uint64_t vb = read64Wrap(voff);
                                        int bx = (int)(vb & 0xFFFFu), by = (int)((vb >> 32) & 0xFFFFu);
                                        if (bx < cgx0) cgx0 = bx; if (bx > cgx1) cgx1 = bx;
                                        if (by < cgy0) cgy0 = by; if (by > cgy1) cgy1 = by;
                                    }
                                    if (a) s_adcSet[curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                    else   s_adcClr[curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                    if (pkIdx >= 0) { if (a) s_pkSet[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                                      else   s_pkClr[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                                      // G132: per-(packer x prim)
                                                      if (a) s_pkPrimNo[pkIdx][curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                                      else   s_pkPrimDraw[pkIdx][curPrim & 7u].fetch_add(1u, std::memory_order_relaxed); }
                                    // G119: screen-position per packer. ALL verts -> centroid (so
                                    // 0x1ff0's on-screen nodraw cavern has a reference); DRAWING
                                    // verts -> on/off-screen.
                                    if (s_g119strip && pkIdx >= 0)
                                    {
                                        uint64_t vlo = read64Wrap(voff);
                                        // PACKED XYZF2: X=lo[0:15], Y=lo[32:47] (12.4 fixed).
                                        uint32_t vx = (uint32_t)(vlo & 0xFFFFu);
                                        uint32_t vy = (uint32_t)((vlo >> 32) & 0xFFFFu);
                                        s_pkAX[pkIdx].fetch_add(vx, std::memory_order_relaxed);
                                        s_pkAY[pkIdx].fetch_add(vy, std::memory_order_relaxed);
                                        s_pkAN[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                        if (a == 0u)
                                        {
                                            s_pkSX[pkIdx].fetch_add(vx, std::memory_order_relaxed);
                                            s_pkSY[pkIdx].fetch_add(vy, std::memory_order_relaxed);
                                            bool on = (vx >= 0x4000u && vx <= 0xD000u && vy >= 0x4000u && vy <= 0xC000u);
                                            if (on) s_pkOn[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                            else    s_pkOff[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                        }
                                    }
                                    // G119: ADC by strip position (l) for the tristrip packer.
                                    if (s_g119strip && pkIdx == 0 && nl >= 3u)
                                    {
                                        uint32_t p = l < 32u ? l : 31u;
                                        if (a) s_posSet[p].fetch_add(1u, std::memory_order_relaxed);
                                        else   s_posClr[p].fetch_add(1u, std::memory_order_relaxed);
                                    }
                                }
                                voff += 16u;
                            }
                        // G132: finalize this copy-tristrip GIFtag's extent (px) -> bucket.
                        if (s_g119strip && pkIdx == 3 && gifPrim == 4u && cgx1 >= 0 && nl >= 3u)
                        {
                            int ext = (cgx1 - cgx0); int ey = (cgy1 - cgy0); if (ey > ext) ext = ey;
                            ext /= 16; // 12.4 -> px
                            uint32_t b = ext < 64 ? 0u : ext < 128 ? 1u : ext < 256 ? 2u : ext < 512 ? 3u :
                                         ext < 1024 ? 4u : ext < 2048 ? 5u : 6u;
                            s_copyExt[b].fetch_add(1u, std::memory_order_relaxed);
                        }
                    }
                    uint32_t ps = 16u;
                    if (fg == 0u) ps += nl * nr * 16u;
                    else if (fg == 1u) { uint32_t rg = nl * nr; ps += rg * 8u; if (rg & 1u) ps += 8u; }
                    else if (fg == 2u) ps += nl * 16u;
                    if (ps == 0u) break;
                    off = wrapOffset(off + ps);
                    if (eop2) eopDone = true;
                }
                if ((s_g118kc.fetch_add(1u, std::memory_order_relaxed) % 4000u) == 0u)
                {
                    std::fprintf(stderr,
                        "[G118:adc] tri(set/clr)=%llu/%llu tristrip=%llu/%llu trifan=%llu/%llu\n",
                        (unsigned long long)s_adcSet[3].load(), (unsigned long long)s_adcClr[3].load(),
                        (unsigned long long)s_adcSet[4].load(), (unsigned long long)s_adcClr[4].load(),
                        (unsigned long long)s_adcSet[5].load(), (unsigned long long)s_adcClr[5].load());
                    std::fprintf(stderr,
                        "[G118:pkr] 0x1ff0tstrip(set/clr)=%llu/%llu 0x1dc0tri=%llu/%llu 0x1c50tfan=%llu/%llu 0x1b68copy=%llu/%llu\n",
                        (unsigned long long)s_pkSet[0].load(), (unsigned long long)s_pkClr[0].load(),
                        (unsigned long long)s_pkSet[1].load(), (unsigned long long)s_pkClr[1].load(),
                        (unsigned long long)s_pkSet[2].load(), (unsigned long long)s_pkClr[2].load(),
                        (unsigned long long)s_pkSet[3].load(), (unsigned long long)s_pkClr[3].load());
                    {
                        // G132: per-(packer x prim) draw/nodraw. prim 3=tri 4=tristrip 5=trifan.
                        const char* pkn132[4] = {"0x1ff0","0x1dc0","0x1c50","0x1b68"};
                        for (int k = 0; k < 4; ++k)
                            std::fprintf(stderr,
                                "[G132:pkprim] %s tri(d/n)=%llu/%llu tstrip=%llu/%llu tfan=%llu/%llu\n",
                                pkn132[k],
                                (unsigned long long)s_pkPrimDraw[k][3].load(), (unsigned long long)s_pkPrimNo[k][3].load(),
                                (unsigned long long)s_pkPrimDraw[k][4].load(), (unsigned long long)s_pkPrimNo[k][4].load(),
                                (unsigned long long)s_pkPrimDraw[k][5].load(), (unsigned long long)s_pkPrimNo[k][5].load());
                    }
                    if (s_g119strip)
                    {
                        // Per-packer DRAWING-vert screen position: on/off-screen + centroid (px).
                        const char* pkn[4] = {"0x1ff0tstrip","0x1dc0tri","0x1c50tfan","0x1b68copy"};
                        for (int k = 0; k < 4; ++k)
                        {
                            uint64_t on = s_pkOn[k].load(), off = s_pkOff[k].load(), t = on + off;
                            double cx = t ? (double)s_pkSX[k].load()/(double)t/16.0 : 0.0;
                            double cy = t ? (double)s_pkSY[k].load()/(double)t/16.0 : 0.0;
                            uint64_t an = s_pkAN[k].load();
                            double ax = an ? (double)s_pkAX[k].load()/(double)an/16.0 : 0.0;
                            double ay = an ? (double)s_pkAY[k].load()/(double)an/16.0 : 0.0;
                            std::fprintf(stderr, "[G119:pos2] %-12s draw=%llu drawOn=%.0f%% drawCentroid=(%.0f,%.0f) ALLn=%llu allCentroid=(%.0f,%.0f)\n",
                                         pkn[k], (unsigned long long)t, t ? 100.0*(double)on/(double)t : 0.0, cx, cy,
                                         (unsigned long long)an, ax, ay);
                        }
                        // Strip-length histogram (matches g117_strip_adc.py len histogram on HW).
                        std::fprintf(stderr, "[G119:len] tstripGIFtags=%llu lenHist(3..16,>16):",
                                     (unsigned long long)s_stripN.load());
                        for (uint32_t k = 3; k <= 16; ++k)
                            std::fprintf(stderr, " %u:%llu", k, (unsigned long long)s_lenHist[k].load());
                        uint64_t gt16 = 0; for (uint32_t k = 17; k <= 32; ++k) gt16 += s_lenHist[k].load();
                        std::fprintf(stderr, " >16:%llu\n", (unsigned long long)gt16);
                        // G132: COPY-packer tristrip GIFtag length histogram (spanning-connector test).
                        std::fprintf(stderr, "[G132:copylen] copyTstripGIFtags=%llu lenHist(3..16,>16):",
                                     (unsigned long long)s_copyStripN.load());
                        for (uint32_t k = 3; k <= 16; ++k)
                            std::fprintf(stderr, " %u:%llu", k, (unsigned long long)s_copyLenHist[k].load());
                        uint64_t cgt16 = 0; for (uint32_t k = 17; k <= 32; ++k) cgt16 += s_copyLenHist[k].load();
                        std::fprintf(stderr, " >16:%llu\n", (unsigned long long)cgt16);
                        // G132: copy-tristrip screen-extent histogram (spanning test). Buckets px:
                        // <64 <128 <256 <512 <1024 <2048 >=2048. Local strips => mostly <128.
                        std::fprintf(stderr, "[G132:copyext] px <64:%llu <128:%llu <256:%llu <512:%llu <1024:%llu <2048:%llu >=2048:%llu\n",
                                     (unsigned long long)s_copyExt[0].load(), (unsigned long long)s_copyExt[1].load(),
                                     (unsigned long long)s_copyExt[2].load(), (unsigned long long)s_copyExt[3].load(),
                                     (unsigned long long)s_copyExt[4].load(), (unsigned long long)s_copyExt[5].load(),
                                     (unsigned long long)s_copyExt[6].load());
                        // ADC=1(nodraw)% by strip position pos0..pos9.
                        std::fprintf(stderr, "[G119:pos] nodraw%% by strip position:");
                        for (uint32_t p = 0; p < 10; ++p)
                        {
                            uint64_t s = s_posSet[p].load(), c = s_posClr[p].load(), t = s + c;
                            double pct = t ? (100.0 * (double)s / (double)t) : 0.0;
                            std::fprintf(stderr, " pos%u:%.0f%%(n=%llu)", p, pct, (unsigned long long)t);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }

            uint32_t pktOff = addr;
            uint32_t totalBytes = 0u;
            bool done = false;

            for (int safety = 0; safety < 256 && !done; ++safety)
            {
                uint64_t tagLo = read64Wrap(pktOff);
                uint32_t nloop = (uint32_t)(tagLo & 0x7FFFu);
                uint8_t flg = (uint8_t)((tagLo >> 58) & 0x3u);
                uint32_t nreg = (uint32_t)((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                bool eop = ((tagLo >> 15) & 0x1ull) != 0ull;

                uint32_t pktSize = 16u;
                if (flg == 0u)
                {
                    pktSize += nloop * nreg * 16u;
                }
                else if (flg == 1u)
                {
                    uint32_t regs = nloop * nreg;
                    pktSize += regs * 8u;
                    if ((regs & 1u) != 0u)
                        pktSize += 8u;
                }
                else if (flg == 2u)
                {
                    pktSize += nloop * 16u;
                }

                if (pktSize == 0u)
                    break;

                totalBytes += pktSize;
                pktOff = wrapOffset(pktOff + pktSize);
                if (eop)
                    done = true;
            }

            if (totalBytes == 0u)
                return;

            const uint32_t debugIndex = s_debugVu1XgkickCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[vu1:xgkick] idx=" << debugIndex
                                                << " addr=0x" << std::hex << addr
                                                << " totalBytes=0x" << totalBytes
                                                << std::dec
                                                << " wrap=" << static_cast<uint32_t>((addr + totalBytes > dataSize) ? 1u : 0u)
                                                << std::endl);
            }

            // G96: title rock TEX0/geometry de-interleaving fix (root of G91/G95). Each title
            // map-part batch UNPACKs its intended TEX0 into VU data memory (state packet), but the
            // forced copy packer's XGKICK never re-emits it, so every rock batch samples whatever
            // the up-front texture reload last bound -> the wall pages (0x2b20/0x2f20/0x3320) are
            // bound to the GS but no geometry is ever drawn while they are active (blue voids /
            // "missing models"). Title-rock-scoped: locate this batch's TEX0 in VU data memory and
            // PREPEND a TEX0_1 A+D GIF register write to the kicked packet, reproducing the HW
            // per-batch bind. DC2_G96_TEXSCAN reports the VU offset; DC2_G96_TEX0BIND applies.
            extern std::atomic<bool> g_dc2TitleRockScope;
            static const bool s_g96bind = (std::getenv("DC2_G96_TEX0BIND") != nullptr);
            static const bool s_g96scan = (std::getenv("DC2_G96_TEXSCAN") != nullptr);
            static const uint32_t s_g107StripAdcFirst = []() -> uint32_t {
                const char *e = std::getenv("DC2_G107_STRIP_ADC_FIRST");
                if (!e)
                    return 0u;
                const long v = std::strtol(e, nullptr, 0);
                return (v > 0) ? static_cast<uint32_t>((v > 64) ? 64 : v) : 0u;
            }();
            static const uint32_t s_g107StripAdcTarget = []() -> uint32_t {
                const char *e = std::getenv("DC2_G107_STRIP_ADC_TBP");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
            }();
            uint64_t g96Tex0 = 0u;
            uint32_t g96Off = 0xffffffffu;
            if ((s_g96bind || s_g96scan || s_g107StripAdcFirst != 0u || g136_trace_enabled()) && vuData &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed))
            {
                for (uint32_t off = 0u; off + 8u <= dataSize; off += 4u)
                {
                    uint64_t q;
                    std::memcpy(&q, vuData + off, 8);
                    const uint32_t tbp = (uint32_t)(q & 0x3FFFu);
                    const uint32_t psm = (uint32_t)((q >> 20) & 0x3Fu);
                    if (psm == 0x13u && tbp >= 0x2720u && tbp <= 0x3960u)
                    {
                        g96Tex0 = q;
                        g96Off = off;
                        break;
                    }
                }
                if (s_g96scan)
                {
                    static std::atomic<uint32_t> s_g96n{0};
                    const uint32_t sn = s_g96n.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 96u)
                        std::fprintf(stderr, "[G96:texscan] kickAddr=0x%x off=0x%x tex0=0x%llx tbp=0x%x\n",
                                     addr, g96Off, (unsigned long long)g96Tex0,
                                     (unsigned)(g96Tex0 & 0x3FFFu));
                    // One-shot hexdump of the low VU data region (where the state packet UNPACKs
                    // matrices/TEX0/light) so we can eyeball the per-batch TEX0 location.
                    if (sn < 4u)
                    {
                        for (uint32_t b = 0u; b + 16u <= 0x180u && b + 16u <= dataSize; b += 16u)
                            std::fprintf(stderr, "[G96:vumem] kick%u 0x%03x: %016llx %016llx\n",
                                         sn, b,
                                         (unsigned long long)(*(uint64_t *)(vuData + b)),
                                         (unsigned long long)(*(uint64_t *)(vuData + b + 8u)));
                    }
                }
            }

            // G98: per-kick world (LW) matrix probe. HW gives each title map-part its own LW
            // matrix in its state packet; the suspicion is the runner uploads all parts' state
            // up front (de-interleaved from geometry) so every kick's VU memory ends up with the
            // SAME (last) part's matrix -> 11/12 parts transform off-screen. Dump the world-matrix
            // rows (VU 0x80/0xa0 rotation, 0xb0 translation) + combined-matrix row0 per kick so a
            // python pass can confirm whether all kicks share one matrix. DC2_G98_MTX, title scope.
            {
                static const bool s_g98mtx = (std::getenv("DC2_G98_MTX") != nullptr);
                if (s_g98mtx && vuData &&
                    g_dc2TitleRockScope.load(std::memory_order_relaxed))
                {
                    static std::atomic<uint32_t> s_g98n{0};
                    const uint32_t sn = s_g98n.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 96u)
                    {
                        auto F = [&](uint32_t o) -> float {
                            float f; std::memcpy(&f, vuData + o, 4); return f;
                        };
                        std::fprintf(stderr,
                            "[G98:mtx] kick=%u worldRot=(%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f) "
                            "comb0=(%.1f,%.1f,%.1f,%.3f)\n",
                            sn, F(0x80), F(0x88), F(0xa0), F(0xb0), F(0xb4), F(0xb8),
                            F(0x40), F(0x44), F(0x48), F(0x4c));
                    }
                }
            }

            auto g136DumpVuQwords = [&](uint32_t seq, const char *label, uint32_t qwBase, uint32_t qwords)
            {
                const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    const uint32_t qw = (qwBase + q) & 0x3FFu;
                    const uint32_t byteOff = qw * 16u;
                    const uint64_t lo = read64Wrap(byteOff);
                    const uint64_t hi = read64Wrap(byteOff + 8u);
                    uint32_t w[4] = {
                        static_cast<uint32_t>(lo & 0xFFFFFFFFu),
                        static_cast<uint32_t>(lo >> 32u),
                        static_cast<uint32_t>(hi & 0xFFFFFFFFu),
                        static_cast<uint32_t>(hi >> 32u),
                    };
                    float f[4] = {};
                    std::memcpy(f, w, sizeof(f));
                    std::fprintf(stderr,
                                 "[G136:vuqw] seq=%u %s qw=0x%x raw=%08x %08x %08x %08x f=(% .6g % .6g % .6g % .6g)\n",
                                 seq, label, qw, w[0], w[1], w[2], w[3],
                                 f[0], f[1], f[2], f[3]);
                }
            };

            auto g137DumpOrigin = [&](uint32_t seq, const char *label, uint32_t qwBase, uint32_t qwords)
            {
                if (!g137_trace_enabled())
                    return;

                const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    const uint32_t qw = (qwBase + q) & 0x3FFu;
                    const Dc2G137VuOrigin &origin = g_dc2G137VuOrigins[qw];
                    std::fprintf(stderr,
                                 "[G137:origin] seq=%u %s qw=0x%x valid=%u oseq=%u cmdOff=0x%x srcOff=0x%x op=0x%02x imm=0x%04x dest=0x%x write=%u srcIdx=%u cl=%u wl=%u mode=%u tops=0x%x top=0x%x raw=%08x %08x %08x %08x src=%08x %08x %08x %08x\n",
                                 seq, label, qw, origin.valid, origin.seq,
                                 origin.cmdOff, origin.srcOff, origin.opcode, origin.imm,
                                 origin.destVec, origin.writeIndex, origin.srcIndex,
                                 origin.cl, origin.wl, origin.mode, origin.tops, origin.top,
                                 origin.raw[0], origin.raw[1], origin.raw[2], origin.raw[3],
                                 origin.srcRaw[0], origin.srcRaw[1], origin.srcRaw[2], origin.srcRaw[3]);
                }
            };

            auto g136DumpPacketQwords = [&](uint32_t seq, const uint8_t *pkt, uint32_t bytes)
            {
                const uint32_t capped = std::min<uint32_t>(bytes / 16u, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    uint32_t w[4] = {};
                    std::memcpy(w, pkt + q * 16u, sizeof(w));
                    std::fprintf(stderr,
                                 "[G136:xgqw] seq=%u q=%u %08x %08x %08x %08x\n",
                                 seq, q, w[0], w[1], w[2], w[3]);
                }
            };

            auto g217JoinXgkick = [&](const uint8_t *pkt, uint32_t bytes)
            {
                if (!s_g217Join || !pkt || bytes < 16u)
                    return;

                const uint64_t tex0Before = s_g217LastTex0;
                uint64_t curTex0 = tex0Before;
                uint64_t geomTex0 = 0u;
                uint32_t texWrites = 0u;
                uint32_t tags = 0u;
                uint32_t invalid = 0u;
                uint32_t curPrim = 0u;
                uint32_t verts[8] = {};
                uint32_t adcDraw[8] = {};
                uint32_t adcNoDraw[8] = {};
                uint32_t minX = 0xFFFFFFFFu, minY = 0xFFFFFFFFu;
                uint32_t maxX = 0u, maxY = 0u;
                uint32_t off = 0u;

                auto bindTex0 = [&](uint64_t value)
                {
                    curTex0 = value;
                    ++texWrites;
                };
                auto visitVertex = [&](uint64_t lo, uint64_t hi)
                {
                    const uint32_t prim = curPrim & 7u;
                    if (prim < 8u)
                    {
                        ++verts[prim];
                        if (((hi >> 47u) & 1ull) != 0ull)
                            ++adcNoDraw[prim];
                        else
                            ++adcDraw[prim];
                    }
                    if (geomTex0 == 0u)
                        geomTex0 = curTex0;
                    const uint32_t x = static_cast<uint32_t>(lo & 0xFFFFu);
                    const uint32_t y = static_cast<uint32_t>((lo >> 32u) & 0xFFFFu);
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                };

                for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                {
                    uint64_t tagLo = 0u, tagHi = 0u;
                    std::memcpy(&tagLo, pkt + off, 8u);
                    std::memcpy(&tagHi, pkt + off + 8u, 8u);
                    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
                    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                    if (nreg == 0u) nreg = 16u;
                    if (((tagLo >> 46u) & 1ull) != 0ull)
                        curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);
                    ++tags;
                    uint32_t dataOff = off + 16u;

                    if (flg == 0u)
                    {
                        for (uint32_t lp = 0u; lp < nloop; ++lp)
                        {
                            for (uint32_t r = 0u; r < nreg; ++r)
                            {
                                if (dataOff + 16u > bytes) { ++invalid; goto g217_done; }
                                const uint8_t rr = static_cast<uint8_t>((tagHi >> ((r & 0xFu) * 4u)) & 0xFu);
                                uint64_t lo = 0u, hi = 0u;
                                std::memcpy(&lo, pkt + dataOff, 8u);
                                std::memcpy(&hi, pkt + dataOff + 8u, 8u);
                                if (rr == 0x0Eu)
                                {
                                    const uint32_t adReg = static_cast<uint32_t>(hi & 0xFFu);
                                    if (adReg == 0x00u) curPrim = static_cast<uint32_t>(lo & 7u);
                                    else if (adReg == 0x06u || adReg == 0x07u) bindTex0(lo);
                                }
                                else if (rr == 0x00u) curPrim = static_cast<uint32_t>(lo & 7u);
                                else if (rr == 0x06u || rr == 0x07u) bindTex0(lo);
                                else if (rr == 0x04u || rr == 0x05u || rr == 0x0Du) visitVertex(lo, hi);
                                dataOff += 16u;
                            }
                        }
                    }
                    else if (flg == 1u)
                    {
                        const uint32_t count = nloop * nreg;
                        for (uint32_t k = 0u; k < count; ++k)
                        {
                            if (dataOff + 8u > bytes) { ++invalid; goto g217_done; }
                            const uint8_t rr = static_cast<uint8_t>((tagHi >> (((k % nreg) & 0xFu) * 4u)) & 0xFu);
                            uint64_t value = 0u;
                            std::memcpy(&value, pkt + dataOff, 8u);
                            if (rr == 0x00u) curPrim = static_cast<uint32_t>(value & 7u);
                            else if (rr == 0x06u || rr == 0x07u) bindTex0(value);
                            dataOff += 8u;
                        }
                        if ((count & 1u) != 0u) dataOff += 8u;
                    }
                    else if (flg == 2u)
                    {
                        const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                        if (imageBytes > static_cast<uint64_t>(bytes - dataOff)) { ++invalid; goto g217_done; }
                        dataOff += static_cast<uint32_t>(imageBytes);
                    }
                    else
                    {
                        ++invalid;
                        goto g217_done;
                    }

                    if (dataOff <= off || dataOff > bytes) { ++invalid; break; }
                    off = dataOff;
                    if (((tagLo >> 15u) & 1ull) != 0ull)
                        break;
                }

            g217_done:
                s_g217LastTex0 = curTex0;
                if (s_g217TagVu && s_g217ExactKind == 0u)
                    return;
                if (!s_g217CharExec)
                    return;
                const uint32_t logIndex = s_g217Logged.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (s_g217Limit != 0u && logIndex > s_g217Limit)
                    return;
                const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
                const uint64_t effectiveTex0 = geomTex0 != 0u ? geomTex0 : curTex0;
                const uint32_t totalVerts = verts[3] + verts[4] + verts[5];
                const uint32_t bboxMinX = minX == 0xFFFFFFFFu ? 0u : minX;
                const uint32_t bboxMinY = minY == 0xFFFFFFFFu ? 0u : minY;
                std::fprintf(stderr,
                    "[G217:join] n=%u tick=%llu exec=%u kick=%u exact=%u pc=0x%x bytes=0x%x tags=%u invalid=%u "
                    "texWrites=%u texBefore=0x%llx texAfter=0x%llx geomTex=0x%llx tbp=0x%x psm=0x%x "
                    "verts=%u tri=%u/%u tstrip=%u/%u tfan=%u/%u bbox=%u,%u..%u,%u "
                    "hdr=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x\n",
                    logIndex, static_cast<unsigned long long>(tick), s_g217ExecSeq, ++s_g217KickSeq,
                    s_g217ExactKind, m_state.pc, bytes, tags, invalid, texWrites,
                    static_cast<unsigned long long>(tex0Before),
                    static_cast<unsigned long long>(curTex0),
                    static_cast<unsigned long long>(effectiveTex0),
                    static_cast<uint32_t>(effectiveTex0 & 0x3FFFu),
                    static_cast<uint32_t>((effectiveTex0 >> 20u) & 0x3Fu), totalVerts,
                    adcDraw[3], adcNoDraw[3], adcDraw[4], adcNoDraw[4], adcDraw[5], adcNoDraw[5],
                    bboxMinX, bboxMinY, maxX, maxY,
                    s_g217Header[0], s_g217Header[1], s_g217Header[2], s_g217Header[3],
                    s_g217Header[4], s_g217Header[5], s_g217Header[6], s_g217Header[7]);
            };

            auto g136DumpXgkick = [&](const uint8_t *pkt, uint32_t bytes)
            {
                if (!pkt || bytes < 16u || !g136_trace_enabled())
                    return;
                const uint32_t activeSeq = g_dc2G136TraceActive.load(std::memory_order_relaxed);
                if (activeSeq == 0u && !g136_trace_all_enabled())
                    return;
                const bool titleScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
                const uint32_t pcFilter = g136_xg_pc_filter();
                const bool interestingPc = (pcFilter != 0xFFFFFFFFu)
                    ? (m_state.pc == pcFilter)
                    : (m_state.pc == 0x1c30u || m_state.pc == 0x1fd0u ||
                       m_state.pc == 0x1da0u || m_state.pc == 0x2180u);
                // G216: the same decoder is useful for the character RTT even though the
                // historical G136 scope was title-only. Both paths remain diagnostic-only.
                if ((!titleScope && !s_g216CharExec) || !interestingPc)
                    return;

                const uint32_t limit = g136_xg_limit();
                const uint32_t logIndex = s_g136XgLogged.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (limit != 0u && logIndex > limit)
                    return;
                const uint32_t seq = (activeSeq != 0u) ? activeSeq : 0xFFFFFFFFu;

                uint64_t primLoops[8] = {};
                uint64_t adcDraw[8] = {};
                uint64_t adcNoDraw[8] = {};
                uint64_t tstripPosDraw[10] = {};
                uint64_t tstripPosNoDraw[10] = {};
                uint64_t tstripLen[17] = {};
                uint32_t tags = 0u;
                uint32_t invalid = 0u;
                uint32_t curPrim = 0u;
                uint32_t off = 0u;

                for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                {
                    uint64_t tagLo = 0u;
                    uint64_t tagHi = 0u;
                    std::memcpy(&tagLo, pkt + off, 8u);
                    std::memcpy(&tagHi, pkt + off + 8u, 8u);
                    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
                    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    const bool pre = ((tagLo >> 46u) & 1ull) != 0ull;
                    const bool eop = ((tagLo >> 15u) & 1ull) != 0ull;
                    if (pre)
                        curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);

                    uint32_t pktSize = 16u;
                    if (flg == 0u)
                        pktSize += nloop * nreg * 16u;
                    else if (flg == 1u)
                    {
                        const uint32_t regs = nloop * nreg;
                        pktSize += regs * 8u;
                        if ((regs & 1u) != 0u)
                            pktSize += 8u;
                    }
                    else if (flg == 2u)
                        pktSize += nloop * 16u;

                    if (pktSize == 0u || off + pktSize > bytes)
                    {
                        ++invalid;
                        break;
                    }

                    if (tags < 6u)
                    {
                        std::fprintf(stderr,
                                     "[G136:xgtag] seq=%u tag=%u off=0x%x nloop=%u flg=%u nreg=%u pre=%u prim=%u eop=%u regs=0x%016llx size=0x%x\n",
                                     seq, tags, off, nloop, flg, nreg, pre ? 1u : 0u,
                                     curPrim, eop ? 1u : 0u,
                                     static_cast<unsigned long long>(tagHi), pktSize);
                    }
                    ++tags;
                    if (curPrim < 8u)
                        primLoops[curPrim] += nloop;

                    if (flg == 0u)
                    {
                        uint8_t regs[16] = {};
                        for (uint32_t r = 0u; r < nreg && r < 16u; ++r)
                            regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                        if ((curPrim & 7u) == 4u)
                        {
                            const uint32_t lenBucket = (nloop < 16u) ? nloop : 16u;
                            tstripLen[lenBucket] += 1u;
                        }

                        uint32_t dataOff = off + 16u;
                        uint32_t vertexIndex = 0u;
                        for (uint32_t lp = 0u; lp < nloop; ++lp)
                        {
                            for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                            {
                                if (dataOff + 16u > off + pktSize)
                                    break;
                                const uint8_t rr = regs[r & 0xFu];
                                uint64_t lo = 0u;
                                uint64_t hi = 0u;
                                std::memcpy(&lo, pkt + dataOff, 8u);
                                std::memcpy(&hi, pkt + dataOff + 8u, 8u);
                                if (rr == 0x0Eu)
                                {
                                    const uint32_t adReg = static_cast<uint32_t>(hi & 0xFFu);
                                    if (adReg == 0u)
                                        curPrim = static_cast<uint32_t>(lo & 0x7u);
                                }
                                else if (rr == 0x00u)
                                {
                                    curPrim = static_cast<uint32_t>(lo & 0x7u);
                                }
                                else if (rr == 0x04u || rr == 0x05u)
                                {
                                    const uint32_t prim = curPrim & 7u;
                                    const bool adc = ((hi >> 47u) & 1ull) != 0ull;
                                    if (prim < 8u)
                                    {
                                        if (adc)
                                            ++adcNoDraw[prim];
                                        else
                                            ++adcDraw[prim];
                                    }
                                    if (prim == 4u)
                                    {
                                        const uint32_t pos = (vertexIndex < 9u) ? vertexIndex : 9u;
                                        if (adc)
                                            ++tstripPosNoDraw[pos];
                                        else
                                            ++tstripPosDraw[pos];
                                    }
                                    ++vertexIndex;
                                }
                            }
                        }
                    }

                    if (eop)
                        break;
                    off += pktSize;
                }

                const uint32_t vi2 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[2]));
                const uint32_t vi12 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[12]));
                std::fprintf(stderr,
                             "[G136:xgsummary] seq=%u n=%u pc=0x%x startPC=0x%x addr=0x%x bytes=0x%x tags=%u invalid=%u vi2=0x%x vi12=0x%x vi1=0x%x vi7=0x%x vi8=0x%x vi10=0x%x itop=0x%x tex0=0x%llx tbp=0x%x texOff=0x%x primLoops tri=%llu tstrip=%llu tfan=%llu adcDraw/No tri=%llu/%llu tstrip=%llu/%llu tfan=%llu/%llu\n",
                             seq, logIndex, m_state.pc, s_g65CurStartPC, addr, bytes,
                             tags, invalid, vi2, vi12,
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[1])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[8])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[10])),
                             m_state.itop,
                             static_cast<unsigned long long>(g96Tex0),
                             static_cast<uint32_t>(g96Tex0 & 0x3FFFu), g96Off,
                             static_cast<unsigned long long>(primLoops[3]),
                             static_cast<unsigned long long>(primLoops[4]),
                             static_cast<unsigned long long>(primLoops[5]),
                             static_cast<unsigned long long>(adcDraw[3]),
                             static_cast<unsigned long long>(adcNoDraw[3]),
                             static_cast<unsigned long long>(adcDraw[4]),
                             static_cast<unsigned long long>(adcNoDraw[4]),
                             static_cast<unsigned long long>(adcDraw[5]),
                             static_cast<unsigned long long>(adcNoDraw[5]));
                std::fprintf(stderr,
                             "[G136:xgstrip] seq=%u len3=%llu len4=%llu len5=%llu len6=%llu len7=%llu len8=%llu len9=%llu len10plus=%llu pos0=%llu/%llu pos1=%llu/%llu pos2=%llu/%llu pos3=%llu/%llu pos4=%llu/%llu pos5plus=%llu/%llu\n",
                             seq,
                             static_cast<unsigned long long>(tstripLen[3]),
                             static_cast<unsigned long long>(tstripLen[4]),
                             static_cast<unsigned long long>(tstripLen[5]),
                             static_cast<unsigned long long>(tstripLen[6]),
                             static_cast<unsigned long long>(tstripLen[7]),
                             static_cast<unsigned long long>(tstripLen[8]),
                             static_cast<unsigned long long>(tstripLen[9]),
                             static_cast<unsigned long long>(tstripLen[10] + tstripLen[11] + tstripLen[12] + tstripLen[13] + tstripLen[14] + tstripLen[15] + tstripLen[16]),
                             static_cast<unsigned long long>(tstripPosDraw[0]), static_cast<unsigned long long>(tstripPosNoDraw[0]),
                             static_cast<unsigned long long>(tstripPosDraw[1]), static_cast<unsigned long long>(tstripPosNoDraw[1]),
                             static_cast<unsigned long long>(tstripPosDraw[2]), static_cast<unsigned long long>(tstripPosNoDraw[2]),
                             static_cast<unsigned long long>(tstripPosDraw[3]), static_cast<unsigned long long>(tstripPosNoDraw[3]),
                             static_cast<unsigned long long>(tstripPosDraw[4]), static_cast<unsigned long long>(tstripPosNoDraw[4]),
                             static_cast<unsigned long long>(tstripPosDraw[5] + tstripPosDraw[6] + tstripPosDraw[7] + tstripPosDraw[8] + tstripPosDraw[9]),
                             static_cast<unsigned long long>(tstripPosNoDraw[5] + tstripPosNoDraw[6] + tstripPosNoDraw[7] + tstripPosNoDraw[8] + tstripPosNoDraw[9]));
                g136DumpVuQwords(seq, "VI2", vi2, g136_qword_limit());
                g136DumpVuQwords(seq, "VI12", vi12, g136_qword_limit());
                g136DumpVuQwords(seq, "QW59", 59u, 1u);
                g137DumpOrigin(seq, "VI2", vi2, g136_qword_limit());
                g137DumpOrigin(seq, "VI12", vi12, g136_qword_limit());
                g137DumpOrigin(seq, "QW59", 59u, 1u);
                g136DumpPacketQwords(seq, pkt, bytes);
            };

            auto g96Submit = [&](const uint8_t *pkt, uint32_t bytes)
            {
                extern std::atomic<bool> g_dc2TitleRockScope;
                // G138: publish the packer PC for the PATH1 record the GS-stream dump is
                // about to write (see g138DumpSubmit in ps2_memory.cpp).
                {
                    extern std::atomic<uint32_t> g_dc2G138XgPc;
                    g_dc2G138XgPc.store(m_state.pc, std::memory_order_relaxed);
                }
                static const bool s_g106pc2180Tfan = (std::getenv("DC2_G106_PC2180_TFAN") != nullptr);
                static const bool s_g106pc2180Stat = (std::getenv("DC2_G106_PC2180_STAT") != nullptr);
                static const bool s_g107StripAdcStat = (std::getenv("DC2_G107_STRIP_ADC_STAT") != nullptr);
                static const bool s_g108AdcReplayStat = (std::getenv("DC2_G108_ADC_REPLAY_STAT") != nullptr);
                static const bool s_g108AdcReplayMatchLen = (std::getenv("DC2_G108_ADC_REPLAY_MATCH_LEN") != nullptr);
                static const uint32_t s_g134CopyExtentSkipPx = []() -> uint32_t {
                    const char *e = std::getenv("DC2_G134_COPY_EXTENT_SKIP");
                    if (!e)
                        return 0u;
                    if (e[0] == '\0' || std::strcmp(e, "1") == 0)
                        return 1024u;
                    if (std::strcmp(e, "0") == 0 || std::strcmp(e, "false") == 0 ||
                        std::strcmp(e, "FALSE") == 0 || std::strcmp(e, "off") == 0 ||
                        std::strcmp(e, "OFF") == 0)
                        return 0u;
                    const unsigned long v = std::strtoul(e, nullptr, 0);
                    return (v > 0ul && v < 8192ul) ? static_cast<uint32_t>(v) : 1024u;
                }();
                static const bool s_g134CopyExtentStat =
                    (std::getenv("DC2_G134_COPY_EXTENT_STAT") != nullptr);
                const bool titleScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
                const std::vector<std::string> &g108Replay = g108_adc_replay_sequences();
                const uint32_t g107Tbp =
                    (g96Tex0 != 0u) ? static_cast<uint32_t>(g96Tex0 & 0x3FFFu) : 0xFFFFFFFFu;
                const bool g107TbpMatch =
                    s_g107StripAdcTarget == 0xFFFFFFFFu || g107Tbp == s_g107StripAdcTarget;
                std::vector<uint8_t> g106Patched;
                if ((s_g106pc2180Tfan || (s_g107StripAdcFirst != 0u && g107TbpMatch) || !g108Replay.empty()) &&
                    m_state.pc == 0x2180u &&
                    titleScope)
                {
                    g106Patched.assign(pkt, pkt + bytes);
                    uint32_t off = 0u;
                    uint32_t patchedTags = 0u;
                    uint32_t patchedAdcVerts = 0u;
                    uint32_t replayPackets = 0u;
                    uint32_t replayVerts = 0u;
                    uint32_t replaySet = 0u;
                    uint32_t replayClear = 0u;
                    uint32_t replayMismatch = 0u;
                    for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                    {
                        uint64_t tagLo = 0u;
                        uint64_t tagHi = 0u;
                        std::memcpy(&tagLo, g106Patched.data() + off, 8u);
                        std::memcpy(&tagHi, g106Patched.data() + off + 8u, 8u);
                        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
                        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                        if (nreg == 0u)
                            nreg = 16u;
                        const bool pre = ((tagLo >> 46u) & 0x1ull) != 0ull;
                        const bool eop = ((tagLo >> 15u) & 0x1ull) != 0ull;
                        const uint32_t originalPrimType =
                            pre ? static_cast<uint32_t>((tagLo >> 47u) & 0x7u) : 0xFFFFFFFFu;
                        uint32_t pktSize = 16u;
                        if (flg == 0u)
                            pktSize += nloop * nreg * 16u;
                        else if (flg == 1u)
                        {
                            const uint32_t regs = nloop * nreg;
                            pktSize += regs * 8u;
                            if ((regs & 1u) != 0u)
                                pktSize += 8u;
                        }
                        else if (flg == 2u)
                            pktSize += nloop * 16u;

                        if (s_g107StripAdcFirst != 0u &&
                            g107TbpMatch &&
                            originalPrimType == 4u &&
                            flg == 0u &&
                            off + pktSize <= bytes)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                            uint32_t dataOff = off + 16u;
                            uint32_t vertexIndex = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x04u || rr == 0x05u)
                                    {
                                        if (vertexIndex < s_g107StripAdcFirst)
                                        {
                                            uint64_t vhi = 0u;
                                            std::memcpy(&vhi, g106Patched.data() + dataOff + 8u, 8u);
                                            if (((vhi >> 47u) & 1ull) == 0ull)
                                                ++patchedAdcVerts;
                                            vhi |= (1ull << 47u);
                                            std::memcpy(g106Patched.data() + dataOff + 8u, &vhi, 8u);
                                        }
                                        ++vertexIndex;
                                    }
                                }
                            }
                        }
                        if (!g108Replay.empty() &&
                            originalPrimType == 4u &&
                            flg == 0u &&
                            off + pktSize <= bytes)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                            uint32_t packetVerts = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                                for (uint32_t r = 0u; r < nreg; ++r)
                                    if (regs[r] == 0x04u || regs[r] == 0x05u)
                                        ++packetVerts;
                            static std::atomic<uint64_t> s_g108ReplayCursor{0};
                            uint64_t seqNo = s_g108ReplayCursor.load(std::memory_order_relaxed);
                            uint64_t advance = 0u;
                            if (s_g108AdcReplayMatchLen)
                            {
                                for (; advance < g108Replay.size(); ++advance)
                                {
                                    const std::string &candidate =
                                        g108Replay[static_cast<size_t>((seqNo + advance) % g108Replay.size())];
                                    if (candidate.size() == packetVerts)
                                        break;
                                }
                                if (advance == g108Replay.size())
                                    advance = 0u;
                            }
                            const std::string &pat =
                                g108Replay[static_cast<size_t>((seqNo + advance) % g108Replay.size())];
                            s_g108ReplayCursor.store(seqNo + advance + 1u, std::memory_order_relaxed);
                            uint32_t dataOff = off + 16u;
                            uint32_t vertexIndex = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x04u || rr == 0x05u)
                                    {
                                        uint64_t vhi = 0u;
                                        std::memcpy(&vhi, g106Patched.data() + dataOff + 8u, 8u);
                                        const bool wantAdc =
                                            vertexIndex < pat.size() && pat[vertexIndex] == '1';
                                        const bool oldAdc = ((vhi >> 47u) & 1ull) != 0ull;
                                        if (wantAdc)
                                        {
                                            vhi |= (1ull << 47u);
                                            if (!oldAdc)
                                                ++replaySet;
                                        }
                                        else
                                        {
                                            vhi &= ~(1ull << 47u);
                                            if (oldAdc)
                                                ++replayClear;
                                        }
                                        std::memcpy(g106Patched.data() + dataOff + 8u, &vhi, 8u);
                                        ++vertexIndex;
                                    }
                                }
                            }
                            if (vertexIndex != pat.size())
                                ++replayMismatch;
                            ++replayPackets;
                            replayVerts += vertexIndex;
                        }

                        if (pre)
                        {
                            uint32_t prim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);
                            if (s_g106pc2180Tfan && (prim & 0x7u) == 4u)
                            {
                                prim = (prim & ~0x7u) | 5u;
                                tagLo = (tagLo & ~(0x7FFull << 47u)) |
                                        (static_cast<uint64_t>(prim) << 47u);
                                std::memcpy(g106Patched.data() + off, &tagLo, 8u);
                                ++patchedTags;
                            }
                        }
                        if (pktSize == 0u || off + pktSize > bytes || eop)
                            break;
                        off += pktSize;
                    }
                    if (s_g106pc2180Stat)
                    {
                        static std::atomic<uint64_t> s_g106PatchCalls{0};
                        const uint64_t n = s_g106PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G106:pc2180] n=%llu bytes=0x%x patchedTags=%u\n",
                                         static_cast<unsigned long long>(n), bytes, patchedTags);
                    }
                    if (s_g107StripAdcStat && patchedAdcVerts != 0u)
                    {
                        static std::atomic<uint64_t> s_g107PatchCalls{0};
                        const uint64_t n = s_g107PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G107:stripadc] n=%llu bytes=0x%x tbp=0x%x first=%u patchedVerts=%u\n",
                                         static_cast<unsigned long long>(n), bytes,
                                         g107Tbp, s_g107StripAdcFirst, patchedAdcVerts);
                    }
                    if (s_g108AdcReplayStat && replayPackets != 0u)
                    {
                        static std::atomic<uint64_t> s_g108ReplayCalls{0};
                        const uint64_t n = s_g108ReplayCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G108:replay] n=%llu bytes=0x%x packets=%u verts=%u set=%u clear=%u mismatch=%u seqTotal=%zu\n",
                                         static_cast<unsigned long long>(n), bytes, replayPackets,
                                         replayVerts, replaySet, replayClear, replayMismatch,
                                         g108Replay.size());
                    }
                    pkt = g106Patched.data();
                }

                // G134: packet-level copy-packer extent restart diagnostic. Live HW confirmed the
                // four bit2=0 title descriptors still route through the copy path and their EE
                // vtable extension emits no geometry, so the remaining divergence is the copy
                // XGKICK packet itself. Default-off: set ADC on the vertex completing any title
                // copy tristrip triangle whose screen-space extent crosses the configured pixel
                // threshold (DC2_G134_COPY_EXTENT_SKIP, "1" = 1024px).
                std::vector<uint8_t> g134Patched;
                if (s_g134CopyExtentSkipPx != 0u &&
                    m_state.pc == 0x1c30u &&
                    titleScope)
                {
                    g134Patched.assign(pkt, pkt + bytes);
                    const uint32_t thresholdRaw = s_g134CopyExtentSkipPx * 16u;
                    uint32_t off = 0u;
                    uint32_t stripTags = 0u;
                    uint32_t triSeen = 0u;
                    uint32_t patchedVerts = 0u;
                    uint32_t existingAdc = 0u;
                    uint32_t invalidTags = 0u;
                    uint32_t curPrim = 0u;
                    for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                    {
                        uint64_t tagLo = 0u;
                        uint64_t tagHi = 0u;
                        std::memcpy(&tagLo, g134Patched.data() + off, 8u);
                        std::memcpy(&tagHi, g134Patched.data() + off + 8u, 8u);
                        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
                        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                        if (nreg == 0u)
                            nreg = 16u;
                        const bool pre = ((tagLo >> 46u) & 0x1ull) != 0ull;
                        const bool eop = ((tagLo >> 15u) & 0x1ull) != 0ull;
                        if (pre)
                            curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);
                        uint32_t pktSize = 16u;
                        if (flg == 0u)
                            pktSize += nloop * nreg * 16u;
                        else if (flg == 1u)
                        {
                            const uint32_t regs = nloop * nreg;
                            pktSize += regs * 8u;
                            if ((regs & 1u) != 0u)
                                pktSize += 8u;
                        }
                        else if (flg == 2u)
                            pktSize += nloop * 16u;

                        if (pktSize == 0u || off + pktSize > bytes)
                        {
                            ++invalidTags;
                            break;
                        }

                        if (flg == 0u)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);

                            struct G134Vertex
                            {
                                uint32_t x;
                                uint32_t y;
                            };
                            G134Vertex prev2[2] = {};
                            uint32_t vertexIndex = 0u;
                            uint32_t activePrim = curPrim;
                            bool countedStripTag = false;
                            uint32_t dataOff = off + 16u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x00u)
                                    {
                                        uint64_t primData = 0u;
                                        std::memcpy(&primData, g134Patched.data() + dataOff, 8u);
                                        activePrim = static_cast<uint32_t>(primData & 0x7u);
                                        curPrim = activePrim;
                                        prev2[0] = {};
                                        prev2[1] = {};
                                        vertexIndex = 0u;
                                        continue;
                                    }
                                    if (rr != 0x04u && rr != 0x05u)
                                        continue;
                                    if ((activePrim & 0x7u) != 4u)
                                        continue;
                                    if (!countedStripTag)
                                    {
                                        ++stripTags;
                                        countedStripTag = true;
                                    }

                                    uint64_t vlo = 0u;
                                    uint64_t vhi = 0u;
                                    std::memcpy(&vlo, g134Patched.data() + dataOff, 8u);
                                    std::memcpy(&vhi, g134Patched.data() + dataOff + 8u, 8u);
                                    const G134Vertex cur = {
                                        static_cast<uint32_t>(vlo & 0xFFFFu),
                                        static_cast<uint32_t>((vlo >> 32u) & 0xFFFFu)
                                    };

                                    if (vertexIndex >= 2u)
                                    {
                                        ++triSeen;
                                        uint32_t minX = prev2[0].x;
                                        uint32_t maxX = prev2[0].x;
                                        uint32_t minY = prev2[0].y;
                                        uint32_t maxY = prev2[0].y;
                                        if (prev2[1].x < minX) minX = prev2[1].x;
                                        if (cur.x < minX) minX = cur.x;
                                        if (prev2[1].x > maxX) maxX = prev2[1].x;
                                        if (cur.x > maxX) maxX = cur.x;
                                        if (prev2[1].y < minY) minY = prev2[1].y;
                                        if (cur.y < minY) minY = cur.y;
                                        if (prev2[1].y > maxY) maxY = prev2[1].y;
                                        if (cur.y > maxY) maxY = cur.y;
                                        const uint32_t extentRaw =
                                            ((maxX - minX) > (maxY - minY)) ? (maxX - minX) : (maxY - minY);
                                        if (extentRaw >= thresholdRaw)
                                        {
                                            const bool oldAdc = ((vhi >> 47u) & 1ull) != 0ull;
                                            if (oldAdc)
                                                ++existingAdc;
                                            else
                                                ++patchedVerts;
                                            vhi |= (1ull << 47u);
                                            std::memcpy(g134Patched.data() + dataOff + 8u, &vhi, 8u);
                                        }
                                    }
                                    prev2[0] = prev2[1];
                                    prev2[1] = cur;
                                    ++vertexIndex;
                                }
                            }
                        }

                        if (eop)
                            break;
                        off += pktSize;
                    }

                    if (s_g134CopyExtentStat || patchedVerts != 0u)
                    {
                        static std::atomic<uint64_t> s_g134PatchCalls{0};
                        static std::atomic<uint64_t> s_g134PatchVerts{0};
                        const uint64_t n = s_g134PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t totalPatch =
                            s_g134PatchVerts.fetch_add(patchedVerts, std::memory_order_relaxed) + patchedVerts;
                        if (n <= 96u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G134:copyextskip] n=%llu bytes=0x%x threshPx=%u tags=%u tri=%u patched=%u existing=%u invalid=%u totalPatched=%llu\n",
                                         static_cast<unsigned long long>(n), bytes, s_g134CopyExtentSkipPx,
                                         stripTags, triSeen, patchedVerts, existingAdc, invalidTags,
                                         static_cast<unsigned long long>(totalPatch));
                    }
                    pkt = g134Patched.data();
                }

                if (s_g96bind && g96Tex0 != 0u)
                {
                    // Prepend GIFtag(NLOOP=1,EOP=0,NREG=1,REGS=A+D) + (TEX0 | reg 0x06) + original.
                    // EOP=0 so the GS continues into the original geometry tags after the bind.
                    std::vector<uint8_t> buf((size_t)bytes + 32u);
                    const uint64_t tagLo = 1ull | (1ull << 60); // nloop=1, nreg=1, eop=0
                    const uint64_t tagHi = 0x0Eull;             // packed A+D
                    const uint64_t adData = g96Tex0;
                    const uint64_t adAddr = 0x06ull;            // GS TEX0_1
                    std::memcpy(buf.data() + 0u, &tagLo, 8);
                    std::memcpy(buf.data() + 8u, &tagHi, 8);
                    std::memcpy(buf.data() + 16u, &adData, 8);
                    std::memcpy(buf.data() + 24u, &adAddr, 8);
                    std::memcpy(buf.data() + 32u, pkt, bytes);
                    if (g_g297KickCapture) // G297 MTVU: capture on the worker, splice on the EE
                        g_g297KickCapture->emplace_back(buf.data(), buf.data() + bytes + 32u);
                    else if (memory)
                        memory->submitGifPacket(GifPathId::Path1, buf.data(), bytes + 32u, !g95_defer_gif_enabled());
                    else
                        gs.processGIFPacket(buf.data(), bytes + 32u);
                }
                else
                {
                    // G217 A/B: the missing MAP-4 FixMDT head/cap use the copy packer.  If a
                    // character-RTT packed tag marks every vertex ADC/no-draw, preserve the two
                    // strip-restart vertices and clear ADC on the remaining vertices.  This is
                    // diagnostic-only and deliberately broad within fbp=0x139; it determines
                    // whether the residual is output ADC versus projected position/submission.
                    static const bool s_g217ForceCopyDraw =
                        (std::getenv("DC2_G217_FORCE_COPY_DRAW") != nullptr);
                    std::vector<uint8_t> g217Patched;
                    if (s_g217ForceCopyDraw && s_g217CharExec && m_state.pc == 0x1c30u)
                    {
                        g217Patched.assign(pkt, pkt + bytes);
                        uint32_t off = 0u;
                        uint32_t changed = 0u;
                        for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                        {
                            uint64_t tagLo = 0u, tagHi = 0u;
                            std::memcpy(&tagLo, g217Patched.data() + off, 8u);
                            std::memcpy(&tagHi, g217Patched.data() + off + 8u, 8u);
                            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7fffu);
                            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 3u);
                            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xfu);
                            if (nreg == 0u) nreg = 16u;
                            uint32_t pktSize = 16u;
                            if (flg == 0u) pktSize += nloop * nreg * 16u;
                            else if (flg == 1u)
                            {
                                const uint32_t regs = nloop * nreg;
                                pktSize += regs * 8u + ((regs & 1u) ? 8u : 0u);
                            }
                            else if (flg == 2u) pktSize += nloop * 16u;
                            if (pktSize == 0u || off + pktSize > bytes) break;

                            if (flg == 0u)
                            {
                                uint32_t vertexCount = 0u;
                                uint32_t adcCount = 0u;
                                uint32_t dataOff = off + 16u;
                                for (uint32_t lp = 0u; lp < nloop; ++lp)
                                {
                                    for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                    {
                                        const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xfu);
                                        if (reg != 0x04u && reg != 0x05u && reg != 0x0du) continue;
                                        uint64_t hi = 0u;
                                        std::memcpy(&hi, g217Patched.data() + dataOff + 8u, 8u);
                                        ++vertexCount;
                                        if (((hi >> 47u) & 1ull) != 0ull) ++adcCount;
                                    }
                                }
                                if (vertexCount >= 3u && adcCount == vertexCount)
                                {
                                    dataOff = off + 16u;
                                    uint32_t vertexIndex = 0u;
                                    for (uint32_t lp = 0u; lp < nloop; ++lp)
                                    {
                                        for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                        {
                                            const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xfu);
                                            if (reg != 0x04u && reg != 0x05u && reg != 0x0du) continue;
                                            if (vertexIndex++ < 2u) continue;
                                            uint64_t hi = 0u;
                                            std::memcpy(&hi, g217Patched.data() + dataOff + 8u, 8u);
                                            hi &= ~(1ull << 47u);
                                            std::memcpy(g217Patched.data() + dataOff + 8u, &hi, 8u);
                                            ++changed;
                                        }
                                    }
                                }
                            }
                            if (((tagLo >> 15u) & 1ull) != 0ull) break;
                            off += pktSize;
                        }
                        if (changed != 0u)
                        {
                            static std::atomic<uint32_t> s_g217ForceLogs{0};
                            const uint32_t n = s_g217ForceLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (n <= 128u)
                                std::fprintf(stderr, "[G217:forcecopy] n=%u pc=0x%x changed=%u bytes=0x%x\n",
                                             n, m_state.pc, changed, bytes);
                            pkt = g217Patched.data();
                        }
                    }
                    if (g_g297KickCapture) // G297 MTVU: capture on the worker, splice on the EE
                        g_g297KickCapture->emplace_back(pkt, pkt + bytes);
                    else if (memory)
                        memory->submitGifPacket(GifPathId::Path1, pkt, bytes, !g95_defer_gif_enabled());
                    else
                        gs.processGIFPacket(pkt, bytes);
                }
            };

            if (s_g215batch || s_g216clip || s_g216ForceGate || s_g217TagVu)
            {
                ++s_g215KickCount;
                s_g215KickBytes += totalBytes;
            }
            if (addr + totalBytes <= dataSize)
            {
                g217JoinXgkick(vuData + addr, totalBytes);
                g136DumpXgkick(vuData + addr, totalBytes);
                f50_12_vu_scan_xgkick_packet(addr, totalBytes, vuData + addr);
                g67_vu_scan_xgkick_packet(addr, totalBytes, vuData + addr, m_state.pc, s_g65CurStartPC, m_state.itop);
                vu1_dump_xgkick(addr, totalBytes, vuData + addr);
                g96Submit(vuData + addr, totalBytes);
            }
            else
            {
                std::vector<uint8_t> wrappedPacket(totalBytes);
                for (uint32_t i = 0; i < totalBytes; ++i)
                {
                    wrappedPacket[i] = vuData[wrapOffset(addr + i)];
                }

                g217JoinXgkick(wrappedPacket.data(), totalBytes);
                g136DumpXgkick(wrappedPacket.data(), totalBytes);
                f50_12_vu_scan_xgkick_packet(addr, totalBytes, wrappedPacket.data());
                g67_vu_scan_xgkick_packet(addr, totalBytes, wrappedPacket.data(), m_state.pc, s_g65CurStartPC, m_state.itop);
                vu1_dump_xgkick(addr, totalBytes, wrappedPacket.data());
                g96Submit(wrappedPacket.data(), totalBytes);
            }
            return;
        }
        case 0x7E: // legacy-unreachable XTOP - DATA top (VIF1 TOPS), not ITOP
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(memory ? (memory->vif1_regs.top & 0x3FFu)
                                                  : (m_state.itop & 0x3FFu));
            return;
        }
        case 0x7F: // legacy-unreachable XITOP - INTEGER top
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(m_state.itop & 0x3FFu);
            return;
        }
        default:
            g22_log_unknown_lower(m_state.pc, instr, "lower-special", opHi, funct);
            return;
        }
    }
    default:
        g22_log_unknown_lower(m_state.pc, instr, "primary", opHi, 0u);
        break;
    }
}
