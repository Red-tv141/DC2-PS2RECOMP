// G435 - the VU1 instruction-pair loop body, shared by the LEGACY and the LEAN loop.
//
// The loop is included TWICE inside VU1Interpreter::run():
//   * DC2_VU1_LOOP_DIAG 1 - the historical body, with the four G295 diagnostic regions
//     present. Reached whenever any per-cycle probe can fire (g295Diag), and it is also the
//     control arm while the lean loop is opt-in.
//   * DC2_VU1_LOOP_DIAG 0 - the same body with those four regions removed by the
//     preprocessor. Every executed statement is IDENTICAL; only never-executed code is gone.
//
// WHY. G421's own census note measured ~3 ms/frame of default-path slowdown from inlining the
// (never-executed) census histogram into this loop, i.e. this loop is sensitive to its own
// instruction footprint, not just to the ops it runs. The four G295 regions are ~1050 lines of
// code that `g295Diag` makes unreachable on every normal run, yet they sit between the hot
// statements and stretch the loop across many more cache lines than it needs.
//
// EXACTNESS. The removed regions are exactly the blocks already guarded by `if (g295Diag)`,
// plus the one local that only they read. The lean copy is only ever selected when g295Diag is
// false, in which case those blocks provably do nothing.
//
// G443 - DC2_G443_FIXED 1 selects a THIRD copy in which every loop-invariant lever predicate is
// a `constexpr` shadow of the run()-scope value instead of a runtime local. Nothing about the
// body changes: the shadows are only reachable through `g443Fixed`, which is the conjunction of
// exactly these equalities, so inside this block each name provably already held the constant it
// is replaced by. See the g443Fixed comment in vu1_upper_opcodes.inc for the premise (G427/G442
// priced ONE added loop-carried local at +2.6..2.8 ms/f, and /FAsc found the loop at ~1.2 IPC
// with 36 spills, i.e. on a register-pressure cliff).
// G485 - REGISTER-FORM G139 PAIR HAZARD as a compile-time property of the copy. When 1, the raw
// `float *g139dst` into m_state.vf and the address-taken `g139old`/`g139new` stack arrays are
// TEXTUALLY ABSENT from this copy, which is the whole point: G435 measured the hazard block at
// ~4.9 ms/f of VU1 busy while its own predicate fires on a small minority of pairs, i.e. almost
// all of that cost is the aliasing those three names force on every m_state.vf access in the
// inline upper AND the inline lower, on all ~1.65 M pairs/frame. A runtime `if (g435HazReg)`
// cannot remove it (the declarations still exist); only a separate copy can. Must be defined
// BEFORE the DC2_G443_FIXED block below, which reads it into a constexpr shadow.
//
// G485 BUILT that copy and REFUTED the lever. Four paired A/Bs (DC2_G419_AB=hazreg, both arms
// confirmed used) read +0.395 / +0.277 / -0.088 / +0.344 ms/f: mean +0.232, sign 3/4 SLOWER. The
// aliasing IS removed, and it does not pay for the three loop-live values the register form adds
// (an index and two __m128) to a loop G483 proved register-starved — the fifth confirmation of
// the G427/G442/G479/G483 live-value law. Note also that G435's ~4.9 ms/f premise came from
// DC2_G435_SKIP=2, a CORRUPTING deletion probe, which G444/G479 say is not a price at all.
// This scaffolding is kept (every arm below is byte-identical at DC2_G485_HAZREG 0) so ONE edit
// in vu1_upper_opcodes.inc can re-instantiate the copy. Do not do so until something has actually
// changed the loop's register pressure. It also corrects G482's record: g435HazReg was never
// structurally unmeasurable, only unmeasured through the wrong flag.
#ifndef DC2_G485_HAZREG
#define DC2_G485_HAZREG 0
#define G485_LOCAL_DEFAULT 1
#endif
#if DC2_G443_FIXED
#pragma warning(push)
#pragma warning(disable : 4456 4457 4459)
    // --- G410 / dispatch ---------------------------------------------------------------
    constexpr bool g410CompiledRun  = true;
    constexpr bool g410VerifyRun    = false;
    constexpr bool g410CountStats   = false;
    // --- census / verifiers ------------------------------------------------------------
    constexpr bool g421On           = false;
    constexpr bool g421VerifyRun    = false;
    constexpr bool g422VerifyRun    = false;
    constexpr bool g426VerifyRun    = false;
    constexpr bool s_g425Verify     = false;
    constexpr bool s_g425VuNan      = false;
    constexpr bool s_g426VerifyPredecode = false;
    constexpr bool s_g427Verify     = false;
    constexpr bool s_g428Verify     = false;
    constexpr bool s_g328Verify     = false;
    constexpr bool s_g328Stat       = false;
    constexpr bool g489Verify       = false;
    // --- inline execution --------------------------------------------------------------
    constexpr bool g421FastUpperRun = true;
    constexpr bool g422FastLowerRun = true;
    constexpr bool g426FusedRun     = false;
    // --- correctness machinery (all default-ON hardware models) -------------------------
    constexpr bool s_qlatOn            = true;
    constexpr bool s_ppipeOn           = true;
    constexpr bool s_scalarPrestallOn  = true;
    constexpr bool s_pairhazOn         = true;
    constexpr int  s_macpipeDepth      = 4;
    constexpr bool s_g328RingOn        = true;
    // --- promoted representations -------------------------------------------------------
    constexpr bool g425Lean         = true;
    constexpr bool g426Predecode    = true;
    constexpr bool g427Lazy         = false;   // superseded by G428/G430
    constexpr bool g428Defer        = true;
    constexpr bool g428DeferPark    = true;
    constexpr bool g430MacPipeOn    = true;
    constexpr bool g430Hist         = true;
    constexpr bool g430Shadow       = false;
    constexpr bool g436Lean         = true;
    constexpr bool g436FlagStore    = false;
    // --- opt-in / never-promoted substrates ---------------------------------------------
    // G485: the register form of the G139 pair hazard is now a COMPILE-TIME property of the copy
    // (DC2_G485_HAZREG), not a runtime lever. G482 recorded `g435HazReg` as structurally
    // unmeasurable because `!g435HazReg` is a conjunct of `g443Fixed`, so the old flag selected a
    // different, slower loop. It is measurable the same way G484 made the wide VF0 store
    // measurable: a copy of THIS body in which the pointer form is textually absent.
    constexpr bool g435HazReg       = (DC2_G485_HAZREG != 0);
    constexpr bool g435WideVf0      = false;
    constexpr bool s_g295KeepHaz    = false;
    constexpr bool s_g106ParallelFlags = false;
    // --- ceiling probes (knowingly-incorrect arms, all off) ------------------------------
    constexpr bool g430SkipUpper    = false;
    constexpr bool g430SkipHaz      = false;
    constexpr bool g435SkipRepin    = false;
    constexpr bool g435SkipHaz      = false;
#pragma warning(pop)
#endif // DC2_G443_FIXED
    // G484 - WIDE VF0 RE-PIN inside the promoted loop. Every copy that does not explicitly ask for
    // it keeps the historical four-scalar-store form, byte-for-byte as before. See the
    // g484WideVf0 comment in vu1_upper_opcodes.inc for why the old DC2_G435_WIDE_VF0 flag could
    // never measure this.
#ifndef DC2_G484_WIDEVF0
#define DC2_G484_WIDEVF0 0
#define G484_LOCAL_DEFAULT 1
#endif
    // G486 built and REFUTED a copy of this body in which the four single-lane FMAC destination
    // masks blend whole lanes and issue one 16-byte store instead of `_mm_store_ss` (mean
    // +0.597 ms/f, sign 4/4 SLOWER). The plumbing was removed so this body is source-identical to
    // G485's; the refutation and its mechanism are recorded at `g428StoreDest` in
    // vu1_g421_fast_upper.inc. Do not rebuild it — a single-lane write cannot be widened without
    // reading the destination back, so the wide form is a load-modify-store, not a store.
    // G479 - the Q/P delay counters as LOOP LOCALS.
    //
    // Both counters are read-modify-written on EVERY pair through a hoisted thread_local pointer,
    // so each pair pays a load, a compare and (on expiry) a store to the same address as the
    // previous pair — a store-to-load forward on the loop's own carried state. The ONLY writer
    // outside this loop body is execLower (the FDIV/EFU producers and WAITQ/WAITP at
    // vu1_dispatch_and_sync.inc:688-1091, all reached through g239_stage_p / g239_wait_p), which
    // this arm brackets with an explicit spill/reload; nothing in g422FastLower, g421FastUpper or
    // execUpper touches them. Every loop exit writes both back before the run() tail reads
    // `*s_pDelayP`, so the architectural state at the E-bit drain is unchanged.
    //
    // This ADDS two loop-carried locals to a loop G427/G442 priced at +2.6..2.8 ms/f for one — but
    // it REMOVES the memory they replace, which is the opposite side of that trade.
    //
    // RESULT: REJECTED (G479). Stacked on the fused record in the same loop copy it took the
    // paired blocked estimator from −0.724 (t = −3.09) / −0.586 (t = −1.92) to −0.178 (t = −0.73),
    // i.e. it gave back essentially the whole fused-fetch win. The G427/G442 live-value law holds:
    // two more loop-carried locals cost more than the two per-pair loads they delete. Compiled out
    // (`DC2_G479_QPREG 0` in every copy) and kept only as this record — do not rebuild it.
#if DC2_G479_QPREG
    int g479Qd = *s_qDelayP;
    int g479Pd = *s_pDelayP;
#define G479_QD g479Qd
#define G479_PD g479Pd
#define G479_QP_SYNC() do { *s_qDelayP = g479Qd; *s_pDelayP = g479Pd; } while (0)
#define G479_QP_RELOAD() do { g479Qd = *s_qDelayP; g479Pd = *s_pDelayP; } while (0)
#else
#define G479_QD (*s_qDelayP)
#define G479_PD (*s_pDelayP)
#define G479_QP_SYNC() ((void)0)
#define G479_QP_RELOAD() ((void)0)
#endif
    // G483 - CARRY THE PROGRAM COUNTER ACROSS THE BACK EDGE.
    //
    // The /MAP + capstone audit of the current binary (G483) reads the loop as:
    //     top:    mov ecx, [rax+0x25c]        ; currentPc = m_state.pc
    //             ...
    //     tail:   mov [rbx+0x25c], eax        ; m_state.pc = nextPC
    //             jb  top
    // i.e. every pair ENDS by storing the PC to VU1State and BEGINS by loading the same four bytes
    // back — a store-to-load forward on the loop's own carried state, and G442 already named that
    // load as the head of the loop's critical chain ("the bounds check, the two fetches and the
    // sidecar load all hang off the m_state.pc load"). Nothing runs between the store and the
    // reload, so the value is provably identical; the load exists only because MSVC must assume
    // the callees in the body alias `m_state`.
    //
    // THE STORE STAYS. execLower/g422FastLower/execUpper read and WRITE m_state.pc (that is how a
    // branch lower reports its target — the body reads it back as `postExecPc`), so VU1State must
    // stay authoritative through the body. Only the reload at the top is removed.
    //
    // EXACTNESS. `g483Pc` is written at exactly the sites that write `m_state.pc` as the LAST word
    // on the value for this iteration (the `m_state.pc = nextPC` at the bottom, and the G475 trace
    // block's post-run store, which is the only other place a pair can leave the body with a new
    // PC). Every exit path leaves `m_state.pc` written as before. DC2_G483_VERIFY=1 asserts
    // `g483Pc == m_state.pc` at the top of every pair.
    //
    // This is NOT G479's rejected Q/P-locals lever. That ADDED two loop-carried locals to keep two
    // counters in registers while their memory stayed live; this REPLACES a load with a value the
    // loop already computes into a register at the bottom, so it adds no new live range.
#if DC2_G483_PCREG
    uint32_t g483Pc = m_state.pc;
#define G483_PC_CUR (g483Pc)
#define G483_PC_SET(v) do { g483Pc = (v); } while (0)
#else
#define G483_PC_CUR (m_state.pc)
#define G483_PC_SET(v) ((void)(v))
#endif
    for (; cycle < maxCycles; ++cycle)
    {
        const uint32_t currentPc = G483_PC_CUR;
#if DC2_G483_PCREG && DC2_G483_VERIFY_PCREG
        if (g483Pc != m_state.pc)
        {
            std::fprintf(stderr, "[G483:verify-BAD] pcreg=0x%x m_state.pc=0x%x cycle=%u\n",
                         g483Pc, m_state.pc, cycle);
            std::abort();
        }
#endif
#if DC2_VU1_LOOP_DIAG
        const int16_t g72_vi1_before = g425Lean ? (int16_t)0 : (int16_t)m_state.vi[1];
#endif // DC2_VU1_LOOP_DIAG
#if DC2_VU1_LOOP_DIAG
        // G295 diag region A (pre-fetch): G216 census, G100/G101/G102/G106/G108 title force
        // path, G75/G85/G87 probes. Interior indentation intentionally unchanged (see the
        // master-gate comment above the loop).
        if (g295Diag)
        {
        if (s_g216CharExec && currentPc == 0x1d48u)
        {
            ++s_g216Stats.gate1d48;
            if ((uint16_t)m_state.vi[1] == (uint16_t)m_state.vi[10]) ++s_g216Stats.taken1d48;
        }
        if (s_g216CharExec && currentPc == 0x1f68u)
        {
            ++s_g216Stats.gate1f68;
            if ((uint16_t)m_state.vi[7] == (uint16_t)m_state.vi[10]) ++s_g216Stats.taken1f68;
        }
        if (s_g216CharExec && currentPc == 0x2128u)
        {
            G216ClipStats &st = s_g216Stats;
            const float *p = m_state.vf[16];
            if (st.count == 0u)
            {
                for (int lane = 0; lane < 4; ++lane)
                    st.minv[lane] = st.maxv[lane] = p[lane];
            }
            else
            {
                for (int lane = 0; lane < 4; ++lane)
                {
                    st.minv[lane] = std::min(st.minv[lane], p[lane]);
                    st.maxv[lane] = std::max(st.maxv[lane], p[lane]);
                }
            }
            bool finite = true;
            for (int lane = 0; lane < 4; ++lane)
                finite = finite && std::isfinite(p[lane]);
            if (!finite) ++st.nonFinite;
            if (p[3] <= 0.0f) ++st.qNonPositive;
            // G215 byte-matched these character-RTT guard limits against PCSX2.
            if (p[0] < 1766.4f || p[0] > 2329.6f ||
                p[1] < 1819.2f || p[1] > 2276.8f)
                ++st.outsideGuard;
            if ((uint16_t)m_state.vi[1] == (uint16_t)m_state.vi[10]) ++st.gateTaken;
            if (st.sampleCount < 4u)
            {
                G216ClipSample &sample = st.samples[st.sampleCount++];
                std::memcpy(sample.p, p, sizeof(sample.p));
                sample.vi1 = (uint16_t)m_state.vi[1];
                sample.vi10 = (uint16_t)m_state.vi[10];
            }
            ++st.count;
        }
        if (s_g216ForceGate && s_g216CharExec &&
            (currentPc == 0x1d48u || currentPc == 0x2128u))
            m_state.vi[1] = m_state.vi[10];
        // G72 force-draw experiment: the tristrip(0x1d48)/trifan(0x2128) packer cull gates
        // (IBEQ VI10,VI1) are never taken (VI10=208, VI1 in {0,1,3,23,...}) -> +2048 ADC=1 on
        // 100% of verts -> title flat blue. Force VI1:=VI10 just before the gate so the branch
        // is taken (skip +2048 = draw) to confirm the over-cull is the flat-blue root. Env
        // DC2_G72_FORCEDRAW; startPc==0x10 only; diagnostic, default off.
        // G100: the decision is hoisted to g100ForceTitleDraw (title-scoped, default-ON).
        static const bool s_g106NoForce1d48 = (std::getenv("DC2_G106_NO_FORCE_1D48") != nullptr);
        static const bool s_g106NoForce2128 = (std::getenv("DC2_G106_NO_FORCE_2128") != nullptr);
        const bool g106GateForceEnabled =
            !((currentPc == 0x1d48u && s_g106NoForce1d48) ||
              (currentPc == 0x2128u && s_g106NoForce2128));
        if (g100ForceTitleDraw &&
            g106GateForceEnabled &&
            (currentPc == 0x1d48u || currentPc == 0x2128u))
        {
            // G101 probe: dump the candidate position VFs at the gate so we can find which one
            // holds the transformed screen position and its on-screen range (to drive a SELECTIVE
            // force = HW per-vertex ADC). DC2_G101_VFPROBE, title-scoped, first 80 gate hits.
            static const bool s_vfprobe = (std::getenv("DC2_G101_VFPROBE") != nullptr);
            if (s_vfprobe)
            {
                static std::atomic<uint32_t> s_vn{0};
                const uint32_t k = s_vn.fetch_add(1u, std::memory_order_relaxed);
                if (k < 2000u)
                {
                    const float *p = m_state.vf[16];
                    std::fprintf(stderr, "[G101:vf] pc=0x%x x=%.0f y=%.0f z=%.0f q=%.4f\n",
                                 currentPc, p[0], p[1], p[2], p[3]);
                }
            }
            // G101 (selective per-vertex ADC = HW near-plane clip): the G100 forced draw skipped the
            // +2048/ADC cull for EVERY vertex, including verts BEHIND the camera (projected q=1/W<=0),
            // which map to flipped/extreme screen positions and swing wildly during the title camera
            // zoom -- the "vertex explosion". Force the draw ONLY for verts in FRONT of the camera;
            // leave behind-camera verts to the natural +2048 -> ADC=1 -> strip restart, exactly like
            // HW's near-plane clip. The projected vertex is VF16=(x,y,z,q); q=VF16.w. A stable binary
            // criterion (no screen-space threshold flicker) that removes only non-visible behind-
            // camera geometry (no new holes). DC2_G101_NO_QCLIP restores the G100 unconditional force;
            // DC2_G101_QMIN tunes the near-plane epsilon.
            static const bool s_noQClip = (std::getenv("DC2_G101_NO_QCLIP") != nullptr);
            static const float s_qmin = []() -> float {
                const char *e = std::getenv("DC2_G101_QMIN");
                return e ? static_cast<float>(std::atof(e)) : 0.0f;
            }();
            static const uint32_t s_g106Force2128Mac = []() -> uint32_t {
                const char *e = std::getenv("DC2_G106_FORCE_2128_MAC");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
            }();
            static const bool s_g108Qclip2128 = (std::getenv("DC2_G108_2128_QCLIP") != nullptr);
            static const bool s_g108Qclip2128Stat = (std::getenv("DC2_G108_2128_Q_STAT") != nullptr);
            float *cur = m_state.vf[16];
            const float qcur = cur[3];
            bool drawIt = false;
            // G102 stat: classify each gate vertex. cat0=front (drawn), cat1=behind clipped to
            // the near plane (drawn), cat2=behind clamped (no anchor, bounded+culled),
            // cat3=behind, no handling. DC2_G102_STAT prints every 20000.
            static const bool s_g102stat = (std::getenv("DC2_G102_STAT") != nullptr);
            static std::atomic<uint32_t> s_g102cat[4];
            int g102cat = 3;
            if (currentPc == 0x2128u && s_g106Force2128Mac != 0xFFFFFFFFu)
            {
                drawIt = ((m_state.mac & 0xFFFFu) == (s_g106Force2128Mac & 0xFFFFu));
                g102cat = drawIt ? 0 : 3;
            }
            else if (s_noQClip || (s_g104triClip && !(s_g108Qclip2128 && currentPc == 0x2128u)))
            {
                drawIt = true; // G100/G104 unconditional force draw; rasterizer clips q<=0 triangles.
                g102cat = (qcur > s_qmin) ? 0 : 3;
            }
            else if (qcur > s_qmin)
            {
                drawIt = true; // G101: vertex in front of the near plane
                g102cat = 0;
            }
            else if (s_g102clip && qcur < 0.0f)
            {
                // G102: this vertex is BEHIND the near plane, so its projected screen position is
                // exploded (q=1/W<0). Even if we cull its own kicking triangle, a tristrip reuses
                // it in the next 1-2 triangles, so the raw exploded position MUST be replaced or
                // it swims. Clip it ONTO the near plane along the edge to the nearest in-front
                // vertex (the immediate predecessor if it is in front, else the strip's most
                // recent in-front vertex) -> a finite, frame-stable position that keeps coverage.
                const float *anchor = nullptr;
                if (g_g102PrevValid && g_g102PrevScreen[3] > s_qmin)
                    anchor = g_g102PrevScreen;        // true adjacent edge (most accurate)
                else if (g_g102LastFrontValid)
                    anchor = g_g102LastFront;         // fallback: fills both-behind runs
                if (anchor)
                {
                    // screen = clip/W, so clip = screen*W with W = 1/q; W is clip-space-linear,
                    // interpolate it to W=Wnear, solve t, re-divide. cur[i]*Wc == clipC is finite
                    // (huge*tiny) so the math is stable despite the exploded screen position.
                    const float Wa = 1.0f / anchor[3];
                    const float Wc = 1.0f / qcur;        // < 0 (behind)
                    const float Wn = s_g102wnear;        // near-plane W (> 0)
                    const float denom = Wc - Wa;
                    float t = (denom != 0.0f) ? (Wn - Wa) / denom : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        const float clipA = anchor[i] * Wa;
                        const float clipC = cur[i] * Wc;
                        const float clipN = clipA + t * (clipC - clipA);
                        cur[i] = clipN / Wn;             // back to screen space
                    }
                    cur[3] = 1.0f / Wn;                  // q at the near plane (> 0)
                    drawIt = true;
                    g102cat = 1;
                }
                else
                {
                    // No in-front anchor yet (strip opens behind the camera). Cannot clip, but a
                    // bounded clamp keeps the position finite/stable (no swimming) instead of the
                    // raw 1e6 explosion; leave it culled (do not force draw).
                    cur[0] = (cur[0] < -2048.0f) ? -2048.0f : (cur[0] > 2560.0f) ? 2560.0f : cur[0];
                    cur[1] = (cur[1] < -2048.0f) ? -2048.0f : (cur[1] > 2560.0f) ? 2560.0f : cur[1];
                    g102cat = 2;
                }
                // Clamp any G102-modified screen XY into the guard band so the GS scissor (not the
                // guard-band cull) does the final on-screen clip -> the clipped triangle fills to
                // the screen edge instead of being guard-culled into a new hole.
                cur[0] = (cur[0] < -1000.0f) ? -1000.0f : (cur[0] > 1512.0f) ? 1512.0f : cur[0];
                cur[1] = (cur[1] < -1000.0f) ? -1000.0f : (cur[1] > 1432.0f) ? 1432.0f : cur[1];
                // Loop 2 (gate 0x2128) FTOI4s VF16 into the emit reg VF17 BEFORE this gate, so
                // also patch the already-computed VF17 = FTOI4(screen) = screen*16 (xy). Loops
                // that FTOI after the gate (0x1d48) re-derive VF17 from the patched VF16
                // (harmless). z/w left to the microcode.
                m_state.vf[17][0] = cur[0] * 16.0f;
                m_state.vf[17][1] = cur[1] * 16.0f;
            }
            if (s_g108Qclip2128Stat && currentPc == 0x2128u)
            {
                static std::atomic<uint64_t> s_g108Front{0};
                static std::atomic<uint64_t> s_g108Cull{0};
                static std::atomic<uint64_t> s_g108Tick{0};
                if (drawIt)
                    s_g108Front.fetch_add(1u, std::memory_order_relaxed);
                else
                    s_g108Cull.fetch_add(1u, std::memory_order_relaxed);
                const uint64_t t = s_g108Tick.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (t <= 24u || (t % 20000ull) == 0ull)
                    std::fprintf(stderr,
                                 "[G108:q2128] n=%llu q=% .6g draw=%u front=%llu cull=%llu\n",
                                 static_cast<unsigned long long>(t), qcur,
                                 static_cast<unsigned>(drawIt ? 1u : 0u),
                                 static_cast<unsigned long long>(s_g108Front.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(s_g108Cull.load(std::memory_order_relaxed)));
            }
            if (s_g102stat)
            {
                s_g102cat[g102cat].fetch_add(1u, std::memory_order_relaxed);
                static std::atomic<uint32_t> s_g102tick{0};
                if ((s_g102tick.fetch_add(1u, std::memory_order_relaxed) % 20000u) == 0u)
                    std::fprintf(stderr, "[G102:stat] front=%u clip=%u clamp=%u none=%u wnear=%.2f\n",
                                 s_g102cat[0].load(), s_g102cat[1].load(),
                                 s_g102cat[2].load(), s_g102cat[3].load(), s_g102wnear);
            }
            // Record this vertex (clipped/clamped or raw) as the predecessor for the next edge,
            // and track the most recent in-front vertex as the both-behind clip anchor.
            g_g102PrevScreen[0] = cur[0];
            g_g102PrevScreen[1] = cur[1];
            g_g102PrevScreen[2] = cur[2];
            g_g102PrevScreen[3] = cur[3];
            g_g102PrevValid = true;
            if (cur[3] > s_qmin)
            {
                g_g102LastFront[0] = cur[0];
                g_g102LastFront[1] = cur[1];
                g_g102LastFront[2] = cur[2];
                g_g102LastFront[3] = cur[3];
                g_g102LastFrontValid = true;
            }
            if (drawIt)
                m_state.vi[1] = m_state.vi[10];
        }
        // G75 route experiment: the title-batch dispatcher (0x0708..) routes to the +2048-gated
        // packers (0x1c50 tristrip etc.) because VI7=4&VI1 and VI8=1&VI1 are both nonzero (VI1 =
        // qword38 selector = 0x17). G74 proved those gated packers emit 100% ADC (never-takeable
        // gate) -> flat blue, while the no-gate packer 0x1b68 COPIES the EE input's per-vertex ADC
        // flag (ILWR VI7,(VI12) @0x1b98 -> &0x8000 -> ISWR @0x1bd8) and so would reproduce HW's
        // per-vertex strip-restart ADC. Force VI7=0 @0x0730 and VI8=0 @0x07b0 to route every
        // startPc=0x10 batch through 0x1b68 and test whether the rock then renders. Run with
        // DC2_G72_DISP to confirm p0x1b68 goes nonzero. Env DC2_G75_ROUTE1B68; diagnostic, off by default.
        {
            static const bool s_g75route = (std::getenv("DC2_G75_ROUTE1B68") != nullptr);
            if (s_g75route && startPc == 0x10u)
            {
                if (currentPc == 0x0730u) m_state.vi[7] = 0;
                else if (currentPc == 0x07b0u) m_state.vi[8] = 0;
            }
        }
        // G85: pin the directional N·L magnitude. The title rock green is the directional
        // light's genuine colour (0,80,125) (R=0) applied as ambient + colour*clamp(N·L);
        // the runner's G/B are ~2.7x HW so the N·L is too large. Dump the transformed normal
        // VF25 (+|N|^2) at pc=0x16f8 and the clamped directional dot VF26 at pc=0x1828 for the
        // first rock vertices. Env DC2_G85_NL; startPc==0x10 only; quiet by default.
        {
            static const bool s_g85nl = (std::getenv("DC2_G85_NL") != nullptr);
            if (s_g85nl && startPc == 0x10u)
            {
                static std::atomic<uint32_t> s_g85nlN{0};
                if (currentPc == 0x16f8u)
                {
                    const uint32_t k = s_g85nlN.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 24u)
                    {
                        const float *n = m_state.vf[25];
                        const float nlen2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
                        std::fprintf(stderr, "[G85:nl] k=%u N=(% .4g % .4g % .4g) |N|^2=% .4g\n",
                                     k, n[0], n[1], n[2], nlen2);
                        std::fflush(stderr);
                    }
                }
                else if (currentPc == 0x1828u)
                {
                    static std::atomic<uint32_t> s_g85vfN{0};
                    const uint32_t k = s_g85vfN.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 24u)
                    {
                        const float *c = m_state.vf[26];
                        std::fprintf(stderr, "[G85:dir] k=%u VF26(dir N.L clamped)=(% .4g % .4g % .4g % .4g)\n",
                                     k, c[0], c[1], c[2], c[3]);
                        std::fflush(stderr);
                    }
                }
            }
        }
        // G87: isolate the POINT-LIGHT contribution to the per-vertex colour. G86 proved the
        // title green is a per-vertex R-deficit (runner R<ambient 46.5) and the only R source is
        // the point light (qw49=(75,75,60)); directional light0=(0,80,125) has R=0. Capture, for
        // the first rock vertices: VF26 (ambient+directional, pc=0x18a0), the point term
        // VF24(N.L)/VF29(colour*atten)/VF31(atten) (pc=0x1970), and the FINAL stored colour VF17
        // (pc=0x19e8, the SQI). If VF26.R~46 but VF17.R<46, the point step is wrong/negative.
        // Env DC2_G87_PL; startPc==0x10 only; quiet by default.
        {
            static const bool s_g87 = (std::getenv("DC2_G87_PL") != nullptr);
            if (s_g87 && startPc == 0x10u)
            {
                if (currentPc == 0x18a0u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *c = m_state.vf[26];
                        std::fprintf(stderr, "[G87:ambdir] k=%u VF26=(% .3g % .3g % .3g % .3g)\n",
                                     k, c[0], c[1], c[2], c[3]); std::fflush(stderr); }
                }
                else if (currentPc == 0x1970u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *nl=m_state.vf[24],*ca=m_state.vf[29],*at=m_state.vf[31];
                        std::fprintf(stderr, "[G87:point] k=%u VF24nl=(% .3g % .3g % .3g % .3g) "
                                     "VF29ca=(% .3g % .3g % .3g) VF31at=(% .3g % .3g % .3g % .3g)\n",
                                     k, nl[0],nl[1],nl[2],nl[3], ca[0],ca[1],ca[2],
                                     at[0],at[1],at[2],at[3]); std::fflush(stderr); }
                }
                else if (currentPc == 0x19e8u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *c = m_state.vf[17];
                        std::fprintf(stderr, "[G87:final] k=%u VF17=(% .3g % .3g % .3g % .3g)\n",
                                     k, c[0], c[1], c[2], c[3]); std::fflush(stderr); }
                }
            }
        }
        } // G295 diag region A end
#endif // DC2_VU1_LOOP_DIAG

        if (currentPc + 8 > codeSize)
        {
            stopReason = "pc-oob";
            G479_QP_SYNC();
            break;
        }

        uint32_t lower, upper;
        G410CompiledUpper *g410Pair = nullptr;
#if DC2_G443_FIXED
        // G443: `g443Fixed` includes `g410Pairs != nullptr`, so the sidecar is present on every
        // pair of this arm. Stating it removes the null test at the fetch AND folds the four
        // `g410Pair != nullptr ? ... : <recompute from the raw words>` ternaries below (E-bit,
        // I-bit, the G239 prestall S2 and the legacy G139 dest decode) to their cached side.
        __assume(g410Pairs != nullptr);
#endif
#if DC2_G479_FUSED
        // G479: ONE 16-byte record, ONE address chain. `fused[i].source` is bit-identical to the
        // live `vuCode + pc` word (precise invalidation, same writer as `sources[i]`) and
        // `fused[i].pair` to `uppers[i]`, so this decodes exactly what the split fetch above does.
        {
            const G479FusedPair &g479fp = g479Fused[currentPc >> 3u];
            const uint64_t g479src = g479fp.source;
            lower = static_cast<uint32_t>(g479src);
            upper = static_cast<uint32_t>(g479src >> 32u);
            g410Pair = const_cast<G410CompiledUpper *>(&g479fp.pair);
#if DC2_G479_VERIFY_FUSED
            uint64_t g479live = 0u;
            std::memcpy(&g479live, vuCode + currentPc, sizeof(g479live));
            if (g479live != g479src ||
                std::memcmp(g410Pair, &g410Pairs[currentPc >> 3u], sizeof(*g410Pair)) != 0)
            {
                std::fprintf(stderr,
                             "[G479:verify-BAD] pc=0x%x fused=%016llx live=%016llx\n",
                             currentPc, (unsigned long long)g479src,
                             (unsigned long long)g479live);
                std::abort();
            }
            g479NoteVerified();
#endif
        }
#else
        if (g410CountStats)
            g410Pair = &g410FetchPair(
                g410Pairs, vuCode, currentPc, g410CountStats, g410VerifyRun,
                lower, upper);
        else
        {
            std::memcpy(&lower, vuCode + currentPc, 4);
            std::memcpy(&upper, vuCode + currentPc + 4, 4);
            if (g410Pairs != nullptr)
                g410Pair = &g410Pairs[currentPc >> 3u];
        }
#endif
        if (!g425Lean)
        {
            lastPc = currentPc;
            lastLower = lower;
            lastUpper = upper;
        }

        if (g421On)
            // G488: read the arm from the CACHE, never from the accessor (G487's rule), so the
            // census reports the dispatch that RAN. With no cache there is no compiled upper
            // either, and `decodeFastOp` is false, so the shadow kinds it returns are exactly the
            // right report: every upper on that path goes through execUpper.
            g421NoteCycle(currentPc,
                          g410Pair != nullptr
                              ? g410Pair->upperKind
                              : g410SelectUpperKind(upper, s_g410Cache.decodeFastOp),
                          upper, lower,
                          pendingBranchTarget == DC2_G442_NO_BRANCH && !m_state.ebit);

#if DC2_G475_TRACE
        // G475: this source-side run length is maintained beside G410's precisely invalidated
        // pair cache. Branch-delay entries and a latched E-bit must still execute one pair in
        // the main loop, so they fail closed here. maxCycles is also an architectural suspension
        // edge; truncate the trace rather than stepping past it.
        if (pendingBranchTarget == DC2_G442_NO_BRANCH && !m_state.ebit)
        {
            const uint32_t g475Avail =
                static_cast<uint32_t>(s_g410Cache.g475RunLen[currentPc >> 3u]);
            const uint32_t g475Budget = maxCycles - cycle;
            const uint32_t g475Count = g475Avail < g475Budget ? g475Avail : g475Budget;
            if (g475Count >= 2u)
            {
#if DC2_G475_VERIFY_TRACE
                // Candidate: compact trace with G422 fast lowers. Reference: the exact old
                // per-pair order with legacy execLower. Safe trace pairs have no upper work,
                // scalar producer/wait, flag consumer, branch, E-bit or I-bit, so Q/P clocks,
                // lower execution, zero-register enforcement and sequential PC are the complete
                // visible transition. G430 history is stamp-only and `cycle` advances below.
                s_g422VerBefore = m_state;
                const bool g475HasMem = vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE;
                if (g475HasMem)
                    std::memcpy(s_g422VerMemBefore, vuData, dataSize);
                const int g475QBefore = *s_qDelayP;
                const int g475PBefore = *s_pDelayP;

                g475ExecuteVnopTrace(m_state, currentPc >> 3u, g475Count,
                                     vuData, dataSize, s_qDelayP, s_pDelayP);
                s_g422VerFast = m_state;
                if (g475HasMem)
                    std::memcpy(s_g422VerMemFast, vuData, dataSize);
                const int g475QFast = *s_qDelayP;
                const int g475PFast = *s_pDelayP;

                m_state = s_g422VerBefore;
                if (g475HasMem)
                    std::memcpy(vuData, s_g422VerMemBefore, dataSize);
                *s_qDelayP = g475QBefore;
                *s_pDelayP = g475PBefore;
                const uint32_t g475First = currentPc >> 3u;
                for (uint32_t g475j = 0u; g475j < g475Count; ++g475j)
                {
                    const size_t g475idx = static_cast<size_t>(g475First + g475j);
                    const G410CompiledUpper &g475pair = s_g410Cache.uppers[g475idx];
                    const uint64_t g475source = s_g410Cache.sources[g475idx];
                    const uint32_t g475lower = static_cast<uint32_t>(g475source);
                    const uint32_t g475upper = static_cast<uint32_t>(g475source >> 32u);
                    m_state.pc = static_cast<uint32_t>(g475idx * 8u);
                    if (*s_qDelayP > 0 && --(*s_qDelayP) == 0)
                        m_state.q = s_vuQPending;
                    if (*s_pDelayP > 0 && --(*s_pDelayP) == 0)
                        m_state.p = s_vuPPending;
                    execLower(g475lower, vuData, dataSize, gs, memory, g475upper);
                    if ((g475pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                    {
                        m_state.vf[0][0] = 0.0f;
                        m_state.vf[0][1] = 0.0f;
                        m_state.vf[0][2] = 0.0f;
                        m_state.vf[0][3] = 1.0f;
                        m_state.vi[0] = 0;
                    }
                }
                const uint32_t g475Next = g475First + g475Count;
                m_state.pc = g475Next < s_g410Cache.uppers.size() ? g475Next * 8u : 0u;

                const bool g475BadState = !g422StatesEqual(s_g422VerFast, m_state);
                const bool g475BadMem = g475HasMem &&
                    std::memcmp(s_g422VerMemFast, vuData, dataSize) != 0;
                const bool g475BadPipe = *s_qDelayP != g475QFast || *s_pDelayP != g475PFast;
                if (g475BadState || g475BadMem || g475BadPipe)
                {
                    std::fprintf(stderr,
                                 "[G475:verify-BAD] pc=0x%x count=%u state=%d mem=%d "
                                 "pipe=%d q=%d/%d p=%d/%d next=%x/%x\n",
                                 currentPc, g475Count, static_cast<int>(g475BadState),
                                 static_cast<int>(g475BadMem), static_cast<int>(g475BadPipe),
                                 g475QFast, *s_qDelayP, g475PFast, *s_pDelayP,
                                 s_g422VerFast.pc, m_state.pc);
                    std::abort();
                }

                m_state = s_g422VerFast;
                if (g475HasMem)
                    std::memcpy(vuData, s_g422VerMemFast, dataSize);
                *s_qDelayP = g475QFast;
                *s_pDelayP = g475PFast;
                const uint64_t g475OldVerified =
                    s_g475Verified.fetch_add(g475Count, std::memory_order_relaxed);
                if (g475OldVerified == 0u ||
                    g475OldVerified / 10000000u !=
                        (g475OldVerified + g475Count) / 10000000u)
                    std::fprintf(stderr, "[G475:verify] pairs=%llu bad=0\n",
                                 static_cast<unsigned long long>(g475OldVerified + g475Count));
#else
                g475ExecuteVnopTrace(m_state, currentPc >> 3u, g475Count,
                                     vuData, dataSize, s_qDelayP, s_pDelayP);
#endif
                g475NoteTrace(g475Count);
                // G483: the trace block is the only other path that leaves the body with a new PC
                // (it `continue`s past the bottom's store), so re-sync the carried copy here.
                G483_PC_SET(m_state.pc);
                cycle += g475Count - 1u;
                continue;
            }
        }
#endif // DC2_G475_TRACE

#ifndef DC2_G490_BLOCKRUN
#define DC2_G490_BLOCKRUN 0
#define G490_LOCAL_DEFAULT 1
#endif
#ifndef DC2_G490_VERIFY_BLOCK
#define DC2_G490_VERIFY_BLOCK 0
#define G490_VERIFY_LOCAL_DEFAULT 1
#endif
#if DC2_G490_BLOCKRUN
        // G490 - WIDENED BLOCK RUN. Pay the per-pair ENVELOPE (G489: 54.7% of all VU1 time) ONCE
        // per run of eligible pairs instead of once per pair. Unlike G475's VNOP-only trace this
        // covers every upper class the inlined g421FastUpper executes, which is the class a block
        // translator would emit. Eligibility (vu1_g490_block_run.inc) admits a pair only when the
        // machinery this body performs around the two slots is provably inert for it, so the run
        // executes exactly: Q advance, P advance, upper, lower, VF0/VI0 re-pin, pc += 8.
        //
        // Fails closed on a pending branch delay slot and on a latched E-bit, exactly like G475:
        // both must step ONE pair through the main body. `maxCycles` is an architectural
        // suspension edge, so the run is truncated to the remaining budget rather than stepping
        // past it.
        if (pendingBranchTarget == DC2_G442_NO_BRANCH && !m_state.ebit)
        {
            const uint32_t g490Avail =
                static_cast<uint32_t>(s_g410Cache.g490RunLen[currentPc >> 3u]);
            const uint32_t g490Budget = maxCycles - cycle;
            uint32_t g490Count = g490Avail < g490Budget ? g490Avail : g490Budget;
#if DC2_G532_FLAGHEAD
            // G532: a typed flag consumer followed by an ordinary run enters the SAME block call
            // that the next pair would already have made. This scoped prologue adds no live state
            // across the normal block dispatch. Sidecar value 3 is G531's precisely-invalidated
            // eligibility proof; the next slot's run length proves an existing block follows.
            if (g490Avail == 0u && (g499BlkArm == 25 || g499BlkArm == 26) &&
                g490Budget > G490_MIN_RUN && currentPc + 16u <= codeSize &&
                s_g410Cache.g502TailAt[currentPc >> 3u] == G531_FLAG_TAIL)
            {
                const uint32_t g532BlockPc = currentPc + 8u;
                const uint32_t g532Avail = static_cast<uint32_t>(
                    s_g410Cache.g490RunLen[g532BlockPc >> 3u]);
                const uint32_t g532Budget = g490Budget - 1u;
                const uint32_t g532Count =
                    g532Avail < g532Budget ? g532Avail : g532Budget;
                if (g532Count >= G490_MIN_RUN)
                {
                    // Preserve the following block's existing G500/G531 tail. A budget-clamped
                    // ordinary run cannot reach its tail, exactly as in the normal hook below.
                    uint32_t g532Tail = 0u;
                    uint32_t g532BPc = 0u;
                    if (g532Count == g532Avail)
                    {
                        uint32_t g532T = static_cast<uint32_t>(
                            s_g410Cache.g502TailAt[g532BlockPc >> 3u]);
                        if (g532T != 0u)
                        {
                            g532BPc = g532BlockPc + 8u * g532Avail;
                            if (g532BPc + 8u > codeSize)
                                g532T = 0u;
                            else
                            {
                                if (g532T == 2u && g532BPc + 16u > codeSize)
                                    g532T = 1u;
                                const uint32_t g532TailPairs0 =
                                    (g532T == G531_FLAG_TAIL) ? 1u : g532T;
                                if (g532Avail + g532TailPairs0 > g532Budget)
                                    g532T = 0u;
                            }
                            g532Tail = g532T;
                        }
                    }
                    const uint32_t g532TailPairs =
                        (g532Tail == G531_FLAG_TAIL) ? 1u : g532Tail;
                    const uint32_t g532Consumed = 1u + g532Count + g532TailPairs;

#if DC2_G490_VERIFY_BLOCK
                    g490VerifyBlock(vuCode, g532BlockPc >> 3u, g532Count);
                    if (g532Tail != 0u && g532Tail != G531_FLAG_TAIL)
                        g500VerifyTail(vuCode, g532BPc >> 3u, g532Tail);
#endif
#if DC2_G506_QPSEG
                    const bool g532QpVerify = g506VerifyOn();
                    const int g532QEntry = g532QpVerify ? *s_qDelayP : 0;
                    const int g532PEntry = g532QpVerify ? *s_pDelayP : 0;
#endif
                    if (g499BlkArm == 26)
                        g532ExecuteBlockHeadVerify(
                            m_state, g532BlockPc >> 3u, g532Count, vuData, dataSize,
                            s_qDelayP, s_pDelayP, g430Base + static_cast<int64_t>(cycle) + 1,
                            g436Hb, g532Tail);
                    else
                        g532ExecuteBlockHeadFlag(
                            m_state, g532BlockPc >> 3u, g532Count, vuData, dataSize,
                            s_qDelayP, s_pDelayP, g430Base + static_cast<int64_t>(cycle) + 1,
                            g436Hb, g532Tail);
#if DC2_G506_QPSEG
                    if (g532QpVerify)
                        g506VerifyBlockQp(g532QEntry, g532PEntry, g532Consumed,
                                          *s_qDelayP, *s_pDelayP, g499BlkArm);
#endif

                    uint32_t g532Next;
                    bool g532Taken = false;
                    if (g532Tail == 0u)
                        g532Next = g532BlockPc + 8u * g532Count;
                    else if (g532Tail == G531_FLAG_TAIL)
                        g532Next = g532BPc + 8u;
                    else
                    {
                        g532Taken = (m_state.pc != g532BPc);
                        if (g532Tail == 2u)
                            g532Next = g532Taken ? (m_state.pc + 8u) : (g532BPc + 16u);
                        else
                        {
                            if (g532Taken)
                            {
                                pendingBranchTarget = m_state.pc + 8u;
                                if (pendingBranchTarget >= codeSize)
                                    pendingBranchTarget = 0u;
                            }
                            g532Next = g532BPc + 8u;
                        }
                    }
                    if (g532Next >= codeSize)
                        g532Next = 0u;
                    if (g500Stat)
                        g500NoteBlock(g500InvArm, g532Count + 1u,
                                      g532TailPairs, g532Taken);
                    if (g502Census)
                        g502NoteBlock(g532Count + 1u, g532TailPairs, g532Taken,
                                      g532Count + 1u, g532Next,
                                      maxCycles - (cycle + g532Consumed),
                                      pendingBranchTarget != DC2_G442_NO_BRANCH, codeSize);
                    m_state.pc = g532Next;
                    G483_PC_SET(g532Next);
                    if (g490Stat)
                        g490NoteBlock(g532Consumed);
                    cycle += g532Consumed - 1u;
                    continue;
                }
            }
#endif
            // G502 CEILING PROBE (`DC2_G419_AB=blksplit`) — it has ANSWERED and is COMPILED OUT
            // (`DC2_G502_SPLIT_PROBE 0`), so `g502Split` is a `constexpr 0` and this folds away.
            // It cut every block whose clamped length reaches 2*MIN_RUN exactly in half, so the SAME
            // pairs ran in the SAME order with bit-identical coverage and only the number of trips
            // round this loop changed (`g490RunLen` decreases by exactly one per slot inside a run,
            // so the second half's own run length is `>= MIN_RUN` and forms a block of its own; the
            // G500 tail attaches to that second half because `g490Count == g490Avail` is false for
            // the first). Four paired runs, +0.360 ms/f mean over 190,653 extra round trips/frame:
            // **A BLOCK ROUND TRIP COSTS 1.89 ns**, which bounds every envelope lever ever proposed
            // for this loop at 158 k × 1.89 = 0.30 ms/f. See the fix log §3.
            if (g502Split && g490Count >= 2u * G490_MIN_RUN)
                g490Count = (g490Count + 1u) / 2u;
            // G500: may this block also execute the CONTROL TRANSFER that ends the run, and
            // that branch's delay slot? Three conditions, all cheap and all once per BLOCK:
            //   * the whole eligible run fits the cycle budget (`g490Count == g490Avail`), so
            //     the pair after it really is the branch and not a budget truncation;
            //   * the tail slots are inside `codeSize` (the pair loop would otherwise have
            //     stopped on `pc-oob`, and the wrap below has to match its sequential wrap);
            //   * the tail itself fits the remaining budget.
            //
            // G502 `blktail0` — REFUTED and COMPILED OUT (`DC2_G502_BLKTAIL0 0`), so `g502Tail0` is
            // a `constexpr 0` and both guards below fold back to the G500 shape exactly. The shape
            // is left here because the refutation is worth reading at the site: admitting a block on
            // `count + tail` instead of on `count` would have captured the 4.15% of all pairs
            // (`[G502:excl]`: branch 2.10% + their delay slots 2.05%) that G500's tail leaves
            // outside — the very population G500 measured at 7.0 ns/pair — and it measured
            // **+0.121 then +0.082 ms/f, slower 4/4 on each of two links**, with coverage doing
            // exactly what it was designed to do (89.00% → 93.31%). **A block does not save the
            // pair-loop iteration it is ENTERED from**, so a new 2-pair block is worth
            // `2 × envelope − 1.89 ns` and that is ≤ 0. See the fix log §6.
            uint32_t g500Tail = 0u;
            uint32_t g500BPc = 0u;
            bool g500Taken = false;
            // G504: arm 6 is arm 3's body with the ring cursor in a local, so it must form the SAME
            // tails — keying this on `== 3` alone would have given the two arms different work and
            // made the paired gate meaningless. (G501 §6's law, one level up from an instrument:
            // never key BEHAVIOUR on one arm value when a later phase adds another arm to the same
            // family.) The test is loop-invariant and present on both arms, so it cancels.
            // G506: arm 7 joins the same family for the same reason, and this is the THIRD time the
            // law has fired here. It was caught by `[G490:block] cover=76.98% avg=7.80` — the
            // pre-G500 figures — on the very first oracle run, i.e. by an invariant, not by the
            // gate. **Any new arm on this body must be added here in the same edit that adds it.**
            // G514: arms 8 (whole-vector store), 10 (+ whole-vector load) and 9 (their oracle) are
            // arm 7's body and join the same family. FOURTH firing of the law, obeyed in the edit
            // that added the arms — cross-checked by `[G490:block] cover=88.99% avg=9.15` being
            // unchanged on every one of them.
            // G515: arm 12 (the NOP peel) is arm 10's body and joins the same family. FIFTH firing,
            // obeyed in the edit that adds the arm — same cross-check.
            // G516: arms 13, 14 and 15 (the decode-time dispatch, each side and both) are arm 12's
            // body and join the same family. SIXTH firing, obeyed in the edit that adds the arms —
            // same cross-check (`[G490:block] cover=88.99% avg=9.15` must be unchanged on each).
            // G517: arms 19, 20, 21 and 22 (the strength-reduced pair loop, each half, both, and
            // its oracle) are arm 18's body and join the same family. SEVENTH firing of the law,
            // obeyed in the edit that adds the arms — same cross-check (`[G490:block] cover=88.99%
            // avg=9.15` must be unchanged on each).
            if ((g499BlkArm == 3 || g499BlkArm == 6 || g499BlkArm == 7 || g499BlkArm == 8 ||
                 g499BlkArm == 9 || g499BlkArm == 10 || g499BlkArm == 12 || g499BlkArm == 13 ||
                 g499BlkArm == 14 || g499BlkArm == 15 || g499BlkArm == 16 ||
                 g499BlkArm == 17 || g499BlkArm == 18 || g499BlkArm == 19 ||
                 g499BlkArm == 20 || g499BlkArm == 21 || g499BlkArm == 22 ||
                 g499BlkArm == 23 || g499BlkArm == 24 || g499BlkArm == 27 ||
                 g499BlkArm == 28 || g499BlkArm == 29
#if DC2_G532_FLAGHEAD
                 || g499BlkArm == 25 || g499BlkArm == 26
#endif
                 ) &&
                g490Count == g490Avail &&
                (g490Count >= G490_MIN_RUN || g502Tail0))
            {
                // G502: ONE byte load at the slot already being indexed. `g502TailAt[i]` IS
                // `g500Tail[i + g490RunLen[i]]`, pre-joined by g490RebuildRunLengths, and it is
                // zero for 87% of hook entries under `blktail0` — so the address arithmetic and
                // both clamps below run only when there really is a tail.
                uint32_t g500T =
                    static_cast<uint32_t>(s_g410Cache.g502TailAt[currentPc >> 3u]);
                const bool g531FlagArm =
                    (g499BlkArm == 23 || g499BlkArm == 24 || g499BlkArm == 27 ||
                     g499BlkArm == 28 || g499BlkArm == 29
#if DC2_G532_FLAGHEAD
                     || g499BlkArm == 25 || g499BlkArm == 26
#endif
                     );
                if (g500T == G531_FLAG_TAIL && !g531FlagArm)
                    g500T = 0u;
                if (g500T != 0u)
                {
                    g500BPc = currentPc + 8u * g490Avail;
                    if (g500BPc + 8u > codeSize)
                        g500T = 0u;
                    else
                    {
                        if (g500T == 2u && g500BPc + 16u > codeSize)
                            g500T = 1u;
                        const uint32_t g531TailPairs =
                            (g500T == G531_FLAG_TAIL) ? 1u : g500T;
                        if (g490Avail + g531TailPairs > g490Budget)
                            g500T = 0u;
                    }
                    g500Tail = g500T;
                }
            }
            // Sidecar value 3 denotes one G531 flag-consumer pair, not three branch-tail pairs.
            // Keep one normalized count for every caller-side budget/stat/cycle calculation.
            const uint32_t g531TailPairs =
                (g500Tail == G531_FLAG_TAIL) ? 1u : g500Tail;
            // `g500Tail != 0` already implies at least one pair, so a lone control transfer forms
            // a one-pair block: it saves the ~7 ns/pair envelope and costs one 1.89 ns round trip.
            // A lone ELIGIBLE pair (`count == 1`, `tail == 0`) is deliberately NOT admitted — G490's
            // realised rate for a plain pair is 0.45 ns, which does not pay for the round trip.
            if (g490Count >= G490_MIN_RUN || (g502Tail0 && g500Tail != 0u))
            {
#if DC2_G490_VERIFY_BLOCK
                // A SEPARATE COPY, selected by DC2_G490_VERIFY=1, exactly like the G483 oracle
                // copy — never a timing arm. G489 §6's rule (a verifier must not sit in a copy its
                // own flag deselects) is satisfied by construction: the flag selects THIS copy.
                g490VerifyBlock(vuCode, currentPc >> 3u, g490Count);
                if (g500Tail != 0u && g500Tail != G531_FLAG_TAIL)
                    g500VerifyTail(vuCode, g500BPc >> 3u, g500Tail);
#endif
                // G499: which block executor. The bodies differ ONLY inside the (noinline) block —
                // the pair loop's own text is unchanged apart from these loop-invariant,
                // perfectly-predicted branches, which are taken once per BLOCK (~160 k/frame
                // against ~1.62 M pairs), not once per pair, and are present in both arms so they
                // cancel in the paired estimator.
#if DC2_G506_QPSEG
                // G506 ORACLE (DC2_G506_VERIFY=1, default off, CALLER side). The lever rests on one
                // claim: no block-eligible pair can RE-ARM either scalar-pipe counter, so inside a
                // block both can only count down. If that holds, the value each counter is left
                // with is exactly `max(entry - pairsConsumed, 0)`; a lower that armed one would
                // leave 7, 13, 18 or 24 there instead, and a mistake in the segmentation would
                // leave anything else. Kept OUT of the leaf deliberately — holding the entry values
                // live across the block is the extra-live-value cost that grew G504's frame
                // (G483's law), and the caller has them for free. One memoised bool per BLOCK.
                // Declared here, ABOVE the whole dispatch chain, so it never lands between an
                // `else` and its statement whatever combination of the arm #ifs is compiled in.
                const bool g506Verify = g506VerifyOn();
                const int g506QEntry = g506Verify ? *s_qDelayP : 0;
                const int g506PEntry = g506Verify ? *s_pDelayP : 0;
#endif
#if DC2_G502_BLKCHAIN
                // G502: the CHAIN arm returns only when the next slot would not have formed a
                // block anyway, so everything the fold below computes is computed inside the leaf
                // instead — once per chain rather than once per block. Nothing else about this
                // iteration differs, which is why `[G500:inv]`'s every column but `calls=` is an
                // invariant across the two arms.
                if (g499BlkArm == 5)
                {
                    G502ChainOut g502o{};
                    g502ExecuteBlockChain(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                          s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb,
                                          g500Tail, g502o, codeSize, g490Budget);
                    if (g502o.pendingValid)
                        pendingBranchTarget = g502o.pendingTarget;
                    m_state.pc = g502o.nextPc;
                    G483_PC_SET(g502o.nextPc);
                    if (g500Stat)
                        g500NoteChain(g500InvArm, g502o.segments, g502o.plain, g502o.tail1,
                                      g502o.tail2, g502o.taken);
                    if (g490Stat)
                        g490NoteBlocks(g502o.segments, g502o.consumed);
                    cycle += g502o.consumed - 1u;
                    continue;
                }
#endif
#if DC2_G499_QPLOCAL
                if (g499BlkArm == 2)
                    g499ExecuteBlockFastQp(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                           s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb);
                else
#endif
#if DC2_G506_QPSEG
                // G514: arm 8 is the shipped default (arm 7's body with the whole-vector VU store);
                // arm 9 is its ORACLE, reachable only via DC2_G514_VERIFY=1.
#if DC2_G517_INDARM
                // G517: REFUTED (see the DC2_G517_INDARM note in vu1_g490_block_run.inc). These four
                // are compiled out, so the shipped dispatch below is byte-identical to G516's.
                if (g499BlkArm == 21)
                    g517ExecuteBlockTailInd(m_state, currentPc >> 3u, g490Count, vuData,
                                            dataSize, s_qDelayP, s_pDelayP,
                                            g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 22)
                    g517ExecuteBlockTailVerify(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 20)
                    g517ExecuteBlockTailStampInd(m_state, currentPc >> 3u, g490Count, vuData,
                                                 dataSize, s_qDelayP, s_pDelayP,
                                                 g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 19)
                    g517ExecuteBlockTailPtrInd(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else
#endif
                // G533 v8: arm 27's single exact block bypasses G531 here. Every miss was
                // canonicalised to arm 23 once per run(), so it follows the shipped call path
                // below with no candidate wrapper/function-return tax.
                if (g533StaticRun &&
                    g533Hot3088Runnable(currentPc >> 3u, g490Count, g500Tail,
                                         vuData, dataSize, g436Hb))
                    g533ExecuteHot3088(m_state, vuData, s_qDelayP, s_pDelayP,
                                       g430Base + (int64_t)cycle, g436Hb);
                else if (g499BlkArm == 29)
                    g533ExecuteBlockTailStat(m_state, currentPc >> 3u, g490Count, vuData,
                                              dataSize, s_qDelayP, s_pDelayP,
                                              g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 28)
                    g533ExecuteBlockTailVerify(m_state, currentPc >> 3u, g490Count, vuData,
                                                dataSize, s_qDelayP, s_pDelayP,
                                                g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 24
#if DC2_G532_FLAGHEAD
                    || g499BlkArm == 26
#endif
                    )
                    g531ExecuteBlockTailVerify(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 23
#if DC2_G532_FLAGHEAD
                         || g499BlkArm == 25
#endif
                         )
                    g531ExecuteBlockTailFlag(m_state, currentPc >> 3u, g490Count, vuData,
                                             dataSize, s_qDelayP, s_pDelayP,
                                             g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 18)
                    g516ExecuteBlockTailDisp(m_state, currentPc >> 3u, g490Count, vuData,
                                             dataSize, s_qDelayP, s_pDelayP,
                                             g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 17)
                    g516ExecuteBlockTailPeel4(m_state, currentPc >> 3u, g490Count, vuData,
                                              dataSize, s_qDelayP, s_pDelayP,
                                              g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 16)
                    g516ExecuteBlockTailVerify(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
#if DC2_G516_UPDESC
                else if (g499BlkArm == 15)
                    g516ExecuteBlockTailDispDense(m_state, currentPc >> 3u, g490Count, vuData,
                                                  dataSize, s_qDelayP, s_pDelayP,
                                                  g430Base + (int64_t)cycle, g436Hb, g500Tail);
#endif
                else if (g499BlkArm == 14)
                    g516ExecuteBlockTailLowDense(m_state, currentPc >> 3u, g490Count, vuData,
                                                 dataSize, s_qDelayP, s_pDelayP,
                                                 g430Base + (int64_t)cycle, g436Hb, g500Tail);
#if DC2_G516_UPDESC
                else if (g499BlkArm == 13)
                    g516ExecuteBlockTailUpDesc(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
#endif
                else if (g499BlkArm == 12)
                    g515ExecuteBlockTailNopPeel(m_state, currentPc >> 3u, g490Count, vuData,
                                                dataSize, s_qDelayP, s_pDelayP,
                                                g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 9)
                    g514ExecuteBlockTailVerify(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 10)
                    g514ExecuteBlockTailVMem(m_state, currentPc >> 3u, g490Count, vuData,
                                             dataSize, s_qDelayP, s_pDelayP,
                                             g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 8)
                    g514ExecuteBlockTailVStore(m_state, currentPc >> 3u, g490Count, vuData,
                                               dataSize, s_qDelayP, s_pDelayP,
                                               g430Base + (int64_t)cycle, g436Hb, g500Tail);
                else if (g499BlkArm == 7)
                    g506ExecuteBlockTailQpSeg(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                              s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle,
                                              g436Hb, g500Tail);
                else
#endif
#if DC2_G504_HISTCUR
                if (g499BlkArm == 6)
                    g504ExecuteBlockTailCur(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                            s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb,
                                            g500Tail);
                else
#endif
#if DC2_G500_BLKBRANCH
                if (g499BlkArm == 3)
                    g500ExecuteBlockTail(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                         s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb,
                                         g500Tail);
                else
#endif
                if (g499BlkArm == 1)
                    g499ExecuteBlockFast(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                         s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb);
                else
                    g490ExecuteBlock(m_state, currentPc >> 3u, g490Count, vuData, dataSize,
                                     s_qDelayP, s_pDelayP, g430Base + (int64_t)cycle, g436Hb);
#if DC2_G506_QPSEG
                if (g506Verify)
                    g506VerifyBlockQp(g506QEntry, g506PEntry, g490Count + g531TailPairs,
                                      *s_qDelayP, *s_pDelayP, g499BlkArm);
#endif
                uint32_t g490Next;
                if (g500Tail == 0u)
                {
                    // Sequential by construction: eligibility excludes every lower that can write
                    // pc (G489's complete branch/jump set), so this is `currentPc + 8 * count`
                    // with the same wrap the tail applies.
                    g490Next = currentPc + 8u * g490Count;
                }
                else if (g500Tail == G531_FLAG_TAIL)
                {
                    // G531 flag consumers cannot write PC. The typed tail consumed exactly one
                    // sequential pair, so no pending branch state is created.
                    g490Next = g500BPc + 8u;
                }
                else
                {
                    // G500: the pair loop's own branch protocol, applied ONCE to the tail instead
                    // of once per pair. `m_state.pc` different from the branch pair's own PC is
                    // exactly the tail's `m_state.pc != currentPc` test, so the two arms agree
                    // even on the degenerate `B 0` encoding that test treats as not-taken.
                    g500Taken = (m_state.pc != g500BPc);
                    if (g500Tail == 2u)
                    {
                        // The delay slot ran inside the block, so the pending target is consumed
                        // here: taken -> the branch target, not taken -> past the delay slot.
                        g490Next = g500Taken ? (m_state.pc + 8u) : (g500BPc + 16u);
                    }
                    else
                    {
                        // The delay slot is not block-eligible; hand it back to the pair loop with
                        // the pending target the loop's own tail would have latched.
                        if (g500Taken)
                        {
                            pendingBranchTarget = m_state.pc + 8u;
                            if (pendingBranchTarget >= codeSize)
                                pendingBranchTarget = 0u;
                        }
                        g490Next = g500BPc + 8u;
                    }
                }
                if (g490Next >= codeSize)
                    g490Next = 0u;
                if (g500Stat)
                    g500NoteBlock(g500InvArm, g490Count, g531TailPairs, g500Taken);
                // G502: block-entry census. Once per BLOCK, out of line, behind a bool cached once
                // per run() — `chain=` is what a chaining executor could have kept, `split=` is the
                // denominator the ceiling probe's frame delta is divided by.
                if (g502Census)
                    g502NoteBlock(g490Count, g531TailPairs, g500Taken,
                                  g490Avail < g490Budget ? g490Avail : g490Budget,
                                  g490Next, maxCycles - (cycle + g490Count + g531TailPairs),
                                  pendingBranchTarget != DC2_G442_NO_BRANCH, codeSize);
                m_state.pc = g490Next;
                G483_PC_SET(g490Next);
                if (g490Stat)
                    g490NoteBlock(g490Count + g531TailPairs);
                cycle += (g490Count + g531TailPairs) - 1u;
                continue;
            }
        }
        // G502: control reaches here only when this pair is about to be executed by the PAIR LOOP
        // rather than by a block, so this call counts exactly the ~11% remainder and classifies it
        // in g490BlockPairEligible's own test order. DYNAMIC by construction — a static slot tally
        // in g490RebuildRunLengths cannot answer this, because the hot loop's slots execute orders
        // of magnitude more often than the cold ones (the roadmap's own warning on task 2). One
        // test of a loop-invariant bool plus a never-taken call (G421 law 17).
        if (g502Census)
        {
            const uint32_t g502NextAvail =
                (currentPc + 16u <= codeSize)
                    ? static_cast<uint32_t>(s_g410Cache.g490RunLen[(currentPc >> 3u) + 1u])
                    : 0u;
            g502NotePlain(g410Pair, s_g410Cache.g490RunLen[currentPc >> 3u], g502NextAvail,
                          maxCycles - cycle, pendingBranchTarget != DC2_G442_NO_BRANCH,
                          m_state.ebit);
        }
#endif // DC2_G490_BLOCKRUN

#if DC2_VU1_LOOP_DIAG
        // G295 diag region B (post-fetch): G68/G69/G117/G132/G237/G70 censuses, VU1 XGKICK
        // trace, G32. Interior indentation intentionally unchanged.
        if (g295Diag)
        {

        // G68: ADC-source histogram. The title 3D map renders flat-blue because all its vertices
        // reach the GS with ADC=1 (bit15 of the stored vertex word). Histogram EVERY integer store
        // to VU memory (ISW opHi=0x05, ISWR opHi=0x40/s2=0x3F) by (pc -> bit15 set/clear count) for
        // the model program (startPc=0x10). The pc that writes many bit15=1 vertex words IS the
        // title's ADC source; comparing title vs costume localizes the divergence. Env DC2_G68_ADC.
        if (startPc == 0x10u)
        {
            static const bool s_g68adc = (std::getenv("DC2_G68_ADC") != nullptr);
            if (s_g68adc && vuData)
            {
                const uint32_t opHi68 = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
                // At each XGKICK (opHi 0x40, s2 0x6C), walk the kicked GIF packet in VU memory and
                // tally output-vertex ADC (bit15 of word3 of XYZF2/XYZ2) split by prim type, plus
                // dump the first few raw vertex word3 values so we can see the ACTUAL ADC source.
                if (opHi68 == 0x40u && S2_FUNC(lower) == 0x6Cu)
                {
                    const uint32_t is68 = (lower >> 11u) & 0x1Fu;
                    uint32_t off = (static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is68])) * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                    static std::atomic<uint32_t> s_kick{0}, s_dumped{0};
                    static std::atomic<uint32_t> s_adcSet{0}, s_adcClr{0};
                    const uint32_t kn = s_kick.fetch_add(1u, std::memory_order_relaxed);
                    uint32_t curPrim = 0xFFu; bool dumpThis = (s_dumped.load(std::memory_order_relaxed) < 6u);
                    uint32_t tag = 0u; uint32_t guard = 0u;
                    while (off + 16u <= dataSize && guard++ < 512u)
                    {
                        uint64_t lo64, hi64; std::memcpy(&lo64, vuData + off, 8); std::memcpy(&hi64, vuData + off + 8, 8);
                        const uint32_t nloop = static_cast<uint32_t>(lo64 & 0x7FFFu);
                        const uint32_t flg = static_cast<uint32_t>((lo64 >> 58u) & 3u);
                        uint32_t nreg = static_cast<uint32_t>((lo64 >> 60u) & 0xFu); if (!nreg) nreg = 16u;
                        const bool pre = ((lo64 >> 46u) & 1u) != 0u;
                        if (pre) curPrim = static_cast<uint32_t>((lo64 >> 47u) & 7u);
                        off += 16u;
                        if (flg != 0u) break;                 // only walk PACKED region
                        uint8_t regs[16]; for (uint32_t r = 0; r < nreg; ++r) regs[r] = (hi64 >> (r * 4u)) & 0xFu;
                        for (uint32_t lp = 0; lp < nloop && off + 16u <= dataSize; ++lp)
                            for (uint32_t r = 0; r < nreg; ++r, off += 16u)
                            {
                                if (off + 16u > dataSize) break;
                                const uint8_t rg = regs[r];
                                if (rg == 0x04u || rg == 0x05u) // XYZF2 / XYZ2
                                {
                                    uint64_t vhi; std::memcpy(&vhi, vuData + off + 8u, 8);
                                    const bool adc = ((vhi >> 47u) & 1u) != 0u;
                                    if (curPrim == 3u || curPrim == 4u || curPrim == 5u)
                                    { if (adc) s_adcSet.fetch_add(1u, std::memory_order_relaxed); else s_adcClr.fetch_add(1u, std::memory_order_relaxed); }
                                    if (dumpThis && s_dumped.load(std::memory_order_relaxed) < 6u)
                                    {
                                        const uint32_t w3 = static_cast<uint32_t>(vhi >> 32u);
                                        std::fprintf(stderr, "[G68:vtx] kick=%u prim=%u reg=0x%x adc=%u word3=0x%08x vhi=0x%016llx\n",
                                                     kn, curPrim, (unsigned)rg, (unsigned)(adc ? 1u : 0u), w3, (unsigned long long)vhi);
                                        s_dumped.fetch_add(1u, std::memory_order_relaxed);
                                    }
                                }
                            }
                    }
                    if ((kn % 2000u) == 0u)
                        std::fprintf(stderr, "[G68:adc] kick=%u outVtx ADC set=%u clr=%u\n",
                                     kn, s_adcSet.load(std::memory_order_relaxed), s_adcClr.load(std::memory_order_relaxed));
                }
            }
        }

        // G69: localize the title's per-vertex ADC SOURCE. G68 proved the title's output
        // vertices carry ADC=1 (word3 bit15) at 73% vs the costume's 26% (inverted). word3 is
        // cleanly 0x8000/0x0 (an integer flag), so the ADC word is written by an INTEGER store
        // (ISW opHi=0x05, or ISWR opHi=0x40/s2=0x3F). Histogram every such store by PC, splitting
        // bit15(value) set/clear and recording the dest-lane mask. The PC with many bit15-SET
        // stores into the W lane (dest&1 = word3) IS the ADC source. Reading that PC's microcode
        // (enable DC2_G61_VUDUMP in the same run for a consistent disasm) classifies the bug as
        // EE-data-copied (preceded by an ILWR of the input flag, like the costume) vs VU-computed
        // (preceded by a winding/cull compare). Env DC2_G69_ADC; startPc==0x10 only; quiet default.
        if (startPc == 0x10u)
        {
            static const bool s_g69adc = (std::getenv("DC2_G69_ADC") != nullptr);
            if (s_g69adc)
            {
                // G69c: one-shot dump of the ACTUALLY-EXECUTING title transform (FTOIs land at
                // 0x1d78/0x1f98/0x2158, past the G39 0x1c90 range) so we can see where the binary
                // per-vertex .w flag (0 / 2048.0) originates: an LQ from input data vs a VU clip/cull.
                static std::atomic<uint32_t> s_g69dump{0};
                if (currentPc >= 0x1c00u && currentPc <= 0x2200u &&
                    s_g69dump.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    for (uint32_t a = 0x1b00u; a <= 0x2200u && a + 8u <= codeSize; a += 8u)
                    {
                        uint32_t lo = 0u, up = 0u;
                        std::memcpy(&lo, vuCode + a, 4);
                        std::memcpy(&up, vuCode + a + 4, 4);
                        std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
                    }
                    std::fflush(stderr);
                }
                const uint8_t opHi69 = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
                const bool isIsw  = (opHi69 == 0x05u);
                const bool isIswr = (opHi69 == 0x40u && S2_FUNC(lower) == 0x3Fu);
                if (isIsw || isIswr)
                {
                    const uint8_t it69   = static_cast<uint8_t>((lower >> 16u) & 0x1Fu);
                    const uint8_t dest69 = static_cast<uint8_t>((lower >> 21u) & 0xFu);
                    const uint16_t val69 = static_cast<uint16_t>(m_state.vi[it69] & 0xFFFFu);
                    const bool b15 = (val69 & 0x8000u) != 0u;
                    static std::atomic<uint32_t> s_setC[2048];
                    static std::atomic<uint32_t> s_clrC[2048];
                    static std::atomic<uint32_t> s_destSeen[2048];
                    const uint32_t idx = (currentPc >> 3u) & 2047u;
                    if (b15) s_setC[idx].fetch_add(1u, std::memory_order_relaxed);
                    else     s_clrC[idx].fetch_add(1u, std::memory_order_relaxed);
                    s_destSeen[idx].fetch_or(dest69, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_tick69{0};
                    if ((s_tick69.fetch_add(1u, std::memory_order_relaxed) % 300000u) == 0u)
                    {
                        std::fprintf(stderr, "[G69:storehist] --- integer-store bit15 histogram by pc ---\n");
                        for (uint32_t i = 0; i < 2048u; ++i)
                        {
                            const uint32_t s = s_setC[i].load(std::memory_order_relaxed);
                            const uint32_t c = s_clrC[i].load(std::memory_order_relaxed);
                            if (s + c >= 2000u)
                                std::fprintf(stderr, "[G69:storehist] pc=0x%x dest=0x%x set=%u clr=%u\n",
                                             i << 3u, s_destSeen[i].load(std::memory_order_relaxed), s, c);
                        }
                    }
                }

                // G69b: the title ADC word3 = FTOI4.w of the projected position's .w lane (the
                // fog/clamp term), packed into the XYZF2 word3. The producing pc varies (the title
                // re-uploads VU microcode per map-part), so detect ANY upper FTOI0/4/12/15 that
                // computes the .w lane (dest&1) and tally bit15 of its integer result by pc, plus
                // sample the inputs (the source .w float, the scale, and VF22 clamp consts). This
                // localizes WHY VF18.w saturates to 0x8000 (=ADC=1) for ~73% of title vertices.
                {
                    const uint8_t upOp = static_cast<uint8_t>(upper & 0x3Fu);
                    if (upOp >= 0x3Cu)
                    {
                        const uint8_t vf = S2_FUNC(upper);
                        if (vf >= 0x14u && vf <= 0x17u) // FTOI0/4/12/15
                        {
                            const uint8_t dst = static_cast<uint8_t>((upper >> 21u) & 0xFu);
                            if (dst & 0x1u) // .w lane (= XYZF2 word3 = ADC)
                            {
                                const uint8_t fs = static_cast<uint8_t>((upper >> 11u) & 0x1Fu);
                                const float scale = (vf == 0x14u) ? 1.0f : (vf == 0x15u) ? 16.0f :
                                                    (vf == 0x16u) ? 4096.0f : 32768.0f;
                                const float win = m_state.vf[fs][3];
                                const int32_t wres = (int32_t)(win * scale);
                                const bool b15 = ((wres >> 15) & 1) != 0;
                                static std::atomic<uint32_t> s_fSet[2048];
                                static std::atomic<uint32_t> s_fClr[2048];
                                const uint32_t fidx = (currentPc >> 3u) & 2047u;
                                if (b15) s_fSet[fidx].fetch_add(1u, std::memory_order_relaxed);
                                else     s_fClr[fidx].fetch_add(1u, std::memory_order_relaxed);
                                static std::atomic<uint32_t> s_fs{0};
                                const uint32_t fi = s_fs.fetch_add(1u, std::memory_order_relaxed);
                                if (fi < 30u)
                                {
                                    const float *vfs = m_state.vf[fs];
                                    const float *v29 = m_state.vf[29];
                                    float q59[4] = {0,0,0,0};
                                    if (59u * 16u + 16u <= dataSize)
                                        std::memcpy(q59, vuData + 59u * 16u, 16);
                                    std::fprintf(stderr,
                                        "[G69:ftoi] n=%u pc=0x%x ftoi=0x%x src=VF%u srcW=% .6g scale=%g -> wres=0x%x adc=%u | VFsrc=% .5g % .5g % .5g % .5g VF29=% .5g % .5g % .5g % .5g qw59=% .5g % .5g % .5g % .5g\n",
                                        fi, currentPc, vf, fs, win, scale, (unsigned)wres, (unsigned)(b15 ? 1u : 0u),
                                        vfs[0], vfs[1], vfs[2], vfs[3], v29[0], v29[1], v29[2], v29[3],
                                        q59[0], q59[1], q59[2], q59[3]);
                                }
                                static std::atomic<uint32_t> s_ftick{0};
                                if ((s_ftick.fetch_add(1u, std::memory_order_relaxed) % 300000u) == 0u)
                                {
                                    std::fprintf(stderr, "[G69:ftoihist] --- FTOI .w-lane bit15 by pc ---\n");
                                    for (uint32_t i = 0; i < 2048u; ++i)
                                    {
                                        const uint32_t s = s_fSet[i].load(std::memory_order_relaxed);
                                        const uint32_t c = s_fClr[i].load(std::memory_order_relaxed);
                                        if (s + c >= 2000u)
                                            std::fprintf(stderr, "[G69:ftoihist] pc=0x%x set=%u clr=%u\n", i << 3u, s, c);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // G117: dedicated capture of the TRISTRIP packer emit (FTOI4 VF17,VF26 @ pc=0x2158, the
        // 0x2070 rock-tristrip loop). The shared G69 sampler (s_fs<30) fills with TRI-packer
        // (0x1f80..) samples and never reaches the tristrip emit. Here we sample 0x2158 directly to
        // answer the decisive G117 question: is the tristrip ADC uniform because VF26.w is PINNED?
        // Log the source VF26 (esp .w pre-FTOI4), the input vertex VF16 (esp .w), VF29 (qw59 cull
        // consts), and the resulting ADC bit. If VF16.w / VF26.w are constant across vertices, the
        // selectivity HW shows must come from a per-vertex input (.w) or a non-zero VF29 the runner
        // zeroes. Env DC2_G117_TS; startPc==0x10. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g117ts = (std::getenv("DC2_G117_TS") != nullptr);
            if (s_g117ts && currentPc == 0x2158u)
            {
                const float *v26 = m_state.vf[26];
                const float *v16 = m_state.vf[16];
                const float *v29 = m_state.vf[29];
                const int32_t wres = (int32_t)(v26[3] * 16.0f);
                const uint32_t adc = ((wres >> 15) & 1) != 0 ? 1u : 0u;
                // running min/max of the .w lanes to see if they vary across the strip
                static thread_local float s_v26wMin = 1e30f, s_v26wMax = -1e30f;
                static thread_local float s_v16wMin = 1e30f, s_v16wMax = -1e30f;
                if (v26[3] < s_v26wMin) s_v26wMin = v26[3];
                if (v26[3] > s_v26wMax) s_v26wMax = v26[3];
                if (v16[3] < s_v16wMin) s_v16wMin = v16[3];
                if (v16[3] > s_v16wMax) s_v16wMax = v16[3];
                static std::atomic<uint32_t> s_n117{0};
                const uint32_t nn = s_n117.fetch_add(1u, std::memory_order_relaxed);
                if (nn < 64u)
                    std::fprintf(stderr,
                        "[G117:ts] n=%u VF26=(% .4g % .4g % .4g % .4g) VF16w=% .4g adc=%u | VF29=(% .4g % .4g % .4g % .4g)\n",
                        nn, v26[0], v26[1], v26[2], v26[3], v16[3], adc,
                        v29[0], v29[1], v29[2], v29[3]);
                if ((nn % 50000u) == 0u && nn > 0u)
                    std::fprintf(stderr,
                        "[G117:tsrange] n=%u VF26.w=[% .4g,% .4g] VF16.w=[% .4g,% .4g] VF29=(% .4g % .4g % .4g % .4g)\n",
                        nn, s_v26wMin, s_v26wMax, s_v16wMin, s_v16wMax,
                        v29[0], v29[1], v29[2], v29[3]);
            }
        }

        // G132: the COPY packer (0x1b68) per-vertex ADC source. Decoded from the copy loop
        // (0x1be0-0x1c28): VF26.xyz = VF16.xyz (screen pos, MOVE @0x1c10); VF26.w =
        // max(min(VF29.x + VF29.w*VF16.w, VF29.y), VF29.z) (the fog/depth clamp, pipelined across
        // 0x1be8/0x1bf0/0x1c10); VF17 = FTOI4(VF26) @0x1c08; the XYZF2 store @0x1c28 packs VF17,
        // ADC = bit15 of VF17.w (= int(VF26.w*16) >= 0x8000 <=> VF26.w >= 2048). There is NO
        // unconditional +2048 here (unlike the 0x1ff0 transform tristrip), so with VF29(qword59)=0
        // the clamp collapses to 0 -> ADC=0 on EVERY copy vertex (= the G118 tally 648996/0
        // all-draw, the spanning-connector-tris root G131). Sample at the store (0x1c28): the
        // stored VF17 (int) gives the ACTUAL ADC; VF16.w is the EE-delivered per-vertex fog/.w the
        // copy is meant to pass through; VF29 is the clamp const. If VF16.w VARIES but VF29=0, the
        // fix is the qword59 upload (restore the passthrough clamp so VF16.w's ADC reaches the GS);
        // if VF16.w is also uniform/zero, the EE never wrote the per-vertex ADC into the copy input.
        // Env DC2_G132_COPYW; startPc==0x10. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g132 = (std::getenv("DC2_G132_COPYW") != nullptr);
            if (s_g132 && currentPc == 0x1c28u)
            {
                const float *v26 = m_state.vf[26];
                const float *v16 = m_state.vf[16];
                const float *v29 = m_state.vf[29];
                int32_t vi17w; std::memcpy(&vi17w, &m_state.vf[17][3], 4); // FTOI4 result (int)
                const uint32_t adc = ((vi17w >> 15) & 1) != 0 ? 1u : 0u;
                static thread_local float s_v16wMin = 1e30f, s_v16wMax = -1e30f;
                static thread_local float s_v26wMin = 1e30f, s_v26wMax = -1e30f;
                if (v16[3] < s_v16wMin) s_v16wMin = v16[3];
                if (v16[3] > s_v16wMax) s_v16wMax = v16[3];
                if (v26[3] < s_v26wMin) s_v26wMin = v26[3];
                if (v26[3] > s_v26wMax) s_v26wMax = v26[3];
                static std::atomic<uint32_t> s_n132{0};
                const uint32_t nn = s_n132.fetch_add(1u, std::memory_order_relaxed);
                if (nn < 48u)
                    std::fprintf(stderr,
                        "[G132:copyw] n=%u VF16=(% .4g % .4g % .4g % .6g) VF26.w=% .6g VF17.w(int)=%d adc=%u | VF29=(% .6g % .6g % .6g % .6g)\n",
                        nn, v16[0], v16[1], v16[2], v16[3], v26[3], vi17w, adc,
                        v29[0], v29[1], v29[2], v29[3]);
                if ((nn % 50000u) == 0u && nn > 0u)
                    std::fprintf(stderr,
                        "[G132:range] n=%u VF16.w=[% .6g,% .6g] VF26.w=[% .6g,% .6g] VF29=(% .6g % .6g % .6g % .6g)\n",
                        nn, s_v16wMin, s_v16wMax, s_v26wMin, s_v26wMax,
                        v29[0], v29[1], v29[2], v29[3]);
            }
        }

        // G237: dungeon-MAP per-vertex colour+alpha trace. The map-env emit loop (0x550-0x648,
        // XGKICK@0x648) stores VF22 as the GS colour (FTOI0 @0x5b0) and VF26.w as alpha
        // (built via MADDw/MINIy/MAXz vs the VF29=qw59 fog consts, DIV @0x5e0). G236 showed the
        // runner collapses this to over-bright (220,252,254,255) vs HW teal (57,105,122,128).
        // Dump the pre-conversion colour VF22 + its inputs (VF30 qw22, VF27=VF25-VF19, VF25
        // transformed, VF19, VF16/VF17 tex-scale, VF20 pos, VF21 in) and the alpha state, for
        // the first N vertices, to localize which contributor diverges. Env DC2_G237_TRACE.
        if (startPc == 0x10u)
        {
            static const bool s_g237 = (std::getenv("DC2_G237_TRACE") != nullptr);
            // Discriminate the MAP program from the other startPC=0x10 programs (title/model):
            // only this uploaded map emit loop has XGKICK (lo=0x80006efc) at code 0x648. Also defer to the
            // map scene via the G138 dump gate so we don't capture the boot/title program.
            extern std::atomic<bool> g_dc2G138DumpGateOpen;
            bool mapProg = false;
            if (s_g237 && vuCode && codeSize >= 0x64Cu)
            {
                uint32_t c648 = 0u; std::memcpy(&c648, vuCode + 0x648u, 4);
                mapProg = (c648 == 0x80006efcu);
            }
            // Fire on the FTOI0 xyzw COLOUR op (reliable across map-part microcode variants,
            // unlike a fixed PC). FTOI0 = Special upper (op 0x3C/0x3D) with S2_FUNC==0x14; the
            // colour store has dest==0xF (all lanes) vs the ADC .w-only path (dest==0x1/0x9).
            if (s_g237 && mapProg && g_dc2G138DumpGateOpen.load(std::memory_order_relaxed))
            {
                const uint8_t upOp237 = static_cast<uint8_t>(upper & 0x3Fu);
                const uint8_t s2_237  = S2_FUNC(upper);
                if (upOp237 >= 0x3Cu && (s2_237 == 0x14u || s2_237 == 0x15u)) // FTOI0 or FTOI4
                {
                    const uint8_t fs = static_cast<uint8_t>((upper >> 11u) & 0x1Fu);
                    const float scale237 = (s2_237 == 0x14u) ? 1.0f : 16.0f;
                    // Target the census-dominant (220,252,254) family: green lane result ~252.
                    const float rx = m_state.vf[fs][0]*scale237, ry = m_state.vf[fs][1]*scale237;
                    const bool washed = (ry >= 240.0f && ry <= 256.0f) && (rx >= 180.0f && rx <= 256.0f);
                    static std::atomic<uint32_t> s_g237n{0};
                    const uint32_t nn = washed ? s_g237n.fetch_add(1u, std::memory_order_relaxed) : 0xFFFFFFFFu;
                    if (washed && nn < 32u)
                    {
                        auto V = [&](int r){ return m_state.vf[r]; };
                        const float *src = m_state.vf[fs];   // pre-FTOI0 colour float
                        std::fprintf(stderr,
                            "[G237:col] n=%u pc=0x%x src=VF%u COL(pre-FTOI0)=(% .3f % .3f % .3f % .3f) | "
                            "Lmtx VF10=(% .4f % .4f % .4f % .4f) VF11=(% .4f % .4f % .4f % .4f) "
                            "VF12=(% .4f % .4f % .4f % .4f) VF13=(% .4f % .4f % .4f % .4f) | "
                            "in VF20=(% .4f % .4f % .4f % .4f) VF25=(% .3f % .3f % .3f % .3f) "
                            "VF19=(% .3f % .3f % .3f % .3f) VF29=(% .3f % .3f % .3f % .3f)\n",
                            nn, currentPc, fs, src[0],src[1],src[2],src[3],
                            V(10)[0],V(10)[1],V(10)[2],V(10)[3], V(11)[0],V(11)[1],V(11)[2],V(11)[3],
                            V(12)[0],V(12)[1],V(12)[2],V(12)[3], V(13)[0],V(13)[1],V(13)[2],V(13)[3],
                            V(20)[0],V(20)[1],V(20)[2],V(20)[3], V(25)[0],V(25)[1],V(25)[2],V(25)[3],
                            V(19)[0],V(19)[1],V(19)[2],V(19)[3], V(29)[0],V(29)[1],V(29)[2],V(29)[3]);
                        std::fflush(stderr);
                    }
                }
            }
        }

        // G70: confirm VU1 data qword 59 (VF29 = per-vertex W/ADC cull consts) is zero on the
        // title and dump the const region. The title flat-blue mechanism (decoded from the
        // executing transform 0x1e78-0x1fc0): sp06 = SUBAz.w ACC,VF0,VF29 -> ACC.w = 1 - VF29.z;
        // MADDw.w VF18 = ACC.w + VF29.w*W_raw; MINIy.w/MAXz.w clamp to [VF29.z, VF29.y]; then
        // ADDi.w VF18,VF18,I(=2048) on the KICKING vertex only; FTOI4 ->word3 ADC. With qword 59
        // = 0: ACC.w=1, clamp->0, +2048 -> 0x8000 -> ADC=1 on every kicking vertex => nothing
        // draws. Compare qw 21/22 (perspective divisors, must be non-zero) to prove a SELECTIVE
        // upload gap. Env DC2_G70; startPc==0x10; one-shot. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g70 = (std::getenv("DC2_G70") != nullptr);
            if (s_g70)
            {
                static std::atomic<uint32_t> s_g70dump{0};
                if (currentPc >= 0x1e00u && currentPc <= 0x2000u &&
                    s_g70dump.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    static const uint32_t qws[] = {4u, 5u, 6u, 7u, 21u, 22u, 30u, 57u, 58u, 59u, 60u, 61u};
                    for (uint32_t i = 0; i < sizeof(qws) / sizeof(qws[0]); ++i)
                    {
                        const uint32_t q = qws[i];
                        float f[4] = {0, 0, 0, 0};
                        if (q * 16u + 16u <= dataSize)
                            std::memcpy(f, vuData + q * 16u, 16);
                        std::fprintf(stderr, "[G70:qw] qw%u = % .6g % .6g % .6g % .6g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                    std::fflush(stderr);
                }
            }
        }

        if (vu1_trace_enabled())
        {
            const uint8_t opHi = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
            if (opHi == 0x40u && S2_FUNC(lower) == 0x6Cu)
            {
                const uint32_t n = s_g22Vu1XgkickOpCount.fetch_add(1u, std::memory_order_relaxed);
                if (n < 96u)
                {
                    const uint32_t is = (lower >> 11u) & 0x1Fu;
                    const uint32_t vi = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is]));
                    std::fprintf(stderr,
                                 "[G22:vu1op] n=%u pc=0x%x cycle=%u lower=0x%08x upper=0x%08x is=VI%u vi=0x%x addr=0x%x itop=0x%x\n",
                                 n, currentPc, cycle, lower, upper, is, vi, vi * 16u, m_state.itop);
                }
            }
        }

        // G32: log the UPPER op paired with each lower in the model pack window so we can
        // see which op produces VF17 (XYZF2 position) / VF21 (RGBAQ) and from what source.
        if (g32_vu_trace_enabled() && currentPc >= 0x1b00u && currentPc <= 0x1c80u)
        {
            const uint32_t n = s_g32Count.load(std::memory_order_relaxed);
            if (false)
            {
                const uint8_t uOp = (uint8_t)((upper >> 25) & 0x3Fu); // bits 30:25 base op
                const uint8_t ud  = (uint8_t)((upper >> 6) & 0x1Fu);
                const uint8_t us  = (uint8_t)((upper >> 11) & 0x1Fu);
                const uint8_t ut  = (uint8_t)((upper >> 16) & 0x1Fu);
                const uint8_t udst = (uint8_t)((upper >> 21) & 0xFu);
                const uint8_t us2 = S2_FUNC(upper);
                std::fprintf(stderr,
                             "[G32:up] pc=0x%x upper=0x%08x op=0x%02x s2=0x%02x dst=0x%x vd=VF%u vs=VF%u vt=VF%u | VF17=(%.5g %.5g %.5g %.5g)\n",
                             currentPc, upper, (uint32_t)uOp, (uint32_t)us2, (uint32_t)udst,
                             (uint32_t)ud, (uint32_t)us, (uint32_t)ut,
                             m_state.vf[17][0], m_state.vf[17][1], m_state.vf[17][2], m_state.vf[17][3]);
            }
        }

        } // G295 diag region B end
#endif // DC2_VU1_LOOP_DIAG

        const bool eBit = g410Pair != nullptr
            ? (g410Pair->pairFlags & G410_PAIR_E_BIT) != 0u
            : ((upper >> 30u) & 1u) != 0u;

        // G27: LOI is signaled by the I-bit (bit 31 of the UPPER word). When set,
        // the LOWER 32 bits are a 32-bit float immediate loaded into the I register
        // and must NOT be decoded as a lower instruction; the upper still executes.
        // The old code keyed LOI on a magic lower value (0x8000033C) that DC2's VU
        // programs never emit, so real LOI immediates (e.g. 0x457ff000 ~= 4093.0f)
        // were decoded as bogus lower ops (0x22/0x23), corrupting the model transform
        // and derailing the microprogram (runaway -> maxcycles, garbage GIF output).
        const bool iBit = g410Pair != nullptr
            ? (g410Pair->pairFlags & G410_PAIR_I_BIT) != 0u
            : ((upper >> 31u) & 1u) != 0u;

        // G87: advance the Q pipeline one cycle BEFORE this word executes. A pending DIV/SQRT/
        // RSQRT result becomes visible in m_state.q when its delay expires; a producer in THIS
        // word (re)arms the delay afterwards (in execLower) so it counts from the next word.
        if (G479_QD > 0 && --G479_QD == 0)
            m_state.q = s_vuQPending;

        // G239: advance the EFU pipeline before this instruction pair. An MFP in the pair
        // where the latency expires sees the newly published architectural P, matching the
        // interpreter ordering in PCSX2 (_vuTestPipes before lower execution).
        if (s_ppipeOn && G479_PD > 0 && --G479_PD == 0)
            m_state.p = s_vuPPending;

        // G239: PCSX2's interpreter performs _vuTestLowerStalls + _vuTestPipes before
        // executing either half of the instruction pair. Publish an older scalar result here
        // when this lower op would stall FDIV/EFU; execLower then arms the new producer (or
        // observes an already-completed WAIT) after the parallel upper has consumed the value.
        // G426: the sidecar already knows whether this pair's lower slot is a scalar-pipe
        // producer or waiter. When it is not, the whole block below is provably a no-op, so the
        // predicate replaces a sidecar byte load plus ten compares with one cached bit test.
        if (s_g426VerifyPredecode && g426Predecode && !iBit)
        {
            const uint8_t liveS2 = ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
            if (g426LowerIsScalarPipe(liveS2) !=
                ((g410Pair->g425Flags & G426_PF_PRESTALL) != 0u))
            {
                std::fprintf(stderr,
                             "[G426:verify] prestall mismatch pc=0x%x lower=%08x s2=%02x "
                             "cached=%d\n",
                             currentPc, lower, (unsigned)liveS2,
                             (int)((g410Pair->g425Flags & G426_PF_PRESTALL) != 0u));
                std::abort();
            }
        }
        if (!iBit && s_scalarPrestallOn &&
            (!g426Predecode || (g410Pair->g425Flags & G426_PF_PRESTALL) != 0u))
        {
            const uint8_t lowerS2239 = g410Pair != nullptr
                ? g410Pair->lowerS2
                : (((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu);
            if (lowerS2239 != 0xffu)
            {
                const bool waitQ239 = lowerS2239 == 0x3bu;
                const bool fdivProducer239 = lowerS2239 >= 0x38u && lowerS2239 <= 0x3au;
                if (s_qlatOn && G479_QD > 0 &&
                    (waitQ239 || (fdivProducer239 && g200_qstall_enabled())))
                {
                    m_state.q = s_vuQPending;
                    G479_QD = 0;
                }

                const bool waitP239 = lowerS2239 == 0x7bu;
                const bool efuProducer239 =
                    lowerS2239 == 0x70u || lowerS2239 == 0x71u ||
                    lowerS2239 == 0x72u || lowerS2239 == 0x73u ||
                    lowerS2239 == 0x76u || lowerS2239 == 0x78u ||
                    lowerS2239 == 0x79u || lowerS2239 == 0x7au;
                if (s_ppipeOn && G479_PD > 0 && (waitP239 || efuProducer239))
                {
                    m_state.p = s_vuPPending;
                    G479_PD = 0;
                }
            }
        }

        // G138: advance the MAC/STATUS flag pipeline one instruction pair. Flag-consuming
        // lower ops executed THIS pair (FMEQ/FMAND/FMOR/FSAND/FSEQ/FSOR) read the snapshot
        // from DEPTH pairs ago via s_vuMacVisible/s_vuStatusVisible. (G141: flag + TLS base
        // hoisted to loop-invariant locals; identical semantics.)
        if (g430Hist)
        {
            // G430: nothing to advance. Publish only for a pair whose lower slot consumes the
            // line (2.06% of pairs), by finding the newest writer at or before pair N-DEPTH.
            uint32_t g430LegacyVis = 0u;
            if (g430Shadow)
            {
                // DC2_G430_VERIFY: keep the legacy EAGER ring running alongside (the inline upper
                // classifies as well under this flag), so every consumer has a ground truth.
                const int g430Next = (*s_headP != 0) ? (*s_headP - 1) : (s_macpipeDepth - 1);
                *s_headP = g430Next;
                g430LegacyVis = s_macPipeP[g430Next];
                *s_statusVisP = s_statusPipeP[g430Next];
            }
            if ((g410Pair->g425Flags & G427_PF_FLAGREAD) != 0u)
            {
                const uint32_t g430Mac =
                    g430ResolveAt(g430Base + (int64_t)cycle - s_macpipeDepth);
                if (g430Shadow && g430Mac != g430LegacyVis)
                {
                    std::fprintf(stderr,
                                 "[G430:verify] mac mismatch pc=0x%x resolved=%08x legacy=%08x\n",
                                 currentPc, g430Mac, g430LegacyVis);
                    std::abort();
                }
                *s_macVisP = g430Mac;
                if (currentPc == 0x2f08u && std::getenv("DC2_G545_CPU_TRACE") != nullptr)
                {
                    static std::atomic<uint32_t> s_g545TraceCount{0u};
                    if (s_g545TraceCount.fetch_add(1u, std::memory_order_relaxed) < 128u)
                        std::fprintf(stderr,
                                     "[G545:cpu-pair-fmand] stamp=%lld mac=%04x vi10=%04x mask=%04x\n",
                                     static_cast<long long>(g430Base + static_cast<int64_t>(cycle)),
                                     g430Mac & 0xffffu,
                                     static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[10])),
                                     static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])));
                }
                if (g430Shadow)
                    ++s_g430VerifyOk;
            }
        }
        else if (g430MacPipeOn)
        {
            const int g138d = s_macpipeDepth;
            if (g428Defer)
            {
                // Advance the line; classify the entry leaving the window only when this pair's
                // lower slot actually consumes it (G427_PF_FLAGREAD, decoded in the sidecar).
                const int g428Next = (*s_headP != 0) ? (*s_headP - 1) : (g138d - 1);
                *s_headP = g428Next;
                if ((g410Pair->g425Flags & G427_PF_FLAGREAD) != 0u)
                {
                    const uint32_t g428Mac = g428ResolveSlot(g428Next);
                    *s_macVisP = g428Mac;
                    // With DC2_G428_VERIFY the eager classifier also ran on every pair and the
                    // pipe holds the legacy value for this slot: an exact end-to-end oracle for
                    // the parked vector, the CARRY fallback and the EAGER tag alike.
                    if (s_g428Verify && s_macPipeP[g428Next] != g428Mac)
                    {
                        std::fprintf(stderr,
                                     "[G428:verify] resolved mac mismatch pc=0x%x slot=%d "
                                     "kind=%u resolved=%08x eager=%08x depth=%d "
                                     "preKind=%u preMac=%08x kinds=%u%u%u%u pipe=%08x,%08x,%08x,%08x\n",
                                     currentPc, g428Next, (unsigned)s_g428Kind[g428Next],
                                     g428Mac, s_macPipeP[g428Next], g138d,
                                     (unsigned)s_g428PreKind, s_g428PreMac,
                                     (unsigned)s_g428Kind[0], (unsigned)s_g428Kind[1],
                                     (unsigned)s_g428Kind[2], (unsigned)s_g428Kind[3],
                                     s_macPipeP[0], s_macPipeP[1], s_macPipeP[2], s_macPipeP[3]);
                        std::abort();
                    }
                }
                // The entry just read is the newest one older than the window, so it becomes the
                // CARRY fallback. This MUST happen here and not at the push: the inline upper
                // parks its result into this very slot, overwriting the parked vector before the
                // end of the pair. (Kinds are only written at the push, so reading the tag here
                // still sees the outgoing entry.)
                const uint8_t g428Old = s_g428Kind[g428Next];
                if (g428Old != G428_MK_CARRY)
                {
                    s_g428PreKind = g428Old;
                    if (g428Old == G428_MK_PENDING)
                    {
                        _mm_store_ps(s_g428PreVec, _mm_load_ps(s_g428PendVec[g428Next]));
                        s_g428PreDest = s_g428PendDest[g428Next];
                    }
                    else
                        s_g428PreMac = s_macPipeP[g428Next];
                }
            }
            else if (g427Lazy)
            {
                // G427: advance the line, publish only for a consumer pair.
                const int g427Next = (*s_headP != 0) ? (*s_headP - 1) : (g138d - 1);
                *s_headP = g427Next;
                if ((g410Pair->g425Flags & G427_PF_FLAGREAD) != 0u)
                {
                    *s_macVisP = s_macPipeP[g427Next];
                    *s_statusVisP = s_statusPipeP[g427Next];
                }
                // The sidecar clears the bit on LOI pairs (their lower word is a float
                // immediate, not an instruction), so the live check must skip them too.
                if (s_g427Verify && !iBit &&
                    g427LowerReadsFlags(lower) !=
                        ((g410Pair->g425Flags & G427_PF_FLAGREAD) != 0u))
                {
                    std::fprintf(stderr,
                                 "[G427:verify] flagread mismatch pc=0x%x lower=%08x cached=%d\n",
                                 currentPc, lower,
                                 (int)((g410Pair->g425Flags & G427_PF_FLAGREAD) != 0u));
                    std::abort();
                }
            }
            else if (s_g328RingOn)
            {
                const int g328next = (s_g328MacPipeHead != 0)
                    ? (s_g328MacPipeHead - 1) : (g138d - 1);
                *s_macVisP = s_macPipeP[g328next];
                *s_statusVisP = s_statusPipeP[g328next];
                s_g328MacPipeHead = g328next;

                if (s_g328Verify)
                {
                    bool g328bad =
                        *s_macVisP != s_g328VerifyMacPipe[g138d - 1] ||
                        *s_statusVisP != s_g328VerifyStatusPipe[g138d - 1];
                    for (int g328i = g138d - 1; g328i > 0; --g328i)
                    {
                        s_g328VerifyMacPipe[g328i] = s_g328VerifyMacPipe[g328i - 1];
                        s_g328VerifyStatusPipe[g328i] = s_g328VerifyStatusPipe[g328i - 1];
                    }
                    if (g328bad)
                    {
                        s_g328Bad.fetch_add(1u, std::memory_order_relaxed);
                        std::fprintf(stderr,
                            "[G328:verify] visible mismatch pc=0x%x depth=%d mac=%08x/%08x status=%08x/%08x\n",
                            currentPc, g138d, *s_macVisP, s_g328VerifyMacPipe[g138d - 1],
                            *s_statusVisP, s_g328VerifyStatusPipe[g138d - 1]);
                        std::abort();
                    }
                }
            }
            else
            {
                *s_macVisP = s_macPipeP[g138d - 1];
                *s_statusVisP = s_statusPipeP[g138d - 1];
                for (int g138i = g138d - 1; g138i > 0; --g138i)
                {
                    s_macPipeP[g138i] = s_macPipeP[g138i - 1];
                    s_statusPipeP[g138i] = s_statusPipeP[g138i - 1];
                }
            }
        }

        if (iBit)
        {
            std::memcpy(&m_state.i, &lower, 4);
            if (g410VerifyRun)
                g410VerifyUpper(lower, upper);
            else if ((!g410CompiledRun || g410Pair->upperKind != 0x6fu) && !g430SkipUpper)
                // G421: LOI pairs are 0.69% of executed pairs. Keeping them on the legacy
                // decoder leaves exactly ONE inlined copy of the SIMD upper body in the hot
                // loop; a second copy cost more in loop footprint than this share can repay.
                DC2_G430_EXEC_UPPER_LEGACY(upper);
        }
        else
        {
            const uint32_t g106MacBeforeUpper = m_state.mac;
            const uint32_t g106StatusBeforeUpper = m_state.status;
            // G139: locate the upper op's VF destination (fd for main-space ops; ft for
            // ITOF/FTOI/ABS in the special group; none for ACC/CLIP writers) so the lower
            // op of this pair can read the pre-upper value (see g139_pairhaz_enabled).
            // G435: `g139dst` is a raw float* into m_state.vf and `g139old/new` are
            // address-taken stack arrays fed by memcpy. Their mere PRESENCE forces MSVC to
            // assume every access to m_state.vf may alias them, across both the inline upper
            // and the inline lower — so the register-form below keeps the same three values in
            // an index + two __m128 registers and never materialises a pointer. See the
            // g435HazReg comment in vu1_upper_opcodes.inc.
#if !DC2_G485_HAZREG
            float *g139dst = nullptr;
            float g139old[4], g139new[4];
#endif
            int g139Idx = -1;
            __m128 g139OldV = _mm_setzero_ps();
            __m128 g139NewV = _mm_setzero_ps();
            if (s_pairhazOn && g426Predecode && !g430SkipHaz && !g435SkipHaz)
            {
                if (s_g426VerifyPredecode)
                {
                    uint32_t liveIdx = 0xffffffffu;
                    const uint8_t liveOp = (uint8_t)(upper & 0x3Fu);
                    if (liveOp < 0x3Cu)
                        liveIdx = (upper >> 6) & 0x1Fu;
                    else
                    {
                        const uint8_t liveS2 = S2_FUNC(upper);
                        if ((liveS2 >= 0x10u && liveS2 <= 0x17u) || liveS2 == 0x1Du)
                            liveIdx = (upper >> 16) & 0x1Fu;
                    }
                    const bool liveHaz =
                        liveIdx != 0xffffffffu &&
                        ((((lower >> 11u) & 0x1Fu) == liveIdx) ||
                         (((lower >> 16u) & 0x1Fu) == liveIdx));
                    if (liveHaz != ((g410Pair->g425Flags & G426_PF_HAZ) != 0u))
                    {
                        std::fprintf(stderr,
                                     "[G426:verify] haz mismatch pc=0x%x upper=%08x lower=%08x "
                                     "live=%d cached=%d\n",
                                     currentPc, upper, lower, (int)liveHaz,
                                     (int)((g410Pair->g425Flags & G426_PF_HAZ) != 0u));
                        std::abort();
                    }
                }
                // G426: hazard applicability is a property of the pair encoding, decoded once.
                if ((g410Pair->g425Flags & G426_PF_HAZ) != 0u)
                {
#if DC2_G485_HAZREG
                    g139Idx = (int)g410Pair->upperVfDest;
                    g139OldV = _mm_loadu_ps(m_state.vf[g139Idx]);
#else
                    if (g435HazReg)
                    {
                        g139Idx = (int)g410Pair->upperVfDest;
                        g139OldV = _mm_loadu_ps(m_state.vf[g139Idx]);
                    }
                    else
                    {
                        g139dst = m_state.vf[g410Pair->upperVfDest];
                        std::memcpy(g139old, g139dst, 16);
                    }
#endif
                }
            }
#if DC2_G485_HAZREG
            // G485: under DC2_G443_FIXED, `g426Predecode` is constexpr true, so the legacy
            // non-predecode hazard branch below is unreachable in this copy. It is elided
            // TEXTUALLY (not merely by constant folding) because its `g139dst` / `g139old` uses
            // are exactly the aliasing this copy exists to remove.
#else
            else if (s_pairhazOn && !g435SkipHaz)
            {
                uint32_t g139DstIdx = 0u;
                if (g410Pair != nullptr)
                {
                    if (g410Pair->upperVfDest != 0xffu)
                    {
                        g139DstIdx = g410Pair->upperVfDest;
                        g139dst = m_state.vf[g139DstIdx];
                    }
                }
                else
                {
                    const uint8_t g139op = (uint8_t)(upper & 0x3Fu);
                    if (g139op < 0x3Cu)
                    {
                        g139DstIdx = (upper >> 6) & 0x1Fu;
                        g139dst = m_state.vf[g139DstIdx];
                    }
                    else
                    {
                        const uint8_t g139s2 = S2_FUNC(upper);
                        if ((g139s2 >= 0x10u && g139s2 <= 0x17u) || g139s2 == 0x1Du)
                        {
                            g139DstIdx = (upper >> 16) & 0x1Fu;
                            g139dst = m_state.vf[g139DstIdx];
                        }
                    }
                }
                // G295: every lower-slot VF access (read or write) names its register in the
                // fs (bits 11-15) or ft (bits 16-20) field. If neither equals the upper's dest,
                // the save/restore/overlay round-trip is the identity — skip it. A VI-indexed
                // false positive just keeps the legacy swap. Kill: DC2_G295_NO_HAZSKIP=1.
                if (g139dst != nullptr && !s_g295KeepHaz &&
                    (g410Pair != nullptr
                         ? (g410Pair->lowerFs != g139DstIdx &&
                            g410Pair->lowerFt != g139DstIdx)
                         : (((lower >> 11u) & 0x1Fu) != g139DstIdx &&
                            ((lower >> 16u) & 0x1Fu) != g139DstIdx)))
                    g139dst = nullptr;
                if (g139dst)
                {
                    std::memcpy(g139old, g139dst, 16);
                }
            }
#endif // DC2_G485_HAZREG
            if (g410VerifyRun)
                g410VerifyUpper(lower, upper);
            else if ((!g410CompiledRun || g410Pair->upperKind != 0x6fu) && !g430SkipUpper)
            {
                DC2_G421_RUN_UPPER(upper, g410Pair != nullptr ? g410Pair->upperKind : 0xffu);
            }
            if (g139Idx >= 0)
            {
                g139NewV = _mm_loadu_ps(m_state.vf[g139Idx]);
                _mm_storeu_ps(m_state.vf[g139Idx], g139OldV);
            }
#if !DC2_G485_HAZREG
            else if (g139dst)
            {
                std::memcpy(g139new, g139dst, 16);
                std::memcpy(g139dst, g139old, 16);
            }
#endif
            if (s_g106ParallelFlags)
            {
                s_g106LowerMacRead = g106MacBeforeUpper;
                s_g106LowerStatusRead = g106StatusBeforeUpper;
                s_g106UseLowerFlagRead = true;
            }
#if DC2_VU1_LOOP_DIAG
            // G295 diag region C (pre-execLower): G198/G200 probes. Interior unchanged.
            if (g295Diag)
            {
            // G198: refuted G197's IALU-latency hypothesis by reading PCSX2's real _vuIALUflush/
            // _vuTestALUStalls (VUops.cpp) -- IALU-pipe bookkeeping only advances VU->cycle (a
            // timing counter used elsewhere for XGKICK pacing), it never delays or forwards a
            // stale VALUE: _vuILW/_vuIADD* write VU->VI[] eagerly every time regardless of the
            // ialu[] pipe state, so modeling that latency in DC2 (an interpreter with no true
            // out-of-order execution) would be a no-op for this bug. Also confirmed the PCSX2
            // DebugServer cannot do a live VU1-internal register A/B (g135's tool note: EE RAM/
            // scratchpad only). Redirect: trace the shared FMAND/IAND/IBEQ chain's actual inputs
            // (VI1/VI7/VI8/VI9/VI10 at pc=0x2128, the same gate G138 already fixed for the title)
            // for the map-piece startPC=0x10 program, so a title-vs-map comparison can be done
            // from log data instead. Default-off (DC2_G198_GATE_TRACE), capped at 4000 hits.
            static const bool s_g198GateOn = (std::getenv("DC2_G198_GATE_TRACE") != nullptr);
            if (s_g198GateOn && startPc == 0x10u && currentPc == 0x2128u)
            {
                static std::atomic<uint32_t> s_g198Hits{0};
                const uint32_t h = s_g198Hits.fetch_add(1u, std::memory_order_relaxed);
                if (h < 4000u)
                {
                    std::fprintf(stderr,
                        "[G198:gate] hit=%u vi1=0x%x vi7=0x%x vi8=0x%x vi9=0x%x vi10=0x%x taken=%d\n",
                        h, (unsigned)(uint16_t)m_state.vi[1], (unsigned)(uint16_t)m_state.vi[7],
                        (unsigned)(uint16_t)m_state.vi[8], (unsigned)(uint16_t)m_state.vi[9],
                        (unsigned)(uint16_t)m_state.vi[10],
                        (int)((int16_t)m_state.vi[1] == (int16_t)m_state.vi[10]));
                }
            }
            // G200: qw30's REAL producer is this same program's setup block (statically confirmed
            // from the G197 full dump: 0x170 OPMULA/OPMSUB cross of VF15xVF16, 0x180 MUL by VF17,
            // 0x198/0x1a0 ADD.x sum -> det in VF24.x, 0x1c0 FMAND VI9,0x80 on the det's S.x flag,
            // 0x1e0/0x200 VI7=0x0/0x20, 0x210 ISW.x VI7,30(VI0)) -- NOT an EE-side upload (G199's
            // info+0x270 refutation + this close G198's "EE-side producer" framing). Trace the det
            // inputs/outputs at the ISW so a host-side recompute can check sign consistency, and a
            // matrix-level A/B vs PCSX2 EE RAM becomes possible. Default-off (DC2_G200_DET_TRACE).
            // NOTE: startPC=0x10 enters at byte 0x10 -> JR to 0x320, SKIPPING the setup block;
            // the det/qw30 setup only runs under a different start (0x0 path). No startPc filter.
            static const bool s_g200DetOn = (std::getenv("DC2_G200_DET_TRACE") != nullptr);
            if (s_g200DetOn && currentPc == 0x210u)
            {
                static std::atomic<uint32_t> s_g200Hits{0};
                const uint32_t h = s_g200Hits.fetch_add(1u, std::memory_order_relaxed);
                if (h < 400u)
                {
                    uint32_t r15[4], r16[4], r17[4], d24;
                    std::memcpy(r15, m_state.vf[15], 16);
                    std::memcpy(r16, m_state.vf[16], 16);
                    std::memcpy(r17, m_state.vf[17], 16);
                    std::memcpy(&d24, &m_state.vf[24][0], 4);
                    std::fprintf(stderr,
                        "[G200:det] hit=%u start=0x%x vi7=0x%x det=%.9g d24=0x%08x"
                        " vf15=%08x,%08x,%08x,%08x vf16=%08x,%08x,%08x,%08x vf17=%08x,%08x,%08x,%08x\n",
                        h, startPc, (unsigned)(uint16_t)m_state.vi[7], m_state.vf[24][0], d24,
                        r15[0], r15[1], r15[2], r15[3],
                        r16[0], r16[1], r16[2], r16[3],
                        r17[0], r17[1], r17[2], r17[3]);
                }
            }
            } // G295 diag region C end
#endif // DC2_VU1_LOOP_DIAG
            // G545 v21 default-off parity trace: record the CPU-authoritative writes to the
            // qword later consumed by the first divergent Dungeon LQs. The three PC tests are
            // exact store sites; the atomic sequence read is therefore absent from other pairs.
            uint32_t g545StoreQ = 0xffffffffu;
            uint32_t g545StoreSource = 0xffffffffu;
            uint32_t g545StoreSourceBits[4]{};
            if ((currentPc == 0x2870u || currentPc == 0x31e0u || currentPc == 0x2e38u) &&
                g_g545CpuTraceSequence.load(std::memory_order_relaxed) == 784u)
            {
                const uint32_t g545LowerOp = (lower >> 25u) & 0x7fu;
                if (g545LowerOp == 0x01u)
                {
                    const uint32_t g545Base = (lower >> 16u) & 31u;
                    g545StoreSource = (lower >> 11u) & 31u;
                    const int32_t g545Imm = static_cast<int32_t>(lower << 21u) >> 21u;
                    g545StoreQ = static_cast<uint32_t>(m_state.vi[g545Base] + g545Imm) & 0x3ffu;
                }
                else if (g545LowerOp == 0x40u && S2_FUNC(lower) == 0x35u)
                {
                    const uint32_t g545Base = (lower >> 16u) & 31u;
                    g545StoreSource = (lower >> 11u) & 31u;
                    g545StoreQ = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[g545Base])) & 0x3ffu;
                }
                if (g545StoreSource < 32u)
                    std::memcpy(g545StoreSourceBits, m_state.vf[g545StoreSource], 16u);
            }
#if DC2_G479_QPREG
            // Same two arms the macro expands to under g443Fixed, with the local Q/P counters
            // spilled around the ONLY callee that can write them.
            if (g410Pair->lowerKind != G422_LK_NONE)
            {
                // G532b: DIV is the first covered lower that writes the Q-pipeline counters.
                // This copy is currently compiled out, but keep its register-local contract exact.
                if (g410Pair->lowerKind == G422_LK_DIV)
                    G479_QP_SYNC();
                g422FastLower(m_state, lower, g410Pair->lowerKind, vuData, dataSize);
                if (g410Pair->lowerKind == G422_LK_DIV)
                    G479_QP_RELOAD();
            }
            else
            {
                G479_QP_SYNC();
                execLower(lower, vuData, dataSize, gs, memory, upper);
                G479_QP_RELOAD();
            }
#else
            DC2_G422_RUN_LOWER();
#endif
            if (g545StoreQ == 860u && vuData != nullptr && g545StoreQ * 16u + 16u <= dataSize)
            {
                uint32_t g545StoredBits[4]{};
                std::memcpy(g545StoredBits, vuData + g545StoreQ * 16u, 16u);
                static std::atomic<uint32_t> s_g545StoreTraceCount{0u};
                if (s_g545StoreTraceCount.fetch_add(1u, std::memory_order_relaxed) < 16u)
                    std::fprintf(stderr,
                                 "[G545:cpu-ramstore] seq=784 pc=%04x instr=%08x q=%u vf=%u source=%08x,%08x,%08x,%08x after=%08x,%08x,%08x,%08x\n",
                                 currentPc, lower, g545StoreQ, g545StoreSource,
                                 g545StoreSourceBits[0], g545StoreSourceBits[1],
                                 g545StoreSourceBits[2], g545StoreSourceBits[3],
                                 g545StoredBits[0], g545StoredBits[1],
                                 g545StoredBits[2], g545StoredBits[3]);
            }
            // G436: `true` is only ever written to this thread_local inside the
            // `if (s_g106ParallelFlags)` block above, so when that lever is off the store is the
            // identity — and it was costing a TLS materialisation plus a store on every pair.
            if (g436FlagStore)
                s_g106UseLowerFlagRead = false;
            if (g139Idx >= 0)
            {
                // Same overlay, expressed as one masked bit-select: masked-off lanes are
                // rewritten with the bits the lower slot left there, so this is value-identical
                // to the four conditional scalar assignments in the control arm below.
                const uint8_t g139m = (uint8_t)((upper >> 21) & 0xFu);
                float *const g139p = m_state.vf[g139Idx];
                const __m128 g139mask =
                    _mm_load_ps(reinterpret_cast<const float *>(g436MaskRow(g436Lean, g139m)));
                _mm_storeu_ps(g139p,
                              _mm_or_ps(_mm_and_ps(g139mask, g139NewV),
                                        _mm_andnot_ps(g139mask, _mm_loadu_ps(g139p))));
            }
#if !DC2_G485_HAZREG
            else if (g139dst)
            {
                // The upper result lands after the lower's same-cycle access: overlay the
                // upper's dest-masked lanes over whatever the lower left in the register.
                const uint8_t g139m = (uint8_t)((upper >> 21) & 0xFu);
                if (g139m & 0x8u) g139dst[0] = g139new[0];
                if (g139m & 0x4u) g139dst[1] = g139new[1];
                if (g139m & 0x2u) g139dst[2] = g139new[2];
                if (g139m & 0x1u) g139dst[3] = g139new[3];
            }
#endif
        }
        // G369 (diagnostic, default-off DC2_G369_VUNAN=1): the cutscene gray screen was measured
        // at the GS as every vertex arriving with NaN/Inf. VIF source data is clean, so the
        // poison is born inside VU1. Report the FIRST instruction pair after which any vf lane
        // becomes non-finite, with the pc and both opcode words, then keep counting.
        if (!g425Lean)
        {
            const bool s_g369vu = s_g425VuNan;
            if (s_g369vu)
            {
                static thread_local bool s_wasClean = true;
                bool bad = false;
                int badReg = -1;
                for (int r = 1; r < 32 && !bad; ++r)
                    for (int l = 0; l < 4; ++l)
                        if (!std::isfinite(m_state.vf[r][l]))
                        {
                            bad = true;
                            badReg = r;
                            break;
                        }
                if (bad && s_wasClean)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 30u || (n % 200u) == 0u)
                        std::fprintf(stderr,
                                     "[G369:vunan] n=%u pc=0x%x upper=0x%08x lower=0x%08x vf%d=(%g %g %g %g)\n",
                                     n, m_state.pc, upper, lower, badReg,
                                     m_state.vf[badReg][0], m_state.vf[badReg][1],
                                     m_state.vf[badReg][2], m_state.vf[badReg][3]);
                }
                s_wasClean = !bad;
            }
        }

        // G138: the newest pipeline entry is this pair's post-upper architectural flags.
        if (g430Hist)
        {
            if (g430Shadow)
            {
                s_macPipeP[*s_headP] = m_state.mac;
                s_statusPipeP[*s_headP] = m_state.status;
            }
        }
        else if (g430MacPipeOn)
        {
            const int g328newest = s_g328RingOn ? *s_headP : 0;
            if (g428Defer)
            {
                // Record what THIS pair did to the MAC, without classifying it. (The outgoing
                // entry was already folded into the CARRY fallback at the advance above.)
                const uint8_t g428Claim = s_g428Claim;
                s_g428Kind[g328newest] = g428Claim;
                if (s_g428Verify)
                {
                    s_macPipeP[g328newest] = m_state.mac;
                    s_statusPipeP[g328newest] = m_state.status;
                }
                else if (g428Claim == G428_MK_EAGER)
                    s_macPipeP[g328newest] = m_state.mac;
                s_g428Claim = G428_MK_CARRY;
            }
            else
            {
            s_macPipeP[g328newest] = m_state.mac;
            s_statusPipeP[g328newest] = m_state.status;
            }
            if (s_g328RingOn && s_g328Verify)
            {
                s_g328VerifyMacPipe[0] = m_state.mac;
                s_g328VerifyStatusPipe[0] = m_state.status;
                for (int g328i = 0; g328i < s_macpipeDepth; ++g328i)
                {
                    const int g328phys = s_g328MacPipeHead + g328i < s_macpipeDepth
                        ? s_g328MacPipeHead + g328i
                        : s_g328MacPipeHead + g328i - s_macpipeDepth;
                    if (s_macPipeP[g328phys] != s_g328VerifyMacPipe[g328i] ||
                        s_statusPipeP[g328phys] != s_g328VerifyStatusPipe[g328i])
                    {
                        s_g328Bad.fetch_add(1u, std::memory_order_relaxed);
                        std::fprintf(stderr,
                            "[G328:verify] logical mismatch pc=0x%x depth=%d slot=%d mac=%08x/%08x status=%08x/%08x\n",
                            currentPc, s_macpipeDepth, g328i,
                            s_macPipeP[g328phys], s_g328VerifyMacPipe[g328i],
                            s_statusPipeP[g328phys], s_g328VerifyStatusPipe[g328i]);
                        std::abort();
                    }
                }
            }
            if (s_g328RingOn && s_g328Stat)
            {
                const uint64_t g328n = s_g328Cycles.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((g328n % 5000000u) == 0u)
                    std::fprintf(stderr, "[G328:stat] cycles=%llu bad=%llu depth=%d\n",
                                 (unsigned long long)g328n,
                                 (unsigned long long)s_g328Bad.load(std::memory_order_relaxed),
                                 s_macpipeDepth);
            }
        }
        // G489 - SKIP THE `postExecPc` READ-BACK ON PAIRS WHOSE LOWER CANNOT BRANCH.
        //
        // The tail asks "did the lower slot branch?" by reading `m_state.pc` back and comparing it
        // with `currentPc`. The G489 /MAP + PC-sample overlay put 22.6% of all VU1 samples in the
        // 512-byte tail window and named `mov rax,[rbp+0x160]` (the `&m_state` spill reload) and
        // `mov edx,[rax+0x25c]` among its hottest instructions, and the answer is a pure function
        // of the lower ENCODING the G410 cache already holds — 90.43% of executed pairs cannot
        // branch at all.
        //
        // RESULT: REFUTED (G489). Six paired A/Bs (DC2_G419_AB=pcskip, both arms confirmed built)
        // read -0.395 / +0.249 / -0.025 / -0.051 / -0.097 / +0.270: mean **-0.008 ms/f**, blocked
        // -0.067, sign 4/6 — a null, and the control arm carried one extra always-true test, which
        // biases that null TOWARD the candidate. The predicate itself is exact (2.5 G pairs,
        // `bad=0`). What it refutes is the premise: `m_state.pc` was stored by the previous pair's
        // own tail, so the read is an L1 store-to-load forward the loop's ILP already hides. Read
        // together with G483 — which measured removing the PC load at the TOP of the loop at
        // +0.47..+0.64 ms/f SLOWER — the PC round trip is closed from BOTH ends: it is not a cost.
        //
        // Compiled out (`DC2_G489_PCSKIP 0`) rather than shipped default-off, because leaving it in
        // would cost the default path an always-true byte test AND 2,884 bytes of `run()` text,
        // which is the resource G481's inline-budget cliff binds on. The decode, the oracle and the
        // A/B arm are kept so ONE edit here re-instantiates it if the loop's shape ever changes.
#ifndef DC2_G489_PCSKIP
#define DC2_G489_PCSKIP 0
#define G489_LOCAL_DEFAULT 1
#endif
#if DC2_G489_PCSKIP
        // `g425Flags` is the same cache byte the G425 re-pin already reads at the bottom of this
        // body, out of the same 16-byte G479 record — no added load, no new address chain.
        const bool g489CanBranch =
            (g410Pair->g425Flags & G489_PF_CANBRANCH) != 0u;
#endif
#if DC2_VU1_LOOP_DIAG || !DC2_G489_PCSKIP
        // The G71/G72/G137/G140 probes below all read `postExecPc`, so the diagnostic copy keeps
        // the unconditional read-back. It is never the promoted path.
        const uint32_t postExecPc = m_state.pc;
#endif
#if DC2_G489_PCSKIP
        // The oracle lives OUTSIDE the DIAG copy guard on purpose: `g489Verify` is a conjunct of
        // `g443Fixed`, so arming it drops the run onto a NON-fixed copy — and if the check were
        // only in the lean copy it would silently never execute (which is exactly what the first
        // G489 oracle run did: `[G489:armed]` printed, `[G489:verify]` never did). `m_state.pc`
        // here is the same value the diagnostic copy calls `postExecPc`.
        if (g489Verify)
        {
            if (!g489CanBranch && m_state.pc != currentPc)
                g489VerifyFail(currentPc, m_state.pc, lower);
            g489NoteVerified(g489CanBranch);
        }
#endif

#if DC2_VU1_LOOP_DIAG
        // G295 diag region D (post-exec): G72 VI1/dispatch, G71 gate, G137 provenance, G140
        // clip census. Interior indentation intentionally unchanged.
        if (g295Diag)
        {

        // G72: trace every change to VI1 in the title/model program (startPc=0x10). The
        // tristrip/trifan packer cull gate IBEQ VI10,VI1 needs VI1==208 to draw but VI1 reads 0.
        // Log pc + old/new VI1 to find VI1's source (input header vs computed). Env DC2_G72_VI1.
        {
            static const bool s_g72vi1 = (std::getenv("DC2_G72_VI1") != nullptr);
            if (s_g72vi1 && startPc == 0x10u && (int16_t)g72_vi1_before != (int16_t)m_state.vi[1])
            {
                static std::atomic<uint32_t> s_n{0};
                if (s_n.fetch_add(1u, std::memory_order_relaxed) < 80u)
                    std::fprintf(stderr, "[G72:vi1] pc=0x%x VI1 %d -> %d (lo=0x%08x)\n",
                                 currentPc, (int)(int16_t)g72_vi1_before,
                                 (int)(int16_t)m_state.vi[1], lower);
            }
        }

        // G71 probe: ground-truth the title-map cull gate. Logs MAC + the integer regs the
        // FMEQ/IBEQ chain reads at the decision PCs, and whether each branch was taken
        // (postExecPc != currentPc for a branch op). Env DC2_G71_PROBE; startPc==0x10 only.
        {
            static const bool s_g71probe = (std::getenv("DC2_G71_PROBE") != nullptr);
            if (s_g71probe && startPc == 0x10u)
            {
                static std::atomic<uint32_t> s_g71n{0};
                const bool atGate =
                    currentPc == 0x1d20u || currentPc == 0x1d28u || currentPc == 0x1d48u ||
                    currentPc == 0x1f00u || currentPc == 0x1f08u || currentPc == 0x1f50u ||
                    currentPc == 0x1f68u || currentPc == 0x2108u || currentPc == 0x2128u;
                if (atGate && s_g71n.fetch_add(1u, std::memory_order_relaxed) < 120u)
                {
                    const bool taken = (postExecPc != currentPc);
                    std::fprintf(stderr,
                        "[G71:gate] pc=0x%x mac=0x%04x VI1=%d VI7=%d VI8=%d VI9=%d VI10=%d VI11=%d taken=%d\n",
                        currentPc, (unsigned)(m_state.mac & 0xFFFFu),
                        (int)(int16_t)m_state.vi[1], (int)(int16_t)m_state.vi[7],
                        (int)(int16_t)m_state.vi[8], (int)(int16_t)m_state.vi[9],
                        (int)(int16_t)m_state.vi[10], (int)(int16_t)m_state.vi[11], (int)taken);
                }
            }
        }

        // G72 probe: the title VU program (startPc=0x10) has a per-batch DISPATCHER at
        // 0x0708..0x0810 that BALs to ONE of four GIF-pack subroutines based on the batch
        // header selectors VI1/VI7/VI8 (loaded by ILW VF1,1(VI12) etc):
        //   0x0730 IBEQ VI0,VI7 ->0x7b0 (VI7==0 path: -> 0x1c50 / 0x1b68)
        //   0x0760 IBEQ VI1,VI10(=4) ->0x790 (VI1==4 -> 0x1ff0; else -> 0x1dc0 = block2 +2048)
        //   0x07b0 IBEQ VI0,VI8 ->0x7e0 (VI8==0 -> 0x1b68; else -> 0x1c50)
        // G71 found the runner runs ONLY block2 (0x1dc0). This probe logs the selector values +
        // branch-taken at the decision PCs and COUNTS how often each of the 4 packers is entered,
        // so we can tell whether the EE feeds only block2-type batches (G66 EE-side delivery) or
        // the dispatch mis-routes. Env DC2_G72_DISP; startPc==0x10 only; quiet default.
        {
            static const bool s_g72disp = (std::getenv("DC2_G72_DISP") != nullptr);
            if (s_g72disp && startPc == 0x10u)
            {
                static std::atomic<uint64_t> s_p1b68{0}, s_p1c50{0}, s_p1dc0{0}, s_p1ff0{0};
                static std::atomic<uint32_t> s_g72n{0}, s_g72rep{0};
                if (currentPc == 0x1b68u) s_p1b68.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1c50u) s_p1c50.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1dc0u) s_p1dc0.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1ff0u) s_p1ff0.fetch_add(1u, std::memory_order_relaxed);
                const bool atDisp =
                    currentPc == 0x0730u || currentPc == 0x0760u || currentPc == 0x07b0u;
                if (atDisp && s_g72n.fetch_add(1u, std::memory_order_relaxed) < 200u)
                {
                    const bool taken = (postExecPc != currentPc);
                    std::fprintf(stderr,
                        "[G72:disp] pc=0x%x VI1=%d VI7=%d VI8=%d VI10=%d VI12=%d taken=%d\n",
                        currentPc,
                        (int)(int16_t)m_state.vi[1], (int)(int16_t)m_state.vi[7],
                        (int)(int16_t)m_state.vi[8], (int)(int16_t)m_state.vi[10],
                        (int)(int16_t)m_state.vi[12], (int)taken);
                }
                // Per-vertex cull gates inside the tristrip (0x1d48) + trifan (0x2128) packers:
                // IBEQ VI10,VI1 -> skip the +2048 ADC (taken = DRAW; fall-through = +2048 = CULL).
                // Tally taken vs not for each, plus a few sample operand values.
                static std::atomic<uint64_t> s_tsDraw{0}, s_tsCull{0}, s_tfDraw{0}, s_tfCull{0};
                static std::atomic<uint32_t> s_gateSamp{0};
                if (currentPc == 0x1d48u || currentPc == 0x2128u)
                {
                    const bool taken = (postExecPc != currentPc); // taken = draw
                    if (currentPc == 0x1d48u) { if (taken) s_tsDraw.fetch_add(1u); else s_tsCull.fetch_add(1u); }
                    else { if (taken) s_tfDraw.fetch_add(1u); else s_tfCull.fetch_add(1u); }
                    if (s_gateSamp.fetch_add(1u, std::memory_order_relaxed) < 40u)
                        std::fprintf(stderr,
                            "[G72:gate] pc=0x%x VI10=%d VI1=%d VI3=%d mac=0x%04x taken(draw)=%d\n",
                            currentPc, (int)(int16_t)m_state.vi[10], (int)(int16_t)m_state.vi[1],
                            (int)(int16_t)m_state.vi[3], (unsigned)(m_state.mac & 0xFFFFu), (int)taken);
                    if (((s_tsDraw.load()+s_tsCull.load()+s_tfDraw.load()+s_tfCull.load()) % 20000u) == 0u)
                        std::fprintf(stderr,
                            "[G72:gatetally] tstrip draw=%llu cull=%llu | trifan draw=%llu cull=%llu\n",
                            (unsigned long long)s_tsDraw.load(), (unsigned long long)s_tsCull.load(),
                            (unsigned long long)s_tfDraw.load(), (unsigned long long)s_tfCull.load());
                }
                // periodic packer-selection tally
                if (currentPc == 0x0810u &&
                    (s_g72rep.fetch_add(1u, std::memory_order_relaxed) % 256u) == 0u)
                {
                    std::fprintf(stderr,
                        "[G72:packers] tristrip0x1c50=%llu trifan0x1ff0=%llu tri0x1dc0(+2048)=%llu p0x1b68=%llu\n",
                        (unsigned long long)s_p1c50.load(), (unsigned long long)s_p1ff0.load(),
                        (unsigned long long)s_p1dc0.load(), (unsigned long long)s_p1b68.load());
                }
            }
        }

        // G137: matched-packet dispatcher provenance. G136 proved the final copy XGKICK is all-draw;
        // this logs the live route decision before the copy packer overwrites VI1/VI7/VI8, and ties
        // the selector/header qwords back to the VIF UNPACK origin map. Default-off and only active
        // while G136 has armed a matched VIF stream.
        {
            static std::atomic<uint32_t> s_g137DispLogged{0};
            const uint32_t seq = g_dc2G136TraceActive.load(std::memory_order_relaxed);
            const bool atDisp = currentPc == 0x0730u || currentPc == 0x0760u || currentPc == 0x07b0u;
            const bool atPacker = currentPc == 0x1b68u || currentPc == 0x1c50u ||
                                  currentPc == 0x1dc0u || currentPc == 0x1ff0u;
            if (seq != 0u && g137_trace_enabled() && startPc == 0x10u &&
                (atDisp || atPacker))
            {
                const uint32_t n = s_g137DispLogged.fetch_add(1u, std::memory_order_relaxed);
                if (n < g137_disp_limit())
                {
                    uint32_t q38[4] = {};
                    uint32_t h0[4] = {};
                    uint32_t h1[4] = {};
                    const uint32_t vi12 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[12])) & 0x3FFu;
                    if (vuData && 0x26u * 16u + 16u <= dataSize)
                        std::memcpy(q38, vuData + 0x26u * 16u, sizeof(q38));
                    if (vuData && vi12 * 16u + 16u <= dataSize)
                        std::memcpy(h0, vuData + vi12 * 16u, sizeof(h0));
                    const uint32_t vi12Next = (vi12 + 1u) & 0x3FFu;
                    if (vuData && vi12Next * 16u + 16u <= dataSize)
                        std::memcpy(h1, vuData + vi12Next * 16u, sizeof(h1));

                    const Dc2G137VuOrigin &o38 = g_dc2G137VuOrigins[0x26u];
                    const Dc2G137VuOrigin &oh0 = g_dc2G137VuOrigins[vi12];
                    const Dc2G137VuOrigin &oh1 = g_dc2G137VuOrigins[vi12Next];
                    std::fprintf(stderr,
                                 "[G137:disp] seq=%u n=%u pc=0x%x post=0x%x kind=%s taken=%u vi1=0x%x vi7=0x%x vi8=0x%x vi10=0x%x vi12=0x%x q38=%08x %08x %08x %08x q38orig(v=%u cmd=0x%x src=0x%x raw=%08x %08x %08x %08x) h0=%08x %08x %08x %08x h0orig(v=%u cmd=0x%x src=0x%x) h1=%08x %08x %08x %08x h1orig(v=%u cmd=0x%x src=0x%x)\n",
                                 seq, n, currentPc, postExecPc, atPacker ? "packer" : "branch",
                                 (postExecPc != currentPc) ? 1u : 0u,
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[1])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[8])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[10])),
                                 vi12,
                                 q38[0], q38[1], q38[2], q38[3],
                                 o38.valid, o38.cmdOff, o38.srcOff,
                                 o38.raw[0], o38.raw[1], o38.raw[2], o38.raw[3],
                                 h0[0], h0[1], h0[2], h0[3],
                                 oh0.valid, oh0.cmdOff, oh0.srcOff,
                                 h1[0], h1[1], h1[2], h1[3],
                                 oh1.valid, oh1.cmdOff, oh1.srcOff);
                }
            }
        }

        // G140: CLIP-route census (env DC2_G140_CLIP). The REAL trifan source is the
        // selector-bit1 (fc4) CLIP dispatcher at 0x600: bit1 batches go to the CLIP
        // packers 0x21b0 (tri) / 0x23e8 (tstrip) -> polygon clipper 0x2740 -> clipped
        // polys emitted as TRIFANS from the template at VU 780 (XGKICK 0x2d88/0x2f38).
        // HW's 1012 wall trifan verts come from here; the runner emits none. Count the
        // route entries + the FCAND/FCOR gates + the emitted fan nloop to find which
        // stage dies. Quiet unless enabled; startPc==0x10 only.
        {
            static const bool s_g140clip = (std::getenv("DC2_G140_CLIP") != nullptr);
            if (s_g140clip && startPc == 0x10u)
            {
                // one-shot pc trace: record 800 executed pcs starting at a clipper entry
                static std::atomic<int> s_trcState{0}; // 0=armed-wait 1=recording 2=done
                static uint16_t s_trc[800];
                static uint16_t s_trcVi11[800];
                static int s_trcN = 0;
                static std::atomic<uint32_t> s_trcSkip{0};
                int st = s_trcState.load(std::memory_order_relaxed);
                if (st == 0 && currentPc == 0x2740u &&
                    s_trcSkip.fetch_add(1u, std::memory_order_relaxed) == 0u)
                    s_trcState.store(1, std::memory_order_relaxed), st = 1;
                if (st == 1)
                {
                    s_trc[s_trcN] = (uint16_t)currentPc;
                    s_trcVi11[s_trcN] = (uint16_t)m_state.vi[11];
                    ++s_trcN;
                    if (s_trcN >= 800)
                    {
                        s_trcState.store(2, std::memory_order_relaxed);
                        for (int i = 0; i < 800; i += 8)
                            std::fprintf(stderr, "[G140:trc] %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d\n",
                                         s_trc[i], (int16_t)s_trcVi11[i], s_trc[i+1], (int16_t)s_trcVi11[i+1],
                                         s_trc[i+2], (int16_t)s_trcVi11[i+2], s_trc[i+3], (int16_t)s_trcVi11[i+3],
                                         s_trc[i+4], (int16_t)s_trcVi11[i+4], s_trc[i+5], (int16_t)s_trcVi11[i+5],
                                         s_trc[i+6], (int16_t)s_trcVi11[i+6], s_trc[i+7], (int16_t)s_trcVi11[i+7]);
                    }
                }
                static std::atomic<uint64_t> s_e21b0{0}, s_e23e8{0}, s_e2740{0}, s_k2d88{0}, s_k2f38{0};
                static std::atomic<uint64_t> s_fcandHit{0}, s_fcandMiss{0}, s_fcorRej{0}, s_fanVerts{0};
                static std::atomic<uint32_t> s_rep{0}, s_samp{0};
                // G545 v10: retain the two cross-product operands/result until the delayed MAC
                // consumer. Only the existing default-off G140 diagnostic copy reaches this.
                static thread_local uint32_t s_g545OpmulaA = 0u, s_g545OpmulaB = 0u;
                static thread_local uint32_t s_g545AccZ = 0u, s_g545OpmsubA = 0u;
                static thread_local uint32_t s_g545OpmsubB = 0u, s_g545ResultZ = 0u;
                switch (currentPc)
                {
                case 0x21b0u: s_e21b0.fetch_add(1u, std::memory_order_relaxed); break;
                case 0x23e8u: s_e23e8.fetch_add(1u, std::memory_order_relaxed); break;
                case 0x2740u:
                {
                    s_e2740.fetch_add(1u, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_entSamp{0};
                    if (s_entSamp.fetch_add(1u, std::memory_order_relaxed) < 10u)
                        std::fprintf(stderr,
                            "[G140:ent] v18=(%.4g,%.4g,%.4g,%.4g) v19=(%.4g,%.4g,%.4g,%.4g) v20=(%.4g,%.4g,%.4g,%.4g)\n",
                            m_state.vf[18][0], m_state.vf[18][1], m_state.vf[18][2], m_state.vf[18][3],
                            m_state.vf[19][0], m_state.vf[19][1], m_state.vf[19][2], m_state.vf[19][3],
                            m_state.vf[20][0], m_state.vf[20][1], m_state.vf[20][2], m_state.vf[20][3]);
                    break;
                }
                // per-plane pass-end bail census: IBEQ VI0,VI11 -> 0x2f48 pcs
                case 0x2988u: case 0x2a38u: case 0x2ae8u:
                case 0x2b98u: case 0x2c48u: case 0x2cf8u:
                {
                    static std::atomic<uint64_t> s_passZero[6]{}, s_passLive[6]{};
                    const uint32_t pi = currentPc == 0x2988u ? 0u : currentPc == 0x2a38u ? 1u :
                                        currentPc == 0x2ae8u ? 2u : currentPc == 0x2b98u ? 3u :
                                        currentPc == 0x2c48u ? 4u : 5u;
                    const int16_t vi11 = (int16_t)m_state.vi[11];
                    (vi11 == 0 ? s_passZero[pi] : s_passLive[pi]).fetch_add(1u, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_passRep{0};
                    if ((s_passRep.fetch_add(1u, std::memory_order_relaxed) % 4000u) == 0u)
                        std::fprintf(stderr,
                            "[G140:pass] z/l p0=%llu/%llu p1=%llu/%llu p2=%llu/%llu p3=%llu/%llu p4=%llu/%llu p5=%llu/%llu\n",
                            (unsigned long long)s_passZero[0].load(), (unsigned long long)s_passLive[0].load(),
                            (unsigned long long)s_passZero[1].load(), (unsigned long long)s_passLive[1].load(),
                            (unsigned long long)s_passZero[2].load(), (unsigned long long)s_passLive[2].load(),
                            (unsigned long long)s_passZero[3].load(), (unsigned long long)s_passLive[3].load(),
                            (unsigned long long)s_passZero[4].load(), (unsigned long long)s_passLive[4].load(),
                            (unsigned long long)s_passZero[5].load(), (unsigned long long)s_passLive[5].load());
                    break;
                }
                case 0x2d08u: // emit path entered (survived all 6 planes)
                {
                    static std::atomic<uint64_t> s_emit{0};
                    const uint64_t e = s_emit.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (e <= 24u || (e % 2000u) == 0u)
                        std::fprintf(stderr, "[G140:emit] n=%llu vi11=%d\n",
                                     (unsigned long long)e, (int)(int16_t)m_state.vi[11]);
                    break;
                }
                case 0x30d0u: // FCGET VI1 just executed
                {
                    static std::atomic<uint32_t> s_fcSamp{0};
                    if (s_fcSamp.fetch_add(1u, std::memory_order_relaxed) < 40u)
                        std::fprintf(stderr, "[G140:fcget] vi1=0x%03x vi5=0x%x vi6=0x%x clip=0x%06x\n",
                                     (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[5],
                                     (uint32_t)(uint16_t)m_state.vi[6], m_state.clip & 0xFFFFFFu);
                    break;
                }
                case 0x30e8u: case 0x3108u: // A/B inside-test IBEQ just executed
                {
                    static std::atomic<uint32_t> s_ibSamp{0};
                    if (s_ibSamp.fetch_add(1u, std::memory_order_relaxed) < 24u)
                        std::fprintf(stderr, "[G140:ib] pc=0x%x vi1=0x%x vi4=0x%x vi5=0x%x vi6=0x%x post=0x%x\n",
                                     currentPc, (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[4],
                                     (uint32_t)(uint16_t)m_state.vi[5], (uint32_t)(uint16_t)m_state.vi[6], postExecPc);
                    break;
                }
                case 0x30b0u: // both CLIPw executed; VF17=A pos, VF21=B pos
                {
                    static std::atomic<uint32_t> s_cwSamp{0};
                    if (s_cwSamp.fetch_add(1u, std::memory_order_relaxed) < 18u)
                        std::fprintf(stderr,
                            "[G140:cw] vi8=0x%x A=(%.4g,%.4g,%.4g,%.4g) B=(%.4g,%.4g,%.4g,%.4g) clip12=0x%03x\n",
                            (uint32_t)(uint16_t)m_state.vi[8],
                            m_state.vf[17][0], m_state.vf[17][1], m_state.vf[17][2], m_state.vf[17][3],
                            m_state.vf[21][0], m_state.vf[21][1], m_state.vf[21][2], m_state.vf[21][3],
                            m_state.clip & 0xFFFu);
                    break;
                }
                case 0x22d8u: case 0x2548u:
                    // FCAND VI1,0x3ffff just executed this pair (lower). VI1!=0 => tri touches a plane.
                    if (m_state.vi[1] != 0) s_fcandHit.fetch_add(1u, std::memory_order_relaxed);
                    else s_fcandMiss.fetch_add(1u, std::memory_order_relaxed);
                    break;
                case 0x22e0u: case 0x2550u:
                    break;
                case 0x2d88u:
                case 0x2f38u:
                {
                    (currentPc == 0x2d88u ? s_k2d88 : s_k2f38).fetch_add(1u, std::memory_order_relaxed);
                    // VI7 = kick addr (780). Read the fan giftag nloop being kicked.
                    const uint32_t kickQw = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])) & 0x3FFu;
                    if (vuData && kickQw * 16u + 16u <= dataSize)
                    {
                        uint32_t w0 = 0u, w1 = 0u;
                        std::memcpy(&w0, vuData + kickQw * 16u, 4);
                        std::memcpy(&w1, vuData + kickQw * 16u + 4u, 4);
                        const uint32_t nloop = w0 & 0x7FFFu;
                        s_fanVerts.fetch_add(nloop, std::memory_order_relaxed);
                        if (s_samp.fetch_add(1u, std::memory_order_relaxed) < 24u)
                            std::fprintf(stderr, "[G140:fan] pc=0x%x kickQw=0x%x w0=%08x w1=%08x nloop=%u\n",
                                         currentPc, kickQw, w0, w1, nloop);
                    }
                    break;
                }
                case 0x2ee0u:
                    std::memcpy(&s_g545OpmulaA, &m_state.vf[21][0], sizeof(uint32_t));
                    std::memcpy(&s_g545OpmulaB, &m_state.vf[20][1], sizeof(uint32_t));
                    std::memcpy(&s_g545AccZ, &m_state.acc[2], sizeof(uint32_t));
                    break;
                case 0x2ee8u:
                    std::memcpy(&s_g545OpmsubA, &m_state.vf[20][0], sizeof(uint32_t));
                    std::memcpy(&s_g545OpmsubB, &m_state.vf[21][1], sizeof(uint32_t));
                    std::memcpy(&s_g545ResultZ, &m_state.vf[23][2], sizeof(uint32_t));
                    break;
                case 0x2f08u:
                    if ((s_g545ResultZ & 0x7f800000u) == 0x7f800000u)
                    {
                        static std::atomic<uint32_t> s_g545MacGateSamples{0u};
                        if (s_g545MacGateSamples.fetch_add(1u, std::memory_order_relaxed) < 128u)
                            std::fprintf(stderr,
                                "[G545:cpu-macgate] accZ=%08x resultZ=%08x visible=%04x expected=%04x mask=%04x opmula=%08x*%08x opmsub=%08x*%08x\n",
                                s_g545AccZ, s_g545ResultZ, s_vuMacVisible & 0xffffu,
                                (uint32_t)(uint16_t)m_state.vi[10],
                                (uint32_t)(uint16_t)m_state.vi[7], s_g545OpmulaA,
                                s_g545OpmulaB, s_g545OpmsubA, s_g545OpmsubB);
                    }
                    break;
                default: break;
                }
                if ((s_rep.fetch_add(1u, std::memory_order_relaxed) % 400000u) == 0u)
                    std::fprintf(stderr,
                        "[G140:clip] e21b0=%llu e23e8=%llu clip2740=%llu kick2d88=%llu kick2f38=%llu "
                        "fcandHit=%llu fcandMiss=%llu fanVerts=%llu\n",
                        (unsigned long long)s_e21b0.load(), (unsigned long long)s_e23e8.load(),
                        (unsigned long long)s_e2740.load(), (unsigned long long)s_k2d88.load(),
                        (unsigned long long)s_k2f38.load(), (unsigned long long)s_fcandHit.load(),
                        (unsigned long long)s_fcandMiss.load(), (unsigned long long)s_fanVerts.load());
            }
        }

        } // G295 diag region D end
#endif // DC2_VU1_LOOP_DIAG

        // Enforce VF0/VI0 invariants. G425: skipped when this pair provably cannot name
        // register 0 as a destination, which makes the re-pin the identity.
        if (!g430SkipHaz && !g435SkipRepin &&
            (!g425Lean || (g410Pair->g425Flags & G425_PF_TOUCHES_ZERO) != 0u))
        {
#if DC2_G484_WIDEVF0
            // G484: ONE 16-byte store of exactly the four constants the scalar form writes.
            // Nothing can observe the store granularity — this is the last statement of the pair
            // and VU1State is thread-private — but the next pair's 16-byte `_mm_loadu_ps` over
            // these bytes can now be store-to-load forwarded instead of waiting for four narrow
            // stores to retire to L1.
            _mm_storeu_ps(m_state.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
#else
            if (g435WideVf0)
            {
                // G435: ONE 16-byte store of the same four constants (see the comment at the
                // declaration of g435WideVf0). Value-identical to the four scalar stores below.
                _mm_storeu_ps(m_state.vf[0], g435Vf0Const);
            }
            else
            {
                // Enforce VF0 invariant
                m_state.vf[0][0] = 0.0f;
                m_state.vf[0][1] = 0.0f;
                m_state.vf[0][2] = 0.0f;
                m_state.vf[0][3] = 1.0f;
            }
#endif
            // Enforce VI0 invariant
            m_state.vi[0] = 0;
        }
        else if (s_g425Verify)
        {
            if (m_state.vf[0][0] != 0.0f || m_state.vf[0][1] != 0.0f ||
                m_state.vf[0][2] != 0.0f || m_state.vf[0][3] != 1.0f ||
                m_state.vi[0] != 0)
            {
                std::fprintf(stderr,
                             "[G425:verify] VF0/VI0 invariant broken pc=0x%x lower=%08x "
                             "upper=%08x vf0=(%g %g %g %g) vi0=%d\n",
                             currentPc, lower, upper,
                             m_state.vf[0][0], m_state.vf[0][1], m_state.vf[0][2],
                             m_state.vf[0][3], (int)m_state.vi[0]);
                std::abort();
            }
        }

        // G479 (REJECTED): folding this wrap to `& (PS2_VU1_CODE_SIZE - 1)` — exact, since pc is
        // a multiple of 8 below a power-of-two code store — read +0.93 ms/f (t = +6.4, 0/5
        // windows) on top of the fused record, a clean sign flip from its −0.91. Removing two
        // compares from this loop is not free; do not rebuild it.
        uint32_t sequentialPC = currentPc + 8;
        if (sequentialPC >= codeSize)
            sequentialPC = 0;
        uint32_t nextPC = sequentialPC;

        // G442: `branchDelayActive` was a second loop-carried local saying only "pendingBranchTarget
        // holds a value". G427 measured ONE extra loop-carried local in this loop at +2.8 ms/f, so
        // the inverse is worth testing: fold the flag into the value with a sentinel that cannot be
        // a real PC (every real target is masked to 14 bits). Value-identical — the flag was set
        // exactly when the target was written and cleared exactly when it was consumed.
        if (pendingBranchTarget != DC2_G442_NO_BRANCH)
        {
            nextPC = pendingBranchTarget;
            pendingBranchTarget = DC2_G442_NO_BRANCH;
        }
#if DC2_VU1_LOOP_DIAG || !DC2_G489_PCSKIP
        else if (postExecPc != currentPc)
        {
            pendingBranchTarget = postExecPc + 8;
            if (pendingBranchTarget >= codeSize)
                pendingBranchTarget = 0;
        }
#else
        // G489 (refuted, compiled out): `&&` short-circuits, so a non-branch pair never touches
        // `m_state.pc` here.
        else if (g489CanBranch && m_state.pc != currentPc)
        {
            pendingBranchTarget = m_state.pc + 8;
            if (pendingBranchTarget >= codeSize)
                pendingBranchTarget = 0;
        }
#endif

        if (nextPC >= codeSize)
            nextPC = 0;
        m_state.pc = nextPC;
        G483_PC_SET(nextPC);

        if (m_state.ebit)
        {
            stopReason = "ebit-latched";
            G479_QP_SYNC();
            break;
        }

        if (eBit)
            m_state.ebit = true;
    }

    // The loop can also fall out at maxCycles; the run() tail reads *s_pDelayP directly.
    G479_QP_SYNC();
#undef G483_PC_CUR
#undef G483_PC_SET
#undef G479_QP_RELOAD
#undef G479_QP_SYNC
#undef G479_PD
#undef G479_QD
#if G484_LOCAL_DEFAULT
#undef DC2_G484_WIDEVF0
#undef G484_LOCAL_DEFAULT
#endif
#if G485_LOCAL_DEFAULT
#undef DC2_G485_HAZREG
#undef G485_LOCAL_DEFAULT
#endif
#if G489_LOCAL_DEFAULT
#undef DC2_G489_PCSKIP
#undef G489_LOCAL_DEFAULT
#endif
#if G490_LOCAL_DEFAULT
#undef DC2_G490_BLOCKRUN
#undef G490_LOCAL_DEFAULT
#endif
#if G490_VERIFY_LOCAL_DEFAULT
#undef DC2_G490_VERIFY_BLOCK
#undef G490_VERIFY_LOCAL_DEFAULT
#endif
