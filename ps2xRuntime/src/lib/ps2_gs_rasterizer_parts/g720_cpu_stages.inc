// One coarse production timer family. No per-entry clocks, atomics, nested diagnostic
// instruments or GPU synchronization. Early returns are charged to their current stage.
// All totals use the same attempted-flush population; consume differences of raw ns
// against G154 n (guest frame boundaries), never its script-clock `frame` field.
struct G720CpuStages
{
    // G724: `Class` measured 2.004 ms/f - the largest CPU-only stage - while G712 prep-ahead was
    // serving 99.9% of the classify pass from a worker. So the classify LOOP is not what that
    // number is; the lap simply spans 773 lines. Split it at its three real seams:
    //   Resid  172..437  residency / persistent-surface acquisition and the target contracts
    //   ClsWin 437..590  prep-ahead consume + the G621 producer row window
    //   Class  590..945  the classify loop itself and its eleven reductions
    // The first split showed `Resid` alone is 2.347 ms/f - 79 us per flush of FIXED prologue with
    // no entry loop in it - so split it again:
    //   ResidA 172..199  G589 drain-transaction record and the transient contract predicates
    //   ResidB 199..320  G700 persistent candidacy / takeover and the G718 probe
    //   Resid  320..437  the g255/g261 RTT admission, g700Acquire and the G266 FBW reset
    //                    (this one can call a BLOCKING g261Materialize)
    enum Stage : unsigned { Preflight, ResidA, ResidB, Resid, ClsWin, Class, Fbz, Deps, Tex, Verts,
                            Submit, Post, Count };
    using Clock = std::chrono::steady_clock;
    bool on;
    bool accepted = false;
    Stage stage = Preflight;
    Clock::time_point started{}, last{};
    uint64_t elapsed[Count]{};
    size_t entries;
    // G785 (DIAGNOSTIC, DC2_G785_STAGE_CLASS=1): time EVERY flush and also book it into a batch
    // class set by the flush (g785ClassifyBatch): 0 other, 1 a textured entry sampling the E91F00
    // T8 page (tbp0 0x2820 inside the 0x141 RTT surface), 2 a native (G778/G784) batch, 3 a
    // textured entry whose texture range lies in a page the GPU can own (RTT-aliased texture).
    unsigned cls = 0;
    static bool classOn()
    {
        static const bool value =
            envFlagEnabled("DC2_G785_STAGE_CLASS") || envFlagEnabled("DC2_G786_OLCLASS");
        return value;
    }

    explicit G720CpuStages(size_t count) : on(enabled() || classOn()), entries(count)
    {
        // Timestamp only a random 1/32 of flushes. Fixed-stride sampling would
        // alias the title's repeating target sequence. Keep all eight laps for
        // each selected flush so stage totals still conserve measured time.
        if (on && !classOn())
        {
            static uint32_t rng = 0x720a319du;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            on = (rng & 31u) == 0u;
        }
        if (on) started = last = Clock::now();
        if (on && classOn())
        {
            rb0 = g_g786RbN.load(std::memory_order_relaxed);
            rbUs0 = g_g786RbUs.load(std::memory_order_relaxed);
        }
    }
    uint64_t rb0 = 0, rbUs0 = 0; // G786: readbacks (colour + depth) issued inside this flush
    static bool enabled()
    {
        static const bool value = envFlagEnabled("DC2_G720_PROFILE");
        return value;
    }
    void lap(Stage next)
    {
        if (!on) return;
        const auto now = Clock::now();
        elapsed[stage] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
        last = now;
        stage = next;
    }
    ~G720CpuStages()
    {
        if (!on) return;
        lap(stage);
        static uint64_t attempts = 0, successes = 0, entryCount = 0, totalNs = 0;
        static uint64_t totals[Count]{};
        ++attempts;
        successes += accepted;
        entryCount += entries;
        totalNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(last - started).count());
        for (unsigned i = 0; i != Count; ++i) totals[i] += elapsed[i];
        if (classOn())
        {
            // G785: cumulative per-class totals, all flushes (sample rate 1). Executor thread only.
            static uint64_t cN[4]{}, cEntries[4]{}, cTotal[4]{}, cStage[4][Count]{};
            const unsigned c = cls < 4u ? cls : 0u;
            ++cN[c];
            cEntries[c] += entries;
            static uint64_t cRb[4]{}, cRbUs[4]{};
            cRb[c] += g_g786RbN.load(std::memory_order_relaxed) - rb0;
            cRbUs[c] += g_g786RbUs.load(std::memory_order_relaxed) - rbUs0;
            if (attempts % 512 == 0)
                std::fprintf(stderr, "[G786:rb] rb0=%llu us0=%llu rb1=%llu us1=%llu rb2=%llu us2=%llu rb3=%llu us3=%llu\n",
                             (unsigned long long)cRb[0], (unsigned long long)cRbUs[0],
                             (unsigned long long)cRb[1], (unsigned long long)cRbUs[1],
                             (unsigned long long)cRb[2], (unsigned long long)cRbUs[2],
                             (unsigned long long)cRb[3], (unsigned long long)cRbUs[3]);
            cTotal[c] += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(last - started).count());
            for (unsigned i = 0; i != Count; ++i) cStage[c][i] += elapsed[i];
            if (attempts % 512 == 0)
                for (unsigned k = 0; k != 4u; ++k)
                    std::fprintf(stderr,
                        "[G785:stage] cls%u n=%llu entries=%llu totalNs=%llu preflight=%llu "
                        "resida=%llu residb=%llu resid=%llu clswin=%llu class=%llu fbz=%llu "
                        "deps=%llu tex=%llu verts=%llu submit=%llu post=%llu\n",
                        k, (unsigned long long)cN[k], (unsigned long long)cEntries[k],
                        (unsigned long long)cTotal[k],
                        (unsigned long long)cStage[k][Preflight], (unsigned long long)cStage[k][ResidA],
                        (unsigned long long)cStage[k][ResidB], (unsigned long long)cStage[k][Resid],
                        (unsigned long long)cStage[k][ClsWin], (unsigned long long)cStage[k][Class],
                        (unsigned long long)cStage[k][Fbz], (unsigned long long)cStage[k][Deps],
                        (unsigned long long)cStage[k][Tex], (unsigned long long)cStage[k][Verts],
                        (unsigned long long)cStage[k][Submit], (unsigned long long)cStage[k][Post]);
            return;
        }
        if (attempts % 128 != 0) return;
        std::fprintf(stderr,
            "[G720:cpu] attempts=%llu accepted=%llu entries=%llu totalNs=%llu "
            "preflightNs=%llu residaNs=%llu residbNs=%llu residNs=%llu clswinNs=%llu classNs=%llu "
            "fbzNs=%llu depsNs=%llu "
            "texNs=%llu vertsNs=%llu submitNs=%llu postNs=%llu sampleRate=32\n",
            (unsigned long long)attempts, (unsigned long long)successes,
            (unsigned long long)entryCount, (unsigned long long)totalNs,
            (unsigned long long)totals[Preflight], (unsigned long long)totals[ResidA],
            (unsigned long long)totals[ResidB], (unsigned long long)totals[Resid],
            (unsigned long long)totals[ClsWin], (unsigned long long)totals[Class],
            (unsigned long long)totals[Fbz], (unsigned long long)totals[Deps],
            (unsigned long long)totals[Tex], (unsigned long long)totals[Verts],
            (unsigned long long)totals[Submit], (unsigned long long)totals[Post]);
    }
};
