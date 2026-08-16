// ===== G605: the exact accelerated leaf for the DOMINANT replayed triangle shape =====
// revision: 1
//
// WHAT THE MEASUREMENT SAID (this phase, `[G605:leaf]` + `[G605:shape]` on route `s03`).
// Ranked by lane CYCLES rather than by draws or entries (g603_rank_fallback_by_ms_not_entries):
//
//   class  leaf        n         px/call  cyc/px   cyc/inside  pro%   share of replay CPU
//   discov tri-fast    7.70 M      18.3   1509.0      860.6     9.6           42.8%
//   disp   tri-fast    166 k     1274.4    949.9      866.9     0.3           36.6%
//   disp   sprite      157 k      629.8    198.5      198.5    71.5           12.5%
//   discov sprite      150 k     4696.7     58.5       58.5     6.8            8.0%
//
// ⭐⭐⭐ TRIANGLES ARE 79.4% OF THE REPLAY'S CPU, AND THE COVERED TRIANGLE PIXEL COSTS ~860 CYCLES
// WHILE THE SPRITE PIXEL IN THE SAME BATCHES COSTS 58.5. The sprite number is low precisely because
// G530 already lowered it (1,310 -> 58.5). The triangle leaf never was.
//
// And `[G605:shape]` says the triangles are not a zoo. ONE state family carries 70.6% of ALL replay
// cycles, split across framebuffers only:
//
//   CT32 fbmsk=0 fba=0 pabe=0 | T8 tex, TFX=MODULATE, tcc=1, BILINEAR | ATE always-passing
//   | Z24 GEQUAL on the G403/G411 private mirror | iip=1 abe=1 | ALPHA in {0x42,0x44,0x48}
//
// The top nine rows of that table (56.5% of all replay cycles) additionally share `fst=0`.
//
// ⭐ WHY THIS IS NOT ANOTHER ADMISSION. G586/G587/G604 each tried to move one of these populations
// to the GPU and each failed exactness on coverage/depth/blend. This phase does not move anything:
// it keeps the CPU band replay, keeps its arithmetic statement-for-statement, and removes the
// ~20 non-inlined calls per pixel that G530 identified as the actual price. There are three
// precedents that reached `bad=0` doing exactly this — G530 (sprites, 238.9 M px), G570 (0x139) and
// G580 (0x13b) — and this file is G580 with its ADDRESS replaced by the PROPERTY, plus the two
// things the wider population needs that G580's single shape did not: FOG, and a general ALPHA
// register instead of the hardcoded 0x44.
//
// THE THREE LOWERINGS (each already proven exact by one of those phases):
//
//   1. BATCH-SHARED CLUT. The generic sampler decodes the palette lazily PER TRIANGLE
//      (`g141ClutValid` is memset per band, per draw). At 2.86 M replayed draws for shape #00 alone
//      that is the G580 revision-2 finding at population scale. A command-graph batch cannot
//      contain a CLUT-overlapping mutation, so one decoded palette per exact CLUT state is
//      immutable for the whole parallel replay. Reuses G580's tables verbatim.
//   2. ROW-HOISTED PRIVATE Z. `g403DisplayZRead`/`g403DisplayZWrite` are two out-of-line calls with
//      four bounds compares each, per pixel, for what is one indexed load and one indexed store
//      into a row the loop already fixed.
//   3. THE EXACT COMMIT. `writePixel` re-derives `activeContext()`, the scissor test, the frame
//      block, the destination address (a call into GSPSMCT32), the four blend selectors out of
//      `ctx.alpha`, the fbmsk decision and the FBA decision — ALL per-draw constants — and then
//      calls two trace hooks. The kernel hoists every one of them and emits writePixel's own
//      32-bit tail statement for statement.
//
// ⛔ WHAT IT DELIBERATELY DOES NOT TOUCH. Coverage, the barycentrics, the Z interpolation, the STQ
// interpolation, the bilinear filter weights, MODULATE and the fog lerp all still run in the proven
// generic code, evaluating the identical float expressions in the identical order. G529 measured
// that class of "rewrite the arithmetic" lever at 0.00 ms/f, and it is where exactness dies.
//
// ADMISSION IS BY PROPERTY, NEVER BY ADDRESS (G526/G527's law), and it is FAIL-CLOSED: anything
// that is not exactly this shape falls through to the untouched generic nest.
//
// ⭐ G608 REVISION 2 — `fst=1` IS NOW ADMITTED TOO, through a second sampler leaf.
// Revision 1 admitted `fst=0` only, because `g580SampleSharedT8` derives its texel coordinate from
// STQ (`s/q * texW`) while the `fst=1` draws carry U/V directly in the 12.4 fixed-point registers.
// That is the WHOLE difference: `g141FastSample`'s two arms share every following statement (the
// −0.5 half-texel bias, the floor, the four wrapped taps, the four `lerpChannel` calls). So the new
// leaf `g608SampleSharedT8Uv` is `g580SampleSharedT8` with its first two lines replaced by
// `g141FastSample`'s own `u/16.0f`, `v/16.0f`, and nothing else — the same "exact by construction"
// argument, one sampler down. Shape rows #06/#07/#08/#11 (`disp tristrip fst=1`, ~14% of the
// replay's cycles at ~858 cyc/inside) are what arrives.
//   (PROMOTED DEFAULT-ON)   DC2_G608_NO_TRI_FST=1   the rollback
//   DC2_G608_TRI_FST=1      retained as the bring-up spelling; it is now a no-op
//
// THE ORACLE (`DC2_G605_VERIFY_TRI=1`). The generic `writePixel` / `g403DisplayZWrite` /
// `g141FastSample` remain the COMMIT AUTHORITY while it is armed: the kernel computes its own
// address, word, depth and texel, the proven path performs the store, and the two are compared.
// A kernel defect therefore cannot reach a pixel while the oracle is on (G530 §5.1's contract).
//
// Flags (all default-OFF, all registered in `tools/run_g477_perf.ps1`'s `$clear` in the same edit):
//   (PROMOTED DEFAULT-ON)      DC2_G605_NO_TRI_SPAN=1  the rollback
//   DC2_G605_TRI_SPAN=1        retained as an explicit bring-up spelling; it is now a no-op
//   DC2_G605_VERIFY_TRI=1      the same-run oracle (implies the lever; NEVER in a timing run)
//   DC2_G605_STAT=1            `[G605:trispan]` / `[G605:reject]` admission census

namespace
{

inline bool g605TriSpanVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G605_VERIFY_TRI");
    return on;
}

inline bool g605TriSpanStatOn()
{
    static const bool on = envFlagEnabled("DC2_G605_STAT") || g605TriSpanVerifyOn();
    return on;
}

inline bool g605TriSpanRejectOn()
{
    static const bool on = envFlagEnabled("DC2_G605_REJECT");
    return on;
}

// PROMOTED DEFAULT-ON (G605). Rollback: DC2_G605_NO_TRI_SPAN=1.
// Gates: mechanism (triangle LOOP wall −21.1%), exactness (bad=0 on 251,766,617 px against a
// same-run oracle in which the generic path performs every store), payoff (two independent
// order-balanced ABBAs on two estimators: −1.102 and −1.680 ms/f, FOUR order blocks, all negative,
// p95 99.32 → 59.22 ms/f).
inline bool g605TriSpanRequested()
{
    static const bool requested = !envFlagEnabled("DC2_G605_NO_TRI_SPAN") ||
                                  g605TriSpanVerifyOn() || envFlagEnabled("DC2_G605_REJECT");
    return requested;
}

// ---------------------------------------------------------------------------------------------
// G608: the `fst=1` admission conjunct. ONE predicate, read by BOTH the entry-level eligibility
// (which decides the batch-shared palette) and the per-draw admission, so the two cannot disagree.
// It is deliberately NOT folded into g605TriSpanRequested(): the ABBA for this leaf had to differ
// from the shipped binary in exactly one conjunct, with G605's own fst=0 population identical and
// live in BOTH arms — which then made it the same-run control (`bad=0` on 251,968,349 px in the
// candidate arm, unchanged from G605's shipped 251,766,617).
//
// ⭐ PROMOTED DEFAULT-ON (G608). Rollback: DC2_G608_NO_TRI_FST=1.
// Gates, all on route `s03`:
//   mechanism — the four `disp tristrip fst=1 tbp=0x2720` shape rows go 847.1/848.7/873.9/877.1 →
//               592.7/603.7/589.7/595.9 cyc/inside, **−31.0%** over 91.4 M inside pixels with the
//               arms' denominators matched to 0.19%; the whole `disp tri-fast` leaf's LOOP wall
//               falls 53,567.7 → 45,220.7 ms (−15.6%). Untouched controls in the same runs:
//               `discov tri-fast` 644.6 → 651.6, `discov sprite` 58.5 → 59.7.
//   exactness — `DC2_G605_VERIFY_TRI=1`, generic path authoritative: **bad=0 on 88,539,956 fst=1
//               pixels / 80,000 triangles**, five comparisons per committed pixel.
//   payoff    — order-balanced A B B A, TWO estimators on the same captures agreeing to 0.006 ms/f:
//               `[G154:perf]` −0.710 ms/f (−1.89%) and `[G146:frame]` −0.704 (−1.87%); FOUR order
//               blocks, all negative (−0.440 / −0.971 / −0.442 / −0.961). Tail: windows >100 ms
//               **3 → 0**, p99 106.31 → 94.36, max 111.59 → 95.79.
inline bool g608TriFstEnabled()
{
    static const bool on = !envFlagEnabled("DC2_G608_NO_TRI_FST");
    return on;
}

inline bool g605TriSpanSelected()
{
    // ⛔ No DC2_G419_AB arm. This lever does not select an authority (the CPU replay stays
    // authoritative either way), but it DOES change which code writes the pixel, and G419's arm
    // flips per FRAME — the control frames would then composite over kernel-written targets. Gate it
    // with alternating whole runs and `tools/g603_window_pair.py`, as G602/G603/G604 did.
    static const bool on = !envFlagEnabled("DC2_G605_NO_TRI_SPAN");
    return on;
}

// Any per-pixel diagnostic that the kernel does not reproduce disarms it entirely. Fail closed.
inline bool g605TriSpanQuiet()
{
    static const bool quiet =
        g580Tri13bQuiet() &&
        !envFlagEnabled("DC2_G580_TRI_13B") &&
        // ⚠️ DC2_G605_CYC is deliberately NOT excluded. It brackets the whole drawTriangle CALL and
        // adds the identical four clock reads per draw to BOTH arms, so it is the mechanism counter
        // for this lever (`cyc/inside`), not a perturbation of it. Excluding it would have left the
        // claimed mechanism unmeasurable — the exact trap g527_lineclip_refuted_by_its_own_census
        // exists to prevent, one step earlier.
        !envFlagEnabled("DC2_G529_STAT") &&
        !envFlagEnabled("DC2_G530_CYC") &&
        !envFlagEnabled("DC2_G141_SAMPLE_VERIFY") &&
        !envFlagEnabled("DC2_G141_NO_TEXQUAD") &&
        !envFlagEnabled("DC2_G141_NO_FASTSAMPLE") &&
        !envFlagEnabled("DC2_G219_SKYPX") &&
        !envFlagEnabled("DC2_G232_WATCH") &&
        !envFlagEnabled("DC2_G365_WATCH") &&
        !envFlagEnabled("DC2_G364_NO_FOG");
    return quiet;
}

// ---------------------------------------------------------------------------------------------
// THE ALPHA TEST, ADMITTED ONLY WHEN IT IS A NO-OP — CHECKED EXHAUSTIVELY, NOT ARGUED.
//
// The measured population is `test=0x5000b`: ATE=1, ATST=GEQUAL, AREF=0, i.e. `alpha >= 0`, which
// every 8-bit alpha satisfies. Rather than encode that reasoning, run the SHIPPED
// `classifyAlphaTest` over all 256 alpha values and admit only if every one returns
// `{writeFramebuffer, writeDepth, !preserveDestinationAlpha}`. Then the per-pixel classification —
// and the branch it feeds — is provably dead, and the commit and the Z write are unconditional.
// Memoised per distinct TEST value on the calling lane (G530's pattern).
inline bool g605AlphaTestAlwaysPassesUncached(uint64_t test)
{
    for (int a = 0; a < 256; ++a)
    {
        const AlphaTestResult r = classifyAlphaTest(test, static_cast<uint8_t>(a));
        if (!r.writeFramebuffer || !r.writeDepth || r.preserveDestinationAlpha)
            return false;
    }
    return true;
}

inline bool g605AlphaTestAlwaysPasses(uint64_t test)
{
    static thread_local uint64_t s_key[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    static thread_local bool s_val[4] = {false, false, false, false};
    static thread_local int s_n = 0;
    for (int i = 0; i < 4; ++i)
        if (s_key[i] == test)
            return s_val[i];
    const bool v = g605AlphaTestAlwaysPassesUncached(test);
    s_key[s_n & 3] = test;
    s_val[s_n & 3] = v;
    ++s_n;
    return v;
}

// ---------------------------------------------------------------------------------------------
// THE BLEND, HOISTED. writePixel decodes these four 2-bit selectors and the FIX byte out of
// ctx.alpha on EVERY pixel and evaluates `pickRGB` as a lambda call per channel per operand.
// They are per-DRAW constants.
struct G605Blend
{
    bool abe = false;
    uint8_t asel = 0u, bsel = 0u, csel = 0u, dsel = 0u, fix = 0u;
};

inline G605Blend g605BuildBlend(bool abe, uint64_t alphaReg)
{
    G605Blend b;
    b.abe = abe;
    b.asel = static_cast<uint8_t>(alphaReg & 3u);
    b.bsel = static_cast<uint8_t>((alphaReg >> 2) & 3u);
    b.csel = static_cast<uint8_t>((alphaReg >> 4) & 3u);
    b.dsel = static_cast<uint8_t>((alphaReg >> 6) & 3u);
    b.fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    return b;
}

// writePixel's 32-bit tail, statement for statement, with the selectors already resolved and the
// address already synthesized. The caller guarantees CT32, fbmsk == 0, fba == 0, pabe == 0 and an
// always-passing alpha test, so the mask blend, the FBA OR and the preserve-alpha read are all
// provably dead here rather than skipped by assumption.
inline uint32_t g605CommitWord(const G605Blend &bl, uint32_t dst,
                               uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa)
{
    uint8_t r = sr, g = sg, b = sb;
    if (bl.abe)
    {
        const int dr = static_cast<int>(dst & 0xFFu);
        const int dg = static_cast<int>((dst >> 8u) & 0xFFu);
        const int db = static_cast<int>((dst >> 16u) & 0xFFu);
        const int da = static_cast<int>((dst >> 24u) & 0xFFu);
        // Identical to writePixel's `pickRGB`: 0 = Cs, 1 = Cd, 2/3 = 0.
        const auto pick = [](uint8_t sel, int cs, int cd) -> int {
            if (sel == 0u)
                return cs;
            if (sel == 1u)
                return cd;
            return 0;
        };
        const int cAlpha = (bl.csel == 0u) ? static_cast<int>(sa)
                                           : ((bl.csel == 1u) ? da : static_cast<int>(bl.fix));
        r = clampU8(((pick(bl.asel, static_cast<int>(sr), dr) -
                      pick(bl.bsel, static_cast<int>(sr), dr)) * cAlpha >> 7) +
                    pick(bl.dsel, static_cast<int>(sr), dr));
        g = clampU8(((pick(bl.asel, static_cast<int>(sg), dg) -
                      pick(bl.bsel, static_cast<int>(sg), dg)) * cAlpha >> 7) +
                    pick(bl.dsel, static_cast<int>(sg), dg));
        b = clampU8(((pick(bl.asel, static_cast<int>(sb), db) -
                      pick(bl.bsel, static_cast<int>(sb), db)) * cAlpha >> 7) +
                    pick(bl.dsel, static_cast<int>(sb), db));
    }
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(sa) << 24u);
}

// ---------------------------------------------------------------------------------------------
// THE PROPERTY — the entry-level form, used to decide the batch-shared CLUT. It must agree
// conjunct-for-conjunct with the per-draw admission in drawTriangle; anything the two could
// disagree about would silently bind a shared palette to a draw the kernel then declines (harmless)
// or, far worse, leave `t_g580SharedSample` bound from a previous entry (which is why the replay
// dispatcher clears it per entry).
//
// ⭐⭐ G608: THE ENTRY PREDICATE NEEDS ITS OWN CENSUS. The per-draw census can only ever report
// `reason=clut` for a population this predicate declines, because "no batch-shared palette bound"
// is the LAST per-draw conjunct and it is the observable consequence of every entry-level failure
// at once. G608's first run read `disp reason=clut tbp=2720` and could not tell an entry-predicate
// rejection from a `minRefs` shortfall — two different fixes. `[G608:entrywhy]` splits them.
enum
{
    kG608EntFrame = 0,
    kG608EntPrim,
    kG608EntFst,
    kG608EntTex,
    kG608EntFilter,
    kG608EntAtest,
    kG608EntZ,
    kG608EntScope,
    kG608EntBounds,
    kG608EntN
};
static const char *const kG608EntName[kG608EntN] = {
    "frame", "prim", "fst", "tex", "filter", "atest", "z", "scope", "bounds"};
std::atomic<uint64_t> g_g608Ent[2][kG608EntN];
std::atomic<uint64_t> g_g608EntOk[2];
std::atomic<uint32_t> g_g608EntShown[2][kG608EntN];

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g608NoteEntry(const G144Entry &e, int reason)
{
    const int fk = e.prim.fst ? 1 : 0;
    if (reason < 0)
    {
        g_g608EntOk[fk].fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_g608Ent[fk][reason].fetch_add(1u, std::memory_order_relaxed);
    if (g_g608EntShown[fk][reason].fetch_add(1u, std::memory_order_relaxed) != 0u)
        return;
    std::fprintf(stderr,
        "[G608:entrywhy] reason=%s fst=%d prim=%u fbp=%03x fbw=%u fpsm=%x fbmsk=%08x fba=%llx "
        "pabe=%u | tex(tbp=%04x tbw=%u psm=%02x tfx=%u tcc=%u cbp=%04x cpsm=%x csm=%u csa=%u "
        "tex1=%llx) | alpha=%llx test=%llx zbuf=%llx | y=%d..%d\n",
        kG608EntName[reason], fk, static_cast<unsigned>(e.prim.type), e.ctx.frame.fbp,
        static_cast<unsigned>(e.ctx.frame.fbw), static_cast<unsigned>(e.ctx.frame.psm),
        e.ctx.frame.fbmsk, static_cast<unsigned long long>(e.ctx.fba), e.pabe ? 1u : 0u,
        e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.tbw),
        static_cast<unsigned>(e.ctx.tex0.psm), static_cast<unsigned>(e.ctx.tex0.tfx),
        static_cast<unsigned>(e.ctx.tex0.tcc), e.ctx.tex0.cbp,
        static_cast<unsigned>(e.ctx.tex0.cpsm), static_cast<unsigned>(e.ctx.tex0.csm),
        static_cast<unsigned>(e.ctx.tex0.csa),
        static_cast<unsigned long long>(e.ctx.tex1),
        static_cast<unsigned long long>(e.ctx.alpha),
        static_cast<unsigned long long>(e.ctx.test),
        static_cast<unsigned long long>(e.ctx.zbuf), e.minY, e.maxY);
    std::fflush(stderr);
}

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g608DumpEntryCensus()
{
    for (int fi = 0; fi < 2; ++fi)
    {
        uint64_t sum = g_g608EntOk[fi].load(std::memory_order_relaxed);
        for (int i = 0; i < kG608EntN; ++i)
            sum += g_g608Ent[fi][i].load(std::memory_order_relaxed);
        if (sum == 0u)
            continue;
        std::fprintf(stderr, "[G608:entry] fst=%d ok=%llu", fi,
                     static_cast<unsigned long long>(g_g608EntOk[fi].load(std::memory_order_relaxed)));
        for (int i = 0; i < kG608EntN; ++i)
            std::fprintf(stderr, " %s=%llu", kG608EntName[i],
                         static_cast<unsigned long long>(
                             g_g608Ent[fi][i].load(std::memory_order_relaxed)));
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

static int g605EntryRejectReason(const G144Entry &e)
{
    if (e.ctx.frame.psm != GS_PSM_CT32 || e.ctx.frame.fbw != 8u ||
        e.ctx.frame.fbmsk != 0u || e.ctx.fba != 0u || e.pabe)
        return kG608EntFrame;
    if (e.prim.type != GS_PRIM_TRIANGLE && e.prim.type != GS_PRIM_TRISTRIP)
        return kG608EntPrim;
    if (!e.prim.tme || !e.prim.iip || e.prim.aa1)
        return kG608EntPrim;
    if (e.prim.fst && !g608TriFstEnabled())
        return kG608EntFst; // G608 leaf not armed — fst=1 stays with the generic nest
    if (e.ctx.tex0.psm != GS_PSM_T8 || e.ctx.tex0.tfx != 0u || e.ctx.tex0.tcc != 1u ||
        e.ctx.tex0.cpsm != GS_PSM_CT32 || e.ctx.tex0.csm != 0u || e.ctx.tex0.csa != 0u ||
        e.ctx.tex0.tbw == 0u)
        return kG608EntTex;
    if (!tex1UsesLinearFilter(e.ctx.tex1))
        return kG608EntFilter;
    if (!g605AlphaTestAlwaysPasses(e.ctx.test))
        return kG608EntAtest;
    // Z: the G403/G411 private PSMZ24 mirror at ZBP 0xd0, GEQUAL.
    // ⭐ ZMSK IS NOT A CONJUNCT. `[G605:rejwhy]` named it as the ONLY thing keeping the whole `disp`
    // triangle population (36.6% of the replay's cycles) out of this kernel: the display draws
    // depth-TEST against the private mirror and do not write it (`zbuf=0x1010000d0`). Reading the
    // mirror is the expensive half either way, and the write is one predicated store on a per-draw
    // constant — the same predicate `zWrite` the generic path already evaluates.
    const uint32_t zbpPage = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu);
    const uint32_t zpsm = static_cast<uint32_t>((e.ctx.zbuf >> 24) & 0xFu);
    const uint32_t ztst = static_cast<uint32_t>((e.ctx.test >> 17) & 0x3u);
    const bool zte = (e.ctx.test >> 16) & 0x1u;
    if (!zte || ztst != 2u || zpsm != 1u || zbpPage != 0xd0u)
        return kG608EntZ;
    if (!g203UniversalZEnabled() || !g404SharedZScope(e.ctx.frame.fbp, 8u, 0xd0u, 1u))
        return kG608EntScope;
    // The depth row pointer indexes a fixed 512x512 mirror, so the band must lie inside it.
    if (e.minY < 0 || e.maxY >= static_cast<int>(kG403DisplayZH))
        return kG608EntBounds;
    return -1;
}

static bool g605SharedEntryEligible(const G144Entry &e)
{
    const int reason = g605EntryRejectReason(e);
    if (g605TriSpanRejectOn())
        g608NoteEntry(e, reason);
    return reason < 0;
}

static bool g605SharedClutBatchSelected(const std::vector<G144Entry> &entries)
{
    if (!g605TriSpanRequested() || !g605TriSpanQuiet() || entries.empty() ||
        !g203UniversalZEnabled())
        return false;
    if (!g605TriSpanSelected() && !g605TriSpanRejectOn())
        return false;
    for (const G144Entry &e : entries)
        if (g605SharedEntryEligible(e))
            return true;
    return false;
}

static bool g605PrepareSharedCluts(const std::vector<G144Entry> &entries,
                                   const G580ClutFillFn &fill)
{
    return g580PrepareSharedClutsWith(entries, fill, &g605SharedEntryEligible);
}

// ---------------------------------------------------------------------------------------------
// G608 — THE `fst=1` SAMPLER LEAF.
//
// A VERBATIM COPY of `g580SampleSharedT8` with its first two statements replaced by
// `g141FastSample`'s own `fst` arm. Everything after the coordinate derivation — the −0.5
// half-texel bias, the floor, the fractional weights, the four wrapped `ReadP8` taps through the
// batch-shared palette, the per-channel `lerpChannel` and the pack — is character-for-character the
// STQ leaf's, which is the same code path the `fst=0` population was proven `bad=0` on across
// 251.8 M pixels. So the only NEW arithmetic in this leaf is `u/16.0f`, and that expression is
// lifted from the generic sampler rather than re-derived.
//
// ⛔ It is a COPY, not a refactor of `g580SampleSharedT8` into a shared core. Both G580's own leaf
// and G605's `fst=0` leaf call that function on the promoted default-ON path; re-shaping it to take
// pre-computed coordinates would change the inlining and register allocation of a population worth
// 70.6% of the replay's cycles to save twenty lines here (g602_prologue_is_set_by_the_coldest_code
// is the same hazard one level down). The duplication is the cheap side of that trade.
//
// `noinline` matches the STQ leaf deliberately: G605 measured 873 → 651 cyc/inside WITH that call
// boundary in place, so keeping it makes the two leaves' `[G605:leaf]` rows comparable.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint32_t g608SampleSharedT8Uv(uint8_t *vram, uint16_t u, uint16_t v)
{
    const G580SharedSample &sample = *t_g580SharedSample;
    const std::array<uint32_t, 256> &clut = g_g580SharedCluts[sample.tableIndex];
    // g141FastSample's `gs->m_prim.fst` arm: UV arrive as 12.4 fixed point already in TEXEL units,
    // so neither Q nor texW/texH takes part (they still set the WRAP domain below, exactly as
    // g141SamplePoint uses g141TexW/g141TexH for both arms).
    const float texUf = static_cast<float>(u) / 16.0f;
    const float texVf = static_cast<float>(v) / 16.0f;
    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);
    if (!t_g580Quad.valid || t_g580Quad.u0 != u0 || t_g580Quad.v0 != v0)
    {
        auto point = [&](int uu, int vv) {
            const int su = wrapTextureCoordinate(uu, sample.wrapU, sample.texW,
                                                 sample.minU, sample.maxU);
            const int sv = wrapTextureCoordinate(vv, sample.wrapV, sample.texH,
                                                 sample.minV, sample.maxV);
            const uint8_t index = static_cast<uint8_t>(GSMem::ReadP8(
                vram, sample.tbp0, sample.pageBw,
                static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            return clut[index];
        };
        t_g580Quad.valid = true;
        t_g580Quad.u0 = u0;
        t_g580Quad.v0 = v0;
        t_g580Quad.c00 = point(u0, v0);
        t_g580Quad.c10 = point(u0 + 1, v0);
        t_g580Quad.c01 = point(u0, v0 + 1);
        t_g580Quad.c11 = point(u0 + 1, v0 + 1);
    }
    const uint32_t c00 = t_g580Quad.c00, c10 = t_g580Quad.c10;
    const uint32_t c01 = t_g580Quad.c01, c11 = t_g580Quad.c11;
    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00), static_cast<uint8_t>(c10),
                                  static_cast<uint8_t>(c01), static_cast<uint8_t>(c11), fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>(c00 >> 8u), static_cast<uint8_t>(c10 >> 8u),
                                  static_cast<uint8_t>(c01 >> 8u), static_cast<uint8_t>(c11 >> 8u), fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>(c00 >> 16u), static_cast<uint8_t>(c10 >> 16u),
                                  static_cast<uint8_t>(c01 >> 16u), static_cast<uint8_t>(c11 >> 16u), fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>(c00 >> 24u), static_cast<uint8_t>(c10 >> 24u),
                                  static_cast<uint8_t>(c01 >> 24u), static_cast<uint8_t>(c11 >> 24u), fx, fy);
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

// ---------------------------------------------------------------------------------------------
// Census. `[G605:trispan]` is the divergence proof a null A/B needs
// (g538_null_ab_needs_a_path_counter); `[G605:reject]` names the FIRST failing conjunct so a
// population that does not arrive can be diagnosed without a rebuild.
// ⭐ SPLIT BY `fst` (G608). A pooled trispan counter cannot serve as the new leaf's divergence
// proof, and it cannot carry the new leaf's own oracle denominator either: G605's fst=0 population
// is live and identical in BOTH arms of the G608 A/B, so a pooled `bad=0` would be dominated by
// pixels the G608 leaf never touched (g589_oracle_with_zero_checks, one level up —
// `bad=0` on the WRONG population is the same failure as `bad=0` with `checks=0`).
std::atomic<uint64_t> g_g605SpanTris[2];
std::atomic<uint64_t> g_g605SpanPx[2];
std::atomic<uint64_t> g_g605SpanBad[2];

inline void g605NoteTriSpan(uint64_t pixels, uint64_t bad, bool fst)
{
    if (!g605TriSpanStatOn())
        return;
    const int k = fst ? 1 : 0;
    const uint64_t tris = g_g605SpanTris[k].fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px = g_g605SpanPx[k].fetch_add(pixels, std::memory_order_relaxed) + pixels;
    const uint64_t totalBad = g_g605SpanBad[k].fetch_add(bad, std::memory_order_relaxed) + bad;
    const bool firstBad = bad != 0u && totalBad == bad;
    // ⚠️ G608: the stride WAS 100000, and the `fst=1` population runs to ~84 k triangles on a whole
    // `s03` route — so the counter printed `tris=8` and then went silent, and the phase read that as
    // "the population never arrives" and went hunting for an admission blocker that did not exist.
    // A census stride must be smaller than the population it is meant to size.
    if (tris <= 8u || (tris % 10000u) == 0u || firstBad)
        std::fprintf(stderr, "[G605:trispan] fst=%d tris=%llu px=%llu bad=%llu mode=%s\n", k,
                     static_cast<unsigned long long>(tris),
                     static_cast<unsigned long long>(px),
                     static_cast<unsigned long long>(totalBad),
                     g605TriSpanVerifyOn() ? "verify" : "fast");
}

enum
{
    kG605RejState = 0, // frame/prim/tex state
    kG605RejSampler,   // the generic fast sampler itself declined
    kG605RejAlphaTest,
    kG605RejDepth,
    kG605RejBounds,
    kG605RejClut,      // no batch-shared palette bound for this entry
    kG605RejN
};
static const char *const kG605RejName[kG605RejN] = {"state", "sampler", "atest", "depth",
                                                    "bounds", "clut"};
// ⭐ BUCKETED BY BATCH CLASS. A whole-population reject count cannot say which POPULATION is
// blocked, and on `s03` the two triangle populations (`disp`, `discov`) are 36.6% and 42.8% of the
// replay's cycles independently — a lever can take one and miss the other while the pooled census
// still looks healthy. That is exactly what the first G605 mechanism run found.
//
// ⭐⭐ G608 ADDS `fst` AS A SECOND BUCKETING AXIS, for the same reason one level down. The first
// G608 run admitted **8** `fst=1` triangles against 7.8 M `fst=0` ones, and a census bucketed only
// by CLASS cannot say why: `disp`'s `ok` and its rejections are both dominated by the fst=0
// population that G605 already won, so the fst=1 blocker is invisible inside the same row. The
// example dump is keyed on (class, reason, fst) too — otherwise the one dump slot per (class,
// reason) is consumed by an fst=0 draw and the population under investigation never prints.
std::atomic<uint64_t> g_g605Rej[2][kG528ClsN][kG605RejN];
std::atomic<uint64_t> g_g605RejOk[2][kG528ClsN];
std::atomic<uint32_t> g_g605RejShown[2][kG528ClsN][kG605RejN];
std::atomic<uint64_t> g_g605RejCalls{0u};

struct G605RejectFacts
{
    uint32_t fbp, fbw, fbmsk, tbp, tbw, cbp;
    uint32_t fpsm, tpsm, tfx, tcc, cpsm, csm, csa;
    uint32_t zbpPage, zpsm, ztst, zte, zmsk;
    uint32_t iip, fst, fge, aa1, abe, tme, pabe, fba, lin, privz, fastSampler;
    uint64_t tex1, alpha, test, zbuf;
    int minX, maxX, minY, maxY;
};

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g605NoteReject(uint32_t fbp, int reason, const G605RejectFacts &f)
{
    const uint32_t k = g528ClassifyFbp(fbp);
    const int cls = (k < static_cast<uint32_t>(kG528ClsN)) ? static_cast<int>(k) : kG528ClsOther;
    const int fk = f.fst ? 1 : 0;
    const uint64_t calls = g_g605RejCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (reason < 0)
        g_g605RejOk[fk][cls].fetch_add(1u, std::memory_order_relaxed);
    else
        g_g605Rej[fk][cls][reason].fetch_add(1u, std::memory_order_relaxed);

    // One fully-dumped example per (class, reason, fst): the first failing conjunct plus every field
    // the admission reads, so the blocker can be named without a second build.
    if (reason >= 0 && g_g605RejShown[fk][cls][reason].fetch_add(1u, std::memory_order_relaxed) == 0u)
        std::fprintf(stderr,
            "[G605:rejwhy] cls=%s reason=%s fbp=%03x fbw=%u fpsm=%x fbmsk=%08x fba=%u pabe=%u | "
            "prim(tme=%u iip=%u fst=%u fge=%u aa1=%u abe=%u) | tex(tbp=%04x tbw=%u psm=%02x tfx=%u "
            "tcc=%u cbp=%04x cpsm=%x csm=%u csa=%u tex1=%llx lin=%u fastSampler=%u) | "
            "alpha=%llx test=%llx | z(zbuf=%llx page=%03x psm=%x ztst=%u zte=%u zmsk=%u privz=%u) | "
            "bbox=%d,%d..%d,%d\n",
            kG528ClsName[cls], kG605RejName[reason], f.fbp, f.fbw, f.fpsm, f.fbmsk, f.fba, f.pabe,
            f.tme, f.iip, f.fst, f.fge, f.aa1, f.abe, f.tbp, f.tbw, f.tpsm, f.tfx, f.tcc, f.cbp,
            f.cpsm, f.csm, f.csa, static_cast<unsigned long long>(f.tex1), f.lin, f.fastSampler,
            static_cast<unsigned long long>(f.alpha), static_cast<unsigned long long>(f.test),
            static_cast<unsigned long long>(f.zbuf), f.zbpPage, f.zpsm, f.ztst, f.zte, f.zmsk,
            f.privz, f.minX, f.minY, f.maxX, f.maxY);

    // ⛔ The periodic dump used to trigger on a SUM that included `ok` — but the `ok` path returns
    // before reaching this code, so the trigger could only fire on a rejection that happened to land
    // exactly on a multiple of 1 M. On a route where the kernel admits nearly everything it fired
    // ZERO times, and the phase read "no [G605:reject] line" as "no rejections". Drive it off this
    // function's OWN call counter instead, which advances on `ok` as well.
    if ((calls % 1000000ull) != 0ull)
        return;
    for (int fi = 0; fi < 2; ++fi)
        for (int c = 0; c < kG528ClsN; ++c)
        {
            uint64_t sum = g_g605RejOk[fi][c].load(std::memory_order_relaxed);
            for (int i = 0; i < kG605RejN; ++i)
                sum += g_g605Rej[fi][c][i].load(std::memory_order_relaxed);
            if (sum == 0u)
                continue;
            std::fprintf(stderr, "[G605:reject] fst=%d %-6s ok=%llu", fi, kG528ClsName[c],
                         static_cast<unsigned long long>(
                             g_g605RejOk[fi][c].load(std::memory_order_relaxed)));
            for (int i = 0; i < kG605RejN; ++i)
                std::fprintf(stderr, " %s=%llu", kG605RejName[i],
                             static_cast<unsigned long long>(
                                 g_g605Rej[fi][c][i].load(std::memory_order_relaxed)));
            std::fprintf(stderr, "\n");
        }
    std::fflush(stderr);
    g608DumpEntryCensus();
}

} // namespace
