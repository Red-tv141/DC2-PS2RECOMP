// G9 costume-route state (driver bodies live near f49_menu_main_key_probe).
static bool g9_costume_enabled();
static bool g_g9_title_forced = false;
static void g94_observe_title_state1_entry(uint8_t *rdram, R5900Context *ctx, const char *site);

static void title_mode_key_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ret = (n < 180u) ? 0u : 5u;
    f29_log_title_state("TitleModeKey", rdram, ctx, n, ret);
    setReturnU32(ctx, ret);
    ctx->pc = getRegU32(ctx, 31);
}

static void f29_title_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu); // -0x6684
    const uint32_t preTitleState = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x0u) : 0xFFFFFFFFu;
    const uint32_t preTitleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x4u) : 0xFFFFFFFFu;
    // G9: the runner's press-start (state 1) never advances headlessly (real
    // TitleModeKey returns 0 with no input). Poke TitleInfo[1]=2 so TitleLoop's
    // own commit branch runs case 2 = MenuMainInit(&MenuArg) and *TitleInfo->2,
    // reaching the New-Game/Load menu where the MenuMainKey hook enters costume.
    // G9: the New-Game selector is forced in f49_title_mode_key_probe (TitleModeKey
    // ret=5) so InitSaveData/SetChrEquipDirect populate the costume list naturally.
    if (f49_trace_loop_timing_enabled() && (n <= 16u || (n % 60u) == 0u))
    {
        std::fprintf(stderr,
                     "[F49:loop-enter] tag=TitleLoop n=%u ra=0x%x loop=%u next=%u pre=%u state=%u sub=%u\n",
                     n, getRegU32(ctx, 31), loopNo, nextLoopNo, preVsync,
                     preTitleState, preTitleSub);
    }
    g94_observe_title_state1_entry(rdram, ctx, "TitleLoop.enter-state1");
    const auto t0 = std::chrono::steady_clock::now();
    TitleLoop__Fv_0x29ffa0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    g94_observe_title_state1_entry(rdram, ctx, "TitleLoop.exit-state1");
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const uint32_t postTitleState = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x0u) : 0xFFFFFFFFu;
        const uint32_t postTitleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x4u) : 0xFFFFFFFFu;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 10u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=TitleLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u state=%u->%u sub=%u->%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync,
                         preTitleState, postTitleState, preTitleSub, postTitleSub);
        }
    }
    f29_log_title_state("TitleLoop", rdram, ctx, n, getRegU32(ctx, 2));
}

static uint32_t g_f50_6_debug_log_count = 0u;
static bool g_f50_6_forced_menu_loop = false;

static void f50_6_enable_debug_menu(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    if (!f50_6_debug_menu_enabled())
        return;

    constexpr uint32_t kDebugFlagAddr = 0x00376FB8u; // gp-0x7538
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopAddr = gp + 0xFFFF8ADCu;     // gp-0x7524 LoopNo
    const uint32_t nextLoopAddr = gp + 0xFFFF8AE0u; // gp-0x7520 NextLoopNo

    const uint32_t flagBefore = dc2_read_u32(rdram, kDebugFlagAddr);
    const uint32_t loopBefore = dc2_read_u32(rdram, loopAddr);
    const uint32_t nextBefore = dc2_read_u32(rdram, nextLoopAddr);

    dc2_write_u32(rdram, kDebugFlagAddr, 1u);

    bool forcedLoop = false;
    if (!g_f50_6_forced_menu_loop && loopBefore == 3u)
    {
        dc2_write_u32(rdram, loopAddr, 0u); // LoopNo 0 = MenuInit/MenuLoop debug menu.
        g_f50_6_forced_menu_loop = true;
        forcedLoop = true;
    }

    const uint32_t flagAfter = dc2_read_u32(rdram, kDebugFlagAddr);
    const uint32_t loopAfter = dc2_read_u32(rdram, loopAddr);
    if (g_f50_6_debug_log_count < 16u &&
        (forcedLoop || flagBefore != 1u || g_f50_6_debug_log_count < 4u))
    {
        ++g_f50_6_debug_log_count;
        std::fprintf(stderr,
                     "[F50.6:debug] site=%s flag=0x%x->0x%x loop=%u->%u next=%u forcedLoop=%u gp=0x%x\n",
                     site ? site : "?", flagBefore, flagAfter, loopBefore, loopAfter,
                     nextBefore, forcedLoop ? 1u : 0u, gp);
        std::fflush(stderr);
    }
}

static void f50_6_menu_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);

    if (f50_6_debug_menu_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuInit.enter n=%u ra=0x%x flag=%u loop=%u next=%u a0=0x%x\n",
                     n, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 4));
    }

    MenuInit__F13INIT_LOOP_ARG_0x191970(rdram, ctx, runtime);

    if (f50_6_debug_menu_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuInit.exit n=%u flag=%u loop=%u next=%u ret=%u\n",
                     n,
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 2));
    }
}

static void f49_menu_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t preDebugFlag = dc2_read_u32(rdram, 0x00376FB8u);
    const uint32_t preMenuMode = dc2_read_u32(rdram, gp + 0xFFFF8B10u); // gp-0x74F0 menu_mode
    const uint32_t preSelect = dc2_read_u32(rdram, gp + 0xFFFF8B24u);  // gp-0x74DC select_1302
    const int32_t preArg = static_cast<int32_t>(dc2_read_u32(
        rdram, 0x00335430u + ((preSelect <= 13u) ? preSelect : 0u) * 4u)); // SelectArg[select]
    const uint32_t prePad = dc2_read_u32(rdram, 0x003D76E4u);          // CGamePad+0x04 current
    const uint32_t prePadPrev = dc2_read_u32(rdram, 0x003D777Cu);      // CGamePad+0x9C previous
    if (f50_6_debug_menu_enabled() && (n <= 64u || prePad != 0u))
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuLoop.enter n=%u frame=%u flag=%u loop=%u next=%u mode=%u select=%u arg=%d pad=0x%x down=0x%x\n",
                     n, g_f40_frame_counter, preDebugFlag, loopNo, nextLoopNo, preMenuMode,
                     preSelect, preArg, prePad, prePad & ~prePadPrev);
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuLoop__Fv_0x191c30(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f50_6_debug_menu_enabled() && (n <= 64u || getRegU32(ctx, 2) != 0u))
    {
        const uint32_t postSelect = dc2_read_u32(rdram, gp + 0xFFFF8B24u);
        const int32_t postArg = static_cast<int32_t>(dc2_read_u32(
            rdram, 0x00335430u + ((postSelect <= 13u) ? postSelect : 0u) * 4u));
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuLoop.exit n=%u frame=%u ret=%u flag=%u loop=%u next=%u mode=%u select=%u arg=%d\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2),
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     dc2_read_u32(rdram, gp + 0xFFFF8B10u),
                     postSelect, postArg);
    }
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 60u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=MenuLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync);
        }
    }
}

static void f49_menu_main_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const auto t0 = std::chrono::steady_clock::now();
    MenuMainLoop__Fv_0x233fc0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 60u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=MenuMainLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync);
        }
    }
}

static void f49_pause_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pauseReqAddr = gp + 0xFFFFA1A8u; // -0x5E58
    const uint32_t pauseReqBefore = dc2_read_u32(rdram, pauseReqAddr);
    const auto t0 = std::chrono::steady_clock::now();
    PauseLoop__Fv_0x309de0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t pauseReqAfter = dc2_read_u32(rdram, pauseReqAddr);
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const uint32_t ret = getRegU32(ctx, 2);
        if (n <= 16u || ret != 0u || (n % 120u) == 0u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:pause] n=%u ra=0x%x ret=%u pauseReq=%u->%u dtMs=%lld\n",
                         n, getRegU32(ctx, 31), ret, pauseReqBefore, pauseReqAfter,
                         static_cast<long long>(dtMs));
        }
    }
}

static void f49_update_gamepad_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    UpDate__8CGamePadFv_0x14a930(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f50_6_enable_debug_menu(rdram, ctx, "UpDate");
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=UpDate n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_update_pad_control_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    Update__11CPadControlFP8CGamePad_0x2ed550(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=PadControlUpdate n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_snd_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    sndStep__Ff_0x18d650(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=sndStep n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_scene_step_snd_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    StepSnd__6CSceneFv_0x2a7940(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=SceneStepSnd n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static int32_t f49_title_phase(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t raw = dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu;
    return static_cast<int16_t>(raw);
}

static uint32_t f49_title_mode(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    return titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
}

static void f49_title_mode_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const int32_t phaseBefore = f49_title_phase(rdram, ctx);
    const uint32_t modeBefore = f49_title_mode(rdram, ctx);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=TitleModeKey.enter n=%u ra=0x%x phase=%d mode=%u\n",
                     n, ra, phaseBefore, modeBefore);
    }

    const auto t0 = std::chrono::steady_clock::now();
    TitleModeKey__Fv_0x2a1220(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    const uint32_t pcAfter = ctx->pc;
    const uint64_t rawRet64 = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[2], 0));
    // Some recompiled callers compare full 64-bit GPRs. Normalize TitleModeKey's
    // return to canonical sign-extended 32-bit form.
    const int32_t rawRet32 = static_cast<int32_t>(getRegU32(ctx, 2));
    int32_t ret32 = rawRet32;
    if (ret32 == 5 && f49_remap_title_ret5_enabled())
        ret32 = 3;
    // G9: headless costume route. Force the New-Game return (5) once at press-start
    // (state 1). TitleLoop case-1 lVar10==5 runs InitSaveData + SetChrEquipDirect (so
    // the costume list is populated) + DAT_01ecd618=0x14 (the Select-Costume selector)
    // + TitleInfo[1]=2, whose commit calls MenuMainInit case 0x14 = MenuCostumeInit.
    if (g9_costume_enabled() && !g_g9_title_forced && modeBefore == 1u && n >= 120u)
    {
        ret32 = 5;
        g_g9_title_forced = true;
        std::fprintf(stderr, "[G9:costume] forced TitleModeKey ret=5 (New Game) at n=%u\n", n);
        std::fflush(stderr);
    }
    setReturnS32(ctx, ret32);

    if (f49_trace_title_path_enabled())
    {
        const int32_t phaseAfter = f49_title_phase(rdram, ctx);
        const uint32_t modeAfter = f49_title_mode(rdram, ctx);
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=TitleModeKey.exit n=%u ra=0x%x pc=0x%x rawRet64=0x%llx rawRet=%u ret=%u dtMs=%lld phase=%d->%d mode=%u->%u\n",
                         n, ra, pcAfter, static_cast<unsigned long long>(rawRet64),
                         static_cast<uint32_t>(rawRet32), static_cast<uint32_t>(ret32), static_cast<long long>(dtMs),
                         phaseBefore, phaseAfter, modeBefore, modeAfter);
        }
    }
}

static void f48_log_map_chain(const char *tag, uint8_t *rdram, R5900Context *ctx, uint32_t n)
{
    if (!f39_trace_map_entry_enabled() || n > 128u)
        return;

    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const char *path = reinterpret_cast<const char *>(getConstMemPtr(rdram, a1));
    char pathBuf[80] = {0};
    if (path != nullptr)
    {
        std::strncpy(pathBuf, path, sizeof(pathBuf) - 1u);
        pathBuf[sizeof(pathBuf) - 1u] = '\0';
    }

    const uint32_t arg5 = dc2_read_u32(rdram, sp + 0x10u);
    std::fprintf(stderr,
                 "[F48:map] tag=%s n=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x arg5=0x%x ra=0x%x gp=0x%x path='%s'\n",
                 tag, n, a0, a1, a2, a3, arg5, ra, gp, path ? pathBuf : "");
}

static void f48_log_loadmap_info(const char *tag, uint8_t *rdram, R5900Context *ctx,
                                 uint32_t n, uint32_t info)
{
    if (!f39_trace_map_entry_enabled() || n > 128u || info == 0u)
        return;
    if (getConstMemPtr(rdram, info) == nullptr)
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t stack = dc2_read_u32(rdram, info + 0x1A4u);
    const uint32_t readDataGlobal = dc2_read_u32(rdram, gp + 0xFFFF8D74u); // gp-0x728c BuffReadData
    const uint32_t stackBase = stack ? dc2_read_u32(rdram, stack + 0x20u) : 0u;
    const uint32_t stackOff = stack ? dc2_read_u32(rdram, stack + 0x24u) : 0u;
    const uint32_t stackLimit = stack ? dc2_read_u32(rdram, stack + 0x28u) : 0u;

    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u info=0x%x w0=0x%x stackSel=%u data=0x%x misc3=0x%x misc4=0x%x flag67=0x%x map68=0x%x stack69=0x%x stackBase=0x%x stackOff=0x%x stackLimit=0x%x readData=0x%x\n",
                 tag, n, info,
                 dc2_read_u32(rdram, info + 0x00u),
                 dc2_read_u32(rdram, info + 0x04u),
                 dc2_read_u32(rdram, info + 0x08u),
                 dc2_read_u32(rdram, info + 0x0Cu),
                 dc2_read_u32(rdram, info + 0x10u),
                 dc2_read_u32(rdram, info + 0x19Cu),
                 dc2_read_u32(rdram, info + 0x1A0u),
                 stack, stackBase, stackOff, stackLimit, readDataGlobal);
    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u mainInfo=[0x%x,0x%x,0x%x,0x%x,0x%x,0x%x] editStack base=0x%x off=0x%x limit=0x%x raw70=0x%x raw74=0x%x raw78=0x%x\n",
                 tag, n,
                 dc2_read_u32(rdram, 0x01F58D50u),
                 dc2_read_u32(rdram, 0x01F58D54u),
                 dc2_read_u32(rdram, 0x01F58D58u),
                 dc2_read_u32(rdram, 0x01F58D5Cu),
                 dc2_read_u32(rdram, 0x01F58D60u),
                 dc2_read_u32(rdram, 0x01F58D64u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x20u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x24u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x28u),
                 dc2_read_u32(rdram, 0x01E9EA70u),
                 dc2_read_u32(rdram, 0x01E9EA74u),
                 dc2_read_u32(rdram, 0x01E9EA78u));
    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u ptrs map=0x%x mapSize=0x%x cfg=0x%x cfgSize=0x%x mpk=0x%x ipk=0x%x efp=0x%x sky=0x%x total66=0x%x place64=0x%x add36=0x%x\n",
                 tag, n,
                 dc2_read_u32(rdram, info + 0x0B8u),
                 dc2_read_u32(rdram, info + 0x0BCu),
                 dc2_read_u32(rdram, info + 0x0C0u),
                 dc2_read_u32(rdram, info + 0x0C4u),
                 dc2_read_u32(rdram, info + 0x0C8u),
                 dc2_read_u32(rdram, info + 0x0CCu),
                 dc2_read_u32(rdram, info + 0x0D0u),
                 dc2_read_u32(rdram, info + 0x0D4u),
                 dc2_read_u32(rdram, info + 0x198u),
                 dc2_read_u32(rdram, info + 0x190u),
                 dc2_read_u32(rdram, info + 0x0D8u));
}

static void f48_log_map_chain_exit(const char *tag, uint8_t *rdram, R5900Context *ctx,
                                   uint32_t n, uint32_t ret, long long dtMs)
{
    if (!f39_trace_map_entry_enabled() || n > 128u)
        return;

    std::fprintf(stderr,
                 "[F48:map] tag=%s n=%u ret=0x%x pc=0x%x ra=0x%x dtMs=%lld gp=0x%x\n",
                 tag, n, ret, ctx->pc, getRegU32(ctx, 31), dtMs, getRegU32(ctx, 28));
}

static void f48_load_map_from_memory_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t info = getRegU32(ctx, 6);
    f48_log_map_chain("LoadMapFromMemory@285670.enter", rdram, ctx, n);
    f48_log_loadmap_info("LoadMapFromMemory@285670.enter", rdram, ctx, n, info);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFromMemory__6CSceneFiP17SCN_LOADMAP_INFO2_0x285670(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_loadmap_info("LoadMapFromMemory@285670.exit", rdram, ctx, n, info);
    f48_log_map_chain_exit("LoadMapFromMemory@285670.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f48_load_map_from_memory2_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t info = getRegU32(ctx, 7);
    f48_log_map_chain("LoadMapFromMemory@2856f0.enter", rdram, ctx, n);
    f48_log_loadmap_info("LoadMapFromMemory@2856f0.enter", rdram, ctx, n, info);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFromMemory__6CSceneFiiP17SCN_LOADMAP_INFO2_0x2856f0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_loadmap_info("LoadMapFromMemory@2856f0.exit", rdram, ctx, n, info);
    f48_log_map_chain_exit("LoadMapFromMemory@2856f0.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f50_7_seed_debug_map_stacks(uint8_t *rdram, uint32_t scene, uint32_t stackSel)
{
    if (!f50_6_debug_menu_enabled() || scene != 0x01DD8260u)
        return;

    static constexpr uint32_t kSceneStackCount = 0x0Cu;
    static constexpr uint32_t kStacks[] = {
        0x01E9EA50u, 0x01E9EA80u, 0x01E9EAB0u, 0x01E9EAE0u, 0x01E9EB10u,
        0x01E9EB40u, 0x01E9EB70u, 0x01E9EBA0u, 0x01E9EBD0u,
    };

    const uint32_t countBefore = dc2_read_u32(rdram, scene + 0x00u);
    const uint32_t mapCountBefore = dc2_read_u32(rdram, scene + 0x27E0u);
    const uint32_t requestedBefore =
        (stackSel < kSceneStackCount) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u;
    uint32_t repaired = 0u;

    if (countBefore < kSceneStackCount)
        dc2_write_u32(rdram, scene + 0x00u, kSceneStackCount);

    for (uint32_t i = 0u; i < static_cast<uint32_t>(sizeof(kStacks) / sizeof(kStacks[0])); ++i)
    {
        const uint32_t slotAddr = scene + 0x08u + i * 4u;
        if (dc2_read_u32(rdram, slotAddr) != 0u)
            continue;
        const uint32_t stack = kStacks[i];
        if (dc2_read_u32(rdram, stack + 0x20u) == 0u || dc2_read_u32(rdram, stack + 0x28u) == 0u)
            continue;
        dc2_write_u32(rdram, slotAddr, stack);
        ++repaired;
    }

    if (mapCountBefore == 0u)
    {
        dc2_write_u32(rdram, scene + 0x0040u, 0x80u); // character slots
        dc2_write_u32(rdram, scene + 0x2044u, 8u);    // camera slots
        dc2_write_u32(rdram, scene + 0x2208u, 8u);    // message slots
        dc2_write_u32(rdram, scene + 0x27E0u, 4u);    // map slots
        dc2_write_u32(rdram, scene + 0x28C4u, 4u);    // sky slots
        dc2_write_u32(rdram, scene + 0x29A8u, 4u);    // game object slots
        dc2_write_u32(rdram, scene + 0x2AACu, 8u);    // effect slots
        dc2_write_u32(rdram, scene + 0x2E50u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E54u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E58u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E5Cu, 0xFFFFFFFFu);

        if (uint8_t *mapSlots = getMemPtr(rdram, scene + 0x27E4u))
            std::memset(mapSlots, 0, 4u * 0x38u);
        if (uint8_t *skySlots = getMemPtr(rdram, scene + 0x28C8u))
            std::memset(skySlots, 0, 4u * 0x38u);
    }

    if (f50_7_trace_selected_map_enabled() && (repaired != 0u || mapCountBefore == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:map-stack] scene=0x%x count=%u->%u mapCount=%u->%u stackSel=%u slot=0x%x->0x%x repaired=%u e1.base=0x%x e1.off=0x%x e1.limit=0x%x\n",
                     scene, countBefore, dc2_read_u32(rdram, scene + 0x00u),
                     mapCountBefore, dc2_read_u32(rdram, scene + 0x27E0u),
                     stackSel, requestedBefore,
                     (stackSel < kSceneStackCount) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u,
                     repaired,
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x20u),
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x24u),
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x28u));
    }
}
