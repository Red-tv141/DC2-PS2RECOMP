void GSRasterizer::drawSprite(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();
    G248SpriteScope g248Scope(ctx.frame.fbp, gs->m_prim.tme, gs->m_prim.abe);
    // G248 state census (DEFAULT OFF): print each distinct RTT-sprite state once.  Kept separate
    // from DC2_G248_STAT because stderr I/O would distort the timing discriminator above.
    {
        static const bool s_g248State = (std::getenv("DC2_G248_STATE") != nullptr);
        if (s_g248State && g248TargetIndex(ctx.frame.fbp) >= 0)
        {
            char key[768];
            std::snprintf(
                key, sizeof(key),
                "fbp=0x%x fbw=%u fpsm=0x%x fbmsk=0x%x prim(tme=%u abe=%u fst=%u ctxt=%u) "
                "tex(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x "
                "cpsm=0x%x csm=%u csa=%u cld=%u) tex1=0x%llx clamp=0x%llx alpha=0x%llx "
                "test=0x%llx zbuf=0x%llx texa(ta0=%u aem=%u ta1=%u) "
                "texclut(cbw=%u cou=%u cov=%u) "
                "pabe=%u fba=0x%llx",
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), ctx.frame.fbmsk,
                static_cast<uint32_t>(gs->m_prim.tme), static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.ctxt),
                ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tw),
                static_cast<uint32_t>(ctx.tex0.th), static_cast<uint32_t>(ctx.tex0.tcc),
                static_cast<uint32_t>(ctx.tex0.tfx), ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm), static_cast<uint32_t>(ctx.tex0.csm),
                static_cast<uint32_t>(ctx.tex0.csa), static_cast<uint32_t>(ctx.tex0.cld),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf),
                static_cast<uint32_t>(gs->m_texa.ta0),
                static_cast<uint32_t>(gs->m_texa.aem),
                static_cast<uint32_t>(gs->m_texa.ta1),
                static_cast<uint32_t>(gs->m_texclut.cbw),
                static_cast<uint32_t>(gs->m_texclut.cou),
                static_cast<uint32_t>(gs->m_texclut.cov),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<unsigned long long>(ctx.fba));
            static std::mutex s_mutex;
            static std::unordered_set<std::string> s_seen;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_seen.size() < 128u && s_seen.insert(key).second)
                std::fprintf(stderr, "[G248:rttstate] %s\n", key);
        }
    }

    // G219 (default-off, DC2_G219_SKYPX=1): sprite writer log at the two probe pixels inside the
    // zoom black rectangle (mirrors the drawTriangle skycol probe, so the full writer sequence at
    // one pixel can be reconstructed across both primitive paths).
    auto g219SpritePixelProbe = [&](const auto &pctx, GS *pgs, int px, int py,
                                    uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa, int texd) {
        static const bool s_g219sp = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (!s_g219sp)
            return;
        if (!((px == 470 && py == 30) || (px == 500 && py == 100)))
            return;
        if (pctx.frame.fbp != 0x0u && pctx.frame.fbp != 0x68u)
            return;
        static std::atomic<uint32_t> s_g219spn{0};
        const uint32_t n = s_g219spn.fetch_add(1u, std::memory_order_relaxed);
        if (n < 2000u)
            std::fprintf(stderr,
                "[G219:sprcol] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x abe=%u texd=%d "
                "rgba=(%u,%u,%u,%u) replay=%u\n",
                n, px, py, pctx.frame.fbp, static_cast<uint32_t>(pgs->m_prim.tme),
                pctx.tex0.tbp0, static_cast<uint32_t>(pgs->m_prim.abe), texd,
                pr, pg, pb, pa, t_g144InReplay ? 1u : 0u);
    };

    // G203: honor the guest's real ZBUF/TEST on SPRITES too. DC2 CLEARS its Z buffer every frame
    // via a full-screen ztst=ALWAYS,zmsk=0 sprite (seen in ttle/map_0/Inventory .gs freezes);
    // drawSprite previously ignored Z entirely, so that clear never reached VRAM and the triangle
    // path read stale Z — which is why prior phases used private title/costume Z buffers and the
    // town artificial clear. Reproducing the clear here lets universal-Z retire all of those.
    // Pure ztst=ALWAYS + zmsk (no-op: always passes, never writes) sprites skip the Z work.
    static const bool s_spriteZDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const bool sZuni = g203UniversalZEnabled();
    const uint32_t sZte  = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t sZtst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    const uint32_t sZbp  = static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5;
    const uint32_t sZpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool sZmsk     = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const uint32_t sZbw  = ctx.frame.fbw;
    const bool sZ16 = (sZpsm == 0x2u || sZpsm == 0xAu || sZpsm == 0xBu);
    const bool sZ24 = (sZpsm == 0x1u);
    const bool sZActive = sZuni && !s_spriteZDisabled && sZte != 0u && gs->m_vram != nullptr &&
                          !(sZtst == 1u && sZmsk); // ALWAYS + no-write == no-op, fast-skip
    uint32_t sZi = 0u;
    if (sZActive)
    {
        float zf = v1.z; // sprite Z is flat: the kick (2nd) vertex's z
        if (zf < 0.0f) zf = 0.0f;
        const float zmax = sZ16 ? 65535.0f : (sZ24 ? 16777215.0f : 4294967295.0f);
        if (zf > zmax) zf = zmax;
        sZi = static_cast<uint32_t>(zf);
    }
    const bool sZWrite = sZActive && !sZmsk && sZtst != 0u; // ZTST=NEVER never writes
    // G219 (default-off, DC2_G219_SKYPX=1): flag any sprite whose Z BUFFER lands in the
    // 0x139-0x140 work-page range — a deferred display sprite with such a zbp would zero the
    // work page during replay (the [G219:dif] clobber signature).
    {
        static const bool s_g219z = (std::getenv("DC2_G219_SKYPX") != nullptr);
        const uint32_t sZbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
        if (s_g219z && sZWrite && sZbpPage >= 0x139u && sZbpPage <= 0x148u)
        {
            static std::atomic<uint32_t> s_g219zn{0};
            const uint32_t n = s_g219zn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
                std::fprintf(stderr,
                    "[G219:spzbp] n=%u fbp=0x%x zbpPage=0x%x zpsm=0x%x ztst=%u zi=%u tme=%u "
                    "tbp=0x%x v=(%.0f,%.0f)-(%.0f,%.0f) replay=%u\n",
                    n, ctx.frame.fbp, sZbpPage, sZpsm, sZtst, sZi,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    v0.x, v0.y, v1.x, v1.y, t_g144InReplay ? 1u : 0u);
        }
    }
    auto spriteZPass = [&](int px, int py) -> bool {
        if (!sZActive) return true;
        const uint32_t zdst =
            sZ16 ? GSMem::ReadZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                 : (sZ24 ? GSMem::ReadZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                         : GSMem::ReadZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py)));
        switch (sZtst)
        {
        case 0:  return false;          // NEVER
        case 1:  return true;           // ALWAYS
        case 2:  return sZi >= zdst;    // GEQUAL
        default: return sZi >  zdst;    // GREATER
        }
    };
    auto spriteZStore = [&](int px, int py) {
        if (!sZWrite) return;
        if (sZ16)      GSMem::WriteZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else if (sZ24) GSMem::WriteZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else           GSMem::WriteZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
    };

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
    {
        // maybe a log here idk ?
        return;
    }

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);
    // G173: during a tile-binning flush, restrict this sprite to the calling lane's scanline band,
    // exactly like drawTriangle's clamp — without this, every lane whose band intersects the bbox
    // replayed the FULL rect (cross-lane write races, alpha blend applied once per lane, submission
    // order broken across bands = the G172 costume prompt-box regression). Row UV interpolation is
    // anchored on unclippedY0, so clamping the row range is exact, not an approximation. If the
    // intersection is empty the loops run zero iterations.
    if (t_g144Banded)
    {
        if (drawY0 < t_g144BandY0) drawY0 = t_g144BandY0;
        if (drawY1 > t_g144BandY1) drawY1 = t_g144BandY1;
    }
    if (drawX1 >= drawX0 && drawY1 >= drawY0)
        g248Scope.setPixels(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                            static_cast<uint64_t>(drawY1 - drawY0 + 1));

    // G219 (default-off, DC2_G219_SKYPX=1): log any SPRITE touching Z inside the zoom black
    // rectangle (per-sprite, not per-pixel) — verifies the game's Z-clear sprite reaches
    // cols 417-511 / rows 0-140 and with what Z value.
    {
        static const bool s_g219s = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219s && (sZActive || sZWrite) &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            drawX1 >= 417 && drawY0 <= 140)
        {
            static std::atomic<uint32_t> s_g219sn{0};
            const uint32_t n = s_g219sn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u || (n % 1024u) == 0u)
                std::fprintf(stderr,
                    "[G219:spz] n=%u fbp=0x%x rect=(%d,%d)-(%d,%d) ztst=%u zmsk=%u zwr=%u "
                    "zi=%u tme=%u tbp=0x%x replay=%u banded=%u\n",
                    n, ctx.frame.fbp, drawX0, drawY0, drawX1, drawY1, sZtst,
                    static_cast<uint32_t>(sZmsk), static_cast<uint32_t>(sZWrite), sZi,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u);
        }
    }

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    const bool looksLikeDisplayCopy =
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        gs->m_prim.fst &&
        gs->m_prim.ctxt &&
        ctx.frame.fbp != ctx.tex0.tbp0 &&
        alphaMode == 0x64u &&
        (alphaFix == 0x60u || alphaFix == 0x80u) &&
        unclippedX0 <= 0 &&
        unclippedY0 <= 0 &&
        unclippedX1 >= 639 &&
        unclippedY1 >= 447;
    if (looksLikeDisplayCopy)
    {
        gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
        gs->m_hasPreferredDisplaySource = true;
    }

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    // G9: broad trace of every wide costume-screen sprite (textured or not) to
    // locate how the highlight bar is drawn and from which texture page.
    {
        static const bool g9b = envFlagEnabled("DC2_TRACE_G9");
        if (g9b && spanX >= 100 && spanY <= 60)
        {
            static std::atomic<uint32_t> s_g9b{0};
            const uint32_t nb = s_g9b.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (nb <= 20u || (nb % 600u) == 0u)
                std::fprintf(stderr,
                    "[G9:wide] n=%u tme=%u abe=%u tbp=0x%x psm=0x%x cbp=0x%x csa=%u "
                    "scr=(%d,%d)-(%d,%d) spanX=%d spanY=%d rgba=(%u,%u,%u,%u) alpha=0x%llx\n",
                    nb, gs->m_prim.tme, gs->m_prim.abe, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                    ctx.tex0.cbp, (unsigned)ctx.tex0.csa, x0, y0, x1, y1, spanX, spanY,
                    v1.r, v1.g, v1.b, v1.a, static_cast<unsigned long long>(ctx.alpha));
            // G11: one-shot decoded dump of the bar texture AT costume-draw time, using
            // the draw's own TEX0/CLUT, so we see whether 0x2aa0 holds the fukusel sheet
            // or stale title data.
            if (gs->m_prim.tme && (ctx.tex0.tbp0 == 0x2aa0u || ctx.tex0.tbp0 == 0x2720u))
            {
                static std::atomic<uint32_t> s_seen2aa0{0}, s_seen2720{0};
                std::atomic<uint32_t> *sp = (ctx.tex0.tbp0 == 0x2aa0u) ? &s_seen2aa0 : &s_seen2720;
                if (sp->fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    const auto &tx = ctx.tex0;
                    const int dw = 1 << tx.tw, dh = 1 << tx.th;
                    char path[128];
                    std::snprintf(path, sizeof(path), "captures/g11_bar_%x_cbp%x.ppm", tx.tbp0, tx.cbp);
                    FILE *fp = std::fopen(path, "wb");
                    if (fp)
                    {
                        std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                        for (int yy = 0; yy < dh; ++yy)
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tx.tbp0,
                                                 tx.tbw >> 1, (uint32_t)xx, (uint32_t)yy));
                                uint32_t c = lookupCLUT(gs, ix, tx.cbp, tx.cpsm, tx.csm, tx.csa, tx.psm);
                                uint8_t rgb[3] = { (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF) };
                                std::fwrite(rgb, 1, 3, fp);
                            }
                        std::fclose(fp);
                        std::fprintf(stderr, "[G11:bardump] wrote %s dw=%d dh=%d tbw=%u cbp=0x%x csa=%u\n",
                            path, dw, dh, tx.tbw, tx.cbp, (unsigned)tx.csa);
                    }
                }
            }
        }
    }

    if (gs->m_prim.tme)
    {
        const auto &tex = ctx.tex0;
        int texW = 1 << tex.tw;
        int texH = 1 << tex.th;
        if (texW == 0)
            texW = 1;
        if (texH == 0)
            texH = 1;

        // G9: env-gated trace of the costume-select highlight bar (wide T8 sprite
        // from the 0x2720 manager atlas). Logs full CLUT params + the sampled centre
        // texel/colour so the runner's brown-vs-dark can be A/B'd vs Select_costume.gs.
        {
            static const bool g9trace = envFlagEnabled("DC2_TRACE_G9");
            if (g9trace && (tex.tbp0 == 0x2720u || tex.tbp0 == 0x2aa0u))
            {
                const int sX = std::abs(static_cast<int>(v1.x) - static_cast<int>(v0.x)) >> 4;
                if (sX >= 150)
                {
                    static std::atomic<uint32_t> s_g9{0};
                    const uint32_t n = s_g9.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 24u)
                    {
                        const int cu = ((static_cast<int>(v0.u) + static_cast<int>(v1.u)) / 2) >> 4;
                        const int cv = ((static_cast<int>(v0.v) + static_cast<int>(v1.v)) / 2) >> 4;
                        uint32_t off = GSPSMT8::addrPSMT8(tex.tbp0, tex.tbw,
                            static_cast<uint32_t>(cu), static_cast<uint32_t>(cv));
                        uint8_t ix = (off < gs->m_vramSize) ? gs->m_vram[off] : 0u;
                        uint32_t col = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        std::fprintf(stderr,
                            "[G9:bar] n=%u tbp=0x%x tbw=%u cbp=0x%x cpsm=%u csm=%u csa=%u "
                            "scr=(%d,%d)-(%d,%d) uv0=(%d,%d) uv1=(%d,%d) ctr=(%d,%d) ix=0x%02x col=0x%08x "
                            "vrgba=(%u,%u,%u,%u) alpha=0x%llx\n",
                            n, tex.tbp0, tex.tbw, tex.cbp, (unsigned)tex.cpsm, (unsigned)tex.csm,
                            (unsigned)tex.csa, x0, y0, x1, y1,
                            static_cast<int>(v0.u) >> 4, static_cast<int>(v0.v) >> 4,
                            static_cast<int>(v1.u) >> 4, static_cast<int>(v1.v) >> 4,
                            cu, cv, ix, col, v1.r, v1.g, v1.b, v1.a,
                            static_cast<unsigned long long>(ctx.alpha));
                    }
                }
            }
        }

        // G8: env-gated one-shot trace of the 2D sprite sampling parameters
        // (filter mode, UV span vs screen span) to characterise font/HUD blur+dots.
        {
            static const bool g8trace = envFlagEnabled("DC2_TRACE_G8");
            if (g8trace)
            {
                static std::atomic<uint32_t> s_g8{0};
                const uint32_t n = s_g8.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 48u)
                {
                    const bool lin = tex1UsesLinearFilter(ctx.tex1);
                    const int uvu = (static_cast<int>(v1.u) - static_cast<int>(v0.u));
                    const int uvv = (static_cast<int>(v1.v) - static_cast<int>(v0.v));
                    std::fprintf(stderr,
                        "[G8:sprite] n=%u tbp=0x%x psm=0x%x tw=%d th=%d tex1=0x%llx lin=%d tfx=%u tcc=%u "
                        "scr=(%d,%d)-(%d,%d) spanX=%d spanY=%d uv0=(%d,%d) uv1=(%d,%d) duv16=(%d,%d) wrap(s=%u,t=%u)\n",
                        n, tex.tbp0, static_cast<uint32_t>(tex.psm), texW, texH,
                        static_cast<unsigned long long>(ctx.tex1), lin ? 1 : 0,
                        static_cast<uint32_t>(tex.tfx), static_cast<uint32_t>(tex.tcc),
                        x0, y0, x1, y1, spanX, spanY,
                        static_cast<int>(v0.u), static_cast<int>(v0.v),
                        static_cast<int>(v1.u), static_cast<int>(v1.v),
                        uvu, uvv,
                        static_cast<uint32_t>(ctx.clamp & 0x3u),
                        static_cast<uint32_t>((ctx.clamp >> 2) & 0x3u));
                }
            }
        }

        float u0f, v0f, u1f, v1f;
        if (gs->m_prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // G9: decisive one-shot diagnostic for the costume-screen black UI boxes
        // (textured T8 sprites from tbp=0x2720, CLUT cbp=0x3fdc rendering black).
        // Distinguishes "atlas region empty" from "CLUT empty" from "center index/color".
        {
            static const bool g9 = envFlagEnabled("DC2_TRACE_COSTUME");
            if (g9 && gs->m_prim.tme && tex.tbp0 == 0x2720u && tex.psm == GS_PSM_T8)
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 40u)
                {
                    const int cu = static_cast<int>((u0f + u1f) * 0.5f);
                    const int cv = static_cast<int>((v0f + v1f) * 0.5f);
                    const uint32_t centerColor =
                        sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                      static_cast<uint16_t>(clampInt(cu * 16, 0, 0xFFFF)),
                                      static_cast<uint16_t>(clampInt(cv * 16, 0, 0xFFFF)));
                    // CLUT @ cbp first 4 CT32 entries (raw VRAM bytes).
                    const uint32_t clutByteOff = tex.cbp * 256u;
                    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                    if (clutByteOff + 16u <= gs->m_vramSize)
                    {
                        std::memcpy(&c0, gs->m_vram + clutByteOff + 0, 4);
                        std::memcpy(&c1, gs->m_vram + clutByteOff + 4, 4);
                        std::memcpy(&c2, gs->m_vram + clutByteOff + 8, 4);
                        std::memcpy(&c3, gs->m_vram + clutByteOff + 12, 4);
                    }
                    // Nonzero byte count in the 0x2720 atlas page (0x272000..0x282000).
                    uint32_t atlasNz = 0;
                    for (uint32_t o = 0x272000u; o < 0x282000u && o < gs->m_vramSize; ++o)
                        if (gs->m_vram[o]) ++atlasNz;
                    std::fprintf(stderr,
                        "[G9:costume] n=%u scr=(%d,%d)-(%d,%d) cu=%d cv=%d cbp=0x%x csa=%u "
                        "centerColor=0x%08x clut[0..3]=%08x,%08x,%08x,%08x atlas0x2720nz=%u tbw=%u\n",
                        n, x0, y0, x1, y1, cu, cv, tex.cbp, static_cast<uint32_t>(tex.csa),
                        centerColor, c0, c1, c2, c3, atlasNz, static_cast<uint32_t>(tex.tbw));
                }
            }
        }

        // G8: per-pixel sampling bias for axis-aligned (FST) sprites. The PS2 GS
        // truncates the interpolated texel coordinate; sampling at the pixel CENTER
        // (+0.5) on a minified font (e.g. 9 source texels mapped to 8 pixels) drops a
        // MIDDLE texel column (breaks glyph strokes) and keeps the trailing inter-glyph
        // gap texel (adds speckle). Left-edge sampling (bias 0.0) instead drops only the
        // trailing gap texel, matching the crisp reference. Override via DC2_GS_UV_BIAS.
        static const float s_uvBias = []() {
            const char *e = std::getenv("DC2_GS_UV_BIAS");
            return (e && *e) ? static_cast<float>(std::atof(e)) : 0.0f;
        }();

        // G248/G250 (PERF, DEFAULT ON): the character/effect RTT family is dominated by
        // textured sprite sampling (~138 ms/guest-frame on map_0).  sampleTexture() re-decodes
        // invariant GS texture state for every pixel and, for the observed PSMT8 linear sprites,
        // repeats four CLUT decodes per pixel.  Hoist the exact point/bilinear sampler used by
        // sampleTexture(), add a per-draw CLUT cache, and reuse an identical bilinear texel quad.
        // Scope is deliberately narrow: only the measured RTT targets, FST sprites, CT32/CT24/T8,
        // default T8 swizzle, no quality/phase traces, and no G45/G51 costume filter override.
        // Unsupported or diagnostic states retain the existing sampleTexture() path.  G250
        // promoted the path after the cross-route soak; DC2_G248_NO_FASTSPRITE=1 is the rollback.
        // DC2_G248_FASTSPRITE remains accepted as a compatibility no-op for older harnesses.
        // Prove per-pixel parity with DC2_G248_VERIFY=1 (bad must remain 0).
        static const bool s_g248FastSprite =
            (std::getenv("DC2_G248_NO_FASTSPRITE") == nullptr);
        static const bool s_g248Verify = (std::getenv("DC2_G248_VERIFY") != nullptr);
        static const bool s_g248NoClutCache = (std::getenv("DC2_G248_NO_CLUTCACHE") != nullptr);
        static const bool s_g248NoTexQuad = (std::getenv("DC2_G248_NO_TEXQUAD") != nullptr);
        static const bool s_g248T8GsmemDefault =
            !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
        static const bool s_g248T8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);
        const uint8_t g248Psm = tex.psm;
        const bool g248FastPsm =
            g248Psm == GS_PSM_CT32 || g248Psm == GS_PSM_CT24 ||
            (g248Psm == GS_PSM_T8 && s_g248T8GsmemDefault && !s_g248T8AliasSet);
        // G268: these four were raw getenv() per sprite draw (and per banded replay lane).
        static const bool s_g248EnvTraceOff = std::getenv("DC2_DUMP_FONT") == nullptr &&
                                              std::getenv("DC2_DUMP_VRAMRGN") == nullptr;
        static const bool s_g248EnvFilterOff = std::getenv("DC2_G45_BILINEAR") == nullptr &&
                                               std::getenv("DC2_G51_MINFILT") == nullptr;
        const bool g248TraceOff =
            !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() &&
            !f50_12_trace_enabled() &&
            (g268LiveEnvRead()
                 ? (std::getenv("DC2_DUMP_FONT") == nullptr &&
                    std::getenv("DC2_DUMP_VRAMRGN") == nullptr)
                 : s_g248EnvTraceOff);
        const bool g248SpecialFilterOff =
            g268LiveEnvRead()
                ? (std::getenv("DC2_G45_BILINEAR") == nullptr &&
                   std::getenv("DC2_G51_MINFILT") == nullptr)
                : s_g248EnvFilterOff;
        const bool g248FastSampleSprite =
            s_g248FastSprite && g248TargetIndex(ctx.frame.fbp) >= 0 && gs->m_prim.fst &&
            g248FastPsm && tex.tbw != 0u && g248TraceOff && g248SpecialFilterOff;
        const uint8_t g248WrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
        const uint8_t g248WrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
        const int g248MinU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
        const int g248MaxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
        const int g248MinV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
        const int g248MaxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
        const bool g248Linear = tex1UsesLinearFilter(ctx.tex1);
        const bool g248UseClutCache =
            g248FastSampleSprite && g248Psm == GS_PSM_T8 && !s_g248NoClutCache;
        // Thread-local avoids growing/clearing every drawSprite stack frame while the prototype is
        // disabled.  Each eligible paletted draw explicitly invalidates its own entries below.
        static thread_local uint32_t g248Clut[256];
        static thread_local uint8_t g248ClutValid[256];
        if (g248UseClutCache)
            std::memset(g248ClutValid, 0, sizeof(g248ClutValid));
        auto g248ClutLookup = [&](uint8_t idx) -> uint32_t {
            if (!g248ClutValid[idx])
            {
                g248Clut[idx] = lookupCLUT(gs, idx, tex.cbp, tex.cpsm,
                                           tex.csm, tex.csa, tex.psm);
                g248ClutValid[idx] = 1u;
            }
            return g248Clut[idx];
        };
        auto g248SamplePoint = [&](int su0, int sv0) -> uint32_t {
            const int su = wrapTextureCoordinate(su0, g248WrapU, texW, g248MinU, g248MaxU);
            const int sv = wrapTextureCoordinate(sv0, g248WrapV, texH, g248MinV, g248MaxV);
            if (g248Psm == GS_PSM_CT32 || g248Psm == GS_PSM_CT24)
                return applyTexa(gs->m_texa, g248Psm,
                                 readTexelPSMCT32(gs, tex.tbp0, tex.tbw, su, sv));
            const uint32_t pageBwT8 = ((tex.tbw >> 1u) != 0u) ? (tex.tbw >> 1u) : 1u;
            const uint8_t idx = static_cast<uint8_t>(
                GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                              static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            return g248UseClutCache
                ? g248ClutLookup(idx)
                : lookupCLUT(gs, idx, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        };
        bool g248QValid = false;
        int g248QU0 = 0, g248QV0 = 0;
        uint32_t g248QC00 = 0, g248QC10 = 0, g248QC01 = 0, g248QC11 = 0;
        auto g248FastSample = [&](uint16_t u, uint16_t v) -> uint32_t {
            const float texUf = static_cast<float>(u) / 16.0f;
            const float texVf = static_cast<float>(v) / 16.0f;
            if (!g248Linear)
                return g248SamplePoint(static_cast<int>(texUf), static_cast<int>(texVf));

            const float sampleU = texUf - 0.5f;
            const float sampleV = texVf - 0.5f;
            const int u0 = static_cast<int>(std::floor(sampleU));
            const int v0 = static_cast<int>(std::floor(sampleV));
            const int u1 = u0 + 1;
            const int v1 = v0 + 1;
            const float fx = sampleU - static_cast<float>(u0);
            const float fy = sampleV - static_cast<float>(v0);
            uint32_t c00, c10, c01, c11;
            if (!s_g248NoTexQuad && g248QValid && u0 == g248QU0 && v0 == g248QV0)
            {
                c00 = g248QC00; c10 = g248QC10; c01 = g248QC01; c11 = g248QC11;
            }
            else
            {
                c00 = g248SamplePoint(u0, v0);
                c10 = g248SamplePoint(u1, v0);
                c01 = g248SamplePoint(u0, v1);
                c11 = g248SamplePoint(u1, v1);
                g248QValid = true; g248QU0 = u0; g248QV0 = v0;
                g248QC00 = c00; g248QC10 = c10; g248QC01 = c01; g248QC11 = c11;
            }
            const uint8_t rr = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                            static_cast<uint8_t>(c10 & 0xFFu),
                                            static_cast<uint8_t>(c01 & 0xFFu),
                                            static_cast<uint8_t>(c11 & 0xFFu), fx, fy);
            const uint8_t gg = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 8) & 0xFFu), fx, fy);
            const uint8_t bb = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 16) & 0xFFu), fx, fy);
            const uint8_t aa = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 24) & 0xFFu), fx, fy);
            return static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8) |
                   (static_cast<uint32_t>(bb) << 16) | (static_cast<uint32_t>(aa) << 24);
        };

        for (int y = drawY0; y <= drawY1; ++y)
        {
            g248QValid = false;
            float ty = (static_cast<float>(y - unclippedY0) + s_uvBias) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + s_uvBias) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                uint16_t g258SampleU = 0u, g258SampleV = 0u;
                if (gs->m_prim.fst)
                {
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texUf * 16.0f)), 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texVf * 16.0f)), 0, 0xFFFF));
                    g258SampleU = sampleU;
                    g258SampleV = sampleV;
                    if (g248FastSampleSprite)
                    {
                        texel = g248FastSample(sampleU, sampleV);
                        if (s_g248Verify)
                        {
                            const uint32_t ref = sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                                               sampleU, sampleV);
                            static std::atomic<uint64_t> s_g248VerifyTotal{0}, s_g248VerifyBad{0};
                            const uint64_t total =
                                s_g248VerifyTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (ref != texel)
                            {
                                const uint64_t bad =
                                    s_g248VerifyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (bad <= 16u)
                                    std::fprintf(stderr,
                                        "[G248:vfy] MISMATCH ref=%08x fast=%08x fbp=0x%x "
                                        "psm=0x%x uv=(%u,%u) linear=%u\n",
                                        ref, texel, ctx.frame.fbp, static_cast<uint32_t>(g248Psm),
                                        static_cast<uint32_t>(sampleU),
                                        static_cast<uint32_t>(sampleV),
                                        static_cast<uint32_t>(g248Linear));
                                texel = ref;
                            }
                            if ((total & 0xFFFFFull) == 0ull)
                                std::fprintf(stderr, "[G248:vfy] tot=%llu bad=%llu\n",
                                    static_cast<unsigned long long>(total),
                                    static_cast<unsigned long long>(
                                        s_g248VerifyBad.load(std::memory_order_relaxed)));
                        }
                    }
                    else
                    {
                        texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                    }
                }
                else
                {
                    texel = sampleTexture(gs,
                                          texUf / static_cast<float>(texW),
                                          texVf / static_cast<float>(texH),
                                          1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                if (g258TraceOn() && g_g255Verify.pending && g_g255Verify.fbp == 0x146u &&
                    g_g255Verify.traceBatch <= 4u && x == 52 && y == 1 && gs->m_prim.fst)
                {
                    const float su = static_cast<float>(g258SampleU) / 16.0f - 0.5f;
                    const float sv = static_cast<float>(g258SampleV) / 16.0f - 0.5f;
                    const int pu = static_cast<int>(std::floor(su));
                    const int pv = static_cast<int>(std::floor(sv));
                    const int fru = static_cast<int>(std::lround((su - static_cast<float>(pu)) * 16.0f));
                    const int frv = static_cast<int>(std::lround((sv - static_cast<float>(pv)) * 16.0f));
                    const uint32_t c00 = g248SamplePoint(pu, pv);
                    const uint32_t c10 = g248SamplePoint(pu + 1, pv);
                    const uint32_t c01 = g248SamplePoint(pu, pv + 1);
                    const uint32_t c11 = g248SamplePoint(pu + 1, pv + 1);
                    std::fprintf(stderr,
                        "[G258:cpu.sample] batch=%llu entry=%zu uv16=(%u,%u) p0=(%d,%d) fr=(%d,%d) "
                        "taps=%08x/%08x/%08x/%08x sample=%02x%02x%02x%02x "
                        "vc=%02x%02x%02x%02x src=%02x%02x%02x%02x\n",
                        static_cast<unsigned long long>(g_g255Verify.traceBatch), t_g258EntryIndex,
                        static_cast<uint32_t>(g258SampleU), static_cast<uint32_t>(g258SampleV),
                        pu, pv, fru, frv, c00, c10, c01, c11,
                        tr, tg, tb, ta, r, g, b, a, color.r, color.g, color.b, color.a);
                }
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f31TexSampleCount{0};
                    if (s_f31TexSampleCount.load(std::memory_order_relaxed) < 32u)
                    {
                        const uint32_t n = s_f31TexSampleCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 32u)
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
                                         "[F31:texsample] n=%u xy=(%d,%d) tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u cbp=0x%x texel=%02x,%02x,%02x,%02x vtx=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x\n",
                                         n, x, y, static_cast<unsigned long long>(tex0Raw),
                                         tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                         static_cast<uint32_t>(tex.psm),
                                         static_cast<uint32_t>(tex.tfx),
                                         static_cast<uint32_t>(tex.tcc),
                                         tex.cbp,
                                         tr, tg, tb, ta,
                                         r, g, b, a,
                                         color.r, color.g, color.b, color.a);
                        }
                    }
                }
                if (!spriteZPass(x, y)) // G203: guest Z test
                    continue;
                const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, color.a);
                g219SpritePixelProbe(ctx, gs, x, y, color.r, color.g, color.b, color.a, 1);
                if (alphaTest.writeFramebuffer)
                    writePixel(gs, x, y, color.r, color.g, color.b, color.a);
                if (alphaTest.writeDepth)
                    spriteZStore(x, y); // G203: guest Z write (e.g. the game's clear-sprite)
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
            {
                if (!spriteZPass(x, y)) // G203: guest Z test
                    continue;
                const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);
                g219SpritePixelProbe(ctx, gs, x, y, r, g, b, a, 0);
                if (alphaTest.writeFramebuffer)
                    writePixel(gs, x, y, r, g, b, a);
                if (alphaTest.writeDepth)
                    spriteZStore(x, y); // G203: guest Z write (e.g. the game's clear-sprite)
            }
    }
}

// G144: drain any trailing deferred triangles at frame end (called from the mgEndFrame override on
// the GUEST thread, before the frame is presented) so tris submitted after the last non-deferrable
// primitive still reach VRAM this frame. No-op when tile-binning is off or the list is empty. Same
// thread as capture ⇒ no data race on g_g144List.
static void g178CensusScan();

void g144FlushPending()
{
    g324NoteObserver(2u); // frame/presentation boundary
    // G305: fence any trailing async packet at the frame boundary (before present), even when the
    // deferred list is empty because the last flush submitted async. Under the default MTGS+pipeline
    // config this runs on the same GS-worker thread as the mid-frame flushes (see g150_frame_barrier
    // note below), so the pending state is single-threaded.
    if (g305AsyncOn())
        g305DrainPending();
    g248Report();
    // G219 (default-off, DC2_G219_SKYPX=1): log the frame-end drain cadence + pending size so
    // "how long do deferred entries live before the trailing flush" is directly visible.
    {
        static const bool s_g219fl = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219fl)
        {
            static std::atomic<uint32_t> s_g219fln{0};
            const uint32_t n = s_g219fln.fetch_add(1u, std::memory_order_relaxed);
            if (n < 200u || (n % 64u) == 0u || !g_g144List.empty())
                std::fprintf(stderr, "[G219:framefl] n=%u list=%zu\n", n, g_g144List.size());
        }
    }
    // G148 sampler-stat dump (DC2_G148_SAMPLESTAT=1). Fires once per guest frame (this runs from the
    // mgEndFrame override). Prints cumulative + 30-frame-window deltas so we can read the steady-state
    // quad hit/miss ratio and leaf-read volume that decide whether a de-swizzle cache is worthwhile.
    {
        static const bool s_g148Stat = (std::getenv("DC2_G148_SAMPLESTAT") != nullptr);
        if (s_g148Stat)
        {
            static uint64_t s_frame = 0;
            static uint64_t s_lastSamp = 0, s_lastHit = 0, s_lastMiss = 0, s_lastPoint = 0, s_lastLeaf = 0;
            if ((++s_frame % 30ull) == 0ull)
            {
                const uint64_t samp = g_g148TexSamples.load(std::memory_order_relaxed);
                const uint64_t hit = g_g148QuadHit.load(std::memory_order_relaxed);
                const uint64_t miss = g_g148QuadMiss.load(std::memory_order_relaxed);
                const uint64_t point = g_g148PointSamp.load(std::memory_order_relaxed);
                const uint64_t leaf = g_g148LeafReads.load(std::memory_order_relaxed);
                const uint64_t dSamp = samp - s_lastSamp;
                const uint64_t dHit = hit - s_lastHit;
                const uint64_t dMiss = miss - s_lastMiss;
                const uint64_t dPoint = point - s_lastPoint;
                const uint64_t dLeaf = leaf - s_lastLeaf;
                const uint64_t bilinear = dHit + dMiss;
                std::fprintf(stderr,
                    "[G148:samplestat] frame=%llu win=30 | samples/f=%llu bilinear/f=%llu point/f=%llu "
                    "quadHit=%.1f%% leafReads/f=%llu leafPerSample=%.2f\n",
                    (unsigned long long)s_frame,
                    (unsigned long long)(dSamp / 30ull),
                    (unsigned long long)(bilinear / 30ull),
                    (unsigned long long)(dPoint / 30ull),
                    bilinear ? 100.0 * (double)dHit / (double)bilinear : 0.0,
                    (unsigned long long)(dLeaf / 30ull),
                    dSamp ? (double)dLeaf / (double)dSamp : 0.0);
                s_lastSamp = samp; s_lastHit = hit; s_lastMiss = miss; s_lastPoint = point; s_lastLeaf = leaf;
            }
        }
    }
    // G149 cache stats (DC2_G149_TEXCACHE=1): builds/hits/ineligible per 30-frame window. A high
    // hit:build ratio means the decode is amortized across many triangles/bands (the intended win);
    // builds ≈ hits means thrash (VRAM gen churning) — check the upload cadence.
    {
        static const bool s_g149Stat = (std::getenv("DC2_G149_TEXCACHE") != nullptr);
        if (s_g149Stat)
        {
            static uint64_t s_frame = 0;
            static uint64_t s_lastHit = 0, s_lastBuild = 0, s_lastInel = 0, s_lastGpu = 0, s_lastGpuFb = 0;
            if ((++s_frame % 30ull) == 0ull)
            {
                const uint64_t hit = g_g149Hits.load(std::memory_order_relaxed);
                const uint64_t build = g_g149Builds.load(std::memory_order_relaxed);
                const uint64_t inel = g_g149Ineligible.load(std::memory_order_relaxed);
                const uint64_t gpu = g_g162GpuHits.load(std::memory_order_relaxed);
                const uint64_t gpuFb = g_g162GpuFallback.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "[G149:texcache] frame=%llu win=30 | hits/f=%llu builds/f=%llu ineligible/f=%llu gen=%llu | g162gpu/f=%llu g162fallback/f=%llu\n",
                    (unsigned long long)s_frame,
                    (unsigned long long)((hit - s_lastHit) / 30ull),
                    (unsigned long long)((build - s_lastBuild) / 30ull),
                    (unsigned long long)((inel - s_lastInel) / 30ull),
                    (unsigned long long)g_g149VramGen.load(std::memory_order_relaxed),
                    (unsigned long long)((gpu - s_lastGpu) / 30ull),
                    (unsigned long long)((gpuFb - s_lastGpuFb) / 30ull));
                s_lastHit = hit; s_lastBuild = build; s_lastInel = inel; s_lastGpu = gpu; s_lastGpuFb = gpuFb;
            }
        }
    }
    g178CensusScan();
    // G260: the frame boundary is the natural full-graph execution point. Under the default
    // MTGS+pipeline config this function runs INSIDE the worker's frame-boundary marker closure
    // (see g150_frame_barrier), i.e. on the same thread that runs the mid-frame band-pool
    // flushes today — the parallel closure is proven there. In the non-pipelined/serial
    // configs it runs on the EE thread at a point where the band pool is NOT proven (the
    // G144-era present-thread race), so fall back to the sequential closure exactly like the
    // legacy tail drain below.
    if (g260NrOn() && g260HasPending())
    {
        extern bool g150_pipeline_enabled();
        G146PerfScope g146FlushScope(g_g146G144FlushFrameNs, g_g146G144FlushFrameCount);
        g260ExecuteAll(kG260Boundary, g150_pipeline_enabled());
    }
    // Sequential (no pool) — safe to call from the mgEndFrame override to drain the trailing tail.
    if (g_g144FlushSeqFn && !g_g144List.empty())
    {
        G146PerfScope g146FlushScope(g_g146G144FlushFrameNs, g_g146G144FlushFrameCount);
        g_g144FlushSeqFn();
    }
    // Conservative phase-1 ownership boundary: frame presentation still sees color readback as
    // before, and guest VRAM leaves mgEndFrame with real Z materialized.  Once route canaries prove
    // every downstream reader has an explicit barrier this can be profiled as a removable fence.
    if (g_g144LastVram)
    {
        // Normal downstream presentation and dumps read guest VRAM. This fence is the display
        // color ownership boundary: at most one readback per dirty display target per frame.
        (void)g278FlushPendingDepth(5); // never cross presentation/frame ownership
        // G289's measured 0x14a work owner is not itself a display surface. Let it cross this
        // boundary only in the exact post-upload state; copyFrameToHostRgbaUnlocked audits every
        // actual display/preferred/fallback source before reading. Raw-VRAM diagnostics retain the
        // old all-authoritative boundary.
        const bool g289FrameCarry =
            s_g289Owner.active && s_g289Owner.dstFbp == 0x14au &&
            s_g289Owner.firstOwnedBlock >= 0x2b20u &&
            s_g289Owner.firstOwnedBlock <= s_g289Owner.lastOwnedBlock &&
            s_g289Owner.lastOwnedBlock == 0x363fu &&
            !g289RawVramDiagOn();
        if (g289FrameCarry)
            g_g289FrameCarries.fetch_add(1u, std::memory_order_relaxed);
        else
            (void)g289MaterializeOwner(5);
        g272MaterializeAll(5);
        g277CensusHardEdge(5);
        g276FlushPendingDisplay(5); // G276: publish the frame's coalesced display readback here
        g242PrepareVramReadAll(g_g144LastVram);
    }
}

// G178 census (DEFAULT OFF, DC2_G178_CENSUS=1): distinct GS render-state keys in the deferred
// list, scanned at EVERY drain point (frame-end AND mid-frame upload flushes — whichever path
// actually drains the list on the live config), printed every ~5s. MEASURE-first (the G161
// lesson): decides exactly which blend equations / texture PSMs / test modes the G178 GPU
// backend's shader+GL-state mapping must implement for the live workload. Diagnostic only.
// Called only from the threads that own g_g144List at a drain point, so no locking needed.
static void g178CensusScan()
{
    static const bool s_g178Census = (std::getenv("DC2_G178_CENSUS") != nullptr);
    if (s_g178Census && !g_g144List.empty())
    {
        static std::map<std::string, uint64_t> s_combo;
        static std::mutex s_comboMx; // frame-end (EE) and upload flush (worker) can interleave
        {
            std::lock_guard<std::mutex> lk(s_comboMx);
            for (const auto &e : g_g144List)
            {
                const uint64_t al = e.ctx.alpha, ts = e.ctx.test, zb = e.ctx.zbuf,
                               t1 = e.ctx.tex1, cl = e.ctx.clamp;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    "prim=%d iip=%u tme=%u fge=%u abe=%u fst=%u pabe=%u | fbp=0x%x fpsm=%u fbmsk=0x%x | "
                    "tex psm=%u cpsm=%u tfx=%u tcc=%u tw=%u th=%u | alpha A=%llu B=%llu C=%llu D=%llu FIX=%llu | "
                    "test ATE=%llu ATST=%llu AREF=%llu AFAIL=%llu DATE=%llu ZTE=%llu ZTST=%llu | zpsm=%llu zmsk=%llu | "
                    "wms=%llu wmt=%llu mmag=%llu mmin=%llu fba=%llu",
                    (int)e.prim.type, e.prim.iip ? 1u : 0u, e.prim.tme ? 1u : 0u, e.prim.fge ? 1u : 0u,
                    e.prim.abe ? 1u : 0u, e.prim.fst ? 1u : 0u, e.pabe ? 1u : 0u,
                    e.ctx.frame.fbp, (unsigned)e.ctx.frame.psm, e.ctx.frame.fbmsk,
                    (unsigned)e.ctx.tex0.psm, (unsigned)e.ctx.tex0.cpsm, (unsigned)e.ctx.tex0.tfx,
                    (unsigned)e.ctx.tex0.tcc, (unsigned)e.ctx.tex0.tw, (unsigned)e.ctx.tex0.th,
                    (unsigned long long)(al & 3u), (unsigned long long)((al >> 2) & 3u),
                    (unsigned long long)((al >> 4) & 3u), (unsigned long long)((al >> 6) & 3u),
                    (unsigned long long)((al >> 32) & 0xffu),
                    (unsigned long long)(ts & 1u), (unsigned long long)((ts >> 1) & 7u),
                    (unsigned long long)((ts >> 4) & 0xffu),
                    (unsigned long long)((ts >> 12) & 3u), (unsigned long long)((ts >> 14) & 1u),
                    (unsigned long long)((ts >> 16) & 1u), (unsigned long long)((ts >> 17) & 3u),
                    (unsigned long long)((zb >> 24) & 0xfu), (unsigned long long)((zb >> 32) & 1u),
                    (unsigned long long)(cl & 3u), (unsigned long long)((cl >> 2) & 3u),
                    (unsigned long long)((t1 >> 5) & 1u), (unsigned long long)((t1 >> 6) & 7u),
                    (unsigned long long)(e.ctx.fba & 1u));
                s_combo[buf]++;
            }
            static auto s_lastPrint = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastPrint > std::chrono::seconds(5))
            {
                s_lastPrint = now;
                std::fprintf(stderr, "[G178:census] distinct=%zu\n", s_combo.size());
                for (const auto &kv : s_combo)
                    std::fprintf(stderr, "[G178:census] %8llu | %s\n",
                                 (unsigned long long)kv.second, kv.first.c_str());
            }
        }
    }
}

// PARALLEL flush for the texture-upload path (called during active guest rendering, not from the
// mgEndFrame context that races the present thread). Keeps the batch parallelism instead of forcing
// a slow single-threaded drain on every mid-frame texture upload. DC2_G144_SEQUPLOAD forces the
// sequential path (A/B / fallback if the parallel upload flush proves unsafe).
// G260: external-linkage bridge for ps2_gs_gpu.cpp — the gate itself (g260NrOn) lives inside
// this file's anonymous namespace, and the transfer hooks in ps2_gs_gpu.cpp need the answer to
// pick the range-tested barrier path.
bool g260NrEnabled()
{
    return g260NrOn();
}

void g144FlushPendingUpload()
{
    // G260: callers of this no-range entry point (local→host reads, any path that did not
    // range-test) get the conservative treatment — execute the entire pending graph.
    if (g260NrOn())
    {
        if (g260HasPending())
        {
            g178CensusScan();
            G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
            static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
            g260ExecuteAll(kG260LocalHost, !s_seqUpload260);
        }
        // G261: an un-range-tested VRAM read/write follows — every GPU-resident RTT row must be
        // in guest VRAM first (conservative, like the execute above).
        (void)g278FlushPendingDepth(6);
        (void)g289MaterializeOwner(6);
        g272MaterializeAll(6);
        g277CensusHardEdge(6);
        g276FlushPendingDisplay(6); // G276: publish deferred display readbacks first
        g261MaterializeAllDirty(kG261MatL2H);
        return;
    }
    if (g_g144List.empty())
        return;
    g178CensusScan();
    G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
    static const bool s_seqUpload = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
    if (s_seqUpload)
    {
        if (g_g144FlushSeqFn)
            g_g144FlushSeqFn();
    }
    else if (g_g144FlushFn)
    {
        g_g144FlushFn();
    }
}

struct G314EdgeScope
{
    bool active = false;
    G314UploadShape *shape = nullptr;
    G144BlockRange dst{};
    std::chrono::steady_clock::time_point t0{};
    uint64_t gpuOk = 0u, gpuFail = 0u, cpuEntries = 0u;
    uint64_t gpuNs = 0u, replayNs = 0u;
    uint64_t submitNs = 0u, submits = 0u, skipRb = 0u;
    uint64_t publications = 0u;
};

static uint64_t g314PublicationCount()
{
    uint64_t total = g_g276Flushes.load(std::memory_order_relaxed) +
                     g_g278Flushes.load(std::memory_order_relaxed) +
                     g_g289Materializes.load(std::memory_order_relaxed);
    for (const auto &v : g_g261Mat)
        total += v.load(std::memory_order_relaxed);
    return total;
}

static uint64_t g314SubmitCount()
{
    return g_g274SubN.load(std::memory_order_relaxed);
}

static uint64_t g314SkipReadbackCount()
{
    return g_g276SubN[1][0].load(std::memory_order_relaxed) +
           g_g276SubN[1][1].load(std::memory_order_relaxed);
}

static G314EdgeScope g314BeginEdge(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                   uint32_t dsax, uint32_t dsay,
                                   uint32_t rrw, uint32_t rrh,
                                   const G144BlockRange &dst)
{
    G314EdgeScope out;
    if (!g314CensusOn())
        return out;

    out.active = true;
    out.shape = g314FindShape(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    out.dst = dst;
    ++out.shape->edges;
    ++g_g314Edges;

    const auto countBatch = [&](const G260RangeSet &wr, const G260RangeSet &rd,
                                size_t entries) {
        ++out.shape->batches;
        out.shape->entries += entries;
        if (wr.unknown || rd.unknown)
            ++out.shape->conflictUnknown;
        if (wr.overlaps(dst))
            ++out.shape->conflictWr;
        if (rd.overlaps(dst))
            ++out.shape->conflictRd;
    };
    for (const G260Batch &b : g_g260Graph)
        if (!b.entries.empty())
            countBatch(b.wr, b.rd, b.entries.size());
    if (!g_g144List.empty())
        countBatch(g_g260OpenWr, g_g260OpenRd, g_g144List.size());

    out.gpuOk = g_g290GpuOk;
    out.gpuFail = g_g290GpuFail;
    out.cpuEntries = g_g290CpuEntries;
    out.gpuNs = g_g290GpuOkNs;
    out.replayNs = g_g290ReplayNs;
    out.submitNs = g_g274SubNs.load(std::memory_order_relaxed);
    out.submits = g314SubmitCount();
    out.skipRb = g314SkipReadbackCount();
    out.publications = g314PublicationCount();
    out.t0 = std::chrono::steady_clock::now();
    g_g314EdgeActive = true;
    return out;
}

static void g314Report()
{
    if (!g314CensusOn() || g_g314Edges == 0u || (g_g314Edges % 512u) != 0u)
        return;
    uint64_t eligible = 0u, resident = 0u, drainNs = 0u, submitNs = 0u,
             eligibleDrainNs = 0u, eligibleSubmitNs = 0u, pubs = 0u;
    for (const G314UploadShape &s : g_g314Shapes)
    {
        if (!s.used || s.edges == 0u)
            continue;
        eligible += s.transportEligible;
        resident += s.residentEligible;
        drainNs += s.drainNs;
        submitNs += s.submitNs;
        eligibleDrainNs += s.eligibleDrainNs;
        eligibleSubmitNs += s.eligibleSubmitNs;
        pubs += s.publicationEvents;
    }
    std::fprintf(stderr,
                 "[G314:sum] edges=%llu transport=%llu resident=%llu drain=%.3f "
                 "submit=%.3f eligibleDrain=%.3f eligibleSubmit=%.3f pubs=%llu ms\n",
                 static_cast<unsigned long long>(g_g314Edges),
                 static_cast<unsigned long long>(eligible),
                 static_cast<unsigned long long>(resident), drainNs / 1.0e6,
                 submitNs / 1.0e6, eligibleDrainNs / 1.0e6,
                 eligibleSubmitNs / 1.0e6,
                 static_cast<unsigned long long>(pubs));
    for (const G314UploadShape &s : g_g314Shapes)
    {
        if (!s.used || s.edges == 0u)
            continue;
        std::fprintf(stderr,
                     "[G314:shape] dbp=%x/%u/%x xy=%u,%u wh=%ux%u edges=%llu "
                     "conf(wr=%llu rd=%llu unk=%llu) graph=%llub/%llue "
                     "gpu=%llu/%llu cpuE=%llu skipRb=%llu/%llu mirror=%llu/%llu "
                     "eligible=%llu/%llu drain=%.3f gpuMs=%.3f replay=%.3f "
                     "submit=%.3f pubs=%llu next=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     s.dbp, s.dbw, s.dpsm, s.dsax, s.dsay, s.rrw, s.rrh,
                     static_cast<unsigned long long>(s.edges),
                     static_cast<unsigned long long>(s.conflictWr),
                     static_cast<unsigned long long>(s.conflictRd),
                     static_cast<unsigned long long>(s.conflictUnknown),
                     static_cast<unsigned long long>(s.batches),
                     static_cast<unsigned long long>(s.entries),
                     static_cast<unsigned long long>(s.gpuOk),
                     static_cast<unsigned long long>(s.gpuFail),
                     static_cast<unsigned long long>(s.cpuEntries),
                     static_cast<unsigned long long>(s.skipRb),
                     static_cast<unsigned long long>(s.submits),
                     static_cast<unsigned long long>(s.mirrorEdges),
                     static_cast<unsigned long long>(s.mirrorTargets),
                     static_cast<unsigned long long>(s.transportEligible),
                     static_cast<unsigned long long>(s.residentEligible),
                     s.drainNs / 1.0e6, s.gpuNs / 1.0e6, s.replayNs / 1.0e6,
                     s.submitNs / 1.0e6,
                     static_cast<unsigned long long>(s.publicationEvents),
                     static_cast<unsigned long long>(s.next[0]),
                     static_cast<unsigned long long>(s.next[1]),
                     static_cast<unsigned long long>(s.next[2]),
                     static_cast<unsigned long long>(s.next[3]),
                     static_cast<unsigned long long>(s.next[4]),
                     static_cast<unsigned long long>(s.next[5]),
                     static_cast<unsigned long long>(s.next[6]));
    }
}

static void g314FinishEdge(G314EdgeScope &edge, uint32_t mirrorMask,
                           uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                           uint32_t dsax, uint32_t dsay,
                           uint32_t rrw, uint32_t rrh)
{
    if (!edge.active || edge.shape == nullptr)
        return;
    g_g314EdgeActive = false;
    G314UploadShape &s = *edge.shape;
    const uint64_t gpuOk = g_g290GpuOk - edge.gpuOk;
    const uint64_t gpuFail = g_g290GpuFail - edge.gpuFail;
    const uint64_t cpuEntries = g_g290CpuEntries - edge.cpuEntries;
    const uint64_t gpuNs = g_g290GpuOkNs - edge.gpuNs;
    const uint64_t replayNs = g_g290ReplayNs - edge.replayNs;
    const uint64_t submitNs = g_g274SubNs.load(std::memory_order_relaxed) - edge.submitNs;
    const uint64_t submits = g314SubmitCount() - edge.submits;
    const uint64_t skipRb = g314SkipReadbackCount() - edge.skipRb;
    const uint64_t publications = g314PublicationCount() - edge.publications;
    const uint64_t drainNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - edge.t0).count());

    s.gpuOk += gpuOk;
    s.gpuFail += gpuFail;
    s.cpuEntries += cpuEntries;
    s.gpuNs += gpuNs;
    s.replayNs += replayNs;
    s.submitNs += submitNs;
    s.submits += submits;
    s.skipRb += skipRb;
    s.publicationEvents += publications;
    s.drainNs += drainNs;
    if (mirrorMask != 0u)
    {
        ++s.mirrorEdges;
        for (uint32_t m = mirrorMask; m != 0u; m >>= 1u)
            s.mirrorTargets += m & 1u;
    }

    // Transport eligibility is intentionally broader than existing G264 mirror admission: it is
    // the absolute ceiling for a NEW ordered backend payload node.  Resident eligibility is the
    // subset already representable by today's exact FBO mirror ownership.
    const bool transport = gpuOk != 0u && gpuFail == 0u && cpuEntries == 0u &&
                           submits == gpuOk && skipRb == submits && publications == 0u;
    if (transport)
    {
        ++s.transportEligible;
        s.eligibleDrainNs += drainNs;
        s.eligibleSubmitNs += submitNs;
        if (mirrorMask != 0u)
            ++s.residentEligible;
    }

    if (g_g314Pending.armed)
    {
        if (edge.dst.valid && g_g314Pending.range.valid &&
            g144RangeOverlaps(edge.dst, g_g314Pending.range))
            g314ResolveNext(kG314NextUploadWrite);
        else
            g314ResolveNext(kG314NextUnresolved);
    }
    g_g314Pending.waitingForComplete = true;
    g_g314Pending.armed = false;
    g_g314Pending.shape = &s;
    g_g314Pending.range = edge.dst;
    g_g314Pending.dbp = dbp;
    g_g314Pending.dbw = dbw;
    g_g314Pending.dpsm = dpsm;
    g_g314Pending.dsax = dsax;
    g_g314Pending.dsay = dsay;
    g_g314Pending.rrw = rrw;
    g_g314Pending.rrh = rrh;
    g314Report();
}

// G145 (experimental, opt-in DC2_G145_DIRTY_UPLOAD=1): dirty-region upload flush gate. G144 flushed
// before every upload/local-copy because any VRAM write can invalidate deferred texture sampling.
// This keeps that behavior on uncertainty, but skips flushes whose destination does not overlap any
// pending texture/CLUT/target range. For local-to-local copies, a source overlap with the pending
// render target also flushes so the copy reads already-rasterized pixels.
void g144FlushPendingUploadRange(uint32_t dbp,
                                 uint8_t dbw,
                                 uint8_t dpsm,
                                 uint32_t dsax,
                                 uint32_t dsay,
                                 uint32_t rrw,
                                 uint32_t rrh)
{
    // G260: a host→local upload is a WAR/WAW edge only when its destination intersects a
    // pending batch's read set (texture/CLUT the recorded draws will sample at execution) or
    // write set (target/Z alias). Real edges execute the whole graph in order; independent
    // uploads record through with no synchronization. Unresolvable ranges fail closed.
    if (g260NrOn())
    {
        const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
        const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
        const bool g289UploadOverwrite =
            !dstUnknown &&
            g289PrepareUploadOverwrite(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, dst);
        if (g289StatOn() && s_g289Owner.active && s_g289Owner.dstFbp == 0x14au)
        {
            const G144BlockRange owner = g289OwnerRange();
            const bool overlap =
                dstUnknown || !owner.valid || (dst.valid && g144RangeOverlaps(owner, dst));
            if (overlap)
            {
                static std::atomic<uint32_t> s_g289UploadEdgeLogs{0};
                const uint32_t n =
                    s_g289UploadEdgeLogs.fetch_add(1u, std::memory_order_relaxed);
                if (n < 32u)
                    std::fprintf(
                        stderr,
                        "[G289:upload-edge] n=%u owner(dst=%03x range=%x..%x valid=%u) "
                        "upload(dbp=%x dbw=%u psm=%x xy=%u,%u size=%ux%u "
                        "range=%x..%x valid=%u unknown=%u)\n",
                        n + 1u, s_g289Owner.dstFbp, owner.first, owner.last,
                        owner.valid ? 1u : 0u, dbp, static_cast<unsigned>(dbw), dpsm,
                        dsax, dsay, rrw, rrh, dst.first, dst.last,
                        dst.valid ? 1u : 0u, dstUnknown ? 1u : 0u);
            }
        }
        if (!g289UploadOverwrite)
            g289MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, 7);
        G314EdgeScope g314Edge;
        if (g260HasPending())
        {
            const bool needsExecute =
                g260VramEventNeedsExecute(G144BlockRange{}, dst, false, dstUnknown);
            if (needsExecute)
            {
                g314Edge = g314BeginEdge(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, dst);
                g178CensusScan();
                G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
                static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
                if (g271PrefixBarrierOn())
                    g271ExecuteConflictPrefix(G144BlockRange{}, dst, false, dstUnknown,
                                              kG260Upload, !s_seqUpload260);
                else
                    g260ExecuteAll(kG260Upload, !s_seqUpload260);
            }
            else
            {
                ++g_g260UploadSkips;
            }
        }
        // G261: the caller writes `dst` into guest VRAM next — if it lands on (or the range is
        // unresolvable near) a GPU-resident RTT row, materialize first so the upload composes
        // over current bytes and the residency record cannot go stale-partial.
        // G264: eligible rects are deferred to a VRAM→FBO mirror at the next flush edge instead
        // (the mask suppresses only the exact targets that accepted the deferral; fail-closed).
        uint32_t g327OverlapMask = 0u;
        if (g327CensusOn() && !dstUnknown)
            for (uint32_t i = 0u; i < kG248TargetCount; ++i)
                if (g_g261Res[i].dirty && g_g261Res[i].range.valid &&
                    g144RangeOverlaps(g_g261Res[i].range, dst))
                    g327OverlapMask |= 1u << i;
        std::chrono::steady_clock::time_point g327T0;
        if (g327CensusOn())
            g327T0 = std::chrono::steady_clock::now();
        const uint32_t g264Mask = dstUnknown
            ? 0u
            : g264NoteUploadForMirror(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
        const uint64_t g327NoteNs = g327CensusOn()
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - g327T0).count())
            : 0u;
        (void)g278FlushPendingDepthForRanges(
            7, G144BlockRange{}, dst, false, dstUnknown);
        if (g327CensusOn())
            g327T0 = std::chrono::steady_clock::now();
        g261MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, kG261MatUpload,
                                 g264Mask);
        const uint64_t g327MatNs = g327CensusOn()
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - g327T0).count())
            : 0u;
        g327RecordUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                         g327OverlapMask, g264Mask, g327NoteNs, g327MatNs);
        (void)g278FlushPendingDepthForDisplayOwners(7);
        g272MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, 7);
        g277CensusRangeEdge(7, G144BlockRange{}, dst, false, dstUnknown);
        // G276/G284: publish deferred display readbacks before this l2h upload edge — unless G284
        // proves the upload dst is disjoint from the pending display rows (then keep it coalesced).
        g276FlushPendingDisplayForRanges(7, G144BlockRange{}, dst, false, dstUnknown);
        g314FinishEdge(g314Edge, g264Mask, dbp, dbw, dpsm,
                       dsax, dsay, rrw, rrh);
        return;
    }
    if (g_g144List.empty())
        return;

    static const bool s_noDirty = (std::getenv("DC2_G145_NO_DIRTY_UPLOAD") != nullptr);
    const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!s_noDirty && !g144PendingNeedsFlushForRanges({}, dst))
    {
        g144DirtyStat("upload", false);
        return;
    }

    g144DirtyStat("upload", true);
    g144FlushPendingUpload();
}

void g144FlushPendingLocalTransferRange(uint32_t sbp,
                                        uint8_t sbw,
                                        uint8_t spsm,
                                        uint32_t ssax,
                                        uint32_t ssay,
                                        uint32_t dbp,
                                        uint8_t dbw,
                                        uint8_t dpsm,
                                        uint32_t dsax,
                                        uint32_t dsay,
                                        uint32_t rrw,
                                        uint32_t rrh)
{
    // G260: a local→local copy READS its source (must observe pending target writes → RAW) and
    // WRITES its destination (WAR/WAW against pending reads and writes). Same fail-closed range
    // logic as the upload edge.
    if (g260NrOn())
    {
        const G144BlockRange src = g144RangeForRect(sbp, spsm, sbw, ssax, ssay, rrw, rrh);
        const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
        const bool srcUnknown = (rrw != 0u && rrh != 0u && !src.valid);
        const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
        g314ObserveTransfer(src, dst, srcUnknown, dstUnknown);
        g289MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        if (g260HasPending())
        {
            if (g260VramEventNeedsExecute(src, dst, srcUnknown, dstUnknown))
            {
                g178CensusScan();
                G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
                static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
                g260ExecuteAll(kG260LocalCopy, !s_seqUpload260);
            }
            else
            {
                ++g_g260UploadSkips;
            }
        }
        // G261: the local→local copy READS src (must observe wave-rendered pixels) and WRITES
        // dst — both must see real VRAM bytes for any GPU-resident RTT row they touch.
        g266NoteLocalCopy(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                          src, dst, srcUnknown, dstUnknown);
        (void)g278FlushPendingDepthForRanges(
            8, src, dst, srcUnknown, dstUnknown);
        const uint32_t g285Consumed = g285PrepareL2lConsume(
            sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
            src, dst, srcUnknown, dstUnknown);
        g261MaterializeForRanges(src, dst, srcUnknown, dstUnknown, kG261MatLocal,
                                 g285Consumed);
        (void)g278FlushPendingDepthForDisplayOwners(8);
        g272MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        g277CensusRangeEdge(8, src, dst, srcUnknown, dstUnknown);
        // G276/G284: publish deferred display readbacks before this l2l edge — unless G284 proves the
        // copy src+dst are both disjoint from the pending display rows (then keep it coalesced).
        if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                                    dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
            g276FlushPendingDisplayForRanges(8, src, dst, srcUnknown, dstUnknown);
        return;
    }
    if (g_g144List.empty())
        return;

    static const bool s_noDirty = (std::getenv("DC2_G145_NO_DIRTY_UPLOAD") != nullptr);
    const G144BlockRange src = g144RangeForRect(sbp, spsm, sbw, ssax, ssay, rrw, rrh);
    const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!s_noDirty && !g144PendingNeedsFlushForRanges(src, dst))
    {
        g144DirtyStat("local", false);
        return;
    }

    g144DirtyStat("local", true);
    g144FlushPendingUpload();
}
