static uint16_t g67_read_u16(uint8_t *rdram, uint32_t addr)
{
    const uint8_t *src = getConstMemPtr(rdram, addr);
    if (!src)
        return 0u;
    return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8u));
}

// G89: g67_title_scope sets g_dc2TitleRockScope (declared at global scope near the top of this
// file, defined in ps2_gs_rasterizer.cpp) each frame so the rasterizer's title-rock guard-band
// cull only fires in the title-map scene.
static bool g67_title_scope(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
    const bool inTitle = loopNo == 3u && titleMap == 0x008FC880u;
    g_dc2TitleRockScope.store(inTitle, std::memory_order_relaxed);
    // G121: A/B the per-object transform-vs-drawing selector field desc+0x2c, runner vs HW.
    // HW (live BP 0x1404d0) has 4 objects with desc+0x2c==0 (drawing/copy route): descs
    // 0x979710/0x9826f0/0x984990/0x9870d0; the 25 rock parts (e.g. 0x906b40) have +0x2c==1
    // (transform). Descriptors are same-address per G116. Read +0x2c for those fixed addrs
    // plus signature fields (+0x00=0xffffffff, +0x20=100.0, +0x70=128.0) to confirm the address
    // holds the SAME object on the runner. If the runner's +0x2c matches HW (0 for the 4,
    // 1 for rock) -> routing matches, the bug is downstream (copy walls on-screen but invisible).
    // If +0x2c differs -> the routing IS the bug -> trace the writer. Env DC2_G121_DESC; one-shot
    // (after title scope is live a few frames so the descriptors are populated); quiet by default.
    if (inTitle && std::getenv("DC2_G121_DESC") != nullptr)
    {
        static std::atomic<uint32_t> s_g121frames{0};
        static std::atomic<bool> s_g121done{false};
        const uint32_t f = s_g121frames.fetch_add(1u, std::memory_order_relaxed);
        if (f >= 30u && !s_g121done.exchange(true))
        {
            static const uint32_t addrs[5] = {0x979710u, 0x9826f0u, 0x984990u, 0x9870d0u, 0x906b40u};
            for (int i = 0; i < 5; ++i)
            {
                const uint32_t a = addrs[i];
                const uint32_t s0  = dc2_read_u32(rdram, a + 0x00u);
                const uint32_t s20 = dc2_read_u32(rdram, a + 0x20u);
                const uint32_t d2c = dc2_read_u32(rdram, a + 0x2cu);
                const uint32_t s70 = dc2_read_u32(rdram, a + 0x70u);
                const uint32_t d84 = dc2_read_u32(rdram, a + 0x84u);
                const bool sig = (s0 == 0xffffffffu && s20 == 0x42c80000u && s70 == 0x43000000u);
                std::fprintf(stderr,
                    "[G121:desc] addr=0x%x +0x2c=0x%x bit2=%d sigOK=%d (+0x00=0x%x +0x20=0x%x +0x70=0x%x +0x84=0x%x) HWexpect_bit2=%d\n",
                    a, d2c, (d2c != 0u) ? 1 : 0, sig ? 1 : 0, s0, s20, s70, d84, (i < 4) ? 0 : 1);
            }
            std::fflush(stderr);
        }
    }
    return inTitle;
}

// =====================================================================================
// PHASE G93 ג€” make the title-rock NATIVE TRANSFORM path render on-screen by fixing the
// renderinfo camera/projection at its SOURCE (Option 3, future-proof), replacing the
// G92 DC2_G92_FORCEMTX constant-injection lever.
//
// ROOT (G92C + G93 live A/B): the per-frame mgRENDER_INFO@0x380ec0 that the transform
// packers consume encodes the WRONG title camera:
//   (1) projection focal = 800 (state-2 TitleProjection) vs HW 480 (state-1);
//   (2) the follow-camera VIEW (renderinfo+0x1a0) has eye~origin (translate (0,0,6.25))
//       vs HW eye 661 back, because GetCameraMatrix__9mgCCamera@0x1314d0 builds the view
//       purely from camera pos(+0x00) -> ref(+0x10) (GetDir), and at TitleMapDraw time the
//       runner's follow camera pos is STALE (the G81 fix writes the camera fields only
//       LATER, inside the mgClipInBoxW geometry-flush hook, and writes a fixed ctor pos
//       (1.2,2.3,550) unsuitable for a follow camera). HW recomputes pos each frame from
//       ref + dist + height + angle.
//   (3) renderinfo lightInfo idx(+0x3f4) = 2 vs HW 0 (lighting variant).
//
// FIX (runs at TitleDraw entry, BEFORE mgSetRenderInfo + TitleMapDraw's GetCameraMatrix):
//   - TitleProjection(0x377E50) = 480 so SetRenderInfo builds the 480 projection.
//   - renderinfo idx(+0x3f4) = 0.
//   - recompute the follow-camera eye from its LIVE ref/dist/height/angle (the HW follow
//     formula eye = ref + (sin(a)*dist, height, cos(a)*dist)) and write camera pos(+0x00),
//     so the real GetCameraMatrix builds the HW view. Derived from live params (NOT a
//     constant matrix) -> future-proof and self-correcting as the camera zooms.
// Title-scoped (g67_title_scope: loopNo==3 && TitleMap==0x8FC880) => costume/dungeon safe.
// Opt-in proof lever only: DC2_G93_FIX=1. Sub-levers DC2_G93_NO_CAM / DC2_G93_NO_FOCAL for A/B.
// =====================================================================================
static void g93_title_renderinfo_fix(uint8_t *rdram, R5900Context *ctx)
{
    // OPT-IN diagnostic lever only (DC2_G93_FIX=1). NOT default-on: it writes GLOBAL state
    // (TitleProjection / renderinfo idx / the title camera object) in the front-end scope,
    // which bleeds into the CONCURRENT costume select (the G79 title<->costume concurrency)
    // and mislocates the Max model. The real fix is the front-end STATE (see G93 fix-log),
    // not this global write. Kept as the proof lever for the renderinfo-camera diagnosis.
    static const bool enabled = dc2_env_flag_enabled("DC2_G93_FIX");
    if (!enabled)
        return;
    if (!g67_title_scope(rdram, ctx))
        return;

    auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
    auto wf = [&](uint32_t a, float v) { uint32_t u; std::memcpy(&u, &v, 4); dc2_write_u32(rdram, a, u); };

    // (1) focal 480 (TitleProjection global) + (3) lightInfo idx 0.
    static const bool noFocal = dc2_env_flag_enabled("DC2_G93_NO_FOCAL");
    if (!noFocal)
    {
        wf(0x00377E50u, 480.0f);              // TitleProjection (state-1 focal)
        dc2_write_u32(rdram, 0x00380EC0u + 0x3F4u, 0u); // renderinfo lightInfo idx
    }

    // (2) follow-camera eye recompute.
    static const bool noCam = dc2_env_flag_enabled("DC2_G93_NO_CAM");
    if (!noCam)
    {
        const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u); // TitleCamera
        if (cam != 0u)
        {
            const float dist = rf(cam + 0x90u);
            // Only once G81 has initialised the title framing (ctor dist=40); skip the
            // fresh ctor camera so we never write garbage from the default ref/origin.
            if (dist > 700.0f)
            {
                const float refx = rf(cam + 0x10u);
                const float refy = rf(cam + 0x14u);
                const float refz = rf(cam + 0x18u);
                const float height = rf(cam + 0x94u);
                const float angle = rf(cam + 0x9Cu); // current (interpolated) follow angle
                const float s = std::sin(angle);
                const float c = std::cos(angle);
                wf(cam + 0x00u, refx + s * dist); // eye.x
                wf(cam + 0x04u, refy + height);   // eye.y
                wf(cam + 0x08u, refz + c * dist); // eye.z
                wf(cam + 0x0Cu, 1.0f);
                if (std::getenv("DC2_G93_CAM") != nullptr)
                {
                    std::fprintf(stderr,
                        "[G93:cam] cam=0x%x dist=%.1f height=%.1f angle=%.4f eye=(%.1f,%.1f,%.1f) ref=(%.1f,%.1f,%.1f)\n",
                        cam, dist, height, angle, refx + s * dist, refy + height, refz + c * dist,
                        refx, refy, refz);
                    std::fflush(stderr);
                }
            }
        }
    }
}

static bool g67_title_tbp(uint32_t tbp)
{
    switch (tbp)
    {
    case 0x2720u: case 0x2760u: case 0x2820u: case 0x2860u:
    case 0x28A0u: case 0x28C0u: case 0x2B20u: case 0x2F20u:
    case 0x3320u: case 0x3720u: case 0x3820u: case 0x3920u:
    case 0x3960u:
        return true;
    default:
        return false;
    }
}

static bool g67_record_tex_hit(G67TexHit *hits, uint32_t *count, uint32_t addr, uint64_t tex0)
{
    uint32_t tbp = 0u, psm = 0u, cbp = 0u;
    f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);
    if (!g67_title_tbp(tbp))
        return false;
    if (psm == 0u)
    {
        if (cbp != 0u && cbp < 0x3FC0u)
            return false;
    }
    else if (psm == 0x13u)
    {
        if (cbp < 0x3FC0u)
            return false;
    }
    else
    {
        return false;
    }

    bool hit3960 = (tbp == 0x3960u);
    for (uint32_t i = 0u; i < *count; ++i)
    {
        if (hits[i].addr == addr && hits[i].tex0 == tex0)
            return hit3960;
    }
    if (*count < 6u)
    {
        G67TexHit &h = hits[*count];
        h.addr = addr;
        h.tbp = tbp;
        h.psm = psm;
        h.cbp = cbp;
        h.tex0 = tex0;
        ++(*count);
    }
    return hit3960;
}

static bool g67_scan_tex0_range_limited(uint8_t *rdram, uint32_t base, uint32_t bytes,
                                        uint32_t limit, G67TexHit *hits, uint32_t *count)
{
    const uint32_t phys = base & 0x1FFFFFFFu;
    if (!g43_guest_ram_addr(phys))
        return false;
    const uint32_t capped = (bytes < limit) ? bytes : limit;
    bool hit3960 = false;
    for (uint32_t off = 0u; off + 8u <= capped; off += 8u)
        hit3960 |= g67_record_tex_hit(hits, count, phys + off, dc2_read_u64(rdram, phys + off));
    return hit3960;
}

static bool g67_scan_tex0_range(uint8_t *rdram, uint32_t base, uint32_t bytes,
                                G67TexHit *hits, uint32_t *count)
{
    return g67_scan_tex0_range_limited(rdram, base, bytes, 0x200u, hits, count);
}

static bool g67_scan_packet_refs(uint8_t *rdram, uint32_t packet, uint32_t qwc,
                                 G67TexHit *hits, uint32_t *count)
{
    const uint32_t phys = packet & 0x1FFFFFFFu;
    if (!g43_guest_ram_addr(phys))
        return false;
    const uint32_t packetBytes = ((qwc != 0u && qwc < 0x40u) ? qwc : 0x20u) * 16u;
    bool hit3960 = g67_scan_tex0_range(rdram, phys, packetBytes, hits, count);

    const uint32_t qwords = packetBytes / 16u;
    for (uint32_t q = 0u; q < qwords; ++q)
    {
        const uint32_t tag = dc2_read_u32(rdram, phys + q * 16u);
        const uint32_t ref = dc2_read_u32(rdram, phys + q * 16u + 4u) & 0x1FFFFFFFu;
        const uint32_t tagClass = tag & 0xF0000000u;
        const uint32_t refQwc = tag & 0xFFFFu;
        if (refQwc == 0u || refQwc > 0x80u)
            continue;
        if (tagClass != 0x10000000u && tagClass != 0x30000000u && tagClass != 0x50000000u)
            continue;
        if (!g43_guest_ram_addr(ref))
            continue;
        hit3960 |= g67_scan_tex0_range(rdram, ref, refQwc * 16u, hits, count);
    }
    return hit3960;
}

static void g67_append_tex_hits(char *buf, size_t cap, int *p, const G67TexHit *hits, uint32_t count)
{
    if (*p >= static_cast<int>(cap))
        return;
    *p += std::snprintf(buf + *p, cap - static_cast<size_t>(*p), " hits=");
    for (uint32_t i = 0u; i < count && *p < static_cast<int>(cap) - 80; ++i)
    {
        *p += std::snprintf(buf + *p, cap - static_cast<size_t>(*p),
            "%s0x%x:tbp=0x%x/psm=0x%x/cbp=0x%x",
            (i == 0u) ? "" : ",", hits[i].addr, hits[i].tbp, hits[i].psm, hits[i].cbp);
    }
}

static uint32_t g67_mgr_count(uint8_t *rdram, uint32_t mgr, uint32_t block)
{
    const uint32_t counts = mgr ? dc2_read_u32(rdram, mgr + 0x18u) : 0u;
    if (!counts || block >= 64u)
        return 0u;
    return dc2_read_u32(rdram, counts + block * 4u);
}

static uint32_t g67_mgr_max_block(uint8_t *rdram, uint32_t mgr)
{
    const uint32_t bounds = mgr ? dc2_read_u32(rdram, mgr + 0x58u) : 0u;
    const uint32_t count = bounds ? dc2_read_u32(rdram, bounds + 0x0Cu) : 0u;
    return count ? count : 64u;
}

static int32_t g67_resolve_mgr_block(uint8_t *rdram, uint32_t mgr, int32_t block)
{
    if (!mgr || block < 0)
        return -1;
    const uint32_t ublock = static_cast<uint32_t>(block);
    const uint32_t maxBlock = g67_mgr_max_block(rdram, mgr);
    if (ublock >= maxBlock)
        return -2;
    if (dc2_read_u32(rdram, mgr + 0x00u) != 0u)
    {
        const uint32_t remap = dc2_read_u32(rdram, mgr + 0x04u);
        if (!remap)
            return -3;
        return static_cast<int32_t>(dc2_read_u32(rdram, remap + ublock * 4u));
    }
    return block;
}

static uint32_t g67_mgr_head(uint8_t *rdram, uint32_t mgr, int32_t block)
{
    if (!mgr || block < 0)
        return 0u;
    const uint32_t heads = dc2_read_u32(rdram, mgr + 0x10u);
    const uint32_t ublock = static_cast<uint32_t>(block);
    if (!heads || ublock >= 64u)
        return 0u;
    return dc2_read_u32(rdram, heads + ublock * 4u);
}

static void g67_append_block_list(char *line, size_t cap, int *p,
                                  uint8_t *rdram, uint32_t buf, int32_t count, int32_t max)
{
    if (*p < 0 || static_cast<size_t>(*p) >= cap || !buf || count <= 0)
        return;
    const int32_t n = (count < max) ? count : max;
    *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p), " out=");
    for (int32_t i = 0; i < n && *p < static_cast<int>(cap) - 24; ++i)
    {
        const int32_t v = static_cast<int32_t>(dc2_read_u32(rdram, buf + static_cast<uint32_t>(i) * 4u));
        *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                            "%s%d", (i == 0) ? "" : ",", v);
    }
}

static void g67_append_mgr_block_summary(char *line, size_t cap, int *p,
                                         uint8_t *rdram, uint32_t mgr, int32_t reqBlock)
{
    if (*p < 0 || static_cast<size_t>(*p) >= cap)
        return;
    const int32_t resolved = g67_resolve_mgr_block(rdram, mgr, reqBlock);
    const uint32_t count = (resolved >= 0) ? g67_mgr_count(rdram, mgr, static_cast<uint32_t>(resolved)) : 0u;
    const uint32_t array = (resolved >= 0) ? g67_mgr_head(rdram, mgr, resolved) : 0u;
    *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                        " req=%d->%d count=%u array=0x%x", reqBlock, resolved, count, array);
    for (uint32_t i = 0u; i < 4u && g43_guest_ram_addr(array) && i < count && *p < static_cast<int>(cap) - 180; ++i)
    {
        const uint32_t slot = count - 1u - i;
        const uint32_t node = dc2_read_u32(rdram, array + slot * 4u);
        if (!g43_guest_ram_addr(node))
            break;
        const uint32_t state = dc2_read_u32(rdram, node + 0x00u);
        const uint32_t packet = dc2_read_u32(rdram, node + 0x04u);
        const int32_t nodeBlock = static_cast<int16_t>(g67_read_u16(rdram, node + 0x0Cu));
        const uint32_t vprog = g67_read_u16(rdram, node + 0x0Eu);
        G67TexHit hits[6]{};
        uint32_t hitCount = 0u;
        const bool hit3960 = g67_scan_tex0_range_limited(rdram, state, 0x1000u, 0x1000u, hits, &hitCount);
        *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                            " | q%u[%u]=0x%x nb=%d vp=0x%x st=0x%x pk=0x%x%s",
                            i, slot, node, nodeBlock, vprog, state, packet, hit3960 ? "/3960" : "");
    }
}

static void g67_frame_test1_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137EC8u);
    const uint32_t outBox = getRegU32(ctx, 7);

    test1__FPA4_fPA4_fPA4_fPfPf_0x135c70(rdram, ctx, runtime);

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    if (!scope)
        return;
    scope->test1Seen = 1u;
    scope->test1OriginalDist = g67_read_f32(rdram, outBox + 0x0Cu);
    scope->test1Dist = scope->test1OriginalDist;
    scope->test1Limit = g67_read_f32(rdram, 0x00381D48u);
    if (scope->distFix && scope->test1Dist < scope->test1Limit)
    {
        dc2_write_u32(rdram, outBox + 0x0Cu, g67_f32_bits(scope->test1Limit));
        scope->test1Dist = scope->test1Limit;
        scope->test1Fixed = 1u;
    }

    if (!trace)
        return;

    const uint32_t n = g_g67_test1.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = g67_focus_frame(scope->frame, scope->f8);
    if (focus || n <= 80u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G67:test1] n=%u frame=0x%x visual=0x%x out=0x%x dist=% .6g orig=% .6g "
                     "limit=% .6g fixed=%u culled=%u\n",
                     n, scope->frame, scope->f8, outBox, scope->test1Dist,
                     scope->test1OriginalDist, scope->test1Limit, scope->test1Fixed,
                     (!scope->test1Fixed && scope->test1Dist < scope->test1Limit) ? 1u : 0u);
        std::fflush(stderr);
    }
}

static void g67_frame_clipbox_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137EF4u);
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);

    mgClipBoxW__FPfPfPfPf_0x12f2e0(rdram, ctx, runtime);

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    uint32_t ret = getRegU32(ctx, 2);
    uint32_t fixed = 0u;
    if (scope && scope->distFix && ret == 0u)
    {
        setReturnU32(ctx, 1u);
        ret = 1u;
        fixed = 1u;
    }
    if (scope)
    {
        scope->clipSeen = 1u;
        scope->clipFixed = fixed;
        scope->clipRet = ret;
        scope->clipA0 = a0;
        scope->clipA1 = a1;
        scope->clipA2 = a2;
        scope->clipA3 = a3;
    }

    if (!trace)
        return;

    const uint32_t n = g_g67_clip.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = scope && g67_focus_frame(scope->frame, scope->f8);
    if (focus || ret == 0u || n <= 80u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G67:clip] n=%u frame=0x%x visual=0x%x ra=0x%x ret=%u fixed=%u "
                     "a=(0x%x,0x%x,0x%x,0x%x) min0=(% .6g,% .6g,% .6g,% .6g) max0=(% .6g,% .6g,% .6g,% .6g)\n",
                     n, scope ? scope->frame : 0u, scope ? scope->f8 : 0u, ra, ret, fixed,
                     a0, a1, a2, a3,
                     g67_read_f32(rdram, a0 + 0x00u), g67_read_f32(rdram, a0 + 0x04u),
                     g67_read_f32(rdram, a0 + 0x08u), g67_read_f32(rdram, a0 + 0x0Cu),
                     g67_read_f32(rdram, a1 + 0x00u), g67_read_f32(rdram, a1 + 0x04u),
                     g67_read_f32(rdram, a1 + 0x08u), g67_read_f32(rdram, a1 + 0x0Cu));
        std::fflush(stderr);
    }
}

// G78: mgClipInBoxW@0x12f380 is the SECOND clip test inside Draw__8mgCFrame (jal @0x137f04,
// ra=0x137f0c -> beq $v0,zero,0x137f20). Its return selects the frame's render-mode:
//   ret != 0  -> 0x137f14: fc0=0, fc4=0  => COPY render-mode (pre-transformed/screen-space)
//   ret == 0  -> 0x137f20: fc0=1 + fc4 set => TRANSFORM render-mode (re-projects + cull)
// CreateRenderInfoPacket builds VU qword 38 (the copy-vs-transform selector) from these flags:
// bit0=(fc0||fc4), bit1=fc4. The VU dispatcher routes a mesh to the copy packer 0x1b68 iff
// sel&5==0 (bit0 & bit2 clear). HW renders the title rock via copy meshes (selector 0x10/0x30,
// fc0=fc4=0; verified live on PCSX2 at CreateRenderInfoPacket@0x1404d0). The runner instead
// gets fc4!=0 for the rock meshes (selector 0x33 -> 0x17) because mgClipInBoxW returns 0 for
// the title rock frame, so every rock mesh routes to the +2048/ADC transform packers, whose
// per-vertex gate (IBEQ VI10(=208),VIxגˆˆ{0,1}) is structurally never taken => ADC=1 on every
// kicking vertex => no primitive ever draws => flat blue (p0x1b68=0). This mirrors the G67
// clip-box fix (which already forces the FIRST clip 0x12f2e0 for the same frame): for the title
// rock frame ONLY (scope->distFix, frame 0x981390/visual 0x987230), force the result to 1 so
// fc4=0 => copy render-mode => the rock is built + drawn WITH its texture (tbp=0x3960) and
// per-vertex ADC (unlike the G75 VU reroute, which forced transform-mode data through copy and
// rendered green/faceted). Kill-switch DC2_G78_NO_COPY_FIX. Trace DC2_TRACE_G67 -> [G78:clipinbox].
static std::atomic<uint32_t> g_g78_clipinbox{0};

// G79: costume->title round-trip leaves the title 3D bg flat-blue + a leaked model at the left.
// Probe whether the costume DRAW (MenuCostumeDraw@0x2be040) is still dispatched after exit (=
// front-end stuck on costume menu id 0x17) or the costume model object just persists/redraws
// independently. Logs the active menu id (MenuCommonInfo+0x54) + MenuActionChara[0]. DC2_TRACE_G79.
static std::atomic<uint32_t> g_g79_costdraw{0};
static std::atomic<uint32_t> g_g79_maindraw{0};
static bool g79_trace_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_TRACE_G79");
    return e;
}
static bool g79_leak_fix_enabled()
{
    // G92 Phase A (2026-06-26): DEFAULT-OFF (was default-on with kill DC2_G79_NO_LEAK_FIX).
    // The suppression gate below (loop==3 && titleMode==2 && menuId==0x17) was ASSUMED unique to
    // the costume->title round-trip leak, but a headless A/B (run_g34_costume / DC2_G9_COSTUME)
    // proved the LEGIT Select-Costume screen hits the exact same state (menuId=0x17 loop=3
    // titleMode=2, [G79:costdraw]) -> the fix wrongly skipped the real costume draw -> BLACK
    // screen (charDraw=0). Since the round-trip fix only ever removed a secondary MODEL leak (its
    // main symptom, the flat-blue bg, stayed DEFERRED) and it broke the PRIMARY costume baseline,
    // recover the baseline by defaulting OFF. Opt back in with DC2_G79_LEAK_FIX=1 for round-trip
    // experiments. The round-trip leak still needs a PROPER discriminator (the three gate globals
    // are identical between legit and leak; a costume Step-ran / exit-flag probe is required).
    static const bool e = dc2_env_flag_enabled("DC2_G79_LEAK_FIX");
    return e;
}
static void g79_costume_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t mcInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF94F8u) : 0u; // MenuCommonInfo (gp-0x6B08)
    const uint32_t menuId = mcInfo ? dc2_read_u32(rdram, mcInfo + 0x54u) : 0xFFFFFFFFu;
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu;
    const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u;
    const uint32_t tMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t mac0 = dc2_read_u32(rdram, 0x01F0CAA0u); // MenuActionChara[0]
    const uint32_t loadPhase = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu; // MenuCosutumeLoadPhase
    const uint32_t n = g_g79_costdraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    // G79 FIX: the costume->title round-trip leaves the MenuMain state machine STUCK at the
    // costume menu id (0x17) while TitleLoop is back on its own New Game menu (loop=3,
    // titleMode=2). The two run CONCURRENTLY: the stuck costume draw leaks the Max model (at the
    // left) AND its RTT/GS state corrupts the title 3D background (flat blue). This inconsistent
    // state (loop=3 && titleMode==2 && menuId==0x17) never happens during a legitimate costume
    // select (there TitleLoop is NOT showing its menu), so suppressing the costume draw here
    // restores the title without affecting real costume select. Kill-switch DC2_G79_NO_LEAK_FIX.
    const bool inconsistent = (loopNo == 3u && tMode == 2u && menuId == 0x17u);
    if (g79_trace_enabled() && (n <= 20u || (n % 120u) == 0u))
        std::fprintf(stderr, "[G79:costdraw] n=%u menuId=0x%x loop=%u titleMode=%u mac0=0x%x loadPhase=%u suppress=%d\n",
                     n, menuId, loopNo, tMode, mac0, loadPhase, (int)(g79_leak_fix_enabled() && inconsistent));
    if (g79_leak_fix_enabled() && inconsistent)
        return; // skip the stuck costume draw -> title 3D bg + menu render cleanly
    MenuCostumeDraw__Fv_0x2be040(rdram, ctx, runtime);
}
static void g79_main_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t mcInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF94F8u) : 0u;
    const uint32_t menuId = mcInfo ? dc2_read_u32(rdram, mcInfo + 0x54u) : 0xFFFFFFFFu;
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu; // LoopNo (3=front-end)
    const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u; // titleInfo (gp-0x6684)
    const uint32_t tMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t tSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t newGameSel = dc2_read_u32(rdram, 0x01ECD62Cu); // DAT_01ecd62c (0xc=New Game)
    const uint32_t n = g_g79_maindraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g79_trace_enabled() && (n <= 8u || (n % 60u) == 0u))
        std::fprintf(stderr, "[G79:maindraw] n=%u menuId=0x%x loop=%u titleMode=%u titleSub=%u newGameSel=0x%x\n",
                     n, menuId, loopNo, tMode, tSub, newGameSel);
    MenuMainDraw__Fv_0x234290(rdram, ctx, runtime);
}

static bool g78_copy_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G78_NO_COPY_FIX");
    return enabled;
}

// G80: the title rock cavern is logical texture block 0 (s19_01..s19_11a, VRAM 0x2720..0x3960,
// confirmed live on PCSX2 by walking texMgr@0x381ef0 block-0 chain) spread across MULTIPLE
// map-part visuals (0x92fb60/0x95b5a0/0x95eb20/0x9643b0, [G67:mdt] counts{0:7}). G78 only forced
// COPY render-mode for the block-1 (s19_05) visual 0x987230 -- the WRONG visual -- so the actual
// rock (block 0) stays TRANSFORM-routed -> the +2048/ADC gate culls every vertex -> flat/cyan
// ([G66:tbp] block-0 textures 0x2b20/0x2f20/0x3960 all nodraw). When enabled, extend the G78
// fc4=copy fix to EVERY title-map frame (g67_title_scope) instead of the single G78 target, so
// all block-0 rock meshes route to the copy packer 0x1b68 and sample their own reloaded textures.
// Default-ON (kill DC2_G80_NO_ROCK_COPY); title-scoped via g67_title_scope (loopNo==3 &&
// titleMap==0x8FC880) so it cannot touch the costume/menu (costume-safe by construction).
// Validated: brown rock formation + logo render (was flat cyan), golden smoke non-black, no
// costume regression. Residual (lower walls s19_02/04/06 still cull + camera) -> next phase.
// G100: master switch for the native forced-draw title-rock path (default-ON, kill
// DC2_G100_NO_FORCEDRAW). When ON, the title rock renders through the fixed transform packers
// (ps2_vu1.cpp::g100ForceTitleDraw) + the G89 guard band, matching the HW per-tbp on-screen
// distribution (0x2720 32%, walls 0x2b20/0x2f20/0x3320, no phantom pages). When reverted, the
// pre-G100 G78/G80 force-COPY path is restored.
static bool g100_native_title_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G100_NO_FORCEDRAW");
    return enabled;
}

// G80 (RETIRED by G100 for the title): the force-COPY band-aid routed the block-0 rock to the
// copy packer 0x1b68. With the native forced-draw transform path active it would DOUBLE-DRAW
// 0x2720 (measured ~5x the HW vertex count, 11% on-screen) and bind the phantom pages
// 0x3820/0x3920 that HW never draws -> over-bright + blue triangular holes. So copy is OFF
// whenever G100 is enabled. Reverts with DC2_G100_NO_FORCEDRAW; DC2_G80_NO_ROCK_COPY still kills
// it explicitly on the reverted path.
static bool g80_rock_copy_enabled()
{
    static const bool enabled =
        !g100_native_title_enabled() && !dc2_env_flag_enabled("DC2_G80_NO_ROCK_COPY");
    return enabled;
}

// G81: the title camera is stuck at mgCCameraFollow CONSTRUCTOR defaults (distance=40, height=30,
// from __ct__15mgCCameraFollow(40,30,0,8) at TitleInit@0x2a0e..) because TitleModeInit@0x2a1020's
// camera-setup block (SetPos/SetRef/SetDistance(1296)/SetHeight(-214)/SetAngle(0)/FollowOn) ran
// while the TitleCamera global (0x00377E38) was 0 (the known headless title-camera init-ordering
// gap, cf. G58). With ctor defaults the camera looks at the origin from distance 40 instead of
// framing the rock cavern (HW ref (-70.1,280.8,-493.0) at distance ~1296->800 zoom-in), so the
// rock renders tiny/misplaced and the green map background fills the screen.
// FIX (default-ON, kill DC2_G81_NO_CAM_FIX): when title-scoped and the camera still holds the ctor
// defaults, replicate TitleModeInit's camera block by writing the fields directly + restart the
// phase-0 zoom-in. One-shot (after the fix distance=1296 and the phase-0 AddDistance(-0.25) drives
// it to 800, so the dist==40 guard never re-fires). HW A/B (PCSX2 New Game menu): phase 0,
// dist 1198.75, height -214, pos(-70.1,66.8,707.7), ref(-70.1,280.8,-493.0); init constants
// pos=data@0x354300=(1.2,2.3,550), ref=data@0x354310=(-70.1,280.8,-493).
static bool g81_cam_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G81_NO_CAM_FIX");
    return enabled;
}

struct G81TitleCameraRepairResult
{
    bool applied;
    uint32_t cam;
    float oldDist;
    float oldHeight;
};

static void g99_snap_title_follow_eye(uint8_t *rdram, uint32_t cam)
{
    auto wf = [&](uint32_t a, float v) {
        uint32_t u;
        std::memcpy(&u, &v, sizeof(u));
        dc2_write_u32(rdram, a, u);
    };
    const float refx = g67_read_f32(rdram, cam + 0x10u);
    const float refy = g67_read_f32(rdram, cam + 0x14u);
    const float refz = g67_read_f32(rdram, cam + 0x18u);
    const float dist = g67_read_f32(rdram, cam + 0x90u);
    const float height = g67_read_f32(rdram, cam + 0x94u);
    const float angle = g67_read_f32(rdram, cam + 0x9cu);
    const float eyeX = refx + std::sin(angle) * dist;
    const float eyeY = refy + height;
    const float eyeZ = refz + std::cos(angle) * dist;

    wf(cam + 0x00u, eyeX);
    wf(cam + 0x04u, eyeY);
    wf(cam + 0x08u, eyeZ);
    wf(cam + 0x0cu, 1.0f);
    wf(cam + 0x20u, eyeX);
    wf(cam + 0x24u, eyeY);
    wf(cam + 0x28u, eyeZ);
    wf(cam + 0x2cu, 1.0f);

    wf(cam + 0xb0u, refx);
    wf(cam + 0xb4u, refy);
    wf(cam + 0xb8u, refz);
    wf(cam + 0xbcu, 0.0f);
}

static G81TitleCameraRepairResult g81_apply_title_camera_setup(uint8_t *rdram)
{
    const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u); // TitleCamera
    if (cam == 0u)
        return {false, 0u, 0.0f, 0.0f};
    const float dist = g67_read_f32(rdram, cam + 0x90u);
    const float height = g67_read_f32(rdram, cam + 0x94u);
    // Only the freshly-constructed, never-title-inited camera (ctor defaults 40.0 / 30.0).
    if (!(dist > 39.5f && dist < 40.5f && height > 29.5f && height < 30.5f))
        return {false, cam, dist, height};
    // pos = data@0x354300 (SetPos/SetNextPos); ref = data@0x354310 (SetRef = vtable+0x20).
    // CRITICAL: the follow camera (FollowOn) interpolates pos->nextpos(+0x20) and ref->nextref(+0x30,
    // the follow TARGET). Set BOTH current and "next" or the follow drifts ref to the origin (the
    // uninitialised target) and the camera looks away from the rock. HW has nextref(+0x30) and the
    // ref-copy(+0x70) all = the SetRef target (-70,281,-493).
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const uint32_t pw = dc2_read_u32(rdram, 0x00354300u + i * 4u); // pos vector
        const uint32_t rw = dc2_read_u32(rdram, 0x00354310u + i * 4u); // ref/target vector
        dc2_write_u32(rdram, cam + 0x00u + i * 4u, pw); // pos
        dc2_write_u32(rdram, cam + 0x20u + i * 4u, pw); // nextpos
        dc2_write_u32(rdram, cam + 0x10u + i * 4u, rw); // ref
        dc2_write_u32(rdram, cam + 0x30u + i * 4u, rw); // nextref (follow target)
        dc2_write_u32(rdram, cam + 0x70u + i * 4u, rw); // ref-copy (matches HW)
    }
    float initDist = 1296.0f; // SetDistance(0x44a28000)
    if (const char *d = std::getenv("DC2_G81_DIST")) { float v = (float)atof(d); if (v > 1.0f) initDist = v; }
    dc2_write_u32(rdram, cam + 0x90u, g67_f32_bits(initDist));
    dc2_write_u32(rdram, cam + 0x94u, g67_f32_bits(-214.0f)); // SetHeight(0xc3560000)
    dc2_write_u32(rdram, cam + 0x98u, g67_f32_bits(0.0f));    // SetAngle(0)
    dc2_write_u32(rdram, cam + 0x9cu, g67_f32_bits(0.0f));    // current angle
    dc2_write_u32(rdram, cam + 0xA0u, 1u);   // FollowOn
    // TitleModeInit immediately steps the follow camera with a negative step, snapping
    // current eye to ref + distance/height. The repair must do the same or the title
    // view matrix yaws while interpolating from the static ctor/init position.
    g99_snap_title_follow_eye(rdram, cam);
    dc2_write_u32(rdram, 0x00377E44u, 0u);   // TitleCameraPhase = 0 (restart the zoom-in)
    return {true, cam, dist, height};
}

static void g81_fix_title_camera(uint8_t *rdram, R5900Context *ctx)
{
    if (!g81_cam_fix_enabled())
        return;
    if (!g67_title_scope(rdram, ctx))
        return;
    const G81TitleCameraRepairResult result = g81_apply_title_camera_setup(rdram);
    if (std::getenv("DC2_G81_CAM") != nullptr)
    {
        if (result.applied)
            std::fprintf(stderr, "[G81:camfix] applied cam=0x%x dist 40->1296 height 30->-214 ref(-70,281,-493) snapFollowEye=1\n", result.cam);
        std::fflush(stderr);
    }
}

// G94: state-1 title init repair. The real TitleModeInit writes TitleProjection=480 in
// a branch-delay slot, but the runner reaches steady state 1 with projection 800 after
// an init/order bounce. Apply the state-1 init postcondition once at state entry, before
// TitleDraw consumes it. This is deliberately not a per-draw renderinfo/global write.
static bool g94_title_state1_init_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G94_NO_INIT_FIX");
    return enabled;
}

static bool g94_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G94");
    return enabled;
}

static bool g94_frontend_title_state(uint8_t *rdram, R5900Context *ctx, uint32_t *titleInfo, uint32_t *state, uint32_t *sub)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t info = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u;
    if (titleInfo) *titleInfo = info;
    if (state) *state = info ? dc2_read_u32(rdram, info + 0x00u) : 0xFFFFFFFFu;
    if (sub) *sub = info ? dc2_read_u32(rdram, info + 0x04u) : 0xFFFFFFFFu;
    return loopNo == 3u && info != 0u;
}

static void g94_apply_title_state1_init(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    if (!g94_title_state1_init_fix_enabled())
        return;

    uint32_t titleInfo = 0u;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t sub = 0xFFFFFFFFu;
    if (!g94_frontend_title_state(rdram, ctx, &titleInfo, &state, &sub))
        return;
    if (state != 1u && sub != 1u)
        return;

    const uint32_t beforeProj = dc2_read_u32(rdram, 0x00377E50u);
    dc2_write_u32(rdram, 0x00377E50u, g67_f32_bits(480.0f));
    const G81TitleCameraRepairResult cam = g81_apply_title_camera_setup(rdram);

    if (g94_trace_enabled())
    {
        const uint32_t afterProj = dc2_read_u32(rdram, 0x00377E50u);
        const float dist = cam.cam ? g67_read_f32(rdram, cam.cam + 0x90u) : 0.0f;
        const float height = cam.cam ? g67_read_f32(rdram, cam.cam + 0x94u) : 0.0f;
        std::fprintf(stderr,
                     "[G94:init] site=%s titleInfo=0x%x state=%u sub=%u proj=0x%08x->0x%08x cam=0x%x camApplied=%u dist=%.2f height=%.2f oldDist=%.2f oldHeight=%.2f pc=0x%x ra=0x%x\n",
                     site, titleInfo, state, sub, beforeProj, afterProj, cam.cam,
                     cam.applied ? 1u : 0u, dist, height, cam.oldDist, cam.oldHeight,
                     ctx->pc, getRegU32(ctx, 31));
        std::fflush(stderr);
    }
}

static void g94_title_mode_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entryRa = getRegU32(ctx, 31);
    TitleModeInit__Fv_0x2a1020(rdram, ctx, runtime);
    if (ctx->pc == getRegU32(ctx, 31) || ctx->pc == entryRa)
        g94_apply_title_state1_init(rdram, ctx, "TitleModeInit");
}

static void g94_observe_title_state1_entry(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    static uint32_t s_lastTitleInfo = 0u;
    static uint32_t s_lastState = 0xFFFFFFFFu;
    static bool s_state1Active = false;

    uint32_t titleInfo = 0u;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t sub = 0xFFFFFFFFu;
    const bool inFrontEnd = g94_frontend_title_state(rdram, ctx, &titleInfo, &state, &sub);
    if (!inFrontEnd)
    {
        s_lastTitleInfo = titleInfo;
        s_lastState = state;
        s_state1Active = false;
        return;
    }

    const bool enteringState1 =
        state == 1u &&
        (!s_state1Active || s_lastTitleInfo != titleInfo || s_lastState != 1u);
    if (enteringState1)
    {
        g94_apply_title_state1_init(rdram, ctx, site);
        s_state1Active = true;
    }
    else if (state != 1u)
    {
        s_state1Active = false;
    }

    s_lastTitleInfo = titleInfo;
    s_lastState = state;
}
