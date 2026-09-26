// ===== G761: THE MAP-125 CHARACTER GROUND SHADOW — A WINDING-BUFFER ORACLE =====
// revision: 6
//
// WHAT THE GAME DOES (read off `ref/dumps/map_125.gs`, HW, one frame):
//
//   1. FB 0x139 / fbw 8 / CT32 is CLEARED to RGBA=0 over [0..512, 0..416].
//   2. 900 untextured TRIFANs (1,800 triangles, 3,600 vertices) are drawn into it with
//      `rgba=(1,1,1,0x40)`, FIX=0x80, `ZTST=GREATER` against the scene's own ZBP=0xd0, `ZMSK=1`:
//        * ALPHA low byte 0x68 = A0 B2 C2 D1 = `Cd + Cs` -> +1 where a FRONT face passes depth,
//        * ALPHA low byte 0x62 = A2 B0 C2 D1 = `Cd - Cs` -> -1 where a BACK face passes depth,
//      saturating at 0 and 255 (`COLCLAMP=1` is MEASURED in the dump — a wrap-vs-clamp mismatch is
//      NOT available as an explanation). This is a textbook Z-PASS STENCIL SHADOW VOLUME kept in
//      the colour channel.
//   3. ONE full-screen sprite into the display target reads the SAME storage back as `TBP=0x2720`
//      (= 0x139 << 5) with **PSM = PSMCT24**, TCC=1, TFX=0, `ALPHA = A2 B1 C0 D1` = `Cd*(1-As/128)`
//      and `ATE=1 ATST=GEQUAL AREF=1`. PSMCT24 takes its alpha from TEXA — MEASURED
//      `TA0=48 AEM=1 TA1=128` — so `a = (rgb==0) ? 0 : 48`: the ALPHA TEST turns the winding
//      number into a BINARY mask and the shadow is a flat `Cd * (1 - 48/128) = 0.625*Cd`.
//
// ⭐ THE ONE FACT THAT DECIDES WHERE TO LOOK: replayed offline out of the dump with the depth test
// REMOVED, the 900 fans cancel to **winding 0 at every pixel** (add-only covers 21,023 px; add+sub
// covers 0). The silhouette is therefore **100 % a product of the depth comparison** — no part of
// its shape comes from the geometry's screen coverage alone.
//
// WHAT IS WRONG: the runner's silhouette is TORN — a diagonal wedge of the torso mass is missing
// and one fragment is detached, bit-stable frame to frame. Already excluded, each on its own
// same-route arm with matched masks:
//   * the GPU/CPU admission choice, resolution, and everything G759 shipped   (G760 §9, six arms)
//   * the G570 whole-batch + GPU-compute 0x139 specialisation  (`DC2_G570_NO_GPU_139=1
//     DC2_G570_NO_BATCH_139=1` -> mask 1,202 px, control 1,202 px, xor 1 px)
//   * the G408 top-left half-open edge rule  (`DC2_G408_LEGACY_SHADOW_COVERAGE=1` -> 1,202 px)
//   * the CONSUMER: the measured darkening ratio is **0.63**, against HW's **0.6235** — TEXA,
//     the alpha test and the blend are all right, so the mask it reads must already be torn.
// Every rasterisation path in the binary agrees with every other one, so what is left is the
// depth comparison itself: either the shadow volume's interpolated Z or the ground's stored Z.
//
// ⛔ REVISION 2 MEASURED THE WRONG INSTANT, AND ITS NUMBER IS STILL WORTH KEEPING.
// Accumulating at the front door reads `g403DisplayZBuf()` BEFORE the flush that fills it, so every
// fragment passed GREATER against a zero mirror: `zrejAdd=0 zrejSub=0 lit=0` on every frame. Two
// facts survive that mistake and are load-bearing:
//   ⭐ `triAdd=818 triSub=982` = **1,800 triangles, exactly the HW dump's count** — no shadow
//      geometry is lost, dropped, clipped or culled on the way in; and
//   ⭐ `covAdd == covSub` EXACTLY, every frame (298,489 / 293,376 / 289,602 …) — the front and back
//      faces cover the identical sample set, so the coverage rule is watertight and the two halves
//      are in perfect balance. **Every pixel of the silhouette's shape is therefore produced by the
//      depth comparison alone**, which is the same conclusion the offline replay reached from the
//      other side.
// ⛔ REVISION 3 COUNTED NOTHING. It moved the accumulation into `drawTriangle`'s pixel loop, on the
// assumption that `DC2_G570_NO_GPU_139=1 DC2_G570_NO_BATCH_139=1` (and then also
// `DC2_G570_NO_PRIVATE_139=1`) routes this class through the generic loop. It does not: on this
// route the 0x139 batch is served by a specialised replay leaf, and `drawTriangle` is never called
// for it — so `[G761:shadow]` printed nothing at all, twice.
//   ⚠️ **Rule G761-1: an oracle placed in ONE rasterisation leaf measures that leaf's admission, not
//   the effect.** Three revisions in a row died of this (drawSprite, front-door depth, drawTriangle).
//
// Revision 4 stops guessing the leaf. It reads the REAL winding buffer out of guest VRAM at 0x2720
// (= 0x139 << 5) at the moment the NEXT clear of 0x139 reaches the front door — by then the resolve
// has forced the flush that rasterised the batch, and the clear itself has not run yet. What the
// surface HOLDS cannot be argued with, whoever wrote it. It dumps `g761_vram_<k>.pgm` (the real
// mask) beside `g761_zdst_<k>.pgm` (the depth the GREATER test is supposed to read).
//
// THIS FILE IS AN INDEPENDENT ORACLE FOR EXACTLY THAT. It hooks `drawPrimitive` — the FRONT DOOR,
// before a draw can be deferred (G144) or handed to the GPU backend (G178) — so it sees every
// shadow triangle on every arm, and it rebuilds the winding buffer in its own memory rather than
// reading guest VRAM (whose authority may be a GPU texture; revision 1 read VRAM at the resolve
// SPRITE and printed nothing at all, because on the shipping path that sprite never reaches
// `drawSprite`). Inert unless its env flags are set:
//
//   DC2_G761_SHADOW_STAT=1     `[G761:shadow]` per-frame census, closed on the next 0x139 clear:
//                              add/sub triangles at the front door, plus `vramLit` and the R-value
//                              histogram of the REAL surface. `vramLit` is the shadow's true pixel
//                              count and `h1..h8+` says whether the winding ever exceeds 1.
//   DC2_G761_SHADOW_DUMP=<n>   at every n-th frame also write 512x448 images:
//                                `g761_vram_<k>.pgm`  ⭐ the REAL winding buffer (0 = no shadow),
//                                `g761_win_<k>.pgm`   the in-probe replay (empty unless a leaf
//                                                     hook fires — see Rule G761-1),
//                                `g761_marg_<k>.pgm`  128 + clamp((zi-zdst)/256) over FRONT faces
//                                                     — 128 means the front face is EXACTLY on the
//                                                     ground's depth, i.e. a precision coin-flip,
//                                `g761_zdst_<k>.pgm`  the ground depth the test read (>>16).
//                              If the tear coincides with margin ~128 the depth arithmetic is the
//                              cause; if the margin there is large and negative a whole FACE is
//                              landing behind the floor and the geometry is.
//   DC2_G761_SHADOW_NOZ=1      ceiling probe in the generic `drawTriangle` only: drop the depth
//                              test for this class. Expect an EMPTY shadow (see the fact above);
//                              it exists to confirm the mechanism, not to fix anything.

namespace
{

inline bool g761ShadowNoZ()
{
    static const bool on = std::getenv("DC2_G761_SHADOW_NOZ") != nullptr;
    return on;
}

inline bool g761ShadowStatOn()
{
    static const bool on = std::getenv("DC2_G761_SHADOW_STAT") != nullptr;
    return on;
}

inline uint32_t g761ShadowDumpEvery()
{
    static const uint32_t every = []() -> uint32_t {
        const char *e = std::getenv("DC2_G761_SHADOW_DUMP");
        if (e == nullptr || e[0] == '\0')
            return 0u;
        const long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<uint32_t>(v) : 1u;
    }();
    return every;
}

inline bool g761ShadowOn()
{
    return g761ShadowStatOn() || g761ShadowDumpEvery() != 0u;
}

constexpr int kG761W = 512;
constexpr int kG761H = 448;

struct G761Acc
{
    std::vector<int32_t> winding;
    std::vector<int32_t> margin;   // min |zi - zdst| over FRONT faces, signed, INT32_MAX = untouched
    std::vector<uint32_t> zdst;
    // ⚠️ Atomic because the CPU band replay rasterises disjoint row bands on several pool lanes:
    // the per-pixel arrays are safe by partition, these counters are not.
    std::atomic<uint64_t> triAdd{0}, triSub{0};
    std::atomic<uint64_t> covAdd{0}, covSub{0};
    std::atomic<uint64_t> zrejAdd{0}, zrejSub{0};
    uint64_t resolves = 0;
    // ⭐ Revision 5: the batch's own triangles, captured at the front door so the probe can REPLAY
    // them against the depth mirror at dump time and compare its own winding buffer with the one
    // the shipped leaf actually produced. That comparison is what makes the defect analysable
    // offline instead of one 25-minute build per question.
    struct Tri { float x[3], y[3], z[3]; int ofx, ofy, sx0, sy0, sx1, sy1; bool add; };
    std::vector<Tri> tris;
    bool pendingDump = false;
    void ensure()
    {
        if (winding.empty())
        {
            winding.assign(static_cast<size_t>(kG761W) * kG761H, 0);
            margin.assign(static_cast<size_t>(kG761W) * kG761H, INT32_MAX);
            zdst.assign(static_cast<size_t>(kG761W) * kG761H, 0u);
        }
    }
    void reset()
    {
        std::fill(winding.begin(), winding.end(), 0);
        std::fill(margin.begin(), margin.end(), INT32_MAX);
        std::fill(zdst.begin(), zdst.end(), 0u);
        triAdd = 0; triSub = 0; covAdd = 0; covSub = 0; zrejAdd = 0; zrejSub = 0;
        tris.clear();
    }
};

G761Acc g_g761;

// ⭐ Revision 3's real sample point: called from INSIDE drawTriangle's pixel loop, once per covered
// sample of a shadow-volume triangle, with the depth values the shipped path actually used.
void g761NoteSample(bool isAdd, int x, int y, uint32_t zi, uint32_t zdst, bool zPass)
{
    if (!g761ShadowOn() || x < 0 || y < 0 || x >= kG761W || y >= kG761H)
        return;
    g_g761.ensure();
    const size_t idx = static_cast<size_t>(y) * kG761W + x;
    g_g761.zdst[idx] = zdst;
    if (isAdd)
    {
        g_g761.covAdd.fetch_add(1u, std::memory_order_relaxed);
        const int32_t m = static_cast<int32_t>(static_cast<int64_t>(zi) -
                                               static_cast<int64_t>(zdst));
        int32_t &cur = g_g761.margin[idx];
        if (cur == INT32_MAX || std::abs(m) < std::abs(cur))
            cur = m;
        if (!zPass) { g_g761.zrejAdd.fetch_add(1u, std::memory_order_relaxed); return; }
    }
    else
    {
        g_g761.covSub.fetch_add(1u, std::memory_order_relaxed);
        if (!zPass) { g_g761.zrejSub.fetch_add(1u, std::memory_order_relaxed); return; }
    }
    int32_t &w = g_g761.winding[idx];
    w += isAdd ? 1 : -1;
    if (w < 0) w = 0;
    if (w > 255) w = 255;
}

void g761NoteTri(bool isAdd)
{
    if (!g761ShadowOn())
        return;
    g_g761.ensure();
    (isAdd ? g_g761.triAdd : g_g761.triSub).fetch_add(1u, std::memory_order_relaxed);
}


// ⭐ The same replay, parameterised by a depth BIAS and writing into a caller buffer. A Z-pass
// stencil volume's whole shape is the GREATER comparison, so if the tear is the depth ARITHMETIC
// (front faces landing a few units under a near-coplanar ground) a small positive bias closes it,
// and if it is not, no bias does. Four biases in one dump = one build instead of four.
void g761ReplayBias(uint8_t *vram, size_t vramSize, int64_t bias, std::vector<int32_t> &out)
{
    out.assign(static_cast<size_t>(kG761W) * kG761H, 0);
    const bool haveZ = vram != nullptr && vramSize >= 0x400000u;
    for (const G761Acc::Tri &t : g_g761.tris)
    {
        const float fx0 = t.x[0], fy0 = t.y[0];
        const float fx1 = t.x[1], fy1 = t.y[1];
        const float fx2 = t.x[2], fy2 = t.y[2];
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (std::fabs(denom) < 0.001f)
            continue;
        const int64_t fx[3] = {static_cast<int64_t>(std::llround(fx0 * 16.0f)),
                               static_cast<int64_t>(std::llround(fx1 * 16.0f)),
                               static_cast<int64_t>(std::llround(fx2 * 16.0f))};
        const int64_t fy[3] = {static_cast<int64_t>(std::llround(fy0 * 16.0f)),
                               static_cast<int64_t>(std::llround(fy1 * 16.0f)),
                               static_cast<int64_t>(std::llround(fy2 * 16.0f))};
        const int sign = denom >= 0.0f ? 1 : -1;
        const auto topLeft = [&](int a, int b) {
            int64_t dx = fx[b] - fx[a];
            int64_t dy = fy[b] - fy[a];
            if (sign < 0) { dx = -dx; dy = -dy; }
            return dy < 0 || (dy == 0 && dx > 0);
        };
        const bool tl[3] = {topLeft(1, 2), topLeft(2, 0), topLeft(0, 1)};
        const float wind = denom < 0.0f ? -1.0f : 1.0f;
        const float invAbsDenom = 1.0f / std::fabs(denom);
        int minX = std::max(t.sx0, static_cast<int>(std::floor(std::min({fx0, fx1, fx2}))));
        int maxX = std::min(t.sx1, static_cast<int>(std::ceil(std::max({fx0, fx1, fx2}))));
        int minY = std::max(t.sy0, static_cast<int>(std::floor(std::min({fy0, fy1, fy2}))));
        int maxY = std::min(t.sy1, static_cast<int>(std::ceil(std::max({fy0, fy1, fy2}))));
        minX = std::max(minX, 0); minY = std::max(minY, 0);
        maxX = std::min(maxX, kG761W - 1); maxY = std::min(maxY, kG761H - 1);
        if (maxX < minX || maxY < minY)
            continue;
        for (int y = minY; y <= maxY; ++y)
        {
            const int64_t sy = static_cast<int64_t>(y) * 16;
            for (int x = minX; x <= maxX; ++x)
            {
                const int64_t sx = static_cast<int64_t>(x) * 16;
                const auto edge = [&](int a, int b) {
                    return ((fx[b] - fx[a]) * (sy - fy[a]) -
                            (fy[b] - fy[a]) * (sx - fx[a])) * sign;
                };
                const int64_t e0 = edge(1, 2), e1 = edge(2, 0), e2 = edge(0, 1);
                if (e0 < 0 || (e0 == 0 && !tl[0]) ||
                    e1 < 0 || (e1 == 0 && !tl[1]) ||
                    e2 < 0 || (e2 == 0 && !tl[2]))
                    continue;
                const float px = static_cast<float>(x);
                const float py = static_cast<float>(y);
                const float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * wind) * invAbsDenom;
                const float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * wind) * invAbsDenom;
                const float w2 = 1.0f - w0 - w1;
                float zf = t.z[0] * w0 + t.z[1] * w1 + t.z[2] * w2;
                if (zf < 0.0f) zf = 0.0f;
                if (zf > 16777215.0f) zf = 16777215.0f;
                const int64_t zi = static_cast<int64_t>(zf) + bias;
                const int64_t zd = haveZ
                    ? static_cast<int64_t>(GSMem::ReadZ24(vram, 0xd0u << 5, 8u,
                                                          static_cast<uint32_t>(x),
                                                          static_cast<uint32_t>(y)) & 0xFFFFFFu)
                    : 0;
                if (zi <= zd)
                    continue;
                int32_t &w = out[static_cast<size_t>(y) * kG761W + x];
                w += t.add ? 1 : -1;
                if (w < 0) w = 0;
                if (w > 255) w = 255;
            }
        }
    }
}

void g761WritePgm(const char *name, uint64_t k, const std::vector<uint8_t> &px)
{
    char path[192];
    std::snprintf(path, sizeof(path), "D:/ps2r/dc2/captures/g761_%s_%03llu.pgm", name,
                  static_cast<unsigned long long>(k));
    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fprintf(fp, "P5\n%d %d\n255\n", kG761W, kG761H);
        std::fwrite(px.data(), 1u, px.size(), fp);
        std::fclose(fp);
    }
}

// ⭐ Revision 5: replay the batch's OWN triangles against the depth mirror as it stands at dump
// time, with drawTriangle's conventions (G408 integer sample centres, integer top-left half-open
// edges, the same float barycentric Z, saturating winding). Its output sits beside the real surface
// in the same dump, so "does the probe's model of this effect reproduce the shipped result" is one
// image diff instead of one build.
// ⛔ `g403DisplayZBuf()` IS EMPTY ON THIS ROUTE. Revision 5 replayed against it and read
// `zdst nonzero = 0` over the whole 512x448 window, so every fragment passed GREATER and the replay
// cancelled to nothing. Under G203 universal-Z the depth the shadow's test actually reads is the
// REAL PSMZ24 surface in guest VRAM at ZBP=0xd0, and the mirror is legacy. Revision 6 reads that.
void g761ReplayStoredTris(uint8_t *vram, size_t vramSize)
{
    const bool haveZ = vram != nullptr && vramSize >= 0x400000u;
    for (const G761Acc::Tri &t : g_g761.tris)
    {
        const float fx0 = t.x[0], fy0 = t.y[0];
        const float fx1 = t.x[1], fy1 = t.y[1];
        const float fx2 = t.x[2], fy2 = t.y[2];
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (std::fabs(denom) < 0.001f)
            continue;
        const int64_t fx[3] = {static_cast<int64_t>(std::llround(fx0 * 16.0f)),
                               static_cast<int64_t>(std::llround(fx1 * 16.0f)),
                               static_cast<int64_t>(std::llround(fx2 * 16.0f))};
        const int64_t fy[3] = {static_cast<int64_t>(std::llround(fy0 * 16.0f)),
                               static_cast<int64_t>(std::llround(fy1 * 16.0f)),
                               static_cast<int64_t>(std::llround(fy2 * 16.0f))};
        const int sign = denom >= 0.0f ? 1 : -1;
        const auto topLeft = [&](int a, int b) {
            int64_t dx = fx[b] - fx[a];
            int64_t dy = fy[b] - fy[a];
            if (sign < 0) { dx = -dx; dy = -dy; }
            return dy < 0 || (dy == 0 && dx > 0);
        };
        const bool tl[3] = {topLeft(1, 2), topLeft(2, 0), topLeft(0, 1)};
        const float wind = denom < 0.0f ? -1.0f : 1.0f;
        const float invAbsDenom = 1.0f / std::fabs(denom);
        int minX = std::max(t.sx0, static_cast<int>(std::floor(std::min({fx0, fx1, fx2}))));
        int maxX = std::min(t.sx1, static_cast<int>(std::ceil(std::max({fx0, fx1, fx2}))));
        int minY = std::max(t.sy0, static_cast<int>(std::floor(std::min({fy0, fy1, fy2}))));
        int maxY = std::min(t.sy1, static_cast<int>(std::ceil(std::max({fy0, fy1, fy2}))));
        minX = std::max(minX, 0); minY = std::max(minY, 0);
        maxX = std::min(maxX, kG761W - 1); maxY = std::min(maxY, kG761H - 1);
        if (maxX < minX || maxY < minY)
            continue;
        for (int y = minY; y <= maxY; ++y)
        {
            const int64_t sy = static_cast<int64_t>(y) * 16;
            for (int x = minX; x <= maxX; ++x)
            {
                const int64_t sx = static_cast<int64_t>(x) * 16;
                const auto edge = [&](int a, int b) {
                    return ((fx[b] - fx[a]) * (sy - fy[a]) -
                            (fy[b] - fy[a]) * (sx - fx[a])) * sign;
                };
                const int64_t e0 = edge(1, 2), e1 = edge(2, 0), e2 = edge(0, 1);
                if (e0 < 0 || (e0 == 0 && !tl[0]) ||
                    e1 < 0 || (e1 == 0 && !tl[1]) ||
                    e2 < 0 || (e2 == 0 && !tl[2]))
                    continue;
                const float px = static_cast<float>(x);
                const float py = static_cast<float>(y);
                const float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * wind) * invAbsDenom;
                const float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * wind) * invAbsDenom;
                const float w2 = 1.0f - w0 - w1;
                float zf = t.z[0] * w0 + t.z[1] * w1 + t.z[2] * w2;
                if (zf < 0.0f) zf = 0.0f;
                if (zf > 16777215.0f) zf = 16777215.0f;
                const uint32_t zi = static_cast<uint32_t>(zf);
                const uint32_t zd = haveZ ? (GSMem::ReadZ24(vram, 0xd0u << 5, 8u,
                                                           static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y)) & 0xFFFFFFu)
                                          : g403DisplayZRead(x, y, 0x139u);
                const bool zPass = zi > zd;
                g761NoteSample(t.add, x, y, zi, zd, zPass);
            }
        }
    }
}

void g761Report(uint8_t *vram, size_t vramSize)
{
    const uint64_t k = ++g_g761.resolves;
    g_g761.ensure();
    g761ReplayStoredTris(vram, vramSize);

    // ⭐ The REAL winding buffer, read straight out of guest VRAM at 0x2720 (= 0x139 << 5). This is
    // leaf-independent: revision 3 hooked `drawTriangle` and counted nothing, because on this route
    // the 0x139 batch is served by a specialised replay and not by the generic loop. What the
    // surface HOLDS cannot be argued with, whoever wrote it.
    // ⚠️ Only true on an arm that keeps 0x139 in guest VRAM — pair with
    // `DC2_G570_NO_PRIVATE_139=1 DC2_G570_NO_GPU_139=1 DC2_G570_NO_BATCH_139=1`.
    uint64_t vramLit = 0ull, vramMaxR = 0ull;
    uint64_t hist[9]{};
    std::vector<uint8_t> vramMask;
    const bool haveVram = vram != nullptr && vramSize >= 0x400000u;
    if (haveVram)
    {
        vramMask.assign(static_cast<size_t>(kG761W) * kG761H, 0u);
        for (uint32_t y = 0; y < 416u; ++y)
            for (uint32_t x = 0; x < 512u; ++x)
            {
                const uint32_t c = GSMem::ReadCT32(vram, 0x2720u, 8u, x, y);
                const uint32_t r = c & 0xFFu;
                if ((c & 0x00FFFFFFu) != 0u) ++vramLit;
                if (r > vramMaxR) vramMaxR = r;
                hist[r < 8u ? r : 8u] += 1ull;
                vramMask[static_cast<size_t>(y) * kG761W + x] =
                    ((c & 0x00FFFFFFu) != 0u) ? 255u : 0u;
            }
    }

    if (g761ShadowStatOn())
    {
        uint64_t lit = 0;
        for (int32_t w : g_g761.winding)
            if (w != 0) ++lit;
        std::fprintf(stderr,
                     "[G761:shadow] n=%llu triAdd=%llu triSub=%llu covAdd=%llu covSub=%llu "
                     "zrejAdd=%llu zrejSub=%llu lit=%llu vramLit=%llu vramMaxR=%llu "
                     "h1=%llu h2=%llu h3=%llu h4=%llu h5=%llu h6=%llu h7=%llu h8+=%llu\n",
                     static_cast<unsigned long long>(k),
                     static_cast<unsigned long long>(g_g761.triAdd.load()),
                     static_cast<unsigned long long>(g_g761.triSub.load()),
                     static_cast<unsigned long long>(g_g761.covAdd.load()),
                     static_cast<unsigned long long>(g_g761.covSub.load()),
                     static_cast<unsigned long long>(g_g761.zrejAdd.load()),
                     static_cast<unsigned long long>(g_g761.zrejSub.load()),
                     static_cast<unsigned long long>(lit),
                     static_cast<unsigned long long>(vramLit),
                     static_cast<unsigned long long>(vramMaxR),
                     static_cast<unsigned long long>(hist[1]),
                     static_cast<unsigned long long>(hist[2]),
                     static_cast<unsigned long long>(hist[3]),
                     static_cast<unsigned long long>(hist[4]),
                     static_cast<unsigned long long>(hist[5]),
                     static_cast<unsigned long long>(hist[6]),
                     static_cast<unsigned long long>(hist[7]),
                     static_cast<unsigned long long>(hist[8]));
    }
    const uint32_t every = g761ShadowDumpEvery();
    if (every != 0u && (k % every) == 0u)
    {
        const size_t n = static_cast<size_t>(kG761W) * kG761H;
        std::vector<uint8_t> img(n);
        if (haveVram)
            g761WritePgm("vram", k, vramMask);
        for (size_t i = 0; i < n; ++i)
            img[i] = g_g761.winding[i] == 0 ? 0u : 255u;
        g761WritePgm("win", k, img);
        for (size_t i = 0; i < n; ++i)
        {
            const int32_t m = g_g761.margin[i];
            if (m == INT32_MAX) { img[i] = 0u; continue; }
            const long v = 128 + std::max<long>(-127, std::min<long>(127, m / 256));
            img[i] = static_cast<uint8_t>(v);
        }
        g761WritePgm("marg", k, img);
        // The depth the shadow's GREATER test actually reads: the real PSMZ24 surface at ZBP=0xd0.
        for (int y = 0; y < kG761H; ++y)
            for (int x = 0; x < kG761W; ++x)
            {
                const uint32_t zd = haveVram
                    ? (GSMem::ReadZ24(vram, 0xd0u << 5, 8u, static_cast<uint32_t>(x),
                                      static_cast<uint32_t>(y)) & 0xFFFFFFu)
                    : 0u;
                img[static_cast<size_t>(y) * kG761W + x] = static_cast<uint8_t>(zd >> 16);
            }
        g761WritePgm("zdst", k, img);
        // ⭐ A Z-pass stencil volume's whole shape IS the GREATER comparison, so if the tear is the
        // depth ARITHMETIC — front faces landing a few units under a near-coplanar ground — a small
        // positive bias closes it, and if it is not, no bias does. Four biases in one dump is one
        // build instead of four.
        static const int64_t kBias[] = {0, 4, 64, 1024, -4};
        std::vector<int32_t> biased;
        char nm[32];
        for (int64_t b : kBias)
        {
            g761ReplayBias(vram, vramSize, b, biased);
            uint64_t blit = 0;
            for (size_t i = 0; i < n; ++i)
            {
                img[i] = biased[i] == 0 ? 0u : 255u;
                if (biased[i] != 0) ++blit;
            }
            std::snprintf(nm, sizeof(nm), "bias%lld", static_cast<long long>(b));
            g761WritePgm(nm, k, img);
            std::fprintf(stderr, "[G761:bias] k=%llu bias=%lld lit=%llu\n",
                         static_cast<unsigned long long>(k),
                         static_cast<long long>(b),
                         static_cast<unsigned long long>(blit));
        }
        std::fprintf(stderr, "[G761:mask] wrote vram/win/marg/zdst k=%llu\n",
                     static_cast<unsigned long long>(k));
    }
    g_g761.reset();
}

// Called from GSRasterizer::drawPrimitive, before any deferral or GPU hand-off.
void g761FrontDoor(uint32_t primType, bool tme, uint32_t fbp, uint64_t alpha,
                   uint32_t tbp0, uint32_t tpsm, uint8_t *vram, size_t vramSize,
                   const GSVertex &v0, const GSVertex &v1, const GSVertex &v2,
                   int ofx, int ofy, int sx0, int sy0, int sx1, int sy1)
{
    if (!g761ShadowOn())
        return;

    // ⭐ THE DUMP INSTANT, and the third one this probe has had to move.
    // ⛔ Not the resolve itself: at the front door its flush has not run, so 0x2720 still holds
    //    the PREVIOUS frame (revision 2).
    // ⛔ Not the next clear of 0x139 either: between the resolve and that clear the game reuses
    //    0x139 as the per-object scratch it composites MAX into, so the surface is the character,
    //    not the winding buffer. Revision 4 measured exactly that and its histogram says so out
    //    loud - `vramMaxR=255`, `h8+=10,040` of 12,538 lit, i.e. arbitrary bright colour where a
    //    winding number can only be a small count.
    // ✅ The FIRST primitive after the resolve: the resolve's own drawPrimitive call has
    //    completed, so the flush it forced has rasterised the batch, and the character composite
    //    has not started (HW puts only TRX uploads in between).
    if (g_g761.pendingDump)
    {
        g_g761.pendingDump = false;
        g761Report(vram, vramSize);
    }

    const uint8_t alphaMode = static_cast<uint8_t>(alpha & 0xFFu);
    if (primType == GS_PRIM_TRIFAN && !tme && fbp == 0x139u &&
        (alphaMode == 0x62u || alphaMode == 0x68u) &&
        static_cast<uint8_t>((alpha >> 32u) & 0xFFu) == 0x80u)
    {
        g_g761.ensure();
        const bool isAdd = alphaMode == 0x68u;
        (isAdd ? g_g761.triAdd : g_g761.triSub).fetch_add(1u, std::memory_order_relaxed);
        if (g_g761.tris.size() < 8192u)
        {
            G761Acc::Tri t;
            t.x[0] = v0.x - static_cast<float>(ofx); t.y[0] = v0.y - static_cast<float>(ofy);
            t.x[1] = v1.x - static_cast<float>(ofx); t.y[1] = v1.y - static_cast<float>(ofy);
            t.x[2] = v2.x - static_cast<float>(ofx); t.y[2] = v2.y - static_cast<float>(ofy);
            t.z[0] = v0.z; t.z[1] = v1.z; t.z[2] = v2.z;
            t.ofx = ofx; t.ofy = ofy;
            t.sx0 = sx0; t.sy0 = sy0; t.sx1 = sx1; t.sy1 = sy1;
            t.add = isAdd;
            g_g761.tris.push_back(t);
        }
        return;
    }

    // The resolve: the one textured PSMCT24 sprite that reads 0x2720 back into a display target.
    if (primType == GS_PRIM_SPRITE && tme && tbp0 == 0x2720u && tpsm == GS_PSM_CT24 &&
        (fbp == 0x0u || fbp == 0x68u) &&
        g_g761.triAdd.load() + g_g761.triSub.load() != 0u)
        g_g761.pendingDump = true;
}

} // namespace
