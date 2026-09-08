// ==============================================================================================
// G711 - VU1 COMPILED-BACKEND CODEGEN CEILING PROBE
// ==============================================================================================
//
// ⛔⛔ THIS FILE IS UNREFERENCED. It is #included by nothing, has no CMake option and no entry in
// `run_g477_perf.ps1`'s $clear list. It was removed from the build under Rule 31 once the direction
// closed, exactly so the shipping source reproduces the deliverable byte for byte and no inert
// diagnostic sits in a hot TU (the same defect this phase's P0 had to repair from G710).
//
// It is RETAINED, unwired, because the measurement it produced is the only ship-shape price this
// project has for VU1 codegen quality, and a future phase that finds a multi-millisecond GS lever
// will want to re-run it rather than re-derive it. THE RESULT (plans/phase-G711-fix-log.md §12.3):
//
//     G430 history stores          0.5250 ns/pair   1.518 ms/f of VU1 busy   (7.6%)
//     VF operand load + dest store 0.1625 ns/pair   0.470 ms/f               (2.4%)
//     ACC load + store             0.0750 ns/pair   0.217 ms/f               (1.1%)
//     ALL per-pair memory traffic  0.8125 ns/pair   2.349 ms/f               (11.7%)
//
// ⇒ Real, and it converts ZERO frame time: VU1 exceeds every other pole on exactly one of the 23
// canonical windows and that window is at the 60 FPS cap. Closed on CONVERSION, not on difficulty.
//
// TO RE-WIRE: restore `option(PS2X_G711_DIAG ...)` + the scoped COMPILE_DEFINITIONS block in
// ps2xRuntime/CMakeLists.txt, the #include in ps2_vu1.cpp (BEFORE vu1_g610_native_jit.inc), the
// three ballast emission sites in g610EmitUpper (the hist loop before the real push; the
// load+storeDest loop after it; `g711BalReport` beside the prologue's ring-cursor load), and the
// two $clear entries. `tools/g711_vu1_ladder.ps1` drives it unchanged.
//
// THE QUESTION. G664 closed the VU1 arc by measuring ADMISSION: 87.94% of pairs already execute as
// compiled x86-64 and the backends refuse nothing. It never measured CODEGEN QUALITY. The emitted
// code round-trips every operand through the VU1State image:
//
//     movups xmm0,[rcx+VF+16*fs]        ; vs           -- every pair
//     movups xmm1,[rcx+VF+16*ft]        ; vt           -- every pair with src==3/0
//     movups xmm2,[rcx+ACC]             ; acc          -- every FMAC (op 2/3/7/8)
//     ... 8 instructions, 4 of them STORES, of G430 history ...   -- every FMAC
//     movups [rcx+VF+16*fd] , xmm2      ; dest         -- every FMAC
//
// `[G483:runprof] loopNsPerPair` reads 6.7 ns/pair on `dragon` = ~21 cycles at 3.2 GHz, which is
// 4x what packed SSE should cost. The hypothesis is that a compiled pair is STORE-PORT and
// STORE-TO-LOAD-FORWARD bound, not FP bound, and that a register-residency rewrite of the emitter
// (VF/ACC held in XMM across a block, dead history elided) would remove most of it.
//
// ⛔⛔⛔ WHY THIS IS BALLAST AND NOT AMPUTATION. The first version of this probe OMITTED the memory
// operations, so the emitted stream had the shape a residency rewrite would produce. It was
// INADMISSIBLE on `dragon` and the failure is instructive:
//
//     arm    mask   loopNsPerPair   d ns/pair      %      pairs/call
//     d1/d2  --     7.000 / 6.400   (drift 0.600)          1,307
//     hist   0x01           6.100       -0.600   -8.96     1,360
//     acc    0x06           5.500       -1.200  -17.91     1,280
//     vf     0x18          10.000       +3.300  +49.25     1,200
//     all    0x1f           9.500       +2.800  +41.79     1,201
//
//   1. control drift 0.600 ns/pair == the size of the `hist` effect;
//   2. `pairs/call` moved 1,200..1,360 (13%) - G612's own gate held it at 1359 vs 1360, and a
//      per-pair RATE over a changed population is not comparable;
//   3. two arms got 42-49% SLOWER while REMOVING memory operations, which is impossible at constant
//      workload and is therefore proof the workload changed - stale operands make the microprogram
//      branch differently and execute a more expensive pair mix.
//
// ⇒ **There is no value-preserving amputation available in this emitter.** Every per-pair memory
// operation carries a value something eventually branches on - including the G430 history, whose
// consumers are flag readers OUTSIDE the compiled body (`hist` still moved `pairs/call` by 4%).
// That is Rule 88 one level deeper than it is usually applied: frame time was correctly ruled out
// as a gate, and `loopNsPerPair` turned out to be just as workload-sensitive.
//
// ⭐ BALLAST is Rule 88's prescribed instrument and it is exact here. Each armed class re-executes
// its own memory traffic N extra times with IDENTICAL values at IDENTICAL addresses:
//
//     HIST  : re-store vec/stamp/dest/kind into the SAME ring slot (the cursor is not advanced),
//             so the ring's final content is bit-identical.
//     ACC   : re-load ACC into a dead scratch register, and re-apply the SAME `storeDest` blend
//             of the SAME value to ACC - idempotent by construction, partial dest included.
//     VF    : re-load VF[fs] into a dead scratch register, and re-apply the SAME `storeDest` blend
//             of the SAME value to VF[fd].
//
// The ballast is emitted at the END of g610EmitUpper, where X3/X4/X5 are already dead (the real
// storeDest has just used X4/X5 as its blend temporaries). Nothing it writes differs by one bit
// from what the arm-free path wrote, so `pairs/call`, `kicks/f` and the rendered image are
// invariant BY CONSTRUCTION - the property the amputation could not have.
//
// ⚠️ WHAT BALLAST UNDER-STATES. It prices the WARM cost of the traffic. Extra INDEPENDENT stores do
// not lengthen the ACC dependency chain, so the store-to-load-forward latency a resident ACC would
// remove is NOT in this number. The result is therefore a LOWER BOUND on the residency prize.
//
// ⭐ THE GATE IS `[G483:runprof] loopNsPerPair` AT MATCHED `pairs/call` - never frame time. That is
// the VU1 pair loop's own rate from three clock reads per KICK, and it is exactly the metric G610
// and G612 were gated on ("loopNsPerPair 11.150 -> 8.614 (-22.7%), pairs/call 1359 vs 1360").
// Convert with:  dVU1busy = dLoopNsPerPair * pairs/f  /  N.
//
// RULE 13. The mask and count are read ONCE, at process scope, and consumed at BLOCK-COMPILE time -
// never inside an emitted body and never per executed pair. In a non-DIAG build both are
// `constexpr 0`, so the compiler folds every use out and `ps2_vu1.obj` is unperturbed
// (Rule 12c / Rule 31).
//
// SCOPE. G612 emits G610's `g610EmitUpper` VERBATIM per basic block, so one change covers native
// REGIONS (L1, 60.29% of pairs) and native BLOCKS (L2, 27.65%) together - 87.94% of all pairs.
//
//   DC2_G711_BAL=<n>            extra repetitions (use 4-8 so a 0.5 ns/pair term clears the drift)
//   DC2_G711_BAL_MASK=<bits>    0x1 = G430 history stores | 0x2 = ACC load+store | 0x4 = VF load+store
//
// ==============================================================================================

#if defined(PS2X_G711_DIAG)

static uint32_t g711EnvU32(const char *name)
{
    const char *s = std::getenv(name);
    if (s == nullptr || *s == '\0')
        return 0u;
    return (uint32_t)std::strtoul(s, nullptr, 0);
}

// Process-scope, read once. Not in any hot path: every caller is the JIT translator.
static uint32_t g711BalN()
{
    static const uint32_t s_n = g711EnvU32("DC2_G711_BAL");
    return s_n;
}

static uint32_t g711BalMask()
{
    static const uint32_t s_m = []() {
        const uint32_t m = g711EnvU32("DC2_G711_BAL_MASK");
        return m == 0u ? 0x7u : m; // a count with no mask means "all three classes"
    }();
    return s_m;
}

static bool g711BalHist() { return g711BalN() != 0u && (g711BalMask() & 0x1u) != 0u; }
static bool g711BalAcc()  { return g711BalN() != 0u && (g711BalMask() & 0x2u) != 0u; }
static bool g711BalVf()   { return g711BalN() != 0u && (g711BalMask() & 0x4u) != 0u; }

// One line at translator bring-up so an armed run PROVES the arm reached the emitter. A mask that
// never reaches a compile is otherwise indistinguishable from a clean control (Rule 69/90).
static void g711BalReport(unsigned long long compiledBlocks)
{
    static bool s_done = false;
    if (s_done || g711BalN() == 0u)
        return;
    s_done = true;
    std::fprintf(stderr,
                 "[G711:bal] ARMED n=%u mask=0x%x hist=%d acc=%d vf=%d firstCompiledBlock=%llu\n",
                 g711BalN(), g711BalMask(), g711BalHist() ? 1 : 0, g711BalAcc() ? 1 : 0,
                 g711BalVf() ? 1 : 0, compiledBlocks);
}

#else // !PS2X_G711_DIAG - inert, and constexpr so every use site folds away

static constexpr uint32_t g711BalN()    { return 0u; }
static constexpr bool     g711BalHist() { return false; }
static constexpr bool     g711BalAcc()  { return false; }
static constexpr bool     g711BalVf()   { return false; }
static inline void        g711BalReport(unsigned long long) {}

#endif // PS2X_G711_DIAG
