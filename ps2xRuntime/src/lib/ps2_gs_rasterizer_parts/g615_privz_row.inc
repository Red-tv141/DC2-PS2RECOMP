// ===== G615: THE PRIVATE-Z DISPLAY SPRITE, ADMITTED INTO THE TWO PROMOTED FAST PATHS =====
// revision: 1
//
// WHY THIS EXISTS — AND WHY THE PHASE BEFORE IT LOOKED SOMEWHERE ELSE.
//
// ⛔⛔ FIRST, A CENSUS DEFECT. `drawSprite`'s only `g605cyc.markLoop()` sat inside
// `if (gs->m_prim.tme)`, so every UNTEXTURED sprite booked its ENTIRE fill loop into the PROLOGUE
// column of the `sprite` leaf. That is the whole content of `[G605:leaf] disp sprite pro% = 38.8` at
// `50.79 us/call` (G613 §5a) — the number ROADMAP named "Target K — the per-DRAW prologue" and told
// the next phase to decompose. With the untextured branch marked and split into its own leaf the
// same route reads `disp sprite pro% = 1.1` at **1.00 us/call**. There was never a prologue there.
// (`[G530:cyc]`'s own comment always said "culled/untextured: all of it is prologue"; the G605 copy
// dropped the word "untextured", and two phases read the column as a per-draw envelope.)
//
// ⭐⭐⭐ WHAT THE HONEST RE-RANK NAMES INSTEAD. On `dungeon1`, whole run, shipped G614 binary:
//
//   leaf                share%   n        px           cyc/inside   pro%
//   discov sprite        62.1     232,759  256,180,224   322.8        1.5
//   disp   sprite        23.7     110,147   45,162,920   701.5        1.1   <- 2.17x the SAME leaf
//   disp   spr-untex     13.9      19,352   59,533,088   312.8        0.4   <- a SOLID COLOUR FILL
//
// Two populations, 37.6% of the route's whole CPU band replay, and ONE cause. Every `disp` draw on
// this route carries `prz=1` — the G403/G411 PRIVATE DEPTH MIRROR is the depth authority for the
// display target — and that single property is a hard REFUSAL conjunct in both promoted fast paths:
//
//   * the G530/G614 sprite SPAN KERNEL refuses on `!g403DisplayZ` (it has no private-Z read arm at
//     all, so admitting one would have read guest VRAM Z and been wrong);
//   * G588's exact SOLID FILL refuses on `g588State = !abe && !sZActive && !sZWrite`.
//
// Both refusals are MEASURED, not inferred: `[G529:sprwhy] target=0 fst=0 tbw=129 trace=0 filter=0`
// (so the T8 display sprites pass the G248 sampler predicate — the span kernel is what rejects
// them) and `[G588:solid] draws=0 pixels=0 why(q=0 layout=0 state=2 alpha=0)` — the promoted solid
// fill is COMPLETELY INERT on this route, refused at `state`.
//
// ⭐⭐⭐ WHAT THE CENSUS THEN DID TO BOTH HALVES OF THAT PLAN — READ THIS BEFORE EXTENDING THE FILE.
//
// Revision 1 built a private-Z ROW ACCESSOR (hoist the mirror base per draw and the row per row, so
// the per-pixel cost is one compare plus an indexed load/store instead of an out-of-line call into a
// function-local-static `std::vector` accessor) and used it to admit BOTH populations. Armed with
// `DC2_G615_STAT=1`, its own coverage census refuted both halves of the premise in one run:
//
//   [G615:privz] span(draws=7140 px=37546952 zRd=0 zSt=0)
//                rej(abe=19352 notpriv=0 ztst=0 nowrite=0) spanRej(samp=102989 other=20)
//
//   * `zRd=0 zSt=0` over 37.5 M ADMITTED PIXELS. The mirror is never read and never written on this
//     route: these draws carry ZTST=ALWAYS with ZMSK set, and drawSprite's own fast-skip rule
//     (`!(sZtst==1 && sZmsk)`) therefore makes `sZActive` FALSE. ⛔ So the refused conjunct
//     `!g403DisplayZ` was guarding a depth operation THAT DOES NOT HAPPEN — it was over-broad, not
//     wrong — and the row accessor it motivated has no work to do. Building a mirror arm anyway
//     would have shipped an UNEXERCISED path certified by `bad=0` over `checks=0`
//     (`g589_oracle_with_zero_checks`). Both are deleted; see the accessor below, kept only because
//     a future route with a live mirror will need exactly it.
//   * `rej(abe=19352)`. G588's solid fill prints its reject class as `state`, and `state` is
//     `!abe && !sZActive && !sZWrite` — a CONJUNCTION of three terms. Revision 1 read "state" as the
//     depth terms and built for them. Split by conjunct, **every one of the 19,352 untextured
//     display fills is refused on ALPHA BLENDING** — not a depth question, and not a constant store
//     either. ⭐⭐ A FIRST FAILING SITE IS NOT A REASON WHEN THE SITE IS A CONJUNCTION.
//
// THE LEVER THAT SURVIVED, IN ONE LINE: admit exactly the subset whose depth operation is a proven
// NO-OP, which needs NO new code in the pixel loop at all.
//
//     g615ZNoop = !sZActive && !sZWrite       // the mirror is provably neither read nor written
//     admit if   g403DisplayZ && g615ZNoop    // instead of refusing on g403DisplayZ alone
//
// For that subset the kernel's `if (sZActive)` read is not taken and its `spriteZStore` self-guards
// on `!sZWrite`, so the admitted draw's pixels are produced by the SAME G530/G614 statements already
// covered by `[G530:span] bad=0` — the kernel's own per-pixel oracle, which authored none of them.
// Exact by construction, with no new branch inside the loop and no new oracle needed.
//
// ⛔ WHAT IT DELIBERATELY DOES NOT DO. It does not touch the depth value, the alpha gate, the
// coverage rule, the blend, or any texel. It changes WHICH LOOP an already-correct draw runs in.
// A lever that changed arithmetic is where exactness dies (G529).
//
// ⛔ NOT IN SCOPE, AND NAMED FOR THE NEXT PHASE (both measured here, neither built):
//   1. the `disp sprite` PSMT4HH population — `spanRej(samp=102989)`, and `[G529:sprwhy] psm=102873
//      psm(c:102873)` agrees: 102,989 private-Z draws (shape rows #08/#09/#12-#15, ~15.2% of replay
//      cycles at 366-855 cyc/inside) are refused by `g248FastPsm`, which admits only CT32/CT24/T8.
//      It needs its own tap leaf. Note it is only 6.5% of admitted pixels' worth by AREA — these are
//      the small draws.
//   2. the untextured BLENDED display fill — `rej(abe=19352)`, 59.5 M px at 312.8 cyc/inside, 13.9%
//      of the route's replay. Needs an exact blended constant-source fill, i.e. G588 with one
//      destination load per group instead of none.
//
// FLAGS (all registered in `tools\run_g477_perf.ps1`'s `$clear` in the same edit that created them):
//   DC2_G615_NO_PRIVZ_SPAN=1    the rollback
//   DC2_G419_AB=privz           the paired gate
//   DC2_G615_STAT=1             `[G615:privz]` coverage + refusal-by-conjunct census. ⭐ Read
//                               `span(draws=…)` beside `spanRej(…)`: the admitted count is only
//                               meaningful against its denominator, and a CLEAN result needs one
//                               just as much as a defect rate does.
//
// ⚠️ `g615PrivZArm()` is DEFINED at GLOBAL scope in g419_ab_instrument.inc, which this TU includes
// far ahead of this file. ⛔ Do NOT forward-declare it below: this file's body is inside an
// anonymous namespace and a declaration there binds to a different, never-defined entity (the
// G568 linkage trap, recorded in `appendix-dc2-project.md` §3).

namespace
{

// ⚠️ READ ONCE PER DRAW. Each is a function-local `static const bool`, i.e. a per-ACCESS TLS init
// guard — exactly the class of cost this lever exists to delete. Never call one inside a pixel loop.
inline bool g615PrivZOn()
{
    const int arm = g615PrivZArm();
    if (arm >= 0)
        return arm != 0;
    return true;
}
inline bool g615SpanOn()
{
    static const bool on = !envFlagEnabled("DC2_G615_NO_PRIVZ_SPAN");
    return on && g615PrivZOn();
}
inline bool g615StatOn()
{
    static const bool on = envFlagEnabled("DC2_G615_STAT");
    return on;
}

// ---------------------------------------------------------------------------------------------
// THE ROW ACCESSOR — ✅ **G617 WIRED IT IN, AND THE LICENSING SIGNAL THIS FILE NAMED IS WHAT DID IT.**
//
// ⚠️ HISTORY, because the reasoning is the point. G615 wrote these, measured `zRd=0 zSt=0` over
// 37.5 M admitted pixels, refused to use them (an unexercised path certified by `bad=0` over
// `checks=0` is not proof), and recorded the reading that would reverse the decision. G617 built its
// untextured-fill kernel with G615's no-op-only depth rule FIRST and its census fired exactly that
// signal: `[G617:fill] rej(z=18176)` of ~19,352 draws — 94% of that population BY DRAW writes depth,
// every one to this mirror. Those same draws carry `ALPHA=0x2a` (the identity blend), so they are
// also the ones whose committed word is a draw CONSTANT: without a depth arm the constant-word
// group store measured `const(draws=0 px=0)` — DEAD CODE. The arm was licensed by data.
//
// ⭐ WHAT IS USED, AND WHAT IS STILL NOT. `g617_sprite_fill.inc`'s hook uses `g615ZBase` +
// `g615ZRowFor` + a per-row constant run, for the WRITE side only, admitted only when
// `sZtst == ALWAYS` (so `spriteZPass` returns true without reading `zdst`) and the destination is
// this mirror. ⛔ `g615ZRead` is STILL UNUSED and still has no route that exercises it: no measured
// route performs a private-mirror depth TEST. Do not wire it in until one does — the standard has
// not changed, only the evidence for the write side.
//
// EXACTNESS ARGUMENT, statement for statement against the shipped pair:
//
//   g403DisplayZRead (x,y,_):  if (x<0||y<0||x>=512||y>=512) return 0;  return buf[y*512+x];
//   g403DisplayZWrite(x,y,_,z):if (x<0||y<0||x>=512||y>=512) return;    buf[y*512+x] = z;
//
// `g615ZRow` resolves the `y` half of that predicate once per row and yields a null row pointer when
// it fails, so `g615ZRead`/`g615ZStore` reproduce the SAME total predicate — `row.p != nullptr`
// carries `0 <= y < 512` and the `x` compare is unchanged. `buf` is `g403DisplayZBuf()`'s flat
// `std::vector<uint32_t>` of exactly 512*512 elements, so `p + y*512 + x` is the same element
// `operator[]` would have selected. Neither shipped function reads its `fbp` argument.
struct G615ZRow
{
    uint32_t *p; // nullptr == this y is outside the mirror, i.e. every access on this row is a no-op
};

inline uint32_t *g615ZBase()
{
    // ONE call to the function-local-static accessor per DRAW instead of one per PIXEL ACCESS.
    return g403DisplayZBuf().data();
}

inline G615ZRow g615ZRowFor(uint32_t *base, int y)
{
    if (base == nullptr || y < 0 || y >= static_cast<int>(kG403DisplayZH))
        return G615ZRow{nullptr};
    return G615ZRow{base + static_cast<size_t>(y) * static_cast<size_t>(kG403DisplayZW)};
}

inline uint32_t g615ZRead(const G615ZRow &row, int x)
{
    if (row.p == nullptr || x < 0 || x >= static_cast<int>(kG403DisplayZW))
        return 0u;
    return row.p[static_cast<size_t>(x)];
}

inline void g615ZStore(const G615ZRow &row, int x, uint32_t z)
{
    if (row.p == nullptr || x < 0 || x >= static_cast<int>(kG403DisplayZW))
        return;
    row.p[static_cast<size_t>(x)] = z;
}

// ---------------------------------------------------------------------------------------------
// THE CENSUS. Per-DRAW granularity only (never per pixel — g526_draw_census.inc's rule), plus one
// pixel-count add per draw. `[G615:privz]` is a coverage report AND a debugger for the admission:
// count REFUSALS BY REASON (`g612_coverage_census_is_a_debugger`). ⭐ Read `bad` only beside
// `spanDraws`/`solidDraws` — `bad=0` with zero admitted draws is a FAILED run, not a proof
// (`g589_oracle_with_zero_checks`).
// ⚠️ REVISION 2. Revision 1 booked a reason only when `sZActive`, which cannot distinguish "the
// depth conjunct rejected this draw" from "ABE did" — and ABE is not a depth question at all. Name
// every conjunct of `g588State`, in the order it is evaluated (`g603_admission_census_names_the_
// blocker`): a first failing conjunct is only useful if the census can also say what comes next.
enum G615SolidRej
{
    kG615SolidRejNone = 0,
    kG615SolidRejAbe,    // ALPHA BLENDING is on: not a constant store, and nothing to do with Z
    kG615SolidRejNotPriv,// depth is active but NOT on the private mirror (guest VRAM Z)
    kG615SolidRejZtst,   // private mirror, but ZTST is not ALWAYS: the test can reject a pixel
    kG615SolidRejNoWrite,// private mirror, ALWAYS, but ZMSK blocks the write (already a Z no-op)
    kG615SolidRejN
};
static const char *const kG615SolidRejName[kG615SolidRejN] = {
    "none", "abe", "notpriv", "ztst", "nowrite"};

// The SPAN side's own refusal reasons — the COVERAGE DENOMINATOR for (b). Without it, "7,015 draws
// admitted" has no meaning: a clean result needs a denominator exactly as much as a defect rate does
// (`g536_coverage_denominator_for_a_defect_rate`).
enum G615SpanRej
{
    kG615SpanRejNone = 0,
    kG615SpanRejSamp, // the G248 fast sampler predicate refused (on this route: PSMT4HH)
    kG615SpanRejFpsm, // destination is not CT32/CT24
    kG615SpanRejFog,  // FGE + fog enabled
    kG615SpanRejOther,// a G530 probe / verifier / degenerate rect / null VRAM
    kG615SpanRejN
};
static const char *const kG615SpanRejName[kG615SpanRejN] = {
    "none", "samp", "fpsm", "fog", "other"};
static std::atomic<uint64_t> g_g615SpanRej[kG615SpanRejN];
static std::atomic<uint64_t> g_g615SpanDraws{0}, g_g615SpanPx{0};
static std::atomic<uint64_t> g_g615SolidRej[kG615SolidRejN];

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g615Report()
{
    // ⚠️ REVISION 2. At 20,000 the whole `dungeon1` run produced ONE print, at `draws=1`, because
    // this population is ~7,000 draws of ~5,000 px each — few draws, huge pixels. A report cadence
    // has to be set from the POPULATION's draw count, not copied from another census's.
    static std::atomic<uint64_t> s_ticks{0};
    if ((s_ticks.fetch_add(1u, std::memory_order_relaxed) % 500ull) != 0ull)
        return;
    std::fprintf(stderr,
                 "[G615:privz] span(draws=%llu px=%llu) solidRej(",
                 static_cast<unsigned long long>(g_g615SpanDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g615SpanPx.load(std::memory_order_relaxed)));
    for (int i = 0; i < kG615SolidRejN; ++i)
        std::fprintf(stderr, "%s=%llu ", kG615SolidRejName[i],
                     static_cast<unsigned long long>(
                         g_g615SolidRej[i].load(std::memory_order_relaxed)));
    std::fprintf(stderr, ") spanRej(");
    for (int i = 0; i < kG615SpanRejN; ++i)
        std::fprintf(stderr, "%s=%llu ", kG615SpanRejName[i],
                     static_cast<unsigned long long>(
                         g_g615SpanRej[i].load(std::memory_order_relaxed)));
    std::fprintf(stderr, ")\n");
    std::fflush(stderr);
}

} // namespace
