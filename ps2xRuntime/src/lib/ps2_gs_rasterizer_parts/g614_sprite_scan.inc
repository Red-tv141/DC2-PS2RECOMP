// ===== G614: the replayed SPRITE PIXEL, decomposed by DELETION =====
// revision: 1
//
// WHERE THIS PHASE STARTS.  G613 registered `dungeon1` (`map/d/d02/f01`) and found the most
// GS-poled route ever measured here: **0 of 108 windows VU1-poled, headroom +18.07 ms/f** (8.8x
// `ridepod`'s), GS own reaching 72.10 ms/f against a VU1 busy that never exceeds 22.5.  Its CPU
// band replay is **99.8% SPRITES** — every kernel from G580 on (G605 span, G608 `fst=1` leaf,
// G609 tight scanline) is inert, because all three require a textured TRIANGLE — and **six
// `[G605:shape]` rows of ONE `discov sprite` family are 67.04% of all replay cycles**: PSMT8
// (`tpsm=13`), `lin=1`, `fst=1`, `tfx=0`, `tcc=1`, `prz=0`, `ztst=1`, at 424.0 cyc/inside with
// `pro% = 1.2`.  A pure pixel loop, on a route with the budget to convert it.
//
// ⭐⭐⭐ G609'S LAW APPLIES AND ITS TWIN DOES NOT.  `cyc/inside` prices the WHOLE admitted pixel and
// nothing inside it, so the next lever cannot be ranked by reading the code — G609 proved that
// twice over on the triangle pixel, where a code-read ranking said "the VRAM taps" and the
// deletion probe said the four `lerpChannel` calls were **50.1%** of the pixel against **8.1%**
// for the taps.  ⛔ The counter-law (`g610_no_deletion_ranking_for_an_interpreter`) does NOT bind
// here: a deleted stage of a RASTERISER pixel changes only pixels, never the loop's own trip
// count.  `[G605:leaf] inside` is set from the draw's bbox BEFORE the loop runs
// (`rasterizer_draw_sprite.inc`, `g605cyc.setInside`), so every arm below carries the IDENTICAL
// denominator by construction and a stage's price is a straight `cyc/inside` difference.
//
// ⛔⛔ CEILING PROBE ONLY.  Every level renders KNOWINGLY WRONG PIXELS and none may ever be
// promoted or used as a correctness switch.  The baseline arm is THE SAME BINARY with
// `DC2_G614_CEIL` unset, so both arms carry these branches and the delta is the stage's own price
// (`17a` derivative/ceiling-probe contract).  The level is read ONCE per drawSprite into plain
// `const bool`s hoisted above the loop nest, so the default binary's inner loop gains only
// loop-invariant branches.
//
//   DC2_G614_CEIL=1  delete the whole SAMPLER      (4 taps + 4 lerps; texel := 0xFFFFFFFF)
//   DC2_G614_CEIL=2  = 1 + delete combineTexture   (isolates the per-pixel combine call)
//   DC2_G614_CEIL=3  delete combine + gate + COMMIT (keeps only the sampler and the Z test)
//   DC2_G614_CEIL=4  delete the four lerpChannel calls only        (texel := q00)
//   DC2_G614_CEIL=5  delete the four g530Tap calls only            (quad := constant, lerp runs)
//   DC2_G614_CEIL=6  delete the COMMIT only        (blend + mask + store)
//   DC2_G614_CEIL=7  delete the WHOLE per-pixel body — bounds everything above
//                    (`g511_probe_bounds_the_gate`: if this reads ~0 the loop is not the subject)

namespace
{

inline int g614CeilProbe()
{
    static const int level = []() -> int {
        const char *e = std::getenv("DC2_G614_CEIL");
        const int n = e ? std::atoi(e) : 0;
        return (n < 0 || n > 7) ? 0 : n;
    }();
    return level;
}

// ⛔ An observable sink for the levels that delete a COMMIT. Without it MSVC is free to delete the
// arithmetic that feeds the store as well, and the probe would price more than the stage it names
// (`17e`: a deletion probe must delete exactly one class of work).
std::atomic<uint32_t> g_g614CeilSink{0};

// The five per-draw booleans the inner loop actually branches on, derived once from the level.
struct G614Ceil
{
    bool any = false;
    bool noTap = false;    // levels 1, 2, 5
    bool noLerp = false;   // levels 1, 2, 4
    bool noComb = false;   // levels 2, 3
    bool noCommit = false; // levels 3, 6
    bool noPixel = false;  // level 7
};

inline G614Ceil g614CeilFlags()
{
    G614Ceil c;
    const int n = g614CeilProbe();
    c.any = (n != 0);
    c.noTap = (n == 1 || n == 2 || n == 5);
    c.noLerp = (n == 1 || n == 2 || n == 4);
    c.noComb = (n == 2 || n == 3);
    c.noCommit = (n == 3 || n == 6);
    c.noPixel = (n == 7);
    return c;
}

// ============================= WHAT THE PROBE SAID, AND THE LEVER =============================
//
// ⭐⭐⭐ THE MEASURED DECOMPOSITION of the `dungeon1` `discov sprite` pixel, all seven arms on one
// binary with denominators identical to 0.0005% (256,286,110 vs 256,286,958 inside px):
//
//   deleted                            cyc/inside    stage      % of pixel
//   -- (baseline)                          436.7        --          100.0
//   whole per-pixel body       (CEIL=7)      17.5     419.2           96.0
//   the whole SAMPLER          (CEIL=1)     118.5     318.2         ⭐ 72.9
//     the four lerpChannel     (CEIL=4)     261.4     175.3         ⭐ 40.1
//     the four g530Tap         (CEIL=5)     280.6     156.1         ⭐ 35.7
//   the COMMIT                 (CEIL=6)     370.6      66.1           15.1
//   combineTexture     (CEIL=2 minus 1)      94.2      24.3            5.6
//   residue (gate, Z store, addressing, memo compares)               ≈ 2.4
//
// ⛔⛔ THE SPRITE PIXEL IS NOT THE TRIANGLE PIXEL, AND ASSUMING IT WAS WOULD HAVE COST HALF THE
// PRIZE.  G609 decomposed the admitted TRIANGLE pixel and found the four bilinear lerps at 50.1%
// against only **8.1%** for the four VRAM taps — so it inlined the filter and left the taps alone.
// Here the same two stages read **40.1% and 35.7%**: the taps are **4.4x** the share they have on
// the triangle.  The reason is structural, not statistical — the triangle's admitted sampler
// (`g580SampleSharedT8` / `g608SampleSharedT8Uv`) lives in THIS translation unit, while the
// sprite's PSMT8 tap leaves it, through `GSMem::ReadP8` into `ps2_gs_memory.cpp`, four times per
// memo miss, each call re-deriving the page/block/column address with divisions and modulos from
// scratch.  ⭐ A STAGE'S SHARE IS A PROPERTY OF THE LEAF THAT IMPLEMENTS IT, NOT OF THE STAGE.
//
// So this lever inlines all THREE non-inlined leaves the probe named — 81.4% of the pixel between
// them — and changes NO arithmetic anywhere:
//
//   (a) the four `lerpChannel` CALLS -> `g609Lerp8`, G609's already-shipped, already-proven-exact
//       inline copy of the SAME statements.  Besides the call itself this deletes THREE
//       function-local `static const bool` reads per call (`g529NoLerpOn`, `g529FastLerpOn`,
//       `g529LerpVfyOn`) — each one a per-ACCESS thread-safe-init guard, so **twelve guard checks
//       per texel** in the hottest leaf on the route.  Admitted only when all three read their
//       shipped default, so the substitution is the identity.
//   (b) the four `GSMem::ReadP8` CALLS -> the row-decomposed `GSPSMT8::addrPSMT8` form.  The
//       source row (v0/v1) is fixed for a whole scanline, so the page row, the block row and the
//       column row are per-ROW constants and the per-tap cost is two table loads and six ALU ops.
//       ⭐ ALGEBRAICALLY IDENTICAL, and the identity is not merely cited: `PixelStorageTraits<P8>`
//       builds `table[b][y][x] = (b + blockTable8[(y/16)%4][(x/16)%8]) * 256 +
//       columnTable8[y%16][x%16]`, and `(id>>5)<<13 + (id&31)*256 == id*256` for every id because
//       32*256 == 1<<13.  `DC2_G614_VERIFY=1` re-reads every tap through `GSMem::ReadP8` and
//       compares, so the claim is CHECKED on the live corpus (`g530`'s own rule for the CT32 tap).
//   (c) `combineTexture` -> an inline copy, statement for statement, switch included.
//
// ⛔ The COMMIT (15.1%) is deliberately NOT touched: its cost is a real destination load plus the
// blend, which is work, not envelope.  A lever there would have to change what is computed.
//
//   DC2_G614_NO_SPRITE_FAST=1   rollback
//   DC2_G419_AB=sprfast         the paired gate
//   DC2_G614_VERIFY=1           ORACLE — the shipped leaf is authoritative for every stage; `bad`
//                               must be 0.  ⛔ NEVER in a timing run (every stage runs twice).
//   DC2_G614_STAT=1             `[G614:spr]` coverage census

// ⚠️ `g614SprFastArm()` is DEFINED in g419_ab_instrument.inc, which this TU includes at line 401 —
// far ahead of this file, so it is already in scope. ⛔ Do NOT forward-declare it here: most of
// this file is inside an anonymous namespace, and a declaration there binds to a DIFFERENT,
// internally-linked entity that never links (the trap `appendix-dc2-project.md` §3 records).
// g530_sprite_span.inc calls g588SolidSpriteArm() the same way, for the same reason.

// ⚠️ READ ONCE PER DRAW. Each of these is a function-local `static const bool`, i.e. a per-ACCESS
// TLS init guard — the exact defect this lever exists to delete (`g609_function_local_static_is_a_
// per_access_tls_guard`). Never call one from inside the pixel loop.
inline bool g614SprFastOn()
{
    const int arm = g614SprFastArm();
    if (arm >= 0)
        return arm != 0;
    static const bool on = !envFlagEnabled("DC2_G614_NO_SPRITE_FAST");
    return on;
}
inline bool g614VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G614_VERIFY");
    return on;
}
inline bool g614StatOn()
{
    static const bool on = envFlagEnabled("DC2_G614_STAT") || g614VerifyOn();
    return on;
}

std::atomic<uint64_t> g_g614Draws{0}, g_g614Px{0}, g_g614Taps{0}, g_g614Lerps{0};
std::atomic<uint64_t> g_g614Slides{0};
std::atomic<uint64_t> g_g614BadTap{0}, g_g614BadLerp{0}, g_g614BadComb{0}, g_g614Reject{0};
std::atomic<uint64_t> g_g614BadSlide{0};

inline void g614Report()
{
    if (!g614StatOn())
        return;
    static std::atomic<uint64_t> s_next{1};
    const uint64_t d = g_g614Draws.load(std::memory_order_relaxed) +
                       g_g614Reject.load(std::memory_order_relaxed);
    uint64_t want = s_next.load(std::memory_order_relaxed);
    if (d < want || !s_next.compare_exchange_strong(want, (want < 20000u) ? 20000u : want + 20000u))
        return;
    std::fprintf(stderr,
                 "[G614:spr] draws=%llu reject=%llu px=%llu taps=%llu lerps=%llu slides=%llu | "
                 "bad(tap=%llu lerp=%llu comb=%llu slide=%llu) verify=%u\n",
                 static_cast<unsigned long long>(g_g614Draws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614Reject.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614Px.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614Taps.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614Lerps.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614Slides.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614BadTap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614BadLerp.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614BadComb.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g614BadSlide.load(std::memory_order_relaxed)),
                 g614VerifyOn() ? 1u : 0u);
    std::fflush(stderr);
}

// ---- (b) the PSMT8 source address, split into the part that is constant for a scanline ----
// `GSPSMT8::addrPSMT8(block, tbw, x, y)`, with every term that depends only on `y` hoisted. The
// remaining per-tap expression is byte-for-byte the same arithmetic on the same tables.
struct G614T8Row
{
    uint32_t pageRow = 0;         // (block >> 5) + (y >> 6) * pagesPerRow
    uint32_t blockLo = 0;         // block & 31
    const uint8_t *bt = nullptr;  // GSPSMT8::blockTable8[(y >> 4) & 3]
    const uint8_t *ct = nullptr;  // GSPSMT8::columnTable8[y & 15]
};

inline G614T8Row g614T8Row(uint32_t block, uint32_t tbw, uint32_t y)
{
    G614T8Row r;
    const uint32_t pagesPerRow = ((tbw >> 1u) != 0u) ? (tbw >> 1u) : 1u;
    r.pageRow = (block >> 5u) + (y >> 6u) * pagesPerRow;
    r.blockLo = block & 0x1Fu;
    r.bt = GSPSMT8::blockTable8[(y >> 4) & 3u];
    r.ct = GSPSMT8::columnTable8[y & 0x0Fu];
    return r;
}

inline uint32_t g614T8Off(const G614T8Row &r, uint32_t x)
{
    const uint32_t page = r.pageRow + (x >> 7u);
    const uint32_t blockId = r.blockLo + r.bt[(x >> 4) & 7u];
    return (page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 0x1Fu) * 256u +
           r.ct[x & 0x0Fu];
}

// ---- (c) combineTexture, inlined ----
// `rasterizer_row_pool.inc`'s combineTexture, statement for statement. `tfx`/`tcc` are handed in as
// plain locals instead of read through a `const GSTex0Reg &`, so MSVC can hoist the switch out of
// the pixel loop; nothing else differs.
inline TextureCombineResult g614Combine(uint8_t tfx, bool textureHasAlpha,
                                        uint8_t vr, uint8_t vg, uint8_t vb, uint8_t va,
                                        uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};
    switch (tfx)
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

} // namespace
