// G483 - publish the pair count of the LAST VU1Interpreter::run() call.
//
// WHY. [G441:kick] times execute()/resume() as one bucket, and G482 measured that bucket at 98.4%
// of the VU1 worker's per-kick span - but it has never been split by KICK KIND. MSCNT `resume`
// kicks are ~30% of kicks and every census this arc (G331, G479, G482) excluded them explicitly.
// A per-kick microsecond figure cannot say whether resume kicks are expensive because they run
// MORE pairs or because their pairs are SLOWER; only pairs/kick can. `cycle` is exactly the pair
// loop's iteration count and already exists as a local in run().
//
// COST. One store of a thread_local at the END of run() (~1,230 kicks/frame, i.e. ~1,230 stores
// per frame against ~1.4 M pair iterations). It is unconditional so the default path never reads
// an env flag here, and the value is only ever CONSUMED under DC2_G441_KICK=1.
//
// THREADING. The counter is thread_local and the only consumer (the MTVU worker's [G441:kick]
// accounting) runs on the same thread that runs the interpreter, so no synchronization is needed.
// On the synchronous (non-MTVU) path the EE thread both writes and reads its own copy.
thread_local uint64_t g_g483LastRunCycles = 0ull;

// G483 - and WHY the loop stopped. 0 = maxcycles (the budget ran out with the microprogram still
// live), 1 = pc-oob, 2 = ebit-latched (the program ended itself, the only "natural" exit).
// A kick that ends on `maxcycles` has burned its whole budget without the microprogram deciding
// it was finished; if that is a large share of pairs it is work the frame may not need at all.
thread_local uint32_t g_g483LastStopReason = 0u;

uint64_t dc2_g483_last_run_cycles()
{
    return g_g483LastRunCycles;
}

uint32_t dc2_g483_last_stop_reason()
{
    return g_g483LastStopReason;
}

// ---------------------------------------------------------------------------------------------
// G483 - run() PROLOGUE / LOOP / EPILOGUE split (DC2_G483_RUNPROF=1, default OFF).
//
// PREMISE. [G483:kind] measured geometry kicks at 18.7 ns/pair over 770 pairs and resume kicks at
// 15.7 ns/pair over 2,633. If the marginal pair cost were the same in both, the only thing that
// can produce that gap is a FIXED per-run() cost amortised over different pair counts. Fitting
// exec = a + b*pairs across the two populations gives b ~= 14.5 ns/pair and a ~= 3.2 us per kick,
// i.e. ~4 ms/f of the frame would sit OUTSIDE the pair loop entirely.
//
// That is a claim no instrument in this arc can currently confirm or refute: [G441:kick] prices
// the worker loop AROUND execute() (G482 measured it at 1.02 us/kick and declared the vein spent),
// and every VU1 census since G331 has measured pairs. Nothing has ever timed the inside of run()
// before its first pair. This does, with three clock reads per kick - never per pair.
//
// The regression fit is only a hypothesis: the two populations also run different microprograms,
// so a composition difference could produce the same gap. This measures `a` directly instead.
thread_local uint64_t g_g483ProNs = 0ull, g_g483LoopNs = 0ull, g_g483EpiNs = 0ull;
thread_local uint64_t g_g483ProCalls = 0ull, g_g483ProPairs = 0ull;
thread_local uint64_t g_g483LastProCalls = 0ull;
thread_local uint64_t g_g483LastProNs = 0ull, g_g483LastLoopNs = 0ull, g_g483LastEpiNs = 0ull;
thread_local uint64_t g_g483LastProPairs = 0ull;

bool dc2_g483_run_prof_on()
{
    static const bool s_on = (std::getenv("DC2_G483_RUNPROF") != nullptr);
    return s_on;
}

void dc2_g483_run_prof_note(uint64_t proNs, uint64_t loopNs, uint64_t epiNs, uint64_t pairs)
{
    g_g483ProNs += proNs;
    g_g483LoopNs += loopNs;
    g_g483EpiNs += epiNs;
    g_g483ProPairs += pairs;
    if ((++g_g483ProCalls % 30000ull) != 0ull)
        return;
    const double n = (double)(g_g483ProCalls - g_g483LastProCalls);
    const double pr = (double)(g_g483ProNs - g_g483LastProNs);
    const double lp = (double)(g_g483LoopNs - g_g483LastLoopNs);
    const double ep = (double)(g_g483EpiNs - g_g483LastEpiNs);
    const double pp = (double)(g_g483ProPairs - g_g483LastProPairs);
    const double tot = (pr + lp + ep) > 0.0 ? (pr + lp + ep) : 1.0;
    std::fprintf(stderr,
                 "[G483:runprof] calls=%llu | prologueUs=%.3f (%.1f%%) loopUs=%.3f (%.1f%%) "
                 "epilogueUs=%.3f (%.1f%%) | pairs/call=%.0f loopNsPerPair=%.1f\n",
                 (unsigned long long)g_g483ProCalls,
                 pr / 1e3 / n, 100.0 * pr / tot,
                 lp / 1e3 / n, 100.0 * lp / tot,
                 ep / 1e3 / n, 100.0 * ep / tot,
                 pp / n, pp > 0.0 ? lp / pp : 0.0);
    g_g483LastProCalls = g_g483ProCalls;
    g_g483LastProNs = g_g483ProNs;
    g_g483LastLoopNs = g_g483LoopNs;
    g_g483LastEpiNs = g_g483EpiNs;
    g_g483LastProPairs = g_g483ProPairs;
}
