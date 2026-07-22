// ===== G161 (MEASURE ONLY, DEFAULT OFF, DC2_G161_STAT=1) — census the G158 plan's eligibility =====
// gate against REAL G144-deferred title geometry, before writing any GPU shader/commit code. The
// signed-off G158 plan (~/.claude/plans/snoopy-roaming-blanket.md) scoped GPU raster to a narrow
// predicate specifically to dodge the two hardest risk classes (CLUT/swizzle decode in a shader,
// and PS2-fixed-point-Z vs GPU-depth-float correctness): textured tri, abe==0 (no blend), fbp
// already known 0x0/0x68 at this call site (G144's deferrable144 already enforced that), PSM in
// {CT32,CT24,CT16,CT16S} (no CLUT4/T8/T4 -- palette lookup deliberately excluded), wrap in
// {REPEAT,CLAMP}, texW*texH <= cap, alpha-test disabled (ctx.test bit0 = ATE). This function does
// NOT touch VRAM, GL, or any new thread -- it only counts, at the exact site G144 already captures
// deferred entries, whether each one *would* pass every clause of that gate. Answers the decisive
// open question before any GPU-thread/shader engineering: is there any eligible geometry at all in
// the title workload (suspected near-zero -- prior phases record the cavern as PSMT8, which this
// gate deliberately excludes)? Same measure-before-build sequencing as G148 (measured the sampler
// before G149 built -- and later refuted -- the CPU decode cache).
namespace
{
std::atomic<uint64_t> g_g161Total{0};
std::atomic<uint64_t> g_g161Eligible{0};
std::atomic<uint64_t> g_g161BadPsm{0};
std::atomic<uint64_t> g_g161BadWrap{0};
std::atomic<uint64_t> g_g161Blend{0};
std::atomic<uint64_t> g_g161Atest{0};
std::atomic<uint64_t> g_g161TooBig{0};

void g161Report()
{
    const uint64_t tot = g_g161Total.load(std::memory_order_relaxed);
    const uint64_t elig = g_g161Eligible.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G161:elig] eligible=%llu total=%llu (%.1f%%) badPsm=%llu badWrap=%llu blend=%llu atest=%llu tooBig=%llu\n",
                 (unsigned long long)elig, (unsigned long long)tot,
                 tot ? 100.0 * static_cast<double>(elig) / static_cast<double>(tot) : 0.0,
                 (unsigned long long)g_g161BadPsm.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161BadWrap.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161Blend.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161Atest.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161TooBig.load(std::memory_order_relaxed));
    std::fflush(stderr);
}
} // namespace

static void g161CensusEligibility(const GSContext &ctx, bool abe)
{
    static const bool s_on = (std::getenv("DC2_G161_STAT") != nullptr);
    if (!s_on)
        return;
    static const size_t s_cap = []() {
        const char *e = std::getenv("DC2_G158_CAP");
        const size_t c = e ? static_cast<size_t>(std::strtoull(e, nullptr, 0)) : 262144u;
        return c ? c : 262144u;
    }();

    const uint64_t tot = g_g161Total.fetch_add(1u, std::memory_order_relaxed) + 1u;
    bool ok = true;
    if (abe) { g_g161Blend.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    if ((ctx.test & 0x1u) != 0u) { g_g161Atest.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    const uint8_t psm = ctx.tex0.psm;
    const bool goodPsm = psm == GS_PSM_CT32 || psm == GS_PSM_CT24 ||
                         psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    if (!goodPsm) { g_g161BadPsm.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    if (!((wrapU == 0u || wrapU == 1u) && (wrapV == 0u || wrapV == 1u)))
    {
        g_g161BadWrap.fetch_add(1u, std::memory_order_relaxed);
        ok = false;
    }
    const size_t texels = (static_cast<size_t>(1) << ctx.tex0.tw) * (static_cast<size_t>(1) << ctx.tex0.th);
    if (texels == 0 || texels > s_cap) { g_g161TooBig.fetch_add(1u, std::memory_order_relaxed); ok = false; }

    if (ok)
        g_g161Eligible.fetch_add(1u, std::memory_order_relaxed);
    if ((tot % 8000u) == 0u)
        g161Report();
}

// [G232] gem-draw pixel accounting (DC2_G232_RTTDUMP=1, default-off). drawPrimitive arms
// this for a gem-red 0x34e0 draw; the immediately following drawTriangle consumes it and
// tallies where the pixels die (bbox -> inside -> ztest -> writePixel). Serial-path only.
static std::atomic<int> g_g232TriArm{0};
static std::atomic<uint32_t> g_g232PxBbox{0}, g_g232PxInside{0}, g_g232PxZfail{0};
static std::atomic<uint32_t> g_g232PxWrite{0}, g_g232PxRedWrite{0}, g_g232PxZlog{0};
// [G232] watched pixel = latest gem-draw centroid; drawTriangle logs EVERY fbp=0x139 draw
// touching it (tbp, zi vs zdst, pass) -> full per-frame Z history at the gem's screen pixel.
static std::atomic<int> g_g232GemX{-1}, g_g232GemY{-1};

// ===== G312 segment census (default-off, DC2_G312_STAT=1) =====
// Splits the drawPrimitive entry->capture window into segments to localize per-draw cost:
//   seg0 = entry stamp -> end of the G45/G49 costume-Z block
//   seg1 = seg0 end -> end of the F31..G34 diagnostic pile
//   seg2 = seg1 end -> capture start, sub-split 2a (DC2_PERF G141 counter block) /
//          2b (tilebin eligibility) / 2c (pre-flush checks)
// G312 finding (2026-07-21): with all diagnostic envs off the whole window is ~119 ns/draw
// (seg0=28, pile=17, seg2=74). Under DC2_PERF=1 the G141 counter block alone inflates to
// ~855 ns/draw (~12.6 ms/f of GS-worker time at 14.7k draws/f) — the entire suspected
// "register-replay/draw-capture pole term" was profiler self-cost, not game work. Keep this
// census for any future prologue attribution; NEVER bucket-hunt inside a DC2_PERF run.
// Report: [G312:seg] cumulative ms + ns/draw every 8000 draws.
static const bool g_g312SegStat = (std::getenv("DC2_G312_STAT") != nullptr);
static std::atomic<uint64_t> g_g312Seg0Ns{0}, g_g312Seg1Ns{0}, g_g312Seg2Ns{0}, g_g312SegN{0};
static std::atomic<uint64_t> g_g312Seg2aNs{0}, g_g312Seg2bNs{0};
static thread_local std::chrono::steady_clock::time_point t_g312Seg2Start{};
static thread_local std::chrono::steady_clock::time_point t_g312Seg2aEnd{}, t_g312Seg2bEnd{};

void GSRasterizer::drawPrimitive(GS *gs)
{
    static const bool s_g141PerfOn = (std::getenv("DC2_PERF") != nullptr);
    static double s_g141Sum = 0.0;
    static uint32_t s_g141Win = 0u;
    G141PerfScope g141Scope(s_g141PerfOn, &s_g141Sum, &s_g141Win, "gs.drawPrimitive", 4000u);
    g141Scope.accumNs = &g_g141GsRasterNs;
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on && !t_g144InReplay)
        {
            extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
            static thread_local char s_g219tag[96];
            const auto &g219ctx = gs->activeContext();
            std::snprintf(s_g219tag, sizeof(s_g219tag), "drawprim(prim=%u tme=%u fbp=0x%x tbp=0x%x)",
                          static_cast<uint32_t>(gs->m_prim.type),
                          static_cast<uint32_t>(gs->m_prim.tme),
                          g219ctx.frame.fbp, g219ctx.tex0.tbp0);
            g219Sentinel(gs->m_vram, gs->m_vramSize, s_g219tag);
        }
    }
    const std::chrono::steady_clock::time_point g156FnT0 =
        g_g156Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const std::chrono::steady_clock::time_point g312T0 =
        g_g312SegStat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    const auto &ctx = gs->activeContext();
    // G45: clear the costume model's PRIVATE Z buffer exactly once per costume frame, on the
    // fbp -> 0x139 transition (the first draw of each frame's model RTT pass). The old clear was
    // gated on the full-RTT black-sprite signature, which does NOT fire every frame in the
    // interactive/zoom pose (Red Vest) -> stale private Z from the prior frame rejected current
    // model triangles via GEQUAL -> the model shredded/flickered. The model render is a
    // contiguous block of fbp=0x139 draws (the composite that follows samples 0x2720 but targets
    // a non-0x139 fbp), so a prev!=0x139 && cur==0x139 edge fires once per frame. Costs one 1MB
    // memset/frame and only matters for the costume RTT; harmless if 0x139 is reused otherwise.
    {
        static std::atomic<uint32_t> s_lastDrawFbp{0xFFFFFFFFu};
        static std::atomic<uint32_t> s_costumeFrameDisplay{0xFFFFFFFFu};
        static const bool s_noZClear = (std::getenv("DC2_NO_ZTEST") != nullptr);
        // G49 kill switch: DC2_G49_PERENTRY restores the old (G47) per-0x139-entry Z clear.
        static const bool s_perEntryZClear = (std::getenv("DC2_G49_PERENTRY") != nullptr);
        const uint32_t prevFbp = s_lastDrawFbp.exchange(ctx.frame.fbp, std::memory_order_relaxed);
        bool didClear = false;
        // G49: the costume model is NOT one contiguous 0x139 block — it is drawn in MANY 0x139
        // segments per frame, each bracketed by a draw to the current display buffer (0x68/0x0,
        // which double-buffers per rendered frame). The old G47 clear fired on EVERY 0x139 entry,
        // so it wiped the private Z between the head sub-meshes (skin -> decal -> cap). With Z
        // wiped, GEQUAL reads 0 and the farther cap/hair (tbp 0x35a0, behind the face) no longer
        // fails the depth test -> it draws DARK over the face (the G48 "streaks" residual). Fix:
        // clear the private Z only ONCE per rendered frame, detected by the display buffer we
        // interleave with CHANGING (a double-buffer flip). Z then persists across the frame's
        // 0x139 segments, so the cap/back faces correctly fail GEQUAL behind the skin. The
        // black-fill-sprite clear (below, ~line 870) remains as a once-per-frame safety net for
        // any frame whose display buffer does not flip.
        if (!s_noZClear && ctx.frame.fbp == 0x139u && prevFbp != 0x139u)
        {
            if (s_perEntryZClear)
            {
                costumeZClear();
                didClear = true;
            }
            else
            {
                const uint32_t lastDisp = s_costumeFrameDisplay.load(std::memory_order_relaxed);
                if (prevFbp != lastDisp)
                {
                    costumeZClear();
                    s_costumeFrameDisplay.store(prevFbp, std::memory_order_relaxed);
                    didClear = true;
                }
            }
        }
        // G49 diag: trace the fbp transition sequence + costume Z clears. Env-gated, quiet.
        static const bool s_g49diag = (std::getenv("DC2_G49_DIAG") != nullptr);
        if (s_g49diag && (prevFbp != ctx.frame.fbp))
        {
            static std::atomic<uint32_t> s_g49seq{0};
            const uint32_t sn = s_g49seq.fetch_add(1u, std::memory_order_relaxed);
            if (sn < 400u)
                std::fprintf(stderr, "[G49:fbp] sn=%u 0x%x->0x%x clear=%u\n",
                             sn, prevFbp, ctx.frame.fbp, (unsigned)didClear);
        }
    }
    const std::chrono::steady_clock::time_point g312T1 =
        g_g312SegStat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // (G312 note: a master skip-branch over the F31..G34 diagnostic pile below was built and
    // REVERTED — the pile costs only ~17 ns/draw with gates off; the suspected ~1.1 us/draw
    // prologue cost was DC2_PERF instrument self-cost. See [G312:seg] census + the G312 fix log.)
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f31PrimCount{0};
        static std::atomic<uint32_t> s_f31TexturedPrimCount{0};
        const uint32_t n = s_f31PrimCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t texturedN = gs->m_prim.tme
                                       ? (s_f31TexturedPrimCount.fetch_add(1u, std::memory_order_relaxed) + 1u)
                                       : 0u;
        if (n <= 64u || (texturedN != 0u && texturedN <= 128u) || (n % 1024u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:prim] n=%u type=%u tme=%u iip=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x tex=(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u) rgba0=%02x,%02x,%02x,%02x rgba1=%02x,%02x,%02x,%02x rgba2=%02x,%02x,%02x,%02x uv0=%u,%u uv1=%u,%u uv2=%u,%u\n",
                         n,
                         static_cast<uint32_t>(gs->m_prim.type),
                         static_cast<uint32_t>(gs->m_prim.tme),
                         static_cast<uint32_t>(gs->m_prim.iip),
                         static_cast<uint32_t>(gs->m_prim.abe),
                         static_cast<uint32_t>(gs->m_prim.fst),
                         static_cast<uint32_t>(gs->m_prim.ctxt),
                         ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                         static_cast<uint32_t>(ctx.frame.psm),
                         ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                         static_cast<uint32_t>(ctx.tex0.psm),
                         static_cast<uint32_t>(ctx.tex0.tw),
                         static_cast<uint32_t>(ctx.tex0.th),
                         static_cast<uint32_t>(ctx.tex0.tcc),
                         static_cast<uint32_t>(ctx.tex0.tfx),
                         ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                         static_cast<uint32_t>(ctx.tex0.csm),
                         static_cast<uint32_t>(ctx.tex0.csa),
                         gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                         gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a,
                         gs->m_vtxQueue[2].r, gs->m_vtxQueue[2].g, gs->m_vtxQueue[2].b, gs->m_vtxQueue[2].a,
                         static_cast<uint32_t>(gs->m_vtxQueue[0].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[0].v >> 4),
                         static_cast<uint32_t>(gs->m_vtxQueue[1].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[1].v >> 4),
                         static_cast<uint32_t>(gs->m_vtxQueue[2].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[2].v >> 4));
        }
    }
    if (renderQualityTraceEnabled())
    {
        static std::atomic<uint32_t> s_f44DrawCount{0};
        const uint32_t n = s_f44DrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
        {
            const uint64_t alpha = ctx.alpha;
            const uint64_t clamp = ctx.clamp;
            const uint64_t zbuf  = ctx.zbuf;
            std::fprintf(stderr,
                "[F44:draw] n=%u tme=%u abe=%u pabe=%u tfx=%u tcc=%u fbp=0x%x fbmsk=0x%x "
                "alpha(A=%u,B=%u,C=%u,D=%u,FIX=%u) "
                "clamp(wms=%u,wmt=%u) "
                "zbuf(zbp=0x%x,zmsk=%u) "
                "v0rgba=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<uint32_t>(ctx.tex0.tfx),
                static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.frame.fbp,
                ctx.frame.fbmsk,
                static_cast<uint32_t>(alpha & 0x3u),
                static_cast<uint32_t>((alpha >> 2) & 0x3u),
                static_cast<uint32_t>((alpha >> 4) & 0x3u),
                static_cast<uint32_t>((alpha >> 6) & 0x3u),
                static_cast<uint32_t>((alpha >> 32) & 0xFFu),
                static_cast<uint32_t>(clamp & 0x3u),
                static_cast<uint32_t>((clamp >> 2) & 0x3u),
                static_cast<uint32_t>(zbuf & 0x1FFu),
                static_cast<uint32_t>((zbuf >> 32) & 0x1u),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g,
                gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
        }
    }
    // [F53] F44's global cap is exhausted before the dungeon map packets arrive.
    // Capture the map texture's draw state independently so UI draws cannot hide it.
    if (renderQualityTraceEnabled() &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == 0x3220u &&
        ctx.tex0.psm == 0x13u)
    {
        static std::atomic<uint32_t> s_f53MapDrawCount{0};
        const uint32_t n = s_f53MapDrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
        {
            const uint64_t alpha = ctx.alpha;
            const uint64_t clamp = ctx.clamp;
            const uint64_t zbuf = ctx.zbuf;
            const uint64_t test = ctx.test;
            std::fprintf(stderr,
                "[F53:draw] n=%u prim=%u ctxt=%u fst=%u tme=%u abe=%u pabe=%u "
                "tfx=%u tcc=%u tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x "
                "fbp=0x%x fbw=%u fpsm=0x%x fbmsk=0x%x "
                "alpha(A=%u,B=%u,C=%u,D=%u,FIX=%u) "
                "clamp(wms=%u,wmt=%u) zbuf(zbp=0x%x,zmsk=%u) "
                "test(ate=%u,atst=%u,aref=%u,afail=%u,date=%u,datm=%u,zte=%u,ztst=%u) "
                "scissor=(%u,%u)-(%u,%u) of=(%d,%d) "
                "v0rgba=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.ctxt),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<uint32_t>(ctx.tex0.tfx),
                static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.tex0.tbp0,
                static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm),
                ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm),
                ctx.frame.fbp,
                static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                ctx.frame.fbmsk,
                static_cast<uint32_t>(alpha & 0x3u),
                static_cast<uint32_t>((alpha >> 2) & 0x3u),
                static_cast<uint32_t>((alpha >> 4) & 0x3u),
                static_cast<uint32_t>((alpha >> 6) & 0x3u),
                static_cast<uint32_t>((alpha >> 32) & 0xFFu),
                static_cast<uint32_t>(clamp & 0x3u),
                static_cast<uint32_t>((clamp >> 2) & 0x3u),
                static_cast<uint32_t>(zbuf & 0x1FFu),
                static_cast<uint32_t>((zbuf >> 32) & 0x1u),
                static_cast<uint32_t>(test & 0x1u),
                static_cast<uint32_t>((test >> 1) & 0x7u),
                static_cast<uint32_t>((test >> 4) & 0xFFu),
                static_cast<uint32_t>((test >> 12) & 0x3u),
                static_cast<uint32_t>((test >> 14) & 0x1u),
                static_cast<uint32_t>((test >> 15) & 0x1u),
                static_cast<uint32_t>((test >> 16) & 0x1u),
                static_cast<uint32_t>((test >> 17) & 0x3u),
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                static_cast<int>(ctx.xyoffset.ofx >> 4),
                static_cast<int>(ctx.xyoffset.ofy >> 4),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g,
                gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
        }
    }
    // [G37:seq] GS-order trace: RTT draws (fbp=0x139 = block 0x2720) and composites sampling
    // block 0x2720 to a display buffer. Shared counter with the upload trace in ps2_gs_gpu.cpp.
    {
        extern std::atomic<uint32_t> g_g37_seq;
        extern bool g_g37_seq_trace;
        extern std::atomic<int> g_g37_armed;
        if (g_g37_seq_trace)
        {
            const bool rttModelDraw = (ctx.frame.fbp == 0x139u && gs->m_prim.tme);
            const bool comp2720 = (gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u && ctx.frame.fbp != 0x139u);
            // Arm on the first model render INTO the RTT; thereafter log ONLY the events that can
            // clobber/consume the RTT (uploads here are logged in ps2_gs_gpu.cpp; composites here),
            // skipping the hundreds of model triangles so the upload/composite ordering is visible.
            if (rttModelDraw && g_g37_armed.load(std::memory_order_relaxed) == 0)
            {
                g_g37_armed.store(1, std::memory_order_relaxed);
                g_g37_seq.store(0u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G37:seq] s=0 MODELRENDER_START fbp=0x139 tbp=0x%x\n", ctx.tex0.tbp0);
                std::fflush(stderr);
            }
            if (comp2720 && g_g37_armed.load(std::memory_order_relaxed) == 1)
            {
                const uint32_t s = g_g37_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (s > 80u) g_g37_armed.store(2, std::memory_order_relaxed);
                if (s <= 80u)
                {
                    std::fprintf(stderr,
                        "[G37:seq] s=%u COMPOSITE destFbp=0x%x prim=%u abe=%u tbp=0x%x tpsm=0x%x tbw=%u v0=(%.0f,%.0f) rgba0=%02x,%02x,%02x,%02x\n",
                        s, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(gs->m_prim.abe), ctx.tex0.tbp0,
                        static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tbw),
                        gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                        gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
                    std::fflush(stderr);
                }
            }
        }
    }
    // [G38] One-shot dump at a costume body-strip draw (fbp=0x139). Decode the BOUND body
    // texture through the runtime's own sampler (GSMem::ReadP8 -> lookupCLUT) so we see what
    // the runtime actually samples for the denim/skin/cloth parts vs the HW .gs. Also dump
    // the raw VRAM blob. DC2_G38_VRAMDUMP=1; trigger texture via DC2_G38_TBP (default 0x34e0).
    {
        static const bool s_g38vram = (std::getenv("DC2_G38_VRAMDUMP") != nullptr);
        static const uint32_t s_g38tbp = []() {
            const char *e = std::getenv("DC2_G38_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0x34e0u;
        }();
        static std::atomic<int> s_g38done{0};
        const auto &tex = ctx.tex0;
        if (s_g38vram && gs->m_vram && gs->m_prim.tme && tex.tbp0 == s_g38tbp &&
            ctx.frame.fbp == 0x139u && s_g38done.exchange(1) == 0)
        {
            FILE *vf = std::fopen("captures/g38_runner_vram.bin", "wb");
            if (vf) { std::fwrite(gs->m_vram, 1, gs->m_vramSize, vf); std::fclose(vf); }
            const int dw = 1 << tex.tw, dh = 1 << tex.th;
            const uint32_t bwv = (tex.tbw > 1u) ? (tex.tbw >> 1) : 1u;
            char path[128];
            std::snprintf(path, sizeof(path), "captures/g38_tex_%x_runtime.ppm", tex.tbp0);
            FILE *fp = std::fopen(path, "wb");
            uint32_t nzColor = 0;
            if (fp)
            {
                std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                for (int yy = 0; yy < dh; ++yy)
                    for (int xx = 0; xx < dw; ++xx)
                    {
                        uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                         static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                        uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        if (c & 0x00FFFFFFu) ++nzColor;
                        uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                           static_cast<uint8_t>((c >> 8) & 0xFF),
                                           static_cast<uint8_t>((c >> 16) & 0xFF) };
                        std::fwrite(rgb, 1, 3, fp);
                    }
                std::fclose(fp);
            }
            // Sample a few CLUT entries directly so we can see if the palette decodes to black.
            uint32_t c0 = lookupCLUT(gs, 1, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c64 = lookupCLUT(gs, 64, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c128 = lookupCLUT(gs, 128, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c200 = lookupCLUT(gs, 200, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            std::fprintf(stderr,
                "[G38:texdump] tbp=0x%x %dx%d tbw=%u bwv=%u psm=0x%x cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "nzColor=%u/%d clut[1]=%08x clut[64]=%08x clut[128]=%08x clut[200]=%08x\n",
                tex.tbp0, dw, dh, (unsigned)tex.tbw, bwv, (unsigned)tex.psm, tex.cbp,
                (unsigned)tex.cpsm, (unsigned)tex.csm, (unsigned)tex.csa, nzColor, dw * dh,
                c0, c64, c128, c200);
            std::fflush(stderr);
        }
    }
    // [G231] Max's chest gem (empty-room repro). The gem's GS draw reaches the runner
    // byte-identically to the HW .gs (fbp=0x139, tbp=0x2720 CT24, TEXA As=128 opaque,
    // ALPHA=0x44, TEST=0x5000b, red vertex modulate) yet produces 0 red pixels. Gate on a
    // RED-vertex textured fbp=0x139 draw (title has no red verts here, so this only fires on
    // the room gem). One-shot: sample the CT24 texel the gem reads through the runtime's own
    // addressing, and dump the raw source pages so we can compare content to the ref freeze.
    {
        static const bool s_g231 = (std::getenv("DC2_G231_GEMDUMP") != nullptr);
        static std::atomic<uint32_t> s_g231n139{0};
        if (s_g231 && gs->m_vram && gs->m_prim.tme && ctx.frame.fbp == 0x139u)
        {
            const uint32_t n = s_g231n139.fetch_add(1, std::memory_order_relaxed) + 1u;
            // Log the distinct texture pages the character samples at fbp=0x139 (first sighting),
            // so we can see whether the gem source pages 0x34e0/0x3460 are ever bound here.
            {
                static std::atomic<uint64_t> s_seen[64];
                static std::atomic<int> s_seenN{0};
                const uint32_t tp = ctx.tex0.tbp0;
                bool known = false;
                const int cnt = s_seenN.load(std::memory_order_relaxed);
                for (int j = 0; j < cnt && j < 64; ++j)
                    if ((s_seen[j].load(std::memory_order_relaxed) & 0x3FFFu) == tp) { known = true; break; }
                if (!known && cnt < 64)
                {
                    int slot = s_seenN.fetch_add(1, std::memory_order_relaxed);
                    if (slot < 64)
                    {
                        s_seen[slot].store(tp, std::memory_order_relaxed);
                        std::fprintf(stderr, "[G231:tbp] fbp=0x139 samples tbp=0x%x psm=0x%x tbw=%u (drawN=%u)\n",
                                     tp, (unsigned)ctx.tex0.psm, (unsigned)ctx.tex0.tbw, n);
                        std::fflush(stderr);
                    }
                }
            }
            // The gem's source texture 0x34e0 (128x128 T8) holds jeans (upper-left) + the red
            // Atlamillia gem in the LOWER-RIGHT corner. Character draws are STQ (fst=0), so read
            // s/t/q and compute the normalized UV. Decode the sampled T8 texel through the CLUT
            // and count how many 0x34e0 draws sample a RED texel -> proves whether the gem
            // sub-mesh actually rasterizes the red gem region.
            if (ctx.tex0.tbp0 == 0x34e0u)
            {
                const uint32_t bwv = (ctx.tex0.tbw > 1u) ? (ctx.tex0.tbw >> 1) : 1u;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                bool sawRed = false; float unRed = 0, vnRed = 0; uint32_t cRed = 0;
                float unMax = 0, vnMax = 0;
                for (int t = 0; t < 3; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    float un = gs->m_vtxQueue[t].s / q;   // normalized U
                    float vn = gs->m_vtxQueue[t].t / q;   // normalized V
                    unMax = std::max(unMax, un); vnMax = std::max(vnMax, vn);
                    int tu = (int)(un * tw); int tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv,
                                          (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                          ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    if (r > 90 && r > g + 40 && r > b + 40) { sawRed = true; unRed = un; vnRed = vn; cRed = c; }
                }
                // Bright GEM red only (r>190,g<80,b<80) — excludes brown skin/leather.
                bool sawGem = false; uint32_t cGem = 0; float ug = 0, vg = 0;
                for (int t = 0; t < 3; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    float un = gs->m_vtxQueue[t].s / q, vn = gs->m_vtxQueue[t].t / q;
                    int tu = (int)(un * tw), tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv, (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    if (r > 190 && g < 80 && b < 80) { sawGem = true; cGem = c; ug = un; vg = vn; }
                }
                static std::atomic<uint32_t> s_34e0n{0}, s_34e0red{0}, s_gemN{0};
                const uint32_t k34 = s_34e0n.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (sawRed) s_34e0red.fetch_add(1, std::memory_order_relaxed);
                if (sawGem)
                {
                    const uint32_t gn = s_gemN.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (gn <= 30u)
                    {
                        // modulate: pixel = texel * vcol / 128 (PS2 modulate, TFX=MODULATE)
                        const uint32_t tr = cGem & 0xFF, tg = (cGem >> 8) & 0xFF, tb = (cGem >> 16) & 0xFF;
                        const uint32_t mr = (tr * gs->m_vtxQueue[0].r) >> 7;
                        const uint32_t mg = (tg * gs->m_vtxQueue[0].g) >> 7;
                        const uint32_t mb = (tb * gs->m_vtxQueue[0].b) >> 7;
                        std::fprintf(stderr,
                            "[G231:GEM] gemDraw#%u (of %u 0x34e0 draws) prim=%u texel=0x%06x UVn=(%.3f,%.3f) "
                            "vcol0=(%u,%u,%u,%u) tfx=%u => modulated=(%u,%u,%u) abe=%u alpha=0x%llx test=0x%llx\n",
                            gn, k34, (unsigned)gs->m_prim.type, cGem, ug, vg,
                            gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                            (unsigned)ctx.tex0.tfx, mr, mg, mb, (unsigned)gs->m_prim.abe,
                            (unsigned long long)(ctx.alpha & 0xFFFFFFFFFFull),
                            (unsigned long long)(ctx.test & 0xFFFFFFFFFFull));
                        std::fflush(stderr);
                    }
                }
                if ((k34 % 5000u) == 0u)
                    std::fprintf(stderr, "[G231:red34e0] CUM 0x34e0draws=%u brownRed=%u GEMbrightRed=%u\n",
                                 k34, s_34e0red.load(std::memory_order_relaxed), s_gemN.load(std::memory_order_relaxed));
            }
        }
    }
    // [G232] Localize the G231 gem pixel-write / composite loss. G231 proved the gem
    // rasterizes bright opaque red at fbp=0x139 yet no red reaches any framebuffer. Split
    // "written then lost" vs "never written" in one run: (a) log each gem-red draw's screen
    // XY / scissor / FBMSK / Z (G231 never logged XY), (b) scan the RTT page (0x272000) for
    // gem red at the NEXT drawPrimitive entry after a gem draw (= post-raster under serial
    // levers), (c) scan + dump the page at composite time (tme && tbp0=0x2720 && fbp!=0x139).
    // DC2_G232_RTTDUMP=1, default-off; run serial (G144/G178/G157/MTGS off) so the CPU path
    // rasters synchronously.
    {
        static const bool s_g232 = (std::getenv("DC2_G232_RTTDUMP") != nullptr);
        // Full 512x416 CT32 RTT extent = fbw 8 pages/row * 13 page-rows * 8KB = 0xD0000 bytes
        // (the first run scanned only 0x80000 = rows 0..255 and missed the character band).
        if (s_g232 && gs->m_vram && 0x272000u + 0xD0000u <= gs->m_vramSize)
        {
            static std::atomic<int> s_g232pending{0};
            static std::atomic<uint32_t> s_g232gemN{0}, s_g232postN{0}, s_g232compN{0}, s_g232dumpN{0};
            const auto scan2720 = [gs](uint32_t &nz, uint32_t &red, uint32_t &firstRedOff, uint32_t &lastNzOff) {
                nz = red = 0; firstRedOff = 0xFFFFFFFFu; lastNzOff = 0;
                const uint32_t *w = reinterpret_cast<const uint32_t *>(gs->m_vram + 0x272000u);
                for (uint32_t i = 0; i < 0xD0000u / 4u; ++i)
                {
                    const uint32_t c = w[i];
                    if (c & 0xFFFFFFu) { ++nz; lastNzOff = i * 4u; }
                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                    if (r > 150u && g < 90u && b < 110u)
                    {
                        ++red;
                        if (firstRedOff == 0xFFFFFFFFu) firstRedOff = i * 4u;
                    }
                }
            };
            // (b) post-raster scan armed by the previous (gem) draw.
            if (s_g232pending.exchange(0, std::memory_order_relaxed) == 1)
            {
                const uint32_t pn = s_g232postN.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (pn <= 30u || (pn % 50u) == 0u)
                {
                    uint32_t nz, red, fro, lnz;
                    scan2720(nz, red, fro, lnz);
                    // pageRow: CT32 fbw=8 -> 8KB page = 64x32 px; row ~ (off/8192)/8*32
                    std::fprintf(stderr,
                        "[G232:postGem] n=%u rtt2720 nz=%u gemRed=%u firstRedOff=0x%x (~row %u) lastNzOff=0x%x (~row %u) "
                        "CUMpx bbox=%u inside=%u zfail=%u write=%u redWrite=%u\n",
                        pn, nz, red, fro, (fro == 0xFFFFFFFFu) ? 0u : (fro / 8192u) / 8u * 32u,
                        lnz, (lnz / 8192u) / 8u * 32u,
                        g_g232PxBbox.load(std::memory_order_relaxed),
                        g_g232PxInside.load(std::memory_order_relaxed),
                        g_g232PxZfail.load(std::memory_order_relaxed),
                        g_g232PxWrite.load(std::memory_order_relaxed),
                        g_g232PxRedWrite.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                }
            }
            // (c) composite-time scan + one-shot dumps (only once a gem draw was seen this run).
            if (gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u && ctx.frame.fbp != 0x139u)
            {
                const uint32_t cn = s_g232compN.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (cn <= 40u || (cn % 100u) == 0u)
                {
                    uint32_t nz, red, fro, lnz;
                    scan2720(nz, red, fro, lnz);
                    std::fprintf(stderr,
                        "[G232:comp] n=%u destFbp=0x%x nz=%u gemRed=%u firstRedOff=0x%x (~row %u) "
                        "lastNzOff=0x%x (~row %u) gemDrawsSoFar=%u\n",
                        cn, ctx.frame.fbp, nz, red, fro,
                        (fro == 0xFFFFFFFFu) ? 0u : (fro / 8192u) / 8u * 32u,
                        lnz, (lnz / 8192u) / 8u * 32u,
                        s_g232gemN.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                    // One-shot after the room's gem draws exist: dump the FULL RTT extent + a
                    // whole-VRAM red census (0x20000 regions) to see where red DID land, if
                    // anywhere.
                    if (s_g232gemN.load(std::memory_order_relaxed) > 100u &&
                        s_g232dumpN.load(std::memory_order_relaxed) < 3u)
                    {
                        const uint32_t dn = s_g232dumpN.fetch_add(1, std::memory_order_relaxed);
                        if (dn < 3u)
                        {
                            char path[128];
                            std::snprintf(path, sizeof(path), "captures/g232_rtt_comp_%u.bin", dn);
                            FILE *fp = std::fopen(path, "wb");
                            if (fp)
                            {
                                std::fwrite(gs->m_vram + 0x272000u, 1, 0xD0000u, fp);
                                std::fclose(fp);
                                std::fprintf(stderr, "[G232:dump] %s full 0xD0000 (comp n=%u)\n", path, cn);
                            }
                            char line[1024]; int lp = 0;
                            lp += std::snprintf(line + lp, sizeof(line) - lp,
                                                "[G232:vramred] regions(0x20000) red:");
                            for (uint32_t rg = 0; rg < gs->m_vramSize / 0x20000u &&
                                                  lp + 24 < (int)sizeof(line); ++rg)
                            {
                                uint32_t rc = 0;
                                const uint32_t *w = reinterpret_cast<const uint32_t *>(
                                    gs->m_vram + rg * 0x20000u);
                                for (uint32_t i = 0; i < 0x20000u / 4u; ++i)
                                {
                                    const uint32_t c = w[i];
                                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu,
                                                   b = (c >> 16) & 0xFFu;
                                    if (r > 150u && g < 90u && b < 110u) ++rc;
                                }
                                if (rc)
                                    lp += std::snprintf(line + lp, sizeof(line) - lp, " 0x%x=%u",
                                                        rg * 0x20000u, rc);
                            }
                            std::fprintf(stderr, "%s\n", line);
                            std::fflush(stderr);
                        }
                    }
                }
            }
            // (a) gem-red draw detect (same texel test as G231's sawGem) + XY/state log + arm (b).
            if (gs->m_prim.tme && ctx.frame.fbp == 0x139u && ctx.tex0.tbp0 == 0x34e0u)
            {
                const uint32_t bwv = (ctx.tex0.tbw > 1u) ? (ctx.tex0.tbw >> 1) : 1u;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                bool sawGem = false;
                for (int t = 0; t < 3 && !sawGem; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    const float un = gs->m_vtxQueue[t].s / q, vn = gs->m_vtxQueue[t].t / q;
                    int tu = (int)(un * tw), tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv,
                                          (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                          ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                    if (r > 190u && g < 80u && b < 80u) sawGem = true;
                }
                if (sawGem)
                {
                    const uint32_t gn = s_g232gemN.fetch_add(1, std::memory_order_relaxed) + 1u;
                    s_g232pending.store(1, std::memory_order_relaxed);
                    g_g232TriArm.store(1, std::memory_order_relaxed);
                    g_g232GemX.store((int)gs->m_vtxQueue[0].x - (int)(ctx.xyoffset.ofx >> 4),
                                     std::memory_order_relaxed);
                    g_g232GemY.store((int)gs->m_vtxQueue[0].y - (int)(ctx.xyoffset.ofy >> 4),
                                     std::memory_order_relaxed);
                    if (gn <= 40u || (gn % 200u) == 0u)
                    {
                        const int ofx = static_cast<int>(ctx.xyoffset.ofx >> 4);
                        const int ofy = static_cast<int>(ctx.xyoffset.ofy >> 4);
                        std::fprintf(stderr,
                            "[G232:gemXY] #%u prim=%u v0=(%.1f,%.1f,z=%.0f) v1=(%.1f,%.1f) v2=(%.1f,%.1f) "
                            "ofxy=(%d,%d) scissor=(%u,%u)-(%u,%u) fbw=%u fpsm=0x%x fbmsk=0x%x "
                            "zbp=0x%x zmsk=%u test=0x%llx\n",
                            gn, (unsigned)gs->m_prim.type,
                            gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y, gs->m_vtxQueue[0].z,
                            gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                            gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y,
                            ofx, ofy,
                            ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                            (unsigned)ctx.frame.fbw, (unsigned)ctx.frame.psm, ctx.frame.fbmsk,
                            (unsigned)(ctx.zbuf & 0x1FFu), (unsigned)((ctx.zbuf >> 32) & 1u),
                            (unsigned long long)(ctx.test & 0xFFFFFFFFFFull));
                        std::fflush(stderr);
                    }
                }
            }
        }
    }
    // [F51.2] Classify the render-to-texture draws that target the manager texture page
    // (fbp=0x139 -> VRAM byte 0x272000, aliasing tbp=0x2720). Is this textured compositing
    // (producing content) or a flat fill that clears the uploaded T8 data to black?
    if (renderQualityTraceEnabled() && ctx.frame.fbp == 0x139u)
    {
        static std::atomic<uint32_t> s_f512DrawCount{0};
        const uint32_t n = s_f512DrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 48u)
        {
            std::fprintf(stderr,
                "[F51.2:rtt139] n=%u prim=%u tme=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x "
                "tex(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x) "
                "scissor=(%u,%u)-(%u,%u) ofxy=(%d,%d) "
                "v0=(%g,%g) v1=(%g,%g) v2=(%g,%g) uv0=(%u,%u) uv1=(%u,%u) "
                "rgba0=%02x,%02x,%02x,%02x rgba1=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.ctxt),
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                static_cast<uint32_t>(ctx.tex0.tcc), static_cast<uint32_t>(ctx.tex0.tfx),
                ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                static_cast<int>(ctx.xyoffset.ofx >> 4), static_cast<int>(ctx.xyoffset.ofy >> 4),
                gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y,
                static_cast<uint32_t>(gs->m_vtxQueue[0].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[0].v >> 4),
                static_cast<uint32_t>(gs->m_vtxQueue[1].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[1].v >> 4),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a);
        }
    }
    // [G37/G220] Historical costume-RTT workaround. G37 bulk-zeroed the shared 0x2720 page
    // because the costume clear rect used to collapse with its GetDrawRect bbox. The G37 bbox
    // repair now gives the game's own clear sprite its intended extent, and G220 proved this
    // synthetic clear redundant on the costume route. Worse, MAP-4 zoom legitimately reuses
    // fbp=0x139/tbp=0x2720 as a screen-effect work page and satisfies the old broad signature:
    // this loop then destroys the captured sky before G144 replays the display composite.
    // Keep the old behavior only as an opt-in historical A/B; never enable it by default.
    const float rttClearX0 = std::min(gs->m_vtxQueue[0].x, gs->m_vtxQueue[1].x) -
        static_cast<float>(ctx.xyoffset.ofx >> 4);
    const float rttClearY0 = std::min(gs->m_vtxQueue[0].y, gs->m_vtxQueue[1].y) -
        static_cast<float>(ctx.xyoffset.ofy >> 4);
    const float rttClearX1 = std::max(gs->m_vtxQueue[0].x, gs->m_vtxQueue[1].x) -
        static_cast<float>(ctx.xyoffset.ofx >> 4);
    const float rttClearY1 = std::max(gs->m_vtxQueue[0].y, gs->m_vtxQueue[1].y) -
        static_cast<float>(ctx.xyoffset.ofy >> 4);
    const bool g37FullRttClear =
        rttClearX0 <= 220.0f && rttClearY0 <= 0.0f &&
        rttClearX1 >= 512.0f && rttClearY1 >= 380.0f;
    if (ctx.frame.fbp == 0x139u && gs->m_prim.type == 6u && gs->m_prim.tme == 0u && gs->m_vram &&
        gs->m_vtxQueue[0].r == 0 && gs->m_vtxQueue[0].g == 0 &&
        gs->m_vtxQueue[0].b == 0 && gs->m_vtxQueue[0].a == 0 &&
        g37FullRttClear)
    {
        static const bool forceLegacyClear =
            envFlagEnabled("DC2_G37_FORCE_CLEAR") && !envFlagEnabled("DC2_G37_NOCLEAR");
        if (forceLegacyClear)
        {
            // Composite samples RTT x:[~224,512] y:[0,381]; clear a small margin wider.
            for (uint32_t y = 0u; y < 400u; ++y)
                for (uint32_t x = 208u; x < 512u; ++x)
                    GSMem::WriteCT32(gs->m_vram, 0x2720u, 8u, x, y, 0u);
            // G41: the model RTT re-renders every frame with depth test GEQUAL; its Z buffer
            // (ZBUF.ZBP, page->block) is never cleared by the runner, so stale z from prior
            // frames rejects current model pixels (shredded model). Clear it to 0 (farthest
            // for GEQUAL) here, mirroring the HW per-frame Z clear. Same rect as the color clear.
            static const bool s_noZClear = (std::getenv("DC2_NO_ZTEST") != nullptr);
            if (!s_noZClear)
            {
                const uint32_t czbp = static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5;
                const uint32_t czbw = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 8u;
                const uint32_t czpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
                const bool cz16 = (czpsm == 0x2u || czpsm == 0xAu || czpsm == 0xBu);
                if (g44FaceZTraceEnabled())
                {
                    static std::atomic<uint32_t> s_g44zc{0};
                    const uint32_t zcn = s_g44zc.fetch_add(1u, std::memory_order_relaxed);
                    if (zcn < 200u)
                    {
                        const uint32_t before38092 = cz16
                            ? GSMem::ReadZ16(gs->m_vram, czbp, czbw, 380u, 92u)
                            : GSMem::ReadZ32(gs->m_vram, czbp, czbw, 380u, 92u);
                        const uint32_t before380124 = cz16
                            ? GSMem::ReadZ16(gs->m_vram, czbp, czbw, 380u, 124u)
                            : GSMem::ReadZ32(gs->m_vram, czbp, czbw, 380u, 124u);
                        std::fprintf(stderr,
                            "[G44:zclear] n=%u zbufpage=0x%x czbp=0x%x czpsm=0x%x czbw=%u "
                            "before(380,92)=%u before(380,124)=%u v0=(%.1f,%.1f) v1=(%.1f,%.1f)\n",
                            zcn, static_cast<uint32_t>(ctx.zbuf & 0x1FFu), czbp, czpsm, czbw,
                            before38092, before380124,
                            gs->m_vtxQueue[0].x - static_cast<float>(ctx.xyoffset.ofx >> 4),
                            gs->m_vtxQueue[0].y - static_cast<float>(ctx.xyoffset.ofy >> 4),
                            gs->m_vtxQueue[1].x - static_cast<float>(ctx.xyoffset.ofx >> 4),
                            gs->m_vtxQueue[1].y - static_cast<float>(ctx.xyoffset.ofy >> 4));
                    }
                }
                static std::atomic<uint32_t> s_g41zc{0};
                if (renderQualityTraceEnabled() && s_g41zc.fetch_add(1u) < 4u)
                    std::fprintf(stderr, "[G41:zclear] zbufpage=0x%x czbp(block)=0x%x czpsm=0x%x czbw=%u\n",
                        (unsigned)(ctx.zbuf & 0x1FFu), czbp, czpsm, czbw);
                // G45: the costume model's Z buffer (block 0x1a00 in the runner's FLAT 4MB VRAM)
                // ALIASES the menu's text/sprite VRAM. That aliasing is bidirectional: the menu
                // text sprites redrawn every frame stomp the model's stored Z (-> GEQUAL rejects
                // model triangles next frame -> flicker / disappearing parts, esp. Red Vest in the
                // interactive/zoom pose), and a wide Z clear here stomps the text. Fix: the
                // costume model (fbp=0x139) uses a PRIVATE off-screen Z buffer (costumeZBuf) that
                // aliases nothing. Clear it FULLY every frame here; per-pixel Z test/write below
                // route to it instead of GSMem on m_vram. No VRAM Z writes -> text intact.
                (void)cz16; (void)czbp; (void)czbw;
                costumeZClear();
            }
            static std::atomic<uint32_t> s_g37clear{0};
            const uint32_t cn = s_g37clear.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (renderQualityTraceEnabled() && (cn <= 4u || (cn % 240u) == 0u))
            {
                std::fprintf(stderr, "[G37:rttclear] n=%u zeroed 0x2720 CT32 region x[208,512) y[0,400)\n", cn);
                std::fflush(stderr);
            }
        }
    }
    // [F51.4] Enumerate what the DISPLAYED dungeon buffer (fbp=0x68) draws sample — the real
    // visible-black gate, distinct from the fbp=0x139 manager RTT bucket. Log distinct TEX0 for
    // textured draws + a tally of textured vs untextured(=flat) draws and their vertex color.
    if (renderQualityTraceEnabled() && ctx.frame.fbp == 0x68u)
    {
        static std::atomic<uint32_t> s_f514Tex{0};
        static std::atomic<uint32_t> s_f514Flat{0};
        static std::atomic<uint64_t> s_f514Seen[32] = {};
        if (gs->m_prim.tme)
        {
            const uint64_t key = (static_cast<uint64_t>(ctx.tex0.tbp0) << 40) |
                                 (static_cast<uint64_t>(ctx.tex0.psm) << 32) |
                                 (static_cast<uint64_t>(ctx.tex0.cbp) << 8) |
                                 (static_cast<uint64_t>(ctx.tex0.cpsm));
            bool fresh = true;
            const uint32_t slots = static_cast<uint32_t>(s_f514Tex.load(std::memory_order_relaxed));
            for (uint32_t i = 0; i < slots && i < 32u; ++i)
                if (s_f514Seen[i].load(std::memory_order_relaxed) == key) { fresh = false; break; }
            if (fresh)
            {
                const uint32_t idx = s_f514Tex.fetch_add(1u, std::memory_order_relaxed);
                if (idx < 32u)
                {
                    s_f514Seen[idx].store(key, std::memory_order_relaxed);
                    std::fprintf(stderr,
                        "[F51.4:fbp68tex] new=%u prim=%u abe=%u tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u "
                        "tcc=%u tfx=%u cbp=0x%x cpsm=0x%x v0rgba=%02x,%02x,%02x,%02x\n",
                        idx,
                        static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                        ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                        static_cast<uint32_t>(ctx.tex0.psm),
                        static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                        static_cast<uint32_t>(ctx.tex0.tcc), static_cast<uint32_t>(ctx.tex0.tfx),
                        ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                        gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
                }
            }
        }
        else
        {
            const uint32_t fn = s_f514Flat.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (fn <= 8u || (fn % 65536u) == 0u)
                std::fprintf(stderr,
                    "[F51.4:fbp68flat] n=%u prim=%u abe=%u v0rgba=%02x,%02x,%02x,%02x v1rgba=%02x,%02x,%02x,%02x\n",
                    fn, static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                    gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                    gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a);
        }
    }
    // [G34] The costume model RTT->display composite (DrawDivSprite4) samples the 0x2720
    // work buffer as CT32 (psm=0). [F51.4] (fbp==0x68 only) and [F509] (paletted only) both
    // miss a CT32 bind, so log every CT32 0x2720 textured draw and the buffer it lands on —
    // tells whether the composite reaches a display buffer (fbp 0x0/0x68), an offscreen one,
    // or never happens.
    if (renderQualityTraceEnabled() && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u)
    {
        // Log EVERY textured draw sampling block 0x2720 (any psm): brick=T8(0x13),
        // the model RTT composite=CT32(0x0). inDiv>0 => emitted from DrawDivSprite4 (the
        // composite). Shows whether the CT32 composite reaches rasterization and its psm.
        const int inDiv = g_g34_in_divsprite.load(std::memory_order_relaxed);
        static std::atomic<uint32_t> s_g34comp{0};
        const uint32_t n = s_g34comp.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 96u || inDiv > 0 || (n % 512u) == 0u)
            std::fprintf(stderr,
                "[G34:composite] n=%u inDiv=%d destFbp=0x%x destFbw=%u destPsm=0x%x prim=%u abe=%u "
                "tbp=0x2720 tpsm=0x%x tbw=%u tw=%u th=%u\n",
                n, inDiv, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th));

        // G37: one-shot dump of the CT32 RTT (block 0x2720, 512x512) that the costume
        // composite samples, so we can SEE whether the model RTT is fully rendered or
        // mostly stale brick. Fires on the first CT32 (psm=0) composite draw.
        static std::atomic<uint32_t> s_g37dump{0};
        const uint32_t g37dn = s_g37dump.fetch_add(1u, std::memory_order_relaxed);
        static const bool s_g37RttDump = (std::getenv("DC2_G37_RTTDUMP") != nullptr);
        if (ctx.tex0.psm == 0u && gs->m_vram &&
            (g268LiveEnvRead() ? (std::getenv("DC2_G37_RTTDUMP") != nullptr) : s_g37RttDump) &&
            (g37dn % 200u) == 150u)  // a late, steady-state composite (overwrites each window)
        {
            FILE *f = std::fopen("D:/ps2r/dc2/captures/g37_rtt_2720.ppm", "wb");
            if (f)
            {
                std::fprintf(f, "P6\n512 512\n255\n");
                for (uint32_t yy = 0u; yy < 512u; ++yy)
                    for (uint32_t xx = 0u; xx < 512u; ++xx)
                    {
                        const uint32_t px = GSMem::ReadCT32(gs->m_vram, 0x2720u, 8u, xx, yy);
                        const unsigned char rgb[3] = {
                            static_cast<unsigned char>(px & 0xFFu),
                            static_cast<unsigned char>((px >> 8u) & 0xFFu),
                            static_cast<unsigned char>((px >> 16u) & 0xFFu)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                std::fclose(f);
                std::fprintf(stderr, "[G37:rttdump] wrote captures/g37_rtt_2720.ppm tbw=%u\n",
                             static_cast<uint32_t>(ctx.tex0.tbw));
                std::fflush(stderr);
            }
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << gs->m_vtxQueue[0].x << "," << gs->m_vtxQueue[0].y << ")"
                      << " uv0=(" << (gs->m_vtxQueue[0].u >> 4) << "," << (gs->m_vtxQueue[0].v >> 4) << ")"
                      << " stq0=(" << gs->m_vtxQueue[0].s << "," << gs->m_vtxQueue[0].t << "," << gs->m_vtxQueue[0].q << ")"
                      << " v1=(" << gs->m_vtxQueue[1].x << "," << gs->m_vtxQueue[1].y << ")"
                      << " uv1=(" << (gs->m_vtxQueue[1].u >> 4) << "," << (gs->m_vtxQueue[1].v >> 4) << ")"
                      << " stq1=(" << gs->m_vtxQueue[1].s << "," << gs->m_vtxQueue[1].t << "," << gs->m_vtxQueue[1].q << ")"
                      << " v2=(" << gs->m_vtxQueue[2].x << "," << gs->m_vtxQueue[2].y << ")"
                      << " uv2=(" << (gs->m_vtxQueue[2].u >> 4) << "," << (gs->m_vtxQueue[2].v >> 4) << ")"
                      << " stq2=(" << gs->m_vtxQueue[2].s << "," << gs->m_vtxQueue[2].t << "," << gs->m_vtxQueue[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(gs->m_vtxQueue[0].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(gs->m_vtxQueue[1].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(gs->m_vtxQueue[2].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((gs->m_prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });
    if (g_g312SegStat)
    {
        const auto g312T2 = std::chrono::steady_clock::now();
        g_g312Seg0Ns.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(g312T1 - g312T0).count(),
            std::memory_order_relaxed);
        g_g312Seg1Ns.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(g312T2 - g312T1).count(),
            std::memory_order_relaxed);
        t_g312Seg2Start = g312T2;
    }

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
    {
        gs->m_hasPreferredDisplaySource = false;
    }

    if (s_g141PerfOn)
    {
        if (gs->m_prim.type == GS_PRIM_SPRITE)
            g_g141SpriteCalls.fetch_add(1u, std::memory_order_relaxed);
        else if (gs->m_prim.type == GS_PRIM_TRIANGLE || gs->m_prim.type == GS_PRIM_TRISTRIP ||
                 gs->m_prim.type == GS_PRIM_TRIFAN)
            g_g141TriCalls.fetch_add(1u, std::memory_order_relaxed);
        static uint64_t s_covTick = 0ull;
        if ((++s_covTick % 8000ull) == 0ull)
        {
            static uint64_t lb = 0, li = 0, ld = 0;
            const uint64_t bb = g_g141BboxPx.load(std::memory_order_relaxed);
            const uint64_t in = g_g141InsidePx.load(std::memory_order_relaxed);
            const uint64_t dr = g_g141DrawnPx.load(std::memory_order_relaxed);
            const uint64_t dbb = bb - lb, din = in - li, ddr = dr - ld;
            lb = bb; li = in; ld = dr;
            std::fprintf(stderr,
                         "[G141:cov] triCalls=%llu sprCalls=%llu | per-8000-prim: bboxPx=%llu insidePx=%llu drawnPx=%llu coverage=%.0f%% overdraw=%.2fx\n",
                         static_cast<unsigned long long>(g_g141TriCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_g141SpriteCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(dbb), static_cast<unsigned long long>(din),
                         static_cast<unsigned long long>(ddr),
                         dbb ? 100.0 * static_cast<double>(din) / static_cast<double>(dbb) : 0.0,
                         din ? static_cast<double>(ddr) / static_cast<double>(din) : 0.0);
        }
    }
    if (g_g312SegStat)
        t_g312Seg2aEnd = std::chrono::steady_clock::now(); // G312 seg2a end (perf-counter block done)