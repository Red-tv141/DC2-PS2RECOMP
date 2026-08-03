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

// G487: within-process paired A/B arm for the compiled flag-consumer lowers. Unlike every other
// arm in this file it is NOT a loop-copy selector — it changes what this cache DECODES, so a flip
// must re-decode the cache (see g410PreparePairs).
extern int g487FlagLowerArm();

// G488: same shape, for the three cross-lane/flag-only UPPER encodings.
extern int g488FastOpArm();

// G489: same shape, for the cached "this lower can write pc" bit that lets the loop tail skip the
// `postExecPc` read-back. Also a cache-decode arm, so a flip re-decodes (see g410PreparePairs).
extern int g489PcSkipArm();

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

    // G489: the lower slot of this pair can WRITE `m_state.pc`, i.e. it is a branch or a jump.
    //
    // The pair loop's tail asks "did the lower slot branch?" by reading `m_state.pc` back and
    // comparing it with `currentPc` (the `postExecPc` round trip). That read is a reload of
    // `&m_state` out of its spill slot plus a dependent 4-byte load, on EVERY pair, in the loop
    // TAIL — the /MAP + PC-sample audit of the G489 binary put 22.6% of all VU1 samples in that
    // 512-byte tail region and named `mov rax,[rbp+0x160]` / `mov edx,[rax+0x25c]` as two of its
    // hottest instructions. But the answer is a pure function of the lower ENCODING, which this
    // sidecar already caches and precisely invalidates.
    //
    // EXACTNESS. `m_state.pc` / `st.pc` is assigned by exactly two executors, and in both the
    // complete set of assigning cases is the branch/jump group:
    //   * VU1Interpreter::execLower  - `case 0x20/0x21/0x24/0x25/0x28/0x29/0x2C/0x2D/0x2E/0x2F`
    //     (B / BAL / JR / JALR / IBEQ / IBNE / IBLTZ / IBGTZ / IBLEZ / IBGEZ), the only ten
    //     `m_state.pc =` statements in the whole file;
    //   * g422FastLower / the G481 inline bodies - `G422_LK_B .. G422_LK_IBGEZ`, which
    //     g422DecodeLowerKind produces from exactly those same ten `opHi` values.
    // execUpper never writes pc. So a pair whose lower `opHi` is outside that set provably leaves
    // `m_state.pc == currentPc`, whichever executor ran it, covered or not.
    //
    // Conservative on the I-bit: an LOI pair's lower word is a float immediate that is never
    // executed as an instruction, so it cannot write pc either — but its bit pattern can land in
    // the branch range, and the bit is set rather than reasoned away.
    constexpr uint8_t G489_PF_CANBRANCH = 0x20u;

    inline bool g489LowerWritesPc(uint32_t lower)
    {
        switch (static_cast<uint8_t>((lower >> 25u) & 0x7fu))
        {
        case 0x20u: // B
        case 0x21u: // BAL
        case 0x24u: // JR
        case 0x25u: // JALR
        case 0x28u: // IBEQ
        case 0x29u: // IBNE
        case 0x2cu: // IBLTZ
        case 0x2du: // IBGTZ
        case 0x2eu: // IBLEZ
        case 0x2fu: // IBGEZ
            return true;
        default:
            return false;
        }
    }

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
        // G487: the flag-consumer arm this cache's `lowerKind` bytes were decoded under. Every
        // decode site (the bulk prepare, the MPG invalidation path and the cache verifier) reads
        // it from HERE rather than from the arm accessor, so the cache can never hold a mixture:
        // the only place the arm is sampled is g410PreparePairs, once per run(), and sampling it
        // is what triggers the rebuild.
        bool decodeFlagLower = false;
        // G488: the compiled cross-lane/flag-only UPPER arm this cache's `upperKind` bytes were
        // decoded under. Same contract as decodeFlagLower — sampled once, in g410PreparePairs.
        bool decodeFastOp = false;
        // G489: the pc-skip arm this cache's `G489_PF_CANBRANCH` bits were decoded under. Same
        // contract as the two above — sampled once, in g410PreparePairs, and a flip rebuilds.
        bool decodePcSkip = false;
        bool decodeArmValid = false;
        std::array<G410CompiledUpper, PS2_VU1_CODE_SIZE / 8u> uppers{};
        std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> sources{};
        // G479: the same two words, interleaved. 16-byte aligned so one record is never split
        // across two cache lines.
        alignas(16) std::array<G479FusedPair, PS2_VU1_CODE_SIZE / 8u> fused{};
        // G475: maximal sequential run starting at each slot for the compact
        // VNOP-upper + covered non-control-lower trace (0 = ineligible, capped at 255).
        std::array<uint8_t, PS2_VU1_CODE_SIZE / 8u> g475RunLen{};
        // G490: the same shape for the WIDENED block run — every covered upper class, not only
        // VNOP. Maintained by the same two call sites as g475RunLen, so precise invalidation
        // covers it identically.
        std::array<uint8_t, PS2_VU1_CODE_SIZE / 8u> g490RunLen{};
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

    // G487: COMPILED FLAG-CONSUMER LOWERS.
    //
    // PREMISE, priced off THIS binary's linker /MAP before anything was built (G476/G480/G483).
    // `execLower`'s call boundary is the largest in the pair loop by a wide margin — its prologue
    // is 8 GPR pushes, `sub rsp,0x728`, TEN `movaps` spills of xmm6-xmm15 and a /GS cookie, all
    // mirrored on return, before the two-level opcode switch is even reached. `[G422:cover]` says
    // 4.61% of pairs (~75 k/frame) cross it, and `[G486:uncov]` says the only coverable mass in
    // that population is the four flag consumers (2.54% of pairs, ~41 k/frame); the rest is DIV
    // (Q-latency staging), XGKICK and the EFU/WAIT scalar pipe, none of which can move.
    //
    // WHY THIS SHAPE. `g421FastUpper` and `g422FastLower` are both FULLY INLINED into
    // `VU1Interpreter::run()` in this binary (confirmed from the /MAP: run() contains no call to
    // either), so a kind that becomes "covered" stops making a call at all rather than trading one
    // call for a cheaper one. That also means the change grows the loop's inlined text, which is
    // the resource G481's inline-budget cliff binds on — hence exactly four bodies, each three or
    // four instructions with no memory access and no branch.
    //
    // WHY THE ARM IS A CACHE PROPERTY. `g422DecodeLowerKind` runs at G410 cache-build time, so
    // flipping the arm has to re-decode the cache. The alternative — selecting in the loop with
    // `kind != NONE && (ARM || kind < FIRST_FLAG)` — would put an extra compare on the CONTROL arm
    // on 59.9% of pairs and bias the gate in the candidate's favour, the one direction that must
    // never be accepted (G486 §4). The rebuild is ~2,048 slots once per G438 hold block.
    //
    // RESULT: PROMOTED default-on. Four paired A/Bs (DC2_G419_AB=flaglower, both arms confirmed
    // selected by [G487:armed] on every run, 3,000 frames each) read −0.452 / −1.047 / −0.489 /
    // −0.214 ms/f — mean **−0.55, sign 4/4 FASTER**, blocked mean −0.60 with 19 of 20 windows
    // favouring. Back-solving: −0.55 ms/f of frame at this phase's VU1 sensitivity is ~0.42 ms of
    // worker time over ~41 k removed calls = **~10 ns per call**, i.e. ~34 cycles — exactly the
    // prologue/epilogue the /MAP predicted. The prediction and the measurement agree, which is why
    // this is a call-boundary result and not a code-shape accident.
    // `[G422:cover]` moved 92.29% → **96.52%** covered and `[G486:uncov]` 4.61% → **2.08%** of
    // cycles, with the four flag opcodes gone from the histogram and only DIV (80.8% of the
    // residue), XGKICK, WAITQ/WAITP, XTOP, MFP, ERLENG and RSQRT left — all structurally
    // non-coverable. The compiled LOWER slot is closed after this phase.
    //
    // Default:  ON.  Rollback DC2_G487_NO_FLAG_LOWER=1 (restores the execLower path exactly).
    //           Paired gate DC2_G419_AB=flaglower, proof DC2_G487_STAT=1,
    //           oracle DC2_G422_VERIFY=1 (already generic over kinds; 350 M+ pairs, bad=0).
    bool g487FlagLowerRequested()
    {
        if (!g487FlagLowerSupported())
            return false;
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "flaglower") == 0;
        }();
        if (s_paired)
            return g487FlagLowerArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G487_NO_FLAG_LOWER");
        return s_on;
    }

    // G488: COMPILED CROSS-LANE / FLAG-ONLY UPPERS (OPMSUB, OPMULA, CLIPw).
    //
    // PREMISE. G487 closed the compiled LOWER slot and PRICED the mechanism that closed it: an
    // interpreter call boundary in this loop is worth ~10 ns (~34 cycles), measured over 41 k
    // removed `execLower` calls and predicted independently by the /MAP disassembly of its
    // prologue. `execUpper` is the same shape one size down — 8 GPR pushes, `sub rsp,0xa0`, four
    // `movaps` of xmm6-xmm9 and a /GS cookie, mirrored on return — and `[G421:cover]` said 2.16%
    // of non-VNOP uppers (~1.08% of pairs, ~17.6 k/frame) still crossed it. That population is
    // EXACTLY the three `cls == 0` entries of G421's descriptor table that this route executes:
    // OPMSUB (0x2E), CLIPw (0x5F) and OPMULA (0x6E). G421 skipped them because they are
    // cross-lane or flag-only, not because they are expensive: OPMSUB/OPMULA are one outer-product
    // permutation of an FMAC that G421 already runs, and CLIPw is six ordered compares.
    //
    // WHY THE ARM IS A CACHE PROPERTY (G487 §4, and the reason it must not be an in-loop test).
    // The fallback decision is `desc[kind].cls == 0`, and `kind` is decoded once at cache-build
    // time into `G410CompiledUpper::upperKind`. Selecting the arm inside the loop would put an
    // extra compare on the CONTROL arm on ~49% of pairs and bias the gate toward the candidate —
    // the one direction that must never be accepted. Instead the CONTROL arm decodes these three
    // encodings to SHADOW kinds (0x72/0x73/0x74) whose descriptor entries are zero, so the loop
    // body is byte-identical in both arms and the only difference is one byte in the pair cache.
    // A flip forces the full 2,048-slot re-decode before a single pair of that run() executes.
    //
    // WHAT THIS CANNOT MEASURE. Both arms carry the new inline bodies in the loop's TEXT, so the
    // paired gate prices the removed call but NOT the added footprint — the resource G481's inline
    // budget binds on. That is checked separately, from the linker /MAP (run() size and whether
    // g421FastUpper is still inlined) plus the default-path level.
    //
    // Default:  ON.  Rollback DC2_G488_NO_FAST_OP=1 (restores the execUpper path exactly).
    //           Paired gate DC2_G419_AB=fastop, proof DC2_G488_STAT=1,
    //           oracle DC2_G421_VERIFY=1 (already generic over kinds).
    bool g488FastOpRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "fastop") == 0;
        }();
        if (s_paired)
            return g488FastOpArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G488_NO_FAST_OP");
        return s_on;
    }

    // G489 - SKIP THE `postExecPc` READ-BACK ON PAIRS WHOSE LOWER CANNOT BRANCH.
    //
    // Same arm contract as G487/G488: the decision is a property of the pair CACHE, sampled once
    // per run(), never a test inside the loop. But it needs no shadow encoding, because the arm is
    // the VALUE of one cache bit rather than a different decode of the opcode:
    //   * CANDIDATE - `G489_PF_CANBRANCH` is set only for the ten branch/jump `opHi` values.
    //   * CONTROL   - the bit is set on EVERY pair, so `g489CanBranch` is always true, the tail
    //                 reads `m_state.pc` and compares it exactly as it did before this phase, and
    //                 the loop body is byte-identical in both arms.
    // The control arm therefore carries one always-true, perfectly-predicted byte test that the
    // pre-G489 binary did not have. That biases the gate TOWARD the candidate, so it is quoted as
    // a floor, and the phase also checks the default-path level against the /MAP relink.
    //
    // Default:  ON.  Rollback DC2_G489_NO_PCSKIP=1 (sets the bit on every pair = old behaviour).
    //           Paired gate DC2_G419_AB=pcskip, proof DC2_G489_STAT=1,
    //           oracle DC2_G489_VERIFY=1 (asserts m_state.pc == currentPc whenever the bit is 0).
    bool g489PcSkipRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "pcskip") == 0;
        }();
        if (s_paired)
            return g489PcSkipArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G489_NO_PCSKIP");
        return s_on;
    }

    bool g489VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G489_VERIFY");
        return s_on;
    }

    // G489 oracle: the predicate says this pair's lower slot cannot write pc, so `m_state.pc` must
    // still be `currentPc` after both slots executed. Aborting is deliberate — a false negative
    // here would silently drop a taken branch, which is a correctness class no route matrix could
    // be trusted to catch. Cold and out of line; only reachable with DC2_G489_VERIFY=1.
    void g489VerifyFail(uint32_t currentPc, uint32_t postPc, uint32_t lower)
    {
        std::fprintf(stderr,
                     "[G489:verify-BAD] pc=0x%x post=0x%x lower=%08x opHi=0x%02x\n",
                     currentPc, postPc, lower,
                     static_cast<unsigned>((lower >> 25u) & 0x7fu));
        std::fflush(stderr);
        std::abort();
    }

    // G489 counters for DC2_G489_VERIFY / DC2_G489_STAT. File-scope statics, NOT members of any
    // thread_local block — G486: a default-off diagnostic must not be able to move the default
    // path's TLS layout.
    std::atomic<uint64_t> g_g489Checked{0ull};
    std::atomic<uint64_t> g_g489CanBranch{0ull};

    // G489 proof-of-selection (G457). Out of line and entirely behind the flag check, so `run()`
    // carries one call and nothing else. Counts the slots the candidate arm actually spares.
    void g489ReportArmed()
    {
        static const bool s_on = (std::getenv("DC2_G489_STAT") != nullptr) ||
                                 (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_on)
            return;
        static std::atomic<uint32_t> s_seen{0u};
        const uint32_t bit = s_g410Cache.decodePcSkip ? 2u : 1u;
        if ((s_seen.fetch_or(bit, std::memory_order_relaxed) & bit) != 0u)
            return;
        size_t canBranch = 0u;
        for (const auto &p : s_g410Cache.uppers)
            if ((p.g425Flags & G489_PF_CANBRANCH) != 0u)
                ++canBranch;
        std::fprintf(stderr, "[G489:armed] pcskip=%d slots=%zu canBranch=%zu (%.2f%%)\n",
                     s_g410Cache.decodePcSkip ? 1 : 0, s_g410Cache.uppers.size(), canBranch,
                     100.0 * static_cast<double>(canBranch) /
                         static_cast<double>(s_g410Cache.uppers.size()));
    }

    void g489NoteVerified(bool canBranch)
    {
        const uint64_t n = g_g489Checked.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if (canBranch)
            g_g489CanBranch.fetch_add(1ull, std::memory_order_relaxed);
        if ((n % 50000000ull) == 0ull)
            std::fprintf(stderr, "[G489:verify] pairs=%llu bad=0 canBranch=%llu (%.2f%%)\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(
                             g_g489CanBranch.load(std::memory_order_relaxed)),
                         100.0 * static_cast<double>(
                             g_g489CanBranch.load(std::memory_order_relaxed)) /
                             static_cast<double>(n));
    }

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

    // G488: the CONTROL arm's kinds for OPMSUB / CLIPw / OPMULA.
    //
    // `g410SelectUpperKind` never produces 0x72-0x7f (op <= 0x2f gives 0x00-0x2f, and Special2
    // gives 0x40 + func with func <= 0x31, i.e. at most 0x71), so these three values are free.
    // They index the ZERO-initialised tail of G421DescTable, so `cls == 0` and `g421FastUpper`
    // falls straight through to execUpper — the pre-G488 behaviour, byte for byte, with no test
    // anywhere in the pair loop. `[G421:cover]`'s covered predicate is already `k < 0x72u`, so the
    // census reports the arm correctly for free, and the histogram keeps the two arms distinct
    // (72/73/74 vs 2e/5f/6e) because it folds with `& 0x7f`.
    constexpr uint8_t G488_UK_SHADOW_OPMSUB = 0x72u;
    constexpr uint8_t G488_UK_SHADOW_CLIPW = 0x73u;
    constexpr uint8_t G488_UK_SHADOW_OPMULA = 0x74u;

    uint8_t g410SelectUpperKind(uint32_t upper, bool fastOp)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3fu);
        if (op <= 0x2fu)
            return (!fastOp && op == 0x2eu) ? G488_UK_SHADOW_OPMSUB : op;

        if (op < 0x3cu)
            return 0xffu;

        const uint8_t func = S2_FUNC(upper);
        if (func > 0x31u || func == 0x2bu)
            return 0xffu;
        if (!fastOp)
        {
            if (func == 0x1fu)
                return G488_UK_SHADOW_CLIPW;
            if (func == 0x2eu)
                return G488_UK_SHADOW_OPMULA;
        }
        return static_cast<uint8_t>(0x40u + func);
    }

    G410CompiledUpper g410DecodePair(uint64_t source, bool flagLower, bool fastOp, bool pcSkip)
    {
        const uint32_t upper = static_cast<uint32_t>(source >> 32u);
        const uint32_t lower = static_cast<uint32_t>(source);
        G410CompiledUpper pair{};
        pair.upperKind = g410SelectUpperKind(upper, fastOp);
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
        pair.lowerKind = g422DecodeLowerKind(lower, flagLower);

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
            // G488: both arms land here with the SAME answer. `upperDst` is derived from the raw
            // encoding, not from the kind, and the three shadow kinds are not 0xff — so a pair's
            // g425Flags byte is identical whether the cache was decoded with the arm on or off,
            // and the arms differ in exactly one field of the sidecar.
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
        // G487: the four flag consumers deliberately fall through to `default` above, i.e. they
        // KEEP the conservative G425 re-pin exactly as they had it while they were G422_LK_NONE.
        // They provably cannot write VF0 or VI0 (FCAND/FCOR write vi[1]; FMAND/FCGET are guarded
        // by `it != 0`), so classifying them as zero-safe would ALSO delete five stores per pair —
        // a second mechanism inside one A/B arm, which the phase policy forbids. Leave it.
        if ((pair.pairFlags & G410_PAIR_I_BIT) != 0u)
            touchesZero = true; // LOI: the lower word is an immediate, not an instruction
        if (touchesZero)
            pair.g425Flags |= G425_PF_TOUCHES_ZERO;
        // G489: the candidate arm sets this only for the ten branch/jump encodings (plus the LOI
        // pair, conservatively); the control arm sets it on every pair, which is exactly the
        // pre-G489 behaviour of the loop tail with the loop text unchanged.
        if (!pcSkip || g489LowerWritesPc(lower) ||
            (pair.pairFlags & G410_PAIR_I_BIT) != 0u)
            pair.g425Flags |= G489_PF_CANBRANCH;
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
        // G487: decode under the arm THIS cache was built with, never under a freshly sampled one,
        // so an MPG write that lands mid-block cannot mix two decodes into one cache.
        const G410CompiledUpper pair =
            g410DecodePair(source, s_g410Cache.decodeFlagLower, s_g410Cache.decodeFastOp,
                           s_g410Cache.decodePcSkip);
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
        // G487: G475's contract is that every covered non-control kind is branch-free AND
        // flag-reader-free — the trace replays a run of pairs without advancing the MAC/STATUS
        // delay line per pair. The four flag consumers break that half of the contract, so they
        // are excluded here and in the census's `g475Safe` (same edit, as G486 §4 required).
        const bool lowerFlag = pair.lowerKind >= G422_LK_FIRST_FLAG;
        return pair.upperKind == 0x6fu && pair.pairFlags == 0u &&
               pair.lowerKind != G422_LK_NONE && !lowerControl && !lowerFlag;
    }

    // G490: defined in vu1_g490_block_run.inc, which must be included after vu1_g421_fast_upper.inc
    // (its eligibility test reads `g421DescTableConst`). Same anonymous namespace, same TU — this
    // declaration only lets the two invalidation sites below call it in source order.
    void g490RebuildRunLengths();

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
        // G487/G488: the ONLY place either decode arm is sampled. A flip re-decodes the whole
        // 2,048-slot cache before a single pair of this run() executes, which is what makes the
        // arm a property of the cache instead of a per-pair test in the loop.
        const bool flagLower = g487FlagLowerRequested();
        const bool fastOp = g488FastOpRequested();
        const bool pcSkip = g489PcSkipRequested();
        if (!s_g410Cache.decodeArmValid || s_g410Cache.decodeFlagLower != flagLower ||
            s_g410Cache.decodeFastOp != fastOp || s_g410Cache.decodePcSkip != pcSkip)
        {
            s_g410Cache.decodeFlagLower = flagLower;
            s_g410Cache.decodeFastOp = fastOp;
            s_g410Cache.decodePcSkip = pcSkip;
            s_g410Cache.decodeArmValid = true;
            s_g410Cache.codeBase = nullptr; // force the full rebuild below
        }
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
            g490RebuildRunLengths();
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
        const G410CompiledUpper expected =
            g410DecodePair(liveSource, s_g410Cache.decodeFlagLower, s_g410Cache.decodeFastOp,
                           s_g410Cache.decodePcSkip);
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
    {
        g475RebuildRunLengths();
        g490RebuildRunLengths();
    }
    if (g410StatOn())
    {
        ++s_g410Cache.invalidations;
        s_g410Cache.invalidatedPairs += invalidated;
    }
}
