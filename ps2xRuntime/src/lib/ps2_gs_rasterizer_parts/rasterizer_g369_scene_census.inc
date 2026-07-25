// G369 — cutscene "gray screen" census (diagnostic only, default-off: DC2_G369_CENSUS=1).
//
// Symptom: during the first cutscene the 3-D scene disappears for a long run of frames
// (dumped display buffer is a uniform ~(10,10,10) fill) while the script/subtitles keep
// running. Two very different layers can produce that:
//   (a) the guest stops BUILDING the scene (no primitives reach the GS), or
//   (b) primitives are still submitted but land off-screen / degenerate (camera-matrix or
//       perspective-divide defect — the user's working theory).
// This census separates them: it counts triangles/sprites per present and accumulates their
// GS screen-space (12.4) bounding box plus how many were rejected as behind-camera (q<=0)
// or zero-area. Printed and reset from the frame-dump site in the present loop.
//
// Counters are only touched when the lever is on (one static bool test on a path that
// already does far more work per primitive), so the default build is unaffected.

std::atomic<bool> g_g369CensusOn{false};
std::atomic<uint64_t> g_g369Tris{0};
std::atomic<uint64_t> g_g369Sprites{0};
std::atomic<uint64_t> g_g369Behind{0};
std::atomic<uint64_t> g_g369Degenerate{0};
std::atomic<uint64_t> g_g369NanQ{0};   // q is NaN/Inf (transform blew up)
std::atomic<uint64_t> g_g369ZeroXY{0}; // all three vertices collapsed onto (0,0)
std::atomic<int64_t> g_g369MinX{INT64_MAX};
std::atomic<int64_t> g_g369MaxX{INT64_MIN};
std::atomic<int64_t> g_g369MinY{INT64_MAX};
std::atomic<int64_t> g_g369MaxY{INT64_MIN};

bool g369CensusEnabled()
{
    static const bool s_on = (std::getenv("DC2_G369_CENSUS") != nullptr);
    return s_on;
}

static inline void g369AccumXY(int64_t x, int64_t y)
{
    auto bumpMin = [](std::atomic<int64_t> &a, int64_t v) {
        int64_t cur = a.load(std::memory_order_relaxed);
        while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    };
    auto bumpMax = [](std::atomic<int64_t> &a, int64_t v) {
        int64_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    };
    bumpMin(g_g369MinX, x);
    bumpMax(g_g369MaxX, x);
    bumpMin(g_g369MinY, y);
    bumpMax(g_g369MaxY, y);
}

void g369CensusFlush(uint64_t tick);
std::atomic<uint64_t> g_g369Tick{0};

// The present tick lives in ps2_runtime.cpp; declare it at BLOCK scope (as G214 does) so the
// surrounding anonymous namespace does not give the declaration internal linkage.
static uint64_t g369PresentTick()
{
    extern std::atomic<uint64_t> g_dc2PresentTick;
    return g_dc2PresentTick.load(std::memory_order_relaxed);
}

static void g369RollTickMaybe()
{
    const uint64_t tk = g369PresentTick();
    uint64_t cur = g_g369Tick.load(std::memory_order_relaxed);
    if (tk == cur)
        return;
    if (g_g369Tick.compare_exchange_strong(cur, tk, std::memory_order_relaxed))
        g369CensusFlush(cur);
}

// Called from drawTriangle. xy are the rasterizer's screen-space pixel coords.
void g369NoteTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                      float q0, float q1, float q2, bool degenerate)
{
    if (!g369CensusEnabled())
        return;
    g369RollTickMaybe();
    g_g369Tris.fetch_add(1u, std::memory_order_relaxed);
    if (!std::isfinite(q0) || q0 <= 0.0f || !std::isfinite(q1) || q1 <= 0.0f ||
        !std::isfinite(q2) || q2 <= 0.0f)
        g_g369Behind.fetch_add(1u, std::memory_order_relaxed);
    if (degenerate)
        g_g369Degenerate.fetch_add(1u, std::memory_order_relaxed);
    if (!std::isfinite(q0) || !std::isfinite(q1) || !std::isfinite(q2) ||
        !std::isfinite(x0) || !std::isfinite(y0))
        g_g369NanQ.fetch_add(1u, std::memory_order_relaxed);
    if (x0 == 0.0f && y0 == 0.0f && x1 == 0.0f && y1 == 0.0f && x2 == 0.0f && y2 == 0.0f)
        g_g369ZeroXY.fetch_add(1u, std::memory_order_relaxed);
    auto clampToI64 = [](float v) -> int64_t {
        if (!std::isfinite(v))
            return 0;
        if (v > 1.0e9f) return (int64_t)1000000000;
        if (v < -1.0e9f) return (int64_t)-1000000000;
        return (int64_t)v;
    };
    g369AccumXY(clampToI64(x0), clampToI64(y0));
    g369AccumXY(clampToI64(x1), clampToI64(y1));
    g369AccumXY(clampToI64(x2), clampToI64(y2));
}

// Front-door hook: called from drawPrimitive BEFORE any G144 tile-bin deferral or G178 GPU
// hand-off, so it sees every primitive the guest submits regardless of which backend runs.
void g369NoteDrawPrim(uint32_t type, const GSVertex &v0, const GSVertex &v1, const GSVertex &v2,
                      uint32_t fbp, uint32_t tbp0, uint32_t tme)
{
    if (g369CensusEnabled() && type >= 3u && type != 6u &&
        (!std::isfinite(v0.q) || !std::isfinite(v0.x)))
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 20u || (n % 2000u) == 0u)
            std::fprintf(stderr,
                         "[G369:poison] n=%u tick=%llu prim=%u fbp=0x%x tbp=0x%x tme=%u "
                         "v0=(%g,%g,q=%g) v1=(%g,%g) v2=(%g,%g)\n",
                         n, (unsigned long long)g369PresentTick(), type, fbp, tbp0, tme,
                         v0.x, v0.y, v0.q, v1.x, v1.y, v2.x, v2.y);
    }
    if (!g369CensusEnabled())
        return;
    g369RollTickMaybe();
    if (type == 6u) // sprite
    {
        g_g369Sprites.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if (type < 3u) // point/line: not scene geometry, ignore
        return;
    const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    g369NoteTriangle(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, v0.q, v1.q, v2.q,
                     std::fabs(area) < 1.0e-6f);
}

void g369NoteSprite()
{
    if (!g369CensusEnabled())
        return;
    g369RollTickMaybe();
    g_g369Sprites.fetch_add(1u, std::memory_order_relaxed);
}

// Emit one line per present tick and reset the window. Driven from the primitive paths
// (the present tick is published by ps2_runtime.cpp as g_dc2PresentTick), so no cross-TU
// symbol is needed — this whole .inc lands in the rasterizer's anonymous namespace.
void g369CensusFlush(uint64_t tick)
{
    const int64_t minX = g_g369MinX.exchange(INT64_MAX, std::memory_order_relaxed);
    const int64_t maxX = g_g369MaxX.exchange(INT64_MIN, std::memory_order_relaxed);
    const int64_t minY = g_g369MinY.exchange(INT64_MAX, std::memory_order_relaxed);
    const int64_t maxY = g_g369MaxY.exchange(INT64_MIN, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G369:census] tick=%llu tris=%llu sprites=%llu behind=%llu degen=%llu "
                 "nan=%llu zeroxy=%llu x=[%lld..%lld] y=[%lld..%lld]\n",
                 (unsigned long long)tick,
                 (unsigned long long)g_g369Tris.exchange(0, std::memory_order_relaxed),
                 (unsigned long long)g_g369Sprites.exchange(0, std::memory_order_relaxed),
                 (unsigned long long)g_g369Behind.exchange(0, std::memory_order_relaxed),
                 (unsigned long long)g_g369Degenerate.exchange(0, std::memory_order_relaxed),
                 (unsigned long long)g_g369NanQ.exchange(0, std::memory_order_relaxed),
                 (unsigned long long)g_g369ZeroXY.exchange(0, std::memory_order_relaxed),
                 (long long)(minX == INT64_MAX ? 0 : minX), (long long)(maxX == INT64_MIN ? 0 : maxX),
                 (long long)(minY == INT64_MAX ? 0 : minY), (long long)(maxY == INT64_MIN ? 0 : maxY));
}
