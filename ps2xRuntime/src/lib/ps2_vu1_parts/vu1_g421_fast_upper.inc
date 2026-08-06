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
// G488 CLOSED the last three: OPMSUB (0x2E), OPMULA (0x6E) and CLIPw (0x5F) — the only `cls == 0`
// encodings this route ever executes — now run inline as well (see the G488 block below). Every
// diagnostic mode that perturbs the FMAC result (DC2_NO_VU1_MAC, DC2_VU1_CLAMP, G71 clamp census,
// G330 rollback) still keeps the legacy execUpper path, and so does the unmapped 0x6B encoding.
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
    //      8 = CLIPw -> m_state.clip only (G488; no VF/ACC, no MAC/STATUS)
    // op:  0 ADD  1 SUB  2 MADD  3 MSUB  4 MUL  5 MAX  6 MIN
    //      7 OPMSUB (G488, cross-lane)        8 OPMULA (G488, cross-lane)
    // src: 0 broadcast(vt, bc)  1 Q  2 I  3 vt
    struct G421UpperDesc
    {
        uint8_t cls = 0u;
        uint8_t op = 0u;
        uint8_t src = 0u;
        uint8_t bc = 0u; // broadcast lane, or ITOF/FTOI scale index
    };

    // G488: 0x100 entries, not 0x80. `g421FastUpper` indexes this with the RAW `upperKind` byte
    // from the G410 sidecar, and that byte is 0xff whenever the upper encoding has no compiled
    // kind (op 0x30-0x3b, Special2 func 0x2b or > 0x31) or the sidecar pointer was null — a read
    // 508 bytes past the end of a 0x80-entry table, i.e. `cls` came from whatever .rdata followed
    // it. `[G410:jit] upperFallback=0` on lean MAP-0 means no such slot is resident on THIS route,
    // which is why it has never fired, but it is a latent out-of-bounds read on any route that has
    // one. Sizing the table to the full uint8_t domain closes it for free: the 0x72-0xff entries
    // are zero-initialised, so `cls == 0` and the caller falls back to execUpper exactly as
    // intended. The upper half also gives G488's A/B its SHADOW KINDS (0x72/0x73/0x74) a home.
    struct G421DescTable
    {
        G421UpperDesc e[0x100] = {};
    };

    // G436: constexpr, so the table below is a link-time constant with NO initialisation guard.
    // The body is unchanged; only the evaluation time moves.
    constexpr G421DescTable g421BuildDescTable()
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
        set(0x2Eu, 1u, 7u, 3u, 0u); // G488 OPMSUB (cross-lane, w lane forced to +0.0)
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
        set(0x5Fu, 8u, 0u, 0u, 0u); // G488 CLIPw (writes only m_state.clip)
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
        set(0x6Eu, 2u, 8u, 3u, 0u); // G488 OPMULA (cross-lane, w lane keeps ACC.w)
        // 0x6F VNOP never reaches here.
        set(0x70u, 6u, 0u, 0u, 0u); // MOVE
        set(0x71u, 7u, 0u, 0u, 0u); // MR32
        return t;
    }

    // G436: the SAME table, constant-initialised. `g421Desc()` used to wrap a function-local
    // `static const` whose thread-safe-initialisation guard makes the accessor un-inlinable, so
    // every covered upper (50.3% of ~1.65 M pairs/frame) paid a real `call g421Desc` — plus the
    // caller-saved spills that a call forces in this very register-hungry loop. Verified in the
    // /FAsc listing of the shipped Release binary: `call ?g421Desc@...` sat directly on the
    // fast-upper path with `mov QWORD PTR $T7[rbp-464], rax` spilling the G430 stamp around it.
    constexpr G421DescTable g421DescTableConst = g421BuildDescTable();

    // The out-of-line accessor is retained as the A/B CONTROL ARM (DC2_G436_LEGACY_ADDR=1) and
    // for the cold census callers. `noinline` pins the legacy call shape so the two arms differ
    // by exactly the thing under test.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    const G421UpperDesc *g421Desc()
    {
        return g421DescTableConst.e;
    }

    // dest bit3 = x (lane 0) ... bit0 = w (lane 3), matching applyDest().
    struct G421MaskTable
    {
        alignas(16) uint32_t m[16][4] = {};
    };

    constexpr G421MaskTable g421BuildMaskTable()
    {
        G421MaskTable t;
        for (unsigned d = 0u; d < 16u; ++d)
            for (unsigned lane = 0u; lane < 4u; ++lane)
                t.m[d][lane] = (d & (0x8u >> lane)) ? 0xFFFFFFFFu : 0u;
        return t;
    }

    // G436: same table, constant-initialised (see g421DescTableConst).
    constexpr G421MaskTable g421MaskTableConst = g421BuildMaskTable();

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    const uint32_t (*g421MaskRows())[4]
    {
        return g421MaskTableConst.m;
    }

    // G488 - CLIPw flag word, computed in registers.
    //
    // execUpper's `case 0x1F` is six ORDERED float comparisons of vs.xyz against +|vt.w| and
    // -|vt.w|, packed as (x>, x<, y>, y<, z>, z<) into bits 0..5. `_mm_cmpgt_ps` / `_mm_cmplt_ps`
    // lower to `cmpltps` with ordered predicates, so a NaN operand yields false on exactly the
    // same side as the C++ `>` / `<` the legacy body uses. `and` with 0x7FFFFFFF is `std::fabs`
    // bit-for-bit (including NaN payloads), and `xor` with the sign bit is the negation — for
    // w = +0.0 it yields -0.0 where the legacy `-w` may yield +0.0, which is unobservable because
    // IEEE comparison treats the two as equal.
    //
    // The interleave of the two 3-bit movemasks is the only part that is not free in SSE2 (there
    // is no PDEP at this ISA baseline), so it is one 64-byte table load off `gt | (lt << 3)`.
    struct G488ClipTable
    {
        uint8_t e[64] = {};
    };

    constexpr G488ClipTable g488BuildClipTable()
    {
        G488ClipTable t;
        for (unsigned gt = 0u; gt < 8u; ++gt)
            for (unsigned lt = 0u; lt < 8u; ++lt)
            {
                unsigned f = 0u;
                for (unsigned lane = 0u; lane < 3u; ++lane)
                {
                    if ((gt & (1u << lane)) != 0u)
                        f |= 1u << (2u * lane); // vs[lane] > +|w|
                    if ((lt & (1u << lane)) != 0u)
                        f |= 2u << (2u * lane); // vs[lane] < -|w|
                }
                t.e[gt | (lt << 3u)] = static_cast<uint8_t>(f);
            }
        return t;
    }

    // Link-time constant with no initialisation guard, for the same reason as g421DescTableConst.
    constexpr G488ClipTable g488ClipTableConst = g488BuildClipTable();

    static DC2_G330_FORCEINLINE uint32_t g488ClipFlags(__m128 vs, __m128 vt)
    {
        const __m128 aw = _mm_and_ps(_mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 3, 3, 3)),
                                     _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
        const __m128 nw = _mm_xor_ps(aw, _mm_castsi128_ps(_mm_set1_epi32(INT32_MIN)));
        const uint32_t gt =
            static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpgt_ps(vs, aw))) & 7u;
        const uint32_t lt =
            static_cast<uint32_t>(_mm_movemask_ps(_mm_cmplt_ps(vs, nw))) & 7u;
        return g488ClipTableConst.e[gt | (lt << 3u)];
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

    // G428 - dest-shape specialised destination write.
    //
    // The G421 bit-select above costs FIVE SSE2 ops on every covered upper (mask-table load,
    // destination load, and, andnot, or, store) regardless of the dest mask, and G426 priced this
    // loop at ~1 host cycle per SSE2 op. The G428 census over lean MAP-0 measured the actual dest
    // distribution of the 45%-of-cycles FMAC population:
    //     0xF = 65.8%   0x1 = 22.2%   0xE = 4.0%   0xD = 3.8%   0xC = 2.5%   0x4 = 1.3%
    //     0x8 = 0.4%    0x2 = 0.0%
    // i.e. 65.8% write all four lanes and a further 23.9% write exactly one. A full-dest write is
    // one unmasked store (the read-modify-write is provably the identity), and a single-lane write
    // is one shuffle plus one 4-byte store. Only the 10.3% genuinely-partial remainder needs the
    // mask table and the bit-select, and it is now the only path that loads either.
    //
    // EXACTNESS. dest==0xF: applyDest() assigns all four lanes, so storing all four lanes is the
    // same write. Single lane L: `_mm_store_ss` writes exactly the 4 bytes of dst[L] with the low
    // lane of the shuffled vector, which is r[L] — the same value applyDest() assigns, and no
    // other byte of the destination is touched. dest==0 writes nothing on either path (the mask is
    // all-zero, so the bit-select rewrites the destination with its own bits).
    static const bool s_g428DestStore = (std::getenv("DC2_G428_NO_DEST_STORE") == nullptr);

    // G436: `leanTab` selects the constant-initialised mask table over the out-of-line accessor.
    // Same 16 bytes either way; only how the address is formed changes.
    static DC2_G330_FORCEINLINE const uint32_t *g436MaskRow(bool leanTab, uint8_t dest)
    {
        return leanTab ? g421MaskTableConst.m[dest] : g421MaskRows()[dest];
    }

    // G486 REFUTED widening the four SINGLE-LANE destination writes below.
    //
    // The premise was G485's: this loop pays a store-to-load forwarding block whenever it writes a
    // VF register narrowly and reads it back as a 16-byte vector, and `_mm_store_ss` writes 4 bytes
    // of a register whose next reader is `_mm_loadu_ps(st.vf[FS])` in the following pair. The site
    // is real and was priced first (G476): ~24.0% of the ~732 k inline FMACs per frame take a
    // single-lane arm (dest 0x1 = 22.3%, 0x4 = 1.3%, 0x8 = 0.4%, 0x2 = 0.0%), plus the cls>=3 share.
    //
    // The candidate replaced each `_mm_store_ss` with a whole-lane blend (`_mm_move_ss` for lane 0,
    // one `unpck*ps` + one `shufps` for lanes 1-3 — bit-exact, no mask constants) and one
    // `_mm_storeu_ps`, built as a loop COPY exactly like G485's widevf0. Four paired A/Bs
    // (DC2_G419_AB=widedest, both arms confirmed used) read +0.732 / +0.476 / +0.553 / +0.627 ms/f:
    // mean **+0.597, sign 4/4 SLOWER**.
    //
    // WHY, and this is the durable half. G485's win was four narrow stores of CONSTANTS collapsing
    // into one wide store, which needs no input. A single-lane write cannot be widened without
    // READING the destination's other three lanes, so the candidate turns a pure store into a
    // load-modify-store: a 16-byte load of `vf[fd]` joins the dependency chain and `old` stays live
    // across the blend. That costs more than the forwarding block it removes. The store-forwarding
    // class needs the wide replacement to be INPUT-FREE; it does not generalise from "a hot loop
    // writes lanes individually" to "any narrow store this loop makes". Do not rebuild it.
    // G501: `G501Pin` says the CALLER has already proved `g436Lean` and `s_g428DestStore` — see
    // `g490BlockBody` in vu1_g490_block_run.inc. Both then constant-fold, which deletes the
    // `g421MaskRows()` arm (a real out-of-line call) and the whole `!s_g428DestStore` control-arm
    // block from this body. Default false, so every existing call site is byte-identical.
    template <bool G501Pin = false>
    static DC2_G330_FORCEINLINE void g428StoreDest(float *dst, __m128 value, uint8_t dest,
                                                   bool leanTab)
    {
        if constexpr (G501Pin)
            leanTab = true;
        if (!(G501Pin || s_g428DestStore))
        {
            // Control arm: the G421 shape, byte-identical to the pre-G428 default.
            g421StoreMasked(
                dst, value,
                _mm_load_ps(reinterpret_cast<const float *>(g436MaskRow(leanTab, dest))));
            return;
        }
        switch (dest)
        {
        case 0xFu:
            _mm_storeu_ps(dst, value);
            return;
        case 0x8u:
            _mm_store_ss(dst + 0, value);
            return;
        case 0x4u:
            _mm_store_ss(dst + 1, _mm_shuffle_ps(value, value, _MM_SHUFFLE(1, 1, 1, 1)));
            return;
        case 0x2u:
            _mm_store_ss(dst + 2, _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
            return;
        case 0x1u:
            _mm_store_ss(dst + 3, _mm_shuffle_ps(value, value, _MM_SHUFFLE(3, 3, 3, 3)));
            return;
        default:
            g421StoreMasked(
                dst, value,
                _mm_load_ps(reinterpret_cast<const float *>(g436MaskRow(leanTab, dest))));
            return;
        }
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
    // G428: classify a parked delay-line entry. Only reached at a consumer pair (2.06% of pairs
    // on lean MAP-0) and at run() exit, so its ~10 SSE2 ops are paid ~20x less often.
    static inline uint32_t g428ResolveSlot(int slot)
    {
        if (s_g428Kind[slot] == G428_MK_PENDING)
            return g421MakeMacReg(_mm_load_ps(s_g428PendVec[slot]), s_g428PendDest[slot]);
        if (s_g428Kind[slot] == G428_MK_EAGER)
            return s_vuMacPipe[slot];
        // CARRY: this pair wrote no MAC, so the visible value is whatever the newest non-CARRY
        // entry that has already left the window produced.
        if (s_g428PreKind == G428_MK_PENDING)
            return g421MakeMacReg(_mm_load_ps(s_g428PreVec), s_g428PreDest);
        return s_g428PreMac;
    }

    // G430: record an inline FMAC result in the stamped history. Called only from the FMAC path
    // (45% of pairs), never from the loop body.
    // G436: `hb` is the run()-hoisted address of the thread's history block. Passing nullptr
    // re-derives it here, which is the DC2_G436_LEGACY_ADDR control arm (one TLS materialisation
    // per push instead of one per run()).
    // G501: same pin as g421FastUpper — when the caller has proved `hb != nullptr` the ternary
    // (a null test plus a reference to the thread_local fallback, on every inline FMAC) folds to
    // the dereference the caller already intended.
    template <bool G501Pin = false>
    static DC2_G330_FORCEINLINE void g430PushPending(G430HistBlock *hb, int64_t stamp, __m128 r,
                                                     uint8_t dest)
    {
        G430HistBlock &H = G501Pin ? *hb : (hb ? *hb : s_g430HB);
        const int slot = H.head;
        _mm_store_ps(H.vec[slot], r);
        H.stamp[slot] = stamp;
        H.dest[slot] = dest;
        H.kind[slot] = G428_MK_PENDING;
        H.head = (slot + 1 < G430_HIST) ? slot + 1 : 0;
    }

    // G504: the SAME push with the ring cursor carried in the CALLER's local instead of in
    // `H.head`. Statement for statement identical to `g430PushPending` above — the four stores are
    // the same stores to the same slot — except that the cursor is read from and written to
    // `slot` rather than to memory.
    //
    // WHY. `[G483:rawpc]` + the linker map (G504 §3) put the single hottest instruction of the
    // whole VU1 worker inside this function, and the block around it at ~9.2% of the thread. The
    // shipped disassembly of `g490BlockBody<1,0,1,1,0>` reads, on EVERY inline FMAC:
    //
    //     mov    rdi, qword ptr [rbp+0x80]      ; reload `hb` from its stack home
    //     movsxd r8,  dword ptr [rdi+0xf0]      ; load H.head
    //     ... four stores ...
    //     mov    dword ptr [rdi+0xf0], ecx      ; store H.head back
    //
    // so consecutive pushes are chained through MEMORY: each `movsxd` waits on the previous
    // iteration's store to forward. That is a genuine loop-carried dependence, which is the one
    // class the out-of-order engine cannot hide — unlike the six refuted "remove an instruction
    // from this loop" levers (G427/G442/G479/G483/G485/G486), whose operands were L1-resident
    // behind perfectly-predicted branches. It is the same class as G485's win.
    //
    // The reload exists because the block calls `g422FastLower` for the lower kinds G481 left
    // out-of-line, and a call clobbers all memory — so every loop-invariant value in this leaf is
    // respilled and reloaded per pair. A LOCAL whose address never escapes is the one thing a call
    // cannot clobber, which is why the cursor must be a local and not a hoisted pointer.
    //
    // EXACTNESS. Inside a block the only writer of `H.head` is this push: `g430PushEager` is called
    // only from vu1_upper_opcodes.inc (the legacy pair-loop macro and run()'s seeding prologue),
    // `g430ResolveAt` scans all G430_HIST entries by STAMP and never reads `head`, and
    // `g422FastLower` — the one call the block makes — cannot reach `s_g430HB` at all (it lives in
    // vu1_g422_fast_lower.inc, which does not name it). So carrying the cursor across the block and
    // writing it back once at the single exit leaves `H` byte-identical to what the per-push form
    // would have left, entry by entry and in the same slots.
    template <bool G501Pin = false>
    static DC2_G330_FORCEINLINE void g430PushPendingAt(G430HistBlock *hb, int &slot, int64_t stamp,
                                                       __m128 r, uint8_t dest)
    {
        G430HistBlock &H = G501Pin ? *hb : (hb ? *hb : s_g430HB);
        const int s = slot;
        _mm_store_ps(H.vec[s], r);
        H.stamp[s] = stamp;
        H.dest[s] = dest;
        H.kind[s] = G428_MK_PENDING;
        slot = (s + 1 < G430_HIST) ? s + 1 : 0;
    }

    // G504: the default target of `g421FastUpper`'s cursor parameter. Every call site that does not
    // opt in instantiates with `G504Cursor = false`, and the `if constexpr` below discards the arm
    // that names it — so this object is never read, never written, and generates no code.
    int g504UnusedSlot = 0;

    // G430: record an already-classified MAC (a writer outside the inline FMAC path, e.g. the
    // legacy execUpper on an encoding G421 does not cover). Rare by construction.
    static inline void g430PushEager(int64_t stamp, uint32_t mac)
    {
        G430HistBlock &H = s_g430HB;
        const int slot = H.head;
        H.stamp[slot] = stamp;
        H.mac[slot] = mac;
        H.kind[slot] = G428_MK_EAGER;
        H.head = (slot + 1 < G430_HIST) ? slot + 1 : 0;
    }

    // G430: the architectural MAC as of pair `target` = the newest recorded writer with a stamp
    // at or before it. Reached only at a flag-reading pair (2.06%) and at run() exit.
    static inline uint32_t g430ResolveAt(int64_t target)
    {
        G430HistBlock &H = s_g430HB;
        int best = -1;
        int64_t bestStamp = INT64_MIN;
        for (int i = 0; i < G430_HIST; ++i)
        {
            const int64_t s = H.stamp[i];
            if (s <= target && s > bestStamp)
            {
                bestStamp = s;
                best = i;
            }
        }
        if (best < 0)
            return 0u; // no writer has ever executed: the legacy pipe reads its zero init
        if (H.kind[best] == G428_MK_PENDING)
            return g421MakeMacReg(_mm_load_ps(H.vec[best]), H.dest[best]);
        return H.mac[best];
    }

    // Convert the delay line OUT of the deferred representation: classify every parked entry and
    // propagate the carried value through the CARRY slots, oldest first, so `s_vuMacPipe` again
    // holds exactly what the eager path would have written on every one of the last DEPTH pairs.
    static void g428MaterializePipe(int depth, int head)
    {
        uint32_t carry = (s_g428PreKind == G428_MK_PENDING)
            ? g421MakeMacReg(_mm_load_ps(s_g428PreVec), s_g428PreDest)
            : s_g428PreMac;
        for (int age = depth - 1; age >= 0; --age)
        {
            const int slot = (head + age < depth) ? head + age : head + age - depth;
            if (s_g428Kind[slot] == G428_MK_PENDING)
                carry = g421MakeMacReg(_mm_load_ps(s_g428PendVec[slot]), s_g428PendDest[slot]);
            else if (s_g428Kind[slot] == G428_MK_EAGER)
                carry = s_vuMacPipe[slot];
            s_vuMacPipe[slot] = carry;
        }
    }

    // G442 - DENORMAL census on the VU1 FMAC path (default OFF: DC2_G442_DENORM=1).
    //
    // PREMISE. G441 priced the pole at ~26 ns/pair (~100 host cycles) against an executed SSE2 mix
    // worth ~12, and G436/G437 closed instruction count, footprint, addressing and data placement.
    // One latency class the arc has never checked is the x86 DENORMAL ASSIST: nothing in this tree
    // touches MXCSR (no FTZ, no DAZ), so a subnormal operand or result on `addps`/`mulps` traps to
    // microcode for tens to ~200 cycles. The PS2 VU has no subnormals at all — it flushes them —
    // so any subnormal seen here is a host-representation artefact, and its cost is pure loss.
    // This census answers "do they occur, and on what share of FMACs" before any lever is built.
    //
    // Counts LANES (not pairs): a subnormal in either source and a subnormal in the result.
    //
    // RESULT (lean MAP-0, 620 M FMACs): srcFmac 5.56%, srcLane 2.55%, dstFmac 2.56%, dstLane 1.20%.
    // Subnormals are real but the assist is cheap on this host: DC2_G442_FTZ=1 (FTZ|DAZ on the VU1
    // worker) is worth only ~0.4 ms/f, so the vein is priced out. The hook itself is compiled out —
    // see the DC2_G442_DENORM_CENSUS note at its call site.
#ifndef DC2_G442_DENORM_CENSUS
#define DC2_G442_DENORM_CENSUS 0
#endif
#if DC2_G442_DENORM_CENSUS
    static const bool s_g442Census = (std::getenv("DC2_G442_DENORM") != nullptr);
    thread_local uint64_t s_g442Fmacs = 0ull;
    thread_local uint64_t s_g442SrcLanes = 0ull;
    thread_local uint64_t s_g442DstLanes = 0ull;
    thread_local uint64_t s_g442SrcFmacs = 0ull;
    thread_local uint64_t s_g442DstFmacs = 0ull;

    // lanes whose value is subnormal: biased exponent 0 and mantissa non-zero.
    static inline uint32_t g442SubMask(__m128 v)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i absBits = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i expBits = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i isZero = _mm_cmpeq_epi32(absBits, _mm_setzero_si128());
        const __m128i expZero = _mm_cmpeq_epi32(expBits, _mm_setzero_si128());
        return (uint32_t)_mm_movemask_ps(
            _mm_castsi128_ps(_mm_andnot_si128(isZero, expZero)));
    }

    static inline void g442NoteFmac(__m128 vs, __m128 b, __m128 r)
    {
        const uint32_t s = g442SubMask(vs) | g442SubMask(b);
        const uint32_t d = g442SubMask(r);
        ++s_g442Fmacs;
        if (s) { s_g442SrcLanes += (uint64_t)_mm_popcnt_u32(s); ++s_g442SrcFmacs; }
        if (d) { s_g442DstLanes += (uint64_t)_mm_popcnt_u32(d); ++s_g442DstFmacs; }
        if ((s_g442Fmacs % 20000000ull) == 0ull)
            std::fprintf(stderr,
                         "[G442:denorm] fmacs=%llu | srcFmac=%.4f%% srcLane=%.4f%% "
                         "dstFmac=%.4f%% dstLane=%.4f%%\n",
                         (unsigned long long)s_g442Fmacs,
                         100.0 * (double)s_g442SrcFmacs / (double)s_g442Fmacs,
                         100.0 * (double)s_g442SrcLanes / (4.0 * (double)s_g442Fmacs),
                         100.0 * (double)s_g442DstFmacs / (double)s_g442Fmacs,
                         100.0 * (double)s_g442DstLanes / (4.0 * (double)s_g442Fmacs));
    }
#endif // DC2_G442_DENORM_CENSUS

    // G501: `G501Pin` is the compile-time form of "`g436Hb` is non-null and `s_g428DestStore` is
    // true", which `g443Fixed` already proves for every caller inside the block copy (it lists
    // `g436Lean && g436Hb != nullptr` among its conjuncts, and G501 adds `s_g428DestStore`).
    // Pinning them here is what lets MSVC delete the `g421Desc()` / `g421MaskRows()` arms — both
    // REAL out-of-line calls — and the reload of `g436Hb` from its stack slot on every pair.
    // Defaults to false, so every pre-existing call site compiles to exactly the same code.
    // G504: `G504Cursor` says the CALLER carries the G430 ring cursor in `g504Slot` (a local of
    // `g490BlockBody`) instead of in `H.head`. Defaults to false with the parameter bound to a
    // never-read global, so every pre-existing call site compiles to exactly the same code.
    template <bool G501Pin = false, bool G504Cursor = false>
    static DC2_G330_FORCEINLINE bool g421FastUpper(VU1State &st, uint32_t upper, uint8_t kind,
                                                   bool g428Defer, bool g428Shadow = false,
                                                   bool g430Hist = false, int64_t g430Stamp = 0,
                                                   G430HistBlock *g436Hb = nullptr,
                                                   int &g504Slot = g504UnusedSlot)
    {
        // G436: a non-null history block also means "lean addressing" — the run() hoist is armed,
        // so the constant-initialised tables are used instead of the out-of-line accessors.
        const bool g436Lean = G501Pin ? true : (g436Hb != nullptr);
        const G421UpperDesc d = g436Lean ? g421DescTableConst.e[kind] : g421Desc()[kind];
        if (d.cls == 0u)
            return false;

        const __m128 vs = _mm_loadu_ps(st.vf[FS(upper)]);
        const uint8_t dest = DEST(upper);

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
            case 8u: // G488 CLIPw : the ONLY covered upper that writes neither VF nor ACC.
                // execUpper: `m_state.clip = (m_state.clip << 6) | flags`, no MAC, no STATUS,
                // no destination write and `dest` is a don't-care field. Returns from inside the
                // switch so the shared VF[ft] store below is not reached.
                st.clip = (st.clip << 6) |
                          g488ClipFlags(vs, _mm_loadu_ps(st.vf[FT(upper)]));
                return true;
            default: // 7u MR32 : {vs[1], vs[2], vs[3], vs[0]}
                r = _mm_shuffle_ps(vs, vs, _MM_SHUFFLE(0, 3, 2, 1));
                break;
            }
            // None of the five VF-writing forms above updates MAC/STATUS, and all write VF[ft].
            g428StoreDest<G501Pin>(st.vf[FT(upper)], r, dest, g436Lean);
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
        // G488 - the two cross-lane FMAC forms. Both build the SAME outer-product permutation:
        //   a  = {vs.y, vs.z, vs.x, vs.w}   bb = {vt.z, vt.x, vt.y, vt.w}
        //   a*bb = {vs.y*vt.z, vs.z*vt.x, vs.x*vt.y, (don't care)}
        // which is execUpper's operand pairing lane for lane. `d.src` is 3 for both, so `b` is
        // already the full `vt` vector.
        case 7u: // OPMSUB : ACC - (vs x vt), w lane forced to +0.0
        {
            const __m128 g488a = _mm_shuffle_ps(vs, vs, _MM_SHUFFLE(3, 0, 2, 1));
            const __m128 g488b = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
            // `result[3] = 0.0f` in execUpper. Masking the w lane to all-zero bits produces the
            // +0.0f bit pattern for ANY value the subtract left there, so the classifier and the
            // destination store both see exactly what the legacy body wrote.
            r = _mm_and_ps(
                _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(g488a, g488b)),
                _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0)));
            break;
        }
        case 8u: // OPMULA : (vs x vt) -> ACC, w lane keeps the CURRENT ACC.w
        {
            const __m128 g488a = _mm_shuffle_ps(vs, vs, _MM_SHUFFLE(3, 0, 2, 1));
            const __m128 g488b = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
            const __m128 g488p = _mm_mul_ps(g488a, g488b);
            // `result[3] = m_state.acc[3]`, read BEFORE applyDestAcc writes ACC — so a dest that
            // includes w rewrites ACC.w with its own value, and the MAC classifies that value.
            // unpackhi gives {p.z, acc.z, p.w, acc.w}; the shuffle then takes {p.x, p.y, p.z, acc.w}.
            const __m128 g488acc = _mm_loadu_ps(st.acc);
            r = _mm_shuffle_ps(g488p, _mm_unpackhi_ps(g488p, g488acc),
                               _MM_SHUFFLE(3, 0, 1, 0));
            break;
        }
        default: r = _mm_min_ps(vs, b); break;
        }

#if DC2_G442_DENORM_CENSUS
        // Compile-time OFF, but MEASURED CHEAP: with the runtime `if (s_g442Census)` present and
        // the flag unset the frame moved +0.08 ms against the same control (two order-balanced
        // A/Bs, 47.74 vs 47.27 candidate means). It is compiled out only because it has already
        // answered its question. Re-arm with /D DC2_G442_DENORM_CENSUS=1; never ship it on.
        if (s_g442Census)
            g442NoteFmac(vs, b, r);
#endif

        if (g430Hist)
        {
            // G430: record (stamp, result, dest). No head advance, no tag, no carry: the consumer
            // finds this entry by stamp.
            // G504: same four stores; only where the ring cursor lives differs.
            if constexpr (G504Cursor)
                g430PushPendingAt<G501Pin>(g436Hb, g504Slot, g430Stamp, r, dest);
            else
                g430PushPending<G501Pin>(g436Hb, g430Stamp, r, dest);
            if (g428Shadow)
            {
                const uint32_t mac = g421MakeMacReg(r, dest);
                st.mac = mac;
                g329UpdateStatus(st.status, mac);
            }
        }
        else if (g428Defer)
        {
            // Park the unclassified result in the delay-line slot this pair owns (the head was
            // advanced before the pair executed, so this IS the entry the push will publish).
            const int slot = s_g328MacPipeHead;
            _mm_store_ps(s_g428PendVec[slot], r);
            s_g428PendDest[slot] = dest;
            s_g428Claim = G428_MK_PENDING;
            if (g428Shadow)
            {
                // DC2_G428_VERIFY: keep the legacy eager classification running alongside, so
                // the delay line carries the ground-truth value the resolver must reproduce.
                const uint32_t mac = g421MakeMacReg(r, dest);
                st.mac = mac;
                g329UpdateStatus(st.status, mac);
            }
        }
        else
        {
            const uint32_t mac = g421MakeMacReg(r, dest);
            st.mac = mac;
            g329UpdateStatus(st.status, mac);
        }
        g428StoreDest<G501Pin>(d.cls == 1u ? st.vf[FD(upper)] : st.acc, r, dest, g436Lean);
        return true;
    }
} // namespace
