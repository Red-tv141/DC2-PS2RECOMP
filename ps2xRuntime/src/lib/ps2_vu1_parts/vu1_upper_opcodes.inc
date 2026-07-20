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

    const uint32_t startPc = m_state.pc;
    uint32_t cycle = 0u;
    g141Scope.unitPtr = &cycle;
    g141Scope.unitSum = &s_g141CycleSum;
    g141Scope.unitLabel = "cycles";
    uint32_t lastPc = m_state.pc;
    uint32_t lastLower = 0u;
    uint32_t lastUpper = 0u;
    const char *stopReason = "maxcycles";
    uint32_t pendingBranchTarget = 0u;
    bool branchDelayActive = false;

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
    uint32_t *const s_macPipeP     = s_vuMacPipe;
    uint32_t *const s_statusPipeP  = s_vuStatusPipe;
    uint32_t *const s_macVisP      = &s_vuMacVisible;
    uint32_t *const s_statusVisP   = &s_vuStatusVisible;
    int      *const s_qDelayP      = &s_vuQDelay;
    int      *const s_pDelayP      = &s_vuPDelay;

    for (; cycle < maxCycles; ++cycle)
    {
        const uint32_t currentPc = m_state.pc;
        const int16_t g72_vi1_before = (int16_t)m_state.vi[1];
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

        if (currentPc + 8 > codeSize)
        {
            stopReason = "pc-oob";
            break;
        }

        uint32_t lower, upper;
        std::memcpy(&lower, vuCode + currentPc, 4);
        std::memcpy(&upper, vuCode + currentPc + 4, 4);
        lastPc = currentPc;
        lastLower = lower;
        lastUpper = upper;

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

        bool eBit = (upper >> 30) & 1;

        // G27: LOI is signaled by the I-bit (bit 31 of the UPPER word). When set,
        // the LOWER 32 bits are a 32-bit float immediate loaded into the I register
        // and must NOT be decoded as a lower instruction; the upper still executes.
        // The old code keyed LOI on a magic lower value (0x8000033C) that DC2's VU
        // programs never emit, so real LOI immediates (e.g. 0x457ff000 ~= 4093.0f)
        // were decoded as bogus lower ops (0x22/0x23), corrupting the model transform
        // and derailing the microprogram (runaway -> maxcycles, garbage GIF output).
        bool iBit = (upper >> 31) & 1;

        // G87: advance the Q pipeline one cycle BEFORE this word executes. A pending DIV/SQRT/
        // RSQRT result becomes visible in m_state.q when its delay expires; a producer in THIS
        // word (re)arms the delay afterwards (in execLower) so it counts from the next word.
        if (*s_qDelayP > 0 && --(*s_qDelayP) == 0)
            m_state.q = s_vuQPending;

        // G239: advance the EFU pipeline before this instruction pair. An MFP in the pair
        // where the latency expires sees the newly published architectural P, matching the
        // interpreter ordering in PCSX2 (_vuTestPipes before lower execution).
        if (s_ppipeOn && *s_pDelayP > 0 && --(*s_pDelayP) == 0)
            m_state.p = s_vuPPending;

        // G239: PCSX2's interpreter performs _vuTestLowerStalls + _vuTestPipes before
        // executing either half of the instruction pair. Publish an older scalar result here
        // when this lower op would stall FDIV/EFU; execLower then arms the new producer (or
        // observes an already-completed WAIT) after the parallel upper has consumed the value.
        if (!iBit && s_scalarPrestallOn)
        {
            const uint8_t lowerOpHi239 = static_cast<uint8_t>((lower >> 25u) & 0x7fu);
            if (lowerOpHi239 == 0x40u)
            {
                const uint8_t lowerS2239 = S2_FUNC(lower);
                const bool waitQ239 = lowerS2239 == 0x3bu;
                const bool fdivProducer239 = lowerS2239 >= 0x38u && lowerS2239 <= 0x3au;
                if (s_qlatOn && *s_qDelayP > 0 &&
                    (waitQ239 || (fdivProducer239 && g200_qstall_enabled())))
                {
                    m_state.q = s_vuQPending;
                    *s_qDelayP = 0;
                }

                const bool waitP239 = lowerS2239 == 0x7bu;
                const bool efuProducer239 =
                    lowerS2239 == 0x70u || lowerS2239 == 0x71u ||
                    lowerS2239 == 0x72u || lowerS2239 == 0x73u ||
                    lowerS2239 == 0x76u || lowerS2239 == 0x78u ||
                    lowerS2239 == 0x79u || lowerS2239 == 0x7au;
                if (s_ppipeOn && *s_pDelayP > 0 && (waitP239 || efuProducer239))
                {
                    m_state.p = s_vuPPending;
                    *s_pDelayP = 0;
                }
            }
        }

        // G138: advance the MAC/STATUS flag pipeline one instruction pair. Flag-consuming
        // lower ops executed THIS pair (FMEQ/FMAND/FMOR/FSAND/FSEQ/FSOR) read the snapshot
        // from DEPTH pairs ago via s_vuMacVisible/s_vuStatusVisible. (G141: flag + TLS base
        // hoisted to loop-invariant locals; identical semantics.)
        if (s_macpipeOn)
        {
            const int g138d = s_macpipeDepth;
            *s_macVisP = s_macPipeP[g138d - 1];
            *s_statusVisP = s_statusPipeP[g138d - 1];
            for (int g138i = g138d - 1; g138i > 0; --g138i)
            {
                s_macPipeP[g138i] = s_macPipeP[g138i - 1];
                s_statusPipeP[g138i] = s_statusPipeP[g138i - 1];
            }
        }

        if (iBit)
        {
            std::memcpy(&m_state.i, &lower, 4);
            execUpper(upper);
        }
        else
        {
            const uint32_t g106MacBeforeUpper = m_state.mac;
            const uint32_t g106StatusBeforeUpper = m_state.status;
            // G139: locate the upper op's VF destination (fd for main-space ops; ft for
            // ITOF/FTOI/ABS in the special group; none for ACC/CLIP writers) so the lower
            // op of this pair can read the pre-upper value (see g139_pairhaz_enabled).
            float *g139dst = nullptr;
            float g139old[4], g139new[4];
            if (s_pairhazOn)
            {
                const uint8_t g139op = (uint8_t)(upper & 0x3Fu);
                uint32_t g139DstIdx = 0u;
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
                // G295: every lower-slot VF access (read or write) names its register in the
                // fs (bits 11-15) or ft (bits 16-20) field. If neither equals the upper's dest,
                // the save/restore/overlay round-trip is the identity — skip it. A VI-indexed
                // false positive just keeps the legacy swap. Kill: DC2_G295_NO_HAZSKIP=1.
                if (g139dst != nullptr && !s_g295KeepHaz &&
                    ((lower >> 11u) & 0x1Fu) != g139DstIdx &&
                    ((lower >> 16u) & 0x1Fu) != g139DstIdx)
                    g139dst = nullptr;
                if (g139dst)
                    std::memcpy(g139old, g139dst, 16);
            }
            execUpper(upper);
            if (g139dst)
            {
                std::memcpy(g139new, g139dst, 16);
                std::memcpy(g139dst, g139old, 16);
            }
            if (s_g106ParallelFlags)
            {
                s_g106LowerMacRead = g106MacBeforeUpper;
                s_g106LowerStatusRead = g106StatusBeforeUpper;
                s_g106UseLowerFlagRead = true;
            }
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
            execLower(lower, vuData, dataSize, gs, memory, upper);
            s_g106UseLowerFlagRead = false;
            if (g139dst)
            {
                // The upper result lands after the lower's same-cycle access: overlay the
                // upper's dest-masked lanes over whatever the lower left in the register.
                const uint8_t g139m = (uint8_t)((upper >> 21) & 0xFu);
                if (g139m & 0x8u) g139dst[0] = g139new[0];
                if (g139m & 0x4u) g139dst[1] = g139new[1];
                if (g139m & 0x2u) g139dst[2] = g139new[2];
                if (g139m & 0x1u) g139dst[3] = g139new[3];
            }
        }
        // G138: the newest pipeline entry is this pair's post-upper architectural flags.
        if (s_macpipeOn)
        {
            s_macPipeP[0] = m_state.mac;
            s_statusPipeP[0] = m_state.status;
        }
        const uint32_t postExecPc = m_state.pc;

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

        // Enforce VF0 invariant
        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        // Enforce VI0 invariant
        m_state.vi[0] = 0;

        uint32_t sequentialPC = currentPc + 8;
        if (sequentialPC >= codeSize)
            sequentialPC = 0;
        uint32_t nextPC = sequentialPC;

        if (branchDelayActive)
        {
            nextPC = pendingBranchTarget;
            branchDelayActive = false;
        }
        else if (postExecPc != currentPc)
        {
            pendingBranchTarget = postExecPc + 8;
            if (pendingBranchTarget >= codeSize)
                pendingBranchTarget = 0;
            branchDelayActive = true;
        }

        if (nextPC >= codeSize)
            nextPC = 0;
        m_state.pc = nextPC;

        if (m_state.ebit)
        {
            stopReason = "ebit-latched";
            break;
        }

        if (eBit)
            m_state.ebit = true;
    }

    // The hardware drains EFU (and FDIV/FMAC) when an E-bit program ends. Preserve an in-flight
    // P only across max-cycle suspension/resume; a normal microprogram completion publishes it.
    if (s_ppipeOn && m_state.ebit && *s_pDelayP > 0)
    {
        m_state.p = s_vuPPending;
        *s_pDelayP = 0;
    }

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
