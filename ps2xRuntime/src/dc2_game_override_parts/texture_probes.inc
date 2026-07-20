// PHASE9: DC2 ג€” placement operator new: return $a1 (the pointer argument)
static void reta1_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, getRegU32(ctx, 5)); // $v0 = $a1
    ctx->pc = getRegU32(ctx, 31);         // return to caller
}

// PHASE9: DC2 ג€” safe no-op for unrecompiled game functions (return 0, advance PC)
static void nop_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F3: DC2 ג€” SCElogoFade shim. Game polls "fade complete?" in MainLoop;
// boot path. Keep the decorative title map draw inert so the title handoff can
// proceed to menu initialization.
static void title_map_draw_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F50.2: sndLoadSound @0x18DA30 headless stub. The real recompiled body
// (de-stubbed in the F50.2 auto-stub->real batch) issues a SIF RPC to the IOP
// sound module and busy-waits for completion; with no IOP/SPU2 the wait never
// returns, hanging InitDungeonMain (caller @0x1CDDF8) forever. This function was
// an auto-stub before F50.2 for every caller (title path included), so returning
// 0 here reverts to the known-good headless behavior and lets dungeon init proceed.
static uint32_t g_f50_2_snd_load_log = 0u;
static void f50_2_snd_load_sound_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f50_2_snd_load_log;
    if (dc2_env_flag_enabled("DC2_TRACE_F50_1") && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F50.2:sndLoadSound] n=%u ra=0x%x a0=0x%x a1=0x%x a2=0x%x (headless no-op)\n",
                     n, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6));
    }
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

static uint32_t f51_guest_nonzero_prefix(uint8_t *rdram, uint32_t addr, uint32_t limit)
{
    if (addr == 0u)
        return 0u;

    uint32_t nonzero = 0u;
    const uint32_t count = (limit > 256u) ? 256u : limit;
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (rdram[(addr + i) & 0x1fffffffu] != 0u)
            ++nonzero;
    }
    return nonzero;
}

static void f51_decode_tex0(uint64_t tex0,
                            uint32_t *tbp,
                            uint32_t *tbw,
                            uint32_t *psm,
                            uint32_t *cbp,
                            uint32_t *cpsm)
{
    if (tbp)  *tbp  = static_cast<uint32_t>(tex0 & 0x3fffull);
    if (tbw)  *tbw  = static_cast<uint32_t>((tex0 >> 14) & 0x3full);
    if (psm)  *psm  = static_cast<uint32_t>((tex0 >> 20) & 0x3full);
    if (cbp)  *cbp  = static_cast<uint32_t>((tex0 >> 37) & 0x3fffull);
    if (cpsm) *cpsm = static_cast<uint32_t>((tex0 >> 51) & 0x0full);
}

static bool f51_target_like_tex0(uint64_t tex0)
{
    uint32_t tbp = 0u, psm = 0u, cbp = 0u;
    f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);
    return tbp == 0x2720u || tbp == 0x2820u || tbp == 0x29a0u ||
           tbp == 0x2f20u || cbp == 0x3fbcu || psm == 0x13u;
}

static void f51_log_texture_chain(uint8_t *rdram, uint32_t call, const char *stage, uint32_t listHead)
{
    uint32_t node = listHead;
    uint32_t guard = 0u;
    while (node != 0u && guard < 16u)
    {
        const uint64_t tex0 = dc2_read_u64(rdram, node + 0x38u);
        uint32_t tbp = 0u, tbw = 0u, psm = 0u, cbp = 0u, cpsm = 0u;
        f51_decode_tex0(tex0, &tbp, &tbw, &psm, &cbp, &cpsm);
        const uint32_t dims = dc2_read_u32(rdram, node);
        const uint32_t width = (dims >> 16) & 0xffffu;
        const uint32_t height = dims & 0xffffu;
        const uint32_t bitdepth = dc2_read_u32(rdram, node + 0x04u) >> 16;
        const uint32_t areaBlocks = dc2_read_u32(rdram, node + 0x28u);
        const uint32_t pix0 = dc2_read_u32(rdram, node + 0x50u);
        const uint32_t pix1 = dc2_read_u32(rdram, node + 0x54u);
        const uint32_t clut = dc2_read_u32(rdram, node + 0x60u);
        const uint32_t next = dc2_read_u32(rdram, node + 0x68u);
        const bool target = f51_target_like_tex0(tex0) || pix0 != 0u || clut != 0u;
        if ((guard < 8u || target) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:tex] call=%u stage=%s node=0x%x guard=%u wh=%ux%u bitdepth=%u area=0x%x "
                         "tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x pix0=0x%x pix1=0x%x clut=0x%x next=0x%x\n",
                         call, stage, node, guard, width, height, bitdepth, areaBlocks,
                         static_cast<unsigned long long>(tex0),
                         tbp, tbw, psm, cbp, cpsm, pix0, pix1, clut, next);
        }
        node = next;
        ++guard;
    }
}

static void f51_mgenddraw_reload_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        if (call <= 64u && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:enddraw] call=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%x\n",
                         call, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                         getRegU32(ctx, 7), getRegU32(ctx, 31));
        }
    }
    mgEndDrawReloadTexture__FiP14mgCDrawManager_0x142560(rdram, ctx, runtime);
}

static void f51_reload_texture_packet_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        const uint32_t mgr = getRegU32(ctx, 4);
        const uint32_t setIdx = getRegU32(ctx, 5);
        const uint32_t packet = getRegU32(ctx, 6);
        const uint32_t curSet = mgr ? dc2_read_u32(rdram, mgr + 0x08u) : 0u;
        const uint32_t count = mgr ? dc2_read_u32(rdram, mgr + 0x0cu) : 0u;
        const uint32_t table = mgr ? dc2_read_u32(rdram, mgr + 0x10u) : 0u;
        const uint32_t listHead = (mgr && table != 0u && setIdx < count) ?
            dc2_read_u32(rdram, table + setIdx * 0x10u + 8u) : 0u;
        if ((call <= 96u || listHead != 0u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadpkt] call=%u mgr=0x%x set=%u packet=0x%x curset=%u count=%u base0=0x%x clutbase=0x%x table=0x%x list=0x%x ra=0x%x\n",
                         call, mgr, setIdx, packet, curSet, count,
                         mgr ? dc2_read_u32(rdram, mgr + 0x00u) : 0u,
                         mgr ? dc2_read_u32(rdram, mgr + 0x04u) : 0u,
                         table, listHead, getRegU32(ctx, 31));
        }
    }

    ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(rdram, ctx, runtime);
}

// PHASE F50.9: mgLoadImage probe. mgLoadImage@0x12e600 builds the VIF1
// BITBLTBUF+IMAGE upload packet for BOTH the texture pixels (large transfers)
// and, via ReloadCLUT@0x12ed90, the palette (CLUT) for paletted textures.
// EABI args: a0=dst($4) a1=DBP($5) a2=DPSM($6) a3=DBW($7) a4/$t0=src($8)
// a5/$t1=param6/qwc($9). The dungeon CLUT shows DBW=1, small qwc (4 or 0x40),
// DPSM=CPSM (PSMCT16=0x2), DBP=CBP (~0x2980). F50.8 found ZERO dpsm=0x2 reach GS,
// so this confirms whether the game even issues the dungeon CLUT upload.
// Always delegates to the real recomp body; logging is gated by DC2_TRACE_F50_9.
static void f50_9_mg_load_image_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        const uint32_t dst = getRegU32(ctx, 4);
        const uint32_t dbp = getRegU32(ctx, 5);
        const uint32_t dpsm = getRegU32(ctx, 6);
        const uint32_t dbw = getRegU32(ctx, 7);
        const uint32_t src = getRegU32(ctx, 8);
        const uint32_t qwc = getRegU32(ctx, 9);
        const uint32_t dsax = getRegU32(ctx, 10);
        const uint32_t dsay = getRegU32(ctx, 11);
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t rrw = dc2_read_u32(rdram, sp + 0u);
        const uint32_t rrh = dc2_read_u32(rdram, sp + 8u);
        const bool hit = (dbp == 0x2720u) || (dpsm == 0x13u) ||
                         (dbp >= 0x2700u && dbp <= 0x3500u) ||
                         (dbp >= 0x3fb0u && dbp <= 0x3fe8u);
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        if ((call <= 160u || hit) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:mgli] call=%u hit=%u dst=0x%x dbp=0x%x byte=0x%x dpsm=0x%x dbw=%u src=0x%x qwc=%u ds=(%u,%u) rr=(%u,%u) srcNz256=%u srcDw0=0x%x ra=0x%x\n",
                         call, hit ? 1u : 0u, dst, dbp, dbp * 256u, dpsm, dbw,
                         src, qwc, dsax, dsay, rrw, rrh,
                         f51_guest_nonzero_prefix(rdram, src, 256u),
                         src ? dc2_read_u32(rdram, src) : 0u,
                         getRegU32(ctx, 31));
        }
    }

    if (f50_9_trace_enabled())
    {
        const uint32_t dbp  = getRegU32(ctx, 5);
        const uint32_t dpsm = getRegU32(ctx, 6);
        const uint32_t dbw  = getRegU32(ctx, 7);
        const uint32_t src  = getRegU32(ctx, 8);
        const uint32_t qwc  = getRegU32(ctx, 9);
        const uint32_t ra   = getRegU32(ctx, 31);
        // CLUT-shaped: DBW==1 and small qwc. Also force-log any PSMCT16 dest.
        const bool clutShaped = (dbw == 1u) || (dpsm == 0x2u) || (dpsm == 0xau);
        static uint32_t s_all = 0u;
        static uint32_t s_clut = 0u;
        const uint32_t n = ++s_all;
        const uint32_t c = clutShaped ? (++s_clut) : 0u;
        if (n <= 96u || (clutShaped && c <= 64u) || (n % 512u) == 0u)
        {
            std::fprintf(stderr,
                         "[F509:mgli] n=%u %s dbp=0x%x(blk*256=0x%x) dpsm=0x%x dbw=%u qwc=%u src=0x%x ra=0x%x\n",
                         n, clutShaped ? "CLUT?" : "tex", dbp, dbp * 256u, dpsm, dbw, qwc, src, ra);
        }
    }
    mgLoadImage__FPUiiiiP1iiiii_0x12e600(rdram, ctx, runtime);
}

static void f50_10_reload_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mgr    = getRegU32(ctx, 4); // param_1: mgCTextureManager*
    const uint32_t setIdx = getRegU32(ctx, 5); // param_2: texture-set index
    const uint32_t packet = getRegU32(ctx, 6); // param_3: output VIF/GIF packet ptr; 0 rewrites TEX0 only.
    bool willReload = false;
    uint32_t m0 = 0u, m1 = 0u, m2 = 0u, m3 = 0u, m4 = 0u;
    if ((f50_10_trace_enabled() || f51_trace_enabled()) && mgr != 0u)
    {
        m0 = dc2_read_u32(rdram, mgr + 0x00u);  // texture VRAM base (iVar2 seed)
        m1 = dc2_read_u32(rdram, mgr + 0x04u);  // CLUT VRAM base (iVar5 seed)
        m2 = dc2_read_u32(rdram, mgr + 0x08u);  // currently-loaded set
        m3 = dc2_read_u32(rdram, mgr + 0x0Cu);  // texture-set count
        m4 = dc2_read_u32(rdram, mgr + 0x10u);  // set table base
        willReload = (m2 != setIdx) && ((int32_t)setIdx >= 0) && ((int32_t)setIdx < (int32_t)m3);
    }

    uint32_t f51Call = 0u;
    uint32_t f51ListHead = 0u;
    if (f51_trace_enabled() && mgr != 0u)
    {
        static uint32_t s_call = 0u;
        f51Call = ++s_call;
        if (m4 != 0u && setIdx < m3)
            f51ListHead = dc2_read_u32(rdram, setIdx * 0x10u + m4 + 8u);
        if ((f51Call <= 96u || f51ListHead != 0u || setIdx == 1u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadraw-in] call=%u mgr=0x%x set=%u packet=0x%x curset=%u count=%u base0=0x%x clutbase=0x%x table=0x%x list=0x%x willReload=%u ra=0x%x\n",
                         f51Call, mgr, setIdx, packet, m2, m3, m0, m1, m4,
                         f51ListHead, willReload ? 1u : 0u, getRegU32(ctx, 31));
            if (f51ListHead != 0u)
                f51_log_texture_chain(rdram, f51Call, "pre", f51ListHead);
        }
    }

    ReloadTexture__17mgCTextureManagerFiPUi_0x12e970(rdram, ctx, runtime);

    if (f51_trace_enabled() && mgr != 0u && f51Call != 0u)
    {
        const uint32_t postCurSet = dc2_read_u32(rdram, mgr + 0x08u);
        if ((f51Call <= 96u || f51ListHead != 0u || setIdx == 1u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadraw-out] call=%u mgr=0x%x set=%u packet=0x%x postCurset=%u retWords=%u list=0x%x\n",
                         f51Call, mgr, setIdx, packet, postCurSet, getRegU32(ctx, 2), f51ListHead);
            if (f51ListHead != 0u)
                f51_log_texture_chain(rdram, f51Call, "post", f51ListHead);
        }
    }

    if (f50_10_trace_enabled() && mgr != 0u)
    {
        // Enumerate every DISTINCT manager instance (uncapped) so a 2nd map/model manager
        // with a base near 0x2580 is not hidden behind the per-call cap.
        static uint32_t s_mgrs[16] = {0};
        static uint32_t s_mgrCount = 0u;
        bool known = false;
        for (uint32_t i = 0u; i < s_mgrCount; ++i)
            if (s_mgrs[i] == mgr) { known = true; break; }
        if (!known && s_mgrCount < 16u)
        {
            s_mgrs[s_mgrCount++] = mgr;
            std::fprintf(stderr,
                         "[F510:mgr] NEW mgr=0x%x base0=0x%x clutbase=0x%x curset=%u count=%u settable=0x%x\n",
                         mgr, m0, m1, m2, m3, m4);
        }
        static uint32_t s_calls = 0u;
        const uint32_t call = ++s_calls;
        if (call <= 24u)
        {
            std::fprintf(stderr,
                         "[F510:reload] call=%u mgr=0x%x set=%u curset=%u count=%u base0=0x%x clutbase=0x%x willReload=%d\n",
                         call, mgr, setIdx, m2, m3, m0, m1, willReload ? 1 : 0);
            // Walk the texture linked-list for this set and dump each node's post-reload TEX0.
            const uint32_t listHead = dc2_read_u32(rdram, setIdx * 0x10u + m4 + 8u);
            uint32_t node = listHead;
            uint32_t guard = 0u;
            while (node != 0u && guard < 24u)
            {
                const uint64_t tex0 = dc2_read_u64(rdram, node + 0x38u);
                const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
                const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
                const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
                const uint32_t cpsm = (uint32_t)((tex0 >> 51) & 0xFull);
                const uint32_t bitd = dc2_read_u32(rdram, node + 0x04u) >> 16; // *(short*)(node+6)
                const uint32_t pix0 = dc2_read_u32(rdram, node + 0x50u);       // mip0 pixel ptr
                const uint32_t clut = dc2_read_u32(rdram, node + 0x60u);       // CLUT ptr
                std::fprintf(stderr,
                             "[F510:tex] call=%u node=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x bitdepth=%u pix0=0x%x clutptr=0x%x\n",
                             call, node, tbp, psm, cbp, cpsm, bitd, pix0, clut);
                node = dc2_read_u32(rdram, node + 0x68u);
                ++guard;
            }
        }
    }
}

static void f50_11_drawprim_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f50_11_trace_enabled())
    {
        const uint32_t env = getRegU32(ctx, 5); // param_2: texEnv (sceGsTex0 at +0)
        if (env != 0u)
        {
            const uint64_t tex0 = dc2_read_u64(rdram, env);
            const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
            const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
            const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
            const uint32_t cpsm = (uint32_t)((tex0 >> 51) & 0xFull);
            const bool paletted = (psm == 0x13u || psm == 0x14u || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu);
            static uint32_t s_all = 0u;
            const uint32_t n = ++s_all;
            // Histogram-friendly: log paletted binds (capped) + always when tbp==0x2580.
            static uint32_t s_pal = 0u;
            const bool hit2580 = (tbp == 0x2580u);
            if ((paletted && ++s_pal <= 120u) || (hit2580 && (n % 256u) == 1u))
            {
                std::fprintf(stderr,
                             "[F511:bind] n=%u env=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x ra=0x%x\n",
                             n, env, tbp, psm, cbp, cpsm, getRegU32(ctx, 31));
                if (hit2580)
                {
                    // Dump the texEnv neighbourhood to identify the owning struct.
                    std::fprintf(stderr,
                                 "[F511:env2580] env=0x%x  +0=0x%llx  +0x28=0x%x +0x40=0x%x +0x48=0x%x +0x50=0x%x +0x58=0x%x +0x60=0x%x  (env-0x38)+0x50=0x%x (env-0x38)+0x60=0x%x\n",
                                 env, (unsigned long long)tex0,
                                 dc2_read_u32(rdram, env + 0x28u), dc2_read_u32(rdram, env + 0x40u),
                                 dc2_read_u32(rdram, env + 0x48u), dc2_read_u32(rdram, env + 0x50u),
                                 dc2_read_u32(rdram, env + 0x58u), dc2_read_u32(rdram, env + 0x60u),
                                 dc2_read_u32(rdram, env - 0x38u + 0x50u), dc2_read_u32(rdram, env - 0x38u + 0x60u));
                }
            }
        }
    }
    Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(rdram, ctx, runtime);
}

static void f50_11_get_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mgr  = getRegU32(ctx, 4);
    const uint32_t name = getRegU32(ctx, 5);
    GetTexture__17mgCTextureManagerFPci_0x12d050(rdram, ctx, runtime);
    if (f50_11_trace_enabled())
    {
        const uint32_t tex = getRegU32(ctx, 2); // v0 return
        if (tex != 0u)
        {
            const uint64_t tex0 = dc2_read_u64(rdram, tex + 0x38u);
            const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
            const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
            const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
            char nm[28]; nm[0] = '\0';
            for (uint32_t i = 0u; i < 24u; ++i)
            {
                const uint8_t b = (uint8_t)(dc2_read_u32(rdram, name + i) & 0xFFu);
                nm[i] = (b >= 0x20u && b < 0x7Fu) ? (char)b : (b == 0u ? '\0' : '.');
                if (b == 0u) break;
            }
            nm[27] = '\0';
            static uint32_t s_n = 0u;
            const uint32_t n = ++s_n;
            const bool hit = (tbp == 0x2580u);
            if (n <= 120u || hit)
                std::fprintf(stderr,
                             "[F511:gettex] n=%u mgr=0x%x name='%s' tex=0x%x tbp=0x%x psm=0x%x cbp=0x%x pix0=0x%x clutptr=0x%x%s\n",
                             n, mgr, nm, tex, tbp, psm, cbp,
                             dc2_read_u32(rdram, tex + 0x50u), dc2_read_u32(rdram, tex + 0x60u),
                             hit ? "  <<< 0x2580" : "");
        }
    }
}
