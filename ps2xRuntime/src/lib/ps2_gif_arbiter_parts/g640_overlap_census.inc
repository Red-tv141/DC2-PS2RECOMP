// ============================================================================
// G640 — the OVERLAP-WINDOW census (default OFF, DC2_G640_OVL=1).
//
// THE QUESTION THIS EXISTS TO ANSWER. `[G447:edge]` says the GS worker — the
// `dragon` pole — is blocked ~10.4 ms/f across five backend edges. Every
// asynchronous-submission attempt on this port (G305, G444, G452, G493) has
// measured NEUTRAL or WORSE, and G493's autopsy named the reason: "the wait
// relocates; it does not disappear ... the packet simply had no CPU to overlap
// with." Nobody has ever MEASURED how much CPU there is to overlap with.
//
// An async break at edge E can hide, at most,
//
//     min( E's own wait , the CPU the worker runs before it blocks again )
//
// so the ceiling on the whole asynchronous-pipeline direction is
//
//     ceiling(K) = SUM over items of min( wait_i , cpu until the K-th next
//                                                  BACKEND edge after item i )
//
// K is the pipeline DEPTH: K=1 is "one packet in flight" (what
// `G178Backend::submitAsync` implements today, kAsyncSlots = 1); K=4 is a deep
// ring that can keep four independent workloads outstanding. Reporting the
// whole K curve prices the ring depth BEFORE anyone builds one, which is the
// G492 law ("a ceiling costs one run and a design costs a phase") applied to a
// scheduling change instead of to a deletion.
//
// WHY IT IS BEHAVIOUR-PURE, unlike the G492 ceiling probes. It deletes nothing,
// skips nothing and reorders nothing: it reads the clock the G447 scope already
// reads and does arithmetic on the timeline. `norender` cannot price this route
// at all — on `dragon` its own `[G492:inv]` invariant fails by a factor of 30
// (`fallback` 0.38/f candidate against 14.44/f control), because a skipped
// render turns a CPU-fallback batch into a "successful" GPU batch and the two
// arms then run different amounts of downstream work.
//
// THE BARRIER SET. Only edges 0..4 (the five GL-backend round trips) are
// barriers. Edges 5 (`vu1-collect`) and 6 (`arbiter-relock`) fire ~1,886 times
// a frame for ~0.45 us each — they are lock acquisitions inside the window
// loop, not submissions, and treating them as pipeline barriers would report a
// gap of nothing after every backend edge. Their wait is still SUBTRACTED from
// the CPU accumulator, so `gap` remains true runnable CPU and never inherits
// their blocked time.
//
// Self-cost: one `steady_clock::now()` and a four-entry ring update per backend
// edge (~63/frame). It rides inside the existing `G447WaitScope`, so a run with
// this armed pays exactly the G447 census cost plus that. NEVER quote a frame
// time from a run with it on.
// ============================================================================

namespace
{
    bool g640OvlOnImpl()
    {
        static const bool on = (std::getenv("DC2_G640_OVL") != nullptr);
        return on;
    }

    constexpr int kG640Barriers = 5;   // edges 0..4 are GL-backend round trips
    constexpr int kG640Depth = 4;      // report pipeline depths K = 1..4

    struct G640Slot
    {
        int edge = -1;
        uint64_t waitNs = 0ull;   // that item's own blocking wait
        uint64_t cpuAt = 0ull;    // the CPU accumulator when it completed
    };

    // GS-worker-thread private: only that thread writes, and only its own
    // frame-boundary branch reads.
    uint64_t g_g640CpuNs = 0ull;                    // runnable CPU, all edge waits excluded
    std::chrono::steady_clock::time_point g_g640LastEnd{};
    G640Slot g_g640Ring[kG640Depth];                // most recent first
    int g_g640RingN = 0;

    // hide[k][edge] = SUM over items of min(wait_i, gap_i(k+1)) in ns.
    uint64_t g_g640HideNs[kG640Depth][kG640Barriers] = {};
    uint64_t g_g640GapNs[kG640Depth][kG640Barriers] = {};
    uint64_t g_g640WaitNs[kG640Barriers] = {};
    uint64_t g_g640N[kG640Barriers] = {};
    // report deltas
    uint64_t g_g640LastHideNs[kG640Depth][kG640Barriers] = {};
    uint64_t g_g640LastGapNs[kG640Depth][kG640Barriers] = {};
    uint64_t g_g640LastWaitNs[kG640Barriers] = {};
    uint64_t g_g640LastN[kG640Barriers] = {};
    uint64_t g_g640Boundaries = 0ull;

    const char *g640EdgeShort(int e)
    {
        switch (e)
        {
        case 0: return "render";
        case 1: return "rdcol";
        case 2: return "wrcol";
        case 3: return "copy";
        case 4: return "other";
        default: return "?";
        }
    }
} // namespace

bool g640OvlOn() { return g640OvlOnImpl(); }

// Called from g447NoteWait for EVERY edge (0..6) on the GS worker, at the
// moment the wait completed. `ns` is that wait's duration, so its start is
// `now - ns` and the CPU that preceded it is `start - lastEnd`.
void g640NoteEdge(int edge, unsigned long long ns)
{
    if (!g640OvlOnImpl())
        return;
    const auto end = std::chrono::steady_clock::now();
    const auto start = end - std::chrono::nanoseconds(static_cast<int64_t>(ns));
    if (g_g640LastEnd.time_since_epoch().count() != 0)
    {
        const int64_t cpu = std::chrono::duration_cast<std::chrono::nanoseconds>(
            start - g_g640LastEnd).count();
        if (cpu > 0)
            g_g640CpuNs += static_cast<uint64_t>(cpu);
    }
    g_g640LastEnd = end;
    if (edge < 0 || edge >= kG640Barriers)
        return; // edges 5/6: their wait stays excluded from CPU, but they are not barriers

    // This backend edge is a barrier: close the gap of every earlier barrier
    // still in the ring. Distance k = 1 means "the very next backend edge".
    for (int i = 0; i < g_g640RingN; ++i)
    {
        const G640Slot &s = g_g640Ring[i];
        if (s.edge < 0)
            continue;
        const uint64_t gap = g_g640CpuNs - s.cpuAt;
        const int k = i; // ring[0] is the immediately preceding barrier => k index 0 == depth 1
        g_g640GapNs[k][s.edge] += gap;
        g_g640HideNs[k][s.edge] += (gap < s.waitNs) ? gap : s.waitNs;
    }
    for (int i = kG640Depth - 1; i > 0; --i)
        g_g640Ring[i] = g_g640Ring[i - 1];
    g_g640Ring[0].edge = edge;
    g_g640Ring[0].waitNs = static_cast<uint64_t>(ns);
    g_g640Ring[0].cpuAt = g_g640CpuNs;
    if (g_g640RingN < kG640Depth)
        ++g_g640RingN;
    g_g640WaitNs[edge] += static_cast<uint64_t>(ns);
    ++g_g640N[edge];
}

// Called from the GS worker's frame-boundary branch, beside g447ReportBoundary.
void g640ReportBoundary()
{
    if (!g640OvlOnImpl())
        return;
    if ((++g_g640Boundaries % 30ull) != 0ull)
        return;
    const double f = 30.0;
    char line[1024];
    int off = std::snprintf(line, sizeof(line), "[G640:ovl] frames=30 |");
    double ceil1 = 0.0, ceil2 = 0.0, ceil4 = 0.0, waitAll = 0.0;
    for (int e = 0; e < kG640Barriers; ++e)
    {
        const double n = static_cast<double>(g_g640N[e] - g_g640LastN[e]) / f;
        const double waitMs =
            static_cast<double>(g_g640WaitNs[e] - g_g640LastWaitNs[e]) / 1.0e6 / f;
        waitAll += waitMs;
        double hide[kG640Depth], gapUs[kG640Depth];
        for (int k = 0; k < kG640Depth; ++k)
        {
            hide[k] = static_cast<double>(g_g640HideNs[k][e] - g_g640LastHideNs[k][e]) /
                      1.0e6 / f;
            const double gapMs =
                static_cast<double>(g_g640GapNs[k][e] - g_g640LastGapNs[k][e]) / 1.0e6 / f;
            gapUs[k] = (n > 0.0) ? gapMs * 1000.0 / n : 0.0;
            g_g640LastHideNs[k][e] = g_g640HideNs[k][e];
            g_g640LastGapNs[k][e] = g_g640GapNs[k][e];
        }
        // hide[] is cumulative in K by construction (gap grows with depth), so
        // depth K's ceiling is simply hide[K-1].
        ceil1 += hide[0];
        ceil2 += hide[1];
        ceil4 += hide[kG640Depth - 1];
        if (off < static_cast<int>(sizeof(line)) - 1)
            off += std::snprintf(line + off, sizeof(line) - off,
                                 " %s(n=%.1f wait=%.2f gap1=%.0fus gap4=%.0fus "
                                 "hide1=%.2f hide4=%.2f)",
                                 g640EdgeShort(e), n, waitMs, gapUs[0],
                                 gapUs[kG640Depth - 1], hide[0], hide[kG640Depth - 1]);
        g_g640LastWaitNs[e] = g_g640WaitNs[e];
        g_g640LastN[e] = g_g640N[e];
    }
    std::fprintf(stderr, "%s | wait=%.2f ceilK1=%.2f ceilK2=%.2f ceilK4=%.2f (ms/f)\n",
                 line, waitAll, ceil1, ceil2, ceil4);
}
