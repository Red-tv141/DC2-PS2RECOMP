static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap); // fwd (defined below)

static bool g53_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G53");
    return enabled;
}

// G53: the main-title 3D background is map "s19", loaded by TitleInit@0x29f1f0 ->
// LoadMap@0x285ce0 (synchronous, last arg 0). LoadMap reads info+0x1a4 =
// GetStack(scene, info[1]); the required .ipk/.efp map files take their destination
// buffer from that stack (*(stack+0x20)+*(stack+0x24)*0x10). In the runner the title
// scene (MainScene @0x1dd8260) boots with its stack-slot table + slot counts ZERO (the
// scene's Initialize/__sinit does not fully run headless ג€” the same gap the debug-menu
// path patches via f50_7_seed_debug_map_stacks), so TitleInit's own
// SetStack(scene,1,0x1f060a0) no-ops (count==0) -> GetStack returns 0 -> the required
// .ipk load gets a NULL buffer -> LoadMapData fails -> the map is never built -> the
// title renders an empty (black) 3D scene. TitleInit DID allocate + fund the map stack
// at 0x1f060a0 (verified base!=0), so we just have to wire it into the scene's slot and
// give the scene non-zero slot counts so LoadMap/LoadMapFromMemory proceed. Self-gated on
// the slot being null (the broken title state) so the already-populated in-game dungeon
// scene is never touched. Kill switch DC2_G53_NO_SEED.
// G55: replicate Initialize__11CMdsListSetFv@0x1690d0 on the title scene's embedded
// CMdsListSet (scene+0x23d0 = 0x1dda630). The headless title scene's Initialize__6CScene
// does not fully run (the same gap G53 worked around for the map stack), so this set keeps
// count=0 (HW=8). With 0 slots, LoadPCPFile__11CMdsListSet cannot place the loaded s19_0.pcp
// model list -> SearchMDS fails -> map pieces get no visual -> all parts cull -> zero 3D
// geometry on the title. Sizing it to 8 empty slots (matching HW) lets LoadData's PCP loop
// fill slot for the title model pack. Gated on count==0 so a populated set is never wiped.
static void g55_init_title_mds_list_set(uint8_t *rdram, uint32_t scene)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G55_NO_MDS_INIT");
    if (disabled)
        return;
    const uint32_t set = scene + 0x23D0u; // MainScene 0x1dd8260 -> 0x1dda630
    if (dc2_read_u32(rdram, set + 0x00u) != 0u)
        return; // already initialized (HW count=8) ג€” do not clobber loaded lists

    dc2_write_u32(rdram, set + 0x00u, 8u); // 8 MDS list slots
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t s = set + 0x10u + i * 0x10u;
        dc2_write_u32(rdram, s + 0x00u, 0u);
        dc2_write_u32(rdram, s + 0x04u, 0u);
        dc2_write_u32(rdram, s + 0x08u, 0u);
    }
    dc2_write_u32(rdram, set + 0x90u, 0x10u); // param_1[0x24] = 16 IMG-list slots
    for (uint32_t i = 0u; i < 0x10u; ++i)
    {
        const uint32_t s = set + 0x94u + i * 0x08u;
        dc2_write_u32(rdram, s + 0x00u, 0u);
        dc2_write_u32(rdram, s + 0x04u, 0u);
    }
    if (g53_trace_enabled())
        std::fprintf(stderr, "[G55:mdsinit] scene=0x%x set=0x%x sized to 8 slots\n", scene, set);
}

static void g53_seed_title_map_stack(uint8_t *rdram, uint32_t scene, uint32_t stackSel)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G53_NO_SEED");
    if (disabled || scene != 0x01DD8260u || stackSel >= 0x0Cu)
        return;

    // G55: size the title scene's MDS model table before the map loads (independent of the
    // stack-slot early-return below, which skips on re-entry once the slot is wired).
    g55_init_title_mds_list_set(rdram, scene);

    const uint32_t slotAddr = scene + 0x08u + stackSel * 4u;
    if (dc2_read_u32(rdram, slotAddr) != 0u)
        return; // slot already wired (dungeon / already-seeded) ג€” leave it alone

    // The map stack TitleInit prepared (SetStack(TitleScene,1,0x1f060a0)). Only adopt it
    // if it is actually funded (base ptr set), so we never point the slot at garbage.
    const uint32_t titleMapStack = 0x01F060A0u;
    if (dc2_read_u32(rdram, titleMapStack + 0x20u) == 0u)
        return;

    if (dc2_read_u32(rdram, scene + 0x00u) < 0x0Cu)
        dc2_write_u32(rdram, scene + 0x00u, 0x0Cu); // stack-slot count
    dc2_write_u32(rdram, slotAddr, titleMapStack);

    // Scene slot-count config: only when unset, so LoadMapFromMemory has map/sky/object/
    // effect slots to populate. NOTE: unlike the debug seed we do NOT touch the +0x2e50..5c
    // index fields or memset the slot arrays ג€” the title sets its own current-camera index
    // (TitleMapDraw reads *(scene+0x2e54) as the camera id; the title's slot 0 = TitleCamera),
    // and forcing those to 0xFFFFFFFF broke GetCamera and suppressed the map draw.
    if (dc2_read_u32(rdram, scene + 0x27E0u) == 0u)
    {
        dc2_write_u32(rdram, scene + 0x0040u, 0x80u); // character slots
        dc2_write_u32(rdram, scene + 0x2044u, 8u);    // camera slots
        dc2_write_u32(rdram, scene + 0x2208u, 8u);    // message slots
        dc2_write_u32(rdram, scene + 0x27E0u, 4u);    // map slots
        dc2_write_u32(rdram, scene + 0x28C4u, 4u);    // sky slots
        dc2_write_u32(rdram, scene + 0x29A8u, 4u);    // game object slots
        dc2_write_u32(rdram, scene + 0x2AACu, 8u);    // effect slots
    }

    if (g53_trace_enabled())
        std::fprintf(stderr, "[G53:seed] scene=0x%x slot=%u <- 0x%x (title map stack)\n",
                     scene, stackSel, titleMapStack);
}

static void f50_7_load_map_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t info = getRegU32(ctx, 6);
    const uint32_t stackSel = info ? dc2_read_u32(rdram, info + 0x04u) : 0xFFFFFFFFu;
    f50_7_seed_debug_map_stacks(rdram, scene, stackSel);
    g53_seed_title_map_stack(rdram, scene, stackSel);
    // G53: title 3D background = map "s19" loaded via TitleInit->LoadMap. Confirm the
    // title's LoadMap actually fires and what it leaves in the scene's map count.
    if (g53_trace_enabled())
    {
        const uint32_t mapId = getRegU32(ctx, 5);
        const uint32_t cntBefore = dc2_read_u32(rdram, scene + 0x00u);
        const uint32_t mapCntBefore = dc2_read_u32(rdram, scene + 0x27E0u);
        // SCN_LOADMAP_INFO2: +0x24 = first-entry "present" flag (LoadMapData breaks if 0),
        // +0x28 = dir string, +0x48 = base name. Empty +0x24 => GetLoadMapInfo got a bad
        // map number (SearchMapNo failed) => no map files requested.
        const uint32_t e0present = info ? dc2_read_u32(rdram, info + 0x24u) : 0xFFFFFFFFu;
        char dir[64] = {0}, base[64] = {0};
        if (info) { f55_read_cstr(rdram, info + 0x28u, dir, sizeof(dir));
                    f55_read_cstr(rdram, info + 0x48u, base, sizeof(base)); }
        LoadMap__6CSceneFiP17SCN_LOADMAP_INFO2i_0x285ce0(rdram, ctx, runtime);
        // info+0x1a4 (=info[0x69]) = LoadMap's GetStack(scene,stackSel); the .ipk/.efp dest
        // buffers derive from *(stackPtr+0x20)+*(stackPtr+0x24)*0x10. A null stackPtr or null
        // stack base => NULL .ipk buffer => required-file load fails => map never built.
        const uint32_t stackPtr = info ? dc2_read_u32(rdram, info + 0x1A4u) : 0u;
        const uint32_t stackBase = stackPtr ? dc2_read_u32(rdram, stackPtr + 0x20u) : 0u;
        const uint32_t stackCur = stackPtr ? dc2_read_u32(rdram, stackPtr + 0x24u) : 0u;
        const uint32_t sceneSlot = (stackSel < 8u) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u;
        // Candidate title-scene stacks allocated by TitleInit (305249/305251) and their bases.
        const uint32_t t_a0_base = dc2_read_u32(rdram, 0x01F060A0u + 0x20u);
        const uint32_t t_a0_cur  = dc2_read_u32(rdram, 0x01F060A0u + 0x24u);
        const uint32_t t_d0_base = dc2_read_u32(rdram, 0x01F060D0u + 0x20u);
        const uint32_t t_70_base = dc2_read_u32(rdram, 0x01F06070u + 0x20u);
        std::fprintf(stderr,
                     "[G53:loadmap] scene=0x%x mapId=%u stackSel=%u ra=0x%x e0present=0x%x dir='%s' base='%s' cnt=%u->%u mapCnt=%u->%u stackPtr=0x%x stackBase=0x%x stackCur=0x%x sceneSlot=0x%x | a0base=0x%x a0cur=0x%x d0base=0x%x m70base=0x%x ret=0x%x\n",
                     scene, mapId, stackSel, getRegU32(ctx, 31), e0present, dir, base,
                     cntBefore, dc2_read_u32(rdram, scene + 0x00u),
                     mapCntBefore, dc2_read_u32(rdram, scene + 0x27E0u),
                     stackPtr, stackBase, stackCur, sceneSlot,
                     t_a0_base, t_a0_cur, t_d0_base, t_70_base,
                     getRegU32(ctx, 2));
        std::fflush(stderr);
        return;
    }
    LoadMap__6CSceneFiP17SCN_LOADMAP_INFO2i_0x285ce0(rdram, ctx, runtime);
}

static bool f50_7_is_mapfunc_pos_return(uint32_t ra)
{
    return ra == 0x00164188u || ra == 0x001641C0u || ra == 0x001641CCu ||
           ra == 0x00164300u || ra == 0x00164368u;
}

static void f50_7_copy_vec4(uint8_t *rdram, uint32_t dst, uint32_t src)
{
    for (uint32_t i = 0u; i < 4u; ++i)
        dc2_write_u32(rdram, dst + i * 4u, dc2_read_u32(rdram, src + i * 4u));
}

static void f50_7_funcpoint_setter_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         const char *tag, uint32_t storeOff, uint32_t slotOff,
                                         PS2Runtime::RecompiledFunction originalFn,
                                         PS2Runtime::RecompiledFunction fallbackFn)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t vec = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);

    if (!f50_6_debug_menu_enabled() || !f50_7_is_mapfunc_pos_return(ra) || self == 0u || vec == 0u)
    {
        originalFn(rdram, ctx, runtime);
        return;
    }

    const uint32_t frame = self + 0x70u;
    uint32_t vt = dc2_read_u32(rdram, frame);
    uint32_t slot = vt ? dc2_read_u32(rdram, vt + slotOff) : 0u;
    bool repairedVt = false;
    if (vt == 0u || slot == 0u)
    {
        dc2_write_u32(rdram, frame, 0x00374F50u);
        vt = dc2_read_u32(rdram, frame);
        slot = vt ? dc2_read_u32(rdram, vt + slotOff) : 0u;
        repairedVt = true;
    }

    if (f50_7_trace_selected_map_enabled() && (n <= 128u || repairedVt || slot == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:funcpoint] tag=%s n=%u self=0x%x frame=0x%x vec=0x%x ra=0x%x vt=0x%x slotOff=0x%x slot=0x%x repair=%u v=(0x%x,0x%x,0x%x,0x%x)\n",
                     tag, n, self, frame, vec, ra, vt, slotOff, slot, repairedVt ? 1u : 0u,
                     dc2_read_u32(rdram, vec + 0x00u),
                     dc2_read_u32(rdram, vec + 0x04u),
                     dc2_read_u32(rdram, vec + 0x08u),
                     dc2_read_u32(rdram, vec + 0x0Cu));
    }

    f50_7_copy_vec4(rdram, self + storeOff, vec);

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, frame);
    f50_set_reg(ctx, 5, vec);
    f50_set_reg(ctx, 31, 0u);

    PS2Runtime::RecompiledFunction targetFn = fallbackFn;
    if (slot != 0u && runtime->hasFunction(slot))
        targetFn = runtime->lookupFunction(slot);
    // G212 (audit, G186 bug class): the vtable slot target is arbitrary guest code; if it
    // contains a back-edge preemption checkpoint, the unconditional ctx->pc = ra below
    // would swallow the yield (the mgCObject fallbacks are checkpoint-free, but a subclass
    // override need not be). Suppress preemption for the emulated call, like G186.
    static const bool s_g212NoPreemptFix = dc2_env_flag_enabled("DC2_G212_NO_PREEMPTFIX");
    if (!s_g212NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    targetFn(rdram, ctx, runtime);
    if (!s_g212NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);

    ctx->r[31] = savedRa;
    ctx->pc = ra;
}

static void f50_7_funcpoint_set_scale_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetScale@1643a0",
                                 0x1A0u, 0x28u,
                                 SetScale__10CFuncPointFPf_0x1643a0,
                                 SetScale__9mgCObjectFPf_0x136340);
}

static void f50_7_funcpoint_set_rotation_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetRotation@1643c0",
                                 0x190u, 0x1Cu,
                                 SetRotation__10CFuncPointFPf_0x1643c0,
                                 SetRotation__9mgCObjectFPf_0x136270);
}

static void f50_7_funcpoint_set_position_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetPosition@1643e0",
                                 0x180u, 0x10u,
                                 SetPosition__10CFuncPointFPf_0x1643e0,
                                 SetPosition__9mgCObjectFPf_0x136190);
}

static void f50_7_mg_active_lighting_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t lightNo = getRegU32(ctx, 4);
    const uint32_t active = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);

    if (f50_7_trace_selected_map_enabled() && f50_6_debug_menu_enabled() && n <= 128u)
    {
        std::fprintf(stderr,
                     "[F50.7:tailfix] tag=mgActiveLighting@143720 n=%u light=%u active=%u ra=0x%x gp=0x%x\n",
                     n, lightNo, active, ra, gp);
    }

    dc2_write_u32(rdram, gp - 0x77E0u, 1u);

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, 0x00380EC0u);
    f50_set_reg(ctx, 5, lightNo);
    f50_set_reg(ctx, 6, active);
    f50_set_reg(ctx, 31, 0u);
    // G212 (audit hit, G186 bug class): ActiveLighting@0x139120 has back-edge preemption
    // checkpoints; a yield here would be swallowed by the unconditional ctx->pc = ra below
    // (half-executed lighting call, resume lost). Suppress preemption for the emulated
    // call, exactly like the G186 fix for the Draw__4CMap override. Kill: DC2_G212_NO_PREEMPTFIX=1.
    static const bool s_g212NoPreemptFix = dc2_env_flag_enabled("DC2_G212_NO_PREEMPTFIX");
    if (!s_g212NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    ActiveLighting__13mgRENDER_INFOFii_0x139120(rdram, ctx, runtime);
    if (!s_g212NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);

    ctx->r[31] = savedRa;
    ctx->pc = ra;
}

static void f50_7_map_draw_tailfix_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         const char *tag, uint32_t directFlag)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t vt = self ? dc2_read_u32(rdram, self + 0x0D00u) : 0u;
    const uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0x08u) : 0u;

    if (f50_7_trace_selected_map_enabled() && f50_6_debug_menu_enabled() && n <= 128u)
    {
        std::fprintf(stderr,
                     "[F50.7:tailfix] tag=%s n=%u self=0x%x direct=%u ra=0x%x vt=0x%x slot=0x%x\n",
                     tag, n, self, directFlag, ra, vt, slot);
    }

    if (slot == 0u || !runtime->hasFunction(slot))
    {
        ctx->pc = slot;
        return;
    }

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, self);
    f50_set_reg(ctx, 5, directFlag);
    f50_set_reg(ctx, 31, 0u);
    // G186 FIX (root cause of Known Issue #3, the 0xe3dc70/0x1dd8260 "Function not
    // found" level-load crashes): the ra=0 sentinel contract below is only safe if the
    // emulated call runs to completion in ONE invocation. If the draw chain back-edge
    // yields (shouldPreemptGuestExecution) mid-call, this probe's C++ frame unwinds and
    // prematurely restores ra; the resumed chain later reloads the fake ra=0 saved in
    // DrawSub's own guest frame, the naked pc=0 reaches the dispatcher, and pc-zero
    // recovery re-enters a mid-function pc WITHOUT restoring $sp — the resumed code then
    // runs ~0x500 deep into live frames, corrupting saved-$ra slots (the garbage values
    // 0xe3dc70 / 0x1dd8260 / stack addresses seen as "missing functions"). Same hazard
    // class G57 fixed for the title draw, same fix: suppress back-edge preemption for
    // the duration of the emulated call. Kill-switch: DC2_G186_NO_PREEMPTFIX=1.
    static const bool s_g186NoPreemptFix = (std::getenv("DC2_G186_NO_PREEMPTFIX") != nullptr);
    if (!s_g186NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    runtime->lookupFunction(slot)(rdram, ctx, runtime);
    if (!s_g186NoPreemptFix)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);

    const uint32_t postPc = ctx->pc;
    ctx->r[31] = savedRa;
    ctx->pc = (postPc == 0u) ? ra : postPc;
}

static void f50_7_map_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_map_draw_tailfix_probe(rdram, ctx, runtime, "Draw@160b10", 0u);
}

static void f50_7_map_draw_direct_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_map_draw_tailfix_probe(rdram, ctx, runtime, "DrawDirect@160b30", 1u);
}

// ---------------------------------------------------------------------------
// PHASE G56: trace the main-title map per-part/per-piece geometry submission.
// The title 3D background (map s19) renders through the deferred mgCDrawManager
// queue. Chain (all indirect-dispatched, so registerFunction fires):
//   TitleMapDraw -> (TitleMap+0xd00 vt+0xc) Draw__4CMap@0x160b10 ->
//   DrawSub__8CEditMap@0x1b4130 -> DrawSub__4CMap@0x15e250 (iterates the 6-entry
//   visible draw list at map+0x364) -> part vt+0x34 Draw__9CMapParts@0x15e3d0 ->
//   DrawSub__9CMapParts@0x166a70 { gate part vt+0x40 = PreDraw__9CMapParts@0x166a00;
//   per-piece embedded vt+0x34 = Draw__9CMapPiece@0x166e40 -> Draw__12CObjectFrame
//   (mgCFrame/mgCVisualMDT geometry emit, queued into mgDrawManager) }.
// G55 made the draw list = 6 (HW-identical) yet [F31:prim] still shows 0 tri. These
// taps count entries + return value to pinpoint where the chain stops emitting
// geometry. Transparent (delegate to the real recompiled body); only the gated log
// changes anything. Gated DC2_TRACE_G56. HW dispatch reference (live PCSX2):
//   part vtable 0x3754d0: +0x34=0x15e3d0 +0x40=0x166a00; piece+0x80 visual=0x903ce0
//   (gate +0x54==0); piece embedded vtable 0x375570: +0x34=0x166e40.
// (externs for the recompiled bodies live in the global extern block near the top.)

static bool g56_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G56");
    return enabled;
}

static bool g195_block_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE");
    return enabled;
}

static std::atomic<uint32_t> g_g195_current_map_part{0u};
static std::atomic<uint32_t> g_g195_current_map_piece{0u};
static constexpr uint32_t kG195ForegroundVisual = 0x0121C940u;

static uint32_t g195_find_piece_index(uint8_t *rdram, uint32_t part, uint32_t piece)
{
    if (part == 0u || piece == 0u)
        return 0xFFFFFFFFu;

    uint32_t node = part + 0xB0u;
    for (uint32_t i = 0u; i < 64u && node != 0u; ++i)
    {
        if (node + 0x10u == piece)
            return i;
        node = dc2_read_u32(rdram, node + 0x00u);
    }
    return 0xFFFFFFFFu;
}

static uint32_t g195_find_frame_for_visual(uint8_t *rdram, uint32_t rootFrame, uint32_t visual,
                                           uint32_t *depthOut, uint32_t *ordinalOut)
{
    if (depthOut) *depthOut = 0xFFFFFFFFu;
    if (ordinalOut) *ordinalOut = 0xFFFFFFFFu;
    if (rootFrame == 0u || visual == 0u)
        return 0u;

    uint32_t stack[64]{};
    uint32_t depth[64]{};
    uint32_t sp = 0u;
    stack[sp] = rootFrame;
    depth[sp] = 0u;
    ++sp;

    uint32_t ordinal = 0u;
    while (sp != 0u && ordinal < 256u)
    {
        --sp;
        const uint32_t frame = stack[sp];
        const uint32_t d = depth[sp];
        if (frame == 0u)
            continue;
        if (dc2_read_u32(rdram, frame + 0xF8u) == visual)
        {
            if (depthOut) *depthOut = d;
            if (ordinalOut) *ordinalOut = ordinal;
            return frame;
        }
        ++ordinal;

        const uint32_t sibling = dc2_read_u32(rdram, frame + 0x5Cu);
        const uint32_t child = dc2_read_u32(rdram, frame + 0x58u);
        if (sibling != 0u && sp < 64u)
        {
            stack[sp] = sibling;
            depth[sp] = d;
            ++sp;
        }
        if (child != 0u && sp < 64u)
        {
            stack[sp] = child;
            depth[sp] = d + 1u;
            ++sp;
        }
    }
    return 0u;
}

static void g56_chain_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          const char *tag,
                          void (*real)(uint8_t *, R5900Context *, PS2Runtime *),
                          unsigned &counter, unsigned &nonzero)
{
    const uint32_t a0 = getRegU32(ctx, 4);
    const unsigned n = ++counter;
    real(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    if (ret != 0u) ++nonzero;
    if (g56_trace_enabled() && (n <= 18u || (n % 1200u) == 0u))
    {
        std::fprintf(stderr, "[G56:%s] n=%u nz=%u a0=0x%x ret=0x%x\n",
                     tag, n, nonzero, a0, ret);
        std::fflush(stderr);
    }
}

// G240: dungeon-1 flat cream/white untextured-quad artifact. CMapParts blend-colour
// slots (part+0x1f0, stride 0x10, count part+0x1e8) are only refreshed from the
// resolved CMapPiece material by UpdateColor when a slot's weight (+0xc) > 0; a slot
// SetColor never touched keeps whatever value it had at allocation time. Dump slot
// state for every drawn CMapParts so a zero/negative-weight slot (or a garbage colour
// in an otherwise-valid slot) can be correlated with the visible artifact.
static bool g240_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G240");
    return enabled;
}

static void g240_scan_mapparts_colors(uint8_t *rdram, uint32_t part)
{
    if (!part) return;
    static std::atomic<uint32_t> s_calls{0u};
    const uint32_t n = s_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint32_t slotCount = dc2_read_u32(rdram, part + 0x1E8u);
    if (slotCount == 0u || slotCount > 32u) return; // sanity guard vs a garbage part ptr
    bool anyBad = false;
    char buf[640];
    int off = std::snprintf(buf, sizeof(buf), "part=0x%x slots=%u", part, slotCount);
    for (uint32_t i = 0; i < slotCount && off > 0 && off < (int)sizeof(buf) - 64; ++i)
    {
        const uint32_t slot = part + 0x1F0u + i * 0x10u;
        const float x = dc2_read_f32(rdram, slot + 0x00u);
        const float y = dc2_read_f32(rdram, slot + 0x04u);
        const float z = dc2_read_f32(rdram, slot + 0x08u);
        const float w = dc2_read_f32(rdram, slot + 0x0Cu);
        if (w <= 0.0f) anyBad = true;
        off += std::snprintf(buf + off, sizeof(buf) - off, " [%u](%.2f,%.2f,%.2f,w=%.2f)", i, x, y, z, w);
    }
    if (anyBad && (n <= 300u || (n % 400u) == 0u))
    {
        std::fprintf(stderr, "[G240:slots] n=%u %s\n", n, buf);
        std::fflush(stderr);
    }
}

static void g56_mapparts_draw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    const bool g195 = g195_block_trace_enabled();
    const bool g240 = g240_trace_enabled();
    const uint32_t part = (g195 || g240) ? getRegU32(ctx, 4) : 0u;
    const uint32_t prevPart = g195 ? g_g195_current_map_part.exchange(part, std::memory_order_relaxed) : 0u;
    if (g240)
        g240_scan_mapparts_colors(rdram, part);
    g56_chain_tap(rdram, ctx, runtime, "parts.draw", Draw__9CMapPartsFv_0x15e3d0, c, nz);
    if (g195)
        g_g195_current_map_part.store(prevPart, std::memory_order_relaxed);
}

static void g56_mapparts_predraw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    g56_chain_tap(rdram, ctx, runtime, "parts.predraw", PreDraw__9CMapPartsFv_0x166a00, c, nz);
}

static void g56_mappiece_draw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    const bool g195 = g195_block_trace_enabled();
    const uint32_t piece = g195 ? getRegU32(ctx, 4) : 0u;
    const uint32_t frame = piece ? dc2_read_u32(rdram, piece + 0x70u) : 0u;
    const uint32_t visual = frame ? dc2_read_u32(rdram, frame + 0xF8u) : 0u;
    const bool g195Target = g195 && (visual == kG195ForegroundVisual);
    const uint32_t prevPiece = g195 ? g_g195_current_map_piece.exchange(piece, std::memory_order_relaxed) : 0u;
    g56_chain_tap(rdram, ctx, runtime, "piece.draw", Draw__9CMapPieceFv_0x166e40, c, nz);
    if (g195Target)
    {
        static std::atomic<uint32_t> s_g195OwnerLog{0u};
        const uint32_t n = s_g195OwnerLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 96u || (n % 720u) == 0u)
        {
            const uint32_t part = g_g195_current_map_part.load(std::memory_order_relaxed);
            const uint32_t pieceIndex = g195_find_piece_index(rdram, part, piece);
            const uint32_t partFlags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
            const uint32_t partPieces = part ? dc2_read_u32(rdram, part + 0xB0u) : 0u;
            const uint32_t partVisual = part ? dc2_read_u32(rdram, part + 0x2F8u) : 0u;
            const uint32_t frameObj = frame ? dc2_read_u32(rdram, frame + 0xF4u) : 0u;
            const uint32_t frameMtx = frame ? dc2_read_u32(rdram, frame + 0xF0u) : 0u;
            const uint32_t pieceFlags = piece ? dc2_read_u32(rdram, piece + 0x84u) : 0u;
            const uint32_t pieceGate = piece ? dc2_read_u32(rdram, piece + 0x88u) : 0u;
            const uint32_t animeCount = piece ? dc2_read_u32(rdram, piece + 0x8Cu) : 0u;
            const uint32_t animeBase = piece ? dc2_read_u32(rdram, piece + 0x90u) : 0u;
            std::fprintf(stderr,
                         "[G195:owner] n=%u part=0x%x partFlags=0x%x partVis=0x%x partPieces=0x%x "
                         "piece=0x%x pieceIdx=%u pieceFlags=0x%x gate=0x%x anime=%u@0x%x "
                         "frame=0x%x frameMtx=0x%x frameObj=0x%x visual=0x%x ret=0x%x\n",
                         n, part, partFlags, partVisual, partPieces,
                         piece, pieceIndex, pieceFlags, pieceGate, animeCount, animeBase,
                         frame, frameMtx, frameObj, visual, getRegU32(ctx, 2));
            std::fflush(stderr);
        }
    }
    if (g195)
        g_g195_current_map_piece.store(prevPiece, std::memory_order_relaxed);
}

static void g195_mdt_owner_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool target = g195_block_trace_enabled() && (self == kG195ForegroundVisual);
    const uint32_t argPacket = target ? getRegU32(ctx, 5) : 0u;
    const uint32_t argMtx = target ? getRegU32(ctx, 6) : 0u;
    const uint32_t argMgr = target ? getRegU32(ctx, 7) : 0u;
    const uint32_t part = target ? g_g195_current_map_part.load(std::memory_order_relaxed) : 0u;
    const uint32_t piece = target ? g_g195_current_map_piece.load(std::memory_order_relaxed) : 0u;
    const uint32_t rootFrame = piece ? dc2_read_u32(rdram, piece + 0x70u) : 0u;
    uint32_t frameDepth = 0xFFFFFFFFu;
    uint32_t frameOrdinal = 0xFFFFFFFFu;
    const uint32_t visualFrame = target ?
        g195_find_frame_for_visual(rdram, rootFrame, self, &frameDepth, &frameOrdinal) : 0u;

    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);

    if (target)
    {
        static std::atomic<uint32_t> s_g195MdtOwnerLog{0u};
        const uint32_t n = s_g195MdtOwnerLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 96u || (n % 720u) == 0u)
        {
            const uint32_t pieceIndex = g195_find_piece_index(rdram, part, piece);
            const uint32_t partFlags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
            const uint32_t partPieces = part ? dc2_read_u32(rdram, part + 0xB0u) : 0u;
            const uint32_t partVisual = part ? dc2_read_u32(rdram, part + 0x2F8u) : 0u;
            const uint32_t pieceFlags = piece ? dc2_read_u32(rdram, piece + 0x84u) : 0u;
            const uint32_t pieceGate = piece ? dc2_read_u32(rdram, piece + 0x88u) : 0u;
            const uint32_t frameObj = visualFrame ? dc2_read_u32(rdram, visualFrame + 0xF4u) : 0u;
            const uint32_t frameMtx = visualFrame ? dc2_read_u32(rdram, visualFrame + 0xF0u) : 0u;
            const uint32_t matBase = dc2_read_u32(rdram, self + 0x44u);
            const uint32_t nodeHead = dc2_read_u32(rdram, self + 0x48u);
            std::fprintf(stderr,
                         "[G195:owner] n=%u src=mdt part=0x%x partFlags=0x%x partVis=0x%x partPieces=0x%x "
                         "piece=0x%x pieceIdx=%u pieceFlags=0x%x gate=0x%x rootFrame=0x%x "
                         "visFrame=0x%x frameDepth=%u frameOrd=%u frameMtx=0x%x frameObj=0x%x "
                         "self=0x%x matBase=0x%x nodeHead=0x%x argPacket=0x%x argMtx=0x%x mgr=0x%x ret=0x%x\n",
                         n, part, partFlags, partVisual, partPieces,
                         piece, pieceIndex, pieceFlags, pieceGate, rootFrame,
                         visualFrame, frameDepth, frameOrdinal, frameMtx, frameObj,
                         self, matBase, nodeHead, argPacket, argMtx, argMgr, getRegU32(ctx, 2));
            std::fflush(stderr);
        }
    }
}

// G56: tap BeginDraw__14mgCDrawManager (reached via mgBeginDraw's indirect tail-call).
// Captures the mempool + its base (mempool+0x20) on entry and the manager's per-block
// table pointers after init. The title passes mempool 0x1f060d0 (base HW=0xd87340).
// If poolBase==0 the title scene's mempool is unfunded (Initialize gap, like G53/G55);
// if poolBase is valid but m4/m6 stay 0, the Alloc path failed. Gated DC2_TRACE_G56.
static void g56_begindraw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u;
    const uint32_t mgr = getRegU32(ctx, 4);
    const uint32_t mempool = getRegU32(ctx, 5);
    const uint32_t blockArr = getRegU32(ctx, 6);
    const uint32_t poolBase = mempool ? dc2_read_u32(rdram, mempool + 0x20u) : 0u;
    const uint32_t poolEna  = mempool ? dc2_read_u32(rdram, mempool + 0x1Cu) : 0u;
    const uint32_t poolCur0 = mempool ? dc2_read_u32(rdram, mempool + 0x24u) : 0u;
    const uint32_t poolLim  = mempool ? dc2_read_u32(rdram, mempool + 0x28u) : 0u;
    const uint32_t m16in = dc2_read_u32(rdram, mgr + 0x58u);              // bounds object
    const uint32_t m17in = dc2_read_u32(rdram, mgr + 0x5Cu);              // fallback mempool
    const uint32_t m7in  = dc2_read_u32(rdram, mgr + 0x1Cu);              // table[0x14] size
    BeginDraw__14mgCDrawManagerFP9mgCMemoryPi_0x135230(rdram, ctx, runtime);
    const unsigned n = ++c;
    if (g56_trace_enabled() && (n <= 24u || (n % 600u) == 0u))
    {
        const uint32_t m1 = dc2_read_u32(rdram, mgr + 0x04u);
        const uint32_t m3 = dc2_read_u32(rdram, mgr + 0x0Cu);
        const uint32_t m4 = dc2_read_u32(rdram, mgr + 0x10u);
        const uint32_t m6 = dc2_read_u32(rdram, mgr + 0x18u);
        const uint32_t poolCur1 = mempool ? dc2_read_u32(rdram, mempool + 0x24u) : 0u;
        std::fprintf(stderr,
                     "[G56:begindraw] n=%u mgr=0x%x mempool=0x%x base=0x%x ena=%u cur=%u->%u lim=%u | m16=0x%x m17=0x%x m7=%u blockArr=0x%x -> m1=0x%x m3=%u m4=0x%x m6=0x%x\n",
                     n, mgr, mempool, poolBase, poolEna, poolCur0, poolCur1, poolLim,
                     m16in, m17in, m7in, blockArr, m1, m3, m4, m6);
        std::fflush(stderr);
    }
}

// G57 bisection tool (kept for G58): optionally SKIP DrawWater (env DC2_G57_SKIP_WATER)
// to test whether the title $ra corruption (TitleMapDraw_sp+0xA0 <- 0x9f84a0) originates in
// the water draw. RESULT: skipping water does NOT change the crash (same bad=0x9f84a0,
// sp=0x1fff760) -> the corruptor is the GEOMETRY-QUEUE chain, not the water. Default: run.
static void g57_drawwater_skip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G57_SKIP_WATER");
    // G129: the title water pool (HW ~966-1012 trifans) renders 0 trifans on the runner at the
    // GS for the WHOLE frame (DC2_TRACE_G14 [G65:vk] t5=0). DrawWater@0x15e800 gates ALL water
    // drawing on `*(CMap+0xcec) > 0 && tex(a2) != 0 && *(CMap+0xcf4) > 0` (the CWaterFrame array
    // counts + the dest texture). Log the gate inputs so we can tell "water never configured /
    // count 0" (root = CMapWater init / map data) from "configured but trifans lost downstream"
    // (mgDrawDirect / CreatePacket__11CWaterFrameFv VU path). Opt-in, quiet. DC2_G129_WATER.
    static const bool g129water = dc2_env_flag_enabled("DC2_G129_WATER");
    if (g129water)
    {
        const uint32_t cmap = getRegU32(ctx, 4); // a0 = CMap*
        const uint32_t tex  = getRegU32(ctx, 6); // a2 = dest mgCTexture*
        int32_t cnt1 = 0, cnt2 = 0, arr1 = 0, arr2 = 0;
        if (cmap)
        {
            cnt1 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcecu));
            arr1 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf0u));
            cnt2 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf4u));
            arr2 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf8u));
        }
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 80u)
            std::fprintf(stderr,
                "[G129:water] call=%u CMap=0x%x tex(a2)=0x%x cnt1(+0xcec)=%d arr1(+0xcf0)=0x%x cnt2(+0xcf4)=%d arr2(+0xcf8)=0x%x gate=%s\n",
                n, cmap, tex, cnt1, (unsigned)arr1, cnt2, (unsigned)arr2,
                (cnt1 > 0 && tex != 0 && cnt2 > 0) ? "ENTER" : "SKIP");
    }
    if (skip) return; // empty body => dispatch handler returns to caller (DrawWater skipped)
    DrawWater__4CMapFP9mgCCameraP10mgCTextureP10mgCTexture_0x15e800(rdram, ctx, runtime);
}

// G194 bisection tools (town dark-wedge overlay + water-region mismatch vs ref/dumps/map_0.png).
// Opt-in skip levers, default OFF (run the real body). Each prints a bounded hit marker when its
// env is set, so a silent log also tells us the hook was bypassed by a direct jal.
static void g194_skip_draweffect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G194_SKIP_DRAWEFFECT");
    if (skip)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u || (n % 128u) == 0u)
            std::fprintf(stderr, "[G194:skip] DrawEffect__6CScene hit n=%u (skipped)\n", n);
        return;
    }
    DrawEffect__6CSceneFi_0x2c8820(rdram, ctx, runtime);
}

static void g194_skip_dof(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G194_SKIP_DOF");
    // G194: input probe for the DOF wedge bug — log the call args + the float
    // param block so they can be A/B'd against the paused PCSX2 session.
    static const bool probe = dc2_env_flag_enabled("DC2_G194_DOF_PROBE");
    if (probe)
    {
        static std::atomic<uint32_t> s_p{0};
        const uint32_t n = s_p.fetch_add(1u, std::memory_order_relaxed);
        if (n < 12u)
        {
            const uint32_t a0 = getRegU32(ctx, 4);
            const uint32_t a1 = getRegU32(ctx, 5);
            const uint32_t a2 = getRegU32(ctx, 6);
            const float f12 = ctx->f[12];
            float p[8] = {};
            if (a1 && a1 < 0x02000000u - 32u)
                std::memcpy(p, rdram + (a1 & 0x01FFFFFFu), sizeof(p));
            uint32_t texw = 0, texh = 0, texTbp = 0;
            if (a2 && a2 < 0x02000000u - 0x40u)
            {
                texw = dc2_read_u32(rdram, a2 + 0x28u);
                texh = dc2_read_u32(rdram, a2 + 0x2cu);
                texTbp = dc2_read_u32(rdram, a2 + 0x20u);
            }
            std::fprintf(stderr,
                "[G194:dof] n=%u mode=%u params=0x%x tex=0x%x f12=%.4f p={%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f} tex+0x20=0x%x +0x28=%u +0x2c=%u\n",
                n, a0, a1, a2, f12, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], texTbp, texw, texh);
            uint8_t blk[0x60] = {};
            std::memcpy(blk, rdram + 0x382490u, sizeof(blk));
            std::fprintf(stderr, "[G194:dof] fbdesc@0x382490:");
            for (size_t i = 0; i < sizeof(blk); ++i)
                std::fprintf(stderr, "%s%02x", (i % 16 == 0) ? " | " : (i % 4 == 0 ? " " : ""), blk[i]);
            std::fprintf(stderr, "\n");
            // G194: raw VRAM region dump (linear bytes as 256x256 gray PPM — swizzle
            // scrambles layout but keeps brightness truth). Pages: DOF blur pyramid.
            static const bool vramDump = dc2_env_flag_enabled("DC2_G194_VRAM_DUMP");
            if (vramDump && n == 5)
            {
                const uint8_t *vram = runtime ? runtime->memory().getGSVRAM() : nullptr;
                if (vram)
                {
                    static const uint32_t blocks[] = {0x2760u, 0x27a0u, 0x2860u, 0x28c0u};
                    for (uint32_t b : blocks)
                    {
                        const uint32_t off = b * 256u;
                        char fn[128];
                        std::snprintf(fn, sizeof(fn), "D:/ps2r/dc2/captures/g194_vram_%04x.ppm", b);
                        FILE *fp = std::fopen(fn, "wb");
                        if (!fp)
                            continue;
                        std::fprintf(fp, "P6\n256 256\n255\n");
                        for (uint32_t i = 0; i < 65536u && off + i < 4u * 1024u * 1024u; ++i)
                        {
                            const uint8_t v = vram[off + i];
                            std::fputc(v, fp); std::fputc(v, fp); std::fputc(v, fp);
                        }
                        std::fclose(fp);
                    }
                    std::fprintf(stderr, "[G194:dof] vram pyramid pages dumped\n");
                }
            }
        }
    }
    if (skip)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u || (n % 128u) == 0u)
            std::fprintf(stderr, "[G194:skip] DepthOfField hit n=%u (skipped)\n", n);
        return;
    }
    DepthOfField__FiPfP10mgCTexturef_0x17e320(rdram, ctx, runtime);
}

static void g194_skip_texanime(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G194_SKIP_TEXANIME");
    if (skip)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u || (n % 128u) == 0u)
            std::fprintf(stderr, "[G194:skip] TexAnime hit n=%u (skipped)\n", n);
        return;
    }
    TexAnime__15mgCTextureAnimeFiP13sceVif1Packet_0x13bd90(rdram, ctx, runtime);
}

static void g195_skip_editmap_draweffect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G195_SKIP_EDITMAP_DRAWEFFECT");
    if (skip)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u || (n % 128u) == 0u)
            std::fprintf(stderr, "[G195:skip] DrawEffect__8CEditMap hit n=%u (skipped)\n", n);
        return;
    }
    DrawEffect__8CEditMapFv_0x29c1c0(rdram, ctx, runtime);
}

static void g195_skip_cmap_draweffect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G195_SKIP_CMAP_DRAWEFFECT");
    if (skip)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u || (n % 128u) == 0u)
            std::fprintf(stderr, "[G195:skip] DrawEffect__4CMap hit n=%u (skipped)\n", n);
        return;
    }
    DrawEffect__4CMapFv_0x15e3f0(rdram, ctx, runtime);
}

static bool g195_moveimage_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G195_MOVEIMAGE_TRACE");
    return enabled;
}

struct G195Tex0Info
{
    uint64_t raw = 0;
    uint32_t tbp = 0;
    uint32_t tbw = 0;
    uint32_t psm = 0;
    uint32_t tw = 0;
    uint32_t th = 0;
    uint32_t cbp = 0;
    uint32_t cpsm = 0;
};

static G195Tex0Info g195_read_tex0_info(uint8_t *rdram, uint32_t mgTex)
{
    G195Tex0Info t{};
    if (mgTex == 0u || mgTex >= 0x02000000u - 0x40u)
        return t;
    t.raw = dc2_read_u64(rdram, mgTex + 0x38u);
    t.tbp = static_cast<uint32_t>(t.raw & 0x3FFFu);
    t.tbw = static_cast<uint32_t>((t.raw >> 14) & 0x3Fu);
    t.psm = static_cast<uint32_t>((t.raw >> 20) & 0x3Fu);
    t.tw = static_cast<uint32_t>((t.raw >> 26) & 0xFu);
    t.th = static_cast<uint32_t>((t.raw >> 30) & 0xFu);
    t.cbp = static_cast<uint32_t>((t.raw >> 37) & 0x3FFFu);
    t.cpsm = static_cast<uint32_t>((t.raw >> 51) & 0xFu);
    return t;
}

static void g195_moveimage_trace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g195_moveimage_trace_enabled())
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 240u)
        {
            const uint32_t srcTex = getRegU32(ctx, 4);
            const uint32_t rect = getRegU32(ctx, 5);
            const uint32_t dstTex = getRegU32(ctx, 6);
            const uint32_t dstX = getRegU32(ctx, 7);
            const uint32_t dstY = getRegU32(ctx, 8);
            const uint32_t dir = getRegU32(ctx, 9);
            const G195Tex0Info src = g195_read_tex0_info(rdram, srcTex);
            const G195Tex0Info dst = g195_read_tex0_info(rdram, dstTex);
            const uint32_t r0 = rect ? dc2_read_u32(rdram, rect + 0u) : 0u;
            const uint32_t r1 = rect ? dc2_read_u32(rdram, rect + 4u) : 0u;
            const uint32_t r2 = rect ? dc2_read_u32(rdram, rect + 8u) : 0u;
            const uint32_t r3 = rect ? dc2_read_u32(rdram, rect + 12u) : 0u;
            std::fprintf(stderr,
                "[G195:moveimg] n=%u ra=0x%x srcTex=0x%x src={tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x cpsm=0x%x raw=0x%llx} "
                "dstTex=0x%x dst={tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x cpsm=0x%x raw=0x%llx} "
                "rect={0x%x,0x%x,0x%x,0x%x} dstXY=(%u,%u) dir=%u\n",
                n, getRegU32(ctx, 31), srcTex, src.tbp, src.tbw, src.psm, src.tw, src.th,
                src.cbp, src.cpsm, static_cast<unsigned long long>(src.raw),
                dstTex, dst.tbp, dst.tbw, dst.psm, dst.tw, dst.th,
                dst.cbp, dst.cpsm, static_cast<unsigned long long>(dst.raw),
                r0, r1, r2, r3, dstX, dstY, dir);
        }
    }
    mgSetPkMoveImage__FP10mgCTexture9mgRect_i_P10mgCTextureiii_0x144560(rdram, ctx, runtime);
}

// G129: probe the SPI map-config WATER_VERTEX handler. If this fires during the title map load,
// the config script DOES request water (so the gap is downstream: cfgWater -> CMap+0xcf0 storage,
// or wrong CMap). If it never fires, the title map's SPI config never dispatches WATER_VERTEX
// (config not parsed/dispatched, or the title map data lacks it). Delegates to the real handler.
static void g129_cfg_water_vertex_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool g129water = dc2_env_flag_enabled("DC2_G129_WATER");
    cfgWATER_VERTEX__FP9SPI_STACKi_0x1648f0(rdram, ctx, runtime);
    if (g129water)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 40u)
            std::fprintf(stderr, "[G129:cfgwater] FIRED call=%u cfgWater(ret v0)=0x%x ra=0x%x\n",
                         n, getRegU32(ctx, 2), getRegU32(ctx, 31));
    }
}

// ---------------------------------------------------------------------------
// PHASE G58: $ra-corruption canary for the title geometry-queue chain.
// TitleMapDraw@0x2a2280 saves $ra at sp+0xA0 = 0x1fff800 (entry $ra=0x2a1b88); the title
// loop dies after ONE frame because that slot is overwritten with 0x9f84a0 (= *(TitleMap
// +0x364), the visible-parts array = DrawSub__4CMap's s0) and the epilogue jr $ra jumps to
// data. G57 ruled DrawWater OUT; the corruptor is in the geometry-queue chain. This canary
// wraps every link, snapshots the fixed slot 0x1fff800 before/after the real body, and logs:
//   - the entry sp of each link (depth profile: a link whose sp >= 0x1fff760 overlaps
//     TitleMapDraw's own frame and can clobber the saved-$ra slot = sp imbalance upstream),
//   - the exact link across which 0x1fff800 flips (innermost flip = culprit boundary).
// The user's A/B (debug-menu + Circle x2 reaches the title WITHOUT this crash) shows the
// corruption is PATH-dependent => consistent with a back-edge-preemption sp leak rather than
// an always-on store-offset bug (the static sp-balance scan of every link found no overshoot
// store). Transparent: delegates to the real body; only the gated log changes anything.
// Gated DC2_TRACE_G58.
