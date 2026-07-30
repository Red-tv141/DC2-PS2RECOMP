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
        uint64_t lowerKind[G422_LK_COUNT] = {};
        // G425: consecutive "zero-work pair" runs. A zero-work pair is VNOP upper + NOP lower
        // with neither the E nor the I bit, i.e. a pair whose only effect is to advance the
        // Q/P delays, the MAC/STATUS delay line and the PC. Bucketed by run length so a
        // run-collapsing lever can be SIZED before it is built (G420 ceiling rule).
        uint64_t zeroPairs = 0ull;                 // total zero-work pairs seen
        uint64_t zeroRuns = 0ull;                  // number of maximal runs
        uint64_t zeroLenHist[9] = {};              // 1..7 exact, [8] = >=8
        uint64_t zeroPairsInRunsGE2 = 0ull;        // pairs collapsible by a >=2 run lever
        uint32_t zeroCur = 0u;                     // current run length (in flight)
    };

    thread_local G421Census s_g421;

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

    void g421NoteCycle(uint8_t kind, uint32_t upper, uint32_t lower)
    {
        ++s_g421.cycles;
        ++s_g421.upperKind[kind == 0xffu ? 0x7fu : (kind & 0x7fu)];
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
        const uint8_t lk = g422DecodeLowerKind(lower);
        if (lk < (uint8_t)G422_LK_COUNT)
            ++s_g421.lowerKind[lk];
    }

    void g421ReportMaybe()
    {
        g421CloseZeroRun(); // a run() boundary always ends a zero-work run
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
