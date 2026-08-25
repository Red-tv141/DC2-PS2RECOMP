void GSRasterizer::drawSprite(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();
    // G408 diagnostic: isolate the four offset CT32 character-RTT sprites which
    // turn sparse source alpha into the visible Map 125 dark speckles.
    static const bool s_g408SkipCharacterRttSprites =
        std::getenv("DC2_G408_SKIP_CHARACTER_RTT_SPRITES") != nullptr;
    if (s_g408SkipCharacterRttSprites &&
        (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
        gs->m_prim.tme && gs->m_prim.fst && gs->m_prim.abe &&
        ctx.tex0.tbp0 == 0x2720u && ctx.tex0.psm == GS_PSM_CT32 &&
        ctx.tex1 == 0x261u && (ctx.alpha & 0xFFu) == 0x44u)
        return;
    g400NoteDrawTarget(gs->m_vram, ctx.frame.fbp);
    g401NoteCompositeDraw(gs->m_vram, ctx, gs->m_prim, v0, v1);
    G248SpriteScope g248Scope(ctx.frame.fbp, gs->m_prim.tme, gs->m_prim.abe);
    // G529: price a REPLAYED sprite by the path it takes. Inert unless DC2_G529_STAT=1 AND this
    // call is inside a band replay — see [G529:spr] in g529_replay_dispatch.inc for why.
    G529SprScope g529spr;
    // G530: the SAME bracket as G529SprScope, but split at the pixel loop and paired with the
    // lane's retired-cycle count — the two things `[G529:spr] ns/px` silently assumed. Inert unless
    // DC2_G530_CYC=1 AND this call is inside a band replay.
    G530CycScope g530cyc;
    // G605: the same bracket again, but bucketed by BATCH CLASS (disp/legacy/discov/other) so the
    // sprite side can be ranked against the triangle side in one table. Inert unless DC2_G605_CYC=1.
    G605CycScope g605cyc(ctx.frame.fbp, kG605LeafSprite);
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
    const uint32_t sZFbp = ctx.frame.fbp;
    const bool g403DisplayZ =
        sZuni && g404SharedZScope(sZFbp, sZbw,
                                  static_cast<uint32_t>(ctx.zbuf & 0x1FFu), sZpsm);
    if (g403DisplayZ)
        g403DisplayZPrepare(sZFbp);
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
    // G357 diagnosis: log Z-writing sprites at the display target — does DC2's per-frame
    // full-screen ztst=ALWAYS/zmsk=0 clear reach the menu Z buffer (zbp=0xd0) with value 0,
    // so the circle field's GEQUAL depth test passes cleanly? (see rasterizer_draw_triangle.inc)
    {
        static const bool s_g357z = (std::getenv("DC2_G357_ZTRACE") != nullptr);
        if (s_g357z && sZWrite && (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u))
        {
            static std::atomic<uint32_t> s_g357sn{0};
            const uint32_t n = s_g357sn.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 200u)
                std::fprintf(stderr,
                    "[G357:zclr] n=%u fbp=0x%x zbp=0x%x zbw=%u ztst=%u zmsk=%u tme=%u sZi=%u "
                    "rect=(%.0f,%.0f)-(%.0f,%.0f)\n",
                    n, ctx.frame.fbp, sZbp, sZbw, sZtst, static_cast<uint32_t>(sZmsk),
                    static_cast<uint32_t>(gs->m_prim.tme), sZi, v0.x, v0.y, v1.x, v1.y);
        }
    }
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
    // G530 decomposition probes — see g530_sprite_span.inc. Loop-invariant, default-off, and each
    // one produces knowingly incorrect pixels; they price the per-pixel classes before any lever.
    // ⛔⛔ G617: BOTH A/B ARMS ARE READ HERE, AT drawSprite ENTRY — NOT INSIDE THE BRANCH EACH
    // LEVER LIVES IN. G615 §6's law is "read the arm unconditionally, before any state conjunct",
    // and revision 1 obeyed the letter of it (each arm was the first statement of its own branch)
    // while breaking the intent: `g617BlendFillOn()` sat inside `else { /* untextured */ }`, which
    // IS the lever's footprint, so the arm was still sampled on the LEVER's schedule. The gate
    // reproduced G615's exact signature — `used(0=228 1=77 2=56 3=0)` with `blocked(w=2)`, i.e. 228
    // of 361 intervals saw neither arm and the estimator had 2 windows. ⭐ THE ARM MUST BE READ ON
    // A PATH EVERY MEMBER OF THE FRAME'S DRAW POPULATION TAKES, which for a sprite lever is the top
    // of drawSprite, before the `tme` split.
    const bool g617FillArm = g617BlendFillOn();
    const bool g617T4hhArmOn = g617T4hhOn();
    const bool g530NoZ    = g530ProbeNoZ();
    const bool g530NoWr   = g530ProbeNoWr();
    const bool g530NoUv   = g530ProbeNoUv();
    const bool g530NoTex  = g530ProbeNoTex();
    const bool g530NoComb = g530ProbeNoComb();
    const bool g530NoPx   = g530ProbeNoPx();
    auto spriteZPass = [&](int px, int py) -> bool {
        if (g530NoZ) return true; // ⛔ probe: deletes the guest-VRAM Z READ
        if (!sZActive) return true;
        const uint32_t zdst = g403DisplayZ
            ? g403DisplayZRead(px, py, sZFbp)
            : (sZ16 ? GSMem::ReadZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                    : (sZ24 ? GSMem::ReadZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                            : GSMem::ReadZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))));
        switch (sZtst)
        {
        case 0:  return false;          // NEVER
        case 1:  return true;           // ALWAYS
        case 2:  return sZi >= zdst;    // GEQUAL
        default: return sZi >  zdst;    // GREATER
        }
    };
    auto spriteZStore = [&](int px, int py) {
        if (g530NoZ) return; // ⛔ probe: deletes the guest-VRAM Z WRITE
        if (!sZWrite) return;
        if (g403DisplayZ) g403DisplayZWrite(px, py, sZFbp, sZi);
        else if (sZ16) GSMem::WriteZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else if (sZ24) GSMem::WriteZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else           GSMem::WriteZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
    };

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    // G365 coverage rule — DEFAULT-ON again since G368 (kill: DC2_G365_NO_SPRITE_EDGE=1).
    //
    // G365 established that a GS sprite covers pixel P iff X0 <= P < X1 in the SUB-PIXEL domain
    // (covered range [ceil(x0), ceil(x1)-1]) rather than the truncate-then-span rule below, because
    // DC2's half-res downscale sprite (display 0xd00 -> RTT 0x139) is emitted at
    // (-0.5,-0.5)-(255.5,207.5) and truncation writes only 0..254 x 0..206, leaving the last
    // column/row of the 256x208 image stale (the source of the right-edge colour line).
    //
    // G366/G367 retired this rule to default-off because turning it on made MAP-160 show the
    // character sliced into 32-row bands. G368 root-caused that tear and it is NOT this rule:
    // block-matching a torn frame against a clean one showed every 32-row band displaced by
    // exactly +128 px, i.e. a 512-px buffer re-read at a 640-px page stride. The culprit is the
    // presentation all-black fallback in GS::latchHostPresentationFrame, which substituted a
    // DRAWING context's FRAME register (DC2 parks context 1 at fbp=0x0 fbw=10) and read the
    // display buffer at that width. With that fixed the full ceil rule runs the MAP-160 dense
    // scan at tornFrames=0/2218 AND removes the right-edge colour line. See
    // plans/phase-G368-fix-log.md.
    static const bool s_g365Edge = !envFlagEnabled("DC2_G365_NO_SPRITE_EDGE");
    // G367 bisect selector (diagnostic): which sprite CLASS the ceil rule is applied to.
    //   bit0 = the display-source / downscale sprite (tex tbp 0xd00) — the one the line needs
    //   bit1 = the RTT blur ping-pong quadrant tiles (tbp 0x2860 / 0x28a0 / 0x28e0)
    //   bit2 = everything else
    // Unset = 7 (all), i.e. the plain G365 behaviour.
    static const int s_g367Sel = []() -> int {
        const char *e = std::getenv("DC2_G367_SEL");
        return e ? std::atoi(e) : 7;
    }();
    const uint32_t g367Tbp = ctx.tex0.tbp0;
    const bool g367IsBlur = (g367Tbp == 0x2860u || g367Tbp == 0x28a0u || g367Tbp == 0x28e0u);
    const bool g367IsDisp = (g367Tbp == 0x0d00u);
    const int g367Bit = g367IsDisp ? 1 : (g367IsBlur ? 2 : 4);
    int x0, y0, x1, y1;
    if (s_g365Edge && (s_g367Sel & g367Bit))
    {
        float fx0 = v0.x - static_cast<float>(ofx);
        float fy0 = v0.y - static_cast<float>(ofy);
        float fx1 = v1.x - static_cast<float>(ofx);
        float fy1 = v1.y - static_cast<float>(ofy);
        if (fx0 > fx1)
            std::swap(fx0, fx1);
        if (fy0 > fy1)
            std::swap(fy0, fy1);
        x0 = static_cast<int>(std::ceil(fx0));
        y0 = static_cast<int>(std::ceil(fy0));
        // Exclusive edges; the span below turns them back into an inclusive last pixel.
        x1 = static_cast<int>(std::ceil(fx1));
        y1 = static_cast<int>(std::ceil(fy1));
        // G367: under the ceil rule a sprite whose span collapses to zero covers NO pixel on
        // hardware, so it must draw nothing. The `max(1, span)` expansion below is a non-hardware
        // hack that only made sense under the truncate convention; keeping it while switching to
        // ceil is what pushed the RTT blur chain's sub-pixel quadrant tiles one pixel outside
        // their 128x128 region and produced G366's page-row tearing.
        if (x1 <= x0 || y1 <= y0)
            return;
    }
    else
    {
        x0 = static_cast<int>(v0.x) - ofx;
        y0 = static_cast<int>(v0.y) - ofy;
        x1 = static_cast<int>(v1.x) - ofx;
        y1 = static_cast<int>(v1.y) - ofy;

        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
    }

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
    // G591 slice A: the G411 private depth mirror's WRITE EPOCH. This is the per-draw predicate
    // every g403DisplayZWrite in this file already sits behind, hoisted to draw scope — not an
    // enumeration of the write sites, so a future writer inside this draw is covered for free.
    // The band is the post-scissor bbox, which is a superset of the rows spriteZStore can reach.
    if (g403DisplayZ && sZWrite)
        g591MirrorDrawWrite(drawY0, drawY1);
    // G592 INVARIANT PROBE (inert unless tracked): a CPU draw must never reach the private mirror
    // while a private-mirror owner is still pending. Deliberately NOT `&& sZWrite` — a depth TEST
    // reads the mirror too, and the consumer test this guards admits reads as consumers.
    g592NoteDisplayZDraw(g403DisplayZ);
    if (drawX1 >= drawX0 && drawY1 >= drawY0)
    {
        g248Scope.setPixels(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                            static_cast<uint64_t>(drawY1 - drawY0 + 1));
        g529spr.setPx(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                      static_cast<uint64_t>(drawY1 - drawY0 + 1));
        g530cyc.setPx(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                      static_cast<uint64_t>(drawY1 - drawY0 + 1));
        g605cyc.setPx(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                          static_cast<uint64_t>(drawY1 - drawY0 + 1),
                      static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                          static_cast<uint64_t>(drawY1 - drawY0 + 1));
        g605cyc.setInside(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                          static_cast<uint64_t>(drawY1 - drawY0 + 1));
        g605cyc.setShape(ctx.frame.fbp, static_cast<uint8_t>(gs->m_prim.type),
                         static_cast<uint8_t>(ctx.frame.psm), ctx.frame.fbmsk, ctx.tex0.tbp0,
                         static_cast<uint8_t>(ctx.tex0.psm), static_cast<uint8_t>(ctx.tex0.tfx),
                         static_cast<uint8_t>(ctx.tex0.tcc), tex1UsesLinearFilter(ctx.tex1),
                         ctx.tex1, ctx.alpha, ctx.test,
                         static_cast<uint8_t>((ctx.zbuf >> 24) & 0xFu),
                         static_cast<uint8_t>((ctx.test >> 17) & 0x3u),
                         gs->m_prim.iip != 0u, gs->m_prim.fst != 0u, gs->m_prim.abe != 0u,
                         gs->m_prim.fge != 0u, gs->m_pabe != 0u, (ctx.fba & 0x1ull) != 0ull,
                         g403DisplayZ, gs->m_prim.tme != 0u);
    }

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

        // [G360] per-sprite vertex-UV probe for the item-grid gray panel (tbp=0x2b20). The
        // per-pixel probe in sampleTexture showed every gray-cell pixel samples texel (0,0);
        // this prints the INCOMING vertex UV/STQ so we can tell whether the guest emitted a
        // degenerate UV (upstream) or the GS derived sampleU=0 from a valid UV (GS bug).
        {
            static const bool s_g360s = (std::getenv("DC2_G360_GRAYBG") != nullptr);
            if (s_g360s && tex.tbp0 == 0x2b20u)
            {
                static std::atomic<uint32_t> s_g360sN{0};
                const uint32_t gsn = s_g360sN.fetch_add(1u, std::memory_order_relaxed);
                if (gsn < 24u)
                    std::fprintf(stderr,
                        "[G360:spr] n=%u fst=%u scr=(%d,%d)-(%d,%d) uv0=(%d,%d) uv1=(%d,%d) "
                        "st0=(%.3f,%.3f,q%.3f) st1=(%.3f,%.3f,q%.3f) tbw=%u cbp=0x%x tw=%d th=%d\n",
                        gsn, static_cast<uint32_t>(gs->m_prim.fst), x0, y0, x1, y1,
                        static_cast<int>(v0.u), static_cast<int>(v0.v),
                        static_cast<int>(v1.u), static_cast<int>(v1.v),
                        v0.s, v0.t, v0.q, v1.s, v1.t, v1.q,
                        static_cast<uint32_t>(tex.tbw), tex.cbp, texW, texH);
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

        const G406UvRoundInfo g406Round =
            g406SpriteUvRoundInfo(v0, v1, ofx, ofy, !tex1UsesLinearFilter(ctx.tex1),
                                  gs->m_prim.tme != 0u, gs->m_prim.fst != 0u);
        float u0f, v0f, u1f, v1f;
        if (gs->m_prim.fst)
        {
            u0f = g406Round.active ? static_cast<float>(v0.u) / 16.0f
                                   : static_cast<float>(v0.u >> 4);
            v0f = g406Round.active ? static_cast<float>(v0.v) / 16.0f
                                   : static_cast<float>(v0.v >> 4);
            u1f = g406Round.active ? static_cast<float>(v1.u) / 16.0f
                                   : static_cast<float>(v1.u >> 4);
            v1f = g406Round.active ? static_cast<float>(v1.v) / 16.0f
                                   : static_cast<float>(v1.v >> 4);
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
        const bool g407SubpixelPhase =
            g406Round.active && g407SpriteSubpixelPhaseEnabled();
        const float g407RawX0 = v0.x - static_cast<float>(ofx);
        const float g407RawY0 = v0.y - static_cast<float>(ofy);
        const float g407RawSpanX = v1.x - v0.x;
        const float g407RawSpanY = v1.y - v0.y;
        if (g407SubpixelPhase && std::fabs(g407RawSpanX) >= (1.0f / 16.0f))
            spriteW = g407RawSpanX;
        if (g407SubpixelPhase && std::fabs(g407RawSpanY) >= (1.0f / 16.0f))
            spriteH = g407RawSpanY;
        if (g407SubpixelPhase)
        {
            if (std::fabs(spriteW) < 1.0f)
                spriteW = spriteW < 0.0f ? -1.0f : 1.0f;
            if (std::fabs(spriteH) < 1.0f)
                spriteH = spriteH < 0.0f ? -1.0f : 1.0f;
        }
        else
        {
            if (spriteW < 1.0f)
                spriteW = 1.0f;
            if (spriteH < 1.0f)
                spriteH = 1.0f;
        }

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
        // G526: admission by property, not by address — see g526FastSpriteTarget. Identical to the
        // pre-G526 predicate unless DC2_G526_ANYTGT=1, and the sampler never reads frame.fbp.
        const bool g248FastSampleSprite =
            s_g248FastSprite && g526FastSpriteTarget(ctx.frame.fbp) && gs->m_prim.fst &&
            g248FastPsm && tex.tbw != 0u && g248TraceOff && g248SpecialFilterOff;
        // G529: book this replayed sprite to fast/slow and name the FIRST failing conjunct, in the
        // same order the predicate evaluates them. Inert unless DC2_G529_STAT=1 and in a replay.
        g529spr.classify(g248FastSampleSprite,
                         !s_g248FastSprite            ? kG529SprROff
                         : !g526FastSpriteTarget(ctx.frame.fbp) ? kG529SprRTarget
                         : !gs->m_prim.fst            ? kG529SprRFst
                         : !g248FastPsm               ? kG529SprRPsm
                         : tex.tbw == 0u              ? kG529SprRTbw
                         : !g248TraceOff              ? kG529SprRTrace
                         : !g248SpecialFilterOff      ? kG529SprRFilter
                                                      : kG529SprROk,
                         tex1UsesLinearFilter(ctx.tex1), g248Psm);
        // ⭐⭐⭐ G617 (b): THE PSMT4HH DISPLAY SPRITE, the LARGEST single item in `dungeon1`'s replay.
        // `[G615:privz] spanRej(samp=102994)` and `[G529:sprwhy] psm=102873 psm(c:102873)` agree
        // that ONE conjunct refuses this whole population — `g248FastPsm`, which admits CT32/CT24/T8
        // — and the split leaf census prices what that refusal costs: `disp spr-slow` is 37.4% of
        // the route's replay at **819.0 cyc/inside**, 5.3x the admitted `disp sprite` leaf's 155.3.
        //
        // ⛔ `g248FastPsm` is NOT widened. It gates `g248FastSample`, whose `g248SamplePoint` has
        // exactly two arms (CT32/CT24, else assume PSMT8) — widening it would send a T4HH draw that
        // the span kernel later refuses into a sampler that reads it as PSMT8. The admission is a
        // SEPARATE predicate with the same shape, and only the span kernel (which gets a real T4HH
        // tap below) consumes it. Fail-closed by construction.
        //
        // ⛔⛔ The arm (`g617T4hhArmOn`) is read at drawSprite ENTRY, before the `tme` split — see
        // the note there. State terms are AND-ed onto it here (G615 §6).
        const bool g617T4hhSprite =
            g617T4hhArmOn && s_g248FastSprite && g526FastSpriteTarget(ctx.frame.fbp) &&
            gs->m_prim.fst && g248Psm == GS_PSM_T4HH && tex.tbw != 0u && g248TraceOff &&
            g248SpecialFilterOff;
        const uint8_t g248WrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
        const uint8_t g248WrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
        const int g248MinU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
        const int g248MaxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
        const int g248MinV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
        const int g248MaxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
        const bool g248Linear = tex1UsesLinearFilter(ctx.tex1);
        // G617 (b): the T4HH tap is a CLUT lookup exactly as the T8 tap is, so it takes the same
        // per-draw 256-entry cache. ⚠️ Widening this is safe ONLY because `g248ClutLookup` is
        // reached from `g248SamplePoint` (which never runs for T4HH — `g248FastSampleSprite` is
        // false for it) and from the G617 tap below. `lookupCLUT`'s other inputs (cbp/cpsm/csm/csa
        // and the source psm, which drives `resolveClutIndex`'s T4 arm) are draw constants, so
        // caching on the raw index is the identity for this draw.
        const bool g248UseClutCache =
            ((g248FastSampleSprite && g248Psm == GS_PSM_T8) || g617T4hhSprite) &&
            !s_g248NoClutCache;
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
        // ⛔ G529 CEILING PROBE (DC2_G529_POINTONLY=1, default-off, KNOWINGLY INCORRECT OUTPUT,
        // NEVER PROMOTE). `[G529:spr]` measured the fast sampler at 397.8 ns/px with 100% of its
        // sprites BILINEAR — ~1,250 host cycles for one pixel, which no amount of arithmetic
        // explains (DC2_G529_NOLERP deleted the entire lerp and moved the frame 0.00 ms). The two
        // remaining candidates are the FOUR swizzled texel taps and the writePixel round trip, and
        // they need different phases. Forcing the point branch cuts taps 4 -> 1 while leaving the
        // write path untouched, so the paired frame delta separates them.
        static const bool s_g529PointOnly = envFlagEnabled("DC2_G529_POINTONLY");
        auto g248FastSample = [&](uint16_t u, uint16_t v) -> uint32_t {
            const float texUf = static_cast<float>(u) / 16.0f;
            const float texVf = static_cast<float>(v) / 16.0f;
            if (!g248Linear || s_g529PointOnly)
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

        // Everything above this line is the PROLOGUE the [G530:cyc] census books separately.
        g530cyc.setFast(g248FastSampleSprite);
        // ============================ G530: THE SPAN KERNEL ============================
        // Full rationale + the exactness argument: g530_sprite_span.inc. Admission is by PROPERTY,
        // never by address (G526/G527's law): the kernel runs when the draw is an axis-aligned
        // textured sprite the G248 fast sampler already admitted, the destination is 32-bit, no
        // sub-pixel/round override is active, no per-pixel diagnostic is armed, and no G529/G530
        // probe is armed. Anything else falls through to the proven generic nest below, unchanged.
        bool g530Handled = false;
        // ⭐⭐⭐ G615 (b): `!g403DisplayZ` USED TO BE A HARD REFUSAL HERE, and it excluded the entire
        // display-target sprite population — 23.7% of `dungeon1`'s replay at 701.5 cyc/inside
        // against 322.8 for the identical leaf on a target without the private mirror. It existed
        // for a real reason (the kernel's Z-read arm below reads guest VRAM Z, which is the WRONG
        // authority for a private-mirror draw), so the conjunct is replaced by the lever that
        // supplies the missing arm — never simply deleted. Fail-closed: with the lever rolled back
        // the predicate is bit-identical to the shipped one.
        // ⚠️ READ ONCE PER DRAW — each of these is a function-local `static const bool`, i.e. a
        // per-ACCESS TLS init guard (`g609_function_local_static_is_a_per_access_tls_guard`).
        //
        // ⭐⭐⭐ THE LEVER IS THAT `!g403DisplayZ` WAS OVER-BROAD, NOT THAT IT WAS WRONG. It rejected
        // every draw whose depth AUTHORITY is the private mirror — including the ones whose depth
        // OPERATION is a proven no-op. On `dungeon1` that is the whole population: `[G615:privz]`
        // measured `zRd=0 zSt=0` over 37.5 M admitted pixels, because these draws carry
        // ZTST=ALWAYS with ZMSK set, so `sZActive` is false by drawSprite's own fast-skip rule.
        // Admitting exactly the no-op subset needs NO new code in the pixel loop: the kernel's
        // `if (sZActive)` read is not taken, and its `spriteZStore` self-guards on `!sZWrite`. The
        // draw's pixels are then covered verbatim by `[G530:span] bad=0`, the kernel's own oracle.
        //
        // ⛔⛔ THE ARM IS READ FIRST, UNCONDITIONALLY, AND THE STATE TERMS ARE AND-ED ONTO IT.
        // Written as `g403DisplayZ && g615ZNoop && g615SpanOn()`, C++ short-circuits and the A/B arm
        // is only ever OBSERVED on draws inside the lever's own footprint — which on this route is a
        // localized spike region. The first `DC2_G419_AB=privz` leg read `used(0=4175 1=119 2=147
        // 3=0)` with `blocked(w=4)`: 4,175 of 4,440 intervals saw NEITHER arm and were dropped, so
        // the estimator had 4 windows instead of ~26. An A/B arm must be sampled on the same
        // schedule as the frames it is attributed to. (G614's `g614SprFastOn()` is read
        // unconditionally two hundred lines below for exactly this reason.)
        const bool g615SpanArm = g615SpanOn();
        const bool g615ZNoop = !sZActive && !sZWrite; // mirror provably neither read nor written
        const bool g615Span = g615SpanArm && g403DisplayZ && g615ZNoop;
        const bool g615Stat = g615StatOn();
        // ⭐⭐ G617: the admission is now a NAMED BOOLEAN evaluated BEFORE `markLoop()`, so the
        // census can book this draw to the leaf that is about to run it. A `setLeaf` after
        // `markLoop` would split one draw across two leaves (its prologue into one column, its loop
        // into another) — the mirror image of the defect G615 found in `pro%`.
        const bool g530Admit =
            g530SpanOn() && !g530AnyProbe() && g530Quiet() &&
            (g248FastSampleSprite || g617T4hhSprite) &&
            !s_g248Verify &&
            (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24) &&
            (!g403DisplayZ || g615Span) && !(gs->m_prim.fge && g364FogEnabled()) &&
            drawX1 >= drawX0 && drawY1 >= drawY0 && gs->m_vram != nullptr;
        g605cyc.setLeaf(g530Admit ? kG605LeafSprite : kG605LeafSpriteSlow);
        g530cyc.markLoop();
        g605cyc.markLoop();
        if (g530Admit)
        {
            // (the G615 span refusal census for this population lives just below the block)
            // --- the alpha test as an interval, verified exhaustively per distinct TEST value ---
            const G530AlphaGate g530Gate = g530BuildAlphaGate(ctx.test);
            bool g530GateOk = true;
            {
                static thread_local uint64_t s_seen[4] = {~0ull, ~0ull, ~0ull, ~0ull};
                static thread_local int s_seenN = 0;
                bool known = false;
                for (int i = 0; i < 4; ++i)
                    if (s_seen[i] == ctx.test) { known = true; break; }
                if (!known)
                {
                    g530GateOk = g530AlphaGateAgrees(ctx.test);
                    if (g530GateOk)
                    {
                        s_seen[s_seenN & 3] = ctx.test;
                        ++s_seenN;
                    }
                    else
                    {
                        g_g530Reject.fetch_add(1u, std::memory_order_relaxed);
                        std::fprintf(stderr, "[G530:gate] REJECT test=0x%llx (interval form "
                                             "disagrees with classifyAlphaTest)\n",
                                     static_cast<unsigned long long>(ctx.test));
                    }
                }
            }
            if (g530GateOk)
            {
            const bool g530Vfy = g530VerifyOn();
            const int g530SpanW = drawX1 - drawX0 + 1;
            // --- per-DRAW: the U schedule. U does not depend on Y, so the divide and the two
            // std::lround calls the generic loop ran per PIXEL run here once per COLUMN, using the
            // IDENTICAL expression (g407SubpixelPhase is excluded above, so sampleX0 ==
            // unclippedX0). Bit-exact by construction, not by a rounding identity. ---
            G530UTable &g530U = g530UTable();
            if (static_cast<int>(g530U.sampleU.size()) < g530SpanW)
            {
                g530U.sampleU.resize(static_cast<size_t>(g530SpanW));
                g530U.u0.resize(static_cast<size_t>(g530SpanW));
                g530U.u1.resize(static_cast<size_t>(g530SpanW));
                g530U.fx.resize(static_cast<size_t>(g530SpanW));
            }
            // g407SubpixelPhase and g406Round.active are per-DRAW selectors, so the kernel simply
            // carries them into the schedule instead of refusing the draw — G406's accurate UV is
            // DEFAULT-ON, so excluding it excluded every sprite on this route.
            const float g530SampleX0 =
                g407SubpixelPhase ? g407RawX0 : static_cast<float>(unclippedX0);
            const float g530SampleY0 =
                g407SubpixelPhase ? g407RawY0 : static_cast<float>(unclippedY0);
            for (int i = 0; i < g530SpanW; ++i)
            {
                const int px = drawX0 + i;
                const float tx = (static_cast<float>(px) - g530SampleX0 + s_uvBias) / spriteW;
                const float tu = u0f + (u1f - u0f) * tx;
                const uint16_t su16 = g406Round.active
                    ? g406RoundUvFixed(tu, px, g406Round.originX, g406Round.flagsU, g248Linear)
                    : static_cast<uint16_t>(
                          clampInt(static_cast<int>(std::lround(tu * 16.0f)), 0, 0xFFFF));
                g530U.sampleU[static_cast<size_t>(i)] = su16;
                const float uf = static_cast<float>(su16) / 16.0f;
                if (g248Linear)
                {
                    const float sU = uf - 0.5f;
                    const int a0 = static_cast<int>(std::floor(sU));
                    g530U.u0[static_cast<size_t>(i)] =
                        wrapTextureCoordinate(a0, g248WrapU, texW, g248MinU, g248MaxU);
                    g530U.u1[static_cast<size_t>(i)] =
                        wrapTextureCoordinate(a0 + 1, g248WrapU, texW, g248MinU, g248MaxU);
                    g530U.fx[static_cast<size_t>(i)] = sU - static_cast<float>(a0);
                }
                else
                {
                    const int a0 = wrapTextureCoordinate(static_cast<int>(uf), g248WrapU, texW,
                                                         g248MinU, g248MaxU);
                    g530U.u0[static_cast<size_t>(i)] = a0;
                    g530U.u1[static_cast<size_t>(i)] = a0;
                    g530U.fx[static_cast<size_t>(i)] = 0.0f;
                }
            }
            // --- per-DRAW: everything writePixel re-derived per pixel ---
            const uint32_t g530Block = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
            const uint32_t g530Fbw = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
            const uint32_t g530Mask = ctx.frame.fbmsk;
            const bool g530Fba = ((ctx.fba & 0x1ull) != 0ull) && ctx.frame.psm != GS_PSM_CT24;
            const bool g530Abe = gs->m_prim.abe;
            const bool g530Pabe = gs->m_pabe;
            const uint8_t g530Asel = static_cast<uint8_t>(ctx.alpha & 3u);
            const uint8_t g530Bsel = static_cast<uint8_t>((ctx.alpha >> 2) & 3u);
            const uint8_t g530Csel = static_cast<uint8_t>((ctx.alpha >> 4) & 3u);
            const uint8_t g530Dsel = static_cast<uint8_t>((ctx.alpha >> 6) & 3u);
            const uint8_t g530Fix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
            const uint32_t g530PageBwT8 =
                ((tex.tbw >> 1u) != 0u) ? (tex.tbw >> 1u) : 1u;
            const uint32_t g530TexTbw = (tex.tbw != 0u) ? tex.tbw : 1u;
            // G617 (b): read once per draw, never inside the loop — each is a per-ACCESS TLS init
            // guard (`g609_function_local_static_is_a_per_access_tls_guard`). Declared here because
            // the T4HH tap lambda below closes over them.
            const bool g617Vfy = g617VerifyOn();
            const bool g617Stat = g617StatOn();
            uint64_t g617TapN = 0u;
            // The post-wrap half of g248SamplePoint — the wrap itself is already in the U/V tables.
            // For the 32-bit formats the address is computed INLINE with GSPSMCT32::addrPSMCT32,
            // the same expression writePixel uses and the one this file's own F62 note records as
            // byte-identical to GSMem's ("proven for 32-bit and the index formats"). That deletes
            // two non-inlined calls per TAP — readTexelPSMCT32 plus the cross-TU GSMem::ReadCT32 —
            // and DC2_G530_VERIFY re-reads through GSMem and compares, so the claim is checked and
            // not merely cited.
            auto g530Tap = [&](int su, int sv) -> uint32_t {
                if (g248Psm == GS_PSM_CT32 || g248Psm == GS_PSM_CT24)
                {
                    // Mask exactly as GSMem's PixelStorageTraits::Read does
                    // (`byte_addr = bits/8 & (MEMORY_SIZE - sizeof(PackedT))`), so an out-of-range
                    // address WRAPS identically instead of reading zero. addrPSMCT32 is always
                    // 4-aligned, so the low two bits the mask clears are already clear.
                    const uint32_t toff = GSPSMCT32::addrPSMCT32(
                        tex.tbp0, g530TexTbw, static_cast<uint32_t>(su), static_cast<uint32_t>(sv)) &
                        static_cast<uint32_t>(GSMem::MEMORY_SIZE - 4u);
                    uint32_t raw = 0u;
                    std::memcpy(&raw, gs->m_vram + toff, 4);
                    if (g530VerifyOn())
                    {
                        const uint32_t ref =
                            GSMem::ReadCT32(gs->m_vram, tex.tbp0, g530TexTbw,
                                            static_cast<uint32_t>(su), static_cast<uint32_t>(sv));
                        if (ref != raw)
                        {
                            const uint64_t bad =
                                g_g530Bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (bad <= 16u)
                                std::fprintf(stderr,
                                    "[G530:vfy] TAP MISMATCH uv=(%d,%d) tbp=0x%x tbw=%u "
                                    "ref=%08x fast=%08x off=0x%x\n",
                                    su, sv, tex.tbp0, g530TexTbw, ref, raw, toff);
                        }
                    }
                    return applyTexa(gs->m_texa, g248Psm, raw);
                }
                const uint8_t idx = static_cast<uint8_t>(
                    GSMem::ReadP8(gs->m_vram, tex.tbp0, g530PageBwT8,
                                  static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
                return g248UseClutCache
                    ? g248ClutLookup(idx)
                    : lookupCLUT(gs, idx, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            };
            // ⭐⭐⭐ G617 (b): the PSMT4HH tap. `PixelStorageTraits<P4HH>` has the SAME PageExtent
            // {64,32}, BlockExtent {8,4} and ColumnExtent {8,8} as C32 and reads through the SAME
            // `PageTableC32`; its ONLY difference is `BitOffset() == 28`. With `UnpackedBitWidth`
            // 32 and `PackedT` = u8 that makes `byte_addr = pixel_addr*4 + 3` and `shift = 4`, and
            // `addrPSMCT32` is exactly `pixel_addr*4` (proven termwise: the traits' `blockId*256`
            // equals `(blockId>>5)<<13 + (blockId&31)*256` because 32*256 == 1<<13). So the tap is
            // ONE byte load and a shift, with the row half of the address hoisted to the scanline —
            // the same lowering G614(b) made for PSMT8, one format over.
            // ⚠️ `tbw` goes in UNHALVED: P4HH's page is 64 wide like C32's, unlike PSMT8 (128) and
            // plain PSMT4 (128), both of which need `tbw >> 1`. That is the whole trap in this
            // format family and it is why the reference call below passes `g530TexTbw` too.
            auto g617TapT4hh = [&](int su, const G530RowAddr &texRow, int svForVfy) -> uint32_t {
                const uint32_t off = g617T4hhNibbleOff(texRow, static_cast<uint32_t>(su));
                const uint8_t idx = static_cast<uint8_t>((gs->m_vram[off] >> 4u) & 0x0Fu);
                if (g617Vfy)
                {
                    // ORACLE: the SHIPPED GSMem reader is authoritative for every tap.
                    const uint8_t ref = static_cast<uint8_t>(
                        GSMem::ReadP4HH(gs->m_vram, tex.tbp0, g530TexTbw,
                                        static_cast<uint32_t>(su),
                                        static_cast<uint32_t>(svForVfy)));
                    if (ref != idx)
                    {
                        const uint64_t bad =
                            g_g617T4hhBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (bad <= 16u)
                            std::fprintf(stderr,
                                "[G617:vfy] T4HH TAP MISMATCH uv=(%d,%d) tbp=0x%x tbw=%u "
                                "ref=%02x fast=%02x off=0x%x\n",
                                su, svForVfy, tex.tbp0, g530TexTbw, ref, idx, off);
                    }
                }
                ++g617TapN;
                return g248ClutLookup(idx);
            };
            // writePixel's 32-bit tail, statement for statement, with every loop-invariant term
            // hoisted and the (already-satisfied) scissor test and (already-computed) alpha-test
            // classification removed.
            auto g530Commit = [&](uint32_t off, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca,
                                  bool preserveA) -> uint32_t {
                uint8_t rr = cr, gg = cg, bb = cb, aa = ca;
                if (g530Abe)
                {
                    uint32_t existing = 0u;
                    std::memcpy(&existing, gs->m_vram + off, 4);
                    const uint8_t dr = static_cast<uint8_t>(existing & 0xFFu);
                    const uint8_t dg = static_cast<uint8_t>((existing >> 8) & 0xFFu);
                    const uint8_t db = static_cast<uint8_t>((existing >> 16) & 0xFFu);
                    const uint8_t da = static_cast<uint8_t>((existing >> 24) & 0xFFu);
                    if (!(g530Pabe && (ca & 0x80u) == 0u))
                    {
                        auto pick = [](uint8_t sel, int cs, int cd) -> int {
                            if (sel == 0) return cs;
                            if (sel == 1) return cd;
                            return 0;
                        };
                        const int cAlpha = (g530Csel == 0) ? static_cast<int>(ca)
                                         : (g530Csel == 1) ? static_cast<int>(da)
                                                           : static_cast<int>(g530Fix);
                        rr = clampU8(((pick(g530Asel, rr, dr) - pick(g530Bsel, rr, dr)) * cAlpha >> 7) +
                                     pick(g530Dsel, rr, dr));
                        gg = clampU8(((pick(g530Asel, gg, dg) - pick(g530Bsel, gg, dg)) * cAlpha >> 7) +
                                     pick(g530Dsel, gg, dg));
                        bb = clampU8(((pick(g530Asel, bb, db) - pick(g530Bsel, bb, db)) * cAlpha >> 7) +
                                     pick(g530Dsel, bb, db));
                    }
                }
                if (!preserveA && g530Fba)
                    aa = static_cast<uint8_t>(aa | 0x80u);
                uint32_t pixel = static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8) |
                                 (static_cast<uint32_t>(bb) << 16) | (static_cast<uint32_t>(aa) << 24);
                if (g530Mask != 0u)
                {
                    uint32_t existing = 0u;
                    std::memcpy(&existing, gs->m_vram + off, 4);
                    pixel = (pixel & ~g530Mask) | (existing & g530Mask);
                }
                if (preserveA)
                {
                    uint32_t existing = 0u;
                    std::memcpy(&existing, gs->m_vram + off, 4);
                    pixel = (pixel & 0x00FFFFFFu) | (existing & 0xFF000000u);
                }
                return pixel;
            };

            // ⛔ G614 CEILING PROBE — read ONCE per draw, never a correctness switch. See
            // g614_sprite_scan.inc for the level table and the exactness/denominator argument.
            const G614Ceil g614C = g614CeilFlags();
            uint32_t g614Sink = 0u;

            // ⭐⭐⭐ G614: the three INLINED leaves the ceiling probe named (81.4% of this pixel).
            // Every predicate is read ONCE here — each is a function-local `static const bool`,
            // i.e. a per-ACCESS thread-safe-init guard, and reading one per pixel is precisely the
            // defect being deleted. Each leaf is admitted only where its substitution is the
            // IDENTITY: the filter only when all three G529 selectors read their shipped default,
            // the tap only for the PSMT8/CLUT-cache source the fast address form covers.
            const bool g614Vfy = g614VerifyOn();
            // ⚠️ The ceiling probe is deliberately ORTHOGONAL to the lever: every CEIL level guards
            // its own stage above the fast/slow choice, so an arm prices a stage of the SHIPPED
            // pixel. (The §5 ranking table in the fix log was taken on the pre-lever binary and is
            // the price of the PRE-G614 pixel; re-run the sweep to re-rank the current one.)
            const bool g614On = g614SprFastOn();
            const bool g614FastLerp = g614On && g248Linear &&
                                      g529FastLerpOn() && !g529NoLerpOn() && !g529LerpVfyOn();
            const bool g614FastTap = g614On && g248Psm == GS_PSM_T8 && g248UseClutCache;
            const bool g614FastComb = g614On;
            const uint8_t g614Tfx = tex.tfx;
            const bool g614Tcc = tex.tcc != 0u;
            uint64_t g614TapN = 0u, g614LerpN = 0u, g614SlideN = 0u;
            if (g614StatOn())
            {
                if (g614On)
                    g_g614Draws.fetch_add(1u, std::memory_order_relaxed);
                else
                    g_g614Reject.fetch_add(1u, std::memory_order_relaxed);
            }
            // The PSMT8 source address with every y-dependent term hoisted to the scanline. Only
            // the two rows this draw's bilinear quad reads are ever built.
            G614T8Row g614RowV0, g614RowV1;
            G530RowAddr g617TexRowV0, g617TexRowV1; // G617 (b): the P4HH source rows
            const uint32_t g614TexBlock = tex.tbp0;
            const uint32_t g614TexTbwRaw = (tex.tbw != 0u) ? tex.tbw : 1u;
            auto g614TapT8 = [&](int su, const G614T8Row &row, int svForVfy) -> uint32_t {
                const uint32_t off = g614T8Off(row, static_cast<uint32_t>(su)) &
                                     static_cast<uint32_t>(GSMem::MEMORY_SIZE - 1u);
                const uint8_t idx = gs->m_vram[off];
                if (g614Vfy)
                {
                    const uint8_t ref = static_cast<uint8_t>(
                        GSMem::ReadP8(gs->m_vram, tex.tbp0, g530PageBwT8,
                                      static_cast<uint32_t>(su), static_cast<uint32_t>(svForVfy)));
                    if (ref != idx)
                    {
                        const uint64_t bad =
                            g_g614BadTap.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (bad <= 16u)
                            std::fprintf(stderr,
                                "[G614:vfy] T8 TAP MISMATCH uv=(%d,%d) tbp=0x%x tbw=%u "
                                "ref=%02x fast=%02x off=0x%x\n",
                                su, svForVfy, tex.tbp0, g614TexTbwRaw, ref, idx, off);
                    }
                }
                ++g614TapN;
                return g248ClutLookup(idx);
            };

            uint64_t g530PxDone = 0u;
            for (int y = drawY0; y <= drawY1; ++y)
            {
                // --- per-ROW: V, the two source rows, the vertical weight, the dest page row ---
                const float ty = (static_cast<float>(y) - g530SampleY0 + s_uvBias) / spriteH;
                const float tv = v0f + (v1f - v0f) * ty;
                const uint16_t sv16 = g406Round.active
                    ? g406RoundUvFixed(tv, y, g406Round.originY, g406Round.flagsV, g248Linear)
                    : static_cast<uint16_t>(
                          clampInt(static_cast<int>(std::lround(tv * 16.0f)), 0, 0xFFFF));
                const float vf = static_cast<float>(sv16) / 16.0f;
                int g530V0 = 0, g530V1 = 0;
                float g530Fy = 0.0f;
                if (g248Linear)
                {
                    const float sV = vf - 0.5f;
                    const int b0 = static_cast<int>(std::floor(sV));
                    g530V0 = wrapTextureCoordinate(b0, g248WrapV, texH, g248MinV, g248MaxV);
                    g530V1 = wrapTextureCoordinate(b0 + 1, g248WrapV, texH, g248MinV, g248MaxV);
                    g530Fy = sV - static_cast<float>(b0);
                }
                else
                {
                    g530V0 = wrapTextureCoordinate(static_cast<int>(vf), g248WrapV, texH,
                                                   g248MinV, g248MaxV);
                    g530V1 = g530V0;
                }
                const G530RowAddr g530Row =
                    g530RowAddr(g530Block, g530Fbw, static_cast<uint32_t>(y));
                // ⭐ G614 (b): the two source scanlines this row's bilinear quad reads. Everything
                // in the PSMT8 address that depends only on v is resolved here, once per row.
                if (g614FastTap)
                {
                    g614RowV0 = g614T8Row(g614TexBlock, g614TexTbwRaw,
                                          static_cast<uint32_t>(g530V0));
                    g614RowV1 = g614T8Row(g614TexBlock, g614TexTbwRaw,
                                          static_cast<uint32_t>(g530V1));
                }
                // ⭐ G617 (b): the two SOURCE scanlines this row's tap(s) read, in the C32 page
                // geometry P4HH shares. Everything in the address that depends only on v resolves
                // here, once per row; the per-tap remainder is a table load and six ALU ops.
                if (g617T4hhSprite)
                {
                    g617TexRowV0 = g530RowAddr(tex.tbp0, g530TexTbw,
                                               static_cast<uint32_t>(g530V0));
                    g617TexRowV1 = g530RowAddr(tex.tbp0, g530TexTbw,
                                               static_cast<uint32_t>(g530V1));
                }
                bool g530QuadValid = false;
                int g530LastU0 = 0, g530LastU1 = 0;
                uint32_t q00 = 0u, q10 = 0u, q01 = 0u, q11 = 0u;

                int x = drawX0;
                while (x <= drawX1)
                {
                    // --- per 8-PIXEL GROUP: the page/block half of the destination address ---
                    const uint32_t g530GBase =
                        g530GroupBase(g530Row, static_cast<uint32_t>(x));
                    const int xEnd = (drawX1 < (x | 7)) ? drawX1 : (x | 7);
                    for (; x <= xEnd; ++x)
                    {
                        if (g614C.noPixel) // ⛔ CEIL=7: bound the whole per-pixel body
                            continue;
                        const size_t i = static_cast<size_t>(x - drawX0);
                        const int su0 = g530U.u0[i];
                        const int su1 = g530U.u1[i];
                        if (!g614C.noTap && // ⛔ CEIL=1/2/5: delete the four VRAM taps
                            (!g530QuadValid || su0 != g530LastU0 || su1 != g530LastU1))
                        {
                            // ⭐⭐⭐ G614 (d) — THE COLUMN SLIDE. `[G614:spr]` measured taps/px =
                            // 3.999 on this route: the (u0,u1) memo NEVER hits, because u advances
                            // about one texel per screen pixel here. But that is exactly the case
                            // in which this column's LEFT pair is the previous column's RIGHT pair:
                            // when su0 == the previous su1, tap(su0,V0) and tap(su0,V1) are the
                            // SAME function of the SAME arguments that produced q10 and q11 one
                            // pixel ago. Reusing them is a memo, not an approximation — it cannot
                            // change a result either way — and it halves the tap count.
                            if (g614FastTap && g248Linear && g530QuadValid && su0 == g530LastU1)
                            {
                                const uint32_t s00 = q10, s01 = q11;
                                q10 = g614TapT8(su1, g614RowV0, g530V0);
                                q11 = g614TapT8(su1, g614RowV1, g530V1);
                                if (g614Vfy)
                                {
                                    const uint32_t r00 = g614TapT8(su0, g614RowV0, g530V0);
                                    const uint32_t r01 = g614TapT8(su0, g614RowV1, g530V1);
                                    if (r00 != s00 || r01 != s01)
                                    {
                                        const uint64_t bad =
                                            g_g614BadSlide.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                        if (bad <= 16u)
                                            std::fprintf(stderr,
                                                "[G614:vfy] SLIDE MISMATCH xy=(%d,%d) u0=%d u1=%d "
                                                "prevU1=%d ref=%08x/%08x slid=%08x/%08x\n",
                                                x, y, su0, su1, g530LastU1, r00, r01, s00, s01);
                                    }
                                }
                                q00 = s00;
                                q01 = s01;
                                ++g614SlideN;
                            }
                            else if (g614FastTap)
                            {
                                q00 = g614TapT8(su0, g614RowV0, g530V0);
                                if (g248Linear)
                                {
                                    q10 = g614TapT8(su1, g614RowV0, g530V0);
                                    q01 = g614TapT8(su0, g614RowV1, g530V1);
                                    q11 = g614TapT8(su1, g614RowV1, g530V1);
                                }
                            }
                            // ⭐ G617 (b). ⚠️ NO COLUMN SLIDE: this population is POINT-sampled
                            // (`lin=0`, `tex1=0x1`/`0x201`), so there is no quad to slide and the
                            // linear arm below exists only so the kernel stays correct if a linear
                            // T4HH draw ever appears. G614's slide is a memo over a bilinear quad's
                            // right pair and has nothing to reuse when there is one tap per pixel.
                            else if (g617T4hhSprite)
                            {
                                q00 = g617TapT4hh(su0, g617TexRowV0, g530V0);
                                if (g248Linear)
                                {
                                    q10 = g617TapT4hh(su1, g617TexRowV0, g530V0);
                                    q01 = g617TapT4hh(su0, g617TexRowV1, g530V1);
                                    q11 = g617TapT4hh(su1, g617TexRowV1, g530V1);
                                }
                            }
                            else
                            {
                            q00 = g530Tap(su0, g530V0);
                            if (g248Linear)
                            {
                                q10 = g530Tap(su1, g530V0);
                                q01 = g530Tap(su0, g530V1);
                                q11 = g530Tap(su1, g530V1);
                            }
                            }
                            g530LastU0 = su0;
                            g530LastU1 = su1;
                            g530QuadValid = true;
                        }
                        uint32_t texel;
                        if (g614C.noLerp) // ⛔ CEIL=1/2/4: delete the four lerpChannel calls
                        {
                            texel = q00;
                        }
                        // ⭐ G614 (a): the bilinear filter, inlined. `g609Lerp8` is G609's own
                        // already-shipped, already-proven-exact copy of `lerpChannel`'s statements
                        // with the CRT rounding call replaced by G529's identity — and the three
                        // per-call `static const bool` init guards gone (twelve per texel).
                        else if (g614FastLerp)
                        {
                            const float fxv = g530U.fx[i];
                            const uint8_t lr = g609Lerp8(
                                static_cast<uint8_t>(q00 & 0xFFu), static_cast<uint8_t>(q10 & 0xFFu),
                                static_cast<uint8_t>(q01 & 0xFFu), static_cast<uint8_t>(q11 & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t lg = g609Lerp8(
                                static_cast<uint8_t>((q00 >> 8) & 0xFFu), static_cast<uint8_t>((q10 >> 8) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 8) & 0xFFu), static_cast<uint8_t>((q11 >> 8) & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t lb = g609Lerp8(
                                static_cast<uint8_t>((q00 >> 16) & 0xFFu), static_cast<uint8_t>((q10 >> 16) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 16) & 0xFFu), static_cast<uint8_t>((q11 >> 16) & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t la = g609Lerp8(
                                static_cast<uint8_t>((q00 >> 24) & 0xFFu), static_cast<uint8_t>((q10 >> 24) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 24) & 0xFFu), static_cast<uint8_t>((q11 >> 24) & 0xFFu),
                                fxv, g530Fy);
                            texel = static_cast<uint32_t>(lr) | (static_cast<uint32_t>(lg) << 8) |
                                    (static_cast<uint32_t>(lb) << 16) | (static_cast<uint32_t>(la) << 24);
                            ++g614LerpN;
                            if (g614Vfy)
                            {
                                // The SHIPPED lerpChannel is authoritative; the inline copy is
                                // compared against what it returns, channel by channel.
                                const uint8_t rr = lerpChannel(
                                    static_cast<uint8_t>(q00 & 0xFFu), static_cast<uint8_t>(q10 & 0xFFu),
                                    static_cast<uint8_t>(q01 & 0xFFu), static_cast<uint8_t>(q11 & 0xFFu),
                                    fxv, g530Fy);
                                const uint8_t rg = lerpChannel(
                                    static_cast<uint8_t>((q00 >> 8) & 0xFFu), static_cast<uint8_t>((q10 >> 8) & 0xFFu),
                                    static_cast<uint8_t>((q01 >> 8) & 0xFFu), static_cast<uint8_t>((q11 >> 8) & 0xFFu),
                                    fxv, g530Fy);
                                const uint8_t rb = lerpChannel(
                                    static_cast<uint8_t>((q00 >> 16) & 0xFFu), static_cast<uint8_t>((q10 >> 16) & 0xFFu),
                                    static_cast<uint8_t>((q01 >> 16) & 0xFFu), static_cast<uint8_t>((q11 >> 16) & 0xFFu),
                                    fxv, g530Fy);
                                const uint8_t ra = lerpChannel(
                                    static_cast<uint8_t>((q00 >> 24) & 0xFFu), static_cast<uint8_t>((q10 >> 24) & 0xFFu),
                                    static_cast<uint8_t>((q01 >> 24) & 0xFFu), static_cast<uint8_t>((q11 >> 24) & 0xFFu),
                                    fxv, g530Fy);
                                if (rr != lr || rg != lg || rb != lb || ra != la)
                                {
                                    const uint64_t bad =
                                        g_g614BadLerp.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                    if (bad <= 16u)
                                        std::fprintf(stderr,
                                            "[G614:vfy] LERP MISMATCH xy=(%d,%d) fx=%.9g fy=%.9g "
                                            "ref=%02x%02x%02x%02x fast=%02x%02x%02x%02x\n",
                                            x, y, fxv, g530Fy, ra, rb, rg, rr, la, lb, lg, lr);
                                }
                            }
                        }
                        else if (g248Linear)
                        {
                            const float fxv = g530U.fx[i];
                            const uint8_t lr = lerpChannel(
                                static_cast<uint8_t>(q00 & 0xFFu), static_cast<uint8_t>(q10 & 0xFFu),
                                static_cast<uint8_t>(q01 & 0xFFu), static_cast<uint8_t>(q11 & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t lg = lerpChannel(
                                static_cast<uint8_t>((q00 >> 8) & 0xFFu), static_cast<uint8_t>((q10 >> 8) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 8) & 0xFFu), static_cast<uint8_t>((q11 >> 8) & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t lb = lerpChannel(
                                static_cast<uint8_t>((q00 >> 16) & 0xFFu), static_cast<uint8_t>((q10 >> 16) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 16) & 0xFFu), static_cast<uint8_t>((q11 >> 16) & 0xFFu),
                                fxv, g530Fy);
                            const uint8_t la = lerpChannel(
                                static_cast<uint8_t>((q00 >> 24) & 0xFFu), static_cast<uint8_t>((q10 >> 24) & 0xFFu),
                                static_cast<uint8_t>((q01 >> 24) & 0xFFu), static_cast<uint8_t>((q11 >> 24) & 0xFFu),
                                fxv, g530Fy);
                            texel = static_cast<uint32_t>(lr) | (static_cast<uint32_t>(lg) << 8) |
                                    (static_cast<uint32_t>(lb) << 16) | (static_cast<uint32_t>(la) << 24);
                        }
                        else
                        {
                            texel = q00;
                        }
                        // ⛔ CEIL=2/3: delete the per-pixel combineTexture call. The substitute
                        // keeps the texel observable so the sampler above is not deleted with it.
                        // ⭐ G614 (c): combineTexture, inlined — same switch, same statements, with
                        // tfx/tcc as plain locals so the switch hoists out of the pixel loop.
                        const TextureCombineResult color = g614C.noComb
                            ? TextureCombineResult{static_cast<uint8_t>(texel & 0xFFu),
                                                   static_cast<uint8_t>((texel >> 8) & 0xFFu),
                                                   static_cast<uint8_t>((texel >> 16) & 0xFFu),
                                                   static_cast<uint8_t>((texel >> 24) & 0xFFu)}
                            : g614FastComb
                            ? g614Combine(
                            g614Tfx, g614Tcc, r, g, b, a,
                            static_cast<uint8_t>(texel & 0xFFu),
                            static_cast<uint8_t>((texel >> 8) & 0xFFu),
                            static_cast<uint8_t>((texel >> 16) & 0xFFu),
                            static_cast<uint8_t>((texel >> 24) & 0xFFu))
                            : combineTexture(
                            tex, r, g, b, a,
                            static_cast<uint8_t>(texel & 0xFFu),
                            static_cast<uint8_t>((texel >> 8) & 0xFFu),
                            static_cast<uint8_t>((texel >> 16) & 0xFFu),
                            static_cast<uint8_t>((texel >> 24) & 0xFFu));
                        if (g614Vfy && g614FastComb)
                        {
                            // The SHIPPED combineTexture is authoritative.
                            const TextureCombineResult ref = combineTexture(
                                tex, r, g, b, a,
                                static_cast<uint8_t>(texel & 0xFFu),
                                static_cast<uint8_t>((texel >> 8) & 0xFFu),
                                static_cast<uint8_t>((texel >> 16) & 0xFFu),
                                static_cast<uint8_t>((texel >> 24) & 0xFFu));
                            if (ref.r != color.r || ref.g != color.g ||
                                ref.b != color.b || ref.a != color.a)
                            {
                                const uint64_t bad =
                                    g_g614BadComb.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (bad <= 16u)
                                    std::fprintf(stderr,
                                        "[G614:vfy] COMBINE MISMATCH xy=(%d,%d) tfx=%u tcc=%u "
                                        "ref=%02x%02x%02x%02x fast=%02x%02x%02x%02x\n",
                                        x, y, g614Tfx, g614Tcc ? 1u : 0u,
                                        ref.a, ref.b, ref.g, ref.r,
                                        color.a, color.b, color.g, color.r);
                            }
                        }
                        // Z test — identical to spriteZPass, with the read skipped for the two
                        // ZTST codes that provably ignore it (NEVER / ALWAYS). That skip is exact:
                        // the generic lambda reads zdst and then never looks at it in those cases.
                        bool zpass = true;
                        if (sZActive)
                        {
                            if (sZtst == 0u)
                                zpass = false;
                            else if (sZtst != 1u)
                            {
                                // ⚠️ G615: this arm reads GUEST VRAM Z, which is the wrong authority
                                // for a g403DisplayZ draw — which is why `!g403DisplayZ` was a
                                // refusal conjunct in the first place. G615 does NOT supply a
                                // private-mirror arm here: it admits only the subset whose depth
                                // operation is a proven no-op (`g615ZNoop`), for which `sZActive` is
                                // false and this whole block is unreachable. A mirror arm would be
                                // an UNEXERCISED path on every route measured so far — `bad=0` with
                                // `checks=0` is a failed run, not a proof.
                                const uint32_t zdst =
                                    sZ16 ? GSMem::ReadZ16(gs->m_vram, sZbp, sZbw,
                                                          static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                                    : sZ24 ? GSMem::ReadZ24(gs->m_vram, sZbp, sZbw,
                                                            static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                                           : GSMem::ReadZ32(gs->m_vram, sZbp, sZbw,
                                                            static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                                zpass = (sZtst == 2u) ? (sZi >= zdst) : (sZi > zdst);
                            }
                        }
                        if (g530Vfy && zpass != spriteZPass(x, y))
                        {
                            const uint64_t bad = g_g530Bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (bad <= 16u)
                                std::fprintf(stderr, "[G530:vfy] ZPASS MISMATCH xy=(%d,%d) "
                                                     "ztst=%u fast=%u\n",
                                             x, y, sZtst, zpass ? 1u : 0u);
                        }
                        if (!zpass)
                            continue;
                        const bool pass = g530Gate.pass(color.a);
                        const bool wfb = pass ? g530Gate.passFb : g530Gate.failFb;
                        const bool wzb = pass ? g530Gate.passZ : g530Gate.failZ;
                        const bool wpa = pass ? g530Gate.passPa : g530Gate.failPa;
                        if (wfb)
                        {
                            const uint32_t off = g530GBase +
                                static_cast<uint32_t>(g530Row.ct[x & 0x7u]) * 4u;
                            // ⛔ CEIL=3/6: delete the blend/mask/store. The address and the colour
                            // stay observable through g614Sink so only the COMMIT is priced.
                            if (g614C.noCommit)
                            {
                                g614Sink += off ^ static_cast<uint32_t>(color.r) ^
                                            (static_cast<uint32_t>(color.a) << 8);
                            }
                            else if (off + 4u <= gs->m_vramSize)
                            {
                                const uint32_t pixel =
                                    g530Commit(off, color.r, color.g, color.b, color.a, wpa);
                                if (g530Vfy)
                                {
                                    // The PROVEN path performs the store; the kernel's word is
                                    // compared against what it actually wrote.
                                    writePixel(gs, x, y, color.r, color.g, color.b, color.a);
                                    uint32_t after = 0u;
                                    std::memcpy(&after, gs->m_vram + off, 4);
                                    g_g530Px.fetch_add(1u, std::memory_order_relaxed);
                                    if (after != pixel)
                                    {
                                        const uint64_t bad =
                                            g_g530Bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                        if (bad <= 16u)
                                            std::fprintf(stderr,
                                                "[G530:vfy] PIXEL MISMATCH xy=(%d,%d) off=0x%x "
                                                "ref=%08x fast=%08x fbp=0x%x abe=%u mask=%08x\n",
                                                x, y, off, after, pixel, ctx.frame.fbp,
                                                g530Abe ? 1u : 0u, g530Mask);
                                    }
                                }
                                else
                                {
                                    std::memcpy(gs->m_vram + off, &pixel, 4);
                                }
                            }
                            else if (g530Vfy)
                            {
                                writePixel(gs, x, y, color.r, color.g, color.b, color.a);
                            }
                        }
                        if (wzb)
                            spriteZStore(x, y); // self-guards on !sZWrite, which g615ZNoop proves
                        ++g530PxDone;
                    }
                }
            }
            if (g615Span && g615Stat)
            {
                g_g615SpanDraws.fetch_add(1u, std::memory_order_relaxed);
                g_g615SpanPx.fetch_add(g530PxDone, std::memory_order_relaxed);
                g615Report();
            }
            // ⭐ G617 (b) coverage. Booked ONLY for the population the lever admits, so `bad=0` can
            // never be read off a path that was not taken (`g589_oracle_with_zero_checks`).
            if (g617T4hhSprite && g617Stat)
            {
                g_g617T4hhDraws.fetch_add(1u, std::memory_order_relaxed);
                g_g617T4hhPx.fetch_add(g530PxDone, std::memory_order_relaxed);
                g_g617T4hhTaps.fetch_add(g617TapN, std::memory_order_relaxed);
                g617Report();
            }
            if (g614C.any)
                g_g614CeilSink.fetch_add(g614Sink, std::memory_order_relaxed);
            if (g614StatOn())
            {
                g_g614Px.fetch_add(g530PxDone, std::memory_order_relaxed);
                g_g614Taps.fetch_add(g614TapN, std::memory_order_relaxed);
                g_g614Lerps.fetch_add(g614LerpN, std::memory_order_relaxed);
                g_g614Slides.fetch_add(g614SlideN, std::memory_order_relaxed);
                g614Report();
            }
            g_g530Px.fetch_add(g530Vfy ? 0u : g530PxDone, std::memory_order_relaxed);
            g_g530Draws.fetch_add(1u, std::memory_order_relaxed);
            g530SpanReport();
            g530Handled = true;
            }
        }

        // ⭐ G615: the COVERAGE DENOMINATOR for (b). Every private-Z textured sprite that did NOT
        // reach the span kernel is booked here with the conjunct that refused it, so "N draws
        // admitted" can be read as a fraction instead of an absolute (`g536_coverage_denominator_
        // for_a_defect_rate` — a CLEAN result needs a denominator too).
        if (g615Stat && g403DisplayZ && !g530Handled && t_g144InReplay)
        {
            // ⚠️ G617: the sampler conjunct is now `g248FastSampleSprite || g617T4hhSprite`. Left
            // as `!g248FastSampleSprite`, this census would book every PSMT4HH draw the G617 tap
            // REFUSED for some other reason to `samp` — the exact mis-attribution that made
            // `spanRej(samp=102994)` readable in the first place. A refusal census has to track its
            // admission predicate or it starts naming the wrong conjunct.
            g_g615SpanRej[!(g248FastSampleSprite || g617T4hhSprite) ? kG615SpanRejSamp
                          : !(ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
                              ? kG615SpanRejFpsm
                          : (gs->m_prim.fge && g364FogEnabled()) ? kG615SpanRejFog
                                                                 : kG615SpanRejOther]
                .fetch_add(1u, std::memory_order_relaxed);
            g615Report();
        }
        if (!g530Handled)
        for (int y = drawY0; y <= drawY1; ++y)
        {
            g248QValid = false;
            const float sampleY0 =
                g407SubpixelPhase ? g407RawY0 : static_cast<float>(unclippedY0);
            float ty = (static_cast<float>(y) - sampleY0 + s_uvBias) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                if (g530NoPx) continue; // ⛔ whole-body ceiling probe
                const float sampleX0 =
                    g407SubpixelPhase ? g407RawX0 : static_cast<float>(unclippedX0);
                // ⛔ G530 NOUV probe: skip the per-pixel divide + interpolate entirely.
                float tx = g530NoUv
                    ? 0.0f
                    : (static_cast<float>(x) - sampleX0 + s_uvBias) / spriteW;
                float texUf = g530NoUv ? u0f : (u0f + (u1f - u0f) * tx);
                uint32_t texel = 0xFFFF00FFu;
                uint16_t g258SampleU = 0u, g258SampleV = 0u;
                if (gs->m_prim.fst)
                {
                    // ⛔ G530 NOUV probe: pin the interpolated coordinate to the row's first pixel,
                    // deleting the per-pixel divide + two std::lround CRT calls (knowingly wrong).
                    const float g530TexUf = g530NoUv ? (u0f) : texUf;
                    const float g530TexVf = g530NoUv ? (v0f) : texVf;
                    uint16_t sampleU = g406Round.active
                        ? g406RoundUvFixed(g530TexUf, x, g406Round.originX,
                                          g406Round.flagsU, g248Linear)
                        : static_cast<uint16_t>(clampInt(
                              static_cast<int>(std::lround(g530TexUf * 16.0f)), 0, 0xFFFF));
                    uint16_t sampleV = g406Round.active
                        ? g406RoundUvFixed(g530TexVf, y, g406Round.originY,
                                          g406Round.flagsV, g248Linear)
                        : static_cast<uint16_t>(clampInt(
                              static_cast<int>(std::lround(g530TexVf * 16.0f)), 0, 0xFFFF));
                    g258SampleU = sampleU;
                    g258SampleV = sampleV;
                    if (g530NoTex)
                    {
                        texel = 0x80808080u; // ⛔ probe: deletes the whole sampler
                    }
                    else if (g248FastSampleSprite)
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

                // ⛔ G530 NOCOMB probe: delete the per-pixel combineTexture switch (the alpha-test
                // classify below is deleted by the same flag). Knowingly wrong pixels.
                const TextureCombineResult color = g530NoComb
                    ? TextureCombineResult{tr, tg, tb, ta}
                    : combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
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
                const AlphaTestResult alphaTest = g530NoComb
                    ? AlphaTestResult{true, true, false}
                    : classifyAlphaTest(ctx.test, color.a);
                // G364: fog the post-texture RGB (a sprite's F is flat = second vertex's).
                uint8_t fr = color.r, fg = color.g, fb = color.b;
                if (gs->m_prim.fge && g364FogEnabled())
                    g364ApplyFog(v1.fog, fr, fg, fb);
                g219SpritePixelProbe(ctx, gs, x, y, fr, fg, fb, color.a, 1);
                // G365 (diagnostic, default-off): textured-SPRITE half of the right-edge-line
                // attribution (see the matching [G365:tri] probe in drawTriangle).
                {
                    static const bool s_g365s = (std::getenv("DC2_G365_WATCH") != nullptr);
                    if (s_g365s && ctx.frame.fbp == 0x68u && x >= 508 && x <= 511 &&
                        (static_cast<uint32_t>(fr) | fg | fb) != 0u && alphaTest.writeFramebuffer)
                    {
                        static std::atomic<uint32_t> s_sn2{0};
                        const uint32_t sn = s_sn2.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (sn <= 200u || (sn % 20000u) == 0u)
                            std::fprintf(stderr,
                                "[G365:spr] n=%u xy=(%d,%d) tbp=0x%x tpsm=0x%x tfx=%u tbw=%u "
                                "rgb=(%u,%u,%u) a=%u v0=(%.1f,%.1f uv=%u,%u) v1=(%.1f,%.1f uv=%u,%u)\n",
                                sn, x, y, tex.tbp0, (unsigned)tex.psm, (unsigned)tex.tfx,
                                (unsigned)tex.tbw, fr, fg, fb, color.a,
                                v0.x, v0.y, (unsigned)v0.u, (unsigned)v0.v,
                                v1.x, v1.y, (unsigned)v1.u, (unsigned)v1.v);
                    }
                }
                if (alphaTest.writeFramebuffer && !g530NoWr) // ⛔ NOWR probe deletes the commit
                    writePixel(gs, x, y, fr, fg, fb, color.a);
                if (alphaTest.writeDepth)
                    spriteZStore(x, y); // G203: guest Z write (e.g. the game's clear-sprite)
            }
        }
    }
    else
    {
        // ⛔⛔ G615 — THE CENSUS DEFECT THAT CREATED "TARGET K". drawSprite's ONLY `markLoop()` sits
        // inside the `if (gs->m_prim.tme)` arm above, so before this line every UNTEXTURED sprite
        // booked its whole fill loop into the PROLOGUE column of the `sprite` leaf. That is the
        // entire content of `[G605:leaf] disp sprite pro% = 38.8 / 50.79 us/call` (G613 §5a), which
        // ROADMAP then named as a per-DRAW prologue to decompose. It is a *second population*.
        // Split it into its own leaf and mark its loop, so both columns mean what they say.
        g605cyc.setLeaf(kG605LeafSpriteUntex);
        g605cyc.markLoop();
        // G364: untextured sprite — flat colour, so fog once up front.
        uint8_t ur = r, ug = g, ub = b;
        if (gs->m_prim.fge && g364FogEnabled())
            g364ApplyFog(v1.fog, ur, ug, ub);

        // G588: all guards describe work writePixel reduces to one unconditional CT32 store.
        // The verifier still calls writePixel and compares the word, so it cannot author pixels.
        const AlphaTestResult g588Alpha = classifyAlphaTest(ctx.test, a);
        const bool g588Selected = g588SolidSpriteOn() && t_g144InReplay;
        const bool g588Quiet = g530Quiet() && !g530AnyProbe();
        const bool g588Layout = gs->m_vram != nullptr && ctx.frame.psm == GS_PSM_CT32 &&
                                ctx.frame.fbmsk == 0u;
        const bool g588State = !gs->m_prim.abe && !sZActive && !sZWrite;
        // ⛔⛔ G615: THE HYPOTHESIS THIS CENSUS REFUTED. `[G588:solid] draws=0` on `dungeon1` while
        // 13.9% of the route's replay (19,352 draws, 59.5 M px at 312.8 cyc/inside) walks the
        // generic per-pixel nest below to store a flat colour. The G588 reject class printed
        // `state`, and `state` is the conjunction of THREE terms — so the phase assumed the depth
        // terms and built a private-Z admission for it. Splitting the reason by conjunct says
        // otherwise: **`rej(abe=19352, notpriv=0, ztst=0, nowrite=0)` — every single one is refused
        // on ALPHA BLENDING**, which is not a depth question and is not a constant store either.
        // The lever that population needs is an exact blended constant-source fill. Named, not built.
        // ⭐⭐ A FIRST FAILING CONJUNCT IS NOT A REASON WHEN THE SITE IS A CONJUNCTION
        // (`g603_admission_census_names_the_blocker`, applied to a census's own reason column).
        const bool g615Stat = g615StatOn();
        if (g615Stat && g588Selected && !g588State)
        {
            g_g615SolidRej[gs->m_prim.abe  ? kG615SolidRejAbe
                           : !g403DisplayZ ? kG615SolidRejNotPriv
                           : sZtst != 1u   ? kG615SolidRejZtst
                                           : kG615SolidRejNoWrite]
                .fetch_add(1u, std::memory_order_relaxed);
            g615Report();
        }
        // `writeDepth` is not a pixel-side requirement here: g588State already proves
        // sZWrite=false, so the generic spriteZStore call is a no-op regardless of AFAIL's depth
        // permission. Requiring !writeDepth rejected every ordinary alpha-test-disabled sprite.
        const bool g588AlphaExact = g588Alpha.writeFramebuffer &&
                                    !g588Alpha.preserveDestinationAlpha;
        const bool g588Exact =
            g588Selected && g588Quiet && g588Layout && g588State && g588AlphaExact;
        if (g588Exact)
        {
            uint8_t outA = a;
            if ((ctx.fba & 0x1ull) != 0ull)
                outA = static_cast<uint8_t>(outA | 0x80u);
            const uint32_t pixel = static_cast<uint32_t>(ur) |
                (static_cast<uint32_t>(ug) << 8u) |
                (static_cast<uint32_t>(ub) << 16u) |
                (static_cast<uint32_t>(outA) << 24u);
            const uint32_t block = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
            const uint32_t width = ctx.frame.fbw != 0u ? ctx.frame.fbw : 1u;
            const bool verify = g588SolidSpriteVerifyOn();
            const bool stat = g588SolidSpriteStatOn();
            uint64_t committed = 0u;
            if (verify)
            {
                // Oracle path: generic writePixel remains authoritative. Reading through the exact
                // fast address proves both the constant word and the factorised address for every
                // eligible pixel; no candidate store can conceal a mismatch.
                for (int y = drawY0; y <= drawY1; ++y)
                {
                    const G530RowAddr row = g530RowAddr(block, width, static_cast<uint32_t>(y));
                    uint32_t groupX = ~0u;
                    uint32_t groupBase = 0u;
                    for (int x = drawX0; x <= drawX1; ++x)
                    {
                        const uint32_t ux = static_cast<uint32_t>(x);
                        const uint32_t gx = ux & ~7u;
                        if (gx != groupX)
                        {
                            groupX = gx;
                            groupBase = g530GroupBase(row, ux);
                        }
                        const uint32_t off = groupBase +
                            static_cast<uint32_t>(row.ct[ux & 0x7u]) * 4u;
                        if (off + 4u > gs->m_vramSize)
                            continue;
                        writePixel(gs, x, y, ur, ug, ub, a);
                        uint32_t actual = 0u;
                        std::memcpy(&actual, gs->m_vram + off, sizeof(actual));
                        if (actual != pixel)
                        {
                            const uint64_t bad =
                                g_g588Bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (bad <= 16u)
                                std::fprintf(stderr,
                                             "[G588:bad] n=%llu fbp=%x fbw=%u xy=%d,%d "
                                             "ref=%08x fast=%08x\n",
                                             static_cast<unsigned long long>(bad), ctx.frame.fbp,
                                             static_cast<uint32_t>(ctx.frame.fbw), x, y,
                                             actual, pixel);
                        }
                        ++committed;
                    }
                }
            }
            else
            {
                // Every columnTable32 row consists of four adjacent word pairs. Full aligned
                // 8-pixel groups therefore need four 8-byte stores, not eight 4-byte stores and
                // eight inner-loop address/branch iterations. Clipped edge pixels retain the
                // original per-pixel bound rule. The pair value is two identical CT32 words, so
                // this is byte-for-byte the same little-endian commit.
                const uint64_t pair = static_cast<uint64_t>(pixel) |
                                      (static_cast<uint64_t>(pixel) << 32u);
                for (int y = drawY0; y <= drawY1; ++y)
                {
                    const G530RowAddr row = g530RowAddr(block, width, static_cast<uint32_t>(y));
                    int x = drawX0;
                    const auto writeOne = [&](int sx) {
                        const uint32_t ux = static_cast<uint32_t>(sx);
                        const uint32_t base = g530GroupBase(row, ux);
                        const uint32_t off = base +
                            static_cast<uint32_t>(row.ct[ux & 0x7u]) * 4u;
                        if (off + 4u <= gs->m_vramSize)
                        {
                            std::memcpy(gs->m_vram + off, &pixel, sizeof(pixel));
                            if (stat)
                                ++committed;
                        }
                    };

                    // Reach an aligned 8-pixel group without changing clipped-edge behaviour.
                    while (x <= drawX1 && (x & 7) != 0)
                    {
                        writeOne(x);
                        ++x;
                    }
                    for (; x + 7 <= drawX1; x += 8)
                    {
                        const uint32_t base =
                            g530GroupBase(row, static_cast<uint32_t>(x));
                        for (unsigned p = 0; p < 4u; ++p)
                        {
                            const uint32_t off = base +
                                static_cast<uint32_t>(row.ct[p * 2u]) * 4u;
                            if (off + 8u <= gs->m_vramSize)
                            {
                                std::memcpy(gs->m_vram + off, &pair, sizeof(pair));
                                if (stat)
                                    committed += 2u;
                            }
                            else if (off + 4u <= gs->m_vramSize)
                            {
                                std::memcpy(gs->m_vram + off, &pixel, sizeof(pixel));
                                if (stat)
                                    ++committed;
                            }
                        }
                    }
                    while (x <= drawX1)
                    {
                        writeOne(x);
                        ++x;
                    }
                }
            }
            if (stat)
            {
                g_g588Pixels.fetch_add(committed, std::memory_order_relaxed);
                g_g588Draws.fetch_add(1u, std::memory_order_relaxed);
                g588SolidSpriteReport();
            }
            return;
        }
        if (g588Selected && g588SolidSpriteStatOn())
        {
            g_g588Reject.fetch_add(1u, std::memory_order_relaxed);
            if (!g588Quiet) g_g588RejQuiet.fetch_add(1u, std::memory_order_relaxed);
            else if (!g588Layout) g_g588RejLayout.fetch_add(1u, std::memory_order_relaxed);
            else if (!g588State) g_g588RejState.fetch_add(1u, std::memory_order_relaxed);
            else g_g588RejAlpha.fetch_add(1u, std::memory_order_relaxed);
            g588SolidSpriteReport();
        }

        // ================== ⭐⭐⭐ G617 (a): THE EXACT BLENDED UNTEXTURED FILL ==================
        // 19,352 draws / 59,533,088 px / 323.8 cyc/inside on `dungeon1` — 25.2% of the route's whole
        // CPU band replay — for a loop whose only per-pixel input is the DESTINATION word, and only
        // when the blend actually reads it. G588 refuses all of them on `!abe`; the shape census,
        // once it could see `tme`, showed HALF of them (`alpha=0x2a`, A=B=C=2 D=0) carry a blend
        // that is algebraically `Cs`. Full rationale + the exactness argument: g617_sprite_fill.inc.
        //
        // ⛔⛔ The arm (`g617FillArm`) is read at drawSprite ENTRY, before the `tme` split — reading
        // it here, at the top of the UNTEXTURED branch, still samples it on the lever's own
        // schedule, which is what broke the first gate. See the note at the entry.
        const bool g617Stat = g617StatOn();
        const bool g617Vfy = g617VerifyOn();
        {
            // Every conjunct is a per-DRAW constant. `g588Quiet` is exactly the right predicate:
            // it is the AND of every per-pixel diagnostic the generic nest can service and a
            // specialized commit cannot (`g530Quiet` + no G530 probe armed).
            const bool fillLayout = gs->m_vram != nullptr && ctx.frame.psm == GS_PSM_CT32;
            // ⭐⭐⭐ THE DEPTH ARM — AND WHY THIS PHASE IS ALLOWED TO BUILD ONE WHEN G615 WAS NOT.
            //
            // G615 refused to wire in its private-Z row accessor and said exactly what would
            // license it: "`zRd`/`zSt` going non-zero is the signal that the retained accessor in
            // `g615_privz_row.inc` is finally worth wiring in." Revision 1 of THIS kernel shipped
            // with G615's no-op-only rule (`!sZActive && !sZWrite`) and its census fired that
            // signal on the first run: **`[G617:fill] rej(z=18176)` of ~19,352 draws** — 94% of
            // Target N BY DRAW writes depth, and every one is a private-mirror (`prz=1`) draw.
            // Those refused draws are shape rows #01/#02, which are also the ones carrying
            // `ALPHA=0x2a` — the identity blend whose committed word is a draw CONSTANT. So the
            // depth arm is not an unexercised path bought on faith: it is the precondition for the
            // constant-word group store, and `[G617:fill] const(draws=0 px=0)` in revision 1 is the
            // measurement proving the group store was otherwise DEAD CODE on this route.
            //
            // Admission covers the two shapes whose depth work needs no READ:
            //   * the proven no-op        (`!sZActive && !sZWrite`), G615's subset; and
            //   * ZTST=ALWAYS + a write   — the test passes every pixel without reading `zdst`
            //     (`spriteZPass`'s `case 1: return true`), so only the STORE remains, and it is
            //     admitted only when the destination is the private MIRROR, whose accessor is
            //     exact and already written.
            // ⛔ A guest-VRAM Z write (GSMem::WriteZ16/24/32) is refused: that is a different
            // address space and would be a second unexercised arm.
            const bool fillZNoop = !sZActive && !sZWrite;
            const bool fillZMirrorWrite = sZtst == 1u && sZWrite && g403DisplayZ;
            const bool fillZOk = fillZNoop || fillZMirrorWrite;
            // The alpha test's only pixel-varying input is `a`, which is FLAT for an untextured
            // sprite, so `g588Alpha` (already classified above) is the whole draw's verdict.
            const bool fillAlpha = g588Alpha.writeFramebuffer;
            const bool fillSelected = g617FillArm && t_g144InReplay;
            const bool fillExact =
                fillSelected && g588Quiet && fillLayout && fillZOk && fillAlpha;
            if (g617Stat && fillSelected && !fillExact)
            {
                g_g617FillRej[!g588Quiet    ? kG617FillRejQuiet
                              : !fillLayout ? kG617FillRejLayout
                              : !fillZOk    ? kG617FillRejZ
                                            : kG617FillRejAlpha]
                    .fetch_add(1u, std::memory_order_relaxed);
                g617Report();
            }
            if (fillExact && drawX1 >= drawX0 && drawY1 >= drawY0)
            {
                // --- per DRAW: the blend classification, the committed colour, the address base ---
                const G617Blend g617Bl =
                    g617ClassifyBlend(ctx.alpha, gs->m_prim.abe != 0u, gs->m_pabe != 0u, a);
                const bool g617PreserveA = g588Alpha.preserveDestinationAlpha;
                const uint32_t g617Mask = ctx.frame.fbmsk;
                const bool g617Fba = ((ctx.fba & 0x1ull) != 0ull) && ctx.frame.psm != GS_PSM_CT24;
                // The destination word is needed per pixel only when the blend reads it, when the
                // fbmsk merges it, or when AFAIL=RGB_ONLY preserves its alpha.
                const bool g617NeedDst =
                    !g617Bl.destIndependent || g617Mask != 0u || g617PreserveA;
                uint8_t g617A = a;
                if (!g617PreserveA && g617Fba)
                    g617A = static_cast<uint8_t>(g617A | 0x80u);
                // The dest-independent colour, computed ONCE. For a dest-reading blend this is the
                // unblended source and the loop recomputes the channels per pixel.
                uint8_t g617R = ur, g617G = ug, g617B = ub;
                if (g617Bl.destIndependent)
                    g617BlendRgb(g617Bl, a, 0u, 0u, 0u, 0u, g617R, g617G, g617B);
                const uint32_t g617Const = static_cast<uint32_t>(g617R) |
                                           (static_cast<uint32_t>(g617G) << 8u) |
                                           (static_cast<uint32_t>(g617B) << 16u) |
                                           (static_cast<uint32_t>(g617A) << 24u);
                const uint64_t g617ConstPair = static_cast<uint64_t>(g617Const) |
                                               (static_cast<uint64_t>(g617Const) << 32u);
                const uint32_t g617Block = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
                const uint32_t g617Fbw = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
                uint64_t g617Px = 0u;
                // ⭐ The private-mirror depth store, hoisted. `g615ZBase()` calls the
                // function-local-`static` `std::vector` accessor ONCE PER DRAW instead of once per
                // pixel access, and `g615ZRowFor` resolves the `y` half of
                // `g403DisplayZWrite`'s bounds predicate once per ROW — leaving `row.p != nullptr`
                // to carry `0 <= y < 512` and the `x` compare unchanged. Exact, statement for
                // statement, against the shipped writer (which ignores its `fbp` argument).
                // ⚠️ `writeDepth` is the ALPHA TEST's depth permission and it is a draw constant
                // here for the same reason `writeFramebuffer` is: `a` is flat.
                const bool g617ZWrite = fillZMirrorWrite && g588Alpha.writeDepth;
                uint32_t *const g617ZBasePtr = g617ZWrite ? g615ZBase() : nullptr;

                // `writePixel`'s 32-bit tail for ONE pixel whose source colour is a draw constant.
                // `existing` is loaded once and reused for the blend, the fbmsk merge and the AFAIL
                // alpha preserve — `writePixel` re-loads it for each, with no store in between, so
                // one load is the same value three times.
                // ⚠️ It COMPUTES the word and returns it; under the oracle it must not STORE, because
                // the proven path has to be the one that writes (`g513_oracle_compares_decisions`).
                const auto g617Word = [&](uint32_t off) -> uint32_t {
                    uint32_t pixel = g617Const;
                    if (g617NeedDst)
                    {
                        uint32_t existing = 0u;
                        std::memcpy(&existing, gs->m_vram + off, 4);
                        uint8_t rr = ur, gg = ug, bb = ub;
                        if (!g617Bl.destIndependent)
                            g617BlendRgb(g617Bl, a,
                                         static_cast<uint8_t>(existing & 0xFFu),
                                         static_cast<uint8_t>((existing >> 8) & 0xFFu),
                                         static_cast<uint8_t>((existing >> 16) & 0xFFu),
                                         static_cast<uint8_t>((existing >> 24) & 0xFFu),
                                         rr, gg, bb);
                        else
                        {
                            rr = g617R; gg = g617G; bb = g617B;
                        }
                        pixel = static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8u) |
                                (static_cast<uint32_t>(bb) << 16u) |
                                (static_cast<uint32_t>(g617A) << 24u);
                        if (g617Mask != 0u)
                            pixel = (pixel & ~g617Mask) | (existing & g617Mask);
                        if (g617PreserveA)
                            pixel = (pixel & 0x00FFFFFFu) | (existing & 0xFF000000u);
                    }
                    return pixel;
                };

                for (int y = drawY0; y <= drawY1; ++y)
                {
                    const G530RowAddr row =
                        g530RowAddr(g617Block, g617Fbw, static_cast<uint32_t>(y));
                    // The depth store is a CONSTANT over the whole rect (`sZi` is the flat sprite
                    // Z), the test is ALWAYS so every pixel reaches it, and the mirror row is
                    // linear — so the per-pixel `g615ZStore` loop collapses to one run per row.
                    // Each pixel is still written exactly once with exactly the same value.
                    const G615ZRow zrow =
                        g617ZWrite ? g615ZRowFor(g617ZBasePtr, y) : G615ZRow{nullptr};
                    const int zx0 = (drawX0 > 0) ? drawX0 : 0;
                    const int zx1 = (drawX1 < static_cast<int>(kG403DisplayZW) - 1)
                                        ? drawX1
                                        : static_cast<int>(kG403DisplayZW) - 1;
                    if (zrow.p != nullptr)
                        for (int zx = zx0; zx <= zx1; ++zx)
                            zrow.p[static_cast<size_t>(zx)] = sZi;
                    int x = drawX0;
                    const auto one = [&](int sx) {
                        const uint32_t ux = static_cast<uint32_t>(sx);
                        const uint32_t off = g530GroupBase(row, ux) +
                                             static_cast<uint32_t>(row.ct[ux & 0x7u]) * 4u;
                        if (off + 4u > gs->m_vramSize)
                            return;
                        const uint32_t want = g617Word(off);
                        if (g617Vfy)
                        {
                            // ⭐⭐ THE DEPTH ARM'S ORACLE IS A SET EQUALITY, NOT A VALUE COMPARE.
                            // Both paths store the same constant `sZi`, so comparing the stored
                            // WORD proves nothing — a row fill that covered the wrong pixels would
                            // pass it. What has to hold is that the run this kernel writes is
                            // exactly the set of pixels the shipped `spriteZStore` would write:
                            // `sZWrite` AND inside the mirror's 512x512 bounds, gated by the alpha
                            // test's depth permission.
                            const bool wantZ = sZWrite && g588Alpha.writeDepth &&
                                               sx >= 0 && y >= 0 &&
                                               sx < static_cast<int>(kG403DisplayZW) &&
                                               y < static_cast<int>(kG403DisplayZH);
                            const bool didZ =
                                (zrow.p != nullptr) && sx >= zx0 && sx <= zx1;
                            if (wantZ != didZ)
                            {
                                const uint64_t bad =
                                    g_g617FillZBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (bad <= 16u)
                                    std::fprintf(stderr,
                                        "[G617:vfy] FILL ZSET MISMATCH xy=(%d,%d) want=%u did=%u "
                                        "ztst=%u zwr=%u prz=%u\n",
                                        sx, y, wantZ ? 1u : 0u, didZ ? 1u : 0u, sZtst,
                                        sZWrite ? 1u : 0u, g403DisplayZ ? 1u : 0u);
                            }
                            // ORACLE: the PROVEN path performs the store; the kernel's word is
                            // compared against what it actually wrote, so a kernel defect can never
                            // author a pixel while the oracle is on.
                            writePixel(gs, sx, y, ur, ug, ub, a);
                            uint32_t after = 0u;
                            std::memcpy(&after, gs->m_vram + off, 4);
                            if (after != want)
                            {
                                const uint64_t bad =
                                    g_g617FillBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (bad <= 16u)
                                    std::fprintf(stderr,
                                        "[G617:vfy] FILL MISMATCH xy=(%d,%d) off=0x%x ref=%08x "
                                        "fast=%08x fbp=0x%x alpha=%llx mask=%08x\n",
                                        sx, y, off, after, want, ctx.frame.fbp,
                                        static_cast<unsigned long long>(ctx.alpha), g617Mask);
                            }
                        }
                        else
                        {
                            std::memcpy(gs->m_vram + off, &want, 4);
                        }
                        ++g617Px;
                    };
                    // ⭐ The constant-word case is G588's promoted commit: every columnTable32 row
                    // is four ADJACENT word pairs, so a full aligned 8-pixel group is four 8-byte
                    // stores instead of eight 4-byte stores and eight address/branch iterations.
                    // Byte-for-byte the same little-endian commit (the pair is two identical words).
                    if (!g617NeedDst && !g617Vfy && g617ColumnPairsAdjacent())
                    {
                        while (x <= drawX1 && (x & 7) != 0)
                        {
                            one(x);
                            ++x;
                        }
                        for (; x + 7 <= drawX1; x += 8)
                        {
                            const uint32_t base =
                                g530GroupBase(row, static_cast<uint32_t>(x));
                            for (unsigned p = 0; p < 4u; ++p)
                            {
                                const uint32_t off =
                                    base + static_cast<uint32_t>(row.ct[p * 2u]) * 4u;
                                if (off + 8u <= gs->m_vramSize)
                                {
                                    std::memcpy(gs->m_vram + off, &g617ConstPair, 8);
                                    g617Px += 2u;
                                }
                                else if (off + 4u <= gs->m_vramSize)
                                {
                                    std::memcpy(gs->m_vram + off, &g617Const, 4);
                                    ++g617Px;
                                }
                            }
                        }
                    }
                    for (; x <= drawX1; ++x)
                        one(x);
                }
                if (g617Stat)
                {
                    g_g617FillDraws.fetch_add(1u, std::memory_order_relaxed);
                    g_g617FillPx.fetch_add(g617Px, std::memory_order_relaxed);
                    if (!g617NeedDst)
                    {
                        g_g617FillConstDraws.fetch_add(1u, std::memory_order_relaxed);
                        g_g617FillConstPx.fetch_add(g617Px, std::memory_order_relaxed);
                    }
                    if (g617ZWrite)
                        g_g617FillZDraws.fetch_add(1u, std::memory_order_relaxed);
                    g617Report();
                }
                return;
            }
        }
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
            {
                if (!spriteZPass(x, y)) // G203: guest Z test
                    continue;
                const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);
                g219SpritePixelProbe(ctx, gs, x, y, ur, ug, ub, a, 0);
                if (alphaTest.writeFramebuffer)
                    writePixel(gs, x, y, ur, ug, ub, a);
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
    // G474: split the remaining frame-boundary flush wall time. Diagnostic only; the default path
    // performs one cached branch and no clock reads. This function is owned by one GS worker.
    static const bool s_g474Boundary = std::getenv("DC2_G474_BOUNDARY") != nullptr;
    const auto g474Start = s_g474Boundary ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    auto g474Last = g474Start;
    uint64_t g474StageNs[6]{};
    const auto g474Lap = [&](unsigned stage) {
        if (!s_g474Boundary)
            return;
        const auto now = std::chrono::steady_clock::now();
        g474StageNs[stage] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - g474Last).count());
        g474Last = now;
    };
    g324NoteObserver(2u); // frame/presentation boundary
    // G305: fence any trailing async packet at the frame boundary (before present), even when the
    // deferred list is empty because the last flush submitted async. Under the default MTGS+pipeline
    // config this runs on the same GS-worker thread as the mid-frame flushes (see g150_frame_barrier
    // note below), so the pending state is single-threaded.
    if (g305DrainRequired())
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
    g474Lap(0); // observer/drain/reports/pending graph/trailing list
    // Conservative phase-1 ownership boundary: frame presentation still sees color readback as
    // before, and guest VRAM leaves mgEndFrame with real Z materialized.  Once route canaries prove
    // every downstream reader has an explicit barrier this can be profiled as a removable fence.
    if (g_g144LastVram)
    {
        // Normal downstream presentation and dumps read guest VRAM. This fence is the display
        // color ownership boundary: at most one readback per dirty display target per frame.
        // G337 (default-off): the boundary Z publish is WAR-dead on the steady route (G336 —
        // the next frame's Z-clear overwrites the published pages in ~0.04 ms, and presentation
        // reads color only). Under the arm the pending owners carry across the frame; every
        // consumer edge (cause 0/3/4/6/7/8 + ranged gates) still publishes lazily, fail-closed.
        if (!g337ZAuthOn())
            (void)g278FlushPendingDepth(5); // never cross presentation/frame ownership
        else
            g_g337Carried.fetch_add(1u, std::memory_order_relaxed);
        g474Lap(1); // depth publication
        // G289's measured 0x14a work owner is not itself a display surface. Let it cross this
        // boundary only in the exact post-upload state; copyFrameToHostRgbaUnlocked audits every
        // actual display/preferred/fallback source before reading. Raw-VRAM diagnostics retain the
        // old all-authoritative boundary.
        // G346: the carry test is per-slot — only the measured exact post-upload 0x14a state may
        // cross the presentation boundary; every other active slot publishes here (closure-off at
        // most slot 0 is active, so this is the original single-owner decision).
        bool g289AnyCarried = false;
        for (uint32_t g289Si = 0u; g289Si < kG345OwnerSlots; ++g289Si)
        {
            G289CopyOwner &g289Slot = s_g289Owners[g289Si];
            if (!g289Slot.active)
                continue;
            const bool g289FrameCarry =
                g289Slot.dstFbp == 0x14au &&
                g289Slot.firstOwnedBlock >= 0x2b20u &&
                g289Slot.firstOwnedBlock <= g289Slot.lastOwnedBlock &&
                g289Slot.lastOwnedBlock == 0x363fu &&
                !g289RawVramDiagOn();
            if (g289FrameCarry)
                g289AnyCarried = true;
            else
                (void)g345MaterializeSlot(g289Slot, 5);
        }
        if (g289AnyCarried)
            g_g289FrameCarries.fetch_add(1u, std::memory_order_relaxed);
        g474Lap(2); // G289/G345 owner-slot publication
        g272MaterializeAll(5);
        g474Lap(3); // remaining resident colour owners
        g277CensusHardEdge(5);
        g276FlushPendingDisplay(5); // G276: publish the frame's coalesced display readback here
        g474Lap(4); // display publication
        g242PrepareVramReadAll(g_g144LastVram);
        g474Lap(5); // private/persistent depth read preparation
        if (g345StatOn())
        {
            static uint64_t s_g345Frames = 0; // frame-boundary owner thread only
            if ((++s_g345Frames % 120u) == 0u)
            {
                g345PrintEscape();
                g347PrintUp139();
                g350PrintOwnCensus();
                g351PrintCon();
            }
        }
    }
    if (s_g474Boundary)
    {
        static uint64_t s_g474Frames = 0u;
        static uint64_t s_g474Ns[7]{};
        ++s_g474Frames;
        for (unsigned i = 0; i < 6; ++i)
            s_g474Ns[i] += g474StageNs[i];
        s_g474Ns[6] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g474Start).count());
        if ((s_g474Frames % 30u) == 0u)
        {
            std::fprintf(stderr,
                         "[G474:boundary] frames=30 total=%.3f pre=%.3f depth=%.3f owners=%.3f "
                         "resident=%.3f display=%.3f prepare=%.3f ms/f\n",
                         static_cast<double>(s_g474Ns[6]) / 30.0e6,
                         static_cast<double>(s_g474Ns[0]) / 30.0e6,
                         static_cast<double>(s_g474Ns[1]) / 30.0e6,
                         static_cast<double>(s_g474Ns[2]) / 30.0e6,
                         static_cast<double>(s_g474Ns[3]) / 30.0e6,
                         static_cast<double>(s_g474Ns[4]) / 30.0e6,
                         static_cast<double>(s_g474Ns[5]) / 30.0e6);
            for (uint64_t &ns : s_g474Ns)
                ns = 0u;
        }
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
        // G656 P9: the exclusive stage split of this edge. `[G313:seg]` proves the whole edge is
        // 95.9% of the `lavaboss` IMAGE layer, but an inclusive timer around a flush measures the
        // work it DRAINS (G429's law), so the WORK/WAIT question is answered here, per stage.
        G656UpLap g656Up(g656DiagOn());
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
        G656_UP_LAP(kG656UpG289Mat);
        G314EdgeScope g314Edge;
        if (g260HasPending())
        {
            // G429 CEILING PROBE (opt-in, DEFAULT-OFF, KNOWINGLY INCORRECT OUTPUT).
            // The upload-forced graph flush is the largest single GS-worker item on lean MAP-0
            // ([G146:g144] upload=21-23 ms/f over ~10 executes/frame). Before designing a
            // texture-version mechanism that would legitimately break this edge, price the
            // ceiling: suppress the WAR/WAW execute entirely and read the frame. This arm
            // renders wrong (pending draws sample post-upload bytes) and must never be promoted.
            static const bool s_g429NoUploadEdge = envFlagEnabled("DC2_G429_NO_UPLOAD_EDGE");
            const bool needsExecute =
                !s_g429NoUploadEdge &&
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
        G656_UP_LAP(kG656UpExec);
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
        // G632: census the declining conjuncts here (and only here — the hook inside
        // g630UploadCompleteInternal re-tests the same predicate and would double-count).
        // G634b: `g634CanIngestUpload` is the SAME predicate the upload-complete hook consults, so
        // publication is skipped here exactly when the hook will mirror the transfer into the raw
        // image instead. It must be a call, not a paraphrase (G629's law) — the two decisions
        // being identical is what makes skipping safe.
        const bool g630UploadPatch =
            g630CanPatchUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, dstUnknown, true) ||
            g634CanIngestUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, dstUnknown);
        const uint64_t g327NoteNs = g327CensusOn()
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - g327T0).count())
            : 0u;
        G656_UP_LAP(kG656UpG264);
        (void)g278FlushPendingDepthForRanges(
            7, G144BlockRange{}, dst, false, dstUnknown);
        G656_UP_LAP(kG656UpG278Dep);
        if (g327CensusOn())
            g327T0 = std::chrono::steady_clock::now();
        g261MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, kG261MatUpload,
                                 g264Mask, g630UploadPatch);
        G656_UP_LAP(kG656UpG261Mat);
        const uint64_t g327MatNs = g327CensusOn()
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - g327T0).count())
            : 0u;
        g327RecordUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                         g327OverlapMask, g264Mask, g327NoteNs, g327MatNs);
        (void)g278FlushPendingDepthForDisplayOwners(7);
        G656_UP_LAP(kG656UpG278Disp);
        g272MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, 7);
        G656_UP_LAP(kG656UpG272Mat);
        g277CensusRangeEdge(7, G144BlockRange{}, dst, false, dstUnknown);
        // G276/G284: publish deferred display readbacks before this l2h upload edge — unless G284
        // proves the upload dst is disjoint from the pending display rows (then keep it coalesced).
        g276FlushPendingDisplayForRanges(7, G144BlockRange{}, dst, false, dstUnknown);
        G656_UP_LAP(kG656UpG276Disp);
        g314FinishEdge(g314Edge, g264Mask, dbp, dbw, dpsm,
                       dsax, dsay, rrw, rrh);
#if defined(PS2X_G656_DIAG)
        if (g656DiagOn())
        {
            g656Up.lap(kG656UpTail);
            g656ReportUpload();
        }
#endif
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

// ===== G432: local->local flush-edge split (behaviour-pure, DC2_G432_CENSUS=1) =====
// The G432 TRXDIR census measured the dir=2 (local->local) A+D write at ~4.6 calls/frame and found
// 94% of its cost inside this flush, not in the pixel copy. G429's law says an inclusive timer
// around a FLUSH measures the work it DRAINS, so the flush total alone cannot say whether there is
// anything to win here. This split separates:
//   mat  : g289MaterializeForRanges — the ownership pin (GPU->VRAM round-trips)
//   exec : g260ExecuteAll — the deferred native-renderer command graph, i.e. CONSERVED draw work
//   n    : exec / skip (needsExecute said no) / idle (nothing pending at all)
// A large `exec` is not a lever. A large `mat`, or a large residue (flushTotal - mat - exec), is.
static bool g432L2lEdgeCensusOn()
{
    static const bool v = envFlagEnabled("DC2_G432_CENSUS");
    return v;
}
static uint64_t g_g432EdgeMatNs = 0, g_g432EdgeExecNs = 0;
static uint64_t g_g432EdgeExecN = 0, g_g432EdgeSkipN = 0, g_g432EdgeIdleN = 0;
static uint64_t g_g432EdgeReports = 0;
// Post-execute publication stages, in call order.
static uint64_t g_g432EdgeStageNs[8] = {};
static const char *const kG432EdgeStageNames[8] = {
    "g266note", "g278depth", "g285cons", "g261mat",
    "g278disp", "g272mat", "g277cen", "g276disp"};
// Sequential lap timer: one clock read per stage boundary, charged to the caller's slot.
struct G432EdgeStage
{
    bool on;
    std::chrono::steady_clock::time_point t;
    explicit G432EdgeStage(bool armed) : on(armed)
    {
        if (on)
            t = std::chrono::steady_clock::now();
    }
    void lap(uint64_t &slot)
    {
        if (!on)
            return;
        const auto now = std::chrono::steady_clock::now();
        slot += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
        t = now;
    }
};
static void g432ReportL2lEdge()
{
    if ((++g_g432EdgeReports % 600u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G432:l2ledge] edges=%llu matMs=%.1f execMs=%.1f exec=%llu skip=%llu idle=%llu",
                 static_cast<unsigned long long>(g_g432EdgeReports),
                 static_cast<double>(g_g432EdgeMatNs) / 1.0e6,
                 static_cast<double>(g_g432EdgeExecNs) / 1.0e6,
                 static_cast<unsigned long long>(g_g432EdgeExecN),
                 static_cast<unsigned long long>(g_g432EdgeSkipN),
                 static_cast<unsigned long long>(g_g432EdgeIdleN));
    for (int i = 0; i < 8; ++i)
        std::fprintf(stderr, " %s=%.1f", kG432EdgeStageNames[i],
                     static_cast<double>(g_g432EdgeStageNs[i]) / 1.0e6);
    std::fprintf(stderr, "\n");
}

// G522 fail-safe state. `armed` is true only between the skipped publication and the caller's
// g144L2lLatePin() call, and only on the edge that skipped — every other path through
// g144FlushPendingLocalTransferRange clears it at entry, so a stale replay is impossible.
struct G522PinDeferred
{
    bool armed = false;
    uint32_t sbp = 0u, ssax = 0u, ssay = 0u, dbp = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint8_t sbw = 0u, spsm = 0u, dbw = 0u, dpsm = 0u;
    G144BlockRange src{}, dst{};
    bool srcUnknown = false, dstUnknown = false;
};
static thread_local G522PinDeferred t_g522Pin;

// Called by GS::performLocalToLocalTransfer immediately after g289TryGpuLocalCopy returns FALSE.
// A no-op unless this edge skipped its publication on the promise of an FBO->FBO blit that did not
// happen; then it runs the two skipped calls verbatim, before the CPU pixel copy reads VRAM.
void g144L2lLatePin()
{
    if (!t_g522Pin.armed)
        return;
    t_g522Pin.armed = false;
    g_g522Late.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t consumed = g285PrepareL2lConsume(
        t_g522Pin.sbp, t_g522Pin.sbw, t_g522Pin.spsm, t_g522Pin.ssax, t_g522Pin.ssay,
        t_g522Pin.dbp, t_g522Pin.dbw, t_g522Pin.dpsm, t_g522Pin.dsax, t_g522Pin.dsay,
        t_g522Pin.rrw, t_g522Pin.rrh, t_g522Pin.src, t_g522Pin.dst,
        t_g522Pin.srcUnknown, t_g522Pin.dstUnknown);
    g261MaterializeForRanges(t_g522Pin.src, t_g522Pin.dst, t_g522Pin.srcUnknown,
                             t_g522Pin.dstUnknown, kG261MatLocal, consumed);
}

// G627: called by GS::performLocalToLocalTransfer immediately after the CPU pixel body, which is
// where guest VRAM first holds the copy's result. A no-op unless this edge took the destination
// publication off the l2l pin. Global-linkage wrapper for the same reason g144L2lLatePin is one:
// the implementation lives in this TU's anonymous namespace and the caller is another TU.
void g627ApplyPendingL2lPatch(uint8_t *vram, uint32_t vramSize)
{
    g627ApplyPendingL2lPatchImpl(vram, vramSize);
}

// The copy did not happen on the CPU after all (an FBO->FBO blit took it). Nothing wrote the
// destination bytes into guest VRAM, so there is nothing to reconcile — drop the pin.
void g627DiscardPendingL2lPatch()
{
    g627DisarmPin();
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
    t_g522Pin.armed = false; // G522: every path below either re-arms it or leaves it disarmed
    g627DisarmPin();         // G627: same contract — only g627TryTakeL2lDest below may re-arm it
    // G434 workload invariant: count every l2l edge on BOTH arms of the deletion probe below.
    {
        extern std::atomic<uint64_t> g_g434L2lEdges;
        static const bool s_g434Inv = (std::getenv("DC2_G434_INV") != nullptr);
        if (s_g434Inv)
            g_g434L2lEdges.fetch_add(1u, std::memory_order_relaxed);
    }
    // G434 CEILING PROBE (DC2_G434_NO_L2LPIN=1, default-off, knowingly WRONG output): delete the
    // ENTIRE local→local dependency-pin edge — g289/g261/g272 materialization, g285 consume,
    // g278 depth flush, g276 display publication and the g260 graph drain. G432 attributed
    // ~5.3 ms/f (g261MaterializeForRanges) + ~2.3 ms/f (g285PrepareL2lConsume) of GS FRONT-thread
    // time here. Prices "what if the whole pin cost were zero". The pixel copy itself is done by
    // the caller and is unaffected.
    {
        static const bool s_g434NoL2lPin = (std::getenv("DC2_G434_NO_L2LPIN") != nullptr);
        if (s_g434NoL2lPin)
            return;
    }
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
        // G343 (behavior-pure, default-off): attribute this l2l edge's materialize/publication
        // time + resident-row demand to the copy shape. Rows estimated BEFORE any materialize
        // clears dirty state; spans exclude the g260 graph execution (draw work, not pin cost).
        const bool g343On = g343CensusOn();
        uint64_t g343SrcRows = 0, g343DstRows = 0, g343FcRows = 0;
        uint32_t g343Unknown = 0, g343HitFbp = 0;
        uint64_t g343MatNs = 0;
        std::chrono::steady_clock::time_point g343T0;
        if (g343On)
        {
            g343EstimateRows(src, dst, srcUnknown, dstUnknown, dbp, dbw, dpsm, rrw, rrh,
                             g343SrcRows, g343DstRows, g343FcRows, g343Unknown, g343HitFbp);
            g343T0 = std::chrono::steady_clock::now();
        }
        // G432: the l2l TRXDIR bucket is 94% "flush", and the flush is this function. Split it
        // once more so the conserved graph-drain (g260ExecuteAll) is separated from the pin cost
        // around it. Behaviour-pure, armed by DC2_G432_CENSUS.
        const bool g432On = g432L2lEdgeCensusOn();
        std::chrono::steady_clock::time_point g432T0;
        if (g432On)
            g432T0 = std::chrono::steady_clock::now();
        g289MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        if (g432On)
        {
            g_g432EdgeMatNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g432T0).count());
        }
        if (g343On)
            g343MatNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g343T0).count());
        if (g260HasPending())
        {
            if (g260VramEventNeedsExecute(src, dst, srcUnknown, dstUnknown))
            {
                g178CensusScan();
                G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
                static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
                if (g432On)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    g260ExecuteAll(kG260LocalCopy, !s_seqUpload260);
                    g_g432EdgeExecNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0).count());
                    ++g_g432EdgeExecN;
                }
                else
                {
                    g260ExecuteAll(kG260LocalCopy, !s_seqUpload260);
                }
            }
            else
            {
                ++g_g260UploadSkips;
                if (g432On)
                    ++g_g432EdgeSkipN;
            }
        }
        else if (g432On)
        {
            ++g_g432EdgeIdleN;
        }
        const bool g455DisplayWillDefer = g455RunwayOn() && g289CanDeferLocalCopy(
            sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
        g455ArmLocalEdge(src, dst, srcUnknown, dstUnknown, g455DisplayWillDefer);
        // G261: the local→local copy READS src (must observe wave-rendered pixels) and WRITES
        // dst — both must see real VRAM bytes for any GPU-resident RTT row they touch.
        G432EdgeStage g432s(g432On);
        g266NoteLocalCopy(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                          src, dst, srcUnknown, dstUnknown);
        g432s.lap(g_g432EdgeStageNs[0]);
        if (g343On)
            g343T0 = std::chrono::steady_clock::now();
        (void)g278FlushPendingDepthForRanges(
            8, src, dst, srcUnknown, dstUnknown);
        g432s.lap(g_g432EdgeStageNs[1]);
        // ===== G522: skip the guest-VRAM PUBLICATION when the blit will stay on the GPU =====
        // g285PrepareL2lConsume and g261MaterializeForRanges both end in g261Materialize — a
        // blocking glReadPixels of the target's dirty rows into guest VRAM — and together they own
        // 4.73 of this edge's 4.90 ms/f (§3). They exist so the CPU pixel copy below can READ real
        // VRAM bytes. When g289CanDeferLocalCopy holds, performLocalToLocalTransfer takes the
        // FBO->FBO backend blit and returns at `if (g289GpuCopy) return;` WITHOUT reading or
        // writing one guest-VRAM byte, so the publication is unobservable to this transfer.
        // Fail-safe: g144L2lLatePin() below replays exactly these two calls if the blit is not
        // taken after all, so the skip can only REORDER a publication, never delete a needed one.
        const bool g522Skip = g522L2lSkipActive() &&
                              g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                                                    dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
        g522NoteEdge(g522Skip);
        t_g522Pin.armed = g522Skip;
        if (g522Skip)
        {
            t_g522Pin.sbp = sbp; t_g522Pin.sbw = sbw; t_g522Pin.spsm = spsm;
            t_g522Pin.ssax = ssax; t_g522Pin.ssay = ssay;
            t_g522Pin.dbp = dbp; t_g522Pin.dbw = dbw; t_g522Pin.dpsm = dpsm;
            t_g522Pin.dsax = dsax; t_g522Pin.dsay = dsay;
            t_g522Pin.rrw = rrw; t_g522Pin.rrh = rrh;
            t_g522Pin.src = src; t_g522Pin.dst = dst;
            t_g522Pin.srcUnknown = srcUnknown; t_g522Pin.dstUnknown = dstUnknown;
        }
        // G627 census (behaviour-pure, DC2_G627_CENSUS=1): the PSMT8-in-CT32 destination class.
        // Runs HERE, immediately before the two calls that clear r.dirty, so it reads the same
        // residency state the materialize is about to consume.
        if (g627CensusOn())
            g627NoteCensus(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay,
                           rrw, rrh, src, dst, srcUnknown, dstUnknown);
        // ===== G627: take the DESTINATION-triggered publication off this edge =====
        // A PSMT8 copy whose destination lands inside a dirty resident CT32 target publishes the
        // whole dirty window (448 rows x 512 px on `ridepod`) to service 5,760 bytes it is about to
        // overwrite. When every conjunct in g627TryTakeL2lDest holds, the returned targets are
        // reconciled by a per-byte-lane VRAM->FBO patch after the copy body instead
        // (g627ApplyPendingL2lPatch), and their kG261MatLocal materialize is skipped here.
        const uint32_t g627Mask =
            g522Skip ? 0u
                     : g627TryTakeL2lDest(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm,
                                          dsax, dsay, rrw, rrh, src, dst,
                                          srcUnknown, dstUnknown);
        const uint32_t g285Consumed =
            g522Skip ? 0u
                     : g285PrepareL2lConsume(
                           sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                           src, dst, srcUnknown, dstUnknown);
        g432s.lap(g_g432EdgeStageNs[2]);
        if (!g522Skip)
            g261MaterializeForRanges(src, dst, srcUnknown, dstUnknown, kG261MatLocal,
                                     g285Consumed | g627Mask);
        g432s.lap(g_g432EdgeStageNs[3]);
        if (g343On)
        {
            g343MatNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g343T0).count());
            g343T0 = std::chrono::steady_clock::now();
        }
        (void)g278FlushPendingDepthForDisplayOwners(8);
        g432s.lap(g_g432EdgeStageNs[4]);
        g272MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        g432s.lap(g_g432EdgeStageNs[5]);
        g277CensusRangeEdge(8, src, dst, srcUnknown, dstUnknown);
        g432s.lap(g_g432EdgeStageNs[6]);
        // G276/G284: publish deferred display readbacks before this l2l edge — unless G284 proves the
        // copy src+dst are both disjoint from the pending display rows (then keep it coalesced).
        if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                                    dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
            g276FlushPendingDisplayForRanges(8, src, dst, srcUnknown, dstUnknown);
        g432s.lap(g_g432EdgeStageNs[7]);
        if (g343On)
        {
            const uint64_t g343PubNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g343T0).count());
            g343NoteL2l(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay,
                        rrw, rrh, g343SrcRows, g343DstRows, g343FcRows,
                        g343Unknown, g343HitFbp, g343MatNs, g343PubNs);
        }
        if (g432On)
            g432ReportL2lEdge();
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
