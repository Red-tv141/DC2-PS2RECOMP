static bool f56_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F56");
    return enabled;
}

static void f56_log_dungeon_state(uint8_t *rdram, uint32_t n)
{
    if (!f56_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static int32_t s_prevStatus = -1;
    static int32_t s_prevTree = -1;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);          // DngStatus
    const int32_t tree = (int32_t)(dc2_read_u32(rdram, 0x003773F4u) & 0xFFFFu); // DngTreeMode (half)
    const bool changed = (status != s_prevStatus) || (tree != s_prevTree);
    if ((changed || (n % 60u == 0u)) && s_lines < 240u)
    {
        ++s_lines;
        const char *mode = (status == 0) ? "3D-DngMainDraw" : ((status == 1 || status == 4) ? "MENU-MainDraw" : ((status == 2) ? "EVENT" : ((status == 3) ? "EVENT-EDIT" : ((status == 5) ? "EXIT" : "?"))));
        std::fprintf(stderr,
                     "[F56:state] n=%u frame=%u DngStatus=%d(%s) DngTreeMode=%d%s\n",
                     n, g_f40_frame_counter, status, mode, tree,
                     changed ? " *CHANGED*" : "");
        s_prevStatus = status;
        s_prevTree = tree;
    }
}

// F57: trace the dungeon floor-entrance EVENT state machine. After the treemap
// confirm advances DngStatus 4->2 (EVENT), RunMainEvent@0x1D1360 sets DngStatus=0
// (free-roam) ONLY when EventLoop@0x2555E0 returns 1, which happens when the event
// CRunScript at 0x1ECE3D0 reports finished (its +0x3c flag, aliased DAT_01ece40c).
// Headlessly that never fires, so the event never terminates. This probe surfaces
// WHICH opcode/wait the script parks on.
//   EventLoop globals:
//     DAT_01ece40c (0x01ECE40C) = script-done flag (== CRunScript+0x3c). !=0 -> EdEventEnd, return 1.
//     DAT_01ece4fc (0x01ECE4FC) = pending return-action code (0xf=load map, 3/4/7/8/...).
//     DAT_01ece500 (0x01ECE500) = door/camera sub-mode (1/2/3/4 = special, 0 = run script).
//     DAT_01ece504 (0x01ECE504) = sound-fade state (1/2/3 = fade/stream skip path).
//     DAT_01ece508 (0x01ECE508) = skip-event arming value.
//   CRunScript@0x1ECE3D0 cursor (layout from resume/run/exe __10CRunScript):
//     +0x38 (0x01ECE408) = current vmcode_t* (resume runs exe while non-zero);
//                          *(vmcode) = opcode id (the exe switch value) = the parked command.
//     +0x40 (0x01ECE410) = skip flag, +0x50 (0x01ECE420) = wait/yield flag.
// Quiet unless DC2_TRACE_F57. Logs on any state change + every 60th event frame, capped.
static bool f57_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F57");
    return enabled;
}

static void f57_log_event_state(uint8_t *rdram, uint32_t n)
{
    if (!f57_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u); // DngStatus
    // Only meaningful during/after the entrance event (status 2 EVENT, or 0 once it
    // terminates). Skip the menu/treemap frames to keep the trace on the blocker.
    if (status != 2 && status != 0)
        return;
    const uint32_t done = dc2_read_u32(rdram, 0x01ECE40Cu);   // CRunScript+0x3c
    const uint32_t act = dc2_read_u32(rdram, 0x01ECE4FCu);    // DAT_01ece4fc
    const uint32_t door = dc2_read_u32(rdram, 0x01ECE500u);   // DAT_01ece500
    const uint32_t snd = dc2_read_u32(rdram, 0x01ECE504u);    // DAT_01ece504
    const uint32_t skip = dc2_read_u32(rdram, 0x01ECE508u);   // DAT_01ece508
    const uint32_t pc = dc2_read_u32(rdram, 0x01ECE408u);     // CRunScript+0x38 (vmcode_t*)
    const uint32_t skipf = dc2_read_u32(rdram, 0x01ECE410u);  // CRunScript+0x40
    const uint32_t yield = dc2_read_u32(rdram, 0x01ECE420u);  // CRunScript+0x50
    const uint32_t op = (pc && pc < 0x02000000u) ? dc2_read_u32(rdram, pc) : 0xFFFFFFFFu; // *(vmcode)=opcode
    const uint32_t sig = status ^ (done << 1) ^ (act << 4) ^ (door << 8) ^ (snd << 12) ^ (op << 16) ^ pc;
    if ((sig != s_prevSig || (n % 60u == 0u)) && s_lines < 300u)
    {
        ++s_lines;
        std::fprintf(stderr,
                     "[F57:event] n=%u frame=%u DngStatus=%d done=0x%x act=0x%x door=0x%x snd=0x%x skip=0x%x pc=0x%x op=0x%x skipf=0x%x yield=0x%x\n",
                     n, g_f40_frame_counter, status, done, act, door, snd, skip, pc, op, skipf, yield);
        s_prevSig = sig;
    }
}

// F59: free-roam (DngStatus==0) render brightness. The event scene (status 2)
// renders the full 3D world bright (~631830 NonZero); on entering free-roam the
// frame drops to ~50000. DngMainDraw@0x1CF090 is the SAME code for status {0,2,3},
// so dim free-roam must be DATA-driven. This reads the exact guest globals that
// function consumes:
//   DngMainMap      = *(gp-0x724c) @0x3772A4 (159632) -- gates the whole map +
//                     character geometry block at 159678; 0 => only sky+HUD draw.
//   BattleAreaScene = *(gp-0x7250) @0x3772A0
//      +0x6c (float) = light-color SCALE multiplied into EVERY light component
//                      (159639-159653); near-0 => unlit/black geometry.
//      +0x08 (uint)  = draw flags: &0x20 skip sky, &0x80 skip map+chara draw.
//   DngMainScene    = *(gp-0x7254) @0x37729C (+0x2e54 cam idx, +0x2e5c map idx).
// PCSX2 ground truth (status 2): DngMainMap=0x106a030, lightScale=1.0, flags=0x401,
// camIdx=1, mapIdx=0. Quiet unless DC2_TRACE_F59. Logs on change + every 60th frame.
static bool f59_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F59");
    return enabled;
}

static void f59_log_freeroam_state(uint8_t *rdram, uint32_t n)
{
    if (!f59_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
    if (status != 0 && status != 2) // free-roam, plus event for A/B contrast
        return;
    const uint32_t dngMap = dc2_read_u32(rdram, 0x003772A4u);
    const uint32_t bas = dc2_read_u32(rdram, 0x003772A0u);
    const uint32_t scene = dc2_read_u32(rdram, 0x0037729Cu);
    const uint32_t scaleU = bas ? dc2_read_u32(rdram, bas + 0x6cu) : 0u;
    float scale = 0.0f;
    std::memcpy(&scale, &scaleU, sizeof(float));
    const uint32_t flags = bas ? dc2_read_u32(rdram, bas + 0x08u) : 0u;
    const uint32_t camIdx = scene ? dc2_read_u32(rdram, scene + 0x2e54u) : 0u;
    const uint32_t mapIdx = scene ? dc2_read_u32(rdram, scene + 0x2e5cu) : 0u;
    const uint32_t sig = (uint32_t)status ^ (dngMap << 1) ^ (scaleU << 3) ^ (flags << 7) ^ (camIdx << 11) ^ (mapIdx << 15);
    if ((sig != s_prevSig || (n % 60u == 0u)) && s_lines < 240u)
    {
        ++s_lines;
        // G223: DAT_01e9f6e8 = InitEyeCamera's unconditional "eye-camera init done"
        // sentinel (also read as the DngMainKey movement gate elsewhere) — a
        // cheap proxy for "has InitEyeCamera__FP12CActionChara run yet" without
        // instrumenting that direct-jal-only function (which has a runner/
        // shadow duplicate and can't be registerFunction-hooked).
        const uint32_t eyeCamSentinel = dc2_read_u32(rdram, 0x01E9F6E8u);
        // G224: DngMainKey's normal (third-person) camera path gates:
        //   *(BAS+0x54)  = camera mode (0 => ControlOn + GetCameraPoly + MoveCamera;
        //                  1/2 => direct SetPos blocks) -- 0x1d241c.
        //   DAT_003D8070 = manual right-stick camera state (nonzero skips MoveCamera).
        const uint32_t camMode = bas ? dc2_read_u32(rdram, bas + 0x54u) : 0xDEADu;
        const uint32_t manualCam = dc2_read_u32(rdram, 0x003D8070u);
        // G223: MainChara ptr @ gp-0x7228 = 0x003772C8; world pos @ +0xe0.
        const uint32_t mainChara = dc2_read_u32(rdram, 0x003772C8u);
        float mcx = 0, mcy = 0, mcz = 0;
        // G224: MainChara's virtual GetPos (vtbl+0x18) is GetPosition__9mgCObjectFPf,
        // which returns the mgCObject row at +0x10 -- NOT the +0xE0 field. The camera
        // follow target is fed from +0x10, so log both to catch a +0x10/+0xE0 desync.
        float ocx = 0, ocy = 0, ocz = 0;
        if (mainChara)
        {
            uint32_t xu = dc2_read_u32(rdram, mainChara + 0xe0u);
            uint32_t yu = dc2_read_u32(rdram, mainChara + 0xe4u);
            uint32_t zu = dc2_read_u32(rdram, mainChara + 0xe8u);
            std::memcpy(&mcx, &xu, 4);
            std::memcpy(&mcy, &yu, 4);
            std::memcpy(&mcz, &zu, 4);
            xu = dc2_read_u32(rdram, mainChara + 0x10u);
            yu = dc2_read_u32(rdram, mainChara + 0x14u);
            zu = dc2_read_u32(rdram, mainChara + 0x18u);
            std::memcpy(&ocx, &xu, 4);
            std::memcpy(&ocy, &yu, 4);
            std::memcpy(&ocz, &zu, 4);
        }
        std::fprintf(stderr,
                     "[F59:freeroam] n=%u frame=%u DngStatus=%d DngMainMap=0x%x BAS=0x%x lightScale=%.4f flags=0x%x camIdx=%u mapIdx=%u eyeCamSentinel=0x%x camMode=0x%x manualCam=0x%x mainChara=0x%x pos=(%.2f,%.2f,%.2f) objPos=(%.2f,%.2f,%.2f)\n",
                     n, g_f40_frame_counter, status, dngMap, bas, scale, flags, camIdx, mapIdx, eyeCamSentinel, camMode, manualCam, mainChara, mcx, mcy, mcz, ocx, ocy, ocz);
        // G223: dump both CSceneCamera slots' assignment state + camera world pos.
        // Layout (AssignCamera/GetSceneCamera/AssignData): CSceneCamera[id] at
        // scene+0x2048+id*0x38; +0x0 flags (bit 0x4 = assigned), +0x34 = mgCCamera*.
        // mgCCamera position is at object offset 0x0 (GetPos passes 'this' straight
        // into sceVu0CopyVector with no offset).
        if (scene)
        {
            for (uint32_t id = 0; id < 3u; ++id)
            {
                const uint32_t elem = scene + 0x2048u + id * 0x38u;
                const uint32_t camFlags = dc2_read_u32(rdram, elem + 0x0u);
                const uint32_t camPtr = dc2_read_u32(rdram, elem + 0x34u);
                float px = 0, py = 0, pz = 0;
                if (camPtr)
                {
                    uint32_t pxu = dc2_read_u32(rdram, camPtr + 0x0u);
                    uint32_t pyu = dc2_read_u32(rdram, camPtr + 0x4u);
                    uint32_t pzu = dc2_read_u32(rdram, camPtr + 0x8u);
                    std::memcpy(&px, &pxu, 4);
                    std::memcpy(&py, &pyu, 4);
                    std::memcpy(&pz, &pzu, 4);
                }
                std::fprintf(stderr,
                             "[G223:cam] n=%u id=%u camFlags=0x%x camPtr=0x%x pos=(%.2f,%.2f,%.2f)\n",
                             n, id, camFlags, camPtr, px, py, pz);
                // G224: CCameraControl internals for slot 0 (the free-roam camera).
                // +0x20 row = MoveCamera's working/next camera pos, +0x30 row = next
                // look-at ref, +0xC0 = ControlOn active flag. If +0x20 tracks the
                // player while +0x0 stays frozen, the commit/interp step is the gap;
                // if +0x20 is frozen too, MoveCamera itself is no-op'ing.
                if (id == 0u && camPtr)
                {
                    float rows[9] = {};
                    for (uint32_t w = 0; w < 3u; ++w)
                    {
                        uint32_t r0 = dc2_read_u32(rdram, camPtr + 0x20u + w * 4u);
                        uint32_t r1 = dc2_read_u32(rdram, camPtr + 0x30u + w * 4u);
                        uint32_t r2 = dc2_read_u32(rdram, camPtr + 0x10u + w * 4u);
                        std::memcpy(&rows[w], &r0, 4);
                        std::memcpy(&rows[3u + w], &r1, 4);
                        std::memcpy(&rows[6u + w], &r2, 4);
                    }
                    const uint32_t ctlOn = dc2_read_u32(rdram, camPtr + 0xC0u);
                    const uint32_t actIdx = dc2_read_u32(rdram, camPtr + 0xF0u);
                    float fol[3] = {};
                    for (uint32_t w = 0; w < 3u; ++w)
                    {
                        uint32_t fu = dc2_read_u32(rdram, camPtr + 0x70u + w * 4u);
                        std::memcpy(&fol[w], &fu, 4);
                    }
                    // G224: the camera's +0x60 vtable drives BOTH the per-frame
                    // Step (slot +0x08, from DngMainDraw) and SetFollow (slot +0x20,
                    // from DngMainKey). On real HW it is 0x376330. If it (or its
                    // slots) read differently here, every camera vtable dispatch is
                    // routing to the wrong function.
                    const uint32_t vt60 = dc2_read_u32(rdram, camPtr + 0x60u);
                    const uint32_t slotStep = vt60 ? dc2_read_u32(rdram, vt60 + 0x8u) : 0u;
                    const uint32_t slotSetF = vt60 ? dc2_read_u32(rdram, vt60 + 0x20u) : 0u;
                    std::fprintf(stderr,
                                 "[G224:cam0] n=%u ref=(%.2f,%.2f,%.2f) nextPos=(%.2f,%.2f,%.2f) nextRef=(%.2f,%.2f,%.2f) follow=(%.2f,%.2f,%.2f) ctlOn=%u actIdx=%u vt60=0x%x step=0x%x setf=0x%x\n",
                                 n, rows[6], rows[7], rows[8], rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], fol[0], fol[1], fol[2], ctlOn, actIdx, vt60, slotStep, slotSetF);
                }
            }
        }
        s_prevSig = sig;
    }
}

// G224: probe on GetPoly__8CEditMap@0x1B0780 (DISPATCH_TABLE_TARGET, jalr-dispatched,
// so registerFunction fires -- unlike the DIRECT_JAL_ONLY camera-path functions).
// DngMainKey's normal camera block calls it (via the GetCameraPoly@0x15F290 thunk,
// polyType a1=3) right before MoveCamera; a NEGATIVE return takes the
// "camera ply not found" printf + early-return path at 0x1d25f4, which would leave the
// free-roam camera frozen. Logging camera-type queries answers BOTH open questions:
// (a) does DngMainKey reach its camera block each free-roam frame, (b) does the poly
// query fail. Gated by DC2_TRACE_F59 (same gate as the rest of the G223/G224 traces).
// Preempt-suppressed around the direct body call (G212 pattern) so the logged v0 is a
// real completed-call result, never a mid-resume artifact.
static void g224_getpoly_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t polyType = getRegU32(ctx, 5);
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const bool trace = f59_trace_enabled() && polyType == 3u;
    // Preempt-suppress only while tracing (so the logged v0 is a completed-call
    // result); on the default path leave preemption semantics untouched.
    if (trace)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    GetPoly__8CEditMapFiP6CCPolyR9mgVu0FBOXi_0x1b0780(rdram, ctx, runtime);
    if (trace)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    if (trace)
    {
        static uint32_t s_lines = 0;
        const int32_t ret = (int32_t)getRegU32(ctx, 2);
        if (s_lines < 200u || (ret < 0 && s_lines < 400u))
        {
            ++s_lines;
            std::fprintf(stderr,
                         "[G224:campoly] n=%u self=0x%x type=%u ra=0x%x ret=%d\n",
                         s_lines, self, polyType, ra, ret);
        }
    }
}

// G224: probe on Step__14CCameraControlFi@0x2EC110 (vtable slot +0x08 of the scene
// camera's control vtable @0x376330 -- jalr-dispatched, hookable). This is the step
// that ultimately commits mgCCamera nextPos(+0x20)->pos(+0x0) via Step__9mgCCamera.
// If it never fires for cam 0x1EA5830 during free-roam, the camera-step iteration is
// the gap; if it fires but pos stays frozen, the Step__9mgCCamera guards
// (this+0x5C suspend / StopCamera@0x376C00) are the gap.
static void g224_camctl_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t arg = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    Step__14CCameraControlFi_0x2ec110(rdram, ctx, runtime);
    if (f59_trace_enabled())
    {
        static uint32_t s_lines = 0;
        if (s_lines < 240u)
        {
            ++s_lines;
            const uint32_t susp = dc2_read_u32(rdram, self + 0x5Cu);
            const uint32_t stop = dc2_read_u32(rdram, 0x00376C00u);
            std::fprintf(stderr,
                         "[G224:camstep] n=%u self=0x%x arg=%d ra=0x%x susp=0x%x stopCam=0x%x\n",
                         s_lines, self, (int32_t)arg, ra, susp, stop);
        }
    }
}

// G224: entry probe on DngMainDraw@0x1CF090 (called from LoopDungeonMain@0x1CEB70
// through a hasFunction() guard). Zero hits while DngMainKey runs every frame would
// mean LoopDungeonMain's own flow never reaches its draw dispatch on this route.
static void g224_dngmaindraw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f59_trace_enabled())
    {
        static uint32_t s_lines = 0;
        if (s_lines < 120u)
        {
            ++s_lines;
            const uint32_t ra = getRegU32(ctx, 31);
            const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
            std::fprintf(stderr, "[G224:dmd] n=%u ra=0x%x DngStatus=%d pc=0x%x\n",
                         s_lines, ra, status, ctx->pc);
        }
    }
    DngMainDraw__Fv_0x1cf090(rdram, ctx, runtime);
}

// G224: probe on SetFollow__15mgCCameraFollowFfff@0x131990 (vtable slot +0x20 of the
// camera control vtable @0x376330, jalr-dispatched from DngMainKey@0x1d2120 with the
// player-derived follow point in f12/f13/f14). Logs the exact follow target the game
// hands the free-roam camera each frame.
static void g224_setfollow_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const float x = ctx->f[12], y = ctx->f[13], z = ctx->f[14];
    SetFollow__15mgCCameraFollowFfff_0x131990(rdram, ctx, runtime);
    if (f59_trace_enabled())
    {
        static uint32_t s_lines = 0;
        if (s_lines < 240u)
        {
            ++s_lines;
            std::fprintf(stderr,
                         "[G224:setfollow] n=%u self=0x%x ra=0x%x follow=(%.2f,%.2f,%.2f)\n",
                         s_lines, self, ra, x, y, z);
        }
    }
}

// F59: UNCONDITIONAL per-presented-frame logger (called from f29 hook, fires every
// frame regardless of active loop). Maps DngStatus + DngMainMap + lightScale across
// the bright->dim transition that f59_log_freeroam_state's sig-dedup hides. Every 30th.
static void f59_log_frame(uint8_t *rdram, uint32_t n, uint32_t loopNo)
{
    if (!f59_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    if (n % 30u != 0u || s_lines >= 400u)
        return;
    ++s_lines;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
    const uint32_t dngMap = dc2_read_u32(rdram, 0x003772A4u);
    const uint32_t bas = dc2_read_u32(rdram, 0x003772A0u);
    const uint32_t scene = dc2_read_u32(rdram, 0x0037729Cu);
    const uint32_t scaleU = bas ? dc2_read_u32(rdram, bas + 0x6cu) : 0u;
    float scale = 0.0f;
    std::memcpy(&scale, &scaleU, sizeof(float));
    const uint32_t flags = bas ? dc2_read_u32(rdram, bas + 0x08u) : 0u;
    const uint32_t mapIdx = scene ? dc2_read_u32(rdram, scene + 0x2e5cu) : 0xDEADu;
    std::fprintf(stderr,
                 "[F59:frame] n=%u frame=%u loopNo=%u DngStatus=%d DngMainMap=0x%x scale=%.3f flags=0x%x mapIdx=%d\n",
                 n, g_f40_frame_counter, loopNo, status, dngMap, scale, flags, (int32_t)mapIdx);
}

static bool g195_camera_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G195_CAMERA_TRACE");
    return enabled;
}

static float dc2_read_f32(uint8_t *rdram, uint32_t addr)
{
    const uint32_t bits = dc2_read_u32(rdram, addr);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static void g195_log_camera_state(uint8_t *rdram, uint32_t n, uint32_t loopNo)
{
    if (!g195_camera_trace_enabled())
        return;

    static uint32_t s_lines = 0u;
    if ((n % 30u) != 0u || s_lines >= 220u)
        return;
    ++s_lines;

    const uint32_t mainScene = dc2_read_u32(rdram, 0x003771A0u);
    const uint32_t sceneCamId = mainScene ? dc2_read_u32(rdram, mainScene + 0x2E54u) : 0xFFFFFFFFu;
    const uint32_t sceneMapId = mainScene ? dc2_read_u32(rdram, mainScene + 0x2E5Cu) : 0xFFFFFFFFu;
    const uint32_t camera = dc2_read_u32(rdram, 0x0037714Cu);
    const uint32_t eventCamera = dc2_read_u32(rdram, 0x00377150u);
    const uint32_t fixCamera = dc2_read_u32(rdram, 0x00377154u);
    const uint32_t editCamera = dc2_read_u32(rdram, 0x00377158u);
    const uint32_t active = camera ? dc2_read_u32(rdram, camera + 0xF0u) : 0xFFFFFFFFu;
    const uint32_t param = (camera && active < 4u) ? (camera + 0xF4u + active * 0x2Cu) : 0u;
    const uint32_t backup = camera ? (camera + 0x1A4u) : 0u;

    float p[10]{};
    float b[10]{};
    for (uint32_t i = 0; i < 10u; ++i)
    {
        p[i] = param ? dc2_read_f32(rdram, param + i * 4u) : 0.0f;
        b[i] = backup ? dc2_read_f32(rdram, backup + i * 4u) : 0.0f;
    }
    const uint32_t pFlags = param ? dc2_read_u32(rdram, param + 0x28u) : 0u;
    const uint32_t bFlags = backup ? dc2_read_u32(rdram, backup + 0x28u) : 0u;

    std::fprintf(stderr,
                 "[G195:camera] n=%u frame=%u loop=%u scene=0x%x sceneCam=%u sceneMap=%u "
                 "cam=0x%x evt=0x%x fix=0x%x edit=0x%x active=%u "
                 "pos={%.2f,%.2f,%.2f} ref={%.2f,%.2f,%.2f} "
                 "param=0x%x p={%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f} pflags=0x%x "
                 "backup={%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f} bflags=0x%x\n",
                 n,
                 g_f40_frame_counter,
                 loopNo,
                 mainScene,
                 sceneCamId,
                 sceneMapId,
                 camera,
                 eventCamera,
                 fixCamera,
                 editCamera,
                 active,
                 camera ? dc2_read_f32(rdram, camera + 0x00u) : 0.0f,
                 camera ? dc2_read_f32(rdram, camera + 0x04u) : 0.0f,
                 camera ? dc2_read_f32(rdram, camera + 0x08u) : 0.0f,
                 camera ? dc2_read_f32(rdram, camera + 0x10u) : 0.0f,
                 camera ? dc2_read_f32(rdram, camera + 0x14u) : 0.0f,
                 camera ? dc2_read_f32(rdram, camera + 0x18u) : 0.0f,
                 param,
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
                 pFlags,
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                 bFlags);
}

static bool g202_town_depth_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G202_TOWN_Z_STAT");
    return enabled;
}

static bool g202_town_depth_ready(uint8_t *rdram, uint32_t loopNo)
{
    if (loopNo != 1u) // edit/town loop; excludes debug menu, title/loading, and dungeon
        return false;
    const uint32_t mainScene = dc2_read_u32(rdram, 0x003771A0u);
    const uint32_t camera = dc2_read_u32(rdram, 0x0037714Cu);
    if (mainScene == 0u || camera == 0u)
        return false;
    const uint32_t sceneCamId = dc2_read_u32(rdram, mainScene + 0x2E54u);
    const uint32_t sceneMapId = dc2_read_u32(rdram, mainScene + 0x2E5Cu);
    const uint32_t activeCam = dc2_read_u32(rdram, camera + 0xF0u);
    return sceneCamId < 0x80u && sceneMapId < 0x80u && activeCam < 4u;
}

static void g202_update_town_depth_scope(uint8_t *rdram, uint32_t n, uint32_t scriptFrame, uint32_t loopNo)
{
    const bool ready = g202_town_depth_ready(rdram, loopNo);
    const bool prev = g_dc2TownDepthScope.exchange(ready, std::memory_order_relaxed);
    if (!g202_town_depth_trace_enabled())
        return;

    static uint32_t s_lines = 0u;
    if (s_lines >= 80u || (prev == ready && (n % 120u) != 0u))
        return;
    ++s_lines;

    const uint32_t mainScene = dc2_read_u32(rdram, 0x003771A0u);
    const uint32_t camera = dc2_read_u32(rdram, 0x0037714Cu);
    const uint32_t sceneCamId = mainScene ? dc2_read_u32(rdram, mainScene + 0x2E54u) : 0xFFFFFFFFu;
    const uint32_t sceneMapId = mainScene ? dc2_read_u32(rdram, mainScene + 0x2E5Cu) : 0xFFFFFFFFu;
    const uint32_t activeCam = camera ? dc2_read_u32(rdram, camera + 0xF0u) : 0xFFFFFFFFu;
    std::fprintf(stderr,
                 "[G202:scope] n=%u frame=%u loop=%u ready=%u prev=%u scene=0x%x sceneCam=%u sceneMap=%u camera=0x%x active=%u\n",
                 n, scriptFrame, loopNo, ready ? 1u : 0u, prev ? 1u : 0u, mainScene,
                 sceneCamId, sceneMapId, camera, activeCam);
}

// F57: per-PRESENTED-FRAME logger (called from f29_mgendframe_probe, which fires
// every frame regardless of which loop function is active). Unlike f57_log_event_state
// (only hit while LoopDungeonMain@0x1CEA00 runs), this survives a loop swap, so it
// reveals what the game transitions to if the dungeon loop exits. Logs the current
// loop number (gp-0x7524) + next loop (gp-0x7520) alongside DngStatus + event globals.
static void f57_log_frame_loop(uint8_t *rdram, uint32_t n, uint32_t loopNo, uint32_t nextLoopNo)
{
    if (!f57_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u); // DngStatus
    const uint32_t done = dc2_read_u32(rdram, 0x01ECE40Cu);
    const uint32_t door = dc2_read_u32(rdram, 0x01ECE500u);
    const uint32_t snd = dc2_read_u32(rdram, 0x01ECE504u);
    const uint32_t pc = dc2_read_u32(rdram, 0x01ECE408u);
    const uint32_t op = (pc && pc < 0x02000000u) ? dc2_read_u32(rdram, pc) : 0xFFFFFFFFu;
    const uint32_t sig = loopNo ^ (nextLoopNo << 4) ^ ((uint32_t)status << 8) ^ (door << 12) ^ (snd << 16) ^ (op << 20);
    if ((sig != s_prevSig || (n % 120u == 0u)) && s_lines < 400u)
    {
        ++s_lines;
        std::fprintf(stderr,
                     "[F57:loop] n=%u loopNo=%u nextLoop=%u DngStatus=%d done=0x%x door=0x%x snd=0x%x op=0x%x\n",
                     n, loopNo, nextLoopNo, status, done, door, snd, op);
        s_prevSig = sig;
    }
}

// F56: trace the floor-select treemap state machine. param object (a0) layout
// (from Step__12CMenuTreeMapFv decompile): *(short)(this+0) = top menu state
// (0xc=jump/confirm-pending, 2=run jump script, 1=enter/fade, 0/else=interactive
// nav+button), *(short)(this+0x14) = interactive sub-state (psVar17[10]:
// 0=floor navigation, 1=enter/decide, 2=L/R page). Logs the trajectory + the
// active scripted pad so we can see whether confirm presses reach the menu and
// which sub-state gates the dungeon load. Quiet unless DC2_TRACE_F56.
static void f56_treemap_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f56_trace_enabled())
    {
        static uint32_t s_calls = 0;
        static uint32_t s_lines = 0;
        static int32_t s_prevTop = -1;
        static int32_t s_prevSub = -1;
        const uint32_t self = getRegU32(ctx, 4);
        const int32_t top = self ? (int32_t)(dc2_read_u32(rdram, self + 0x00u) & 0xFFFFu) : -1;
        const int32_t sub = self ? (int32_t)(dc2_read_u32(rdram, self + 0x14u) & 0xFFFFu) : -1;
        // CGamePad singleton @0x3D76E0: +0x4 = current held buttons (CGamePad's
        // own bit layout, tested by Down__8CGamePad), +0x9c = previous frame.
        // Logging this while injecting a known scePad bit reveals the remap that
        // MenuCheckPushButton consumes (decide = Down(0x10) -> uVar7==4 -> top=0xc).
        const uint32_t cgcur = dc2_read_u32(rdram, 0x003D76E4u);
        // psVar17[0x90] (byte 0x120) = currently-selected room GLID_INFO* (the
        // floor the confirm would enter). psVar17[0x8c] (byte 0x118) = dungeon id.
        // If room==0 the confirm path beeps (MenuSePlay 5) instead of arming the
        // DngAskMessageDrawFlag "enter floor?" dialog -> 3D never loads.
        const uint32_t room = self ? dc2_read_u32(rdram, self + 0x120u) : 0u;
        const uint32_t dngId = self ? (dc2_read_u32(rdram, self + 0x118u) & 0xFFFFu) : 0u;
        // padtbl_3372 (US +8 = 0x350c68): MenuCheckPushButton maps RIGHT->*0x350c68,
        // DOWN->*0x350c6c. Whichever == 1 is the uVar7==1 "arm enter-dialog" button.
        const uint32_t padR = dc2_read_u32(rdram, 0x00350C68u);
        const uint32_t padD = dc2_read_u32(rdram, 0x00350C6Cu);
        const uint32_t c = ++s_calls;
        const bool changed = (top != s_prevTop) || (sub != s_prevSub);
        if ((changed || g_f40_pressed != 0u || (c % 60u == 0u)) && s_lines < 220u)
        {
            ++s_lines;
            std::fprintf(stderr,
                         "[F56:treemap] c=%u frame=%u this=0x%x top=%d sub=%d pad=0x%04x cgcur=0x%08x room=0x%x dng=%u padR=%u padD=%u%s\n",
                         c, g_f40_frame_counter, self, top, sub,
                         (unsigned)g_f40_pressed, cgcur, room, dngId, padR, padD, changed ? " *CHANGED*" : "");
            s_prevTop = top;
            s_prevSub = sub;
        }
    }
    Step__12CMenuTreeMapFv_0x1eff40(rdram, ctx, runtime);
}

static void f48_4_loop_dungeon_main_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_main_log;
    f56_log_dungeon_state(rdram, n);
    f57_log_event_state(rdram, n);
    f59_log_freeroam_state(rdram, n);
    if (f48_4_trace_postcostume_enabled() && n <= 6u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=LoopDungeonMain.enter n=%u frame=%u ra=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31));
    }
    // F50.4: on the first dungeon frame, dump the player-character state that
    // DngStep@0x1D06C0 dereferences. The crash is a null-pointer vtable call:
    //   lw a0,-0x7228(gp) ; MainChara   -> a0=0x0 at [dispatch:first-bad-pc]
    //   lw t9,0x0(a0)     ; *MainChara  -> reads guest_ram[0]
    //   lw t9,0xdc(t9)    ; vtable+0xdc -> 0x1, jalr -> bad=0x1
    // MainChara = GetCharacter(DngMainScene,0) (InitDungeonMain @0x1cc3bc). If
    // it is 0, slot 0 of the scene character array was never populated. Dump the
    // scene char count (scene+0x40, set to 0x80 by Initialize__6CSceneFv), slot 0
    // active flag (slot+0x00) and object (slot+0x34) to localise the break.
    if (f48_4_trace_postcostume_enabled() && n == 1u)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t mainChara = dc2_read_u32(rdram, gp + 0xFFFF8DD8u); // gp-0x7228
        const uint32_t scene = dc2_read_u32(rdram, gp + 0xFFFF8DACu);     // gp-0x7254 DngMainScene
        const uint32_t count = scene ? dc2_read_u32(rdram, scene + 0x40u) : 0u;
        const uint32_t slot0 = scene ? (scene + 0x44u) : 0u;
        const uint32_t slot0Flag = slot0 ? dc2_read_u32(rdram, slot0 + 0x00u) : 0u;
        const uint32_t slot0Obj = slot0 ? dc2_read_u32(rdram, slot0 + 0x34u) : 0u;
        const uint32_t objVt = mainChara ? dc2_read_u32(rdram, mainChara + 0x00u) : 0u;
        const uint32_t vtDc = objVt ? dc2_read_u32(rdram, objVt + 0xDCu) : 0u;
        // The scene-entry init at InitDungeonMain L158085 dispatches through the
        // CScene vtable embedded at scene+0x10548, slot +0x8. Dump that vtable and
        // its slot-8 target so we can see whether it resolves to the real
        // Initialize/InitAllData (which sets scene[0x40]=0x80) or a stub.
        const uint32_t sceneVt = scene ? dc2_read_u32(rdram, scene + 0x10548u) : 0u;
        const uint32_t sceneVt8 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x8u) : 0u;
        const uint32_t sceneVt4 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x4u) : 0u;
        const uint32_t sceneVt0 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x0u) : 0u;
        std::fprintf(stderr,
                     "[F50.4:mainchara] MainChara=0x%x scene=0x%x count=%u slot0=0x%x flag=0x%x slot0obj=0x%x vt=0x%x vt+0xdc=0x%x\n",
                     mainChara, scene, count, slot0, slot0Flag, slot0Obj, objVt, vtDc);
        std::fprintf(stderr,
                     "[F50.4:scenevt] sceneVt=0x%x [+0]=0x%x [+4]=0x%x [+8]=0x%x\n",
                     sceneVt, sceneVt0, sceneVt4, sceneVt8);
    }
    // F64: the audio-gated entrance-event skip is driven from f29_mgendframe_probe
    // (mgEndFrame runs every frame inside LoopDungeonMain's internal loop; this
    // wrapper only fires once on entry).
    LoopDungeonMain__Fv_0x1cea00(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 6u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=LoopDungeonMain.exit n=%u frame=%u ret=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2));
    }
}

// F49.5: LoopExit[3] = TitleExit @0x29FF30 is dispatched via the MainLoop loop
// table when TitleLoop returns nonzero (NextLoop(2) set). Bracket it to see
// whether the loop-transition (break -> LoopExit -> LoopNo=2 -> sceGsSyncV ->
// LoopInit[2]) actually executes after the costume confirm. Quiet unless
// DC2_TRACE_F48_4 is set; bounded.
static uint32_t g_f48_4_titleexit_log = 0u;

static void f48_4_title_exit_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_titleexit_log;
    const uint32_t gp = getRegU32(ctx, 28);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=TitleExit.enter n=%u frame=%u ra=0x%x loop=%u next=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u));
    }
    TitleExit__Fv_0x29ff30(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=TitleExit.exit n=%u frame=%u\n",
                     n, g_f40_frame_counter);
    }
}

// F49.5: CaptureEnd @0x14B600 is the last call in MainLoop's loop-transition
// (line ~118461) immediately before the inter-loop sceGsSyncV sequence. If this
// fires after the costume confirm but the transition sceGsSyncV (n>9) does not,
// the hang is in CaptureEnd or the sceGsSyncV itself; if it never fires, the
// hang is in the buffer-copy block before it. Quiet unless DC2_TRACE_F48_4.
static uint32_t g_f48_4_captureend_log = 0u;

static void f48_4_capture_end_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_captureend_log;
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=CaptureEnd.enter n=%u frame=%u ra=0x%x a0=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31), getRegU32(ctx, 4));
    }
    CaptureEnd__8CGamePadFv_0x14b600(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=CaptureEnd.exit n=%u frame=%u\n",
                     n, g_f40_frame_counter);
    }
}

static void f49_finish_for_mc_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t pcBefore = ctx->pc;
    const uint32_t s0Before = getRegU32(ctx, 16);
    const uint32_t s1Before = getRegU32(ctx, 17);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=FinishForMC.enter n=%u ra=0x%x pc=0x%x s0=0x%x s1=0x%x\n",
                     n, ra, pcBefore,
                     s0Before, s1Before);
    }
    const auto s0 = ctx->r[16];
    const auto s1 = ctx->r[17];
    const auto s2 = ctx->r[18];
    const auto s3 = ctx->r[19];
    const auto s4 = ctx->r[20];
    const auto s5 = ctx->r[21];
    const auto s6 = ctx->r[22];
    const auto s7 = ctx->r[23];
    const auto t0 = std::chrono::steady_clock::now();
    FinishForMC__18CMemoryCardManagerFv_0x2f19a0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    const uint32_t pcAfterRaw = ctx->pc;
    bool tailReturnFixed = false;
    // FinishForMC is a tail jump to sceMcEnd (0x122560). If sceMcEnd leaves
    // PC unchanged, normalize to the original caller continuation.
    if (pcAfterRaw == 0x00122560u)
    {
        ctx->pc = ra;
        tailReturnFixed = true;
    }
    ctx->r[16] = s0;
    ctx->r[17] = s1;
    ctx->r[18] = s2;
    ctx->r[19] = s3;
    ctx->r[20] = s4;
    ctx->r[21] = s5;
    ctx->r[22] = s6;
    ctx->r[23] = s7;
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const uint32_t pcAfter = ctx->pc;
        const uint32_t raAfter = getRegU32(ctx, 31);
        const uint32_t s0After = getRegU32(ctx, 16);
        const uint32_t s1After = getRegU32(ctx, 17);
        std::fprintf(stderr,
                     "[F49:title] tag=FinishForMC.exit n=%u ra=0x%x ret=0x%x dtMs=%lld pcRaw=0x%x pc=0x%x tailFix=%u raAfter=0x%x s0=0x%x s1=0x%x\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs),
                     pcAfterRaw, pcAfter, tailReturnFixed ? 1u : 0u,
                     raAfter, s0After, s1After);
    }
}

static void f49_init_save_data_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr, "[F49:title] tag=InitSaveData.enter n=%u ra=0x%x\n", n, ra);
    }
    InitSaveData__Fv_0x1908a0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr, "[F49:title] tag=InitSaveData.exit n=%u ra=0x%x ret=0x%x\n", n, ra, getRegU32(ctx, 2));
    }
}

static void f49_get_user_data_man_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    GetUserDataMan__Fv_0x196be0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr, "[F49:title] tag=GetUserDataMan n=%u ra=0x%x ret=0x%x\n", n, ra, getRegU32(ctx, 2));
    }
}

static void f49_debug_get_item_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t mode = getRegU32(ctx, 5);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=DebugGetItem.enter n=%u ra=0x%x udm=0x%x mode=%u\n",
                     n, ra, udm, mode);
    }
    DebugGetItem__FP16CUserDataManageri_0x1a1b80(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=DebugGetItem.exit n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_set_chr_equip_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t slot = getRegU32(ctx, 5);
    const uint32_t equip = getRegU32(ctx, 6);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetChrEquip.enter n=%u ra=0x%x udm=0x%x slot=%u equip=%u\n",
                     n, ra, udm, slot, equip);
    }
    SetChrEquipDirect__16CUserDataManagerFii_0x19d560(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetChrEquip.exit n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_get_chara_data_ptr_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=GetCharaDataPtr.enter n=%u ra=0x%x udm=0x%x idx=%u\n",
                     n, ra, udm, idx);
    }
    GetCharaDataPtr__16CUserDataManagerFi_0x19b490(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=GetCharaDataPtr.exit n=%u ra=0x%x ret=0x%x pc=0x%x\n",
                     n, ra, getRegU32(ctx, 2), ctx->pc);
    }
}

static void f49_set_menu_load_item_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t arg = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetMenuLoadItemNo.enter n=%u ra=0x%x arg=%u pc=0x%x\n",
                     n, ra, arg, ctx->pc);
    }
    const auto t0 = std::chrono::steady_clock::now();
    SetMenuLoadItemNo__Fi_0x2afdb0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=SetMenuLoadItemNo.exit n=%u ra=0x%x dtMs=%lld pc=0x%x\n",
                     n, ra, static_cast<long long>(dtMs), ctx->pc);
    }
}

static void f49_menu_item_chara_data_load_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t mem = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t infos = getRegU32(ctx, 6);
    const uint32_t force = getRegU32(ctx, 7);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuItemCharaDataLoad.enter n=%u ra=0x%x mem=0x%x idx=%u infos=0x%x force=%u\n",
                     n, ra, mem, idx, infos, force);
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuItemCharaDataLoad__FP9mgCMemoryiPP17MENU_BGREAD_INFO2i_0x2b90d0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuItemCharaDataLoad.exit n=%u ra=0x%x dtMs=%lld pc=0x%x ret=0x%x\n",
                     n, ra, static_cast<long long>(dtMs), ctx->pc, getRegU32(ctx, 2));
    }
}

struct F48_2CharBgState
{
    int32_t readNo;
    int32_t monsterNo;
    int32_t statusBit;
    int32_t moveX;
    int32_t movePhase;
    int32_t readPhase;
    int32_t screenWidth;
    uint32_t nowReadMainChara;
    int32_t texMoveMode;
    uint32_t menuPtr;
    uint32_t menuStep;
    int32_t menuSel;
};

static uint32_t g_f48_2_read_call = 0u;
static uint32_t g_f48_2_last_read_call = 0u;
static int32_t g_f48_2_last_read_ret = -9999;

static F48_2CharBgState f48_2_read_charbg_state(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    F48_2CharBgState s{};
    s.readNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84ECu) & 0xFFFFu); // gp-0x7B14
    s.monsterNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84F0u) & 0xFFFFu); // gp-0x7B10
    s.statusBit = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C18u) & 0xFFFFu); // gp-0x63E8
    s.moveX = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C0Cu)); // gp-0x63F4
    s.movePhase = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C10u)); // gp-0x63F0
    s.readPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C00u) & 0xFFFFu); // gp-0x6400
    s.screenWidth = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8780u)); // gp-0x7880
    s.nowReadMainChara = dc2_read_u32(rdram, gp + 0xFFFF9C04u); // gp-0x63FC
    s.texMoveMode = static_cast<int8_t>(dc2_read_u32(rdram, gp + 0xFFFF9B71u) & 0xFFu); // gp-0x648F
    s.menuPtr = menuPtr;
    s.menuStep = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x54u) : 0xFFFFFFFFu;
    s.menuSel = menuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu) : -1;
    return s;
}

static const char *f48_2_read_block_reason(const F48_2CharBgState &pre,
                                           const F48_2CharBgState &post,
                                           int32_t ret)
{
    if (ret == 2)
        return "none/read_returned_2";
    if (pre.readNo < 0)
        return "0x2bbc8c NowReadMainCharaNo<0 early_ret0";
    if ((pre.statusBit & 2) == 0)
        return "0x2bbcb4 CheckPushButton_ret0";
    if (pre.readNo == 3 && pre.monsterNo < 0)
        return "unexpected/readNo3_monsterNo<0_should_ret2";
    if (pre.nowReadMainChara == 0u)
        return "0x2bbd04 NowReadMainChara_null_ret0";
    if (ret == 1 && post.readPhase == pre.readPhase)
        return "ReadBGSync_busy_or_phase_not_ready_ret1";
    if (ret == 1)
        return "ReadBG_state_machine_not_done_ret1";
    return "ret0_or_unknown";
}

static void f48_2_log_charbg(const char *tag,
                             uint32_t n,
                             uint32_t ra,
                             const F48_2CharBgState &pre,
                             const F48_2CharBgState &post,
                             int32_t ret,
                             const char *reason,
                             int32_t readRetForKey)
{
    if (!f48_2_trace_charbg_enabled() || g_f48_2_log_count >= 128u)
        return;

    const bool changed =
        pre.readNo != post.readNo ||
        pre.statusBit != post.statusBit ||
        pre.moveX != post.moveX ||
        pre.movePhase != post.movePhase ||
        pre.readPhase != post.readPhase ||
        pre.nowReadMainChara != post.nowReadMainChara ||
        pre.menuStep != post.menuStep;

    if (g_f48_2_log_count < 24u || changed || ret != 0 || (n % 30u) == 0u)
    {
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=%s n=%u frame=%u ra=0x%x menu=0x%x step=%u sel=%d readNo=%d->%d monsterNo=%d status=0x%x->0x%x moveX=%d->%d movePhase=%d->%d readPhase=%d->%d screenWidth=%d chara=0x%x->0x%x texMode=%d ret=%d readRet=%d block='%s'\n",
                     tag, n, g_f40_frame_counter, ra,
                     post.menuPtr, post.menuStep, post.menuSel,
                     pre.readNo, post.readNo, pre.monsterNo,
                     static_cast<uint32_t>(pre.statusBit), static_cast<uint32_t>(post.statusBit),
                     pre.moveX, post.moveX, pre.movePhase, post.movePhase,
                     pre.readPhase, post.readPhase, post.screenWidth,
                     pre.nowReadMainChara, post.nowReadMainChara, post.texMoveMode,
                     ret, readRetForKey, reason ? reason : "");
        ++g_f48_2_log_count;
    }
}

static void f48_2_read_main_chara_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_2_read_call;
    const uint32_t ra = getRegU32(ctx, 31);
    const F48_2CharBgState pre = f48_2_read_charbg_state(rdram, ctx);
    ReadMainCharaBG__Fv_0x2bbc80(rdram, ctx, runtime);
    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const F48_2CharBgState post = f48_2_read_charbg_state(rdram, ctx);
    g_f48_2_last_read_call = n;
    g_f48_2_last_read_ret = ret;
    f48_2_log_charbg("ReadMainCharaBG.exit", n, ra, pre, post, ret,
                     f48_2_read_block_reason(pre, post, ret), ret);
}

static void f48_2_key_main_chara_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t readCallBefore = g_f48_2_last_read_call;
    const F48_2CharBgState pre = f48_2_read_charbg_state(rdram, ctx);
    KeyMainCharaBG__Fv_0x2bc150(rdram, ctx, runtime);
    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const F48_2CharBgState post = f48_2_read_charbg_state(rdram, ctx);
    const int32_t readRet = (g_f48_2_last_read_call != readCallBefore) ? g_f48_2_last_read_ret : -9999;
    const char *reason = "none/key_returned_1";
    if (readRet != 2)
        reason = "0x2bc1fc ReadMainCharaBG_ret_ne_2";
    else if (post.moveX < post.screenWidth)
        reason = "0x2bc210 NowMainCharaChngTexMoveX<mgScreenWidth";
    f48_2_log_charbg("KeyMainCharaBG.exit", n, ra, pre, post, ret, reason, readRet);
}

static void f49_read_bg_sync_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t loadMenuSpinCount = 0u;
    static const uint32_t loadMenuSpinLimit = []() -> uint32_t
    {
        const char *value = std::getenv("DC2_READBGSYNC_SPIN_MAX");
        if (value == nullptr || *value == '\0')
            return 256u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || (end && *end != '\0') || parsed < 1ul || parsed > 1000000ul)
            return 256u;
        return static_cast<uint32_t>(parsed);
    }();
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const bool logThis = f49_trace_title_path_enabled() && (n <= 32u || (n % 120u) == 0u);
    const auto t0 = std::chrono::steady_clock::now();
    ReadBGSync__Fv_0x148e70(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    // LoadMenuData(0x2BC8D0) polls ReadBGSync at 0x2BCC10. In runtime this path
    // can spin forever on ret=0. Bound the spin and force ready at that callsite.
    if (ra == 0x002BCC10u)
    {
        if (getRegU32(ctx, 2) == 0u)
        {
            ++loadMenuSpinCount;
            if (loadMenuSpinCount >= loadMenuSpinLimit)
            {
                setReturnU32(ctx, 1u);
                if (f49_trace_title_path_enabled())
                {
                    std::fprintf(stderr,
                                 "[F49:title] tag=ReadBGSync.force ra=0x%x spins=%u limit=%u ret=1\n",
                                 ra, loadMenuSpinCount, loadMenuSpinLimit);
                }
                loadMenuSpinCount = 0u;
            }
        }
        else
        {
            loadMenuSpinCount = 0u;
        }
    }
    else
    {
        loadMenuSpinCount = 0u;
    }

    if (logThis)
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=ReadBGSync n=%u ra=0x%x ret=0x%x dtMs=%lld pc=0x%x\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs), ctx->pc);
    }
}
