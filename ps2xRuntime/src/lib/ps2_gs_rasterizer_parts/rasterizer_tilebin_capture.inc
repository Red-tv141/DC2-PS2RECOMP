    // G458: exact empty-work rejection.  Keep this after the stateful prologue in the preceding
    // part, but before any capture, target-switch flush, ownership edge, or inline raster work.
    // DC2_G458_STAT=1 enables cumulative per-type counts without timing every draw.
    const bool g458MissesScissor = g458CullClippedOn() && [&]() {
        int vertexCount = 3;
        switch (gs->m_prim.type)
        {
        case GS_PRIM_POINT: vertexCount = 1; break;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
        case GS_PRIM_SPRITE: vertexCount = 2; break;
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN: vertexCount = 3; break;
        default: return false;
        }

        float minX = gs->m_vtxQueue[0].x;
        float maxX = minX;
        float minY = gs->m_vtxQueue[0].y;
        float maxY = minY;
        for (int i = 1; i < vertexCount; ++i)
        {
            minX = std::min(minX, gs->m_vtxQueue[i].x);
            maxX = std::max(maxX, gs->m_vtxQueue[i].x);
            minY = std::min(minY, gs->m_vtxQueue[i].y);
            maxY = std::max(maxY, gs->m_vtxQueue[i].y);
        }

        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        // Expand outward before rejecting.  This is deliberately more conservative than each
        // leaf rasterizer: it also covers line/point integer truncation and the legacy sprite-edge
        // rollback, so no sub-pixel primitive that could reach the boundary pixel is removed.
        const float cx = maxX - ofx;
        const float fx = minX - ofx;
        const float cy = maxY - ofy;
        const float fy = minY - ofy;
        const float sx0 = static_cast<float>(ctx.scissor.x0);
        const float sx1 = static_cast<float>(ctx.scissor.x1);
        const float sy0 = static_cast<float>(ctx.scissor.y0);
        const float sy1 = static_cast<float>(ctx.scissor.y1);

        // G519: the four rounding calls above are the only reason this cull touches the CRT.
        // `ceilf(a) < b` and `floorf(a) > b` are decided WITHOUT rounding whenever b is an exact
        // integer, which every GSScissorReg field is (uint16_t). See kG519LeverCullCmp in
        // g419_ab_instrument.inc for the proof, including the NaN/+-Inf cases.
        // Gated on the arm VALUE (G504): -1 = not under the A/B, 0 = control (CRT), 1 = candidate.
        // No `extern` redeclaration here on purpose: all four are DEFINED earlier in this same TU
        // (g419_ab_instrument.inc, include line 145 vs this file's 160), so an unqualified call
        // resolves by ordinary lookup whatever their linkage is. A block-scope `extern` would
        // instead assert GLOBAL linkage and break if the definitions ever move into the anonymous
        // namespace this TU opens (the appendix §3 anon-namespace linkage trap).
        const int g519Arm = g519CullCmpArm();
        const bool g519UseCmp = (g519Arm >= 0) ? (g519Arm == 1) : g519CullCmpDefaultOn();

        if (g519UseCmp)
        {
            const bool cmpForm = cx <= sx0 - 1.0f || fx >= sx1 + 1.0f ||
                                 cy <= sy0 - 1.0f || fy >= sy1 + 1.0f;
            // The oracle recomputes the CRT form, so it must live OUT OF LINE: leaving it inline
            // put four never-executed ceilf/floorf CALL SITES inside the candidate's own basic
            // block — exactly the work this lever exists to delete — and the first G519 gate paid
            // for it (+0.51/+0.71 ms/f, blocked +0.68). G490's law: a never-selected copy costs
            // real ms/f. g519VerifyCull is __declspec(noinline) for the same reason.
            if (g519CullVerifyOn())
                g519VerifyCull(cx, fx, cy, fy, sx0, sx1, sy0, sy1, cmpForm);
            return cmpForm;
        }
        return std::ceil(cx) < sx0 || std::floor(fx) > sx1 ||
               std::ceil(cy) < sy0 || std::floor(fy) > sy1;
    }();
    if (g458MissesScissor)
    {
        if (g458CullStatOn())
        {
            static std::atomic<uint64_t> s_culled[7]{};
            static std::atomic<uint64_t> s_total{0};
            const uint32_t type = static_cast<uint32_t>(gs->m_prim.type);
            if (type < 7u)
                s_culled[type].fetch_add(1u, std::memory_order_relaxed);
            const uint64_t n = s_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((n & 0x1FFFull) == 0u)
                std::fprintf(stderr,
                             "[G458:cull] total=%llu type(0..6)=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(s_culled[0].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[1].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[2].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[3].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[4].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[5].load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_culled[6].load(std::memory_order_relaxed)));
        }
        g526SetExit(kG526Cull); // G526 exit 1
        return;
    }

    // G459: G458 rejects only primitives whose bounding box misses SCISSOR. A diagonal triangle
    // can overlap that box while its complete area remains outside. Test the remaining triangle
    // against the rectangle's two axes (already covered by G458) and the three triangle-edge
    // normals. The rectangle is expanded one pixel on every side: normal triangles sample at
    // x/y+0.5, G408 shadow triangles sample at integer coordinates, and the legacy barycentric
    // tolerance expands an edge by <1 pixel over the GS coordinate range. Strict separation from
    // this larger rectangle therefore cannot remove a pixel accepted by any current leaf.
    const bool g459Stat = g459TriScissorStatOn();
    const bool g459Triangle =
        gs->m_prim.type == GS_PRIM_TRIANGLE ||
        gs->m_prim.type == GS_PRIM_TRISTRIP ||
        gs->m_prim.type == GS_PRIM_TRIFAN;
    const bool g459Separated = g459Stat && g459Triangle && [&]() {
        double x[3], y[3];
        const double ofx = static_cast<double>(ctx.xyoffset.ofx >> 4);
        const double ofy = static_cast<double>(ctx.xyoffset.ofy >> 4);
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &v = gs->m_vtxQueue[i];
            // Custom near-plane handling in drawTriangle may replace mixed/behind vertices.
            // Fail closed unless all vertices are ordinary finite, positive-Q inputs.
            if (!std::isfinite(v.x) || !std::isfinite(v.y) ||
                !std::isfinite(v.q) || v.q <= 0.0f)
                return false;
            x[i] = static_cast<double>(v.x) - ofx;
            y[i] = static_cast<double>(v.y) - ofy;
        }

        const double rx0 = static_cast<double>(ctx.scissor.x0) - 1.0;
        const double rx1 = static_cast<double>(ctx.scissor.x1) + 1.0;
        const double ry0 = static_cast<double>(ctx.scissor.y0) - 1.0;
        const double ry1 = static_cast<double>(ctx.scissor.y1) + 1.0;
        auto edgeSeparates = [&](int a, int b, int inside) {
            const double dx = x[b] - x[a];
            const double dy = y[b] - y[a];
            const double interior = dx * (y[inside] - y[a]) - dy * (x[inside] - x[a]);
            if (interior == 0.0)
                return false; // degenerate edge/triangle: legacy leaf decides

            // E(p) = (-dy)*px + dx*py + constant. Select rectangle extrema without four
            // corner evaluations; strict opposite-side separation is the SAT rejection.
            const double cx = -dy;
            const double cy = dx;
            const double c = dy * x[a] - dx * y[a];
            const double minE = c + cx * (cx >= 0.0 ? rx0 : rx1) +
                                      cy * (cy >= 0.0 ? ry0 : ry1);
            const double maxE = c + cx * (cx >= 0.0 ? rx1 : rx0) +
                                      cy * (cy >= 0.0 ? ry1 : ry0);
            return interior > 0.0 ? maxE < 0.0 : minE > 0.0;
        };
        return edgeSeparates(0, 1, 2) ||
               edgeSeparates(1, 2, 0) ||
               edgeSeparates(2, 0, 1);
    }();

    if (g459Stat && g459Triangle)
    {
        static std::atomic<uint64_t> s_tested{0}, s_separated{0};
        const uint64_t tested = s_tested.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (g459Separated)
            s_separated.fetch_add(1u, std::memory_order_relaxed);
        if ((tested & 0x7FFFull) == 0u)
            std::fprintf(stderr, "[G459:sat] tested=%llu separated=%llu\n",
                         static_cast<unsigned long long>(tested),
                         static_cast<unsigned long long>(
                             s_separated.load(std::memory_order_relaxed)));
    }
    g595Lap(kG595Cull); // G595: the G458 scissor cull + the G459 separation stat

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
                g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List);
                g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "pre");
                // G178: whole-flush GPU attempt (default OFF, DC2_G178_GPU=1). On success the
                // batch is already rendered AND read back into VRAM — skip the CPU replay
                // entirely. On ANY unsupported entry it returns false having changed nothing,
                // and the proven CPU band replay below runs for the whole flush.
#if defined(PS2X_G657_DIAG)
                // G657 P4: the accepted-GPU half of the coverage metric. Paired with the fallback
                // lap below, these two are the only two exits from this decision, so their batch /
                // entry / wall totals partition the whole raster population by construction.
                const bool g657On = g657WallOn();
                const std::chrono::steady_clock::time_point g657T0 =
                    g657On ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
                const size_t g657Entries = g_g144List.size();
                const uint32_t g657Fbp = g_g144List.empty() ? 0u : g_g144List[0].ctx.frame.fbp;
#endif
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
#if defined(PS2X_G657_DIAG)
                    if (g657On)
                        g657NoteGpuOk(g528ClassifyFbp(g657Fbp), g657Fbp,
                                      static_cast<unsigned long long>(g657Entries),
                                      static_cast<unsigned long long>(
                                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - g657T0).count()));
#endif
                    g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List, "post-gpu");
                    g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "post-gpu");
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
                // G583: rank the CPU-fallback population by REPLAYED ENTRIES. Deliberately NOT
                // gated on g290P — the entry weight is the whole point and must not require the
                // probe's per-flush clock pair to be armed.
                g583NoteFallback(g_g144List);
#if defined(PS2X_G662_DIAG)
                // G662 P10: THIS is the CPU band-replay choke point — g178TryFlushGpu has already
                // returned false, so every entry in g_g144List is about to replay on the CPU, and
                // each passes here exactly once. An entry that writes the G403/G411 private depth
                // mirror is therefore precisely a CPU writer of it, which is the one fact that
                // decides whether bounded GPU-exclusive depth ownership is available at all.
                // ⛔ G661 asked the same question from inside g261PrepareCpuReplay, which is
                // reached only after `if (!g261WaveOn() || !g261AnyDirty() || g_g144List.empty())
                // return;` — so it saw entries only in flushes where a resident target was ALREADY
                // dirty, and reached cpuEntriesSeen=1 on `combat` and `ridepod`. Its own fix-log
                // named this site as the correction; this is it.
                for (const G144Entry &g662e : g_g144List)
                    g662NoteCpuReplayEntry(g662e.ctx.frame.fbp, g662e.ctx.zbuf);
#endif
#if defined(PS2X_G657_DIAG)
                // G657 P3: charge the WHOLE CPU-fallback tail - publication prologue, band replay,
                // and the post-replay notes - to the reject family that sent the batch here. The
                // destructor fires at the end of this closure, i.e. after the last write, so the
                // reason and the wall it caused are one fact rather than two joined after the run.
                // ⛔ CONSTRUCTED HERE, NOT LOWER DOWN, because `g603NoteFallbackAdmission` below
                // CLOBBERS `g_g266ClassifySite` (its own comment says so, which is why G583 reads
                // it first). Same ordering hazard, same fix. Rule 17 still applies to the run
                // itself: DC2_G603_ADMIT doubles the classification work per fallback entry, so it
                // changes the population and must never share a process with DC2_G657_REJWALL.
                const G657WallLap g657Lap(g657On, static_cast<unsigned>(g_g266FlushRej & 0xF),
                                          static_cast<unsigned>(g_g266ClassifySite & 0xF),
                                          g528ClassifyFbp(g657Fbp), g657Fbp,
                                          static_cast<unsigned long long>(g657Entries));
#endif
                // G603 (default-off, DC2_G603_ADMIT=1): the same choke point, asking the question
                // G583's reason code cannot answer — WOULD the classifier admit these entries, and
                // if not, is the first failing conjunct the last one? Must follow g583NoteFallback:
                // the dry-run clobbers g_g266ClassifySite, which G583 reads.
                g603NoteFallbackAdmission(g_g144List);
                // G589: this batch leaves the pure-GPU path. The CPU band replay writes guest VRAM
                // and the private depth mirror through paths that publish no residency metadata,
                // so the drain memo's key cannot observe them. Fail CLOSED for the rest of the
                // transaction.
                g589DisarmTransaction();
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
                // The proven CPU replay and its decoded textures consume guest VRAM directly.
                // Publish any RTT rows left GPU-resident by a G261 wave (only where this list's
                // target/Z/texture/CLUT pages actually touch them), then any GPU-owned depth.
                // G528: the seven calls that used to sit inline here now live in g528FlushPrologue,
                // which runs them UNCHANGED by default and — under DC2_G528_SCOPEDPUB=1 — scopes the
                // four whole-state publications to this batch's own g_g528Rd/g_g528Wr page ranges.
                // Census DC2_G528_PUB=1, oracle DC2_G528_VERIFY=1, ceiling probe DC2_G528_NO_PUB=1.
                g528FlushPrologue(3, g_g144LastGs->m_vram, g_g144BlockFbp);
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
                // G529: `replay` (the bucket this whole tail is booked to by [G290:probe]) is
                // 986 us per batch for 3.4 sprite entries. Lap the three things inside it that are
                // not rasterization — the Z clears, the bbox scan, and the pool round trip — before
                // believing any of it is pixels. Census DC2_G529_STAT=1, lever DC2_G529_INLINEREPLAY=1.
                const bool g529On = g529StatOn();
                std::chrono::steady_clock::time_point g529T = g529On
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                uint64_t g529ZclrNs = 0u, g529ScanNs = 0u;
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
                if (g529On)
                {
                    const auto t = std::chrono::steady_clock::now();
                    g529ZclrNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     t - g529T).count();
                    g529T = t;
                }
                int y0 = 0x7fffffff, y1 = -0x7fffffff;
                // G529: the scissor-bounded row-span area, accumulated in the loop that already
                // walks every entry for the bbox. This is the quantity GSRowPool::run divides, so
                // it — not the entry COUNT — is what says whether a split can pay for itself.
                // Computed UNCONDITIONALLY so both A/B arms carry the identical per-entry work and
                // the gate measures only the lane decision (g520_arm_symmetry_is_a_codegen_property).
                uint64_t g529Px = 0u;
                for (const auto &e : g_g144List)
                {
                    if (e.minY < y0) y0 = e.minY;
                    if (e.maxY > y1) y1 = e.maxY;
                    const int r0 = (e.minY > (int)e.ctx.scissor.y0) ? e.minY : (int)e.ctx.scissor.y0;
                    const int r1 = (e.maxY < (int)e.ctx.scissor.y1) ? e.maxY : (int)e.ctx.scissor.y1;
                    if (r1 >= r0 && e.ctx.scissor.x1 >= e.ctx.scissor.x0)
                        g529Px += (uint64_t)(r1 - r0 + 1) *
                                  (uint64_t)(e.ctx.scissor.x1 - e.ctx.scissor.x0 + 1);
                }
                if (y1 >= y0)
                {
                    const uint64_t g529Rows = (uint64_t)(y1 - y0 + 1);
                    const size_t g529Entries = g_g144List.size();
                    const int g529Lanes = g529ReplayLanes(g529Rows, g529Px, g_g144Threads);
                    if (g529On)
                    {
                        const auto t = std::chrono::steady_clock::now();
                        g529ScanNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         t - g529T).count();
                        g529T = t;
                        g529BeginDispatch();
                    }
                    // G638b (no-op unless DC2_G638_PREP=1): lap the phases of THIS window. It is
                    // the same bracket `[G529:cls] wall` uses, so the laps sum to that wall and
                    // name the ~9.8 ms/f of `dragon`'s legacy dispatch that is not rasterization.
                    auto g638T = g_g638PrepCensusOn ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
                    const auto g638Lap = [&g638T](int phase) {
                        if (!g_g638PrepCensusOn)
                            return;
                        const auto now = std::chrono::steady_clock::now();
                        g638PrepNote(phase, static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - g638T).count()));
                        g638T = now;
                    };
                    GS *rgs = g_g144LastGs;
                    const bool g570Linear139 = g570Cpu139LinearBatchCandidate();
                    static const bool s_g570BatchStat =
                        envFlagEnabled("DC2_G570_BATCH_STAT");
                    static const bool s_g571Split =
                        envFlagEnabled("DC2_G571_SPLIT");
                    const auto g570PrepT = (s_g570BatchStat || s_g571Split)
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    const bool g570PreparedBatch = g570Batch139Prepare();
                    const uint64_t g570PrepUs = (s_g570BatchStat || s_g571Split)
                        ? static_cast<uint64_t>(std::chrono::duration_cast<
                              std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - g570PrepT).count())
                        : 0u;
                    const bool g570GpuBatch =
                        g570PreparedBatch && g570Batch139GpuOn();
                    {
                        if (s_g570BatchStat && !g_g144List.empty() &&
                            g_g144List.front().ctx.frame.fbp == 0x139u)
                        {
                            static std::atomic<uint64_t> s_total{0u}, s_fast{0u};
                            static std::atomic<uint64_t> s_entries{0u}, s_fastEntries{0u};
                            const uint64_t total = s_total.fetch_add(
                                1u, std::memory_order_relaxed) + 1u;
                            s_entries.fetch_add(g_g144List.size(), std::memory_order_relaxed);
                            if (g570PreparedBatch)
                            {
                                s_fast.fetch_add(1u, std::memory_order_relaxed);
                                s_fastEntries.fetch_add(g_g144List.size(),
                                                       std::memory_order_relaxed);
                            }
                            if (total <= 16u || (total % 20u) == 0u ||
                                !g570PreparedBatch || g570PrepUs >= 5000u)
                                std::fprintf(stderr,
                                    "[G570:batchstat] total=%llu fast=%llu entries=%llu "
                                    "fastEntries=%llu lastEntries=%zu admitted=%u reject=%d "
                                    "prep=%.3fms\n",
                                    static_cast<unsigned long long>(total),
                                    static_cast<unsigned long long>(s_fast.load(
                                        std::memory_order_relaxed)),
                                    static_cast<unsigned long long>(s_entries.load(
                                        std::memory_order_relaxed)),
                                    static_cast<unsigned long long>(s_fastEntries.load(
                                        std::memory_order_relaxed)),
                                    g_g144List.size(), g570PreparedBatch ? 1u : 0u,
                                    g570Batch139LastReject(),
                                    static_cast<double>(g570PrepUs) / 1000.0);
                        }
                    }
                    const bool g570Cpu139Batch =
                        g570Cpu139BatchOn() && !g_g144List.empty() &&
                        g_g144List.front().ctx.frame.fbp == 0x139u;
                    if (g570Cpu139Batch && !g570PreparedBatch)
                        g570Cpu139PrepareDepthTiles();
                    g638Lap(kG638PhG570Prep);
                    // G605: one palette-decode pass per batch, whichever leaf claims it. G580's
                    // single 0x13b shape wins when present (it is the narrower contract); otherwise
                    // G605's property-admitted family gets the same tables. They must never both
                    // run — the second call would `assign()` over the first's samples.
                    const auto g605ClutFill = [self, rgs](const G144Entry &e,
                                                          std::array<uint32_t, 256> &out) {
                        static thread_local GS s_clutGs;
                        s_clutGs.m_vram = rgs->m_vram;
                        s_clutGs.m_vramSize = rgs->m_vramSize;
                        s_clutGs.m_privRegs = rgs->m_privRegs;
                        s_clutGs.m_texa = e.texa;
                        s_clutGs.m_texclut = e.texclut;
                        for (uint32_t i = 0; i < 256u; ++i)
                            out[i] = self->lookupCLUT(
                                &s_clutGs, static_cast<uint8_t>(i), e.ctx.tex0.cbp,
                                e.ctx.tex0.cpsm, e.ctx.tex0.csm,
                                e.ctx.tex0.csa, e.ctx.tex0.psm);
                    };
                    const bool g580SharedClutBatch =
                        g580SharedClutBatchSelected(g_g144List) &&
                        g580PrepareSharedCluts(g_g144List, g605ClutFill);
                    const bool g605SharedClutBatch =
                        !g580SharedClutBatch &&
                        g605SharedClutBatchSelected(g_g144List) &&
                        g605PrepareSharedCluts(g_g144List, g605ClutFill);
                    const bool g605AnySharedClut = g580SharedClutBatch || g605SharedClutBatch;
                    g638Lap(kG638PhClut);
                    const auto g571ClearT = s_g571Split
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    // G629 (default-off, DC2_G629_STAT=1): its own clock, because the G529 census
                    // that already measures this wall is a SEPARATE arm and the two must be
                    // readable independently — arming G529 to get G629's denominator would put
                    // G529's per-lane clock reads inside the population G629 is pricing.
                    const bool g629On = g629StatOn();
                    const auto g629T0 = g629On ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                    // ⛔⛔⛔ G629 CEILING PROBE (DC2_G629_CEIL=1): skip the replay for every batch
                    // the GPU backend would claim. KNOWINGLY WRONG PIXELS — timing only, never
                    // promoted. It bounds every backend that could be built for this population
                    // (G511's law), which is why it is measured BEFORE the backend is written.
                    const bool g629Skip = g629CeilSkipBatch(g_g144List);
                    // G629 (default-off, DC2_G629_GPU=1): the exact GPU-resident batched replay.
                    // Returns true only when it has fully replayed the batch and committed it, so
                    // every refusal — an inadmissible entry, an unavailable backend, a GL error —
                    // simply leaves the untouched CPU pool below to run. Fail closed by
                    // construction: there is no path on which both arms write, and none on which
                    // neither does. Under DC2_G629_VERIFY=1 it deliberately returns FALSE after
                    // producing its candidate, so the CPU arm still writes the authoritative
                    // pixels and g629VerifyAfterCpu compares against them.
                    const bool g629Gpu =
                        !g629Skip && g629TryReplayGpu(g_g144List, rgs, rgs->m_vram,
                                                      rgs->m_vramSize, g605ClutFill);
                    g638Lap(kG638PhG629);
                    if (!g629Skip && !g629Gpu)
                    {
                    g630PrepareCpuReplay();
                    // G637 (default-off, DC2_G637_COOP=1): the cooperative, weight-balanced,
                    // work-stealing replacement for the fixed equal-row eight-lane barrier. With
                    // the lever off this forwards to the very same
                    // `GSRowPool::instance().run(y0, y1, g529Lanes, job)` call, so the shipped path
                    // is unchanged; with it on, the SAME job body runs over a different set of
                    // contiguous disjoint row ranges (exactness argument unchanged — see
                    // g637_coop_replay.inc §0 and GSRowPool::runCoop).
                    g637RunReplay(y0, y1, g529Lanes, g_g144List,
                                              [self, rgs, g570Linear139,
                                               g570PreparedBatch,
                                               g570GpuBatch,
                                               g605AnySharedClut](int a, int b) {
                        G529LaneScope g529lane; // inert unless DC2_G529_STAT=1
                        static thread_local GS s_rg;
                        s_rg.m_vram = rgs->m_vram;
                        s_rg.m_vramSize = rgs->m_vramSize;
                        s_rg.m_privRegs = rgs->m_privRegs;
                        t_g144InReplay = true;
                        t_g144Banded = true;
                        t_g144BandY0 = a;
                        t_g144BandY1 = b;
                        auto g570DrawEntry = [&](const G144Entry &e) {
                            if (e.maxY < a || e.minY > b)
                                return; // bbox does not intersect this band
                            const size_t entryIndex =
                                static_cast<size_t>(&e - g_g144List.data());
                            if (g605AnySharedClut)
                                g580BindSharedSample(entryIndex);
                            else
                                g580ClearSharedSample();
                            s_rg.m_ctx[0] = e.ctx;
                            s_rg.m_prim = e.prim;
                            s_rg.m_prim.ctxt = false; // active context forced to m_ctx[0] = snapshot
                            s_rg.m_texa = e.texa;
                            s_rg.m_texclut = e.texclut;
                            s_rg.m_pabe = e.pabe;
                            t_g364FogCol = e.fogcol; // G364
                            t_g144TitleRockScopeOverride = true;
                            t_g144TitleRockScope = e.titleRockScope;
                            t_g144TownDepthScopeOverride = true;
                            t_g144TownDepthScope = e.townDepthScope;
                            s_rg.m_vtxQueue[0] = e.v[0];
                            s_rg.m_vtxQueue[1] = e.v[1];
                            s_rg.m_vtxQueue[2] = e.v[2];
                            t_g258EntryIndex = entryIndex;
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

                            // The admitted batch starts with its untextured color clear.  Load this
                            // lane only after that clear has committed, so the linear surface has
                            // exactly the same initial words as the generic replay.  If the clear's
                            // bbox misses a lane, this is never reached there and that lane safely
                            // stays on the ordinary swizzled path.
                            if (g570Linear139 && &e == g_g144List.data())
                                g570Cpu139LinearBegin(rgs->m_vram, rgs->m_vramSize, a, b);
                        };

                        if (g570PreparedBatch)
                        {
                            // The validated list is exactly one ordinary clear followed by the
                            // ordered shadow-volume class. Keep the clear on the established sprite
                            // path, then bypass drawTriangle's repeated lane-local front-end.
                            g570DrawEntry(g_g144List.front());
                            if (!g570GpuBatch)
                            {
                            static const bool s_g570TriVerify =
                                envFlagEnabled("DC2_G570_BATCH_TRI_VERIFY");
                            static std::atomic<uint32_t> s_g570TriVerifyClaims{0u};
                            bool g570DoTriVerify = false;
                            if (s_g570TriVerify && a == 0)
                                g570DoTriVerify = s_g570TriVerifyClaims.fetch_add(
                                    1u, std::memory_order_relaxed) < 64u;
                            if (g570DoTriVerify)
                            {
                                std::vector<uint32_t> state, candidate, reference;
                                g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize, a, b, state);
                                for (size_t tri = 0; tri < g570Batch139TriCount(); ++tri)
                                {
                                    g570Batch139ReplayTri(rgs->m_vram, rgs->m_vramSize,
                                                          a, b, tri);
                                    g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                         a, b, candidate);
                                    g570Batch139WriteBand(rgs->m_vram, rgs->m_vramSize,
                                                          a, b, state);
                                    g570DrawEntry(g_g144List[tri + 1u]);
                                    g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                         a, b, reference);
                                    size_t firstBad = static_cast<size_t>(-1);
                                    for (size_t px = 0; px < candidate.size(); ++px)
                                        if (candidate[px] != reference[px])
                                        {
                                            firstBad = px;
                                            break;
                                        }
                                    if (firstBad != static_cast<size_t>(-1))
                                    {
                                        constexpr size_t g570W =
                                            static_cast<size_t>(kG403DisplayZW);
                                        const G144Entry &badEntry = g_g144List[tri + 1u];
                                        std::fprintf(stderr,
                                            "[G570:batchtribad] tri=%zu xy=(%zu,%d) "
                                            "candidate=%08x reference=%08x alpha=%llx "
                                            "v0=(%.9g,%.9g,%.9g) v1=(%.9g,%.9g,%.9g) "
                                            "v2=(%.9g,%.9g,%.9g)\n",
                                            tri, firstBad % g570W,
                                            a + static_cast<int>(firstBad / g570W),
                                            candidate[firstBad], reference[firstBad],
                                            static_cast<unsigned long long>(badEntry.ctx.alpha),
                                            badEntry.v[0].x, badEntry.v[0].y, badEntry.v[0].z,
                                            badEntry.v[1].x, badEntry.v[1].y, badEntry.v[1].z,
                                            badEntry.v[2].x, badEntry.v[2].y, badEntry.v[2].z);
                                        for (size_t rest = tri + 1u;
                                             rest < g570Batch139TriCount(); ++rest)
                                            g570DrawEntry(g_g144List[rest + 1u]);
                                        break;
                                    }
                                    state.swap(reference);
                                }
                            }
                            else
                            {
                                std::vector<uint32_t> initial, candidate;
                                if (g570Batch139VerifyOn())
                                    g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                         a, b, initial);
                                const uint64_t candidatePixels =
                                    g570Batch139ReplayBand(rgs->m_vram, rgs->m_vramSize, a, b);
                                if (g570Batch139VerifyOn())
                                {
                                    g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                         a, b, candidate);
                                    g570Batch139WriteBand(rgs->m_vram, rgs->m_vramSize,
                                                          a, b, initial);
                                    for (size_t i = 1; i < g_g144List.size(); ++i)
                                        g570DrawEntry(g_g144List[i]);
                                    std::vector<uint32_t> reference;
                                    g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                         a, b, reference);
                                    uint64_t bad = 0u;
                                    size_t firstBad = static_cast<size_t>(-1);
                                    const size_t n = std::min(candidate.size(), reference.size());
                                    for (size_t i = 0; i < n; ++i)
                                        if (candidate[i] != reference[i])
                                        {
                                            if (firstBad == static_cast<size_t>(-1)) firstBad = i;
                                            ++bad;
                                        }
                                    bad += static_cast<uint64_t>(candidate.size() > n
                                        ? candidate.size() - n : reference.size() - n);
                                    if (bad != 0u && firstBad != static_cast<size_t>(-1))
                                    {
                                        static std::atomic<uint32_t> s_g570BatchFirst{0u};
                                        const uint32_t k = s_g570BatchFirst.fetch_add(
                                            1u, std::memory_order_relaxed);
                                        if (k < 32u)
                                        {
                                            constexpr size_t g570W =
                                                static_cast<size_t>(kG403DisplayZW);
                                            const int badX = static_cast<int>(firstBad % g570W);
                                            const int badY = a + static_cast<int>(firstBad / g570W);
                                            std::fprintf(stderr,
                                                "[G570:batchbad] k=%u band=%d..%d xy=(%d,%d) "
                                                "candidate=%08x reference=%08x bad=%llu\n",
                                                k, a, b, badX, badY, candidate[firstBad],
                                                reference[firstBad],
                                                static_cast<unsigned long long>(bad));
                                        }
                                    }
                                    g570Batch139NoteVerify(candidatePixels, bad);
                                }
                            }
                            }
                        }
                        else
                        {
                            // G637: replay only the entries whose bbox can reach this chunk's rows.
                            // The bucket membership predicate IS `e.maxY < a || e.minY > b` — the
                            // same test g570DrawEntry applies on its first line — evaluated ONCE per
                            // (entry, chunk) on the dispatcher instead of once per entry per chunk
                            // here, and filled in submission order, so both the visited SET and its
                            // ORDER are identical to the full walk. When the index is absent
                            // (lever off, DC2_G637_NO_INDEX=1, or the legacy pool path) this falls
                            // through to that walk unchanged.
                            const int g637Chunk = g_g637IdxEnt ? g637ChunkOf(a) : -1;
                            if (g637Chunk >= 0)
                            {
                                const uint32_t g637Lo = g_g637IdxOff[g637Chunk];
                                const uint32_t g637Hi = g_g637IdxOff[g637Chunk + 1];
                                for (uint32_t g637K = g637Lo; g637K < g637Hi; ++g637K)
                                    g570DrawEntry(g_g144List[g_g637IdxEnt[g637K]]);
                            }
                            else
                            {
                                for (const auto &e : g_g144List)
                                    g570DrawEntry(e);
                            }
                        }
                        if (g570Linear139)
                            g570Cpu139LinearEnd(rgs->m_vram, rgs->m_vramSize);
                        t_g149Px = nullptr; t_g149Valid = nullptr;
                        g580ClearSharedSample();
                        t_g258EntryIndex = static_cast<size_t>(-1);
                        t_g144TitleRockScopeOverride = false;
                        t_g144TownDepthScopeOverride = false;
                        t_g144InReplay = false;
                        t_g144Banded = false;
                                              });
                    g630NoteCpuReplay();
                    }
                    g638Lap(kG638PhPool);
                    const uint64_t g571ClearUs = s_g571Split
                        ? static_cast<uint64_t>(std::chrono::duration_cast<
                              std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - g571ClearT).count())
                        : 0u;
                    if (g570GpuBatch)
                    {
                        // G641 (observer while DC2_G641_NEC is unset): classify how much of the
                        // 0x139 round trip below the producer can actually write. Placed before the
                        // gather so the window is available to every phase of the chain.
                        g641NoteBatch();
                        constexpr int g570Y0 = 0;
                        constexpr int g570Y1 = static_cast<int>(kG403DisplayZH) - 1;
                        // ⭐⭐⭐ G636: this block reads target 0x139's whole colour band out of
                        // GUEST VRAM and the shared depth mirror, and writes the band back — and
                        // it runs even when the resident GPU replay took this flush. Settle both
                        // resident authorities first or it commits pre-replay bytes over the
                        // replay's own output (the measured `ridepod` chassis loss).
                        g636SettleSurfaceBand(0x139u, 8u, g570Y0, g570Y1, true);
                        std::vector<uint32_t> initial, candidate;
                        auto g571PhaseT = s_g571Split
                            ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
                        // G641: narrowed to the producer's ragged tile extent when armed; the
                        // legacy whole-surface gather otherwise. Under the oracle the FULL gather
                        // runs, because the oracle needs the untouched `initial` for its
                        // "outside the window nothing changed" half.
                        if (g641WinUsable() && !g641WinVerifyOn())
                            g641ReadShadowBand(rgs->m_vram, rgs->m_vramSize,
                                               g570Y0, g570Y1, initial);
                        else
                            g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                 g570Y0, g570Y1, initial);
                        const uint64_t g571ReadUs = s_g571Split
                            ? static_cast<uint64_t>(std::chrono::duration_cast<
                                  std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - g571PhaseT).count())
                            : 0u;
                        if (s_g571Split)
                            g571PhaseT = std::chrono::steady_clock::now();
                        const bool gpuOk = g570Batch139ReplayGpu(initial, candidate);
                        const uint64_t g571GpuUs = s_g571Split
                            ? static_cast<uint64_t>(std::chrono::duration_cast<
                                  std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - g571PhaseT).count())
                            : 0u;
                        if (g570Batch139GpuVerifyOn())
                            g570Batch139ResetCpuDebug();
                        const auto replayCpuBatch = [&] {
                            GSRowPool::instance().run(
                                g570Y0, g570Y1, g529Lanes,
                                [rgs](int a, int b) {
                                    g570Batch139ReplayBand(
                                        rgs->m_vram, rgs->m_vramSize, a, b);
                                });
                        };
                        if (s_g571Split)
                            g571PhaseT = std::chrono::steady_clock::now();
                        if (gpuOk)
                        {
                            // G641: the oracle runs BEFORE the commit so it compares candidates,
                            // not already-published VRAM, and so a failure is visible even though
                            // the arm still publishes exactly what it would have published.
                            g641RunVerify(initial, candidate);
                            g641NoteWindowUse(g641WinUsable());
                            g641WriteShadowBand(rgs->m_vram, rgs->m_vramSize,
                                                g570Y0, g570Y1, candidate);
                        }
                        else
                            replayCpuBatch();
                        const uint64_t g571CommitUs = s_g571Split
                            ? static_cast<uint64_t>(std::chrono::duration_cast<
                                  std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - g571PhaseT).count())
                            : 0u;

                        if (s_g571Split)
                            g571PhaseT = std::chrono::steady_clock::now();
                        if (gpuOk && g570Batch139GpuVerifyOn())
                        {
                            g570Batch139WriteBand(rgs->m_vram, rgs->m_vramSize,
                                                  g570Y0, g570Y1, initial);
                            replayCpuBatch();
                            std::vector<uint32_t> reference;
                            g570Batch139ReadBand(rgs->m_vram, rgs->m_vramSize,
                                                 g570Y0, g570Y1, reference);
                            uint64_t bad = 0u;
                            size_t firstBad = static_cast<size_t>(-1);
                            const size_t n = std::min(candidate.size(), reference.size());
                            for (size_t i = 0; i < n; ++i)
                                if (candidate[i] != reference[i])
                                {
                                    if (firstBad == static_cast<size_t>(-1))
                                        firstBad = i;
                                    ++bad;
                                }
                            bad += static_cast<uint64_t>(candidate.size() > n
                                ? candidate.size() - n : reference.size() - n);
                            if (bad != 0u && firstBad != static_cast<size_t>(-1))
                            {
                                static std::atomic<uint32_t> s_g570GpuFirst{0u};
                                const uint32_t k = s_g570GpuFirst.fetch_add(
                                    1u, std::memory_order_relaxed);
                                if (k < 32u)
                                {
                                    constexpr size_t width =
                                        static_cast<size_t>(kG403DisplayZW);
                                    std::fprintf(stderr,
                                        "[G570:gpubad] k=%u xy=(%zu,%zu) "
                                        "candidate=%08x reference=%08x bad=%llu\n",
                                        k, firstBad % width, firstBad / width,
                                        candidate[firstBad], reference[firstBad],
                                        static_cast<unsigned long long>(bad));
                                }
                            }
                            g570Batch139NoteGpuVerify(candidate.size(), bad);
                        }
                        if (s_g571Split)
                        {
                            const auto g571Done = std::chrono::steady_clock::now();
                            const uint64_t g571OracleUs = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    g571Done - g571PhaseT).count());
                            const uint64_t g571TotalUs = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    g571Done - g570PrepT).count());
                            std::fprintf(stderr,
                                         "[G571:batch] entries=%zu prep=%.3f clear=%.3f "
                                         "read=%.3f gpu=%.3f commit=%.3f oracle=%.3f "
                                         "total=%.3fms ok=%u\n",
                                         g_g144List.size(), g570PrepUs / 1000.0,
                                         g571ClearUs / 1000.0, g571ReadUs / 1000.0,
                                         g571GpuUs / 1000.0, g571CommitUs / 1000.0,
                                         g571OracleUs / 1000.0, g571TotalUs / 1000.0,
                                         gpuOk ? 1u : 0u);
                        }
                    }
                    g638Lap(kG638PhG570Gpu);
                    if (g529On)
                    {
                        g529EndDispatch();
                        const uint64_t wallNs =
                            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g529T).count();
                        g529NoteDispatch(g528ClassifyFbp(g_g144BlockFbp), g_g144BlockFbp,
                                         wallNs, g529ZclrNs, g529ScanNs, g529Entries,
                                         g529Rows, g529Px, g529Lanes);
                    }
                    // G629: book this batch's ADMISSIBILITY against the pool wall it just cost.
                    // Placed AFTER the pool returns so the classification cannot perturb the wall
                    // it is attributing (g526_draw_census.inc's rule).
                    // G629 oracle: the CPU arm has now committed the authoritative pixels, so the
                    // candidate captured before it ran can be compared against them.
                    g629VerifyAfterCpu(rgs->m_vram);
                    if (g629On)
                        g629NoteBatch(g_g144List,
                                      (uint64_t)std::chrono::duration_cast<
                                          std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - g629T0).count(),
                                      g529Rows);
                    g638Lap(kG638PhTail);
                    if (g_g638PrepCensusOn)
                        g638PrepEndBatch();
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
                g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List, "post-cpu");
                g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "post-cpu");
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
                g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List);
                g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "pre");
                // G178: same whole-flush GPU attempt as the parallel closure above.
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
                    g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List, "post-gpu");
                    g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "post-gpu");
                    g_g144List.clear();
                    return;
                }
                // G589: same fail-closed disarm as the parallel closure above.
                g589DisarmTransaction();
                // G528: identical prologue to the parallel closure above (see its comment). Routing
                // both through one function is what keeps DC2_G144_SEQ a pure parallelism bisect.
                g528FlushPrologue(3, g_g144LastGs->m_vram, g_g144BlockFbp);
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
                // ⭐⭐⭐ G636: THE SEQUENTIAL DRAIN NEVER HAD G630'S CONTRACT. It is the frame-end
                // tail closure and it runs the identical CPU rasterizer over `g_g144List` — reading
                // and writing guest VRAM and the shared `g403DisplayZBuf()` depth mirror — with no
                // publication in front of it and no invalidation behind it. The parallel closure
                // above has had both since G630; this one was simply missed, so under
                // `DC2_G630_RESIDENT` it read pre-replay bytes and left the GPU depth mirror
                // believing it was still current.
                g630PrepareCpuReplay();
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
                    t_g364FogCol = e.fogcol; // G364
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
                g630NoteCpuReplay(); // G636: the missing other half of the contract (see above)
                g242NoteCpuReplayWrites(g_g144LastGs->m_vram);
                // G261: the wave arm disables the G260 pre-execution RTT generation bump (it
                // would poison GPU-resident targets); publish the replay's RTT writes here,
                // after the bytes actually changed.
                g261NoteCpuRttWrites(g_g144LastGs->m_vram);
                g286NoteCpuTransientWrites(g_g144LastGs->m_vram);
                g255CompareCpuVerify(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize);
                g401NoteCompositeEntries(g_g144LastGs->m_vram, g_g144List, "post-cpu");
                g401NoteDisplayFlush(g_g144LastGs->m_vram, g_g144List, "post-cpu");
                g_g144List.clear();
            };
        }

        g595Lap(kG595Body); // G595: lane count, flush-closure assignment test, g_g144Last* stores
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
        // G527: discover this render target if it is new (one relaxed byte load per draw when the
        // address is already known), then admit by PROPERTY instead of by the five-address list.
        // g527DeferTarget == `legacy five OR discovered`, so with the registry disarmed this is
        // bit-identical to the pre-G527 predicate. See g527_target_registry.inc.
        g527NoteTargetDraw(ctx.frame.fbp);
        const bool g252RttTarget =
            s_g252RttDefer && isSprite144 && g527DeferTarget(ctx.frame.fbp);
        // G286 admits the measured opaque textured full-screen sprite class. G558 optionally adds
        // the measured opaque solid (TME=0) S05 terminal sprite; other 0x13b shapes remain on the
        // proven CPU path instead of silently widening shader semantics.
        const bool g286Transient =
            g286TransientTarget(ctx.frame.fbp) && diagOff144 && isSprite144 &&
            (gs->m_prim.tme || g558Solid13bOn()) && !gs->m_prim.abe;
        // G573: one measured Ridepod class only. Keep TRIFAN and every other state inline until
        // independently observed; the downstream classifier remains an all-or-nothing fail-closed
        // gate, so unsupported blend/depth/texture state replays through the exact CPU bands.
        const bool g573Tri13b =
            g286TransientTarget(ctx.frame.fbp) && diagOff144 &&
            (pt144 == GS_PRIM_TRIANGLE || pt144 == GS_PRIM_TRISTRIP) &&
            ctx.frame.psm == GS_PSM_CT32 && gs->m_prim.tme && gs->m_prim.abe &&
            g573Tri13bOn();
        // G260 (opt-in): under the native-renderer command graph, RTT-family TRIANGLES (both
        // textured and untextured — the census shows both) become recordable too, closing the
        // G247 structural exclusion that rasterized them inline scalar on the GS thread. They
        // join the same per-target batches as the G252 RTT sprites and execute through the same
        // banded replay, in submission order. Without DC2_G260_NR this bool is constant false
        // and the shipped eligibility is bit-identical.
        static const bool s_g260NoRttTri = envFlagEnabled("DC2_G260_NO_RTTTRI"); // bisect lever
        // G527: same widening for the RTT-triangle class. DC2_G527_NO_TRI=1 bisects it back to the
        // legacy five while leaving discovered SPRITES admitted.
        const bool g260RttTri = g260NrOn() && !s_g260NoRttTri && isTri144 && diagOff144 &&
                                (g248TargetIndex(ctx.frame.fbp) >= 0 ||
                                 (g527TriOn() && g527DiscoveredTarget(ctx.frame.fbp)));
        // G595: arm-aware (DC2_G419_AB=g595line). Identical to g270LineWaveOn() outside the A/B.
        // `isLine144` FIRST: the conjuncts are side-effect-free and order-independent in value but
        // not in COST — lines are 4 of 19,229 captured entries/frame, so the type test
        // short-circuits the accessor on 99.98% of draws. ⚠️ MEASURED, NOT ASSUMED: this reorder
        // did NOT recover the shipped prologue's +0.14 ms/f (`pro` 1.783 -> 1.773 against a 1.633
        // control), and neither did hoisting the arm accessor behind a cached bool. Both
        // hypotheses are REFUTED; the residual is unattributed and needs /MAP+capstone, not
        // another guess (G595 §6). Kept because it is the strictly better shape.
        const bool g270LineWave = isLine144 && g270LineWaveActive() && !gs->m_prim.tme &&
                                  diagOff144 &&
                                  (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u);
        const bool deferrable144 = ((isTri144 || isSprite144) && diagOff144 &&
                                    (isTri144 ? (gs->m_prim.tme || g178GpuOn()) : true) &&
                                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u ||
                                      g252RttTarget)) ||
                                   g260RttTri || g270LineWave || g286Transient || g573Tri13b;
        if (g_g312SegStat)
            t_g312Seg2bEnd = std::chrono::steady_clock::now(); // G312 seg2b end (eligibility computed)
        g595Lap(kG595Elig); // G595: the deferral predicate itself
        g526Lap(kG526Pro); // G526: entry -> eligibility decided (shared by defer and inline)
        // G526 §2: name the inline population. The coarse pass proved ~20 primitives/frame carry
        // the whole per-primitive residual on Georama; this records WHICH they are (target fbp,
        // psm, tme/abe and scissor-clipped area) so the fix can be chosen rather than guessed.
        if (g_g526Draw && !deferrable144)
        {
            const int g526ox = ctx.xyoffset.ofx >> 4;
            const int g526oy = ctx.xyoffset.ofy >> 4;
            const uint32_t g526nv = (pt144 == GS_PRIM_SPRITE || isLine144) ? 2u : 3u;
            float g526x0 = 1e30f, g526x1 = -1e30f, g526y0 = 1e30f, g526y1 = -1e30f;
            for (uint32_t i = 0; i < g526nv; ++i)
            {
                const float px = gs->m_vtxQueue[i].x - static_cast<float>(g526ox);
                const float py = gs->m_vtxQueue[i].y - static_cast<float>(g526oy);
                if (px < g526x0) g526x0 = px;
                if (px > g526x1) g526x1 = px;
                if (py < g526y0) g526y0 = py;
                if (py > g526y1) g526y1 = py;
            }
            const int g526cx0 = clampInt(static_cast<int>(g526x0), ctx.scissor.x0, ctx.scissor.x1);
            const int g526cx1 = clampInt(static_cast<int>(g526x1), ctx.scissor.x0, ctx.scissor.x1);
            const int g526cy0 = clampInt(static_cast<int>(g526y0), ctx.scissor.y0, ctx.scissor.y1);
            const int g526cy1 = clampInt(static_cast<int>(g526y1), ctx.scissor.y0, ctx.scissor.y1);
            const uint64_t g526px =
                static_cast<uint64_t>(g526cx1 >= g526cx0 ? (g526cx1 - g526cx0 + 1) : 0) *
                static_cast<uint64_t>(g526cy1 >= g526cy0 ? (g526cy1 - g526cy0 + 1) : 0);
            g526SetInlineKey(ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.psm),
                             gs->m_prim.tme ? 1u : 0u, gs->m_prim.abe ? 1u : 0u,
                             g248TargetIndex(ctx.frame.fbp), g526px);
        }
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
            if (g573Tri13b)
                g573NoteTri13bAdmission(pt144);
            const std::chrono::steady_clock::time_point g156CapT0 =
                g_g156Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            // G312 segment census: close seg2 (post-pile -> capture start) and report periodically.
            if (g_g312SegStat)
            {
                const auto g312End = std::chrono::steady_clock::now();
                g_g312Seg2Ns.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g312End - t_g312Seg2Start)
                        .count(),
                    std::memory_order_relaxed);
                g_g312Seg2aNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t_g312Seg2aEnd - t_g312Seg2Start)
                        .count(),
                    std::memory_order_relaxed);
                g_g312Seg2bNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t_g312Seg2bEnd - t_g312Seg2aEnd)
                        .count(),
                    std::memory_order_relaxed);
                const uint64_t n312 = g_g312SegN.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((n312 % 8000u) == 0u)
                {
                    const double s0 = g_g312Seg0Ns.load(std::memory_order_relaxed) / 1.0e6;
                    const double s1 = g_g312Seg1Ns.load(std::memory_order_relaxed) / 1.0e6;
                    const double s2 = g_g312Seg2Ns.load(std::memory_order_relaxed) / 1.0e6;
                    const double s2a = g_g312Seg2aNs.load(std::memory_order_relaxed) / 1.0e6;
                    const double s2b = g_g312Seg2bNs.load(std::memory_order_relaxed) / 1.0e6;
                    std::fprintf(stderr,
                                 "[G312:seg] n=%llu seg0(g45)=%.1fms (%.0f ns/d) seg1(pile)=%.1fms "
                                 "(%.0f ns/d) seg2(elig)=%.1fms (%.0f ns/d) [2a(cnt)=%.0f 2b(elig)=%.0f "
                                 "2c(preflush)=%.0f ns/d]\n",
                                 (unsigned long long)n312, s0, s0 * 1e6 / (double)n312, s1,
                                 s1 * 1e6 / (double)n312, s2, s2 * 1e6 / (double)n312,
                                 s2a * 1e6 / (double)n312, s2b * 1e6 / (double)n312,
                                 (s2 - s2a - s2b) * 1e6 / (double)n312);
                }
            }
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
            g526Lap(kG526Sw); // G526: target-switch batch close / mid-flush drain
            g_g144BlockFbp = ctx.frame.fbp;
            // Screen bbox rows computed EXACTLY as drawTriangle/drawSprite (same ofy + scissor
            // clamp) so the band prefilter is tight and never narrower than the real coverage.
            const GSVertex &cv0 = gs->m_vtxQueue[0];
            const GSVertex &cv1 = gs->m_vtxQueue[1];
            const int cofy = ctx.xyoffset.ofy >> 4;
            int crawMinY, crawMaxY;
            // G521: build the entry DIRECTLY in g_g144List instead of in a stack local that is
            // then moved in. G510's capmove made the push a move, but G144Entry is trivially
            // copyable except for one shared_ptr, so the move still transfers all 256 bytes —
            // the entry is written twice per captured primitive, ~9,927 times a frame on the pole
            // thread. Nothing between here and the push returns, throws, flushes or reads
            // g_g144List, so constructing in the final slot lands the identical bytes.
            // The G144NoInit tag DEFAULT-initializes; a bare emplace_back() would value-initialize
            // and zero-fill 256 bytes the control arm never writes.
            // Rollback DC2_G521_NO_CAPINPLACE=1. Paired gate: DC2_G419_AB=capinplace.
#if DC2_G521_INPLACEARM
            const bool g521InPlace = g521CapInPlaceActive();
            G144Entry g521Local;
            if (g521InPlace)
                g_g144List.emplace_back(G144NoInit{});
            // ⚠️ G521 §6b — THIS LINE IS WHY THE LEVER LOST. A runtime-varying reference turns the
            // ~40 field writes below from direct stores at known rsp offsets into indirect stores
            // through a base pointer, on BOTH arms. G520 §7b's codegen-symmetry law, hitting the
            // candidate instead of the control.
            G144Entry &e = g521InPlace ? g_g144List.back() : g521Local;
#else
            G144Entry e; // pre-G521 shape, restored (capinplace REFUTED)
#endif
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
            e.fogcol = g_g364FogCol; // G364
            e.titleRockScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
            e.townDepthScope = g_dc2TownDepthScope.load(std::memory_order_relaxed);
            e.minY = clampInt(crawMinY, ctx.scissor.y0, ctx.scissor.y1);
            e.maxY = clampInt(crawMaxY, ctx.scissor.y0, ctx.scissor.y1);
            g526Lap(kG526Build); // G526: bbox + G144Entry field fill
            // G149: decode this tri's texture ONCE now (guest thread), so all band lanes read it
            // lock-free at replay. Null unless texcache-enabled + eligible. Sprites never use the
            // G149 texcache hook (drawSprite() samples VRAM directly, same as its inline path).
            e.decoded = isTri144 ? g149BuildDecoded(this, gs, ctx, gs->m_texa, gs->m_vram) : G149Buf{};
            g526Lap(kG526Dec); // G526: per-triangle texture decode at capture time
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
                g595NoteAppend(e); // G595: state-identity memo in front of g260NoteAppend
            g336NoteConsumer(e); // G336: first next-frame consumer of boundary-published state (default-off)
            g526Lap(kG526Note); // G526: g260NoteAppend page-range accumulation + g336
            // G510: `e` is a loop-local, fully assigned above and dead from here on (only the
            // g156 stat block and the `return` follow), so moving it into the list lands the
            // identical bytes in the identical slot and removes one whole G144Entry copy —
            // ~150 bytes of GSContext + registers, three GSVertex, and a G149Buf
            // copy-construct/destroy pair — from the GS worker's per-primitive path (~9,900/frame
            // on lean MAP-0). Exact by construction; nothing observes `e` after the push.
            // Rollback: DC2_G510_NO_CAPMOVE=1. Paired gate: DC2_G419_AB=capmove | g510both.
            g521ReportEntrySize(); // G521: one-shot layout report, inert unless DC2_G521_SIZE=1
#if !DC2_G521_INPLACEARM
            if (g510CapMoveActive())
                g_g144List.push_back(std::move(e));
            else
                g_g144List.push_back(e);
#else
            if (!g521InPlace)
            {
                if (g510CapMoveActive())
                    g_g144List.push_back(std::move(e));
                else
                    g_g144List.push_back(e);
            }
            else if (g521VerifyOn())
            {
                // Oracle: the entry is already in its slot, so re-derive what the CONTROL arm
                // would have produced (the same field values, read back out of the slot) and
                // compare the groups the flush's five pre-passes actually read. Out of line via
                // g521NoteEntry so the comparison is not inside the measured basic block.
                const G144Entry &g521Slot = g_g144List.back();
                const bool bad =
                    std::memcmp(&g521Slot.ctx, &ctx, sizeof(GSContext)) != 0 ||
                    std::memcmp(&g521Slot.prim, &gs->m_prim, sizeof(GSPrimReg)) != 0 ||
                    std::memcmp(&g521Slot.texa, &gs->m_texa, sizeof(GSTexaReg)) != 0 ||
                    std::memcmp(&g521Slot.texclut, &gs->m_texclut, sizeof(GSTexClutReg)) != 0 ||
                    g521Slot.pabe != gs->m_pabe || g521Slot.fogcol != g_g364FogCol ||
                    g521Slot.minY != clampInt(crawMinY, ctx.scissor.y0, ctx.scissor.y1) ||
                    g521Slot.maxY != clampInt(crawMaxY, ctx.scissor.y0, ctx.scissor.y1);
                g521NoteEntry(bad);
            }
#endif
            if (g_g156Stat)
                g_g156CaptureNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g156CapT0)
                        .count(),
                    std::memory_order_relaxed);
            g526Lap(kG526Push);      // G526: g_g144List.push_back
            g526SetExit(kG526Defer); // G526 exit 2
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
        g526Lap(kG526Drain); // G526: inline path — pending-graph drain before this primitive
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
            g345AnyOwnerActive()
                ? g289AuditInline(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut)
                : G289InlineAudit{};
        if (g345AnyOwnerActive() && g289StatOn())
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
        g526Lap(kG526Edge); // G526: inline path — ownership/materialize/publish edge
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
    g526Lap(kG526Raster); // G526: inline path — the actual CPU rasterization of this primitive

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
            g178NoteVramWriteRectTagged(gs->m_vram,
                                  GSInternal::framePageBaseToBlock(ctx178.frame.fbp),
                                  ctx178.frame.fbw, ctx178.frame.psm,
                                  static_cast<uint32_t>(std::max(bx0, 0)),
                                  static_cast<uint32_t>(std::max(by0, 0)),
                                  static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                  static_cast<uint32_t>(by1 - std::max(by0, 0) + 1),
                                  6u); // G582 source 6: inline-draw bbox color bump
            if (((ctx178.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRectTagged(gs->m_vram,
                                      static_cast<uint32_t>(ctx178.zbuf & 0x1FFu) << 5u,
                                      ctx178.frame.fbw,
                                      static_cast<uint32_t>((ctx178.zbuf >> 24u) & 0xFu),
                                      static_cast<uint32_t>(std::max(bx0, 0)),
                                      static_cast<uint32_t>(std::max(by0, 0)),
                                      static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                      static_cast<uint32_t>(by1 - std::max(by0, 0) + 1),
                                      7u); // G582 source 7: inline-draw bbox depth bump
        }
    }
}
