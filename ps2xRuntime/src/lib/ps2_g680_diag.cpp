// ===== G680: storage + reporter for the item-menu ICON SPRITE geometry census ================
//
// A COLD TU BY CONSTRUCTION (rule 12b), exactly like ps2_g662_diag.cpp / ps2_g663_diag.cpp. The
// dedupe set, the formatting and the printf all live here; the hot TU sees one out-of-line call,
// and in the default build not even that (the call site is `#if`-excluded, rule 12c).
//
// Compiled ONLY when PS2X_G680_DIAG is on — the file is not in the default source list at all.
//
// WHAT IT ANSWERS. The Item menu's fish icons carry short black dashes above them. G680 already
// proved by elimination that the ICON DRAWS paint them (dropping every sprite that samples the
// icon atlas takes the dash rows 32 -> 0), and that the artwork, the CLUT, the filter and the
// sizes the guest asks for are all correct. What is NOT known is the exact affine map: the 12.4
// screen rect, the 12.4 UV rect, the -0.5-texel FST shift the front end applies, the G406/G407
// rounding state and the G679 exact-lattice period that travel beside them. This prints all of it
// per distinct icon sprite so the texel row each fragment lands on can be computed offline.
//
// Reporting obeys G657 measurement law 10: the denominator (`n`) is counted unconditionally and
// printed on every line, so "no line" can never be confused with "zero".

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace
{
std::atomic<unsigned long long> g_n{0};
std::mutex g_m;
std::unordered_set<std::string> g_seen;
size_t g_cap = 0;

bool armed()
{
    static const bool a = (std::getenv("DC2_G680_ICON") != nullptr);
    return a;
}

size_t cap()
{
    if (g_cap == 0)
    {
        const char *e = std::getenv("DC2_G680_ICON_MAX");
        g_cap = e ? static_cast<size_t>(std::atoi(e)) : 400u;
        if (g_cap == 0)
            g_cap = 400u;
    }
    return g_cap;
}
} // namespace

// ===== [G680:snap] — the BLAST RADIUS of the ceil-coverage repair =============================
// The snap is the identity on an integer rect, so the population it can move is exactly the
// sprites with a fractional 12.4 edge. Counted unconditionally and reported keyed to its own
// denominator (G657 measurement law 10), split by whether the sprite is a textured FST blit (the
// class whose UV anchor also moves) or not.
void g680NoteSpriteSnap(int snapped, int tme, int fst)
{
    static std::atomic<unsigned long long> s_all{0}, s_moved{0}, s_movedTexFst{0}, s_texFst{0};
    const unsigned long long n = s_all.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (tme && fst)
        s_texFst.fetch_add(1u, std::memory_order_relaxed);
    if (snapped)
    {
        s_moved.fetch_add(1u, std::memory_order_relaxed);
        if (tme && fst)
            s_movedTexFst.fetch_add(1u, std::memory_order_relaxed);
    }
    if (n == 1u || (n % 2000000u) == 0u)
        std::fprintf(stderr,
                     "[G680:snap] sprites=%llu moved=%llu (%.4f%%) texFst=%llu movedTexFst=%llu\n",
                     n, s_moved.load(std::memory_order_relaxed),
                     100.0 * static_cast<double>(s_moved.load(std::memory_order_relaxed)) /
                         static_cast<double>(n),
                     s_texFst.load(std::memory_order_relaxed),
                     s_movedTexFst.load(std::memory_order_relaxed));
}

// ===== [G680:oracle] — THE EXACTNESS ORACLE ===================================================
//
// INVARIANT: on hardware a sprite's sampled texel can never leave the texel range its own two UV
// vertices define. The GS covers pixel P iff `ceil(p0) <= P < ceil(p1)` and evaluates the texture
// coordinate as `t0 + (P - p0)·dt/dp`, so at the first covered pixel `P - p0 = ceil(p0) - p0 >= 0`
// and at the last `P - p0 < p1 - p0`; the coordinate therefore stays inside `[t0, t1)`.
//
// GL's pixel-CENTRE coverage rule starts at `ceil(p0 - 0.5)`, which for a fractional `p0` with
// `frac(p0) <= 0.5` is one pixel EARLIER — there `P - p0 < 0`, the coordinate is BELOW `t0`, and a
// NEAREST sampler reads the texel row/column before the sprite's own source rectangle. It also
// stops one pixel early and never reads the source rect's last texel.
//
// This counts both ends, per AXIS, for the old rule and the new one, over the same denominator.
// The value formula is identical in both arms (G680's endpoint shift is algebraically exact for any
// snapped rect), so this isolates the COVERAGE rule and nothing else.
namespace
{
struct AxisOracle
{
    std::atomic<unsigned long long> axes{0}, oldLow{0}, oldHigh{0}, newLow{0}, newHigh{0};
};
AxisOracle g_or;

inline void oracleAxis(int p0f, int p1f, int t0f, int t1f)
{
    if (p1f == p0f)
        return;
    const double p0 = p0f / 16.0, p1 = p1f / 16.0;
    const double t0 = t0f / 16.0, t1 = t1f / 16.0;
    const double step = (t1 - t0) / (p1 - p0);
    const double lo = (t0 < t1) ? t0 : t1, hi = (t0 < t1) ? t1 : t0;
    const double tLo = std::floor(lo), tHi = std::ceil(hi) - 1.0;
    const auto first = [&](double rule) { return std::ceil(p0 < p1 ? p0 - rule : p1 - rule); };
    const auto last = [&](double rule) { return std::ceil(p0 < p1 ? p1 - rule : p0 - rule) - 1.0; };
    const auto texel = [&](double p) { return std::floor(t0 + (p - p0) * step); };
    g_or.axes.fetch_add(1u, std::memory_order_relaxed);
    // old = GL pixel-centre rule (rule 0.5), new = GS ceil rule (rule 0.0)
    for (int arm = 0; arm < 2; ++arm)
    {
        const double rule = (arm == 0) ? 0.5 : 0.0;
        const double a = texel(first(rule)), b = texel(last(rule));
        const double mn = (a < b) ? a : b, mx = (a < b) ? b : a;
        if (mn < tLo)
            (arm == 0 ? g_or.oldLow : g_or.newLow).fetch_add(1u, std::memory_order_relaxed);
        if (mx > tHi)
            (arm == 0 ? g_or.oldHigh : g_or.newHigh).fetch_add(1u, std::memory_order_relaxed);
    }
}
} // namespace

void g680NoteSpriteOracle(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1)
{
    oracleAxis(x0, x1, u0, u1);
    oracleAxis(y0, y1, v0, v1);
    const unsigned long long n = g_or.axes.load(std::memory_order_relaxed);
    if (n == 2u || (n % 1000000u) < 2u)
        std::fprintf(stderr,
                     "[G680:oracle] axes=%llu | OLD outsideLo=%llu (%.3f%%) outsideHi=%llu (%.3f%%)"
                     " | NEW outsideLo=%llu outsideHi=%llu\n",
                     n, g_or.oldLow.load(std::memory_order_relaxed),
                     100.0 * static_cast<double>(g_or.oldLow.load(std::memory_order_relaxed)) /
                         static_cast<double>(n),
                     g_or.oldHigh.load(std::memory_order_relaxed),
                     100.0 * static_cast<double>(g_or.oldHigh.load(std::memory_order_relaxed)) /
                         static_cast<double>(n),
                     g_or.newLow.load(std::memory_order_relaxed),
                     g_or.newHigh.load(std::memory_order_relaxed));
}

// One line per DISTINCT icon-sprite shape. All positions are 12.4 fixed point relative to the
// XYOFFSET; all UVs are 12.4 texel coordinates. `q*` is the pixel rect the front end finally
// pushed (already multiplied by 16 by the caller so the two are directly comparable).
void g680NoteIconSprite(unsigned tbp, unsigned tbw, unsigned psm,
                        int texW, int texH, int pushTexW, int pushTexH,
                        int x0, int y0, int x1, int y1,
                        int u0, int v0, int u1, int v1,
                        int qx0, int qy0, int qx1, int qy1,
                        double shU0, double shU1, double shV0, double shV1,
                        unsigned flagsU, unsigned flagsV, int originX, int originY,
                        unsigned periodU, unsigned periodV,
                        int screenSprite, unsigned wrapU, unsigned wrapV, int cpuPath)
{
    if (!armed())
        return;
    const unsigned long long n = g_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "[G680:icon] tbp=0x%04x tbw=%u psm=0x%02x tex=%dx%d push=%dx%d "
                  "xy=(%d,%d)-(%d,%d) q=(%d,%d)-(%d,%d) uv=(%d,%d)-(%d,%d) "
                  "shU=(%.5f,%.5f) shV=(%.5f,%.5f) fl=(%u,%u) org=(%d,%d) per=(%u,%u) "
                  "ss=%d wrap=(%u,%u) cpu=%d",
                  tbp, tbw, psm, texW, texH, pushTexW, pushTexH,
                  x0, y0, x1, y1, qx0, qy0, qx1, qy1, u0, v0, u1, v1,
                  shU0, shU1, shV0, shV1, flagsU, flagsV, originX, originY,
                  periodU, periodV, screenSprite, wrapU, wrapV, cpuPath);
    std::lock_guard<std::mutex> lk(g_m);
    if (g_seen.size() >= cap())
        return;
    if (!g_seen.insert(line).second)
        return;
    std::fprintf(stderr, "%s n=%llu\n", line, n);
}
