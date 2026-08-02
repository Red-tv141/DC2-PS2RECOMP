// G410 - cached compiled execution for VU1 upper micro-ops.
//
// The VU1 code store is writable through VIF MPG, so a cache keyed only by PC is unsafe.
// VIF1 is the runtime's sole VU-code write funnel. It precisely invalidates touched slots on
// both the synchronous code store and the MTVU worker's ordered replay. Verification adds an
// exact 64-bit source guard on every hit to prove that the write boundary remains complete.
//
// Default:  enabled
// Kill:     DC2_G410_NO_VU_COMPILED=1
// Verify:   DC2_G410_VERIFY=1 (shadow-executes the legacy upper decoder)
// Stats:    DC2_G410_STAT=1

#include <array>

// G479: within-process paired A/B arm for the fused pair record (see G479FusedPair below).
extern int g479LeanFetchArm();

// G483: within-process paired A/B arm for the register-carried program counter.
extern int g483PcRegArm();

// G484: within-process paired A/B arm for the wide VF0 re-pin.
extern int g484WideVf0Arm();

namespace
{
    struct G410CompiledUpper
    {
        uint8_t upperVfDest = 0xffu;
        uint8_t upperKind = 0xffu;
        uint8_t lowerFs = 0u;
        uint8_t lowerFt = 0u;
        uint8_t pairFlags = 0u;
        uint8_t lowerS2 = 0xffu;
        uint8_t lowerKind = 0u; // G422: G422_LK_* (0 = not covered -> execLower)
        uint8_t g425Flags = 0u; // G425: see G425_PF_*
    };

    // G425: set when EITHER slot of this pair can name VF0 or VI0 as a destination. The run
    // loop re-pins VF0=(0,0,0,1) and VI0=0 after every pair to enforce the hardware invariant;
    // when no writer of this pair can reach register 0 that re-pin is provably the identity and
    // its five stores are skippable. Conservative by construction: any lower slot the G422
    // decoder does not cover, and any upper encoding whose VF destination is not one of the
    // known forms, sets the bit.
    constexpr uint8_t G425_PF_TOUCHES_ZERO = 0x01u;

    // G426: two more per-pair predicates the run loop used to RECOMPUTE on every executed pair,
    // although both are pure functions of the same 64-bit pair source this sidecar already
    // caches and invalidates.
    //   * G426_PF_PRESTALL - the lower slot is a scalar-pipe producer or waiter (FDIV/EFU/WAITQ/
    //     WAITP), i.e. the only case in which the G239 scalar-prestall block can publish
    //     anything. Clear on ~every pair of the transform loop, where the block otherwise costs
    //     a sidecar load plus ten compares per pair to decide to do nothing.
    //   * G426_PF_HAZ - the G139 same-pair hazard actually applies: the upper slot has a VF
    //     destination AND the lower slot names that register in fs or ft. The run loop used to
    //     re-derive the index and run both compares per pair; the answer never changes for a
    //     given pair encoding.
    constexpr uint8_t G426_PF_PRESTALL = 0x02u;
    constexpr uint8_t G426_PF_HAZ = 0x04u;

    // G427: the lower slot READS the pipelined MAC/STATUS snapshot. The G138/G328 delay line
    // publishes `s_vuMacVisible`/`s_vuStatusVisible` on every executed pair, but the only
    // consumers of those two words are six lower-slot opcodes (FSEQ/FSAND/FSOR and
    // FMEQ/FMAND/FMOR, plus FCGET in the legacy DC2_VU1_NO_FMSWAPFIX mapping). On every other
    // pair the publication is two loads and two stores whose result nothing can observe, so the
    // delay line only has to ADVANCE (a register index) and RECORD (the newest entry).
    constexpr uint8_t G427_PF_FLAGREAD = 0x08u;

    // G428: the STICKY subset of the above - FSEQ/FSAND/FSOR read `s_vuStatusVisible`, whose
    // sticky bits accumulate over every FMAC ever executed and therefore cannot be reconstructed
    // lazily. FMEQ/FMAND/FMOR read only `s_vuMacVisible`, which is a per-pair snapshot.
    constexpr uint8_t G428_PF_STATUSREAD = 0x10u;

    inline bool g428LowerReadsStatus(uint32_t lower)
    {
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25u) & 0x7fu);
        return opHi == 0x14u || opHi == 0x16u || opHi == 0x17u; // FSEQ / FSAND / FSOR
    }

    // The complete set of lower opcodes that read s_vuMacVisible / s_vuStatusVisible; see the
    // `switch (opHi)` in execLower. FCAND/FCOR (0x12/0x13) read m_state.clip, not the delay line.
    inline bool g427LowerReadsFlags(uint32_t lower)
    {
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25u) & 0x7fu);
        switch (opHi)
        {
        case 0x14u: // FSEQ
        case 0x16u: // FSAND
        case 0x17u: // FSOR
        case 0x18u: // FMEQ
        case 0x1au: // FMAND
        case 0x1bu: // FMOR
        case 0x1cu: // FCGET (reads MAC only under DC2_VU1_NO_FMSWAPFIX; kept unconditionally)
            return true;
        default:
            return false;
        }
    }

    // The G239 scalar-prestall set: WAITQ, DIV/SQRT/RSQRT, WAITP and the EFU producers.
    inline bool g426LowerIsScalarPipe(uint8_t lowerS2)
    {
        if (lowerS2 == 0xffu)
            return false;
        if (lowerS2 == 0x3bu || lowerS2 == 0x7bu)
            return true;                                   // WAITQ / WAITP
        if (lowerS2 >= 0x38u && lowerS2 <= 0x3au)
            return true;                                   // DIV / SQRT / RSQRT
        return lowerS2 == 0x70u || lowerS2 == 0x71u ||
               lowerS2 == 0x72u || lowerS2 == 0x73u ||
               lowerS2 == 0x76u || lowerS2 == 0x78u ||
               lowerS2 == 0x79u || lowerS2 == 0x7au;        // EFU producers
    }
    static_assert(sizeof(G410CompiledUpper) == 8u);

    constexpr uint8_t G410_PAIR_E_BIT = 0x01u;
    constexpr uint8_t G410_PAIR_I_BIT = 0x02u;

    // G479 - FUSED PAIR RECORD.
    //
    // PREMISE (G442/G443, still the standing model of this loop). The binding resource in the pair
    // loop is LIVE VALUES and dependency LATENCY, not instruction count: one added loop-carried
    // local was measured at +2.6..2.8 ms/f against 0.068 host cycles per deleted instruction.
    // G442's own note names the chain precisely — "the bounds check, the two `vuCode + currentPc`
    // fetches and the `g410Pairs[pc >> 3]` sidecar load" all hang off the `m_state.pc` load at the
    // top of the iteration. That is TWO independent address computations and THREE loads from TWO
    // arrays before the pair can start.
    //
    // MECHANISM. `sources[i]` already holds exactly the 64-bit pair word that lives at
    // `codeBase + i*8` (it is what g410VerifyCachedPair compares against, and precise invalidation
    // keeps it exact), and `uppers[i]` is exactly 8 bytes. Storing them ADJACENT as one 16-byte
    // record turns the whole fetch into a single scaled index off one base: one address chain, one
    // cache line, and the VU1 code store leaves the hot loop's working set entirely.
    //
    // EXACTNESS. `fused[i]` is written only by g410CompilePairSlow, from the same `source` and the
    // same `g410DecodePair(source)` that feed `sources[i]` / `uppers[i]`, in the same statement. The
    // control arm keeps reading the old arrays, so both arms decode identical words.
    // Verify: DC2_G479_VERIFY=1 re-reads the live code word per pair and aborts on a mismatch.
    struct G479FusedPair
    {
        uint64_t source;
        G410CompiledUpper pair;
    };
    static_assert(sizeof(G479FusedPair) == 16u);

    struct G410CompiledCache
    {
        const uint8_t *codeBase = nullptr;
        std::array<G410CompiledUpper, PS2_VU1_CODE_SIZE / 8u> uppers{};
        std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> sources{};
        // G479: the same two words, interleaved. 16-byte aligned so one record is never split
        // across two cache lines.
        alignas(16) std::array<G479FusedPair, PS2_VU1_CODE_SIZE / 8u> fused{};
        // G475: maximal sequential run starting at each slot for the compact
        // VNOP-upper + covered non-control-lower trace (0 = ineligible, capped at 255).
        std::array<uint8_t, PS2_VU1_CODE_SIZE / 8u> g475RunLen{};
        uint64_t hits = 0u;
        uint64_t compiles = 0u;
        uint64_t upperFallbacks = 0u;
        uint64_t verified = 0u;
        uint64_t verifyBad = 0u;
        uint64_t invalidations = 0u;
        uint64_t invalidatedPairs = 0u;
        uint64_t lastReportOps = 0u;
        // G428: how many slots of the WHOLE code store decode to a lower slot that can read the
        // MAC / STATUS delay line. A deferred-classification lever needs STATUS to be provably
        // dead, and STATUS can only be read by executing an FSEQ/FSAND/FSOR pair, so a zero here
        // over the entire (precisely invalidated) code store is a sound static gate.
        uint32_t statusReaderSlots = 0u;
        uint32_t macReaderSlots = 0u;
    };

    thread_local G410CompiledCache s_g410Cache;

    bool g410CompiledRequested()
    {
        static const bool s_on =
            !f50_12_vu_env_flag("DC2_G410_NO_VU_COMPILED");
        return s_on;
    }

    // G479: paired-arm accessor (DC2_G419_AB=leanfetch) or the standalone bring-up flag. Read once
    // per run() — never per pair — so one microprogram never changes loop copies mid-execution.
    bool g479LeanFetchRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "leanfetch") == 0;
        }();
        if (s_paired)
            return g479LeanFetchArm() > 0;
        // PROMOTED default-on (G479). Rollback: DC2_G479_NO_LEAN_FETCH=1 restores the split
        // vuCode + uppers[] fetch exactly.
        static const bool s_on = !f50_12_vu_env_flag("DC2_G479_NO_LEAN_FETCH");
        return s_on;
    }

    // G483: register-carried PC arm. Paired gate DC2_G419_AB=pcreg, standalone DC2_G483_PCREG=1.
    bool g483PcRegRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "pcreg") == 0;
        }();
        if (s_paired)
            return g483PcRegArm() > 0;
        static const bool s_on = f50_12_vu_env_flag("DC2_G483_PCREG");
        return s_on;
    }

    // G484 built it, G485 PROMOTED it: the pair's VF0 re-pin is ONE 16-byte store of (0,0,0,1)
    // instead of four 4-byte scalar stores. Four paired A/Bs read −0.691 / −0.172 / −0.768 /
    // −0.846 ms/f (mean −0.619, sign 4/4) on lean MAP-0. Value-identical bytes — the win is
    // store-to-load forwarding into the next pair's 16-byte `_mm_loadu_ps(vf[FS])`.
    // Paired gate DC2_G419_AB=widevf0, standalone DC2_G484_WIDE_VF0=1,
    // ROLLBACK DC2_G485_NO_WIDE_VF0=1. Read once per run().
    bool g484WideVf0Requested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "widevf0") == 0;
        }();
        if (s_paired)
            return g484WideVf0Arm() > 0;
        // PROMOTED default-on (G485). Rollback restores the four scalar stores exactly.
        static const bool s_on = !f50_12_vu_env_flag("DC2_G485_NO_WIDE_VF0");
        return s_on;
    }

    // G486: WIDE SINGLE-LANE FMAC DESTINATION STORE.
    //
    // G428 measured the dest distribution of the inline FMAC population and specialised the two
    // shapes that dominate it: dest==0xF became one unmasked 16-byte store, and the four
    // single-lane masks became one shuffle plus a 4-byte `_mm_store_ss`. G486 re-prices the second
    // half of that decision under G485's store-forwarding result: a 4-byte store into a 16-byte
    // register that the NEXT pair fetches with `_mm_loadu_ps(st.vf[FS])` cannot be
    // store-to-load forwarded, so the load waits for it to retire to L1. The candidate replaces
    // each `_mm_store_ss` with a whole-lane blend of the destination's own bits and one 16-byte
    // store, which forwards. Value-identical: every lane the blend writes is either the FMAC
    // result lane applyDest() assigns or the destination's own current bits.
    //
    // RESULT: REFUTED. Four paired A/Bs (DC2_G419_AB=widedest, both arms confirmed used) read
    // +0.732 / +0.476 / +0.553 / +0.627 ms/f — mean +0.597, sign 4/4 SLOWER. The blend cannot be
    // built without READING the destination's other three lanes, so the candidate replaces a pure
    // 4-byte store with a load-modify-store: a 16-byte load of `vf[fd]` joins the dependency chain
    // and `old` stays live across the blend. That costs more than the forwarding block it removes.
    // The store-forwarding class needs the wide replacement to be INPUT-FREE (G485's VF0 re-pin
    // stores constants); it does not generalise to every narrow store. The arm, the accessor, the
    // loop copy and the whole `wideDest` plumbing were REMOVED, so the default path is
    // source-identical to G485's. Full record at `g428StoreDest` in vu1_g421_fast_upper.inc.

    bool g483VerifyPcOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G483_VERIFY");
        return s_on;
    }

    bool g479VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G479_VERIFY");
        return s_on;
    }

    bool g410VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G410_VERIFY");
        return s_on;
    }

    bool g410StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G410_STAT");
        return s_on;
    }

    bool g410UpperDiagnosticsCompatible()
    {
        // These probes live inside execUpper itself. The run-loop master diagnostic gate
        // handles the other per-pair probes and disables G410 at the call site.
        static const bool s_ok =
            std::getenv("DC2_G65_CLIP") == nullptr;
        return s_ok;
    }

    uint8_t g410SelectUpperKind(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3fu);
        if (op <= 0x2fu)
            return op;

        if (op < 0x3cu)
            return 0xffu;

        const uint8_t func = S2_FUNC(upper);
        return func <= 0x31u && func != 0x2bu
            ? static_cast<uint8_t>(0x40u + func)
            : 0xffu;
    }

    G410CompiledUpper g410DecodePair(uint64_t source)
    {
        const uint32_t upper = static_cast<uint32_t>(source >> 32u);
        const uint32_t lower = static_cast<uint32_t>(source);
        G410CompiledUpper pair{};
        pair.upperKind = g410SelectUpperKind(upper);
        pair.lowerFs = FS(lower);
        pair.lowerFt = FT(lower);
        if ((upper & (1u << 30u)) != 0u)
            pair.pairFlags |= G410_PAIR_E_BIT;
        if ((upper & (1u << 31u)) != 0u)
            pair.pairFlags |= G410_PAIR_I_BIT;
        if (((lower >> 25u) & 0x7fu) == 0x40u)
            pair.lowerS2 = S2_FUNC(lower);
        // G422: predecode the lower slot's inline-execution kind here, so the hot loop never
        // decodes it. The cache verifier's whole-struct memcmp covers this field too.
        pair.lowerKind = g422DecodeLowerKind(lower);

        const uint8_t upperOp = static_cast<uint8_t>(upper & 0x3fu);
        if (upperOp < 0x3cu)
            pair.upperVfDest = FD(upper);
        else
        {
            const uint8_t upperS2 = S2_FUNC(upper);
            if ((upperS2 >= 0x10u && upperS2 <= 0x17u) ||
                upperS2 == 0x1du)
                pair.upperVfDest = FT(upper);
        }
        // G425: can either slot write VF0 or VI0?
        bool touchesZero = false;
        // Upper slot VF destination. `upperVfDest` above is the G139 hazard field and does NOT
        // cover MOVE/MR32 (special2 0x30/0x31), which also write VF[ft], so recompute here.
        {
            uint32_t upperDst = 0xffffffffu;
            if (upperOp < 0x3cu)
                upperDst = FD(upper);
            else
            {
                const uint8_t s2 = S2_FUNC(upper);
                if ((s2 >= 0x10u && s2 <= 0x17u) || s2 == 0x1du ||
                    s2 == 0x30u || s2 == 0x31u)
                    upperDst = FT(upper);
            }
            if (upperDst == 0u)
                touchesZero = true;
            if (pair.upperKind == 0xffu)
                touchesZero = true; // legacy execUpper decoder: be conservative
        }
        switch (pair.lowerKind)
        {
        case G422_LK_NOP:
        case G422_LK_SQ:
        case G422_LK_ISW:
        case G422_LK_ISWR:
        case G422_LK_B:
        case G422_LK_JR:
        case G422_LK_IBEQ:
        case G422_LK_IBNE:
        case G422_LK_IBLTZ:
        case G422_LK_IBGTZ:
        case G422_LK_IBLEZ:
        case G422_LK_IBGEZ:
            break; // no register-0 destination possible
        case G422_LK_LQ:
        case G422_LK_MFIR:
        case G422_LK_MOVE:
        case G422_LK_MR32:
        case G422_LK_ILW:
        case G422_LK_ILWR:
        case G422_LK_IADDIU:
        case G422_LK_ISUBIU:
        case G422_LK_MTIR:
        case G422_LK_BAL:
        case G422_LK_JALR:
            if (FT(lower) == 0u)
                touchesZero = true;
            break;
        // The post-increment/decrement forms write VF[ft] (LQI/LQD) or only VI (SQI/SQD), and
        // in every case bump the pointer register named by fs/it. Keep both fields in scope.
        case G422_LK_LQI:
        case G422_LK_LQD:
            if (FT(lower) == 0u || FS(lower) == 0u)
                touchesZero = true;
            break;
        case G422_LK_SQI:
        case G422_LK_SQD:
            if (FT(lower) == 0u)
                touchesZero = true;
            break;
        case G422_LK_IADD:
        case G422_LK_ISUB:
        case G422_LK_IADDI:
        case G422_LK_IAND:
        case G422_LK_IOR:
            if (((lower >> 6u) & 0x1fu) == 0u)
                touchesZero = true;
            break;
        default:
            touchesZero = true; // G422_LK_NONE and anything new: legacy execLower
            break;
        }
        if ((pair.pairFlags & G410_PAIR_I_BIT) != 0u)
            touchesZero = true; // LOI: the lower word is an immediate, not an instruction
        if (touchesZero)
            pair.g425Flags |= G425_PF_TOUCHES_ZERO;
        // G426: an LOI pair's lower word is a float immediate, so neither predicate applies
        // (the run loop already gates both on !iBit / the LOI branch).
        if ((pair.pairFlags & G410_PAIR_I_BIT) == 0u)
        {
            if (g426LowerIsScalarPipe(pair.lowerS2))
                pair.g425Flags |= G426_PF_PRESTALL;
            if (pair.upperVfDest != 0xffu &&
                (pair.lowerFs == pair.upperVfDest || pair.lowerFt == pair.upperVfDest))
                pair.g425Flags |= G426_PF_HAZ;
            // G427: does this pair's lower slot consume the MAC/STATUS delay line?
            if (g427LowerReadsFlags(lower))
                pair.g425Flags |= G427_PF_FLAGREAD;
            if (g428LowerReadsStatus(lower))
                pair.g425Flags |= G428_PF_STATUSREAD;
        }
        return pair;
    }

    void g410CompilePairSlow(
        size_t index, const uint8_t *codeBase, bool countStats)
    {
        uint64_t source = 0u;
        std::memcpy(&source, codeBase + index * sizeof(source), sizeof(source));
        const G410CompiledUpper pair = g410DecodePair(source);
        // G428: keep the whole-code-store reader tallies exact across precise invalidation.
        const uint8_t oldFlags = s_g410Cache.uppers[index].g425Flags;
        if ((oldFlags & G428_PF_STATUSREAD) != 0u)
            --s_g410Cache.statusReaderSlots;
        if ((oldFlags & G427_PF_FLAGREAD) != 0u)
            --s_g410Cache.macReaderSlots;
        if ((pair.g425Flags & G428_PF_STATUSREAD) != 0u)
            ++s_g410Cache.statusReaderSlots;
        if ((pair.g425Flags & G427_PF_FLAGREAD) != 0u)
            ++s_g410Cache.macReaderSlots;
        s_g410Cache.sources[index] = source;
        s_g410Cache.uppers[index] = pair;
        // G479: the fused record is written from the SAME two values in the same statement, so it
        // can never carry a different decode than the split arrays.
        s_g410Cache.fused[index].source = source;
        s_g410Cache.fused[index].pair = pair;
        if (countStats)
        {
            ++s_g410Cache.compiles;
            if (pair.upperKind == 0xffu)
                ++s_g410Cache.upperFallbacks;
        }
    }

    bool g475TracePairEligible(const G410CompiledUpper &pair)
    {
        const bool lowerControl = pair.lowerKind >= G422_LK_B &&
                                  pair.lowerKind <= G422_LK_IBGEZ;
        return pair.upperKind == 0x6fu && pair.pairFlags == 0u &&
               pair.lowerKind != G422_LK_NONE && !lowerControl;
    }

    void g475RebuildRunLengths()
    {
        uint32_t run = 0u;
        for (size_t i = s_g410Cache.uppers.size(); i-- > 0u;)
        {
            if (g475TracePairEligible(s_g410Cache.uppers[i]))
            {
                if (run < 255u)
                    ++run;
                s_g410Cache.g475RunLen[i] = static_cast<uint8_t>(run);
            }
            else
            {
                run = 0u;
                s_g410Cache.g475RunLen[i] = 0u;
            }
        }
    }

    G410CompiledUpper *g410PreparePairs(const uint8_t *codeBase, bool countStats)
    {
        if (s_g410Cache.codeBase != codeBase)
        {
            s_g410Cache.codeBase = codeBase;
            s_g410Cache.statusReaderSlots = 0u;
            s_g410Cache.macReaderSlots = 0u;
            for (size_t index = 0u; index < s_g410Cache.uppers.size(); ++index)
                s_g410Cache.uppers[index].g425Flags = 0u;
            for (size_t index = 0u; index < s_g410Cache.uppers.size(); ++index)
                g410CompilePairSlow(index, codeBase, countStats);
            g475RebuildRunLengths();
        }
        return s_g410Cache.uppers.data();
    }

    [[noreturn]] void g410MissedInvalidation(
        uint32_t pc, uint64_t cachedSource, uint64_t liveSource)
    {
        ++s_g410Cache.verifyBad;
        std::fprintf(stderr,
                     "[G410:verify] missed code invalidation pc=0x%x "
                     "cached=%016llx live=%016llx\n",
                     pc,
                     static_cast<unsigned long long>(cachedSource),
                     static_cast<unsigned long long>(liveSource));
        std::abort();
    }

    [[noreturn]] void g410MismatchedPredecode(
        uint32_t pc, const G410CompiledUpper &cached, const G410CompiledUpper &expected)
    {
        ++s_g410Cache.verifyBad;
        uint64_t cachedBits = 0u;
        uint64_t expectedBits = 0u;
        std::memcpy(&cachedBits, &cached, sizeof(cachedBits));
        std::memcpy(&expectedBits, &expected, sizeof(expectedBits));
        std::fprintf(stderr,
                     "[G410:verify] mismatched predecode pc=0x%x "
                     "cached=%016llx expected=%016llx\n",
                     pc,
                     static_cast<unsigned long long>(cachedBits),
                     static_cast<unsigned long long>(expectedBits));
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g410VerifyCachedPair(
        uint32_t pc, uint64_t liveSource, const G410CompiledUpper &pair)
    {
        const size_t index = pc >> 3u;
        if (s_g410Cache.sources[index] != liveSource)
            g410MissedInvalidation(pc, s_g410Cache.sources[index], liveSource);
        const G410CompiledUpper expected = g410DecodePair(liveSource);
        if (std::memcmp(&pair, &expected, sizeof(pair)) != 0)
            g410MismatchedPredecode(pc, pair, expected);
    }

    DC2_G330_FORCEINLINE G410CompiledUpper &g410FetchPair(
        G410CompiledUpper *uppers, const uint8_t *codeBase, uint32_t pc,
        bool countStats, bool verifyCache, uint32_t &lower, uint32_t &upper)
    {
        const size_t index = pc >> 3u;
        G410CompiledUpper &pair = uppers[index];
        uint64_t liveSource = 0u;
        std::memcpy(&liveSource, codeBase + pc, sizeof(liveSource));
        if (countStats)
            ++s_g410Cache.hits;
        if (verifyCache)
            g410VerifyCachedPair(pc, liveSource, pair);
        lower = static_cast<uint32_t>(liveSource);
        upper = static_cast<uint32_t>(liveSource >> 32u);
        return pair;
    }

    bool g410UpperStatesEqual(const VU1State &a, const VU1State &b)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 &&
               a.mac == b.mac && a.status == b.status && a.clip == b.clip;
    }

    // G479: exactness counter for the fused record (DC2_G479_VERIFY=1 arm only).
    std::atomic<uint64_t> s_g479Verified{0u};
    void g479NoteVerified()
    {
        const uint64_t n = s_g479Verified.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 20000000ull) == 0ull)
            std::fprintf(stderr, "[G479:verify] pairs=%llu bad=0\n",
                         (unsigned long long)n);
    }

    void g410ReportMaybe()
    {
        if (!g410StatOn())
            return;
        const uint64_t ops = s_g410Cache.hits + s_g410Cache.compiles;
        if (ops - s_g410Cache.lastReportOps < 5000000u)
            return;
        s_g410Cache.lastReportOps = ops;
        const double hitPct = ops != 0u
            ? 100.0 * static_cast<double>(s_g410Cache.hits) / static_cast<double>(ops)
            : 0.0;
        std::fprintf(stderr,
                     "[G410:jit] ops=%llu hit=%llu compile=%llu hitPct=%.3f "
                     "upperFallback=%llu invalidations=%llu invalidatedPairs=%llu "
                     "verified=%llu bad=%llu statusReaderSlots=%u macReaderSlots=%u\n",
                     static_cast<unsigned long long>(ops),
                     static_cast<unsigned long long>(s_g410Cache.hits),
                     static_cast<unsigned long long>(s_g410Cache.compiles),
                     hitPct,
                     static_cast<unsigned long long>(s_g410Cache.upperFallbacks),
                     static_cast<unsigned long long>(s_g410Cache.invalidations),
                     static_cast<unsigned long long>(s_g410Cache.invalidatedPairs),
                     static_cast<unsigned long long>(s_g410Cache.verified),
                     static_cast<unsigned long long>(s_g410Cache.verifyBad),
                     s_g410Cache.statusReaderSlots,
                     s_g410Cache.macReaderSlots);
    }
} // namespace

// Called from the only two code-write sites: VIF1's live MPG store and MTVU's ordered worker
// replay. Both calls execute on the same thread that can consume the corresponding code buffer,
// so the thread-local compiled cache needs no cross-thread synchronization.
void dc2_g410_vu_code_written(const uint8_t *codeBase, uint32_t offset, uint32_t sizeBytes)
{
    if (codeBase == nullptr || sizeBytes == 0u || s_g410Cache.codeBase != codeBase)
        return;

    const uint64_t end64 = static_cast<uint64_t>(offset) + sizeBytes;
    if (offset >= PS2_VU1_CODE_SIZE)
        return;
    const uint32_t end = static_cast<uint32_t>(
        end64 > PS2_VU1_CODE_SIZE ? PS2_VU1_CODE_SIZE : end64);
    const size_t first = static_cast<size_t>(offset >> 3u);
    const size_t last = static_cast<size_t>((end - 1u) >> 3u);
    uint64_t invalidated = 0u;
    for (size_t index = first; index <= last; ++index)
    {
        uint64_t liveSource = 0u;
        std::memcpy(
            &liveSource, codeBase + index * sizeof(liveSource), sizeof(liveSource));
        if (s_g410Cache.sources[index] != liveSource)
        {
            g410CompilePairSlow(index, codeBase, g410StatOn());
            ++invalidated;
        }
    }
    if (invalidated != 0u)
        g475RebuildRunLengths();
    if (g410StatOn())
    {
        ++s_g410Cache.invalidations;
        s_g410Cache.invalidatedPairs += invalidated;
    }
}
