bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool writeDepth;
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false, false};
        case 2: // ZB_ONLY
            return {false, true, false};
        case 3: // RGB_ONLY
            return {true, false, true};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        if (sourcePsm == GS_PSM_T4 ||
            sourcePsm == GS_PSM_T4HL ||
            sourcePsm == GS_PSM_T4HH)
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);
            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        else if ((sourcePsm == GS_PSM_T8 || sourcePsm == GS_PSM_T8H) && csm == 0u)
        {
            // F62: T8H shares the T8 CSM1 CLUT swizzle (spec-correct; the old code
            // only swizzled plain T8).
            clutIndex = swizzleClutIndexCSM1(clutIndex);
        }

        return clutIndex;
    }

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    // G45: private off-screen Z buffer for the costume model RTT (fbp=0x139). The game's Z block
    // (0x1a00) aliases menu text VRAM in the runner's flat 4MB, so storing model Z there both
    // corrupts the text and is corrupted BY the per-frame text sprites -> model flicker. This
    // buffer aliases nothing; it is cleared fully once per costume frame (costumeZClear) and the
    // model's GEQUAL test/write route here. 512x512 covers the whole RTT at native res.
    constexpr uint32_t kCostumeZW = 512u;
    constexpr uint32_t kCostumeZH = 512u;
    inline std::vector<uint32_t> &costumeZBuf()
    {
        static std::vector<uint32_t> buf(kCostumeZW * kCostumeZH, 0u);
        return buf;
    }
    inline uint32_t costumeZRead(int x, int y)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return 0u;
        return costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)];
    }
    inline void costumeZWrite(int x, int y, uint32_t z)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return;
        costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)] = z;
    }
    inline void costumeZClear()
    {
        std::fill(costumeZBuf().begin(), costumeZBuf().end(), 0u);
    }

    int wrapTextureCoordinate(int coordinate, uint8_t mode, int size, int regionMin, int regionMax)
    {
        switch (mode)
        {
        case 0: // REPEAT
            return coordinate & (size - 1);
        case 1: // CLAMP
            return clampInt(coordinate, 0, size - 1);
        case 2: // REGION_CLAMP
            return clampInt(coordinate, regionMin, regionMax);
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) &
                                     static_cast<uint32_t>(regionMin)) |
                                    static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    // ===== G529: the bilinear lerp's final rounding is a CRT CALL, four per pixel =====
    //
    // THE MEASUREMENT (G529 §3). `[G529:spr]` books every sprite drawn inside a band replay to the
    // path it took, with its own pixel count and elapsed time:
    //
    //   bucket   n        px        summed lane ms   ns/px   bilinear
    //   fast     157,753  170.1 M   67,648           397.8   157,753  (100%)
    //   slow      85,107   14.5 M    3,668           252.8         0
    //   other     37,143   60.3 M    7,430           123.3         0
    //
    // The G248/G250 hoisted sampler is 86% of all replayed-sprite CPU on Georama, and EVERY sprite
    // that reaches it is BILINEAR — so every pixel runs four g248SamplePoint taps and then four
    // lerpChannel calls, and each of those ends in `std::lround`.
    //
    // `std::lround(float)` is `lroundf`: it must round HALF AWAY FROM ZERO, which SSE's
    // cvtss2si (round-to-nearest-EVEN) does not do, so MSVC cannot fold it to one instruction and
    // emits a CRT call. That is four CRT calls per pixel. G519 already established the law that a
    // CRT/DLL leaf row IS a priceable call class in this rasterizer (it removed 58,400 ceilf/floorf
    // per frame for -0.337 ms/f); this is the same class, two orders of magnitude more of it.
    //
    // ⭐ THE REPLACEMENT IS PROVABLY EXACT, not approximately equal.
    // `top` and `bottom` are convex combinations of channel values in [0,255] with fx in [0,1), so
    // both lie in [0,255]; `v = top + (bottom-top)*fy` is a convex combination of those with
    // fy in [0,1), so v is in [0,255] and in particular **v >= 0**. For v >= 0, "round half away
    // from zero" IS "round half up", i.e. floor(v + 1/2). Computing it as:
    //
    //     int n = (int)v;            // cvttss2si -- truncation, exact for 0 <= v <= 255
    //     float frac = v - (float)n; // EXACT (Sterbenz: n <= v < n+1, same binade or adjacent)
    //     if (frac >= 0.5f) ++n;
    //
    // reproduces lroundf bit-for-bit on this domain while never forming `v + 0.5f` (which could
    // round UP across the tie and change the answer for values just below x.5). Four instructions,
    // no call, no libm.
    //
    //   DC2_G529_FASTLERP=1  retained bring-up spelling; now a no-op (PROMOTED DEFAULT-ON by G609)
    //   DC2_G529_NO_FASTLERP=1  the rollback
    //   DC2_G529_LERPVFY=1   ORACLE. Runs BOTH forms per channel and counts disagreements. Any
    //                        nonzero `bad` means the identity above is violated on this host.
    //   DC2_G529_NOLERP=1    CEILING PROBE, KNOWINGLY INCORRECT OUTPUT, NEVER PROMOTE. Returns the
    //                        c00 tap unfiltered, deleting the entire bilinear filter. Bounds every
    //                        lever that could ever be built here (g511_probe_bounds_the_gate).
    //
    // ⭐ PROMOTED DEFAULT-ON BY G609. Rollback: DC2_G529_NO_FASTLERP=1.
    // ⭐⭐⭐ WHY IT TOOK TWO PHASES: G529 built this, proved it exact, and measured it at 0.00 ms/f —
    // on a WHOLE-FRAME gate, which cannot resolve a sub-ms lever (`g594_frame_gate_cannot_resolve_
    // sub_ms`). G609 built `[G605:leaf] cyc/inside` denominators and a stage CEILING PROBE, and the
    // probe said the four `lerpChannel` calls are **50.1%** of the admitted triangle pixel
    // (`DC2_G609_CEIL=6`: 526.7 -> 262.9 cyc/inside) against only 8.1% for the four VRAM taps. The
    // lever did not change; the INSTRUMENT did.
    // Gates:
    //   mechanism  — `[G605:leaf]`, `DC2_G605_CYC=1` only, both arms. On `s05`, `discov sprite`
    //                (90.0% of that route's replay CPU) goes **691.5 -> 428.7 cyc/inside (-38.0%)**
    //                on IDENTICAL denominators (n=669,462, px=599,236,608 in both arms), i.e.
    //                130,204.8 -> 80,737.7 ms of LOOP wall. On `s03` the same leaf goes
    //                59.5 -> 40.4 (-32.1%).
    //   exactness  — `DC2_G529_LERPVFY=1`: **bad=0 on 1,677,721,600 channel roundings**, both forms
    //                computed per channel. Independently, G609's row oracle compared the kernel's
    //                inlined copy of this identity against the shipped `std::lround` path and
    //                returned **bad=0 on 687,795,440 per-pixel colour+depth comparisons**.
    //   payoff     — order-balanced A B B A on `s05` (the GS-worker-bound route), two estimators
    //                agreeing to 0.002 ms/f: `[G154:perf]` **-0.274 ms/f (-1.27%)** se 0.039 and
    //                `[G146:frame]` **-0.276 (-1.27%)** se 0.036. FOUR order blocks, all negative
    //                (-0.331 / -0.223 / -0.329 / -0.221).
    //   distribution — `s05` windows over 33.33 ms **21 -> 13**; p95 33.85 -> 33.26.
    // ⚠️ On `s03` the same lever is NOT A RESULT at the frame (blocks disagree, +0.021 / -0.304)
    // even though its mechanism is confirmed there too — because post-G609 `s03` is **77.4%
    // VU1-poled**, so GS-side wall removed in a VU1-poled window buys nothing
    // (`g593_frame_is_max_vu1_gs`). Gate a GS lever on a GS-poled route.
    inline bool g529FastLerpOn()
    {
        static const bool on = !envFlagEnabled("DC2_G529_NO_FASTLERP");
        return on;
    }
    inline bool g529LerpVfyOn()
    {
        static const bool on = envFlagEnabled("DC2_G529_LERPVFY");
        return on;
    }
    inline bool g529NoLerpOn()
    {
        static const bool on = envFlagEnabled("DC2_G529_NOLERP");
        return on;
    }
    std::atomic<uint64_t> g_g529LerpTot{0}, g_g529LerpBad{0};

    // floor(v + 1/2) for v >= 0, without ever forming v + 0.5f. See the derivation above.
    inline int g529RoundHalfUpNonNeg(float v)
    {
        const int n = static_cast<int>(v);
        return (v - static_cast<float>(n) >= 0.5f) ? (n + 1) : n;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        // ⛔ ceiling probe first: it must delete the work, not merely reorder it.
        if (g529NoLerpOn())
            return c00;
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        const float v = top + (bottom - top) * fy;
        if (!g529FastLerpOn())
            return clampU8(static_cast<int>(std::lround(v)));
        const uint8_t fast = clampU8(g529RoundHalfUpNonNeg(v));
        if (g529LerpVfyOn())
        {
            const uint8_t ref = clampU8(static_cast<int>(std::lround(v)));
            g_g529LerpTot.fetch_add(1u, std::memory_order_relaxed);
            if (ref != fast)
            {
                const uint64_t bad = g_g529LerpBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (bad <= 16u)
                    std::fprintf(stderr, "[G529:lerpvfy] MISMATCH v=%.9g ref=%u fast=%u\n",
                                 static_cast<double>(v), (unsigned)ref, (unsigned)fast);
            }
            if ((g_g529LerpTot.load(std::memory_order_relaxed) & 0x3FFFFFFull) == 0ull)
                std::fprintf(stderr, "[G529:lerpvfy] tot=%llu bad=%llu\n",
                             (unsigned long long)g_g529LerpTot.load(std::memory_order_relaxed),
                             (unsigned long long)g_g529LerpBad.load(std::memory_order_relaxed));
            return ref; // the oracle arm ships the REFERENCE value, so a defect cannot reach pixels
        }
        return fast;
    }
}

// G143 (perf, DEFAULT OFF): persistent worker pool that rasterizes DISJOINT scanline ranges of a
// single triangle concurrently. A dispatch (`run`) splits [y0,y1] into `lanes` contiguous chunks;
// lane 0 runs on the CALLER, lanes 1..lanes-1 on workers; `run` blocks until all lanes finish, so
// the triangle is fully drawn before drawTriangle returns (cross-triangle order preserved). Single
// caller only (the guest thread inside a VU1 XGKICK) — dispatches never overlap. No allocation on
// the hot path. Deadlock-safe: workers sleep on a generation counter; the barrier is a pending
// countdown. Correct/exact ONLY when the lane job touches disjoint pixels + thread-local scratch.
// G420: defined in g420_pack_residue.inc, which this TU includes much later. Selects the narrowed
// dispatch below (and lets the G419 within-process instrument alternate it per frame).
static bool g420PoolNarrowArm();

// ===== G638: per-CALL-SITE census of the legacy equal-band barrier (DIAGNOSIS ONLY, default OFF) =====
//
// PREMISE. G637 replaced the equal-band barrier with a cooperative work-stealing engine for ONE of
// the seven `GSRowPool::run` call sites (the CPU band replay). The other six — readback unpack,
// VRAM materialization x3, texture decode, and the g143 row split — still cut equal rows and still
// BLOCK their caller. When the caller is the GS worker and `gsStallMs/f == 0.00`, that block is
// booked to `GS own` in full, which on a GS-poled route is frame time (PS2_PROJECT_STATE.md's
// "Delete Work, Not Waiting" G637 exception). No instrument has ever said WHICH site owns it:
// `[G446:gsprof]` names `GSRowPool::run` as a wait CALLER but cannot see who called `run`.
//
// This buckets by `_ReturnAddress()`, so no call site has to change and no site can be forgotten.
// Per bucket it separates the three terms that select different levers:
//   wall = the whole dispatch as the caller sees it
//   lane = the caller's own band (real work — only deleting work shrinks it)
//   wait = wall - lane = the caller ASLEEP at the join (what a scheduling lever can delete)
// Prints preferred-base addresses (`pref=`), which resolve against build64/Release/dc2_runner.map
// without paying DbgHelp's tens of seconds on this 155 MB image (G563's law).
//
// Arm with DC2_G638_POOL=1. Every hook is a no-op otherwise; the env read is cached at namespace
// scope, never function-local (MSVC would emit a per-access TLS `_Init_global_epoch` guard).
#include <intrin.h>

static const bool g_g638PoolCensusOn = (std::getenv("DC2_G638_POOL") != nullptr);

struct G638PoolSite
{
    std::atomic<uint64_t> pref{0ull};
    std::atomic<uint64_t> calls{0ull};
    std::atomic<uint64_t> wallNs{0ull};
    std::atomic<uint64_t> laneNs{0ull};
    std::atomic<uint64_t> rows{0ull};
};
static constexpr uint32_t kG638PoolSites = 64u;
static G638PoolSite g_g638PoolSites[kG638PoolSites];
static std::atomic<uint64_t> g_g638PoolCalls{0ull};

// Return address -> the address the .map lists (the image is ASLR-relocated at load).
// `__ImageBase` is the linker-provided base of THIS module, so no <windows.h> is needed here —
// this .inc is pulled into ps2_gs_rasterizer.cpp long before any Win32 header.
extern "C" char __ImageBase[];
static uint64_t g638PrefOf(uint64_t rip)
{
    const uint64_t base = reinterpret_cast<uint64_t>(&__ImageBase[0]);
    return (rip >= base) ? (rip - base + 0x140000000ull) : rip;
}

static void g638PoolReport()
{
    struct Row { uint64_t pref, calls, wall, lane, rows; };
    Row rows[kG638PoolSites];
    uint32_t n = 0u;
    uint64_t totWall = 0ull, totWait = 0ull;
    for (uint32_t i = 0; i < kG638PoolSites; ++i)
    {
        const uint64_t c = g_g638PoolSites[i].calls.load(std::memory_order_relaxed);
        if (c == 0ull)
            continue;
        rows[n].pref = g_g638PoolSites[i].pref.load(std::memory_order_relaxed);
        rows[n].calls = c;
        rows[n].wall = g_g638PoolSites[i].wallNs.load(std::memory_order_relaxed);
        rows[n].lane = g_g638PoolSites[i].laneNs.load(std::memory_order_relaxed);
        rows[n].rows = g_g638PoolSites[i].rows.load(std::memory_order_relaxed);
        totWall += rows[n].wall;
        totWait += (rows[n].wall > rows[n].lane) ? (rows[n].wall - rows[n].lane) : 0ull;
        ++n;
    }
    // Rank by WAIT, not wall: wall includes the caller's own real work, which no scheduling
    // lever can delete. The lever this census selects is paid out of `wait`.
    for (uint32_t i = 1u; i < n; ++i)
        for (uint32_t j = i; j > 0u; --j)
        {
            const uint64_t a = (rows[j].wall > rows[j].lane) ? rows[j].wall - rows[j].lane : 0ull;
            const uint64_t b = (rows[j - 1].wall > rows[j - 1].lane) ? rows[j - 1].wall - rows[j - 1].lane : 0ull;
            if (a <= b)
                break;
            const Row t = rows[j]; rows[j] = rows[j - 1]; rows[j - 1] = t;
        }
    std::fprintf(stderr,
                 "[G638:pool] sites=%u calls=%llu cumWallMs=%.1f cumWaitMs=%.1f (cumulative; "
                 "de-cumulate a window against the nearest [G146:frame] n=)\n",
                 n, (unsigned long long)g_g638PoolCalls.load(std::memory_order_relaxed),
                 totWall / 1e6, totWait / 1e6);
    for (uint32_t i = 0; i < n; ++i)
    {
        const double wall = rows[i].wall / 1e6;
        const double lane = rows[i].lane / 1e6;
        const double wait = (wall > lane) ? (wall - lane) : 0.0;
        std::fprintf(stderr,
                     "[G638:pool]   pref=0x%llx calls=%llu wallMs=%.1f laneMs=%.1f waitMs=%.1f "
                     "(%5.2f%% of wait) us/call=%.2f rows/call=%.1f\n",
                     (unsigned long long)rows[i].pref, (unsigned long long)rows[i].calls,
                     wall, lane, wait,
                     totWait ? 100.0 * wait * 1e6 / (double)totWait : 0.0,
                     rows[i].calls ? (wall * 1000.0 / (double)rows[i].calls) : 0.0,
                     rows[i].calls ? ((double)rows[i].rows / (double)rows[i].calls) : 0.0);
    }
}

static void g638PoolNote(uint64_t rip, uint64_t wallNs, uint64_t laneNs, uint64_t rowCount)
{
    const uint64_t pref = g638PrefOf(rip);
    uint32_t idx = static_cast<uint32_t>((pref >> 4) ^ (pref >> 13)) & (kG638PoolSites - 1u);
    for (uint32_t probe = 0; probe < kG638PoolSites; ++probe)
    {
        uint64_t have = g_g638PoolSites[idx].pref.load(std::memory_order_relaxed);
        if (have == 0ull)
        {
            uint64_t expect = 0ull;
            if (!g_g638PoolSites[idx].pref.compare_exchange_strong(expect, pref,
                                                                   std::memory_order_relaxed))
                have = expect;
            else
                have = pref;
        }
        if (have == pref)
        {
            g_g638PoolSites[idx].calls.fetch_add(1ull, std::memory_order_relaxed);
            g_g638PoolSites[idx].wallNs.fetch_add(wallNs, std::memory_order_relaxed);
            g_g638PoolSites[idx].laneNs.fetch_add(laneNs, std::memory_order_relaxed);
            g_g638PoolSites[idx].rows.fetch_add(rowCount, std::memory_order_relaxed);
            break;
        }
        idx = (idx + 1u) & (kG638PoolSites - 1u);
    }
    const uint64_t t = g_g638PoolCalls.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((t % 20000ull) == 0ull)
        g638PoolReport();
}

// ===== G638b: PHASE lap of the CPU-fallback dispatch window (DIAGNOSIS ONLY, default OFF) =====
//
// PREMISE. On `dragon`'s static tail `[G529:cls]` books ONE `legacy` dispatch per frame at
// 9,957 us, and `[G290:probe]`'s `replay` bucket at 12.04 ms/f — but `[G638:pool]` measures the
// GSRowPool::run inside it at ~0.13 ms. So ~9.8 ms/f of the GS pole is inside `g529BeginDispatch()
// .. g529EndDispatch()` and is NOT the parallel rasterization. `[G529:cls]`'s own `zclr`/`scan`
// laps are 1.0 / 66.8 ms CUMULATIVE over a whole run, so they are not it either, and the shared-
// CLUT build is refuted by its own census (`[G580:clut] keys=1..2`, so the linear key scan runs
// 1-2 compares per entry and the 256-entry fill runs once or twice per batch).
//
// ⛔ The G529 header comment calls its `wall` "GSRowPool::run, caller-observed". That is STALE:
// `g529T` is reset after the bbox scan and read again after the whole `if (y1 >= y0)` body, so its
// `wall` also contains g570 preparation, CLUT selection, the G629 admission walk and the G570 GPU
// batch block. G637 §1's "rt = wall - lane0 = GS-thread SLEEP" inherits that overstatement.
// `[G638:pool]` brackets the pool itself and is the instrument to quote for the barrier.
//
// Arm with DC2_G638_PREP=1. Cumulative ns per phase; de-cumulate against `[G146:frame] n=`.
static const bool g_g638PrepCensusOn = (std::getenv("DC2_G638_PREP") != nullptr);
enum G638Phase : int
{
    kG638PhG570Prep = 0,   // g570Cpu139LinearBatchCandidate + g570Batch139Prepare + depth tiles
    kG638PhClut = 1,       // g580/g605 shared-CLUT selection + build (incl. the eligibility walk)
    kG638PhG629 = 2,       // g629CeilSkipBatch + g629TryReplayGpu admission walk
    kG638PhPool = 3,       // g630PrepareCpuReplay + g637RunReplay (the parallel rasterization)
    kG638PhG570Gpu = 4,    // the g570GpuBatch settle/read/replay/write block
    kG638PhTail = 5,       // g629VerifyAfterCpu + g629NoteBatch + g529EndDispatch
    // Sub-laps of kG638PhG570Prep, charged from inside g570Batch139Prepare. They are NOT summed
    // into the total below (that would double-count) — read them against `g570prep`.
    kG638PhG570Tris = 6,     // the per-triangle validate + fixed-point setup + span push loop
    kG638PhG570Payload = 7,  // g570Batch139BuildGpuPayload (tile binning + GPU buffer fill)
    kG638PhCount = 8,
    kG638PhSummed = 6        // phases [0, kG638PhSummed) partition the dispatch window
};
static const char *const kG638PhaseName[kG638PhCount] = {
    "g570prep", "clut", "g629admit", "poolreplay", "g570gpubatch", "tail",
    "  ^tris", "  ^payload"};
static std::atomic<uint64_t> g_g638PhaseNs[kG638PhCount];
static std::atomic<uint64_t> g_g638PhaseBatches{0ull};

static void g638PrepReport()
{
    uint64_t tot = 0ull;
    for (int i = 0; i < kG638PhSummed; ++i)
        tot += g_g638PhaseNs[i].load(std::memory_order_relaxed);
    const uint64_t n = g_g638PhaseBatches.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[G638:prep] batches=%llu totalMs=%.1f (cumulative)\n",
                 (unsigned long long)n, tot / 1e6);
    for (int i = 0; i < kG638PhCount; ++i)
    {
        const uint64_t v = g_g638PhaseNs[i].load(std::memory_order_relaxed);
        std::fprintf(stderr, "[G638:prep]   %-13s %9.1f ms (%5.2f%%) %8.1f us/batch\n",
                     kG638PhaseName[i], v / 1e6, tot ? 100.0 * (double)v / (double)tot : 0.0,
                     n ? (double)v / 1e3 / (double)n : 0.0);
    }
}

static void g638PrepNote(int phase, uint64_t ns)
{
    if (phase >= 0 && phase < kG638PhCount)
        g_g638PhaseNs[phase].fetch_add(ns, std::memory_order_relaxed);
}

static void g638PrepEndBatch()
{
    const uint64_t n = g_g638PhaseBatches.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((n % 400ull) == 0ull)
        g638PrepReport();
}

// ===== G653 P7 / P29: cooperative-dispatch diagnostics — COMPILE-TIME OPT-IN =====
//
// P7 needed a WORK / WAIT / BARRIER decomposition of `runCoop()`: `[G638:pool]` brackets only the
// LEGACY `run()` path, so the promoted G637 cooperative dispatch had none, and "poolreplay is 69.8%
// of the flush" could not distinguish rasterization from scheduling (rule 9: delete work, not
// waiting). The census was built, it answered the question, and its numbers are in the G653 fix log.
//
// ⛔⛔ IT IS NOT IN THE DEFAULT BUILD, AND THIS IS THE MEASURED REASON.
// The first G653 attempt kept the census hooks and a P29 spin-dispatch prototype as ordinary
// runtime-flagged code in this file — every hook guarded by a `const bool`, the prototype
// DEFAULT-OFF, behaviour byte-for-byte unchanged. An order-balanced A B B A between that binary and
// the one before it, on `dragon`, 2,700 matched presents per arm and no drift:
//
//     block 1 +0.591 ms/f    block 2 +0.377 ms/f    POOLED +0.484 ms/f (GS own +0.443)
//
// Nothing behavioural changed. That 0.484 ms/f is PURE CODE LAYOUT — PS2_PROJECT_STATE.md rule 12b,
// reproduced on a lever that does nothing. ⭐ And no env rollback can find it: every rollback
// restores behaviour and none removes a byte. So the diagnostics leave the default build entirely.
//
// Build with -DPS2X_G653_POOLDIAG=ON to get `[G653:coop]` (DC2_G653_POOLCOOP=1) and the P29
// spin-dispatch prototype (DC2_G653_POOLSPIN_US=<us>) back.
#if defined(PS2X_G653_POOLDIAG)
extern const bool g653PoolCoopOn;
extern std::atomic<uint64_t> g_g653CoopCalls;
extern std::atomic<uint64_t> g_g653CoopSerialCalls;
extern std::atomic<uint64_t> g_g653CoopWallNs;
extern std::atomic<uint64_t> g_g653CoopSetupNs;
extern std::atomic<uint64_t> g_g653CoopCallerNs;
extern std::atomic<uint64_t> g_g653CoopChunkNs;
extern std::atomic<uint64_t> g_g653CoopBarrierNs;
extern std::atomic<uint64_t> g_g653CoopWakeNs;
extern std::atomic<uint64_t> g_g653CoopWakeSamples;
extern std::atomic<uint64_t> g_g653CoopChunks;
extern std::atomic<uint64_t> g_g653CoopCallerChunks;
extern std::atomic<uint64_t> g_g653CoopSeats;
extern std::atomic<uint64_t> g_g653CoopMaxChunkNs;
void g653PoolCoopReport();

static inline uint64_t g653NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ===== G653 P29: the spin budget for the cooperative dispatch's wake and join =====
//
// Namespace scope, never a function-local `static const` — MSVC compiles the latter into a
// per-ACCESS `_Init_thread_header` epoch check, and this is read on every worker wake and every
// join (G619's law; the same trap G649 fixed in `pushDispatchPc` and G651 in
// `shouldPreemptGuestExecution`).
//
// ⛔ SPINNING IS NOT FREE ON THIS HOST. Six physical cores already carry EE + VU1 + GS + GL + seven
// pool threads, and G419's `rbspin` arm measured +0.170 ms/f for spinning on a thread that was
// going to block anyway. The budget is therefore BOUNDED and armed by measurement, not by
// principle: it must show up as a frame win on an uncapped GS-poled route or it does not ship.
#define G653_PAUSE() _mm_pause()

// ⛔ DEFAULT OFF, and the arithmetic says why it has to earn its default. `dragon` runs ~12
// dispatches per 27 ms frame, so the mean gap between dispatches is ~2.2 ms — two orders of
// magnitude longer than any sane spin budget. Most spins will therefore burn their WHOLE budget
// and then sleep anyway, at 7 workers x budget of core time per dispatch. The mechanism can only
// win if the wake latency it deletes (17.5 us, P7) is worth more than the cores it steals from
// EE / VU1 / GL on a 6-core host. That is a measurement, not a prediction — arm it with
// DC2_G653_POOLSPIN_US=<us>; DC2_G653_NO_POOLSPIN=1 forces it off once/if it is ever promoted.
static const uint32_t g_g653PoolSpinUs = [] () -> uint32_t {
    if (std::getenv("DC2_G653_NO_POOLSPIN") != nullptr)
        return 0u;
    const char *v = std::getenv("DC2_G653_POOLSPIN_US");
    if (!v || !*v)
        return 0u;
    const long n = std::strtol(v, nullptr, 10);
    if (n <= 0)
        return 0u;
    return static_cast<uint32_t>(n > 1000 ? 1000 : n);
}();

#endif // PS2X_G653_POOLDIAG
// In the DEFAULT build nothing above exists and every call site below is `#if`-excluded too, so the
// emitted `ps2_gs_rasterizer.obj` is the pre-G653 one. That is the point: a `const bool` guard
// leaves the BYTES in place, and the measurement above says the bytes are what cost 0.484 ms/f.

class GSRowPool
{
public:
    static GSRowPool &instance()
    {
        static GSRowPool p;
        return p;
    }
    int laneCap() const { return static_cast<int>(m_threads.size()) + 1; }

    void run(int y0, int y1, int lanes, const std::function<void(int, int)> &job)
    {
        // G638 (no-op unless DC2_G638_POOL=1): read the CALL SITE first, before any branch can
        // change the frame, so each of the seven `run` sites is attributed to itself.
        const uint64_t g638Rip =
            g_g638PoolCensusOn ? reinterpret_cast<uint64_t>(_ReturnAddress()) : 0ull;
        const std::chrono::steady_clock::time_point g638T0 =
            g_g638PoolCensusOn ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
        const uint64_t g638Rows =
            (y1 >= y0) ? static_cast<uint64_t>(y1 - y0 + 1) : 0ull;
        lanes = std::max(1, std::min(lanes, laneCap()));
        if (lanes <= 1 || y1 < y0)
        {
            if (y1 >= y0)
                job(y0, y1);
            if (g_g638PoolCensusOn)
            {
                // Serial path: the caller does all of it, so wall == lane and wait is zero. It
                // still has to be booked, or a site that mostly takes this path looks free.
                const uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g638T0).count());
                g638PoolNote(g638Rip, ns, ns, g638Rows);
            }
            return;
        }
        const int total = y1 - y0 + 1;
        auto bound = [&](int i) { return y0 + static_cast<int>(static_cast<int64_t>(total) * i / lanes); };
        // G420 narrowed dispatch. The legacy join waits for EVERY worker to wake, take the mutex,
        // find no lane left and report, so a `lanes`-way split still pays an m_threads.size()-way
        // convoy — which is why the lane count could never move G419's fixed ~0.085 ms dispatch.
        // Narrowed, m_pending counts LANES (exactly lanes-1 of them are handed to workers) and each
        // worker subtracts the number it took, so the join waits only for threads that did work.
        // Surplus workers still wake, but nobody blocks on them.
        //
        // SAFETY: the legacy form is what guarantees no worker is still inside the lane loop when
        // the next generation is armed (run returns only after all of them reported). The narrowed
        // form gives that up, so the lane loop re-checks m_gen under the same mutex the dispatcher
        // holds while it bumps m_gen and resets m_nextLane. A late worker therefore cannot steal a
        // lane from the next generation or run it with stale y0/total/lanes/job.
        const bool narrow = g420PoolNarrowArm();

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_coop = false; // G637: this generation is a legacy equal-band dispatch
            m_job = &job;
            m_y0 = y0;
            m_total = total;
            m_lanes = lanes;
            m_narrow = narrow;
            m_nextLane.store(1, std::memory_order_relaxed);
            m_pending = narrow ? (lanes - 1)                        // lanes actually handed out
                               : static_cast<int>(m_threads.size()); // every worker reports
            ++m_gen;
#if defined(PS2X_G653_POOLDIAG)
            // G653 P29: publish the wake hint INSIDE the lock, after every field a woken worker
            // reads. A spinner that observes it therefore observes all of them.
            m_genA.store(m_gen, std::memory_order_release);
#endif
        }
        m_cvWork.notify_all();

        // Caller runs lane 0 itself (no wasted thread).
        job(bound(0), bound(1) - 1);
        const std::chrono::steady_clock::time_point g638TLane =
            g_g638PoolCensusOn ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};

        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [&] { return m_pending == 0; });
            m_job = nullptr;
        }
        if (g_g638PoolCensusOn)
        {
            const auto now = std::chrono::steady_clock::now();
            g638PoolNote(g638Rip,
                         static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(now - g638T0).count()),
                         static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(g638TLane - g638T0).count()),
                         g638Rows);
        }
    }

    // ===== G637: cooperative, over-decomposed, work-stealing dispatch =====
    //
    // ⭐⭐⭐ WHY run() ABOVE CANNOT BE FIXED IN PLACE. It splits [y0,y1] into `lanes` EQUAL ROW bands
    // and blocks the caller after lane 0. Equal rows are not equal work: `[G529:disp]` on `ridepod`
    // measures the caller's own band at 0.318 ms against a 1.692 ms mean WORKER band (5.3x) while
    // the dispatch wall is 3.45 ms — i.e. the join waits for ONE unlucky band and the GS thread,
    // which is that route's SOLE pole, spends 6.47 of every 7.12 ms/f of replay wall asleep. No
    // choice of `lanes` repairs that, because the defect is the STATIC EQUAL-ROW PARTITION, not the
    // lane count (G529 already refuted the lane-count lever in the direction it was built).
    //
    // runCoop() takes an explicit, caller-computed partition — `bounds[0..chunks]`, a strictly
    // increasing row fence with bounds[0] == y0 and bounds[chunks] == y1+1 — and lets EVERY
    // participant, the caller included, claim chunks from ONE shared cursor until the batch is
    // exhausted. Over-decomposition (chunks > lanes) plus dynamic claiming converts the join from
    // "wait for the worst band" into "wait for the worst CHUNK", and the caller keeps rasterizing
    // instead of sleeping.
    //
    // ⭐ EXACTNESS IS THE SAME ARGUMENT run() ALREADY RELIES ON, unchanged: every chunk is a
    // contiguous row range, the chunks are disjoint and cover [y0,y1] exactly once, and the job
    // replays the WHOLE entry list in submission order clipped to its own range. Disjoint rows =>
    // disjoint framebuffer/Z writes; in-order inside a range => blend/Z order preserved. Nothing
    // about WHICH pixels a row produces depends on how the rows were grouped.
    //
    // The cursor packs generation<<32 | index into ONE atomic, so a worker still finishing the
    // PREVIOUS generation can never claim a chunk of the next one — it fails the generation test and
    // leaves. That is G420's `m_gen`-re-check-under-the-mutex guarantee without a mutex round trip
    // per claim, which matters here precisely because there are now many more claims than lanes.
    //
    // `seats` bounds how many WORKERS may join (the caller is always a participant), so the total
    // concurrency stays exactly what the lane policy asked for even though the pool owns more
    // threads than that.
    void runCoop(const int *bounds, int chunks, int seats,
                 const std::function<void(int, int)> &job,
                 uint32_t *outCallerChunks = nullptr)
    {
        if (outCallerChunks)
            *outCallerChunks = 0u;
        if (!bounds || chunks <= 0)
            return;
        if (chunks == 1 || seats <= 0 || m_threads.empty())
        {
            // Same serial path run() takes at lanes<=1: no notify_all, no join, no worker wake.
            for (int i = 0; i < chunks; ++i)
                if (bounds[i + 1] - 1 >= bounds[i])
                    job(bounds[i], bounds[i + 1] - 1);
            if (outCallerChunks)
                *outCallerChunks = static_cast<uint32_t>(chunks);
#if defined(PS2X_G653_POOLDIAG)
            if (g653PoolCoopOn)
                g_g653CoopSerialCalls.fetch_add(1ull, std::memory_order_relaxed);
#endif
            return;
        }

#if defined(PS2X_G653_POOLDIAG)
        const uint64_t g653T0 = g653PoolCoopOn ? g653NowNs() : 0ull;
#endif

        uint32_t gen;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_coop = true;
            m_coopBounds = bounds;
            m_coopChunks = chunks;
            m_coopJob = &job;
            m_coopRemaining.store(chunks, std::memory_order_relaxed);
            m_coopSeats.store(seats, std::memory_order_relaxed);
            gen = static_cast<uint32_t>(++m_gen);
            m_coopCursor.store(static_cast<uint64_t>(gen) << 32, std::memory_order_release);
#if defined(PS2X_G653_POOLDIAG)
            m_genA.store(m_gen, std::memory_order_release); // G653 P29 wake hint
#endif
        }
#if defined(PS2X_G653_POOLDIAG)
        if (g653PoolCoopOn)
            m_g653DispatchNs.store(g653T0, std::memory_order_release);
        // G653 P29: `notify_all` with seven waiters is a kernel call and P7 measured the dispatcher
        // paying 20.8 us for the publish-plus-notify pair. If at least `seats` workers are already
        // spinning on the generation mirror they have ALREADY been told, so the syscall buys
        // nothing. Any worker that is genuinely asleep is not counted, so this can only skip the
        // notify when it is redundant — and a worker whose spin budget expires between the count
        // and the skip still re-checks `m_gen` under the mutex before waiting, so it cannot miss
        // the generation, only take the slow path to it.
        if (g_g653PoolSpinUs == 0u || m_g653Spinning.load(std::memory_order_acquire) < seats)
#endif
        m_cvWork.notify_all();
#if defined(PS2X_G653_POOLDIAG)
        const uint64_t g653TSetup = g653PoolCoopOn ? g653NowNs() : 0ull;
#endif

        uint32_t mine = 0;
        for (;;)
        {
            const int idx = coopClaim(gen, chunks);
            if (idx < 0)
                break;
            ++mine;
            coopRunChunk(idx, bounds, &job);
        }
        if (outCallerChunks)
            *outCallerChunks = mine;
#if defined(PS2X_G653_POOLDIAG)
        const uint64_t g653TClaims = g653PoolCoopOn ? g653NowNs() : 0ull;

        // G653 P29: the JOIN half. P7 measured `barrier` at 33.2 us/dispatch, 21.0% of the wall -
        // the caller parked at the fence after its own claims ran out. The chunks it is waiting on
        // are already RUNNING (the cursor is exhausted before the caller ever gets here), so the
        // remaining wait is bounded by ONE chunk's tail, not by a scheduling decision. Spinning
        // that out avoids a condvar sleep/wake pair whose latency is the same ~17 us the worker
        // wake costs, and whose length is comparable to what it is waiting for.
        if (g_g653PoolSpinUs > 0u && m_coopRemaining.load(std::memory_order_acquire) != 0)
        {
            const uint64_t deadline = g653NowNs() + g_g653PoolSpinUs * 1000ull;
            while (m_coopRemaining.load(std::memory_order_acquire) != 0)
            {
                if (g653NowNs() >= deadline)
                    break;
                G653_PAUSE();
            }
        }
#endif
        if (m_coopRemaining.load(std::memory_order_acquire) != 0)
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [&] { return m_coopRemaining.load(std::memory_order_acquire) == 0; });
        }
#if defined(PS2X_G653_POOLDIAG)
        if (g653PoolCoopOn)
        {
            const uint64_t g653TEnd = g653NowNs();
            // `setup` is the mutex publish plus notify_all; `caller` is this thread's own claim
            // loop (its real rasterization AND its claim CAS overhead); `barrier` is what it spent
            // parked at the join once its own claims ran out. Those three partition `wall` for the
            // CALLER. Worker work is recovered in the reporter from the summed chunk time.
            g_g653CoopWallNs.fetch_add(g653TEnd - g653T0, std::memory_order_relaxed);
            g_g653CoopSetupNs.fetch_add(g653TSetup - g653T0, std::memory_order_relaxed);
            g_g653CoopCallerNs.fetch_add(g653TClaims - g653TSetup, std::memory_order_relaxed);
            g_g653CoopBarrierNs.fetch_add(g653TEnd - g653TClaims, std::memory_order_relaxed);
            g_g653CoopChunks.fetch_add(static_cast<uint64_t>(chunks), std::memory_order_relaxed);
            g_g653CoopCallerChunks.fetch_add(mine, std::memory_order_relaxed);
            g_g653CoopSeats.fetch_add(static_cast<uint64_t>(seats), std::memory_order_relaxed);
            const uint64_t n = g_g653CoopCalls.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
            if ((n % 20000ull) == 0ull)
                g653PoolCoopReport();
        }
#endif
        // Every claimed chunk has COMPLETED (that is what the countdown counts), so no worker is
        // inside the job body and the caller's `bounds`/`job` may now die with this frame.
    }

private:
    // Lock-free claim. Returns the chunk index, or -1 when this generation is exhausted or has
    // already been superseded (a late worker from the previous dispatch).
    int coopClaim(uint32_t gen, int chunks)
    {
        uint64_t cur = m_coopCursor.load(std::memory_order_acquire);
        for (;;)
        {
            if (static_cast<uint32_t>(cur >> 32) != gen)
                return -1;
            const uint32_t idx = static_cast<uint32_t>(cur);
            if (idx >= static_cast<uint32_t>(chunks))
                return -1;
            if (m_coopCursor.compare_exchange_weak(cur,
                                                   (static_cast<uint64_t>(gen) << 32) | (idx + 1u),
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                return static_cast<int>(idx);
        }
    }

    // Runs one claimed chunk and reports it. The countdown must be decremented for EVERY claimed
    // chunk including an empty one, or the join never reaches zero.
    void coopRunChunk(int idx, const int *bounds, const std::function<void(int, int)> *job)
    {
        const int a = bounds[idx];
        const int b = bounds[idx + 1] - 1;
#if defined(PS2X_G653_POOLDIAG)
        const uint64_t g653T = g653PoolCoopOn ? g653NowNs() : 0ull;
#endif
        if (job && b >= a)
            (*job)(a, b);
#if defined(PS2X_G653_POOLDIAG)
        if (g653PoolCoopOn)
        {
            // Summed across EVERY participant, so this is aggregate work, not a timeline. The
            // widest single chunk is tracked separately: an over-decomposed dispatch is only
            // tail-safe while max(chunk) stays small against the wall.
            const uint64_t dt = g653NowNs() - g653T;
            g_g653CoopChunkNs.fetch_add(dt, std::memory_order_relaxed);
            uint64_t hi = g_g653CoopMaxChunkNs.load(std::memory_order_relaxed);
            while (dt > hi &&
                   !g_g653CoopMaxChunkNs.compare_exchange_weak(hi, dt, std::memory_order_relaxed))
            {
            }
        }
#endif
        if (m_coopRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            // Taken so the waiter cannot evaluate its predicate between our store and its wait.
            std::lock_guard<std::mutex> lk(m_mtx);
            m_cvDone.notify_all();
        }
    }

private:
    GSRowPool()
    {
        unsigned hc = std::thread::hardware_concurrency();
        int n = (hc > 1u) ? static_cast<int>(hc) - 1 : 0;
        n = std::min(n, 15);
        for (int i = 0; i < n; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }
    ~GSRowPool()
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
            ++m_gen;
#if defined(PS2X_G653_POOLDIAG)
            // G653 P29: mirror both, so a spinning worker leaves its spin immediately at shutdown
            // instead of burning its whole budget first.
            m_stopA.store(true, std::memory_order_release);
            m_genA.store(m_gen, std::memory_order_release);
#endif
        }
        m_cvWork.notify_all();
        for (auto &t : m_threads)
            if (t.joinable())
                t.join();
    }
    void workerLoop()
    {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lk(m_mtx);
        for (;;)
        {
            // ===== G653 P29: SPIN BEFORE SLEEPING =====
            //
            // P7 measured the cooperative dispatch's own overhead at 34% of its wall on `dragon`:
            // setup 20.8 us/dispatch (the mutex publish plus `notify_all`) and barrier 33.2
            // us/dispatch, with a worker WAKE LATENCY of 17.5 us. None of that is rasterization,
            // and PS2_PROJECT_STATE.md rule 9 says only work reductions convert - but this IS the
            // work of the dispatch itself, paid ~12 times a frame.
            //
            // `notify_all` on a condvar with seven waiters is a kernel call that wakes seven
            // threads; the wake alone is the 17.5 us. A worker that is already SPINNING on the
            // generation counter sees the new dispatch in tens of nanoseconds instead, and - the
            // larger half - lets the dispatcher SKIP `notify_all` entirely when every seat it
            // wants is already spinning.
            //
            // ⭐ EXACTNESS IS UNCHANGED AND UNCHANGEABLE HERE. This alters only WHEN a worker
            // notices a generation, never which rows a chunk covers, which entries a chunk
            // replays, or the order of writes inside it. `runCoop`'s contiguous / disjoint /
            // in-order argument is untouched, and every claim still goes through the same
            // generation-tagged CAS.
            //
            // Rollback: DC2_G653_NO_POOLSPIN=1. Budget: DC2_G653_POOLSPIN_US.
#if defined(PS2X_G653_POOLDIAG)
            if (g_g653PoolSpinUs > 0u && !m_stop && m_gen == seen)
            {
                lk.unlock();
                m_g653Spinning.fetch_add(1, std::memory_order_acq_rel);
                const uint64_t deadline = g653NowNs() + g_g653PoolSpinUs * 1000ull;
                while (m_genA.load(std::memory_order_acquire) == seen &&
                       !m_stopA.load(std::memory_order_acquire))
                {
                    if (g653NowNs() >= deadline)
                        break;
                    G653_PAUSE();
                }
                m_g653Spinning.fetch_sub(1, std::memory_order_acq_rel);
                lk.lock();
            }
#endif
            m_cvWork.wait(lk, [&] { return m_stop || m_gen != seen; });
            if (m_stop)
                return;
            seen = m_gen;
            if (m_coop)
            {
                // G637 cooperative generation. Take a seat if the lane policy left one; a worker
                // that misses out simply goes back to sleep, so the pool's thread count never
                // widens the concurrency the caller asked for.
                if (m_coopSeats.fetch_sub(1, std::memory_order_relaxed) <= 0)
                    continue;
                const uint32_t cgen = static_cast<uint32_t>(seen);
                const int chunks = m_coopChunks;
                const int *bounds = m_coopBounds;
                const std::function<void(int, int)> *cjob = m_coopJob;
                lk.unlock();
#if defined(PS2X_G653_POOLDIAG)
                if (g653PoolCoopOn)
                {
                    // G653 P7: WAKE LATENCY - dispatch instant to the moment a seated worker is
                    // actually running. This is the part `barrier` cannot see: if wake dominates,
                    // the condvar handoff (not the partition and not the kernel) owns the span.
                    const uint64_t t0 = m_g653DispatchNs.load(std::memory_order_acquire);
                    const uint64_t now = g653NowNs();
                    if (t0 != 0ull && now > t0)
                    {
                        g_g653CoopWakeNs.fetch_add(now - t0, std::memory_order_relaxed);
                        g_g653CoopWakeSamples.fetch_add(1ull, std::memory_order_relaxed);
                    }
                }
#endif
                for (;;)
                {
                    const int idx = coopClaim(cgen, chunks);
                    if (idx < 0)
                        break;
                    coopRunChunk(idx, bounds, cjob);
                }
                lk.lock();
                continue;
            }
            const std::function<void(int, int)> *job = m_job;
            const int y0 = m_y0, total = m_total, lanes = m_lanes;
            const bool narrow = m_narrow;
            int took = 0;
            for (;;)
            {
                // G420: m_gen is read under the same mutex the dispatcher holds while it bumps the
                // generation and resets m_nextLane, so a worker that is late out of the PREVIOUS
                // generation cannot take a lane belonging to the next one. Inert for the legacy
                // form (which cannot produce a late worker) and load-bearing for the narrowed one.
                if (m_gen != seen)
                    break;
                const int lane = m_nextLane.fetch_add(1, std::memory_order_relaxed);
                if (lane >= lanes)
                    break;
                const int a = y0 + static_cast<int>(static_cast<int64_t>(total) * lane / lanes);
                const int b = y0 + static_cast<int>(static_cast<int64_t>(total) * (lane + 1) / lanes) - 1;
                lk.unlock();
                if (job && b >= a)
                    (*job)(a, b);
                lk.lock();
                ++took;
            }
            if (narrow)
            {
                // Exactly lanes-1 lanes exist and every one is taken by some worker, so the
                // subtractions sum to the initial m_pending and the countdown always reaches 0.
                if (took > 0 && (m_pending -= took) == 0)
                    m_cvDone.notify_one();
            }
            else if (--m_pending == 0)
            {
                m_cvDone.notify_one();
            }
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mtx;
    std::condition_variable m_cvWork, m_cvDone;
    const std::function<void(int, int)> *m_job = nullptr;
    int m_y0 = 0, m_total = 0, m_lanes = 1, m_pending = 0;
    bool m_narrow = false; // G420: join waits only for workers that took a lane
    std::atomic<int> m_nextLane{0};
    uint64_t m_gen = 0;
    bool m_stop = false;
#if defined(PS2X_G653_POOLDIAG)
    // G653 P29: lock-free MIRRORS of the two words a sleeping worker waits on, so a spinning
    // worker can observe a new dispatch without holding m_mtx. Both are written under m_mtx (or
    // immediately after it, with release ordering) and only ever read by the spin loop, which then
    // re-acquires m_mtx and re-reads the authoritative `m_gen` / `m_stop`. The mirrors are a
    // WAKE HINT, never the source of truth — that is what keeps G420's
    // "re-check m_gen under the mutex" guarantee intact.
    std::atomic<uint64_t> m_genA{0};
    std::atomic<bool> m_stopA{false};
    std::atomic<int> m_g653Spinning{0};
#endif
    // G637 cooperative dispatch state. All four scalars are written by the dispatcher under m_mtx
    // and read by a worker under m_mtx at its wake, so they need no atomicity of their own; the
    // cursor and the two counters are the lock-free part.
    bool m_coop = false;
    const int *m_coopBounds = nullptr;
    int m_coopChunks = 0;
    const std::function<void(int, int)> *m_coopJob = nullptr;
    std::atomic<uint64_t> m_coopCursor{0}; // generation<<32 | next chunk index
    std::atomic<int> m_coopRemaining{0};   // chunks not yet COMPLETED
    std::atomic<int> m_coopSeats{0};       // worker seats still available this generation
#if defined(PS2X_G653_POOLDIAG)
    // G653 P7/P29: dispatch timestamp for the wake-latency sample, plus the lock-free mirrors and
    // spinner count. Absent from the default build (see the header on PS2X_G653_POOLDIAG).
    std::atomic<uint64_t> m_g653DispatchNs{0};
#endif
};
