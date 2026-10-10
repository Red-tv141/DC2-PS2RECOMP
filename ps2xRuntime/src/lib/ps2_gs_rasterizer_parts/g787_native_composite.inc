// ===== G787 — NATIVE OUTLINE COMPOSITE (plans/phase-G787-ledger.md, ps2_g787_composite_api.inc) =====
// Rasterizer-TU side. Included right after rasterizer_tilebinning_and_probes.inc (needs G144Entry),
// at global scope (the GS TU calls the two marker entry points).
//
// LIVE: the EE emitted the composite's exact header and a payload-less sprite tag whose REGS qword
// carries the descriptor seq. At that tag (after its PRE PRIM write, i.e. with every header register
// applied) the parse thread regenerates the payload from the descriptor:
//   sprite j is kicked with the same register writes the PACKED payload would make (A+D UV /
//   XYZF2|XYZF3: identical to the PACKED decode when DC2_G65_FORCEDRAW / DC2_G105_SYNTH_ADC are off,
//   the process contract), so culling, deferral and every front-door effect are the legacy ones.
//   The first kick that reaches the G144 DEFER push is converted IN PLACE into a macro entry that
//   covers sprites [j, n): x/y/u/v become the union, v[2] (zero on every legacy sprite) carries the
//   geometry, and the remaining sprites are not kicked. g260NoteAppend reads state only (whole
//   scissor / texture ranges), so the batch's page sets are unchanged.
//   A kick that is culled or taken inline leaves the hook armed and the next sprite is kicked:
//   until a deferral happens the composite IS the legacy one, sprite for sprite.
// The executor expands a macro in the vertex build with the per-entry code unchanged (one call per
// sprite, same state index); the CPU replay expands it back into legacy entries first.
//
// VERIFY: the legacy payload runs; the Begin2 TEXFLUSH marker arms a shadow comparison of every
// pushed composite sprite against the descriptor's expansion (x, y, u, v exact; z, fog, rgba, q,
// s, t identical to the first pushed sprite of the call — the template the macro expansion uses).

#include <intrin.h>
#include <cstring>

G787Desc g_g787Ring[kG787Ring];
G787Counters g_g787C;
int g_g787Mode = [] {
    // Static initialization: plain getenv only (envFlagEnabled reaches a census in another TU).
    auto on = [](const char *n) {
        const char *v = std::getenv(n);
        return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
    };
    if (on("DC2_G787_NO_NATIVE") || std::getenv("DC2_G787_OLSKIP") != nullptr || on("DC2_G65_FORCEDRAW") ||
        on("DC2_G105_SYNTH_ADC"))
        return 0;
    // G787 sec.10: native composite is DEFAULT ON in the experimental runtime (every gate passed:
    // plans/phase-G787-ledger.md sec.10). Rollback DC2_G787_NO_NATIVE=1 (or DC2_G787_NATIVE=0).
    const char *e = std::getenv("DC2_G787_NATIVE");
    if (e == nullptr)
        return 1;
    if (std::strcmp(e, "verify") == 0)
        return 2;
    return std::atoi(e) != 0 ? 1 : 0;
}();

struct G787PushHook
{
    uint32_t mode = 0;     // 0 off, 1 convert the next pushed sprite into a macro, 2 verify
    uint32_t consumed = 0; // mode 1: set by the push that converted
    uint32_t start = 0;    // mode 1: the sprite being kicked
    uint32_t seq = 0;
    G787Geom g;
    uint32_t sprites = 0;
    // verify cursor + the template of the first pushed sprite
    uint32_t cursor = 0, matched = 0, haveRef = 0;
    GSVertex ref[2]{};
};
static G787PushHook g_g787Push;

static void g787Add(uint64_t &c, uint64_t n)
{
    _InterlockedExchangeAdd64(reinterpret_cast<volatile long long *>(&c), static_cast<long long>(n));
}

static const G787Desc *g787Lookup(uint32_t seq)
{
    const G787Desc &d = g_g787Ring[seq & (kG787Ring - 1u)];
    if (d.seq != seq)
        return nullptr;
    std::atomic_thread_fence(std::memory_order_acquire);
    return &d;
}

static void g787VerifyFinish()
{
    G787PushHook &h = g_g787Push;
    if (h.mode != 2u)
        return;
    g_g787C.vCulled += h.sprites - h.matched;
    h.mode = 0u;
}

// GS parse thread, writeRegister(TEXFLUSH) with the magic value: a VERIFY call's header.
void g787OnTexflushMarker(uint64_t value)
{
    g787VerifyFinish();
    ++g_g787C.markers;
    const uint32_t seq = static_cast<uint32_t>(value);
    const G787Desc *d = g787Lookup(seq);
    if (d == nullptr || d->verify == 0u)
    {
        ++g_g787C.orphans;
        return;
    }
    G787PushHook &h = g_g787Push;
    h = G787PushHook{};
    h.mode = 2u;
    h.seq = seq;
    h.g = d->g;
    h.sprites = d->sprites;
}

static uint64_t g787XyzValue(int32_t x, int32_t y, uint32_t w2, uint32_t w3)
{
    // The PACKED XYZF2 decode: x lo16, y lo16 of word 1, z = word2 bits 4..27, fog = word3 bits 4..11.
    return static_cast<uint64_t>(static_cast<uint16_t>(x)) |
           (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16) |
           (static_cast<uint64_t>((w2 >> 4) & 0xFFFFFFu) << 32) |
           (static_cast<uint64_t>((w3 >> 4) & 0xFFu) << 56);
}

// GS parse thread, processGIFPacket: a payload-less sprite tag carrying a LIVE descriptor.
void g787OnTagMarker(GS *gs, uint64_t tagHi)
{
    g787VerifyFinish();
    ++g_g787C.markers;
    const uint32_t seq = static_cast<uint32_t>(tagHi);
    const G787Desc *d = g787Lookup(seq);
    if (d == nullptr || d->verify != 0u)
    {
        ++g_g787C.orphans; // nothing to draw: the EE already dropped this payload
        return;
    }
    const G787Geom g = d->g;
    G787PushHook &h = g_g787Push;
    bool covered = false;
    g787ForEachSprite(g, 0u, [&](uint32_t j, const int32_t *o) {
        h = G787PushHook{};
        h.mode = 1u;
        h.start = j;
        h.seq = seq;
        h.g = g;
        h.sprites = d->sprites;
        gs->writeRegister(GS_REG_UV, static_cast<uint64_t>(static_cast<uint16_t>(o[0])) |
                                         (static_cast<uint64_t>(static_cast<uint16_t>(o[1])) << 16));
        gs->writeRegister(((g.xyzW3a >> 15) & 1u) ? GS_REG_XYZF3 : GS_REG_XYZF2,
                          g787XyzValue(o[2], o[3], g.xyzW2, g.xyzW3a));
        gs->writeRegister(GS_REG_UV, static_cast<uint64_t>(static_cast<uint16_t>(o[4])) |
                                         (static_cast<uint64_t>(static_cast<uint16_t>(o[5])) << 16));
        gs->writeRegister(((g.xyzW3b >> 15) & 1u) ? GS_REG_XYZF3 : GS_REG_XYZF2,
                          g787XyzValue(o[6], o[7], g.xyzW2, g.xyzW3b));
        ++g_g787C.kicked;
        covered = h.consumed != 0u;
        return !covered; // a macro now covers the rest of the call
    });
    if (covered)
    {
        ++g_g787C.macros;
        g_g787C.macroSprites += d->sprites - h.start;
    }
    h.mode = 0u;
}

// ---- macro entry -------------------------------------------------------------------------------
static inline bool g787IsMacro(const G144Entry &e)
{
    return e.prim.type == GS_PRIM_SPRITE && e.v[2].q == kG787MacroQ;
}

static G787Geom g787MacroGeom(const G144Entry &e, uint32_t &first)
{
    const GSVertex &t = e.v[2];
    G787Geom g;
    g.x0 = static_cast<int32_t>(t.x);
    g.y0 = static_cast<int32_t>(t.y);
    g.x1 = static_cast<int32_t>(t.s);
    g.y1 = static_cast<int32_t>(t.t);
    g.d = static_cast<int32_t>(t.z);
    g.offx16 = static_cast<int32_t>(t.u);
    g.offy16 = static_cast<int32_t>(t.v);
    g.screenH16 = static_cast<int32_t>(t.r) | (static_cast<int32_t>(t.g) << 8);
    g.kind = t.b;
    first = static_cast<uint32_t>(t.a) | (static_cast<uint32_t>(t.fog) << 8);
    return g;
}

// The sprite's bbox rows exactly as the G144 capture computes them (sprite formula, scissor clamp).
static void g787SpriteRows(G144Entry &e)
{
    const int cofy = e.ctx.xyoffset.ofy >> 4;
    const float sfy0 = e.v[0].y - static_cast<float>(cofy);
    const float sfy1 = e.v[1].y - static_cast<float>(cofy);
    const int lo = static_cast<int>(std::floor(std::min(sfy0, sfy1)));
    const int hi = static_cast<int>(std::ceil(std::max(sfy0, sfy1)));
    const int y0 = static_cast<int>(e.ctx.scissor.y0), y1 = static_cast<int>(e.ctx.scissor.y1);
    e.minY = lo < y0 ? y0 : (lo > y1 ? y1 : lo);
    e.maxY = hi < y0 ? y0 : (hi > y1 ? y1 : hi);
}

static inline void g787SetXyuv(GSVertex &v, int32_t x, int32_t y, int32_t u, int32_t vv)
{
    v.x = static_cast<float>(static_cast<uint16_t>(x)) / 16.0f;
    v.y = static_cast<float>(static_cast<uint16_t>(y)) / 16.0f;
    v.u = static_cast<uint16_t>(u);
    v.v = static_cast<uint16_t>(vv);
}

// Capture push site (GSRasterizer::drawPrimitive, parse thread): `e` is about to be pushed.
static void g787OnPush(G144Entry &e)
{
    G787PushHook &h = g_g787Push;
    if (h.mode == 1u)
    {
        if (e.prim.type != GS_PRIM_SPRITE)
            return;
        // Union of sprites [start, n), and the tag.
        int32_t mnx = 1 << 30, mny = 1 << 30, mxx = -(1 << 30), mxy = -(1 << 30);
        int32_t mnu = 1 << 30, mnv = 1 << 30, mxu = -(1 << 30), mxv = -(1 << 30);
        g787ForEachSprite(h.g, h.start, [&](uint32_t, const int32_t *o) {
            mnu = std::min(mnu, std::min(o[0], o[4])); mxu = std::max(mxu, std::max(o[0], o[4]));
            mnv = std::min(mnv, std::min(o[1], o[5])); mxv = std::max(mxv, std::max(o[1], o[5]));
            mnx = std::min(mnx, std::min(o[2], o[6])); mxx = std::max(mxx, std::max(o[2], o[6]));
            mny = std::min(mny, std::min(o[3], o[7])); mxy = std::max(mxy, std::max(o[3], o[7]));
            return true;
        });
        g787SetXyuv(e.v[0], mnx, mny, mnu, mnv);
        g787SetXyuv(e.v[1], mxx, mxy, mxu, mxv);
        g787SpriteRows(e);
        GSVertex t{};
        t.q = kG787MacroQ;
        t.x = static_cast<float>(h.g.x0);
        t.y = static_cast<float>(h.g.y0);
        t.s = static_cast<float>(h.g.x1);
        t.t = static_cast<float>(h.g.y1);
        t.z = static_cast<float>(h.g.d);
        t.u = static_cast<uint16_t>(h.g.offx16);
        t.v = static_cast<uint16_t>(h.g.offy16);
        t.r = static_cast<uint8_t>(h.g.screenH16 & 0xFF);
        t.g = static_cast<uint8_t>((h.g.screenH16 >> 8) & 0xFF);
        t.b = static_cast<uint8_t>(h.g.kind);
        t.a = static_cast<uint8_t>(h.start & 0xFF);
        t.fog = static_cast<uint8_t>((h.start >> 8) & 0xFF);
        e.v[2] = t;
        h.consumed = 1u;
        h.mode = 0u;
        return;
    }
    // VERIFY: a non-sprite push ends the call's payload.
    if (e.prim.type != GS_PRIM_SPRITE)
    {
        g787VerifyFinish();
        return;
    }
    bool found = false;
    uint32_t at = h.cursor;
    g787ForEachSprite(h.g, h.cursor, [&](uint32_t j, const int32_t *o) {
        GSVertex a = e.v[0], b = e.v[1];
        g787SetXyuv(a, o[2], o[3], o[0], o[1]);
        g787SetXyuv(b, o[6], o[7], o[4], o[5]);
        if (std::memcmp(&a, &e.v[0], sizeof(GSVertex)) == 0 && std::memcmp(&b, &e.v[1], sizeof(GSVertex)) == 0)
        {
            found = true;
            at = j;
            return false;
        }
        return true;
    });
    if (!found)
    {
        ++g_g787C.vForeign;
        g787VerifyFinish();
        return;
    }
    ++g_g787C.vChecks;
    if (!h.haveRef)
    {
        h.ref[0] = e.v[0];
        h.ref[1] = e.v[1];
        h.haveRef = 1u;
    }
    else
    {
        // Every non-geometry field must equal the call's first pushed sprite (the macro template).
        GSVertex a = h.ref[0], b = h.ref[1];
        a.x = e.v[0].x; a.y = e.v[0].y; a.u = e.v[0].u; a.v = e.v[0].v;
        b.x = e.v[1].x; b.y = e.v[1].y; b.u = e.v[1].u; b.v = e.v[1].v;
        if (std::memcmp(&a, &e.v[0], sizeof(GSVertex)) != 0 || std::memcmp(&b, &e.v[1], sizeof(GSVertex)) != 0)
            ++g_g787C.vBad;
    }
    // The z / fog the descriptor predicts.
    if (e.v[0].z != static_cast<float>((h.g.xyzW2 >> 4) & 0xFFFFFFu) || e.v[0].fog != ((h.g.xyzW3a >> 4) & 0xFFu) ||
        e.v[1].fog != ((h.g.xyzW3b >> 4) & 0xFFu) || e.v[2].q != 0.0f)
        ++g_g787C.vBad;
    ++h.matched;
    h.cursor = at + 1u;
    if (h.cursor >= h.sprites)
        g787VerifyFinish();
}

// Expand a macro entry: fn(sprite entry, continuation) once per covered sprite, in guest order.
template <typename Fn>
static void g787ExpandEntry(const G144Entry &m, Fn &&fn)
{
    uint32_t first = 0u;
    const G787Geom g = g787MacroGeom(m, first);
    G144Entry t = m;
    t.v[2] = GSVertex{};
    bool cont = false;
    uint64_t n = 0;
    g787ForEachSprite(g, first, [&](uint32_t, const int32_t *o) {
        g787SetXyuv(t.v[0], o[2], o[3], o[0], o[1]);
        g787SetXyuv(t.v[1], o[6], o[7], o[4], o[5]);
        g787SpriteRows(t);
        fn(static_cast<const G144Entry &>(t), cont);
        cont = true;
        ++n;
        return true;
    });
    g787Add(g_g787C.xMacros, 1u);
    g787Add(g_g787C.xSprites, n);
}

// CPU replay (a refused GPU flush): the list goes back to legacy sprite entries.
static void g787ExpandListForCpu()
{
    bool any = false;
    for (const G144Entry &e : g_g144List)
        if (g787IsMacro(e)) { any = true; break; }
    if (!any)
        return;
    std::vector<G144Entry> out;
    out.reserve(g_g144List.size() + 64u);
    for (G144Entry &e : g_g144List)
    {
        if (!g787IsMacro(e)) { out.push_back(std::move(e)); continue; }
        g787ExpandEntry(e, [&](const G144Entry &t, bool) { out.push_back(t); });
    }
    g_g144List.swap(out);
    g787Add(g_g787C.xCpuExpand, 1u);
}

// ---- G787 draw collapse (plans/phase-G787-ledger.md §9) ------------------------------------------
// Every regenerated sprite of a macro used to become its own G178Draw: the G684 merge predicate
// compares the G406 accurate-UV record (origin, flags, period), and `origin` is the sprite's own
// x0/y0. The shader reads origin/period ONLY inside `g406RoundAxis`'s `exact` term, and that term
// is consulted only when the axis carries G406_ROUND_DOWN (`down = (f & 2) && !exact`,
// `up = (f & 1) || ((f & 2) && exact)`). An axis without ROUND_DOWN therefore has a DEAD origin and
// a DEAD period: zeroing both cannot change a single sampled coordinate. (The G560 per-vertex state
// program that would carry them is compiled out of release builds, PS2X_G684_LEGACY_G560_RUNS=OFF.)
//   DC2_G787_COLLAPSE=1  EXACT (DEFAULT since `_e`): canonicalise the dead G406 fields of every macro
//                        sprite, so the sprites of one composite call share one draw record wherever
//                        their live G406 state agrees. Rollback DC2_G787_NO_COLLAPSE=1 / COLLAPSE=0.
//   DC2_G787_COLLAPSE=2  ⛔ CEILING (corrupting): a continuation sprite ignores the G406 record
//                        entirely and always joins its predecessor's draw (one draw per macro).
//   DC2_G787_DCENSUS=1   diagnostic: per macro, sprites / draws started / draws under the raw and
//                        canonical keys / G406 flag classes.
static int g787CollapseMode()
{
    // G787 sec.10: the EXACT canonicalisation is DEFAULT ON (dragon -0.45..-0.9 ms/f, oracle 0 bad over
    // 18.7 M records). Rollback DC2_G787_NO_COLLAPSE=1 (or DC2_G787_COLLAPSE=0).
    static const int v = [] {
        const char *n = std::getenv("DC2_G787_NO_COLLAPSE");
        if (n != nullptr && *n != '\0' && std::strcmp(n, "0") != 0)
            return 0;
        const char *e = std::getenv("DC2_G787_COLLAPSE");
        return e ? std::atoi(e) : 1;
    }();
    return v;
}
static bool g787DrawCensusOn()
{
    static const bool v = [] {
        const char *e = std::getenv("DC2_G787_DCENSUS");
        return e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0;
    }();
    return v;
}
static inline void g787CanonG406(G406UvRoundInfo &r)
{
    if ((r.flagsU & G406_ROUND_DOWN) == 0u) { r.originX = 0; r.periodU = 0u; }
    if ((r.flagsV & G406_ROUND_DOWN) == 0u) { r.originY = 0; r.periodV = 0u; }
}
static inline uint64_t g787G406Key(const G406UvRoundInfo &r)
{
    return static_cast<uint64_t>(r.originX & 0xFFF) | (static_cast<uint64_t>(r.originY & 0xFFF) << 12u) |
           (static_cast<uint64_t>(r.flagsU & 15u) << 24u) | (static_cast<uint64_t>(r.flagsV & 15u) << 28u) |
           (static_cast<uint64_t>(r.periodU) << 32u) | (static_cast<uint64_t>(r.periodV) << 48u);
}
//   DC2_G787_CVERIFY=1   diagnostic ORACLE (with COLLAPSE=1): every canonicalised record is checked
//                        against an independent CPU port of the GLSL `g406RoundAxis` — flags equal,
//                        a ROUND_DOWN axis keeps origin + period, and the rounded coordinate is
//                        bit-identical under the raw and the canonical record over every pixel of a
//                        36-px window around the raw origin x a boundary-straddling value grid x
//                        both filter modes → `[G787:cv]`.
static bool g787CVerifyOn()
{
    static const bool v = [] {
        const char *e = std::getenv("DC2_G787_CVERIFY");
        return e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0;
    }();
    return v;
}
// Split reasons, first failing conjunct of the G684 merge predicate, for a draw STARTED inside a macro.
enum G787Split : int
{
    kG787SplitEmpty = 0,  // no previous draw in this build range (or merging disabled)
    kG787SplitBound,      // G786 versioned-override boundary (head only)
    kG787SplitState,      // G178 entry state differs: blend / material / scissor / depth / colour (head only)
    kG787SplitSrc,        // G261 producer surface differs (head only)
    kG787SplitTex,        // texture key differs
    kG787SplitFlags,      // G406 rounding flags differ
    kG787SplitOrigin,     // G406 origin differs (after canonicalisation: a LIVE ROUND_DOWN origin)
    kG787SplitPeriod,     // G406 period differs
    kG787SplitScreen,     // G675 screen-sprite class differs
    kG787SplitAdj,        // vertex range not adjacent
    kG787SplitN
};
// Per-build-range scratch (one per g684BuildVertexRange invocation; ranges may run on pool threads).
struct G787MacroScratch
{
    bool in = false;
    uint32_t sprites = 0, rawRuns = 0, canonRuns = 0, live = 0;
    uint64_t rawKey = ~0ull, canonKey = ~0ull;
    uint32_t cls[16]{};
    uint32_t split[2][kG787SplitN]{}; // [0] the macro's head sprite, [1] a continuation sprite
    uint32_t cvSprites = 0, cvChanged = 0, cvEvals = 0, cvBad = 0;
};
struct G787DrawCensus
{
    uint64_t macros = 0, sprites = 0, draws = 0, rawRuns = 0, canonRuns = 0, live = 0;
    uint64_t cls[16]{};
    uint64_t split[2][kG787SplitN]{};
    uint64_t cvSprites = 0, cvChanged = 0, cvEvals = 0, cvBad = 0;
};
static G787DrawCensus g_g787Dc;
// Independent CPU reference of the GLSL `g406RoundAxis` (lle_gpu_raster_backend.inc), written from
// the shader text, not shared with any producer. `flagsIn` = flags | period << 8 exactly as uploaded.
static float g787RefRoundAxis(float value, int pixel, int origin, int flagsIn, bool lin)
{
    const int flags = flagsIn & 15;
    const int period = (flagsIn >> 8) & 4095;
    const bool exact = pixel == origin || (period != 0 && ((pixel - origin) % period) == 0);
    const bool legacy = (flags & 4) != 0;
    const float boundary = (legacy || lin) ? std::floor(value * 2.0f + 0.5f) * 0.5f : std::floor(value + 0.5f);
    const float threshold = legacy ? 0.25f : (1.0f / 64.0f);
    if (std::fabs(value - boundary) > threshold)
        return value;
    const bool down = (flags & 2) != 0 && !exact;
    const bool up = (flags & 1) != 0 || ((flags & 2) != 0 && exact);
    return down ? boundary - (legacy ? 0.25f : (1.0f / 16.0f)) : (up ? boundary + (legacy ? 0.25f : 0.0f) : value);
}
// One axis of the oracle. Returns the number of disagreeing evaluations (0 = canonicalisation dead).
static uint32_t g787VerifyAxis(int rawOrigin, unsigned rawFlags, unsigned rawPeriod, int canOrigin,
                               unsigned canFlags, unsigned canPeriod, uint32_t &evals)
{
    uint32_t bad = 0;
    if (rawFlags != canFlags)
        ++bad;
    if ((rawFlags & G406_ROUND_DOWN) != 0u && (rawOrigin != canOrigin || rawPeriod != canPeriod))
        ++bad;
    // As the shader sees them: the origin goes through G178Draw's uint16 field, period through 12 bits.
    const int ro = static_cast<int>(static_cast<uint16_t>(rawOrigin));
    const int co = static_cast<int>(static_cast<uint16_t>(canOrigin));
    const int rf = static_cast<int>(rawFlags) | (static_cast<int>(rawPeriod & 4095u) << 8);
    const int cf = static_cast<int>(canFlags) | (static_cast<int>(canPeriod & 4095u) << 8);
    static const float kOff[] = {0.0f, 1.0f / 128.0f, -1.0f / 128.0f, 1.0f / 64.0f, -1.0f / 64.0f,
                                 1.0f / 32.0f, 0.25f, -0.25f, 0.5f};
    for (int p = ro - 2; p < ro + 34; ++p)
        for (float off : kOff)
            for (int lin = 0; lin < 2; ++lin)
            {
                const float v = 7.0f + off;
                const float a = g787RefRoundAxis(v, p, ro, rf, lin != 0);
                const float b = g787RefRoundAxis(v, p, co, cf, lin != 0);
                bad += std::memcmp(&a, &b, sizeof a) != 0 ? 1u : 0u;
                ++evals;
            }
    return bad;
}
static void g787VerifyCanon(G787MacroScratch &s, const G406UvRoundInfo &raw, const G406UvRoundInfo &can)
{
    ++s.cvSprites;
    s.cvChanged += (raw.originX != can.originX || raw.originY != can.originY || raw.periodU != can.periodU ||
                    raw.periodV != can.periodV) ? 1u : 0u;
    uint32_t ev = 0;
    s.cvBad += g787VerifyAxis(raw.originX, raw.flagsU, raw.periodU, can.originX, can.flagsU, can.periodU, ev);
    s.cvBad += g787VerifyAxis(raw.originY, raw.flagsV, raw.periodV, can.originY, can.flagsV, can.periodV, ev);
    s.cvEvals += ev;
}
static void g787CensusSprite(G787MacroScratch &s, const G406UvRoundInfo &raw)
{
    G406UvRoundInfo c = raw;
    g787CanonG406(c);
    const uint64_t rk = g787G406Key(raw), ck = g787G406Key(c);
    s.rawRuns += rk != s.rawKey ? 1u : 0u;
    s.canonRuns += ck != s.canonKey ? 1u : 0u;
    s.rawKey = rk;
    s.canonKey = ck;
    s.live += ((raw.flagsU | raw.flagsV) & G406_ROUND_DOWN) != 0u ? 1u : 0u;
    ++s.cls[(raw.flagsU & 3u) | ((raw.flagsV & 3u) << 2u)];
    ++s.sprites;
}
static void g787CensusMacro(const G787MacroScratch &s, size_t drawsStarted)
{
    G787DrawCensus &k = g_g787Dc;
    const uint64_t m = static_cast<uint64_t>(
        _InterlockedExchangeAdd64(reinterpret_cast<volatile long long *>(&k.macros), 1)) + 1u;
    g787Add(k.sprites, s.sprites);
    g787Add(k.draws, drawsStarted);
    g787Add(k.rawRuns, s.rawRuns);
    g787Add(k.canonRuns, s.canonRuns);
    g787Add(k.live, s.live);
    for (int c = 0; c < 16; ++c)
        if (s.cls[c] != 0u)
            g787Add(k.cls[c], s.cls[c]);
    for (int h = 0; h < 2; ++h)
        for (int r = 0; r < kG787SplitN; ++r)
            if (s.split[h][r] != 0u)
                g787Add(k.split[h][r], s.split[h][r]);
    if (s.cvSprites != 0u)
    {
        g787Add(k.cvSprites, s.cvSprites);
        g787Add(k.cvChanged, s.cvChanged);
        g787Add(k.cvEvals, s.cvEvals);
        g787Add(k.cvBad, s.cvBad);
    }
    if ((m & 8191u) == 0u)
    {
        std::fprintf(stderr,
                     "[G787:dcr] macros=%llu head(empty,bound,state,src,tex,flags,origin,period,screen,adj)="
                     "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu cont=%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                     "%llu,%llu,%llu\n",
                     (unsigned long long)m, (unsigned long long)k.split[0][0], (unsigned long long)k.split[0][1],
                     (unsigned long long)k.split[0][2], (unsigned long long)k.split[0][3],
                     (unsigned long long)k.split[0][4], (unsigned long long)k.split[0][5],
                     (unsigned long long)k.split[0][6], (unsigned long long)k.split[0][7],
                     (unsigned long long)k.split[0][8], (unsigned long long)k.split[0][9],
                     (unsigned long long)k.split[1][0], (unsigned long long)k.split[1][1],
                     (unsigned long long)k.split[1][2], (unsigned long long)k.split[1][3],
                     (unsigned long long)k.split[1][4], (unsigned long long)k.split[1][5],
                     (unsigned long long)k.split[1][6], (unsigned long long)k.split[1][7],
                     (unsigned long long)k.split[1][8], (unsigned long long)k.split[1][9]);
        if (g787CVerifyOn())
            std::fprintf(stderr, "[G787:cv] macros=%llu sprites=%llu changed=%llu evals=%llu bad=%llu\n",
                         (unsigned long long)m, (unsigned long long)k.cvSprites,
                         (unsigned long long)k.cvChanged, (unsigned long long)k.cvEvals,
                         (unsigned long long)k.cvBad);
    }
    if ((m & 8191u) == 0u)
        std::fprintf(stderr,
                     "[G787:dc] mode=%d macros=%llu sprites=%llu drawsStarted=%llu rawRuns=%llu canonRuns=%llu "
                     "liveDownSprites=%llu cls(fu|fv<<2)=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                     "%llu,%llu,%llu,%llu\n",
                     g787CollapseMode(), (unsigned long long)m, (unsigned long long)k.sprites,
                     (unsigned long long)k.draws, (unsigned long long)k.rawRuns, (unsigned long long)k.canonRuns,
                     (unsigned long long)k.live, (unsigned long long)k.cls[0], (unsigned long long)k.cls[1],
                     (unsigned long long)k.cls[2], (unsigned long long)k.cls[3], (unsigned long long)k.cls[4],
                     (unsigned long long)k.cls[5], (unsigned long long)k.cls[6], (unsigned long long)k.cls[7],
                     (unsigned long long)k.cls[8], (unsigned long long)k.cls[9], (unsigned long long)k.cls[10],
                     (unsigned long long)k.cls[11], (unsigned long long)k.cls[12], (unsigned long long)k.cls[13],
                     (unsigned long long)k.cls[14], (unsigned long long)k.cls[15]);
}
