void VU1Interpreter::execUpper(uint32_t instr)
{
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    const float *vs = m_state.vf[fs];
    const float *vt = m_state.vf[ft];
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > m_state.i) ? vs[c] : m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < m_state.i) ? vs[c] : m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = m_state.acc[0] - vs[1] * vt[2];
        result[1] = m_state.acc[1] - vs[2] * vt[0];
        result[2] = m_state.acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;

    // Special1 group (0x3C..0x3F with secondary field)
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t vuFunc = S2_FUNC(instr);

        if (vuFunc <= 0x0Fu)
        {
            const float bc = broadcast(vt, vuFunc & 3u);
            const uint8_t group = vuFunc >> 2u;
            for (int c = 0; c < 4; c++)
            {
                if (group == 0u) // ADDAx/y/z/w
                    result[c] = vs[c] + bc;
                else if (group == 1u) // SUBAx/y/z/w
                    result[c] = vs[c] - bc;
                else if (group == 2u) // MADDAx/y/z/w
                    result[c] = m_state.acc[c] + vs[c] * bc;
                else // MSUBAx/y/z/w
                    result[c] = m_state.acc[c] - vs[c] * bc;
            }
            applyDestAcc(result, dest);
            return;
        }

        if (vuFunc >= 0x10u && vuFunc <= 0x13u) // ITOF0/4/12/15
        {
            const float scale = (vuFunc == 0x10u) ? 1.0f :
                                (vuFunc == 0x11u) ? (1.0f / 16.0f) :
                                (vuFunc == 0x12u) ? (1.0f / 4096.0f) :
                                                     (1.0f / 32768.0f);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &vs[c], 4);
                result[c] = (float)iv * scale;
            }
            // G32: ITOF/FTOI/ABS are 2-operand VU ops; destination is ft (bits 20:16),
            // NOT fd. Writing fd left the model's XYZF2 source VF (ft) stale -> every packed
            // vertex shared one stale position -> model collapsed to a point. (VU1 is only
            // used by the skinned-model path; the dungeon/title use the VIF1 DIRECT path.)
            applyDest(m_state.vf[ft], result, dest);
            return;
        }

        if (vuFunc >= 0x14u && vuFunc <= 0x17u) // FTOI0/4/12/15
        {
            const float scale = (vuFunc == 0x14u) ? 1.0f :
                                (vuFunc == 0x15u) ? 16.0f :
                                (vuFunc == 0x16u) ? 4096.0f :
                                                     32768.0f;
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = (int32_t)(vs[c] * scale);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(m_state.vf[ft], result, dest); // G32: dest = ft, not fd
            return;
        }

        if (vuFunc >= 0x18u && vuFunc <= 0x1Bu) // MULAx/y/z/w
        {
            const float bc = broadcast(vt, vuFunc & 3u);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyDestAcc(result, dest);
            return;
        }

        switch (vuFunc)
        {
        case 0x1C: // MULAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(m_state.vf[ft], result, dest); // G32: ABS dest = ft, not fd
            return;
        case 0x1E: // MULAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x1F: // CLIPw
        {
            float w = std::fabs(vt[3]);
            uint32_t flags = 0;
            if (vs[0] > +w)
                flags |= 0x01;
            if (vs[0] < -w)
                flags |= 0x02;
            if (vs[1] > +w)
                flags |= 0x04;
            if (vs[1] < -w)
                flags |= 0x08;
            if (vs[2] > +w)
                flags |= 0x10;
            if (vs[2] < -w)
                flags |= 0x20;
            m_state.clip = (m_state.clip << 6) | flags;
            // G65: aggregate CLIPw outcomes per program startPC so we can tell whether the
            // title's geometry genuinely projects outside the ±W clip volume (all flags set =
            // wrong projection/camera) vs a downstream ADC bug. Sample a few (xyz,w).
            static const bool s_g65clip = (std::getenv("DC2_G65_CLIP") != nullptr);
            if (s_g65clip)
            {
                static std::atomic<uint64_t> s_tot{0}, s_clipped{0}, s_wneg{0}, s_samp{0};
                s_tot.fetch_add(1u, std::memory_order_relaxed);
                if (flags) s_clipped.fetch_add(1u, std::memory_order_relaxed);
                if (vt[3] <= 0.0f) s_wneg.fetch_add(1u, std::memory_order_relaxed);
                if (s_samp.fetch_add(1u, std::memory_order_relaxed) < 16u)
                    std::fprintf(stderr, "[G65:clip] startPC=0x%x flags=0x%02x vs=(% .3g % .3g % .3g) w=% .3g\n",
                                 s_g65CurStartPC, flags, vs[0], vs[1], vs[2], vt[3]);
                const uint64_t t = s_tot.load(std::memory_order_relaxed);
                if ((t % 20000u) == 0u)
                    std::fprintf(stderr, "[G65:clipagg] startPC=0x%x total=%llu clipped=%llu wNegOrZero=%llu\n",
                                 s_g65CurStartPC, (unsigned long long)t,
                                 (unsigned long long)s_clipped.load(), (unsigned long long)s_wneg.load());
            }
            return;
        }
        case 0x20: // ADDAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = m_state.acc[3];
            applyDestAcc(result, dest);
            return;
        case 0x2F: // VNOP
            return;
        case 0x30: // MOVE
            applyDest(m_state.vf[ft], vs, dest);
            return;
        case 0x31: // MR32
        {
            float tmp[4] = {vs[1], vs[2], vs[3], vs[0]};
            applyDest(m_state.vf[ft], tmp, dest);
            return;
        }
        default:
            g22_log_unknown_upper(m_state.pc, instr, "special2", op, vuFunc);
            return;
        }

        uint8_t special = (instr >> 6) & 0x1F;
        uint8_t sop = (instr & 0x3) | ((instr >> 4) & 0x3C);
        (void)sop;

        switch (instr & 0x3F)
        {
        case 0x3C: // Special1 (ADDAx..ADDAw, SUBAx..SUBAw, MADDAx..MADDAw, MSUBAx..MSUBAw, etc.)
        {
            uint8_t funct = (instr >> 6) & 0x1F;
            uint8_t bc2 = (instr >> 0) & 0x3;
            (void)bc2;
            switch (funct)
            {
            case 0x00:
            case 0x01:
            case 0x02:
            case 0x03: // ADDAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x04:
            case 0x05:
            case 0x06:
            case 0x07: // SUBAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x08:
            case 0x09:
            case 0x0A:
            case 0x0B: // MADDAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x0C:
            case 0x0D:
            case 0x0E:
            case 0x0F: // MSUBAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x10: // ITOF0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv;
                }
                applyDest(vd, result, dest);
                return;
            case 0x11: // ITOF4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 16.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x12: // ITOF12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 4096.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x13: // ITOF15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 32768.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x14: // FTOI0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)vs[c];
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x15: // FTOI4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 16.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x16: // FTOI12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 4096.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x17: // FTOI15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 32768.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x18:
            case 0x19:
            case 0x1A:
            case 0x1B: // MULAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x1C: // MULAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x1D: // ABS
                for (int c = 0; c < 4; c++)
                    result[c] = std::fabs(vs[c]);
                applyDest(vd, result, dest);
                return;
            case 0x1E: // MULAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x1F: // CLIP
            {
                float w = std::fabs(vt[3]);
                uint32_t flags = 0;
                if (vs[0] > +w)
                    flags |= 0x01;
                if (vs[0] < -w)
                    flags |= 0x02;
                if (vs[1] > +w)
                    flags |= 0x04;
                if (vs[1] < -w)
                    flags |= 0x08;
                if (vs[2] > +w)
                    flags |= 0x10;
                if (vs[2] < -w)
                    flags |= 0x20;
                m_state.clip = (m_state.clip << 6) | flags;
                return;
            }
            default:
                g22_log_unknown_upper(m_state.pc, instr, "special1", instr & 0x3Fu, funct);
                return;
            }
        }
        case 0x3D: // Special2 (ADDAq, MADDAq, ADDAi, MADDAi, ADDA, MADDA, MULA, OPMULA, ...)
        {
            uint8_t funct = (instr >> 6) & 0x1F;
            switch (funct)
            {
            case 0x00: // ADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x01: // MADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x02: // ADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x03: // MADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x04: // SUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x05: // MSUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x06: // SUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x07: // MSUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x08: // ADDA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x09: // MADDA
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0A: // MULA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0C: // SUBA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0D: // MSUBA
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0E: // OPMULA
                result[0] = vs[1] * vt[2];
                result[1] = vs[2] * vt[0];
                result[2] = vs[0] * vt[1];
                result[3] = 0.0f;
                applyDestAcc(result, dest);
                return;
            case 0x0F: // NOP
                return;
            default:
                g22_log_unknown_upper(m_state.pc, instr, "special2", instr & 0x3Fu, funct);
                return;
            }
        }
        case 0x3E: // Special (more upper ops, rarely used)
            g22_log_unknown_upper(m_state.pc, instr, "special3e", op, special);
            return;
        case 0x3F: // Special (upper NOP typically)
            return;
        }
        return;
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33: // iadd-like upper? No, these are valid upper ops
    default:
        // NOP / unimplemented upper
        g22_log_unknown_upper(m_state.pc, instr, "primary", op, 0u);
        return;
    }
}

// ============================================================================
// Lower instructions
// ============================================================================
