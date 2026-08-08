// G526 — PER-PRIMITIVE PATH ATTRIBUTION (diagnostic only, default-off: DC2_G526_DRAW=1).
//
// WHY THIS EXISTS
// ---------------
// G525 measured the same quantity on eleven scenes and found exactly one that tracks frame time:
// the price of a GIF draw, 0.24 us (lean MAP-0) -> 8.90 us (Dungeon 6), a 37x range. On Georama
// that term is 31.2 ms/f = 69% of the whole frame. But it was derived as `[G332] front` MINUS the
// flush -- a RESIDUAL -- and G505/G507 forbid ranking with a residual. G521's `G144Entry` state
// table was the primary task for TEN phases and was refuted before its first edit for exactly this
// reason. So: split the residual into named scopes, THEN build.
//
// WHAT IT SPLITS
// --------------
// `[G147:gif] draw` wraps GSRasterizer::drawPrimitive. Every call leaves by one of four exits, and
// the cost of the four is not remotely alike. This census times the WHOLE call and books it to the
// exit it actually took, so the four columns sum to `[G147] draw` by construction:
//
//   rej    -- G401 reserved-PSM early return (should be ~0)
//   cull   -- G458 scissor cull: the primitive is provably empty and returns before any capture
//   defer  -- captured into g_g144List (the tile-bin fast path) and returned
//   inline -- NOT deferrable: drains the pending graph, runs the ownership/materialize edge, and
//             rasterizes on THIS thread. This is the one that can cost microseconds.
//
// DC2_G526_SPLIT=1 adds the sub-laps inside `defer` and `inline` (see the G526Sub enum). Keep it
// off for the reconciliation run -- the extra clock pairs are ~4 more per draw.
//
// INSTRUMENT HYGIENE (G505/G513/G502)
// -----------------------------------
//  * Accumulators are thread_local and non-atomic: the pole is ONE thread and an atomic RMW per
//    draw would be a self-inflicted cost on the very path being priced.
//  * The report prints a `self` row -- the measured cost of the instrument's own clock reads --
//    so a reader can tell attribution from self-cost without re-deriving it (G312's trap: the
//    whole suspected per-draw pole turned out to be DC2_PERF's own instrument).
//  * Counts are printed next to every time, because a price is COUNT x COST (G508) and a bucket
//    total divided by the wrong population is how G511 turned 6.7 ns/entry into 144.
//  * Everything is CUMULATIVE. De-cumulate against two `[G154] n=` lines exactly as G525 §11 says.
//  * With the flag unset this file contributes one predicted-not-taken branch per draw and no
//    clock reads at all.
// ⭐⭐ G524's CONVERSION LAW MADE MEASURABLE. A ms/f span on the GS worker is a price only to the
// extent it is NOT inside a blocking call: adding CPU work there costs ~0.7x, deleting blocked
// GPU-wait returns ~0.1x. A wall-clock lap cannot tell the two apart, and the G526 §1 pass produced
// exactly the ambiguity that predicts — `raster` at 25 ms/f while the PC sampler showed no leaf
// above 0.6% and 42.6% of samples outside the exe. So pair every wall lap with the thread's own
// retired-cycle count: cycles/ns near the core clock means real occupancy, near zero means the
// thread was parked. Declared by hand rather than by including <windows.h>, which this TU does not
// pull in (see the G447 note in ps2_gs_rasterizer.cpp).
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void *Thread,
                                                                   unsigned long long *CycleTime);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);

namespace
{

static const bool g_g526Draw = (std::getenv("DC2_G526_DRAW") != nullptr);
static const bool g_g526Split = (std::getenv("DC2_G526_SPLIT") != nullptr);
// DC2_G526_CYC=1 adds the retired-cycle pair to the inline raster lap (the only bucket big enough
// for the question to matter). Separate flag: QueryThreadCycleTime is ~50 ns and must not be paid
// on the 7,900-draw/frame path just to price 20 of them.
static const bool g_g526Cyc = (std::getenv("DC2_G526_CYC") != nullptr);

inline uint64_t g526Cycles()
{
    unsigned long long c = 0ull;
    if (g_g526Cyc)
        QueryThreadCycleTime(GetCurrentThread(), &c);
    return static_cast<uint64_t>(c);
}

enum G526Exit
{
    kG526Rej = 0,   // G401 reserved TEX0.PSM
    kG526Cull,      // G458 scissor-disjoint
    kG526Defer,     // captured into g_g144List
    kG526Inline,    // rasterized on the calling thread
    kG526ExitN
};

// Sub-laps. `defer` and `inline` share the prologue lap (entry -> eligibility decided).
enum G526Sub
{
    kG526Pro = 0,   // drawPrimitive entry -> deferral eligibility computed
    kG526Sw,        // defer: target-switch batch close / mid-flush drain
    kG526Build,     // defer: bbox + G144Entry field fill
    kG526Dec,       // defer: g149BuildDecoded (per-triangle texture decode)
    kG526Note,      // defer: g260NoteAppend + g336NoteConsumer (page-range accumulation)
    kG526Push,      // defer: g_g144List.push_back
    kG526Drain,     // inline: g260ExecuteAll / g144TimedMidFlush
    kG526Edge,      // inline: g278/g289/g272/g277/g276/g261/g242 ownership+materialize edge
    kG526Raster,    // inline: the drawSprite/drawTriangle/drawLine call itself
    kG526SubN
};

static const char *const kG526ExitName[kG526ExitN] = {"rej", "cull", "defer", "inline"};
static const char *const kG526SubName[kG526SubN] = {
    "pro", "sw", "build", "dec", "note", "push", "drain", "edge", "raster"};

struct G526Acc
{
    uint64_t exitNs[kG526ExitN];
    uint64_t exitN[kG526ExitN];
    uint64_t subNs[kG526SubN];
    uint64_t subN[kG526SubN];
    uint64_t selfNs;    // instrument's own clock-read cost, measured (not modelled)
    uint64_t reports;
    uint64_t subCyc[kG526SubN]; // retired cycles per sub-lap (DC2_G526_CYC=1); see the header note
    // Per-exit primitive-type histogram, so `inline` can be named rather than guessed.
    uint64_t inlineType[8];
    uint64_t cullType[8];
};
static thread_local G526Acc t_g526{};

// Live state for the primitive currently inside drawPrimitive.
static thread_local std::chrono::steady_clock::time_point t_g526T0{};
static thread_local std::chrono::steady_clock::time_point t_g526Mark{};
static thread_local uint32_t t_g526Exit = kG526Inline;
static thread_local uint32_t t_g526Type = 0u;
static thread_local bool t_g526Live = false;
// Key of the primitive currently on the inline path (filled at the eligibility point, consumed by
// the scope destructor once its total span is known).
static thread_local uint32_t t_g526IFbp = 0u, t_g526IPsm = 0u, t_g526ITme = 0u, t_g526IAbe = 0u;
static thread_local int32_t t_g526ITgt = -1;
static thread_local uint64_t t_g526IPx = 0u;
static thread_local uint64_t t_g526CycMark = 0u;

inline uint64_t g526Ns(std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Measure the clock pair's own cost once every report window, on the same thread and the same
// clock, so `self` is this host's real number rather than an assumed 20-25 ns.
static void g526MeasureSelf()
{
    const auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < 64; ++i)
    {
        const auto x = std::chrono::steady_clock::now();
        (void)x;
    }
    const auto b = std::chrono::steady_clock::now();
    t_g526.selfNs = g526Ns(a, b) / 64u; // ns per single clock read
}

static void g526ReportInline(); // G526 §2 table, defined below g526Report

static void g526Report()
{
    uint64_t totNs = 0, totN = 0;
    for (int i = 0; i < kG526ExitN; ++i)
    {
        totNs += t_g526.exitNs[i];
        totN += t_g526.exitN[i];
    }
    g526MeasureSelf();
    // Clock reads per draw: 2 for the outer pair, plus the sub-laps when DC2_G526_SPLIT is on.
    const double readsPerDraw = g_g526Split ? 6.0 : 2.0;
    std::fprintf(stderr,
                 "[G526:draw] n=%llu totalMs=%.1f | rej=%llu/%.1f cull=%llu/%.1f defer=%llu/%.1f "
                 "inline=%llu/%.1f | usEach(cull=%.2f defer=%.2f inline=%.2f) "
                 "selfNs/read=%llu selfMs=%.1f\n",
                 (unsigned long long)totN, totNs / 1e6,
                 (unsigned long long)t_g526.exitN[kG526Rej], t_g526.exitNs[kG526Rej] / 1e6,
                 (unsigned long long)t_g526.exitN[kG526Cull], t_g526.exitNs[kG526Cull] / 1e6,
                 (unsigned long long)t_g526.exitN[kG526Defer], t_g526.exitNs[kG526Defer] / 1e6,
                 (unsigned long long)t_g526.exitN[kG526Inline], t_g526.exitNs[kG526Inline] / 1e6,
                 t_g526.exitN[kG526Cull] ? (double)t_g526.exitNs[kG526Cull] / 1e3 /
                                               (double)t_g526.exitN[kG526Cull] : 0.0,
                 t_g526.exitN[kG526Defer] ? (double)t_g526.exitNs[kG526Defer] / 1e3 /
                                                (double)t_g526.exitN[kG526Defer] : 0.0,
                 t_g526.exitN[kG526Inline] ? (double)t_g526.exitNs[kG526Inline] / 1e3 /
                                                 (double)t_g526.exitN[kG526Inline] : 0.0,
                 (unsigned long long)t_g526.selfNs,
                 (double)totN * readsPerDraw * (double)t_g526.selfNs / 1e6);
    // The primitive-type histogram of the two exits that are not the fast path. GS_PRIM: 0=point
    // 1=line 2=linestrip 3=tri 4=tristrip 5=trifan 6=sprite.
    std::fprintf(stderr,
                 "[G526:type] inline(pt,ln,ls,tri,ts,tf,spr)=%llu,%llu,%llu,%llu,%llu,%llu,%llu | "
                 "cull=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 (unsigned long long)t_g526.inlineType[0], (unsigned long long)t_g526.inlineType[1],
                 (unsigned long long)t_g526.inlineType[2], (unsigned long long)t_g526.inlineType[3],
                 (unsigned long long)t_g526.inlineType[4], (unsigned long long)t_g526.inlineType[5],
                 (unsigned long long)t_g526.inlineType[6],
                 (unsigned long long)t_g526.cullType[0], (unsigned long long)t_g526.cullType[1],
                 (unsigned long long)t_g526.cullType[2], (unsigned long long)t_g526.cullType[3],
                 (unsigned long long)t_g526.cullType[4], (unsigned long long)t_g526.cullType[5],
                 (unsigned long long)t_g526.cullType[6]);
    if (g_g526Split)
    {
        std::fprintf(stderr, "[G526:split]");
        for (int i = 0; i < kG526SubN; ++i)
            std::fprintf(stderr, " %s=%.1f/%llu", kG526SubName[i], t_g526.subNs[i] / 1e6,
                         (unsigned long long)t_g526.subN[i]);
        std::fprintf(stderr, "\n");
        if (g_g526Cyc)
        {
            // cyc/ns near the core clock (~4.0 on this class of host) = real occupancy; near 0 =
            // the thread was parked inside a blocking call and G524's 0.1x return rate applies.
            std::fprintf(stderr, "[G526:cyc]");
            for (int i = 0; i < kG526SubN; ++i)
                std::fprintf(stderr, " %s=%.2f", kG526SubName[i],
                             t_g526.subNs[i] ? (double)t_g526.subCyc[i] / (double)t_g526.subNs[i]
                                             : 0.0);
            std::fprintf(stderr, "\n");
        }
    }
    g526ReportInline();
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// G526 §2 — NAME THE INLINE POPULATION.
//
// The coarse pass proved the whole per-primitive residual is ~20 primitives/frame on Georama
// (1,448 us each) and ~4/frame on MAP-0 (317 us each). A price is COUNT x COST (G508) and both
// halves are now known -- what is NOT known is WHICH primitives they are, and that decides whether
// the fix is "make them deferrable", "make them cheaper", or "make them rarer" (G522's law).
//
// So: key every inline primitive by the fields that decide its eligibility and its raster cost,
// and accumulate count + ns + pixel area per distinct key. Small fixed table, linear probe, no
// allocation, only ever touched on a path that already costs hundreds of microseconds.
struct G526InlineKey
{
    uint32_t type;
    uint32_t fbp;
    uint32_t fpsm;
    uint32_t tme;
    uint32_t abe;
    int32_t targetIdx;  // g248TargetIndex(fbp): >=0 means a known RTT target
    uint64_t n;
    uint64_t ns;
    uint64_t px;        // summed scissor-clipped bbox area
};
static thread_local G526InlineKey t_g526Inl[24]{};
static thread_local uint32_t t_g526InlN = 0u;

static void g526NoteInline(uint32_t type, uint32_t fbp, uint32_t fpsm, uint32_t tme, uint32_t abe,
                           int32_t targetIdx, uint64_t ns, uint64_t px)
{
    for (uint32_t i = 0; i < t_g526InlN; ++i)
    {
        G526InlineKey &k = t_g526Inl[i];
        if (k.type == type && k.fbp == fbp && k.fpsm == fpsm && k.tme == tme && k.abe == abe)
        {
            ++k.n;
            k.ns += ns;
            k.px += px;
            return;
        }
    }
    if (t_g526InlN >= 24u)
        return;
    G526InlineKey &k = t_g526Inl[t_g526InlN++];
    k.type = type; k.fbp = fbp; k.fpsm = fpsm; k.tme = tme; k.abe = abe;
    k.targetIdx = targetIdx; k.n = 1u; k.ns = ns; k.px = px;
}

static void g526ReportInline()
{
    for (uint32_t i = 0; i < t_g526InlN; ++i)
    {
        const G526InlineKey &k = t_g526Inl[i];
        std::fprintf(stderr,
                     "[G526:inl] type=%u fbp=0x%x fpsm=0x%x tme=%u abe=%u tgt=%d | n=%llu "
                     "totalMs=%.1f usEach=%.1f pxEach=%llu nsPerPx=%.2f\n",
                     k.type, k.fbp, k.fpsm, k.tme, k.abe, k.targetIdx,
                     (unsigned long long)k.n, k.ns / 1e6,
                     k.n ? (double)k.ns / 1e3 / (double)k.n : 0.0,
                     (unsigned long long)(k.n ? k.px / k.n : 0u),
                     k.px ? (double)k.ns / (double)k.px : 0.0);
    }
    std::fflush(stderr);
}

// Outer scope: one clock pair per drawPrimitive, booked to whichever exit was taken.
struct G526DrawScope
{
    G526DrawScope(uint32_t primType)
    {
        if (!g_g526Draw)
            return;
        t_g526Live = true;
        t_g526Exit = kG526Inline; // fall-through default; the early exits overwrite it
        t_g526Type = primType & 7u;
        t_g526T0 = std::chrono::steady_clock::now();
        t_g526Mark = t_g526T0;
        t_g526CycMark = g526Cycles();
    }
    ~G526DrawScope()
    {
        if (!g_g526Draw)
            return;
        const uint64_t d = g526Ns(t_g526T0, std::chrono::steady_clock::now());
        t_g526.exitNs[t_g526Exit] += d;
        ++t_g526.exitN[t_g526Exit];
        if (t_g526Exit == kG526Inline)
        {
            ++t_g526.inlineType[t_g526Type];
            g526NoteInline(t_g526Type, t_g526IFbp, t_g526IPsm, t_g526ITme, t_g526IAbe,
                           t_g526ITgt, d, t_g526IPx);
        }
        else if (t_g526Exit == kG526Cull)
            ++t_g526.cullType[t_g526Type];
        t_g526Live = false;
        const uint64_t tot = t_g526.exitN[kG526Rej] + t_g526.exitN[kG526Cull] +
                             t_g526.exitN[kG526Defer] + t_g526.exitN[kG526Inline];
        if ((tot % 40000ull) == 0ull)
            g526Report();
    }
};

inline void g526SetExit(uint32_t which)
{
    if (g_g526Draw)
        t_g526Exit = which;
}

// Record the identity of a primitive that just failed the deferral test. Called with plain
// integers so this file needs no GS type visibility; the caller does the scissor clip because it
// already has the vertices in registers.
inline void g526SetInlineKey(uint32_t fbp, uint32_t fpsm, uint32_t tme, uint32_t abe,
                             int32_t tgt, uint64_t px)
{
    if (!g_g526Draw)
        return;
    t_g526IFbp = fbp;
    t_g526IPsm = fpsm;
    t_g526ITme = tme;
    t_g526IAbe = abe;
    t_g526ITgt = tgt;
    t_g526IPx = px;
}

// Close one sub-lap: books [t_g526Mark, now) to `which` and re-arms the mark. Only the SPLIT flag
// pays for these; the coarse reconciliation run must not carry four extra clock reads per draw.
inline void g526Lap(uint32_t which)
{
    if (!g_g526Draw || !g_g526Split || !t_g526Live)
        return;
    const auto now = std::chrono::steady_clock::now();
    t_g526.subNs[which] += g526Ns(t_g526Mark, now);
    ++t_g526.subN[which];
    t_g526Mark = now;
    if (g_g526Cyc)
    {
        const uint64_t c = g526Cycles();
        t_g526.subCyc[which] += (c > t_g526CycMark) ? (c - t_g526CycMark) : 0ull;
        t_g526CycMark = c;
    }
}

// Re-arm without booking (used to skip a region that belongs to no bucket).
inline void g526Mark()
{
    if (g_g526Draw && g_g526Split)
        t_g526Mark = std::chrono::steady_clock::now();
}

} // namespace
