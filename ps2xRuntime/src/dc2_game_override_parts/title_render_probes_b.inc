static void g78_frame_clipinbox_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137F0Cu);
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);

    mgClipInBoxW__FPfPfPfPf_0x12f380(rdram, ctx, runtime);

    // G81: re-apply the title camera setup if it is stuck at ctor defaults (self-guarded, one-shot).
    g81_fix_title_camera(rdram, ctx);

    // G88: one-shot dump of the mgRenderInfo VIEWPORT/guard block (0x380ec0 + float idx 0x3a8..0x3c7)
    // to A/B the projection SCALE vs HW (HW vp[0x3b8]=(256,208), guard ֲ±2047). The title rock streaks
    // = geometry fanned to the guard band ֲ±2047 => the runner's NDC->screen scale or perspective
    // divide diverges from HW even though lightInfo (+0x400) is byte-identical. DC2_G88_VP, title scope.
    if (std::getenv("DC2_G88_VP") != nullptr && g67_title_scope(rdram, ctx))
    {
        static bool s_done = false;
        if (!s_done)
        {
            s_done = true;
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            const uint32_t base = 0x00380ec0u;
            std::fprintf(stderr, "[G88:vp] idx@+0x3f4=%u\n", dc2_read_u32(rdram, base + 0x3f4u));
            for (uint32_t i = 0x3a8u; i <= 0x3c7u; i += 4u)
                std::fprintf(stderr, "[G88:vp]  vp[0x%x]: %12.4f %12.4f %12.4f %12.4f\n",
                    i, rf(base + i * 4u), rf(base + (i + 1) * 4u), rf(base + (i + 2) * 4u), rf(base + (i + 3) * 4u));
            std::fflush(stderr);
        }
    }

    // G92 Phase C: one-shot dump of the title transform projection matrix CHAIN in mgRENDER_INFO
    // (base 0x380ec0) so we can A/B it BYTE-for-byte against the live HW reference captured from
    // PCSX2 at the New Game title (phase-G92C-fix-log.md). These matrices are built by
    // SetRenderInfo__13mgRENDER_INFO@0x138b00 (real recompiled body) and consumed by
    // CreateRenderInfoPacket__12mgCVisualMDT@0x1404d0 to build the VU MVP = (View*Proj @ +0x10) * LW.
    // If any Y row diverges from HW => the EE matrix math (sceVu0* helpers / FPU) is the bug; if they
    // match => the divergence is in the per-draw LW combine / VU apply (transform packet). Byte offsets:
    //   +0x10  View*Proj combined (the MVP-no-world the else-branch multiplies by LW)
    //   +0x110 projection (focal 480)   +0x150 view*aspect   +0x1a0 view   +0x260 viewport scale
    // DC2_G92_MTX, title scope.
    if (std::getenv("DC2_G92_MTX") != nullptr && g67_title_scope(rdram, ctx))
    {
        // G93: dump at a LATER (steady-state) frame, not frame-1 (the title camera is
        // still at ctor defaults on frame 1). Fire once around the 400th title-clip call.
        static std::atomic<uint32_t> s_mtxN{0};
        const uint32_t mn = s_mtxN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const char *mf = std::getenv("DC2_G92_MTX_FRAME");
        const uint32_t target = mf ? (uint32_t)std::atoi(mf) : 400u;
        static bool s_done = false;
        if (!s_done && mn >= target)
        {
            s_done = true;
            const uint32_t base = 0x00380ec0u;
            auto dumpMat = [&](uint32_t off, const char *label) {
                std::fprintf(stderr, "[G92:mtx] --- %s (base+0x%x) ---\n", label, off);
                for (uint32_t r = 0u; r < 4u; ++r)
                {
                    float f[4];
                    for (uint32_t c = 0u; c < 4u; ++c)
                    {
                        uint32_t u = dc2_read_u32(rdram, base + off + (r * 4u + c) * 4u);
                        std::memcpy(&f[c], &u, 4);
                    }
                    std::fprintf(stderr, "[G92:mtx]   %12.4f %12.4f %12.4f %12.4f\n", f[0], f[1], f[2], f[3]);
                }
            };
            std::fprintf(stderr, "[G92:mtx] fcc=0x%x fcc+0x8c=%.4f fc4=0x%x fc0=0x%x idx@+0x3f4=%u\n",
                         dc2_read_u32(rdram, base + 0xfccu),
                         [&]{ uint32_t u = dc2_read_u32(rdram, dc2_read_u32(rdram, base + 0xfccu) + 0x8cu); float f; std::memcpy(&f,&u,4); return f; }(),
                         dc2_read_u32(rdram, base + 0xfc4u), dc2_read_u32(rdram, base + 0xfc0u),
                         dc2_read_u32(rdram, base + 0x3f4u));
            dumpMat(0x10u,  "View*Proj combined +0x10");
            dumpMat(0x110u, "projection +0x110");
            dumpMat(0x150u, "view*aspect +0x150");
            dumpMat(0x1a0u, "view +0x1a0");
            dumpMat(0x260u, "viewport scale +0x260");
            std::fflush(stderr);
        }
    }

    // G92 Phase C VALIDATION lever (NOT a shipped fix): overwrite the title renderinfo projection/
    // view matrix chain with the live HW reference captured from PCSX2 at the New Game menu. Runs in
    // the title draw chain (mgClipInBoxW, after mgSetRenderInfo, before CreateRenderInfoPacket reads
    // renderinfo) so the transform packets are built with the HW camera. If the transform path's
    // off-screen (7.4%) coverage jumps toward HW (~48%) under this lever, it PROVES the off-screen
    // projection is the wrong renderinfo camera (runner focal 800 + camera at origin) vs HW
    // (focal 480 + camera 661 back), not a viewport/VU-apply bug. DC2_G92_FORCEMTX, title scope.
    if (std::getenv("DC2_G92_FORCEMTX") != nullptr && g67_title_scope(rdram, ctx))
    {
        const uint32_t base = 0x00380ec0u;
        auto wf = [&](uint32_t off, float v) { uint32_t u; std::memcpy(&u, &v, 4); dc2_write_u32(rdram, base + off, u); };
        auto wmat = [&](uint32_t off, const float m[16]) { for (uint32_t i = 0u; i < 16u; ++i) wf(off + i * 4u, m[i]); };
        static const float mvp[16] = {  // +0x10 View*Proj combined (HW)
            480.0f, 0.0f, 0.0f, 0.0f,
            366.2069f, -184.7677f, -298.6458f, 0.1788f,
            -2014.9930f, -2115.1277f, 1643.2494f, -0.9839f,
            1388447.375f, 1460146.625f, 49000152.0f, 661.5232f };
        static const float proj[16] = { // +0x110 projection (HW, focal 480)
            480.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 480.0f, 0.0f, 0.0f,
            2048.0f, 2048.0f, -1670.1670f, 1.0f,
            0.0f, 0.0f, 50105008.0f, 0.0f };
        static const float vasp[16] = { // +0x150 view*aspect (HW)
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, -1.1479f, 0.1788f, 0.0f,
            0.0f, -0.2086f, -0.9839f, 0.0f,
            70.1f, 219.4734f, 661.5232f, 1.0f };
        static const float view[16] = { // +0x1a0 view (HW, camera 661 back, pitch 10.3deg)
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.9839f, 0.1788f, 0.0f,
            0.0f, -0.1788f, -0.9839f, 0.0f,
            70.1f, 188.1201f, 661.5232f, 1.0f };
        wmat(0x10u, mvp);
        wmat(0x110u, proj);
        wmat(0x150u, vasp);
        wmat(0x1a0u, view);
    }

    // G90: one-shot dump of the title texMgr (0x381ef0) block->texture chains for the drawn
    // blocks (0,1,3,6,7) so we can A/B block->VRAM tbp + texture sizes vs HW (tools/pcsx2tex.py).
    // HW cursor[0]=0x2720; each block reloads contiguously from there by texture size. The runner
    // misses HW's 0x2b20/0x2f20/0x3320 (the 1024-block big rock walls) -> cursor/size math diverges.
    if (std::getenv("DC2_G90_TEXMGR") != nullptr && g67_title_scope(rdram, ctx))
    {
        static bool s_done = false;
        if (!s_done)
        {
            s_done = true;
            const uint32_t TM = 0x00381ef0u;
            const uint32_t cursor = dc2_read_u32(rdram, TM + 0x00u);
            const uint32_t clut = dc2_read_u32(rdram, TM + 0x04u);
            const uint32_t last = dc2_read_u32(rdram, TM + 0x08u);
            const uint32_t cnt = dc2_read_u32(rdram, TM + 0x0Cu);
            const uint32_t arr = dc2_read_u32(rdram, TM + 0x10u);
            std::fprintf(stderr, "[G90:texmgr] cursor=0x%x clut=0x%x last=%d count=%u arr=0x%x\n",
                         cursor, clut, (int32_t)last, cnt, arr);
            const int blocks[5] = {0, 1, 3, 6, 7};
            for (int bi = 0; bi < 5; ++bi)
            {
                const int blk = blocks[bi];
                const uint32_t rec = arr + (uint32_t)blk * 0x10u;
                uint32_t t = dc2_read_u32(rdram, rec + 8u);
                std::fprintf(stderr, "[G90:texmgr] block %d rec=0x%x head=0x%x\n", blk, rec, t);
                for (int i = 0; t != 0u && i < 12; ++i)
                {
                    const int16_t w = (int16_t)(dc2_read_u32(rdram, t + 0x0u) >> 16); // +2 (hword in low word? read raw)
                    const uint32_t wh = dc2_read_u32(rdram, t + 0x00u); // [+0]=?,[+2]=w,[+4]=h... read +0x00 word then hwords
                    const uint16_t ww = (uint16_t)(dc2_read_u32(rdram, t + 0x00u) >> 16);
                    const uint16_t hh = (uint16_t)(dc2_read_u32(rdram, t + 0x04u) & 0xFFFFu);
                    const uint16_t fmt = (uint16_t)(dc2_read_u32(rdram, t + 0x04u) >> 16);
                    const uint32_t tex0lo = dc2_read_u32(rdram, t + 0x38u);
                    const uint32_t tbp = tex0lo & 0x3FFFu;
                    const uint32_t tbw = (tex0lo >> 14) & 0x3Fu;
                    const uint32_t psm = (tex0lo >> 20) & 0x3Fu;
                    const int32_t size = (int32_t)dc2_read_u32(rdram, t + 0x28u);
                    const uint32_t nxt = dc2_read_u32(rdram, t + 0x68u);
                    std::fprintf(stderr, "[G90:texmgr]   tex=0x%x wh=(%u,%u) fmt=%u tbp=0x%x tbw=%u psm=0x%x size=%d next=0x%x\n",
                                 t, ww, hh, fmt, tbp, tbw, psm, size, nxt);
                    (void)w; (void)wh;
                    t = nxt;
                }
            }
            std::fflush(stderr);
        }
    }

    // G81: title-camera A/B vs HW (read on the DOMINANT direct title-draw path; the f48
    // override only sees the early indirect path). HW ref @ New Game menu: phase 0, dist 1198.75,
    // height -214, pos(-70.1,66.8,707.7), ref(-70.1,280.8,-493.0). Reads the camera globals at
    // fixed addresses: TitleCamera ptr @0x377E38 -> distance(+0x90)/height(+0x94)/pos(+0)/ref(+0x10).
    if (std::getenv("DC2_G81_CAM") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_camN{0};
        const uint32_t cn = s_camN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (cn <= 4u || (cn % 400u) == 0u)
        {
            const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u);
            const uint32_t phase = dc2_read_u32(rdram, 0x00377E44u);
            const uint32_t counter = dc2_read_u32(rdram, 0x00377E48u);
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            std::fprintf(stderr,
                         "[G81:cam] cn=%u cam=0x%x phase=%u counter=%u dist=%.2f height=%.2f pos=(%.1f,%.1f,%.1f) ref=(%.1f,%.1f,%.1f)\n",
                         cn, cam, phase, counter,
                         cam ? rf(cam + 0x90u) : 0.f, cam ? rf(cam + 0x94u) : 0.f,
                         cam ? rf(cam + 0x00u) : 0.f, cam ? rf(cam + 0x04u) : 0.f, cam ? rf(cam + 0x08u) : 0.f,
                         cam ? rf(cam + 0x10u) : 0.f, cam ? rf(cam + 0x14u) : 0.f, cam ? rf(cam + 0x18u) : 0.f);
            std::fflush(stderr);
        }
    }

    // G82: read the runner's title-map lighting (mgRenderInfo@0x380ec0). The rock renders green
    // because the per-vertex shade is wrong; HW's rock vertex colour == its AMBIENT (46.5,69,61.5)
    // (TitleMapDraw: GetLightInfo(TitleMap) -> *1.5 -> mgSetAmbient). lightInfo = 0x380ec0 +
    // idx*0x150 + 0x400 (idx=*(0x380ec0+0x3f4)); ambient=lightInfo+0x80, lightcol=+0x00, lightdir=+0x40.
    if (std::getenv("DC2_G82_LIGHT") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_litN{0};
        const uint32_t ln = s_litN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (ln <= 3u || (ln % 400u) == 0u)
        {
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            std::fprintf(stderr,
                "[G82:light] ln=%u idx=%u amb=(%.1f,%.1f,%.1f,%.1f) col0=(%.3f,%.3f,%.3f) dir0=(%.1f,%.1f,%.1f) fog=(%.1f,%.1f,%.1f,%.1f)\n",
                ln, idx, rf(li + 0x80u), rf(li + 0x84u), rf(li + 0x88u), rf(li + 0x8Cu),
                rf(li + 0x00u), rf(li + 0x04u), rf(li + 0x08u),
                rf(li + 0x40u), rf(li + 0x44u), rf(li + 0x48u),
                rf(0x00381E9Cu), rf(0x00381EA0u), rf(0x00381EA4u), rf(0x00381EA8u));
            // G84: full light-COLOUR matrix (3 cols) + dir cols 1/2 to compare vs HW
            // (HW: col0=(-0.894,0,0) col1=(-0.447,0,0) col2=0 -> R-only). If the runner's
            // cols carry G/B the matrix is built wrong; if R-only the consumption transposes.
            std::fprintf(stderr,
                "[G84:lightmtx] colR=(%.3f,%.3f,%.3f) colG=(%.3f,%.3f,%.3f) colB=(%.3f,%.3f,%.3f) | c0=(%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f) c2=(%.3f,%.3f,%.3f) | d1=(%.1f,%.1f,%.1f) d2=(%.1f,%.1f,%.1f)\n",
                rf(li + 0x00u), rf(li + 0x10u), rf(li + 0x20u),
                rf(li + 0x04u), rf(li + 0x14u), rf(li + 0x24u),
                rf(li + 0x08u), rf(li + 0x18u), rf(li + 0x28u),
                rf(li + 0x00u), rf(li + 0x04u), rf(li + 0x08u),
                rf(li + 0x10u), rf(li + 0x14u), rf(li + 0x18u),
                rf(li + 0x20u), rf(li + 0x24u), rf(li + 0x28u),
                rf(li + 0x50u), rf(li + 0x54u), rf(li + 0x58u),
                rf(li + 0x60u), rf(li + 0x64u), rf(li + 0x68u));
            std::fflush(stderr);
        }
    }

    // G84: ambient-perturbation mechanism test (diagnostic; env-gated, off by default).
    // Scale the stored title-map ambient RGB (lightInfo+0x80) by DC2_G84_AMBSCALE just before
    // the per-part render packet is built (mgClipInBoxW runs in Draw__8mgCFrame, upstream of
    // Draw__12mgCVisualMDT/CreateRenderInfoPacket). Then watch [G83:rgba]: if the VU-output
    // colour scales with this -> the colour is (re)computed from this ambient downstream
    // (ambient double-apply / wrong consume); if it does NOT move -> colour is baked upstream.
    {
        static const char *s_ambEnv = std::getenv("DC2_G84_AMBSCALE");
        static const float s_ambScale = s_ambEnv ? std::strtof(s_ambEnv, nullptr) : 0.0f;
        if (s_ambScale > 0.0f && g67_title_scope(rdram, ctx))
        {
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            auto scaleF = [&](uint32_t a) {
                uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4);
                f *= s_ambScale; std::memcpy(&u, &f, 4); dc2_write_u32(rdram, a, u);
            };
            scaleF(li + 0x80u); scaleF(li + 0x84u); scaleF(li + 0x88u);
        }
    }

    // G84: directional-light isolation. Scale the whole light-COLOUR matrix (lightInfo +0x00..0x2F,
    // 3 columns) by DC2_G84_LITSCALE. LITSCALE=0 -> ambient-only; if the green G/B vanishes the
    // directional term is the inflation source, if it persists the per-vertex model colour is.
    {
        static const char *s_litEnv = std::getenv("DC2_G84_LITSCALE");
        static const bool s_litSet = (s_litEnv != nullptr);
        static const float s_litScale = s_litEnv ? std::strtof(s_litEnv, nullptr) : 1.0f;
        if (s_litSet && g67_title_scope(rdram, ctx))
        {
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            auto scaleF = [&](uint32_t a) {
                uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4);
                f *= s_litScale; std::memcpy(&u, &f, 4); dc2_write_u32(rdram, a, u);
            };
            for (uint32_t o = 0u; o < 0x30u; o += 4u) scaleF(li + o);
        }
    }

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    uint32_t ret = getRegU32(ctx, 2);
    uint32_t fixed = 0u;
    // G79 NOTE: broadening this from the single title rock frame (0x981390) to EVERY title map
    // frame (`|| g67_title_scope(rdram,ctx)`) was tried to fix the costume->title round-trip
    // flat-blue and did NOT help ג€” proving the round-trip regression is NOT this routing (the
    // title-map scene isn't restored at all post-costume: `g67_title_scope` is false because the
    // titleMap pointer 0x00377E34 != 0x008FC880). Reverted to the narrow, more-correct scope
    // (only the known-visible rock frame; broadening would also force genuinely off-screen parts
    // to draw, which HW culls). The round-trip needs a scene-restoration fix (see phase-G79).
    if (g78_copy_fix_enabled() && scope && scope->distFix && ret == 0u)
    {
        setReturnU32(ctx, 1u);
        ret = 1u;
        fixed = 1u;
    }
    if (!g67_trace_enabled())
        return;
    const uint32_t n = g_g78_clipinbox.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = scope && g67_focus_frame(scope->frame, scope->f8);
    if (focus || ret == 0u || n <= 80u || (n % 240u) == 0u)
    {
        // G79: also log the front-end scene state (loopNo @ gp-0x7524, titleMap @ 0x00377E34)
        // so a post-costume capture shows whether the title-map scene is restored
        // (titleMap should be 0x008FC880; g67_title_scope true only then).
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu;
        const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
        std::fprintf(stderr,
                     "[G78:clipinbox] n=%u frame=0x%x visual=0x%x ra=0x%x ret=%u fixed=%u "
                     "loop=%u titleMap=0x%x a=(0x%x,0x%x,0x%x,0x%x)\n",
                     n, scope ? scope->frame : 0u, scope ? scope->f8 : 0u, ra, ret, fixed,
                     loopNo, titleMap, a0, a1, a2, a3);
        std::fflush(stderr);
    }
}

// G215: EE-side per-frame cull discriminator for the MAP-4 head/cap drop. The character model
// VIF packets are BUILT synchronously on the EE thread inside the COutLineDraw window
// (g_g37_in_outline>0) but VU1-EXECUTED later at the deferred frame flush, so a VU1-side probe
// scoped to that window sees nothing (proven G215). This probe instead observes the EE-side
// traversal Draw__8mgCFrame@0x137e10 does, whose own-mesh emit is gated by mgClipBoxW/test1 — a
// camera-angle-dependent per-frame bbox cull. ret==0 with a non-null visual (f4) at a frame that
// returns >0 from another camera angle => the EE frustum/guard cull dropped it upstream of VU1.
static bool g215_frame_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G215_FRAME");
    return e;
}

// G215: dump the runner's live guard/clip data and compare to the real-HW reference read from
// PCSX2 at MAP-4 (mgRenderInfo idx 0x3a8..: max(2329.6,2276.8) min(1766.4,1819.2) guard(4095,1);
// EE mgClipBoxW: DAT_381da0=(256,208,0,30000) DAT_381db0=(-256,-208,0,3)). A divergence here is a
// guard-data root: the VU1 per-vertex guard would cull on-screen head verts the EE frame test (which
// passes, all frames ret>0) admits. Throttled, default-off (DC2_G215_GUARD).
static void g215_dump_guard(uint8_t *rdram)
{
    static const bool en = dc2_env_flag_enabled("DC2_G215_GUARD");
    if (!en) return;
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
    if (n != 0u && (n % 200u) != 0u) return;
    auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, sizeof(f)); return static_cast<double>(f); };
    const uint32_t ri = dc2_read_u32(rdram, 0x00382144u);
    std::fprintf(stderr, "[G215:guard] n=%u ri=0x%x d381d48=%g\n", n, ri, rf(0x00381d48u));
    std::fprintf(stderr, "[G215:guard]   EEclip max=(%g,%g,%g,%g) min=(%g,%g,%g,%g) band=(%g,%g)/(%g,%g)\n",
                 rf(0x00381da0u), rf(0x00381da4u), rf(0x00381da8u), rf(0x00381dacu),
                 rf(0x00381db0u), rf(0x00381db4u), rf(0x00381db8u), rf(0x00381dbcu),
                 rf(0x00381dc0u), rf(0x00381dc4u), rf(0x00381dd0u), rf(0x00381dd4u));
    if (ri > 0x100000u && ri < 0x2000000u)
    {
        std::fprintf(stderr, "[G215:guard]   VUguard max=(%g,%g,%g,%g) min=(%g,%g,%g,%g) band max=(%g,%g) min=(%g,%g)\n",
                     rf(ri + 0x3a8u * 4u), rf(ri + 0x3a9u * 4u), rf(ri + 0x3aau * 4u), rf(ri + 0x3abu * 4u),
                     rf(ri + 0x3acu * 4u), rf(ri + 0x3adu * 4u), rf(ri + 0x3aeu * 4u), rf(ri + 0x3afu * 4u),
                     rf(ri + 0x3b0u * 4u), rf(ri + 0x3b1u * 4u), rf(ri + 0x3b4u * 4u), rf(ri + 0x3b5u * 4u));
    }
    std::fflush(stderr);
}

static void g67_frame_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const bool fixEnabled = g67_title_dist_fix_enabled();
    const bool title = (trace || fixEnabled) && g67_title_scope(rdram, ctx);
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t f0 = frame ? dc2_read_u32(rdram, frame + 0xF0u) : 0u;
    const uint32_t f4 = frame ? dc2_read_u32(rdram, frame + 0xF4u) : 0u;
    const uint32_t f8 = frame ? dc2_read_u32(rdram, frame + 0xF8u) : 0u;
    const uint32_t desc18 = f4 ? dc2_read_u32(rdram, f4 + 0x18u) : 0u;
    const uint32_t desc40 = f4 ? dc2_read_u32(rdram, f4 + 0x40u) : 0u;
    const uint32_t desc48 = f4 ? dc2_read_u32(rdram, f4 + 0x48u) : 0u;
    const uint32_t child = frame ? dc2_read_u32(rdram, frame + 0x58u) : 0u;
    const bool focus = g67_focus_frame(frame, f8);
    const bool target = g67_title_frame_target(frame, f8);
    const bool traceTrack = trace && (title || focus);
    // G80: broaden the copy/uncull scope from the single G78 target frame to EVERY title-map
    // frame so the block-0 rock visuals (not just block-1 s19_05) route to copy. Opt-in.
    const bool fixTrack = fixEnabled && title && (target || g80_rock_copy_enabled());
    const bool track = traceTrack || fixTrack;
    const uint32_t before1 = track ? g67_mgr_count(rdram, 0x003820E0u, 1u) : 0u;
    const uint32_t before3 = track ? g67_mgr_count(rdram, 0x003820E0u, 3u) : 0u;
    const uint32_t seq = track ? (g_g67_frame.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;

    uint32_t stackIndex = 0xFFFFFFFFu;
    if (track && g_g67_frame_depth < 64u)
    {
        stackIndex = g_g67_frame_depth++;
        G67FrameScope &scope = g_g67_frame_stack[stackIndex];
        std::memset(&scope, 0, sizeof(scope));
        scope.seq = seq;
        scope.frame = frame;
        scope.f0 = f0;
        scope.f4 = f4;
        scope.f8 = f8;
        scope.desc18 = desc18;
        scope.desc40 = desc40;
        scope.desc48 = desc48;
        scope.child = child;
        scope.before1 = before1;
        scope.before3 = before3;
        scope.distFix = fixTrack ? 1u : 0u;
        scope.clipRet = 0xFFFFFFFFu;
    }

    Draw__8mgCFrameFPUi_0x137e10(rdram, ctx, runtime);

    const uint32_t ret = getRegU32(ctx, 2);
    g215_dump_guard(rdram);
    // G215: character-window per-frame emit trace (default-off; needs DC2_G205_FVAR13_TRACE for
    // the outline wrapper that sets g_g37_in_outline). Read-only post-call (safe per G212 audit).
    if (g215_frame_enabled() && g_g37_in_outline.load(std::memory_order_relaxed) > 0)
    {
        static std::atomic<uint32_t> s_g215fseq{0};
        const uint32_t fs = s_g215fseq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G215:frame] seq=%u tick=%llu frame=0x%x f0=0x%x f4=0x%x f8=0x%x child=0x%x ret=0x%x\n",
                     fs, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     frame, f0, f4, f8, child, ret);
        std::fflush(stderr);
    }
    if (!track)
        return;

    G67FrameScope scope{};
    if (stackIndex != 0xFFFFFFFFu && stackIndex < 64u)
    {
        scope = g_g67_frame_stack[stackIndex];
        g_g67_frame_depth = stackIndex;
    }
    else
    {
        scope.seq = seq;
        scope.frame = frame;
        scope.f0 = f0;
        scope.f4 = f4;
        scope.f8 = f8;
        scope.desc18 = desc18;
        scope.desc40 = desc40;
        scope.desc48 = desc48;
        scope.child = child;
        scope.before1 = before1;
        scope.before3 = before3;
    }

    if (!traceTrack)
        return;

    const uint32_t after1 = g67_mgr_count(rdram, 0x003820E0u, 1u);
    const uint32_t after3 = g67_mgr_count(rdram, 0x003820E0u, 3u);
    const bool queued = (after1 != scope.before1) || (after3 != scope.before3);
    const bool logLine = focus || queued || seq <= 180u || (ret == 0u && f4 != 0u);
    if (!logLine)
        return;

    std::fprintf(stderr,
                 "[G67:frame] n=%u depth=%u frame=0x%x child=0x%x f0=0x%x f4=0x%x "
                 "desc18=0x%x desc40=0x%x desc48=0x%x f8=0x%x ret=0x%x reason=%s "
                 "test1=%u fixed=%u dist=% .6g orig=% .6g limit=% .6g clip=%u/%u clipFixed=%u "
                 "counts1=%u->%u counts3=%u->%u\n",
                 seq, stackIndex == 0xFFFFFFFFu ? g_g67_frame_depth : stackIndex,
                 frame, child, f0, f4, desc18, desc40, desc48, f8, ret,
                 g67_frame_reason(scope, ret), scope.test1Seen, scope.test1Fixed,
                 scope.test1Dist, scope.test1OriginalDist, scope.test1Limit,
                 scope.clipSeen, scope.clipRet, scope.clipFixed,
                 scope.before1, after1, scope.before3, after3);
    std::fflush(stderr);
}

static void g67_get_texture_block_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t set = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t buf = getRegU32(ctx, 6);
    const uint32_t max = getRegU32(ctx, 7);
    const uint32_t ra = getRegU32(ctx, 31);

    GetTextureBlockNo__11CMdsListSetFiPii_0x168fd0(rdram, ctx, runtime);

    if (!g67_trace_enabled())
        return;
    const bool titleSet = (set == 0x01DDA630u);
    const bool titleCaller = (ra == 0x002A2598u || ra == 0x002A2644u);
    if (!titleSet && !titleCaller && !g67_title_scope(rdram, ctx))
        return;

    const uint32_t n = g_g67_block.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 160u && (n % 240u) != 0u)
        return;

    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const uint32_t imgCnt = set ? dc2_read_u32(rdram, set + 0x90u) : 0u;
    uint32_t foundEntry = 0xFFFFFFFFu;
    uint32_t foundBase = 0u;
    uint32_t foundCount = 0u;
    const uint32_t scanCount = (imgCnt < 32u) ? imgCnt : 32u;
    if (idx < 32u)
    {
        for (uint32_t i = 0u; i < scanCount; ++i)
        {
            const uint32_t flag = dc2_read_u32(rdram, set + 0x94u + i * 8u);
            const uint32_t tbl = dc2_read_u32(rdram, set + 0x98u + i * 8u);
            if (!flag || !tbl)
                continue;
            const uint32_t base = dc2_read_u32(rdram, tbl + idx * 4u);
            const uint32_t count = dc2_read_u32(rdram, tbl + 0x80u + idx * 4u);
            if (count != 0u || base != 0xFFFFFFFFu)
            {
                foundEntry = i;
                foundBase = base;
                foundCount = count;
                break;
            }
        }
    }

    char line[1024];
    int p = std::snprintf(line, sizeof(line),
        "[G67:block] n=%u set=0x%x idx=%u ret=%d max=%u buf=0x%x ra=0x%x pass=%s imgCnt=%u found=%u base=0x%x cnt=%u",
        n, set, idx, ret, max, buf, ra,
        (ra == 0x002A2598u) ? "0-5" : ((ra == 0x002A2644u) ? "6-15" : "other"),
        imgCnt, foundEntry, foundBase, foundCount);
    g67_append_block_list(line, sizeof(line), &p, rdram, buf, ret, 24);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

static void g67_map_drawsub_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const uint32_t map = getRegU32(ctx, 4);
    const int32_t mode = static_cast<int32_t>(getRegU32(ctx, 5));
    const uint32_t n = trace ? (g_g67_mapdraw.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
    if (trace && (n <= 16u || (n % 120u) == 0u))
    {
        const uint32_t drawCount = map ? dc2_read_u32(rdram, map + 0x360u) : 0u;
        const uint32_t drawList = map ? dc2_read_u32(rdram, map + 0x364u) : 0u;
        const uint32_t flags = map ? dc2_read_u32(rdram, map + 0xCB0u) : 0u;
        char line[1024];
        int p = std::snprintf(line, sizeof(line),
            "[G67:map.drawsub] n=%u map=0x%x mode=%d drawCount=%u drawList=0x%x flags=0x%x parts=",
            n, map, mode, drawCount, drawList, flags);
        const uint32_t lim = (drawCount < 10u) ? drawCount : 10u;
        for (uint32_t i = 0u; i < lim && p < static_cast<int>(sizeof(line)) - 80; ++i)
        {
            const uint32_t part = drawList ? dc2_read_u32(rdram, drawList + i * 4u) : 0u;
            const uint32_t vt = part ? dc2_read_u32(rdram, part) : 0u;
            const uint32_t pflags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
            p += std::snprintf(line + p, sizeof(line) - static_cast<size_t>(p),
                               "%s0x%x(vt=0x%x fl=0x%x)", (i == 0u) ? "" : ",", part, vt, pflags);
        }
        std::fprintf(stderr, "%s\n", line);
        std::fflush(stderr);
    }

    DrawSub__4CMapFi_0x15e250(rdram, ctx, runtime);
}

static void g67_mapparts_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const uint32_t part = getRegU32(ctx, 4);
    const uint32_t before1 = trace ? g67_mgr_count(rdram, 0x003820E0u, 1u) : 0u;
    const uint32_t before3 = trace ? g67_mgr_count(rdram, 0x003820E0u, 3u) : 0u;

    g56_mapparts_draw_tap(rdram, ctx, runtime);

    if (!trace)
        return;
    const uint32_t n = g_g67_partsdraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 80u && (n % 240u) != 0u)
        return;
    const uint32_t vt = part ? dc2_read_u32(rdram, part + 0x00u) : 0u;
    const uint32_t flags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
    const uint32_t lightMtx = part ? dc2_read_u32(rdram, part + 0x2E4u) : 0u;
    const uint32_t plight = part ? dc2_read_u32(rdram, part + 0x2E8u) : 0u;
    const uint32_t lightInfo = part ? dc2_read_u32(rdram, part + 0x300u) : 0u;
    std::fprintf(stderr,
        "[G67:parts.draw] n=%u part=0x%x vt=0x%x flags=0x%x lightMtx=0x%x plight=0x%x lightInfo=0x%x ret=0x%x counts1=%u->%u counts3=%u->%u\n",
        n, part, vt, flags, lightMtx, plight, lightInfo, getRegU32(ctx, 2),
        before1, g67_mgr_count(rdram, 0x003820E0u, 1u),
        before3, g67_mgr_count(rdram, 0x003820E0u, 3u));
    std::fflush(stderr);
}

static void g67_mgenddraw_reload_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const int32_t reqBlock = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t mgrArg = getRegU32(ctx, 5);
    const uint32_t mgr = mgrArg ? mgrArg : 0x003820E0u;
    const uint32_t ra = getRegU32(ctx, 31);

    if (trace)
    {
        const uint32_t n = g_g67_enddraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 200u || (n % 360u) == 0u)
        {
            char line[1400];
            int p = std::snprintf(line, sizeof(line),
                "[G67:end.reload] n=%u mgr=0x%x mgrArg=0x%x ra=0x%x", n, mgr, mgrArg, ra);
            g67_append_mgr_block_summary(line, sizeof(line), &p, rdram, mgr, reqBlock);
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }

    // G90: per-block reload diagnostics ג€” find why blocks 1/3/6/7 never get tbps written.
    // Capture mgr remap + texMgr cache/cursor BEFORE, the recompiled return AFTER.
    const bool g90 = (std::getenv("DC2_G90_RELOAD") != nullptr) && g67_title_scope(rdram, ctx);
    uint32_t mgr0 = 0u, mgr1 = 0u, remap = 0u, tmCache = 0u, tmCursor = 0u, tmCount = 0u, headTbp = 0u;
    if (g90)
    {
        mgr0 = dc2_read_u32(rdram, mgr + 0x00u);
        mgr1 = dc2_read_u32(rdram, mgr + 0x04u);
        const uint32_t texMgr = dc2_read_u32(rdram, mgr + 0x58u);
        tmCache = dc2_read_u32(rdram, texMgr + 0x08u);
        tmCursor = dc2_read_u32(rdram, texMgr + 0x00u);
        tmCount = dc2_read_u32(rdram, texMgr + 0x0Cu);
        if (mgr0 != 0u && mgrArg == 0u) {} // remap only consulted when mgr[0]!=0
        if (mgr0 != 0u && reqBlock >= 0)
            remap = dc2_read_u32(rdram, mgr1 + (uint32_t)reqBlock * 4u);
        else
            remap = (uint32_t)reqBlock;
        const uint32_t arr = dc2_read_u32(rdram, texMgr + 0x10u);
        const uint32_t head = (reqBlock >= 0) ? dc2_read_u32(rdram, arr + (uint32_t)reqBlock * 0x10u + 8u) : 0u;
        headTbp = head ? (dc2_read_u32(rdram, head + 0x38u) & 0x3FFFu) : 0xFFFFu;
    }

    mgEndDrawReloadTexture__FiP14mgCDrawManager_0x142560(rdram, ctx, runtime);

    if (g90)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 40u)
        {
            const uint32_t texMgr = dc2_read_u32(rdram, mgr + 0x58u);
            const uint32_t arr = dc2_read_u32(rdram, texMgr + 0x10u);
            const uint32_t head = (reqBlock >= 0) ? dc2_read_u32(rdram, arr + (uint32_t)reqBlock * 0x10u + 8u) : 0u;
            const uint32_t postTbp = head ? (dc2_read_u32(rdram, head + 0x38u) & 0x3FFFu) : 0xFFFFu;
            std::fprintf(stderr,
                "[G90:reload] n=%u block=%d ra=0x%x mgr0=0x%x mgr1=0x%x remap=%d cache=%d cursor=0x%x count=%u headTbp:0x%x->0x%x ret=%u\n",
                n, reqBlock, ra, mgr0, mgr1, (int32_t)remap, (int32_t)tmCache, tmCursor, tmCount,
                headTbp, postTbp, getRegU32(ctx, 2));
            std::fflush(stderr);
        }
    }
}

static void g67_mgenddraw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    static const bool g195BlockTrace = dc2_env_flag_enabled("DC2_G195_FG_TRACE") ||
                                       dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE") ||
                                       (std::getenv("DC2_G88_GEO") != nullptr);
    const int32_t reqBlock = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t mgrArg = getRegU32(ctx, 5);
    const uint32_t mgr = mgrArg ? mgrArg : 0x003820E0u;
    const uint32_t ra = getRegU32(ctx, 31);

    if (trace)
    {
        const uint32_t n = g_g67_enddraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 200u || (n % 360u) == 0u)
        {
            char line[1400];
            int p = std::snprintf(line, sizeof(line),
                "[G67:end.draw] n=%u mgr=0x%x mgrArg=0x%x ra=0x%x", n, mgr, mgrArg, ra);
            g67_append_mgr_block_summary(line, sizeof(line), &p, rdram, mgr, reqBlock);
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }

    // G90: tag the rasterizer with the block currently flushing so [G88:geo] can attribute
    // each drawn rock triangle to its source block.
    const bool inTitle = g67_title_scope(rdram, ctx);
    if (inTitle || g195BlockTrace)
        g_dc2TitleCurBlock.store(reqBlock, std::memory_order_relaxed);
    mgEndDraw__FiP14mgCDrawManager_0x142580(rdram, ctx, runtime);
    if (inTitle || g195BlockTrace)
        g_dc2TitleCurBlock.store(-1, std::memory_order_relaxed);
}
