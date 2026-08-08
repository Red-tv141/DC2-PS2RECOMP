// ===== ⛔ G527 `lineclip` — REFUTED BY ITS OWN CENSUS, COMPILED OUT (DC2_G527_LINECLIP_ARM 0) =====
//
// ⛔ DO NOT REBUILD THIS LEVER. Set DC2_G527_LINECLIP_ARM to 1 only to re-run the census below.
// At 0 the shipped drawLine is byte-for-byte the pre-G527 body; there is no added per-step compare.
//
// THE HYPOTHESIS (wrong). drawLine steps from the raw v0 -> v1 in screen space and relies on
// writePixel to reject every out-of-scissor pixel, so a line whose endpoints sit far off-screen
// should walk thousands of iterations that write nothing. `[G526:inl]` priced Georama's linestrips
// at 1.59 + 1.65 ms/f over 464 written pixels each — **1.57 us of CPU per written pixel** — and the
// G277-R comment names "the offscreen sceGsSwapDBuff sync linestrips at x ~ 1792..2303", which is
// exactly that shape. The inference was that the walk is mostly off-screen.
//
// ⛔ THE REFUTATION. `[G527:line]` (DC2_G527_LINESTAT=1) counts steps WALKED against steps the
// unclipped walk would take. On Georama, over **14,000 lines / 6,496,000 steps**:
//
//     [G527:line] calls=14000 stepsWalked=6496000 stepsUnclipped=6496000
//                 ratio=1.0000 wholeLinesSkipped=0
//
// **The clip skips nothing. Every one of the 464 steps is inside the scissor** (G458 already culls
// the fully-offscreen ones — `cull` is 3,978 draws/f at 0.06 us). So the 1.57 us/px is not walk
// length, and no amount of clipping can recover it.
//
// ⭐ WHERE THE TIME ACTUALLY IS — `[G526:split]` with `tgtreg` armed reads, for the inline exit,
// **drain 2074.3 ms / edge 290.4 / raster 40.2**. An inline primitive's cost is the mid-frame graph
// DRAIN it forces, not its own rasterization: `raster` is under 2% of it. That also explains why
// the same linestrips fell from 3.24 ms/f to **0.69 ms/f** merely because `tgtreg` changed what was
// pending when they drained — the cost was never theirs. The live lever for this class is therefore
// `DC2_G270_LINE_WAVE=1` (already implemented, default-off since G270 closed it on a pixel-COVERAGE
// argument), not anything inside this function. See plans/phase-G527-fix-log.md §5.
//
// The clip itself is exact — DC2_G527_LINEVFY=1 compared **6,496,000 emitted pixels against the
// original unclipped walk with bad=0** — so this is a refutation on PAYOFF, not on correctness.
//
// THE FIX. Bresenham is an affine recurrence, so the state at any step k has a closed form and the
// walk can start and stop at the first/last step that can possibly write. With
//   m = max(|dx|,|dy|) = totalSteps,  n = min(|dx|,|dy|)
// the MAJOR axis advances by exactly 1 every step (it needs m increments in m steps), so
//   major(k) = major0 + s_major * k
// and the MINOR axis advances on a standard DDA schedule
//   c_k  = number of minor increments after k steps = (2*n*k + m) / (2*m)     [integer floor]
//   minor(k) = minor0 + s_minor * c_k
// The error accumulator follows from the code's own update rule (an x-increment adds dy, a
// y-increment adds dx), so with dx = |Dx| >= 0 and dy = -|Dy| <= 0 as the function computes them:
//   err_k = (dx + dy) + dx * (#y-increments in k steps) + dy * (#x-increments in k steps)
// Both coordinates are monotone in k, so each scissor bound gives one contiguous k-interval; their
// intersection with [0, m] is the only range that can write. Everything outside is skipped without
// being simulated.
//
// Under a banded replay the write predicate is `y in band AND (x,y) in scissor`, so the y bound is
// clipped to scissor ∩ band — which makes the 8-lane replay cost proportional to each lane's own
// rows instead of every lane re-walking the whole line (the G270 comment's "every band replays the
// whole Bresenham walk" is exactly the cost being removed here).
//
// EXACTNESS. The emitted pixel sequence is identical by construction: the same (x, y, step,
// totalSteps) tuples in the same order, hence the same t = step/totalSteps and the same colours.
// It is proved in-process rather than argued: DC2_G527_LINEVFY=1 runs the ORIGINAL unclipped walk
// alongside, records every pixel each version would emit, and compares them element by element
// ([G527:linevfy] bad must stay 0) — the oracle discipline of
// g512_oracle_must_compare_against_the_replaced_body.
//
// Disabled when phase diagnostics are on: writePixel's [F30:px.enter]/[F30:px.scissCull] counters
// observe the REJECTED pixels, so a diagnostic build must keep walking them.
//
// Levers (inert at ARM 0): DC2_G527_LINECLIP=1 · DC2_G527_NO_LINECLIP=1 ·
// DC2_G527_LINEVFY=1 (per-pixel oracle) · DC2_G527_LINESTAT=1 ([G527:line] census).

// G521 precedent: a refuted lever is COMPILED OUT, not merely defaulted off, so the shipped path
// cannot pay for it (here: one extra compare per Bresenham step and two std::abs in the prologue).
#ifndef DC2_G527_LINECLIP_ARM
#define DC2_G527_LINECLIP_ARM 0
#endif

#if DC2_G527_LINECLIP_ARM
static bool g527LineClipOn()
{
    static const bool on = envFlagEnabled("DC2_G527_LINECLIP") &&
                           !envFlagEnabled("DC2_G527_NO_LINECLIP");
    return on;
}

static bool g527LineVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G527_LINEVFY");
    return on;
}

static bool g527LineStatOn()
{
    static const bool on = envFlagEnabled("DC2_G527_LINESTAT");
    return on;
}

static std::atomic<uint64_t> g_g527LineCalls{0}, g_g527LineStepsWalked{0},
    g_g527LineStepsTotal{0}, g_g527LineSkippedWhole{0}, g_g527LineVfyTot{0}, g_g527LineVfyBad{0};

static inline int64_t g527CeilDiv(int64_t a, int64_t b) // b > 0
{
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) == (b < 0))) ? q + 1 : q;
}

struct G527LinePx
{
    int x, y;
    uint8_t r, g, b, a;
};

// Per-pixel oracle: the clipped walk must emit exactly the sequence the original unclipped walk
// emitted — same pixels, same order, same colours. Compares against the very body it replaces
// (g512_oracle_must_compare_against_the_replaced_body), never an independent re-derivation.
static void g527NoteLineVerify(const std::vector<G527LinePx> &ref,
                               const std::vector<G527LinePx> &got)
{
    const uint64_t tot =
        g_g527LineVfyTot.fetch_add(std::max(ref.size(), got.size()), std::memory_order_relaxed) +
        std::max(ref.size(), got.size());
    bool bad = ref.size() != got.size();
    for (size_t i = 0; !bad && i < ref.size(); ++i)
        bad = ref[i].x != got[i].x || ref[i].y != got[i].y || ref[i].r != got[i].r ||
              ref[i].g != got[i].g || ref[i].b != got[i].b || ref[i].a != got[i].a;
    if (bad)
    {
        const uint64_t n = g_g527LineVfyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
            std::fprintf(stderr, "[G527:linevfy] MISMATCH n=%llu ref=%zu got=%zu\n",
                         static_cast<unsigned long long>(n), ref.size(), got.size());
    }
    static std::atomic<uint64_t> s_tick{0};
    if (((s_tick.fetch_add(1u, std::memory_order_relaxed) + 1u) % 2000u) == 0u)
        std::fprintf(stderr, "[G527:linevfy] tot=%llu bad=%llu\n",
                     static_cast<unsigned long long>(tot),
                     static_cast<unsigned long long>(
                         g_g527LineVfyBad.load(std::memory_order_relaxed)));
}

static void g527LineReport()
{
    if (!g527LineStatOn())
        return;
    const uint64_t calls = g_g527LineCalls.load(std::memory_order_relaxed);
    const uint64_t walked = g_g527LineStepsWalked.load(std::memory_order_relaxed);
    const uint64_t total = g_g527LineStepsTotal.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G527:line] calls=%llu stepsWalked=%llu stepsUnclipped=%llu ratio=%.4f "
                 "wholeLinesSkipped=%llu\n",
                 static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(walked),
                 static_cast<unsigned long long>(total),
                 total ? static_cast<double>(walked) / static_cast<double>(total) : 0.0,
                 static_cast<unsigned long long>(
                     g_g527LineSkippedWhole.load(std::memory_order_relaxed)));
}
#endif // DC2_G527_LINECLIP_ARM

void GSRasterizer::drawLine(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

#if DC2_G527_LINECLIP_ARM
    // ---- G527: analytic clip of the walk ---------------------------------------------------
    // kHi defaults to "no bound" so the loop keeps its original termination condition when the
    // lever is off, when diagnostics observe rejected pixels, or when the closed form does not
    // apply (a degenerate single-point line).
    int kHi = std::numeric_limits<int>::max();
    const bool g527ClipActive = g527LineClipOn() && !phaseDiagnosticsEnabled() &&
                                (std::abs(x1 - x0) != 0 || std::abs(y1 - y0) != 0);
    const bool g527Vfy = g527ClipActive && g527LineVerifyOn();
    std::vector<G527LinePx> *g527Ref = nullptr;
    std::vector<G527LinePx> *g527Got = nullptr;
    if (g527Vfy)
    {
        // Record what the ORIGINAL unclipped walk would emit, from untouched state, before the
        // clipped walk runs. Thread-local so a banded replay's lanes cannot collide.
        static thread_local std::vector<G527LinePx> s_ref, s_got;
        s_ref.clear();
        s_got.clear();
        g527Ref = &s_ref;
        g527Got = &s_got;
        int rx = x0, ry = y0, rerr = err, rstep = 0;
        for (;;)
        {
            const float t = static_cast<float>(rstep) / static_cast<float>(totalSteps);
            uint8_t r, g, b, a;
            if (gs->m_prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
                g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
                b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
                a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
            }
            else
            {
                r = v1.r; g = v1.g; b = v1.b; a = v1.a;
            }
            const bool inBand = !t_g144Banded || (ry >= t_g144BandY0 && ry <= t_g144BandY1);
            const bool inSciss = rx >= ctx.scissor.x0 && rx <= ctx.scissor.x1 &&
                                 ry >= ctx.scissor.y0 && ry <= ctx.scissor.y1;
            if (inBand && inSciss && s_ref.size() < 65536u)
                s_ref.push_back(G527LinePx{rx, ry, r, g, b, a});
            if (rx == x1 && ry == y1)
                break;
            const int e2 = 2 * rerr;
            if (e2 >= dy) { rerr += dy; rx += sx; }
            if (e2 <= dx) { rerr += dx; ry += sy; }
            ++rstep;
        }
    }

    if (g527ClipActive)
    {
        const int64_t m = std::max(std::abs(x1 - x0), std::abs(y1 - y0)); // == totalSteps, > 0
        const int64_t n = std::min(std::abs(x1 - x0), std::abs(y1 - y0));
        const bool xMajor = std::abs(x1 - x0) >= std::abs(y1 - y0);

        // Write window: the scissor, narrowed to this lane's rows during a banded replay (the
        // write predicate below is exactly `in band AND in scissor`).
        int64_t wx0 = ctx.scissor.x0, wx1 = ctx.scissor.x1;
        int64_t wy0 = ctx.scissor.y0, wy1 = ctx.scissor.y1;
        if (t_g144Banded)
        {
            wy0 = std::max<int64_t>(wy0, t_g144BandY0);
            wy1 = std::min<int64_t>(wy1, t_g144BandY1);
        }

        int64_t kLo = 0, kHi64 = m;
        bool empty = (wx1 < wx0) || (wy1 < wy0);

        // Major axis: coord(k) = p0 + s*k, one increment per step.
        auto clipMajor = [&](int64_t p0, int64_t s, int64_t lo, int64_t hi) {
            if (s > 0) { kLo = std::max(kLo, lo - p0); kHi64 = std::min(kHi64, hi - p0); }
            else       { kLo = std::max(kLo, p0 - hi); kHi64 = std::min(kHi64, p0 - lo); }
        };
        // Minor axis: coord(k) = q0 + s*c_k with c_k = (2*n*k + m) / (2*m), monotone in k.
        auto clipMinor = [&](int64_t q0, int64_t s, int64_t lo, int64_t hi) {
            int64_t cLo, cHi;
            if (s > 0) { cLo = lo - q0; cHi = hi - q0; }
            else       { cLo = q0 - hi; cHi = q0 - lo; }
            if (n == 0)
            {
                // c_k == 0 for every k (floor(m / 2m) == 0): the minor coord never moves.
                if (cLo > 0 || cHi < 0)
                    empty = true;
                return;
            }
            // c_k >= cLo  <=>  2*n*k + m >= 2*m*cLo
            kLo = std::max(kLo, g527CeilDiv(2 * m * cLo - m, 2 * n));
            // c_k <= cHi  <=>  2*n*k + m < 2*m*(cHi + 1)
            kHi64 = std::min(kHi64, g527CeilDiv(2 * m * cHi + m, 2 * n) - 1);
        };

        if (xMajor)
        {
            clipMajor(x0, sx, wx0, wx1);
            clipMinor(y0, sy, wy0, wy1);
        }
        else
        {
            clipMajor(y0, sy, wy0, wy1);
            clipMinor(x0, sx, wx0, wx1);
        }
        if (kLo < 0) kLo = 0;
        if (kHi64 > m) kHi64 = m;

        if (g527LineStatOn())
        {
            g_g527LineStepsTotal.fetch_add(static_cast<uint64_t>(m) + 1u,
                                           std::memory_order_relaxed);
            g_g527LineStepsWalked.fetch_add(
                (empty || kHi64 < kLo) ? 0u : static_cast<uint64_t>(kHi64 - kLo + 1),
                std::memory_order_relaxed);
            if (empty || kHi64 < kLo)
                g_g527LineSkippedWhole.fetch_add(1u, std::memory_order_relaxed);
            // Accumulate BEFORE reporting, or the first line prints its own counters as zeroes.
            if (((g_g527LineCalls.fetch_add(1u, std::memory_order_relaxed) + 1u) % 2000ull) == 0ull)
                g527LineReport();
        }

        if (empty || kHi64 < kLo)
        {
            if (g527Vfy)
                g527NoteLineVerify(*g527Ref, *g527Got);
            return; // no step of this walk can write a pixel
        }

        if (kLo > 0)
        {
            // Fast-forward the recurrence to step kLo. c = minor increments performed by then.
            const int64_t c = (2 * n * kLo + m) / (2 * m);
            const int64_t nxInc = xMajor ? kLo : c; // x-increments applied
            const int64_t nyInc = xMajor ? c : kLo; // y-increments applied
            x0 += static_cast<int>(sx * nxInc);
            y0 += static_cast<int>(sy * nyInc);
            // An x-increment adds dy to err, a y-increment adds dx (the loop's own update rule).
            err = static_cast<int>(static_cast<int64_t>(dx + dy) +
                                   static_cast<int64_t>(dx) * nyInc +
                                   static_cast<int64_t>(dy) * nxInc);
            step = static_cast<int>(kLo);
        }
        kHi = static_cast<int>(kHi64);
    }
    // ---------------------------------------------------------------------------------------
#endif // DC2_G527_LINECLIP_ARM

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (gs->m_prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        // G270 CPU fallback: every band replays the whole Bresenham walk (so color/step state is
        // identical) but writes only its owned rows. Bands remain disjoint and submission order is
        // preserved within each band, matching the established G144 triangle/sprite contract.
        // G527: with the clip active the walk itself is already restricted to scissor ∩ band, so
        // this test only re-states what the k-range guarantees.
        if (!t_g144Banded || (y0 >= t_g144BandY0 && y0 <= t_g144BandY1))
        {
#if DC2_G527_LINECLIP_ARM
            if (g527Got != nullptr)
            {
                if (x0 >= ctx.scissor.x0 && x0 <= ctx.scissor.x1 &&
                    y0 >= ctx.scissor.y0 && y0 <= ctx.scissor.y1 &&
                    g527Got->size() < 65536u)
                    g527Got->push_back(G527LinePx{x0, y0, r, g, b, a});
            }
#endif
            writePixel(gs, x0, y0, r, g, b, a);
        }

        if (x0 == x1 && y0 == y1)
            break;
#if DC2_G527_LINECLIP_ARM
        if (step >= kHi)
            break;
#endif

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }

#if DC2_G527_LINECLIP_ARM
    if (g527Vfy)
        g527NoteLineVerify(*g527Ref, *g527Got);
#endif
}
