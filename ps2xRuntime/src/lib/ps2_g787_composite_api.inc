// ==============================================================================================
// G787 — NATIVE OUTLINE COMPOSITE: the cross-TU surface between the EE seam
// (dc2_game_override_parts/g787_outline_composite.inc), the GS parse thread (gpu_gif_and_registers.inc
// TEXFLUSH / PRIM cases) and the rasterizer (capture push + flush expansion). plans/phase-G787-ledger.md.
//
// COutLineDraw::Draw(ff) composites the work RTT W onto the screen with two mgCDrawPrim calls:
//   DrawDivSprite4(prim, rect, tex, col, d, z)    kind 1: rect tiled on the 0x200 GS grid; per tile
//                                                  4 sprites at (+d,0) (-d,0) (0,+d) (0,-d), UV = tile
//   DrawDivSprite (prim, rect, tex, col, 0, 0, z) kind 2: rect split into 0x200-wide columns of
//                                                  screen-height rows, XY = UV + screen offset
// Each sprite is 4 PACKED qwords (UV, XYZF2, UV, XYZF2). The geometry is a pure function of the
// engine parameters below, so the EE drops the payload and the rest of the pipe regenerates it:
//   * the GS parse thread re-kicks sprites from it (exact legacy fallback, A+D writes), and
//   * converts the first DEFERRED sprite's G144 entry into ONE macro entry covering the rest; the
//     executor expands the macro's sprites in the vertex build with the per-sprite code unchanged.
//
// Declarations + pure inline functions only (Rule 12b): no std:: type, fixed capacity.
// ==============================================================================================
#ifndef DC2_G787_COMPOSITE_API_INC
#define DC2_G787_COMPOSITE_API_INC 1

#include <cstdint>

// Engine-seam geometry of one composite call (1/16-pixel GS units).
struct G787Geom
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the call's mgRect (screen space, before the GS offset)
    int32_t d = 0;                          // outline offset (kind 1); 0 for kind 2
    int32_t offx16 = 0, offy16 = 0;         // mgScreenOffx*16, mgScreenOffy*16
    int32_t screenH16 = 0;                  // mgScreenHeight*16 (kind 2 row height)
    uint32_t kind = 0;                      // 1 DrawDivSprite4, 2 DrawDivSprite
    // The constant upper words of the PACKED qwords (read from the guest per call).
    uint32_t uvW2a = 0, uvW3a = 0, uvW2b = 0, uvW3b = 0; // kind 1 UV words 2/3 (vertex 0, vertex 1)
    uint32_t xyzW2 = 0;                                   // XYZF2 word 2 (the z argument)
    uint32_t xyzW3a = 0, xyzW3b = 0;                      // XYZF2 word 3 (vertex 0, vertex 1)
};

// One EE -> GS-parse descriptor. Ring slot = seq & (kG787Ring - 1); `seq` is published last.
constexpr uint32_t kG787Ring = 8192u;
struct G787Desc
{
    G787Geom g;
    uint32_t verify = 0; // 1: the legacy payload follows (shadow oracle)
    uint32_t sprites = 0;
    volatile uint32_t seq = 0;
};
extern G787Desc g_g787Ring[kG787Ring];

// TEXFLUSH value written in place of Begin2's header TEXFLUSH(0): magic | verify bit | seq.
constexpr uint64_t kG787MarkerMagic = 0xC787ull;
inline uint64_t g787MarkerValue(uint32_t seq, bool verify)
{
    return (kG787MarkerMagic << 48) | (verify ? (1ull << 47) : 0ull) | static_cast<uint64_t>(seq);
}

// Macro entry tag (in the sprite's otherwise-zero v[2]): q = kG787MacroQ; x,y,s,t = rect; z = d;
// u,v = offx16, offy16; r|g<<8 = screenH16; b = kind; a|fog<<8 = first sprite index covered.
constexpr float kG787MacroQ = -78701.0f;

// Process-wide mode (resolved once, read by every side): 0 off, 1 live, 2 verify (shadow oracle).
extern int g_g787Mode;

// Counters (monotonic, relaxed). EE thread: calls / verified / payload byte mismatches / patch
// refusals; parse thread: markers / orphans / fallback-kicked sprites / macros + covered sprites /
// verify checks + mismatches + foreign pushes; exec thread: macros + sprites expanded, CPU-replay
// expansions.
struct G787Counters
{
    uint64_t eeCalls = 0, eeLive = 0, eeVerify = 0, eeVerifyBad = 0, eePatchMiss = 0, eeEmpty = 0;
    uint64_t markers = 0, orphans = 0, kicked = 0, macros = 0, macroSprites = 0, primNot2 = 0;
    uint64_t vChecks = 0, vBad = 0, vForeign = 0, vCulled = 0;
    uint64_t xMacros = 0, xSprites = 0, xCpuExpand = 0;
};
extern G787Counters g_g787C;

// GS parse thread entry points (defined in the rasterizer TU, g787_native_composite.inc).
class GS;
void g787OnTagMarker(GS *gs, uint64_t tagHi);  // processGIFPacket: payload-less sprite tag (LIVE)
void g787OnTexflushMarker(uint64_t value);     // writeRegister(TEXFLUSH) magic value (VERIFY)

// ---- the pure geometry: sprite j of a call, exactly as the guest loops it -----------------------
// Floor to the 0x200 grid the way the guest's signed (v + 0x200) / 0x200 * 0x200 does.
inline int32_t g787NextGrid(int32_t v)
{
    const int32_t t = v + 0x200;
    return (t / 0x200) * 0x200;
}

// Enumerate sprites [first, n): fn(j, out) with out = {U0, V0, X0, Y0, U1, V1, X1, Y1} (UV in the
// guest's UV units = rect space, XY absolute GS). Returns the total sprite count. `fn` may return
// false to stop early.
template <typename Fn>
inline uint32_t g787ForEachSprite(const G787Geom &g, uint32_t first, Fn &&fn)
{
    uint32_t j = 0;
    if (g.kind == 1u)
    {
        const int32_t X1 = g.x1 + g.offx16, Y1 = g.y1 + g.offy16;
        for (int32_t cx = g.x0 + g.offx16; cx < X1;)
        {
            int32_t nx = g787NextGrid(cx);
            if (X1 < nx) nx = X1;
            for (int32_t cy = g.y0 + g.offy16; cy < Y1;)
            {
                int32_t ny = g787NextGrid(cy);
                if (Y1 < ny) ny = Y1;
                const int32_t u0 = cx - g.offx16, v0 = cy - g.offy16, u1 = nx - g.offx16, v1 = ny - g.offy16;
                const int32_t dx[4] = {g.d, -g.d, 0, 0}, dy[4] = {0, 0, g.d, -g.d};
                for (int k = 0; k < 4; ++k, ++j)
                {
                    if (j < first) continue;
                    const int32_t o[8] = {u0, v0, cx + dx[k], cy + dy[k], u1, v1, nx + dx[k], ny + dy[k]};
                    if (!fn(j, o)) return j + 1u;
                }
                cy = ny;
            }
            cx = nx;
        }
    }
    else if (g.kind == 2u)
    {
        for (int32_t cx = g.x0; cx < g.x1;)
        {
            int32_t nx = cx + 0x200;
            if (g.x1 < nx) nx = g.x1;
            for (int32_t cy = g.y0; cy < g.y1; ++j)
            {
                int32_t ny = cy + g.screenH16;
                if (g.y1 < ny) ny = g.y1;
                if (j >= first)
                {
                    const int32_t o[8] = {cx, cy, cx + g.offx16, cy + g.offy16, nx, ny, nx + g.offx16, ny + g.offy16};
                    if (!fn(j, o)) return j + 1u;
                }
                if (g.screenH16 <= 0) { ++j; break; } // never loops forever on a bad descriptor
                cy = ny;
            }
            cx = nx;
        }
    }
    return j;
}

inline uint32_t g787SpriteCount(const G787Geom &g)
{
    return g787ForEachSprite(g, 0xFFFFFFFFu, [](uint32_t, const int32_t *) { return true; });
}

// The 4 PACKED qwords (lo, hi pairs: UV0, XYZF0, UV1, XYZF1) the guest writes for one sprite.
inline void g787SpritePacked(const G787Geom &g, const int32_t *o, uint64_t q[8])
{
    auto pack = [](uint32_t w0, uint32_t w1) { return static_cast<uint64_t>(w0) | (static_cast<uint64_t>(w1) << 32); };
    const bool four = g.kind == 1u;
    q[0] = pack(static_cast<uint32_t>(o[0]), static_cast<uint32_t>(o[1]));
    q[1] = four ? pack(g.uvW2a, g.uvW3a) : 0ull;
    q[2] = pack(static_cast<uint32_t>(o[2]), static_cast<uint32_t>(o[3]));
    q[3] = pack(g.xyzW2, g.xyzW3a);
    q[4] = pack(static_cast<uint32_t>(o[4]), static_cast<uint32_t>(o[5]));
    q[5] = four ? pack(g.uvW2b, g.uvW3b) : 0ull;
    q[6] = pack(static_cast<uint32_t>(o[6]), static_cast<uint32_t>(o[7]));
    q[7] = pack(g.xyzW2, g.xyzW3b);
}

#endif // DC2_G787_COMPOSITE_API_INC
