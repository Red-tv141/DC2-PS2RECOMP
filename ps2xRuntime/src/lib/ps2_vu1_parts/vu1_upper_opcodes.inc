void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    static const bool s_g141PerfOn = (std::getenv("DC2_PERF") != nullptr);
    static double s_g141Sum = 0.0;
    static uint32_t s_g141Win = 0u;
    static uint64_t s_g141CycleSum = 0ull;
    G141PerfScope g141Scope(s_g141PerfOn, &s_g141Sum, &s_g141Win, "vu1.run", 2000u);
    g141Scope.accumNs = &g_g141Vu1RunNs;

    // G483: run() entry stamp for the prologue/loop/epilogue split (DC2_G483_RUNPROF=1).
    const uint64_t g483T0 = dc2_g483_run_prof_on()
        ? (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count()
        : 0ull;

    // G425: VU1-worker internal attribution (default OFF). One clock read per run() call.
    const bool g425Prof = g425ProfileOn();
    const uint64_t g425RunT0 = g425Prof ? g425NowNs() : 0ull;

    // G437 premise probe (default OFF, DC2_G437_ALIGN=1): report the runtime alignment of the
    // VU1 register file. Every fast-path upper does `_mm_loadu_ps(st.vf[fs])` /
    // `_mm_storeu_ps(st.vf[fd])` / `_mm_loadu_ps(st.acc)`; if the base is not 16-byte aligned then
    // every one of those 16-byte accesses is unaligned, one in four splits a 64-byte cache line,
    // and a split store NEVER forwards to a dependent load (~15 cycle penalty instead of ~5).
    // That is a per-pair LATENCY cost, which is exactly the class G436 left open.
    {
        static const bool s_g437Align = (std::getenv("DC2_G437_ALIGN") != nullptr);
        if (s_g437Align)
        {
            static std::atomic<uint32_t> s_g437Once{0u};
            if (s_g437Once.fetch_add(1u, std::memory_order_relaxed) == 0u)
            {
                std::fprintf(stderr,
                             "[G437:align] state=%llu vf=%llu acc=%llu alignof(VU1State)=%zu\n",
                             (unsigned long long)(reinterpret_cast<uintptr_t>(&m_state) & 63u),
                             (unsigned long long)(reinterpret_cast<uintptr_t>(m_state.vf) & 63u),
                             (unsigned long long)(reinterpret_cast<uintptr_t>(m_state.acc) & 63u),
                             alignof(VU1State));
                std::fflush(stderr);
            }
        }
    }

    // G438 alignment sweep (default OFF, DC2_G438_ALIGN=1). G437 found the VU1 register file at
    // `base % 64 == 52` and one `alignas(64)` was worth 7% of the frame, so every OTHER object the
    // hot loop touches with a 16-byte access has to be printed rather than assumed. A 16-byte
    // access at a 16-byte-aligned address can never straddle a 64-byte line, so the pass criterion
    // is `& 15 == 0`; the `& 63` column additionally shows whether a multi-line block starts on a
    // line boundary. One-shot, first run() only, zero cost when the flag is absent.
    {
        static const bool s_g438Align = (std::getenv("DC2_G438_ALIGN") != nullptr);
        if (s_g438Align)
        {
            static std::atomic<uint32_t> s_g438Once{0u};
            if (s_g438Once.fetch_add(1u, std::memory_order_relaxed) == 0u)
            {
                const auto m64 = [](const void *p) -> unsigned long long
                { return (unsigned long long)(reinterpret_cast<uintptr_t>(p) & 63u); };
                std::fprintf(stderr,
                             "[G438:align] vu1state=%llu/%zu vf=%llu acc=%llu "
                             "g430hb=%llu/%zu g430vec=%llu g428pend=%llu g428pre=%llu "
                             "vucode=%llu vudata=%llu sizeof(VU1State)=%zu\n",
                             m64(&m_state), alignof(VU1State), m64(m_state.vf), m64(m_state.acc),
                             m64(&s_g430HB), alignof(G430HistBlock), m64(s_g430HB.vec),
                             m64(s_g428PendVec), m64(s_g428PreVec),
                             m64(vuCode), m64(vuData), sizeof(VU1State));
                std::fflush(stderr);
            }
        }
    }

    const uint32_t startPc = m_state.pc;
    uint32_t cycle = 0u;
    g141Scope.unitPtr = &cycle;
    g141Scope.unitSum = &s_g141CycleSum;
    g141Scope.unitLabel = "cycles";
    uint32_t lastPc = m_state.pc;
    uint32_t lastLower = 0u;
    uint32_t lastUpper = 0u;
    const char *stopReason = "maxcycles";
    // G442: one loop-carried local instead of two — see the fold at the bottom of the pair loop.
    // 0xFFFFFFFF cannot be a branch target (every one is `& 0x3FFF`), so it is a safe "empty".
#define DC2_G442_NO_BRANCH 0xFFFFFFFFu
    uint32_t pendingBranchTarget = DC2_G442_NO_BRANCH;

    // G100: native title-rock forced draw (default-ON, kill DC2_G100_NO_FORCEDRAW). The
    // transform packers' +2048/ADC cull gate (IBEQ VI10,VI1 @0x1d48/0x2128) is structurally
    // never taken (VI10=208, VI1 in {0,1}) so 100% of the title rock verts get +2048 -> ADC=1
    // -> no-draw -> flat blue (G70-G74). Forcing VI1:=VI10 at the gate makes the branch taken
    // (skip +2048 = draw) so the native transform path renders the cavern/walls/water that the
    // retired G78/G80 copy band-aid could not. Title-scoped via g_dc2TitleRockScope (constant
    // for the duration of one VU program) so costume/dungeon are untouched; the off-screen
    // strip-restart that HW's ADC performs is approximated by the G89 rasterizer guard band.
    // Legacy diagnostic DC2_G72_FORCEDRAW still forces regardless of scope.
    // G138: RETIRED BY DEFAULT. The gate is no longer "structurally never taken" — that was a
    // double defect in the RUNNER, fixed at the root: (1) the lower-opcode table had FMEQ/FMAND
    // swapped (0x18/0x1A) so the FMAND guard-mask cascade ran as FMEQ 0/1 compares, and (2) MAC
    // flags were read un-pipelined so each FMAND saw the wrong FMAC's flags. With both fixed the
    // natural per-vertex ADC is bit-exact vs the HW reference (G138 join), and forcing the gate
    // OVER-draws (it overrides the faithful cull and double-draws the 0x1f68 3-vert loop).
    // Re-enable the old band-aid with DC2_G100_FORCE_DRAW=1 (plus DC2_VU1_NO_FMSWAPFIX=1 +
    // DC2_VU1_NO_MACPIPE=1 to reproduce the full pre-G138 render).
    static const bool s_g100native = (std::getenv("DC2_G100_FORCE_DRAW") != nullptr &&
                                      std::getenv("DC2_G100_NO_FORCEDRAW") == nullptr);
    static const bool s_g72force   = (std::getenv("DC2_G72_FORCEDRAW") != nullptr);
    // G101 A/B: disable the forced draw while KEEPING the rest of the G100 path (copy-off) so the
    // gate's NATURAL per-vertex cull (now that G71 computes the MAC flags VI7 feeds, and G99 fixed
    // the camera) can be measured against HW's selective ADC. Diagnostic only.
    static const bool s_gateNatural = (std::getenv("DC2_G101_GATE_NATURAL") != nullptr);
    const bool g100ForceTitleDraw =
        startPc == 0x10u &&
        (s_g72force ||
         (s_g100native && !s_gateNatural && g_dc2TitleRockScope.load(std::memory_order_relaxed)));

    // G102 attempted to patch individual behind-camera strip vertices in VU space. That cannot
    // represent true clipping for a shared tristrip vertex (each adjacent triangle needs its own
    // edge intersection), and its fallback anchor can connect unrelated batches. G104 supersedes
    // it with title-only triangle clipping in the GS rasterizer, so the VU-side vertex rewrite is
    // now opt-in for A/B only.
    static const bool s_g102clip =
        (std::getenv("DC2_G102_ENABLE_VU_CLIP") != nullptr &&
         std::getenv("DC2_G102_NO_CLIP") == nullptr);
    // G104 default path: let every title-rock vertex reach the GS unchanged and clip assembled
    // triangles there, where all three vertices are known. Kill with DC2_G104_NO_TRI_CLIP.
    static const bool s_g104triClip = (std::getenv("DC2_G104_NO_TRI_CLIP") == nullptr);
    static const float s_g102wnear = []() -> float {
        const char *e = std::getenv("DC2_G102_WNEAR");
        const float v = e ? static_cast<float>(std::atof(e)) : 4.0f;
        return (v > 0.0f) ? v : 4.0f;
    }();
    if (g100ForceTitleDraw)
    {
        // Reset the immediate-predecessor adjacency at the start of each title VU run/strip.
        // g_g102LastFront is deliberately NOT reset: it is only written by in-front title
        // vertices and only read in title scope, so persisting it across batches gives a valid
        // clip anchor to batches that OPEN behind the camera (otherwise those verts have no
        // anchor and leave holes). The near-static title camera makes a prior-batch front
        // vertex a good anchor.
        g_g102PrevValid = false;
    }

    // G141 perf: hoist the per-cycle correctness-machinery flags and thread_local base
    // pointers OUT of the hot loop. g87/g138/g139 enabled() are process-constant magic-static
    // bools (a guard check per call) and the pipeline state is thread_local (a TLS lookup per
    // access, ~8x/cycle for the MAC shift register). Both are invariant for the whole run on a
    // single thread, so resolving them ONCE here removes ~50% of VU1 time (measured: MAC-pipe
    // machinery ~108ns of ~218ns/cycle). Behavior-identical — same storage, same values; golden
    // title smoke must stay 211646. TLS addresses are stable for the thread's lifetime, and run()
    // is re-entered per MSCAL so each call re-caches its own thread's base.
    const bool s_qlatOn        = g87_q_latency_enabled();
    const bool s_ppipeOn       = g239_p_pipeline_enabled();
    const bool s_scalarPrestallOn = g239_scalar_prestall_enabled();
    const bool s_macpipeOn     = g138_macpipe_enabled();
    const int  s_macpipeDepth  = s_macpipeOn ? g138_macpipe_depth() : 4;
    const bool s_pairhazOn     = g139_pairhaz_enabled();
    // G328/G413: the G138 implementation shifted DEPTH-1 MAC words plus DEPTH-1 STATUS words
    // on every executed VU pair. A circular physical layout is the same delay line with no
    // bulk copies. G413 promotes it together with G330's fused classifier; either the master
    // rollback or this component rollback restores the legacy shift register. VERIFY keeps a
    // shadow copy and aborts on the first visible/logical mismatch.
    static const bool s_g328RingOn =
        (std::getenv("DC2_G413_NO_DUAL_POLE") == nullptr) &&
        (std::getenv("DC2_G328_NO_MAC_RING") == nullptr);
    static const bool s_g328Verify = (std::getenv("DC2_G328_VERIFY") != nullptr);
    static const bool s_g328Stat = (std::getenv("DC2_G328_STAT") != nullptr);
    static std::atomic<uint64_t> s_g328Cycles{0u};
    static std::atomic<uint64_t> s_g328Bad{0u};
    (void)s_qlatOn;

    // G295: the per-cycle diagnostic probe pile (G68..G237 census/trace blocks accumulated over
    // 200+ phases) costs real EE-thread time even with every probe disabled — ~20 gated branches
    // + pc/startPc compares per cycle in the hottest loop of the frame (VU1proper = 72-74% of
    // steady MAP-0). One master gate, computed once per run, skips all four probe regions when no
    // per-cycle diagnostic can fire this run; any probe env (or the G100 title force path / G216
    // char census / G106 parallel-flags lever / VU1 trace) routes through the legacy interleaved
    // body unchanged. The semantic core (fetch, LOI/E-bit, Q/P pipes, scalar prestall, MAC pipe,
    // G139 pair hazard, exec, branch delay) is IDENTICAL on both paths.
    // Kill: DC2_G295_VU1_DIAG_LOOP=1 forces the legacy body unconditionally.
    // NOTE: any future per-cycle probe added to run() must add its env name to this list.
    static const bool s_g295ForceDiagLoop = (std::getenv("DC2_G295_VU1_DIAG_LOOP") != nullptr);
    static const bool s_g295AnyProbeEnv = [] {
        static const char *const k[] = {
            "DC2_G75_ROUTE1B68", "DC2_G85_NL", "DC2_G87_PL", "DC2_G68_ADC", "DC2_G69_ADC",
            "DC2_G117_TS", "DC2_G132_COPYW", "DC2_G237_TRACE", "DC2_G70", "DC2_G198_GATE_TRACE",
            "DC2_G200_DET_TRACE", "DC2_G72_VI1", "DC2_G71_PROBE", "DC2_G72_DISP",
            "DC2_G140_CLIP", "DC2_G101_VFPROBE", "DC2_G102_STAT", "DC2_G108_2128_QCLIP",
            "DC2_G108_2128_Q_STAT", "DC2_G106_FORCE_2128_MAC"};
        for (const char *n : k)
            if (std::getenv(n) != nullptr)
                return true;
        return false;
    }();
    // G295: skip the G139 pair-hazard save/restore/overlay when the lower op names the upper's
    // dest VF through NEITHER of its register fields. Every lower-slot VF access (read: SQ*,
    // DIV/SQRT/RSQRT, MTIR, RINIT, RXOR, EFU ops — via fs/ftf; write: LQ*, MFIR, MFP, RGET,
    // RNEXT — via ft) encodes the VF index in bits 11-15 (fs) or 16-20 (ft), so an index
    // mismatch on both fields proves the save/restore round-trip is the identity. A VI-indexed
    // false positive merely keeps the legacy swap. Kill: DC2_G295_NO_HAZSKIP=1.
    static const bool s_g295KeepHaz = (std::getenv("DC2_G295_NO_HAZSKIP") != nullptr);
    const bool g295Diag = s_g295ForceDiagLoop || s_g295AnyProbeEnv || g100ForceTitleDraw ||
                          s_g216CharExec || s_g106ParallelFlags || vu1_trace_enabled() ||
                          g32_vu_trace_enabled() || g137_trace_enabled();
    // G410: the compiled path is deliberately excluded from diagnostic runs because several
    // historical probes live inside execUpper or depend on its exact call boundary. Normal
    // execution recognizes the compiled zero-work VNOP form directly from the fetched word;
    // audit modes also consume the write-maintained sidecar to prove MPG invalidation coverage.
    const bool g410CompiledRun =
        g410CompiledRequested() && g410UpperDiagnosticsCompatible() && !g295Diag;
    const bool g410VerifyRun = g410CompiledRun && g410VerifyOn();
    const bool g410CountStats = g410CompiledRun && (g410StatOn() || g410VerifyRun);
    G410CompiledUpper *const g410Pairs =
        g410CompiledRun ? g410PreparePairs(vuCode, g410CountStats) : nullptr;
    auto g410VerifyUpper = [&](uint32_t lowerWord, uint32_t upperWord) {
        const VU1State before = m_state;
        // Special2 function 0x2f with opcode low bits 0x3f is VNOP. Its register and
        // destination fields are don't-care, so bits 10:0 fully identify the operation.
        if ((upperWord & 0x7ffu) != 0x2ffu)
            execUpper(upperWord);
        const VU1State compiled = m_state;
        m_state = before;
        execUpper(upperWord);
        if (!g410UpperStatesEqual(m_state, compiled))
        {
            ++s_g410Cache.verifyBad;
            std::fprintf(stderr,
                         "[G410:verify] mismatch pc=0x%x lower=%08x upper=%08x "
                         "mac=%08x/%08x status=%08x/%08x clip=%08x/%08x\n",
                         m_state.pc, lowerWord, upperWord,
                         compiled.mac, m_state.mac, compiled.status, m_state.status,
                         compiled.clip, m_state.clip);
            std::abort();
        }
        ++s_g410Cache.verified;
        m_state = compiled;
    };
    // G421: dynamic hot-loop census (default OFF; one cached master bool).
    const bool g421On = g421CensusOn();
    // G421: inline SIMD upper execution. Requires the G410 sidecar (for upperKind) and the
    // unperturbed FMAC contract — every diagnostic that changes what an upper op writes keeps
    // the legacy execUpper() path.
    const bool g421FastUpperRun =
        g421FastUpperRequested() && g410CompiledRun && !g410VerifyRun &&
        s_g330FusedMacOn && !s_vu1MacDisabled && !s_g71clampcount && !s_vu1Clamp;
    const bool g421VerifyRun = g421FastUpperRun && g421VerifyOn();
    static std::atomic<uint64_t> s_g421Verified{0ull};
    // Cold shadow-verify path (DC2_G421_VERIFY): run the fast path, restore, run the legacy
    // decoder from the same pre-state, and compare every field execUpper can write. Returns
    // false when the encoding is not covered, so the caller falls through to execUpper.
    auto g421VerifyUpperSlow = [&](uint32_t upperWord, uint8_t upperKind) -> bool {
        s_g421VerBefore = m_state;
        if (!g421FastUpper(m_state, upperWord, upperKind, false))
            return false;
        s_g421VerFast = m_state;
        m_state = s_g421VerBefore;
        execUpper(upperWord);
        const VU1State &fast = s_g421VerFast;
        const bool bad = std::memcmp(fast.vf, m_state.vf, sizeof(fast.vf)) != 0 ||
                         std::memcmp(fast.acc, m_state.acc, sizeof(fast.acc)) != 0 ||
                         fast.mac != m_state.mac || fast.status != m_state.status ||
                         fast.clip != m_state.clip;
        if (bad)
        {
            std::fprintf(stderr,
                         "[G421:verify] mismatch pc=0x%x upper=%08x kind=%02x "
                         "mac=%08x/%08x status=%08x/%08x\n",
                         m_state.pc, upperWord, (unsigned)upperKind,
                         fast.mac, m_state.mac, fast.status, m_state.status);
            std::abort();
        }
        if ((s_g421Verified.fetch_add(1ull, std::memory_order_relaxed) % 50000000ull) == 0ull)
            std::fprintf(stderr, "[G421:verify] ok=%llu bad=0\n",
                         (unsigned long long)s_g421Verified.load(std::memory_order_relaxed));
        return true;
    };
    // G426: branchless fused upper FMAC dispatch. Sits in front of the G421 fast path and shares
    // every one of its preconditions; kinds outside the FMAC family return false and fall through
    // to G421 unchanged. The control table pointer is resolved ONCE per run() so the hot loop
    // never pays the function-local-static guard.
    const bool g426FusedRun = g421FastUpperRun && g426FusedRequested();
    const bool g426VerifyRun = g426FusedRun && g426VerifyOn();
    const G426Ctl *const g426Ctl = g426FusedRun ? g426CtlTable() : nullptr;
    static std::atomic<uint64_t> s_g426Verified{0ull};
    // Cold shadow-verify path (DC2_G426_VERIFY): run the fused path, restore, run the legacy
    // decoder from the same pre-state, and compare every field execUpper can write. Returns
    // false when the encoding is not an FMAC form, so the caller falls through to G421.
    auto g426VerifyUpperSlow = [&](uint32_t upperWord, uint8_t upperKind) -> bool {
        s_g426VerBefore = m_state;
        if (!g426FastUpper(m_state, upperWord, upperKind, g426Ctl))
            return false;
        s_g426VerFast = m_state;
        m_state = s_g426VerBefore;
        execUpper(upperWord);
        const VU1State &fast = s_g426VerFast;
        const bool bad = std::memcmp(fast.vf, m_state.vf, sizeof(fast.vf)) != 0 ||
                         std::memcmp(fast.acc, m_state.acc, sizeof(fast.acc)) != 0 ||
                         fast.mac != m_state.mac || fast.status != m_state.status ||
                         fast.clip != m_state.clip;
        if (bad)
        {
            std::fprintf(stderr,
                         "[G426:verify] mismatch pc=0x%x upper=%08x kind=%02x "
                         "mac=%08x/%08x status=%08x/%08x\n",
                         m_state.pc, upperWord, (unsigned)upperKind,
                         fast.mac, m_state.mac, fast.status, m_state.status);
            std::abort();
        }
        if ((s_g426Verified.fetch_add(1ull, std::memory_order_relaxed) % 50000000ull) == 0ull)
            std::fprintf(stderr, "[G426:verify] ok=%llu bad=0\n",
                         (unsigned long long)s_g426Verified.load(std::memory_order_relaxed));
        return true;
    };
    // G422: inline lower-slot execution. Requires the G410 sidecar (for lowerKind) and that no
    // probe living inside a covered execLower body is armed.
    const bool g422FastLowerRun =
        g422FastLowerRequested() && g410CompiledRun && !g410VerifyRun && g422LowerDiagFree();
    const bool g422VerifyRun = g422FastLowerRun && g422VerifyOn();
    // G480: skip the g422FastLower CALL on a NOP lower slot.
    //
    // G422 was built on the premise that "NOP -> the call disappears entirely (39.3% of pairs)",
    // which holds only if `g422FastLower` is actually inlined. It is NOT: it is declared
    // `static __forceinline`, MSVC declined (33 kinds is far past its budget), and the linker map
    // for this binary carries a real out-of-line body at 0x14031eb30 with its own unwind and
    // $chain$ records. The G478 VU1-worker PC sampler attributes 4.0-4.5% of the pole thread to
    // the public symbol immediately BEFORE it (`std::_Atomic_integral<unsigned int,4>::fetch_or`,
    // 0x140318e70) — the G471 PDB fold, resolved here from the map rather than guessed.
    //
    // So ~39% of ~1.41 M pairs/frame make a five-argument call into a function whose entire body
    // for that kind is `case G422_LK_NOP: return;`. Hoisting that one comparison into the loop is
    // exact by construction (read the case), costs one more perfectly-predicted branch on the
    // pairs that do call, and deletes ~554 k null calls per frame.
    //
    // Loop-INVARIANT (read once per run(), never per pair), so it joins g422FastLowerRun rather
    // than adding a loop-CARRIED value — the resource G427/G442/G479 proved this loop binds on.
    // Promoted default-on; rollback DC2_G480_NO_NOPSKIP=1; paired gate DC2_G419_AB=nopskip.
    const bool g480NopSkip = g422FastLowerRun && !g422VerifyRun && g480NopSkipRun();
    // G481: inline the seven HOT lower kinds (LQ/SQI/IADDI/LQI/IBNE/IADDIU/IAND, ~42% of all pairs
    // per [G479:lk]) into the loop, leaving the other 26 kinds behind the existing call. Same
    // premise and the same shape as G480's NOP skip, one step further: the call MSVC refused to
    // inline is what is being removed, not the work. Bodies are shared token-for-token with
    // g422FastLower through vu1_g481_hot_lower.inc, so the two paths cannot drift.
    // Loop-INVARIANT (read once per run()); rollback DC2_G481_NO_HOT_LOWER=1; gate
    // DC2_G419_AB=hotlower; shadow oracle DC2_G481_VERIFY=1.
    const bool g481VerifyRun = g422FastLowerRun && !g422VerifyRun && g481VerifyOn();
    const bool g481HotLower = g422FastLowerRun && !g422VerifyRun && g481HotLowerRun();
    static std::atomic<uint64_t> s_g422Verified{0ull};
    static std::atomic<uint64_t> s_g481Verified{0ull};
    // Cold shadow-verify path (DC2_G422_VERIFY): run the fast path, restore both the register
    // state AND all 16 KB of VU data memory, re-run the legacy execLower from the same
    // pre-state, and compare everything either path can write.
    auto g422VerifyLowerSlow = [&](uint32_t lowerWord, uint32_t upperWord, uint8_t lowerKind) {
        const bool touchesMem = g422KindStores(lowerKind);
        s_g422VerBefore = m_state;
        if (touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE)
            std::memcpy(s_g422VerMemBefore, vuData, dataSize);
        g422FastLower(m_state, lowerWord, lowerKind, vuData, dataSize);
        s_g422VerFast = m_state;
        if (touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE)
        {
            std::memcpy(s_g422VerMemFast, vuData, dataSize);
            std::memcpy(vuData, s_g422VerMemBefore, dataSize);
        }
        m_state = s_g422VerBefore;
        execLower(lowerWord, vuData, dataSize, gs, memory, upperWord);
        const bool badState = !g422StatesEqual(s_g422VerFast, m_state);
        const bool badMem = touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE &&
                            std::memcmp(s_g422VerMemFast, vuData, dataSize) != 0;
        if (badState || badMem)
        {
            std::fprintf(stderr,
                         "[G422:verify] mismatch pc=0x%x lower=%08x kind=%u state=%d mem=%d "
                         "pc=%08x/%08x\n",
                         s_g422VerBefore.pc, lowerWord, (unsigned)lowerKind,
                         (int)badState, (int)badMem, s_g422VerFast.pc, m_state.pc);
            std::abort();
        }
        if ((s_g422Verified.fetch_add(1ull, std::memory_order_relaxed) % 50000000ull) == 0ull)
            std::fprintf(stderr, "[G422:verify] ok=%llu bad=0\n",
                         (unsigned long long)s_g422Verified.load(std::memory_order_relaxed));
    };
    // G481 shadow oracle (DC2_G481_VERIFY=1): execute the INLINE hot body, snapshot everything it
    // can write, restore, execute the out-of-line g422FastLower from the identical pre-state, and
    // compare the whole VU1State plus all 16 KB of VU data memory. The bodies are shared tokens so
    // this can only fail on a macro-expansion defect — which is exactly what it is here to catch.
    auto g481VerifyHotSlow = [&](uint32_t lowerWord, uint8_t lowerKind) {
        const bool touchesMem = g422KindStores(lowerKind);
        s_g422VerBefore = m_state;
        if (touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE)
            std::memcpy(s_g422VerMemBefore, vuData, dataSize);
        bool handled = false;
        DC2_G481_HOT_LOWER(lowerKind, m_state, lowerWord, vuData, dataSize, handled);
        if (!handled)
            return false;
        s_g422VerFast = m_state;
        if (touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE)
        {
            std::memcpy(s_g422VerMemFast, vuData, dataSize);
            std::memcpy(vuData, s_g422VerMemBefore, dataSize);
        }
        m_state = s_g422VerBefore;
        g422FastLower(m_state, lowerWord, lowerKind, vuData, dataSize);
        const bool badState = !g422StatesEqual(s_g422VerFast, m_state);
        const bool badMem = touchesMem && vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE &&
                            std::memcmp(s_g422VerMemFast, vuData, dataSize) != 0;
        if (badState || badMem)
        {
            std::fprintf(stderr,
                         "[G481:verify] mismatch pc=0x%x lower=%08x kind=%u state=%d mem=%d\n",
                         s_g422VerBefore.pc, lowerWord, (unsigned)lowerKind,
                         (int)badState, (int)badMem);
            std::abort();
        }
        if ((s_g481Verified.fetch_add(1ull, std::memory_order_relaxed) % 50000000ull) == 0ull)
            std::fprintf(stderr, "[G481:verify] ok=%llu bad=0\n",
                         (unsigned long long)s_g481Verified.load(std::memory_order_relaxed));
        return true;
    };
// Runs the lower slot. A macro for the same reason as DC2_G421_RUN_UPPER: when the lever is off
// the control arm must keep the byte-identical legacy `execLower(...)` call, reached through one
// loop-invariant, perfectly-predicted branch.
#define DC2_G422_RUN_LOWER()                                                          \
    do {                                                                              \
        if (g422FastLowerRun)                                                         \
        {                                                                             \
            const uint8_t g422kind = g410Pair->lowerKind;                             \
            /* G481 REJECTED (+0.53 ms/f, t = 1.33): folding the NOP test, the hot     \
               switch and the G422_LK_NONE test into ONE jump on the kind adds no text \
               and removes two compares per pair — and still LOSES the whole win. A    \
               switch whose default arm is a call re-shapes the loop the compiler had  \
               already scheduled around three predictable compares. Do not retry. */    \
            /* G480: g422FastLower's NOP case is exactly `return;` — do not call it. */\
            if (g480NopSkip && g422kind == G422_LK_NOP)                               \
                break;                                                                \
            /* G481: the seven hot kinds run inline; no call at all. */                \
            if (g481HotLower)                                                         \
            {                                                                         \
                if (g481VerifyRun)                                                    \
                {                                                                     \
                    if (g481VerifyHotSlow(lower, g422kind))                           \
                        break;                                                        \
                }                                                                     \
                else                                                                  \
                {                                                                     \
                    bool g481Handled = false;                                         \
                    DC2_G481_HOT_LOWER(g422kind, m_state, lower, vuData, dataSize,    \
                                       g481Handled);                                  \
                    if (g481Handled)                                                  \
                        break;                                                        \
                }                                                                     \
            }                                                                         \
            if (g422kind != G422_LK_NONE)                                             \
            {                                                                         \
                if (g422VerifyRun)                                                    \
                    g422VerifyLowerSlow(lower, upper, g422kind);                      \
                else                                                                  \
                    g422FastLower(m_state, lower, g422kind, vuData, dataSize);        \
                break;                                                                \
            }                                                                         \
        }                                                                             \
        execLower(lower, vuData, dataSize, gs, memory, upper);                        \
    } while (0)
// Runs the upper slot. A macro, not a lambda: the default (lever-off) path must keep the exact
// legacy `execUpper(upper)` call shape with no extra frame, since it is the control arm. When
// the lever is off this collapses to one loop-invariant, perfectly-predicted branch.
#define DC2_G421_RUN_UPPER(upperWord, upperKind)                                      \
    do {                                                                              \
        if (g426FusedRun)                                                             \
        {                                                                             \
            if (g426VerifyRun)                                                        \
            {                                                                         \
                if (g426VerifyUpperSlow((upperWord), (upperKind))) break;             \
            }                                                                         \
            else if (g426FastUpper(m_state, (upperWord), (upperKind), g426Ctl))       \
                break;                                                                \
        }                                                                             \
        if (g421FastUpperRun)                                                         \
        {                                                                             \
            if (g421VerifyRun)                                                        \
            {                                                                         \
                if (g421VerifyUpperSlow((upperWord), (upperKind))) break;             \
            }                                                                         \
            else if (g421FastUpper(m_state, (upperWord), (upperKind),                 \
                                   g428DeferPark, s_g428Verify || g430Shadow,          \
                                   g430Hist, g430Base + (int64_t)cycle, g436Hb))       \
                break;                                                                \
        }                                                                             \
        DC2_G430_EXEC_UPPER_LEGACY(upperWord);                                        \
    } while (0)
// G430: an upper the inline path does not cover still writes the MAC, and under the stamped
// history m_state.mac is otherwise stale. Resolve it to the live value first, let the legacy
// decoder update it, then record the result as an already-classified history entry. Off the
// history path this is the byte-identical legacy call.
#define DC2_G430_EXEC_UPPER_LEGACY(uw)                                                \
    do {                                                                              \
        if (g430Hist)                                                                 \
        {                                                                             \
            m_state.mac = g430ResolveAt(g430Base + (int64_t)cycle);                   \
            execUpper(uw);                                                            \
            g430PushEager(g430Base + (int64_t)cycle, m_state.mac);                    \
        }                                                                             \
        else                                                                          \
            execUpper(uw);                                                            \
    } while (0)
    // G425: lean per-pair bookkeeping. Three pieces of the loop body exist only to feed
    // diagnostics that cannot fire on a normal run:
    //   * lastPc / lastLower / lastUpper (3 stores/pair) feed the [G22:vu1stop] trace only,
    //   * g72_vi1_before (1 load/pair) feeds G295 diag region D only,
    //   * the G369 non-finite VF scan carries a function-local magic-static guard per pair,
    // and a fourth, the VF0/VI0 hardware-invariant re-pin (5 stores/pair), is provably the
    // identity on every pair whose two slots cannot name register 0 (G425_PF_TOUCHES_ZERO,
    // decoded once into the G410 sidecar). None of this changes what any instruction computes.
    // RESULT: NOT PROMOTED. The lever is exact (DC2_G425_VERIFY clean over a full MAP-0 run)
    // and it does remove per-pair work, but the frame and VU1-worker effects both sit under the
    // run-level noise floor of this machine (pooled 4v4 in both arm orders: 51.21 vs 51.73 ms,
    // with the two batches disagreeing in sign). Per the "neutral after parity" rule it ships
    // OPT-IN so the substrate survives for a later re-price (the G423 precedent), and the
    // default path keeps the legacy bookkeeping byte-for-byte.
    // Arm:    DC2_G425_LEAN_PAIR=1
    // Verify: DC2_G425_VERIFY=1 asserts the VF0/VI0 invariant after every executed pair.
    static const bool s_g425LeanOn = (std::getenv("DC2_G425_LEAN_PAIR") != nullptr) &&
                                     (std::getenv("DC2_G425_NO_LEAN_PAIR") == nullptr);
    static const bool s_g425VuNan = (std::getenv("DC2_G369_VUNAN") != nullptr);
    static const bool s_g425Verify = (std::getenv("DC2_G425_VERIFY") != nullptr);
    // G426: sidecar-resolved per-pair predicates. G426's first lever (branchless fused upper
    // dispatch, DC2_G426_FUSED_UPPER) proved this loop is THROUGHPUT-bound, not
    // misprediction-bound: ~20 extra SSE2 ops per executed upper cost 4.2 ms/f (~1 host cycle
    // per op), so the only way to move it is to execute FEWER instructions per pair. This lever
    // does exactly that by moving three per-pair decisions that are pure functions of the pair
    // encoding into the G410 sidecar, which already caches and invalidates that encoding:
    //   * the G239 scalar-prestall applicability test (G426_PF_PRESTALL),
    //   * the G139 same-pair hazard applicability test (G426_PF_HAZ),
    //   * G425's VF0/VI0 re-pin elision (G425_PF_TOUCHES_ZERO), promoted here from opt-in
    //     because on its own it sat under the run-level noise floor, and the three together do
    //     not.
    // Nothing about what an instruction computes changes; each predicate is the same boolean the
    // loop used to recompute. The G139 form is only equivalent while the G295 hazard-skip is
    // active, so DC2_G295_NO_HAZSKIP=1 keeps the legacy chain.
    // Kill: DC2_G426_NO_PREDECODE=1
    static const bool s_g426PredecodeOn =
        (std::getenv("DC2_G426_NO_PREDECODE") == nullptr);
    // DC2_G426_VERIFY_PREDECODE=1 recomputes both predicates from the live instruction words on
    // every executed pair and aborts on the first disagreement with the cached bits.
    static const bool s_g426VerifyPredecode =
        (std::getenv("DC2_G426_VERIFY_PREDECODE") != nullptr);
    const bool g426Predecode = s_g426PredecodeOn && !g295Diag && !s_g295KeepHaz &&
                               g410Pairs != nullptr;
    const bool g425Lean = (s_g425LeanOn || g426Predecode) && !g295Diag && !s_g425VuNan &&
                          g410Pairs != nullptr;
    // G427: lazy MAC/STATUS delay-line publication. The G138/G328 line is advanced on every
    // executed pair, but `s_vuMacVisible`/`s_vuStatusVisible` are read by exactly six lower
    // opcodes (G427_PF_FLAGREAD, decoded once into the G410 sidecar). On every other pair the
    // two loads and two stores are unobservable: only the ring index has to move, and it is a
    // thread_local that this loop is the sole writer of, so it is held in a register for the
    // whole run and written back once. Same delay line, same values, fewer dynamic instructions
    // per pair — the only lever shape G426 proved can win at this site.
    //
    // RESULT: NOT PROMOTED — exact but FRAME-NEUTRAL, and the reason is the phase's real finding.
    // G426 priced this loop at ~1 host cycle per added SSE2 op, and that price does NOT transfer
    // to scalar bookkeeping: removing four L1-resident scalar memory ops per pair (~1.26 M pairs/f)
    // moved the pooled frame by +0.12 ms with the arms fully overlapping in both orders. The pair
    // loop's critical resource is the SIMD FMAC work, and the scalar delay-line traffic issues on
    // ports that were otherwise idle. Ships opt-in (the G423/G425 precedent) so the substrate and
    // its verifier survive for a re-price on a different binary or host.
    // Arm:    DC2_G427_LAZY_FLAGS=1     Kill: DC2_G427_NO_LAZY_FLAGS=1
    // Verify: DC2_G427_VERIFY=1 recomputes the predicate from the live lower word every pair.
    static const bool s_g427LazyOn = (std::getenv("DC2_G427_LAZY_FLAGS") != nullptr) &&
                                     (std::getenv("DC2_G427_NO_LAZY_FLAGS") == nullptr);
    static const bool s_g427Verify = (std::getenv("DC2_G427_VERIFY") != nullptr);
    const bool g427Lazy = s_g427LazyOn && !g295Diag && g410Pairs != nullptr &&
                          s_macpipeOn && s_g328RingOn && !s_g328Verify && !s_g328Stat;

    // G428: DEFERRED MAC CLASSIFICATION. The G329/G330 classifier is ~10 SSE2 ops and runs on
    // every inline FMAC (45.0% of executed pairs, ~567 k/frame on lean MAP-0), but its product is
    // consumed by only 2.06% of pairs, and only through the G138 delay line. So the delay-line
    // entry parks the raw result vector + dest mask and the classifier runs at the CONSUMER.
    // This is the shape G426/G427 proved is the only one that can win here: strictly fewer SIMD
    // ops on the FMAC path, with the added bookkeeping being scalar/memory traffic (which G427
    // measured as free at this site) and no new value live across the loop body.
    //
    // SOUNDNESS. MAC is a per-pair snapshot and is fully reconstructible from (result, dest).
    // STATUS is NOT: its sticky bits accumulate over every FMAC, so a deferred replay cannot
    // rebuild it. The lever is therefore gated on the G410 cache proving that the ENTIRE code
    // store decodes to zero STATUS consumers (`statusReaderSlots == 0`; FSEQ/FSAND/FSOR are the
    // only three, and DC2's VU1 microcode contains none — measured over a full MAP-0 route).
    // Any MPG upload that introduces one flips the gate back off from the next run() onwards.
    // Kill: DC2_G428_NO_DEFER_MAC=1   Verify: DC2_G428_VERIFY=1
    static const bool s_g428DeferOn = (std::getenv("DC2_G428_NO_DEFER_MAC") == nullptr);
    static const bool s_g428Verify = (std::getenv("DC2_G428_VERIFY") != nullptr);
    const bool g428Defer =
        s_g428DeferOn && !g295Diag && g410Pairs != nullptr && s_macpipeOn && s_g328RingOn &&
        !s_g328Verify && !s_g328Stat && !g427Lazy && g421FastUpperRun && !g421VerifyRun &&
        !g426FusedRun && !g410VerifyRun && !s_g106ParallelFlags && !s_g330Verify &&
        s_g410Cache.statusReaderSlots == 0u;

    // G430 CEILING PROBE. Opt-in, default-OFF, KNOWINGLY INCORRECT OUTPUT. Deletes one class of
    // per-pair work from the VU1 pair loop so the phase can PRICE that class before designing a
    // lever for it (the DC2_G429_NO_UPLOAD_EDGE doctrine: delete the edge in a throwaway arm and
    // read the frame). Bits of DC2_G430_SKIP:
    //   1 = the whole G138/G328/G428 MAC delay line (advance + push), i.e. every per-pair
    //       bookkeeping op the flag pipeline costs. This is the probe that SIZED the phase:
    //       -8 ms off the VU1 worker, -9 ms off the frame, at an unchanged workload.
    //   2 = ALL upper-slot execution (the FMAC SIMD work itself).
    //   4 = only the G428 park store on the FMAC path.
    //   8 = the G139 same-pair hazard save/restore AND the VF0/VI0 re-pin.
    // Never promote. Output is wrong on every non-zero setting.
    static const unsigned s_g430Skip = [] {
        const char *e = std::getenv("DC2_G430_SKIP");
        return e ? static_cast<unsigned>(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    const bool g430SkipMacPipe = (s_g430Skip & 1u) != 0u && !g295Diag;
    const bool g430SkipUpper   = (s_g430Skip & 2u) != 0u && !g295Diag;
    const bool g430SkipPark    = (s_g430Skip & 4u) != 0u && !g295Diag;
    const bool g430SkipHaz     = (s_g430Skip & 8u) != 0u && !g295Diag;
    const bool g430MacPipeOn   = s_macpipeOn && !g430SkipMacPipe;
    const bool g428DeferPark   = g428Defer && !g430SkipPark;

    // G430: STAMPED MAC HISTORY. Same delay line, expressed as (pairStamp, result, dest) records
    // written by the FMAC itself and searched at the consumer, so the ~55% of pairs that write no
    // MAC and the ~98% that read none pay NOTHING. Shares every G428 precondition (it is a
    // different representation of the same deferred line) and adds the ring-size bound.
    // Kill: DC2_G430_NO_MAC_HISTORY=1   Verify: DC2_G430_VERIFY=1
    static const bool s_g430HistOn = (std::getenv("DC2_G430_NO_MAC_HISTORY") == nullptr);
    static const bool s_g430Verify = (std::getenv("DC2_G430_VERIFY") != nullptr);
    const bool g430Hist = g428Defer && s_g430HistOn && !s_g428Verify && !g430SkipMacPipe &&
                          !g430SkipPark && s_macpipeDepth < G430_HIST;
    const int64_t g430Base = s_g430PairBase;
    const bool g430Shadow = g430Hist && s_g430Verify;
    static std::atomic<uint64_t> s_g430VerifyOk{0ull};

    // DISARM first: put the delay line back into the plain (non-deferred) ring shape the G428
    // transition below expects to find, so the two converters compose in either order.
    if (s_g430HistArmed && !g430Hist)
    {
        for (int age = s_macpipeDepth - 1; age >= 0; --age)
        {
            const int slot = (s_g328MacPipeHead + age < s_macpipeDepth)
                                 ? s_g328MacPipeHead + age
                                 : s_g328MacPipeHead + age - s_macpipeDepth;
            s_vuMacPipe[slot] = g430ResolveAt(g430Base - 1 - age);
            s_vuStatusPipe[slot] = m_state.status;
            s_g428Kind[slot] = G428_MK_EAGER;
        }
        s_g428PreKind = G428_MK_EAGER;
        s_g428PreMac = g430ResolveAt(g430Base - 1 - s_macpipeDepth);
        s_g428Claim = G428_MK_CARRY;
        s_g428Armed = false;
        s_g430HistArmed = false;
    }

    // The gate is re-evaluated per run() (the code store can change between runs), so reconcile
    // the two delay-line representations on every transition, before the first pair executes.
    if (g428Defer != s_g428Armed)
    {
        if (!g428Defer)
            g428MaterializePipe(s_macpipeDepth, s_g328MacPipeHead);
        else
        {
            s_g428PreKind = G428_MK_EAGER;
            s_g428PreMac = m_state.mac;
        }
        for (int i = 0; i < 8; ++i)
            s_g428Kind[i] = G428_MK_EAGER;
        s_g428Claim = G428_MK_CARRY;
        s_g428Armed = g428Defer;
    }

    // ARM: seed the history from whichever ring representation is now live, stamping the ring's
    // logical ages backwards from the last completed pair, so the first DEPTH consumers of this
    // run resolve to exactly what the ring would have published.
    if (g430Hist && !s_g430HistArmed)
    {
        for (int i = 0; i < G430_HIST; ++i)
            s_g430HB.stamp[i] = INT64_MIN;
        s_g430HB.head = 0;
        // Resolve the deferred ring in place first (oldest-first CARRY propagation), so every
        // slot below is a plain value.
        if (s_g428Armed)
            g428MaterializePipe(s_macpipeDepth, s_g328MacPipeHead);
        for (int age = s_macpipeDepth - 1; age >= 0; --age)
        {
            const int slot = (s_g328MacPipeHead + age < s_macpipeDepth)
                                 ? s_g328MacPipeHead + age
                                 : s_g328MacPipeHead + age - s_macpipeDepth;
            g430PushEager(g430Base - 1 - age, s_vuMacPipe[slot]);
        }
        s_g430HistArmed = true;
    }

    uint32_t *const s_macPipeP     = s_vuMacPipe;
    uint32_t *const s_statusPipeP  = s_vuStatusPipe;
    uint32_t *const s_macVisP      = &s_vuMacVisible;
    uint32_t *const s_statusVisP   = &s_vuStatusVisible;
    int      *const s_qDelayP      = &s_vuQDelay;
    int      *const s_pDelayP      = &s_vuPDelay;

    // G427: the delay-line head lives in the same thread_local block as the pipes; hoist its
    // address once, exactly as G141 did for the pipes themselves. (An earlier variant held the
    // head in a loop-carried local and wrote it back at the exits; that was measurably SLOWER —
    // the extra live value costs more in this loop's register pressure than the TLS traffic.)
    int *const s_headP = &s_g328MacPipeHead;

    // G435: LEAN PAIR LOOP. The four G295 diagnostic regions are ~1050 lines of code that
    // `g295Diag` makes unreachable on every normal run, but they still sit *inside* the loop
    // body, between the hot statements. G421's own census note measured ~3 ms/f of default-path
    // slowdown from adding never-executed histogram code to this very loop, i.e. the loop is
    // sensitive to its instruction footprint, not only to what it executes. This selects a
    // preprocessor copy of the identical body with those regions removed; it can only be chosen
    // when g295Diag is false, in which case the removed blocks provably do nothing.
    // RESULT: NOT PROMOTED — exact, but frame-NEUTRAL-to-NEGATIVE. Pooled 2v2 in both arm
    // orders: lean 49.16 vs legacy 48.63 ms, same sign in both blocks. MSVC already sinks the
    // cold `if (g295Diag)` regions out of the loop's straight-line path, so removing their TEXT
    // buys nothing and the extra copy of the body costs I-cache. Ships OPT-IN (the G423/G425/G427
    // precedent) so the substrate survives a re-price on another compiler or host; the default
    // path is the byte-identical historical body.
    // Arm: DC2_G435_LEAN_LOOP=1   Kill: DC2_G435_NO_LEAN_LOOP=1
    static const bool s_g435LeanOn = (std::getenv("DC2_G435_LEAN_LOOP") != nullptr) &&
                                     (std::getenv("DC2_G435_NO_LEAN_LOOP") == nullptr);
    const bool g435LeanLoop = s_g435LeanOn && !g295Diag;

    // G435: WIDE VF0 RE-PIN. The VF0/VI0 hardware-invariant re-pin at the bottom of the pair
    // loop wrote VF0 as FOUR separate 4-byte scalar stores. VF0 = (0,0,0,1) is the microcode's
    // universal constant register, so the very next pair almost always issues a 16-byte
    // `_mm_loadu_ps(st.vf[FS])` over those exact bytes — a narrow-store → wide-load overlap that
    // store-to-load forwarding cannot satisfy, so the load waits for the four stores to retire
    // to L1 (~12+ cycles on this class of core) on a large share of 1.65 M pairs/frame. The
    // `DC2_G430_SKIP=8` ceiling probe priced this whole block at −4.6 ms of VU1 busy and
    // −5.7 ms/f of frame, while the G421 census reports the G139 hazard half of it firing on
    // 0.0% of pairs — i.e. essentially all of that price is these four stores.
    //
    // EXACTNESS. One 16-byte store of the same four constants writes exactly the same bytes as
    // the four scalar stores, and nothing can observe the store granularity: this is the last
    // statement of the pair and VU1State is only read by this thread. The four-store form is
    // kept as the control arm.
    // Kill: DC2_G435_NO_WIDE_VF0=1   Verify: DC2_G425_VERIFY=1 (asserts the invariant per pair)
    static const bool s_g435WideVf0On = (std::getenv("DC2_G435_WIDE_VF0") != nullptr) &&
                                        (std::getenv("DC2_G435_NO_WIDE_VF0") == nullptr);
    const bool g435WideVf0 = s_g435WideVf0On;
    const __m128 g435Vf0Const = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);

    // G435 CEILING PROBE (opt-in, default-OFF, KNOWINGLY INCORRECT OUTPUT). `DC2_G430_SKIP=8`
    // deletes the G139 same-pair hazard block AND the VF0/VI0 re-pin together and prices the pair
    // at −5.7 ms/f, but the two are unrelated mechanisms and the `[G421:vu] hazSave` counter that
    // would have split them is DEAD (g421NoteCycle never increments it — see G435 fix log).
    // Bits of DC2_G435_SKIP: 1 = only the VF0/VI0 re-pin · 2 = only the G139 hazard save /
    // restore / overlay. Never promote; both produce wrong output.
    static const unsigned s_g435Skip = [] {
        const char *e = std::getenv("DC2_G435_SKIP");
        return e ? static_cast<unsigned>(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    const bool g435SkipRepin = (s_g435Skip & 1u) != 0u && !g295Diag;
    const bool g435SkipHaz   = (s_g435Skip & 2u) != 0u && !g295Diag;

    // G435: REGISTER-FORM G139 PAIR HAZARD. The split ceiling probe above prices the hazard
    // block at ~4.9 ms/f of VU1 busy (45.9 -> 41.0) and the VF0/VI0 re-pin at only ~1.5, yet the
    // hazard's own predicate (G426_PF_HAZ) is true on a small minority of pairs — so almost none
    // of that 4.9 ms is work the block DOES. The cost is what its variables do to the rest of the
    // loop: `float *g139dst` is a raw pointer into `m_state.vf`, and `g139old`/`g139new` are
    // address-taken stack arrays written through `memcpy`. Between them they force the compiler
    // to assume that every `m_state.vf` access in the inline upper AND the inline lower may
    // alias, on all ~1.65 M pairs/frame, whether or not the hazard fires.
    //
    // The register form keeps exactly the same three values — the destination register INDEX and
    // the pre-upper / post-upper vectors — in an int and two __m128s, so no pointer into
    // `m_state.vf` is ever live across the two exec sites. The final overlay becomes the same
    // masked bit-select G421 already uses for a dest write.
    //
    // EXACTNESS. `g139Idx >= 0` is true exactly when `g139dst != nullptr` was; the load/store
    // pair moves the same 16 bytes; the bit-select rewrites masked-off lanes with their own bits,
    // matching the four conditional scalar assignments operand for operand.
    // RESULT: NOT PROMOTED — exact, but frame-NEUTRAL, which REFUTES the alias hypothesis above.
    // Pooled 2v2 in both arm orders: 47.92 (register form) vs 47.55 (pointer form), and the two
    // blocks disagree in sign (+0.90 / −0.16), i.e. under this machine's run-level floor. The
    // 4.9 ms the ceiling probe attributes to this block is therefore NOT alias poisoning, and the
    // probe's own workload is not invariant (`kicks/f` 1225 under the probe vs 1294 on the
    // baseline) — deleting the hazard changes what the microcode computes and hence how much it
    // draws. Ships opt-in so the substrate survives a re-price.
    // Arm: DC2_G435_HAZ_REG=1   Kill: DC2_G435_NO_HAZ_REG=1
    static const bool s_g435HazRegOn = (std::getenv("DC2_G435_HAZ_REG") != nullptr) &&
                                       (std::getenv("DC2_G435_NO_HAZ_REG") == nullptr);
    const bool g435HazReg = s_g435HazRegOn;

    // G436: LEAN HOT-PATH ADDRESSING. Nothing about what an instruction computes changes; this
    // removes work MSVC added to reach data the loop was already using. Three sites, all read off
    // the /FAsc listing of the shipped G435 Release binary:
    //   1. `const G421UpperDesc d = g421Desc()[kind];` compiled to a real `call ?g421Desc` on
    //      EVERY covered upper (50.3% of ~1.65 M pairs/f) because the table was a function-local
    //      `static const` whose thread-safe-init guard blocks inlining. The call also forced
    //      caller-saved spills in the loop (`mov QWORD PTR $T7[rbp-464], rax` around it).
    //      Same for `g421MaskRows()` on the partial-dest store and the G139 overlay.
    //   2. `g430PushPending` (every inline FMAC, ~45% of pairs) touched FIVE separate
    //      thread_locals; MSVC re-materialised the TLS base for each, so the push was ~28
    //      instructions with five dependent `gs:[88]` -> `[base+_tls_index*8]` load pairs.
    //   3. `s_g106UseLowerFlagRead = false;` ran unconditionally on every pair (5 instructions,
    //      2 dependent TLS loads) although the only writer of `true` is inside
    //      `if (s_g106ParallelFlags)`, which is off by default — so the store is provably the
    //      identity whenever that lever is off.
    // The lean arm reads the SAME tables (now constant-initialised) and the SAME history fields
    // (now one thread_local object hoisted once per run()), in the same order.
    // Kill: DC2_G436_LEGACY_ADDR=1 restores the out-of-line accessors, the per-push TLS
    // materialisation and the unconditional flag store.
    static const bool s_g436LegacyAddr = (std::getenv("DC2_G436_LEGACY_ADDR") != nullptr);
    G430HistBlock *const g436Hb = s_g436LegacyAddr ? nullptr : &s_g430HB;
    const bool g436Lean = !s_g436LegacyAddr;
    // Identity whenever G106 parallel flags are off (the default); see note 3 above.
    const bool g436FlagStore = s_g436LegacyAddr || s_g106ParallelFlags;

    // G442 - keep the interpreter PC in a REGISTER.
    //
    // PREMISE. G441 left the pole at ~26 ns/pair (~100 host cycles) against an executed mix worth
    // ~12, with instruction count (G436), footprint/form (G435), scalar bookkeeping (G427),
    // dispatch (G410) and data placement (G437) all closed, i.e. the residue is LATENCY. The pair
    // loop's own carried dependency ran through MEMORY: `m_state.pc` was stored at the bottom of
    // every iteration and re-loaded at the top of the next one, and the whole address chain of the
    // pair — the bounds check, the two `vuCode + currentPc` fetches and the `g410Pairs[pc >> 3]`
    // sidecar load — hangs off that load. A 4-byte store-to-load forward is ~5 cycles before the
    // pair's first useful instruction can issue, every pair, and nothing after it can start early.
    //
    // MECHANISM. Mirror the PC in a local. `m_state.pc` keeps receiving exactly the same write at
    // exactly the same point, so every reader outside this loop (resume(), execLower's branch
    // targets and its diagnostics, the G22 stop trace) is untouched; only the READS move to the
    // register. The post-execution read cannot be dropped unconditionally — a branch lower rewrites
    // pc — so it is kept for the ten branch/jump kinds and for any encoding the G422 fast lower
    // does not cover, which the G410 sidecar already classifies per pair.
    //
    // EXACTNESS. `g442RegPc` is assigned from the same `nextPC` the store uses, so the two can
    // never diverge. The only new assumption is "a pair whose lower is a covered non-branch kind
    // leaves pc alone", which DC2_G442_VERIFY=1 checks on EVERY such pair against the real
    // m_state.pc and aborts on a mismatch.
    //
    // RESULT: REJECTED, +2.56 ms/f against the saved pre-phase binary (47.27 vs 44.71, both
    // order-balanced blocks agreeing in sign). Removing the forward bought less than the two things
    // it cost: one extra per-pair sidecar predicate and — decisively — ONE extra loop-carried local
    // in a loop G427 already measured at +2.8 ms per added loop-carried local. Do not rebuild it.
    // The surviving lever from that measurement is the INVERSE, at the bottom of the pair loop.

    // G443 - FIXED-CONFIGURATION PAIR LOOP.
    //
    // PREMISE. Two independent measurements now price the SAME resource in this loop: G427 read
    // +2.8 ms/f and G442 read +2.56 ms/f for ONE added loop-carried local, against G436's 0.068
    // host cycles per DELETED instruction — a ~40:1 asymmetry that says the binding resource is
    // live values, not instructions. The /FAsc audit agrees: 2487 instructions, zero calls, 36
    // stack spills, ~1.2 IPC. Everything above this line is a loop-INVARIANT predicate that the
    // loop nevertheless has to keep live (or respill) for its whole run: ~40 of them, plus the
    // pointers and per-pair state that actually do work.
    //
    // MECHANISM. When every one of those predicates already equals its default value, select a
    // copy of the identical loop body in which each is a `constexpr` shadow instead of a runtime
    // local. MSVC then constant-folds the predicate, deletes the branch and deletes the
    // unreachable arm — which is the only lever shape that reduces live-value pressure without
    // changing a single executed operation.
    //
    // EXACTNESS. `g443Fixed` is literally the conjunction of "each predicate == the constant that
    // replaces it", evaluated from the same expressions the other two arms use, so inside the
    // block every shadow provably holds the value the name already had. Any env flag, any
    // diagnostic, any verifier, any ceiling probe, any opt-in substrate, any MPG upload that
    // introduces a STATUS reader, or a non-default MAC-pipe depth drops the run back onto the
    // existing arms, which are untouched. (This is a code-SHAPE change: A/B it against a saved
    // pre-phase binary, never against an in-binary switch — G436.)
    // Kill: DC2_G443_NO_FIXED_LOOP=1
    static const bool s_g443NoFixed = (std::getenv("DC2_G443_NO_FIXED_LOOP") != nullptr);
    const bool g443Fixed =
        !s_g443NoFixed &&
        // dispatch / sidecar
        !g295Diag && g410CompiledRun && !g410VerifyRun && !g410CountStats &&
        g410Pairs != nullptr &&
        // census + every shadow verifier
        !g421On && !g421VerifyRun && !g422VerifyRun && !g426VerifyRun &&
        !s_g425Verify && !s_g425VuNan && !s_g426VerifyPredecode && !s_g427Verify &&
        !s_g428Verify && !s_g328Verify && !s_g328Stat &&
        // inline execution
        g421FastUpperRun && g422FastLowerRun && !g426FusedRun &&
        // correctness machinery (all default-ON hardware models)
        s_qlatOn && s_ppipeOn && s_scalarPrestallOn && s_pairhazOn && s_macpipeOn &&
        s_macpipeDepth == 4 && s_g328RingOn &&
        // promoted representations
        g425Lean && g426Predecode && !g427Lazy && g428Defer && g428DeferPark &&
        g430MacPipeOn && g430Hist && !g430Shadow && g436Lean && !g436FlagStore &&
        g436Hb != nullptr &&
        // opt-in / never-promoted substrates
        !g435HazReg && !g435WideVf0 && !s_g295KeepHaz && !s_g106ParallelFlags &&
        // ceiling probes
        !g430SkipMacPipe && !g430SkipUpper && !g430SkipPark && !g430SkipHaz &&
        !g435SkipRepin && !g435SkipHaz;
    // Premise gate for the phase: prove the fixed arm is the one this route actually runs, and
    // name the first predicate that refused it if it is not. One-shot, default OFF.
    {
        static const bool s_g443Stat = (std::getenv("DC2_G443_STAT") != nullptr);
        if (s_g443Stat)
        {
            static std::atomic<uint32_t> s_once{0u};
            if (s_once.fetch_add(1u, std::memory_order_relaxed) == 0u)
            {
                std::fprintf(stderr,
                             "[G443:gate] fixed=%d diag=%d comp=%d pairs=%d fastU=%d fastL=%d "
                             "predec=%d lean=%d defer=%d hist=%d ring=%d depth=%d g436=%d "
                             "haz=%d prestall=%d qlat=%d ppipe=%d\n",
                             (int)g443Fixed, (int)g295Diag, (int)g410CompiledRun,
                             (int)(g410Pairs != nullptr), (int)g421FastUpperRun,
                             (int)g422FastLowerRun, (int)g426Predecode, (int)g425Lean,
                             (int)g428Defer, (int)g430Hist, (int)s_g328RingOn, s_macpipeDepth,
                             (int)g436Lean, (int)s_pairhazOn, (int)s_scalarPrestallOn,
                             (int)s_qlatOn, (int)s_ppipeOn);
                std::fflush(stderr);
            }
        }
    }

    // G479: fused pair record. Read the arm ONCE per run() (never per pair) so a microprogram
    // never changes loop copies mid-execution, and hoist the base pointer exactly like g410Pairs.
    const bool g479LeanFetch = g443Fixed && g479LeanFetchRequested();
    const bool g479Verify = g479VerifyOn();
    const G479FusedPair *const g479Fused = s_g410Cache.fused.data();
    (void)g479Fused;

    // G483: register-carried PC. Read ONCE per run() (G479's rule) so a microprogram can never
    // switch loop copies mid-execution. Default OFF during bring-up; paired gate
    // DC2_G419_AB=pcreg, standalone arm DC2_G483_PCREG=1, oracle DC2_G483_VERIFY=1.
    const bool g483PcReg = g443Fixed && g483PcRegRequested();
    const bool g483VerifyPc = g483VerifyPcOn();
    if (g483VerifyPc)
    {
        // One-shot proof that the oracle copy is actually the one running — a silent oracle that
        // was never selected proves nothing (G457).
        static std::atomic<uint32_t> s_g483Once{0u};
        if (s_g483Once.fetch_add(1u, std::memory_order_relaxed) == 0u)
            std::fprintf(stderr, "[G483:verify] armed pcreg=%d fixed=%d leanfetch=%d\n",
                         g483PcReg ? 1 : 0, g443Fixed ? 1 : 0, g479LeanFetch ? 1 : 0);
    }

    // G484: WIDE VF0 RE-PIN, made MEASURABLE.
    //
    // G435 built the exact 16-byte form of the VF0 re-pin and shipped it opt-in
    // (`DC2_G435_WIDE_VF0=1`). It has been dead ever since: `!g435WideVf0` is a conjunct of
    // `g443Fixed`, so arming the old flag does not put the wide store into the promoted loop —
    // it drops the whole run onto a different, slower loop copy and measures THAT (the G482
    // "structurally unmeasurable" class). This selects the wide store INSIDE the promoted
    // fixed+fused body, which is the only place its price can be read.
    //
    // G485 PROMOTED it default-ON (−0.619 ms/f mean, sign 4/4 over four paired A/Bs).
    // Read ONCE per run() (G479's rule) so a microprogram can never switch loop copies
    // mid-execution. Paired gate DC2_G419_AB=widevf0, standalone arm DC2_G484_WIDE_VF0=1,
    // rollback DC2_G485_NO_WIDE_VF0=1.
    const bool g484WideVf0 = g443Fixed && g479LeanFetch && !g479Verify && !g483PcReg &&
                             g484WideVf0Requested();
    // G486 REFUTED the wide single-lane FMAC destination store (`widedest`). It was built exactly
    // like G485's widevf0 — a copy of the promoted body in which the four single-lane dest masks
    // blend whole lanes and issue one 16-byte store instead of a 4-byte `_mm_store_ss` — and four
    // paired A/Bs read +0.732 / +0.476 / +0.553 / +0.627 ms/f (mean +0.597, sign 4/4 SLOWER).
    // WHY, and this is the durable half: G485's win was four narrow stores of CONSTANTS collapsing
    // into one wide store, which needs no input. A single-lane write cannot be widened without
    // READING the destination's other three lanes, so the candidate turns a pure store into a
    // load-modify-store — it puts a 16-byte load of `vf[fd]` on the dependency chain (and holds
    // `old` live across the blend) to avoid a forwarding block that costs less. The
    // store-forwarding class does NOT generalise from "write lanes individually" to "any narrow
    // store"; it needs the wide replacement to be input-free. The copy, its arm and the whole
    // `wideDest` plumbing were REMOVED, so this selector and the pair-loop body are
    // source-identical to G485's. The full record is at `g428StoreDest` in
    // vu1_g421_fast_upper.inc. Do not rebuild it.
    // G485 REFUTED the register-form G139 pair hazard (DC2_G485_HAZREG). It WAS measurable — a
    // copy in which the pointer form is textually absent, layered on the promoted body, which is
    // the isolation G482 recorded as impossible. Four paired A/Bs read +0.395 / +0.277 / −0.088 /
    // +0.344 ms/f (mean +0.232, sign 3/4 SLOWER). Removing the aliasing does not pay for the three
    // loop-live values it adds (an index and two __m128) to a register-starved loop — the fifth
    // confirmation of the G427/G442/G479/G483 live-value law. G435's ~4.9 ms/f premise came from
    // DC2_G435_SKIP=2, a CORRUPTING deletion probe, which G444/G479 say is not a price at all.
    // The copy, its arm and its accessor were removed; the #if scaffolding in
    // vu1_g435_pair_loop.inc remains so one edit can re-instantiate it if the loop's register
    // pressure ever changes. Do not rebuild it before that.
    {
        // G457: a copy that was never selected proves nothing — but this is the DEFAULT path now,
        // so the proof must stay quiet unless asked for. DC2_G485_STAT=1 prints it for the default
        // path; any DC2_G419_AB run prints it automatically.
        static const bool s_g484Stat = (std::getenv("DC2_G485_STAT") != nullptr) ||
                                       (std::getenv("DC2_G419_AB") != nullptr);
        if (s_g484Stat)
        {
            static std::atomic<uint32_t> s_g484Once{0u};
            if (s_g484Once.fetch_add(1u, std::memory_order_relaxed) == 0u)
                std::fprintf(stderr, "[G484:armed] widevf0=%d fixed=%d leanfetch=%d\n",
                             g484WideVf0 ? 1 : 0, g443Fixed ? 1 : 0, g479LeanFetch ? 1 : 0);
        }
    }

    // G483: prologue/loop/epilogue split (DC2_G483_RUNPROF=1). Three clock reads per KICK, never
    // per pair; the default path evaluates one cached bool.
    const bool g483RunProf = dc2_g483_run_prof_on();
    const uint64_t g483T1 = g483RunProf ? (uint64_t)std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0ull;

        // G435: copies of the same loop body (vu1_g435_pair_loop.inc). The lean copy is
        // the identical body with the four never-executed G295 diagnostic regions removed by the
        // preprocessor. Selected only when no per-cycle probe can fire. G443 adds a third copy
        // of the lean body whose lever predicates are compile-time constants.
        if (g443Fixed && g479LeanFetch && g483PcReg && g483VerifyPc)
        {
            // G483 ORACLE COPY: the candidate body with the per-pair assertion that the carried
            // PC equals `m_state.pc` (DC2_G483_VERIFY=1). Never a timing arm.
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 1
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
#define DC2_G483_PCREG 1
#define DC2_G483_VERIFY_PCREG 1
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed && g479LeanFetch && !g479Verify && g483PcReg)
        {
            // G483 CANDIDATE COPY: identical to the promoted fixed+fused body below, with the
            // program counter carried in a loop-carried local across the back edge instead of
            // reloaded from `m_state.pc`. A separate COPY (rather than a per-pair select) because
            // an in-binary selector inside the loop pollutes BOTH arms (G436).
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 1
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
#define DC2_G483_PCREG 1
#define DC2_G483_VERIFY_PCREG 0
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed && g479LeanFetch && g479Verify)
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 1
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 1
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g484WideVf0)
        {
            // G485 PROMOTED COPY (built G484): identical to the fixed+fused body below, except
            // that the VF0 re-pin at the bottom of the pair writes ONE 16-byte store of (0,0,0,1)
            // instead of four 4-byte scalar stores. This is the DEFAULT path; the body below is
            // now the DC2_G485_NO_WIDE_VF0=1 rollback. Value-identical bytes; the next pair's
            // 16-byte `_mm_loadu_ps(vf[FS])` over the same address can then be satisfied by
            // store-to-load forwarding instead of waiting for four narrow stores to retire.
            // A separate COPY (not an in-loop select) because an in-binary selector inside the
            // loop pollutes BOTH arms (G436).
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 1
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
#define DC2_G483_PCREG 0
#define DC2_G483_VERIFY_PCREG 0
#define DC2_G484_WIDEVF0 1
#include "vu1_g435_pair_loop.inc"
#undef DC2_G484_WIDEVF0
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed && g479LeanFetch)
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 1
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed && g475TraceRequested() && g475VerifyOn())
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 1
#define DC2_G475_VERIFY_TRACE 1
#define DC2_G479_FUSED 0
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed && g475TraceRequested())
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 1
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 0
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g443Fixed)
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 1
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 0
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else if (g435LeanLoop)
        {
#define DC2_VU1_LOOP_DIAG 0
#define DC2_G443_FIXED 0
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 0
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }
        else
        {
#define DC2_VU1_LOOP_DIAG 1
#define DC2_G443_FIXED 0
#define DC2_G475_TRACE 0
#define DC2_G475_VERIFY_TRACE 0
#define DC2_G479_FUSED 0
#define DC2_G479_QPREG 0
#define DC2_G479_VERIFY_FUSED 0
// G483: every copy that does not explicitly ask for the register-carried PC gets the historical
// `m_state.pc` reload, byte-for-byte as before.
#ifndef DC2_G483_PCREG
#define DC2_G483_PCREG 0
#endif
#ifndef DC2_G483_VERIFY_PCREG
#define DC2_G483_VERIFY_PCREG 0
#endif
#include "vu1_g435_pair_loop.inc"
#undef DC2_G483_VERIFY_PCREG
#undef DC2_G483_PCREG
#undef DC2_G479_VERIFY_FUSED
#undef DC2_G479_QPREG
#undef DC2_G479_FUSED
#undef DC2_G475_VERIFY_TRACE
#undef DC2_G475_TRACE
#undef DC2_G443_FIXED
#undef DC2_VU1_LOOP_DIAG
        }

    const uint64_t g483T2 = g483RunProf ? (uint64_t)std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0ull;

    // The hardware drains EFU (and FDIV/FMAC) when an E-bit program ends. Preserve an in-flight
    // P only across max-cycle suspension/resume; a normal microprogram completion publishes it.
    if (s_ppipeOn && m_state.ebit && *s_pDelayP > 0)
    {
        m_state.p = s_vuPPending;
        *s_pDelayP = 0;
    }

    // G428: the architectural m_state.mac is stale while the run defers classification. Publish
    // the newest entry's real value once per run() (~1243 runs/frame, i.e. ~0.1% of the pairs the
    // eager path classified) so anything outside this loop that inspects VU1State sees the same
    // MAC the legacy path would have left. STATUS is not reconstructible and is provably dead
    // while the gate holds (`statusReaderSlots == 0`).
    // Carry the pair counter forward unconditionally so stamps stay monotonic across MSCALs on
    // this thread, whatever representation each run used.
    s_g430PairBase = g430Base + (int64_t)cycle;
    if (g430Hist)
    {
        // Publish the architectural MAC once per run() (~1243 runs/frame).
        m_state.mac = g430ResolveAt(g430Base + (int64_t)cycle);
        if (g430Shadow)
        {
            const uint64_t ok = s_g430VerifyOk.load(std::memory_order_relaxed);
            static thread_local uint64_t s_g430LastReport = 0ull;
            if (ok - s_g430LastReport >= 5000000ull)
            {
                s_g430LastReport = ok;
                std::fprintf(stderr, "[G430:verify] ok=%llu bad=0\n",
                             (unsigned long long)ok);
            }
        }
    }
    else if (g428Defer && !s_g428Verify)
    {
        int g428Slot = *s_headP;
        for (int i = 0; i < s_macpipeDepth; ++i)
        {
            if (s_g428Kind[g428Slot] != G428_MK_CARRY)
                break;
            g428Slot = (g428Slot + 1 < s_macpipeDepth) ? g428Slot + 1 : 0;
        }
        m_state.mac = g428ResolveSlot(g428Slot);
    }

    // G483: publish this run's pair-loop iteration count for [G441:kick]'s resume/geometry split.
    g_g483LastRunCycles = (uint64_t)cycle;
    // `stopReason` is one of exactly three string literals set in this TU; the first character
    // discriminates them ("maxcycles" / "pc-oob" / "ebit-latched") without a strcmp.
    g_g483LastStopReason = (stopReason[0] == 'e') ? 2u : ((stopReason[0] == 'p') ? 1u : 0u);
    if (g483RunProf)
    {
        const uint64_t g483T3 = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        dc2_g483_run_prof_note(g483T1 - g483T0, g483T2 - g483T1, g483T3 - g483T2,
                               (uint64_t)cycle);
    }

    if (g425Prof)
    {
        s_g425.runNs += g425NowNs() - g425RunT0;
        ++s_g425.runs;
        g425ReportMaybe();
    }

    if (g410CompiledRun)
        g410ReportMaybe();

    if (g421On)
    {
        ++s_g421.runs;
        g421ReportMaybe();
    }
#undef DC2_G421_RUN_UPPER
#undef DC2_G422_RUN_LOWER

    if (vu1_trace_enabled())
    {
        const uint32_t n = s_g22Vu1StopCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 128u)
        {
            std::fprintf(stderr,
                         "[G22:vu1stop] n=%u startPc=0x%x cycles=%u reason=%s pc=0x%x lastPc=0x%x lastLower=0x%08x lastUpper=0x%08x ebit=%u itop=0x%x\n",
                         n, startPc, cycle, stopReason, m_state.pc, lastPc, lastLower, lastUpper,
                         m_state.ebit ? 1u : 0u, m_state.itop);
        }
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
