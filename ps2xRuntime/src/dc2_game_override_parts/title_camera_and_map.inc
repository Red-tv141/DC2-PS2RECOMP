static bool f48_vtable_entry_is_callable(PS2Runtime *runtime, uint32_t fn)
{
    return runtime != nullptr && fn != 0u && runtime->hasFunction(fn);
}

static bool f48_title_camera_vtable_is_valid(uint8_t *rdram, PS2Runtime *runtime, uint32_t vtable)
{
    if (vtable == 0u)
        return false;

    const uint32_t initFn = dc2_read_u32(rdram, vtable + 0x08u);
    const uint32_t moveFn = dc2_read_u32(rdram, vtable + 0x18u);
    return f48_vtable_entry_is_callable(runtime, initFn) &&
           f48_vtable_entry_is_callable(runtime, moveFn);
}

static uint32_t f48_title_scene_camera(uint8_t *rdram, R5900Context *ctx, uint32_t *sceneOut, uint32_t *cameraIdOut, uint32_t *cameraCountOut)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t scene = dc2_read_u32(rdram, gp + 0xFFFF99ECu);
    const uint32_t cameraId = scene ? dc2_read_u32(rdram, scene + 0x2E54u) : 0xFFFFFFFFu;
    const uint32_t cameraCount = scene ? dc2_read_u32(rdram, scene + 0x2044u) : 0u;
    if (sceneOut)
        *sceneOut = scene;
    if (cameraIdOut)
        *cameraIdOut = cameraId;
    if (cameraCountOut)
        *cameraCountOut = cameraCount;

    if (scene == 0u)
        return 0u;

    if (cameraId >= cameraCount)
        return 0u;

    const uint32_t cameraEntry = scene + cameraId * 0x38u + 0x2048u;
    return dc2_read_u32(rdram, cameraEntry + 0x34u);
}

static bool f48_assign_title_scene_camera(uint8_t *rdram, uint32_t scene, uint32_t cameraId, uint32_t camera)
{
    if (scene == 0u || camera == 0u || cameraId > 1u)
        return false;

    const uint32_t cameraEntry = scene + cameraId * 0x38u + 0x2048u;
    const uint32_t minCount = cameraId + 1u;
    const uint32_t cameraCount = dc2_read_u32(rdram, scene + 0x2044u);
    if (cameraCount < minCount)
        dc2_write_u32(rdram, scene + 0x2044u, minCount);

    dc2_write_u32(rdram, cameraEntry + 0x00u, 4u);
    dc2_write_u32(rdram, cameraEntry + 0x08u, 0u);
    dc2_write_u32(rdram, cameraEntry + 0x34u, camera);
    return true;
}

static bool f48_repair_title_scene_cameras(uint8_t *rdram, R5900Context *ctx, uint32_t scene)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleCamera = dc2_read_u32(rdram, gp + 0xFFFF9948u);
    const uint32_t titleCamera2 = dc2_read_u32(rdram, gp + 0xFFFF994Cu);
    bool assigned = false;

    if (titleCamera != 0u)
    {
        const uint32_t slot0 = dc2_read_u32(rdram, scene + 0x2048u + 0x34u);
        if (slot0 == 0u)
            assigned = f48_assign_title_scene_camera(rdram, scene, 0u, titleCamera) || assigned;
    }

    if (titleCamera2 != 0u)
    {
        const uint32_t slot1 = dc2_read_u32(rdram, scene + 0x2048u + 0x38u + 0x34u);
        if (slot1 == 0u)
            assigned = f48_assign_title_scene_camera(rdram, scene, 1u, titleCamera2) || assigned;
    }

    return assigned;
}

// G58 ROOT FIX ג€” title-scene camera never lands in its scene slot, so GetCamera returns
// NULL and TitleMapDraw's camera-follow runs on a0=0, ending in a jr-to-data (0x9f84a0)
// that kills the title loop after one frame (black screen). ROOT (live-PCSX2 A/B + probes):
// TitleInit calls AssignCamera(scene,0,cam=0x785f70) and (scene,1,cam=0x786050) BEFORE
// CScene::Initialize@0x282ea0 has set the camera count (scene+0x2044), so GetSceneCamera
// returns 0 and the assign fails (ret=-1); Initialize sets count=8 only afterwards, leaving
// slot0/1 empty (HW: slot0.valid=0x4, slot0.obj=0x785f70). The existing F48 repair only runs
// on the indirect-dispatch title-draw path, which the dominant DIRECT path bypasses. This
// wraps GetCamera__6CScene so the title camera is (re)assigned on demand, on every path, just
// before it is used; self-heals if the late Initialize re-zeroes the slot. Scoped to the
// front-end (LoopNo==3) + the title scene (MainScene 0x1dd8260, shared with the dungeon) +
// an empty slot, so it never perturbs the dungeon. Kill switch DC2_G58_NO_CAMFIX.
static void g58_getcamera_title_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G58_NO_CAMFIX");
    if (!disabled)
    {
        const uint32_t scene = getRegU32(ctx, 4);
        const uint32_t id    = getRegU32(ctx, 5);
        const uint32_t gp    = getRegU32(ctx, 28);
        const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu); // 3 = front-end/title
        if (loopNo == 3u && scene == 0x01DD8260u && id <= 1u)
        {
            const uint32_t count = dc2_read_u32(rdram, scene + 0x2044u);
            if (count > id)
            {
                const uint32_t slot = scene + 0x2048u + 56u * id;
                if (dc2_read_u32(rdram, slot + 0x34u) == 0u)
                {
                    // titleCamera[id] = *(gp-0x66B8 + 4*id) (set by TitleInit's mgCCamera ctor)
                    const uint32_t cam = dc2_read_u32(rdram, gp + 0xFFFF9948u + 4u * id);
                    if (cam != 0u)
                    {
                        f48_assign_title_scene_camera(rdram, scene, id, cam);
                        static unsigned camFixLogs = 0u;
                        if (f48_trace_title_camera_enabled() && camFixLogs < 8u)
                        {
                            ++camFixLogs;
                            std::fprintf(stderr, "[G58:camfix] assigned scene=0x%x id=%u cam=0x%x (count=%u)\n",
                                         scene, id, cam, count);
                            std::fflush(stderr);
                        }
                    }
                }
            }
        }
    }
    GetCamera__6CSceneFi_0x2838c0(rdram, ctx, runtime);
}

static void g59_get_texture_block_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t set = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t buf = getRegU32(ctx, 6);
    const uint32_t max = getRegU32(ctx, 7);
    const uint32_t ra = getRegU32(ctx, 31);

    GetTextureBlockNo__11CMdsListSetFiPii_0x168fd0(rdram, ctx, runtime);

    if (!g59_trace_enabled() || ctx->pc != ra)
        return;

    const bool titleSet = (set == 0x01DDA630u);
    const bool titleCaller = (ra == 0x002A2598u || ra == 0x002A2644u);
    if (!titleSet && !titleCaller)
        return;

    static uint32_t logs = 0u;
    if (logs >= 96u)
        return;
    ++logs;

    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t mdsCnt = set ? dc2_read_u32(rdram, set + 0x00u) : 0u;
    const uint32_t imgCnt = set ? dc2_read_u32(rdram, set + 0x90u) : 0u;
    const uint32_t mds7Name = set ? dc2_read_u32(rdram, set + 0x10u + 7u * 0x10u) : 0u;
    const uint32_t mds7Count = set ? dc2_read_u32(rdram, set + 0x14u + 7u * 0x10u) : 0u;
    const uint32_t mds7List = set ? dc2_read_u32(rdram, set + 0x18u + 7u * 0x10u) : 0u;
    const uint32_t img15Flag = set ? dc2_read_u32(rdram, set + 0x94u + 15u * 8u) : 0u;
    const uint32_t img15Tbl = set ? dc2_read_u32(rdram, set + 0x98u + 15u * 8u) : 0u;
    const uint32_t img15Base = (img15Tbl && idx < 32u) ? dc2_read_u32(rdram, img15Tbl + idx * 4u) : 0u;
    const uint32_t img15Count = (img15Tbl && idx < 32u) ? dc2_read_u32(rdram, img15Tbl + 0x80u + idx * 4u) : 0u;

    uint32_t foundEntry = 0xFFFFFFFFu;
    uint32_t foundFlag = 0u;
    uint32_t foundTbl = 0u;
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
                foundFlag = flag;
                foundTbl = tbl;
                foundBase = base;
                foundCount = count;
                break;
            }
        }
    }

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    std::fprintf(stderr,
                 "[G59:block] n=%u set=0x%x idx=%u max=%u ret=%d(0x%x) buf=0x%x "
                 "out=%x,%x,%x,%x ra=0x%x pc=0x%x loop=%u mdsCnt=%u imgCnt=%u "
                 "mds7(name=0x%x cnt=%u list=0x%x) found=%u flag=0x%x tbl=0x%x base=0x%x cnt=%u "
                 "img15(flag=0x%x tbl=0x%x base=0x%x cnt=%u)\n",
                 logs, set, idx, max, static_cast<int32_t>(ret), ret, buf,
                 dc2_read_u32(rdram, buf + 0u),
                 dc2_read_u32(rdram, buf + 4u),
                 dc2_read_u32(rdram, buf + 8u),
                 dc2_read_u32(rdram, buf + 12u),
                 ra, ctx->pc, loopNo, mdsCnt, imgCnt,
                 mds7Name, mds7Count, mds7List,
                 foundEntry, foundFlag, foundTbl, foundBase, foundCount,
                 img15Flag, img15Tbl, img15Base, img15Count);
    std::fflush(stderr);
}

static bool g59_title_scope(uint8_t *rdram, R5900Context *ctx, uint32_t *sceneOut = nullptr)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t scene = gp ? dc2_read_u32(rdram, gp + 0xFFFF99ECu) : 0u;
    if (sceneOut)
        *sceneOut = scene;
    return loopNo == 3u && scene == 0x01DD8260u;
}

static uint32_t g59_title_camera_ptr(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t scene = 0u;
    if (!g59_title_scope(rdram, ctx, &scene))
        return 0u;

    uint32_t id = scene ? dc2_read_u32(rdram, scene + 0x2E54u) : 0u;
    if (id > 1u)
        id = 0u;

    const uint32_t slot = scene + 0x2048u + 56u * id;
    uint32_t camera = dc2_read_u32(rdram, slot + 0x34u);
    if (!camera)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        camera = gp ? dc2_read_u32(rdram, gp + 0xFFFF9948u + 4u * id) : 0u;
    }
    return camera;
}

static void g59_repair_title_map_resume_frame(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    if (!g59_title_scope(rdram, ctx))
        return;

    static const uint32_t kExpectedTitleMapReturn = 0x002A1B88u;
    const uint32_t sp = getRegU32(ctx, 29);
    const bool titleMapFrame = (sp >= 0x001FFF700u && sp <= 0x001FFF800u);
    if (!titleMapFrame)
        return;

    const uint32_t raLo = dc2_read_u32(rdram, sp + 0xA0u);
    const uint32_t raHi = dc2_read_u32(rdram, sp + 0xA4u);
    const uint32_t camera = g59_title_camera_ptr(rdram, ctx);
    const uint32_t s7 = getRegU32(ctx, 23);
    bool fixedRa = false;
    bool fixedS7 = false;

    if (sp >= 0x00100000u && sp < 0x02000000u &&
        (raLo != kExpectedTitleMapReturn || raHi != 0u))
    {
        dc2_write_u32(rdram, sp + 0xA0u, kExpectedTitleMapReturn);
        dc2_write_u32(rdram, sp + 0xA4u, 0u);
        fixedRa = true;
    }

    if (s7 == 0u && camera != 0u)
    {
        f50_set_reg(ctx, 23, camera);
        fixedS7 = true;
    }

    if (g59_trace_enabled() && (fixedRa || fixedS7))
    {
        static uint32_t logs = 0u;
        if (logs < 32u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:statefix] tag=%s sp=0x%x raSlot=0x%x:%x s7=0x%x cam=0x%x fixedRa=%u fixedS7=%u pc=0x%x\n",
                         tag, sp, raHi, raLo, s7, camera, fixedRa ? 1u : 0u,
                         fixedS7 ? 1u : 0u, ctx->pc);
            std::fflush(stderr);
        }
    }
}

static void g59_repair_title_map_resume_sp(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    if (!g59_title_scope(rdram, ctx))
        return;

    const uint32_t sp = getRegU32(ctx, 29);
    if (sp < 0x001FFEF00u || sp > 0x001FFEFFFu)
        return;

    const uint32_t fixedSp = sp + 0x830u;
    f50_set_reg(ctx, 29, fixedSp);

    if (g59_trace_enabled())
    {
        static uint32_t logs = 0u;
        if (logs < 16u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:spfix] tag=%s sp=0x%x -> 0x%x pc=0x%x\n",
                         tag, sp, fixedSp, ctx->pc);
            std::fflush(stderr);
        }
    }
}

static bool g59_repair_title_camera_arg(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool titleCameraCaller =
        ra == 0x002A2734u || ra == 0x002A273Cu ||
        ra == 0x002A2774u || ra == 0x002A27A4u || ra == 0x002A27D4u ||
        ra == 0x002A284Cu || ra == 0x002A285Cu || ra == 0x002A2868u;
    if (!titleCameraCaller || getRegU32(ctx, 4) != 0u)
        return false;

    const uint32_t camera = g59_title_camera_ptr(rdram, ctx);
    if (!camera)
        return false;

    f50_set_reg(ctx, 4, camera);

    if (g59_trace_enabled())
    {
        static uint32_t logs = 0u;
        if (logs < 24u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:camarg] tag=%s ra=0x%x repaired a0=0x%x\n",
                         tag, ra, camera);
            std::fflush(stderr);
        }
    }
    return true;
}

static void g59_title_map_resume_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *tag)
{
    g59_repair_title_map_resume_sp(rdram, ctx, tag);
    g59_repair_title_map_resume_frame(rdram, ctx, tag);
    TitleMapDraw__Fv_0x2a2280(rdram, ctx, runtime);
}

static void g59_title_map_resume_2548(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g59_title_map_resume_fix(rdram, ctx, runtime, "TitleMapDraw@2a2548"); }

static void g59_title_map_resume_2644(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g59_title_map_resume_fix(rdram, ctx, runtime, "TitleMapDraw@2a2644"); }

static void g59_cam_adddistance_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "AddDistance");
    // G103 probe: measure the title-camera zoom rate PER AddDistance call. HW zooms at
    // -0.25/frame (verified live). If dist drops 0.25 per call here, the rate is correct and
    // any wall-clock framing lag is a runner frame-rate (perf) issue, not a camera bug.
    static const bool s_g103zoom = (std::getenv("DC2_G103_ZOOM") != nullptr);
    auto g103rf = [&](uint32_t a) -> float { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
    uint32_t g103cam = 0u;
    float g103before = 0.0f;
    if (s_g103zoom)
    {
        g103cam = getRegU32(ctx, 4);
        if (g103cam) g103before = g103rf(g103cam + 0x90u);
    }
    AddDistance__15mgCCameraFollowFf_0x131a20(rdram, ctx, runtime);
    if (s_g103zoom && g103cam)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        const float after = g103rf(g103cam + 0x90u);
        if (n < 8u || (n % 256u) == 0u)
            std::fprintf(stderr, "[G103:zoom] call=%u cam=0x%x ra=0x%x dist %.3f->%.3f (d=%.4f)\n",
                         n, g103cam, getRegU32(ctx, 31), g103before, after, after - g103before);
    }
    g59_repair_title_map_resume_frame(rdram, ctx, "AddDistance.post");
}

static void g59_cam_getdistance_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "GetDistance");
    GetDistance__15mgCCameraFollowFv_0x131a10(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, "GetDistance.post");
}

static void g59_cam_addheight_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "AddHeight");
    AddHeight__15mgCCameraFollowFf_0x131a50(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, "AddHeight.post");
}

static void g59_mgdraw_frame_tailfix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entrySp = getRegU32(ctx, 29);
    const uint32_t entryRa = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = gp ? dc2_read_u32(rdram, gp + 0xFFFF9944u) : 0u;
    const bool titleLike = (loopNo == 3u && titleMap == 0x008FC880u);
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t vt = frame ? dc2_read_u32(rdram, frame + 0x00u) : 0u;
    const uint32_t drawSlot = vt ? dc2_read_u32(rdram, vt + 0x34u) : 0u;

    mgDraw__FP8mgCFrame_0x142f90(rdram, ctx, runtime);

    const uint32_t postPc = ctx->pc;
    const uint32_t postSp = getRegU32(ctx, 29);
    const uint32_t postRa = getRegU32(ctx, 31);
    bool repaired = false;
    uint32_t savedRa = 0u;
    uint32_t savedRaHi = 0u;

    if (titleLike &&
        entryRa == 0x00169FFCu &&
        drawSlot == 0x00137E10u &&
        postPc == drawSlot &&
        entrySp >= 0x10u &&
        postSp == entrySp - 0x10u)
    {
        savedRa = dc2_read_u32(rdram, postSp + 0x00u);
        savedRaHi = dc2_read_u32(rdram, postSp + 0x04u);
        if (savedRa == entryRa && savedRaHi == 0u)
        {
            f50_set_reg(ctx, 31, savedRa);
            f50_set_reg(ctx, 29, entrySp);
            ctx->pc = savedRa;
            repaired = true;
        }
    }

    if (g59_trace_enabled() && (titleLike || repaired || postPc == drawSlot))
    {
        static uint32_t logs = 0u;
        if (logs < 64u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:mgdrawfix] n=%u frame=0x%x slot=0x%x entry(sp=0x%x ra=0x%x) "
                         "post(pc=0x%x sp=0x%x ra=0x%x saved=0x%x:%x) loop=%u map=0x%x repaired=%u\n",
                         logs, frame, drawSlot, entrySp, entryRa,
                         postPc, postSp, postRa, savedRaHi, savedRa,
                         loopNo, titleMap, repaired ? 1u : 0u);
            std::fflush(stderr);
        }
    }
}

// Draw__8mgCFrameFv@0x1387f0 is a no-frame thunk:
//   a1 = 0; jr *(frame->vtable + 0x44)
// The generated body returns to the dispatcher with pc set to the target slot
// instead of completing the inherited tail-call. mgDraw then exits with its
// stack frame still active. Run the virtual target synchronously so it returns
// to mgDraw's real continuation (usually 0x142fac).
static void g59_frame_draw_tailcall_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = gp ? dc2_read_u32(rdram, gp + 0xFFFF9944u) : 0u;
    const bool titleLike = (loopNo == 3u && titleMap == 0x008FC880u);
    const uint32_t vt = frame ? dc2_read_u32(rdram, frame + 0x00u) : 0u;
    const uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0x44u) : 0u;

    f50_set_reg(ctx, 5, 0u);
    f50_set_reg(ctx, 25, slot);

    bool called = false;
    bool normalized = false;
    uint32_t postPc = slot;
    uint32_t postSp = sp;
    uint32_t postRa = ra;

    if (slot != 0u && slot != 0x001387F0u && runtime->hasFunction(slot))
    {
        auto targetFn = runtime->lookupFunction(slot);
        ctx->pc = slot;
        targetFn(rdram, ctx, runtime);
        called = true;
        postPc = ctx->pc;
        postSp = getRegU32(ctx, 29);
        postRa = getRegU32(ctx, 31);

        if (postPc == slot || postPc == 0u)
        {
            ctx->pc = ra;
            postPc = ctx->pc;
            normalized = true;
        }
    }
    else
    {
        ctx->pc = slot;
    }

    if (g59_trace_enabled() && (titleLike || slot == 0x00137E10u || !called || normalized))
    {
        static uint32_t logs = 0u;
        if (logs < 96u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:framevfix] n=%u frame=0x%x vt=0x%x slot=0x%x ra=0x%x "
                         "sp=0x%x post(pc=0x%x sp=0x%x ra=0x%x) called=%u normalized=%u "
                         "loop=%u map=0x%x\n",
                         logs, frame, vt, slot, ra,
                         sp, postPc, postSp, postRa, called ? 1u : 0u, normalized ? 1u : 0u,
                         loopNo, titleMap);
            std::fflush(stderr);
        }
    }
}

static bool g53_trace_enabled(); // defined below; used by the title draw probe
static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap); // fwd

// PHASE F48: keep real TitleMapDraw active, but repair the specific scene
// camera object it is about to virtual-dispatch through if the vtable pointer
// does not resolve to registered PS2 functions. This targets the observed
// bad-PC chain at TitleMapDraw+0x7c (ra=0x2a22fc) without broad dispatch policy.
static void f48_title_map_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t fixes = 0u;
    static uint32_t logs = 0u;

    const uint32_t n = ++calls;
    uint32_t scene = 0u;
    uint32_t cameraId = 0xFFFFFFFFu;
    uint32_t cameraCount = 0u;
    uint32_t camera = f48_title_scene_camera(rdram, ctx, &scene, &cameraId, &cameraCount);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleCamera = dc2_read_u32(rdram, gp + 0xFFFF9948u);
    const uint32_t titleCamera2 = dc2_read_u32(rdram, gp + 0xFFFF994Cu);
    const bool assignedSceneCamera = (camera == 0u && scene != 0u) ?
        f48_repair_title_scene_cameras(rdram, ctx, scene) : false;
    if (assignedSceneCamera)
        camera = f48_title_scene_camera(rdram, ctx, &scene, &cameraId, &cameraCount);

    const uint32_t beforeVtable = camera ? dc2_read_u32(rdram, camera + 0x60u) : 0u;
    const uint32_t beforeFn8 = beforeVtable ? dc2_read_u32(rdram, beforeVtable + 0x08u) : 0u;
    const uint32_t beforeFn18 = beforeVtable ? dc2_read_u32(rdram, beforeVtable + 0x18u) : 0u;
    const bool validBefore = f48_title_camera_vtable_is_valid(rdram, runtime, beforeVtable);
    bool fixed = false;

    if (camera != 0u && !validBefore)
    {
        dc2_write_u32(rdram, camera + 0x60u, 0x00374E70u);
        fixed = true;
        ++fixes;
    }

    if (f48_trace_title_camera_enabled() && logs < 64u &&
        (fixed || n <= 12u || (n % 60u) == 0u))
    {
        const uint32_t afterVtable = camera ? dc2_read_u32(rdram, camera + 0x60u) : 0u;
        const uint32_t afterFn8 = afterVtable ? dc2_read_u32(rdram, afterVtable + 0x08u) : 0u;
        const uint32_t afterFn18 = afterVtable ? dc2_read_u32(rdram, afterVtable + 0x18u) : 0u;
        std::fprintf(stderr,
                     "[F48:titlecam] n=%u scene=0x%x count=%u camId=%u camera=0x%x titleCamera=0x%x titleCamera2=0x%x assigned=%u vt=0x%x fn8=0x%x fn18=0x%x fixed=%u fixes=%u afterVt=0x%x afterFn8=0x%x afterFn18=0x%x\n",
                     n, scene, cameraCount, cameraId, camera, titleCamera, titleCamera2,
                     assignedSceneCamera ? 1u : 0u, beforeVtable, beforeFn8, beforeFn18,
                     fixed ? 1u : 0u, fixes, afterVtable, afterFn8, afterFn18);
        ++logs;
    }

    // G53 (draw side): TitleMapDraw gates all 3D geometry on the global TitleMap
    // (= GetMap(TitleScene, scene[0xb97])). GetMap returns 0 unless map slot 0
    // (scene+0x27e4) has its active word (+0) != 0, and then returns the map ptr
    // (slot+0x34). HW: TitleMap=0x8fc880, slot active, ptr set. Dump both so we can
    // see whether the seeded/built map ever registers into the scene map slot.
    // Run the real draw FIRST so PreDraw__4CMap has rebuilt the per-frame visible draw
    // list (map+0x360) for THIS frame before we sample it.
    // G57: suppress back-edge preemption for the whole title draw. The draw flushes
    // through the deferred mgCDrawManager whose BeginDraw is reached via an override that
    // calls the recompiled body directly; a mid-loop preemption there leaks a resume PC
    // this path cannot re-enter, leaving the manager half-initialized (mgr[4]/mgr[6] null)
    // so the deferred flush dereferences null and aborts the title draw before any geometry
    // reaches the GS. Suppressing preemption makes the bodies run to completion in one call.
    // Kill switch DC2_G57_NO_SUPPRESS (default ON).
    // G57: when the title draw is reached through this (indirect-dispatch) override, it runs
    // TitleMapDraw as a direct C++ call. A mid-loop back-edge preemption inside the deferred
    // draw chain then returns a resume PC this path cannot re-enter, leaving e.g. BeginDraw's
    // mgr[4]/mgr[6] null -> deferred-flush null-deref. Suppress preemption for the duration so
    // the recompiled bodies run to completion in one call. Inert on the dominant DIRECT path
    // (TitleModeDraw -> jal TitleMapDraw bypasses this override), which the normal dispatch
    // loop already drives correctly. Kill switch DC2_G57_NO_SUPPRESS (default ON).
    // G127 (FIX, default-ON, kill-switch DC2_G127_NO_CAMID_FIX): the debug-menu "title"
    // entry reaches TitleMapDraw with the TitleScene active-camera index (scene+0x2e54) = -1
    // (HW = 0). On the normal boot TitleInit->AssignCamera sets it (AssignCamera@0x2837e0:
    // `if (scene+0x2e54 >= 0) skip; else scene+0x2e54 = assignedId`), but a debug re-entry
    // re-runs Initialize__6CScene (which resets active to -1) WITHOUT re-running AssignCamera
    // (the camera stays registered in slot 0). With active = -1, GetCamera(scene,-1) returns
    // NULL -> TitleMapDraw builds a ZERO view matrix -> mgSetViewMatrix writes 0 to
    // renderinfo +0x10/+0x90/+0x150 -> PreDraw__4CMap's mgInsideScreen frustum-culls all 6
    // parts -> map+0x360 draw list = 0 -> nothing reaches the VU -> flat blue.
    // Fix = restore the scene's OWN invariant using AssignCamera's OWN rule: when the active
    // index is -1 and a valid camera is registered in slot 0, select it. Touches only the
    // TitleScene object's field (NOT a shared global -> cannot leak into a concurrent screen,
    // unlike the rejected G93 TitleProjection/camera writes); idempotent (fires once, the
    // condition is then false); inert on the auto route (active already 0) and on title states
    // that don't draw the map (MenuMainDraw / costume / dungeon never reach TitleMapDraw).
    {
        static const bool g127NoCamIdFix = dc2_env_flag_enabled("DC2_G127_NO_CAMID_FIX");
        const uint32_t gpFix = getRegU32(ctx, 28);
        const uint32_t tsceneFix = gpFix ? dc2_read_u32(rdram, gpFix + 0xFFFF99ECu) : 0u;
        if (!g127NoCamIdFix && tsceneFix != 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2E54u) == 0xFFFFFFFFu &&
            dc2_read_u32(rdram, tsceneFix + 0x2044u) > 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2048u + 0x00u) != 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2048u + 0x34u) != 0u)
        {
            dc2_write_u32(rdram, tsceneFix + 0x2E54u, 0u);
        }
    }

    static const bool g57SuppressDisabled = dc2_env_flag_enabled("DC2_G57_NO_SUPPRESS");
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    TitleMapDraw__Fv_0x2a2280(rdram, ctx, runtime);
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);


    // G65 (diagnosis): one-shot dump of the title model data region (frame tree +
    // mesh/strip data for s19_0.pcp; HW base 0x903000..0x912000) so it can be diffed
    // against the live-PCSX2 dump (captures/g65_hw_model.bin). If the runner region is
    // mostly zero / differs, the title geometry data never loaded (load gap); if it
    // matches HW, the 0-strip-count defect is in the render packet build, not the data.
    // Env-gated DC2_G65_MODELDUMP; fires once, after the model is loaded (n>=8).
    if (n >= 8u && std::getenv("DC2_G65_MODELDUMP") != nullptr)
    {
        static bool s_g65dumped = false;
        if (!s_g65dumped)
        {
            const uint32_t start = 0x00903000u, end = 0x00912000u;
            const uint8_t* src = getConstMemPtr(rdram, start);
            if (src)
            {
                if (FILE* f = std::fopen("D:/ps2r/dc2/captures/g65_runner_model.bin", "wb"))
                {
                    std::fwrite(src, 1, (size_t)(end - start), f);
                    std::fclose(f);
                    size_t nz = 0; for (uint32_t i = 0; i < (end - start); ++i) if (src[i]) ++nz;
                    std::fprintf(stderr, "[G65:modeldump] wrote 0x%x..0x%x bytes=%u nonzero=%zu\n",
                                 start, end, end - start, nz);
                }
            }
            s_g65dumped = true;
        }
    }

    static uint32_t g53DrawLogs = 0u;
    if (g53_trace_enabled() && scene != 0u && g53DrawLogs < 200u && (n <= 8u || (n % 120u) == 0u))
    {
        const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
        const uint32_t nowMapNo = dc2_read_u32(rdram, scene + 0x2E5Cu);
        const uint32_t mapSlotCnt = dc2_read_u32(rdram, scene + 0x27E0u);
        const uint32_t slot0 = scene + 0x27E4u;
        const uint32_t slot0Active = dc2_read_u32(rdram, slot0 + 0x00u);
        const uint32_t slot0MapPtr = dc2_read_u32(rdram, slot0 + 0x34u);
        // CMap geometry: DrawSub__4CMapFi iterates *(map+0x360) parts from *(map+0x364);
        // each drawn via Draw__9CMapParts (vt+0x34). HW: cnt=6 parts@0x9f84a0 flags=0x161.
        uint32_t partsCnt = 0u, partsArr = 0u, mapFlags = 0u, p0 = 0u, p0vt = 0u;
        uint32_t mBase = 0u, mCnt = 0u, m0active = 0u, m0vis = 0u, m0vt = 0u;
        if (titleMap)
        {
            partsCnt = dc2_read_u32(rdram, titleMap + 0x360u);
            partsArr = dc2_read_u32(rdram, titleMap + 0x364u);
            mapFlags = dc2_read_u32(rdram, titleMap + 0xCB0u);
            if (partsArr) { p0 = dc2_read_u32(rdram, partsArr); if (p0) p0vt = dc2_read_u32(rdram, p0); }
            // Master parts (built by LoadMapFromMemory): base +0x32c, count +0x330,
            // stride 0x310. Per part: active +0x70 (byte), in-frustum/visible +0x2f8.
            mBase = dc2_read_u32(rdram, titleMap + 0x32Cu);
            mCnt  = dc2_read_u32(rdram, titleMap + 0x330u);
            if (mBase && mCnt) {
                m0active = dc2_read_u32(rdram, mBase + 0x70u) & 0xFFu;
                m0vis = dc2_read_u32(rdram, mBase + 0x2F8u);
                m0vt = dc2_read_u32(rdram, mBase);
            }
        }
        // Clip/view-proj matrix globals @0x380f50 used by mgInsideScreen (VU0 box proj).
        // HW row0 ~ [480,0,0,0]. part0+0x230 (=[0x8c]) must be !=0 (geometry present).
        auto rf = [&](uint32_t a){ uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f,&u,4); return f; };
        const uint32_t m0geom = mBase ? dc2_read_u32(rdram, mBase + 0x230u) : 0u;
        // part0+0xb0 = CMapPiece list head (HW=0x9fe340). part0+0x80 = active byte.
        // CreateBoundBox iterates this list to set +0x230. Empty list => +0x230 stays 0.
        const uint32_t m0pieces = mBase ? dc2_read_u32(rdram, mBase + 0xB0u) : 0u;
        uint32_t pc0visual = 0u;
        if (m0pieces) pc0visual = dc2_read_u32(rdram, m0pieces + 0x80u); // = piece+0x70 visual
        // MDS model table (map+0x100 = CMdsListSet, count = *CMdsListSet). HW: 0x1dda630, count=8.
        // SearchMDS uses this to resolve a piece's model; empty table => visual stays 0.
        const uint32_t mdsSet = titleMap ? dc2_read_u32(rdram, titleMap + 0x100u) : 0u;
        const uint32_t mdsCount = mdsSet ? dc2_read_u32(rdram, mdsSet) : 0u;
        // G55: how many of the mdsCount slots are FILLED (name word != 0). HW fills exactly 1
        // (s19_0.pcp in slot 7). 0 filled with count=8 => GetPCPName / GetPackFile failed.
        uint32_t mdsFilled = 0u, slotName0 = 0u;
        for (uint32_t i = 0u; i < mdsCount && i < 16u; ++i)
        {
            const uint32_t nm = dc2_read_u32(rdram, mdsSet + 0x10u + i * 0x10u);
            if (nm) { ++mdsFilled; if (!slotName0) slotName0 = nm; }
        }
        // CMapInfo PCP name list (parsed from s19.map): map+0x44 count, map+0x48 array.
        const uint32_t pcpCnt = titleMap ? dc2_read_u32(rdram, titleMap + 0x44u) : 0u;
        const uint32_t pcp0 = titleMap ? dc2_read_u32(rdram, titleMap + 0x48u) : 0u;
        char pcpName[24] = {0};
        if (pcp0) f55_read_cstr(rdram, pcp0, pcpName, sizeof(pcpName));
        // G55: the actual per-frame visible draw list count (map+0x360, rebuilt by
        // PreDraw__4CMap). HW=6 (all parts pass InsideScreen). 0 => parts still culled
        // despite bbox (VU0 box/clip projection rejects the box -> camera/clip issue).
        // Tally how many of the 6 master parts are flagged visible (+0x2f8) + have bbox.
        uint32_t visParts = 0u, bboxParts = 0u, visualParts = 0u;
        for (uint32_t i = 0u; i < mCnt && i < 6u; ++i)
        {
            const uint32_t p = mBase + i * 0x310u;
            if (dc2_read_u32(rdram, p + 0x2F8u)) ++visParts;
            if (dc2_read_u32(rdram, p + 0x230u)) ++bboxParts;
            const uint32_t pc = dc2_read_u32(rdram, p + 0xB0u);
            if (pc && dc2_read_u32(rdram, pc + 0x80u)) ++visualParts;
        }
        // G126: the debug-menu "title" route reaches this draw with drawList(map+0x360)=0 while
        // the auto-advance route gets 6. drawList is rebuilt per-frame by PreDraw__4CMap's
        // mgInsideScreen, which uses the clip/view-proj at 0x380f50 (HW row0.x ~480) + the title
        // camera. Dump loopNo (g67_title_scope gate), the title camera dist/phase, and clip row0.x
        // to see whether the debug route has the title camera/clip set up at all.
        const uint32_t gpG53 = getRegU32(ctx, 28);
        const uint32_t loopNoG53 = gpG53 ? dc2_read_u32(rdram, gpG53 + 0xFFFF8ADCu) : 0u;
        const uint32_t tcamG53 = dc2_read_u32(rdram, 0x00377E38u);
        const float camDistG53 = tcamG53 ? rf(tcamG53 + 0x90u) : 0.0f;
        const uint32_t camPhaseG53 = dc2_read_u32(rdram, 0x00377E44u);
        const float clip00G53 = rf(0x00380F50u);
        const float ri10G53  = rf(0x00380ED0u); // renderinfo +0x10 row0.x (View*Proj, HW 480)
        const float ri110G53 = rf(0x00380FD0u); // renderinfo +0x110 row0.x (proj, HW 480)
        const float ri150G53 = rf(0x00381010u); // renderinfo +0x150 row0.x (view*aspect, HW ~1)
        const uint32_t riIdxG53 = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
        std::fprintf(stderr,
                     "[G53:draw] n=%u loopNo=%u TitleMap=0x%x masterCnt=%u drawList(map+0x360)=%u visParts=%u/%u bboxParts=%u visualParts=%u camDist=%.1f camPhase=%u clip00=%.1f ri10=%.1f ri110=%.1f ri150=%.3f riIdx=%u | mdsCount=%u mdsFilled=%u pcpCnt=%u pcp0='%s'\n",
                     n, loopNoG53, titleMap, mCnt, partsCnt, visParts, mCnt, bboxParts, visualParts,
                     camDistG53, camPhaseG53, clip00G53, ri10G53, ri110G53, ri150G53, riIdxG53,
                     mdsCount, mdsFilled, pcpCnt, pcpName);
        // G127 diag: why are +0x10/+0x150 zero on the debug route? Dump the view matrix
        // (+0x1a0, HW row0.x=1 translate.x=70.1) + the title camera eye/ref (HW eye.x=-70.1
        // ref.x=-70.1 eye.z=-493 ref.z=775.25). If eye/ref are 0 -> TitleModeInit camera
        // block was skipped on the debug entry; if eye/ref set but view 0 -> GetCameraMatrix.
        const float riView00 = rf(0x00380EC0u + 0x1A0u);
        const float riViewTx = rf(0x00380EC0u + 0x1D0u);
        const float camEyeX  = tcamG53 ? rf(tcamG53 + 0x10u) : 0.0f;
        const float camEyeZ  = tcamG53 ? rf(tcamG53 + 0x18u) : 0.0f;
        const float camRefX  = tcamG53 ? rf(tcamG53 + 0x20u) : 0.0f;
        const float camRefZ  = tcamG53 ? rf(tcamG53 + 0x28u) : 0.0f;
        const uint32_t camVtG53 = tcamG53 ? dc2_read_u32(rdram, tcamG53 + 0x60u) : 0u;
        const uint32_t tsceneG53 = gpG53 ? dc2_read_u32(rdram, gpG53 + 0xFFFF99ECu) : 0u; // TitleScene (gp-0x6614)
        const uint32_t camIdG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2E54u) : 0xFFFFFFFFu;
        const uint32_t camCntG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2044u) : 0u;
        const uint32_t slot0ActG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2048u + 0x00u) : 0u;
        const uint32_t slot0CamG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2048u + 0x34u) : 0u;
        std::fprintf(stderr,
                     "[G127:cam] n=%u tcam=0x%x vt=0x%x TitleScene=0x%x camId=%d camCnt=%u slot0(act=%u cam=0x%x) view00=%.3f viewTx=%.2f eye=(%.1f,%.1f) ref=(%.1f,%.1f)\n",
                     n, tcamG53, camVtG53, tsceneG53, (int)camIdG53, camCntG53, slot0ActG53, slot0CamG53,
                     riView00, riViewTx, camEyeX, camEyeZ, camRefX, camRefZ);
        // G56: the title geometry flushes through the deferred mgCDrawManager (global
        // mgDrawManager @ 0x3820E0) keyed by the MDS set's IMG-block table. The flush
        // crashes (jalr -> data ptr) when G55 enables the draw, so dump the manager's
        // per-block tables + the one populated MDS IMG slot. HW: mgr[1]=0xd87340
        // mgr[3]=0x40 mgr[4]=0xd87620 mgr[6]=0xd87840 (all valid title-heap bufs);
        // MDS img[15] flag=0x903a20 tbl=0x903a30 (the rest 0).
        const uint32_t mgr = 0x003820E0u;
        const uint32_t m0 = dc2_read_u32(rdram, mgr + 0x00u);   // remap enable
        const uint32_t m1 = dc2_read_u32(rdram, mgr + 0x04u);   // remap table
        const uint32_t m3 = dc2_read_u32(rdram, mgr + 0x0Cu);   // block count
        const uint32_t m4 = dc2_read_u32(rdram, mgr + 0x10u);   // per-block packet-list array
        const uint32_t m6 = dc2_read_u32(rdram, mgr + 0x18u);   // per-block count array
        const uint32_t m16 = dc2_read_u32(rdram, mgr + 0x58u);  // bounds object
        const uint32_t m16c = m16 ? dc2_read_u32(rdram, m16 + 0x0Cu) : 0u; // max block
        const uint32_t imgCnt = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x90u) : 0u;
        const uint32_t img15f = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x94u + 15u * 8u) : 0u;
        const uint32_t img15t = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x98u + 15u * 8u) : 0u;
        std::fprintf(stderr,
                     "[G56:mgr] mgDrawManager m0=0x%x m1=0x%x m3=%u m4=0x%x m6=0x%x m16=0x%x m16+c=%u | imgCnt=%u img15(flag=0x%x tbl=0x%x)\n",
                     m0, m1, m3, m4, m6, m16, m16c, imgCnt, img15f, img15t);
        std::fflush(stderr);
    }
}

// PHASE F29: Hold the title loop for several frames before reusing F21's forced
// "PUSH START -> menu" transition. Returning 5 immediately makes MainLoop break
// its inner frame loop after exactly one mgEndFrame.
static void f29_log_title_state(const char *tag, uint8_t *rdram, R5900Context *ctx, uint32_t n, uint32_t ret)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t titlePhaseRaw = dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu;
    const int32_t titlePhase = static_cast<int16_t>(titlePhaseRaw);
    const uint32_t alphaPush = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x14u) : 0u;
    const uint32_t alphaLogo = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x1Cu) : 0u;
    const uint32_t alphaMenu = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x20u) : 0u;
    const uint32_t alphaCursor = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x28u) : 0u;

    if (f29_should_log(n))
    {
        std::fprintf(stderr,
                     "[F29:%s] n=%u ret=%u pc=0x%x ra=0x%x loop=%u next=%u titleInfo=0x%x mode=%u sub=%u phase=%d alpha14=0x%x alpha1c=0x%x alpha20=0x%x alpha28=0x%x\n",
                     tag, n, ret, ctx->pc, getRegU32(ctx, 31), loopNo, nextLoopNo,
                     titleInfo, titleMode, titleSub, titlePhase,
                     alphaPush, alphaLogo, alphaMenu, alphaCursor);
    }
}

static void f29_log_counters(const char *tag, uint32_t n, PS2Runtime *runtime, R5900Context *ctx)
{
    if (!f29_should_log(n))
        return;

    const auto &memory = runtime->memory();
    std::fprintf(stderr,
                 "[F29:%s] n=%u pc=0x%x ra=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                 tag, n, ctx->pc, getRegU32(ctx, 31),
                 static_cast<unsigned long long>(memory.dmaStartCount()),
                 static_cast<unsigned long long>(memory.gifCopyCount()),
                 static_cast<unsigned long long>(memory.gsWriteCount()),
                 static_cast<unsigned long long>(memory.vifWriteCount()));
}

static void f29_log_title_draw_state(const char *tag, uint32_t n, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!f29_should_log(n))
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t texChronicle = dc2_read_u32(rdram, gp + 0xFFFF99C4u);
    const uint32_t texLogo = dc2_read_u32(rdram, gp + 0xFFFF99C8u);
    const auto &memory = runtime->memory();

    std::fprintf(stderr,
                 "[F29:%s] n=%u pc=0x%x ra=0x%x titleInfo=0x%x mode=%u sub=%u texChronicle=0x%x texLogo=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                 tag, n, ctx->pc, getRegU32(ctx, 31), titleInfo, titleMode, titleSub,
                 texChronicle, texLogo,
                 static_cast<unsigned long long>(memory.dmaStartCount()),
                 static_cast<unsigned long long>(memory.gifCopyCount()),
                 static_cast<unsigned long long>(memory.gsWriteCount()),
                 static_cast<unsigned long long>(memory.vifWriteCount()));
}

static void f29_log_packet_state(const char *tag, uint32_t n, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!f29_should_log(n))
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp + 0xFFFF8774u); // mgVif1Packet
    const uint32_t ptr = pkt ? dc2_read_u32(rdram, pkt + 0u) : 0u;
    const uint32_t buf = pkt ? dc2_read_u32(rdram, pkt + 4u) : 0u;
    const uint32_t dmatag = pkt ? dc2_read_u32(rdram, pkt + 8u) : 0u;
    const uint32_t direct = pkt ? dc2_read_u32(rdram, pkt + 12u) : 0u;
    const uint32_t gifptag = pkt ? dc2_read_u32(rdram, pkt + 20u) : 0u;
    const uint32_t tag0 = buf ? dc2_read_u32(rdram, buf + 0u) : 0u;
    const uint32_t tag1 = buf ? dc2_read_u32(rdram, buf + 4u) : 0u;
    const uint32_t tag2 = buf ? dc2_read_u32(rdram, buf + 8u) : 0u;
    const uint32_t tag3 = buf ? dc2_read_u32(rdram, buf + 12u) : 0u;
    const uint32_t next0 = buf ? dc2_read_u32(rdram, buf + 16u) : 0u;
    const uint32_t next1 = buf ? dc2_read_u32(rdram, buf + 20u) : 0u;
    auto &memory = runtime->memory();
    const uint32_t chcr = memory.readIORegister(0x10009000u);
    const uint32_t madr = memory.readIORegister(0x10009010u);
    const uint32_t qwc = memory.readIORegister(0x10009020u);
    const uint32_t tadr = memory.readIORegister(0x10009030u);

    std::fprintf(stderr,
                 "[F29:%s] n=%u pkt=0x%x ptr=0x%x buf=0x%x dmatag=0x%x direct=0x%x gifptag=0x%x tag=%08x,%08x,%08x,%08x next=%08x,%08x vif1[chcr=%08x madr=%08x qwc=%08x tadr=%08x]\n",
                 tag, n, pkt, ptr, buf, dmatag, direct, gifptag,
                 tag0, tag1, tag2, tag3, next0, next1,
                 chcr, madr, qwc, tadr);
}

static void f47_log_menu_state(uint8_t *rdram,
                               R5900Context *ctx,
                               uint32_t mgFrame,
                               uint32_t scriptFrame,
                               uint32_t loopNo,
                               uint32_t nextLoopNo)
{
    if (!f47_trace_menu_state_enabled())
        return;

    static uint32_t logs = 0u;
    static uint32_t lastLoop = 0xFFFFFFFFu;
    static uint32_t lastNextLoop = 0xFFFFFFFFu;
    static uint32_t lastMenuPtr = 0xFFFFFFFFu;
    static uint32_t lastMenuStep = 0xFFFFFFFFu;
    static uint32_t lastMenuSel = 0xFFFFFFFFu;
    static uint32_t lastTitlePhase = 0xFFFFFFFFu;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu); // gp-0x6684
    const int32_t titlePhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu); // gp-0x6658
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x00u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x04u) : 0xFFFFFFFFu;

    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6b08
    const uint32_t menuStep = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x54u) : 0xFFFFFFFFu;
    const int32_t menuSel = menuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu) : -1;
    const uint32_t menuFlags = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x00u) : 0u;

    const bool changed =
        loopNo != lastLoop ||
        nextLoopNo != lastNextLoop ||
        menuPtr != lastMenuPtr ||
        menuStep != lastMenuStep ||
        static_cast<uint32_t>(menuSel) != lastMenuSel ||
        static_cast<uint32_t>(titlePhase) != lastTitlePhase;

    if (logs < 128u && (logs < 16u || changed || (mgFrame % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F47:menu] n=%u frame=%u loop=%u next=%u titlePhase=%d titleMode=%u titleSub=%u menu=0x%x step=%u sel=%d flags=0x%x ra=0x%x\n",
                     mgFrame, scriptFrame, loopNo, nextLoopNo,
                     titlePhase, titleMode, titleSub,
                     menuPtr, menuStep, menuSel, menuFlags, getRegU32(ctx, 31));
        ++logs;
    }

    lastLoop = loopNo;
    lastNextLoop = nextLoopNo;
    lastMenuPtr = menuPtr;
    lastMenuStep = menuStep;
    lastMenuSel = static_cast<uint32_t>(menuSel);
    lastTitlePhase = static_cast<uint32_t>(titlePhase);
}
