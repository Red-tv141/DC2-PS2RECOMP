// ===== G629: THE GPU-RESIDENT BATCHED REPLAY — CPU MARSHALLING SIDE =====
// revision: 1   (DEFAULT-OFF: DC2_G629_GPU=1 arms it, DC2_G629_VERIFY=1 is the per-pixel oracle)
//
// Included AFTER `g609_tri_scan.inc` because the marshalling reads the same per-draw quantities the
// CPU kernel does (`g530RowAddr`, `g403DisplayZBuf`, `g605BuildBlend`, `fabsQ`, `clampU8`), and the
// whole point of this file is that the two arms compute from ONE source of truth.
//
// ---------------------------------------------------------------------------------------------
// ⭐⭐⭐ THE EXACTNESS ARGUMENT, AND WHY IT IS NOT AN ARGUMENT
//
// G586/G587/G604 each failed exactness by routing this population through GL's FIXED-FUNCTION
// rasteriser, whose coverage rule, interpolation and blend rounding are the DRIVER's, not the GS's.
// This file never touches that path. It hands a COMPUTE shader the same float inputs the CPU kernel
// consumes and asks it to evaluate the same expression tree, then compares every pixel.
//
// The one empirical fact that makes this possible was bought by G570/G571 and is the reason this
// design is not speculative: on this host, **GLSL binary32 reproduces MSVC's float results bit for
// bit when the expression tree is decomposed into named `precise` temporaries** — measured at
// `bad=0` over 15,728,640 pixels on the 0x139 shadow batch. Everything below is the discipline that
// keeps that property:
//
//   * every per-draw float that the CPU computes ONCE (edge coefficients, `invAbsDenom`, `winding`)
//     is computed on the CPU and UPLOADED, never re-derived in GLSL from the vertices;
//   * every per-pixel expression is written in the shader in the SAME association order as
//     `g609TriScanRowT`, one named `precise` temporary per operation;
//   * the texture is pre-decoded on the CPU through the SHIPPED `GSMem::ReadP8` + `lookupCLUT`, so
//     the shader never re-implements the PSMT8 swizzle or the palette — it indexes a linear image;
//   * the CT32 destination swizzle IS re-implemented in GLSL, because it is pure integer LUT
//     arithmetic (`GSPSMCT32::addrPSMCT32`, two constant tables) with no rounding at all;
//   * the blend and the MODULATE combine are integer, including the `>>7` on a possibly-negative
//     product — GLSL's `>>` on a signed int is an arithmetic shift, matching C++.
//
// ⛔ AND NONE OF THAT IS TRUSTED. `DC2_G629_VERIFY=1` runs the CPU band replay FIRST (it is the
// authority and it commits the pixels), snapshots its result, restores the pre-batch destination,
// runs the GPU arm, and compares word for word. The GPU arm's output is discarded under the oracle,
// so a kernel defect cannot reach a pixel while it is armed — G609's contract, one level up.
//
// ---------------------------------------------------------------------------------------------
// WHAT IS UPLOADED PER BATCH, AND WHY IT IS BOUNDED
//
// `[G629:row]` / `[G629:tex]` measured the batch shape on `ridepod` before any of this was written:
// ≤5 distinct textures per batch, one target, ~1,500–2,000 entries, ~2 batches/frame. So:
//
//   colour   the target's CONTIGUOUS VRAM BYTE RANGE, raw. No CPU swizzle in either direction —
//            the shader addresses it with the same block/column tables `writePixel` uses. This is
//            the difference between a memcpy and G571's measured ~1.1 ms/batch of CPU pack/unpack.
//   depth    the G403/G411 private mirror, linear, ONLY when the batch's depth contract is PRIV.
//            G628 proved the ZTST=ALWAYS + ZMSK=1 shape needs no depth stage at all.
//   recs     one fixed-width record per entry, in SUBMISSION ORDER — GS blending is order-dependent
//            per pixel, so the shader walks them in order and the batch is atomic.
//   tex      ≤5 pre-decoded linear RGBA images.
//   tiles    16x16 tile bins with a conservative bbox, so a pixel only walks records that can
//            cover it. G571 measured this as the difference between 31.18 ms and 10.30 ms of GPU
//            interval on its own batch — it is not optional.

namespace
{

// ---------------------------------------------------------------------------------------------
// Arms

static bool g629GpuRequested()
{
    static const bool on = (envFlagEnabled("DC2_G629_GPU") ||
                            envFlagEnabled("DC2_G630_RESIDENT")) &&
                           !envFlagEnabled("DC2_G629_NO_GPU") &&
                           !envFlagEnabled("DC2_G630_NO_RESIDENT");
    return on;
}

static bool g629GpuVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G629_VERIFY");
    return on;
}

// ---------------------------------------------------------------------------------------------
// The record format. Fixed width so the shader indexes it with a multiply, and every field is a
// per-DRAW constant the CPU already has — the shader derives nothing it could get wrong.

enum
{
    kG629RecWords = 48,

    kG629RKind = 0,   // 0 = triangle, 1 = untextured sprite (the batch's leading clear)
    kG629RFlags = 1,  // bit0 TEXTURED, bit1 FST, bit2 ZPRIV, bit3 ZWRITE, bit4 FOG, bit5 ABE
    kG629RMinX = 2,
    kG629RMaxX = 3,
    kG629RMinY = 4,
    kG629RMaxY = 5,
    kG629RA0 = 6,     // (fy1 - fy2)  — precomputed, never re-derived in GLSL
    kG629RB0 = 7,     // (fx2 - fx1)
    kG629RA1 = 8,     // (fy2 - fy0)
    kG629RB1 = 9,     // (fx0 - fx2)
    kG629RFx2 = 10,
    kG629RFy2 = 11,
    kG629RWinding = 12,
    kG629RInvDenom = 13,
    kG629REdgeEps = 14,
    kG629RZ0 = 15,
    kG629RZ1 = 16,
    kG629RZ2 = 17,
    kG629RCol0 = 18,  // packed RGBA8 per vertex
    kG629RCol1 = 19,
    kG629RCol2 = 20,
    kG629RS0 = 21, kG629RT0 = 22, kG629RQ0 = 23,
    kG629RS1 = 24, kG629RT1 = 25, kG629RQ1 = 26,
    kG629RS2 = 27, kG629RT2 = 28, kG629RQ2 = 29,
    kG629RFogPack = 30, // fog0 | fog1<<8 | fog2<<16
    kG629RFogCol = 31,
    kG629RBlend = 32,   // asel | bsel<<2 | csel<<4 | dsel<<6 | fix<<8
    kG629RTexIdx = 33,
    kG629RColorBlock = 34,
    kG629RFbw = 35,
    kG629RU0 = 36, kG629RV0 = 37, // FST 12.4 texel coords, per vertex
    kG629RU1 = 38, kG629RV1 = 39,
    kG629RU2 = 40, kG629RV2 = 41,
    kG629RSprCol = 42,           // sprite flat colour (packed RGBA8)

    kG629FlagTextured = 1u << 0,
    kG629FlagFst = 1u << 1,
    kG629FlagZPriv = 1u << 2,
    kG629FlagZWrite = 1u << 3,
    kG629FlagFog = 1u << 4,
    kG629FlagAbe = 1u << 5,

    // Per-texture descriptor
    kG629TexWords = 10,
    kG629TW = 0, kG629TH = 1, kG629TWrapU = 2, kG629TWrapV = 3,
    kG629TMinU = 4, kG629TMaxU = 5, kG629TOffset = 6, kG629TMinV = 7, kG629TMaxV = 8,

    kG629TileSize = 16,
};

// Reused across batches so a steady-state batch allocates nothing (`clear()` keeps capacity).
static std::vector<uint32_t> g_g629Recs;
static std::vector<uint32_t> g_g629TexDesc;
static std::vector<uint32_t> g_g629TexData;
static std::vector<uint32_t> g_g629TexMinV;
static std::vector<uint32_t> g_g629TexMaxV;
static std::vector<uint32_t> g_g629TileOff;
static std::vector<uint32_t> g_g629TileRef;
static std::vector<uint32_t> g_g629Color;
static std::vector<uint32_t> g_g629ColorOut;
static std::vector<uint32_t> g_g629Depth;
static std::vector<uint32_t> g_g629Snapshot;

struct G629Window
{
    uint32_t firstPage = 0u, pageCount = 0u;
    bool valid = false;
};

// Oracle handoff state: the GPU candidate is produced BEFORE the CPU arm runs, kept here, and
// compared after the CPU commits.
static G629Window g_g629VerifyWin{};
static bool g_g629VerifyArmed = false;
static int g_g629VerifyW = 0, g_g629VerifyH = 0, g_g629VerifyBlock = 0, g_g629VerifyFbw = 0;

struct G629TexSlot
{
    uint64_t texKey = 0ull, clutKey = 0ull;
    uint32_t offset = 0u, w = 0u, h = 0u;
};
static std::vector<G629TexSlot> g_g629TexSlots;

static inline uint32_t g629PackRgba(const GSVertex &v)
{
    return static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) |
           (static_cast<uint32_t>(v.b) << 16) | (static_cast<uint32_t>(v.a) << 24);
}

static inline uint32_t g629Bits(float f)
{
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// ---------------------------------------------------------------------------------------------
// Texture decode. Runs the SHIPPED `GSMem::ReadP8` and the SHIPPED CLUT the band replay would have
// used, once per distinct texture, into a linear `texW * texH` RGBA image. The shader then does
// only `wrap()` + an index — it never re-implements the PSMT8 block swizzle or the palette, which
// is where an independent GPU implementation would most easily diverge.
// ⛔ NOTE ON ACCESS. This file is a free-function anonymous-namespace unit, so it CANNOT touch
// `GS`'s private members or call `GSRasterizer::lookupCLUT` — only the flush closure (a member
// context) can. The palette is therefore filled through the SAME `G580ClutFillFn` callback the CPU
// arm already builds at the call site (`g605ClutFill`), which is strictly better than a private
// re-implementation: both arms provably decode the palette with one piece of code.
static uint32_t g629DecodeTexture(uint8_t *vram, const G580ClutFillFn &clutFill,
                                  const G144Entry &e, uint32_t texW, uint32_t texH)
{
    const uint32_t offset = static_cast<uint32_t>(g_g629TexData.size());
    g_g629TexData.resize(offset + static_cast<size_t>(texW) * texH);

    std::array<uint32_t, 256> clut{};
    clutFill(e, clut);
    const uint32_t tbp0 = e.ctx.tex0.tbp0;
    // ⛔ `pageBw` is NOT `tex0.tbw`. `G580SharedSample` (g580_cpu_triangle.inc:234) stores
    // `max(1, tbw >> 1)`, and the CPU sampler passes THAT to `GSMem::ReadP8`. Decoding with the raw
    // `tbw` reads the PSMT8 texture at double the page stride, which produced garbage indices and a
    // saturated-white palette entry — the first oracle run's `cpu=80131a94 gpu=80ffffff`.
    const uint32_t pageBw = (e.ctx.tex0.tbw >> 1u) < 1u ? 1u
                          : static_cast<uint32_t>(e.ctx.tex0.tbw) >> 1u;
    for (uint32_t y = 0; y < texH; ++y)
        for (uint32_t x = 0; x < texW; ++x)
        {
            const uint8_t idx = static_cast<uint8_t>(
                GSMem::ReadP8(vram, tbp0, pageBw, x, y));
            g_g629TexData[offset + static_cast<size_t>(y) * texW + x] = clut[idx];
        }
    return offset;
}

// ---------------------------------------------------------------------------------------------
// The target's contiguous VRAM byte window. Bounded exactly the way `g609RowWithinVram` bounds a
// row, but over the whole batch, so the shader's computed offsets provably land inside the uploaded
// range and the batch is refused rather than clamped if they would not.
static G629Window g629TargetWindow(const G144Entry &e0, int maxY, uint32_t vramSize)
{
    G629Window w{};
    const uint32_t block = GSInternal::framePageBaseToBlock(e0.ctx.frame.fbp);
    const uint32_t fbw = e0.ctx.frame.fbw;
    if (fbw == 0u || maxY < 0)
        return w;
    w.firstPage = block >> 5u;
    // `g530GroupBase` reaches page = pageRow + (x>>6) with pageRow = firstPage + (y>>5)*fbw, and the
    // `(blockId >> 5) << 13` term can add exactly one more page. `+2` covers that and the final
    // word, which is what makes the shader's bound a proof rather than a hope.
    const uint32_t lastPage = w.firstPage +
                              (static_cast<uint32_t>(maxY) >> 5u) * fbw +
                              ((fbw * 64u - 1u) >> 6u) + 2u;
    if (((lastPage + 1u) << 13u) > vramSize)
        return w;
    w.pageCount = lastPage - w.firstPage + 1u;
    w.valid = true;
    return w;
}

// ---------------------------------------------------------------------------------------------
// RECORD BUILD. Every float below is computed with the SAME expression `drawTriangle` uses, in the
// same order, from the same captured entry — `fx0 = v.x - float(ofx)`, `denom`, `winding`,
// `invAbsDenom`. The shader then consumes them; it never re-derives any of them, which is what
// keeps the two arms' per-pixel arithmetic identical rather than merely equivalent.
static bool g629BuildRecords(uint8_t *vram, const G580ClutFillFn &clutFill,
                             const std::vector<G144Entry> &list,
                             int &outMinY, int &outMaxY)
{
    g_g629Recs.clear();
    g_g629TexDesc.clear();
    g_g629TexData.clear();
    g_g629TexSlots.clear();

    outMinY = 0x7fffffff;
    outMaxY = -0x7fffffff;

    for (const G144Entry &e : list)
    {
        const GSContext &ctx = e.ctx;
        const size_t base = g_g629Recs.size();
        g_g629Recs.resize(base + kG629RecWords, 0u);
        uint32_t *R = g_g629Recs.data() + base;

        const int ofx = static_cast<int>(ctx.xyoffset.ofx) >> 4;
        const int ofy = static_cast<int>(ctx.xyoffset.ofy) >> 4;
        const bool isSpr = (e.prim.type == GS_PRIM_SPRITE);

        uint32_t flags = 0u;
        if (e.prim.tme) flags |= kG629FlagTextured;
        if (e.prim.fst) flags |= kG629FlagFst;
        if (e.prim.abe) flags |= kG629FlagAbe;

        // Depth contract, recomputed here from the captured entry exactly as the classifier does.
        const uint32_t zte = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
        const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
        const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
        const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
        const bool zmsk = ((ctx.zbuf >> 32) & 0x1u) != 0u;
        const bool zEnabled = g203UniversalZEnabled() && zte != 0u;
        const bool zWrite = zEnabled && !zmsk && ztst != 0u;
        const bool privMirror = g203UniversalZEnabled() &&
                                g404SharedZScope(ctx.frame.fbp, ctx.frame.fbw, zbpPage, zpsm);
        const bool zPriv = zEnabled && (zpsm == 0x1u) && ztst == 2u &&
                           zbpPage == 0xd0u && privMirror;
        if (zPriv) flags |= kG629FlagZPriv;
        if (zPriv && zWrite) flags |= kG629FlagZWrite;

        const G605Blend bl = g605BuildBlend(e.prim.abe != 0, ctx.alpha);
        R[kG629RBlend] = static_cast<uint32_t>(bl.asel) |
                         (static_cast<uint32_t>(bl.bsel) << 2) |
                         (static_cast<uint32_t>(bl.csel) << 4) |
                         (static_cast<uint32_t>(bl.dsel) << 6) |
                         (static_cast<uint32_t>(bl.fix) << 8);
        R[kG629RColorBlock] = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        R[kG629RFbw] = ctx.frame.fbw;

        if (isSpr)
        {
            // The batch's leading clear. Two vertices; the GS fills the clipped rect with the
            // SECOND vertex's colour, which is what the untextured sprite leaf commits.
            R[kG629RKind] = 1u;
            // ⛔⛔ THE SPRITE RECT IS **G365's CEIL RULE**, NOT A TRUNCATED MIN/MAX.
            // `drawSprite` (rasterizer_draw_sprite.inc:249..288) takes ceil of both corners, treats
            // the second as EXCLUSIVE, and converts back with `span = max(1, x1 - x0)` /
            // `X1 = x0 + span - 1`. This file's first version truncated instead, which made the
            // batch's leading CLEAR one pixel short on each axis. The oracle found it as
            // `xy=(448,377) tileRecs=1 bboxHits=1 cpu=9c817c7a gpu=00000000` — a pixel whose ONLY
            // covering record was the clear, written by the CPU and never visited by the shader.
            float sx0 = e.v[0].x - static_cast<float>(ofx);
            float sy0 = e.v[0].y - static_cast<float>(ofy);
            float sx1 = e.v[1].x - static_cast<float>(ofx);
            float sy1 = e.v[1].y - static_cast<float>(ofy);
            if (sx0 > sx1) std::swap(sx0, sx1);
            if (sy0 > sy1) std::swap(sy0, sy1);
            int x0 = static_cast<int>(std::ceil(sx0));
            int y0 = static_cast<int>(std::ceil(sy0));
            int x1 = static_cast<int>(std::ceil(sx1));
            int y1 = static_cast<int>(std::ceil(sy1));
            // A collapsed span covers NO pixel on hardware and `drawSprite` returns early. Refuse
            // the batch rather than emit a record that would silently disagree.
            if (x1 <= x0 || y1 <= y0)
                return false;
            x1 = x0 + std::max(1, x1 - x0) - 1;
            y1 = y0 + std::max(1, y1 - y0) - 1;
            x0 = clampInt(x0, ctx.scissor.x0, ctx.scissor.x1);
            x1 = clampInt(x1, ctx.scissor.x0, ctx.scissor.x1);
            y0 = clampInt(y0, ctx.scissor.y0, ctx.scissor.y1);
            y1 = clampInt(y1, ctx.scissor.y0, ctx.scissor.y1);
            R[kG629RMinX] = static_cast<uint32_t>(x0);
            R[kG629RMaxX] = static_cast<uint32_t>(x1);
            R[kG629RMinY] = static_cast<uint32_t>(y0);
            R[kG629RMaxY] = static_cast<uint32_t>(y1);
            R[kG629RSprCol] = g629PackRgba(e.v[1]);
            R[kG629RFlags] = flags & kG629FlagAbe;
            if (y0 < outMinY) outMinY = y0;
            if (y1 > outMaxY) outMaxY = y1;
            // ⛔ The CPU row pool is dispatched over the ENTRY's captured `minY..maxY`, not over a
            // bbox this file recomputes. If the two ever disagree the CPU writes rows the shader
            // never visits, and those words read back as the uploaded pre-state — the
            // `gpu=00000000` mismatches. Fold the captured band in so the covered region is a
            // superset of what the CPU touches.
            if (e.minY < outMinY) outMinY = e.minY;
            if (e.maxY > outMaxY) outMaxY = e.maxY;
            continue;
        }

        const GSVertex &v0 = e.v[0], &v1 = e.v[1], &v2 = e.v[2];
        const float fx0 = v0.x - static_cast<float>(ofx);
        const float fy0 = v0.y - static_cast<float>(ofy);
        const float fx1 = v1.x - static_cast<float>(ofx);
        const float fy1 = v1.y - static_cast<float>(ofy);
        const float fx2 = v2.x - static_cast<float>(ofx);
        const float fy2 = v2.y - static_cast<float>(ofy);
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (!(denom != 0.0f))
            return false; // degenerate — the CPU path returns early; refuse the batch instead
        const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
        const float invAbsDenom = 1.0f / std::fabs(denom);

        int minX = clampInt(static_cast<int>(std::floor(std::min({fx0, fx1, fx2}))),
                            ctx.scissor.x0, ctx.scissor.x1);
        int maxX = clampInt(static_cast<int>(std::ceil(std::max({fx0, fx1, fx2}))),
                            ctx.scissor.x0, ctx.scissor.x1);
        int minY = clampInt(static_cast<int>(std::floor(std::min({fy0, fy1, fy2}))),
                            ctx.scissor.y0, ctx.scissor.y1);
        int maxY = clampInt(static_cast<int>(std::ceil(std::max({fy0, fy1, fy2}))),
                            ctx.scissor.y0, ctx.scissor.y1);

        R[kG629RKind] = 0u;
        R[kG629RMinX] = static_cast<uint32_t>(minX);
        R[kG629RMaxX] = static_cast<uint32_t>(maxX);
        R[kG629RMinY] = static_cast<uint32_t>(minY);
        R[kG629RMaxY] = static_cast<uint32_t>(maxY);
        R[kG629RA0] = g629Bits(fy1 - fy2);
        R[kG629RB0] = g629Bits(fx2 - fx1);
        R[kG629RA1] = g629Bits(fy2 - fy0);
        R[kG629RB1] = g629Bits(fx0 - fx2);
        R[kG629RFx2] = g629Bits(fx2);
        R[kG629RFy2] = g629Bits(fy2);
        R[kG629RWinding] = g629Bits(winding);
        R[kG629RInvDenom] = g629Bits(invAbsDenom);
        R[kG629REdgeEps] = g629Bits(1.0e-4f);
        R[kG629RZ0] = g629Bits(v0.z);
        R[kG629RZ1] = g629Bits(v1.z);
        R[kG629RZ2] = g629Bits(v2.z);
        R[kG629RCol0] = g629PackRgba(v0);
        R[kG629RCol1] = g629PackRgba(v1);
        R[kG629RCol2] = g629PackRgba(v2);
        R[kG629RS0] = g629Bits(v0.s); R[kG629RT0] = g629Bits(v0.t); R[kG629RQ0] = g629Bits(v0.q);
        R[kG629RS1] = g629Bits(v1.s); R[kG629RT1] = g629Bits(v1.t); R[kG629RQ1] = g629Bits(v1.q);
        R[kG629RS2] = g629Bits(v2.s); R[kG629RT2] = g629Bits(v2.t); R[kG629RQ2] = g629Bits(v2.q);
        R[kG629RU0] = v0.u; R[kG629RV0] = v0.v;
        R[kG629RU1] = v1.u; R[kG629RV1] = v1.v;
        R[kG629RU2] = v2.u; R[kG629RV2] = v2.v;

        if (e.prim.fge && g364FogEnabled())
        {
            flags |= kG629FlagFog;
            R[kG629RFogPack] = static_cast<uint32_t>(v0.fog) |
                               (static_cast<uint32_t>(v1.fog) << 8) |
                               (static_cast<uint32_t>(v2.fog) << 16);
            R[kG629RFogCol] = e.fogcol;
        }

        if (e.prim.tme)
        {
            const uint32_t texW = 1u << (ctx.tex0.tw & 0xFu);
            const uint32_t texH = 1u << (ctx.tex0.th & 0xFu);
            const uint64_t texKey = (static_cast<uint64_t>(ctx.tex0.tbp0) << 24) |
                                    (static_cast<uint64_t>(ctx.tex0.tbw) << 16) |
                                    (static_cast<uint64_t>(ctx.tex0.tw) << 8) |
                                    static_cast<uint64_t>(ctx.tex0.th);
            const uint64_t clutKey = (static_cast<uint64_t>(ctx.tex0.cbp) << 32) |
                                     (static_cast<uint64_t>(ctx.tex0.csa) << 24) |
                                     (static_cast<uint64_t>(e.texclut.cbw) << 16) |
                                     (static_cast<uint64_t>(e.texclut.cou) << 8) |
                                     static_cast<uint64_t>(e.texclut.cov);
            uint32_t slot = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < g_g629TexSlots.size(); ++i)
                if (g_g629TexSlots[i].texKey == texKey && g_g629TexSlots[i].clutKey == clutKey)
                { slot = i; break; }
            if (slot == 0xFFFFFFFFu)
            {
                if (g_g629TexSlots.size() >= 32u)
                    return false; // the census measured <=5; refuse rather than grow unbounded
                G629TexSlot s{};
                s.texKey = texKey; s.clutKey = clutKey;
                s.w = texW; s.h = texH;
                s.offset = g629DecodeTexture(vram, clutFill, e, texW, texH);
                slot = static_cast<uint32_t>(g_g629TexSlots.size());
                g_g629TexSlots.push_back(s);

                const int minU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
                const int maxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
                const int minV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
                const int maxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
                const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
                const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2) & 0x3u);
                // ⛔ REGION modes can address outside the decoded rect, where the CPU's ReadP8
                // would still read real VRAM. Refuse rather than clamp — a clamp would be a
                // SILENT divergence, which is the one failure mode this design cannot tolerate.
                if (wrapU > 1u || wrapV > 1u)
                {
                    if (minU < 0 || maxU >= static_cast<int>(texW) ||
                        minV < 0 || maxV >= static_cast<int>(texH))
                        return false;
                }
                const size_t d = g_g629TexDesc.size();
                g_g629TexDesc.resize(d + kG629TexWords, 0u);
                g_g629TexDesc[d + kG629TW] = texW;
                g_g629TexDesc[d + kG629TH] = texH;
                g_g629TexDesc[d + kG629TWrapU] = wrapU;
                g_g629TexDesc[d + kG629TWrapV] = wrapV;
                g_g629TexDesc[d + kG629TMinU] = static_cast<uint32_t>(minU);
                g_g629TexDesc[d + kG629TMaxU] = static_cast<uint32_t>(maxU);
                g_g629TexDesc[d + kG629TOffset] = s.offset;
                g_g629TexDesc[d + kG629TMinV] = static_cast<uint32_t>(minV);
                g_g629TexDesc[d + kG629TMaxV] = static_cast<uint32_t>(maxV);
            }
            R[kG629RTexIdx] = slot;
        }

        R[kG629RFlags] = flags;
        if (minY < outMinY) outMinY = minY;
        if (maxY > outMaxY) outMaxY = maxY;
        if (e.minY < outMinY) outMinY = e.minY;
        if (e.maxY > outMaxY) outMaxY = e.maxY;
        // ⛔ The shader's grid is `fbw * 64` wide. A scissor may legitimately reach past that (the
        // CT32 addressing simply runs into the next page), and the CPU rasteriser will write those
        // pixels — so a batch that can reach them must FAIL CLOSED here rather than be silently
        // half-replayed.
        if (maxX >= static_cast<int>(ctx.frame.fbw) * 64)
            return false;
    }
    return !g_g629Recs.empty() && outMaxY >= outMinY;
}

// ---------------------------------------------------------------------------------------------
// TILE BINNING. Conservative bbox only — G571 measured tile culling as the single largest term in
// its GPU interval (31.18 ms -> 10.30 ms), and a bbox bin can only ever RETAIN work, never drop a
// covered pixel, so it cannot change a pixel.
static void g629BuildTiles(int width, int height, int &tilesX, int &tilesY)
{
    tilesX = (width + kG629TileSize - 1) / kG629TileSize;
    tilesY = (height + kG629TileSize - 1) / kG629TileSize;
    const size_t tileCount = static_cast<size_t>(tilesX) * tilesY;
    const uint32_t recCount = static_cast<uint32_t>(g_g629Recs.size() / kG629RecWords);

    std::vector<uint32_t> counts(tileCount + 1u, 0u);
    for (uint32_t r = 0; r < recCount; ++r)
    {
        const uint32_t *R = g_g629Recs.data() + static_cast<size_t>(r) * kG629RecWords;
        const int tx0 = static_cast<int>(R[kG629RMinX]) / kG629TileSize;
        const int tx1 = static_cast<int>(R[kG629RMaxX]) / kG629TileSize;
        const int ty0 = static_cast<int>(R[kG629RMinY]) / kG629TileSize;
        const int ty1 = static_cast<int>(R[kG629RMaxY]) / kG629TileSize;
        for (int ty = ty0; ty <= ty1 && ty < tilesY; ++ty)
            for (int tx = tx0; tx <= tx1 && tx < tilesX; ++tx)
                if (tx >= 0 && ty >= 0)
                    ++counts[static_cast<size_t>(ty) * tilesX + tx];
    }
    g_g629TileOff.assign(tileCount + 1u, 0u);
    uint32_t run = 0u;
    for (size_t t = 0; t < tileCount; ++t)
    {
        g_g629TileOff[t] = run;
        run += counts[t];
    }
    g_g629TileOff[tileCount] = run;
    g_g629TileRef.assign(run, 0u);
    std::vector<uint32_t> cursor(g_g629TileOff.begin(), g_g629TileOff.end() - 1);
    for (uint32_t r = 0; r < recCount; ++r)
    {
        const uint32_t *R = g_g629Recs.data() + static_cast<size_t>(r) * kG629RecWords;
        const int tx0 = static_cast<int>(R[kG629RMinX]) / kG629TileSize;
        const int tx1 = static_cast<int>(R[kG629RMaxX]) / kG629TileSize;
        const int ty0 = static_cast<int>(R[kG629RMinY]) / kG629TileSize;
        const int ty1 = static_cast<int>(R[kG629RMaxY]) / kG629TileSize;
        for (int ty = ty0; ty <= ty1 && ty < tilesY; ++ty)
            for (int tx = tx0; tx <= tx1 && tx < tilesX; ++tx)
                if (tx >= 0 && ty >= 0)
                    g_g629TileRef[cursor[static_cast<size_t>(ty) * tilesX + tx]++] = r;
    }
}

// ---------------------------------------------------------------------------------------------
// Counters. A lever with no path counter cannot prove its arms diverged
// (`g538_null_ab_needs_a_path_counter`), and an oracle whose denominator is not printed beside its
// `bad` is `g589_oracle_with_zero_checks` waiting to happen.
static std::atomic<uint64_t> g_g629GpuBatchCount{0}, g_g629Refused{0}, g_g629Failed{0};
static std::atomic<uint64_t> g_g629Checks{0}, g_g629Bad{0}, g_g629VerifyBatches{0};
// Words inside the uploaded PAGE window that no target pixel maps to. Not a defect — reported so
// the covered denominator is never quietly smaller than the window.
static std::atomic<uint64_t> g_g629Uncov{0};
static std::atomic<uint64_t> g_g629GpuNs{0}, g_g629PrepNs{0};

static void g629Report(bool force)
{
    const uint64_t n = g_g629GpuBatchCount.load(std::memory_order_relaxed);
    if (!force && (n == 0u || (n % 256ull) != 0ull))
        return;
    std::fprintf(stderr,
                 "[G629:gpu] batches=%llu refused=%llu failed=%llu prep=%.1fms gpu=%.1fms "
                 "| oracle batches=%llu checks=%llu bad=%llu uncovered=%llu\n",
                 (unsigned long long)n,
                 (unsigned long long)g_g629Refused.load(std::memory_order_relaxed),
                 (unsigned long long)g_g629Failed.load(std::memory_order_relaxed),
                 (double)g_g629PrepNs.load(std::memory_order_relaxed) / 1e6,
                 (double)g_g629GpuNs.load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g629VerifyBatches.load(std::memory_order_relaxed),
                 (unsigned long long)g_g629Checks.load(std::memory_order_relaxed),
                 (unsigned long long)g_g629Bad.load(std::memory_order_relaxed),
                 (unsigned long long)g_g629Uncov.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// THE ENTRY POINT. Returns true when the GPU arm has fully replayed the batch and the caller must
// NOT run the CPU pool. Returns false for every refusal — the batch then takes the untouched CPU
// path, so every failure mode here is a performance loss and never a correctness one.
static bool g629TryReplayGpu(std::vector<G144Entry> &list, GS *gs, uint8_t *vram,
                             uint32_t vramSize, const G580ClutFillFn &clutFill)
{
    if (!g629GpuRequested() || list.empty() || vram == nullptr)
        return false;
    if (!g629BatchAdmissible(list))
    {
        g_g629Refused.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    // G630 keeps target aliases in one raw image. CPU-side PSMT8 decode remains the one admitted
    // sampler source, so publish only a rare GPU-owned texture/CLUT overlap before decoding it.
    g630PrepareGpuReplayInputs(list);

    const auto tPrep0 = std::chrono::steady_clock::now();
    int recMinY = 0, recMaxY = 0;
    if (!g629BuildRecords(vram, clutFill, list, recMinY, recMaxY))
    {
        g_g629Refused.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    const G144Entry &e0 = list[0];
    const int width = static_cast<int>(e0.ctx.frame.fbw) * 64;
    // The shader's grid starts at row 0, so the covered region is rows [0, recMaxY]. `recMinY` is
    // only used to prove the batch is non-empty.
    const int height = recMaxY + 1;
    if (width <= 0 || height <= 0 || height > static_cast<int>(kG403DisplayZH) ||
        width > static_cast<int>(kG403DisplayZW))
    {
        g_g629Refused.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    const G629Window win = g629TargetWindow(e0, recMaxY, vramSize);
    if (!win.valid)
    {
        g_g629Refused.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    int tilesX = 0, tilesY = 0;
    g629BuildTiles(width, height, tilesX, tilesY);

    // Does any record need the private depth mirror?
    bool wantDepth = false;
    const uint32_t recCount = static_cast<uint32_t>(g_g629Recs.size() / kG629RecWords);
    for (uint32_t r = 0; r < recCount; ++r)
        if (g_g629Recs[static_cast<size_t>(r) * kG629RecWords + kG629RFlags] & kG629FlagZPriv)
        { wantDepth = true; break; }

    if (g630DomainOn())
    {
        auto packet = g630TakeReplayPacket();
        packet->recs.swap(g_g629Recs);
        packet->texDesc.swap(g_g629TexDesc);
        packet->texData.swap(g_g629TexData);
        packet->tileOff.swap(g_g629TileOff);
        packet->tileRef.swap(g_g629TileRef);
        packet->width = width;
        packet->height = height;
        packet->tilesX = tilesX;
        packet->block = static_cast<int>(GSInternal::framePageBaseToBlock(e0.ctx.frame.fbp));
        packet->fbw = static_cast<int>(e0.ctx.frame.fbw);
        packet->wantDepth = wantDepth;
        const bool submitted = g630SubmitReplay(packet, list, gs, vram, vramSize,
                                                win.firstPage, win.pageCount,
                                                e0.ctx.frame.fbp, e0.ctx.frame.fbw, height);
        if (!submitted)
        {
            g_g629Recs.swap(packet->recs);
            g_g629TexDesc.swap(packet->texDesc);
            g_g629TexData.swap(packet->texData);
            g_g629TileOff.swap(packet->tileOff);
            g_g629TileRef.swap(packet->tileRef);
            g_g629Failed.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        g_g629GpuBatchCount.fetch_add(1u, std::memory_order_relaxed);
        g629Report(false);
        return true;
    }

    // ---- the uploads: raw VRAM window + (optionally) the linear private mirror ----
    const size_t colorWords = static_cast<size_t>(win.pageCount) * (8192u / 4u);
    const uint8_t *vsrc = vram + (static_cast<size_t>(win.firstPage) << 13u);
    g_g629Color.resize(colorWords);
    std::memcpy(g_g629Color.data(), vsrc, colorWords * 4u);
    if (wantDepth)
    {
        std::vector<uint32_t> &zbuf = g403DisplayZBuf();
        g_g629Depth.assign(zbuf.begin(), zbuf.end());
    }
    else
        g_g629Depth.clear();

    const bool verify = g629GpuVerifyOn();
    if (verify)
    {
        // ⭐ THE ORACLE. Snapshot the PRE-batch destination, let the SHIPPED CPU band replay produce
        // the authoritative result, capture it, restore the pre-state, run the GPU arm on identical
        // inputs, compare, and finally put the CPU's result back. The GPU arm's output is therefore
        // never committed while the oracle is armed — a kernel defect cannot reach a pixel.
        g_g629Snapshot.assign(g_g629Color.begin(), g_g629Color.end());
    }
    const auto tPrep1 = std::chrono::steady_clock::now();
    g_g629PrepNs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tPrep1 - tPrep0).count()),
        std::memory_order_relaxed);

    const auto tGpu0 = std::chrono::steady_clock::now();
    const bool ok = g629_backend_replay(g_g629Recs, g_g629TexDesc, g_g629TexData,
                                        g_g629TileOff, g_g629TileRef,
                                        g_g629Color, g_g629Depth,
                                        width, height, tilesX,
                                        static_cast<int>(GSInternal::framePageBaseToBlock(
                                            e0.ctx.frame.fbp)),
                                        static_cast<int>(e0.ctx.frame.fbw),
                                        static_cast<int>(win.firstPage), wantDepth);
    g_g629GpuNs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tGpu0).count()),
        std::memory_order_relaxed);

    if (!ok)
    {
        g_g629Failed.fetch_add(1u, std::memory_order_relaxed);
        g629Report(false);
        return false; // fail closed: the CPU pool still runs, output unchanged
    }

    if (verify)
    {
        // Keep the GPU candidate, restore the pre-state, and hand the batch back to the caller so
        // the SHIPPED CPU replay writes the authoritative pixels. The comparison happens in
        // g629VerifyAfterCpu, once the CPU result exists.
        g_g629ColorOut.assign(g_g629Color.begin(), g_g629Color.end());
        std::memcpy(vram + (static_cast<size_t>(win.firstPage) << 13u),
                    g_g629Snapshot.data(), colorWords * 4u);
        g_g629VerifyWin = win;
        g_g629VerifyW = width;
        g_g629VerifyH = height;
        g_g629VerifyBlock = static_cast<int>(GSInternal::framePageBaseToBlock(e0.ctx.frame.fbp));
        g_g629VerifyFbw = static_cast<int>(e0.ctx.frame.fbw);
        g_g629VerifyArmed = true;
        g_g629GpuBatchCount.fetch_add(1u, std::memory_order_relaxed);
        g629Report(false);
        return false; // the CPU arm runs and commits; we compare against it afterwards
    }

    // ---- commit: the raw window straight back into guest VRAM, plus the mirror ----
    std::memcpy(vram + (static_cast<size_t>(win.firstPage) << 13u),
                g_g629Color.data(), colorWords * 4u);
    if (wantDepth && g_g629Depth.size() == g403DisplayZBuf().size())
        std::memcpy(g403DisplayZBuf().data(), g_g629Depth.data(), g_g629Depth.size() * 4u);

    g_g629GpuBatchCount.fetch_add(1u, std::memory_order_relaxed);
    g629Report(false);
    return true;
}

// Called immediately AFTER the CPU pool has run, when the oracle armed this batch. Compares the
// GPU candidate captured before the CPU arm ran against what the CPU actually committed.
static void g629VerifyAfterCpu(uint8_t *vram)
{
    if (!g_g629VerifyArmed || vram == nullptr)
        return;
    g_g629VerifyArmed = false;
    const size_t words = g_g629ColorOut.size();
    if (words == 0u)
        return;
    const uint32_t *cpu = reinterpret_cast<const uint32_t *>(
        vram + (static_cast<size_t>(g_g629VerifyWin.firstPage) << 13u));
    // ⭐ THE DENOMINATOR HAS TO BE THE SHADER'S OWN PIXELS.
    // The uploaded window is a contiguous PAGE range, so it necessarily contains words that no
    // (x,y) inside `uWidth x uHeight` maps to — CT32 pages are 64x32 and a target is neither. For
    // those words `g_g629ColorOut` still holds the pre-batch value, so if the CPU arm writes any of
    // them the comparison reports a difference that says nothing about the kernel. Rebuild exactly
    // the set of words the shader is RESPONSIBLE for, using the same address math it uses, and
    // report both figures: `bad` over the raw window and `badCov` over the kernel's own pixels.
    // Reporting both is the point — collapsing to the covered set alone could hide a real defect
    // in which the kernel writes a word it should not have.
    static std::vector<uint8_t> s_cov;
    static std::vector<int32_t> s_covXY; // packed y<<16|x for the pixel that owns each word
    s_cov.assign(words, 0u);
    s_covXY.assign(words, -1);
    const uint32_t blk = static_cast<uint32_t>(g_g629VerifyBlock);
    const uint32_t fbwv = (g_g629VerifyFbw != 0) ? static_cast<uint32_t>(g_g629VerifyFbw) : 1u;
    for (int y = 0; y < g_g629VerifyH; ++y)
        for (int x = 0; x < g_g629VerifyW; ++x)
        {
            const uint32_t page = (blk >> 5u) + (static_cast<uint32_t>(y) >> 5u) * fbwv +
                                  (static_cast<uint32_t>(x) >> 6u);
            const uint32_t blockId = (blk & 31u) +
                GSPSMCT32::blockTable32[(static_cast<uint32_t>(y) >> 3u) & 3u]
                                       [(static_cast<uint32_t>(x) >> 3u) & 7u];
            const uint32_t off = ((page - g_g629VerifyWin.firstPage) << 13u) +
                                 ((blockId >> 5u) << 13u) + (blockId & 31u) * 256u +
                                 GSPSMCT32::columnTable32[static_cast<uint32_t>(y) & 7u]
                                                         [static_cast<uint32_t>(x) & 7u] * 4u;
            const size_t w = off >> 2u;
            if (w < words)
            {
                s_cov[w] = 1u;
                s_covXY[w] = (y << 16) | x;
            }
        }

    uint64_t bad = 0u, badCov = 0u, covWords = 0u;
    size_t firstBad = static_cast<size_t>(-1), firstBadCov = static_cast<size_t>(-1);
    for (size_t i = 0; i < words; ++i)
    {
        if (s_cov[i])
            ++covWords;
        if (cpu[i] == g_g629ColorOut[i])
            continue;
        if (firstBad == static_cast<size_t>(-1))
            firstBad = i;
        ++bad;
        if (s_cov[i])
        {
            if (firstBadCov == static_cast<size_t>(-1))
                firstBadCov = i;
            ++badCov;
        }
    }
    g_g629Checks.fetch_add(covWords, std::memory_order_relaxed);
    g_g629Bad.fetch_add(badCov, std::memory_order_relaxed);
    g_g629Uncov.fetch_add(bad - badCov, std::memory_order_relaxed);
    g_g629VerifyBatches.fetch_add(1u, std::memory_order_relaxed);
    if (bad != 0u)
    {
        static std::atomic<uint32_t> s_shown{0u};
        const uint32_t k = s_shown.fetch_add(1u, std::memory_order_relaxed);
        if (k < 24u)
        {
            int bx = -1, by = -1, tile = -1, tileRecs = -1, hits = -1;
            if (firstBadCov != static_cast<size_t>(-1) && s_covXY[firstBadCov] >= 0)
            {
                bx = s_covXY[firstBadCov] & 0xFFFF;
                by = (s_covXY[firstBadCov] >> 16) & 0xFFFF;
                const int tilesX = (g_g629VerifyW + kG629TileSize - 1) / kG629TileSize;
                tile = (by / kG629TileSize) * tilesX + (bx / kG629TileSize);
                if (tile >= 0 && static_cast<size_t>(tile) + 1u < g_g629TileOff.size())
                    tileRecs = static_cast<int>(g_g629TileOff[tile + 1] - g_g629TileOff[tile]);
                hits = 0;
                const uint32_t rc =
                    static_cast<uint32_t>(g_g629Recs.size() / kG629RecWords);
                for (uint32_t r = 0; r < rc; ++r)
                {
                    const uint32_t *R = g_g629Recs.data() + static_cast<size_t>(r) * kG629RecWords;
                    if (bx >= static_cast<int>(R[kG629RMinX]) &&
                        bx <= static_cast<int>(R[kG629RMaxX]) &&
                        by >= static_cast<int>(R[kG629RMinY]) &&
                        by <= static_cast<int>(R[kG629RMaxY]))
                        ++hits;
                }
            }
            std::fprintf(stderr,
                         "[G629:bad] k=%u bad=%llu badCov=%llu cov=%llu/%zu W=%d H=%d recs=%zu | "
                         "firstBadCov w=%zd xy=(%d,%d) tile=%d tileRecs=%d bboxHits=%d "
                         "cpu=%08x gpu=%08x\n",
                         k, (unsigned long long)bad, (unsigned long long)badCov,
                         (unsigned long long)covWords, words,
                         g_g629VerifyW, g_g629VerifyH,
                         g_g629Recs.size() / kG629RecWords,
                         (firstBadCov == static_cast<size_t>(-1))
                             ? (ptrdiff_t)-1 : (ptrdiff_t)firstBadCov,
                         bx, by, tile, tileRecs, hits,
                         (firstBadCov == static_cast<size_t>(-1))
                             ? 0u : cpu[firstBadCov],
                         (firstBadCov == static_cast<size_t>(-1))
                             ? 0u : g_g629ColorOut[firstBadCov]);
        }
    }
    g629Report(false);
}

} // namespace
