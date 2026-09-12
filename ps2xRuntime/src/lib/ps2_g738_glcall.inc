// ==============================================================================================
// G738 — GL COMMAND-STREAM CENSUS: the COLD reporter
// ==============================================================================================
//
// Included by ps2_g713_pipeline.cpp, which is cold and already carries <cstdio>. The hot TU keeps
// only the counters and one `fetch_add` per site; all formatting text lives here (G710 priced
// compile-time-dead text in a hot TU at +0.362 ms/f, larger than most levers this project shipped).
//
// The denominator is the caller's: `g738ReportGlCalls` is called from the `[G154:perf]` window on
// the EE thread and is handed that window's own RENDERED-frame count. That is the whole point of
// the file — see ps2_g738_glcall_api.inc for what the old GL-worker-side denominator was.

static const char *const kG738GlClsNames[kG738GlClsCount] = {
    "scissor", "depth", "blend", "bindtex", "texparam", "uniform",
    "DRAW", "colormask", "rawbind", "rawparam", "activetex", "batch"};

const char *g738GlClsName(int cls)
{
    if (cls < 0 || cls >= kG738GlClsCount)
        return "?";
    return kG738GlClsNames[cls];
}

static bool g738GlCensusOn()
{
    static const bool on = (std::getenv("DC2_G738_GLCENSUS") != nullptr);
    return on;
}

// ⭐ `[G738:clocks]` — the three clocks this process has, side by side, once per perf window.
//
// This row exists because the arc has now been caught twice dividing by the wrong one:
//   * RENDERED frames (`n`) — the only real frame denominator. Published as `g_dc2RenderedFrame`.
//   * SCRIPT frames — ~2.214 per rendered frame (G706 Rule 76 measured the constant).
//   * HOST PRESENT ticks — free-run at ~58 Hz independently of the guest (G598), so the ratio to
//     rendered frames is `58 / fps` and changes with the scene. `[G480:pkt]`, `[G645:eepool]` and
//     `[G482:memo]` all used this as their divisor.
// Plus the GL worker's readback-batch count, which `[G496:gl]`/`[G496:cal]` called a frame (8.12x).
//
// Printing all of them beside `n` makes the factor a measurement instead of an assumption.
static bool g738ClocksOn()
{
    static const bool on = (std::getenv("DC2_G738_CLOCKS") != nullptr);
    return on;
}

void g738ReportGlCalls(uint32_t n, uint32_t windowFrames)
{
    if (windowFrames == 0u)
        return;

    // ⛔ RULE 63, STRUCTURALLY. The per-class census must NOT be a precondition for the BALLAST
    // row: the census's own cost scales with the call count the ballast changes, so a price run
    // has to be able to read `drawBallast/f` with the census OFF. The snapshot is twelve relaxed
    // loads once per 60 frames on the EE frame boundary, so it is taken unconditionally and the
    // two rows decide for themselves.
    G738GlSnapshot s{};
    g738_glcall_snapshot(s);

    static G738GlSnapshot s_last{};
    static bool s_primed = false;
    if (!s_primed)
    {
        // ⭐ Prime the baseline from the FIRST read, never from zero. A `s_last = {}` start makes
        // the first printed window report everything accumulated since process start divided by
        // one window — the exact defect that produced `[G735:cpu] vu1CpuMs/f=34.90` on a 19 ms
        // frame and that G737 §9.5 could not explain. One skipped window is the correct price.
        s_last = s;
        s_primed = true;
        return;
    }

    const double w = static_cast<double>(windowFrames);
    const double batches = static_cast<double>(s.batches - s_last.batches) / w;
    const double draws = static_cast<double>(s.drawsSeen - s_last.drawsSeen) / w;

    if (g738ClocksOn() || g738GlCensusOn())
    {
        static uint64_t s_lastTick = 0ull;
        static uint32_t s_lastScript = 0u;
        const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
        const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
        if (s_lastTick != 0ull)
            std::fprintf(stderr,
                         "[G738:clocks] n=%u window=%u renderedFrames=%.0f presentTicks=%.0f "
                         "(x%.2f) scriptFrames=%.0f (x%.3f)\n",
                         n, windowFrames, w, static_cast<double>(tick - s_lastTick),
                         static_cast<double>(tick - s_lastTick) / w,
                         static_cast<double>(sf - s_lastScript),
                         static_cast<double>(sf - s_lastScript) / w);
        s_lastTick = tick;
        s_lastScript = sf;
    }

    if (g738GlCensusOn())
    {
        char line[1024];
        int off =
            std::snprintf(line, sizeof(line), "[G738:glcall] n=%u window=%u", n, windowFrames);

        uint64_t totI = 0ull, totE = 0ull;
        for (int i = 0; i < kG738GlClsCount; ++i)
        {
            const uint64_t di = s.issued[i] - s_last.issued[i];
            const uint64_t de = s.elided[i] - s_last.elided[i];
            totI += di;
            totE += de;
            if ((di + de) > 0ull && off < static_cast<int>(sizeof(line)) - 1)
                off += std::snprintf(line + off, sizeof(line) - off, " %s=%.0f/%.0f",
                                     g738GlClsName(i), static_cast<double>(di) / w,
                                     static_cast<double>(di + de) / w);
        }

        const double issuedPerF = static_cast<double>(totI) / w;
        std::fprintf(stderr,
                     "%s | issued/f=%.0f of %.0f (elided %.1f%%) batches/f=%.1f "
                     "logicalDraws/f=%.0f callsPerDraw=%.2f\n",
                     line, issuedPerF, static_cast<double>(totI + totE) / w,
                     (totI + totE) > 0ull
                         ? 100.0 * static_cast<double>(totE) / static_cast<double>(totI + totE)
                         : 0.0,
                     batches, draws, draws > 0.0 ? issuedPerF / draws : 0.0);
    }

    // The ballast rows. `calls/f` is what the arm actually issued (the state ballast abstains on an
    // unknown cache record, so this is measured, not N x draws). A reader computes
    // ns/call = (frameMs_armed - frameMs_control) * 1e6 / calls/f, against a control run of the
    // SAME binary with the ballast unset — no second image, so Rule 46 does not apply.
    const uint64_t db = s.drawBallastCalls - s_last.drawBallastCalls;
    const uint64_t sb = s.stateBallastCalls - s_last.stateBallastCalls;
    if (db != 0ull || sb != 0ull)
        std::fprintf(stderr,
                     "[G738:ballast] n=%u window=%u drawBallast/f=%.0f stateBallast/f=%.0f "
                     "logicalDraws/f=%.0f\n",
                     n, windowFrames, static_cast<double>(db) / w, static_cast<double>(sb) / w,
                     draws);

    s_last = s;
}
