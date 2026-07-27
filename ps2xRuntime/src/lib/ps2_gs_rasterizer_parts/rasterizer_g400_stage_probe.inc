// G400 — Dungeon 6 free-roam darkening: IN-FRAME stage probe for the fbp=0x139 scene RTT.
//
// G399 measured 0x139 only at present-latch time, i.e. AFTER DC2's free-roam post-process has
// already rendered fbp=0x13b/0x13c/0x14b into sub-rectangles of the same VRAM footprint. That
// cannot tell "0x139 was rendered dark" apart from "0x139 was rendered correctly and a later
// pass inside its footprint overwrote/darkened it".
//
// This probe samples the raw-VRAM 0x139 surface at every FRAME-target switch inside a frame,
// so the loss can be located between two consecutive draw groups.
//
// Diagnostic only, default-off, no behaviour change. Pair with DC2_G399_SURFDUMP=1 so raw VRAM
// stays authoritative under the G178 GPU raster path (g289RawVramDiagOn()).
//   DC2_G400_STAGE=1        enable
//   DC2_G400_FROM=<tick>    first present tick to report on (default 0)
//   DC2_G400_TO=<tick>      last present tick to report on (default FROM+4)
//   DC2_G400_STEP=<n>       pixel stride of the sampling grid (default 4)

static bool g400StageOn()
{
    static const bool s_on = (std::getenv("DC2_G400_STAGE") != nullptr);
    return s_on;
}

static uint64_t g400EnvU64(const char *name, uint64_t fallback)
{
    const char *e = std::getenv(name);
    if (!e || !*e)
        return fallback;
    return static_cast<uint64_t>(std::strtoull(e, nullptr, 0));
}

// Sample the PSMCT32 512x416 fbw=8 surface at `fbp` straight out of guest VRAM.
static void g400SampleSurface(const uint8_t *vram, uint32_t fbp, double &meanL, double &meanA, uint32_t &maxL)
{
    meanL = 0.0;
    meanA = 0.0;
    maxL = 0u;
    if (vram == nullptr)
        return;
    static const uint32_t s_step = static_cast<uint32_t>(g400EnvU64("DC2_G400_STEP", 4ull));
    const uint32_t step = (s_step == 0u) ? 4u : s_step;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    double sumL = 0.0, sumA = 0.0;
    uint64_t n = 0ull;
    for (uint32_t y = 0u; y < 416u; y += step)
    {
        for (uint32_t x = 0u; x < 512u; x += step)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(base, 8u, x, y);
            uint32_t px = 0u;
            std::memcpy(&px, vram + off, 4u);
            const uint32_t r = px & 0xFFu, g = (px >> 8) & 0xFFu, b = (px >> 16) & 0xFFu;
            const uint32_t l = (r * 299u + g * 587u + b * 114u) / 1000u;
            sumL += static_cast<double>(l);
            sumA += static_cast<double>((px >> 24) & 0xFFu);
            if (l > maxL)
                maxL = l;
            ++n;
        }
    }
    if (n)
    {
        meanL = sumL / static_cast<double>(n);
        meanA = sumA / static_cast<double>(n);
    }
}

// Called from the draw entry points. Prints one line per FRAME-target transition inside the
// reporting window, carrying the state of 0x139 as it stood BEFORE the new target's draws.
static void g400NoteDrawTarget(const uint8_t *vram, uint32_t fbp)
{
    if (!g400StageOn())
        return;
    extern std::atomic<uint64_t> g_dc2PresentTick; // defined in ps2_runtime.cpp
    // AUTO mode (no DC2_G400_FROM): arm on the first draw into fbp=0x068, i.e. the first frame
    // DC2 switches its free-roam composite on — the exact onset G398 pinned. Reporting then runs
    // for DC2_G400_TICKS present ticks. Comparing arms by this guest-side event instead of a host
    // tick is mandatory on this route (G398 §4 trap).
    static const bool s_auto = (std::getenv("DC2_G400_FROM") == nullptr);
    static const uint64_t s_ticks = g400EnvU64("DC2_G400_TICKS", 3ull);
    static uint64_t s_armed = ~0ull;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    if (s_auto)
    {
        if (s_armed == ~0ull)
        {
            if (fbp != 0x068u)
                return;
            s_armed = tick;
        }
        if (tick > s_armed + s_ticks)
            return;
    }
    else
    {
        static const uint64_t s_from = g400EnvU64("DC2_G400_FROM", 0ull);
        static const uint64_t s_to = g400EnvU64("DC2_G400_TO", s_from + 4ull);
        if (tick < s_from || tick > s_to)
            return;
    }

    static uint32_t s_last = 0xFFFFFFFFu;
    static uint64_t s_lastTick = ~0ull;
    static uint32_t s_step = 0u;
    if (tick != s_lastTick)
    {
        s_lastTick = tick;
        s_step = 0u;
        s_last = 0xFFFFFFFFu;
    }
    if (fbp == s_last)
        return;
    const uint32_t prev = s_last;
    s_last = fbp;

    double meanL = 0.0, meanA = 0.0, meanL68 = 0.0, meanA68 = 0.0;
    uint32_t maxL = 0u, maxL68 = 0u;
    g400SampleSurface(vram, 0x139u, meanL, meanA, maxL);
    g400SampleSurface(vram, 0x068u, meanL68, meanA68, maxL68);
    std::fprintf(stderr,
                 "[G400:stage] tick=%llu step=%u prev=0x%03x next=0x%03x "
                 "s139 meanL=%.2f maxL=%u meanA=%.2f | s068 meanL=%.2f maxL=%u meanA=%.2f\n",
                 static_cast<unsigned long long>(tick), s_step++,
                 (prev == 0xFFFFFFFFu) ? 0xFFFu : prev, fbp, meanL, maxL, meanA,
                 meanL68, maxL68, meanA68);
    std::fflush(stderr);
}
