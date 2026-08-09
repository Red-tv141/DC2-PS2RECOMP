// G421 - VU1 hot-loop dynamic census (diagnostic only, default OFF).
//
// The G421 premise profile put the VU1 worker at ~98% of the lean MAP-0 frame
// (busyMs/f 65.2 vs frame 66.2), so the interpreter loop is the pole. Before
// designing any lever this census answers the two questions the premise gate
// demands: how many instruction pairs run per frame (=> ns/pair), and which
// upper/lower operations those pairs actually are.
//
// Arm:   DC2_G421_CENSUS=1
// Cost:  a handful of thread-local increments per executed pair, ALL behind one
//        cached master bool, so the default path is untouched. The absolute
//        frame under this census is inflated; use it for RELATIVE mix only
//        (G312 self-cost rule).
namespace
{
    struct G421Census
    {
        uint64_t runs = 0ull;
        uint64_t cycles = 0ull;
        uint64_t upperExec = 0ull;   // execUpper() actually called
        uint64_t upperVnop = 0ull;   // G410 compiled VNOP, call skipped
        uint64_t iBits = 0ull;       // LOI pairs (lower word is a float immediate)
        uint64_t hazSave = 0ull;     // G139 pair-hazard save/restore round-trips taken
        uint64_t lowerExec = 0ull;   // execLower() calls that were not the early-out NOP
        uint64_t lowerNop = 0ull;
        // Dynamic op mix. Index space matches G410's upperKind: 0x00-0x2f = main
        // upper opcode, 0x40+func = Special2 upper, 0x7f = decoder fallback.
        uint64_t upperKind[0x80] = {};
        uint64_t lowerOpHi[0x80] = {};  // (lower >> 25) & 0x7f
        uint64_t lowerS2[0x80] = {};    // S2_FUNC(lower) when opHi == 0x40
        // G422: executed-lower coverage, derived from the SAME decoder the G422 lever
        // dispatches on, so the two cannot drift apart.
        // G487: FIXED capacity, deliberately decoupled from G422_LK_COUNT. Sizing it by the enum
        // meant that extending the covered set RESIZED this thread_local member and shifted every
        // thread_local declared after it — the exact data-placement hazard G486 §5 recorded (G437
        // priced one such shift at 3.48 ms/f). 64 slots is paid once, here, and no future coverage
        // phase has to touch the layout again.
        static constexpr size_t kLowerKindCap = 64u;
        static_assert(G422_LK_COUNT <= kLowerKindCap, "G487: raise kLowerKindCap");
        uint64_t lowerKind[kLowerKindCap] = {};
        // G486: WHICH lower opcodes are the uncovered 4.61% of pairs. `[G422:cover]` has read
        // "uncovered=7.70% of executed lowers" for four phases without ever naming them, and
        // `[G421:mix] lowerS2` cannot: its index is S2_FUNC, which for a top-level Special2 op
        // (IADD/IAND/IOR...) is a function of bits 6-10 and 0-1 only, so covered and uncovered
        // encodings collide in the same bucket. This histogram is keyed on the decision the G422
        // decoder actually makes, so an entry here is by construction an `execLower()` call:
        //   index = (opHi == 0x40) ? (0x80 | S2_FUNC(lower)) : opHi
        // Every one of these pairs pays a call into the full ~1400-line execLower switch instead
        // of the compact g422FastLower one. Extending coverage is an OUT-OF-LINE change, so it
        // cannot hit the G481 inline-budget cliff — but only if the mass is in ops that are cheap
        // and side-effect-free (an XGKICK or a DIV carries its own machinery and moving it buys
        // nothing). Price the population before building anything (G476).
        //
        // NOTE the storage: the two counters live OUTSIDE this struct and are NOT thread_local
        // (`s_g486UncovHist` / `s_g486UncovTotal` below). `G421Census` is a thread_local object,
        // and adding 2 KB to it shifts every thread_local declared after it — a data-placement
        // change on the pole's own TLS block, which G437 priced at 3.48 ms/f for one alignment.
        // A default-OFF diagnostic must never be able to move the default path.
        // G425: consecutive "zero-work pair" runs. A zero-work pair is VNOP upper + NOP lower
        // with neither the E nor the I bit, i.e. a pair whose only effect is to advance the
        // Q/P delays, the MAC/STATUS delay line and the PC. Bucketed by run length so a
        // run-collapsing lever can be SIZED before it is built (G420 ceiling rule).
        uint64_t zeroPairs = 0ull;                 // total zero-work pairs seen
        uint64_t zeroRuns = 0ull;                  // number of maximal runs
        uint64_t zeroLenHist[9] = {};              // 1..7 exact, [8] = >=8
        uint64_t zeroPairsInRunsGE2 = 0ull;        // pairs collapsible by a >=2 run lever
        uint32_t zeroCur = 0u;                     // current run length (in flight)
        // G428: SIMD-op accounting for the fast-upper path. The pair loop is throughput-bound
        // on SSE2 ops (G426) and scalar bookkeeping is a closed vein (G427), so the only
        // remaining question is where the SIMD ops go. Two shares decide it:
        //   * destHist  - the dest mask of every FMAC that runs inline. dest==0xF lets the
        //                 masked store (load+and+andnot+or+store) collapse to one store.
        //   * fmacExec  - FMAC uppers, i.e. the ops that pay the ~10-op G329/G330 classifier.
        //   * flagRead  - executed lower slots that actually CONSUME MAC/STATUS. If this is 0
        //                 the classifier's product is dead except for the architectural tail.
        uint64_t destHist[16] = {};
        uint64_t fmacExec = 0ull;     // covered upper with cls 1 or 2 (writes MAC/STATUS)
        uint64_t nonFmacCovered = 0ull; // covered upper with cls >= 3 (no MAC/STATUS)
        uint64_t flagReadExec = 0ull; // executed lower reads s_vuMacVisible/s_vuStatusVisible
        uint64_t statusReadExec = 0ull; // subset: FSEQ/FSAND/FSOR (the STICKY consumers)
        // G475: runway for a compact VNOP-upper + covered non-control lower trace. Unlike the
        // older zero-work census, this admits useful lower-slot work; unlike a source scan, it
        // follows the actual dynamic PC stream and rejects branch-delay/latched-E entries.
        uint64_t g475Pairs = 0ull;
        uint64_t g475Runs = 0ull;
        uint64_t g475PairsInRunsGE2 = 0ull;
        uint64_t g475LenHist[9] = {}; // 1..7 exact, [8] = >=8
        uint32_t g475Cur = 0u;
        uint32_t g475PrevPc = 0xffffffffu;
        // G479: WHERE the pairs are, in microprogram space. Every closed vein on this thread
        // (G426/G435/G436/G442/G475) made a pair cheaper; the only class left is executing
        // FEWER pairs, and that decision needs the dynamic PC distribution, not the opcode mix.
        // 2048 slots = the whole VU1 code store at one entry per instruction pair.
        uint64_t pcHist[PS2_VU1_CODE_SIZE / 8u] = {};
        // G479: pairs per run() call, bucketed by power of two. `cyc/run` is a mean over a
        // bimodal population (a transform batch and a short setup/resume kick look nothing
        // alike), so the mean alone cannot say whether the mass is in many small kicks or a
        // few huge ones.
        uint64_t runLenHist[24] = {};
        uint64_t runStart[0x80] = {}; // startPC>>3 for the low code region, bucketed
        uint32_t curRunPairs = 0u;
        uint32_t curRunStartPc = 0xffffffffu;
    };

    thread_local G421Census s_g421;
    // G486: uncovered-lower histogram. File-scope static, NOT part of the thread_local census
    // struct — see the uncovHist note in G421Census.
    uint64_t s_g486UncovHist[0x100] = {};
    uint64_t s_g486UncovTotal = 0ull;

    bool g421CensusOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G421_CENSUS");
        return s_on;
    }

    uint64_t g421ReportEvery()
    {
        static const uint64_t s_every = []() -> uint64_t {
            const char *v = std::getenv("DC2_G421_CENSUS_EVERY");
            const long long n = v ? std::atoll(v) : 0LL;
            return n > 0 ? (uint64_t)n : 60000ull; // ~48 frames at 1244 kicks/f
        }();
        return s_every;
    }

    void g421DumpHist(const char *label, const uint64_t *hist, size_t n, uint64_t total)
    {
        // Print the entries that carry the mass, biggest first, capped.
        size_t order[0x80];
        size_t used = 0u;
        for (size_t i = 0u; i < n; ++i)
            if (hist[i] != 0ull)
                order[used++] = i;
        for (size_t a = 0u; a + 1u < used; ++a)
            for (size_t b = a + 1u; b < used; ++b)
                if (hist[order[b]] > hist[order[a]])
                    std::swap(order[a], order[b]);
        std::fprintf(stderr, "[G421:mix] %s total=%llu distinct=%zu",
                     label, (unsigned long long)total, used);
        const size_t cap = used < 14u ? used : 14u;
        for (size_t k = 0u; k < cap; ++k)
            std::fprintf(stderr, " %02x=%.1f%%", (unsigned)order[k],
                         100.0 * (double)hist[order[k]] / (double)(total ? total : 1ull));
        std::fprintf(stderr, "\n");
    }

    // Kept OUT OF LINE on purpose. Inlining the histogram work into run()'s hot loop measurably
    // enlarged the loop's instruction footprint and slowed the *default* path (~3 ms/frame),
    // which would have corrupted the control arm. The loop now carries only a predicted test on
    // a loop-invariant bool plus a call that never executes unless the census is armed.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g421CloseZeroRun()
    {
        if (s_g421.zeroCur == 0u)
            return;
        const uint32_t len = s_g421.zeroCur;
        ++s_g421.zeroRuns;
        ++s_g421.zeroLenHist[len < 8u ? len : 8u];
        if (len >= 2u)
            s_g421.zeroPairsInRunsGE2 += len;
        s_g421.zeroCur = 0u;
    }

    void g475CloseRunway()
    {
        if (s_g421.g475Cur == 0u)
            return;
        const uint32_t len = s_g421.g475Cur;
        ++s_g421.g475Runs;
        ++s_g421.g475LenHist[len < 8u ? len : 8u];
        if (len >= 2u)
            s_g421.g475PairsInRunsGE2 += len;
        s_g421.g475Cur = 0u;
        s_g421.g475PrevPc = 0xffffffffu;
    }

    void g421NoteCycle(uint32_t pc, uint8_t kind, uint32_t upper, uint32_t lower,
                       bool traceEntrySafe)
    {
        ++s_g421.cycles;
        // G479: dynamic PC distribution + per-run pair count (see the struct comment).
        {
            const uint32_t slot = (pc >> 3u);
            if (slot < (PS2_VU1_CODE_SIZE / 8u))
                ++s_g421.pcHist[slot];
            if (s_g421.curRunPairs == 0u)
                s_g421.curRunStartPc = pc;
            ++s_g421.curRunPairs;
        }
        ++s_g421.upperKind[kind == 0xffu ? 0x7fu : (kind & 0x7fu)];
        // G428: fast-upper SIMD accounting (same descriptor table the lever dispatches on).
        if (kind < 0x80u && ((upper >> 31u) & 1u) == 0u)
        {
            const uint8_t cls = g421Desc()[kind & 0x7fu].cls;
            if (cls == 1u || cls == 2u)
            {
                ++s_g421.fmacExec;
                ++s_g421.destHist[(upper >> 21u) & 0xFu];
            }
            else if (cls != 0u)
                ++s_g421.nonFmacCovered;
        }
        // G425 zero-work run census.
        if (kind == 0x6fu && ((upper >> 30u) & 3u) == 0u &&
            (lower == 0x00000000u || lower == 0x8000033Cu))
        {
            ++s_g421.zeroPairs;
            ++s_g421.zeroCur;
        }
        else
        {
            g421CloseZeroRun();
        }
        // G475: G422's dense covered kinds 8..17 are control transfers. All other covered
        // kinds are branch-free and scalar-pipe/flag-reader-free by G422's contract.
        const uint8_t g475LowerKind =
            g422DecodeLowerKind(lower, s_g410Cache.decodeFlagLower,
                                (s_g410Cache.decodeArmState & 2u) != 0u);
        const bool g475Control = g475LowerKind >= G422_LK_B &&
                                 g475LowerKind <= G422_LK_IBGEZ;
        // G487: a flag consumer is covered but NOT trace-safe — same exclusion as
        // g475TracePairEligible, made in the same edit so the two cannot drift.
        const bool g475Flag = g475LowerKind >= (uint8_t)G422_LK_FIRST_FLAG;
        const bool g475Scalar = g475LowerKind == (uint8_t)G422_LK_DIV;
        const bool g475Safe = traceEntrySafe && kind == 0x6fu &&
                              ((upper >> 30u) & 3u) == 0u &&
                              g475LowerKind != G422_LK_NONE && !g475Control && !g475Flag &&
                              !g475Scalar;
        if (g475Safe)
        {
            if (s_g421.g475Cur != 0u && pc != s_g421.g475PrevPc + 8u)
                g475CloseRunway();
            ++s_g421.g475Pairs;
            ++s_g421.g475Cur;
            s_g421.g475PrevPc = pc;
        }
        else
        {
            g475CloseRunway();
        }
        if (((upper >> 31u) & 1u) != 0u)
            return;
        if (lower == 0x00000000u || lower == 0x8000033Cu)
        {
            ++s_g421.lowerNop;
            return;
        }
        ++s_g421.lowerExec;
        const uint8_t op = (uint8_t)((lower >> 25u) & 0x7fu);
        ++s_g421.lowerOpHi[op];
        if (op == 0x40u)
            ++s_g421.lowerS2[S2_FUNC(lower) & 0x7fu];
        if (op == 0x14u || op == 0x16u || op == 0x17u || op == 0x18u ||
            op == 0x1au || op == 0x1bu || op == 0x1cu)
        {
            ++s_g421.flagReadExec;
            if (op == 0x14u || op == 0x16u || op == 0x17u)
                ++s_g421.statusReadExec;
        }
        // G487: decode under the arm the G410 pair cache was actually built with, so
        // `[G422:cover]` / `[G479:lk]` / `[G486:uncov]` report the dispatch that RAN.
        const uint8_t lk = g422DecodeLowerKind(lower, s_g410Cache.decodeFlagLower,
                                               (s_g410Cache.decodeArmState & 2u) != 0u);
        if (lk < (uint8_t)G422_LK_COUNT)
            ++s_g421.lowerKind[lk];
        // G486: name the uncovered lowers (see the uncovHist comment).
        if (lk == (uint8_t)G422_LK_NONE)
        {
            const uint32_t uidx = (op == 0x40u) ? (0x80u | (uint32_t)S2_FUNC(lower))
                                                : (uint32_t)op;
            ++s_g486UncovHist[uidx & 0xFFu];
            ++s_g486UncovTotal;
        }
    }

    // G479: a run() boundary closes the per-run pair count. Called from the same site that
    // increments `runs`, i.e. once per MSCAL/MSCNT, not per pair.
    void g479CloseRun()
    {
        if (s_g421.curRunPairs == 0u)
            return;
        uint32_t b = 0u;
        uint32_t n = s_g421.curRunPairs;
        while (n > 1u && b + 1u < 24u) { n >>= 1u; ++b; }
        ++s_g421.runLenHist[b];
        const uint32_t sslot = s_g421.curRunStartPc >> 3u;
        ++s_g421.runStart[sslot < 0x80u ? sslot : 0x7fu];
        s_g421.curRunPairs = 0u;
        s_g421.curRunStartPc = 0xffffffffu;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g421ReportMaybe()
    {
        g479CloseRun();
        g421CloseZeroRun(); // a run() boundary always ends a zero-work run
        g475CloseRunway();
        if (s_g421.runs % g421ReportEvery() != 0ull)
            return;
        const double runs = (double)(s_g421.runs ? s_g421.runs : 1ull);
        const double cyc = (double)(s_g421.cycles ? s_g421.cycles : 1ull);
        std::fprintf(stderr,
                     "[G421:vu] runs=%llu cycles=%llu cyc/run=%.1f | upperExec=%.1f%% vnop=%.1f%% "
                     "iBit=%.2f%% hazSave=%.1f%% lowerExec=%.1f%% lowerNop=%.1f%%\n",
                     (unsigned long long)s_g421.runs,
                     (unsigned long long)s_g421.cycles,
                     (double)s_g421.cycles / runs,
                     100.0 * (double)s_g421.upperExec / cyc,
                     100.0 * (double)s_g421.upperVnop / cyc,
                     100.0 * (double)s_g421.iBits / cyc,
                     100.0 * (double)s_g421.hazSave / cyc,
                     100.0 * (double)s_g421.lowerExec / cyc,
                     100.0 * (double)s_g421.lowerNop / cyc);
        // Fast-upper coverage, derived from the same descriptor table the lever uses, so it
        // cannot drift from it. Costs nothing in the hot loop.
        {
            uint64_t vnop = s_g421.upperKind[0x6fu];
            uint64_t covered = 0ull, uncovered = 0ull;
            for (unsigned k = 0u; k < 0x80u; ++k)
            {
                if (k == 0x6fu)
                    continue;
                const uint64_t n = s_g421.upperKind[k];
                if (n == 0ull)
                    continue;
                if (k < 0x72u && g421Desc()[k].cls != 0u)
                    covered += n;
                else
                    uncovered += n;
            }
            const double exec = (double)(covered + uncovered);
            std::fprintf(stderr,
                         "[G421:cover] vnop=%.1f%% ofCycles | nonVnop=%llu covered=%.2f%% "
                         "uncovered=%.2f%% (%llu)\n",
                         100.0 * (double)vnop / cyc,
                         (unsigned long long)(covered + uncovered),
                         100.0 * (double)covered / (exec > 0.0 ? exec : 1.0),
                         100.0 * (double)uncovered / (exec > 0.0 ? exec : 1.0),
                         (unsigned long long)uncovered);
        }
        // G422 fast-lower coverage over EXECUTED lowers (the NOP share is counted separately
        // because for those the lever removes the call outright rather than replacing a body).
        {
            uint64_t covered = 0ull, uncovered = s_g421.lowerKind[G422_LK_NONE];
            for (unsigned k = 1u; k < (unsigned)G422_LK_COUNT; ++k)
                covered += s_g421.lowerKind[k];
            const double exec = (double)(covered + uncovered);
            std::fprintf(stderr,
                         "[G422:cover] exec=%llu covered=%.2f%% uncovered=%.2f%% (%llu) | "
                         "nopSkipped=%.1f%% ofCycles\n",
                         (unsigned long long)(covered + uncovered),
                         100.0 * (double)covered / (exec > 0.0 ? exec : 1.0),
                         100.0 * (double)uncovered / (exec > 0.0 ? exec : 1.0),
                         (unsigned long long)uncovered,
                         100.0 * (double)s_g421.lowerNop / cyc);
        }
        // G425: zero-work pair run-length distribution.
        {
            const double zp = (double)(s_g421.zeroPairs ? s_g421.zeroPairs : 1ull);
            std::fprintf(stderr,
                         "[G425:zero] pairs=%llu (%.1f%% ofCycles) runs=%llu avgLen=%.2f "
                         "inRunsGE2=%llu (%.1f%% ofZero) len1=%.1f%% len2=%.1f%% len3=%.1f%% "
                         "len4=%.1f%% len5=%.1f%% len6=%.1f%% len7=%.1f%% len8+=%.1f%%\n",
                         (unsigned long long)s_g421.zeroPairs,
                         100.0 * (double)s_g421.zeroPairs / cyc,
                         (unsigned long long)s_g421.zeroRuns,
                         (double)s_g421.zeroPairs / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         (unsigned long long)s_g421.zeroPairsInRunsGE2,
                         100.0 * (double)s_g421.zeroPairsInRunsGE2 / zp,
                         100.0 * (double)s_g421.zeroLenHist[1] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[2] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[3] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[4] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[5] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[6] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[7] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull),
                         100.0 * (double)s_g421.zeroLenHist[8] / (double)(s_g421.zeroRuns ? s_g421.zeroRuns : 1ull));
        }
        // G479: dynamic PC distribution. Top slots by executed-pair share, plus how much of the
        // stream the hottest 16/64/256 slots carry — a concentrated distribution says a
        // specialised path can reach most of the thread; a flat one says it cannot.
        {
            // Static, not a local: MSVC reserves the maximum frame for the whole function, and
            // this report is called (inlined or not) from run(), whose hot loop must not pay a
            // 16 KB frame / stack probe for a default-off diagnostic.
            static thread_local size_t order[PS2_VU1_CODE_SIZE / 8u];
            size_t used = 0u;
            for (size_t i = 0u; i < (PS2_VU1_CODE_SIZE / 8u); ++i)
                if (s_g421.pcHist[i] != 0ull)
                    order[used++] = i;
            std::sort(order, order + used, [](size_t a, size_t b) {
                return s_g421.pcHist[a] > s_g421.pcHist[b];
            });
            uint64_t c16 = 0ull, c64 = 0ull, c256 = 0ull;
            for (size_t k = 0u; k < used; ++k)
            {
                const uint64_t v = s_g421.pcHist[order[k]];
                if (k < 16u) c16 += v;
                if (k < 64u) c64 += v;
                if (k < 256u) c256 += v;
            }
            std::fprintf(stderr,
                         "[G479:pc] live=%zu top16=%.1f%% top64=%.1f%% top256=%.1f%% |",
                         used, 100.0 * (double)c16 / cyc, 100.0 * (double)c64 / cyc,
                         100.0 * (double)c256 / cyc);
            const size_t cap = used < 20u ? used : 20u;
            for (size_t k = 0u; k < cap; ++k)
                std::fprintf(stderr, " %04zx:%.2f", order[k] * 8u,
                             100.0 * (double)s_g421.pcHist[order[k]] / cyc);
            std::fprintf(stderr, "\n");
        }
        // G479: which G422 lower kinds carry the executed mass. The inline lower switch is the
        // largest block of TEXT in the pair loop and G435 already proved this loop pays for text
        // it never executes, so the outlining decision needs the per-kind shares, not the
        // aggregate coverage number.
        {
            uint64_t tot = 0ull;
            for (unsigned k = 0u; k < (unsigned)G422_LK_COUNT; ++k)
                tot += s_g421.lowerKind[k];
            std::fprintf(stderr, "[G479:lk] exec=%llu |", (unsigned long long)tot);
            for (unsigned k = 0u; k < (unsigned)G422_LK_COUNT; ++k)
                if (s_g421.lowerKind[k] != 0ull)
                    std::fprintf(stderr, " %u=%.2f%%", k,
                                 100.0 * (double)s_g421.lowerKind[k] / cyc);
            std::fprintf(stderr, "\n");
        }
        // G479: pairs-per-run distribution (power-of-two buckets) and where runs START.
        {
            std::fprintf(stderr, "[G479:run] runs=%llu |",
                         (unsigned long long)s_g421.runs);
            for (uint32_t b = 0u; b < 24u; ++b)
                if (s_g421.runLenHist[b] != 0ull)
                    std::fprintf(stderr, " 2^%u=%.1f%%", b,
                                 100.0 * (double)s_g421.runLenHist[b] / runs);
            std::fprintf(stderr, " | startPC");
            for (uint32_t s = 0u; s < 0x80u; ++s)
                if (s_g421.runStart[s] != 0ull)
                    std::fprintf(stderr, " %03x:%.1f%%", s * 8u,
                                 100.0 * (double)s_g421.runStart[s] / runs);
            std::fprintf(stderr, "\n");
        }
        // G475 compact lower-only trace runway. This is the fraction that can avoid the
        // full upper/hazard/dispatch half of the pair loop while still doing useful lower work.
        {
            const double gp = (double)(s_g421.g475Pairs ? s_g421.g475Pairs : 1ull);
            std::fprintf(stderr,
                         "[G475:runway] pairs=%llu (%.1f%% ofCycles) runs=%llu avgLen=%.2f "
                         "inRunsGE2=%llu (%.1f%% ofPairs) "
                         "len1=%.1f%% len2=%.1f%% len3=%.1f%% len4=%.1f%% "
                         "len5=%.1f%% len6=%.1f%% len7=%.1f%% len8+=%.1f%%\n",
                         (unsigned long long)s_g421.g475Pairs,
                         100.0 * (double)s_g421.g475Pairs / cyc,
                         (unsigned long long)s_g421.g475Runs,
                         (double)s_g421.g475Pairs /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         (unsigned long long)s_g421.g475PairsInRunsGE2,
                         100.0 * (double)s_g421.g475PairsInRunsGE2 / gp,
                         100.0 * (double)s_g421.g475LenHist[1] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[2] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[3] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[4] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[5] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[6] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[7] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull),
                         100.0 * (double)s_g421.g475LenHist[8] /
                             (double)(s_g421.g475Runs ? s_g421.g475Runs : 1ull));
        }
        // G428: where the SIMD ops go on the fast-upper path.
        {
            const double fm = (double)(s_g421.fmacExec ? s_g421.fmacExec : 1ull);
            std::fprintf(stderr,
                         "[G428:simd] fmac=%llu (%.1f%% ofCycles) nonFmacCovered=%llu | "
                         "destF=%.2f%% destXYZ=%.2f%% destX=%.2f%% other=%.2f%% | "
                         "flagRead=%llu (%.4f%% ofCycles) statusRead=%llu\n",
                         (unsigned long long)s_g421.fmacExec,
                         100.0 * (double)s_g421.fmacExec / cyc,
                         (unsigned long long)s_g421.nonFmacCovered,
                         100.0 * (double)s_g421.destHist[0xF] / fm,
                         100.0 * (double)s_g421.destHist[0xE] / fm,
                         100.0 * (double)s_g421.destHist[0x8] / fm,
                         100.0 * (double)(s_g421.fmacExec - s_g421.destHist[0xF] -
                                          s_g421.destHist[0xE] - s_g421.destHist[0x8]) / fm,
                         (unsigned long long)s_g421.flagReadExec,
                         100.0 * (double)s_g421.flagReadExec / cyc,
                         (unsigned long long)s_g421.statusReadExec);
            g421DumpHist("destMask", s_g421.destHist, 16u, s_g421.fmacExec);
        }
        // G486: the uncovered lowers, biggest first. `4x` = a top-level opHi, `S2:xx` = a
        // Special2 S2_FUNC. Shares are of the uncovered population AND of all cycles, because the
        // second is what sizes a lever.
        {
            const double ut = (double)(s_g486UncovTotal ? s_g486UncovTotal : 1ull);
            size_t uorder[0x100];
            size_t uused = 0u;
            for (size_t i = 0u; i < 0x100u; ++i)
                if (s_g486UncovHist[i] != 0ull)
                    uorder[uused++] = i;
            std::sort(uorder, uorder + uused, [](size_t a, size_t b) {
                return s_g486UncovHist[a] > s_g486UncovHist[b];
            });
            std::fprintf(stderr, "[G486:uncov] n=%llu (%.2f%% ofCycles) distinct=%zu |",
                         (unsigned long long)s_g486UncovTotal,
                         100.0 * (double)s_g486UncovTotal / cyc, uused);
            const size_t ucap = uused < 16u ? uused : 16u;
            for (size_t k = 0u; k < ucap; ++k)
            {
                const size_t i = uorder[k];
                if (i & 0x80u)
                    std::fprintf(stderr, " S2:%02x=%.1f%%/%.2f%%", (unsigned)(i & 0x7Fu),
                                 100.0 * (double)s_g486UncovHist[i] / ut,
                                 100.0 * (double)s_g486UncovHist[i] / cyc);
                else
                    std::fprintf(stderr, " op:%02x=%.1f%%/%.2f%%", (unsigned)i,
                                 100.0 * (double)s_g486UncovHist[i] / ut,
                                 100.0 * (double)s_g486UncovHist[i] / cyc);
            }
            std::fprintf(stderr, "\n");
        }
        g421DumpHist("upperKind", s_g421.upperKind, 0x80u, s_g421.cycles);
        g421DumpHist("lowerOpHi", s_g421.lowerOpHi, 0x80u, s_g421.cycles);
        g421DumpHist("lowerS2", s_g421.lowerS2, 0x80u, s_g421.cycles);
    }

// ---------------------------------------------------------------------------
// G425 - VU1-worker INTERNAL attribution (diagnostic only, default OFF).
//
// The premise gate says the VU1 worker is a pole at ~47-48 ms/f. Before optimizing the
// instruction loop we have to know how much of that time is the loop at all: XGKICK runs
// INSIDE execLower on the same thread and forwards whole GIF packets, so packet walking and
// submission are charged to the VU1 worker too.
//
// Arm: DC2_G425_PROFILE=1  (report every DC2_G425_PROFILE_EVERY run() calls, default 20000)
// ---------------------------------------------------------------------------
namespace
{
    struct G425Prof
    {
        uint64_t runNs = 0ull;      // total time inside VU1Interpreter::run()
        uint64_t runs = 0ull;
        uint64_t kickNs = 0ull;     // total time inside the XGKICK lower-op body
        uint64_t kicks = 0ull;
        uint64_t walkNs = 0ull;     // GIF tag walk that sizes the packet
        uint64_t submitNs = 0ull;   // g96Submit (packet hand-off to the GIF arbiter)
        uint64_t kickBytes = 0ull;
        uint64_t lastReport = 0ull;
    };

    thread_local G425Prof s_g425;

    bool g425ProfileOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G425_PROFILE");
        return s_on;
    }

    uint64_t g425ReportEvery()
    {
        static const uint64_t s_every = []() -> uint64_t {
            const char *v = std::getenv("DC2_G425_PROFILE_EVERY");
            const long long n = v ? std::atoll(v) : 0LL;
            return n > 0 ? (uint64_t)n : 20000ull;
        }();
        return s_every;
    }

    inline uint64_t g425NowNs()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g425ReportMaybe()
    {
        if (s_g425.runs - s_g425.lastReport < g425ReportEvery())
            return;
        s_g425.lastReport = s_g425.runs;
        const double run = (double)(s_g425.runNs ? s_g425.runNs : 1ull);
        std::fprintf(stderr,
                     "[G425:vu1prof] runs=%llu runMs=%.1f | kicks=%llu kickMs=%.1f (%.1f%% ofRun) "
                     "walkMs=%.1f submitMs=%.1f | loopMs=%.1f (%.1f%%) kickBytes/kick=%.0f "
                     "us/kick=%.2f\n",
                     (unsigned long long)s_g425.runs, s_g425.runNs / 1e6,
                     (unsigned long long)s_g425.kicks, s_g425.kickNs / 1e6,
                     100.0 * (double)s_g425.kickNs / run,
                     s_g425.walkNs / 1e6, s_g425.submitNs / 1e6,
                     (double)(s_g425.runNs - s_g425.kickNs) / 1e6,
                     100.0 * (double)(s_g425.runNs - s_g425.kickNs) / run,
                     (double)s_g425.kickBytes / (double)(s_g425.kicks ? s_g425.kicks : 1ull),
                     (double)s_g425.kickNs / 1e3 / (double)(s_g425.kicks ? s_g425.kicks : 1ull));
    }
} // namespace

} // namespace
