// ============================================================================================
// G612 — NATIVE VU1 *REGION* COMPILATION (whole-microprogram control flow, x86-64)
// ============================================================================================
//
// WHAT THIS IS, AND WHY IT IS NOT G610/G611 WITH MORE ADMISSIONS.
//
// G610 built an x86-64 translator for one G490 block: a straight run of pairs with no control
// flow. G611 took it to 99.70% of block-eligible pairs (`refused = 0`, `fbSeg = 0`). What is left
// is not an admissions problem at all — it is a SHAPE problem, and the census names it twice:
//
//   * `[G490:block] cover = 77.21%`  ⇒ **22.79% of every executed pair never reaches a block**,
//     and pays the full generic per-pair envelope. The largest single item in it is the control
//     transfer itself and its delay slot.
//   * `[G610:why] fbTail = 993,036,203` against `nativeBlocks = 1,883,242,496` ⇒ **52.7% of every
//     native block entry hands its branch/flag TAIL straight back to the pair loop**, because a
//     loop body always ends in its own back-edge. Those are also the LONG blocks.
//
// So the interpreter is not being asked to execute rare opcodes any more. It is being asked to
// execute the LOOP. Every iteration of a VU1 loop costs: one block round trip (G502 priced it at
// 1.89 ns), one generic pair envelope for the branch, one more for the delay slot, and a second
// round trip to re-enter. G607 measured the five dominant loops at 51.65% of all executed pairs
// with a mean of 10.7 iterations per loop ENTRY — i.e. the port pays that tax ten times where a
// native backend would pay it once.
//
// A REGION is the fix. It is a set of basic blocks discovered by walking the microprogram's own
// static control-flow graph out of one entry slot, emitted as ONE machine-code body in which:
//
//   * every basic block is G610's straight-line emission, verbatim (this file calls
//     `g610EmitUpper` / `g610EmitLower` — it does not re-implement a single arithmetic op, so
//     G611's MEASURED operand order and every G610 exactness argument carry over unchanged);
//   * every branch is a NATIVE compare-and-jump with the VU delay slot emitted between the
//     compare and the jump, which is exactly where the hardware puts it;
//   * every back-edge is a native `jmp`, so a whole VU1 loop runs to completion inside one call;
//   * anything the closed op set does not cover — XGKICK, a flag consumer, DIV, an E-bit pair, an
//     indirect jump's destination — is a REGION EXIT: the body writes `st.pc`, returns the number
//     of pairs it consumed, and the shipped interpreter executes that one pair. The slot AFTER an
//     exit is itself made a region entry wherever it can be, so control comes straight back into
//     native code — which is what turns "the interpreter owns the tail" into "the interpreter
//     owns one pair".
//
// ⛔ THE SEVEN PRECONDITIONS. Each is CHECKED at translation or at the entry, never assumed.
//   1. Everything G490 already requires of every emitted pair (E/I bit, G239 prestall, G139 pair
//      hazard, flag reader, a lower that can write pc) — re-read from `g490RunLen`, the same
//      precisely-invalidated sidecar the pair loop dispatches on. A pair with `g490RunLen == 0`
//      is never emitted as a plain pair.
//   2. A branch pair is emitted only where `g500Tail[i]` says the sidecar ALREADY admits it as a
//      G500 control-transfer tail, and only with `g500Tail[i] == 2` — i.e. its delay slot is
//      itself a plain eligible pair. `g500Tail[i] == 1` (delay slot not eligible) is a region
//      exit, because a region must never leave a pending branch target behind.
//   3. `qDelay == 0 && pDelay == 0` at the ENTRY. A region's pair count is dynamic, so the static
//      Q/P segmentation G506/G610/G611 rely on cannot be computed for it. Nothing a region emits
//      can ARM either counter (DIV/EFU/WAITQ/WAITP all decode outside the emitted set and outside
//      `g490BlockPairEligible`), so both counters are provably still 0 at every exit and the
//      generic loop's per-pair Q/P advance is the identity throughout. This is checked at the
//      hook, once per entry, and counted (`[G612:why] skipQp`) so "provably" stays a measurement.
//   4. `dataSize` and `codeSize` are the values the body was emitted for. Both are baked into
//      address masks and PC constants.
//   5. The G430 ring cursor is loaded once at entry and stored once at every exit — G504's
//      exactness argument, sound here for G610's reason: the emitted body CALLS NOTHING.
//   6. The budget. `maxCycles` is an architectural suspension edge, so each basic block tests
//      that it fits in what is left BEFORE executing any of it and exits cleanly if it does not.
//      The caller additionally refuses the entry when the entry block alone does not fit, which
//      is what guarantees a region always consumes at least one pair and the pair loop can never
//      spin on a zero-progress entry.
//   7. `g610EmitUpper`/`g610EmitLower` are AUTHORITATIVE over `g612PlainEmittable`. The predicate
//      exists to plan the CFG; if the emitter ever refuses a pair the predicate accepted, the
//      whole region is discarded (`[G612:why] mismatch`). A drift between the two is therefore a
//      lost region, never a wrong one.
//
// REGISTERS. Unlike a G610 block, a region runs a whole loop, so it can afford a prologue: the
// entry thunk saves RBP/RSI/RDI and gains three live values a block did not have.
//
//   RCX = VU1State*      RDX = vuData         R9  = G430HistBlock*    R10 = G430 ring cursor
//   R11 = constant pool  RAX = scratch
//   R8  = stampBase + PAIRS CONSUMED SO FAR   ⭐ advanced by one `add r8, n` per basic block
//   RBP = the ORIGINAL stampBase              (so `consumed = r8 - rbp` at any exit)
//   RSI = stampBase + budget                  (so the budget test is one `lea` and one `cmp`)
//   RDI = the branch condition / the indirect target, carried ACROSS the delay slot
//
// ⭐ Keeping R8 as `stampBase + consumed` rather than adding a second counter is what lets this
// file call `g610EmitUpper` COMPLETELY UNMODIFIED: its history stamp is `lea rax, [r8 + j]` with
// `j` the pair's index inside its basic block, which is exactly the right absolute stamp once R8
// carries the running total. Not one byte of G610's emitter changes, so not one byte of G611's
// measured operand order, dest-mask blending or G546 canonicalisation is re-derived here.
//
// Verify: DC2_G612_VERIFY=1   Stats: DC2_G612_STAT=1   Gate: DC2_G419_AB=vureg
// Rollback: DC2_G612_NO_REGION=1
// ============================================================================================

#ifndef DC2_G612_REGION_BACKEND
#define DC2_G612_REGION_BACKEND 1
#endif
#ifndef DC2_G612_DEFAULT_ON
#define DC2_G612_DEFAULT_ON 1
#endif

#if DC2_G612_REGION_BACKEND && DC2_G610_JIT_BACKEND

// Declared at GLOBAL scope, above this file's anonymous namespace, or it binds to an
// internal-linkage symbol and fails to link (appendix-dc2-project.md §3).
extern int g612RegionArm();

namespace
{
    // -------------------------------------------------------------------------------------------
    // §1 CONSTANTS, CAPS AND THE EXTRA REGISTER NAMES
    // -------------------------------------------------------------------------------------------
    constexpr int32_t G612_OFF_PC = 604; // VU1State::pc
    static_assert(offsetof(VU1State, pc) == (size_t)G612_OFF_PC, "G612: pc offset");

    // The three callee-saved registers a region uses. G610 needed none of them (its bodies have no
    // prologue at all); a region amortises three pushes over a whole loop.
    enum : unsigned { G612_RBP = 5u, G612_RSI = 6u, G612_RDI = 7u };

    // These mirror two `vu1_g490_block_run.inc` constants. That file is included AFTER this one;
    // the static_asserts that tie them together live there, beside the definitions.
    constexpr uint32_t G612_MIN_ENTRY = 2u;   // == G490_MIN_RUN
    constexpr uint8_t G612_FLAG_TAIL = 3u;    // == G531_FLAG_TAIL

    // ⭐ G651: the admission predicates in §3 run ahead of the flag block in §5, so the FCGET arm is
    // forward-declared here. Its definition (and the whole exactness argument) is beside
    // `g612VerifyOn`.
    bool g651VuClipOn();
    bool g652VuLoiOn();
    bool g652VuShortEntryOn();
    bool g652VuHazStoreOn();

    constexpr uint32_t G612_MAX_BB = 48u;          // basic blocks per region
    constexpr uint32_t G612_MAX_PAIRS = 512u;      // emitted pairs per region
    constexpr size_t G612_MAX_REGION_BYTES = 96u * 1024u;
    constexpr size_t G612_ARENA_BYTES = 32u * 1024u * 1024u;
    // ⚠️ SIZED FROM A MEASUREMENT, NOT A GUESS. The first bring-up run set this to 96 and
    // `[G612:why] table = 1308` — thirteen regions refused for table space for every one admitted.
    // The cause was not the microprogram: it was this file starting a NEW region at every unbound
    // eligible slot, so one loop body produced a fresh overlapping region per slot inside it. The
    // `s_g612Claim` bitmap below fixes that at source; this number covers what is left.
    constexpr uint32_t G612_MAX_REGIONS = 256u;
    constexpr uint32_t G612_SLOTS = PS2_VU1_CODE_SIZE / 8u;
    constexpr uint32_t G612_IMAGE_RING = 4u;       // whole-image memo (see §7)

    enum : uint8_t
    {
        G612_T_EXIT = 0u, // the pair at start + plain is not emittable: write pc, return
        G612_T_FALL = 1u, // straight-line successor is another leader
        G612_T_BR = 2u,   // conditional branch + delay slot
        G612_T_JMP = 3u,  // B / BAL (static target) + delay slot
        G612_T_JR = 4u,   // JR / JALR + delay slot, then a dynamic exit
    };

    enum G612Refuse : int
    {
        G612_RF_ENTRY = 0,
        G612_RF_CAPS,
        G612_RF_MISMATCH,
        G612_RF_ARENA,
        G612_RF_ENCODE,
        G612_RF_TRIVIAL,
        G612_RF_TABLE,
        G612_RF_COUNT
    };
    const char *const kG612RfName[G612_RF_COUNT] = {"entry", "caps", "mismatch", "arena",
                                                    "encode", "trivial", "table"};

    // x86 condition codes used here.
    enum : uint8_t
    {
        G612_CC_E = 0x4u, G612_CC_NE = 0x5u, G612_CC_A = 0x7u, G612_CC_L = 0xCu,
        G612_CC_GE = 0xDu, G612_CC_LE = 0xEu, G612_CC_G = 0xFu,
    };

    // -------------------------------------------------------------------------------------------
    // §2 ENCODER ADDITIONS. Same doctrine as G610 §5: one helper per instruction form, nothing
    // chooses an encoding at run time, so a wrong byte is a wrong CONSTANT in exactly one place.
    // -------------------------------------------------------------------------------------------
    struct G612Emit
    {
        G610Emit *e = nullptr;

        void u8(uint8_t v) { e->u8(v); }
        void i32(int32_t v) { e->i32(v); }

        // ---- 64-bit integer forms G610 never needed ----
        void addImm64(unsigned r, int32_t v)
        { e->rex(true, 0u, 0u, r); u8(0x81); e->modRR(0u, r); i32(v); }
        void movR64(unsigned rd, unsigned rs)          // mov rd, rs
        { e->rex(true, rs, 0u, rd); u8(0x89); e->modRR(rs, rd); }
        void addR64(unsigned rd, unsigned rs)          // add rd, rs
        { e->rex(true, rs, 0u, rd); u8(0x01); e->modRR(rs, rd); }
        void subR64(unsigned rd, unsigned rs)          // sub rd, rs
        { e->rex(true, rs, 0u, rd); u8(0x29); e->modRR(rs, rd); }
        void cmpR64(unsigned rd, unsigned rs)          // cmp rd, rs
        { e->rex(true, rs, 0u, rd); u8(0x39); e->modRR(rs, rd); }

        // mov r32, [rsp + disp32]. RSP's low three bits are 100, so this is the ONE form in the
        // whole backend that needs an explicit no-index SIB byte — G610 §2's note that no emitted
        // base ever requires one stops being true the moment a region reads its fifth argument.
        void movLd32Rsp(unsigned r, int32_t disp)
        {
            e->rex(false, r, 0u, 0u);
            u8(0x8B);
            u8((uint8_t)(0x80u | ((r & 7u) << 3) | 4u)); // mod=10 rm=100 -> SIB follows
            u8(0x24);                                    // scale=0 index=100(none) base=100(RSP)
            i32(disp);
        }

        void pushR(unsigned r) { if (r >= 8u) u8(0x41); u8((uint8_t)(0x50u + (r & 7u))); }
        void popR(unsigned r) { if (r >= 8u) u8(0x41); u8((uint8_t)(0x58u + (r & 7u))); }

        // ---- 16-bit compares. The branch bodies compare `(int16_t)vi[x]`, so a 16-bit compare of
        // the two low halves IS that expression, with no sign extension anywhere. ----
        void movLd16(unsigned r, unsigned b, int32_t d)   // mov r16, [b+d]
        { u8(0x66); e->rex(false, r, 0u, b); u8(0x8B); e->memD(r, b, d); }
        void cmpLd16(unsigned r, unsigned b, int32_t d)   // cmp r16, [b+d]
        { u8(0x66); e->rex(false, r, 0u, b); u8(0x3B); e->memD(r, b, d); }

        void testR32(unsigned r) { e->rex(false, r, 0u, r); u8(0x85); e->modRR(r, r); }
        void movR32(unsigned rd, unsigned rs)
        { e->rex(false, rs, 0u, rd); u8(0x89); e->modRR(rs, rd); }
        void cmpImm32(unsigned r, int32_t v)
        { e->rex(false, 0u, 0u, r); u8(0x81); e->modRR(7u, r); i32(v); }

        // setcc DIL and `test dil, dil`. Both need a REX byte with no field set (0x40) purely to
        // select the low byte of RDI instead of BH — `G610Emit::rex` deliberately suppresses that
        // byte, so these two emit it themselves.
        void setccDil(uint8_t cc)
        { u8(0x40); u8(0x0F); u8((uint8_t)(0x90u + cc)); u8(0xC7); }
        void testDil()
        { u8(0x40); u8(0x84); u8(0xFF); }

        // ---- control transfer. Both forms are rel32 and both are patched later; the return value
        // is the OFFSET OF THE rel32 FIELD. ----
        size_t jmpRel32()
        { u8(0xE9); const size_t at = e->n; i32(0); return at; }
        size_t jccRel32(uint8_t cc)
        { u8(0x0F); u8((uint8_t)(0x80u + cc)); const size_t at = e->n; i32(0); return at; }
    };

    // Patch one rel32. Refuses (rather than writing out of bounds) if the buffer overflowed — an
    // encoder that ran out of room leaves `at` pointing at `cap`.
    bool g612Patch(G610Emit &e, size_t at, int64_t dstOff)
    {
        if (e.bad || at + 4u > e.n)
            return false;
        const int64_t rel = dstOff - (int64_t)(at + 4u);
        if (rel < -2147483648ll || rel > 2147483647ll)
            return false;
        const int32_t v = (int32_t)rel;
        std::memcpy(&e.p[at], &v, 4);
        return true;
    }

    // -------------------------------------------------------------------------------------------
    // §3 THE ADMISSION PREDICATE (planning only — precondition 7 makes the emitter authoritative)
    // -------------------------------------------------------------------------------------------
    bool g612UpperEmittable(uint8_t kind, uint32_t upper)
    {
        if (kind == 0x6fu)
            return true;                                // VNOP
        const G421UpperDesc d = g421DescTableConst.e[kind];
        if (d.cls == 0u)
            return false;
        const bool ops = g611OpsEmit();
        if (d.cls == 8u)
            return ops;                                 // CLIPw
        if (d.cls >= 3u)
            return d.cls <= 7u;                         // ITOF / FTOI / ABS / MOVE / MR32
        switch (d.op)                                   // cls 1 (-> VF) / cls 2 (-> ACC)
        {
        case 0u: case 1u: case 2u: case 3u: case 4u:
            return true;
        case 5u: case 6u:                               // MIN / MAX
            return (DEST(upper) == 0xFu) || ops;        // partial dest needs G546 canonicalisation
        case 7u: case 8u:
            return ops;                                 // OPMSUB / OPMULA
        default:
            return false;
        }
    }

    bool g612LowerEmittable(uint8_t kind)
    {
        // ⭐ G651: FCGET reads only `m_state.clip` under the constant its own decode pins. See the
        // header note beside `g651VuClipOn`. Admitted only through `g651RegionClipPairOk`, which is
        // the gate that also re-checks every non-flag clause of `g490BlockPairEligible`.
        if (kind == G422_LK_FCGET)
            return g651VuClipOn();
        switch (kind)
        {
        case G422_LK_NOP:
        case G422_LK_IADDI: case G422_LK_IADDIU: case G422_LK_ISUBIU:
        case G422_LK_IADD:  case G422_LK_ISUB:   case G422_LK_IAND: case G422_LK_IOR:
        case G422_LK_MOVE:  case G422_LK_MR32:   case G422_LK_MTIR: case G422_LK_MFIR:
        case G422_LK_LQ:    case G422_LK_LQI:    case G422_LK_LQD:
        case G422_LK_SQ:    case G422_LK_SQI:    case G422_LK_SQD:
        case G422_LK_ILW:   case G422_LK_ILWR:   case G422_LK_ISW:  case G422_LK_ISWR:
            return true;
        default:
            return false;
        }
    }

    // ⭐ G651: the one class `g490BlockPairEligible` excludes for a reason that does not hold inside
    // a region. EVERY OTHER clause of that predicate is re-checked here, by hand, in the same order:
    // no E-bit / I-bit / LOI, no G239 prestall, no G139 pair hazard, an upper the emitter runs. The
    // clause that is deliberately NOT re-checked is `G427_PF_FLAGREAD`, and the header note above is
    // the argument for why. `G428_PF_STATUSREAD` IS refused — a STATUS reader is a different line
    // with a different publication and this lever does not touch it.
    bool g651RegionClipPairOk(uint32_t slot)
    {
        if (!g651VuClipOn() || slot >= G612_SLOTS)
            return false;
        const G410CompiledUpper &p = s_g410Cache.uppers[slot];
        if (p.lowerKind != G422_LK_FCGET)
            return false;
        if (p.pairFlags != 0u)
            return false;                               // E-bit / I-bit / LOI
        if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G428_PF_STATUSREAD)) != 0u)
            return false;
        if (p.upperKind == 0x6fu)
            return true;                                // VNOP: the loop skips the upper
        return g421DescTableConst.e[p.upperKind].cls != 0u;
    }

    // G652 P13: SQI's same-pair hazard means its VF source must observe the pre-upper value.
    // The generic loop implements that by saving the old VF, executing upper, temporarily
    // restoring old VF for lower, then overlaying the upper result. SQI never writes a VF, so the
    // exact equivalent is much smaller: emit SQI first and upper second. The store and VI
    // post-increment are invisible to every upper opcode, while the upper's FMAC history record
    // is invisible to SQI. Keep this admission deliberately limited to the measured opcode.
    bool g652RegionHazStorePairOk(uint32_t slot)
    {
        if (!g652VuHazStoreOn() || slot >= G612_SLOTS)
            return false;
        const G410CompiledUpper &p = s_g410Cache.uppers[slot];
        if (p.pairFlags != 0u || p.lowerKind != G422_LK_SQI ||
            (p.g425Flags & G426_PF_HAZ) == 0u)
            return false;
        if ((p.g425Flags & (G426_PF_PRESTALL | G427_PF_FLAGREAD | G428_PF_STATUSREAD)) != 0u)
            return false;
        if (p.upperKind == 0x6fu)
            return true;
        return g421DescTableConst.e[p.upperKind].cls != 0u;
    }

    // A PLAIN region pair: block-eligible by the shipped sidecar AND inside the emitted op set.
    bool g612PlainEmittable(uint32_t slot)
    {
        if (slot >= G612_SLOTS)
            return false;
        const G410CompiledUpper &p = s_g410Cache.uppers[slot];
        // G652 P3: an I-bit pair executes the upper after loading the lower word into st.i; the
        // lower word is not an instruction. E-bit remains an exit because it latches program end.
        if (g652VuLoiOn() && p.pairFlags == G410_PAIR_I_BIT)
        {
            const G479FusedPair &fp = s_g410Cache.fused[slot];
            return g612UpperEmittable(fp.pair.upperKind, (uint32_t)(fp.source >> 32));
        }
        // precondition 1 — or, for FCGET only, the hand-checked equivalent above.
        if (s_g410Cache.g490RunLen[slot] == 0u && !g651RegionClipPairOk(slot) &&
            !g652RegionHazStorePairOk(slot))
            return false;
        const G479FusedPair &fp = s_g410Cache.fused[slot];
        return g612UpperEmittable(fp.pair.upperKind, (uint32_t)(fp.source >> 32)) &&
               g612LowerEmittable(fp.pair.lowerKind);
    }

    // A region BRANCH pair: the sidecar must already admit it as a G500 tail WITH its delay slot
    // (`g500Tail == 2`, precondition 2), the upper must be emittable, and the lower must be one of
    // the six conditional forms, B/BAL, or JR/JALR.
    bool g612BranchEmittable(uint32_t slot)
    {
        if (slot + 1u >= G612_SLOTS)
            return false;
        if (s_g410Cache.g500Tail[slot] != 2u)
            return false;
        const G479FusedPair &fp = s_g410Cache.fused[slot];
        if (!g612UpperEmittable(fp.pair.upperKind, (uint32_t)(fp.source >> 32)))
            return false;
        switch (fp.pair.lowerKind)
        {
        case G422_LK_B: case G422_LK_BAL: case G422_LK_JR: case G422_LK_JALR:
        case G422_LK_IBEQ: case G422_LK_IBNE: case G422_LK_IBLTZ:
        case G422_LK_IBGTZ: case G422_LK_IBLEZ: case G422_LK_IBGEZ:
            return g612PlainEmittable(slot + 1u);       // the delay slot
        default:
            return false;
        }
    }

    // -------------------------------------------------------------------------------------------
    // §4 REGION STRUCTURES AND THE BACKEND
    // -------------------------------------------------------------------------------------------
    using G612Fn = uint32_t (*)(VU1State *, uint8_t *, int64_t, G430HistBlock *, uint32_t);

    struct G612Bb
    {
        uint32_t start = 0u;
        uint16_t plain = 0u;      // plain pairs before the terminator
        uint16_t pairs = 0u;      // plain + 2 for a branch terminator
        uint8_t term = G612_T_EXIT;
        uint8_t brKind = 0u;
        uint32_t takenSlot = 0u;
        uint32_t fallSlot = 0u;
        uint32_t bodyOff = 0u;
        uint32_t entryOff = 0xFFFFFFFFu;
    };

    struct G612SlotRec
    {
        G612Fn fn = nullptr;
        uint16_t pairs = 0u;      // the ENTRY block's pair count (the caller's budget precheck)
        // ⭐ Does ANY pair anywhere in this region read `st.q` (an upper with descriptor
        // `src == 1`)? If none does, a pending Q publication is UNOBSERVABLE inside the region and
        // may be deferred to the exit — which is G611 §3's argument, generalised from "before the
        // publication index" to "anywhere in the body". Nothing G490 admits reads P at all (MFP
        // decodes to G422_LK_NONE), so P is always deferrable and needs no flag.
        uint8_t readsQ = 0u;
    };

    struct G612Region
    {
        uint64_t key = 0ull;      // 0 = free
        uint32_t nBb = 0u;
        uint8_t readsQ = 0u;
        uint32_t bbSlot[G612_MAX_BB] = {};
        G612Fn bbFn[G612_MAX_BB] = {};
        uint16_t bbPairs[G612_MAX_BB] = {};
    };

    struct G612Backend
    {
        uint8_t *arena = nullptr;
        size_t used = 0u;
        bool arenaFull = false;
        uint32_t dataSize = 0u;
        uint32_t codeSize = 0u;
        std::array<G612SlotRec, G612_SLOTS> slot{};
        std::array<G612Region, G612_MAX_REGIONS> region{};
        uint32_t nRegion = 0u;
        // §7 whole-image memo
        uint64_t imgKey[G612_IMAGE_RING] = {};
        std::array<std::array<G612SlotRec, G612_SLOTS>, G612_IMAGE_RING> imgSlot{};
        uint32_t imgNext = 0u;
    };
    thread_local G612Backend s_g612;

    thread_local uint64_t s_g612Compiled = 0u, s_g612Bbs = 0u, s_g612CodeBytes = 0u;
    thread_local uint64_t s_g612Bound = 0u, s_g612Rebuilds = 0u;
    thread_local uint64_t s_g612ImgHit = 0u, s_g612ImgMiss = 0u;
    thread_local uint64_t s_g612CacheHit = 0u, s_g612CacheMiss = 0u, s_g612ArenaResets = 0u;
    thread_local uint64_t s_g612Runs = 0u, s_g612Pairs = 0u, s_g612MaxPairs = 0u;
    thread_local uint64_t s_g612SkipQp = 0u, s_g612SkipBudget = 0u, s_g612SkipSize = 0u;
    thread_local uint64_t s_g612Verified = 0u;
    thread_local uint64_t s_g612RfWhy[G612_RF_COUNT] = {};
    // ⭐ WHERE the regions stop, by slot. "The regions ran 4.1 pairs on average" is not a
    // diagnosis; "83% of the exits are one XGKICK at pc=0x1a38" is. Only an exit-site histogram
    // separates them (G603's law: name the blocker).
    thread_local std::array<uint32_t, G612_SLOTS> s_g612ExitAt{};

    bool g612StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G612_STAT");
        return s_on;
    }
    bool g612VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G612_VERIFY");
        return s_on;
    }
    bool g652VuLoiOn()
    {
        // P3 prototype remains available for exactness/census work, but its independent
        // warmed timing gate found no VU benefit. Keep the proven legacy exit as default.
        static const bool s_on = f50_12_vu_env_flag("DC2_G652_VU_LOI") &&
                                 !f50_12_vu_env_flag("DC2_G652_NO_VU_LOI");
        return s_on;
    }
    bool g652VuShortEntryOn()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G652_NO_VU_SHORTENTRY");
        return s_on;
    }
    bool g652VuHazStoreOn()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G652_NO_VU_HAZSTORE");
        return s_on;
    }
    thread_local std::array<uint32_t, G612_SLOTS> s_g652ExitIAt{};
    thread_local std::array<uint32_t, G612_SLOTS> s_g652ExitEAt{};
    thread_local std::array<uint32_t, G612_SLOTS> s_g652FmandPendingAt{};
    thread_local std::array<uint32_t, G612_SLOTS> s_g652FmandMaterialAt{};

    // G652: `s_g612ExitAt` is intentionally a slot histogram, but VU micro-memory is
    // self-modifying.  Labelling that accumulated count with the *latest* image in
    // `s_g410Cache` produced plausible-looking lies when one slot hosted several programs.
    // Keep an exact raw-instruction identity alongside the legacy location census.  This table
    // exists only behind DC2_G612_STAT, so the larger key and open-addressed lookup do not touch
    // the production path.
    constexpr uint32_t G652_EXIT_ID_CAP = 16384u;
    struct G652ExitIdentity
    {
        uint64_t source = 0u;
        uint64_t count = 0u;
        uint64_t iCount = 0u;
        uint64_t eCount = 0u;
        uint64_t fmandPending = 0u;
        uint64_t fmandMaterial = 0u;
        uint16_t slot = 0u;
        uint8_t lowerKind = 0u;
        uint8_t upperKind = 0u;
        uint8_t pairFlags = 0u;
        uint8_t g425Flags = 0u;
        uint8_t tail = 0u;
        uint8_t used = 0u;
    };
    thread_local std::array<G652ExitIdentity, G652_EXIT_ID_CAP> s_g652ExitIdentity{};
    thread_local uint64_t s_g652ExitIdentityDropped = 0u;

    void g652ExitIdentityNote(uint32_t slot, int fmandState)
    {
        const G410CompiledUpper &p = s_g410Cache.uppers[slot];
        const uint64_t source = s_g410Cache.sources[slot];
        const uint8_t tail = s_g410Cache.g500Tail[slot];
        uint64_t hash = source ^ (static_cast<uint64_t>(slot) * 0x9e3779b97f4a7c15ull);
        hash ^= static_cast<uint64_t>(tail) * 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 30u;
        hash *= 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 27u;
        hash *= 0x94d049bb133111ebull;
        hash ^= hash >> 31u;
        uint32_t at = static_cast<uint32_t>(hash) & (G652_EXIT_ID_CAP - 1u);
        for (uint32_t probe = 0u; probe < G652_EXIT_ID_CAP; ++probe)
        {
            G652ExitIdentity &r = s_g652ExitIdentity[at];
            if (r.used == 0u)
            {
                r.used = 1u;
                r.source = source;
                r.slot = static_cast<uint16_t>(slot);
                r.lowerKind = p.lowerKind;
                r.upperKind = p.upperKind;
                r.pairFlags = p.pairFlags;
                r.g425Flags = p.g425Flags;
                r.tail = tail;
            }
            if (r.source == source && r.slot == slot && r.lowerKind == p.lowerKind &&
                r.upperKind == p.upperKind && r.pairFlags == p.pairFlags &&
                r.g425Flags == p.g425Flags && r.tail == tail)
            {
                ++r.count;
                if ((p.pairFlags & G410_PAIR_I_BIT) != 0u)
                    ++r.iCount;
                if ((p.pairFlags & G410_PAIR_E_BIT) != 0u)
                    ++r.eCount;
                if (fmandState > 0)
                    ++r.fmandPending;
                else if (fmandState == 0)
                    ++r.fmandMaterial;
                return;
            }
            at = (at + 1u) & (G652_EXIT_ID_CAP - 1u);
        }
        ++s_g652ExitIdentityDropped;
    }

    bool g652MacHistoryPendingAt(const G430HistBlock &h, int64_t target)
    {
        int best = -1;
        int64_t bestStamp = INT64_MIN;
        for (int i = 0; i < G430_HIST; ++i)
        {
            const int64_t stamp = h.stamp[i];
            if (stamp <= target && stamp > bestStamp)
            {
                bestStamp = stamp;
                best = i;
            }
        }
        return best >= 0 && h.kind[best] == G428_MK_PENDING;
    }
    // ============================================================================================
    // ⭐ G651 (ROADMAP Priority 3) — FCGET EMITTED INSIDE THE REGION BODY
    //
    // PREMISE (G650 §P7, re-stated). The VU1 backend is at 100% native COVERAGE (`refused = 0`,
    // every `[G610:why]` / `[G612:why]` counter 0) and the remaining cost is not refusals but
    // **931,135,488 REGION ROUND TRIPS on `georama`** at 19.68 pairs each. `[G612:exit]` names the
    // program points, and the largest single one is a flag consumer:
    //
    //     0x30d0 : 225,966,473  lk=37 (FCGET)   <- 24% of every region entry on the VU1-poled route
    //
    // Each of those costs a region EXIT (store `st.pc`, return), one generic interpreter pair, and
    // a fresh region ENTRY with its prologue — to execute `vi[it] = clip & 0xFFF`.
    //
    // ⭐ WHY FCGET IS ADMISSIBLE AND THE OTHER FLAG CONSUMERS ARE NOT.
    // `G422_LK_FCGET` is only ever DECODED when `g487FlagLowerSupported()` holds, and that pins
    // `DC2_VU1_NO_FMSWAPFIX` unset — i.e. `s_fmFixed == true`. Under that constant
    // `VU1Interpreter::execLower` case 0x1C reduces to exactly `if (it) vi[it] = clip & 0xFFF`
    // (`vu1_dispatch_and_sync.inc:331`): it reads `m_state.clip` and NOTHING else. `clip` is an
    // architectural word the region ALREADY maintains in memory — G611's CLIPw emission ends with
    // `shl [rcx+OFF_CLIP], 6` + `or` — so the whole body is three x86-64 instructions against a
    // value the region owns, with no new emitter primitive.
    //
    // ⚠️ THE ONE SIDE EFFECT THAT IS SKIPPED, AND WHY IT IS DEAD. `g427LowerReadsFlags` marks 0x1C
    // as a MAC reader *unconditionally* (its own comment says "reads MAC only under
    // DC2_VU1_NO_FMSWAPFIX; kept unconditionally"), so the generic pair loop publishes
    // `s_vuMacVisible = g430ResolveAt(...)` before executing it. An in-region FCGET does not.
    // That publication is unobservable because **`s_vuMacVisible` is never a carried value**: every
    // opcode that reads it (FMEQ 0x18, FMAND 0x1A, FMOR 0x1B, and 0x1C only when `!s_fmFixed`) is
    // itself a `G427_PF_FLAGREAD` pair, and every path that can execute one republishes it from the
    // G430 ring first — the pair loop at `vu1_g435_pair_loop.inc:1896/1921`, the G531/G532 typed
    // flag tails at `vu1_g490_block_run.inc:2439/2974`, and the hand-written fragments at 4324.
    // G490 blocks, G610 blocks and G612 regions all EXCLUDE flag readers, so none of them can
    // observe a stale line either. The region keeps the ring cursor exact (precondition 5), so the
    // republication a later consumer performs reads the same ring it would have read.
    //
    // ⛔ FCAND (0x12) and FCOR (0x13) are NOT built, and the reason is the ORACLE, not the emission.
    // Neither sets `G427_PF_FLAGREAD`, so neither is `g531FlagTailEligible`, so `g502TailAt` is 0
    // at their slot and `g612RefRun` — the reference executor `DC2_G612_VERIFY=1` compares against —
    // has no path that advances over one. Building them would mean building a second reference, and
    // `[G612:exit]` lists neither in its top eight on `georama`: the prize is FCGET's 226 M.
    //
    // Oracle `DC2_G612_VERIFY=1` (whole VU1State + the G430 ring + all of VU data RAM after every
    // region). Rollback `DC2_G651_NO_VUCLIP=1`. Census `[G651:vuclip]`.
    // ============================================================================================
    bool g651VuClipOn()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G651_NO_VUCLIP");
        return s_on;
    }
    thread_local uint64_t s_g651ClipEmitted = 0ull; // FCGET pairs emitted into region bodies
    thread_local uint64_t s_g651ClipSlots = 0ull;   // distinct slots admitted
    bool g612Paired()
    {
        static const bool s_p = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "vureg") == 0;
        }();
        return s_p;
    }
    // PROCESS level: do the region bodies EXIST. True for either arm of the paired A/B, so a flip
    // to the candidate arm can never find an empty slot table (G490 §2's structurally-unmeasurable
    // lever trap — the fourth recurrence in this arc).
    bool g612BackendEnabled()
    {
        static const bool s_on =
            g612Paired() ||
            (!f50_12_vu_env_flag("DC2_G612_NO_REGION") &&
             (DC2_G612_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G612_REGION")));
        return s_on;
    }
    // PER-RUN() arm: does the HOOK use them. Loop-invariant, read once per run() exactly like
    // every other arm in this family (G479's rule).
    bool g612RegionRunOn()
    {
        if (g612RegionArm() >= 0)
            return g612RegionArm() > 0;
        static const bool s_on =
            !f50_12_vu_env_flag("DC2_G612_NO_REGION") &&
            (DC2_G612_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G612_REGION"));
        return s_on;
    }

    void g612ArenaInit()
    {
#if defined(_WIN32)
        if (s_g612.arena == nullptr && !s_g612.arenaFull)
        {
            s_g612.arena = (uint8_t *)VirtualAlloc(nullptr, G612_ARENA_BYTES, 0x1000u | 0x2000u,
                                                   0x40u);
            if (s_g612.arena == nullptr)
                s_g612.arenaFull = true;
        }
#else
        s_g612.arenaFull = true;
#endif
    }

    // Bump allocator with no free list: when it fills, every region is dropped at once and every
    // memo emptied with them. Rare by construction (the live image set is small) and counted, so
    // "rare" stays a measurement.
    void g612ArenaReset()
    {
        s_g612.used = 0u;
        s_g612.nRegion = 0u;
        for (auto &r : s_g612.region)
            r.key = 0ull;
        for (uint32_t k = 0u; k < G612_IMAGE_RING; ++k)
        {
            s_g612.imgKey[k] = 0ull;
            s_g612.imgSlot[k].fill(G612SlotRec{});
        }
        ++s_g612ArenaResets;
        for (auto &s : s_g612.slot)
            s = G612SlotRec{};
    }

    // -------------------------------------------------------------------------------------------
    // §5 CFG DISCOVERY AND LAYOUT
    //
    // Two passes over the same walk. DISCOVERY ignores leaders and only collects them; LAYOUT
    // re-walks with the final leader set and stops a block at the next leader. Both read only the
    // precisely-invalidated sidecars (`g490RunLen`, `g500Tail`, `fused`), never the live code
    // words, so a region can no more describe a stale encoding than a G610 block can.
    //
    // ⚠️ A block STOPS at a leader only while it is scanning PLAIN pairs. Once the terminator is
    // known to be a branch, its DELAY SLOT is included unconditionally even if that slot is also a
    // leader — a delay slot is architecturally part of the branch, and a second block starting
    // there simply emits the pair a second time. Duplicated bytes, never duplicated semantics.
    // -------------------------------------------------------------------------------------------
    struct G612Plan
    {
        uint32_t leader[G612_MAX_BB] = {};
        uint32_t nLeader = 0u;
        G612Bb bb[G612_MAX_BB] = {};
        uint32_t nBb = 0u;
        uint32_t totalPairs = 0u;
        bool anyBranch = false;
    };
    // At namespace scope, not a function-local static: a function-local `static thread_local` is a
    // PER-ACCESS TLS guard (G609's law). This one only ever runs in the cold rebuild pass, but the
    // law is cheap to obey and expensive to remember.
    thread_local G612Plan s_g612Plan;

    bool g612IsLeader(const G612Plan &p, uint32_t slot)
    {
        for (uint32_t k = 0u; k < p.nLeader; ++k)
            if (p.leader[k] == slot)
                return true;
        return false;
    }

    bool g612AddLeader(G612Plan &p, uint32_t slot)
    {
        if (slot >= G612_SLOTS)
            return true;                              // out of image: not an error, just not added
        if (g612IsLeader(p, slot))
            return true;
        if (p.nLeader >= G612_MAX_BB)
            return false;
        p.leader[p.nLeader++] = slot;
        return true;
    }

    // The static branch target of a non-indirect control transfer, as a SLOT index. Mirrors
    // `G481_LOWER_BODY_B` / `_IBEQ`: `target = (pc + 8 + imm * 8) & 0x3FFF`.
    uint32_t g612StaticTarget(uint32_t slot, uint32_t lower)
    {
        const int32_t imm = (int32_t)IMM11(lower);
        const uint32_t pc = slot * 8u;
        const uint32_t target = (uint32_t)((int32_t)pc + 8 + imm * 8) & 0x3FFFu;
        return target >> 3u;
    }

    void g612WalkBb(const G612Plan &p, uint32_t start, bool stopAtLeader,
                    G612Bb *out, uint32_t *succA, uint32_t *succB)
    {
        *succA = 0xFFFFFFFFu;
        *succB = 0xFFFFFFFFu;
        *out = G612Bb{};
        out->start = start;

        uint32_t i = start;
        for (;;)
        {
            if (i >= G612_SLOTS || out->plain >= G612_MAX_PAIRS - 2u)
            {
                out->term = G612_T_EXIT;
                out->pairs = out->plain;
                return;
            }
            if (stopAtLeader && i != start && g612IsLeader(p, i))
            {
                out->term = G612_T_FALL;
                out->fallSlot = i;
                out->pairs = out->plain;
                *succB = i;
                return;
            }
            if (g612PlainEmittable(i))
            {
                ++out->plain;
                ++i;
                continue;
            }
            if (g612BranchEmittable(i))
            {
                const G479FusedPair &fp = s_g410Cache.fused[i];
                const uint32_t lower = (uint32_t)fp.source;
                out->brKind = fp.pair.lowerKind;
                out->pairs = (uint16_t)(out->plain + 2u);
                out->fallSlot = i + 2u;
                if (out->brKind == G422_LK_JR || out->brKind == G422_LK_JALR)
                {
                    out->term = G612_T_JR;   // dynamic destination: run the delay slot, then exit
                    return;
                }
                const uint32_t t = g612StaticTarget(i, lower);
                if (t == i + 1u)
                {
                    // ⭐ THE DEGENERATE ZERO DISPLACEMENT. `st.pc = target - 8` leaves pc EQUAL to
                    // the branch pair's own pc, and the pair loop's `taken = (st.pc != branchPc)`
                    // test therefore reads NOT TAKEN and falls through to branchPc + 16 — for a
                    // conditional, on BOTH outcomes. G500's exactness note calls this out
                    // explicitly. Reproduce the SHIPPED behaviour, not the architectural one.
                    out->term = G612_T_JMP;
                    out->takenSlot = i + 2u;
                    *succA = out->takenSlot;
                    return;
                }
                if (out->brKind == G422_LK_B || out->brKind == G422_LK_BAL)
                {
                    out->term = G612_T_JMP;
                    out->takenSlot = t;
                    *succA = t;
                    return;
                }
                out->term = G612_T_BR;
                out->takenSlot = t;
                *succA = t;
                *succB = i + 2u;
                return;
            }
            // Anything else — XGKICK, a flag consumer, DIV, an E-bit pair, a branch whose delay
            // slot is not eligible — is a REGION EXIT at this slot.
            out->term = G612_T_EXIT;
            out->pairs = out->plain;
            return;
        }
    }

    bool g612Discover(G612Plan &p, uint32_t entrySlot)
    {
        p = G612Plan{};
        if (!g612AddLeader(p, entrySlot))
            return false;
        for (uint32_t k = 0u; k < p.nLeader; ++k)
        {
            G612Bb tmp;
            uint32_t a = 0xFFFFFFFFu, b = 0xFFFFFFFFu;
            g612WalkBb(p, p.leader[k], false, &tmp, &a, &b);
            if (tmp.term == G612_T_BR || tmp.term == G612_T_JMP || tmp.term == G612_T_JR)
                p.anyBranch = true;
            if (tmp.term == G612_T_EXIT && tmp.plain > 0u)
            {
                // The pair at `start + plain` is the one the interpreter has to run. The slot AFTER
                // it is a natural re-entry — that is what turns "the interpreter owns the tail"
                // into "the interpreter owns ONE pair". Only when that pair is not itself a
                // control transfer: a delay slot can never be entered with
                // `pendingBranchTarget == NO_BRANCH`, so a leader there would be dead code.
                const uint32_t x = tmp.start + tmp.plain;
                const uint8_t xt = (x < G612_SLOTS) ? s_g410Cache.g500Tail[x] : 0u;
                if (x + 1u < G612_SLOTS && xt != 1u && xt != 2u && g612PlainEmittable(x + 1u))
                    if (!g612AddLeader(p, x + 1u))
                        return false;
            }
            if (a != 0xFFFFFFFFu && !g612AddLeader(p, a))
                return false;
            if (b != 0xFFFFFFFFu && !g612AddLeader(p, b))
                return false;
        }
        return true;
    }

    bool g612Layout(G612Plan &p)
    {
        // Leaders in ascending slot order, so a fall-through usually lands on the next emitted
        // block and needs no jump at all.
        for (uint32_t a = 0u; a + 1u < p.nLeader; ++a)
            for (uint32_t b = a + 1u; b < p.nLeader; ++b)
                if (p.leader[b] < p.leader[a])
                {
                    const uint32_t t = p.leader[a];
                    p.leader[a] = p.leader[b];
                    p.leader[b] = t;
                }
        p.nBb = 0u;
        p.totalPairs = 0u;
        for (uint32_t k = 0u; k < p.nLeader; ++k)
        {
            uint32_t a = 0xFFFFFFFFu, b = 0xFFFFFFFFu;
            G612Bb bb;
            g612WalkBb(p, p.leader[k], true, &bb, &a, &b);
            if (p.totalPairs + bb.pairs > G612_MAX_PAIRS)
            {
                // Out of budget for more blocks. Stop here: every edge into a block that was not
                // emitted becomes an exit stub at that pc, which is exactly the right behaviour —
                // the pair loop takes over there and re-enters at the next bound leader.
                ++s_g612RfWhy[G612_RF_CAPS];
                break;
            }
            p.totalPairs += bb.pairs;
            p.bb[p.nBb++] = bb;
        }
        return p.nBb != 0u;
    }

    int g612FindBb(const G612Plan &p, uint32_t slot)
    {
        for (uint32_t k = 0u; k < p.nBb; ++k)
            if (p.bb[k].start == slot)
                return (int)k;
        return -1;
    }

    // -------------------------------------------------------------------------------------------
    // §6 EMISSION
    // -------------------------------------------------------------------------------------------
    struct G612Fix
    {
        size_t at = 0u;      // offset of the rel32 field
        int32_t bb = -1;     // >= 0 : block index | -1 : shared epilogue | -2 : exit stub for `pc`
        uint32_t pc = 0u;
    };

    // The branch CONDITION, evaluated BEFORE the delay slot and parked in DIL.
    void g612EmitCondition(G610Emit &e, G612Emit &x, uint8_t kind, uint32_t lower)
    {
        const unsigned it = LIT(lower), is = LIS(lower);
        if (kind == G422_LK_IBEQ || kind == G422_LK_IBNE)
        {
            x.movLd16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            x.cmpLd16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            x.setccDil(kind == G422_LK_IBEQ ? G612_CC_E : G612_CC_NE);
            return;
        }
        // The four `(int16_t)vi[is] <cmp> 0` forms.
        e.movsx16M(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
        x.testR32(G610_RAX);
        uint8_t cc = G612_CC_L;
        if (kind == G422_LK_IBGTZ) cc = G612_CC_G;
        else if (kind == G422_LK_IBLEZ) cc = G612_CC_LE;
        else if (kind == G422_LK_IBGEZ) cc = G612_CC_GE;
        x.setccDil(cc);
    }

    // The G425 re-pin, byte for byte as `g610TranslateBlock` emits it.
    void g612EmitRepin(G610Emit &e, const G479FusedPair &fp)
    {
        if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) == 0u)
            return;
        e.movapsLd(G610_X4, G610_R11, G610_C_VF0);
        e.movupsSt(G610_X4, G610_RCX, G610_OFF_VF);
        e.movStImm32(G610_RCX, G610_OFF_VI, 0u);
    }

    // One pair: upper, lower, G425 re-pin. Returns false to abandon the region — precondition 7:
    // the EMITTER is authoritative over the predicate.
    bool g612EmitPair(G610Emit &e, uint32_t slot, uint32_t j, uint32_t dataSize)
    {
        const G479FusedPair &fp = s_g410Cache.fused[slot];
        const uint32_t upper = (uint32_t)(fp.source >> 32);
        const uint32_t lower = (uint32_t)fp.source;
        int why = 0;
        bool usesHist = false;
        bool used = false;
        const bool loi = g652VuLoiOn() &&
                         fp.pair.pairFlags == G410_PAIR_I_BIT;
        const bool hazardStore = g652RegionHazStorePairOk(slot);
        if (loi)
            e.movStImm32(G610_RCX, G610_OFF_I, lower);  // visible to this pair's upper
        // SQI must read the pre-upper VF. It cannot affect any upper input, so scheduling this
        // lower first is the exact same-cycle overlay without save/restore traffic.
        if (hazardStore && !g610EmitLower(e, lower, fp.pair.lowerKind, dataSize, &why))
            return false;
        if (!g610EmitUpper(e, upper, fp.pair.upperKind, j, usesHist, used, &why))
            return false;
        if (loi)
        {
            // The lower word is the immediate, so there is no lower instruction to emit.
        }
        else if (fp.pair.lowerKind == G422_LK_FCGET)
        {
            // ⭐ G651: `if (it != 0) st.vi[it] = (int32_t)(st.clip & 0xFFFu);`
            // Three instructions, and the transcription is the interpreter's own statement:
            //   mov eax, [rcx + OFF_CLIP]
            //   and eax, 0xFFF
            //   mov [rcx + OFF_VI + 4*it], eax        (raw: the value is 0..4095, so the int16
            //                                          truncation the other integer lowers apply
            //                                          would be the identity, and `Raw` is exact)
            // `it == 0` writes nothing, exactly as the interpreter's guard says.
            if (!g651RegionClipPairOk(slot))
                return false;                          // precondition 7: the emitter is the authority
            const unsigned it = LIT(lower);
            if (it != 0u)
            {
                e.movLd32(G610_RAX, G610_RCX, G610_OFF_CLIP);
                e.andImm(G610_RAX, 0x0FFF);
                e.storeViRaw(it);
            }
            ++s_g651ClipEmitted;
        }
        else if (!hazardStore && !g610EmitLower(e, lower, fp.pair.lowerKind, dataSize, &why))
            return false;
        g612EmitRepin(e, fp);
        return true;
    }

    bool g612EmitRegion(G612Plan &p, uint32_t dataSize, uint32_t codeSize, G612Region *out)
    {
        g612ArenaInit();
        if (s_g612.arena == nullptr)
        {
            ++s_g612RfWhy[G612_RF_ARENA];
            return false;
        }
        if (s_g612.used + G612_MAX_REGION_BYTES > G612_ARENA_BYTES)
            g612ArenaReset();

        G610Emit e;
        e.p = s_g612.arena + s_g612.used;
        e.cap = G612_MAX_REGION_BYTES;
        e.n = 0u;
        G612Emit x;
        x.e = &e;

        G612Fix fixes[G612_MAX_BB * 4u];
        uint32_t nFix = 0u;
        auto addFix = [&](size_t at, int32_t bb, uint32_t pc) {
            if (nFix < G612_MAX_BB * 4u)
            {
                fixes[nFix].at = at;
                fixes[nFix].bb = bb;
                fixes[nFix].pc = pc;
                ++nFix;
            }
            else
                e.bad = true;
        };

        // ---- the bodies ----
        for (uint32_t k = 0u; k < p.nBb && !e.bad; ++k)
        {
            G612Bb &bb = p.bb[k];
            bb.bodyOff = (uint32_t)e.n;

            // Budget: `lea rax, [r8 + pairs]; cmp rax, rsi; ja <exit at this block's own pc>`.
            // R8 already carries stampBase + consumed and RSI carries stampBase + budget, so this
            // ONE comparison is the whole `cycle + n <= maxCycles` test with no second counter.
            if (bb.pairs != 0u)
            {
                e.lea64(G610_RAX, G610_R8, (int32_t)bb.pairs);
                x.cmpR64(G610_RAX, G612_RSI);
                addFix(x.jccRel32(G612_CC_A), -2, bb.start * 8u);
            }

            for (uint32_t j = 0u; j < bb.plain && !e.bad; ++j)
                if (!g612EmitPair(e, bb.start + j, j, dataSize))
                {
                    ++s_g612RfWhy[G612_RF_MISMATCH];
                    return false;
                }
            if (e.bad)
                break;

            if (bb.term == G612_T_EXIT || bb.term == G612_T_FALL)
            {
                if (bb.plain != 0u)
                    x.addImm64(G610_R8, (int32_t)bb.plain);
                if (bb.term == G612_T_EXIT)
                    addFix(x.jmpRel32(), -2, (bb.start + bb.plain) * 8u);
                else
                {
                    const int dst = g612FindBb(p, bb.fallSlot);
                    if (dst < 0)
                        addFix(x.jmpRel32(), -2, bb.fallSlot * 8u);
                    else if ((uint32_t)dst != k + 1u)
                        addFix(x.jmpRel32(), dst, 0u);
                }
                continue;
            }

            // ---- the terminator pair ----
            const uint32_t bslot = bb.start + bb.plain;
            const G479FusedPair &bp = s_g410Cache.fused[bslot];
            const uint32_t bupper = (uint32_t)(bp.source >> 32);
            const uint32_t blower = (uint32_t)bp.source;
            const uint32_t bpc = bslot * 8u;
            {
                int why = 0;
                bool usesHist = false, used = false;
                if (!g610EmitUpper(e, bupper, bp.pair.upperKind, bb.plain, usesHist, used, &why))
                {
                    ++s_g612RfWhy[G612_RF_MISMATCH];
                    return false;
                }
            }

            if (bb.term == G612_T_BR)
                g612EmitCondition(e, x, bb.brKind, blower);
            else if (bb.term == G612_T_JR)
            {
                // `target = ((uint16_t)vi[is] * 8) & 0x3FFF`, carried across the delay slot in EDI.
                const unsigned is = LIS(blower);
                e.movzx16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
                e.shlImm(G610_RAX, 3u);
                e.andImm(G610_RAX, 0x3FFF);
                x.movR32(G612_RDI, G610_RAX);
            }
            // BAL / JALR link. `st.pc` is the branch pair's own pc at this point in the shipped
            // body, so `(st.pc + 16) / 8` is a translate-time constant. It is written BEFORE the
            // delay slot, exactly as the interpreter does, so a delay slot that names the same VI
            // register still wins.
            if (bb.brKind == G422_LK_BAL || bb.brKind == G422_LK_JALR)
            {
                const unsigned it = LIT(blower);
                if (it != 0u)
                    e.movStImm32(G610_RCX, G610_OFF_VI + (int32_t)(4u * it), (bpc + 16u) / 8u);
            }
            g612EmitRepin(e, bp);

            // ---- the DELAY SLOT, between the compare and the jump, exactly where the hardware
            // puts it. A plain eligible pair by construction (precondition 2). ----
            if (!g612EmitPair(e, bslot + 1u, (uint32_t)bb.plain + 1u, dataSize))
            {
                ++s_g612RfWhy[G612_RF_MISMATCH];
                return false;
            }
            x.addImm64(G610_R8, (int32_t)bb.pairs);

            if (bb.term == G612_T_JR)
            {
                // `taken = (target - 8 != branchPc)`; taken -> pc = target, else pc = branchPc + 16.
                x.cmpImm32(G612_RDI, (int32_t)(bpc + 8u));
                addFix(x.jccRel32(G612_CC_E), -2, bpc + 16u);
                e.movSt32(G612_RDI, G610_RCX, G612_OFF_PC);
                addFix(x.jmpRel32(), -1, 0u);           // straight to the shared epilogue
                continue;
            }
            if (bb.term == G612_T_JMP)
            {
                const int dst = g612FindBb(p, bb.takenSlot);
                if (dst < 0)
                    addFix(x.jmpRel32(), -2, bb.takenSlot * 8u);
                else if ((uint32_t)dst != k + 1u)
                    addFix(x.jmpRel32(), dst, 0u);
                continue;
            }
            // Conditional: `test dil, dil; jnz taken`, then fall through (or jump) to not-taken.
            x.testDil();
            {
                const int dst = g612FindBb(p, bb.takenSlot);
                const size_t at = x.jccRel32(G612_CC_NE);
                if (dst < 0)
                    addFix(at, -2, bb.takenSlot * 8u);
                else
                    addFix(at, dst, 0u);
            }
            {
                const int dst = g612FindBb(p, bb.fallSlot);
                if (dst < 0)
                    addFix(x.jmpRel32(), -2, bb.fallSlot * 8u);
                else if ((uint32_t)dst != k + 1u)
                    addFix(x.jmpRel32(), dst, 0u);
            }
        }
        if (e.bad)
        {
            ++s_g612RfWhy[G612_RF_ENCODE];
            return false;
        }

        // ---- the shared epilogue ----
        const uint32_t epilogueAt = (uint32_t)e.n;
        e.movSt32(G610_R10, G610_R9, G610_OFF_HHEAD);   // G504: stored once per invocation
        x.movR64(G610_RAX, G610_R8);
        x.subR64(G610_RAX, G612_RBP);                   // consumed = (stampBase + n) - stampBase
        x.popR(G612_RDI);
        x.popR(G612_RSI);
        x.popR(G612_RBP);
        e.ret();

        // ---- the exit stubs, one per distinct pc constant ----
        size_t stubOff[G612_MAX_BB * 4u];
        uint32_t stubPc[G612_MAX_BB * 4u];
        uint32_t nStub = 0u;
        for (uint32_t f = 0u; f < nFix && !e.bad; ++f)
        {
            if (fixes[f].bb != -2)
                continue;
            bool have = false;
            for (uint32_t s = 0u; s < nStub; ++s)
                if (stubPc[s] == fixes[f].pc)
                {
                    have = true;
                    break;
                }
            if (have)
                continue;
            if (nStub >= G612_MAX_BB * 4u)
            {
                e.bad = true;
                break;
            }
            stubPc[nStub] = fixes[f].pc;
            stubOff[nStub] = e.n;
            uint32_t pcw = fixes[f].pc;
            if (pcw >= codeSize)
                pcw = 0u;                               // the pair loop's own sequential wrap
            e.movStImm32(G610_RCX, G612_OFF_PC, pcw);
            const size_t at = x.jmpRel32();
            if (!g612Patch(e, at, (int64_t)epilogueAt))
            {
                e.bad = true;
                break;
            }
            ++nStub;
        }
        if (e.bad)
        {
            ++s_g612RfWhy[G612_RF_ENCODE];
            return false;
        }

        // ---- the entry thunks, emitted last so the hot bodies stay contiguous ----
        for (uint32_t k = 0u; k < p.nBb && !e.bad; ++k)
        {
            G612Bb &bb = p.bb[k];
            if (bb.pairs == 0u)
                continue;                               // a zero-pair block is never an entry
            bb.entryOff = (uint32_t)e.n;
            x.pushR(G612_RBP);
            x.pushR(G612_RSI);
            x.pushR(G612_RDI);
            // The 5th integer argument sits at [rsp+0x28] on entry; three pushes moved it by 24.
            x.movLd32Rsp(G612_RSI, 0x28 + 0x18);
            x.movR64(G612_RBP, G610_R8);                // the original stampBase
            x.addR64(G612_RSI, G610_R8);                // rsi = stampBase + budget
            e.movLd32(G610_R10, G610_R9, G610_OFF_HHEAD);
            e.movImm64(G610_R11, (uint64_t)(uintptr_t)&g610Consts);
            const size_t at = x.jmpRel32();
            if (!g612Patch(e, at, (int64_t)bb.bodyOff))
            {
                e.bad = true;
                break;
            }
        }
        if (e.bad)
        {
            ++s_g612RfWhy[G612_RF_ENCODE];
            return false;
        }

        // ---- fixups ----
        for (uint32_t f = 0u; f < nFix; ++f)
        {
            int64_t dst;
            if (fixes[f].bb == -1)
                dst = (int64_t)epilogueAt;
            else if (fixes[f].bb == -2)
            {
                int found = -1;
                for (uint32_t s = 0u; s < nStub; ++s)
                    if (stubPc[s] == fixes[f].pc)
                    {
                        found = (int)s;
                        break;
                    }
                if (found < 0)
                    return false;
                dst = (int64_t)stubOff[found];
            }
            else
                dst = (int64_t)p.bb[fixes[f].bb].bodyOff;
            if (!g612Patch(e, fixes[f].at, dst))
            {
                ++s_g612RfWhy[G612_RF_ENCODE];
                return false;
            }
        }

        // ---- publish ----
        out->nBb = p.nBb;
        // Does any pair anywhere in this region read `st.q`? Same test the G610 translator uses
        // per block, taken over the WHOLE region because any block of it can be re-entered.
        out->readsQ = 0u;
        for (uint32_t k = 0u; k < p.nBb && out->readsQ == 0u; ++k)
            for (uint32_t j = 0u; j < p.bb[k].pairs; ++j)
            {
                const uint32_t sl = p.bb[k].start + j;
                if (sl >= G612_SLOTS)
                    break;
                const uint8_t uk = s_g410Cache.fused[sl].pair.upperKind;
                if (uk == 0x6fu)
                    continue;
                const G421UpperDesc d = g421DescTableConst.e[uk];
                if (d.cls != 0u && d.cls < 3u && d.src == 1u)
                {
                    out->readsQ = 1u;
                    break;
                }
            }
        for (uint32_t k = 0u; k < p.nBb; ++k)
        {
            out->bbSlot[k] = p.bb[k].start;
            out->bbPairs[k] = p.bb[k].pairs;
            out->bbFn[k] = (p.bb[k].entryOff == 0xFFFFFFFFu)
                               ? nullptr
                               : (G612Fn)(void *)(e.p + p.bb[k].entryOff);
        }
        s_g612.used += (e.n + 15u) & ~(size_t)15u;
        s_g612CodeBytes += e.n;
        s_g612Bbs += p.nBb;
        ++s_g612Compiled;
#if defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), e.p, e.n);
#endif
        return true;
    }

    // -------------------------------------------------------------------------------------------
    // §7 THE MEMOS AND THE COLD REBUILD PASS
    //
    // DC2 re-uploads VU1 microcode ~2.6 times per FRAME — `[G610:cache] rebuilds = 42,059` on the
    // measured route — so doing ANY per-slot work on every rebuild is the first thing that has to
    // be priced. G610 pays 2,048 hash-and-probe operations per rebuild. A CFG walk is far dearer
    // than a hash, so G612 memoises one level up:
    //
    //   * the WHOLE-IMAGE memo. One FNV-64 over all 2,048 source words per rebuild; on a hit the
    //     entire slot table is restored with one copy and not a single CFG is walked. With ~3 live
    //     images and a ring of 4 this is what runs on essentially every rebuild.
    //   * the REGION memo, keyed on the region's complete description (every block's
    //     start/plain/terminator/target and the exact 64-bit source word of every pair it emits —
    //     precisely the set of inputs the emitted bytes are a function of). This is what stops a
    //     genuinely new image from re-emitting the regions it shares with an old one.
    // -------------------------------------------------------------------------------------------
    uint64_t g612ImageKey()
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0u; i < G612_SLOTS; ++i)
            h = (h ^ s_g410Cache.sources[i]) * 0x100000001b3ull;
        return h ? h : 1ull;
    }

    uint64_t g612PlanKey(const G612Plan &p)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
        mix((uint64_t)p.nBb);
        for (uint32_t k = 0u; k < p.nBb; ++k)
        {
            const G612Bb &bb = p.bb[k];
            mix(bb.start);
            mix((uint64_t)bb.plain | ((uint64_t)bb.pairs << 16) | ((uint64_t)bb.term << 32) |
                ((uint64_t)bb.brKind << 40));
            mix((uint64_t)bb.takenSlot | ((uint64_t)bb.fallSlot << 32));
            for (uint32_t j = 0u; j < bb.pairs; ++j)
                if (bb.start + j < G612_SLOTS)
                    mix(s_g410Cache.fused[bb.start + j].source);
        }
        return h ? h : 1ull;
    }

    void g612Bind(const G612Region &r)
    {
        for (uint32_t k = 0u; k < r.nBb; ++k)
        {
            if (r.bbFn[k] == nullptr || r.bbSlot[k] >= G612_SLOTS)
                continue;
            G612SlotRec &s = s_g612.slot[r.bbSlot[k]];
            if (s.fn != nullptr)
                continue;                              // first region to reach a slot owns it
            s.fn = r.bbFn[k];
            s.pairs = r.bbPairs[k];
            s.readsQ = r.readsQ;
            ++s_g612Bound;
        }
    }

    // Every slot any emitted region EXECUTES, whether or not it is a block leader. Without this the
    // pass starts a fresh region at slot i, then another at i+1, i+2, … all the way through the
    // same loop body — 1,404 emission attempts and 1,308 table refusals on the first bring-up run
    // for what is really a few dozen distinct control-flow graphs.
    thread_local std::array<uint8_t, G612_SLOTS> s_g612Claim{};

    void g612Claim(const G612Plan &p)
    {
        for (uint32_t k = 0u; k < p.nBb; ++k)
            for (uint32_t j = 0u; j < p.bb[k].pairs; ++j)
                if (p.bb[k].start + j < G612_SLOTS)
                    s_g612Claim[p.bb[k].start + j] = 1u;
    }

    void g612CompileAll(uint32_t dataSize, uint32_t codeSize)
    {
        if (!g612BackendEnabled())
            return;
        ++s_g612Rebuilds;
        s_g612.dataSize = dataSize;
        s_g612.codeSize = codeSize;

        const uint64_t img = g612ImageKey();
        for (uint32_t r = 0u; r < G612_IMAGE_RING; ++r)
            if (s_g612.imgKey[r] == img)
            {
                ++s_g612ImgHit;
                s_g612.slot = s_g612.imgSlot[r];
                return;
            }
        ++s_g612ImgMiss;
        for (auto &s : s_g612.slot)
            s = G612SlotRec{};

        G612Plan &s_plan = s_g612Plan;
        s_g612Claim.fill(0u);
        for (uint32_t i = 0u; i < G612_SLOTS; ++i)
        {
            if (s_g612.slot[i].fn != nullptr || s_g612Claim[i] != 0u)
                continue;                              // already inside an earlier region
            // G652 P13: dynamic JR/JALR exits commonly land on one plain pair followed by an
            // already-supported IBEQ/IBNE tail. The old two-pair block threshold left those
            // fragments permanently unbound. Let the CFG planner price all emittable starts;
            // its existing `anyBranch` test rejects truly trivial one-pair regions before emit.
            if (s_g410Cache.g490RunLen[i] < G612_MIN_ENTRY && !g652VuShortEntryOn())
                continue;
            if (!g612PlainEmittable(i))
                continue;
            if (!g612Discover(s_plan, i))
            {
                ++s_g612RfWhy[G612_RF_CAPS];
                continue;
            }
            if (!s_plan.anyBranch)
            {
                // No control transfer anywhere in the region: G610's block already runs this, and a
                // region would only add a prologue. Refuse, so the two backends never race for the
                // same straight-line population.
                ++s_g612RfWhy[G612_RF_TRIVIAL];
                continue;
            }
            if (!g612Layout(s_plan))
            {
                ++s_g612RfWhy[G612_RF_CAPS];
                continue;
            }
            // ⚠️ NOT `bb[0].start == i`. `g612Layout` SORTS the leaders into ascending slot order
            // so a fall-through usually needs no jump, and any region whose branch targets a lower
            // slot therefore has some other block at index 0. The first bring-up run refused 518
            // regions on exactly that mistake — an entry test that reads a LAYOUT artefact instead
            // of asking whether the entry slot has a block at all.
            {
                const int e0 = g612FindBb(s_plan, i);
                if (e0 < 0 || s_plan.bb[e0].pairs == 0u)
                {
                    ++s_g612RfWhy[G612_RF_ENTRY];
                    continue;
                }
            }

            const uint64_t key = g612PlanKey(s_plan);
            int hit = -1;
            for (uint32_t r = 0u; r < s_g612.nRegion; ++r)
                if (s_g612.region[r].key == key)
                {
                    hit = (int)r;
                    break;
                }
            if (hit >= 0)
            {
                ++s_g612CacheHit;
                g612Bind(s_g612.region[hit]);
                g612Claim(s_plan);
                continue;
            }
            ++s_g612CacheMiss;
            if (s_g612.nRegion >= G612_MAX_REGIONS)
            {
                ++s_g612RfWhy[G612_RF_TABLE];
                continue;
            }
            // ⚠️ EMIT INTO A LOCAL, NOT INTO THE TABLE SLOT. `g612EmitRegion` may reset the arena,
            // which zeroes `region[]` and `nRegion` — a reference taken before the call would then
            // name a cleared entry at a stale index and the region would be published into the
            // wrong place. This is the same class of mistake as G611's `allowReset` note: never
            // hold a pointer into a table across a call that may rebuild it.
            static thread_local G612Region s_tmp;
            s_tmp = G612Region{};
            if (!g612EmitRegion(s_plan, dataSize, codeSize, &s_tmp))
                continue;
            if (s_g612.nRegion < G612_MAX_REGIONS)
            {
                s_tmp.key = key;
                s_g612.region[s_g612.nRegion] = s_tmp;
                ++s_g612.nRegion;
            }
            else
                ++s_g612RfWhy[G612_RF_TABLE];
            g612Bind(s_tmp);
            g612Claim(s_plan);
        }

        s_g612.imgKey[s_g612.imgNext] = img;
        s_g612.imgSlot[s_g612.imgNext] = s_g612.slot;
        s_g612.imgNext = (s_g612.imgNext + 1u) % G612_IMAGE_RING;
    }

    // -------------------------------------------------------------------------------------------
    // §8 REPORTING
    // -------------------------------------------------------------------------------------------
    void g612Report()
    {
        if (!g612StatOn())
            return;
        std::fprintf(stderr,
                     "[G612:reg] compiled=%llu bbs=%llu codeKiB=%.1f bound=%llu live=%u | "
                     "runs=%llu pairs=%llu avg=%.2f max=%llu\n",
                     (unsigned long long)s_g612Compiled, (unsigned long long)s_g612Bbs,
                     (double)s_g612CodeBytes / 1024.0, (unsigned long long)s_g612Bound,
                     s_g612.nRegion,
                     (unsigned long long)s_g612Runs, (unsigned long long)s_g612Pairs,
                     s_g612Runs ? (double)s_g612Pairs / (double)s_g612Runs : 0.0,
                     (unsigned long long)s_g612MaxPairs);
        std::fprintf(stderr,
                     "[G612:why] skipQp=%llu skipBudget=%llu skipSize=%llu rebuilds=%llu "
                     "imgHit=%llu imgMiss=%llu regHit=%llu regMiss=%llu arenaResets=%llu |",
                     (unsigned long long)s_g612SkipQp, (unsigned long long)s_g612SkipBudget,
                     (unsigned long long)s_g612SkipSize, (unsigned long long)s_g612Rebuilds,
                     (unsigned long long)s_g612ImgHit, (unsigned long long)s_g612ImgMiss,
                     (unsigned long long)s_g612CacheHit, (unsigned long long)s_g612CacheMiss,
                     (unsigned long long)s_g612ArenaResets);
        for (int k = 0; k < G612_RF_COUNT; ++k)
            std::fprintf(stderr, " %s=%llu", kG612RfName[k],
                         (unsigned long long)s_g612RfWhy[k]);
        std::fprintf(stderr, "\n");
        uint32_t topIdx[8] = {};
        uint32_t topVal[8] = {};
        for (uint32_t i = 0u; i < G612_SLOTS; ++i)
        {
            const uint32_t v = s_g612ExitAt[i];
            if (v == 0u)
                continue;
            for (int s = 0; s < 8; ++s)
                if (v > topVal[s])
                {
                    for (int t = 7; t > s; --t)
                    {
                        topVal[t] = topVal[t - 1];
                        topIdx[t] = topIdx[t - 1];
                    }
                    topVal[s] = v;
                    topIdx[s] = i;
                    break;
                }
        }
        std::fprintf(stderr, "[G612:exitslot]");
        for (int s = 0; s < 8; ++s)
            if (topVal[s] != 0u)
                std::fprintf(stderr, " 0x%04x:%u(i=%u,e=%u)", topIdx[s] * 8u, topVal[s],
                             s_g652ExitIAt[topIdx[s]], s_g652ExitEAt[topIdx[s]]);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[G652:fmand]");
        for (int s = 0; s < 8; ++s)
            if (topVal[s] != 0u &&
                (s_g652FmandPendingAt[topIdx[s]] != 0u ||
                 s_g652FmandMaterialAt[topIdx[s]] != 0u))
                std::fprintf(stderr, " 0x%04x:pending=%u,material=%u", topIdx[s] * 8u,
                             s_g652FmandPendingAt[topIdx[s]],
                             s_g652FmandMaterialAt[topIdx[s]]);
        std::fprintf(stderr, "\n");
        uint32_t idTop[12] = {};
        uint64_t idVal[12] = {};
        for (uint32_t i = 0u; i < G652_EXIT_ID_CAP; ++i)
        {
            const uint64_t v = s_g652ExitIdentity[i].count;
            if (v == 0u)
                continue;
            for (int s = 0; s < 12; ++s)
                if (v > idVal[s])
                {
                    for (int t = 11; t > s; --t)
                    {
                        idVal[t] = idVal[t - 1];
                        idTop[t] = idTop[t - 1];
                    }
                    idVal[s] = v;
                    idTop[s] = i;
                    break;
                }
        }
        std::fprintf(stderr, "[G652:exitid] dropped=%llu",
                     (unsigned long long)s_g652ExitIdentityDropped);
        for (int s = 0; s < 12; ++s)
            if (idVal[s] != 0u)
            {
                const G652ExitIdentity &r = s_g652ExitIdentity[idTop[s]];
                std::fprintf(stderr,
                             " 0x%04x/%016llx:%llu(lk=%u,uk=0x%02x,pf=0x%x,g425=0x%x,"
                             "tail=%u,i=%llu,e=%llu,fp=%llu,fm=%llu)",
                             (unsigned)r.slot * 8u, (unsigned long long)r.source,
                             (unsigned long long)r.count, (unsigned)r.lowerKind,
                             (unsigned)r.upperKind, (unsigned)r.pairFlags,
                             (unsigned)r.g425Flags, (unsigned)r.tail,
                             (unsigned long long)r.iCount, (unsigned long long)r.eCount,
                             (unsigned long long)r.fmandPending,
                             (unsigned long long)r.fmandMaterial);
            }
        std::fprintf(stderr, "\n");
        // ⭐ G651: how many FCGET pairs the emitter actually placed inside region bodies. `on=0`
        // with a non-zero `[G612:exit] lk=37` row means the arm is disarmed, not that the lever
        // failed to find work.
        std::fprintf(stderr, "[G651:vuclip] on=%u fcgetEmitted=%llu\n",
                     g651VuClipOn() ? 1u : 0u, (unsigned long long)s_g651ClipEmitted);
        std::fflush(stderr);
    }
} // namespace

#endif // DC2_G612_REGION_BACKEND && DC2_G610_JIT_BACKEND
