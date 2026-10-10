// ===== G787 — OUTLINE COMPOSITE FRONT END (plans/phase-G787-ledger.md) ==========================
// COutLineDraw::Draw(ff) (0x17C2D0) renders an outlined character into its work RTT W
// (mgDrawDirect at 0x17C614, the "silhouette") and then puts it on screen with two composites that
// sample W through a 32x32-pixel tiling: DrawDivSprite4 (0x17CB20, 4 sprites per tile offset by
// +-d in x and y, outline colour) and DrawDivSprite (0x17C8F0, one sprite per 32-px column, the
// body image), followed by a one-register TEXFLUSH prim (Begin/Direct(0x3f)/End). Both composites
// are mgCDrawPrim PATH2 packets: VIF1 DMA CALL -> DIRECT -> GS. No VU1.
//
// ⛔ WHY G786's DC2_G786_OLSKIP composite bits never armed: they keyed on the G785 family mask
// (g_mask bit 10 = "inside Draw(ff)"). The EE unwinds to the dispatcher inside Draw(ff) (the
// family census sees only ~0.25 MDT seams per outline2 call inside the mask), and the rest of the
// function — every composite call — resumes from a mid-function entry AFTER familyWrapper<10>
// restored g_mask. The probes below key on the CALL SITE (the return address), which every
// resumed or direct path still carries.
//
// Env (diagnostic / timing probes only; none is a correctness configuration):
//   DC2_G787_OLSKIP=<hex>  register the call-site wrappers; skip (visibly corrupting):
//                          bit0 the silhouette mgDrawDirect (ra 0x17C61C)
//                          bit1 DrawDivSprite4 (ra 0x17C7D0)          outline copies
//                          bit2 DrawDivSprite  (ra 0x17C888)          body copy
//                          bit3 the trailing TEXFLUSH prim (Begin 0x17C894 / Direct 0x17C8A4 / End 0x17C8AC)
//                          0 = symmetric control: wrappers registered, nothing skipped.
//   DC2_G787_CCENSUS=1     composite contract census ([G787:cc] every 600 SF, first samples in full).
//
// NATIVE COMPOSITE (g_g787Mode, resolved in the rasterizer TU from DC2_G787_NATIVE / _NO_NATIVE;
// DEFAULT ON since g778g787_e, rollback DC2_G787_NO_NATIVE=1):
//   live   — each composite call runs with an EMPTY rect (the guest still emits its exact state
//            header: TEXFLUSH, PRMODECONT, DrawEnv, TEX1/TEX0, RGBAQ and the sprite GIF tag with
//            NLOOP 0), the header's first TEXFLUSH value is patched to a marker carrying the
//            descriptor seq, and the GS parse thread regenerates the payload from the descriptor
//            (ps2_g787_composite_api.inc).
//   verify — the legacy payload runs unchanged; the EE compares it BYTE FOR BYTE with the
//            descriptor's expansion, and the marker (verify bit) makes the parse thread compare every
//            captured sprite with its own expansion. Shadow oracle; nothing is suppressed.
#include "../lib/ps2_g787_composite_api.inc"
namespace g787 {
using RecompiledFunction = PS2Runtime::RecompiledFunction;

inline bool skipArmed()
{
    static const bool v = std::getenv("DC2_G787_OLSKIP") != nullptr;
    return v;
}
inline uint32_t skipMask()
{
    static const uint32_t v = [] {
        const char *e = std::getenv("DC2_G787_OLSKIP");
        return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    return v;
}
inline bool ccensusOn()
{
    static const bool v = std::getenv("DC2_G787_CCENSUS") != nullptr;
    return v;
}

constexpr uint32_t kDrawDirect = 0x00142FD0u, kDiv4 = 0x0017CB20u, kDiv = 0x0017C8F0u;
constexpr uint32_t kBegin = 0x001344A0u, kDirect = 0x00134D80u, kEnd = 0x00134690u, kEnd2 = 0x00134A20u;
constexpr uint32_t kRaSil = 0x0017C61Cu, kRaDiv4 = 0x0017C7D0u, kRaDiv = 0x0017C888u;
constexpr uint32_t kRaTfBegin = 0x0017C894u, kRaTfDirect = 0x0017C8A4u, kRaTfEnd = 0x0017C8ACu;
// mgCDrawPrim::End2 called at the end of DrawDivSprite (0x17cae8) / DrawDivSprite4 (0x17ce08).
constexpr uint32_t kRaEnd2Div = 0x0017CAF0u, kRaEnd2Div4 = 0x0017CE10u;

enum Site : uint32_t { SSil = 0, SDiv4, SDiv, STfBegin, STfDirect, STfEnd, SEnd2, SCount };
inline const char *const kSiteName[SCount] = {"sil", "div4", "div", "tfBegin", "tfDirect", "tfEnd", "end2"};
inline RecompiledFunction prev[SCount]{};
inline uint64_t calls[SCount]{}, skips[SCount]{};

// Census (EE thread only).
struct Cc
{
    uint64_t div4Calls = 0, divCalls = 0, div4Tiles = 0, divCols = 0, div4Sprites = 0, divSprites = 0;
    uint64_t eeNs[2]{};
    int offLo = 1 << 30, offHi = -(1 << 30), zLo = 1 << 30, zHi = -(1 << 30), zNonZero = 0;
    int wLo = 1 << 30, wHi = 0, hLo = 1 << 30, hHi = 0;
    uint32_t tex0Lo = ~0u, tex0Hi = 0u, colAlo = 1u << 30, colAhi = 0u;
    uint32_t samples = 0;
    uint32_t lastSf = 0;
};
inline Cc cc;

inline uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

inline void ccReport()
{
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf / 600u == cc.lastSf / 600u)
        return;
    cc.lastSf = sf;
    std::fprintf(stderr,
                 "[G787:cc] sf=%u div4=%llu tiles=%llu spr=%llu eeUs=%.1f div=%llu cols=%llu spr=%llu eeUs=%.1f "
                 "off=%d..%d z=%d..%d zNZ=%d w16=%d..%d h16=%d..%d tex0lo=%x..%x alpha=%u..%u\n",
                 sf, (unsigned long long)cc.div4Calls, (unsigned long long)cc.div4Tiles,
                 (unsigned long long)cc.div4Sprites, cc.eeNs[0] / 1000.0, (unsigned long long)cc.divCalls,
                 (unsigned long long)cc.divCols, (unsigned long long)cc.divSprites, cc.eeNs[1] / 1000.0, cc.offLo,
                 cc.offHi, cc.zLo, cc.zHi, cc.zNonZero, cc.wLo, cc.wHi, cc.hLo, cc.hHi, cc.tex0Lo, cc.tex0Hi,
                 cc.colAlo, cc.colAhi);
}

inline void skipReport()
{
    static uint32_t last = 0;
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf / 600u == last / 600u)
        return;
    last = sf;
    std::fprintf(stderr, "[G787:skip] sf=%u mask=0x%x", sf, skipMask());
    for (uint32_t s = 0; s < SCount; ++s)
        std::fprintf(stderr, " %s=%llu/%llu", kSiteName[s], (unsigned long long)skips[s],
                     (unsigned long long)calls[s]);
    std::fputc('\n', stderr);
}

// rect: mgRect<int> {x0, y0, x1, y1} in 1/16 screen pixels.
inline void ccNote(uint8_t *rdram, R5900Context *ctx, bool four)
{
    const uint32_t rect = getRegU32(ctx, 5) & 0x1fffffffu;
    const uint32_t tex = getRegU32(ctx, 6) & 0x1fffffffu;
    const uint32_t col = getRegU32(ctx, 7) & 0x1fffffffu;
    const int x0 = static_cast<int>(dc2_read_u32(rdram, rect + 0u)), y0 = static_cast<int>(dc2_read_u32(rdram, rect + 4u));
    const int x1 = static_cast<int>(dc2_read_u32(rdram, rect + 8u)), y1 = static_cast<int>(dc2_read_u32(rdram, rect + 12u));
    const int off = four ? static_cast<int>(getRegU32(ctx, 8)) : 0;
    const int z = four ? static_cast<int>(getRegU32(ctx, 9)) : static_cast<int>(getRegU32(ctx, 10));
    const uint32_t tex0lo = tex ? dc2_read_u32(rdram, tex + 0x38u) : 0u;
    const uint32_t a = col ? dc2_read_u32(rdram, col + 12u) : 0u;
    // Tiling exactly as the guest loops it (DrawDivSprite4: 0x200 x 0x200 tiles; DrawDivSprite:
    // 0x200-wide columns of screen height — counted as columns here).
    uint64_t tiles = 0, cols = 0;
    for (int x = x0; x < x1;)
    {
        int nx = ((x + 0x200) >> 9) << 9;
        if (!four) nx = x + 0x200;
        if (nx > x1) nx = x1;
        ++cols;
        if (four)
            for (int y = y0; y < y1;)
            {
                int ny = ((y + 0x200) >> 9) << 9;
                if (ny > y1) ny = y1;
                ++tiles;
                y = ny;
            }
        x = nx;
    }
    if (four) { ++cc.div4Calls; cc.div4Tiles += tiles; cc.div4Sprites += tiles * 4u; }
    else { ++cc.divCalls; cc.divCols += cols; cc.divSprites += cols; }
    cc.offLo = std::min(cc.offLo, off); cc.offHi = std::max(cc.offHi, off);
    cc.zLo = std::min(cc.zLo, z); cc.zHi = std::max(cc.zHi, z);
    if (z != 0) ++cc.zNonZero;
    cc.wLo = std::min(cc.wLo, x1 - x0); cc.wHi = std::max(cc.wHi, x1 - x0);
    cc.hLo = std::min(cc.hLo, y1 - y0); cc.hHi = std::max(cc.hHi, y1 - y0);
    cc.tex0Lo = std::min(cc.tex0Lo, tex0lo); cc.tex0Hi = std::max(cc.tex0Hi, tex0lo);
    cc.colAlo = std::min(cc.colAlo, a); cc.colAhi = std::max(cc.colAhi, a);
    if (cc.samples < 24u)
    {
        ++cc.samples;
        std::fprintf(stderr,
                     "[G787:ccs] %s prim=%x rect=(%d,%d)-(%d,%d) off=%d z=%d tex=%x tex0=%08x%08x tex1=%08x%08x "
                     "col=(%u,%u,%u,%u) tiles=%llu cols=%llu\n",
                     four ? "div4" : "div", getRegU32(ctx, 4), x0, y0, x1, y1, off, z, tex,
                     tex ? dc2_read_u32(rdram, tex + 0x3cu) : 0u, tex0lo,
                     tex ? dc2_read_u32(rdram, tex + 0x44u) : 0u, tex ? dc2_read_u32(rdram, tex + 0x40u) : 0u,
                     col ? dc2_read_u32(rdram, col) : 0u, col ? dc2_read_u32(rdram, col + 4u) : 0u,
                     col ? dc2_read_u32(rdram, col + 8u) : 0u, a, (unsigned long long)tiles,
                     (unsigned long long)cols);
    }
}

// ---- native composite (EE seam) -----------------------------------------------------------------
inline uint32_t g_seq = 0;
struct PendingPatch { uint32_t active = 0, prim = 0, seq = 0; };
inline PendingPatch g_pending;

inline void ncReport()
{
    static uint32_t last = 0;
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf / 600u == last / 600u)
        return;
    last = sf;
    const G787Counters &c = g_g787C;
    std::fprintf(stderr,
                 "[G787:nc] sf=%u mode=%d ee=%llu live=%llu verify=%llu vBytesBad=%llu patchMiss=%llu empty=%llu "
                 "markers=%llu orphans=%llu kicked=%llu macros=%llu macroSpr=%llu primNot2=%llu "
                 "vChecks=%llu vBad=%llu vForeign=%llu vCulled=%llu xMacros=%llu xSpr=%llu xCpu=%llu\n",
                 sf, g_g787Mode, (unsigned long long)c.eeCalls, (unsigned long long)c.eeLive,
                 (unsigned long long)c.eeVerify, (unsigned long long)c.eeVerifyBad,
                 (unsigned long long)c.eePatchMiss, (unsigned long long)c.eeEmpty, (unsigned long long)c.markers,
                 (unsigned long long)c.orphans, (unsigned long long)c.kicked, (unsigned long long)c.macros,
                 (unsigned long long)c.macroSprites, (unsigned long long)c.primNot2,
                 (unsigned long long)c.vChecks, (unsigned long long)c.vBad, (unsigned long long)c.vForeign,
                 (unsigned long long)c.vCulled, (unsigned long long)c.xMacros, (unsigned long long)c.xSprites,
                 (unsigned long long)c.xCpuExpand);
}

// Engine parameters of the call being entered (a0 prim, a1 rect, t0/t1/t2 the int args).
inline G787Geom readGeom(uint8_t *rdram, R5900Context *ctx, bool four)
{
    G787Geom g;
    const uint32_t rect = getRegU32(ctx, 5) & 0x1fffffffu;
    const uint32_t gp = getRegU32(ctx, 28);
    g.x0 = static_cast<int32_t>(dc2_read_u32(rdram, rect + 0u));
    g.y0 = static_cast<int32_t>(dc2_read_u32(rdram, rect + 4u));
    g.x1 = static_cast<int32_t>(dc2_read_u32(rdram, rect + 8u));
    g.y1 = static_cast<int32_t>(dc2_read_u32(rdram, rect + 12u));
    const int32_t offx = static_cast<int32_t>(dc2_read_u32(rdram, gp - 0x7868u)); // mgScreenOffx
    const int32_t offy = static_cast<int32_t>(dc2_read_u32(rdram, gp - 0x7864u)); // mgScreenOffy
    const int32_t scrH = static_cast<int32_t>(dc2_read_u32(rdram, gp - 0x787Cu)); // mgScreenHeight
    g.screenH16 = scrH * 16;
    if (four)
    {
        g.kind = 1u;
        g.d = static_cast<int32_t>(getRegU32(ctx, 8));  // param_5
        g.xyzW2 = getRegU32(ctx, 9);                     // param_6
        g.offx16 = offx * 16;
        g.offy16 = offy * 16;
        // The stack templates DrawDivSprite4 copies from 0x3d0700/10/20/30 (XYZ0, XYZ1, UV0, UV1).
        g.xyzW3a = dc2_read_u32(rdram, 0x003d070Cu);
        g.xyzW3b = dc2_read_u32(rdram, 0x003d071Cu);
        g.uvW2a = dc2_read_u32(rdram, 0x003d0728u);
        g.uvW3a = dc2_read_u32(rdram, 0x003d072Cu);
        g.uvW2b = dc2_read_u32(rdram, 0x003d0738u);
        g.uvW3b = dc2_read_u32(rdram, 0x003d073Cu);
    }
    else
    {
        g.kind = 2u;
        g.offx16 = static_cast<int32_t>(getRegU32(ctx, 8)) + offx * 16; // param_5 + offset
        g.offy16 = static_cast<int32_t>(getRegU32(ctx, 9)) + offy * 16; // param_6 + offset
        g.xyzW2 = getRegU32(ctx, 10);                                     // param_7 (z)
    }
    return g;
}

// The guest's own side buffer for this call: prim+0xd4 = Begin2's allocation (DMA CNT tag, DIRECT,
// GIF A+D tag, then TEXFLUSH(0) at +0x20). Returns 0 when the header does not have that shape.
inline uint32_t headerTexflush(uint8_t *rdram, uint32_t prim)
{
    const uint32_t buf = dc2_read_u32(rdram, prim + 0xd4u) & 0x1fffffffu;
    if (buf == 0u || dc2_read_u32(rdram, buf + 0u) != 0x10000007u || dc2_read_u32(rdram, buf + 0xCu) != 0x50000007u ||
        dc2_read_u64(rdram, buf + 0x20u) != 0ull || dc2_read_u64(rdram, buf + 0x28u) != 0x3full)
        return 0u;
    return buf + 0x20u;
}

inline uint32_t publish(const G787Geom &g, uint32_t sprites, bool verify)
{
    const uint32_t seq = ++g_seq;
    G787Desc &d = g_g787Ring[seq & (kG787Ring - 1u)];
    d.g = g;
    d.verify = verify ? 1u : 0u;
    d.sprites = sprites;
    std::atomic_thread_fence(std::memory_order_release);
    d.seq = seq;
    return seq;
}

// Verify: the payload the legacy call just wrote (the sprite GIF tag is at prim+0xe8) against the
// descriptor's expansion, qword for qword. Returns false on any difference.
inline bool verifyPayload(uint8_t *rdram, uint32_t prim, const G787Geom &g, uint32_t sprites)
{
    const uint32_t tag = dc2_read_u32(rdram, prim + 0xe8u) & 0x1fffffffu;
    const uint64_t lo = dc2_read_u64(rdram, tag), hi = dc2_read_u64(rdram, tag + 8u);
    if (tag == 0u || hi != 0x43ull || ((lo >> 60) & 0xFull) != 2ull || (lo & 0x7FFFull) != 2ull * sprites)
        return false;
    bool ok = true;
    uint32_t at = tag + 0x10u;
    g787ForEachSprite(g, 0u, [&](uint32_t, const int32_t *o) {
        uint64_t q[8];
        g787SpritePacked(g, o, q);
        for (int k = 0; k < 8 && ok; ++k)
            ok = dc2_read_u64(rdram, at + 8u * static_cast<uint32_t>(k)) == q[k];
        at += 0x40u;
        return ok;
    });
    return ok;
}

// Returns true when the call was handled here (the caller must not run the original again).
inline bool nativeCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, RecompiledFunction orig,
                       bool four, uint32_t ra)
{
    ++g_g787C.eeCalls;
    ncReport();
    const G787Geom g = readGeom(rdram, ctx, four);
    const uint32_t sprites = g787SpriteCount(g);
    const uint32_t prim = getRegU32(ctx, 4) & 0x1fffffffu;
    const uint32_t rect = getRegU32(ctx, 5) & 0x1fffffffu;
    const uint32_t entryPc = four ? kDiv4 : kDiv;
    // A generated body either leaves pc at its entry or sets it to ra on `jr ra`; anything else
    // means the EE unwound to the dispatcher inside the call.
    auto returned = [&]() {
        if (ctx->pc != ra && ctx->pc != entryPc)
            return false;
        ctx->pc = ra;
        return true;
    };
    if (g_g787Mode == 2)
    {
        orig(rdram, ctx, runtime);
        if (!returned())
            return true; // unwound: nothing to verify
        ++g_g787C.eeVerify;
        if (!verifyPayload(rdram, prim, g, sprites))
        {
            if (++g_g787C.eeVerifyBad <= 8u)
                std::fprintf(stderr, "[G787:vbytes] BAD kind=%u rect=(%d,%d)-(%d,%d) d=%d off=(%d,%d) h16=%d sprites=%u\n",
                             g.kind, g.x0, g.y0, g.x1, g.y1, g.d, g.offx16, g.offy16, g.screenH16, sprites);
            return true;
        }
        const uint32_t tf = headerTexflush(rdram, prim);
        if (tf == 0u) { ++g_g787C.eePatchMiss; return true; }
        const uint32_t seq = publish(g, sprites, true);
        const uint64_t mv = g787MarkerValue(seq, true);
        dc2_write_u32(rdram, tf, static_cast<uint32_t>(mv));
        dc2_write_u32(rdram, tf + 4u, static_cast<uint32_t>(mv >> 32));
        return true;
    }
    if (sprites == 0u)
    {
        ++g_g787C.eeEmpty;
        return false; // nothing to regenerate: run the original as is
    }
    // LIVE: the original runs with an EMPTY rect, so it emits its exact header and a payload-less
    // sprite tag. The marker goes into that tag from the End2 wrapper below (the tag is final after
    // EndPrim2), which a preempted-and-resumed body still reaches through the dispatcher.
    const uint32_t savedX1 = dc2_read_u32(rdram, rect + 8u);
    dc2_write_u32(rdram, rect + 8u, dc2_read_u32(rdram, rect + 0u));
    g_pending.seq = publish(g, sprites, false);
    g_pending.prim = prim;
    g_pending.active = 1u;
    orig(rdram, ctx, runtime);
    // The guest read the rect at entry (before any preemptible back-edge); restore it either way.
    dc2_write_u32(rdram, rect + 8u, savedX1);
    if (returned() && g_pending.active)
    {
        ++g_g787C.eePatchMiss; // returned without reaching End2: nothing will draw this composite
        g_pending.active = 0u;
    }
    return true;
}

// mgCDrawPrim::End2 inside a LIVE composite call: the sprite tag (prim+0xe8, written by
// BeginPrim2(6, 0x43, 0, 2), finalised by EndPrim2) must be PACKED, PRE, NREG 2, NLOOP 0, REGS 0x43.
// With NLOOP 0 the GIF never reads its REGS qword; it carries the descriptor to the parse thread
// (processGIFPacket -> g787OnTagMarker).
inline void placeMarker(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t prim = getRegU32(ctx, 4) & 0x1fffffffu;
    if (!g_pending.active || prim != g_pending.prim)
        return;
    g_pending.active = 0u;
    const uint32_t tag = dc2_read_u32(rdram, prim + 0xe8u) & 0x1fffffffu;
    const uint64_t tagLo = tag ? dc2_read_u64(rdram, tag) : 0ull;
    const bool tagOk = tag != 0u && (tagLo & 0x7FFFull) == 0ull && ((tagLo >> 46) & 1ull) == 1ull &&
                       ((tagLo >> 58) & 3ull) == 0ull && ((tagLo >> 60) & 0xFull) == 2ull &&
                       dc2_read_u64(rdram, tag + 8u) == 0x43ull;
    if (!tagOk)
    {
        if (++g_g787C.eePatchMiss <= 8u)
            std::fprintf(stderr, "[G787:tag] unexpected sprite tag prim=%x tag=%x lo=%016llx\n", prim, tag,
                         (unsigned long long)tagLo);
        return;
    }
    const uint64_t mv = g787MarkerValue(g_pending.seq, false);
    dc2_write_u32(rdram, tag + 8u, static_cast<uint32_t>(mv));
    dc2_write_u32(rdram, tag + 12u, static_cast<uint32_t>(mv >> 32));
    ++g_g787C.eeLive;
}

template <uint32_t S>
void siteWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if constexpr (S == SEnd2)
    {
        if (ra == kRaEnd2Div || ra == kRaEnd2Div4)
            placeMarker(rdram, ctx);
        prev[S](rdram, ctx, runtime);
        return;
    }
    constexpr uint32_t kRa[SCount] = {kRaSil, kRaDiv4, kRaDiv, kRaTfBegin, kRaTfDirect, kRaTfEnd, 0u};
    if (ra != kRa[S])
    {
        prev[S](rdram, ctx, runtime);
        return;
    }
    ++calls[S];
    skipReport();
    const uint32_t bit = S <= SDiv ? (1u << S) : 8u;
    if (skipMask() & bit)
    {
        ++skips[S];
        if (S == SSil)
            setReturnU32(ctx, 0u); // mgDrawDirect: v0 = 0 qwords
        ctx->pc = ra;
        return;
    }
    if constexpr (S == SDiv4 || S == SDiv)
    {
        if (g_g787Mode != 0 && nativeCall(rdram, ctx, runtime, prev[S], S == SDiv4, ra))
            return;
        if (ccensusOn())
        {
            ccNote(rdram, ctx, S == SDiv4);
            const uint64_t t0 = nowNs();
            prev[S](rdram, ctx, runtime);
            cc.eeNs[S == SDiv4 ? 0 : 1] += nowNs() - t0;
            ccReport();
            return;
        }
    }
    prev[S](rdram, ctx, runtime);
}
} // namespace g787

static void g787_register_outline_probes(PS2Runtime &runtime)
{
    if (!g787::skipArmed() && !g787::ccensusOn() && g_g787Mode == 0)
        return;
    const uint32_t addr[g787::SCount] = {g787::kDrawDirect, g787::kDiv4, g787::kDiv,
                                         g787::kBegin,      g787::kDirect, g787::kEnd, g787::kEnd2};
    const g787::RecompiledFunction w[g787::SCount] = {
        &g787::siteWrapper<0>, &g787::siteWrapper<1>, &g787::siteWrapper<2>,
        &g787::siteWrapper<3>, &g787::siteWrapper<4>, &g787::siteWrapper<5>, &g787::siteWrapper<6>};
    // Native-only configuration: just the two composite call sites (the generic mgCDrawPrim
    // Begin/Direct/End and mgDrawDirect stay unwrapped).
    const bool probes = g787::skipArmed() || g787::ccensusOn();
    for (uint32_t s = 0; s < g787::SCount; ++s)
    {
        if (!probes && s != g787::SDiv4 && s != g787::SDiv && s != g787::SEnd2)
            continue;
        if (s == g787::SEnd2 && g_g787Mode != 1)
            continue; // the marker placement exists for the LIVE mode only
        g787::prev[s] = runtime.lookupFunction(addr[s]);
        if (!g787::prev[s]) { std::fprintf(stderr, "[G787:probe] FATAL no function at %x\n", addr[s]); std::abort(); }
        runtime.registerFunction(addr[s], w[s]);
    }
    std::fprintf(stderr, "[G787:probe] armed olskip=%d mask=0x%x ccensus=%d native=%d\n", g787::skipArmed() ? 1 : 0,
                 g787::skipMask(), g787::ccensusOn() ? 1 : 0, g_g787Mode);
}
