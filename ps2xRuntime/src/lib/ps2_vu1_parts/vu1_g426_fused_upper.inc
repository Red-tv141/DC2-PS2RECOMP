// G426 - branchless fused dispatch for the VU1 upper FMAC slot.
//
// PREMISE (current-binary, this phase). G425 re-ran the thread gate and left VU1 the pole by
// ~4 ms, with `VU1Interpreter::run()` spending 97.6-98.2% of its time in the instruction pair
// loop at ~27.7 ns/pair. G425 priced and rejected every *trimming* lever around that loop
// (zero-work run collapsing, readback caching, lean per-pair bookkeeping) and named the per-pair
// DISPATCH as the only remaining vein.
//
// MECHANISM. G421 already executes the arithmetic upper slot inline with SSE2, but it reaches
// the arithmetic through a chain of DATA-DEPENDENT branches, one per executed pair:
//     load desc -> `if (cls == 0)` -> `if (cls >= 3)` -> switch(src) -> switch(bc) -> switch(op)
// The last three are jump tables. A VU microprogram's op sequence is data-driven (different
// vertex kinds take different packer paths), so those indirect branches are exactly the shape an
// ITTAGE predictor does worst on, and every miss costs ~15-20 host cycles inside a loop that is
// only ~90 cycles wide in total.
//
// This lever pre-resolves the whole FMAC family into ONE 128-byte control record per upper kind
// (already available in the G410 sidecar), then computes the result with masks and selects only.
// The executed code for ADDbc, MSUBi, MULq, MAX and MINI is literally the same straight line;
// nothing about the pair steers a branch except the single loop-invariant lever test.
//
// EXACTNESS. Every arithmetic form keeps its legacy SSE2 instruction, operand-for-operand:
//     ADD  : _mm_add_ps(vs, b)            p = vs , q = b    , add
//     SUB  : _mm_sub_ps(vs, b)            p = vs , q = b    , sub
//     MADD : _mm_add_ps(acc, vs*b)        p = acc, q = vs*b , add
//     MSUB : _mm_sub_ps(acc, vs*b)        p = acc, q = vs*b , sub
//     MUL  : _mm_mul_ps(vs, b)            taken from `prod` directly
//     MAX  : _mm_max_ps(vs, b)            MIN : _mm_min_ps(vs, b)
// MUL is routed around the adder deliberately: `0 + (vs*b)` is NOT value-identical when the
// product is -0.0, so the fused path selects the product itself. No negation trick is used
// either, so NaN sign/payload propagation is untouched. The lane selects are pure
// AND/ANDNOT/OR bit-selects over all-ones/all-zero masks, so each lane carries the bits of
// exactly one candidate.
//
// The broadcast source is read as a scalar from a precomputed (base, byte offset) pair:
//   * bc broadcast -> base vf[ft], offset bc*4
//   * Q / I        -> base &state, offset offsetof(VU1State, q/i)
//   * full vt      -> base vf[ft], offset 0 (the scalar is then masked away by vtMask)
// so no address is ever formed outside the object that owns it.
//
// MAC/STATUS come from the same g421MakeMacReg / g329UpdateStatus classifier G330 ships, and the
// destination write is the same g421StoreMasked bit-select. Ops outside the FMAC family
// (ITOF/FTOI/ABS/MOVE/MR32, and the cross-lane OPMSUB/OPMULA/CLIPw) return false and fall
// through to the unchanged G421 path, which in turn falls through to legacy execUpper.
//
// Arm:      DC2_G426_FUSED_UPPER=1
// Kill:     DC2_G426_NO_FUSED_UPPER=1
// Verify:   DC2_G426_VERIFY=1  (shadow-executes legacy execUpper and compares the whole state)
namespace
{
    struct G426Ctl
    {
        __m128 vtMask;   // b   = vt (full vector)        : else the broadcast scalar
        __m128 qMulMask; // q   = vs*b                    : else b
        __m128 pAccMask; // p   = acc                     : else vs
        __m128 subMask;  // r   = p - q                   : else p + q
        __m128 mulMask;  // r   = vs*b (MUL bypass)       : else the adder result
        __m128 mmMask;   // r   = the min/max result      : else the adder result
        __m128 minMask;  // mm  = min(vs,b)               : else max(vs,b)
        uint32_t srcUseFt; // 1 -> scalar base is vf[ft], 0 -> scalar base is the VU1State
        uint32_t srcOff;   // byte offset from that base
        uint32_t accDest;  // 1 -> destination is ACC, 0 -> destination is vf[fd]
        uint32_t valid;    // 1 -> this kind is covered by the fused FMAC path
    };
    static_assert(sizeof(G426Ctl) == 128u, "G426Ctl must stay two cache lines");

    struct G426CtlTable
    {
        G426Ctl e[0x80] = {};
    };

    G426CtlTable g426BuildCtlTable()
    {
        G426CtlTable t;
        const __m128 kOnes = _mm_castsi128_ps(_mm_set1_epi32(-1));
        const __m128 kZero = _mm_setzero_ps();
        const G421UpperDesc *desc = g421Desc();
        for (unsigned kind = 0u; kind < 0x80u; ++kind)
        {
            G426Ctl &c = t.e[kind];
            c.vtMask = kZero;
            c.qMulMask = kZero;
            c.pAccMask = kZero;
            c.subMask = kZero;
            c.mulMask = kZero;
            c.mmMask = kZero;
            c.minMask = kZero;
            c.srcUseFt = 1u;
            c.srcOff = 0u;
            c.accDest = 0u;
            c.valid = 0u;

            const G421UpperDesc d = desc[kind];
            if (d.cls != 1u && d.cls != 2u)
                continue; // ITOF/FTOI/ABS/MOVE/MR32/CLIPw and unsupported stay on the G421 path
            // G488: OPMSUB (op 7) and OPMULA (op 8) are cls 1/2 FMACs but their operands are a
            // CROSS-LANE permutation, which this control record cannot express — its `op` switch
            // would silently fold them into its `default` (MIN). They stay on the G421 path.
            if (d.op > 6u)
                continue;

            c.valid = 1u;
            c.accDest = (d.cls == 2u) ? 1u : 0u;
            // src: 0 broadcast(vt, bc) | 1 Q | 2 I | 3 vt
            switch (d.src)
            {
            case 0u:
                c.srcUseFt = 1u;
                c.srcOff = static_cast<uint32_t>(d.bc) * 4u;
                break;
            case 1u:
                c.srcUseFt = 0u;
                c.srcOff = static_cast<uint32_t>(offsetof(VU1State, q));
                break;
            case 2u:
                c.srcUseFt = 0u;
                c.srcOff = static_cast<uint32_t>(offsetof(VU1State, i));
                break;
            default:
                c.srcUseFt = 1u;
                c.srcOff = 0u;
                c.vtMask = kOnes;
                break;
            }
            // op: 0 ADD | 1 SUB | 2 MADD | 3 MSUB | 4 MUL | 5 MAX | 6 MIN
            switch (d.op)
            {
            case 0u:
                break;
            case 1u:
                c.subMask = kOnes;
                break;
            case 2u:
                c.qMulMask = kOnes;
                c.pAccMask = kOnes;
                break;
            case 3u:
                c.qMulMask = kOnes;
                c.pAccMask = kOnes;
                c.subMask = kOnes;
                break;
            case 4u:
                c.mulMask = kOnes;
                break;
            case 5u:
                c.mmMask = kOnes;
                break;
            default:
                c.mmMask = kOnes;
                c.minMask = kOnes;
                break;
            }
        }
        return t;
    }

    const G426Ctl *g426CtlTable()
    {
        static const G426CtlTable s_table = g426BuildCtlTable();
        return s_table.e;
    }

    bool g426FusedRequested()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G426_FUSED_UPPER") &&
                                 !f50_12_vu_env_flag("DC2_G426_NO_FUSED_UPPER");
        return s_on;
    }

    bool g426VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G426_VERIFY");
        return s_on;
    }

    // Shadow-verify scratch, for the same reason G421 keeps its copies out of run()'s frame.
    thread_local VU1State s_g426VerBefore;
    thread_local VU1State s_g426VerFast;

    static DC2_G330_FORCEINLINE __m128 g426Sel(__m128 mask, __m128 a, __m128 b)
    {
        return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
    }

    // Executes the upper slot with no data-dependent branch. Returns false when the kind is not
    // an FMAC form, in which case the caller falls through to the G421 / legacy path.
    static DC2_G330_FORCEINLINE bool g426FastUpper(
        VU1State &st, uint32_t upper, uint8_t kind, const G426Ctl *ctl)
    {
        // G488: this table is 0x80 entries and `kind` is the raw sidecar byte, which is 0xff for an
        // encoding with no compiled kind. Same latent out-of-bounds read G488 closed in
        // G421DescTable, closed here with a bound test instead because a G426Ctl is 128 bytes and
        // this whole path is default-off (DC2_G426_FUSED_UPPER).
        if (kind >= 0x80u)
            return false;
        const G426Ctl &c = ctl[kind];
        if (c.valid == 0u)
            return false;

        const float *const ftp = st.vf[FT(upper)];
        const __m128 vs = _mm_loadu_ps(st.vf[FS(upper)]);
        const __m128 vt = _mm_loadu_ps(ftp);

        const uint8_t *const scalarBase = c.srcUseFt
            ? reinterpret_cast<const uint8_t *>(ftp)
            : reinterpret_cast<const uint8_t *>(&st);
        const __m128 scalar = _mm_load_ss(
            reinterpret_cast<const float *>(scalarBase + c.srcOff));
        const __m128 bcast = _mm_shuffle_ps(scalar, scalar, _MM_SHUFFLE(0, 0, 0, 0));

        const __m128 b = g426Sel(c.vtMask, vt, bcast);
        const __m128 prod = _mm_mul_ps(vs, b);
        const __m128 q = g426Sel(c.qMulMask, prod, b);
        const __m128 p = g426Sel(c.pAccMask, _mm_loadu_ps(st.acc), vs);

        __m128 r = g426Sel(c.subMask, _mm_sub_ps(p, q), _mm_add_ps(p, q));
        r = g426Sel(c.mulMask, prod, r);
        r = g426Sel(c.mmMask,
                    g426Sel(c.minMask, _mm_min_ps(vs, b), _mm_max_ps(vs, b)),
                    r);

        const uint8_t dest = DEST(upper);
        const uint32_t mac = g421MakeMacReg(r, dest);
        st.mac = mac;
        g329UpdateStatus(st.status, mac);
        const __m128 mask =
            _mm_load_ps(reinterpret_cast<const float *>(g421MaskRows()[dest]));
        g421StoreMasked(c.accDest ? st.acc : st.vf[FD(upper)], r, mask);
        return true;
    }
} // namespace
