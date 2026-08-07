// G481 - the seven HOT G422 lower kinds, as macro bodies shared by both execution sites.
//
// PREMISE (G480 §5, and the standing pole gate). `g422FastLower` is declared `static
// __forceinline` and MSVC DECLINES it (33 switch kinds), so it is a real out-of-line body with its
// own unwind records. G480 proved that fact from the linker /MAP and took the free half of it: the
// NOP kind, whose body is literally `return;`, is now tested in the loop and never calls. That was
// -0.440 ms/f over ~39% of pairs.
//
// The remaining ~60% of pairs still make a five-argument out-of-line call. `[G479:lk]` gives the
// dynamic kind distribution over ~1.41 M pairs/frame:
//     LQ 9.4% | SQI 8.2% | IADDI 7.0% | LQI 6.8% | IBNE 4.4% | IADDIU 3.1% | IAND 3.0%
// i.e. the top seven kinds are ~42% of ALL pairs, and every one of them is a handful of
// instructions - strictly less work than the call that reaches it. Inlining exactly these seven
// into the loop removes the call on ~42% of pairs; G480's NOP result prices that at the same
// order.
//
// WHY ONLY SEVEN. G421/G435 both measured this loop paying for text it never executes (G421's
// census inlining cost ~3 ms/f of default path). The other 26 kinds stay behind the existing
// out-of-line `g422FastLower` call, so the loop's text growth is bounded by these seven small
// bodies rather than by the whole 33-kind switch.
//
// EXACTNESS BY CONSTRUCTION. Each body below is defined ONCE, here, and expanded at BOTH sites:
// inside `g422FastLower`'s switch (vu1_g422_fast_lower.inc) and inside the loop's inline switch
// (DC2_G481_HOT_LOWER in vu1_upper_opcodes.inc). There is no transcription to drift: the two paths
// are the same tokens. The bodies themselves are unchanged from G422's verbatim transcription of
// the corresponding `VU1Interpreter::execLower` cases - same address masking, same
// `addr + 16 <= dataSize` guard, same `!= 0` register-write guards, same int16 truncations, same
// read-modify-write tmp[4] staging.
//
// Each macro is a STATEMENT BLOCK that falls off its end (no `return`), so the caller appends
// whatever its context needs (`return;` in the function, `break;` in the loop switch). None of the
// seven has an interior early exit, which is why exactly these are expressible this way.
//
// Status:   PROMOTED default-on (G481).
// Kill:     DC2_G481_NO_HOT_LOWER=1  (every kind goes back through the g422FastLower call)
// Verify:   DC2_G481_VERIFY=1        (shadow-executes the out-of-line g422FastLower from the same
//                                     pre-state and compares VU1State + all 16 KB of VU data RAM)
// Gate:     DC2_G419_AB=hotlower

#define G481_LOWER_BODY_LQ(st, instr, vuData, dataSize)                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        const int16_t imm = IMM11(instr);                                                 \
        uint32_t addr = ((uint32_t)(int32_t)((st).vi[is] + imm)) * 16u;                   \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
        {                                                                                 \
            float tmp[4];                                                                 \
            std::memcpy(tmp, (vuData) + addr, 16);                                        \
            g422ApplyDest((st).vf[it], tmp, dest);                                        \
        }                                                                                 \
    }

#define G481_LOWER_BODY_LQI(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[is]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
        {                                                                                 \
            float tmp[4];                                                                 \
            std::memcpy(tmp, (vuData) + addr, 16);                                        \
            g422ApplyDest((st).vf[it], tmp, dest);                                        \
        }                                                                                 \
        if (is != 0)                                                                      \
            (st).vi[is] = (int16_t)((st).vi[is] + 1);                                     \
    }

#define G481_LOWER_BODY_SQI(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[it]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
        {                                                                                 \
            float tmp[4];                                                                 \
            std::memcpy(tmp, (vuData) + addr, 16);                                        \
            if (dest & 0x8)                                                               \
                tmp[0] = (st).vf[is][0];                                                  \
            if (dest & 0x4)                                                               \
                tmp[1] = (st).vf[is][1];                                                  \
            if (dest & 0x2)                                                               \
                tmp[2] = (st).vf[is][2];                                                  \
            if (dest & 0x1)                                                               \
                tmp[3] = (st).vf[is][3];                                                  \
            std::memcpy((vuData) + addr, tmp, 16);                                        \
        }                                                                                 \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[it] + 1);                                     \
    }

#define G481_LOWER_BODY_IADDI(st, instr)                                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const int16_t imm5 = (int16_t)((int32_t)(((instr) >> 6) & 0x1F) << 27 >> 27);     \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[is] + imm5);                                  \
    }

#define G481_LOWER_BODY_IADDIU(st, instr)                                                 \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const int16_t imm =                                                               \
            (int16_t)((int16_t)((instr) & 0x7FF) | (((instr) >> 10) & 0x7800));           \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[is] + imm);                                   \
    }

#define G481_LOWER_BODY_IAND(st, instr)                                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t id = LID(instr);                                                    \
        if (id != 0)                                                                      \
            (st).vi[id] = (st).vi[is] & (st).vi[it];                                       \
    }

#define G481_LOWER_BODY_IBNE(st, instr)                                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const int16_t imm = IMM11(instr);                                                 \
        if ((int16_t)(st).vi[is] != (int16_t)(st).vi[it])                                 \
        {                                                                                 \
            const uint32_t target = ((st).pc + 8 + imm * 8) & 0x3FFF;                     \
            (st).pc = target - 8;                                                         \
        }                                                                                 \
    }

// The remaining PURE-INTEGER kinds. Individually small in the census ([G479:lk] on the G481
// binary: IBEQ 2.40%, JR 0.86%, BAL 0.82%, B 0.38%, IADD 0.38%, ISUBIU 0.35%, IOR 0.06%) but
// together ~5.3% of all pairs, and each is three or four integer instructions that touch only
// `vi[]` and `pc` — the same class as the four kept inline, so the trade looked identical.
//
// RESULT: REJECTED, and not marginally — adding these eight to the four below took the SAME
// paired gate to **+81 ms/frame** (candidate 110-112 ms/f against a 30-32 ms/f control in the same
// process, t = 187). A 4x collapse cannot be register pressure or text locality; the loop crossed
// some MSVC inline/optimisation budget and the whole of `VU1Interpreter::run` was re-generated in
// a far worse shape (the G421 inline upper slot is the obvious casualty, being by far the largest
// inlined block in the loop). The bodies are still expanded by `g422FastLower`'s switch, so they
// are live, exact code — they are simply NOT named by the loop's dispatch.
//
// THE RULE THIS ESTABLISHES: this loop's inline budget is a CLIFF, not a slope. Measure every
// added kind; never assume a body that is smaller than the call it replaces is affordable.

#define G481_LOWER_BODY_IBEQ(st, instr)                                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const int16_t imm = IMM11(instr);                                                 \
        if ((int16_t)(st).vi[is] == (int16_t)(st).vi[it])                                 \
        {                                                                                 \
            const uint32_t target = ((st).pc + 8 + imm * 8) & 0x3FFF;                     \
            (st).pc = target - 8;                                                         \
        }                                                                                 \
    }

#define G481_LOWER_BODY_B(st, instr)                                                      \
    {                                                                                     \
        const int16_t imm = IMM11(instr);                                                 \
        const uint32_t target = ((st).pc + 8 + imm * 8) & 0x3FFF;                         \
        (st).pc = target - 8;                                                             \
    }

#define G481_LOWER_BODY_BAL(st, instr)                                                    \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const int16_t imm = IMM11(instr);                                                 \
        const uint32_t target = ((st).pc + 8 + imm * 8) & 0x3FFF;                         \
        if (it != 0)                                                                      \
            (st).vi[it] = (int32_t)(((st).pc + 16) / 8);                                  \
        (st).pc = target - 8;                                                             \
    }

#define G481_LOWER_BODY_JR(st, instr)                                                     \
    {                                                                                     \
        const uint8_t is = LIS(instr);                                                    \
        const uint32_t target = ((uint32_t)(uint16_t)(st).vi[is] * 8u) & 0x3FFF;          \
        (st).pc = target - 8;                                                             \
    }

#define G481_LOWER_BODY_IADD(st, instr)                                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t id = LID(instr);                                                    \
        if (id != 0)                                                                      \
            (st).vi[id] = (int16_t)((st).vi[is] + (st).vi[it]);                           \
    }

#define G481_LOWER_BODY_ISUB(st, instr)                                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t id = LID(instr);                                                    \
        if (id != 0)                                                                      \
            (st).vi[id] = (int16_t)((st).vi[is] - (st).vi[it]);                           \
    }

#define G481_LOWER_BODY_IOR(st, instr)                                                    \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t id = LID(instr);                                                    \
        if (id != 0)                                                                      \
            (st).vi[id] = (st).vi[is] | (st).vi[it];                                       \
    }

#define G481_LOWER_BODY_ISUBIU(st, instr)                                                 \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const int16_t imm =                                                               \
            (int16_t)((int16_t)((instr) & 0x7FF) | (((instr) >> 10) & 0x7800));           \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[is] - imm);                                   \
    }

// --------------------------------------------------------------------------------------------
// G481 second arm - the three HOT MEMORY kinds in a BRANCHLESS SSE2 lane-blend form.
//
// RESULT: REJECTED. Stacked on the integer set below (which alone reads -0.578 ms/f, t = -3.49,
// 5/5) these three took the SAME paired gate to **+0.190 ms/f SLOWER** (t = 3.08, 0/5), i.e. the
// three memory kinds cost ~+0.77 even in this five-op branchless form with no stack slot and no
// `tmp[4]`. The cost is not the body's arithmetic - it is the loop's text and XMM pressure. Kept
// UNEXPANDED (a macro that no dispatch names emits nothing) purely as this record: do not rebuild
// it, and do not conclude the scalar staging buffer was the problem.
//
// The scalar bodies above are what `g422FastLower` executes, and inlining them into the loop was
// measured SLOWER: each carries a 16-byte `tmp[4]` stack buffer, two `memcpy`s and four
// conditional lane stores, which is a lot of text and a stack slot in a loop already at ~1.2 IPC
// with 36 spills (G442). The blend form is five SSE2 ops with no branch, no stack slot and no
// call - the same shape G421/G428 already promoted for the UPPER slot's destination write, using
// the same constant mask table.
//
// EXACTNESS. `movups`/`andps`/`andnps`/`orps`/`movups` move and select BITS; no lane is ever fed
// to an FP arithmetic unit, so no value can be renormalised, quieted or clamped. Lane L of the
// result is the loaded word when `dest` selects it and the destination's own previous word when it
// does not - exactly what the four conditional scalar assignments produce. `st.vf[is]` (VU
// registers) and `vuData` (VU RAM) are disjoint objects, so the load-then-store in SQI cannot
// alias. The address arithmetic, the `dataSize` mask, the `addr + 16 <= dataSize` guard and the
// post-increment guards are the SAME tokens as the scalar bodies. The claim is nevertheless
// checked, not asserted: DC2_G481_VERIFY=1 shadow-runs the scalar out-of-line body from the same
// pre-state and compares the whole VU1State plus all 16 KB of VU RAM.
#define G481_MASKROW(dest) \
    _mm_load_ps(reinterpret_cast<const float *>(g421MaskTableConst.m[(dest)]))

#define G481_LOWER_BODY_LQ_SIMD(st, instr, vuData, dataSize)                              \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        const int16_t imm = IMM11(instr);                                                 \
        uint32_t addr = ((uint32_t)(int32_t)((st).vi[is] + imm)) * 16u;                   \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            g421StoreMasked((st).vf[it],                                                  \
                            _mm_loadu_ps((const float *)((vuData) + addr)),               \
                            G481_MASKROW(dest));                                          \
    }

#define G481_LOWER_BODY_LQI_SIMD(st, instr, vuData, dataSize)                             \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[is]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            g421StoreMasked((st).vf[it],                                                  \
                            _mm_loadu_ps((const float *)((vuData) + addr)),               \
                            G481_MASKROW(dest));                                          \
        if (is != 0)                                                                      \
            (st).vi[is] = (int16_t)((st).vi[is] + 1);                                     \
    }

#define G481_LOWER_BODY_SQI_SIMD(st, instr, vuData, dataSize)                             \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[it]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            g421StoreMasked((float *)((vuData) + addr),                                   \
                            _mm_loadu_ps((st).vf[is]),                                    \
                            G481_MASKROW(dest));                                          \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[it] + 1);                                     \
    }

// ---------------------------------------------------------------------------------------------
// G514 — THE THREE VU-DATA-MEMORY STORE BODIES WITHOUT THE STAGING BUFFER.
//
// These are NOT G481's `*_SIMD` macros above (which are unexpanded and stay that way). The
// difference is the `dest == 0xF` arm: 16 bytes are written with ONE store and NOTHING is read
// back, because every lane is overwritten. G481's blend form always paid a mask-table load and a
// destination load even when the mask selected all four lanes.
//
// WHY THEY EXIST — measured, not inferred (G514 §3). `[G478:vu1prof]` resolved against the linker
// `/MAP` puts 78.6% of the VU1 worker inside `g490BlockBody<1,0,1,1,0,0,1>` and 9.1% of the whole
// thread on four instructions, one per store site, every one of them the `movups [rdx+r12], xmm0`
// that follows `movups xmm0, [rbp-0x41]` — a 16-byte load overlapping four 4-byte `movss` stores
// issued three instructions earlier. That is a store-to-load forwarding failure by construction,
// and `rbp-0x41` is 16-byte MISaligned on top of it. The scalar bodies are already inlined into
// the block leaf (it contains zero calls), so replacing them there removes text.
//
// EXACTNESS.
//   * `dest == 0xF`: the scalar body loads 16 bytes, overwrites all four lanes from `st.vf[is]`,
//     and stores 16 bytes back. Every loaded byte is dead. `_mm_storeu_ps(dst, _mm_loadu_ps(src))`
//     writes the same 16 source bytes to the same address. `movups`/`movss` both MOVE BITS — no
//     lane reaches an FP unit, so no value can be renormalised, quieted or clamped, and a float
//     assignment on x64/SSE is itself a `movss` bit move.
//   * otherwise: the G421 bit-select `g421StoreMasked`, which writes the loaded word back into
//     every lane the mask does not select — value-identical to the four conditional assignments,
//     and the same helper G421/G428/G485 already ship for the upper slot's destination write.
//   * `st.vf[is]` (VU1State) and `vuData` (VU RAM) are disjoint objects, so the load cannot alias
//     the store. `addr` is a multiple of 16, so the destination store is in fact ALIGNED.
//   * the address arithmetic, the `dataSize` mask, the `addr + 16 <= dataSize` guard and the
//     post-in/decrement guards are the SAME tokens as the scalar bodies above.
// Checked, not asserted: DC2_G514_VERIFY=1 re-derives the 16 bytes the SCALAR body would have
// written — from a snapshot taken BEFORE the fast store — and compares them against VU RAM
// afterwards. It compares against the body being REPLACED (G512 §5's rule), not against itself.
#define G514_STORE16(dstPtr, srcVec, dest)                                                \
    {                                                                                     \
        if ((dest) == 0xFu)                                                               \
            _mm_storeu_ps((dstPtr), (srcVec));                                            \
        else                                                                              \
            g421StoreMasked((dstPtr), (srcVec), G481_MASKROW(dest));                      \
    }

#define G514_LOWER_BODY_SQ(st, instr, vuData, dataSize)                                   \
    {                                                                                     \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        const int16_t imm = IMM11(instr);                                                 \
        uint32_t addr = ((uint32_t)(int32_t)((st).vi[it] + imm)) * 16u;                   \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_STORE16((float *)((vuData) + addr), _mm_loadu_ps((st).vf[is]), dest)     \
    }

#define G514_LOWER_BODY_SQI(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[it]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_STORE16((float *)((vuData) + addr), _mm_loadu_ps((st).vf[is]), dest)     \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[it] + 1);                                     \
    }

// G514b — THE SAME TRANSFER IN THE OTHER DIRECTION, AND THE ONE THAT IS ON THE CHAIN.
//
// LQ / LQI / LQD stage 16 bytes of VU RAM into `tmp[4]` and then hand them to `g422ApplyDest`,
// which writes `st.vf[it]` with up to FOUR 4-byte `movss` stores. The very next pairs read that
// same register with `movups xmm, [rbx + ft*16]` — a 16-byte load overlapping four 4-byte stores,
// i.e. the identical forwarding failure as the store side BUT ON THE DEPENDENCY CHAIN, because a
// VF register is consumed immediately whereas VU RAM is a dead end. That distinction is the whole
// reason these are a separate arm: G514 §6 measured the STORE side at −0.7% of the loop (the store
// buffer hides it) and this side is the one G485's `widevf0` already won on, for the same reason.
//
// Population (`[G421:mix]`, this session): LQ 9.4% + LQI 6.8% of ALL pairs — 1.5x the store side.
// Exactness argument, oracle and aliasing note are the store side's, verbatim; `g422ApplyDest`'s
// four conditional lane assignments and the bit-select write the same bits into the same lanes.
#define G514_LOAD16(dstPtr, srcVec, dest)                                                 \
    {                                                                                     \
        if ((dest) == 0xFu)                                                               \
            _mm_storeu_ps((dstPtr), (srcVec));                                            \
        else                                                                              \
            g421StoreMasked((dstPtr), (srcVec), G481_MASKROW(dest));                      \
    }

#define G514_LOWER_BODY_LQ(st, instr, vuData, dataSize)                                   \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        const int16_t imm = IMM11(instr);                                                 \
        uint32_t addr = ((uint32_t)(int32_t)((st).vi[is] + imm)) * 16u;                   \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_LOAD16((st).vf[it],                                                      \
                        _mm_loadu_ps((const float *)((vuData) + addr)), dest)             \
    }

#define G514_LOWER_BODY_LQI(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[is]) * 16u;                           \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_LOAD16((st).vf[it],                                                      \
                        _mm_loadu_ps((const float *)((vuData) + addr)), dest)             \
        if (is != 0)                                                                      \
            (st).vi[is] = (int16_t)((st).vi[is] + 1);                                     \
    }

#define G514_LOWER_BODY_LQD(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        if (is != 0)                                                                      \
            (st).vi[is] = (int16_t)((st).vi[is] - 1);                                     \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[is]) * 16u;                           \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_LOAD16((st).vf[it],                                                      \
                        _mm_loadu_ps((const float *)((vuData) + addr)), dest)             \
    }

#define G514_LOWER_BODY_SQD(st, instr, vuData, dataSize)                                  \
    {                                                                                     \
        const uint8_t it = LIT(instr);                                                    \
        const uint8_t is = LIS(instr);                                                    \
        const uint8_t dest = (uint8_t)(((instr) >> 21) & 0xF);                            \
        if (it != 0)                                                                      \
            (st).vi[it] = (int16_t)((st).vi[it] - 1);                                     \
        uint32_t addr = ((uint32_t)(uint16_t)(st).vi[it]) * 16u;                          \
        addr &= ((dataSize) - 1);                                                         \
        if (addr + 16 <= (dataSize))                                                      \
            G514_STORE16((float *)((vuData) + addr), _mm_loadu_ps((st).vf[is]), dest)     \
    }

// The loop-side dispatch. `kind` is the G410 sidecar byte; only the covered kinds are handled here
// and every other kind (including G422_LK_NONE) falls out of the switch so the caller can take its
// existing path. Written as a macro rather than a function for the same reason DC2_G422_RUN_LOWER
// is: a function here is exactly the call this phase exists to remove.
//
// WHICH KINDS. Measured, not assumed. The first arm inlined all SEVEN hot kinds and read
// **+0.50 ms/f SLOWER** (t = 2.24, 0/5 windows) — see the G481 fix log. The three memory kinds
// (LQ 9.4%, SQI 8.2%, LQI 6.8%) each carry a 16-byte `tmp[4]` staging buffer, a masked lane
// overlay and two `memcpy`s, i.e. XMM pressure and a stack slot inside a loop /FAsc already found
// at ~1.2 IPC with 36 spills (G442). This is the G421/G435 law again: the loop pays for text.
//
// The four INTEGER kinds (IADDI 7.0%, IBNE 4.4%, IADDIU 3.1%, IAND 3.0% = ~17.5% of all pairs)
// are three or four integer instructions each, touch only `vi[]`/`pc`, and need no XMM and no
// stack. Those are the ones kept inline. The memory kinds stay behind the out-of-line
// `g422FastLower` call, where their staging buffer costs the loop nothing.
#define DC2_G481_HOT_LOWER(kind, st, instr, vuData, dataSize, handledOut)                 \
    do {                                                                                  \
        (void)(vuData); (void)(dataSize);                                                 \
        (handledOut) = true;                                                              \
        switch (kind)                                                                     \
        {                                                                                 \
        case G422_LK_IADDI:  G481_LOWER_BODY_IADDI((st), (instr));                        \
            break;                                                                        \
        case G422_LK_IBNE:   G481_LOWER_BODY_IBNE((st), (instr));                         \
            break;                                                                        \
        case G422_LK_IADDIU: G481_LOWER_BODY_IADDIU((st), (instr));                       \
            break;                                                                        \
        case G422_LK_IAND:   G481_LOWER_BODY_IAND((st), (instr));                         \
            break;                                                                        \
        default:                                                                          \
            (handledOut) = false;                                                         \
            break;                                                                        \
        }                                                                                 \
    } while (0)
