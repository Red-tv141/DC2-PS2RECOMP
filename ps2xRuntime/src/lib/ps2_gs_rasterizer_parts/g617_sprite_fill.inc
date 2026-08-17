// ===== G617: THE TWO REMAINING REPLAYED-SPRITE POPULATIONS ON `dungeon1` =====
// revision: 1
//
// ⭐⭐⭐ THE PHASE STARTED BY REPAIRING THE INSTRUMENT, AND THE REPAIR CHANGED THE RANKING.
//
// G615 left two named targets and a size for each, both read off `[G605:leaf] disp sprite` and
// `[G605:shape]`. Both sizes were wrong, for the same reason G615's own "Target K" was wrong: a
// census bucket held TWO POPULATIONS.
//
//   * `[G605:leaf] disp sprite` averaged the draws the G530/G614 span kernel ADMITTED with the ones
//     it REFUSED. On `dungeon1` that row was 94% refused BY DRAW and 17% refused BY PIXEL, so
//     neither its `cyc/inside` nor its share named anything at all.
//   * `[G605:shape]`'s second column was `kPrimName[gs->m_prim.type]`, i.e. the GS PRIMITIVE, and
//     `tme` was in neither the row key nor the print — so an UNTEXTURED fill hashed into the same
//     row as a TEXTURED sprite whenever the stale TEX0 state matched. The ten printed `disp sprite`
//     rows summed to 85.5 M px against a leaf total of 45.2 M.
//
// Split by admission (`kG605LeafSpriteSlow`) and keyed on the resolved leaf, the same route reads:
//
//   leaf                  n        inside px    cyc/inside   LOOP ms   share%
//   disp   spr-slow        59,203    5,745,312     ⭐ 819.0    11,840.6   ⭐ 37.4
//   discov sprite          77,112   85,292,880        294.5     8,735.2      27.9
//   disp   spr-untex       19,352   59,533,088        323.8     7,995.6   ⭐ 25.2
//   disp   sprite           7,170   37,547,332     ⭐ 155.3     2,921.4       9.2
//
// ⭐⭐⭐ `disp spr-slow` — the REFUSED textured display sprite — is the LARGEST single item in the
// route's replay, and it costs **5.3x** the admitted leaf per pixel (819.0 against 155.3). ROADMAP
// carried it as "Target M … only 6.5% of the admitted pixel AREA", which is true and was read as
// "small". Area is not cost when the two populations run different loops.
//
// ⭐⭐⭐ AND THE SHAPE TABLE, ONCE IT COULD SEE `tme`, NAMED THE OTHER TARGET'S BLEND:
//
//   #01/#02 disp spr-untex n=8960 px=14,909,440 cyc/inside=391.9 tme=0 alpha=2a abe=1 fbmsk=0 …
//
// `ALPHA = 0x2a` is `A=2 B=2 C=2 D=0`, i.e. `(0 - 0) * FIX >> 7 + Cs` — **algebraically the
// identity**. Half of Target N's pixels are alpha-blended draws whose blend cannot read, and cannot
// change, the destination. G588's promoted exact solid fill refuses them on `!abe` alone.
//
// =============================================================================================
// LEVER (a) — "Target N": THE UNTEXTURED **BLENDED** DISPLAY FILL      `DC2_G617_NO_BLEND_FILL=1`
// =============================================================================================
//
// 19,352 draws / 59.5 M px / 323.8 cyc/inside for a loop that writes ONE COLOUR. Everything the
// generic nest re-derives per pixel is a draw constant: the alpha-test classification (the source
// alpha never varies), the scissor test (drawX0..drawX1 is ALREADY the scissor intersection), the
// destination page/block address, the blend selectors, the FBA decision and the fbmsk. What is NOT
// constant is the destination word — but only when the blend actually reads it.
//
// So the kernel classifies the blend ALGEBRAICALLY, once per draw:
//
//   dest-independent  <=>  asel != 1 && bsel != 1 && csel != 1 && dsel != 1
//
// (`pick(sel, Cs, Cd)` returns `Cd` only for `sel == 1`, and `cAlpha` reads `Ad` only for
// `csel == 1`.) When that holds the committed word is a DRAW CONSTANT and the commit is G588's
// proven aligned pair store with no load at all. When it does not, the kernel loads the destination
// once per pixel and runs `writePixel`'s blend statements verbatim on hoisted selectors.
//
// ⭐ EXACT BY CONSTRUCTION, STATEMENT FOR STATEMENT. The per-pixel body below is `writePixel`'s
// 32-bit tail with the branches whose outcome is a draw constant resolved:
//   - the scissor test is dropped because the caller already clipped the rect to the scissor;
//   - `classifyAlphaTest(ctx.test, a)` is evaluated ONCE (its only pixel-varying input is `a`,
//     which is flat for an untextured sprite);
//   - PABE's `(a & 0x80)` test is likewise a draw constant;
//   - the address is `GSPSMCT32::addrPSMCT32` factored into a per-ROW base, a per-8-PIXEL-GROUP
//     page/block term and a column-table load — G588's already-promoted, already-oracle-proven
//     decomposition, reused unchanged;
//   - `writePixel` re-loads the destination separately for the blend, for the fbmsk merge and for
//     the AFAIL alpha preserve. No store happens between those loads, so ONE load is the same
//     value three times.
// ⛔ Nothing else is dropped: the fbmsk merge, the AFAIL RGB_ONLY alpha preserve, the FBA OR, the
// PABE bypass, the `off + 4 <= vramSize` bound and the CT24 exclusion from FBA are all reproduced.
//
// =============================================================================================
// LEVER (b) — "Target M": THE **PSMT4HH** DISPLAY SPRITE TAP              `DC2_G617_NO_T4HH=1`
// =============================================================================================
//
// `[G615:privz] spanRej(samp=102994)` and `[G529:sprwhy] psm=102873 psm(c:102873)`: every refused
// display sprite is refused by ONE conjunct of the G248 fast-sampler predicate — `g248FastPsm`,
// which admits CT32 / CT24 / T8 and nothing else. The population is PSMT4HH (`tpsm=0x2c`), 4-bit
// indices packed into the HIGH NIBBLE of the HIGH BYTE of a CT32 word.
//
// ⭐⭐⭐ AND THAT FORMAT NEEDS NO NEW ADDRESS MACHINERY AT ALL. `PixelStorageTraits<P4HH>` has the
// SAME `PageExtent {64,32}`, `BlockExtent {8,4}` and `ColumnExtent {8,8}` as `C32`, and reads
// through the SAME `PageTableC32`; the ONLY difference is `BitOffset() == 28`, so
//
//     Read(P4HH) at (x,y)  ==  ( vram[ addrPSMCT32(tbp0, tbw, x, y) + 3 ] >> 4 ) & 0x0F
//
// with the byte address masked exactly as the traits mask it (`& (MEMORY_SIZE - 1)` for a `u8`
// PackedT). That is the same class of identity G614(b) proved for PSMT8, one format over, and it
// reuses `G530RowAddr` / `g530GroupBase` verbatim rather than adding a table.
// `DC2_G617_VERIFY=1` re-reads every tap through the shipped `GSMem::ReadP4HH` and compares, so the
// identity is CHECKED on the live corpus and not merely cited (G530's own rule for the CT32 tap).
//
// ⭐ The population is POINT-sampled (`lin=0`, `tex1=0x1` / `0x201` — `tex1UsesLinearFilter` is
// false for both), so there is no bilinear quad and no column slide to build: one tap per pixel.
//
// FLAGS (all registered in `tools\run_g477_perf.ps1`'s `$clear` in the same edit that created them):
//   DC2_G617_NO_BLEND_FILL=1   rollback for lever (a)
//   DC2_G617_NO_T4HH=1         rollback for lever (b)
//   DC2_G419_AB=blendfill      the paired gate for (a)
//   DC2_G419_AB=t4hh           the paired gate for (b)
//   DC2_G617_VERIFY=1          ORACLE. The SHIPPED path is authoritative for both levers:
//                              (a) `writePixel` performs the store and the kernel's word is
//                              compared against what it actually wrote; (b) every tap is re-read
//                              through `GSMem::ReadP4HH`. ⛔ NEVER in a timing run.
//   DC2_G617_STAT=1            `[G617:fill]` / `[G617:t4hh]` coverage + refusal-by-CONJUNCT census.
//                              ⭐ Read the admitted count beside its `rej(…)` denominator.
//
// ⚠️ `g617BlendFillArm()` / `g617T4hhArm()` are DEFINED at GLOBAL scope in g419_ab_instrument.inc,
// which this TU includes far ahead of this file. ⛔ Do NOT forward-declare them below: this file's
// body is inside an anonymous namespace and a declaration there would bind to a different,
// never-defined entity (the G568 linkage trap, recorded in `appendix-dc2-project.md` §3).

namespace
{

// ⚠️ READ ONCE PER DRAW. Each is a function-local `static const bool`, i.e. a per-ACCESS TLS init
// guard — exactly the class of cost these levers exist to delete. Never call one inside a pixel
// loop (`g609_function_local_static_is_a_per_access_tls_guard`).
//
// ⛔⛔ AND READ THE A/B ARM UNCONDITIONALLY, BEFORE ANY STATE CONJUNCT. G615 §6: written behind
// `&&`, the arm is sampled on the LEVER's schedule instead of the FRAME's and the estimator loses
// the intervals it most needs (`used(0=4175 …)` beside `blocked(w=4)`). Both call sites below read
// these first and AND the state terms onto the result.
inline bool g617BlendFillOn()
{
    const int arm = g617BlendFillArm();
    if (arm >= 0)
        return arm != 0;
    static const bool on = !envFlagEnabled("DC2_G617_NO_BLEND_FILL");
    return on;
}

inline bool g617T4hhOn()
{
    const int arm = g617T4hhArm();
    if (arm >= 0)
        return arm != 0;
    static const bool on = !envFlagEnabled("DC2_G617_NO_T4HH");
    return on;
}

inline bool g617VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G617_VERIFY");
    return on;
}

inline bool g617StatOn()
{
    static const bool on = envFlagEnabled("DC2_G617_STAT") || g617VerifyOn();
    return on;
}

// ---------------------------------------------------------------------------------------------
// (a) THE BLEND, CLASSIFIED ONCE PER DRAW
//
// `writePixel`'s blend is
//     C = clamp( (pick(A,Cs,Cd) - pick(B,Cs,Cd)) * cAlpha >> 7 + pick(D,Cs,Cd) )
//     cAlpha = (C==0) ? As : (C==1) ? Ad : FIX
// and `pick` returns the DESTINATION only for selector value 1. So the whole expression is
// independent of the destination word exactly when no selector is 1 — which is a property of the
// ALPHA register alone, i.e. a draw constant. ⭐ On `dungeon1` this is TRUE for 29.8 M of Target
// N's 59.5 M pixels (`alpha=0x2a`: A=B=C=2, D=0, so the result is literally `Cs`).
struct G617Blend
{
    uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0, fix = 0;
    bool active = false;      // ABE, and PABE does not bypass it for this draw's constant alpha
    bool destIndependent = true;
};

inline G617Blend g617ClassifyBlend(uint64_t alphaReg, bool abe, bool pabe, uint8_t srcA)
{
    G617Blend b;
    b.asel = static_cast<uint8_t>(alphaReg & 3u);
    b.bsel = static_cast<uint8_t>((alphaReg >> 2) & 3u);
    b.csel = static_cast<uint8_t>((alphaReg >> 4) & 3u);
    b.dsel = static_cast<uint8_t>((alphaReg >> 6) & 3u);
    b.fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    // PABE disables blending when the source alpha MSB is clear — and for an untextured sprite the
    // source alpha is flat, so this is a per-DRAW decision, exactly as writePixel evaluates it.
    b.active = abe && !(pabe && (srcA & 0x80u) == 0u);
    b.destIndependent =
        !b.active || (b.asel != 1u && b.bsel != 1u && b.csel != 1u && b.dsel != 1u);
    return b;
}

// `writePixel`'s three blend statements, verbatim, on hoisted selectors. `dr/dg/db/da` are the
// destination channels; for the dest-independent case the caller passes zeroes and the result is
// the draw's constant colour.
inline void g617BlendRgb(const G617Blend &bl, uint8_t srcA,
                         uint8_t dr, uint8_t dg, uint8_t db, uint8_t da,
                         uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (!bl.active)
        return;
    auto pick = [](uint8_t sel, int cs, int cd) -> int {
        if (sel == 0) return cs;
        if (sel == 1) return cd;
        return 0;
    };
    const int cAlpha = (bl.csel == 0) ? static_cast<int>(srcA)
                     : (bl.csel == 1) ? static_cast<int>(da)
                                      : static_cast<int>(bl.fix);
    const uint8_t sr = r, sg = g, sb = b;
    r = clampU8(((pick(bl.asel, sr, dr) - pick(bl.bsel, sr, dr)) * cAlpha >> 7) +
                pick(bl.dsel, sr, dr));
    g = clampU8(((pick(bl.asel, sg, dg) - pick(bl.bsel, sg, dg)) * cAlpha >> 7) +
                pick(bl.dsel, sg, dg));
    b = clampU8(((pick(bl.asel, sb, db) - pick(bl.bsel, sb, db)) * cAlpha >> 7) +
                pick(bl.dsel, sb, db));
}

// ⭐⭐ THE 8-PIXEL GROUP PAIR STORE, AS A CHECKED IDENTITY RATHER THAN A CITED ONE.
// G588's promoted commit writes a full aligned group as four 8-byte stores at
// `base + columnTable32[y&7][p*2]*4`, which is byte-for-byte the per-pixel commit ONLY IF each row
// of the column table consists of four ADJACENT word pairs, i.e. `ct[2p] + 1 == ct[2p+1]` for every
// row and every p. G588 states that in a comment; nothing tests it. It is 32 compares over the
// whole table, so it costs one run of a `static const bool` to turn the claim into evidence —
// exactly what `g530AlphaGateAgrees` does for the alpha gate, and the same reason
// (`g536_invariant_untested_at_the_consuming_edge`: an invariant protects only the edges that test
// it). If it ever failed, the kernel simply does not take the group path.
inline bool g617ColumnPairsAdjacent()
{
    static const bool ok = []() -> bool {
        for (int y = 0; y < 8; ++y)
            for (int p = 0; p < 4; ++p)
                if (GSPSMCT32::columnTable32[y][p * 2] + 1 !=
                    GSPSMCT32::columnTable32[y][p * 2 + 1])
                    return false;
        return true;
    }();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// (b) THE PSMT4HH TAP, ROW-DECOMPOSED
//
// `G530RowAddr` already carries every y-dependent term of `addrPSMCT32` for a CT32 surface, and
// P4HH shares C32's page/block/column geometry exactly (see the header comment). So the source row
// is a `G530RowAddr` built with the TEXTURE's block and tbw, and the tap is the CT32 byte address
// plus 3, high nibble. ⚠️ `tbw` goes in UNHALVED — P4HH's page is 64 wide like C32's, unlike PSMT8
// (128) and PSMT4 (128), both of which need `tbw >> 1`.
inline uint32_t g617T4hhNibbleOff(const G530RowAddr &row, uint32_t x)
{
    return (g530GroupBase(row, x) + static_cast<uint32_t>(row.ct[x & 0x7u]) * 4u + 3u) &
           static_cast<uint32_t>(GSMem::MEMORY_SIZE - 1u);
}

// ---------------------------------------------------------------------------------------------
// THE CENSUS — per-DRAW granularity only (g526_draw_census.inc's rule), plus one pixel add per
// draw. It is a coverage report AND a debugger for the admission: count REFUSALS BY REASON
// (`g612_coverage_census_is_a_debugger`), and never read `bad` without the admitted count beside
// it (`g589_oracle_with_zero_checks`).
enum G617FillRej
{
    kG617FillRejNone = 0,
    kG617FillRejQuiet,   // a per-pixel diagnostic is armed: the generic nest must service it
    kG617FillRejLayout,  // destination is not CT32, or VRAM is absent
    kG617FillRejZ,       // the guest Z test/write is LIVE (this kernel has no depth arm)
    kG617FillRejAlpha,   // the alpha test does not write the framebuffer for this draw's alpha
    kG617FillRejN
};
static const char *const kG617FillRejName[kG617FillRejN] = {
    "none", "quiet", "layout", "z", "alpha"};

static std::atomic<uint64_t> g_g617FillDraws{0}, g_g617FillPx{0}, g_g617FillConstDraws{0},
                             g_g617FillConstPx{0}, g_g617FillBad{0}, g_g617FillZBad{0},
                             g_g617FillZDraws{0};
static std::atomic<uint64_t> g_g617FillRej[kG617FillRejN];
static std::atomic<uint64_t> g_g617T4hhDraws{0}, g_g617T4hhPx{0}, g_g617T4hhTaps{0},
                             g_g617T4hhBad{0};

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g617Report()
{
    // ⚠️ The cadence is set from THIS population's draw count, not copied from another census's:
    // Target N is ~19 k draws of ~3,000 px each over a whole route (G615's revision-2 lesson —
    // a 20,000-draw threshold printed exactly once, at draws=1).
    static std::atomic<uint64_t> s_ticks{0};
    if ((s_ticks.fetch_add(1u, std::memory_order_relaxed) % 500ull) != 0ull)
        return;
    std::fprintf(stderr,
                 "[G617:fill] draws=%llu px=%llu const(draws=%llu px=%llu) zwr(draws=%llu) "
                 "bad(px=%llu zset=%llu) rej(",
                 static_cast<unsigned long long>(g_g617FillDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617FillPx.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_g617FillConstDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_g617FillConstPx.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617FillZDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617FillBad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617FillZBad.load(std::memory_order_relaxed)));
    for (int i = 0; i < kG617FillRejN; ++i)
        std::fprintf(stderr, "%s=%llu ", kG617FillRejName[i],
                     static_cast<unsigned long long>(
                         g_g617FillRej[i].load(std::memory_order_relaxed)));
    std::fprintf(stderr, ")\n[G617:t4hh] draws=%llu px=%llu taps=%llu bad=%llu verify=%u\n",
                 static_cast<unsigned long long>(g_g617T4hhDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617T4hhPx.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617T4hhTaps.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g617T4hhBad.load(std::memory_order_relaxed)),
                 g617VerifyOn() ? 1u : 0u);
    std::fflush(stderr);
}

} // namespace
