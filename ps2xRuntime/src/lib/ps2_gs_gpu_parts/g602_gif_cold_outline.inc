// G602 — Layer 4 (GIF decode) hot-path shape.
//
// WHY THIS EXISTS
// ---------------
// G593 §1.1c priced layer 4 at 4.87 ms/f on `s05` (2.79 per-vertex over 24,569 kicks = 114 ns/kick,
// 2.08 per-state-register over ~116k descriptors = 17.9 ns each). The `/MAP` + capstone read of the
// shipped post-G601 binary says where most of that goes, and it is not the decode arithmetic:
//
//   ?writeRegisterPacked@GS@@  141k calls/f   3 shadow saves + 5 pushes + sub rsp,0x100 + 4 movaps
//   ?vertexKick@GS@@            24.6k calls/f 8 pushes + sub rsp,0x148 + NINE movaps (xmm6..xmm14)
//   ?writeRegister@GS@@         55.8k calls/f 7 pushes + sub rsp,0x130 + 2 movaps + /GS cookie
//
// That is exactly 17d §2.1's measured call-boundary class (G487: 8 pushes + big frame + ten movaps
// = ~10 ns ~ 34 cycles per call). None of those nonvolatile XMM saves is needed by the decode; they
// exist because each function textually contains large DEFAULT-OFF diagnostic blocks that print
// floats, and MSVC allocates the callee-saved XMM file for the WHOLE function.
//
// Every helper below is one of those cold blocks, moved out of the hot member and marked
// `__declspec(noinline)` so its register pressure cannot leak back into the caller's prologue.
// Behaviour is unchanged: each helper re-tests the identical predicate the inline block did and the
// call site is guarded by the same flag, now read from a namespace-scope `const bool` instead of a
// function-local magic static (17d §2.1 G488: a function-local `static const bool` in a hot case is
// an `_Init_thread_epoch` TLS compare on every call).
//
// ROLLBACK
// --------
//   DC2_G602_COLD_OUTLINE  0 -> the helpers become `inline`, MSVC folds them back into the callers
//                              and the pre-G602 fat prologues return. Source stays single-copy, so
//                              the two shapes cannot drift (17d §3.1: a *source* duplicate of a hot
//                              body is the thing to avoid, and dead text inside the enclosing
//                              function is not free).
//   DC2_G602_FLAG_GLOBALS  0 -> the hot predicates fall back to the original accessor calls.
//
// There is deliberately NO runtime env arm: this is a pure code-SHAPE change with no semantic
// variable, so a runtime arm could only be a second copy of the same code, which 17d §3.1 shows
// costs real frame time even when nothing branches to it. The payoff is gated cross-binary,
// order-balanced A B B A, on `[G147:gif] parserOther` (a GS-worker sub-timer) with `[G434:inv]`
// proving the two binaries ran the same workload — the same discipline G593 used for its ceiling.

// ONE switch rolls the whole phase back to the pre-G602 shape; the three sub-switches exist so a
// later session can bisect which third paid. Flip DC2_G602_FASTDECODE to 0, rebuild `ps2_runtime`
// + `dc2_runner`, and the binary is the control arm of this phase's A/B.
#ifndef DC2_G602_FASTDECODE
#define DC2_G602_FASTDECODE 1
#endif
#ifndef DC2_G602_COLD_OUTLINE
#define DC2_G602_COLD_OUTLINE DC2_G602_FASTDECODE
#endif
#ifndef DC2_G602_FLAG_GLOBALS
#define DC2_G602_FLAG_GLOBALS DC2_G602_FASTDECODE
#endif
// The PACKED descriptor walk in processGIFPacket: bounds-checked once per tag instead of once per
// descriptor, `tagHi` nibbles read in place instead of staged through a `uint8_t regs[16]`, and the
// loop-invariant `[G171:regstat]` census branch lifted OUT of the ~141 k/frame inner loop into its
// own duplicated loop (G593's remedy — it measured that exact branch at 0.64 ms/f).
#ifndef DC2_G602_TAGWALK
#define DC2_G602_TAGWALK DC2_G602_FASTDECODE
#endif

// The title-rock scope flag is defined at GLOBAL scope in dc2_game_override.cpp. It MUST be
// declared here, above the anonymous namespace: a block-scope `extern` inside an anon-namespace
// function binds to an INTERNAL-linkage symbol and fails to link (the DC2 appendix's
// anon-namespace trap, §3). The pre-G602 sites got away with it because they sat in `GS::` member
// functions, which are at global scope.
extern std::atomic<bool> g_dc2TitleRockScope;

#if DC2_G602_COLD_OUTLINE
#define G602_COLD __declspec(noinline) static
#else
// Rollback must restore the pre-G602 CODEGEN, not just the pre-G602 answer, or the A/B control is
// an intermediate shape and the phase measures short. Plain `inline` is a request MSVC declined for
// the larger blocks (17d §4): with it the control kept only 4 of vertexKick's 9 nonvolatile XMM
// spills. `__forceinline` puts them back where they were.
#define G602_COLD __forceinline static
#endif

namespace
{
    // ---------------------------------------------------------------------------------------
    // §1  Hot-path predicate gates.
    //
    // Namespace-scope (NOT function-local) exactly as G508's `g_g508LockSkip` already is, so the
    // hot path pays one `cmp byte ptr [rip+X], 0` instead of the MSVC magic-static sequence
    // (`mov eax,0x4d3c0 ; mov ecx,[rax+tlsblock] ; cmp [rip+guard],ecx ; jg init`). Dynamic
    // initialisers run after CRT environment setup, which is why `g_g508LockSkip` has been safe
    // here since G508.
    //
    // Each gate is the OR of every flag the cold block it guards can test, so the call site is a
    // single test and the helper re-checks the exact original predicate.
    // ---------------------------------------------------------------------------------------

    // vertexKick
    const bool g_g602KickG196 =
        envFlagEnabled("DC2_G196_ADC_TRACE") || envFlagEnabled("DC2_G268_LIVE_ENVREAD");
    const bool g_g602KickG107 = envFlagEnabled("DC2_G107_ADC_NO_ADVANCE");
    const bool g_g602KickG123 = envFlagEnabled("DC2_G123_SEQ");
    const bool g_g602KickG14 = envFlagEnabled("DC2_TRACE_G14");
    const bool g_g602KickInv = envFlagEnabled("DC2_G434_INV");
    const bool g_g602KickNoDraw = envFlagEnabled("DC2_G434_NO_DRAW");

    // writeRegisterPacked
    const bool g_g602PackedForceDraw = envFlagEnabled("DC2_G65_FORCEDRAW");
    const bool g_g602PackedSynthAdc = envFlagEnabled("DC2_G105_SYNTH_ADC");
    const bool g_g602PackedG52Hold = !envFlagEnabled("DC2_G52_NO_HOLDCOLOR");
    const bool g_g602PackedRgbaqTrace =
        envFlagEnabled("DC2_G52_SEQ") || envFlagEnabled("DC2_PHASE_TRACE");
    const bool g_g602PackedXyzTrace =
        envFlagEnabled("DC2_G52_SEQ") || envFlagEnabled("DC2_TRACE_G31");
    const bool g_g602PackedAdTrace = envFlagEnabled("DC2_TRACE_AD");

    // writeRegister
    const bool g_g602RegCensus = envFlagEnabled("DC2_G508_REGCENSUS");
    const bool g_g602RegPhaseTrace = envFlagEnabled("DC2_PHASE_TRACE");
    const bool g_g602RegQualityTrace = envFlagEnabled("DC2_TRACE_RENDER_QUALITY");
    const bool g_g602RegFrameTrace = envFlagEnabled("DC2_TRACE_FRAME_REGS");
    const bool g_g602RegTex0Trace =
        (std::getenv("DC2_G91_TEX0") != nullptr) ||
        envFlagEnabled("DC2_G123_SEQ") ||
        envFlagEnabled("DC2_TRACE_RENDER_QUALITY");
    const bool g_g602RegBitbltTrace =
        envFlagEnabled("DC2_TRACE_FRAME_REGS") ||
        envFlagEnabled("DC2_TRACE_RENDER_QUALITY") ||
        envFlagEnabled("DC2_TRACE_T8_UPLOAD");
    const bool g_g602RegReservedTrace =
        (std::getenv("DC2_G397_LEGACY_RESERVED_DISPLAY") != nullptr) ||
        (std::getenv("DC2_G397_PRESENT_TRACE") != nullptr);

    // Rollback shape only (DC2_G602_FLAG_GLOBALS 0): the SAME predicates expressed the way the
    // pre-G602 sites did — a function-local `static const bool`, i.e. an MSVC magic static whose
    // guard is an `_Init_thread_epoch` TLS compare on every call. Kept so the rollback restores the
    // original CODEGEN, not merely the original answer.
    bool g602LegacyKickG196() { static const bool v = envFlagEnabled("DC2_G196_ADC_TRACE"); return g268LiveEnvRead() || v; }
    bool g602LegacyKickG107() { static const bool v = envFlagEnabled("DC2_G107_ADC_NO_ADVANCE"); return v; }
    bool g602LegacyKickInv() { static const bool v = envFlagEnabled("DC2_G434_INV"); return v; }
    bool g602LegacyKickNoDraw() { static const bool v = envFlagEnabled("DC2_G434_NO_DRAW"); return v; }
    bool g602LegacyForceDraw() { static const bool v = envFlagEnabled("DC2_G65_FORCEDRAW"); return v; }
    bool g602LegacyG52Hold() { static const bool v = envFlagEnabled("DC2_G52_NO_HOLDCOLOR"); return !v; }
    bool g602LegacyG52Seq() { static const bool v = envFlagEnabled("DC2_G52_SEQ"); return v; }
    bool g602LegacyG91Tex0() { static const bool v = (std::getenv("DC2_G91_TEX0") != nullptr); return v; }
} // namespace  (the gate macros below must not be inside it)

// The hot call sites spell the gate as a macro so `DC2_G602_FLAG_GLOBALS 0` restores the original
// accessor call — i.e. the magic-static guard — at every site without editing one of them.
#if DC2_G602_FLAG_GLOBALS
#define G602_GATE_KICK_G196      g_g602KickG196
#define G602_GATE_KICK_G107      g_g602KickG107
#define G602_GATE_KICK_G123      g_g602KickG123
#define G602_GATE_KICK_G14       g_g602KickG14
#define G602_GATE_KICK_INV       g_g602KickInv
#define G602_GATE_KICK_NODRAW    g_g602KickNoDraw
#define G602_GATE_PK_FORCEDRAW   g_g602PackedForceDraw
#define G602_GATE_PK_SYNTHADC    g_g602PackedSynthAdc
#define G602_GATE_PK_G52HOLD     g_g602PackedG52Hold
#define G602_GATE_PK_RGBAQTRACE  g_g602PackedRgbaqTrace
#define G602_GATE_PK_XYZTRACE    g_g602PackedXyzTrace
#define G602_GATE_PK_ADTRACE     g_g602PackedAdTrace
#define G602_GATE_RG_CENSUS      g_g602RegCensus
#define G602_GATE_RG_PHASE       g_g602RegPhaseTrace
#define G602_GATE_RG_QUALITY     g_g602RegQualityTrace
#define G602_GATE_RG_FRAME       g_g602RegFrameTrace
#define G602_GATE_RG_TEX0        g_g602RegTex0Trace
#define G602_GATE_RG_BITBLT      g_g602RegBitbltTrace
#define G602_GATE_RG_RESERVED    g_g602RegReservedTrace
#else
#define G602_GATE_KICK_G196      g602LegacyKickG196()
#define G602_GATE_KICK_G107      g602LegacyKickG107()
#define G602_GATE_KICK_G123      g123SeqEnabled()
#define G602_GATE_KICK_G14       g14TraceEnabled()
#define G602_GATE_KICK_INV       g602LegacyKickInv()
#define G602_GATE_KICK_NODRAW    g602LegacyKickNoDraw()
#define G602_GATE_PK_FORCEDRAW   g602LegacyForceDraw()
#define G602_GATE_PK_SYNTHADC    g105SynthAdcEnabled()
#define G602_GATE_PK_G52HOLD     g602LegacyG52Hold()
#define G602_GATE_PK_RGBAQTRACE  (g602LegacyG52Seq() || phaseDiagnosticsEnabled())
#define G602_GATE_PK_XYZTRACE    (g602LegacyG52Seq() || g31TraceEnabled())
#define G602_GATE_PK_ADTRACE     f33AdTraceEnabled()
#define G602_GATE_RG_CENSUS      g508RegCensusOn()
#define G602_GATE_RG_PHASE       phaseDiagnosticsEnabled()
#define G602_GATE_RG_QUALITY     renderQualityTraceEnabled()
#define G602_GATE_RG_FRAME       frameRegisterTraceEnabled()
#define G602_GATE_RG_TEX0        (g602LegacyG91Tex0() || g123SeqEnabled() || \
                                  renderQualityTraceEnabled())
#define G602_GATE_RG_BITBLT      (frameRegisterTraceEnabled() || renderQualityTraceEnabled() || \
                                  t8UploadTraceEnabled())
#define G602_GATE_RG_RESERVED    true
#endif

namespace
{

    // ---------------------------------------------------------------------------------------
    // §2  vertexKick's cold blocks
    // ---------------------------------------------------------------------------------------

    // G196 raw per-kick ADC/drawing trace for tbp=0x2a20, scoped identically to G195:fgtri.
    // G268: the live-env form is preserved verbatim — DC2_G268_LIVE_ENVREAD=1 restores the historical
    // per-call getenv, which is why it is folded into this helper's own gate.
    G602_COLD void g602ColdKickG196(uint32_t primType, bool tme, uint32_t tbp0, uint32_t fbp,
                                    bool drawing, uint32_t vtxCount, const GSVertex &v)
    {
        static const bool s_g196AdcTrace = envFlagEnabled("DC2_G196_ADC_TRACE");
        if (!(g268LiveEnvRead() ? envFlagEnabled("DC2_G196_ADC_TRACE") : s_g196AdcTrace))
            return;
        (void)primType;
        if (tme && tbp0 == 0x2a20u && (fbp == 0u || fbp == 0x68u))
        {
            static std::atomic<uint32_t> s_g196{0};
            const uint32_t n = s_g196.fetch_add(1u, std::memory_order_relaxed);
            if (n < 512u)
            {
                std::fprintf(stderr,
                    "[G196:kick] n=%u prim=%u drawing=%u vtxCount=%u fbp=0x%x x=%.3f y=%.3f z=%.1f q=%g\n",
                    n, primType,
                    static_cast<uint32_t>(drawing ? 1u : 0u), vtxCount, fbp,
                    v.x, v.y, v.z, v.q);
            }
        }
    }

    // G107 A/B: ADC is "drawing kick disabled". For title-rock strips, test the stricter reading
    // that an ADC vertex must not advance the primitive queue either. Returns true when the caller
    // must return without advancing.
    G602_COLD bool g602ColdKickG107(uint32_t primType, bool tme, uint32_t tbp0, uint32_t fbp,
                                    uint32_t vtxCount)
    {
        static const bool s_g107NoAdvance = envFlagEnabled("DC2_G107_ADC_NO_ADVANCE");
        if (!s_g107NoAdvance ||
            !g_dc2TitleRockScope.load(std::memory_order_relaxed) ||
            primType != GS_PRIM_TRISTRIP)
            return false;
        if (tme && fbp != 0x139u && g105TitleRockTbp(tbp0))
        {
            static const bool s_stat = envFlagEnabled("DC2_G107_ADC_STAT");
            if (s_stat)
            {
                static std::atomic<uint64_t> s_skip{0};
                const uint64_t n = s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u || (n & 0x3FFull) == 0ull)
                    std::fprintf(stderr,
                                 "[G107:adcskip] n=%llu prim=%u tbp=0x%x vtxCount=%u\n",
                                 static_cast<unsigned long long>(n), primType, tbp0, vtxCount);
            }
            return true;
        }
        return false;
    }

    // [G123] ordered interleave: accumulate this tri-class kick into the current draw-run keyed by
    // the bound TEX0; a tbp change flushes the run. Pairs with the TEX0 BIND events.
    G602_COLD void g602ColdKickG123(uint32_t primType, bool tme, uint32_t tbp0, bool drawing)
    {
        if (!g123SeqEnabled())
            return;
        const uint32_t tt123 = primType & 7u;
        const bool triClass123 = (tt123 == GS_PRIM_TRIANGLE || tt123 == GS_PRIM_TRISTRIP ||
                                  tt123 == GS_PRIM_TRIFAN);
        if (triClass123 && tme && g_dc2TitleRockScope.load(std::memory_order_relaxed))
        {
            if (g105TitleRockTbp(tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                if (tbp0 != s_g123RunTbp || tt123 != s_g123RunPrim)
                    g123FlushRun();
                s_g123RunTbp = tbp0;
                s_g123RunPrim = tt123;
                if (drawing)
                    ++s_g123RunDraw;
                else
                    ++s_g123RunNodraw;
            }
        }
    }

    // G65/G66/G14: per-prim-type and per-tbp draw/nodraw tallies plus the sampled coordinate traces.
    // This is the block that owns most of vertexKick's nine nonvolatile XMM saves — nine floats per
    // fprintf, three times over.
    G602_COLD void g602ColdKickTrace(uint32_t primType, bool tme, uint32_t tbp0, bool drawing,
                                     uint32_t vtxCount, const GSVertex *queue, int maxVerts)
    {
        if (!g14TraceEnabled())
            return;
        {
            static std::atomic<uint64_t> s_all[8]{}, s_nodraw[8]{};
            const uint32_t tt = primType & 7u;
            s_all[tt].fetch_add(1u, std::memory_order_relaxed);
            if (!drawing) s_nodraw[tt].fetch_add(1u, std::memory_order_relaxed);

            {
                static std::mutex s_g66Mtx;
                struct TbpRec { uint32_t tbp; uint32_t tme; uint64_t draw; uint64_t nodraw; };
                static TbpRec s_g66[64]{};
                static uint32_t s_g66N = 0;
                const bool triClass = (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP || tt == GS_PRIM_TRIFAN);
                if (triClass)
                {
                    const uint32_t tbp = tme ? tbp0 : 0xFFFFFFFFu;
                    std::lock_guard<std::mutex> lk(s_g66Mtx);
                    uint32_t idx = 0xFFFFFFFFu;
                    for (uint32_t i = 0; i < s_g66N; ++i)
                        if (s_g66[i].tbp == tbp) { idx = i; break; }
                    if (idx == 0xFFFFFFFFu && s_g66N < 64u) { idx = s_g66N++; s_g66[idx].tbp = tbp; s_g66[idx].tme = tme ? 1u : 0u; }
                    if (idx != 0xFFFFFFFFu) { if (drawing) s_g66[idx].draw++; else s_g66[idx].nodraw++; }
                    static uint64_t s_g66Tick = 0;
                    if ((++s_g66Tick % 20000ull) == 0ull)
                    {
                        for (uint32_t i = 0; i < s_g66N; ++i)
                            std::fprintf(stderr, "[G66:tbp] tbp=0x%x tme=%u draw=%llu nodraw=%llu\n",
                                s_g66[i].tbp, s_g66[i].tme,
                                (unsigned long long)s_g66[i].draw, (unsigned long long)s_g66[i].nodraw);
                    }
                }
            }
            static std::atomic<uint64_t> s_n{0};
            const uint64_t vkn = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (vkn >= 80000u && vkn < 80360u &&
                (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP) && vtxCount >= 3u)
            {
                const GSVertex& a = queue[(vtxCount - 3) % maxVerts];
                const GSVertex& b = queue[(vtxCount - 2) % maxVerts];
                const GSVertex& c = queue[(vtxCount - 1) % maxVerts];
                std::fprintf(stderr, "[G65:late] t=%u draw=%u tme=%u tbp=0x%x v0=(%.0f,%.0f,%.0f) v1=(%.0f,%.0f,%.0f) v2=(%.0f,%.0f,%.0f)\n",
                    tt, (unsigned)(drawing?1u:0u), (unsigned)(tme?1u:0u), tbp0,
                    a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z);
            }
            if ((vkn % 20000u) == 0u)
                std::fprintf(stderr, "[G65:vk] ALL t0..7= %llu %llu %llu %llu %llu %llu %llu %llu | NODRAW tri/tstrip/tfan= %llu %llu %llu\n",
                    (unsigned long long)s_all[0].load(),(unsigned long long)s_all[1].load(),(unsigned long long)s_all[2].load(),
                    (unsigned long long)s_all[3].load(),(unsigned long long)s_all[4].load(),(unsigned long long)s_all[5].load(),
                    (unsigned long long)s_all[6].load(),(unsigned long long)s_all[7].load(),
                    (unsigned long long)s_nodraw[3].load(),(unsigned long long)s_nodraw[4].load(),(unsigned long long)s_nodraw[5].load());
        }

        if (drawing)
        {
            static std::atomic<uint32_t> s_pt[8]{};
            const uint32_t t = primType & 7u;
            const uint32_t c = s_pt[t].fetch_add(1u, std::memory_order_relaxed);
            const bool tri = (t == GS_PRIM_TRIANGLE || t == GS_PRIM_TRISTRIP || t == GS_PRIM_TRIFAN);
            if (tri && c < 40u)
            {
                std::fprintf(stderr, "[G14:tri] type=%u n=%u tme=%u v0=(%.1f,%.1f,%.1f) v1=(%.1f,%.1f,%.1f) v2=(%.1f,%.1f,%.1f)\n",
                             t, c, (uint32_t)(tme ? 1u : 0u),
                             queue[0].x, queue[0].y, queue[0].z,
                             queue[1].x, queue[1].y, queue[1].z,
                             queue[2].x, queue[2].y, queue[2].z);
                std::fflush(stderr);
            }
            if (((c + 1u) % 5000u) == 0u || (tri && c == 0u))
            {
                std::fprintf(stderr, "[G14:primsum] point=%u line=%u lstrip=%u tri=%u tstrip=%u tfan=%u sprite=%u\n",
                             s_pt[GS_PRIM_POINT].load(), s_pt[GS_PRIM_LINE].load(), s_pt[GS_PRIM_LINESTRIP].load(),
                             s_pt[GS_PRIM_TRIANGLE].load(), s_pt[GS_PRIM_TRISTRIP].load(), s_pt[GS_PRIM_TRIFAN].load(),
                             s_pt[GS_PRIM_SPRITE].load());
                std::fflush(stderr);
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // §3  writeRegisterPacked's cold blocks
    // ---------------------------------------------------------------------------------------

    // G105 synthetic-ADC scissor guard for the title rock (packed XYZF2 / XYZ2). Returns true when
    // the vertex must be treated as ADC; `outRestart` mirrors DC2_G105_KEEP_QUEUE.
    // This block owns the float scissor arithmetic in writeRegisterPacked.
    G602_COLD bool g602ColdPackedSynthAdc(uint16_t x, uint16_t y, const GSContext &ctx,
                                          uint32_t primType, bool tme, bool &outRestart)
    {
        if (!g105SynthAdcEnabled() ||
            !g_dc2TitleRockScope.load(std::memory_order_relaxed) ||
            primType != GS_PRIM_TRISTRIP ||
            !tme ||
            ctx.frame.fbp == 0x139u ||
            !g105TitleRockTbp(ctx.tex0.tbp0))
            return false;

        const float guard = g105AdcGuard();
        const float sx = static_cast<float>(x) / 16.0f -
                         static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float sy = static_cast<float>(y) / 16.0f -
                         static_cast<float>(ctx.xyoffset.ofy >> 4);
        const bool outside =
            sx < static_cast<float>(ctx.scissor.x0) - guard ||
            sx > static_cast<float>(ctx.scissor.x1) + guard ||
            sy < static_cast<float>(ctx.scissor.y0) - guard ||
            sy > static_cast<float>(ctx.scissor.y1) + guard;
        if (!outside)
            return false;

        outRestart = g105ResetStripEnabled();
        static const bool s_stat = envFlagEnabled("DC2_G105_ADC_STAT");
        if (s_stat)
        {
            static std::atomic<uint64_t> s_forced{0}, s_seen{0};
            const uint64_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint64_t fcnt = s_forced.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((n & 0x1FFFull) == 0ull)
                std::fprintf(stderr,
                             "[G105:adc] seen=%llu forced=%llu guard=%.0f tbp=0x%x xy=(%.1f,%.1f)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(fcnt),
                             guard, ctx.tex0.tbp0, sx, sy);
        }
        return true;
    }

    // [G52:rgba] sequence + [F31:packed-rgbaq] histogram (packed descriptor 0x01).
    G602_COLD void g602ColdPackedRgbaqTrace(uint32_t primType, uint32_t fbp,
                                            uint8_t curR, uint8_t curG, uint8_t curB, uint8_t curA,
                                            uint64_t lo, uint64_t hi)
    {
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && primType == GS_PRIM_TRISTRIP && fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52c{0};
                const uint32_t n = s_g52c.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr, "[G52:rgba] n=%u col=(%u,%u,%u,%u) lo=0x%016llx hi=0x%016llx\n",
                                 n, (unsigned)curR, (unsigned)curG, (unsigned)curB, (unsigned)curA,
                                 static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
            }
        }
        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31PackedRgbaCount{0};
            const uint32_t n = s_f31PackedRgbaCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool rgbNz = (curR | curG | curB) != 0u;
            if (n <= 32u || rgbNz || (n % 1024u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:packed-rgbaq] n=%u rgba=%02x,%02x,%02x,%02x lo=0x%016llx hi=0x%016llx\n",
                             n, curR, curG, curB, curA,
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
    }

    // [G52:seq] + [G31:xyzf2] traces (packed descriptor 0x04).
    G602_COLD void g602ColdPackedXyzTrace(uint32_t primType, const GSContext &ctx,
                                          uint8_t curR, uint8_t curG, uint8_t curB, uint8_t curA,
                                          uint32_t vtxCount, bool adk,
                                          uint16_t x, uint16_t y, uint32_t z, uint8_t f,
                                          uint64_t lo, uint64_t hi, int maxVerts)
    {
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && primType == GS_PRIM_TRISTRIP && ctx.frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52{0};
                const uint32_t n = s_g52.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr,
                                 "[G52:seq] n=%u adc=%u xy=(%.1f,%.1f) col=(%u,%u,%u,%u) tbp=0x%x hi=0x%016llx\n",
                                 n, (unsigned)(adk ? 1u : 0u), x / 16.0f, y / 16.0f,
                                 (unsigned)curR, (unsigned)curG, (unsigned)curB, (unsigned)curA,
                                 ctx.tex0.tbp0,
                                 static_cast<unsigned long long>(hi));
            }
        }
        if (g31TraceEnabled() &&
            (primType == GS_PRIM_TRIANGLE ||
             primType == GS_PRIM_TRISTRIP ||
             primType == GS_PRIM_TRIFAN))
        {
            static std::atomic<uint32_t> s_g31PackedXyzf2Count{0};
            const uint32_t n = s_g31PackedXyzf2Count.fetch_add(1u, std::memory_order_relaxed);
            if (n < 160u)
            {
                std::fprintf(stderr,
                             "[G31:xyzf2] n=%u prim=%u slot=%u countBefore=%u adc=%u x=%u(%.1f) y=%u(%.1f) z=0x%x f=%u lo=0x%016llx hi=0x%016llx\n",
                             n,
                             primType,
                             static_cast<uint32_t>(vtxCount % maxVerts),
                             vtxCount,
                             static_cast<uint32_t>(adk ? 1u : 0u),
                             static_cast<uint32_t>(x), x / 16.0f,
                             static_cast<uint32_t>(y), y / 16.0f,
                             z,
                             static_cast<uint32_t>(f),
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
    }

    // [F33:ad] A+D register trace (packed descriptor 0x0E).
    G602_COLD void g602ColdPackedAdTrace(uint8_t addr, uint64_t lo)
    {
        if (f33AdTraceEnabled() && f33TraceAdRegister(addr))
        {
            static std::atomic<uint32_t> s_f33AdCount{0};
            const uint32_t n = s_f33AdCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 1024u)
                f33TraceAdWrite(n, addr, lo);
        }
    }

    // ---------------------------------------------------------------------------------------
    // §4  writeRegister's cold blocks
    // ---------------------------------------------------------------------------------------

    // PHASE F30/F31: the whole `phaseDiagnosticsEnabled()` preamble — three separate tests of the
    // same cross-TU-shaped accessor on every one of ~55.8k calls per frame.
    G602_COLD void g602ColdRegPreamble(uint8_t regAddr, uint64_t value,
                                       uint8_t curR, uint8_t curG, uint8_t curB,
                                       bool prmodecont)
    {
        if (!phaseDiagnosticsEnabled())
            return;
        {
            static std::atomic<uint32_t> s_f30GsReg{0};
            static std::atomic<uint32_t> s_f30Rgbaq{0};
            static std::atomic<uint32_t> s_f30RgbaqNz{0};
            const uint32_t n = s_f30GsReg.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (regAddr == 0x01u)
            {
                const uint32_t k = s_f30Rgbaq.fetch_add(1, std::memory_order_relaxed) + 1u;
                const uint32_t lo = static_cast<uint32_t>(value & 0xFFFFFFFFu);
                const bool rgbNz = (lo & 0x00FFFFFFu) != 0u;
                if (rgbNz)
                    s_f30RgbaqNz.fetch_add(1, std::memory_order_relaxed);
                if (k <= 8u || (k & 0x3FFu) == 0u || rgbNz)
                {
                    std::fprintf(stderr,
                                 "[F30:rgbaq] k=%u value=0x%016llx rgbNz=%u nzTotal=%u\n",
                                 k, static_cast<unsigned long long>(value),
                                 rgbNz ? 1u : 0u, s_f30RgbaqNz.load());
                }
            }
            if (regAddr >= 0x50u && regAddr <= 0x53u)
            {
                static std::atomic<uint32_t> s_f31UploadRegCount{0};
                const uint32_t k = s_f31UploadRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (k <= 64u || (k % 256u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[F31:upload-reg] k=%u reg=0x%02x value=0x%016llx\n",
                                 k, regAddr, static_cast<unsigned long long>(value));
                }
            }
            if (n <= 12u || (n % 256u) == 0u)
            {
                std::fprintf(stderr,
                             "[F30:gsreg] n=%u reg=0x%02x lo=0x%08x\n",
                             n, regAddr, static_cast<uint32_t>(value & 0xFFFFFFFFu));
            }
        }
        (void)curR; (void)curG; (void)curB;
        if (regAddr == GS_REG_PRIM)
        {
            static std::atomic<uint32_t> s_f31PrimRegCount{0};
            const uint32_t n = s_f31PrimRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u || (n % 4096u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:prim-reg] n=%u value=0x%llx prmodecont=%u type=%u iip=%u tme=%u abe=%u fst=%u ctxt=%u\n",
                             n, static_cast<unsigned long long>(value),
                             prmodecont ? 1u : 0u,
                             static_cast<uint32_t>(value & 0x7u),
                             static_cast<uint32_t>((value >> 3u) & 1u),
                             static_cast<uint32_t>((value >> 4u) & 1u),
                             static_cast<uint32_t>((value >> 6u) & 1u),
                             static_cast<uint32_t>((value >> 8u) & 1u),
                             static_cast<uint32_t>((value >> 9u) & 1u));
            }
        }
        else if (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2)
        {
            static std::atomic<uint32_t> s_f31Tex0RegCount{0};
            const uint32_t n = s_f31Tex0RegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u || (n % 1024u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:tex0-reg] n=%u reg=0x%02x value=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u\n",
                             n, regAddr, static_cast<unsigned long long>(value),
                             static_cast<uint32_t>(value & 0x3FFFu),
                             static_cast<uint32_t>((value >> 14u) & 0x3Fu),
                             static_cast<uint32_t>((value >> 20u) & 0x3Fu),
                             static_cast<uint32_t>((value >> 26u) & 0xFu),
                             static_cast<uint32_t>((value >> 30u) & 0xFu),
                             static_cast<uint32_t>((value >> 34u) & 0x1u),
                             static_cast<uint32_t>((value >> 35u) & 0x3u),
                             static_cast<uint32_t>((value >> 37u) & 0x3FFFu),
                             static_cast<uint32_t>((value >> 51u) & 0xFu),
                             static_cast<uint32_t>((value >> 55u) & 0x1u),
                             static_cast<uint32_t>((value >> 56u) & 0x1Fu),
                             static_cast<uint32_t>((value >> 61u) & 0x7u));
            }
        }
    }

    // [G91:tex0] / [G123:seq] BIND / [G34:tex0bind2720] / [G35:seq] / [F509:tex0bind]
    G602_COLD void g602ColdRegTex0(uint8_t regAddr, int ci, const GSTex0Reg &t)
    {
        {
                static const bool s_g91tex0 = (std::getenv("DC2_G91_TEX0") != nullptr);
            if (s_g91tex0 && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                t.tbp0 >= 0x2720u && t.tbp0 <= 0x3960u)
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (k <= 4000u)
                    std::fprintf(stderr, "[G91:tex0] k=%u ci=%d tbp=0x%x psm=0x%x cbp=0x%x tw=%u th=%u\n",
                        k, ci, t.tbp0, (unsigned)t.psm, t.cbp, (unsigned)t.tw, (unsigned)t.th);
            }
        }
        {
                if (g123SeqEnabled() && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                g105TitleRockTbp(t.tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                g123FlushRun();
                if (s_g123Events < g123Cap())
                {
                    const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G123:seq] %u BIND tbp=0x%x psm=0x%x ci=%d\n",
                                 seq, t.tbp0, (unsigned)t.psm, ci);
                    ++s_g123Events;
                }
            }
        }
        if (renderQualityTraceEnabled() && t.tbp0 == 0x2720u)
        {
            static std::atomic<uint32_t> s_g34ct32{0};
            static std::atomic<uint32_t> s_g34t8{0};
            static std::atomic<uint32_t> s_g34oth{0};
            std::atomic<uint32_t> *c = (t.psm == 0u) ? &s_g34ct32
                                       : ((t.psm == GS_PSM_T8) ? &s_g34t8 : &s_g34oth);
            const uint32_t bn = c->fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((t.psm == 0u && bn <= 32u) || (bn % 8192u) == 0u)
                std::fprintf(stderr,
                    "[G34:tex0bind2720] psm=0x%x tbw=%u tw=%u th=%u bn=%u (ct32=%u t8=%u oth=%u)\n",
                    (unsigned)t.psm, (unsigned)t.tbw, (unsigned)t.tw, (unsigned)t.th, bn,
                    s_g34ct32.load(std::memory_order_relaxed),
                    s_g34t8.load(std::memory_order_relaxed),
                    s_g34oth.load(std::memory_order_relaxed));
        }
        if (renderQualityTraceEnabled() && g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            const int seqCi = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
            if (t.tbp0 == 0x2720u && t.psm == 0u)
            {
                g_g34SeqWindow.store(70, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x2720 psm=0x0 tbw=%u  (CT32 ARM)\n",
                             seqCi, (unsigned)t.tbw);
            }
            else if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0)
            {
                g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x%x psm=0x%x tbw=%u%s\n",
                             seqCi, t.tbp0, (unsigned)t.psm, (unsigned)t.tbw,
                             (t.tbp0 == 0x2720u && t.psm != 0u) ? "   <-- T8 0x2720 CLOBBER" : "");
            }
        }
        if (renderQualityTraceEnabled() &&
            (t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH))
        {
            if (t.tbp0 != 0x1a00u)
            {
                static std::atomic<uint32_t> s_f509Tex0Bind{0};
                const uint32_t n = s_f509Tex0Bind.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 300u)
                    std::fprintf(stderr, "[F509:tex0bind] n=%u ci=%d tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u\n",
                                 n, ci, t.tbp0, (unsigned)t.tbw, (unsigned)t.psm,
                                 t.cbp, (unsigned)t.cpsm, (unsigned)t.cld);
            }
        }
    }

    // [F49:frame]
    G602_COLD void g602ColdRegFrame(uint8_t regAddr, int ci, uint64_t value,
                                    const GSFrameReg &prev, const GSFrameReg &now,
                                    bool primCtxt, const GSBitBltBuf &bb)
    {
        if (!frameRegisterTraceEnabled())
            return;
        static std::atomic<uint32_t> s_f49FrameRegCount{0};
        const uint32_t n = s_f49FrameRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool zeroed = (now.fbp == 0u || now.fbw == 0u);
        const bool changed = prev.fbp != now.fbp ||
                             prev.fbw != now.fbw ||
                             prev.psm != now.psm ||
                             prev.fbmsk != now.fbmsk;
        if ((changed && (n <= 256u || (n % 512u) == 0u)) || zeroed)
        {
            std::fprintf(stderr,
                         "[F49:frame] n=%u reg=0x%02x ctx=%d raw=0x%016llx prev(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) new(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) zero=%u primCtxt=%u bitblt(dbp=0x%x dbw=%u dpsm=0x%x)\n",
                         n, regAddr, ci, static_cast<unsigned long long>(value),
                         prev.fbp, prev.fbw, static_cast<uint32_t>(prev.psm), prev.fbmsk,
                         now.fbp, now.fbw, static_cast<uint32_t>(now.psm), now.fbmsk,
                         zeroed ? 1u : 0u,
                         primCtxt ? 1u : 0u,
                         bb.dbp, static_cast<uint32_t>(bb.dbw), static_cast<uint32_t>(bb.dpsm));
        }
    }

    // [F49:bitblt] / [F37:bitblt] / [F43:t8] / [F509:hit*]
    G602_COLD void g602ColdRegBitblt(uint64_t value, const GSBitBltBuf &bb,
                                     uint32_t prevDbp, uint8_t prevDbw, uint8_t prevDpsm,
                                     bool primCtxt, const GSFrameReg &f0, const GSFrameReg &f1)
    {
        if (frameRegisterTraceEnabled())
        {
            static std::atomic<uint32_t> s_f49BitbltRegCount{0};
            const uint32_t n = s_f49BitbltRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool changed = prevDbp != bb.dbp || prevDbw != bb.dbw || prevDpsm != bb.dpsm;
            if (changed && (n <= 128u || (n % 256u) == 0u))
            {
                std::fprintf(stderr,
                             "[F49:bitblt] n=%u raw=0x%016llx dst=0x%x/%u/0x%x src=0x%x/%u/0x%x primCtxt=%u frame0=0x%x/%u frame1=0x%x/%u\n",
                             n, static_cast<unsigned long long>(value),
                             bb.dbp, static_cast<uint32_t>(bb.dbw), static_cast<uint32_t>(bb.dpsm),
                             bb.sbp, static_cast<uint32_t>(bb.sbw), static_cast<uint32_t>(bb.spsm),
                             primCtxt ? 1u : 0u,
                             f0.fbp, f0.fbw, f1.fbp, f1.fbw);
            }
        }
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37BitbltCount{0};
            const uint32_t n = s_f37BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:bitblt] n=%u raw=0x%016llx sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n, static_cast<unsigned long long>(value),
                             bb.sbp, static_cast<uint32_t>(bb.sbw), static_cast<uint32_t>(bb.spsm),
                             bb.dbp, static_cast<uint32_t>(bb.dbw), static_cast<uint32_t>(bb.dpsm));
        }
        if (t8UploadTraceEnabled())
        {
            static std::atomic<uint32_t> s_f43BitbltCount{0};
            const uint32_t n = s_f43BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 128u)
                std::fprintf(stderr, "[F43:t8] n=%u sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n,
                             bb.sbp, static_cast<uint32_t>(bb.sbw), static_cast<uint32_t>(bb.spsm),
                             bb.dbp, static_cast<uint32_t>(bb.dbw), static_cast<uint32_t>(bb.dpsm));
        }
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f509Tex2580{0};
            static std::atomic<uint32_t> s_f509Clut2980{0};
            static std::atomic<uint32_t> s_f509Total{0};
            const uint32_t tot = s_f509Total.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (bb.dbp == 0x2580u)
            {
                const uint32_t h = s_f509Tex2580.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2580] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, bb.dbp, (unsigned)bb.dbw, (unsigned)bb.dpsm);
            }
            if (bb.dbp == 0x2980u)
            {
                const uint32_t h = s_f509Clut2980.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2980] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, bb.dbp, (unsigned)bb.dbw, (unsigned)bb.dpsm);
            }
            if ((tot % 256u) == 0u)
                std::fprintf(stderr, "[F509:bbtally] total=%u hit2580=%u hit2980=%u\n",
                             tot, s_f509Tex2580.load(), s_f509Clut2980.load());
        }
    }

    // [G171:regstat] per-descriptor timing census + its [G171:slowad] follow-up. Default-off, and
    // outlined so the census body is not TEXT inside processGIFPacket (17d §3.1 — dead code in the
    // enclosing function of a hot loop is not free).
    G602_COLD void g602ColdG171Note(uint8_t desc, uint64_t hi, long long ns,
                                    uint32_t primType, bool tme, bool abe,
                                    const GSFrameReg &frame, const GSScissorReg &scissor,
                                    const GSVertex &vA, const GSVertex &vB)
    {
        g_g171RegNs[desc].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
        g_g171RegCount[desc].fetch_add(1u, std::memory_order_relaxed);
        if (desc != 0x0Eu)
            return;
        const uint8_t adAddr = static_cast<uint8_t>(hi & 0xFFu);
        g_g171AdNs[adAddr].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
        g_g171AdCount[adAddr].fetch_add(1u, std::memory_order_relaxed);
        // Identify WHAT is drawn on the slow A+D XYZ2/XYZF2 vertex-kick path. Rate-limited.
        if ((adAddr == 0x04u || adAddr == 0x05u) && ns > 5000)
        {
            static std::atomic<uint32_t> s_g171SlowCount{0};
            const uint32_t sn = s_g171SlowCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (sn <= 40u || (sn % 200u) == 0u)
            {
                std::fprintf(stderr,
                             "[G171:slowad] sn=%u addr=0x%02x ns=%lld prim=%u tme=%u abe=%u fbp=0x%x fbw=%u psm=0x%x scissor=(%d,%d)-(%d,%d) vtx=(%.1f,%.1f)-(%.1f,%.1f)\n",
                             sn, adAddr, ns,
                             primType, (unsigned)tme, (unsigned)abe,
                             frame.fbp, frame.fbw, static_cast<uint32_t>(frame.psm),
                             scissor.x0, scissor.y0, scissor.x1, scissor.y1,
                             vA.x, vA.y, vB.x, vB.y);
            }
        }
    }

    // [G35:seq] PRIM
    G602_COLD void g602ColdRegPrimTrace(uint32_t primType, bool ctxt, bool tme, bool abe)
    {
        if (!renderQualityTraceEnabled() ||
            g_g34SeqWindow.load(std::memory_order_relaxed) <= 0 ||
            g_g34SeqCap.load(std::memory_order_relaxed) >= 1600u)
            return;
        g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
        g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[G35:seq] PRIM type=%u ctxt=%u tme=%u abe=%u\n",
                     primType, (unsigned)ctxt, (unsigned)tme, (unsigned)abe);
    }

    // [G34:vtx2720ct32] + [G35:seq] XYZF2/XYZ2 — the last float-printing blocks inside
    // GS::writeRegister, and therefore the reason it still saved xmm6/xmm7 after the first pass.
    G602_COLD void g602ColdRegVertexTrace(bool isXyzf, const GSVertex &vtx, const GSTex0Reg &tex0,
                                          bool primCtxt, bool tme, uint32_t primType,
                                          uint32_t vtxCount, uint16_t curU, uint16_t curV)
    {
        if (!renderQualityTraceEnabled())
            return;
        if (!isXyzf && tme && tex0.tbp0 == 0x2720u && tex0.psm == 0u)
        {
            static std::atomic<uint32_t> s_g34v{0};
            const uint32_t vn = s_g34v.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (vn <= 64u)
                std::fprintf(stderr, "[G34:vtx2720ct32] vn=%u xyz2 vtxCount=%u xy=(%.1f,%.1f) prim=%u ctxt=%u uv=(%u,%u)\n",
                             vn, vtxCount, vtx.x, vtx.y,
                             primType, (unsigned)primCtxt,
                             (unsigned)curU, (unsigned)curV);
        }
        if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
            g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
            g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G35:seq] %s xy=(%.1f,%.1f) curTex0(tbp=0x%x psm=0x%x) ctxt=%u%s\n",
                         isXyzf ? "XYZF2" : "XYZ2",
                         vtx.x, vtx.y, tex0.tbp0, (unsigned)tex0.psm, (unsigned)primCtxt,
                         (tex0.tbp0 == 0x2720u) ? ((tex0.psm == 0u) ? "  <-- verts under CT32 (GOOD)" : "  <-- verts under T8 (BUG)") : "");
        }
    }

    // [F37:trxreg]
    G602_COLD void g602ColdRegTrxreg(const GSTrxReg &tr)
    {
        if (!renderQualityTraceEnabled())
            return;
        static std::atomic<uint32_t> s_f37TrxregCount{0};
        const uint32_t n = s_f37TrxregCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
            std::fprintf(stderr, "[F37:trxreg] n=%u rrw=%u rrh=%u\n", n, tr.rrw, tr.rrh);
    }

    // [F37:trxdir]
    G602_COLD void g602ColdRegTrxdir(uint32_t trxdir, const GSBitBltBuf &bb, const GSTrxReg &tr)
    {
        if (!renderQualityTraceEnabled())
            return;
        static std::atomic<uint32_t> s_f37TrxdirCount{0};
        const uint32_t n = s_f37TrxdirCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
            std::fprintf(stderr, "[F37:trxdir] n=%u dir=%u dbp=0x%x dpsm=0x%x rr=(%u,%u)\n",
                         n, trxdir, bb.dbp,
                         static_cast<uint32_t>(bb.dpsm),
                         tr.rrw, tr.rrh);
    }

    // G397: 0x59..0x5f are reserved in the GIF A+D register namespace. Privileged PCRTC registers
    // are MMIO-only and must never be writable through an A+D packet.
    G602_COLD void g602ColdRegReserved(uint8_t regAddr, uint64_t value, GSRegisters *privRegs)
    {
        static const bool s_g397LegacyAliases =
            (std::getenv("DC2_G397_LEGACY_RESERVED_DISPLAY") != nullptr);
        static const bool s_g397Trace =
            (std::getenv("DC2_G397_PRESENT_TRACE") != nullptr);
        static std::atomic<uint32_t> s_g397ReservedWrites{0u};

        if (s_g397LegacyAliases && privRegs)
        {
            switch (regAddr)
            {
            case 0x59: privRegs->dispfb1 = value; break;
            case 0x5a: privRegs->display1 = value; break;
            case 0x5b: privRegs->dispfb2 = value; break;
            case 0x5c: privRegs->display2 = value; break;
            case 0x5f: privRegs->bgcolor = value; break;
            default: break;
            }
        }

        const uint32_t n = s_g397ReservedWrites.fetch_add(1u, std::memory_order_relaxed);
        if (s_g397Trace && n < 64u)
        {
            std::fprintf(stderr,
                         "[G397:reserved-gs] n=%u reg=0x%x value=0x%llx action=%s\n",
                         n + 1u,
                         static_cast<unsigned>(regAddr),
                         static_cast<unsigned long long>(value),
                         s_g397LegacyAliases ? "legacy-alias" : "ignored");
        }
    }
}
