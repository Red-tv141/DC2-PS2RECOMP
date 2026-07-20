    // ===== G144 frame-level tile-binning capture/flush (G168: DEFAULT 8 lanes) =====
    // Runs AFTER the perf counters (so deferred tris still count) and BEFORE the switch (so the
    // flush's parallel raster time is still inside this drawPrimitive's G141PerfScope → measurable).
    if (g_g144Threads < 0)
    {
        const char *e144 = std::getenv("DC2_G144_TILEBIN");
        int n144 = (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
                       ? 0
                       : (e144 ? std::atoi(e144) : 8);
        g_g144Threads = (n144 < 0) ? 0 : (n144 > 16 ? 16 : n144);
    }
    if (g_g144Threads > 1 && !t_g144InReplay)
    {
        g_g144LastGs = gs;
        g_g144LastVram = gs->m_vram;
        // Assign the flush closure ONCE (keeps this member's friend access). Parallel band replay:
        // each lane owns a disjoint scanline band and replays every overlapping entry IN SUBMISSION
        // ORDER, clipped to its band via the thread_local clamp → disjoint pixel writes,
        // order-preserved blend/Z. Bit-exact. Uses g_g144LastGs so g144FlushPending() (frame end)
        // can invoke it too.
        if (!g_g144FlushFn)
        {
            GSRasterizer *self = this;
            g_g144FlushFn = [self]() {
                // G307 Pillar-1 feasibility census (no-op unless DC2_G307_PIPE=1): brackets this
                // raster and accounts the decode<->raster pipeline overlap ceiling. Constructed
                // while g_g144List is still intact (snapshots the batch output), destroyed after
                // the closure clears it (banks the raster wall time). Covers EVERY flush trigger
                // (mid target-switch / upload edge / frame end) since all run this closure.
                G307FlushScope g307scope;
                const bool g290P = g290ProbeOn();
                const auto g290T0 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g_g144LastGs)
                    g219Sentinel(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize, "flush-preg162");
                // G166: resolve any batched T8 GPU-decode requests FIRST, unconditionally -- every
                // e.decoded the replay below reads must already be pixel-filled (see
                // g162ResolvePendingT8Batch's comment for why this ordering is safe by construction).
                g162ResolvePendingT8Batch();
                const auto g290T1 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290ResolveNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T1 - g290T0)
                            .count());
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                g219Sentinel(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize, "flush-start");
                // G178: whole-flush GPU attempt (default OFF, DC2_G178_GPU=1). On success the
                // batch is already rendered AND read back into VRAM — skip the CPU replay
                // entirely. On ANY unsupported entry it returns false having changed nothing,
                // and the proven CPU band replay below runs for the whole flush.
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
                    if (g290P)
                    {
                        ++g_g290GpuOk;
                        g_g290GpuOkNs += static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g290T1)
                                .count());
                    }
                    g_g144List.clear();
                    return;
                }
                const auto g290T2 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                {
                    ++g_g290GpuFail;
                    g_g290GpuFailNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T2 - g290T1)
                            .count());
                    g_g290CpuEntries += g_g144List.size();
                    static uint64_t s_g290FailLogs = 0;
                    if (s_g290FailLogs < 48u || (g_g290GpuFail % 64u) == 0u)
                    {
                        ++s_g290FailLogs;
                        const G144Entry &e0 = g_g144List[0];
                        std::fprintf(stderr,
                                     "[G290:gpufail] n=%llu entries=%zu fbp=%03x fbw=%u fpsm=%x "
                                     "zbp=%03x rej=%s site=%d e0(prim=%u tme=%u tpsm=%x tbp=%x "
                                     "abe=%u sc=%u,%u..%u,%u)\n",
                                     (unsigned long long)g_g290GpuFail, g_g144List.size(),
                                     e0.ctx.frame.fbp, (unsigned)e0.ctx.frame.fbw,
                                     e0.ctx.frame.psm, (unsigned)(e0.ctx.zbuf & 0x1FFu),
                                     kG266FrName[g_g266FlushRej & 0xF], g_g266ClassifySite,
                                     (unsigned)e0.prim.type, e0.prim.tme ? 1u : 0u,
                                     e0.prim.tme ? e0.ctx.tex0.psm : 0u,
                                     e0.prim.tme ? e0.ctx.tex0.tbp0 : 0u,
                                     e0.prim.abe ? 1u : 0u, e0.ctx.scissor.x0,
                                     e0.ctx.scissor.y0, e0.ctx.scissor.x1, e0.ctx.scissor.y1);
                    }
                }
                // G272: CPU fallback consumes/writes guest VRAM. Any earlier display wave must be
                // published first; the current failed batch has not been marked resident.
                (void)g278FlushPendingDepth(3); // CPU replay needs authoritative guest Z
                (void)g289MaterializeOwner(3);
                g272MaterializeAll(3);
                g277CensusHardEdge(3);
                g276FlushPendingDisplay(3); // G276: publish deferred display readbacks first
                // The proven CPU replay and its decoded textures consume guest VRAM directly.
                // Publish any RTT rows left GPU-resident by a G261 wave (only where this list's
                // target/Z/texture/CLUT pages actually touch them), then any GPU-owned depth.
                g261PrepareCpuReplay();
                g242PrepareVramReadAll(g_g144LastGs->m_vram);
                const auto g290T3 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290PubNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T3 - g290T2)
                            .count());
                // Pre-run the title-Z fbp-transition clear ONCE, single-threaded (replay skips it).
                // G203: universal-Z uses no artificial clears — the game's own clear-sprite populates
                // VRAM Z during the replay below (the g202 clear here is what stomped block 0x1a00 /
                // the menu font staging into inventory/loading; skip it entirely under universal-Z).
                if (!g203UniversalZEnabled())
                {
                    if (g106TitleZPrivateEnabled())
                        g106TitleZClearIfNeeded(g_g144BlockFbp);
                    const GSContext &c = g_g144List[0].ctx;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = g_g144List[0].townDepthScope;
                    g202TownZClearIfNeeded(g_g144LastGs->m_vram, c.frame.fbp, c.zbuf, c.frame.fbw);
                    t_g144TownDepthScopeOverride = false;
                }
                int y0 = 0x7fffffff, y1 = -0x7fffffff;
                for (const auto &e : g_g144List)
                {
                    if (e.minY < y0) y0 = e.minY;
                    if (e.maxY > y1) y1 = e.maxY;
                }
                if (y1 >= y0)
                {
                    GS *rgs = g_g144LastGs;
                    GSRowPool::instance().run(y0, y1, g_g144Threads, [self, rgs](int a, int b) {
                        static thread_local GS s_rg;
                        s_rg.m_vram = rgs->m_vram;
                        s_rg.m_vramSize = rgs->m_vramSize;
                        s_rg.m_privRegs = rgs->m_privRegs;
                        t_g144InReplay = true;
                        t_g144Banded = true;
                        t_g144BandY0 = a;
                        t_g144BandY1 = b;
                        for (const auto &e : g_g144List)
                        {
                            if (e.maxY < a || e.minY > b)
                                continue; // bbox does not intersect this band
                            s_rg.m_ctx[0] = e.ctx;
                            s_rg.m_prim = e.prim;
                            s_rg.m_prim.ctxt = false; // active context forced to m_ctx[0] = snapshot
                            s_rg.m_texa = e.texa;
                            s_rg.m_texclut = e.texclut;
                            s_rg.m_pabe = e.pabe;
                            t_g144TitleRockScopeOverride = true;
                            t_g144TitleRockScope = e.titleRockScope;
                            t_g144TownDepthScopeOverride = true;
                            t_g144TownDepthScope = e.townDepthScope;
                            s_rg.m_vtxQueue[0] = e.v[0];
                            s_rg.m_vtxQueue[1] = e.v[1];
                            s_rg.m_vtxQueue[2] = e.v[2];
                            t_g258EntryIndex = static_cast<size_t>(&e - g_g144List.data());
                            if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                            else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                            // G172: widened G144 to also defer SPRITE entries (see the capture site);
                            // dispatch by the captured primitive type instead of always drawTriangle.
                            if (g270LineType(static_cast<uint32_t>(e.prim.type)))
                                self->drawLine(&s_rg);
                            else if (e.prim.type == GS_PRIM_SPRITE)
                                self->drawSprite(&s_rg);
                            else
                                self->drawTriangle(&s_rg);
                        }
                        t_g149Px = nullptr; t_g149Valid = nullptr;
                        t_g258EntryIndex = static_cast<size_t>(-1);
                        t_g144TitleRockScopeOverride = false;
                        t_g144TownDepthScopeOverride = false;
                        t_g144InReplay = false;
                        t_g144Banded = false;
                    });
                }
                const auto g290T4 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290ReplayNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T4 - g290T3)
                            .count());
                g242NoteCpuReplayWrites(g_g144LastGs->m_vram);
                // G261: the wave arm disables the G260 pre-execution RTT generation bump (it
                // would poison GPU-resident targets); publish the replay's RTT writes here,
                // after the bytes actually changed.
                g261NoteCpuRttWrites(g_g144LastGs->m_vram);
                g286NoteCpuTransientWrites(g_g144LastGs->m_vram);
                g255CompareCpuVerify(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize);
                if (g290P)
                    g_g290NoteNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - g290T4)
                            .count());
                g_g144List.clear();
            };
            // Sequential drain (frame end): no pool, no bands — replay every entry full-range on the
            // calling (guest) thread, in order. Correctness-safe fallback for the trailing tail.
            g_g144FlushSeqFn = [self]() {
                G307FlushScope g307scope; // G307 census (see g_g144FlushFn above); no-op unless DC2_G307_PIPE=1
                // G166: same unconditional resolve as g_g144FlushFn above (either closure may run
                // first, e.g. under DC2_G144_SEQ, so both must resolve before reading e.decoded).
                g162ResolvePendingT8Batch();
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                // G178: same whole-flush GPU attempt as the parallel closure above.
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
                    g_g144List.clear();
                    return;
                }
                (void)g278FlushPendingDepth(3); // CPU replay needs authoritative guest Z
                (void)g289MaterializeOwner(3);
                g272MaterializeAll(3); // CPU replay boundary (see parallel closure)
                g277CensusHardEdge(3);
                g276FlushPendingDisplay(3); // G276: publish deferred display readbacks first
                g261PrepareCpuReplay(); // G261: materialize touched GPU-resident RTT rows first
                g242PrepareVramReadAll(g_g144LastGs->m_vram);
                // G203: skip all artificial Z pre-clears under universal-Z (see the parallel closure).
                if (!g203UniversalZEnabled())
                {
                    if (g106TitleZPrivateEnabled())
                        g106TitleZClearIfNeeded(g_g144BlockFbp);
                    const GSContext &c = g_g144List[0].ctx;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = g_g144List[0].townDepthScope;
                    g202TownZClearIfNeeded(g_g144LastGs->m_vram, c.frame.fbp, c.zbuf, c.frame.fbw);
                    t_g144TownDepthScopeOverride = false;
                }
                static thread_local GS s_seq;
                GS *rgs = g_g144LastGs;
                s_seq.m_vram = rgs->m_vram;
                s_seq.m_vramSize = rgs->m_vramSize;
                s_seq.m_privRegs = rgs->m_privRegs;
                t_g144InReplay = true;
                t_g144Banded = false; // full row range
                for (const auto &e : g_g144List)
                {
                    s_seq.m_ctx[0] = e.ctx;
                    s_seq.m_prim = e.prim;
                    s_seq.m_prim.ctxt = false;
                    s_seq.m_texa = e.texa;
                    s_seq.m_texclut = e.texclut;
                    s_seq.m_pabe = e.pabe;
                    t_g144TitleRockScopeOverride = true;
                    t_g144TitleRockScope = e.titleRockScope;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = e.townDepthScope;
                    s_seq.m_vtxQueue[0] = e.v[0];
                    s_seq.m_vtxQueue[1] = e.v[1];
                    s_seq.m_vtxQueue[2] = e.v[2];
                    t_g258EntryIndex = static_cast<size_t>(&e - g_g144List.data());
                    if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                    else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                    // G172: see the parallel-replay closure above — dispatch by captured prim type.
                    if (g270LineType(static_cast<uint32_t>(e.prim.type)))
                        self->drawLine(&s_seq);
                    else if (e.prim.type == GS_PRIM_SPRITE)
                        self->drawSprite(&s_seq);
                    else
                        self->drawTriangle(&s_seq);
                }
                t_g149Px = nullptr; t_g149Valid = nullptr;
                t_g258EntryIndex = static_cast<size_t>(-1);
                t_g144TitleRockScopeOverride = false;
                t_g144TownDepthScopeOverride = false;
                t_g144InReplay = false;
                g242NoteCpuReplayWrites(g_g144LastGs->m_vram);
                // G261: the wave arm disables the G260 pre-execution RTT generation bump (it
                // would poison GPU-resident targets); publish the replay's RTT writes here,
                // after the bytes actually changed.
                g261NoteCpuRttWrites(g_g144LastGs->m_vram);
                g286NoteCpuTransientWrites(g_g144LastGs->m_vram);
                g255CompareCpuVerify(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize);
                g_g144List.clear();
            };
        }

        const uint32_t pt144 = static_cast<uint32_t>(gs->m_prim.type);
        const bool isTri144 = (pt144 == GS_PRIM_TRIANGLE || pt144 == GS_PRIM_TRISTRIP || pt144 == GS_PRIM_TRIFAN);
        const bool isLine144 = g270LineType(pt144);
        // G172: G171 found that EVERY sprite draw forced a full mid-frame flush of the pending
        // triangle list (the "non-deferrable primitive" drain below) before rasterizing inline —
        // with ~100+ sprites/frame (2D title UI: logo/press-start/menu overlays, all alpha-blended
        // to fbp=0x0) this fragmented one large deferred batch into 100+ tiny ones, paying the
        // parallel-dispatch overhead that many times over (measured ~76-79ms/frame, ~99% of the
        // whole packed-register dispatch cost). Widen the SAME proven defer/band-replay path to
        // GS_PRIM_SPRITE at the same display-framebuffer gate, so sprites replay in the same
        // band-parallel pass as triangles instead of each forcing their own flush.
        // G144 tile-bin is itself default-ON since G168. G172 sprite-widening (root-caused +
        // fixed at G173, dungeon-3D soak clean at G187) is promoted to DEFAULT-ON here too —
        // kill via DC2_G172_NO_SPRITE_DEFER=1 or DC2_G172_SPRITE_DEFER=0.
        // G178: GPU mode needs sprites in the deferred list too (an inline sprite would blend
        // against a VRAM framebuffer the GPU owns mid-frame) — force the proven G172/G173 sprite
        // deferral on whenever the GPU lever is on (redundant now that it's default-on, kept for
        // the kill-switch-off + GPU-on combination).
        static const bool s_g172SpriteDefer =
            (!envFlagEnabled("DC2_G172_NO_SPRITE_DEFER") && !envFlagDisabled("DC2_G172_SPRITE_DEFER")) ||
            g178GpuOn();
        const bool isSprite144 = s_g172SpriteDefer && (pt144 == GS_PRIM_SPRITE);
        const bool diagOff144 = !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() && !f50_12_trace_enabled();
        // Deferrable by default = a triangle/sprite to a DISPLAY framebuffer (0x0/0x68), never
        // RTT or any diagnostic build. G252 below adds a narrow default-off RTT experiment.
        // Triangles still require textured (tme) as before; sprites defer whether textured or a
        // solid fill (both are equally safe to replay — writePixel/sampleTexture are unchanged).
        // G178: untextured triangles are as replay-safe as untextured sprites (drawTriangle is
        // unchanged) — widen the tme gate under the GPU lever so display-fbp shade-only tris
        // don't rasterize inline against a framebuffer the GPU owns mid-frame.
        // G252 (DEFAULT ON since G259): CPU band replay for the five measured RTT sprite targets.
        // G259 revalidated a material, repeatable MAP-0 win on current source (control 1.957 fps ->
        // G252 2.297 fps steady, 3 reps/arm, +17.4% FPS / -14.7% frame time, arms non-overlapping
        // and far outside run-to-run variance), with the full G253 route matrix (~199M texels,
        // bad=0), the exact golden title frame (211646), and an 896-frame dense title temporal gate
        // (all 211642..211650, no dropout) clean. Promoted default-on. Kill switch
        // DC2_G252_NO_RTT_DEFER=1 restores inline RTT rasterization. Target switches and every
        // upload/local-copy drain this list, preserving render-target write -> later texture-read
        // order. GPU RTT (DC2_G252_GPU_RTT / G255/G256) remains separately disabled.
        static const bool s_g252RttDefer =
            !envFlagEnabled("DC2_G252_NO_RTT_DEFER") || g255GpuRttOn();
        const bool g252RttTarget =
            s_g252RttDefer && isSprite144 && g248TargetIndex(ctx.frame.fbp) >= 0;
        // G286 admits only the measured opaque textured full-screen sprite class. Other 0x13b
        // shapes remain on the proven CPU path instead of silently widening shader semantics.
        const bool g286Transient =
            g286TransientTarget(ctx.frame.fbp) && diagOff144 && isSprite144 &&
            gs->m_prim.tme && !gs->m_prim.abe;
        // G260 (opt-in): under the native-renderer command graph, RTT-family TRIANGLES (both
        // textured and untextured — the census shows both) become recordable too, closing the
        // G247 structural exclusion that rasterized them inline scalar on the GS thread. They
        // join the same per-target batches as the G252 RTT sprites and execute through the same
        // banded replay, in submission order. Without DC2_G260_NR this bool is constant false
        // and the shipped eligibility is bit-identical.
        static const bool s_g260NoRttTri = envFlagEnabled("DC2_G260_NO_RTTTRI"); // bisect lever
        const bool g260RttTri = g260NrOn() && !s_g260NoRttTri && isTri144 && diagOff144 &&
                                g248TargetIndex(ctx.frame.fbp) >= 0;
        const bool g270LineWave = g270LineWaveOn() && isLine144 && !gs->m_prim.tme &&
                                  diagOff144 &&
                                  (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u);
        const bool deferrable144 = ((isTri144 || isSprite144) && diagOff144 &&
                                    (isTri144 ? (gs->m_prim.tme || g178GpuOn()) : true) &&
                                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u ||
                                      g252RttTarget)) ||
                                   g260RttTri || g270LineWave || g286Transient;
        // G156: count deferred vs inline tris + the non-deferrable reason (see g156Report). Report
        // fires from HERE (every tri) so a line prints even if 100% defer (inline=0) or 100% inline.
        if (g_g156Stat && isTri144)
        {
            if (deferrable144)
                g_g156DeferredTri.fetch_add(1u, std::memory_order_relaxed);
            else
            {
                g_g156InlineTri.fetch_add(1u, std::memory_order_relaxed);
                if (!gs->m_prim.tme)
                    g_g156NoTme.fetch_add(1u, std::memory_order_relaxed);
                else if (ctx.frame.fbp != 0x0u && ctx.frame.fbp != 0x68u)
                    g_g156BadFbp.fetch_add(1u, std::memory_order_relaxed);
                else
                    g_g156Diag.fetch_add(1u, std::memory_order_relaxed);
            }
            static std::atomic<uint64_t> s_g156Tick{0};
            if ((s_g156Tick.fetch_add(1u, std::memory_order_relaxed) % 8000u) == 0u)
                g156Report();
        }
        // DC2_G144_SEQ: route mid-frame flushes through the SEQUENTIAL (no-pool) path too — isolates
        // deferral/ordering correctness from the parallel band logic.
        static const bool s_g144Seq = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &g144DoFlush = s_g144Seq ? g_g144FlushSeqFn : g_g144FlushFn;
        auto g144TimedMidFlush = [&]() {
            G146PerfScope g146FlushScope(g_g146G144FlushMidNs, g_g146G144FlushMidCount);
            g144DoFlush();
        };
        if (deferrable144)
        {
            if (g286Transient)
                g_g286Captured.fetch_add(1u, std::memory_order_relaxed);
            const std::chrono::steady_clock::time_point g156CapT0 =
                g_g156Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (g_g156Stat)
                g_g156PrologueNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(g156CapT0 - g156FnT0)
                        .count(),
                    std::memory_order_relaxed);
            // G266: a same-fbp FBW switch is a batch boundary too (see g266FbwSplitOn). The
            // legacy default path (split off) keeps the historical fbp-only condition — its CPU
            // replay handles mixed widths and the default must stay bit-identical.
            const bool g266FbwSwitch = g266FbwSplitOn() && !g_g144List.empty() &&
                                       ctx.frame.fbw != g_g144List.front().ctx.frame.fbw;
            if (!g_g144List.empty() && (ctx.frame.fbp != g_g144BlockFbp || g266FbwSwitch))
            {
                if (g254DependencyStatEnabled())
                {
                    G144Entry candidate{};
                    candidate.ctx = ctx;
                    candidate.prim = gs->m_prim;
                    candidate.texclut = gs->m_texclut;
                    const uint32_t depMask = g254DependencyMask(candidate);
                    g254NoteSwitch(depMask, g_g144BlockFbp, ctx.frame.fbp, g_g144List.size());
                }
                g253NoteMidBarrier(true, g_g144List.front().ctx.frame.fbp,
                                   ctx.frame.fbp, g_g144List.size(), pt144,
                                   gs->m_prim.tme, gs->m_prim.abe);
                // G260: a target switch is a batch boundary, not an execution point. Close the
                // open batch into the frame graph (O(1) move) and keep recording; the graph
                // executes at the next real dependency edge or the frame boundary, batch by
                // batch in submission order. Legacy path (NR off): drain immediately as before.
                if (g260NrOn())
                {
                    g260CloseOpenBatch();
                    // DC2_G260_EAGER=1 (bisect lever): execute at every batch close — restores
                    // the immediate-drain cadence while keeping NR plumbing, isolating "delayed
                    // execution across upload edges" from "batch/replay plumbing".
                    static const bool s_g260Eager = envFlagEnabled("DC2_G260_EAGER");
                    if (s_g260Eager)
                        g260ExecuteAll(kG260Legacy, true);
                    else if (g_g260Graph.size() > 96u)
                        g260ExecuteAll(kG260Runaway, true); // fail-safe cap, keeps memory bounded
                }
                else
                {
                    g144TimedMidFlush(); // fbp changed → different target; drain before switching blocks
                }
            }
            g_g144BlockFbp = ctx.frame.fbp;
            // Screen bbox rows computed EXACTLY as drawTriangle/drawSprite (same ofy + scissor
            // clamp) so the band prefilter is tight and never narrower than the real coverage.
            const GSVertex &cv0 = gs->m_vtxQueue[0];
            const GSVertex &cv1 = gs->m_vtxQueue[1];
            const int cofy = ctx.xyoffset.ofy >> 4;
            int crawMinY, crawMaxY;
            G144Entry e;
            if (isSprite144)
            {
                // Sprite bbox: 2-vertex unclipped rect, min/max SWAPPED (drawSprite's own formula),
                // not the triangle 3-way min/max.
                const float sfy0 = cv0.y - static_cast<float>(cofy);
                const float sfy1 = cv1.y - static_cast<float>(cofy);
                crawMinY = static_cast<int>(std::floor(std::min(sfy0, sfy1)));
                crawMaxY = static_cast<int>(std::ceil(std::max(sfy0, sfy1)));
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = GSVertex{};
                // G172: hoist drawSprite()'s "looksLikeDisplayCopy" detection to capture time and
                // apply it to the LIVE gs immediately (same instant it would fire inline) — this
                // mutates gs->m_hasPreferredDisplaySource/m_preferredDisplaySourceFrame/
                // m_preferredDisplayDestFbp, read later by latchHostPresentationFrame(). Duplicated
                // (not shared) so the already-proven inline drawSprite() path is never touched;
                // the replay's own drawSprite() call redundantly recomputes it on the throwaway
                // scratch GS, which is harmless (never read back).
                const int sox = ctx.xyoffset.ofx >> 4;
                int sx0 = static_cast<int>(cv0.x) - sox;
                int sx1 = static_cast<int>(cv1.x) - sox;
                int sy0 = static_cast<int>(cv0.y) - cofy;
                int sy1 = static_cast<int>(cv1.y) - cofy;
                if (sx0 > sx1) std::swap(sx0, sx1);
                if (sy0 > sy1) std::swap(sy0, sy1);
                const uint64_t alphaReg = ctx.alpha;
                const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
                const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
                const bool looksLikeDisplayCopy144 =
                    gs->m_prim.tme && gs->m_prim.abe && gs->m_prim.fst && gs->m_prim.ctxt &&
                    ctx.frame.fbp != ctx.tex0.tbp0 &&
                    alphaMode == 0x64u && (alphaFix == 0x60u || alphaFix == 0x80u) &&
                    sx0 <= 0 && sy0 <= 0 && sx1 >= 639 && sy1 >= 447;
                if (looksLikeDisplayCopy144)
                {
                    gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
                    gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
                    gs->m_hasPreferredDisplaySource = true;
                }
            }
            else if (isLine144)
            {
                // drawLine truncates the already pixel-unit vertex coordinates before applying
                // XYOFFSET. Preserve that exact row range for band prefiltering and replay.
                const int ly0 = static_cast<int>(cv0.y) - cofy;
                const int ly1 = static_cast<int>(cv1.y) - cofy;
                crawMinY = std::min(ly0, ly1);
                crawMaxY = std::max(ly0, ly1);
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = GSVertex{};
                g270NoteCapture(cv0, cv1);
            }
            else
            {
                const GSVertex &cv2 = gs->m_vtxQueue[2];
                const float cfy0 = cv0.y - static_cast<float>(cofy);
                const float cfy1 = cv1.y - static_cast<float>(cofy);
                const float cfy2 = cv2.y - static_cast<float>(cofy);
                crawMinY = static_cast<int>(std::floor(std::min({cfy0, cfy1, cfy2})));
                crawMaxY = static_cast<int>(std::ceil(std::max({cfy0, cfy1, cfy2})));
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = cv2;
            }
            e.ctx = ctx;
            e.prim = gs->m_prim;
            e.texa = gs->m_texa;
            e.texclut = gs->m_texclut;
            e.pabe = gs->m_pabe;
            e.titleRockScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
            e.townDepthScope = g_dc2TownDepthScope.load(std::memory_order_relaxed);
            e.minY = clampInt(crawMinY, ctx.scissor.y0, ctx.scissor.y1);
            e.maxY = clampInt(crawMaxY, ctx.scissor.y0, ctx.scissor.y1);
            // G149: decode this tri's texture ONCE now (guest thread), so all band lanes read it
            // lock-free at replay. Null unless texcache-enabled + eligible. Sprites never use the
            // G149 texcache hook (drawSprite() samples VRAM directly, same as its inline path).
            e.decoded = isTri144 ? g149BuildDecoded(this, gs, ctx, gs->m_texa, gs->m_vram) : G149Buf{};
            if (isTri144)
                g161CensusEligibility(ctx, gs->m_prim.abe);
            // G219 (default-off, DC2_G219_SKYPX=1): fingerprint the 0x2720 work-page range at
            // CAPTURE time for the display composite that samples it — paired with the identical
            // fingerprint at raster time in drawTriangle, this discriminates "VRAM clobbered
            // between capture and flush" from "sampler reads a different address in replay".
            {
                static const bool s_g219f = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219f && isTri144 && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u &&
                    gs->m_vram && 0x272000u + 0x8000u <= gs->m_vramSize)
                {
                    static std::atomic<uint32_t> s_g219fn{0};
                    const uint32_t n = s_g219fn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u)
                    {
                        uint64_t sum = 0;
                        const uint64_t *p = reinterpret_cast<const uint64_t *>(gs->m_vram + 0x272000u);
                        for (uint32_t i = 0; i < 0x1000u; ++i)
                            sum += p[i];
                        std::fprintf(stderr, "[G219:cap] n=%u fbp=0x%x sum=%016llx\n",
                                     n, ctx.frame.fbp, (unsigned long long)sum);
                        extern uint8_t g_g219Snap[0x8000];
                        std::memcpy(g_g219Snap, gs->m_vram + 0x272000u, 0x8000u);
                    }
                }
            }
            if (g260NrOn())
                g260NoteAppend(e); // accumulate this entry's R/W block ranges (open batch)
            g_g144List.push_back(e);
            if (g_g156Stat)
                g_g156CaptureNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g156CapT0)
                        .count(),
                    std::memory_order_relaxed);
            return; // deferred — rasterized later at flush
        }
        // Non-deferrable primitive (line/point/untextured-off-target/RTT sprite/etc — anything that
        // didn't satisfy deferrable144 above, e.g. a sprite/tri to a non-display fbp like 0x139):
        // drain the pending list FIRST so global submission order (3D under the 2D menu) is
        // preserved, then draw this prim normally.
        // G219 (default-off, DC2_G219_SKYPX=1): log the non-deferrable drains around the 0x139
        // work-page cycle so the copy/composite/clear interleave is visible in the trace.
        {
            static const bool s_g219d = (std::getenv("DC2_G219_SKYPX") != nullptr);
            if (s_g219d && ctx.frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g219dn{0};
                const uint32_t n = s_g219dn.fetch_add(1u, std::memory_order_relaxed);
                if (n < 2000u)
                    std::fprintf(stderr,
                        "[G219:nondef] n=%u prim=%u tme=%u fbp=0x%x tbp=0x%x list=%zu\n",
                        n, static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(gs->m_prim.tme), ctx.frame.fbp, ctx.tex0.tbp0,
                        g_g144List.size());
            }
        }
        if (g260NrOn())
        {
            // G260: an inline (non-recordable) primitive is about to read/write VRAM directly.
            // Execute the whole pending graph first so it observes the same VRAM evolution
            // order as the immediate-drain baseline. Covers both the open batch and any closed
            // batches (the legacy branch below only ever has the open list).
            if (g260HasPending())
            {
                if (!g_g144List.empty())
                    g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                                       ctx.frame.fbp, g_g144List.size(), pt144,
                                       gs->m_prim.tme, gs->m_prim.abe);
                g260ExecuteAll(kG260NonDef, true);
            }
        }
        else if (!g_g144List.empty())
        {
            g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                               ctx.frame.fbp, g_g144List.size(), pt144,
                               gs->m_prim.tme, gs->m_prim.abe);
            g144TimedMidFlush();
        }
        // This primitive will execute on the CPU (RTT/line/point/diagnostic path).  The preceding
        // drain may have left guest Z resident on the GPU, so restore authoritative bytes before
        // any CPU depth test, texture sample, or Z write.
        // G261: same for GPU-resident RTT rows — materialize any resident target this
        // primitive's frame/Z/texture/CLUT pages touch before the CPU reads or writes them.
        // G278 range-exact edge: culled or disjoint inline primitives do not break a same-key Z
        // run; real target/Z/texture/CLUT aliases publish before resident color materialization.
        (void)g278FlushPendingDepthForInline(
            gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        (void)g278FlushPendingDepthForDisplayOwners(4);
        const G289InlineAudit g289Inline =
            s_g289Owner.active
                ? g289AuditInline(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut)
                : G289InlineAudit{};
        if (s_g289Owner.active && g289StatOn())
        {
            static std::atomic<uint32_t> s_g289InlineLog{0};
            const uint32_t n = s_g289InlineLog.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(
                    stderr,
                    "[G289:inline-edge] n=%u owner=%x..%x carry=%u empty=%u unknown=%u "
                    "prim=%u tme=%u abe=%u fst=%u "
                    "frame=%03x/%u/%x sc=%u,%u..%u,%u tex=%x/%u/%x/%u/%u "
                    "zbuf=%llx test=%llx box=%d,%d..%d,%d "
                    "ranges(c=%x..%x z=%x..%x t=%x..%x p=%x..%x) "
                    "v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                    n + 1u, s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock,
                    g289Inline.carry ? 1u : 0u, g289Inline.empty ? 1u : 0u,
                    g289Inline.unknown ? 1u : 0u,
                    static_cast<unsigned>(gs->m_prim.type), gs->m_prim.tme ? 1u : 0u,
                    gs->m_prim.abe ? 1u : 0u, gs->m_prim.fst ? 1u : 0u,
                    ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw), ctx.frame.psm,
                    ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                    ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), ctx.tex0.psm,
                    static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                    static_cast<unsigned long long>(ctx.zbuf),
                    static_cast<unsigned long long>(ctx.test),
                    g289Inline.x0, g289Inline.y0, g289Inline.x1, g289Inline.y1,
                    g289Inline.color.first, g289Inline.color.last,
                    g289Inline.depth.first, g289Inline.depth.last,
                    g289Inline.texture.first, g289Inline.texture.last,
                    g289Inline.clut.first, g289Inline.clut.last,
                    gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                    gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                    gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y);
        }
        if (g289Inline.carry)
            g_g289InlineCarries.fetch_add(1u, std::memory_order_relaxed);
        else
            (void)g289MaterializeOwner(4);
        g272MaterializeAll(4); // inline CPU primitive: conservative display-color consumer edge
        g277CensusInlineEdge(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        g276FlushPendingDisplay(4); // G276: publish deferred display readbacks first
        g261PrepareInline(ctx, gs->m_prim, gs->m_texclut);
        g242PrepareVramReadAll(gs->m_vram);
    }

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        // G156: time inline (non-deferred) tri raster on the calling thread. Under MTGS+G144 only
        // NON-deferrable tris reach here (deferrable ones returned early), so inlineMs pairs with
        // g_g156InlineTri. Replay calls drawTriangle directly (never this switch) → no double count.
        if (g_g156Stat && !t_g144InReplay)
        {
            const auto g156T0 = std::chrono::steady_clock::now();
            drawTriangle(gs);
            g_g156InlineTriNs.fetch_add(
                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g156T0)
                    .count(),
                std::memory_order_relaxed);
        }
        else
            drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixel(gs, px, py, v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }

    // G178: an INLINE (non-deferred) draw just wrote guest VRAM directly (e.g. the costume RTT at
    // fbp=0x139). Bump the write generations of the pages its scissored frame region can touch so
    // any GPU-resident texture decoded from those pages (RTT-as-texture) is re-decoded, not stale.
    // Conservative (scissor rect ⊇ written pixels); a no-op single boolean read when the lever is
    // off, and skipped during replay (replay is the GPU path's own CPU fallback, whose target is
    // the display fb the front-end re-uploads unconditionally anyway).
    if (g178GpuOn() && !t_g144InReplay)
    {
        const auto &ctx178 = gs->activeContext();
        // G277-R: bump the prim's CLAMPED bbox ∩ scissor, not the whole scissor rect. Writes are
        // confined to that intersection by construction, so this stays conservative — but a fully
        // scissored-out prim (e.g. the offscreen sceGsSwapDBuff sync linestrips at x≈1792..2303)
        // rasterizes NOTHING and must bump NOTHING. The old whole-scissor bump invalidated the
        // full display target ~1×/frame per buffer, forcing a spurious fb re-upload every batch
        // (and, under G276 deferral, the stale-VRAM clobber behind the MAP-0 Max-body flicker).
        // Kill (restores whole-scissor bump): DC2_G277_NO_INLINE_BBOX_BUMP=1.
        static const bool s_g277BboxBump = !envFlagEnabled("DC2_G277_NO_INLINE_BBOX_BUMP");
        int bx0 = ctx178.scissor.x0, bx1 = ctx178.scissor.x1;
        int by0 = ctx178.scissor.y0, by1 = ctx178.scissor.y1;
        bool bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        if (s_g277BboxBump && !bumpEmpty)
        {
            int nv = 3;
            switch (gs->m_prim.type)
            {
            case GS_PRIM_POINT: nv = 1; break;
            case GS_PRIM_LINE:
            case GS_PRIM_LINESTRIP:
            case GS_PRIM_SPRITE: nv = 2; break;
            default: nv = 3; break;
            }
            const int ofx178 = ctx178.xyoffset.ofx >> 4;
            const int ofy178 = ctx178.xyoffset.ofy >> 4;
            float mnx = gs->m_vtxQueue[0].x, mxx = gs->m_vtxQueue[0].x;
            float mny = gs->m_vtxQueue[0].y, mxy = gs->m_vtxQueue[0].y;
            for (int vi = 1; vi < nv; ++vi)
            {
                mnx = std::min(mnx, gs->m_vtxQueue[vi].x);
                mxx = std::max(mxx, gs->m_vtxQueue[vi].x);
                mny = std::min(mny, gs->m_vtxQueue[vi].y);
                mxy = std::max(mxy, gs->m_vtxQueue[vi].y);
            }
            bx0 = std::max(bx0, static_cast<int>(std::floor(mnx)) - ofx178);
            bx1 = std::min(bx1, static_cast<int>(std::ceil(mxx)) - ofx178);
            by0 = std::max(by0, static_cast<int>(std::floor(mny)) - ofy178);
            by1 = std::min(by1, static_cast<int>(std::ceil(mxy)) - ofy178);
            bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        }
        if (!bumpEmpty)
        {
            g178NoteVramWriteRect(gs->m_vram,
                                  GSInternal::framePageBaseToBlock(ctx178.frame.fbp),
                                  ctx178.frame.fbw, ctx178.frame.psm,
                                  static_cast<uint32_t>(std::max(bx0, 0)),
                                  static_cast<uint32_t>(std::max(by0, 0)),
                                  static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                  static_cast<uint32_t>(by1 - std::max(by0, 0) + 1));
            if (((ctx178.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRect(gs->m_vram,
                                      static_cast<uint32_t>(ctx178.zbuf & 0x1FFu) << 5u,
                                      ctx178.frame.fbw,
                                      static_cast<uint32_t>((ctx178.zbuf >> 24u) & 0xFu),
                                      static_cast<uint32_t>(std::max(bx0, 0)),
                                      static_cast<uint32_t>(std::max(by0, 0)),
                                      static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                      static_cast<uint32_t>(by1 - std::max(by0, 0) + 1));
        }
    }
}