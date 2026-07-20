static const uint32_t kG58RaSlot = 0x1FFF800u; // TitleMapDraw sp+0xA0 (saved $ra)

static bool g58_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G58");
    return enabled;
}

static void g58_canary(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                       const char *tag,
                       void (*real)(uint8_t *, R5900Context *, PS2Runtime *))
{
    static unsigned s_logBudget = 0u;
    const uint32_t entrySp = getRegU32(ctx, 29);
    const uint32_t slotBefore = dc2_read_u32(rdram, kG58RaSlot);

    if (g58_trace_enabled() && s_logBudget < 4000u)
    {
        // First few entries per tag: capture the call-depth (sp) profile + the slot value.
        static const char *s_seenTags[16] = {nullptr};
        static unsigned s_tagCount[16] = {0u};
        unsigned ti = 0u;
        for (; ti < 16u && s_seenTags[ti]; ++ti)
            if (s_seenTags[ti] == tag) break;
        if (ti < 16u)
        {
            if (!s_seenTags[ti]) s_seenTags[ti] = tag;
            if (s_tagCount[ti] < 8u)
            {
                ++s_tagCount[ti];
                ++s_logBudget;
                std::fprintf(stderr, "[G58:enter] tag=%s entrySp=0x%x slot(0x1fff800)=0x%x%s\n",
                             tag, entrySp, slotBefore,
                             (entrySp >= 0x1FFF760u) ? "  <-- OVERLAPS TitleMapDraw FRAME" : "");
                std::fflush(stderr);
            }
        }
    }

    real(rdram, ctx, runtime);

    const uint32_t slotAfter = dc2_read_u32(rdram, kG58RaSlot);
    const uint32_t exitSp = getRegU32(ctx, 29);
    if (g58_trace_enabled() && slotBefore != slotAfter)
    {
        // Innermost link prints first (it returns first) => first [G58:slotflip] = culprit.
        std::fprintf(stderr,
                     "[G58:slotflip] tag=%s 0x1fff800: 0x%x -> 0x%x | entrySp=0x%x exitSp=0x%x pc=0x%x\n",
                     tag, slotBefore, slotAfter, entrySp, exitSp, ctx->pc);
        std::fflush(stderr);
    }
}

static void g58_canary_map_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__4CMap@160b10", f50_7_map_draw_probe); }
static void g58_canary_editmap_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__8CEditMap@1b4130", DrawSub__8CEditMapFi_0x1b4130); }
static void g58_canary_map_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__4CMap@15e250", DrawSub__4CMapFi_0x15e250); }
static void g58_canary_parts_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__9CMapParts@15e3d0", g56_mapparts_draw_tap); }
static void g58_canary_parts_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__9CMapParts@166a70", DrawSub__9CMapPartsFi_0x166a70); }
static void g58_canary_piece_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__9CMapPiece@166e40", g56_mappiece_draw_tap); }
static void g58_canary_objframe_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__12CObjectFrame@169fd0", Draw__12CObjectFrameFv_0x169fd0); }
static void g58_canary_mgframe_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__8mgCFrame@137e10", Draw__8mgCFrameFPUi_0x137e10); }
static void g58_canary_visualmdt_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__12mgCVisualMDT@13f4e0", Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0); }

// G58: log r31 (return addr) + a0 (the mgCCameraFollow `this`) on entry to the title
// camera-follow accessors, and ctx->pc on exit. The crash trace ends with AddHeight x3 then
// a jump to 0x9f84a0; the 3 TitleMapDraw call sites set r31 to constants (0x2a2774/a4/d4), so
// an entry r31 of 0x9f84a0 (or an exit pc of 0x9f84a0) pins where the bad return addr enters.
static void g58_camera_accessor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                const char *tag,
                                void (*real)(uint8_t *, R5900Context *, PS2Runtime *))
{
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t a0Before = getRegU32(ctx, 4);
    const bool repaired = g59_repair_title_camera_arg(rdram, ctx, tag);
    const uint32_t a0 = getRegU32(ctx, 4);
    static unsigned budget = 0u;
    const bool flag = (a0Before == 0u) || repaired || (ra < 0x100000u) || (ra >= 0x300000u); // suspicious ra/this
    real(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, tag);
    const uint32_t outPc = ctx->pc;
    if (g58_trace_enabled() && (flag || outPc == 0x9f84a0u) && budget < 60u)
    {
        ++budget;
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t slotLo = dc2_read_u32(rdram, sp + 0xA0u);
        const uint32_t slotHi = dc2_read_u32(rdram, sp + 0xA4u);
        std::fprintf(stderr, "[G58:cam] %s entry_ra=0x%x a0=0x%x usedA0=0x%x repaired=%u sp=0x%x slot=0x%x:%x -> exit_pc=0x%x%s\n",
                     tag, ra, a0Before, a0, repaired ? 1u : 0u,
                     sp, slotHi, slotLo, outPc,
                     (outPc == 0x9f84a0u) ? "  <-- BAD JUMP" : "");
        std::fflush(stderr);
    }
}
static void g58_cam_adddistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "AddDistance@131a20", AddDistance__15mgCCameraFollowFf_0x131a20); }
static void g58_cam_getdistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "GetDistance@131a10", GetDistance__15mgCCameraFollowFv_0x131a10); }
static void g58_cam_addheight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "AddHeight@131a50", AddHeight__15mgCCameraFollowFf_0x131a50); }

// G58: log AssignCamera(scene,id,camera,name) args + the resulting slot.obj. TitleInit
// assigns the title camera into scene slot 0/1; the runner leaves slot0.obj=0 (HW=0x785f70).
static void g58_assign_camera_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t id    = getRegU32(ctx, 5);
    const uint32_t cam   = getRegU32(ctx, 6);
    const uint32_t name  = getRegU32(ctx, 7);
    const uint32_t ra    = getRegU32(ctx, 31);
    AssignCamera__6CSceneFiP9mgCCameraPc_0x283740(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t slotObj = (scene && id < 8u) ? dc2_read_u32(rdram, scene + 0x2048u + 56u*id + 0x34u) : 0xFFFFFFFFu;
    static unsigned n = 0u;
    if (g58_trace_enabled() && n < 40u)
    {
        ++n;
        std::fprintf(stderr, "[G58:assigncam] scene=0x%x id=%u cam=0x%x name=0x%x ra=0x%x -> ret=%d slot%u.obj=0x%x\n",
                     scene, id, cam, name, ra, (int)ret, id, slotObj);
        std::fflush(stderr);
    }
}

// G193: town/edit-map flat-blue camera diagnosis. Orders, within the EditInit frame,
// (a) the scene->Initialize() virtual dispatch (sets count=8, active=-1, clears slots),
// (b) EditInit's 5 AssignCamera calls, and (c) any later writer that re-resets the state —
// to pin why the runner ends the MAP-0 route at count=8/active=-1/slot0.obj=0 (flat blue
// via the G127 zero-view-matrix mechanism) while real HW ends count=8/active=0/slots
// populated (PCSX2 A/B, phase G192/G193). Quiet unless DC2_TRACE_G193.
static bool g193_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G193");
    return enabled;
}

static void g193_scene_snapshot(uint8_t *rdram, const char *tag, uint32_t scene, uint32_t ra)
{
    if (!g193_trace_enabled() || scene == 0u)
        return;
    static unsigned s_lines = 0u;
    if (s_lines >= 64u)
        return;
    ++s_lines;
    std::fprintf(stderr,
                 "[G193:%s] scene=0x%x ra=0x%x vptr=0x%x count=%d active=%d slot0(w0=0x%x,obj=0x%x)\n",
                 tag, scene, ra,
                 dc2_read_u32(rdram, scene + 0x10548u),
                 (int)dc2_read_u32(rdram, scene + 0x2044u),
                 (int)dc2_read_u32(rdram, scene + 0x2E54u),
                 dc2_read_u32(rdram, scene + 0x2048u + 0x00u),
                 dc2_read_u32(rdram, scene + 0x2048u + 0x34u));
    std::fflush(stderr);
}

static void g193_scene_initialize_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    g193_scene_snapshot(rdram, "SceneInit.enter", scene, ra);
    Initialize__6CSceneFv_0x282ea0(rdram, ctx, runtime);
    g193_scene_snapshot(rdram, "SceneInit.exit", scene, ra);
}

// forward decl (defined with the F50.4 dungeon-route repair below)
static void f50_4_repair_main_scene_vtable(uint8_t *rdram);

// G193 FIX (default-ON, kill DC2_G193_NO_SCENEVT_FIX=1): the F50.4 MainScene-vtable
// repair, applied at EditInit entry. Root: __sinit_mainloop@0x373580 does not take
// effect headless, so MainScene+0x10548 (__vt__6CScene) is 0. F50.4 repaired this at
// InitDungeonMain entry only; the town/edit route (LoopInit[1] = EditInit@0x1A9F40) has
// the IDENTICAL scene-entry dispatch (0x1AA930: lw t9,0x548(scene+0x10000); lw t9,0x8(t9);
// jalr) which silently no-ops on the null vtable -> Initialize__6CScene never runs ->
// camera count stays 0 -> EditInit's 5 AssignCamera calls all fail -> no active camera ->
// zero view matrix -> every map part frustum-culled -> flat blue (G127 mechanism, G192/G193
// diagnosis). Same idempotent restore-the-boot-invariant pattern as the dungeon fix.
static void g193_edit_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_dc2TownDepthScope.store(false, std::memory_order_relaxed);
    static const bool s_noFix = dc2_env_flag_enabled("DC2_G193_NO_SCENEVT_FIX");
    if (!s_noFix)
        f50_4_repair_main_scene_vtable(rdram);
    const uint32_t ra = getRegU32(ctx, 31);
    g193_scene_snapshot(rdram, "EditInit.enter", 0x01DD8260u, ra);
    EditInit__F13INIT_LOOP_ARG_0x1a9f40(rdram, ctx, runtime);
    g193_scene_snapshot(rdram, "EditInit.exit", 0x01DD8260u, ra);
}

static void g193_assign_camera_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t id = getRegU32(ctx, 5);
    const uint32_t cam = getRegU32(ctx, 6);
    const uint32_t name = getRegU32(ctx, 7);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t countBefore = scene ? dc2_read_u32(rdram, scene + 0x2044u) : 0u;
    AssignCamera__6CSceneFiP9mgCCameraPc_0x283740(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t slotObj = (scene && id < 8u) ? dc2_read_u32(rdram, scene + 0x2048u + 56u * id + 0x34u) : 0xFFFFFFFFu;
    static unsigned n = 0u;
    if (g193_trace_enabled() && n < 40u)
    {
        ++n;
        std::fprintf(stderr,
                     "[G193:assigncam] scene=0x%x id=%u cam=0x%x name=0x%x ra=0x%x countBefore=%d -> ret=%d slot%u.obj=0x%x active=%d\n",
                     scene, id, cam, name, ra, (int)countBefore, (int)ret, id, slotObj,
                     scene ? (int)dc2_read_u32(rdram, scene + 0x2E54u) : -999);
        std::fflush(stderr);
    }
}

static void f48_load_map_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f48_log_map_chain("LoadMapFile@164480.enter", rdram, ctx, n);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFile__4CMapFPciP9mgCMemoryi_0x164480(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_map_chain_exit("LoadMapFile@164480.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f48_create_map_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f48_log_map_chain("CreateMap@1600d0.enter", rdram, ctx, n);
    const auto t0 = std::chrono::steady_clock::now();
    CreateMap__4CMapFP11CMdsListSetP9mgCMemory_0x1600d0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_map_chain_exit("CreateMap@1600d0.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f49_title_mccheck_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=TitleMCCheckKey.enter n=%u ra=0x%x\n",
                     n, ra);
    }

    const auto t0 = std::chrono::steady_clock::now();
    TitleMCCheckKey__Fv_0x2a2ad0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=TitleMCCheckKey.exit n=%u ra=0x%x ret=%u dtMs=%lld\n",
                         n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
        }
    }
}

static void f49_mc_manager_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t self = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MCStep.enter n=%u ra=0x%x self=0x%x\n",
                     n, ra, self);
    }

    const auto t0 = std::chrono::steady_clock::now();
    Step__18CMemoryCardManagerFv_0x2f1fc0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=MCStep.exit n=%u ra=0x%x self=0x%x ret=%u dtMs=%lld\n",
                         n, ra, self, getRegU32(ctx, 2), static_cast<long long>(dtMs));
        }
    }
}

static void f49_menu_check_push_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    MenuCheckPushButton__Fv_0x23e1b0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuCheckPush n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_convert_check_push_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t in = getRegU32(ctx, 4);
    ConvertCheckPushButton__Fi_0x23e2e0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=ConvertPush n=%u ra=0x%x in=0x%x out=0x%x\n",
                     n, ra, in, getRegU32(ctx, 2));
    }
}

static void f49_menu_main_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t arg = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainInit.enter n=%u ra=0x%x arg=0x%x\n",
                     n, ra, arg);
    }

    const auto t0 = std::chrono::steady_clock::now();
    MenuMainInit__FP13MENU_INIT_ARG_0x232df0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainInit.exit n=%u ra=0x%x ret=0x%x dtMs=%lld\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
    }
}

// G9: headless Select-Costume route. Scripted pad input cannot pass MenuMainKey
// (route-B / F49 blocker), so we drive the MenuMain state machine directly. At the
// title New-Game/Load menu (LoopNo=3, TitleInfo[0]=2) we set MenuCommonInfo+0x50=0x14
// (the costume-select init selector) and invoke MenuMainInit, which runs case 0x14 =
// MenuCostumeInit and advances +0x54 to 0x17 (the costume KeyStep). From then on the
// game's own per-frame MenuMainKey runs the costume menu. Env-gated DC2_G9_COSTUME so
// the golden title/treemap smoke is never perturbed.
static bool g9_costume_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G9_COSTUME");
    return e;
}

static bool g36_packet_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G36_PACKET");
    return enabled;
}

static bool g43_face_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G43_FACE");
    return enabled;
}
static bool g9_auto_confirm_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G9_AUTO_CONFIRM");
    return enabled;
}
static int g_g9_phase = 0;     // 0=warmup, 1=armed, 2=done
static uint32_t g_g9_warm = 0u;
// G10: optional phase-4 Cross pulse for post-prompt experiments. Keep the default
// costume capture parked on the "Select Max's costume." prompt so it matches the
// hardware reference; set DC2_G9_AUTO_CONFIRM=1 to advance into the interactive list.
static std::atomic<uint32_t> g_g9_confirm_mask{0u};

static void f49_menu_main_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t lastTarget = 0xFFFFFFFFu;
    static uint32_t lastStep = 0xFFFFFFFFu;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t preMenuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    const uint32_t preMenuStep = preMenuPtr ? dc2_read_u32(rdram, preMenuPtr + 0x54u) : 0xFFFFFFFFu;
    const uint32_t tableTarget = (preMenuStep < 128u) ? dc2_read_u32(rdram, 0x00350900u + preMenuStep * 4u) : 0u;
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainKey.enter n=%u ra=0x%x\n",
                     n, ra);
    }
    // G12 FIX: __sinit_menudraw.cpp@0x374310 (which Set's the front-end menu UV rects)
    // does not run headless, so menu_long_hand@0x1ECCE50 ג€” the long-hand cursor's atlas
    // UV rect ג€” stays (0,0,0,0). MenuCursorDraw@0x223a50 then samples a zero-size region
    // at texel (0,0) of the fukusel atlas (block 0x2c20), so the Select-Costume / menu
    // cursor renders as a solid black-transparent square instead of the hand pointer.
    // Restore the value __sinit_menudraw sets, (U,V,W,H)=(0x3e,1,0x28,0x18); verified
    // against live PCSX2 (menu_long_hand=(62,1,40,24)). Idempotent: only writes when the
    // rect is still zero. Same un-run-__sinit class as f50_4/f50_7. The cursor texObj
    // bind (tbp=0x2c20 cbp=0x3fd4) was already byte-identical to HW ג€” only the UV was bad.
    {
        constexpr uint32_t kMenuLongHand = 0x01ECCE50u;
        if (dc2_read_u32(rdram, kMenuLongHand + 0x0u) == 0u &&
            dc2_read_u32(rdram, kMenuLongHand + 0x8u) == 0u)
        {
            dc2_write_u32(rdram, kMenuLongHand + 0x0u, 0x3Eu); // U
            dc2_write_u32(rdram, kMenuLongHand + 0x4u, 0x01u); // V
            dc2_write_u32(rdram, kMenuLongHand + 0x8u, 0x28u); // W
            dc2_write_u32(rdram, kMenuLongHand + 0xCu, 0x18u); // H
        }
    }
    // G13 FIX (Known Issue #4 ג€” costume NAMES): the CMenuCostumeSel ctor (0x2bc500) caches
    // the player char-data ptr at MenuCosPtr+0x2d0 = GetCharaDataPtr(GetUserDataMan(),0)
    // (= ActiveSaveData+0x1d2a0+0x3f48). Draw@0x2bd640 only renders the three costume NAMES
    // (Hunting Cap / Denim Overalls / Leather Shoes, via GetName@0x197700) when that ptr is
    // non-zero. In the headless route the ctor runs before ActiveSaveData is funded, so it
    // cached 0 (verified: [G13:state] charData=0x0 while ActiveSaveData=0x1e01810 is valid and
    // byte-identical to HW). The name STRINGS are static ELF message tables (0x3361f0/0x365858),
    // already present ג€” only the cached ptr is wrong. Idempotently repair it (same un-run-init
    // / stale-cache class as the menu_long_hand fix above and f50_4/f50_7). Gated on the costume
    // being the active menu (MenuCosutumeLoadPhase 1..4) so a stale MenuCosPtr can't be touched.
    {
        const uint32_t phase = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu; // MenuCosutumeLoadPhase
        if (phase >= 1u && phase <= 4u)
        {
            const uint32_t mcp = dc2_read_u32(rdram, 0x00378120u); // MenuCosPtr (gp-0x63d0)
            if (mcp > 0x80000u && mcp < 0x2000000u &&
                dc2_read_u32(rdram, mcp + 0x2D0u) == 0u)
            {
                const uint32_t asd = dc2_read_u32(rdram, 0x00376FE4u); // ActiveSaveData (gp-0x750c)
                if (asd > 0x80000u && asd < 0x2000000u)
                    dc2_write_u32(rdram, mcp + 0x2D0u, asd + 0x1D2A0u + 0x3F48u);
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuMainKey__Fv_0x233ff0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainKey.exit n=%u ra=0x%x ret=0x%x dtMs=%lld\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
    }
    if (f48_2_trace_charbg_enabled() && g_f48_2_log_count < 128u &&
        (n <= 16u || preMenuStep != lastStep || tableTarget != lastTarget || (n % 30u) == 0u))
    {
        const uint32_t postMenuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
        const uint32_t postMenuStep = postMenuPtr ? dc2_read_u32(rdram, postMenuPtr + 0x54u) : 0xFFFFFFFFu;
        const int32_t postMenuSel = postMenuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, postMenuPtr + 0x50u) & 0xFFFFu) : -1;
        const int32_t readNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84ECu) & 0xFFFFu); // gp-0x7B14
        const int32_t statusBit = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C18u) & 0xFFFFu); // gp-0x63E8
        const int32_t moveX = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C0Cu)); // gp-0x63F4
        const int32_t movePhase = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C10u)); // gp-0x63F0
        const int32_t readPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C00u) & 0xFFFFu); // gp-0x6400
        const int32_t screenWidth = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8780u)); // gp-0x7880
        const uint32_t nowReadMainChara = dc2_read_u32(rdram, gp + 0xFFFF9C04u); // gp-0x63FC
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=MenuMainKey.dispatch n=%u frame=%u ra=0x%x menu=0x%x step=%u->%u sel=%d target=0x%x ret=%u readNo=%d status=0x%x moveX=%d movePhase=%d readPhase=%d screenWidth=%d chara=0x%x block='%s'\n",
                     n, g_f40_frame_counter, ra, postMenuPtr, preMenuStep, postMenuStep,
                     postMenuSel, tableTarget, getRegU32(ctx, 2), readNo,
                     static_cast<uint32_t>(statusBit), moveX, movePhase,
                     readPhase, screenWidth, nowReadMainChara,
                     (tableTarget == 0x002BC150u) ? "dispatches_KeyMainCharaBG" : "runtime_target_not_KeyMainCharaBG");
        ++g_f48_2_log_count;
    }
    lastTarget = tableTarget;
    lastStep = preMenuStep;

    // G9: log once when the MenuMain state machine has entered costume-select
    // (the title case-2 commit ran MenuMainInit with selector 0x14). +0x50=0x14,
    // +0x54=0x17 (costume KeyStep) is the live PCSX2 signature.
    if (g9_costume_enabled() && g_g9_phase < 2)
    {
        const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // MenuCommonInfo
        if (menuPtr != 0u)
        {
            const uint32_t sel = dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu;
            const uint32_t step = dc2_read_u32(rdram, menuPtr + 0x54u);
            if (sel == 0x14u || step == 0x17u)
            {
                if (g_g9_phase == 0)
                    std::fprintf(stderr, "[G9:costume] in costume-select sel=0x%x step=0x%x menuPtr=0x%x\n",
                                 sel, step, menuPtr);
                g_g9_phase = 2;
                std::fflush(stderr);
            }
        }
    }
    // G10: trace the costume load-phase state machine (MenuCosutumeLoadPhase @ gp-0x63dc
    // = 0x378114, halfword). The "Select Max's costume." prompt persists until the
    // costume char/texture BG read completes (phase 2 -> 3 gated on ReadBGSync()==0),
    // then a 15-frame settle reaches phase 4 (interactive list). Find where it sticks.
    if (g9_costume_enabled() && g_g9_phase == 2)
    {
        static uint32_t s_last = 0xFFFFFFFFu;
        static uint32_t s_n = 0u;
        const uint32_t ph = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu;
        if (ph != s_last || (++s_n % 120u) == 0u)
        {
            std::fprintf(stderr, "[G10:loadphase] MenuCosutumeLoadPhase=%u\n", ph);
            std::fflush(stderr);
            s_last = ph;
        }
        // G11: one-shot dump of the costume sheet filename (LoadFileMenu arg @0x36f5a8).
        {
            static bool s_once = false;
            if (!s_once)
            {
                s_once = true;
                const char *fn = reinterpret_cast<const char *>(getConstMemPtr(rdram, 0x0036F5A8u));
                char nm[64] = {0};
                if (fn) { std::strncpy(nm, fn, sizeof(nm) - 1u); }
                std::fprintf(stderr, "[G11:sheet] LoadFileMenu name@0x36f5a8='%s'\n", nm);
                std::fflush(stderr);
            }
        }
        // G10: phase-4 frame counter for diagnostics, and optional prompt confirm
        // pulse. Default stays on the HW-reference prompt; DC2_G9_AUTO_CONFIRM=1
        // restores the old "advance into the list" behavior.
        if (ph >= 4u)
        {
            static uint32_t s_cf = 0u;
            const uint32_t phase4Frame = s_cf++;
            if (g9_auto_confirm_enabled())
            {
                const uint32_t cyc = phase4Frame % 48u;
                g_g9_confirm_mask.store(cyc < 6u ? 0x4000u : 0u, std::memory_order_relaxed);
            }
            else
            {
                g_g9_confirm_mask.store(0u, std::memory_order_relaxed);
            }

            // G12: A/B the costume texObj binding vs live HW. HW @ populated list:
            // mgr(0x381ef0) base=0x2720, costume mgCTexture (*(MenuCosPtr+0x2d4)) idx=0x54
            // tbp=0x2720; slot[0x54] = {0x2720/sz0x200, 0x2920/0x300, 0x2c20/0x100}.
            // If the runner binds the bars to 0x2aa0, this pins whether the mgr base,
            // the slot accumulation, or a stale +0x38 diverges. Throttled, bounded.
            // G12 verification aid (DC2_G12_FORCE_CURSOR, default off): the scripted
            // headless route never sets the cursor-active gate (MenuCosPtr+0x14a), so the
            // cursor does not draw and the menu_long_hand UV fix can't be seen in a frame.
            // Force the gate so a capture shows the hand cursor render. Test-only.
            {
                const uint32_t mcpF = dc2_read_u32(rdram, 0x00378120u);
                if (mcpF > 0x80000u && mcpF < 0x2000000u && dc2_env_flag_enabled("DC2_G12_FORCE_CURSOR"))
                    dc2_write_u32(rdram, mcpF + 0x294u, 1u);
            }

            static uint32_t s_g12n = 0u;
            const uint32_t g12c = s_cf;
            if (g12c == 6u || g12c == 90u)
            {
                ++s_g12n;
                const uint32_t mgr = 0x00381EF0u;
                const uint32_t base = dc2_read_u32(rdram, mgr + 0x00u) & 0x3FFFu;
                const uint32_t cacheIdx = dc2_read_u32(rdram, mgr + 0x08u);
                const uint32_t count = dc2_read_u32(rdram, mgr + 0x0Cu);
                const uint32_t slotArr = dc2_read_u32(rdram, mgr + 0x10u);
                const uint32_t mcp = dc2_read_u32(rdram, 0x00378120u); // MenuCosPtr (gp-0x63d0)
                std::fprintf(stderr, "[G12:mgr] base=0x%x cacheIdx=0x%x count=%u slotArr=0x%x MenuCosPtr=0x%x\n",
                             base, cacheIdx, count, slotArr, mcp);
                // G13 (Known Issue #4): costume NAMES + 3D Max model. A/B vs live HW
                // (populated list, phase 4): charData(MenuCosPtr+0x2d0)=0x1e229f8,
                // convtbl_5277@0x3769EC=[2,4,3], item entries type=4 sub=0x6f/0x104/0x75,
                // MenuActionChara@0x1f0caa0=0xebf9c0 vtable=0x3756f0, nametbl@0x1e9b060 has data.
                {
                    const uint32_t ph = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu;
                    const uint32_t charData = (mcp > 0x80000u && mcp < 0x2000000u)
                                                  ? dc2_read_u32(rdram, mcp + 0x2D0u) : 0u;
                    const uint32_t mac = dc2_read_u32(rdram, 0x01F0CAA0u); // MenuActionChara[0]
                    const uint32_t macVt = (mac > 0x80000u && mac < 0x2000000u)
                                               ? dc2_read_u32(rdram, mac) : 0u;
                    const uint32_t ntg = dc2_read_u32(rdram, 0x01E9B060u);
                    const uint32_t modelTexIdx = (mcp > 0x80000u && mcp < 0x2000000u)
                                                     ? dc2_read_u32(rdram, mcp + 0x24u) : 0u;
                    const uint32_t asd = dc2_read_u32(rdram, 0x00376FE4u); // ActiveSaveData (gp-0x750c)
                    const uint32_t udm = asd ? (asd + 0x1D2A0u) : 0u;
                    std::fprintf(stderr, "[G13:state] phase=%u charData=0x%x MenuActionChara=0x%x vtable=0x%x nametbl@1e9b060=0x%x modelTexIdx=0x%x ActiveSaveData=0x%x udm=0x%x udm+0x3f48=0x%x\n",
                                 ph, charData, mac, macVt, ntg, modelTexIdx, asd, udm, udm ? udm + 0x3F48u : 0u);
                    // G13 model scope: dump CActionChara model-related fields. HW (populated
                    // list) MenuActionChara[0] holds a built model (mgCFrame/mesh + "body").
                    if (mac > 0x80000u && mac < 0x2000000u)
                    {
                        std::fprintf(stderr, "[G13:model] mac+0x4..0x40:");
                        for (uint32_t o = 0x04u; o <= 0x40u; o += 4u)
                            std::fprintf(stderr, " %x:0x%x", o, dc2_read_u32(rdram, mac + o));
                        std::fprintf(stderr, "\n[G13:model] mac+0x110..0x140:");
                        for (uint32_t o = 0x110u; o <= 0x140u; o += 4u)
                            std::fprintf(stderr, " %x:0x%x", o, dc2_read_u32(rdram, mac + o));
                        std::fprintf(stderr, "\n");
                    }
                    if (charData > 0x80000u && charData < 0x2000000u)
                    {
                        const uint32_t convWord = dc2_read_u32(rdram, 0x003769ECu);
                        for (int i = 0; i < 3; ++i)
                        {
                            const uint32_t cv = (convWord >> (i * 8)) & 0xFFu;
                            const uint32_t entry = charData + cv * 0x6Cu + 0x170u;
                            const int32_t type = (int16_t)(dc2_read_u32(rdram, entry) & 0xFFFFu);
                            const int32_t sub = (int16_t)(dc2_read_u32(rdram, entry + 2u) & 0xFFFFu);
                            std::fprintf(stderr, "[G13:item] slot%d convtbl=%u entry=0x%x type=%d sub=%d(0x%x)\n",
                                         i, cv, entry, type, sub, (uint32_t)(sub & 0xFFFF));
                        }
                    }
                }
                if (mcp > 0x80000u && mcp < 0x2000000u && slotArr != 0u)
                {
                    const uint32_t P = dc2_read_u32(rdram, mcp + 0x2D4u); // costume mgCTexture
                    if (P > 0x80000u && P < 0x2000000u)
                    {
                        const uint32_t idx = dc2_read_u32(rdram, P) & 0xFFFFu;
                        const uint32_t tex0lo = dc2_read_u32(rdram, P + 0x38u);
                        std::fprintf(stderr, "[G12:texObj] P=0x%x idx=0x%x +0x38.tbp0=0x%x size=0x%x next=0x%x\n",
                                     P, idx, tex0lo & 0x3FFFu, dc2_read_u32(rdram, P + 0x28u),
                                     dc2_read_u32(rdram, P + 0x68u));
                    }
                    // G12: cursor texObj (MenuCosPtr+0x2d8 = +0x16c shorts) + UV rect
                    // menu_long_hand@0x1ecce50. HW: cursor tbp=0x2c20 cbp=0x3fd4 tbw=4 T8,
                    // UV=(62,1,40,24), enable(+0x294)=1. The recomp draws it as a black square.
                    {
                        const uint32_t cur = dc2_read_u32(rdram, mcp + 0x2D8u);
                        const uint32_t en = dc2_read_u32(rdram, mcp + 0x294u);
                        const uint32_t u = dc2_read_u32(rdram, 0x01ECCE50u);
                        const uint32_t v = dc2_read_u32(rdram, 0x01ECCE54u);
                        const uint32_t w = dc2_read_u32(rdram, 0x01ECCE58u);
                        const uint32_t h = dc2_read_u32(rdram, 0x01ECCE5Cu);
                        if (cur > 0x80000u && cur < 0x2000000u)
                        {
                            const uint32_t t0 = dc2_read_u32(rdram, cur + 0x38u);
                            const uint32_t t0h = dc2_read_u32(rdram, cur + 0x3Cu);
                            std::fprintf(stderr, "[G12:cursor] obj=0x%x idx=0x%x tbp=0x%x tbw=%u psm=0x%x cbp=0x%x size=0x%x src=0x%x enable=%u UV=(%d,%d,%d,%d)\n",
                                         cur, dc2_read_u32(rdram, cur) & 0xFFFFu, t0 & 0x3FFFu, (t0 >> 14) & 0x3Fu,
                                         (t0 >> 20) & 0x3Fu, (t0h >> 5) & 0x3FFFu, dc2_read_u32(rdram, cur + 0x28u),
                                         dc2_read_u32(rdram, cur + 0x50u), en, (int)u, (int)v, (int)w, (int)h);
                        }
                        else
                            std::fprintf(stderr, "[G12:cursor] obj=0x%x (null/oob) enable=%u UV=(%d,%d,%d,%d)\n",
                                         cur, en, (int)u, (int)v, (int)w, (int)h);
                    }
                    // G12: scan the slot array for the tbw=8 common-menu-frame texObjs (the
                    // costume row-bar atlas). HW slot[64] chains 0x2720->0x2aa0->... tbw=8 with
                    // valid src(+0x50) ptrs. Report tbp/tbw/psm + src so we can tell whether the
                    // runner's common frame is loaded (src!=0) and where it binds vs HW's 0x2aa0.
                    for (uint32_t s = 0; s < count && s < 130u; ++s)
                    {
                        uint32_t cur = dc2_read_u32(rdram, slotArr + s * 0x10u + 8u);
                        for (int d = 0; d < 4 && cur > 0x80000u && cur < 0x2000000u; ++d)
                        {
                            const uint32_t t0 = dc2_read_u32(rdram, cur + 0x38u);
                            const uint32_t tbp = t0 & 0x3FFFu;
                            const uint32_t tbw = (t0 >> 14) & 0x3Fu;
                            const uint32_t psm = (t0 >> 20) & 0x3Fu;
                            if (tbp == 0x2aa0u || (tbw == 8u && (tbp == 0x2720u || tbp == 0x2b20u || tbp == 0x2f20u)))
                                std::fprintf(stderr, "[G12:slot] s=%u d=%d obj=0x%x tbp=0x%x tbw=%u psm=0x%x size=0x%x src=0x%x\n",
                                             s, d, cur, tbp, tbw, psm, dc2_read_u32(rdram, cur + 0x28u),
                                             dc2_read_u32(rdram, cur + 0x50u));
                            cur = dc2_read_u32(rdram, cur + 0x68u);
                        }
                    }
                }
                std::fflush(stderr);
            }
        }
    }
}

// G11: trace the costume sheet load + texture-manager IMG entry. The bars sample
// the texture from fukusel.img (LoadFileMenu @0x36f5a8 -> EnterIMGFile -> GetTexture).
// If the load fails or the IMG enters at a block != the drawn 0x2720, the bars fall
// back to stale title-atlas data at 0x2aa0.
static void g11_loadfilemenu_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a0 = getRegU32(ctx, 4);
    char nm[64] = {0};
    const char *fn = reinterpret_cast<const char *>(getConstMemPtr(rdram, a0));
    if (fn) std::strncpy(nm, fn, sizeof(nm) - 1u);
    LoadFileMenu__FPcP1i_0x251100(rdram, ctx, runtime);
    if (g9_costume_enabled())
    {
        std::fprintf(stderr, "[G11:LoadFileMenu] name='%s' ret=%d (size)\n",
                     nm, static_cast<int32_t>(getRegU32(ctx, 2)));
        std::fflush(stderr);
    }
}

static void g11_enterimg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t buf = getRegU32(ctx, 5);  // a1 = IMG buffer
    const uint32_t blk = getRegU32(ctx, 6);  // a2 = *param_3 (tex block)
    EnterIMGFile__17mgCTextureManagerFPUciP9mgCMemoryP15mgCEnterIMGInfo_0x12da90(rdram, ctx, runtime);
    if (g9_costume_enabled())
    {
        std::fprintf(stderr, "[G11:EnterIMG] buf=0x%x blockArg=0x%x ret=0x%x\n",
                     buf, blk, getRegU32(ctx, 2));
        std::fflush(stderr);
    }
}

// G11: log each ReloadTexture(texIndex) and the tbp it will bind (texture obj +0x38,
// low 14 bits), so we see which costume draw binds 0x2aa0 vs 0x2720. mgr=0x381ef0,
// table base=*(mgr+0x10); entry=texIndex*0x10+base; obj=*(entry+8); tbp=*(u16*)(obj+0x38)&0x3fff.
static void g11_reloadtex_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int32_t texIdx = static_cast<int32_t>(getRegU32(ctx, 5)); // a1
    uint32_t tbp = 0xFFFFu;
    if (g9_costume_enabled() && g_g9_phase == 2 && texIdx >= 0)
    {
        const uint32_t mgr = 0x00381EF0u;
        const uint32_t tblBase = dc2_read_u32(rdram, mgr + 0x10u);
        if (tblBase != 0u)
        {
            const uint32_t obj = dc2_read_u32(rdram, tblBase + static_cast<uint32_t>(texIdx) * 0x10u + 8u);
            if (obj != 0u)
                tbp = dc2_read_u32(rdram, obj + 0x38u) & 0x3FFFu;
        }
        static uint32_t c = 0u;
        if (c++ < 40u)
        {
            std::fprintf(stderr, "[G11:reload] texIdx=%d -> tbp=0x%x ra=0x%x\n",
                         texIdx, tbp, getRegU32(ctx, 31));
            std::fflush(stderr);
        }
    }
    ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(rdram, ctx, runtime);
}

static const char *f48_2_costume_block_reason(int32_t state,
                                               int32_t ret,
                                               int32_t loadPhase,
                                               int32_t loadTimer,
                                               uint32_t menuFlag1)
{
    if (ret == 1)
        return "none/MenuCostumeKey_returned_1";
    if (state == 2)
        return "0x2bd434 fade_or_close_not_complete_ret0";
    if (state == 1 && (loadTimer <= 15 || loadPhase <= 3))
        return "state1_wait_MenuCosutumeLoadPhase_gt3_and_timer_gt15";
    if (state == 0)
        return "state0_costume_select_loop_ret0";
    return menuFlag1 ? "menu_fade_flag_set_ret0" : "state_not_ready_ret0";
}

static const char *f48_3_costume_branch_reason(int32_t preState,
                                               int32_t postState,
                                               int32_t preSub,
                                               int32_t postSub,
                                               int32_t preCursor,
                                               int32_t postCursor,
                                               uint32_t postSelect,
                                               uint32_t postPush,
                                               int32_t ret)
{
    if (ret == 1)
        return "0x2bcdd4_return_1_after_state2_fade";
    if ((postPush & 2u) != 0u && postState == 2)
        return "0x2bd170_cancel_back_uVar8_bit2_sets_state2_fade";
    if ((postPush & 1u) != 0u && postState == 2 && postCursor == 3)
        return "0x2bd3d8_yes_confirm_cursor3_sets_state2_fade";
    if ((postPush & 1u) != 0u && postState == 2 && postCursor == 4)
        return "0x2bd2b8_yes_confirm_cursor4_sets_state2_fade";
    if ((postPush & 1u) != 0u && postCursor == 3 && postSub != preSub)
        return "0x2bd1c0_confirm_bit1_cursor3_opens_yesno";
    if ((postPush & 1u) != 0u && postCursor == 4 && postSub != preSub)
        return "0x2bd244_confirm_bit1_cursor4_opens_yesno";
    if ((postPush & 1u) != 0u)
        return "0x2bd1b8_confirm_bit1_ignored_cursor_not_3_or_4";
    if (postCursor != preCursor && (postSelect & 2u) != 0u)
        return "0x2bcfa0_select_bit2_cursor_increment";
    if (postCursor != preCursor && (postSelect & 1u) != 0u)
        return "0x2bcfa0_select_bit1_cursor_decrement";
    if (preState == 1 && postState == 0)
        return "0x2bccf0_loader_gate_complete_state0";
    if (postState == 2)
        return "state2_wait_fade_return1";
    if (postState == 0)
        return "state0_wait_select_or_push";
    return "state_not_ready";
}

static void f48_2_menu_costume_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static int32_t lastState = -9999;
    static int32_t lastLoadPhase = -9999;
    static int32_t lastRet = -9999;
    static int32_t lastF483State = -9999;
    static int32_t lastF483Sub = -9999;
    static int32_t lastF483Cursor = -9999;
    static uint32_t lastF483Select = 0xFFFFFFFFu;
    static uint32_t lastF483Push = 0xFFFFFFFFu;
    static uint32_t lastF483PadEdge = 0xFFFFFFFFu;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    const uint32_t costume = dc2_read_u32(rdram, gp + 0xFFFF9C30u); // gp-0x63D0
    const int32_t preState = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x00u) & 0xFFFFu) : -9999;
    const int32_t preSub = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x02u) & 0xFFFFu) : -9999;
    const int32_t preCursor = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x1D0u)) : -9999;
    const int32_t preChar = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x258u) & 0xFFFFu) : -9999;
    const int32_t preLoadTimer = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x2A8u)) : -9999;
    const int32_t preLoadPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C24u) & 0xFFFFu); // gp-0x63DC
    const uint32_t preD62c = dc2_read_u32(rdram, 0x01ECD62Cu);
    const uint32_t preMenuFlag1 = menuPtr ? (dc2_read_u32(rdram, menuPtr + 0x00u) >> 8) & 0xFFu : 0u;
    const uint32_t preSelect = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x04u) : 0u;
    const uint32_t prePush = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x08u) : 0u;
    const uint32_t padCurr = dc2_read_u32(rdram, 0x003D76E4u); // CGamePad+0x04
    const uint32_t padPrev = dc2_read_u32(rdram, 0x003D777Cu); // CGamePad+0x9C
    const uint32_t padEdge = padCurr & ~padPrev;
    const int32_t languageCode = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8AD0u)); // gp-0x7530
    const uint32_t padTableBase = (languageCode > 0) ? 0x00350C68u : 0x00350C60u;
    const uint32_t padTable0 = dc2_read_u32(rdram, padTableBase + 0u);
    const uint32_t padTable1 = dc2_read_u32(rdram, padTableBase + 4u);

    MenuCostumeKey__Fv_0x2be030(rdram, ctx, runtime);

    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const int32_t postState = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x00u) & 0xFFFFu) : -9999;
    const int32_t postSub = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x02u) & 0xFFFFu) : -9999;
    const int32_t postCursor = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x1D0u)) : -9999;
    const int32_t postChar = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x258u) & 0xFFFFu) : -9999;
    const int32_t postLoadTimer = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x2A8u)) : -9999;
    const int32_t postLoadPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C24u) & 0xFFFFu); // gp-0x63DC
    const uint32_t postD62c = dc2_read_u32(rdram, 0x01ECD62Cu);
    const uint32_t postMenuFlag1 = menuPtr ? (dc2_read_u32(rdram, menuPtr + 0x00u) >> 8) & 0xFFu : 0u;
    const uint32_t postSelect = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x04u) : 0u;
    const uint32_t postPush = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x08u) : 0u;

    const bool changed = postState != lastState || postLoadPhase != lastLoadPhase || ret != lastRet ||
                          preState != postState || preLoadPhase != postLoadPhase || preD62c != postD62c;
    if (f48_2_trace_charbg_enabled() && g_f48_2_log_count < 128u &&
        (n <= 24u || changed || ret != 0 || (n % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=MenuCostumeKey.exit n=%u frame=%u ra=0x%x menu=0x%x costume=0x%x state=%d->%d sub=%d->%d cursor=%d->%d char=%d->%d loadPhase=%d->%d loadTimer=%d->%d d62c=0x%x->0x%x menuFlag1=%u->%u ret=%d block='%s'\n",
                     n, g_f40_frame_counter, ra, menuPtr, costume,
                     preState, postState, preSub, postSub, preCursor, postCursor,
                     preChar, postChar, preLoadPhase, postLoadPhase, preLoadTimer, postLoadTimer,
                     preD62c, postD62c, preMenuFlag1, postMenuFlag1, ret,
                     f48_2_costume_block_reason(postState, ret, postLoadPhase, postLoadTimer, postMenuFlag1));
        ++g_f48_2_log_count;
    }

    const bool f483Changed =
        postState != lastF483State ||
        postSub != lastF483Sub ||
        postCursor != lastF483Cursor ||
        postSelect != lastF483Select ||
        postPush != lastF483Push ||
        padEdge != lastF483PadEdge ||
        preState != postState ||
        preSub != postSub ||
        preCursor != postCursor ||
        ret != 0;
    if (f48_3_trace_costume_enabled() && g_f48_3_log_count < 128u &&
        (n <= 24u || f483Changed || postPush != 0u || postSelect != 0u || (n % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F48.3:costume] tag=MenuCostumeKey.exit n=%u frame=%u ra=0x%x script=0x%04x active=%u padCurr=0x%x padPrev=0x%x padEdge=0x%x lang=%d padtbl=%u/%u menu=0x%x costume=0x%x state=%d->%d sub=%d->%d cursor=%d->%d char=%d->%d select=0x%x->0x%x push=0x%x->0x%x loadPhase=%d->%d loadTimer=%d->%d ret=%d branch='%s'\n",
                     n, g_f40_frame_counter, ra,
                     static_cast<unsigned>(g_f40_pressed), g_f40_active ? 1u : 0u,
                     padCurr, padPrev, padEdge, languageCode, padTable0, padTable1,
                     menuPtr, costume, preState, postState, preSub, postSub,
                     preCursor, postCursor, preChar, postChar,
                     preSelect, postSelect, prePush, postPush,
                     preLoadPhase, postLoadPhase, preLoadTimer, postLoadTimer, ret,
                     f48_3_costume_branch_reason(preState, postState, preSub, postSub,
                                                 preCursor, postCursor, postSelect, postPush, ret));
        ++g_f48_3_log_count;
    }

    lastState = postState;
    lastLoadPhase = postLoadPhase;
    lastRet = ret;
    lastF483State = postState;
    lastF483Sub = postSub;
    lastF483Cursor = postCursor;
    lastF483Select = postSelect;
    lastF483Push = postPush;
    lastF483PadEdge = padEdge;
}
