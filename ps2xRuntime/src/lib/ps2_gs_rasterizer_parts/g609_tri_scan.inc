// ===== G609: the TIGHT SCANLINE for the G605/G608-admitted replayed triangle =====
// revision: 1
//
// WHAT THE MEASUREMENT SAID (G608 §7.1, route `s03`, `DC2_G605_CYC=1`):
//
//   leaf              share of replay CPU   cyc/inside   pro%
//   discov tri-fast          43.6%             651.4     12.9
//   disp   tri-fast          34.7%             621.6      0.4
//   disp   sprite            10.8%             150.7     67.8
//   discov sprite            10.7%              60.7      6.6
//
// ⭐⭐⭐ Triangles are 78.3% of the CPU band replay, and the LOWERED triangle pixel still costs
// 10.2x the LOWERED sprite pixel — both already through a promoted span kernel, in the same
// batches. G608 named the residue: per-pixel interpolation (⛔ NOT exact-lowerable — a DDA
// rewrite is not bit-identical), TWO per-pixel CALL boundaries, and ~7 default-off per-pixel
// DIAGNOSTIC blocks between the Z test and the exact leaf.
//
// WHAT THIS FILE DOES. G605/G608 put the exact leaf INSIDE the generic per-pixel nest, as an arm.
// Every admitted pixel therefore still walks the whole generic scaffolding to reach it:
//
//   * `s_g408IntegerPixelCenter`, `s_g357z`, `s_g219` are FUNCTION-LOCAL statics with dynamic
//     initialisers, so MSVC emits a thread-safe-init guard (a TLS `_Init_thread_epoch` load, a
//     compare and a branch) on EVERY ACCESS — three of them per pixel, one of them before the
//     coverage test;
//   * `g195ForegroundTraceTarget(ctx, x, y)` is an out-of-line CALL per pixel carrying a fourth;
//   * ~12 further per-draw-constant branches (`g570Cpu139`, `g408ShadowVolume`, `g232tri` x3,
//     `zEnabled`, `g580Tri13b`, `g605Tri`, `g357FlatDraw`, `g106TitleZ`, `g44FaceZ`, `g232watch`)
//     and the `ztst` switch, none of which can vary within a draw;
//   * and — per `g602_prologue_is_set_by_the_coldest_code` — the ~1,100 lines of default-off
//     `fprintf` diagnostics in the same lambda set its callee-saved prologue and its register
//     allocation. G602 took −11.78% off `parserOther` by removing exactly that pressure from the
//     GIF decoder, changing no arithmetic.
//
// So this is not a new lowering: it is the SAME lowering, given its own scanline. When the draw is
// admitted (and no per-pixel diagnostic the kernel does not reproduce is armed), the row runs in
// `g609TriScanRowT`, an outlined `noinline` function whose body is the generic loop's own
// statements with every per-DRAW-constant conjunct resolved at admission:
//
//   admission conjunct              what it makes dead              in the generic loop
//   ------------------------------  ------------------------------  ------------------------
//   !g408ShadowVolume               the 12.4 fixed-point edge arm    2095..2112
//   !g570Cpu139, !g580Tri13b        two whole leaf arms              2068..2078, 2434..
//   zEnabled && z24 && ztst == 2    the zmax select and the switch   2129, 2185..2191
//   g403DisplayZ (private mirror)   five Z-source arms               2134..2184
//   !g357FlatDraw                   the flat-blit override           2192
//   alpha test always passes        `classifyAlphaTest` per pixel    (already G605's)
//   tfx == MODULATE && tcc == 1     `combineTexture`'s switch + CALL 2362 (see below)
//
// ⭐ THE ARITHMETIC IS COPIED TEXTUALLY, NOT RE-DERIVED. `w0`/`w1`/`w2`, the Z interpolation and
// its clamps, the shade interpolation, both sampler arms, the fog lerp and `g605CommitWord` appear
// here as the SAME EXPRESSIONS IN THE SAME ORDER as `rasterizer_draw_triangle.inc`, deliberately
// un-hoisted and un-refactored so the compiler makes the identical rounding decisions
// (`g605_lowering_is_exact_by_construction`). Nothing is rewritten; things are only DELETED, and
// only where the admission predicate proves them dead.
//
// ⭐ THE ONE CALL THIS FILE DOES REMOVE (Target G, half of it). `combineTexture` is a per-pixel
// call whose `switch (tex.tfx)` and `tex.tcc != 0` test are BOTH per-draw constants that G605's
// admission already pins: `ctx.tex0.tfx == 0u` (MODULATE) and `ctx.tex0.tcc == 1u`. Under those two
// conjuncts the function reduces, statement for statement, to the four `clampU8((t * v) >> 7)`
// lines inlined below. That is dead-code elimination licensed by the admission, not an
// approximation. `DC2_G609_NO_COMBINE=1` restores the real call for a mechanism arm.
//   ⛔ The two SAMPLER calls are NOT inlined here. They are `noinline` on purpose (G608 §1.1) and
//   inlining them would change the register allocation of a population worth 78.3% of the replay
//   in the same edit as everything else. That is a separate slice with its own arm.
//
// THE ORACLE (`DC2_G609_VERIFY=1`) — a SAME-RUN, PER-PIXEL, ROW-LEVEL comparison, and the reason
// this file can claim exactness at all. `DC2_G605_VERIFY_TRI` cannot be reused: its authority is
// `g141FastSample`/`writePixel`, which are LAMBDAS local to `drawTriangle` and unreachable from an
// outlined function. So instead, for every eligible row:
//
//   1. snapshot the destination colour words and the private-Z entries the row can touch;
//   2. run the SHIPPED generic loop for the row (the G605/G608 arm, already `bad=0` on 340 M px);
//   3. capture what it produced, and RESTORE the snapshot;
//   4. run `g609TriScanRowT` over the same row;
//   5. compare word for word and depth for depth, and count every disagreement.
//
// The generic path performs the authoritative store in step 2 and the kernel's output is then
// discarded unless it agrees — so a kernel defect cannot reach a pixel while the oracle is armed
// (G530 §5.1's contract, one level up: it verifies the WHOLE ROW rather than one leaf).
//
// Flags (all default-OFF; all registered in `tools/run_g477_perf.ps1`'s `$clear` in the same edit):
//   DC2_G609_TRI_SCAN=1    arm the tight scanline           (bring-up spelling)
//   DC2_G609_NO_TRI_SCAN=1 the rollback once promoted
//   DC2_G609_VERIFY=1      the same-run row oracle (implies the lever; NEVER in a timing run)
//   DC2_G609_STAT=1        `[G609:scan]` coverage + rejection census
//   DC2_G609_NO_COMBINE=1  keep the real `combineTexture` CALL inside the kernel
//
// ⭐⭐⭐ G628 REVISION — THE `tme=0` INSTANTIATION (Target J).
// `g609TriScanRowT` gains `TEXTURED` as its FIRST template parameter. With `TEXTURED == false` the
// sampler call, the bilinear quad memo, the CLUT indirection, the STQ/UV interpolation and
// `combineTexture` are ABSENT from the instantiation — not skipped by a runtime branch — and the
// Gouraud shade goes straight to the fog lerp and `g605CommitWord`. That is exactly what the
// generic loop's own untextured arm does (`rasterizer_draw_triangle.inc:2652..2666` then :3033), so
// the lowering is a DELETION licensed by the admission, never a rewrite
// (`g605_lowering_is_exact_by_construction`). Coverage, the barycentrics, the Z interpolation and
// test, the fog lerp and the commit are the SAME statements the textured instantiation ships.
//   ⚠️ A TEMPLATE parameter, not a runtime bool, for the same reason slice B used one: a runtime
//   switch would leave the sampler body in the untextured arm's register allocation and prologue
//   (`g602_prologue_is_set_by_the_coldest_code`), which is the entire mechanism being bought.

namespace
{

// ---------------------------------------------------------------------------------------------
// Flags

inline bool g609TriScanVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G609_VERIFY");
    return on;
}

inline bool g609TriScanStatOn()
{
    static const bool on = envFlagEnabled("DC2_G609_STAT") || g609TriScanVerifyOn();
    return on;
}

// ⭐ PROMOTED DEFAULT-ON (G609 slice A). Rollback: DC2_G609_NO_TRI_SCAN=1.
// Gates, all on route `s03`, all against the shipped default-ON generic loop as the control:
//   mechanism  — `[G605:leaf]` on an INSTRUMENT-MATCHED pair (`DC2_G605_CYC=1` and nothing else in
//                both arms), denominators matched to 4e-7: `disp tri-fast` cyc/inside
//                614.9 -> 533.0 (**-13.3%**) and LOOP wall 45,638.6 -> 39,603.5 ms (-13.2%);
//                `discov tri-fast` 646.7 -> 566.0 (**-12.5%**), LOOP 50,550.2 -> 44,272.7 (-12.4%).
//                Untouched control in the same runs: `discov sprite` 59.4 -> 59.4 (0.0%).
//   exactness  — `DC2_G609_VERIFY=1`, the generic loop authoritative: **bad=0 on 680,896,350
//                per-pixel colour+depth comparisons** over 64,000,000 rows / 342.5 M committed px,
//                `noBounds=0`, `crashLines=0`.
//   payoff     — order-balanced A B B A, two estimators on the SAME captures agreeing to
//                0.008 ms/f: `[G154:perf]` **-0.564 ms/f (-1.53%)**, se 0.125, and `[G146:frame]`
//                **-0.556 (-1.51%)**, se 0.105. FOUR order blocks, all negative
//                (-0.589 / -0.538 / -0.578 / -0.534).
//   distribution — p50 34.31 -> 32.61, p99 94.22 -> 88.60, max 95.62 -> 89.40,
//                windows over 33.33 ms 87 -> 83.
// `DC2_G609_TRI_SCAN=1` is retained as the bring-up spelling; it is now a no-op.
inline bool g609TriScanRequested()
{
    static const bool on = !envFlagEnabled("DC2_G609_NO_TRI_SCAN");
    return on;
}

inline bool g609InlineCombine()
{
    static const bool on = !envFlagEnabled("DC2_G609_NO_COMBINE");
    return on;
}

// ⭐ SLICE B (Target G's other half) — the SAMPLER call boundary, inside the kernel only.
// `g580SampleSharedT8` / `g608SampleSharedT8Uv` are `noinline` by design (G608 §1.1), and each
// per-pixel call additionally re-reads TWO thread_locals (`t_g580SharedSample`, `t_g580Quad`) and
// indexes a std::vector, all of which are per-DRAW constants or a pure per-ROW memo. Inside the
// outlined kernel the same statements can be inlined with the sample, the palette pointer and the
// bilinear quad memo hoisted — no arithmetic changes, the memo is a cache and cannot change a
// result, and the whole thing is covered by the same `DC2_G609_VERIFY=1` row oracle.
//   ⚠️ Selected by a TEMPLATE parameter, not a runtime bool, so the control instantiation contains
//   no inlined sampler at all. A runtime switch would leave the inlined body in the OFF arm's
//   register allocation and bias the control (`g602_prologue_is_set_by_the_coldest_code`).
// ⭐ PROMOTED DEFAULT-ON (G609 slices B+C). Rollback: DC2_G609_NO_SAMP_INLINE=1.
// ⚠️ WHAT THIS FLAG IS WORTH IS SLICE C, NOT SLICE B. Measured on the SAME instrument-matched pair
// before slice C existed, the inlined sampler ALONE moved `disp tri-fast` 534.9 -> 527.6 cyc/inside
// (-1.4%) and `discov tri-fast` 561.6 -> 562.7 (+0.2%) — a null, with `sampInlRows` = 63,800,000 =
// every kernel row proving the arms DID diverge (`g538_null_ab_needs_a_path_counter`). The call
// boundary was never the price. What the boundary bought was the ABILITY to lower the rounding.
// Gates for the promoted pair, route `s03`:
//   mechanism  — `[G605:leaf]`, `DC2_G605_CYC=1` and nothing else in both arms:
//                `disp tri-fast` 534.7 -> **338.3** cyc/inside (**-36.7%**), LOOP 39,718.6 ->
//                24,901.4 ms; `discov tri-fast` 573.4 -> **411.7** (**-28.2%**), LOOP 44,780.2 ->
//                32,159.4 ms. Untouched controls in the same runs: `discov sprite` 60.9 -> 61.2
//                (+0.5%), `disp sprite` 201.0 -> 204.6 (+1.8%), its PRO 94.96 -> 93.80 us/call.
//                27.4 s of replay LOOP wall removed = **-21.6% of the whole CPU band replay**.
//   exactness  — `DC2_G609_VERIFY=1`, the shipped `std::lround` path authoritative:
//                **bad=0 on 681,310,857 per-pixel colour+depth comparisons** / 344.3 M committed px.
//   payoff     — order-balanced A B B A, two estimators agreeing to **0.000 ms/f**:
//                `[G154:perf]` **-1.170 ms/f (-3.23%)** se 0.325 and `[G146:frame]` **-1.170
//                (-3.23%)** se 0.276. FOUR order blocks, all negative (-1.261 / -1.079 x2).
//   distribution — p50 33.38 -> 31.86, p95 54.93 -> 53.62, p99 **87.80 -> 73.47**, max 89.23 ->
//                **74.60**, windows over 66.7 ms **6 -> 2**, over 33.33 ms 86 -> 77.
// `DC2_G609_SAMP_INLINE=1` is retained as the bring-up spelling; it is now a no-op.
inline bool g609SamplerInline()
{
    static const bool on = !envFlagEnabled("DC2_G609_NO_SAMP_INLINE");
    return on;
}

// Any per-pixel diagnostic the tight scanline does not reproduce disarms it entirely. Fail closed.
//
// ⭐ THE LIST IS EXACTLY THE PRE-LEAF ONES. Everything the generic loop does AFTER the G605 arm's
// `continue` (G82 rockavg, G43 sample, F31 tritex, G195 dofpx, G219 skycol/zwr, G365, the G48/G51
// skips, `s_g141SkipWrite`) is already unreachable for an admitted pixel, so excluding it here
// would only shrink the population for no exactness gain. What DOES sit between the Z test and the
// leaf is these seven, and `g605TriSpanQuiet()` already covers DC2_G219_SKYPX / DC2_G232_WATCH /
// DC2_G365_WATCH / the G141 sampler probes / DC2_G364_NO_FOG on top.
//
// ⚠️ EACH TEST MATCHES THE SPELLING AT THE SITE IT GUARDS. `s_g357z`, `s_g357flat`,
// `s_g408IntegerPixelCenter` and `s_g232w` are `getenv(..) != nullptr` (PRESENCE), while the G44 /
// G106 / G195 helpers use `envFlagEnabled` (presence AND not "0"/"false"/"off"). Using the wrong
// one here would let `DC2_G357_ZTRACE=0` arm the diagnostic and disarm the guard at the same time.
//
// ⚠️ DC2_G605_CYC is deliberately NOT excluded — it brackets the whole `drawTriangle` CALL and adds
// the identical clock reads per draw to BOTH arms, so it is this lever's mechanism counter
// (`cyc/inside`), not a perturbation of it (G605's own note, one level down).
inline bool g609TriScanQuiet()
{
    static const bool quiet =
        std::getenv("DC2_G357_ZTRACE") == nullptr &&
        std::getenv("DC2_G357_FLAT") == nullptr &&
        std::getenv("DC2_G408_INTEGER_PIXEL_CENTER") == nullptr &&
        std::getenv("DC2_G232_RTTDUMP") == nullptr &&
        !envFlagEnabled("DC2_TRACE_G44_FACE_Z") &&
        !envFlagEnabled("DC2_G106_TITLE_Z_STAT") &&
        !envFlagEnabled("DC2_G195_FG_TRACE") &&
        // Slice C: the two G529 probes `lerpChannel` reads that the inlined filter does not
        // reproduce. `DC2_G529_NOLERP` is a ceiling probe that renders garbage and `DC2_G529_LERPVFY`
        // runs both rounding forms and counts; when either is armed the kernel steps aside entirely
        // so the generic path keeps honouring them. (`DC2_G529_FASTLERP` needs no exclusion — it
        // selects the SAME rounding this kernel inlines, so the two cannot disagree.)
        !envFlagEnabled("DC2_G529_NOLERP") &&
        !envFlagEnabled("DC2_G529_LERPVFY");
    return quiet;
}

// ---------------------------------------------------------------------------------------------
// `[G609:scan]` — the coverage denominator. ⭐ A CLEAN RESULT NEEDS ONE TOO
// (`g536_coverage_denominator_for_a_defect_rate`), and the stride must be smaller than the
// population it sizes (`g608_census_stride_smaller_than_population`), so the periodic dump is
// driven off a counter that advances on EVERY row, taken or not.
static std::atomic<uint64_t> g_g609Rows{0};      // eligible rows run by the kernel
static std::atomic<uint64_t> g_g609RowsNoBounds{0}; // eligible draws, row declined on VRAM bound
static std::atomic<uint64_t> g_g609Px{0};        // pixels the kernel committed
static std::atomic<uint64_t> g_g609Inside{0};    // pixels that passed coverage inside the kernel
static std::atomic<uint64_t> g_g609Bad{0};       // oracle disagreements (colour or depth)
static std::atomic<uint64_t> g_g609Checks{0};    // oracle comparisons performed
static std::atomic<uint64_t> g_g609Calls{0};     // rows OFFERED to the kernel (the denominator)
// ⭐ THE SPAN-WASTE DENOMINATOR. `walked` is the x range the kernel actually iterates; `inside` is
// what passes the exact float coverage test. Their ratio prices the G141 conservative x-span for a
// population of 7.9 M draws averaging 32 inside pixels each, which no existing census reports:
// `[G605:leaf]` has `bboxPerCall` (the whole bbox) and `insidePerCall`, but nothing in between.
static std::atomic<uint64_t> g_g609Walked{0};
static std::atomic<uint64_t> g_g609Draws{0};     // admitted DRAWS (so rows/draw is readable)
// ⭐ SLICE B'S DIVERGENCE COUNTER. A null A/B needs a counter proving the arms diverged
// (`g538_null_ab_needs_a_path_counter`) — without it "no effect" and "never ran" are the same line.
static std::atomic<uint64_t> g_g609SampInlRows{0};
static std::atomic<uint32_t> g_g609CeilSink{0}; // ⛔ ceiling probe 4's observable sink
// ⭐ The bilinear tap-quad MISS count. 4 wrap+ReadP8+CLUT taps per miss, 0 per hit — so
// misses/inside is the multiplier on whatever one tap costs, and it is the number that decides
// whether the next lever is "make a tap cheaper" or "miss less often".
static std::atomic<uint64_t> g_g609QuadMiss{0};
// ⭐⭐ G628: the UNTEXTURED instantiation's OWN denominators. A pooled row/px/oracle count cannot
// serve as this leaf's divergence proof or as its `bad=0` denominator — the textured population is
// live and identical in BOTH arms of the G628 A/B, so a pooled `bad=0` would be dominated by pixels
// the untextured kernel never touched (`g589_oracle_with_zero_checks`: `bad=0` on the WRONG
// population is the same failure as `bad=0` with `checks=0`).
static std::atomic<uint64_t> g_g609UntexRows{0};
static std::atomic<uint64_t> g_g609UntexInside{0};
static std::atomic<uint64_t> g_g609UntexPx{0};
static std::atomic<uint64_t> g_g609UntexChecks{0};
static std::atomic<uint64_t> g_g609UntexBad{0};

__declspec(noinline) static void g609DumpScanCensus()
{
    const uint64_t rows = g_g609Rows.load(std::memory_order_relaxed);
    const uint64_t draws = g_g609Draws.load(std::memory_order_relaxed);
    const uint64_t walked = g_g609Walked.load(std::memory_order_relaxed);
    const uint64_t inside = g_g609Inside.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G609:scan] rows=%llu offered=%llu noBounds=%llu inside=%llu px=%llu "
                 "walked=%llu draws=%llu rowsPerDraw=%.2f walkedPerRow=%.2f insidePerRow=%.2f "
                 "inside/walked=%.3f quadMiss=%llu missPerInside=%.3f "
                 "sampInlRows=%llu checks=%llu bad=%llu "
                 "untexRows=%llu untexInside=%llu untexPx=%llu untexChecks=%llu untexBad=%llu "
                 "mode=%s\n",
                 (unsigned long long)rows,
                 (unsigned long long)g_g609Calls.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609RowsNoBounds.load(std::memory_order_relaxed),
                 (unsigned long long)inside,
                 (unsigned long long)g_g609Px.load(std::memory_order_relaxed),
                 (unsigned long long)walked,
                 (unsigned long long)draws,
                 draws ? (double)rows / (double)draws : 0.0,
                 rows ? (double)walked / (double)rows : 0.0,
                 rows ? (double)inside / (double)rows : 0.0,
                 walked ? (double)inside / (double)walked : 0.0,
                 (unsigned long long)g_g609QuadMiss.load(std::memory_order_relaxed),
                 inside ? (double)g_g609QuadMiss.load(std::memory_order_relaxed) / (double)inside : 0.0,
                 (unsigned long long)g_g609SampInlRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609Checks.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609Bad.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609UntexRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609UntexInside.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609UntexPx.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609UntexChecks.load(std::memory_order_relaxed),
                 (unsigned long long)g_g609UntexBad.load(std::memory_order_relaxed),
                 g609TriScanVerifyOn() ? "verify" : "live");
}

inline void g609NoteScanDraw()
{
    if (g609TriScanStatOn())
        g_g609Draws.fetch_add(1u, std::memory_order_relaxed);
}

inline void g609NoteScanRow(uint64_t inside, uint64_t px, bool bounded,
                            uint64_t walked = 0u, bool sampInl = false, bool untex = false)
{
    if (!g609TriScanStatOn())
        return;
    const uint64_t n = g_g609Calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (bounded)
        g_g609Rows.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g609RowsNoBounds.fetch_add(1u, std::memory_order_relaxed);
    if (sampInl)
        g_g609SampInlRows.fetch_add(1u, std::memory_order_relaxed);
    if (untex && bounded)
    {
        g_g609UntexRows.fetch_add(1u, std::memory_order_relaxed);
        g_g609UntexInside.fetch_add(inside, std::memory_order_relaxed);
        g_g609UntexPx.fetch_add(px, std::memory_order_relaxed);
    }
    g_g609Inside.fetch_add(inside, std::memory_order_relaxed);
    g_g609Px.fetch_add(px, std::memory_order_relaxed);
    g_g609Walked.fetch_add(walked, std::memory_order_relaxed);
    // ⭐ The stride must be smaller than the population it sizes, and a low-population VERIFY run
    // must still print (`g608_census_stride_smaller_than_population`). Hence both a first-N rule and
    // a periodic one, driven off a counter that advances on EVERY offered row, taken or declined.
    if (n <= 4ull || (n % 200000ull) == 0ull)
        g609DumpScanCensus();
}

// ---------------------------------------------------------------------------------------------
// The per-DRAW constants the tight scanline needs. Built once in `drawTriangle`, immediately after
// the G605 admission, from the SAME locals the generic loop reads.
struct G609TriScan
{
    const GSVertex *v0 = nullptr;
    const GSVertex *v1 = nullptr;
    const GSVertex *v2 = nullptr;
    float fx0 = 0.0f, fy0 = 0.0f, fx1 = 0.0f, fy1 = 0.0f, fx2 = 0.0f, fy2 = 0.0f;
    float winding = 1.0f;
    float invAbsDenom = 0.0f;
    float edgeEpsilon = 1.0e-4f;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
    const GSTex0Reg *tex0 = nullptr;
    G605Blend blend{};
    bool fog = false;
    bool zWrite = false;
    bool inlineCombine = true;
    // Slice B: the two per-DRAW constants the `noinline` samplers re-read from thread_local storage
    // on every pixel. Hoisted here in `drawTriangle`, which runs on the SAME thread as the row loop
    // (G143 row threading requires `!t_g144InReplay` and the G605 admission requires it, so the two
    // are mutually exclusive and the thread_local read is safe to lift).
    const G580SharedSample *sample = nullptr;
    const uint32_t *clut = nullptr;
    // ⛔ CEILING PROBE ONLY (DC2_G609_CEIL). Renders garbage; never promoted, never a correctness
    // switch. It exists because `cyc/inside` prices the WHOLE admitted pixel and nothing inside it,
    // so the next lever cannot be ranked without a stage decomposition. The baseline arm is THE SAME
    // BINARY with ceil=0, so both arms carry these branches and the delta is the stage's own price
    // (`17a` derivative/ceiling-probe contract).
    //   1 = delete the SAMPLER (texel := 0xFFFFFFFF, s_g141SkipSample's own substitute)
    //   2 = 1 + delete MODULATE and the fog lerp
    //   3 = delete everything after the Z test (coverage + Z only)
    //   4 = delete only the COMMIT (the dst load, the blend and the two stores)
    //   5 = delete the four wrap+ReadP8+CLUT TAPS inside the sampler   (needs DC2_G609_SAMP_INLINE)
    //   6 = delete the four bilinear lerpChannel calls                 (needs DC2_G609_SAMP_INLINE)
    int ceil = 0;
    bool stat = false; // census armed: flush the per-row quad-miss counter
};

inline int g609CeilProbe()
{
    static const int level = []() -> int {
        const char *e = std::getenv("DC2_G609_CEIL");
        const int n = e ? std::atoi(e) : 0;
        return (n < 0 || n > 6) ? 0 : n;
    }();
    return level;
}

// The bilinear tap memo, as a LOCAL of the row loop instead of `t_g580Quad`. It is keyed on
// (u0, v0) and reset per row exactly as `g580BeginSharedSampleRow()` resets the thread_local, so it
// is the same memo with the same lifetime — and a memo cannot change a result either way.
struct G609Quad
{
    bool valid = false;
    int u0 = 0, v0 = 0;
    uint32_t c00 = 0u, c10 = 0u, c01 = 0u, c11 = 0u;
};

// ⭐⭐⭐ SLICE C — THE BILINEAR FILTER'S FINAL ROUNDING, WHICH THE CEILING PROBE SAYS IS HALF THE
// PIXEL. `DC2_G609_CEIL=6` (delete the four `lerpChannel` calls) takes `disp tri-fast` from
// **526.7 to 262.9 cyc/inside** on 215.5 M matched pixels, while `DC2_G609_CEIL=5` (delete the four
// wrap+ReadP8+CLUT TAPS) takes it to only 484.3. The memory access is 8.1% of the pixel and the
// FILTER ARITHMETIC is 50.1% — the opposite of what a texture sampler is supposed to look like.
//
// The reason is in `lerpChannel` itself (rasterizer_row_pool.inc:266): it ends in `std::lround(v)`,
// which must round HALF AWAY FROM ZERO — something SSE's `cvtss2si` (round-to-nearest-EVEN) does not
// do — so MSVC cannot fold it and emits a **CRT call**. Four per sampled texel. On top of that the
// function opens with THREE function-local `static const bool` probe reads (`g529NoLerpOn`,
// `g529FastLerpOn`, `g529LerpVfyOn`), each of which MSVC guards with a thread-safe-init check:
// twelve of those per texel, in the hottest leaf in the program.
//
// ⭐ G529 ALREADY PROVED THE REPLACEMENT EXACT AND ALREADY SHIPPED IT — default-OFF, because at the
// time the only available gate was whole-frame and it read 0.00 ms/f
// (`g594_frame_gate_cannot_resolve_sub_ms`). `top` and `bottom` are convex combinations of channel
// values in [0,255] and `v` is a convex combination of those, so **v >= 0**; for v >= 0 "round half
// away from zero" IS "round half up" = floor(v + 1/2), computed without ever forming `v + 0.5f`.
// This is that function, inlined, with the three probe reads gone because `g609TriScanQuiet()`
// already excluded every flag they test.
//
// ⛔ NOT an arithmetic rewrite. `top`, `bottom` and `v` are byte-for-byte `lerpChannel`'s own
// statements; only the rounding CALL is replaced by the identity G529 derived — and the G609 row
// oracle re-proves that identity against the shipped `std::lround` path on the live corpus, which
// is a stronger check than the derivation.
inline uint8_t g609Lerp8(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
{
    const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
    const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
    const float v = top + (bottom - top) * fy;
    const int n = static_cast<int>(v);
    return static_cast<uint8_t>(clampU8((v - static_cast<float>(n) >= 0.5f) ? (n + 1) : n));
}

// `g580SampleSharedT8` (FST=false) and `g608SampleSharedT8Uv` (FST=true), statement for statement,
// with `sample`, `clut` and the quad handed in instead of re-fetched. Nothing else differs.
template <bool FST>
inline uint32_t g609SampleT8(const G580SharedSample &sample, const uint32_t *clut,
                             uint8_t *vram, G609Quad &quad, int ceil, uint32_t &quadMiss,
                             float s, float t, float q, uint16_t u, uint16_t v)
{
    float texUf, texVf;
    if constexpr (FST)
    {
        (void)s; (void)t; (void)q;
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        (void)u; (void)v;
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(sample.texW);
        texVf = t * invQ * static_cast<float>(sample.texH);
    }
    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);
    if (!quad.valid || quad.u0 != u0 || quad.v0 != v0)
    {
        // The MISS counter lives inside the already-expensive path, so it is free where it matters
        // and it is what turns "the sampler is 69% of the pixel" into a rankable statement: 4 taps
        // per miss, 0 per hit.
        ++quadMiss;
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
        quad.valid = true;
        quad.u0 = u0;
        quad.v0 = v0;
        if (ceil == 5) // ⛔ ceiling: the four wrap+ReadP8+CLUT taps deleted
        {
            quad.c00 = quad.c10 = quad.c01 = quad.c11 = 0xFFFFFFFFu;
        }
        else
        {
            quad.c00 = point(u0, v0);
            quad.c10 = point(u0 + 1, v0);
            quad.c01 = point(u0, v0 + 1);
            quad.c11 = point(u0 + 1, v0 + 1);
        }
    }
    const uint32_t c00 = quad.c00, c10 = quad.c10;
    const uint32_t c01 = quad.c01, c11 = quad.c11;
    if (ceil == 6) // ⛔ ceiling: the four bilinear lerpChannel calls deleted
        return c00;
    const uint8_t r = g609Lerp8(static_cast<uint8_t>(c00), static_cast<uint8_t>(c10),
                                static_cast<uint8_t>(c01), static_cast<uint8_t>(c11), fx, fy);
    const uint8_t g = g609Lerp8(static_cast<uint8_t>(c00 >> 8u), static_cast<uint8_t>(c10 >> 8u),
                                static_cast<uint8_t>(c01 >> 8u), static_cast<uint8_t>(c11 >> 8u), fx, fy);
    const uint8_t b = g609Lerp8(static_cast<uint8_t>(c00 >> 16u), static_cast<uint8_t>(c10 >> 16u),
                                static_cast<uint8_t>(c01 >> 16u), static_cast<uint8_t>(c11 >> 16u), fx, fy);
    const uint8_t a = g609Lerp8(static_cast<uint8_t>(c00 >> 24u), static_cast<uint8_t>(c10 >> 24u),
                                static_cast<uint8_t>(c01 >> 24u), static_cast<uint8_t>(c11 >> 24u), fx, fy);
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

// Conservative upper bound on any byte offset the CT32 commit can reach on this row, over
// x in [0, xEnd]. `g530GroupBase` is NOT monotonic in x (the block table permutes within a page),
// so bound each term instead of evaluating the maximum:
//     off = (page << 13) + ((blockId >> 5) << 13) + (blockId & 31) * 256 + ct[x & 7] * 4
//     page    <= pageRow + (xEnd >> 6)
//     blockId  = blockLo + bt[..]  <= 31 + 31 = 62   =>   (blockId >> 5) <= 1
//     (blockId & 31) <= 31,   ct[..] <= 255 (uint8_t)
// If the bound fits in VRAM, the per-pixel `fastOff + 4 <= vramSize` test the generic leaf performs
// is provably true for every pixel of the row and the kernel can drop it. If it does not, the row
// is declined and runs on the untouched generic path — fail closed, never fail silent.
inline bool g609RowWithinVram(const G530RowAddr &row, int xEnd, uint32_t vramSize)
{
    if (xEnd < 0)
        return false;
    const uint32_t pageMax = row.pageRow + (static_cast<uint32_t>(xEnd) >> 6u);
    if (pageMax > (1u << 18u))
        return false; // absurd; decline rather than overflow the bound below
    const uint32_t bound = (pageMax << 13u) + (1u << 13u) + 31u * 256u + 255u * 4u + 4u;
    return bound <= vramSize;
}

// ---------------------------------------------------------------------------------------------
// THE KERNEL. One scanline of an admitted triangle.
//
// Templated on the TEXTURE arm and the DEPTH arm (G628) and on the sampler arm, so every per-draw
// branch is resolved at compile time; the instantiations are otherwise the same text. `noinline`
// keeps it out of `drawTriangle`'s own register allocation and prologue, which is half the point of
// the slice.
//
// ⭐⭐⭐ `ZPRIV == false` IS THE "DEPTH IS PROVABLY DEAD" ARM (G628). The `ridepod` untextured
// population runs `ZTST=ALWAYS` with `ZMSK=1`: the test passes for every pixel whatever the buffer
// holds, and nothing is ever written back. The generic loop still INTERPOLATES z, CLAMPS it, and
// READS the destination (`GSMem::ReadZ24`) before discarding all three — so deleting the stage is
// dead-code elimination licensed by the admission, not a semantic change. `depthRow` is legitimately
// NULL in this instantiation and must never be dereferenced by it.
template <bool TEXTURED, bool ZPRIV, bool FST, bool SAMPINL>
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void g609TriScanRowT(const G609TriScan &S, int y, float py,
                            int xStart, int xEnd,
                            const G530RowAddr &colorRow,
                            uint32_t *depthRow,
                            long &insideAcc, int &drawnAcc, uint64_t &pxAcc)
{
    (void)y;
    const GSVertex &v0 = *S.v0;
    const GSVertex &v1 = *S.v1;
    const GSVertex &v2 = *S.v2;
    const float fx0 = S.fx0, fy0 = S.fy0, fx1 = S.fx1, fy1 = S.fy1, fx2 = S.fx2, fy2 = S.fy2;
    const float winding = S.winding;
    const float invAbsDenom = S.invAbsDenom;
    const float kEdgeEpsilon = S.edgeEpsilon;

    uint32_t colorGroup = ~0u;
    uint32_t colorGroupBase = 0u;
    uint32_t ceilSink = 0u; // ⛔ ceiling probe 4 only
    uint32_t quadMiss = 0u; // slice B only; bilinear tap-quad misses on this row
    G609Quad quad{}; // slice B only; the row memo, reset here exactly as g580BeginSharedSampleRow()

    for (int x = xStart; x <= xEnd; ++x)
    {
        // --- coverage: rasterizer_draw_triangle.inc:2079..2116, verbatim ---
        const float px = static_cast<float>(x) + 0.5f;
        const float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
        const float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
            continue;
        ++insideAcc;

        // --- depth: :2127..2190, with zEnabled / z24 / ztst==2 / g403DisplayZ resolved ---
        uint32_t zi = 0u;
        if constexpr (ZPRIV)
        {
            float zf = v0.z * w0 + v1.z * w1 + v2.z * w2;
            if (zf < 0.0f) zf = 0.0f;
            const float zmax = 16777215.0f; // z24 by admission
            if (zf > zmax) zf = zmax;
            zi = static_cast<uint32_t>(zf);
            const uint32_t zdst = depthRow[x];
            if (!(zi >= zdst)) // ztst == 2 (GEQUAL) by admission
                continue;
        }
        if (S.ceil == 3) // ⛔ ceiling: coverage + Z only
        {
            ++pxAcc;
            ++drawnAcc;
            continue;
        }

        // --- the G605/G608 exact leaf: :2332..2426, verbatim ---
        const uint8_t shadeR = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
        const uint8_t shadeG = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
        const uint8_t shadeB = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
        const uint8_t shadeA = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));

        uint8_t fr, fg, fb, fa;
        if constexpr (!TEXTURED)
        {
            // ⭐⭐⭐ G628 — THE `tme=0` SUBSET. The generic loop's untextured arm computes exactly
            // these four values (`rasterizer_draw_triangle.inc:2652..2666`, the `gs->m_prim.iip`
            // branch, which the admission pins) and then SKIPS the whole `if (gs->m_prim.tme)`
            // block, so `combineTexture` is never called on the generic path either. Nothing is
            // rewritten here; the sampler, the CLUT, the STQ/UV interpolation and the combiner are
            // simply ABSENT FROM THIS INSTANTIATION.
            (void)quad; (void)quadMiss;
            fr = shadeR; fg = shadeG; fb = shadeB; fa = shadeA;
        }
        else
        {
        uint32_t texel = 0xFFFFFFFFu; // ⛔ ceil 1/2's substitute (s_g141SkipSample's own)
        if (S.ceil != 1 && S.ceil != 2)
        {
            if constexpr (FST)
            {
                const uint16_t iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                const uint16_t iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                if constexpr (SAMPINL)
                    texel = g609SampleT8<true>(*S.sample, S.clut, S.vram, quad, S.ceil, quadMiss,
                                               0.0f, 0.0f, 1.0f, iu, iv);
                else
                    texel = g608SampleSharedT8Uv(S.vram, iu, iv);
            }
            else
            {
                const float is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                const float it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                const float iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                if constexpr (SAMPINL)
                    texel = g609SampleT8<false>(*S.sample, S.clut, S.vram, quad, S.ceil, quadMiss,
                                                is, it, iq, 0u, 0u);
                else
                    texel = g580SampleSharedT8(S.vram, is, it, iq);
            }
        }
        const uint8_t tr = static_cast<uint8_t>(texel & 0xffu);
        const uint8_t tg = static_cast<uint8_t>((texel >> 8u) & 0xffu);
        const uint8_t tb = static_cast<uint8_t>((texel >> 16u) & 0xffu);
        const uint8_t ta = static_cast<uint8_t>((texel >> 24u) & 0xffu);

        if (S.ceil == 2) // ⛔ ceiling: MODULATE and the fog lerp deleted
        {
            fr = shadeR; fg = shadeG; fb = shadeB; fa = shadeA;
        }
        else if (S.inlineCombine)
        {
            // combineTexture with tfx == 0 (MODULATE) and tcc == 1, both pinned by the admission.
            // rasterizer_row_pool.inc:84..87, statement for statement.
            fr = static_cast<uint8_t>(clampU8((tr * shadeR) >> 7));
            fg = static_cast<uint8_t>(clampU8((tg * shadeG) >> 7));
            fb = static_cast<uint8_t>(clampU8((tb * shadeB) >> 7));
            fa = static_cast<uint8_t>(clampU8((ta * shadeA) >> 7));
        }
        else
        {
            const TextureCombineResult color =
                combineTexture(*S.tex0, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);
            fr = color.r; fg = color.g; fb = color.b; fa = color.a;
        }
        }
        if (S.fog && S.ceil != 2)
        {
            const float ff = static_cast<float>(v0.fog) * w0 +
                             static_cast<float>(v1.fog) * w1 +
                             static_cast<float>(v2.fog) * w2;
            g364ApplyFog(static_cast<uint32_t>(clampU8(static_cast<int>(ff + 0.5f))), fr, fg, fb);
        }

        if (S.ceil != 4) // ⛔ ceiling 4: the dst load, the blend and the two stores deleted
        {
            const uint32_t group = static_cast<uint32_t>(x) >> 3u;
            if (group != colorGroup)
            {
                colorGroup = group;
                colorGroupBase = g530GroupBase(colorRow, static_cast<uint32_t>(x));
            }
            const uint32_t fastOff =
                colorGroupBase + static_cast<uint32_t>(colorRow.ct[x & 7]) * 4u;
            // `g609RowWithinVram` proved `fastOff + 4 <= vramSize` for every x on this row.
            uint32_t dstWord = 0u;
            std::memcpy(&dstWord, S.vram + fastOff, sizeof(dstWord));
            const uint32_t fastPixel = g605CommitWord(S.blend, dstWord, fr, fg, fb, fa);
            std::memcpy(S.vram + fastOff, &fastPixel, sizeof(fastPixel));
            // `S.zWrite` is false by admission whenever ZPRIV is false (ZMSK=1), but the guard is
            // structural rather than argued: the null `depthRow` must be unreachable from here.
            if constexpr (ZPRIV)
            {
                if (S.zWrite)
                    depthRow[x] = zi;
            }
        }
        else
        {
            // Keep the computed colour observable so the whole chain above is not dead-coded away
            // — a ceiling probe that deletes the work it is trying to price measures nothing
            // (`17e`). One local add per pixel, one atomic store per ROW.
            ceilSink += static_cast<uint32_t>(fr) + fg + fb + fa + zi;
        }
        ++pxAcc;
        ++drawnAcc;
    }
    if (S.ceil == 4)
        g_g609CeilSink.store(ceilSink, std::memory_order_relaxed);
    if (S.stat)
        g_g609QuadMiss.fetch_add(quadMiss, std::memory_order_relaxed);
}

inline void g609TriScanRow(const G609TriScan &S, bool textured, bool zpriv, bool fst, bool sampInl,
                           int y, float py,
                           int xStart, int xEnd,
                           const G530RowAddr &colorRow, uint32_t *depthRow,
                           long &insideAcc, int &drawnAcc, uint64_t &pxAcc)
{
    // Six instantiations: TWO untextured (G628 — one per depth arm; `fst` and the slice-B arm are
    // both meaningless without a sampler, so there is exactly one body each) and four textured, one
    // per (sampler arm x slice-B arm), all of which the textured admission pins to `ZPRIV=true`.
    // Every selector is a per-DRAW constant, so this dispatch is once per row, and no instantiation
    // carries another's body — which is what makes each arm a clean control.
    if (!textured)
    {
        if (zpriv)
            g609TriScanRowT<false, true, false, false>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                       insideAcc, drawnAcc, pxAcc);
        else
            g609TriScanRowT<false, false, false, false>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                        insideAcc, drawnAcc, pxAcc);
        return;
    }
    if (fst)
    {
        if (sampInl)
            g609TriScanRowT<true, true, true, true>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                    insideAcc, drawnAcc, pxAcc);
        else
            g609TriScanRowT<true, true, true, false>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                     insideAcc, drawnAcc, pxAcc);
    }
    else
    {
        if (sampInl)
            g609TriScanRowT<true, true, false, true>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                     insideAcc, drawnAcc, pxAcc);
        else
            g609TriScanRowT<true, true, false, false>(S, y, py, xStart, xEnd, colorRow, depthRow,
                                                      insideAcc, drawnAcc, pxAcc);
    }
}

// ---------------------------------------------------------------------------------------------
// THE ORACLE. A row-sized snapshot of everything the row can write. Thread-local because a G144
// band replay runs eight of these concurrently, and `kG403DisplayZW` bounds the span by admission.
struct G609RowShadow
{
    static constexpr int kMax = 2048;
    int n = 0;
    int x0 = 0;
    uint32_t off[kMax]{};
    uint32_t savedColor[kMax]{};
    uint32_t savedDepth[kMax]{};
    uint32_t refColor[kMax]{};
    uint32_t refDepth[kMax]{};
};

inline G609RowShadow &g609Shadow()
{
    static thread_local G609RowShadow s;
    return s;
}

// Step 1 — snapshot the row's destination words and depth entries.
inline bool g609VerifySave(const G609TriScan &S, int xStart, int xEnd,
                           const G530RowAddr &colorRow, const uint32_t *depthRow)
{
    G609RowShadow &sh = g609Shadow();
    const int n = xEnd - xStart + 1;
    if (n <= 0 || n > G609RowShadow::kMax)
        return false;
    sh.n = n;
    sh.x0 = xStart;
    uint32_t group = ~0u;
    uint32_t base = 0u;
    for (int i = 0; i < n; ++i)
    {
        const int x = xStart + i;
        const uint32_t g = static_cast<uint32_t>(x) >> 3u;
        if (g != group)
        {
            group = g;
            base = g530GroupBase(colorRow, static_cast<uint32_t>(x));
        }
        const uint32_t off = base + static_cast<uint32_t>(colorRow.ct[x & 7]) * 4u;
        sh.off[i] = off;
        uint32_t w = 0u;
        if (off + 4u <= S.vramSize)
            std::memcpy(&w, S.vram + off, sizeof(w));
        sh.savedColor[i] = w;
        // G628: the "depth is provably dead" arm has NO depth row (ZTST=ALWAYS + ZMSK=1), and
        // neither the generic path nor the kernel writes one — so there is nothing to snapshot and
        // nothing to compare. The COLOUR half of the oracle is unaffected and still runs on every
        // pixel of the row.
        sh.savedDepth[i] = depthRow ? depthRow[x] : 0u;
    }
    return true;
}

// Step 3 — capture what the SHIPPED generic loop produced, then put the row back as it was.
inline void g609VerifyCaptureAndRestore(const G609TriScan &S, uint32_t *depthRow)
{
    G609RowShadow &sh = g609Shadow();
    for (int i = 0; i < sh.n; ++i)
    {
        const int x = sh.x0 + i;
        uint32_t w = 0u;
        if (sh.off[i] + 4u <= S.vramSize)
            std::memcpy(&w, S.vram + sh.off[i], sizeof(w));
        sh.refColor[i] = w;
        sh.refDepth[i] = depthRow ? depthRow[x] : 0u;
        if (sh.off[i] + 4u <= S.vramSize)
            std::memcpy(S.vram + sh.off[i], &sh.savedColor[i], sizeof(uint32_t));
        if (depthRow)
            depthRow[x] = sh.savedDepth[i];
    }
}

// Step 5 — compare, then hand the row back to the AUTHORITATIVE generic result either way, so a
// kernel defect can never reach a pixel while the oracle is armed.
inline void g609VerifyCompare(const G609TriScan &S, uint32_t *depthRow, bool untex = false)
{
    G609RowShadow &sh = g609Shadow();
    uint64_t bad = 0u;
    for (int i = 0; i < sh.n; ++i)
    {
        const int x = sh.x0 + i;
        uint32_t w = 0u;
        if (sh.off[i] + 4u <= S.vramSize)
            std::memcpy(&w, S.vram + sh.off[i], sizeof(w));
        if (w != sh.refColor[i] || (depthRow && depthRow[x] != sh.refDepth[i]))
            ++bad;
        if (sh.off[i] + 4u <= S.vramSize)
            std::memcpy(S.vram + sh.off[i], &sh.refColor[i], sizeof(uint32_t));
        if (depthRow)
            depthRow[x] = sh.refDepth[i];
    }
    g_g609Checks.fetch_add(static_cast<uint64_t>(sh.n), std::memory_order_relaxed);
    // G628: the untextured leaf carries its OWN oracle denominator — see g_g609UntexChecks.
    if (untex)
    {
        g_g609UntexChecks.fetch_add(static_cast<uint64_t>(sh.n), std::memory_order_relaxed);
        if (bad != 0u)
            g_g609UntexBad.fetch_add(bad, std::memory_order_relaxed);
    }
    if (bad != 0u)
    {
        const uint64_t total = g_g609Bad.fetch_add(bad, std::memory_order_relaxed) + bad;
        static std::atomic<uint32_t> s_logs{0};
        if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr, "[G609:verify] MISMATCH row x0=%d n=%d bad=%llu total=%llu untex=%u\n",
                         sh.x0, sh.n, (unsigned long long)bad, (unsigned long long)total,
                         untex ? 1u : 0u);
    }
}

} // namespace
