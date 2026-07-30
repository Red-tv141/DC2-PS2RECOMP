// G421 - inline SIMD execution of the VU1 upper slot.
//
// PREMISE (current-binary, this phase). Lean MAP-0 is ~66-68 ms/f and the G303 thread
// attribution puts the VU1 worker at busyMs/f 65.2 against a 66.2 ms frame (~98%), with the
// GS worker at 60.1 (91%) and the EE thread at 19.3 cpu-ms (29%). The VU1 interpreter is the
// pole. The G421 census measures 1309 executed pairs per program run and ~1244 runs/frame =
// ~1.63 M pairs/frame, i.e. ~40 ns/pair (~130 host cycles) — far above what a predecoded
// interpreter step should cost.
//
// MECHANISM. 50.3% of executed pairs carry a non-VNOP upper op, and every one of them leaves
// run() through a non-inlined execUpper() call that re-derives dest/ft/fs/fd, dispatches
// through a ~64-way jump table, computes four lanes with scalar float ops and stores the
// destination through four conditional branches. G410 already proved the price of that call
// boundary: skipping it for the 49.5% VNOP share bought ~8.6 ns per skipped call. This lever
// executes the arithmetic upper families *inline* in the hot loop with SSE2, so the remaining
// ~0.82 M non-VNOP uppers per frame stop paying the call.
//
// EXACTNESS. The build is MSVC /O2 with no /arch:AVX and default /fp:precise, so
// addps/subps/mulps/maxps/minps are per-lane IEEE-754 single identical to the scalar lane
// loops, and no FMA contraction can occur (MADD stays mul-then-add, two roundings).
//   * _mm_max_ps(a,b) is `a > b ? a : b` and _mm_min_ps(a,b) is `a < b ? a : b`, matching the
//     legacy ternaries operand-for-operand (including their NaN / signed-zero behaviour).
//   * FTOI uses _mm_cvttps_epi32 (truncate toward zero, 0x80000000 out of range) which is what
//     MSVC x64 emits for the legacy (int32_t)(float) cast (cvttss2si).
//   * ITOF uses _mm_cvtepi32_ps, exact for every int32.
//   * The destination write is a bit-select against the same 4-bit dest mask, so masked-off
//     lanes are rewritten with their own bits — value-identical to the legacy branch chain.
//   * MAC/STATUS come from the same classifier G330 already ships, fed from a register instead
//     of memory.
// Ops that are cross-lane or flag-only (OPMSUB, OPMULA, CLIPw, MSUBA's unmapped encoding) and
// every diagnostic mode that perturbs the FMAC result (DC2_NO_VU1_MAC, DC2_VU1_CLAMP,
// G71 clamp census, G330 rollback) stay on the legacy execUpper path.
//
// Status:   PROMOTED default-on.
// Kill:     DC2_G421_NO_FAST_UPPER=1  (restores the legacy execUpper path exactly)
// Verify:   DC2_G421_VERIFY=1  (shadow-executes legacy execUpper and compares the whole state)
namespace
{
    // cls: 0 = unsupported (fall back to execUpper)
    //      1 = FMAC -> VF[fd] (+MAC/STATUS)   2 = FMAC -> ACC (+MAC/STATUS)
    //      3 = ITOF -> VF[ft]                 4 = FTOI -> VF[ft]
    //      5 = ABS  -> VF[ft]                 6 = MOVE -> VF[ft]     7 = MR32 -> VF[ft]
    // op:  0 ADD  1 SUB  2 MADD  3 MSUB  4 MUL  5 MAX  6 MIN
    // src: 0 broadcast(vt, bc)  1 Q  2 I  3 vt
    struct G421UpperDesc
    {
        uint8_t cls = 0u;
        uint8_t op = 0u;
        uint8_t src = 0u;
        uint8_t bc = 0u; // broadcast lane, or ITOF/FTOI scale index
    };

    struct G421DescTable
    {
        G421UpperDesc e[0x80] = {};
    };

    G421DescTable g421BuildDescTable()
    {
        G421DescTable t;
        auto set = [&t](unsigned kind, uint8_t cls, uint8_t op, uint8_t src, uint8_t bc) {
            t.e[kind] = G421UpperDesc{cls, op, src, bc};
        };
        // ---- main upper opcode space (kind == upper & 0x3f, values 0x00-0x2f) ----
        for (unsigned b = 0u; b < 4u; ++b)
        {
            set(0x00u + b, 1u, 0u, 0u, (uint8_t)b); // ADDbc
            set(0x04u + b, 1u, 1u, 0u, (uint8_t)b); // SUBbc
            set(0x08u + b, 1u, 2u, 0u, (uint8_t)b); // MADDbc
            set(0x0Cu + b, 1u, 3u, 0u, (uint8_t)b); // MSUBbc
            set(0x10u + b, 1u, 5u, 0u, (uint8_t)b); // MAXbc
            set(0x14u + b, 1u, 6u, 0u, (uint8_t)b); // MINIbc
            set(0x18u + b, 1u, 4u, 0u, (uint8_t)b); // MULbc
        }
        set(0x1Cu, 1u, 4u, 1u, 0u); // MULq
        set(0x1Du, 1u, 5u, 2u, 0u); // MAXi
        set(0x1Eu, 1u, 4u, 2u, 0u); // MULi
        set(0x1Fu, 1u, 6u, 2u, 0u); // MINIi
        set(0x20u, 1u, 0u, 1u, 0u); // ADDq
        set(0x21u, 1u, 2u, 1u, 0u); // MADDq
        set(0x22u, 1u, 0u, 2u, 0u); // ADDi
        set(0x23u, 1u, 2u, 2u, 0u); // MADDi
        set(0x24u, 1u, 1u, 1u, 0u); // SUBq
        set(0x25u, 1u, 3u, 1u, 0u); // MSUBq
        set(0x26u, 1u, 1u, 2u, 0u); // SUBi
        set(0x27u, 1u, 3u, 2u, 0u); // MSUBi
        set(0x28u, 1u, 0u, 3u, 0u); // ADD
        set(0x29u, 1u, 2u, 3u, 0u); // MADD
        set(0x2Au, 1u, 4u, 3u, 0u); // MUL
        set(0x2Bu, 1u, 5u, 3u, 0u); // MAX
        set(0x2Cu, 1u, 1u, 3u, 0u); // SUB
        set(0x2Du, 1u, 3u, 3u, 0u); // MSUB
        // 0x2E OPMSUB is cross-lane -> legacy
        set(0x2Fu, 1u, 6u, 3u, 0u); // MINI

        // ---- Special2 space (kind == 0x40 + func) ----
        for (unsigned b = 0u; b < 4u; ++b)
        {
            set(0x40u + b, 2u, 0u, 0u, (uint8_t)b); // ADDAbc
            set(0x44u + b, 2u, 1u, 0u, (uint8_t)b); // SUBAbc
            set(0x48u + b, 2u, 2u, 0u, (uint8_t)b); // MADDAbc
            set(0x4Cu + b, 2u, 3u, 0u, (uint8_t)b); // MSUBAbc
            set(0x50u + b, 3u, 0u, 0u, (uint8_t)b); // ITOF0/4/12/15
            set(0x54u + b, 4u, 0u, 0u, (uint8_t)b); // FTOI0/4/12/15
            set(0x58u + b, 2u, 4u, 0u, (uint8_t)b); // MULAbc
        }
        set(0x5Cu, 2u, 4u, 1u, 0u); // MULAq
        set(0x5Du, 5u, 0u, 0u, 0u); // ABS
        set(0x5Eu, 2u, 4u, 2u, 0u); // MULAi
        // 0x5F CLIPw writes only m_state.clip -> legacy
        set(0x60u, 2u, 0u, 1u, 0u); // ADDAq
        set(0x61u, 2u, 2u, 1u, 0u); // MADDAq
        set(0x62u, 2u, 0u, 2u, 0u); // ADDAi
        set(0x63u, 2u, 2u, 2u, 0u); // MADDAi
        set(0x64u, 2u, 1u, 1u, 0u); // SUBAq
        set(0x65u, 2u, 3u, 1u, 0u); // MSUBAq
        set(0x66u, 2u, 1u, 2u, 0u); // SUBAi
        set(0x67u, 2u, 3u, 2u, 0u); // MSUBAi
        set(0x68u, 2u, 0u, 3u, 0u); // ADDA
        set(0x69u, 2u, 2u, 3u, 0u); // MADDA
        set(0x6Au, 2u, 4u, 3u, 0u); // MULA
        // 0x6B is func 0x2B, which the legacy decoder does not implement (G410 already maps it
        // to the fallback kind). Leave unsupported so behaviour is preserved exactly.
        set(0x6Cu, 2u, 1u, 3u, 0u); // SUBA
        set(0x6Du, 2u, 3u, 3u, 0u); // MSUBA
        // 0x6E OPMULA is cross-lane -> legacy.  0x6F VNOP never reaches here.
        set(0x70u, 6u, 0u, 0u, 0u); // MOVE
        set(0x71u, 7u, 0u, 0u, 0u); // MR32
        return t;
    }

    const G421UpperDesc *g421Desc()
    {
        static const G421DescTable s_table = g421BuildDescTable();
        return s_table.e;
    }

    // dest bit3 = x (lane 0) ... bit0 = w (lane 3), matching applyDest().
    struct G421MaskTable
    {
        alignas(16) uint32_t m[16][4] = {};
    };

    G421MaskTable g421BuildMaskTable()
    {
        G421MaskTable t;
        for (unsigned d = 0u; d < 16u; ++d)
            for (unsigned lane = 0u; lane < 4u; ++lane)
                t.m[d][lane] = (d & (0x8u >> lane)) ? 0xFFFFFFFFu : 0u;
        return t;
    }

    const uint32_t (*g421MaskRows())[4]
    {
        static const G421MaskTable s_table = g421BuildMaskTable();
        return s_table.m;
    }

    // Same classifier as g329MakeMacSimd, fed from a register instead of a memory operand.
    static inline uint32_t g421MakeMacReg(__m128 value, uint8_t dest)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i zero = _mm_setzero_si128();
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i absBits = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i expBits = _mm_and_si128(bits, expMask);
        const __m128i isZero = _mm_cmpeq_epi32(absBits, zero);
        const __m128i expZero = _mm_cmpeq_epi32(expBits, zero);
        const __m128i isUnder = _mm_andnot_si128(isZero, expZero);
        const __m128i isOver = _mm_cmpeq_epi32(expBits, expMask);
        const uint32_t d = static_cast<uint32_t>(dest);
        const uint32_t z = g329ReverseLaneNibble(
            static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isZero)))) & d;
        const uint32_t s = g329ReverseLaneNibble(
            static_cast<uint32_t>(_mm_movemask_ps(value))) & d;
        const uint32_t u = g329ReverseLaneNibble(
            static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isUnder)))) & d;
        const uint32_t o = g329ReverseLaneNibble(
            static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isOver)))) & d;
        return z | (s << 4u) | (u << 8u) | (o << 12u);
    }

    // Bit-select the dest lanes; masked-off lanes are rewritten with their own bits, so this is
    // value-identical to applyDest()'s four conditional stores.
    static inline void g421StoreMasked(float *dst, __m128 value, __m128 mask)
    {
        const __m128 old = _mm_loadu_ps(dst);
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, old)));
    }

    // PROMOTED default-on. The former positive bring-up flag DC2_G421_FAST_UPPER is now a
    // harmless no-op, the same retirement G410 gave DC2_G410_VU_COMPILED.
    bool g421FastUpperRequested()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G421_NO_FAST_UPPER");
        return s_on;
    }

    bool g421VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G421_VERIFY");
        return s_on;
    }

    // Shadow-verify scratch. Deliberately NOT locals inside run(): two VU1State copies are
    // ~1.4 KB and MSVC reserves the maximum frame for the whole function, so putting them on
    // run()'s stack grows the hot loop's frame for a path that never executes by default.
    thread_local VU1State s_g421VerBefore;
    thread_local VU1State s_g421VerFast;

    // Executes the upper slot in registers. Returns false when the encoding is not covered, in
    // which case the caller must run the legacy execUpper().
    static DC2_G330_FORCEINLINE bool g421FastUpper(VU1State &st, uint32_t upper, uint8_t kind)
    {
        const G421UpperDesc d = g421Desc()[kind];
        if (d.cls == 0u)
            return false;

        const __m128 vs = _mm_loadu_ps(st.vf[FS(upper)]);
        const uint8_t dest = DEST(upper);
        const __m128 mask = _mm_load_ps(reinterpret_cast<const float *>(g421MaskRows()[dest]));

        if (d.cls >= 3u)
        {
            __m128 r;
            switch (d.cls)
            {
            case 3u: // ITOF0/4/12/15 : (float)bits(vs) * scale
            {
                static const float kItof[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f,
                                               1.0f / 32768.0f};
                r = _mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(vs)),
                               _mm_set1_ps(kItof[d.bc]));
                break;
            }
            case 4u: // FTOI0/4/12/15 : bits((int32_t)(vs * scale))
            {
                static const float kFtoi[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
                r = _mm_castsi128_ps(
                    _mm_cvttps_epi32(_mm_mul_ps(vs, _mm_set1_ps(kFtoi[d.bc]))));
                break;
            }
            case 5u: // ABS
                r = _mm_and_ps(vs, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
                break;
            case 6u: // MOVE
                r = vs;
                break;
            default: // 7u MR32 : {vs[1], vs[2], vs[3], vs[0]}
                r = _mm_shuffle_ps(vs, vs, _MM_SHUFFLE(0, 3, 2, 1));
                break;
            }
            // None of these five update MAC/STATUS, and all write VF[ft].
            g421StoreMasked(st.vf[FT(upper)], r, mask);
            return true;
        }

        __m128 b;
        switch (d.src)
        {
        case 0u:
        {
            const __m128 vt = _mm_loadu_ps(st.vf[FT(upper)]);
            switch (d.bc)
            {
            case 0u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(0, 0, 0, 0)); break;
            case 1u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(1, 1, 1, 1)); break;
            case 2u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(2, 2, 2, 2)); break;
            default: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 3, 3, 3)); break;
            }
            break;
        }
        case 1u: b = _mm_set1_ps(st.q); break;
        case 2u: b = _mm_set1_ps(st.i); break;
        default: b = _mm_loadu_ps(st.vf[FT(upper)]); break;
        }

        __m128 r;
        switch (d.op)
        {
        case 0u: r = _mm_add_ps(vs, b); break;
        case 1u: r = _mm_sub_ps(vs, b); break;
        case 2u: r = _mm_add_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, b)); break;
        case 3u: r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, b)); break;
        case 4u: r = _mm_mul_ps(vs, b); break;
        case 5u: r = _mm_max_ps(vs, b); break;
        default: r = _mm_min_ps(vs, b); break;
        }

        const uint32_t mac = g421MakeMacReg(r, dest);
        st.mac = mac;
        g329UpdateStatus(st.status, mac);
        g421StoreMasked(d.cls == 1u ? st.vf[FD(upper)] : st.acc, r, mask);
        return true;
    }
} // namespace
