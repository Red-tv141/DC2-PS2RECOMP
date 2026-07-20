void GSRasterizer::drawTriangle(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const GSVertex &v2 = gs->m_vtxQueue[2];
    const auto &ctx = gs->activeContext();

    // G104: clip title-rock triangles after GS primitive assembly, not per VU vertex. G102's
    // VU-side replacement was non-local for strips/fans: one shared behind-camera vertex can need
    // two different edge intersections. Here the complete triangle is available, so mixed q<=0/q>0
    // primitives are split and re-enter the normal raster path. Default uses screen-space q crossing
    // because GS XY is already packed; DC2_G104_HOMOG_CLIP restores the homogeneous A/B.
    if (!g_g104TriClipReentry &&
        g104TriClipEnabled() &&
        g104TitleRockTriangle(gs->m_prim.tme, ctx.frame.fbp, ctx.tex0.tbp0) &&
        !g106NoTriClipTriangle(gs->m_prim.tme,
                               static_cast<uint32_t>(gs->m_prim.type),
                               ctx.frame.fbp,
                               ctx.tex0.tbp0))
    {
        const bool in0 = g104InsideQ(v0);
        const bool in1 = g104InsideQ(v1);
        const bool in2 = g104InsideQ(v2);
        const int insideCount = (in0 ? 1 : 0) + (in1 ? 1 : 0) + (in2 ? 1 : 0);

        static const bool s_g104stat = envFlagEnabled("DC2_G104_STAT");
        static std::atomic<uint64_t> s_g104Pass{0};
        static std::atomic<uint64_t> s_g104Clip{0};
        static std::atomic<uint64_t> s_g104Drop{0};
        static std::atomic<uint64_t> s_g104Emit{0};
        auto g104LogMaybe = [&]() {
            if (!s_g104stat)
                return;
            const uint64_t total =
                s_g104Pass.load(std::memory_order_relaxed) +
                s_g104Clip.load(std::memory_order_relaxed) +
                s_g104Drop.load(std::memory_order_relaxed);
            if ((total & 0x3FFFull) == 0ull)
                std::fprintf(stderr,
                             "[G104:triclip] pass=%llu clip=%llu drop=%llu emit=%llu mode=%s wnear=%.2f qnear=%.5g\n",
                             static_cast<unsigned long long>(s_g104Pass.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Clip.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Drop.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Emit.load(std::memory_order_relaxed)),
                             g104ScreenClipEnabled() ? "screen" : "homog",
                             g104Wnear(), g104Qnear());
        };

        // G128: near-plane REJECT for title-rock straddlers that cross the camera plane.
        // A vertex with q<=0 is BEHIND the camera; its perspective-divided screen XY was
        // saturated/wrapped by FTOI4 (12.4, 16-bit) before the GS saw it, so the homogeneous
        // clip's reconstruction clip=screen*W recovers garbage -> the screen-spanning dark
        // wedges (g127_smoke_check.png). The pre-FTOI4 float position needed for a correct
        // clip is not available here (it would require a VU-side side channel that desyncs
        // against the copy packer). Dropping the straddling triangle (HW's near-plane clip
        // would keep a sliver; we lose it) removes the wedges cleanly with at most a
        // one-triangle-deep ragged edge. Title-scoped (inside g104TitleRockTriangle). Kill
        // with DC2_G128_NO_BEHIND_DROP=1 to restore the G125/G126 homog-clip-only behaviour.
        // G139: RETIRED by default (was default-ON). Natural gate + fixed VU positions leave no
        // straddler wedges to drop (A/B g100_g139nb_keepz.png). Re-enable DC2_G128_FORCE_BEHIND_DROP=1.
        static const bool s_g128NoBehindDrop =
            std::getenv("DC2_G128_FORCE_BEHIND_DROP") == nullptr ||
            envFlagEnabled("DC2_G128_NO_BEHIND_DROP");
        static std::atomic<uint64_t> s_g128BehindDrop{0};
        if (!s_g128NoBehindDrop)
        {
            const bool behind0 = !std::isfinite(v0.q) || v0.q <= 0.0f;
            const bool behind1 = !std::isfinite(v1.q) || v1.q <= 0.0f;
            const bool behind2 = !std::isfinite(v2.q) || v2.q <= 0.0f;
            if (behind0 || behind1 || behind2)
            {
                s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
                if (s_g104stat)
                {
                    const uint64_t bd = s_g128BehindDrop.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if ((bd & 0x3FFFull) == 0ull)
                        std::fprintf(stderr, "[G128:behinddrop] dropped=%llu\n",
                                     static_cast<unsigned long long>(bd));
                }
                return;
            }
        }

        if (insideCount == 0)
        {
            s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
            g104LogMaybe();
            return;
        }

        if (insideCount == 3)
        {
            s_g104Pass.fetch_add(1u, std::memory_order_relaxed);
            g104LogMaybe();
        }
        else
        {
            GSVertex inPoly[6] = {v0, v1, v2};
            GSVertex outPoly[6]{};
            int inCount = 3;
            int outCount = 0;
            const float wNear = g104Wnear();

            for (int i = 0; i < inCount; ++i)
            {
                const GSVertex &a = inPoly[i];
                const GSVertex &b = inPoly[(i + 1) % inCount];
                const bool aIn = g104InsideQ(a);
                const bool bIn = g104InsideQ(b);

                if (aIn && bIn)
                {
                    outPoly[outCount++] = b;
                }
                else if (aIn && !bIn)
                {
                    outPoly[outCount++] = g104IntersectNearW(a, b, wNear);
                }
                else if (!aIn && bIn)
                {
                    outPoly[outCount++] = g104IntersectNearW(a, b, wNear);
                    outPoly[outCount++] = b;
                }
            }

            if (outCount < 3)
            {
                s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
                g104LogMaybe();
                return;
            }

            s_g104Clip.fetch_add(1u, std::memory_order_relaxed);
            s_g104Emit.fetch_add(static_cast<uint64_t>(outCount - 2), std::memory_order_relaxed);
            g104LogMaybe();

            const GSVertex saved0 = gs->m_vtxQueue[0];
            const GSVertex saved1 = gs->m_vtxQueue[1];
            const GSVertex saved2 = gs->m_vtxQueue[2];
            const bool savedReentry = g_g104TriClipReentry;
            g_g104TriClipReentry = true;
            for (int i = 1; i + 1 < outCount; ++i)
            {
                gs->m_vtxQueue[0] = outPoly[0];
                gs->m_vtxQueue[1] = outPoly[i];
                gs->m_vtxQueue[2] = outPoly[i + 1];
                drawTriangle(gs);
            }
            g_g104TriClipReentry = savedReentry;
            gs->m_vtxQueue[0] = saved0;
            gs->m_vtxQueue[1] = saved1;
            gs->m_vtxQueue[2] = saved2;
            return;
        }
    }

    // G82: per-TRIANGLE (vertex-comparable) rock colour aggregator, so we can compare the
    // runner's raw per-vertex rock colours to HW (g82_gs_verts.py, per-vertex) apples-to-apples
    // -- the per-pixel [G82:rockavg] is area-weighted and not comparable. Gated DC2_G82_VTX.
    {
        static const bool s_g82vtx = (std::getenv("DC2_G82_VTX") != nullptr);
        if (s_g82vtx && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::mutex s_vmtx;
            struct VAcc { uint32_t tbp; uint64_t n, r,g,b,a; };
            static VAcc s_v[96]{}; static uint32_t s_vN = 0;
            std::lock_guard<std::mutex> lk(s_vmtx);
            uint32_t idx = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < s_vN; ++i) if (s_v[i].tbp == ctx.tex0.tbp0) { idx = i; break; }
            if (idx == 0xFFFFFFFFu && s_vN < 96u) { idx = s_vN++; s_v[idx].tbp = ctx.tex0.tbp0; }
            if (idx != 0xFFFFFFFFu) {
                VAcc &A = s_v[idx];
                A.n += 3; A.r += v0.r+v1.r+v2.r; A.g += v0.g+v1.g+v2.g; A.b += v0.b+v1.b+v2.b; A.a += v0.a+v1.a+v2.a;
            }
            static uint64_t s_vt = 0;
            if ((++s_vt % 40000ull) == 0ull)
                for (uint32_t i = 0; i < s_vN; ++i) { VAcc &A = s_v[i]; if (!A.n) continue;
                    std::fprintf(stderr, "[G82:vtx] tbp=0x%x verts=%llu avgcol=(%llu,%llu,%llu,%llu)\n",
                        A.tbp, (unsigned long long)A.n, (unsigned long long)(A.r/A.n),
                        (unsigned long long)(A.g/A.n), (unsigned long long)(A.b/A.n), (unsigned long long)(A.a/A.n)); }
        }
    }

    // G86: title-scoped texel dump. Decode the BOUND texture page through the runtime's own
    // sampler for each distinct rock tbp the first time it is bound in a title draw (fbp!=0x139).
    // Tells us whether the page the runner samples holds detailed brown rock (right content) or
    // green/uniform/garbage (wrong texture addressing). DC2_G86_TEXDUMP=1.
    {
        static const bool s_g86 = (std::getenv("DC2_G86_TEXDUMP") != nullptr);
        if (s_g86 && gs->m_vram && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3a00u)
        {
            static std::mutex s_m86;
            static uint32_t s_seen[64]{}; static uint32_t s_seenN = 0;
            const auto &tex = ctx.tex0;
            std::lock_guard<std::mutex> lk(s_m86);
            bool dumped = false;
            for (uint32_t i = 0; i < s_seenN; ++i) if (s_seen[i] == tex.tbp0) { dumped = true; break; }
            if (!dumped && s_seenN < 64u)
            {
                s_seen[s_seenN++] = tex.tbp0;
                const int dw = 1 << tex.tw, dh = 1 << tex.th;
                const uint32_t bwv = (tex.tbw > 1u) ? (tex.tbw >> 1) : 1u;
                char path[128];
                std::snprintf(path, sizeof(path), "captures/g86_tex_%x.ppm", tex.tbp0);
                FILE *fp = std::fopen(path, "wb");
                uint64_t sr = 0, sg = 0, sb = 0; uint32_t nz = 0;
                if (fp) std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                for (int yy = 0; yy < dh; ++yy)
                    for (int xx = 0; xx < dw; ++xx)
                    {
                        uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                         static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                        uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        uint8_t rr = c & 0xFF, gg = (c >> 8) & 0xFF, bb = (c >> 16) & 0xFF;
                        sr += rr; sg += gg; sb += bb; if (c & 0xFFFFFF) ++nz;
                        uint8_t rgb[3] = { rr, gg, bb };
                        if (fp) std::fwrite(rgb, 1, 3, fp);
                    }
                if (fp) std::fclose(fp);
                const uint32_t np = (uint32_t)(dw * dh);
                std::fprintf(stderr,
                    "[G86:texdump] tbp=0x%x %dx%d tbw=%u psm=0x%x cbp=0x%x avgtexel=(%llu,%llu,%llu) nz=%u/%u\n",
                    tex.tbp0, dw, dh, (unsigned)tex.tbw, (unsigned)tex.psm, tex.cbp,
                    (unsigned long long)(sr/np), (unsigned long long)(sg/np),
                    (unsigned long long)(sb/np), nz, np);
                std::fflush(stderr);
            }
        }
    }

    // G240: compare the foliage cutout's effective T8 indices and CT32 CLUT alpha against
    // the supplied hardware dump. The source IMAGE packets are byte-identical; this probe
    // observes the final values at the exact GEQUAL/AREF=64 draw boundary. Default-off.
    {
        static const bool s_g240TexAlpha = (std::getenv("DC2_G240_TEXALPHA") != nullptr);
        const auto &tex = ctx.tex0;
        const bool foliage2920 = tex.tbp0 == 0x2920u && tex.cbp == 0x3fb4u;
        const bool foliage2a20 = tex.tbp0 == 0x2a20u && tex.cbp == 0x3fb0u;
        if (s_g240TexAlpha && gs->m_vram && gs->m_prim.tme && tex.psm == GS_PSM_T8 &&
            tex.tcc != 0u && (foliage2920 || foliage2a20) &&
            (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u) && ctx.test == 0x5040bu)
        {
            static std::mutex s_g240Mtx;
            static uint32_t s_g240Seen = 0u;
            std::lock_guard<std::mutex> lock(s_g240Mtx);
            const uint32_t bit = foliage2920 ? 1u : 2u;
            if ((s_g240Seen & bit) == 0u)
            {
                s_g240Seen |= bit;
                const int width = 1 << tex.tw;
                const int height = 1 << tex.th;
                const uint32_t pageBwT8 = (tex.tbw > 1u) ? (tex.tbw >> 1u) : 1u;
                char indexPath[128], alphaPath[128], clutPath[128];
                std::snprintf(indexPath, sizeof(indexPath), "captures/g240_tex_%x_index.pgm", tex.tbp0);
                std::snprintf(alphaPath, sizeof(alphaPath), "captures/g240_tex_%x_alpha.pgm", tex.tbp0);
                std::snprintf(clutPath, sizeof(clutPath), "captures/g240_clut_%x.rgba", tex.cbp);
                FILE *indexFile = std::fopen(indexPath, "wb");
                FILE *alphaFile = std::fopen(alphaPath, "wb");
                FILE *clutFile = std::fopen(clutPath, "wb");
                if (indexFile) std::fprintf(indexFile, "P5\n%d %d\n255\n", width, height);
                if (alphaFile) std::fprintf(alphaFile, "P5\n%d %d\n255\n", width, height);

                uint64_t alphaHist[256]{};
                uint64_t indexHist[256]{};
                for (int y = 0; y < height; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        const uint8_t index = static_cast<uint8_t>(GSMem::ReadP8(
                            gs->m_vram, tex.tbp0, pageBwT8,
                            static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                        const uint32_t color = lookupCLUT(gs, index, tex.cbp, tex.cpsm,
                                                          tex.csm, tex.csa, tex.psm);
                        const uint8_t alpha = static_cast<uint8_t>(color >> 24u);
                        ++indexHist[index];
                        ++alphaHist[alpha];
                        if (indexFile) std::fwrite(&index, 1, 1, indexFile);
                        if (alphaFile) std::fwrite(&alpha, 1, 1, alphaFile);
                    }
                }
                if (indexFile) std::fclose(indexFile);
                if (alphaFile) std::fclose(alphaFile);

                if (clutFile)
                {
                    for (uint32_t index = 0; index < 256u; ++index)
                    {
                        const uint32_t color = lookupCLUT(gs, static_cast<uint8_t>(index),
                                                          tex.cbp, tex.cpsm, tex.csm,
                                                          tex.csa, tex.psm);
                        std::fwrite(&color, 1, sizeof(color), clutFile);
                    }
                    std::fclose(clutFile);
                }

                uint64_t below64 = 0u, atLeast64 = 0u;
                for (uint32_t alpha = 0; alpha < 256u; ++alpha)
                {
                    if (alpha < 64u) below64 += alphaHist[alpha];
                    else atLeast64 += alphaHist[alpha];
                }
                std::fprintf(stderr,
                    "[G240:texalpha] tbp=0x%x cbp=0x%x size=%dx%d tbw=%u cpsm=0x%x csm=%u csa=%u "
                    "cld=%u tex1=0x%llx texclut=(%u,%u,%u) test=0x%llx texa=(%u,%u,%u) "
                    "alpha_lt64=%llu alpha_ge64=%llu idx0=%llu idx173=%llu vtxa=(%u,%u,%u)\n",
                    tex.tbp0, tex.cbp, width, height, static_cast<unsigned>(tex.tbw),
                    static_cast<unsigned>(tex.cpsm), static_cast<unsigned>(tex.csm),
                    static_cast<unsigned>(tex.csa), static_cast<unsigned>(tex.cld),
                    static_cast<unsigned long long>(ctx.tex1),
                    static_cast<unsigned>(gs->m_texclut.cbw),
                    static_cast<unsigned>(gs->m_texclut.cou),
                    static_cast<unsigned>(gs->m_texclut.cov),
                    static_cast<unsigned long long>(ctx.test),
                    static_cast<unsigned>(gs->m_texa.ta0), static_cast<unsigned>(gs->m_texa.aem),
                    static_cast<unsigned>(gs->m_texa.ta1),
                    static_cast<unsigned long long>(below64),
                    static_cast<unsigned long long>(atLeast64),
                    static_cast<unsigned long long>(indexHist[0]),
                    static_cast<unsigned long long>(indexHist[173]),
                    static_cast<unsigned>(v0.a), static_cast<unsigned>(v1.a),
                    static_cast<unsigned>(v2.a));
                std::fflush(stderr);
            }
        }
    }

    // G87: streaks diagnosis. Dump the first rock triangles' SCREEN positions + texcoords
    // (FST/ST/Q + derived UV s/q,t/q) so we can tell horizontal-geometry-stretch from degenerate
    // texture coords. DC2_G87_UV=1, title scope (fbp!=0x139), rock tbps.
    {
        static const bool s_g87uv = (std::getenv("DC2_G87_UV") != nullptr);
        if (s_g87uv && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (k < 24u)
            {
                const int ox = ctx.xyoffset.ofx >> 4, oy = ctx.xyoffset.ofy >> 4;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                auto uvof = [&](const GSVertex &v, float &uu, float &vv) {
                    if (gs->m_prim.fst) { uu = v.u / 16.0f; vv = v.v / 16.0f; }
                    else { float q = (v.q != 0.0f) ? v.q : 1.0f; uu = v.s / q * tw; vv = v.t / q * th; }
                };
                float u0,vv0,u1,vv1,u2,vv2; uvof(v0,u0,vv0); uvof(v1,u1,vv1); uvof(v2,u2,vv2);
                std::fprintf(stderr,
                    "[G87:uv] k=%u tbp=0x%x fst=%u tw=%d th=%d  px0=(%.0f,%.0f) px1=(%.0f,%.0f) px2=(%.0f,%.0f)  "
                    "stq0=(%.4f,%.4f,%.4f) uv=(%.0f,%.0f)|(%.0f,%.0f)|(%.0f,%.0f)\n",
                    k, ctx.tex0.tbp0, (unsigned)gs->m_prim.fst, tw, th,
                    v0.x-ox, v0.y-oy, v1.x-ox, v1.y-oy, v2.x-ox, v2.y-oy,
                    v0.s, v0.t, v0.q, u0,vv0, u1,vv1, u2,vv2);
                std::fflush(stderr);
            }
        }
    }

    // G88: streaks. Compact per-triangle log of ALL title rock draws (no 24-cap, all rock
    // tbps) so a python pass can bucket per (prim,tbp): count / on-screen ratio / X-Y ranges
    // = the runner analogue of tools/g84_gs_geom.py on HW. DC2_G88_GEO=1, title scope.
    {
        static const bool s_g88geo = (std::getenv("DC2_G88_GEO") != nullptr);
        if (s_g88geo && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (k < 20000u)
            {
                const int ox = ctx.xyoffset.ofx >> 4, oy = ctx.xyoffset.ofy >> 4;
                std::fprintf(stderr, "[G88:geo] blk=%d prim=%u tbp=0x%x ofx=%d ofy=%d sc=[%u,%u,%u,%u] raw0=(%.0f,%.0f) raw1=(%.0f,%.0f) raw2=(%.0f,%.0f)\n",
                    g_dc2TitleCurBlock.load(std::memory_order_relaxed),
                    (unsigned)gs->m_prim.type, ctx.tex0.tbp0, ox, oy,
                    ctx.scissor.x0, ctx.scissor.x1, ctx.scissor.y0, ctx.scissor.y1,
                    v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
            }
        }
    }

    // G106: default-off title batch isolation. Drop one fully assembled title-rock texture
    // page and/or primitive class so we can identify which runner batch hides the HW mural.
    if (g106ShouldSkipTriangle(gs->m_prim.tme,
                               static_cast<uint32_t>(gs->m_prim.type),
                               ctx.frame.fbp,
                               ctx.tex0.tbp0))
    {
        if (g106SkipStatEnabled())
        {
            static std::atomic<uint64_t> s_skip{0};
            const uint64_t n = s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n & 0x3FFFull) == 0ull)
                std::fprintf(stderr,
                    "[G106:skip] n=%llu prim=%u tbp=0x%x fbp=0x%x\n",
                    static_cast<unsigned long long>(n),
                    static_cast<uint32_t>(gs->m_prim.type),
                    ctx.tex0.tbp0,
                    ctx.frame.fbp);
        }
        return;
    }

    // G89: GUARD-BAND CULL for the title rock copy path. The title rock is forced through the
    // copy packer (G78/G80, VU prog 0x1b68) which -- unlike the transform program (0x1d00, the
    // +2048 ADC gate) -- NEVER sets the per-vertex ADC bit. On HW ~60% of the rock tristrip verts
    // carry ADC=1 (drawing-kick disabled = strip restart), so verts that project off-screen never
    // form triangles. The runner's copy path emits ADC=0 for every vert, so the GS forms triangles
    // that connect on-screen verts to far-off-screen verts; the scissor then clips them into full-
    // width horizontal bands = the "green streaks". Root cause: the runner's GS rasterizer has no
    // guard-band primitive culling (a real GS feature). Replicate HW's ADC strip-restart by culling
    // any title-rock triangle that has a vertex far outside the drawing rect. The legitimate cavern
    // triangles span <=60px and sit on-screen; the streak triangles have verts hundreds of px past
    // the guard (e.g. screen-X -1760 and +2127 in the same tri). A guard of 512px keeps ~31% of the
    // rock triangles, matching HW's drawn fraction (tools/g84_gs_geom.py on new_game.gs: tri 31%,
    // tstrip 35%). NOTE this overturns G88's "4x world-matrix over-scale": the title LW matrix is
    // unit-scale on BOTH runner and HW (verified live, frame 0x981390 +0x70) and the per-prim screen
    // centroids match HW (~2050) -- the geometry is correctly positioned; only the off-screen strip
    // segments were wrongly drawn. Scoped to the title rock (tme + fbp!=0x139 + rock tbp range) so
    // dungeon/costume are untouched. Default-ON; kill DC2_G89_NO_GUARD_CULL; guard DC2_G89_GUARD.
    {
        // G139: RETIRED by default (was default-ON). The natural FMAND guard gate (G138) is the
        // real HW cull; the spatial guard approximation is obsolete (A/B g100_g139nb_keepz.png).
        // Re-enable with DC2_G89_FORCE_GUARD_CULL=1 (DC2_G89_GUARD still tunes the width).
        static const bool s_g89off = (std::getenv("DC2_G89_FORCE_GUARD_CULL") == nullptr) ||
                                     (std::getenv("DC2_G89_NO_GUARD_CULL") != nullptr);
        if (!g_g104TriClipReentry && !s_g89off && g144EffectiveTitleRockScope() &&
            gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static const float s_guard = []() -> float {
                const char *e = std::getenv("DC2_G89_GUARD");
                if (e) return static_cast<float>(std::atof(e));
                // G100: the native forced-draw route (default) draws the full transform
                // geometry, so the guard that approximates HW's ADC strip-restart must be
                // wider than the old copy-path 512 (which left blue triangular holes). 1024
                // fills the holes at the settled menu while still culling the far off-screen
                // strip segments. The reverted copy path (DC2_G100_NO_FORCEDRAW) keeps 512.
                return (std::getenv("DC2_G100_NO_FORCEDRAW") == nullptr) ? 1024.0f : 512.0f;
            }();
            const float ox = static_cast<float>(ctx.xyoffset.ofx >> 4);
            const float oy = static_cast<float>(ctx.xyoffset.ofy >> 4);
            const float loX = static_cast<float>(ctx.scissor.x0) - s_guard;
            const float hiX = static_cast<float>(ctx.scissor.x1) + s_guard;
            const float loY = static_cast<float>(ctx.scissor.y0) - s_guard;
            const float hiY = static_cast<float>(ctx.scissor.y1) + s_guard;
            const float sx0 = v0.x - ox, sy0 = v0.y - oy;
            const float sx1 = v1.x - ox, sy1 = v1.y - oy;
            const float sx2 = v2.x - ox, sy2 = v2.y - oy;
            if (sx0 < loX || sx0 > hiX || sy0 < loY || sy0 > hiY ||
                sx1 < loX || sx1 > hiX || sy1 < loY || sy1 > hiY ||
                sx2 < loX || sx2 > hiX || sy2 < loY || sy2 > hiY)
                return;

            // G101: cull the "vertex explosion" -- stretched spanning connector tris. HW restarts
            // the tristrip at off-screen verts (per-vertex ADC) so it never draws the long tris
            // that bridge an on-screen vert to an off-screen one; the runner's G100 forced-draw
            // has no per-vertex ADC, so those connectors draw as giant stretched triangles that
            // swim across the screen as the title camera pans. They are distinguishable from real
            // geometry: every FULLY on-screen rock tri has a short screen-space edge (<=256px in
            // the new_game reference), while the connectors have >=1 off-screen vert AND a long
            // edge. So: if a tri has any vert outside the scissor AND its longest screen-space edge
            // exceeds the threshold, drop it -- fully on-screen tris (any size) and compact edge
            // overhangs are kept. NOTE (G101 A/B): culling these connectors regresses to blue holes
            // because they also carry on-screen coverage -- explosion and coverage are the SAME
            // tris. Left as an OPT-IN diagnostic (DC2_G101_EDGE_CULL) pending the gate-natural fix;
            // tune DC2_G101_MAXEDGE.
            static const bool s_edgeOn = (std::getenv("DC2_G101_EDGE_CULL") != nullptr);
            if (s_edgeOn)
            {
                static const float s_maxEdge = []() -> float {
                    const char *e = std::getenv("DC2_G101_MAXEDGE");
                    return e ? static_cast<float>(std::atof(e)) : 384.0f;
                }();
                const float scX0 = static_cast<float>(ctx.scissor.x0);
                const float scX1 = static_cast<float>(ctx.scissor.x1);
                const float scY0 = static_cast<float>(ctx.scissor.y0);
                const float scY1 = static_cast<float>(ctx.scissor.y1);
                const bool spanning =
                    sx0 < scX0 || sx0 > scX1 || sy0 < scY0 || sy0 > scY1 ||
                    sx1 < scX0 || sx1 > scX1 || sy1 < scY0 || sy1 > scY1 ||
                    sx2 < scX0 || sx2 > scX1 || sy2 < scY0 || sy2 > scY1;
                if (spanning)
                {
                    const float d01x = sx0 - sx1, d01y = sy0 - sy1;
                    const float d12x = sx1 - sx2, d12y = sy1 - sy2;
                    const float d20x = sx2 - sx0, d20y = sy2 - sy0;
                    const float e01 = d01x * d01x + d01y * d01y;
                    const float e12 = d12x * d12x + d12y * d12y;
                    const float e20 = d20x * d20x + d20y * d20y;
                    float emax2 = e01 > e12 ? e01 : e12;
                    if (e20 > emax2) emax2 = e20;
                    if (emax2 > s_maxEdge * s_maxEdge)
                        return;
                }
            }
        }
    }

    const uint32_t g43Skip = g43SkipTbp();
    if (g43Skip != 0xFFFFFFFFu &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == g43Skip)
    {
        return;
    }

    // [F51.6:entry] Confirm whether drawTriangle is reached for the displayed buffer at all.
    if (renderQualityTraceEnabled())
    {
        static std::atomic<uint32_t> s_f516entry{0};
        const uint32_t n = s_f516entry.fetch_add(1u, std::memory_order_relaxed);
        if (n < 80u)
            std::fprintf(stderr, "[F51.6:entry] n=%u fbp=0x%x tme=%u type=%u ctxt=%u tbp=0x%x psm=0x%x\n",
                n, ctx.frame.fbp, (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.type,
                (unsigned)gs->m_prim.ctxt, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm);
    }

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    // G32: for the costume model tristrips (verts in the ~2000-unit packed range), report
    // the screen mapping so we can tell on-screen-but-occluded from scissored-off. Gated.
    if (renderQualityTraceEnabled() && gs->m_prim.type == GS_PRIM_TRISTRIP && v0.x > 1500.0f)
    {
        static std::atomic<uint32_t> s_g32tri{0};
        const uint32_t n = s_g32tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 400u)
            std::fprintf(stderr,
                "[G32:tri] n=%u fbp=0x%x fbw=%u psm=0x%x tme=%u abe=%u tbp=0x%x tfx=%u tcc=%u "
                "scis=(%u,%u)-(%u,%u) ofxy=(%d,%d) v0px=(%.1f,%.1f) v1px=(%.1f,%.1f) v2px=(%.1f,%.1f) "
                "rgba0=(%u,%u,%u,%u) rgba1=(%u,%u,%u,%u) rgba2=(%u,%u,%u,%u) "
                "z=(%.0f,%.0f,%.0f) zbuf(zbp=0x%x,zpsm=0x%x,zmsk=%u) test(zte=%u,ztst=%u)\n",
                n, ctx.frame.fbp, (unsigned)ctx.frame.fbw, (unsigned)ctx.frame.psm,
                (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.abe, ctx.tex0.tbp0,
                (unsigned)ctx.tex0.tfx, (unsigned)ctx.tex0.tcc,
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1, ofx, ofy,
                v0.x - (float)ofx, v0.y - (float)ofy, v1.x - (float)ofx, v1.y - (float)ofy,
                v2.x - (float)ofx, v2.y - (float)ofy,
                (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                (unsigned)v1.r, (unsigned)v1.g, (unsigned)v1.b, (unsigned)v1.a,
                (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a,
                v0.z, v1.z, v2.z,
                (unsigned)(ctx.zbuf & 0x1FFu), (unsigned)((ctx.zbuf >> 24) & 0xFu),
                (unsigned)((ctx.zbuf >> 32) & 0x1u),
                (unsigned)((ctx.test >> 16) & 0x1u), (unsigned)((ctx.test >> 17) & 0x3u));
    }

    const bool g43FaceUv =
        g43FaceUvTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        (ctx.tex0.tbp0 == 0x3460u || ctx.tex0.tbp0 == 0x3480u) &&
        gs->m_prim.type == GS_PRIM_TRISTRIP &&
        v0.x > 1500.0f;
    if (g43FaceUv)
    {
        static std::atomic<uint32_t> s_g43tri{0};
        const uint32_t n = s_g43tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 220u)
        {
            std::fprintf(stderr,
                "[G43:triuv] n=%u fbp=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x csa=%u fst=%u tfx=%u tcc=%u "
                "px0=(%.1f,%.1f) px1=(%.1f,%.1f) px2=(%.1f,%.1f) "
                "uv0=(%u,%u) uv1=(%u,%u) uv2=(%u,%u) "
                "stq0=(%.5g,%.5g,%.5g) stq1=(%.5g,%.5g,%.5g) stq2=(%.5g,%.5g,%.5g) "
                "rgba0=(%u,%u,%u,%u) rgba1=(%u,%u,%u,%u) rgba2=(%u,%u,%u,%u)\n",
                n, ctx.frame.fbp, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                ctx.tex0.cbp, (unsigned)ctx.tex0.cpsm, (unsigned)ctx.tex0.csa,
                (unsigned)gs->m_prim.fst, (unsigned)ctx.tex0.tfx, (unsigned)ctx.tex0.tcc,
                v0.x - (float)ofx, v0.y - (float)ofy,
                v1.x - (float)ofx, v1.y - (float)ofy,
                v2.x - (float)ofx, v2.y - (float)ofy,
                (unsigned)(v0.u >> 4), (unsigned)(v0.v >> 4),
                (unsigned)(v1.u >> 4), (unsigned)(v1.v >> 4),
                (unsigned)(v2.u >> 4), (unsigned)(v2.v >> 4),
                v0.s, v0.t, v0.q, v1.s, v1.t, v1.q, v2.s, v2.t, v2.q,
                (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                (unsigned)v1.r, (unsigned)v1.g, (unsigned)v1.b, (unsigned)v1.a,
                (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a);
        }
    }

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int rawMinX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int rawMaxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int rawMinY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int rawMaxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    int minX = clampInt(rawMinX, ctx.scissor.x0, ctx.scissor.x1);
    int maxX = clampInt(rawMaxX, ctx.scissor.x0, ctx.scissor.x1);
    int minY = clampInt(rawMinY, ctx.scissor.y0, ctx.scissor.y1);
    int maxY = clampInt(rawMaxY, ctx.scissor.y0, ctx.scissor.y1);
    // G144: during a tile-binning flush, restrict this triangle to the calling lane's scanline band
    // (intersect the bbox with [BandY0,BandY1]). Everything downstream (x-span, y-loop, Z, write)
    // then operates only on in-band rows, so lanes touch disjoint pixels. If the intersection is
    // empty this entry does not cover the band and the loops run zero iterations.
    if (t_g144Banded)
    {
        if (minY < t_g144BandY0) minY = t_g144BandY0;
        if (maxY > t_g144BandY1) maxY = t_g144BandY1;
    }

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);

    // G219 (default-off, DC2_G219_SKYPX=1): triangle-level probe for the sky dome (tbp=0x2820)
    // on the display targets — logs every such triangle that reaches the raster stage with its
    // clamped bbox, so a zero-pixel result in the black region can be split into "triangle never
    // got here" vs "bbox/span clipped it".
    {
        static const bool s_g219t = (std::getenv("DC2_G219_SKYPX") != nullptr);
        // G219: raster-time fingerprint pair for the [G219:cap] capture-time one above.
        if (s_g219t && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            gs->m_vram && 0x272000u + 0x8000u <= gs->m_vramSize)
        {
            static std::atomic<uint32_t> s_g219rn{0};
            const uint32_t n = s_g219rn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
            {
                uint64_t sum = 0;
                const uint64_t *p = reinterpret_cast<const uint64_t *>(gs->m_vram + 0x272000u);
                for (uint32_t i = 0; i < 0x1000u; ++i)
                    sum += p[i];
                std::fprintf(stderr, "[G219:ras] n=%u fbp=0x%x sum=%016llx replay=%u banded=%u\n",
                             n, ctx.frame.fbp, (unsigned long long)sum,
                             t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u);
                // Diff vs the capture-time snapshot: the changed offsets identify the clobbering
                // writer (color-page vs Z-page vs copy stride patterns).
                extern uint8_t g_g219Snap[0x8000];
                const uint64_t *q = reinterpret_cast<const uint64_t *>(g_g219Snap);
                uint32_t shown = 0, diffs = 0;
                for (uint32_t i = 0; i < 0x1000u; ++i)
                {
                    if (p[i] != q[i])
                    {
                        ++diffs;
                        if (shown < 6u)
                        {
                            std::fprintf(stderr,
                                "[G219:dif] off=0x%05x old=%016llx new=%016llx\n",
                                0x272000u + i * 8u, (unsigned long long)q[i], (unsigned long long)p[i]);
                            ++shown;
                        }
                    }
                }
                if (diffs)
                    std::fprintf(stderr, "[G219:dif] totalDiffQwords=%u of 4096\n", diffs);
            }
        }
        if (s_g219t &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            rawMaxX >= 440 && rawMinY <= 100 && (rawMaxX - rawMinX) > 60)
        {
            static std::atomic<uint32_t> s_g219tn{0};
            const uint32_t n = s_g219tn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 300u || (n % 256u) == 0u)
                std::fprintf(stderr,
                    "[G219:skytri] n=%u fbp=0x%x prim=%u tme=%u tbp=0x%x bbox=(%d,%d)-(%d,%d) "
                    "raw=(%d,%d)-(%d,%d) denom=%g replay=%u banded=%u band=(%d,%d) "
                    "px=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) q=(%.4g,%.4g,%.4g)\n",
                    n, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY,
                    denom, t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u,
                    t_g144BandY0, t_g144BandY1,
                    fx0, fy0, fx1, fy1, fx2, fy2, v0.q, v1.q, v2.q);
        }
    }

    static const bool s_g195FgTrace = envFlagEnabled("DC2_G195_FG_TRACE");
    if ((g268LiveEnvRead() ? envFlagEnabled("DC2_G195_FG_TRACE") : s_g195FgTrace) &&
        gs->m_vram &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == 0x2a20u &&
        (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u))
    {
        static std::atomic<uint32_t> s_g195FgTri{0};
        const uint32_t n = s_g195FgTri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 256u || (n % 512u) == 0u)
        {
            std::fprintf(stderr,
                "[G195:fgtri] n=%u blk=%d fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u abe=%u "
                "tex0=0x%016llx tbw=%u psm=0x%x tw=%u th=%u tfx=%u tcc=%u cbp=0x%x cpsm=0x%x csa=%u "
                "alpha=0x%llx test=0x%llx zbuf=0x%llx tex1=0x%llx clamp=0x%llx "
                "bbox=(%d,%d)-(%d,%d) rawbbox=(%d,%d)-(%d,%d) denom=%g of=(%d,%d) "
                "v0=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u) "
                "v1=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u) "
                "v2=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u)\n",
                n, g_dc2TitleCurBlock.load(std::memory_order_relaxed),
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.iip),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
                static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.psm),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                static_cast<uint32_t>(ctx.tex0.tfx), static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                static_cast<uint32_t>(ctx.tex0.csa),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY, denom, ofx, ofy,
                fx0, fy0, v0.z, static_cast<uint32_t>(v0.r), static_cast<uint32_t>(v0.g),
                static_cast<uint32_t>(v0.b), static_cast<uint32_t>(v0.a),
                v0.s, v0.t, v0.q, static_cast<uint32_t>(v0.u), static_cast<uint32_t>(v0.v),
                fx1, fy1, v1.z, static_cast<uint32_t>(v1.r), static_cast<uint32_t>(v1.g),
                static_cast<uint32_t>(v1.b), static_cast<uint32_t>(v1.a),
                v1.s, v1.t, v1.q, static_cast<uint32_t>(v1.u), static_cast<uint32_t>(v1.v),
                fx2, fy2, v2.z, static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a),
                v2.s, v2.t, v2.q, static_cast<uint32_t>(v2.u), static_cast<uint32_t>(v2.v));
        }
    }

    if (g195DofTraceEnabled() &&
        gs->m_vram &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 >= 0x2720u &&
        ctx.tex0.tbp0 <= 0x3a00u &&
        ctx.frame.fbp != 0x139u)
    {
        struct G195State
        {
            uint32_t fbp, fbw, tbp, tbw, cbp;
            uint8_t fpsm, psm, tfx, tcc, tw, th, cpsm, csm, csa;
            uint8_t prim, fst, iip, abe;
            uint64_t alpha, test, tex1, clamp;
            uint32_t hits;
        };
        static std::mutex s_stateMutex;
        static G195State s_seen[256]{};
        static uint32_t s_seenN = 0;
        const G195State key{
            ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw), ctx.tex0.tbp0,
            static_cast<uint32_t>(ctx.tex0.tbw), ctx.tex0.cbp,
            static_cast<uint8_t>(ctx.frame.psm), static_cast<uint8_t>(ctx.tex0.psm),
            static_cast<uint8_t>(ctx.tex0.tfx), static_cast<uint8_t>(ctx.tex0.tcc),
            static_cast<uint8_t>(ctx.tex0.tw), static_cast<uint8_t>(ctx.tex0.th),
            static_cast<uint8_t>(ctx.tex0.cpsm), static_cast<uint8_t>(ctx.tex0.csm),
            static_cast<uint8_t>(ctx.tex0.csa), static_cast<uint8_t>(gs->m_prim.type),
            static_cast<uint8_t>(gs->m_prim.fst), static_cast<uint8_t>(gs->m_prim.iip),
            static_cast<uint8_t>(gs->m_prim.abe), ctx.alpha, ctx.test, ctx.tex1, ctx.clamp, 0u
        };
        uint32_t idx = 0xFFFFFFFFu;
        bool firstHit = false;
        uint32_t hits = 0;
        {
            std::lock_guard<std::mutex> lk(s_stateMutex);
            for (uint32_t i = 0; i < s_seenN; ++i)
            {
                const G195State &s = s_seen[i];
                if (s.fbp == key.fbp && s.fbw == key.fbw && s.tbp == key.tbp &&
                    s.tbw == key.tbw && s.cbp == key.cbp && s.fpsm == key.fpsm &&
                    s.psm == key.psm && s.tfx == key.tfx && s.tcc == key.tcc &&
                    s.tw == key.tw && s.th == key.th && s.cpsm == key.cpsm &&
                    s.csm == key.csm && s.csa == key.csa && s.prim == key.prim &&
                    s.fst == key.fst && s.iip == key.iip && s.abe == key.abe &&
                    s.alpha == key.alpha && s.test == key.test && s.tex1 == key.tex1 &&
                    s.clamp == key.clamp)
                {
                    idx = i;
                    break;
                }
            }
            if (idx == 0xFFFFFFFFu && s_seenN < 256u)
            {
                idx = s_seenN++;
                s_seen[idx] = key;
                firstHit = true;
            }
            if (idx != 0xFFFFFFFFu)
                hits = ++s_seen[idx].hits;
        }
        if (idx != 0xFFFFFFFFu && (firstHit || hits == 2u || hits == 4u || hits == 16u || hits == 256u))
        {
            std::fprintf(stderr,
                "[G195:state] k=%u hits=%u fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u abe=%u "
                "tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u tw=%u th=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "alpha=0x%llx test=0x%llx tex1=0x%llx clamp=0x%llx bbox=(%d,%d)-(%d,%d) "
                "rawbbox=(%d,%d)-(%d,%d) rgba2=(%u,%u,%u,%u)\n",
                idx, hits, key.fbp, key.fbw, static_cast<uint32_t>(key.fpsm),
                static_cast<uint32_t>(key.prim), static_cast<uint32_t>(key.fst),
                static_cast<uint32_t>(key.iip), static_cast<uint32_t>(key.abe),
                key.tbp, key.tbw, static_cast<uint32_t>(key.psm),
                static_cast<uint32_t>(key.tfx), static_cast<uint32_t>(key.tcc),
                static_cast<uint32_t>(key.tw), static_cast<uint32_t>(key.th), key.cbp,
                static_cast<uint32_t>(key.cpsm), static_cast<uint32_t>(key.csm),
                static_cast<uint32_t>(key.csa), static_cast<unsigned long long>(key.alpha),
                static_cast<unsigned long long>(key.test), static_cast<unsigned long long>(key.tex1),
                static_cast<unsigned long long>(key.clamp),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY,
                static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a));
        }
    }

    const bool g195DofTri =
        g195DofTraceEnabled() &&
        gs->m_vram &&
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        ctx.tex0.tbp0 >= 0x2720u &&
        ctx.tex0.tbp0 <= 0x2a80u &&
        ctx.tex0.psm == GS_PSM_T8 &&
        ctx.tex0.tcc == 1u &&
        ctx.tex0.cbp >= 0x3fb0u &&
        ctx.tex0.cbp <= 0x3fbcu &&
        (ctx.alpha == 0x44u || ctx.alpha == 0x48u || ctx.alpha == 0x800000002au) &&
        (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u);
    if (g195DofTri)
    {
        static std::atomic<uint32_t> s_tri{0};
        const uint32_t n = s_tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 96u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                "[G195:doftri] n=%u fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u "
                "tex0=0x%016llx tbw=%u tw=%u th=%u tex1=0x%llx clamp=0x%llx alpha=0x%llx test=0x%llx "
                "bbox=(%d,%d)-(%d,%d) rawbbox=(%d,%d)-(%d,%d) denom=%g "
                "v0=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u) "
                "v1=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u) "
                "v2=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u)\n",
                n, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.iip),
                static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
                static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.tw),
                static_cast<uint32_t>(ctx.tex0.th),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY, denom,
                fx0, fy0, v0.z, static_cast<uint32_t>(v0.r), static_cast<uint32_t>(v0.g),
                static_cast<uint32_t>(v0.b), static_cast<uint32_t>(v0.a),
                v0.s, v0.t, v0.q, static_cast<uint32_t>(v0.u), static_cast<uint32_t>(v0.v),
                fx1, fy1, v1.z, static_cast<uint32_t>(v1.r), static_cast<uint32_t>(v1.g),
                static_cast<uint32_t>(v1.b), static_cast<uint32_t>(v1.a),
                v1.s, v1.t, v1.q, static_cast<uint32_t>(v1.u), static_cast<uint32_t>(v1.v),
                fx2, fy2, v2.z, static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a),
                v2.s, v2.t, v2.q, static_cast<uint32_t>(v2.u), static_cast<uint32_t>(v2.v));
        }

        bool doDump = false;
        {
            static std::mutex s_dumpMutex;
            static uint64_t s_dumpedKeys[32]{};
            static uint32_t s_dumpedN = 0;
            const uint64_t dumpKey =
                (static_cast<uint64_t>(ctx.tex0.tbp0) << 40) |
                (static_cast<uint64_t>(ctx.tex0.tbw & 0x3fu) << 32) |
                (static_cast<uint64_t>(ctx.tex0.cbp & 0x3fffu) << 16) |
                (static_cast<uint64_t>(ctx.tex0.tw & 0xfu) << 8) |
                static_cast<uint64_t>(ctx.tex0.th & 0xfu);
            std::lock_guard<std::mutex> lk(s_dumpMutex);
            bool seen = false;
            for (uint32_t i = 0; i < s_dumpedN; ++i)
            {
                if (s_dumpedKeys[i] == dumpKey)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen && s_dumpedN < 32u)
            {
                s_dumpedKeys[s_dumpedN++] = dumpKey;
                doDump = true;
            }
        }
        if (doDump)
        {
            const int dw = 1 << ctx.tex0.tw;
            const int dh = 1 << ctx.tex0.th;
            const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
            char rgbPath[128];
            char alphaPath[128];
            std::snprintf(rgbPath, sizeof(rgbPath), "D:/ps2r/dc2/captures/g195_dof_%04x_rgb.ppm", ctx.tex0.tbp0);
            std::snprintf(alphaPath, sizeof(alphaPath), "D:/ps2r/dc2/captures/g195_dof_%04x_alpha.ppm", ctx.tex0.tbp0);
            FILE *rgb = std::fopen(rgbPath, "wb");
            FILE *alpha = std::fopen(alphaPath, "wb");
            if (rgb) std::fprintf(rgb, "P6\n%d %d\n255\n", dw, dh);
            if (alpha) std::fprintf(alpha, "P6\n%d %d\n255\n", dw, dh);
            uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            uint32_t nzRgb = 0, nzA = 0, idx0 = 0, idxNon0 = 0, a80 = 0, a7fPlus = 0;
            for (int yy = 0; yy < dh; ++yy)
            {
                for (int xx = 0; xx < dw; ++xx)
                {
                    const uint8_t ix = static_cast<uint8_t>(
                        GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                      static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                                  ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                    const uint8_t rr = static_cast<uint8_t>(c & 0xFFu);
                    const uint8_t gg = static_cast<uint8_t>((c >> 8) & 0xFFu);
                    const uint8_t bb = static_cast<uint8_t>((c >> 16) & 0xFFu);
                    const uint8_t aa = static_cast<uint8_t>((c >> 24) & 0xFFu);
                    sumR += rr; sumG += gg; sumB += bb; sumA += aa;
                    if ((rr | gg | bb) != 0u) ++nzRgb;
                    if (aa != 0u) ++nzA;
                    if (ix == 0u) ++idx0; else ++idxNon0;
                    if (aa == 0x80u) ++a80;
                    if (aa >= 0x7fu) ++a7fPlus;
                    if (rgb)
                    {
                        std::fputc(rr, rgb); std::fputc(gg, rgb); std::fputc(bb, rgb);
                    }
                    if (alpha)
                    {
                        std::fputc(aa, alpha); std::fputc(aa, alpha); std::fputc(aa, alpha);
                    }
                }
            }
            if (rgb) std::fclose(rgb);
            if (alpha) std::fclose(alpha);
            const uint32_t pixels = static_cast<uint32_t>(dw * dh);
            std::fprintf(stderr,
                "[G195:dofdump] tex=0x%x size=%dx%d tbw=%u pageBw=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "avg=(%llu,%llu,%llu,%llu) nzRgb=%u/%u nzA=%u/%u idx0=%u idxNon0=%u a80=%u a7fPlus=%u "
                "files=%s,%s\n",
                ctx.tex0.tbp0, dw, dh, static_cast<uint32_t>(ctx.tex0.tbw), pageBwT8, ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm), static_cast<uint32_t>(ctx.tex0.csm),
                static_cast<uint32_t>(ctx.tex0.csa),
                pixels ? static_cast<unsigned long long>(sumR / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumG / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumB / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumA / pixels) : 0ull,
                nzRgb, pixels, nzA, pixels, idx0, idxNon0, a80, a7fPlus,
                rgbPath, alphaPath);
        }
    }

    // G50: tally costume-model triangles per texture page (drawn vs degenerate/dropped + bbox).
    if (g50PartsEnabled() && ctx.frame.fbp == 0x139u && gs->m_prim.tme)
    {
        g50Record(ctx.tex0.tbp0, std::fabs(denom) < 0.001f,
                  fx0, fy0, fx1, fy1, fx2, fy2);
        g50DumpMaybe();
    }

    // G214: per-tick head-page triangle counter (see g214CapFlushLocked).
    if (g214CapTraceEnabled() && ctx.frame.fbp == 0x139u && gs->m_prim.tme)
    {
        extern std::atomic<uint64_t> g_dc2PresentTick; // defined in ps2_runtime.cpp
        const uint64_t tk = g_dc2PresentTick.load(std::memory_order_relaxed);
        const uint32_t tbp = ctx.tex0.tbp0;
        std::lock_guard<std::mutex> lk(g_g214Mutex);
        if (tk != g_g214Tick)
            g214CapFlushLocked(tk);
        switch (tbp)
        {
        case 0x35a0u: g_g214Cap++;  break;
        case 0x3420u: g_g214Face++; break;
        case 0x34c0u: g_g214Hair++; break;
        case 0x3480u: g_g214Head++; break;
        case 0x3460u: g_g214Body++; break;
        case 0x34a0u: g_g214Neck++; break;
        default: break;
        }
    }

    // [F51.7] Global textured-triangle classifier: where does textured 3D geometry go, and does it
    // rasterize? Logs raw (pre-offset) GS vertex coords so a collapsed transform is visible.
    // Gated DC2_TRACE_RENDER_QUALITY.
    const bool f516 = renderQualityTraceEnabled();
    const bool f53map = f516 &&
                        gs->m_prim.tme &&
                        ctx.tex0.tbp0 == 0x3220u &&
                        ctx.tex0.psm == 0x13u;
    static std::atomic<uint32_t> s_f53MapTotal{0};
    static std::atomic<uint32_t> s_f53MapPositive{0};
    static std::atomic<uint32_t> s_f53MapZero{0};
    static std::atomic<uint32_t> s_f53MapDegen{0};
    static std::atomic<uint32_t> s_f53MapSummary{0};
    int f516cov = 0;
    if (std::fabs(denom) < 0.001f)
    {
        if (f53map)
        {
            const uint32_t total = s_f53MapTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint32_t degen = s_f53MapDegen.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (degen <= 16u)
                std::fprintf(stderr,
                    "[F53:tri] total=%u class=degen degen=%u fbp=0x%x denom=%g "
                    "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) "
                    "of=(%d,%d)\n",
                    total, degen, ctx.frame.fbp, denom,
                    v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                    ofx, ofy);
        }
        if (f516)
        {
            static std::atomic<uint32_t> s_f517deg{0};
            if (s_f517deg.fetch_add(1u) < 60u)
                std::fprintf(stderr,
                    "[F51.7:tri] DEGEN fbp=0x%x tme=%u tbp=0x%x psm=0x%x fst=%u denom=%g "
                    "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) "
                    "st0=(%.3f,%.3f) of=(%d,%d) scissor=(%u,%u)-(%u,%u)\n",
                    ctx.frame.fbp, (unsigned)gs->m_prim.tme, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                    (unsigned)gs->m_prim.fst, denom,
                    v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                    v0.s, v0.t, ofx, ofy,
                    ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
        return;
    }

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // [G42] EXPERIMENT: backface cull for the costume model RTT (fbp=0x139). The head/cap
    // renders as a "hollow face" because the concave cap's back-facing interior triangles draw
    // over the front of the face and the GEQUAL Z-test cannot separate them (back-cap Z competes
    // with face Z). PS2 culls back faces in the VU1 microprogram; the runner emits both windings.
    // DC2_G42_CULL=1 culls denom<0, =2 culls denom>0 (try both to find the back-facing sign).
    static const int s_g42cull = []() {
        const char *e = std::getenv("DC2_G42_CULL");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g42cull && ctx.frame.fbp == 0x139u && gs->m_prim.type == GS_PRIM_TRISTRIP)
    {
        if ((s_g42cull == 1 && denom < 0.0f) || (s_g42cull == 2 && denom > 0.0f))
            return;
    }

    // [G51] PARITY-AWARE backface cull for the costume model (fbp=0x139). A naive fixed-winding
    // cull (G42) shreds the model because a TRISTRIP's geometric winding ALTERNATES every triangle
    // (the GS/VU1 flips the cull test per strip-triangle). The dark face "speckle" dots are the
    // head's BACK-FACING tristrip triangles (far side of the skinned head surface) bleeding through
    // the front skin's sub-pixel coverage gaps: their interpolated vertex shade is dark (~0x38-0x60
    // vs front skin ~0x96) and their q is tiny (far) — see the [G43:sample] trace. They are NOT a
    // texture/CLUT/filtering defect (the decoded 0x3480 texture is clean; a 7x7 texel box-average
    // does not remove the dots) and NOT the decal pages (0x34a0/0x35a0 skip does not remove them).
    // Cull by the PARITY-NORMALIZED winding so all front faces share one sign. Strip parity is
    // derived header-free from vertex continuity: a strip continuation re-uses the previous
    // triangle's last two vertices as its first two (the sliding window in ps2_gs_gpu.cpp).
    static const int s_g51cull = []() {
        const char *e = std::getenv("DC2_G51_CULL");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g51cull && ctx.frame.fbp == 0x139u && gs->m_prim.type == GS_PRIM_TRISTRIP)
    {
        static float s_pv1x = 0.0f, s_pv1y = 0.0f, s_pv2x = 0.0f, s_pv2y = 0.0f;
        static bool s_have = false;
        static int s_parity = 0;
        const bool cont = s_have && v0.x == s_pv1x && v0.y == s_pv1y &&
                          v1.x == s_pv2x && v1.y == s_pv2y;
        s_parity = cont ? (s_parity ^ 1) : 0;
        s_pv1x = v1.x; s_pv1y = v1.y; s_pv2x = v2.x; s_pv2y = v2.y; s_have = true;
        const float adj = winding * (s_parity ? -1.0f : 1.0f);
        if ((s_g51cull == 1 && adj < 0.0f) || (s_g51cull == 2 && adj > 0.0f))
            return;
    }

    // G41: per-pixel depth test. The PS2 GS depth-tests every primitive via TEST.ZTE/ZTST
    // and reads/writes the Z buffer at ZBUF.ZBP. The rasterizer previously ignored Z entirely
    // (painter's order), so overlapping skinned-model tristrips drew back-over-front (the costume
    // Max "hollow face" — back of cap/hair showed through the front). Honor it now.
    // Decode once: TEST.ZTE=bit16, ZTST=bits17-18; ZBUF.ZBP=bits0-8, ZPSM=bits24-27, ZMSK=bit32.
    static const bool s_zTestDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const bool g203Uni = g203UniversalZEnabled();
    const bool g106TitleZ =
        g106TitleZScope(gs->m_prim.tme, ctx.frame.fbp, ctx.tex0.tbp0);
    // G144: skip the fbp-transition clear during a parallel replay (it uses a racy file-static
    // last-fbp and would spuriously wipe the shared title-Z mid-band). The flush pre-runs it once,
    // single-threaded, before dispatching the bands.
    // G203: in universal-Z mode the game's own clear-sprite populates VRAM Z — no artificial clear.
    if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled() && !t_g144InReplay)
        g106TitleZClearIfNeeded(ctx.frame.fbp);
    const uint32_t zte  = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    // ZBUF.ZBP is a page base; GSMem Z addressing wants a block base (page<<5), matching
    // writePixel's framePageBaseToBlock(fbp) convention.
    const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
    const uint32_t zbp  = zbpPage << 5;
    const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool zmsk     = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const uint32_t zbw  = ctx.frame.fbw;
    const bool g202TownZ = g202TownZScope(ctx.frame.fbp, zbpPage, zpsm);
    if (!g203Uni && g202TownZ && !t_g144InReplay)
        g202TownZClearIfNeeded(gs->m_vram, ctx.frame.fbp, ctx.zbuf, ctx.frame.fbw);
    // G203: universal-Z honors guest ZBUF/TEST for ALL targets against real VRAM Z; the legacy path
    // kept the per-screen whitelist (costume 0x139, title-rock, town state-scope).
    const bool zScope = g203Uni || (ctx.frame.fbp == 0x139u) || g106TitleZ || g202TownZ;
    const bool zEnabled = !s_zTestDisabled && zScope && zte != 0u && gs->m_vram != nullptr;
    const bool z16 = (zpsm == 0x2u || zpsm == 0xAu || zpsm == 0xBu); // PSMZ16 / PSMZ16S
    const bool z24 = (zpsm == 0x1u); // PSMZ24
    const bool zWrite = zEnabled && !zmsk && ztst != 0u; // ZTST=NEVER never writes
    // G219 (default-off, DC2_G219_SKYPX=1): triangle twin of the [G219:spzbp] sprite watch.
    {
        static const bool s_g219z = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219z && zWrite && zbpPage >= 0x139u && zbpPage <= 0x148u)
        {
            static std::atomic<uint32_t> s_g219tzn{0};
            const uint32_t n = s_g219tzn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
                std::fprintf(stderr,
                    "[G219:trzbp] n=%u fbp=0x%x zbpPage=0x%x zpsm=0x%x ztst=%u tme=%u tbp=0x%x "
                    "bbox=(%d,%d)-(%d,%d) replay=%u\n",
                    n, ctx.frame.fbp, zbpPage, zpsm, ztst,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    minX, minY, maxX, maxY, t_g144InReplay ? 1u : 0u);
        }
    }
    const bool g44FaceZ =
        g44FaceZTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        gs->m_prim.type == GS_PRIM_TRISTRIP &&
        g44FaceZTargetTbp(ctx.tex0.tbp0) &&
        maxX >= 344 && minX <= 412 &&
        maxY >= 40 && minY <= 144;

    // G141 sub-profile: local coverage tally (flushed to the globals once after the loop, so no
    // per-pixel atomic). bbox area vs inside-count exposes bounding-box scan waste.
    long g141BboxPx = (maxX >= minX && maxY >= minY)
                          ? static_cast<long>(maxX - minX + 1) * static_cast<long>(maxY - minY + 1)
                          : 0L;
    long g141InsidePx = 0;

    // [G232] consume the gem-draw arm set by drawPrimitive for this same primitive.
    // s_g232w short-circuits first so the default path pays no atomics here.
    static const bool s_g232w = (std::getenv("DC2_G232_RTTDUMP") != nullptr);
    const bool g232tri = s_g232w && g_g232TriArm.exchange(0, std::memory_order_relaxed) != 0;
    if (g232tri && g141BboxPx > 0)
        g_g232PxBbox.fetch_add(static_cast<uint32_t>(g141BboxPx), std::memory_order_relaxed);
    const int g232wx = s_g232w ? g_g232GemX.load(std::memory_order_relaxed) : -1;
    const int g232wy = s_g232w ? g_g232GemY.load(std::memory_order_relaxed) : -1;
    // Watch every triangle touching the gem pixel on ANY target sharing the game's single Z
    // page (0xd0) — room draws (fbp 0/0x68) and character RTT draws (0x139) alike.
    const bool g232watch = s_g232w && g232wx >= 0 && zbpPage == 0xd0u;

    // G141 (eliminate-work, behavior-identical): on each scanline the inside set is a contiguous
    // interval (convex triangle + affine barycentric), so narrow the x scan to a conservative span
    // derived from the SAME edge functions instead of edge-testing every bbox pixel (measured ~70%
    // of per-pixel work was wasted on outside pixels). The per-pixel EXACT float test in the inner
    // loop is UNCHANGED; the span is computed in double and padded by floor/ceil ±1px so no pixel
    // the exact test would accept is ever skipped -> output is bit-identical (golden 211646 /
    // frame_001500.ppm byte-equal). Per-x slope of each barycentric = its px coefficient. Kill
    // with DC2_G141_NO_XSPAN=1 to restore the full-bbox scan for A/B.
    static const bool s_g141NoXspan = (std::getenv("DC2_G141_NO_XSPAN") != nullptr);
    // G141 cost-split PROBES (measurement levers, default off, render garbage when on — used only to
    // apportion the ~170 ms GS raster between the per-pixel texture sample and the pixel write before
    // refactoring either). DC2_G141_SKIP_SAMPLE bypasses sampleTexture; DC2_G141_SKIP_WRITE bypasses
    // writePixel. Delta in [G141:perf] GSraster ms = that stage's cost. Never ship enabled.
    static const bool s_g141SkipSample = (std::getenv("DC2_G141_SKIP_SAMPLE") != nullptr);
    static const bool s_g141SkipWrite = (std::getenv("DC2_G141_SKIP_WRITE") != nullptr);
    const double g141S0 = static_cast<double>(fy1 - fy2) * static_cast<double>(winding) * static_cast<double>(invAbsDenom);
    const double g141S1 = static_cast<double>(fy2 - fy0) * static_cast<double>(winding) * static_cast<double>(invAbsDenom);
    const double g141S2 = -g141S0 - g141S1;
    // G141 (drawn-pixel lever, user-approved SAFE inline+hoist): the ~68 ms/frame texture sampler
    // (skip-probe = 40% of GS raster) is a NON-inlined sampleTexture call per drawn pixel that
    // re-derefs activeContext() and re-decodes texW/clamp/wrap/psm every pixel (all per-triangle
    // invariant). For the dominant cavern case (textured, non-RTT, POINT sampling, traces off,
    // common PSM) run an inlined fast-path that hoists those invariants and reuses the EXACT leaf
    // reads (ReadP8/readTexel*/lookupCLUT/applyTexa/wrapTextureCoordinate) -> bit-identical by
    // construction. Everything else (bilinear, costume RTT fbp=0x139, Z-as-texture, non-default T8
    // config, any trace) FALLS BACK to sampleTexture. Kill DC2_G141_NO_FASTSAMPLE=1. Bit-exactness
    // is PROVEN per-pixel with DC2_G141_SAMPLE_VERIFY=1 ([G141:vfy] tot/bad; bad must stay 0).
    static const bool s_g141NoFastSample = (std::getenv("DC2_G141_NO_FASTSAMPLE") != nullptr);
    static const bool s_g141SampleVerify = (std::getenv("DC2_G141_SAMPLE_VERIFY") != nullptr);
    static const bool s_g141T8GsmemDefault =
        !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
    static const bool s_g141T8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);
    const uint8_t g141Psm = ctx.tex0.psm;
    const bool g141FastPsm =
        g141Psm == GS_PSM_CT32 || g141Psm == GS_PSM_CT24 ||
        g141Psm == GS_PSM_CT16 || g141Psm == GS_PSM_CT16S ||
        g141Psm == GS_PSM_T4 || g141Psm == GS_PSM_T4HL || g141Psm == GS_PSM_T4HH ||
        (g141Psm == GS_PSM_T8 && s_g141T8GsmemDefault && !s_g141T8AliasSet);
    const int g141TexW = 1 << ctx.tex0.tw;
    const int g141TexH = 1 << ctx.tex0.th;
    const uint8_t g141WrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t g141WrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    const int g141MinU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
    const int g141MaxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
    const int g141MinV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
    const int g141MaxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
    const bool g141TraceOff =
        !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() && !f50_12_trace_enabled();
    // The cavern is BILINEAR (mmag=1) — sampleTexture's 4-tap path is the bulk of the 68 ms — so the
    // fast path covers both POINT and BILINEAR (g141Linear), matching sampleTexture's own selection
    // (for fbp!=0x139: point iff !tex1UsesLinearFilter, else bilinear; box-filter/g45ForceLinear are
    // fbp==0x139-only and excluded here).
    const bool g141Linear = tex1UsesLinearFilter(ctx.tex1);
    const bool g141FastSampleTri =
        !s_g141NoFastSample && gs->m_prim.tme && g141TraceOff &&
        ctx.frame.fbp != 0x139u && g141FastPsm && ctx.tex0.tbw != 0u;
    // G141 one-shot: report WHY textured non-RTT tris do/don't take the fast path (DC2_G141_FASTDIAG).
    {
        static const bool s_g141FastDiag = (std::getenv("DC2_G141_FASTDIAG") != nullptr);
        if (s_g141FastDiag && gs->m_prim.tme && ctx.frame.fbp != 0x139u)
        {
            static std::atomic<uint32_t> s_fd{0};
            const uint32_t k = s_fd.fetch_add(1u, std::memory_order_relaxed);
            if (k < 24u)
                std::fprintf(stderr,
                    "[G141:fastdiag] k=%u fbp=0x%x psm=0x%x tbw=%u tex1=0x%llx mmag=%u mmin=%u "
                    "fastPsm=%u linear=%u tbw0=%u traceOff=%u -> fast=%u\n",
                    k, ctx.frame.fbp, (unsigned)g141Psm, (unsigned)ctx.tex0.tbw,
                    (unsigned long long)ctx.tex1,
                    (unsigned)((ctx.tex1 >> 5) & 0x1u), (unsigned)((ctx.tex1 >> 6) & 0x7u),
                    (unsigned)g141FastPsm, (unsigned)tex1UsesLinearFilter(ctx.tex1),
                    (unsigned)(ctx.tex0.tbw == 0u), (unsigned)g141TraceOff,
                    (unsigned)g141FastSampleTri);
        }
    }
    // G142 (drawn-pixel eliminate-work, SAFE/bit-exact): per-triangle CLUT decode cache. The
    // bilinear cavern samples 4 CLUT-decoded taps/pixel (g141FastSample -> 4x g141SamplePoint ->
    // lookupCLUT = resolveClutIndex + VRAM CLUT read + applyTexa). The CLUT decode is per-triangle
    // INVARIANT (GS state + CLUT VRAM are constant within a primitive), so memoize each distinct
    // texel index ONCE per triangle: g141ClutLookup(idx) == lookupCLUT(gs, idx, ...) by construction
    // (proven per-pixel by DC2_G141_SAMPLE_VERIFY). Lazy decode-on-first-use => never issues more
    // lookupCLUT than the uncached path (safe for point/small tris too); caps at <=256 (T8) / 16 (T4)
    // decodes/tri vs 4*~230 today. Fresh per triangle => NO cross-draw staleness, no invalidation
    // needed. Kill DC2_G141_NO_CLUTCACHE=1.
    static const bool s_g141NoClutCache = (std::getenv("DC2_G141_NO_CLUTCACHE") != nullptr);
    // G148 sampler-stat gate (measurement only; see the g_g148* globals). Captured once per triangle.
    static const bool s_g148SampleStat = (std::getenv("DC2_G148_SAMPLESTAT") != nullptr);
    const bool g148Stat = s_g148SampleStat;
    const bool g141Paletted =
        g141Psm == GS_PSM_T8 || g141Psm == GS_PSM_T4 ||
        g141Psm == GS_PSM_T4HL || g141Psm == GS_PSM_T4HH;
    const bool g141UseClutCache = g141FastSampleTri && g141Paletted && !s_g141NoClutCache;
    // G143: thread_local so row-parallel lanes each get their own cache; reset per lane in g143RangeFn.
    static thread_local uint32_t g141Clut[256];
    static thread_local uint8_t g141ClutValid[256];
    auto g141ClutLookup = [&](uint8_t idx) -> uint32_t {
        if (!g141ClutValid[idx])
        {
            g141Clut[idx] = lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm,
                                       ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            g141ClutValid[idx] = 1u;
        }
        return g141Clut[idx];
    };
    // G149 (perf, DEFAULT OFF): the decoded, CLUT-pre-applied buffer for THIS triangle's texture is
    // built ONCE at DEFER time on the single guest thread (see g149BuildForDefer at the tilebin defer
    // site) and handed to every band lane via a thread_local, so the read below is LOCK-FREE — no
    // mutex on the hot path. t_g149Decoded is non-null only inside a G144 tilebin replay of an
    // eligible fast-sampler tri; nullptr for direct/non-tilebin draws → the normal sampler runs.
    static const bool s_g149Verify = (std::getenv("DC2_G149_TCVERIFY") != nullptr);
    const uint32_t *g149Px = t_g149Px; // eager-decoded read-only buffer (null unless texcache replay)
    // Inner point sampler = sampleTexture's samplePoint lambda (wrap + format read), leaves reused.
    auto g141SamplePoint = [&](int su0, int sv0) -> uint32_t {
        if (g148Stat) g_g148LeafReads.fetch_add(1u, std::memory_order_relaxed);
        const int su = wrapTextureCoordinate(su0, g141WrapU, g141TexW, g141MinU, g141MaxU);
        const int sv = wrapTextureCoordinate(sv0, g141WrapV, g141TexH, g141MinV, g141MaxV);
        // G149 fast path: single read-only linear lookup of the pre-decoded, CLUT-pre-applied buffer.
        const size_t g149Idx = static_cast<size_t>(sv) * static_cast<size_t>(g141TexW) + static_cast<size_t>(su);
        if (g149Px && !s_g149Verify)
            return g149Px[g149Idx];
        uint32_t leaf;
        switch (g141Psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
            leaf = applyTexa(gs->m_texa, g141Psm, readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv));
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            leaf = applyTexa(gs->m_texa, g141Psm, readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv));
            break;
        case GS_PSM_T8:
        {
            const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
            const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                    static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            leaf = g141UseClutCache ? g141ClutLookup(idx)
                                    : lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            break;
        }
        default: // T4 / T4HL / T4HH
        {
            const uint32_t idx = readTexelPSMT4(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv);
            leaf = g141UseClutCache ? g141ClutLookup(static_cast<uint8_t>(idx))
                                    : lookupCLUT(gs, static_cast<uint8_t>(idx), ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            break;
        }
        }
        // G149 verify (DC2_G149_TCVERIFY): A/B the pre-decoded buffer vs the live leaf — bad must be 0.
        if (g149Px && s_g149Verify)
        {
            const uint32_t cached = g149Px[g149Idx];
            static std::atomic<uint64_t> s_tcTot{0}, s_tcBad{0};
            const uint64_t tt = s_tcTot.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (cached != leaf)
            {
                const uint64_t bb = s_tcBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (bb <= 16u)
                    std::fprintf(stderr, "[G149:vfy] MISMATCH cached=%08x leaf=%08x psm=0x%x su=%d sv=%d\n",
                                 cached, leaf, (unsigned)g141Psm, su, sv);
            }
            if ((tt & 0xFFFFFull) == 0ull)
                std::fprintf(stderr, "[G149:vfy] tot=%llu bad=%llu\n",
                             (unsigned long long)tt, (unsigned long long)s_tcBad.load(std::memory_order_relaxed));
        }
        return leaf;
    };
    // G142 texel-quad cache (bilinear MAGNIFIED textures, e.g. the cavern MMAG=1): consecutive
    // pixels along a scanline map to the SAME 4 taps (u0,v0 unchanged), so cache the 4 corner
    // colours keyed on (u0,v0) and skip the 4x wrap+ReadP8+CLUT when unchanged (the per-pixel
    // fx/fy lerp still runs). Reuse ONLY on identical tap coords => bit-exact (proven by
    // DC2_G141_SAMPLE_VERIFY). Fresh per triangle. Kill DC2_G141_NO_TEXQUAD=1.
    static const bool s_g141NoTexQuad = (std::getenv("DC2_G141_NO_TEXQUAD") != nullptr);
    // G143: thread_local (per row-parallel lane); g141QValid reset per row in g143RangeFn.
    static thread_local bool g141QValid;
    static thread_local int g141QU0, g141QV0;
    static thread_local uint32_t g141QC00, g141QC10, g141QC01, g141QC11;
    auto g141FastSample = [&](float s, float t, float q, uint16_t u, uint16_t v) -> uint32_t {
        if (g148Stat) g_g148TexSamples.fetch_add(1u, std::memory_order_relaxed);
        float texUf, texVf;
        if (gs->m_prim.fst)
        {
            texUf = static_cast<float>(u) / 16.0f;
            texVf = static_cast<float>(v) / 16.0f;
        }
        else
        {
            const float invQ = 1.0f / fabsQ(q);
            texUf = s * invQ * static_cast<float>(g141TexW);
            texVf = t * invQ * static_cast<float>(g141TexH);
        }
        if (!g141Linear)
        {
            if (g148Stat) g_g148PointSamp.fetch_add(1u, std::memory_order_relaxed);
            return g141SamplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
        }
        // Bilinear = sampleTexture's 4-tap path (identical float ops + lerpChannel).
        const float sampleU = texUf - 0.5f;
        const float sampleV = texVf - 0.5f;
        const int u0 = static_cast<int>(std::floor(sampleU));
        const int v0 = static_cast<int>(std::floor(sampleV));
        const int u1 = u0 + 1;
        const int v1 = v0 + 1;
        const float fx = sampleU - static_cast<float>(u0);
        const float fy = sampleV - static_cast<float>(v0);
        uint32_t c00, c10, c01, c11;
        if (!s_g141NoTexQuad && g141QValid && u0 == g141QU0 && v0 == g141QV0)
        {
            if (g148Stat) g_g148QuadHit.fetch_add(1u, std::memory_order_relaxed);
            c00 = g141QC00; c10 = g141QC10; c01 = g141QC01; c11 = g141QC11;
        }
        else
        {
            if (g148Stat) g_g148QuadMiss.fetch_add(1u, std::memory_order_relaxed);
            c00 = g141SamplePoint(u0, v0);
            c10 = g141SamplePoint(u1, v0);
            c01 = g141SamplePoint(u0, v1);
            c11 = g141SamplePoint(u1, v1);
            g141QValid = true; g141QU0 = u0; g141QV0 = v0;
            g141QC00 = c00; g141QC10 = c10; g141QC01 = c01; g141QC11 = c11;
        }
        const uint8_t rr = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu), static_cast<uint8_t>(c10 & 0xFFu),
                                       static_cast<uint8_t>(c01 & 0xFFu), static_cast<uint8_t>(c11 & 0xFFu), fx, fy);
        const uint8_t gg = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu), static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 8) & 0xFFu), static_cast<uint8_t>((c11 >> 8) & 0xFFu), fx, fy);
        const uint8_t bb = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu), static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 16) & 0xFFu), static_cast<uint8_t>((c11 >> 16) & 0xFFu), fx, fy);
        const uint8_t aa = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu), static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 24) & 0xFFu), static_cast<uint8_t>((c11 >> 24) & 0xFFu), fx, fy);
        return static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8) |
               (static_cast<uint32_t>(bb) << 16) | (static_cast<uint32_t>(aa) << 24);
    };
    // G143 (perf, DEFAULT OFF): the per-row body as a lane function so a worker pool can rasterize
    // disjoint scanline ranges concurrently. Bit-exact: disjoint rows => disjoint FB/Z pixel writes;
    // the CLUT/texel-quad caches are thread_local (reset per lane / per row here); the pixel counters
    // are per-lane; cross-triangle submission order is preserved (drawTriangle still finishes before
    // the next). insideAcc/drawnAcc replace the g141InsidePx/f516cov increments so lanes don't race.
    auto g143RangeFn = [&](int y0, int y1, long &insideAcc, int &drawnAcc)
    {
        if (g141UseClutCache)
            std::memset(g141ClutValid, 0, sizeof(g141ClutValid));
        for (int y = y0; y <= y1; ++y)
        {
            g141QValid = false;
            float py = static_cast<float>(y) + 0.5f;
        int xStart = minX, xEnd = maxX;
        if (!s_g141NoXspan)
        {
            const double eps = static_cast<double>(kEdgeEpsilon);
            const double basePx = static_cast<double>(minX) + 0.5;
            const double pyd = static_cast<double>(py);
            const double w0b = ((static_cast<double>(fy1 - fy2)) * (basePx - static_cast<double>(fx2)) +
                                (static_cast<double>(fx2 - fx1)) * (pyd - static_cast<double>(fy2))) *
                               static_cast<double>(winding) * static_cast<double>(invAbsDenom);
            const double w1b = ((static_cast<double>(fy2 - fy0)) * (basePx - static_cast<double>(fx2)) +
                                (static_cast<double>(fx0 - fx2)) * (pyd - static_cast<double>(fy2))) *
                               static_cast<double>(winding) * static_cast<double>(invAbsDenom);
            const double w2b = 1.0 - w0b - w1b;
            // Solve wi(t) = wib + t*Si >= -eps for t = (x - minX). Lower/upper bounds per edge.
            double tLo = 0.0;
            double tHi = static_cast<double>(maxX - minX);
            const double tiny = 1e-9;
            const double lo0 = -eps - w0b, lo1 = -eps - w1b, lo2 = -eps - w2b;
            if (g141S0 > tiny)       { const double c = lo0 / g141S0; if (c > tLo) tLo = c; }
            else if (g141S0 < -tiny) { const double c = lo0 / g141S0; if (c < tHi) tHi = c; }
            if (g141S1 > tiny)       { const double c = lo1 / g141S1; if (c > tLo) tLo = c; }
            else if (g141S1 < -tiny) { const double c = lo1 / g141S1; if (c < tHi) tHi = c; }
            if (g141S2 > tiny)       { const double c = lo2 / g141S2; if (c > tLo) tLo = c; }
            else if (g141S2 < -tiny) { const double c = lo2 / g141S2; if (c < tHi) tHi = c; }
            double tLoC = std::floor(tLo) - 1.0;
            if (tLoC < 0.0) tLoC = 0.0;
            double tHiC = std::ceil(tHi) + 1.0;
            const double span = static_cast<double>(maxX - minX);
            if (tHiC > span) tHiC = span;
            xStart = minX + static_cast<int>(tLoC);
            xEnd = minX + static_cast<int>(tHiC);
        }
        for (int x = xStart; x <= xEnd; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;
            ++insideAcc;
            if (g232tri)
                g_g232PxInside.fetch_add(1u, std::memory_order_relaxed);

            // G41: depth test (Z is interpolated linearly in screen space by the GS).
            uint32_t zi = 0u;
            uint32_t zdst = 0u;
            bool zPass = true;
            if (zEnabled)
            {
                float zf = v0.z * w0 + v1.z * w1 + v2.z * w2;
                if (zf < 0.0f) zf = 0.0f;
                const float zmax = z16 ? 65535.0f : (z24 ? 16777215.0f : 4294967295.0f);
                if (zf > zmax) zf = zmax;
                zi = static_cast<uint32_t>(zf);
                // G45: costume model (fbp=0x139) uses the private, non-aliasing Z buffer.
                // G203: universal-Z reads the real VRAM Z for every target (private buffers retired).
                if (!g203Uni && ctx.frame.fbp == 0x139u)
                    zdst = costumeZRead(x, y);
                else if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled())
                    zdst = g106TitleZRead(x, y, ctx.frame.fbp);
                else
                    zdst = z16
                        ? GSMem::ReadZ16(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                        : (z24
                            ? GSMem::ReadZ24(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                            : GSMem::ReadZ32(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                switch (ztst)
                {
                case 0:  zPass = false; break;            // NEVER
                case 1:  zPass = true;  break;            // ALWAYS
                case 2:  zPass = (zi >= zdst); break;     // GEQUAL
                default: zPass = (zi >  zdst); break;     // GREATER
                }
            }
            // G219 (default-off, DC2_G219_SKYPX=1): MAP-4 zoom top-right black region. Trace the
            // sky-dome pixels (display fbp, tbp=0x2820) inside the black rectangle (x>=417,
            // y<=140) — zi vs zdst discriminates "sky fails Z" from "sky never rasterized here".
            {
                static const bool s_g219 = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219 &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    gs->m_prim.tme && ctx.tex0.tbp0 == 0x2820u &&
                    x >= 417 && y <= 140 && ((x & 7) == 1) && ((y & 7) == 1))
                {
                    static std::atomic<uint32_t> s_g219n{0};
                    const uint32_t n = s_g219n.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u || (n % 4096u) == 0u)
                        std::fprintf(stderr,
                            "[G219:skypx] n=%u xy=(%d,%d) fbp=0x%x prim=%u zen=%u ztst=%u "
                            "zi=%u zdst=%u pass=%u replay=%u vz=(%.0f,%.0f,%.0f)\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                            static_cast<uint32_t>(zEnabled), ztst, zi, zdst,
                            static_cast<uint32_t>(zPass), t_g144InReplay ? 1u : 0u,
                            v0.z, v1.z, v2.z);
                }
            }
            if (g106TitleZ && g106TitleZTraceEnabled())
            {
                static std::atomic<uint64_t> s_g106Test{0};
                static std::atomic<uint64_t> s_g106Reject{0};
                const uint64_t tn = s_g106Test.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (!zPass)
                    s_g106Reject.fetch_add(1u, std::memory_order_relaxed);
                if (tn <= 32u || (tn & 0x3FFFFull) == 0ull)
                    std::fprintf(stderr,
                        "[G106:zpx] n=%llu fbp=0x%x tbp=0x%x prim=%u xy=(%d,%d) "
                        "zen=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u private=%u "
                        "zi=%u zdst=%u pass=%u reject=%llu\n",
                        static_cast<unsigned long long>(tn), ctx.frame.fbp, ctx.tex0.tbp0,
                        static_cast<uint32_t>(gs->m_prim.type), x, y,
                        static_cast<uint32_t>(zEnabled), ztst, zbp, zbw, zpsm,
                        static_cast<uint32_t>(zmsk),
                        g106TitleZPrivateEnabled() ? 1u : 0u,
                        zi, zdst, static_cast<uint32_t>(zPass),
                        static_cast<unsigned long long>(s_g106Reject.load(std::memory_order_relaxed)));
            }
            if (g195ForegroundTraceTarget(ctx, x, y) && ctx.tex0.tbp0 == 0x2a20u)
            {
                static std::atomic<uint32_t> s_g195Zpx{0};
                const uint32_t zn = s_g195Zpx.fetch_add(1u, std::memory_order_relaxed);
                if (zn < 512u)
                {
                    std::fprintf(stderr,
                        "[G195:zpx] n=%u xy=(%d,%d) fbp=0x%x tbp=0x%x prim=%u "
                        "zen=%u scopeTown=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u "
                        "zi=%u zdst=%u pass=%u z=(%.0f,%.0f,%.0f)\n",
                        zn, x, y, ctx.frame.fbp, ctx.tex0.tbp0,
                        static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(zEnabled), static_cast<uint32_t>(g202TownZ),
                        ztst, zbp, zbw, zpsm, static_cast<uint32_t>(zmsk),
                        zi, zdst, static_cast<uint32_t>(zPass),
                        v0.z, v1.z, v2.z);
                    std::fflush(stderr);
                }
            }
            if (g44FaceZ &&
                x >= 344 && x <= 412 && y >= 40 && y <= 144 &&
                (static_cast<uint32_t>(x) & 3u) == 0u &&
                (static_cast<uint32_t>(y) & 3u) == 0u)
            {
                static std::atomic<uint32_t> s_g44z{0};
                const uint32_t zn = s_g44z.fetch_add(1u, std::memory_order_relaxed);
                if (zn < 5000u)
                {
                    std::fprintf(stderr,
                        "[G44:zpx] n=%u xy=(%d,%d) tbp=0x%x psm=0x%x prim=%u abe=%u fst=%u "
                        "zen=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u zi=%u zdst=%u pass=%u "
                        "w=(%.3f,%.3f,%.3f) vz=(%.0f,%.0f,%.0f) bbox=(%d,%d)-(%d,%d)\n",
                        zn, x, y, ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.psm),
                        static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                        static_cast<uint32_t>(gs->m_prim.fst),
                        static_cast<uint32_t>(zEnabled), ztst, zbp, zbw, zpsm,
                        static_cast<uint32_t>(zmsk), zi, zdst, static_cast<uint32_t>(zPass),
                        w0, w1, w2, v0.z, v1.z, v2.z, minX, minY, maxX, maxY);
                }
            }
            if (g232tri)
            {
                const uint32_t zl = g_g232PxZlog.fetch_add(1u, std::memory_order_relaxed);
                if (zl < 16u)
                    std::fprintf(stderr,
                        "[G232:gempx] xy=(%d,%d) zen=%u ztst=%u zi=%u zdst=%u pass=%u zbp=0x%x zbw=%u zpsm=0x%x\n",
                        x, y, static_cast<uint32_t>(zEnabled), ztst, zi, zdst,
                        static_cast<uint32_t>(zPass), zbp, zbw, zpsm);
                if (!zPass)
                    g_g232PxZfail.fetch_add(1u, std::memory_order_relaxed);
            }
            if (g232watch && x == g232wx && y == g232wy)
            {
                static std::atomic<uint32_t> s_g232zh{0};
                const uint32_t n = s_g232zh.fetch_add(1u, std::memory_order_relaxed);
                if (n < 500u)
                    std::fprintf(stderr,
                        "[G232:zhist] xy=(%d,%d) fbp=0x%x tbp=0x%x prim=%u tme=%u abe=%u zen=%u ztst=%u "
                        "zi=%u zdst=%u pass=%u zwr=%u gem=%u\n",
                        x, y, ctx.frame.fbp, ctx.tex0.tbp0, (unsigned)gs->m_prim.type,
                        (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.abe,
                        (unsigned)zEnabled, ztst, zi, zdst, (unsigned)zPass,
                        (unsigned)zWrite, g232tri ? 1u : 0u);
            }
            if (!zPass)
                continue;

            uint8_t r, g, b, a;
            if (gs->m_prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (gs->m_prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (gs->m_prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // G222: real GS STQ semantics — S/T/Q interpolate LINEARLY in screen space and
                    // the single divide happens per pixel in the sampler (u = lerp(S)/lerp(Q)*texW).
                    // The old path divided per VERTEX then re-multiplied; the iq factors cancel to a
                    // pure AFFINE interpolation of s/q, visible as bent texture seams at a quad's
                    // shared diagonal on large deep triangles (MAP-2 dock floor, G222).
                    // DC2_G222_AFFINE_TEX=1 restores the old affine behavior.
                    static const bool s_g222Affine = envFlagEnabled("DC2_G222_AFFINE_TEX");
                    if (s_g222Affine)
                    {
                        const float invQ0 = 1.0f / fabsQ(v0.q);
                        const float invQ1 = 1.0f / fabsQ(v1.q);
                        const float invQ2 = 1.0f / fabsQ(v2.q);
                        const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                        const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                        const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                        iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                        is = sOverQ * iq;
                        it = tOverQ * iq;
                    }
                    else
                    {
                        is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                        it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                        iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    }
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel;
                if (s_g141SkipSample)
                {
                    texel = 0xFFFFFFFFu;
                }
                else if (g141FastSampleTri)
                {
                    texel = g141FastSample(is, it, iq, iu, iv);
                    if (s_g141SampleVerify)
                    {
                        const uint32_t ref = sampleTexture(gs, is, it, iq, iu, iv);
                        static std::atomic<uint64_t> s_vTot{0}, s_vBad{0};
                        const uint64_t tt = s_vTot.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (ref != texel)
                        {
                            const uint64_t bb = s_vBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (bb <= 16u)
                                std::fprintf(stderr, "[G141:vfy] MISMATCH ref=%08x fast=%08x psm=0x%x fst=%u\n",
                                             ref, texel, (unsigned)g141Psm, (unsigned)gs->m_prim.fst);
                        }
                        if ((tt & 0xFFFFFull) == 0ull)
                            std::fprintf(stderr, "[G141:vfy] tot=%llu bad=%llu\n",
                                         (unsigned long long)tt,
                                         (unsigned long long)s_vBad.load(std::memory_order_relaxed));
                    }
                }
                else
                {
                    texel = sampleTexture(gs, is, it, iq, iu, iv);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);
                // G82: title rock renders bright green instead of brown. Log sampled texel vs
                // per-vertex shade vs MODULATE output for rock-range textures so we can tell a
                // texel-wrong fault (texture data / CLUT / UV) from a shade-wrong fault (vertex
                // colour). HW rock: T8 texel brown/grey, shade greenish-grey (46,69,61). Gated.
                {
                    static const bool s_g82rock = (std::getenv("DC2_G82_ROCK") != nullptr);
                    if (s_g82rock && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
                        ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
                    {
                        // Aggregate avg texel + avg shade per (tbp,fbp) so we can compare the
                        // runner's per-vertex shade to HW (g82_gs_verts.py) apples-to-apples per
                        // texture. Periodically dump the table.
                        static std::mutex s_g82mtx;
                        struct Acc { uint32_t tbp,fbp; uint64_t n, tr,tg,tb, sr,sg,sb, alo,ahi; };
                        static Acc s_acc[96]{};
                        static uint32_t s_accN = 0;
                        std::lock_guard<std::mutex> lk(s_g82mtx);
                        uint32_t idx = 0xFFFFFFFFu;
                        for (uint32_t i = 0; i < s_accN; ++i)
                            if (s_acc[i].tbp == ctx.tex0.tbp0 && s_acc[i].fbp == ctx.frame.fbp) { idx = i; break; }
                        if (idx == 0xFFFFFFFFu && s_accN < 96u) { idx = s_accN++; s_acc[idx].tbp = ctx.tex0.tbp0; s_acc[idx].fbp = ctx.frame.fbp; }
                        if (idx != 0xFFFFFFFFu) {
                            Acc &A = s_acc[idx];
                            A.n++; A.tr += tr; A.tg += tg; A.tb += tb; A.sr += r; A.sg += g; A.sb += b;
                            if (A.n == 1) { A.alo = A.ahi = ta; } else { if (ta < A.alo) A.alo = ta; if (ta > A.ahi) A.ahi = ta; }
                        }
                        static uint64_t s_tick = 0;
                        if ((++s_tick % 200000ull) == 0ull)
                            for (uint32_t i = 0; i < s_accN; ++i) {
                                Acc &A = s_acc[i]; if (!A.n) continue;
                                std::fprintf(stderr, "[G82:rockavg] fbp=0x%x tbp=0x%x n=%llu texel=(%llu,%llu,%llu) shade=(%llu,%llu,%llu) ta=[%llu,%llu]\n",
                                    A.fbp, A.tbp, (unsigned long long)A.n,
                                    (unsigned long long)(A.tr/A.n),(unsigned long long)(A.tg/A.n),(unsigned long long)(A.tb/A.n),
                                    (unsigned long long)(A.sr/A.n),(unsigned long long)(A.sg/A.n),(unsigned long long)(A.sb/A.n),
                                    (unsigned long long)A.alo,(unsigned long long)A.ahi);
                            }
                    }
                }
                if (g43FaceUv && x >= 300 && x <= 450 && y >= 90 && y <= 180)
                {
                    static std::atomic<uint32_t> s_g43sample{0};
                    const uint32_t sn = s_g43sample.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 240u)
                    {
                        std::fprintf(stderr,
                            "[G43:sample] n=%u xy=(%d,%d) w=(%.3f,%.3f,%.3f) uv=(%u,%u) stq=(%.5g,%.5g,%.5g) "
                            "texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x\n",
                            sn, x, y, w0, w1, w2,
                            (unsigned)(iu >> 4), (unsigned)(iv >> 4),
                            is, it, iq,
                            tr, tg, tb, ta,
                            shadeR, shadeG, shadeB, shadeA,
                            color.r, color.g, color.b, color.a);
                    }
                }
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f31TriTexSampleCount{0};
                    if (s_f31TriTexSampleCount.load(std::memory_order_relaxed) < 64u)
                    {
                        const uint32_t n = s_f31TriTexSampleCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u)
                        {
                            const uint64_t tex0Raw =
                                static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) |
                                (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 14) |
                                (static_cast<uint64_t>(tex.psm & 0x3Fu) << 20) |
                                (static_cast<uint64_t>(tex.tw & 0xFu) << 26) |
                                (static_cast<uint64_t>(tex.th & 0xFu) << 30) |
                                (static_cast<uint64_t>(tex.tcc & 0x1u) << 34) |
                                (static_cast<uint64_t>(tex.tfx & 0x3u) << 35) |
                                (static_cast<uint64_t>(tex.cbp & 0x3FFFu) << 37) |
                                (static_cast<uint64_t>(tex.cpsm & 0xFu) << 51) |
                                (static_cast<uint64_t>(tex.csm & 0x1u) << 55) |
                                (static_cast<uint64_t>(tex.csa & 0x1Fu) << 56) |
                                (static_cast<uint64_t>(tex.cld & 0x7u) << 61);
                            std::fprintf(stderr,
                                         "[F31:tritex] n=%u xy=(%d,%d) w=(%.3f,%.3f,%.3f) tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u cbp=0x%x texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x st=(%.4f,%.4f,%.4f) uv=(%u,%u)\n",
                                         n, x, y, w0, w1, w2,
                                         static_cast<unsigned long long>(tex0Raw),
                                         tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                         static_cast<uint32_t>(tex.psm),
                                         static_cast<uint32_t>(tex.tfx),
                                         static_cast<uint32_t>(tex.tcc),
                                         tex.cbp,
                                         tr, tg, tb, ta,
                                         shadeR, shadeG, shadeB, shadeA,
                                         color.r, color.g, color.b, color.a,
                                         is, it, iq, iu, iv);
                        }
                    }
                }

                if (g195DofTri)
                {
                    static std::atomic<uint32_t> s_g195Px{0};
                    const uint32_t pn = s_g195Px.fetch_add(1u, std::memory_order_relaxed);
                    const bool targetPx =
                        (x == 170 && y == 160) ||
                        (x == 250 && y == 180) ||
                        (x == 380 && y == 160) ||
                        (x == 230 && y == 300) ||
                        (x == 400 && y == 300);
                    if (targetPx || pn < 160u || (pn % 4096u) == 0u)
                    {
                        const bool linear = tex1UsesLinearFilter(ctx.tex1);
                        float texUf = 0.0f, texVf = 0.0f;
                        if (gs->m_prim.fst)
                        {
                            texUf = static_cast<float>(iu) / 16.0f;
                            texVf = static_cast<float>(iv) / 16.0f;
                        }
                        else
                        {
                            const float invQ = 1.0f / fabsQ(iq);
                            texUf = is * invQ * static_cast<float>(g141TexW);
                            texVf = it * invQ * static_cast<float>(g141TexH);
                        }

                        const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
                        auto readIdx = [&](int su0, int sv0, uint8_t &ix, uint32_t &col, int &su, int &sv) {
                            su = wrapTextureCoordinate(su0, g141WrapU, g141TexW, g141MinU, g141MaxU);
                            sv = wrapTextureCoordinate(sv0, g141WrapV, g141TexH, g141MinV, g141MaxV);
                            ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                                                     static_cast<uint32_t>(su),
                                                                     static_cast<uint32_t>(sv)));
                            col = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                             ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                        };

                        int su0 = 0, sv0 = 0, su1 = 0, sv1 = 0, su2 = 0, sv2 = 0, su3 = 0, sv3 = 0;
                        uint8_t ix0 = 0, ix1 = 0, ix2 = 0, ix3 = 0;
                        uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                        float fx = 0.0f, fy = 0.0f;
                        if (linear)
                        {
                            const float sampleU = texUf - 0.5f;
                            const float sampleV = texVf - 0.5f;
                            const int u0 = static_cast<int>(std::floor(sampleU));
                            const int v0s = static_cast<int>(std::floor(sampleV));
                            fx = sampleU - static_cast<float>(u0);
                            fy = sampleV - static_cast<float>(v0s);
                            readIdx(u0,     v0s,     ix0, c0, su0, sv0);
                            readIdx(u0 + 1, v0s,     ix1, c1, su1, sv1);
                            readIdx(u0,     v0s + 1, ix2, c2, su2, sv2);
                            readIdx(u0 + 1, v0s + 1, ix3, c3, su3, sv3);
                        }
                        else
                        {
                            readIdx(static_cast<int>(texUf), static_cast<int>(texVf), ix0, c0, su0, sv0);
                        }

                        const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
                        const uint32_t bytesPerPixel =
                            (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S) ? 2u : 4u;
                        uint32_t off = 0u;
                        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
                        {
                            off = GSPSMCT32::addrPSMCT32(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                                         widthBlocks,
                                                         static_cast<uint32_t>(x),
                                                         static_cast<uint32_t>(y));
                        }
                        else
                        {
                            off = addrPSMCT16Family(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                                    widthBlocks,
                                                    ctx.frame.psm,
                                                    static_cast<uint32_t>(x),
                                                    static_cast<uint32_t>(y));
                        }

                        uint32_t dst = 0u;
                        if (off + bytesPerPixel <= gs->m_vramSize)
                        {
                            if (bytesPerPixel == 2u)
                            {
                                uint16_t packed = 0u;
                                std::memcpy(&packed, gs->m_vram + off, 2);
                                dst = decodePSMCT16(packed);
                            }
                            else
                            {
                                std::memcpy(&dst, gs->m_vram + off, 4);
                            }
                        }

                        const AlphaTestResult at = classifyAlphaTest(ctx.test, color.a);
                        uint8_t finalR = color.r, finalG = color.g, finalB = color.b, finalA = color.a;
                        const uint8_t dr = static_cast<uint8_t>(dst & 0xFFu);
                        const uint8_t dg = static_cast<uint8_t>((dst >> 8) & 0xFFu);
                        const uint8_t db = static_cast<uint8_t>((dst >> 16) & 0xFFu);
                        const uint8_t da = static_cast<uint8_t>((dst >> 24) & 0xFFu);
                        if (at.writeFramebuffer && gs->m_prim.abe &&
                            !(gs->m_pabe && (color.a & 0x80u) == 0u))
                        {
                            const uint64_t alphaReg = ctx.alpha;
                            const uint8_t asel = alphaReg & 3u;
                            const uint8_t bsel = (alphaReg >> 2) & 3u;
                            const uint8_t csel = (alphaReg >> 4) & 3u;
                            const uint8_t dsel = (alphaReg >> 6) & 3u;
                            const uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
                            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int {
                                if (sel == 0u) return cs;
                                if (sel == 1u) return cd;
                                return 0;
                            };
                            const int cAlpha = (csel == 0u) ? color.a : (csel == 1u) ? da : fix;
                            finalR = clampU8(((pickRGB(asel, color.r, dr) - pickRGB(bsel, color.r, dr)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.r, dr));
                            finalG = clampU8(((pickRGB(asel, color.g, dg) - pickRGB(bsel, color.g, dg)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.g, dg));
                            finalB = clampU8(((pickRGB(asel, color.b, db) - pickRGB(bsel, color.b, db)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.b, db));
                        }
                        if (!at.preserveDestinationAlpha &&
                            (ctx.fba & 0x1ull) != 0ull &&
                            ctx.frame.psm != GS_PSM_CT24)
                        {
                            finalA = static_cast<uint8_t>(finalA | 0x80u);
                        }
                        uint32_t finalPixel = static_cast<uint32_t>(finalR) |
                            (static_cast<uint32_t>(finalG) << 8) |
                            (static_cast<uint32_t>(finalB) << 16) |
                            (static_cast<uint32_t>(finalA) << 24);
                        if (ctx.frame.fbmsk != 0u && off + bytesPerPixel <= gs->m_vramSize)
                            finalPixel = (finalPixel & ~ctx.frame.fbmsk) | (dst & ctx.frame.fbmsk);
                        if (at.preserveDestinationAlpha)
                            finalPixel = (finalPixel & 0x00FFFFFFu) | (dst & 0xFF000000u);

                        std::fprintf(stderr,
                            "[G195:dofpx] n=%u target=%u xy=(%d,%d) fbp=0x%x off=0x%x prim=%u tbp=0x%x cbp=0x%x "
                            "bbox=(%d,%d)-(%d,%d) lin=%u texuv=(%.3f,%.3f) fx=%.3f fy=%.3f "
                            "tap0=(%d,%d ix=%u col=%08x) tap1=(%d,%d ix=%u col=%08x) "
                            "tap2=(%d,%d ix=%u col=%08x) tap3=(%d,%d ix=%u col=%08x) "
                            "texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x src=%02x,%02x,%02x,%02x "
                            "dst=%08x drgba=%02x,%02x,%02x,%02x alpha=0x%llx test=0x%llx atWrite=%u atKeepA=%u "
                            "final=%08x w=(%.4f,%.4f,%.4f) st=(%.5g,%.5g,%.5g) uv=(%u,%u)\n",
                            pn, targetPx ? 1u : 0u, x, y, ctx.frame.fbp, off,
                            static_cast<uint32_t>(gs->m_prim.type), ctx.tex0.tbp0, ctx.tex0.cbp,
                            minX, minY, maxX, maxY, linear ? 1u : 0u, texUf, texVf, fx, fy,
                            su0, sv0, static_cast<uint32_t>(ix0), c0,
                            su1, sv1, static_cast<uint32_t>(ix1), c1,
                            su2, sv2, static_cast<uint32_t>(ix2), c2,
                            su3, sv3, static_cast<uint32_t>(ix3), c3,
                            tr, tg, tb, ta,
                            shadeR, shadeG, shadeB, shadeA,
                            color.r, color.g, color.b, color.a,
                            dst, dr, dg, db, da,
                            static_cast<unsigned long long>(ctx.alpha),
                            static_cast<unsigned long long>(ctx.test),
                            at.writeFramebuffer ? 1u : 0u,
                            at.preserveDestinationAlpha ? 1u : 0u,
                            finalPixel,
                            w0, w1, w2, is, it, iq, static_cast<uint32_t>(iu), static_cast<uint32_t>(iv));
                    }
                }

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            // G240: alpha-test failure controls framebuffer and depth writes independently.
            // The old path let writePixel suppress AFAIL=KEEP framebuffer writes, then wrote Z
            // unconditionally below. Transparent cutout texels consequently occluded later
            // primitives. Keep ZB_ONLY depth-only behavior while suppressing depth for KEEP,
            // FB_ONLY, and RGB_ONLY, matching TEST.AFAIL.
            const AlphaTestResult fragmentAlpha = classifyAlphaTest(ctx.test, a);

            // G48: the costume head sub-meshes (skin / hair / hat / dark face-shadow decals)
            // interleave, and the RTT is cleared to BLACK, so a DARK alpha-blended decal that hits
            // a still-uncovered pixel BEFORE the opaque skin reaches it locks in a dark
            // decal-over-black dot (and its Z write then rejects the later skin). On HW the opaque
            // skin covers the pixel first, then the dark decal blends onto real skin. Reproduce
            // that ordering without reordering the VU1 draw stream: at a still-black costume RTT
            // pixel, SKIP a DARK source fragment so a later brighter mesh (skin) can fill it; BRIGHT
            // fragments (skin, the brown hat) are written and made opaque by the opaque-over-black
            // path in writePixel. Distinguishing dark vs bright by source luma is what lets the hat
            // (a decal page, bright brown) survive while the dark face-shadow decals defer to skin.
            // The old G45 NoDot skipped ALL decals over black -> hid the hat. Tune: DC2_G48_DARK.
            //
            // G50: the dark-skip must be SCOPED TO THE HEAD/FACE BAND. The lower body (striped
            // trousers tbp~0x3460/0x34a0/0x34e0/0x3500/0x3540 at RTT y>=185, brown boots tbp=0x35e0
            // at y288-352) is legitimately DARK base-mesh geometry with no brighter mesh coming, so
            // a global dark-skip wrongly leaves those pixels black -> the composite shows the wall
            // background THROUGH the legs/boots (sparse "strands" = only the lighter trouser stripes
            // survive). The face dark-DECAL-over-black problem is confined to the head (RTT y<=125
            // per the G50 per-page screen bbox). Bound the skip to the head band so the lower body's
            // dark base fragments are written. Boundary env-overridable: DC2_G48_HEADY (RTT-space y).
            static const int s_g48HeadY = []() -> int {
                const char *e = std::getenv("DC2_G48_HEADY");
                return e ? std::atoi(e) : 160;
            }();
            // G50: exempt the head's opaque BASE meshes (hat/skin/hair) so their dark fragments are
            // WRITTEN (full hat, no black holes in the face); keep the skip for decal pages.
            static const bool s_g50NoExempt = (std::getenv("DC2_G50_NOEXEMPT") != nullptr);
            const bool g50BaseMesh = !s_g50NoExempt && g50HeadBaseMeshPage(ctx.tex0.tbp0);
            if (ctx.frame.fbp == 0x139u && gs->m_vram && y <= s_g48HeadY && !g50BaseMesh)
            {
                // G52: the G48 luma dark-skip was a HEURISTIC band-aid for the face "speckle" dots,
                // whose REAL cause (spurious fully-zero RGBAQ model verts) is now fixed at source in
                // ps2_gs_gpu.cpp (hold-last-color). The dark-skip is therefore obsolete AND harmful:
                // it eats dark hat-brim fragments on costume combos whose hat texture pages are not in
                // the Red-Vest-derived base-mesh exemption list (e.g. the default Hunting Cap + Denim
                // Overalls combo, ref/dumps/Select_costume.png — the brim renders with missing chunks).
                // Default OFF; the whole machinery stays env-gated for diagnostics (DC2_G48_DARK=200
                // restores the old behavior). With it off the face stays clean (G52) and the hat is full.
                static const int s_g48Dark = []() -> int {
                    const char *e = std::getenv("DC2_G48_DARK");
                    return e ? std::atoi(e) : 0;
                }();
                if (s_g48Dark > 0 &&
                    (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) < s_g48Dark)
                {
                    const uint32_t fblk = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
                    const uint32_t wblk = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
                    const uint32_t doff = GSPSMCT32::addrPSMCT32(fblk, wblk,
                                                                 static_cast<uint32_t>(x),
                                                                 static_cast<uint32_t>(y));
                    if (doff + 4u <= gs->m_vramSize)
                    {
                        uint32_t dst = 0u;
                        std::memcpy(&dst, gs->m_vram + doff, 4);
                        if ((dst & 0x00FFFFFFu) == 0u)
                            continue; // dark fragment over still-black RTT -> let skin fill first
                    }
                }
            }

            // [G51] Diagnostic: the skin (face + hands) "speckle" dots are LOW-ALPHA dark cel/toon
            // shadow-overlay fragments (interpolated vertex alpha ~0x30-0x76 vs opaque skin 0x80; see
            // the [G43:sample] trace). Skip fragments whose alpha is below DC2_G51_SKIPLOWA on the
            // costume RTT skin pages to test whether removing the overlay pass removes the dots.
            static const int s_g51LowA = []() -> int {
                const char *e = std::getenv("DC2_G51_SKIPLOWA");
                return e ? std::atoi(e) : 0;
            }();
            if (s_g51LowA > 0 && ctx.frame.fbp == 0x139u &&
                (ctx.tex0.tbp0 == 0x3480u || ctx.tex0.tbp0 == 0x3460u) &&
                static_cast<int>(a) < s_g51LowA)
            {
                continue;
            }

            // G219 (default-off, DC2_G219_SKYPX=1): color-level probe for the sky-dome trifan
            // pixels inside the black rectangle — logs the post-interpolation fragment color
            // heading into writePixel, discriminating flat-black fragments from blend loss.
            {
                static const bool s_g219c = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219c &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    ((x == 470 && y == 30) || (x == 500 && y == 100)))
                {
                    static std::atomic<uint32_t> s_g219cn{0};
                    const uint32_t n = s_g219cn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 2000u)
                        std::fprintf(stderr,
                            "[G219:skycol] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x abe=%u iip=%u "
                            "rgba=(%u,%u,%u,%u) vrgba0=(%u,%u,%u,%u) vrgba2=(%u,%u,%u,%u) replay=%u\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.tme),
                            ctx.tex0.tbp0, static_cast<uint32_t>(gs->m_prim.abe),
                            static_cast<uint32_t>(gs->m_prim.iip),
                            r, g, b, a,
                            (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                            (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a,
                            t_g144InReplay ? 1u : 0u);
                }
            }
            if (g232tri)
            {
                g_g232PxWrite.fetch_add(1u, std::memory_order_relaxed);
                if (r > 150u && g < 90u && b < 110u)
                    g_g232PxRedWrite.fetch_add(1u, std::memory_order_relaxed);
            }
            if (!s_g141SkipWrite && fragmentAlpha.writeFramebuffer)
                writePixel(gs, x, y, r, g, b, a);
            // G219 (default-off, DC2_G219_SKYPX=1): identify every draw WRITING Z inside the
            // black rectangle — finds what poisons zdst so the sky's GEQUAL fails there.
            {
                static const bool s_g219w = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219w && zWrite && fragmentAlpha.writeDepth &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    x >= 417 && y <= 140 && ((x & 15) == 1) && ((y & 15) == 1))
                {
                    static std::atomic<uint32_t> s_g219wn{0};
                    const uint32_t n = s_g219wn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u || (n % 4096u) == 0u)
                        std::fprintf(stderr,
                            "[G219:zwr] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x prim=%u "
                            "zi=%u replay=%u\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.tme),
                            ctx.tex0.tbp0, static_cast<uint32_t>(gs->m_prim.type), zi,
                            t_g144InReplay ? 1u : 0u);
                }
            }
            if (zWrite && fragmentAlpha.writeDepth)
            {
                // G45: costume model (fbp=0x139) writes its private, non-aliasing Z buffer so it
                // neither corrupts nor is corrupted by the menu text VRAM it would otherwise alias.
                // G203: universal-Z writes the real VRAM Z for every target (private buffers retired).
                if (!g203Uni && ctx.frame.fbp == 0x139u)
                    costumeZWrite(x, y, zi);
                else if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled())
                    g106TitleZWrite(x, y, ctx.frame.fbp, zi);
                else if (z16)
                    GSMem::WriteZ16(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
                else if (z24)
                    GSMem::WriteZ24(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
                else
                    GSMem::WriteZ32(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
            }
            ++drawnAcc;
        }
        }
    };
    // G143 dispatch: sequential unless DC2_G143_THREADS>1 AND the triangle is big enough AND no
    // per-pixel diagnostic is active (those use shared static/atomic state that would race). The
    // costume RTT (fbp=0x139) is excluded. Bit-exact by construction; DEFAULT OFF pending a
    // full-frame golden diff before it is ever promoted to default-on.
    {
        static const int s_g143Threads = []() {
            const char *e = std::getenv("DC2_G143_THREADS");
            int n = e ? std::atoi(e) : 0;
            return n < 0 ? 0 : (n > 16 ? 16 : n);
        }();
        // NOTE: DC2_PERF is intentionally NOT a gate — under threading each lane increments its own
        // local counters (no race; the per-triangle coverage stat just reads 0), while the frame /
        // GSraster wall-clock scopes are measured on the guest thread and stay valid, so the speedup
        // is measurable with DC2_PERF=1. DC2_G82_ROCK genuinely mutates shared state per pixel → gate.
        static const bool s_g143G82 = (std::getenv("DC2_G82_ROCK") != nullptr);
        const bool g143DiagActive =
            s_g143G82 || g43FaceUv || g44FaceZ ||
            (g106TitleZ && g106TitleZTraceEnabled());
        // Thresholds env-tunable for sweeps (row-within-tri threading only pays when a lane's pixel
        // work >> the per-dispatch barrier cost; default high so only genuinely big tris thread).
        // Defaults are the measured sweet spot: threading pays down to fairly small tris here (the
        // cavern is many medium tris, not a few huge ones), and GSraster plateaus ~129 ms (from
        // ~154) at threads=8. Below this the per-dispatch barrier starts to dominate.
        static const int s_g143MinRows = []() { const char *e = std::getenv("DC2_G143_MINROWS"); return e ? std::atoi(e) : 8; }();
        static const long s_g143MinArea = []() { const char *e = std::getenv("DC2_G143_MINAREA"); return e ? std::atol(e) : 512L; }();
        const int g143Rows = maxY - minY + 1;
        const bool g143DoThread =
            s_g143Threads > 1 && !g143DiagActive && g141FastSampleTri &&
            ctx.frame.fbp != 0x139u && g143Rows >= s_g143MinRows &&
            !t_g144InReplay && // never nest G143 row-threads inside a G144 band replay (same pool)
            (static_cast<long>(g143Rows) * static_cast<long>(maxX - minX + 1)) >= s_g143MinArea;
        if (g143DoThread)
        {
            GSRowPool::instance().run(minY, maxY, s_g143Threads, [&](int a, int b) {
                long ia = 0;
                int da = 0;
                g143RangeFn(a, b, ia, da);
            });
        }
        else
        {
            g143RangeFn(minY, maxY, g141InsidePx, f516cov);
        }
    }

    // G141 sub-profile flush (one atomic add per triangle, gated).
    {
        static const bool s_g141GsPerfOn = (std::getenv("DC2_PERF") != nullptr);
        if (s_g141GsPerfOn)
        {
            g_g141BboxPx.fetch_add(static_cast<uint64_t>(g141BboxPx), std::memory_order_relaxed);
            g_g141InsidePx.fetch_add(static_cast<uint64_t>(g141InsidePx), std::memory_order_relaxed);
            g_g141DrawnPx.fetch_add(static_cast<uint64_t>(f516cov), std::memory_order_relaxed);
        }
    }

    if (f516)
    {
        static std::atomic<uint32_t> s_f517end{0};
        if (s_f517end.fetch_add(1u) < 80u)
            std::fprintf(stderr,
                "[F51.7:tri] OK fbp=0x%x tme=%u cov=%d tbp=0x%x psm=0x%x fst=%u bbox=(%d,%d)-(%d,%d) "
                "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) st0=(%.3f,%.3f) of=(%d,%d)\n",
                ctx.frame.fbp, (unsigned)gs->m_prim.tme, f516cov, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm, (unsigned)gs->m_prim.fst,
                minX, minY, maxX, maxY,
                v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v0.s, v0.t, ofx, ofy);
    }
    if (f53map)
    {
        const uint32_t total = s_f53MapTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool positive = f516cov > 0;
        const uint32_t classCount = positive
            ? s_f53MapPositive.fetch_add(1u, std::memory_order_relaxed) + 1u
            : s_f53MapZero.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((positive && classCount <= 32u) || (!positive && classCount <= 16u))
            std::fprintf(stderr,
                "[F53:tri] total=%u class=%s classCount=%u fbp=0x%x cov=%d "
                "bbox=(%d,%d)-(%d,%d) rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) "
                "rawv2=(%.1f,%.1f,%.1f) of=(%d,%d)\n",
                total, positive ? "positive" : "zero", classCount, ctx.frame.fbp, f516cov,
                minX, minY, maxX, maxY,
                v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                ofx, ofy);
        if ((total % 256u) == 0u &&
            s_f53MapSummary.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr,
                "[F53:tri-summary] total=%u positive=%u zero=%u\n",
                total,
                s_f53MapPositive.load(std::memory_order_relaxed),
                s_f53MapZero.load(std::memory_order_relaxed));
    }
}