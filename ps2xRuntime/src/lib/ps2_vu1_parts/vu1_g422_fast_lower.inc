// G422 - inline execution of the VU1 lower slot.
//
// PREMISE (current-binary, this phase, per the standing "attribute to the THREAD first" rule).
// DC2_G303_INSTR=1 + DC2_G182_EE_STAT=1 on the shipped G421 binary, lean MAP-0:
//     VU1 worker busyMs/f 61.6-64.3 | GS worker 58.8-61.7 | frame 64.7-68.0 | gsStall 0.0
// The VU1 worker is still the pole, but only by ~2.5-3 ms, so this phase's payoff is capped by
// the GS worker taking over — that cap is a property of the frame, not of the lever.
//
// SIZING (DC2_G421_CENSUS=1, MAP-0, 1.02 G pairs):
//     lowerExec = 60.0% of pairs, lowerNop = 39.3%, iBit = 0.7%
// i.e. execLower() is CALLED on 99.3% of ~1.63 M pairs/frame, and 39.3% of those calls do
// nothing at all: the function's very first statement is `if (instr == 0 || instr == 0x8000033C)
// return;`. That call is not free — it is a 6-argument call (three arguments spill to the stack
// under the Windows x64 convention) into a non-inlined function. G410 already priced this exact
// boundary at ~8.6 ns per skipped execUpper() call.
//
// MECHANISM. Predecode the lower word once, into the byte the G410 sidecar already carries per
// pair, and execute the covered kinds inline in run():
//   * NOP                                    -> the call disappears entirely (39.3% of pairs)
//   * integer ALU  IADD/ISUB/IADDI/IAND/IOR/IADDIU/ISUBIU
//   * VU-memory    LQ/SQ/ILW/ISW/LQI/SQI/LQD/SQD/ILWR/ISWR   (SQI 8.2% + LQI 6.8% of all pairs)
//   * moves        MOVE/MR32/MTIR/MFIR
//   * branches     B/BAL/JR/JALR/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ
// Everything else (DIV/SQRT/RSQRT and the whole Q/P pipeline, XGKICK, WAITQ/WAITP, the EFU
// group, the flag ops FCxx/FSxx/FMxx, RNEXT/RGET/RINIT/RXOR, MFP, XTOP/XITOP) keeps the legacy
// execLower() path untouched, so no scalar-pipeline, flag-pipeline or GS-kick semantics move.
//
// EXACTNESS is by construction: each covered body below is a verbatim transcription of the
// corresponding case in VU1Interpreter::execLower (vu1_dispatch_and_sync.inc), including the
// address masking, the `addr + 16 <= dataSize` guard, the `!= 0` register-write guards, the
// int16 truncations and the read-modify-write tmp[4] staging that makes a masked store exact
// when it and is alias. The legacy `applyDest()` member is transcribed as its four conditional
// lane stores. Nothing is re-ordered and no expression is re-associated.
//
// The covered handlers also contain PC-scoped diagnostic hooks (G31 LQI trace, G133 copy-packer
// ADC skip inside SQI, G370 load/store/div watches, G63/G64 IAND gate). Every one of them is
// env-gated and OFF by default; the fast path does not carry them, so arming any of those envs
// (or anything that already trips the G295 master diagnostic gate) routes the whole slot back
// through execLower().
//
// Status:   PROMOTED default-on.
// Kill:     DC2_G422_NO_FAST_LOWER=1  (restores the legacy execLower path exactly)
// Verify:   DC2_G422_VERIFY=1  (shadow-executes legacy execLower from the same pre-state and
//                               compares the whole VU1State *and* all 16 KB of VU data memory)
// G480 NOP-skip paired gate (defined in ps2_gs_rasterizer_parts/g419_ab_instrument.inc).
// -1 = this lever is not selected (honour DC2_G480_NO_NOPSKIP), 0 = control, 1 = candidate.
extern int g480NopSkipArm();
// G481 hot-kind inline paired gate (same file). -1 = not selected, 0 = control, 1 = candidate.
extern int g481HotLowerArm();

namespace
{
    // Dense kind space, so the dispatch below compiles to one jump table.
    enum : uint8_t
    {
        G422_LK_NONE = 0u, // not covered -> execLower()
        G422_LK_NOP,
        G422_LK_LQ,
        G422_LK_SQ,
        G422_LK_ILW,
        G422_LK_ISW,
        G422_LK_IADDIU,
        G422_LK_ISUBIU,
        G422_LK_B,
        G422_LK_BAL,
        G422_LK_JR,
        G422_LK_JALR,
        G422_LK_IBEQ,
        G422_LK_IBNE,
        G422_LK_IBLTZ,
        G422_LK_IBGTZ,
        G422_LK_IBLEZ,
        G422_LK_IBGEZ,
        G422_LK_IADD,
        G422_LK_ISUB,
        G422_LK_IADDI,
        G422_LK_IAND,
        G422_LK_IOR,
        G422_LK_MOVE,
        G422_LK_MR32,
        G422_LK_LQI,
        G422_LK_SQI,
        G422_LK_LQD,
        G422_LK_SQD,
        G422_LK_MTIR,
        G422_LK_MFIR,
        G422_LK_ILWR,
        G422_LK_ISWR,
        // G487: the FLAG CONSUMERS. `[G486:uncov]` measured the population that still crosses
        // execLower's call boundary and it is only two things — these four (2.54% of all pairs,
        // ~41 k/frame) and DIV (1.68%, which carries the Q-latency staging and is therefore not
        // a candidate). Their bodies are three or four instructions on values the pair loop has
        // ALREADY resolved: `m_state.clip` for FCAND/FCOR/FCGET and `s_vuMacVisible` for FMAND,
        // which the loop publishes exactly for these pairs (G427_PF_FLAGREAD).
        // These four MUST stay after every kind above: `g475TracePairEligible` and the census's
        // `g475Safe` classify "control transfer" as the closed range [G422_LK_B, G422_LK_IBGEZ],
        // and both now exclude this range explicitly (a flag reader is not trace-safe).
        G422_LK_FCAND,  // opHi 0x12 - reads m_state.clip
        G422_LK_FCOR,   // opHi 0x13 - reads m_state.clip
        G422_LK_FMAND,  // opHi 0x1A - reads the G138 MAC delay line (s_vuMacVisible)
        G422_LK_FCGET,  // opHi 0x1C - reads m_state.clip (default s_fmFixed mapping)
        G422_LK_COUNT
    };

    // G487: the first covered kind that reads flag state. Everything below it is branch-free and
    // flag-reader-free, which is the contract G475's trace runway depends on.
    constexpr uint8_t G422_LK_FIRST_FLAG = G422_LK_FCAND;

    // G487: the flag-consumer kinds are only ever produced when the runtime is in the exact
    // configuration their transcribed bodies assume. Both conditions are process-wide constants
    // read from the environment once, so this is a compile-once property of the run, not a
    // per-pair test:
    //   * `s_fmFixed` (DC2_VU1_NO_FMSWAPFIX unset) — the G138 opcode mapping the bodies below
    //     transcribe. Under the legacy mapping FMAND and FMEQ swap and FCGET reads MAC, so the
    //     bodies would be wrong.
    //   * `g138_macpipe_enabled()` (DC2_VU1_NO_MACPIPE unset) — FMAND's `macRead` source. With the
    //     pipe off, execLower reads `s_g106LowerMacRead`/`m_state.mac` through a chain this path
    //     does not reproduce.
    // Either one non-default and every flag consumer decodes to G422_LK_NONE, i.e. straight back
    // onto the interpreter. Note this is deliberately NOT gated on g422LowerDiagFree(): none of
    // the eight probe envs listed there lives in a flag-consumer body, and any of them already
    // forces the whole slot back through execLower at the caller.
    bool g487FlagLowerSupported()
    {
        static const bool s_ok = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr) &&
                                 g138_macpipe_enabled();
        return s_ok;
    }

    // Pure function of the lower word AND of the flag-consumer arm the G410 pair cache was built
    // with. Called once per pair at G410 compile time (and by the G410 cache verifier, and by the
    // census), never in the hot loop — which is exactly why the arm has to be a property of the
    // CACHE and not an in-loop compare (G486 §4: an extra per-pair test would sit on the control
    // arm on 59.9% of pairs and bias the gate toward the candidate).
    uint8_t g422DecodeLowerKind(uint32_t lower, bool flagLower)
    {
        if (lower == 0x00000000u || lower == 0x8000033Cu)
            return G422_LK_NOP;

        switch ((lower >> 25u) & 0x7Fu)
        {
        case 0x12u: return flagLower ? G422_LK_FCAND : G422_LK_NONE;
        case 0x13u: return flagLower ? G422_LK_FCOR : G422_LK_NONE;
        case 0x1Au: return flagLower ? G422_LK_FMAND : G422_LK_NONE;
        case 0x1Cu: return flagLower ? G422_LK_FCGET : G422_LK_NONE;
        case 0x00u: return G422_LK_LQ;
        case 0x01u: return G422_LK_SQ;
        case 0x04u: return G422_LK_ILW;
        case 0x05u: return G422_LK_ISW;
        case 0x08u: return G422_LK_IADDIU;
        case 0x09u: return G422_LK_ISUBIU;
        case 0x20u: return G422_LK_B;
        case 0x21u: return G422_LK_BAL;
        case 0x24u: return G422_LK_JR;
        case 0x25u: return G422_LK_JALR;
        case 0x28u: return G422_LK_IBEQ;
        case 0x29u: return G422_LK_IBNE;
        case 0x2Cu: return G422_LK_IBLTZ;
        case 0x2Du: return G422_LK_IBGTZ;
        case 0x2Eu: return G422_LK_IBLEZ;
        case 0x2Fu: return G422_LK_IBGEZ;
        case 0x40u:
            switch (lower & 0x3Fu)
            {
            case 0x30u: return G422_LK_IADD;
            case 0x31u: return G422_LK_ISUB;
            case 0x32u: return G422_LK_IADDI;
            case 0x34u: return G422_LK_IAND;
            case 0x35u: return G422_LK_IOR;
            case 0x3Cu:
            case 0x3Du:
            case 0x3Eu:
            case 0x3Fu:
                switch (S2_FUNC(lower))
                {
                case 0x30u: return G422_LK_MOVE;
                case 0x31u: return G422_LK_MR32;
                case 0x34u: return G422_LK_LQI;
                case 0x35u: return G422_LK_SQI;
                case 0x36u: return G422_LK_LQD;
                case 0x37u: return G422_LK_SQD;
                case 0x3Cu: return G422_LK_MTIR;
                case 0x3Du: return G422_LK_MFIR;
                case 0x3Eu: return G422_LK_ILWR;
                case 0x3Fu: return G422_LK_ISWR;
                default: return G422_LK_NONE;
                }
            default: return G422_LK_NONE;
            }
        default: return G422_LK_NONE;
        }
    }

    // The kinds that write VU data memory. Only these need the (expensive) memory leg of the
    // shadow verifier.
    bool g422KindStores(uint8_t kind)
    {
        return kind == G422_LK_SQ || kind == G422_LK_ISW || kind == G422_LK_SQI ||
               kind == G422_LK_SQD || kind == G422_LK_ISWR;
    }

    bool g422FastLowerRequested()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G422_NO_FAST_LOWER");
        return s_on;
    }

    bool g422VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G422_VERIFY");
        return s_on;
    }

    // G480: hoist the NOP test out of the (out-of-line) g422FastLower call. Read once per run(),
    // never per pair, and — like G475/G479 — the A/B arm is HELD for a block of frames so a single
    // microprogram can never change dispatch shape mid-execution. See the g480NopSkip comment in
    // vu1_upper_opcodes.inc for the mechanism and the map evidence.
    bool g480NopSkipRun()
    {
        if (g480NopSkipArm() >= 0)
            return g480NopSkipArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G480_NO_NOPSKIP");
        return s_on;
    }

    // G481: execute the seven HOT kinds inline in the loop instead of through the (out-of-line,
    // MSVC-refused-to-inline) g422FastLower call. Loop-INVARIANT like g480NopSkip: read once per
    // run(), never per pair, and the A/B arm is HELD for a block of frames.
    bool g481HotLowerRun()
    {
        if (g481HotLowerArm() >= 0)
            return g481HotLowerArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G481_NO_HOT_LOWER");
        return s_on;
    }

    bool g481VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G481_VERIFY");
        return s_on;
    }

    // Every env that arms a probe living INSIDE one of the covered execLower bodies (or inside
    // execLower's own prologue). Any of them keeps the whole lower slot on the legacy path.
    // NOTE: DC2_TRACE_VU1 (which arms the G39 load trace) and DC2_TRACE_G32 already force
    // g295Diag, which the caller requires to be false; they are listed for documentation.
    bool g422LowerDiagFree()
    {
        static const bool s_free = []() -> bool {
            static const char *const k[] = {
                "DC2_TRACE_VU1",      // execLower prologue: G39 load trace arming
                "DC2_TRACE_G31",      // LQI value trace
                "DC2_TRACE_G32",      // execLower prologue: packing-loop trace
                "DC2_G133_COPY_QSKIP",// execLower prologue capture + SQI ADC override
                "DC2_G370_VUSTORE",   // SQ/SQI/SQD store watch
                "DC2_G370_VULOAD",    // LQ/LQD load watch
                "DC2_G63_GATE",       // IAND gate census
                "DC2_G64_FORCE_ENABLE_FIX"}; // IAND gate repair
            for (const char *n : k)
                if (std::getenv(n) != nullptr)
                    return false;
            return true;
        }();
        return s_free;
    }

    // Verbatim transcription of VU1Interpreter::applyDest (vu1_core_execution.inc).
    static DC2_G330_FORCEINLINE void g422ApplyDest(float *dst, const float *result, uint8_t dest)
    {
        if (dest & 0x8)
            dst[0] = result[0];
        if (dest & 0x4)
            dst[1] = result[1];
        if (dest & 0x2)
            dst[2] = result[2];
        if (dest & 0x1)
            dst[3] = result[3];
    }

    // Executes one covered lower op. `kind` must not be G422_LK_NONE.
    static DC2_G330_FORCEINLINE void g422FastLower(
        VU1State &st, uint32_t instr, uint8_t kind, uint8_t *vuData, uint32_t dataSize)
    {
        switch (kind)
        {
        case G422_LK_NOP:
            return;

        // G481: the seven HOT kinds' bodies live in vu1_g481_hot_lower.inc so that this
        // out-of-line path and the loop's inline path expand the SAME tokens. Nothing about the
        // executed code changed when they moved.
        case G422_LK_LQ:
            G481_LOWER_BODY_LQ(st, instr, vuData, dataSize);
            return;
        case G422_LK_SQ:
        {
            const uint8_t is = LIS(instr);
            const uint8_t it = LIT(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            const int16_t imm = IMM11(instr);
            uint32_t addr = ((uint32_t)(int32_t)(st.vi[it] + imm)) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                if (dest & 0x8)
                    tmp[0] = st.vf[is][0];
                if (dest & 0x4)
                    tmp[1] = st.vf[is][1];
                if (dest & 0x2)
                    tmp[2] = st.vf[is][2];
                if (dest & 0x1)
                    tmp[3] = st.vf[is][3];
                std::memcpy(vuData + addr, tmp, 16);
            }
            return;
        }
        case G422_LK_ILW:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            const int16_t imm = IMM11(instr);
            uint32_t addr = ((uint32_t)(int32_t)(st.vi[is] + imm)) * 16u;
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
                    st.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
            }
            return;
        }
        case G422_LK_ISW:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            const int16_t imm = IMM11(instr);
            uint32_t addr = ((uint32_t)(int32_t)(st.vi[is] + imm)) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                uint32_t val = (uint32_t)(uint16_t)(st.vi[it] & 0xFFFF);
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
        case G422_LK_IADDIU:
            G481_LOWER_BODY_IADDIU(st, instr);
            return;
        case G422_LK_ISUBIU:
            G481_LOWER_BODY_ISUBIU(st, instr);
            return;
        case G422_LK_B:
            G481_LOWER_BODY_B(st, instr);
            return;
        case G422_LK_BAL:
            G481_LOWER_BODY_BAL(st, instr);
            return;
        case G422_LK_JR:
            G481_LOWER_BODY_JR(st, instr);
            return;
        case G422_LK_JALR:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint32_t target = ((uint32_t)(uint16_t)st.vi[is] * 8u) & 0x3FFF;
            if (it != 0)
                st.vi[it] = (int32_t)((st.pc + 16) / 8);
            st.pc = target - 8;
            return;
        }
        case G422_LK_IBEQ:
            G481_LOWER_BODY_IBEQ(st, instr);
            return;
        case G422_LK_IBNE:
            G481_LOWER_BODY_IBNE(st, instr);
            return;
        case G422_LK_IBLTZ:
        {
            const uint8_t is = LIS(instr);
            const int16_t imm = IMM11(instr);
            if ((int16_t)st.vi[is] < 0)
            {
                const uint32_t target = (st.pc + 8 + imm * 8) & 0x3FFF;
                st.pc = target - 8;
            }
            return;
        }
        case G422_LK_IBGTZ:
        {
            const uint8_t is = LIS(instr);
            const int16_t imm = IMM11(instr);
            if ((int16_t)st.vi[is] > 0)
            {
                const uint32_t target = (st.pc + 8 + imm * 8) & 0x3FFF;
                st.pc = target - 8;
            }
            return;
        }
        case G422_LK_IBLEZ:
        {
            const uint8_t is = LIS(instr);
            const int16_t imm = IMM11(instr);
            if ((int16_t)st.vi[is] <= 0)
            {
                const uint32_t target = (st.pc + 8 + imm * 8) & 0x3FFF;
                st.pc = target - 8;
            }
            return;
        }
        case G422_LK_IBGEZ:
        {
            const uint8_t is = LIS(instr);
            const int16_t imm = IMM11(instr);
            if ((int16_t)st.vi[is] >= 0)
            {
                const uint32_t target = (st.pc + 8 + imm * 8) & 0x3FFF;
                st.pc = target - 8;
            }
            return;
        }
        case G422_LK_IADD:
            G481_LOWER_BODY_IADD(st, instr);
            return;
        case G422_LK_ISUB:
            G481_LOWER_BODY_ISUB(st, instr);
            return;
        case G422_LK_IADDI:
            G481_LOWER_BODY_IADDI(st, instr);
            return;
        case G422_LK_IAND:
            G481_LOWER_BODY_IAND(st, instr);
            return;
        case G422_LK_IOR:
            G481_LOWER_BODY_IOR(st, instr);
            return;
        case G422_LK_MOVE:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            float tmp[4];
            std::memcpy(tmp, st.vf[is], 16);
            g422ApplyDest(st.vf[it], tmp, dest);
            return;
        }
        case G422_LK_MR32:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            const float tmp[4] = {st.vf[is][1], st.vf[is][2], st.vf[is][3], st.vf[is][0]};
            g422ApplyDest(st.vf[it], tmp, dest);
            return;
        }
        case G422_LK_LQI:
            G481_LOWER_BODY_LQI(st, instr, vuData, dataSize);
            return;
        case G422_LK_SQI:
            G481_LOWER_BODY_SQI(st, instr, vuData, dataSize);
            return;
        case G422_LK_LQD:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            if (is != 0)
                st.vi[is] = (int16_t)(st.vi[is] - 1);
            uint32_t addr = ((uint32_t)(uint16_t)st.vi[is]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                g422ApplyDest(st.vf[it], tmp, dest);
            }
            return;
        }
        case G422_LK_SQD:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            if (it != 0)
                st.vi[it] = (int16_t)(st.vi[it] - 1);
            uint32_t addr = ((uint32_t)(uint16_t)st.vi[it]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                if (dest & 0x8)
                    tmp[0] = st.vf[is][0];
                if (dest & 0x4)
                    tmp[1] = st.vf[is][1];
                if (dest & 0x2)
                    tmp[2] = st.vf[is][2];
                if (dest & 0x1)
                    tmp[3] = st.vf[is][3];
                std::memcpy(vuData + addr, tmp, 16);
            }
            return;
        }
        case G422_LK_MTIR:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const int comp = (instr >> 21) & 0x3;
            uint32_t fval;
            std::memcpy(&fval, &st.vf[is][comp], 4);
            if (it != 0)
                st.vi[it] = (int32_t)(int16_t)(fval & 0xFFFF);
            return;
        }
        case G422_LK_MFIR:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            float result[4];
            const int32_t val = (int32_t)(int16_t)(st.vi[is] & 0xFFFF);
            std::memcpy(&result[0], &val, 4);
            result[1] = result[0];
            result[2] = result[0];
            result[3] = result[0];
            g422ApplyDest(st.vf[it], result, dest);
            return;
        }
        case G422_LK_ILWR:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            if (it == 0)
                return;
            uint32_t addr = ((uint32_t)(uint16_t)st.vi[is]) * 16u;
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
                st.vi[it] = (int32_t)(int16_t)(value & 0xFFFFu);
            }
            return;
        }
        case G422_LK_ISWR:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dest = (uint8_t)((instr >> 21) & 0xF);
            uint32_t addr = ((uint32_t)(uint16_t)st.vi[is]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                uint32_t value = (uint32_t)(uint16_t)(st.vi[it] & 0xFFFF);
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
        // G487: the four flag consumers, transcribed verbatim from VU1Interpreter::execLower
        // (vu1_dispatch_and_sync.inc cases 0x12 / 0x13 / 0x1A / 0x1C) under the two constants
        // g487FlagLowerSupported() pins: `s_fmFixed == true` and `g138_macpipe_enabled() == true`.
        // With those two fixed, execLower's bodies reduce to exactly these statements — the
        // `s_g106UseLowerFlagRead` alternative is unreachable (it is written only inside the
        // `s_g106ParallelFlags` block, which `g443Fixed` excludes) and every `if (1 != 0)` is the
        // identity. Nothing is re-ordered and no expression is re-associated.
        // The shadow oracle DC2_G422_VERIFY=1 re-runs the real execLower from the identical
        // pre-state and compares the whole VU1State, so a drift here cannot ship silently.
        case G422_LK_FCAND:
        {
            const uint32_t imm24 = instr & 0xFFFFFF;
            st.vi[1] = ((st.clip & imm24) != 0) ? 1 : 0;
            return;
        }
        case G422_LK_FCOR:
        {
            const uint32_t imm24 = instr & 0xFFFFFF;
            st.vi[1] = ((st.clip | imm24) == 0xFFFFFF) ? 1 : 0;
            return;
        }
        case G422_LK_FMAND:
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            if (it != 0)
                st.vi[it] = (int32_t)(s_vuMacVisible & (uint32_t)(uint16_t)st.vi[is]);
            return;
        }
        case G422_LK_FCGET:
        {
            const uint8_t it = LIT(instr);
            if (it != 0)
                st.vi[it] = (int32_t)(st.clip & 0xFFFu);
            return;
        }
        default:
            return;
        }
    }

    // Shadow-verify scratch. Thread-local on purpose: kept off run()'s stack frame, and the VU1
    // interpreter can run on either the EE thread or the MTVU worker.
    thread_local VU1State s_g422VerBefore{};
    thread_local VU1State s_g422VerFast{};
    thread_local uint8_t s_g422VerMemBefore[PS2_VU1_DATA_SIZE];
    thread_local uint8_t s_g422VerMemFast[PS2_VU1_DATA_SIZE];

    bool g422StatesEqual(const VU1State &a, const VU1State &b)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 &&
               std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 &&
               std::memcmp(&a.q, &b.q, sizeof(a.q)) == 0 &&
               std::memcmp(&a.p, &b.p, sizeof(a.p)) == 0 &&
               std::memcmp(&a.i, &b.i, sizeof(a.i)) == 0 &&
               a.pc == b.pc && a.mac == b.mac && a.clip == b.clip && a.status == b.status &&
               a.ebit == b.ebit && a.itop == b.itop && a.xitop == b.xitop;
    }
} // namespace
