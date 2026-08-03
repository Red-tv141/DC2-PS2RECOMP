// G490 - WIDENED BLOCK RUN EXECUTOR, on the PROMOTED (fixed + fused + wide-VF0) loop body.
//
// WHY THIS EXISTS, and why it is not G475.
//
// G489's `/MAP` + PC-sample overlay measured the pair loop at ~63 cycles / ~75 retired
// instructions per pair, of which **54.7% is the per-pair ENVELOPE** (pc-oob test, index math,
// record load, upper/lower split, flag extract, sequential-PC / branch fold / pc store / ebit /
// loop condition) and 20.7% is field extraction from words that never change for a given PC.
// 60 FPS needs 1.55x on this thread. The structural answer is to pay the envelope ONCE PER RUN
// of covered pairs instead of once per pair — which is the premise the whole block-translation
// arc rests on. This file prices that premise at the SOURCE level before anything is emitted.
//
// G475 already built a run executor and G478 measured it +0.34 ms/f SLOWER, which the ROADMAP
// asked G490 to understand first. Two things were found:
//
//   1. **The G475 trace copy is STRUCTURALLY UNREACHABLE on the current binary.** Its two
//      selector arms in vu1_upper_opcodes.inc sit BELOW `else if (g443Fixed && g479LeanFetch)`,
//      and `g479LeanFetch` has been default-ON since G479 was promoted. Arming
//      `DC2_G419_AB=vnoptrace` today cannot put the trace into the promoted loop; it can only
//      drop the whole run onto a different, slower copy. This is exactly the G482/G484
//      "structurally unmeasurable lever" class, one arc later.
//   2. Therefore G478's `+0.34` describes a PRE-G479, PRE-G485 body: a non-fused split fetch and
//      four narrow VF0 stores, in both arms. It is not evidence about today's loop.
//
// So this is the re-price the ROADMAP asked for, done the only way it can be done — as a copy of
// the SHIPPED body (DC2_G484_WIDEVF0 1 + DC2_G479_FUSED 1) with the run hook added, so the
// control arm is the promoted default and nothing else differs.
//
// WHAT IS WIDER THAN G475. G475 covered `upperKind == 0x6f` (VNOP) only — 34.5% of MAP-0 pairs,
// and the half of the stream that carries no upper work at all, i.e. the half where the envelope
// is the SMALLEST fraction of the pair. G490 covers **every upper class `g421FastUpper` executes**
// (all `cls != 0`), because the run body can call the same inlined SIMD upper with the same G430
// stamped-history push that the pair loop passes it. That is the class a block translator would
// emit, so this measures the arc's real premise instead of its easiest corner.
//
// EXACTNESS — what the elision rests on. A pair is eligible only when the per-pair machinery this
// loop performs around the two slots is PROVABLY INERT for it (g490BlockPairEligible):
//   * `pairFlags == 0`     -> no E-bit (no latch, no drain) and no I-bit (the lower word is not a
//                             float immediate), so the LOI arm and the ebit test are no-ops.
//   * `!G426_PF_PRESTALL`  -> the G239 scalar-prestall block is guarded by this bit; clear means
//                             the lower is neither a FDIV/EFU producer nor WAITQ/WAITP.
//   * `!G427_PF_FLAGREAD`  -> under the promoted `g430Hist` model the MAC/STATUS line is NOT
//                             advanced per pair; the only per-pair work is `g430ResolveAt` for a
//                             consumer pair. Clear means there is none.
//   * `!G426_PF_HAZ`       -> the G139 pair-hazard save/restore/overlay is guarded by this bit.
//   * lower is a covered, NON-CONTROL, NON-FLAG kind -> `g422FastLower` executes it, and G489
//                             established from the complete `m_state.pc =` set in the source that
//                             only the ten branch/jump lowers can write pc. So `postExecPc ==
//                             currentPc`, the branch fold is dead, and the PC is sequential.
//   * upper is VNOP (the loop skips it) or `cls != 0` (`g421FastUpper` returns true).
// What remains per pair is then exactly: Q advance, P advance, upper, lower, VF0/VI0 re-pin,
// pc += 8 — which is what the block below executes, in that order.
//
// Verify: DC2_G490_VERIFY=1 re-derives every one of those predicates from the LIVE code words for
//         every pair of every executed block and aborts on the first disagreement (it also
//         re-reads the cached source, so a missed invalidation inside a block cannot hide).
// Stats:  DC2_G490_STAT=1 -> `[G490:block] runs= pairs= avg= cover=`.
// Arm:    DC2_G419_AB=blockrun (paired, hold 16 — it is a cache-decode arm) or DC2_G490_BLOCK=1.
extern int g490BlockRunArm();

namespace
{
    // Minimum run length worth the call boundary. 2 mirrors G475 so the two are comparable.
    constexpr uint32_t G490_MIN_RUN = 2u;

    bool g490BlockRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blockrun") == 0;
        }();
        if (s_paired)
            return g490BlockRunArm() > 0;
        // PROMOTED default-on (G490): mean -0.560 ms/f raw / -0.523 blocked over four paired
        // A/Bs, sign 4/4, blocked t -2.0..-4.8, both arms confirmed built on every run.
        // Rollback: DC2_G490_NO_BLOCK=1 restores the G485 promoted copy exactly.
        static const bool s_on = !f50_12_vu_env_flag("DC2_G490_NO_BLOCK");
        return s_on;
    }

    bool g490VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_VERIFY");
        return s_on;
    }

    bool g490StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_STAT");
        return s_on;
    }

    // ---------------------------------------------------------------------------------------
    // Eligibility + the per-slot maximal run length, maintained beside G410's precisely
    // invalidated pair cache (same contract as G475's table: rebuilt on a full cache build and on
    // every MPG invalidation, so it can never describe a stale encoding).
    // ---------------------------------------------------------------------------------------
    bool g490BlockPairEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;                                   // E-bit or LOI
        if (pair.lowerKind == G422_LK_NONE)
            return false;                                   // needs the legacy execLower
        if (pair.lowerKind >= G422_LK_B && pair.lowerKind <= G422_LK_IBGEZ)
            return false;                                   // branch / jump: writes pc
        if (pair.lowerKind >= G422_LK_FIRST_FLAG)
            return false;                                   // reads the MAC/STATUS line
        if ((pair.g425Flags &
             (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u)
            return false;                                   // per-pair machinery is live
        if (pair.upperKind == 0x6fu)
            return true;                                    // VNOP: the loop skips the upper
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    void g490RebuildRunLengths()
    {
        uint32_t run = 0u;
        for (size_t i = s_g410Cache.uppers.size(); i-- > 0u;)
        {
            if (g490BlockPairEligible(s_g410Cache.uppers[i]))
            {
                if (run < 255u)
                    ++run;
                s_g410Cache.g490RunLen[i] = static_cast<uint8_t>(run);
            }
            else
            {
                run = 0u;
                s_g410Cache.g490RunLen[i] = 0u;
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Stats. Out of line and behind a cached bool, so the promoted copy carries one test and a
    // never-taken call (G421 law 17: never inline a census into this loop).
    // ---------------------------------------------------------------------------------------
    thread_local uint64_t s_g490Runs = 0u;
    thread_local uint64_t s_g490Pairs = 0u;
    thread_local uint64_t s_g490TotalPairs = 0u;
    thread_local uint64_t s_g490LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490NoteBlock(uint32_t count)
    {
        ++s_g490Runs;
        s_g490Pairs += count;
        if (s_g490Pairs - s_g490LastReport >= 50000000u)
        {
            s_g490LastReport = s_g490Pairs;
            std::fprintf(stderr,
                         "[G490:block] runs=%llu pairs=%llu avg=%.2f cover=%.2f%%\n",
                         static_cast<unsigned long long>(s_g490Runs),
                         static_cast<unsigned long long>(s_g490Pairs),
                         static_cast<double>(s_g490Pairs) /
                             static_cast<double>(s_g490Runs ? s_g490Runs : 1u),
                         s_g490TotalPairs
                             ? 100.0 * static_cast<double>(s_g490Pairs) /
                                   static_cast<double>(s_g490TotalPairs)
                             : 0.0);
        }
    }

    // Called ONCE per run() (~1,230/frame), never per pair, so `cover=` is exact without putting
    // a counter in the loop.
    void g490NoteTotalPairs(uint32_t cycles)
    {
        s_g490TotalPairs += cycles;
    }

    // ---------------------------------------------------------------------------------------
    // Oracle. Re-derives, from the LIVE code words, every predicate the elision rests on. This is
    // the risk that is actually new in G490 (a wrong eligibility decode silently drops a hazard,
    // a prestall or a flag read); the arithmetic of the two slots is already covered by G421's
    // and G422's own shadow oracles, and the compact-loop SHAPE by G475's 40 M-pair state oracle.
    // ---------------------------------------------------------------------------------------
    std::atomic<uint64_t> s_g490Verified{0u};

    [[noreturn]] void g490VerifyFail(const char *what, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::fprintf(stderr,
                     "[G490:verify-BAD] %s pc=0x%x lower=%08x upper=%08x\n",
                     what, pc, lower, upper);
        std::fflush(stderr);
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490VerifyBlock(const uint8_t *vuCode, uint32_t firstIndex, uint32_t count)
    {
        for (uint32_t j = 0u; j < count; ++j)
        {
            const size_t index = static_cast<size_t>(firstIndex + j);
            const uint32_t pc = static_cast<uint32_t>(index * 8u);
            uint64_t live = 0u;
            std::memcpy(&live, vuCode + pc, sizeof(live));
            const G410CompiledUpper &p = s_g410Cache.uppers[index];
            if (live != s_g410Cache.sources[index] || live != s_g410Cache.fused[index].source)
                g490VerifyFail("stale-source", pc, (uint32_t)live, (uint32_t)(live >> 32));

            const uint32_t lower = static_cast<uint32_t>(live);
            const uint32_t upper = static_cast<uint32_t>(live >> 32);

            // E-bit / I-bit straight off the raw upper word.
            if (((upper >> 30u) & 1u) != 0u || ((upper >> 31u) & 1u) != 0u)
                g490VerifyFail("ei-bit", pc, lower, upper);
            // G239 scalar prestall.
            const uint8_t liveS2 =
                ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
            if (g426LowerIsScalarPipe(liveS2))
                g490VerifyFail("prestall", pc, lower, upper);
            // G427 MAC/STATUS consumer.
            if (g427LowerReadsFlags(lower))
                g490VerifyFail("flagread", pc, lower, upper);
            // G489: only the ten branch/jump lowers write pc.
            if (g489LowerWritesPc(lower))
                g490VerifyFail("writes-pc", pc, lower, upper);
            // G139 pair hazard, re-derived exactly as the G426 predecode verifier does.
            uint32_t liveIdx = 0xffffffffu;
            const uint8_t liveOp = static_cast<uint8_t>(upper & 0x3Fu);
            if (liveOp < 0x3Cu)
                liveIdx = (upper >> 6) & 0x1Fu;
            else
            {
                const uint8_t liveS2u = S2_FUNC(upper);
                if ((liveS2u >= 0x10u && liveS2u <= 0x17u) || liveS2u == 0x1Du)
                    liveIdx = (upper >> 16) & 0x1Fu;
            }
            if (liveIdx != 0xffffffffu &&
                ((((lower >> 11u) & 0x1Fu) == liveIdx) || (((lower >> 16u) & 0x1Fu) == liveIdx)))
                g490VerifyFail("pairhaz", pc, lower, upper);
            // And the cached flags this block's eligibility was actually taken from.
            if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u ||
                p.pairFlags != 0u || !g490BlockPairEligible(p))
                g490VerifyFail("cached-flags", pc, lower, upper);
        }
        const uint64_t old = s_g490Verified.fetch_add(count, std::memory_order_relaxed);
        if (old == 0u || old / 100000000u != (old + count) / 100000000u)
            std::fprintf(stderr, "[G490:verify] pairs=%llu bad=0\n",
                         static_cast<unsigned long long>(old + count));
    }

    // ---------------------------------------------------------------------------------------
    // THE BLOCK. `noinline` on purpose: G481 measured this loop's budget binding on `run()`'s
    // instruction FOOTPRINT, so a second inlined copy of the SIMD upper + fast lower inside
    // run() would be paid by every pair to help a subset. Non-volatile registers survive the
    // call, and the hook sits at the TOP of the iteration where no pair-local value is live yet,
    // so the boundary costs the argument setup and nothing that the pair body holds.
    //
    // Order is the pair loop's order, statement for statement, for the eligible class:
    //   Q advance -> P advance -> upper -> lower -> VF0/VI0 re-pin.
    // ---------------------------------------------------------------------------------------
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g490ExecuteBlock(VU1State &st, uint32_t firstIndex, uint32_t count,
                          uint8_t *vuData, uint32_t dataSize,
                          int *qDelay, int *pDelay,
                          int64_t stampBase, G430HistBlock *hb)
    {
        const G479FusedPair *const fused = s_g410Cache.fused.data();
        for (uint32_t j = 0u; j < count; ++j)
        {
            const G479FusedPair &fp = fused[firstIndex + j];
            const uint64_t src = fp.source;
            const uint32_t lower = static_cast<uint32_t>(src);
            const uint32_t upper = static_cast<uint32_t>(src >> 32);

            if (*qDelay > 0 && --(*qDelay) == 0)
                st.q = s_vuQPending;
            if (*pDelay > 0 && --(*pDelay) == 0)
                st.p = s_vuPPending;

            // `g443Fixed` pins the four trailing arguments the pair loop passes: g428DeferPark
            // true, shadow false, g430Hist true, and the hoisted history block. Eligibility
            // guarantees `cls != 0`, so the return value is always true and the legacy
            // execUpper fallback the macro carries is unreachable here.
            if (fp.pair.upperKind != 0x6fu)
                g421FastUpper(st, upper, fp.pair.upperKind, true, false, true,
                              stampBase + static_cast<int64_t>(j), hb);

            g422FastLower(st, lower, fp.pair.lowerKind, vuData, dataSize);

            if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                // G484's promoted wide form, byte-for-byte with the pair loop's re-pin.
                _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                st.vi[0] = 0;
            }
        }
    }

    // One-shot proof of selection (G457): a copy that was never selected proves nothing, and a
    // run-length table that covers nothing is a null with no information in it.
    void g490ReportArmed(bool armed)
    {
        static const bool s_show = g490StatOn() || (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_show)
            return;
        static std::atomic<uint32_t> s_once[2] = {};
        const int slot = armed ? 1 : 0;
        if (s_once[slot].fetch_add(1u, std::memory_order_relaxed) != 0u)
            return;
        uint32_t eligible = 0u;
        uint32_t runs = 0u;
        for (size_t i = 0u; i < s_g410Cache.g490RunLen.size(); ++i)
        {
            if (s_g410Cache.g490RunLen[i] != 0u)
                ++eligible;
            if (s_g410Cache.g490RunLen[i] >= G490_MIN_RUN &&
                (i == 0u || s_g410Cache.g490RunLen[i - 1u] == 0u))
                ++runs;
        }
        std::fprintf(stderr,
                     "[G490:armed] block=%d eligibleSlots=%u (%.2f%%) runStarts=%u\n",
                     armed ? 1 : 0, eligible,
                     100.0 * static_cast<double>(eligible) /
                         static_cast<double>(s_g410Cache.g490RunLen.size()),
                     runs);
        std::fflush(stderr);
    }
} // namespace
