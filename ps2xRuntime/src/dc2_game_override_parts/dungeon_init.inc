// PHASE F50.1: identify the dungeon scene-init sub-object whose auto-stub ctor
// leaves a garbage vtable (0x49497350) after Alloc@0x139d20 + placement
// new@0x1398e0. Activated only once InitDungeonMain @0x1CC040 is entered (set in
// f48_4_init_dungeon_main_probe below), so we capture only dungeon-path
// allocations. The last [F50.1:new] line before the dispatcher's bad=0x49497350
// warning is the caller doing the bad `new`; its $ra maps to the recomp function
// whose next `jal <ctor>` is the broken constructor.
// Behaviorally identical to reta1_stub (returns $a1) and quiet unless
// DC2_TRACE_F50_1 is set.
static bool f50_1_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_1");
    return enabled;
}

static bool g_f50_1_dungeon_active = false;
static uint32_t g_f50_1_new_log = 0u;

static void f50_1_placement_new_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ptr = getRegU32(ctx, 5); // $a1 (the pointer placement-new returns)
    if (f50_1_trace_enabled() && g_f50_1_dungeon_active && g_f50_1_new_log < 128u)
    {
        const uint32_t n = ++g_f50_1_new_log;
        std::fprintf(stderr,
                     "[F50.1:new] n=%u frame=%u ra=0x%x size=%u ptr=0x%x preVt=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     getRegU32(ctx, 4), ptr,
                     ptr ? dc2_read_u32(rdram, ptr) : 0u);
    }
    setReturnU32(ctx, ptr);       // $v0 = $a1
    ctx->pc = getRegU32(ctx, 31); // return to caller
}

// PHASE F50.1: repair the auto-stubbed memoryInit__Fv @0x1CBD70.
// The generated recomp/memoryInit__Fv_0x1cbd70.cpp is a setReturnS32(ctx,0) auto-stub
// (it is commented out of [general].stubs in ref/config_auto_recomp.toml but the live
// recomp output is stale). InitDungeonMain @0x1CC094 calls it first thing; because it
// does nothing, the global mgCMemory pools are never created, so InitDungeonMain's
// Alloc() returns 0, every dungeon allocation gets ptr=0, and constructing on null
// yields the garbage vtable 0x49497350. This override faithfully replays the real
// memoryInit body (ref/assembly.txt @0x1CBD70) by calling the real recompiled helpers
// (all present). memoryInit runs only on dungeon entry, so the default-smoke (title)
// path is unaffected.
//
// Set an arbitrary GPR low word with R5900 sign-extension (mirrors setReturnU32).
static inline void f50_set_reg(R5900Context *ctx, int reg, uint32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

// Complete a synchronous guest call even when regenerated loop code returns at
// a cooperative preemption point. The caller uses RA=0 as its return sentinel.
static uint32_t f50_run_guest_call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                   uint32_t addr)
{
    ctx->pc = addr;
    constexpr uint32_t kMaxResumeSlices = 1u << 20;
    for (uint32_t slice = 0; slice < kMaxResumeSlices; ++slice)
    {
        auto fn = runtime->lookupFunction(ctx->pc);
        if (!fn)
        {
            break;
        }

        fn(rdram, ctx, runtime);
        if (ctx->pc == 0u)
        {
            return getRegU32(ctx, 2);
        }
    }

    std::fprintf(stderr, "[F50.1:call] synchronous guest call did not complete addr=0x%x pc=0x%x\n",
                 addr, ctx->pc);
    return getRegU32(ctx, 2);
}

// Call a recompiled guest function by address with up to 4 args; return $v0.
static uint32_t f50_callg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    f50_set_reg(ctx, 4, a0);
    f50_set_reg(ctx, 5, a1);
    f50_set_reg(ctx, 6, a2);
    f50_set_reg(ctx, 7, a3);
    f50_set_reg(ctx, 31, 0u); // $ra sentinel
    return f50_run_guest_call(rdram, ctx, runtime, addr);
}

static uint32_t f50_callg_u64a0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                uint32_t addr, uint64_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(a0));
    f50_set_reg(ctx, 5, a1);
    f50_set_reg(ctx, 6, a2);
    f50_set_reg(ctx, 7, a3);
    f50_set_reg(ctx, 31, 0u);
    return f50_run_guest_call(rdram, ctx, runtime, addr);
}

static bool f55_trace_enabled();

// F55: init__Fv@0x15C160 stays nop'd for its IOP-module-load loops, but its
// InitCDFile@0x148F70 call is load-bearing: it fills the guest disc file-header
// table (header_buff@0x382680, header_num@gp-0x7764, data_sector@gp-0x775C) from
// cdrom0:\DATA.HD3;1. Without it every SearchFile-based load (LoadFileBG, the
// dungeon-map menu pack menu/N/dmapN.pac, ...) silently fails, which is the F53
// dungeon free-map black-rectangle root cause. Run the real generated body here.
static void f55_boot_init_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const auto savedRa = ctx->r[31];
    static bool s_initCdDone = false;
    if (!s_initCdDone)
    {
        s_initCdDone = true;
        const uint32_t ret = f50_callg(rdram, ctx, runtime, 0x148F70u, 0u, 0u, 0u, 0u);
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t headerNum = dc2_read_u32(rdram, gp - 0x7764u);
        const uint32_t dataSector = dc2_read_u32(rdram, gp - 0x775Cu);
        if (f55_trace_enabled() || headerNum == 0u)
        {
            std::fprintf(stderr,
                         "[F55:initcd] InitCDFile ret=0x%x header_num=%u data_sector=%u buff[0..3]=0x%x 0x%x 0x%x 0x%x\n",
                         ret, headerNum, dataSector,
                         dc2_read_u32(rdram, 0x382680u), dc2_read_u32(rdram, 0x382684u),
                         dc2_read_u32(rdram, 0x382688u), dc2_read_u32(rdram, 0x38268Cu));
        }
    }
    ctx->r[31] = savedRa;
    setReturnU32(ctx, 0u);
    ctx->pc = ra;
}

static void f50_1_memory_init(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t retAddr = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pMainBuffer = gp - 0x7290u;  // global mgCMemory* MainBuffer
    const uint32_t pBuffReadData = gp - 0x728Cu; // global void* BuffReadData

    // MainBuffer = GetMainStack(); reset its stack offset (+0x24) and enable flag (+0x1c).
    const uint32_t mainBuf = f50_callg(rdram, ctx, runtime, 0x1908F0u, 0, 0, 0, 0);
    if (f50_1_trace_enabled())
        std::fprintf(stderr,
                     "[F50.1:meminit] mainBuf=0x%x pre f1c=0x%x base20=0x%x off24=0x%x lim28=0x%x\n",
                     mainBuf, dc2_read_u32(rdram, mainBuf + 0x1Cu), dc2_read_u32(rdram, mainBuf + 0x20u),
                     dc2_read_u32(rdram, mainBuf + 0x24u), dc2_read_u32(rdram, mainBuf + 0x28u));
    dc2_write_u32(rdram, pMainBuffer, mainBuf);
    dc2_write_u32(rdram, mainBuf + 0x24u, 0u);
    dc2_write_u32(rdram, mainBuf + 0x1Cu, 0u);

    // VIF1 packet double-buffer: two 10000-byte stack allocs -> mgInitVif1Packet(a,b,160000).
    const uint32_t vif0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    const uint32_t vif1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x141E10u, vif0, vif1, 160000u, 0);
    if (f50_1_trace_enabled())
        std::fprintf(stderr, "[F50.1:meminit] vif0=0x%x vif1=0x%x\n", vif0, vif1);

    // Packet buffers 0x1e9f230 / 0x1e9f260 (20000 each) + mgSetPacketBuffer.
    uint32_t b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    uint32_t b1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F230u, b0, 20000u, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F260u, b1, 20000u, 0);
    f50_callg(rdram, ctx, runtime, 0x141EF0u, 0x1E9F230u, 0x1E9F260u, 0, 0);

    // Data buffers 0x1e9f290 / 0x1e9f2c0 (60000 each) + mgSetDataBuffer(...,1).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 60000u, 0, 0);
    b1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 60000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F290u, b0, 60000u, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F2C0u, b1, 60000u, 0);
    f50_callg(rdram, ctx, runtime, 0x142040u, 0x1E9F290u, 0x1E9F2C0u, 1u, 0);

    // WorkData pool 0x1e9f3b0 (10000) + name (_1014 @0x366c78).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F3B0u, b0, 10000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C78u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F3B0u, 0x366C78u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F3D4u, 0u);
    dc2_write_u32(rdram, 0x1E9F3CCu, 0u);

    // Texture manager: SetTableBuffer(table,0x140,0xaf,MainBuffer); Initialize(mgr,vramTop,-1).
    f50_callg(rdram, ctx, runtime, 0x12C830u, 0x381EF0u, 0x140u, 0xAFu, mainBuf);
    const uint32_t vramTop = f50_callg(rdram, ctx, runtime, 0x1908D0u, 0, 0, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x12CAD0u, 0x381EF0u, vramTop, 0xFFFFFFFFu, 0);

    // Character pool 0x1e9f3e0 (GetCharaMemAllocSize bytes) + name (_1015 @0x366c88).
    const uint32_t charaSize = f50_callg(rdram, ctx, runtime, 0x1E9B60u, 0, 0, 0, 0);
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, charaSize, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F3E0u, b0, charaSize, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C88u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F3E0u, 0x366C88u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F404u, 0u);
    dc2_write_u32(rdram, 0x1E9F3FCu, 0u);

    // ScriptData pool 0x1e9f560 (20000) + name (_1016 @0x366c98).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F560u, b0, 20000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C98u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F560u, 0x366C98u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F584u, 0u);
    dc2_write_u32(rdram, 0x1E9F57Cu, 0u);

    // EffectScriptData heap 0x1e9f590 (40000 via SetHeapMem) + name (_1017 @0x366cb0).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 40000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139930u, 0x1E9F590u, b0, 40000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366CB0u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F590u, 0x366CB0u, 0, 0);

    // BuffReadData = stAlloc64(MainBuffer, 200000).
    const uint32_t readData = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 200000u, 0, 0);
    dc2_write_u32(rdram, pBuffReadData, readData);
    if (f50_1_trace_enabled())
        std::fprintf(stderr, "[F50.1:meminit] done charaSize=%u readData=0x%x off24=0x%x\n",
                     charaSize, readData, dc2_read_u32(rdram, mainBuf + 0x24u));

    setReturnS32(ctx, 0);
    ctx->pc = retAddr;
}

// PHASE F48.4: confirm the post-costume new-game loop transition.
// After MenuCostumeKey returns 1 with DAT_01ecd62c==0xc, TitleLoop calls
// NextLoop(2); MainLoop then switches LoopNo 3->2 and calls LoopInit[2] =
// InitDungeonMain @0x1CC040, followed each frame by LoopMain[2] =
// LoopDungeonMain @0x1CEA00. These wrappers log entry/exit so we can see
// whether the new-game dungeon init is reached and whether it returns.
// Bounded and quiet unless DC2_TRACE_F48_4 is set.
static bool f48_4_trace_postcostume_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F48_4");
    return enabled;
}

static uint32_t g_f48_4_init_log = 0u;
static uint32_t g_f48_4_main_log = 0u;

// F50.4 FIX ג€” restore MainScene's embedded CScene vtable pointer.
//
// Root cause: the static initializer __sinit_mainloop.cpp @0x373580 (real recomp
// body ps2___sinit_mainloop_cpp_0x373580) does not take effect in the headless
// run, so MainScene+0x10548 ג€” the CScene vtable pointer it writes with
// __vt__6CScene (0x375FE0) ג€” stays 0. InitDungeonMain's scene-entry dispatch
// (decompiled L158085 / asm 0x1CC1C4-0x1CC1D8:
//     lw t9,0x548(scene+0x10000) ; t9 = *(scene+0x10548)  -> 0 (null vtable)
//     lw t9,0x8(t9)              ; vtable slot +8 = Initialize__6CSceneFv@0x282EA0
//     jalr t9                    ; this = scene)
// is intended to call Initialize__6CSceneFv(MainScene), which sets the scene
// character count (scene+0x40) to 0x80 and initialises all 128 CSceneCharacter
// slots. With the vtable pointer null the jalr targets *(0+8); the dispatcher
// silently recovers to ra (no-op), so Initialize never runs, count stays 0, the
// 8 party AssignChara calls in InitDungeonMain all fail (GetSceneCharacter sees
// count 0), GetCharacter(DngMainScene,0) returns 0 -> MainChara=0, and the first
// LoopDungeonMain frame null-derefs it in DngStep@0x1D06C0 (bad=0x1 ra=0x1d0838).
//
// Fix: write the vtable pointer __sinit_mainloop should have set, before the
// scene-entry dispatch runs, so the game's own Initialize dispatch fires. The
// CScene vtable (0x375FE0) is static rodata and always loaded; verified at
// runtime that 0x375FE0+8 == 0x282EA0 (Initialize__6CSceneFv). Idempotent: only
// writes when the slot is still null. Both addresses are fixed ELF symbols,
// consistent with the other absolute-address game overrides in this file.
static void f50_4_repair_main_scene_vtable(uint8_t *rdram)
{
    constexpr uint32_t kMainScene = 0x01DD8260u; // &MainScene (GetMainScene__Fv @0x190870)
    constexpr uint32_t kSceneVt = 0x00375FE0u;   // __vt__6CScene
    if (dc2_read_u32(rdram, kMainScene + 0x10548u) == 0u)
    {
        dc2_write_u32(rdram, kMainScene + 0x10548u, kSceneVt);
    }
}

// F50.7: repair dungeon globals whose vtables are normally installed by
// __sinit_dng_main.cpp @0x373CA0. That initializer does not reliably take
// effect on the headless route, leaving CRandomCircle's embedded CCharacter2
// at 0x01EA4B60 null. InitDungeonMain later calls Initialize__13CRandomCircle,
// whose virtual CCharacter2 initialize dispatch silently no-ops when this
// vtable is zero; the first DngStep then tail-jumps through slot +0xD4 and
// hits pc-zero. Install only the missing final vtable pointers and the adjacent
// fields the static initializer clears, then let the game's own Initialize
// functions run.
static uint32_t f50_7_repair_dng_main_static_objects(uint8_t *rdram)
{
    constexpr uint32_t kRandomCircleChara = 0x01EA4B60u;
    constexpr uint32_t kGeoStone = 0x01EA51C0u;
    constexpr uint32_t kCharacter2Vt = 0x00375810u; // __vt__11CCharacter2
    constexpr uint32_t kGeoStoneVt = 0x00376040u;   // __vt__9CGeoStone
    uint32_t repaired = 0u;

    const uint32_t rcVt = dc2_read_u32(rdram, kRandomCircleChara);
    const uint32_t rcInit = rcVt ? dc2_read_u32(rdram, rcVt + 0x3Cu) : 0u;
    const uint32_t rcStep = rcVt ? dc2_read_u32(rdram, rcVt + 0xD4u) : 0u;
    if (rcVt == 0u || rcInit == 0u || rcStep == 0u)
    {
        dc2_write_u32(rdram, kRandomCircleChara, kCharacter2Vt);
        dc2_write_u32(rdram, kRandomCircleChara + 0x35Cu, 0u);
        dc2_write_u32(rdram, kRandomCircleChara + 0x360u, 0u);
        dc2_write_u32(rdram, kRandomCircleChara + 0x364u, 0u);
        repaired |= 1u;
    }

    const uint32_t geoVt = dc2_read_u32(rdram, kGeoStone);
    const uint32_t geoInit = geoVt ? dc2_read_u32(rdram, geoVt + 0x3Cu) : 0u;
    if (geoVt == 0u || geoInit == 0u)
    {
        dc2_write_u32(rdram, kGeoStone, kGeoStoneVt);
        dc2_write_u32(rdram, kGeoStone + 0x55Cu, 0u);
        dc2_write_u32(rdram, kGeoStone + 0x560u, 0u);
        dc2_write_u32(rdram, kGeoStone + 0x564u, 0u);
        repaired |= 2u;
    }

    if (f50_7_trace_selected_map_enabled() && repaired != 0u)
    {
        std::fprintf(stderr,
                     "[F50.7:dng-sinit] repaired=0x%x rcVt=0x%x->0x%x rcStep=0x%x geoVt=0x%x->0x%x\n",
                     repaired, rcVt, dc2_read_u32(rdram, kRandomCircleChara),
                     dc2_read_u32(rdram, dc2_read_u32(rdram, kRandomCircleChara) + 0xD4u),
                     geoVt, dc2_read_u32(rdram, kGeoStone));
    }

    return repaired;
}

static uint32_t f50_7_equip_id(uint8_t *rdram, uint32_t charaData, uint32_t offset)
{
    return charaData ? (dc2_read_u32(rdram, charaData + offset) & 0xFFFFu) : 0u;
}

static bool f50_7_has_missing_dungeon_equipment(uint8_t *rdram, uint32_t charaData)
{
    if (charaData == 0u)
        return true;
    return f50_7_equip_id(rdram, charaData, 0x322u) == 0u ||
           f50_7_equip_id(rdram, charaData, 0x24Au) == 0u ||
           f50_7_equip_id(rdram, charaData, 0x2B6u) == 0u;
}

static void f50_7_apply_direct_equips(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                      uint32_t udm, uint32_t slot, const uint32_t *items, uint32_t count)
{
    for (uint32_t i = 0u; i < count; ++i)
        f50_callg(rdram, ctx, runtime, 0x0019D560u, udm, slot, items[i], 0u);
}

static uint32_t f50_7_first_costume_id(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                       uint64_t costumeAttr, uint32_t costumeType, uint32_t scratch)
{
    if (uint8_t *buf = getMemPtr(rdram, scratch))
        std::memset(buf, 0, 0x20u);

    const uint32_t count =
        f50_callg_u64a0(rdram, ctx, runtime, 0x002F2B20u, costumeAttr, costumeType, scratch, 0u);
    const uint32_t first = dc2_read_u32(rdram, scratch) & 0xFFFFu;
    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr,
                     "[F50.7:costume-list] type=%u count=%u first=%u attr=0x%08x%08x scratch=0x%x\n",
                     costumeType, count, first,
                     static_cast<uint32_t>(costumeAttr >> 32u),
                     static_cast<uint32_t>(costumeAttr), scratch);
    }
    return (count != 0u && first != 0xFFFFu) ? first : 0u;
}

static bool f50_7_seed_used_data_id(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    uint32_t udm, uint32_t charaData, uint32_t idOffset,
                                    uint32_t itemId)
{
    if (udm == 0u || charaData == 0u || itemId == 0u ||
        f50_7_equip_id(rdram, charaData, idOffset) != 0u)
        return false;

    const uint32_t usedData = charaData + idOffset - 2u;
    f50_callg(rdram, ctx, runtime, 0x0019E9E0u, udm, usedData, itemId, 0u);
    if (f50_7_equip_id(rdram, charaData, idOffset) == 0u)
    {
        dc2_write_u16(rdram, usedData + 0x00u, 1u);
        dc2_write_u16(rdram, usedData + 0x02u, static_cast<uint16_t>(itemId));
        dc2_write_u16(rdram, usedData + 0x10u, 1u);
    }
    return f50_7_equip_id(rdram, charaData, idOffset) != 0u;
}

static bool f50_7_seed_costume_slot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    uint32_t udm, uint32_t charaData, uint32_t idOffset,
                                    uint64_t costumeAttr, uint32_t costumeType, uint32_t scratch)
{
    const uint32_t itemId =
        f50_7_first_costume_id(rdram, ctx, runtime, costumeAttr, costumeType, scratch);
    return f50_7_seed_used_data_id(rdram, ctx, runtime, udm, charaData, idOffset, itemId);
}

static void f50_7_repair_debug_dungeon_equipment(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static bool done = false;
    if (done || !f50_6_debug_menu_enabled())
        return;
    done = true;

    alignas(16) __m128i savedRegs[32];
    std::memcpy(savedRegs, ctx->r, sizeof(savedRegs));
    const uint32_t savedPc = ctx->pc;
    const uint32_t savedSp = getRegU32(ctx, 29);

    const uint32_t udm = f50_callg(rdram, ctx, runtime, 0x00196BE0u, 0u, 0u, 0u, 0u);
    uint32_t chara0 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u) : 0u;
    uint32_t chara1 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u) : 0u;
    const uint32_t activeBefore = udm ? f50_7_equip_id(rdram, udm, 0x44D96u) : 0xFFFFFFFFu;
    const uint32_t c0MainBefore = f50_7_equip_id(rdram, chara0, 0x322u);
    const uint32_t c0SideBefore = f50_7_equip_id(rdram, chara0, 0x24Au);
    const uint32_t c0SkinBefore = f50_7_equip_id(rdram, chara0, 0x2B6u);
    const uint32_t c1MainBefore = f50_7_equip_id(rdram, chara1, 0x322u);
    const uint32_t c1SideBefore = f50_7_equip_id(rdram, chara1, 0x24Au);
    const uint32_t c1SkinBefore = f50_7_equip_id(rdram, chara1, 0x2B6u);
    bool repaired = false;
    uint32_t costumeSeeds = 0u;

    if (udm != 0u &&
        (f50_7_has_missing_dungeon_equipment(rdram, chara0) ||
         f50_7_has_missing_dungeon_equipment(rdram, chara1)))
    {
        // Use the native debug dungeon preset first; direct equips below cover any
        // slots the preset leaves empty in this headless route.
        f50_callg(rdram, ctx, runtime, 0x001A1B80u, udm, 4u, 0u, 0u);
        repaired = true;

        chara0 = f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u);
        chara1 = f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u);
        if (f50_7_has_missing_dungeon_equipment(rdram, chara0))
        {
            static constexpr uint32_t kMaxEquips[] = {2u, 0x17u, 10u};
            f50_7_apply_direct_equips(rdram, ctx, runtime, udm, 0u, kMaxEquips,
                                      static_cast<uint32_t>(sizeof(kMaxEquips) / sizeof(kMaxEquips[0])));
        }
        if (f50_7_has_missing_dungeon_equipment(rdram, chara1))
        {
            static constexpr uint32_t kMonicaEquips[] = {0x7Fu, 0x85u, 0x10Au, 0x6Eu, 0x5Bu};
            f50_7_apply_direct_equips(rdram, ctx, runtime, udm, 1u, kMonicaEquips,
                                      static_cast<uint32_t>(sizeof(kMonicaEquips) / sizeof(kMonicaEquips[0])));
        }

        static constexpr uint64_t kDefaultCostumeAttr = 0x00000001274521CBull;
        const uint64_t savedCostumeAttr = dc2_read_u64(rdram, udm + 0x45598u);
        const uint64_t costumeAttr = savedCostumeAttr ? savedCostumeAttr : kDefaultCostumeAttr;
        if (savedCostumeAttr == 0u)
        {
            dc2_write_u32(rdram, udm + 0x45598u, static_cast<uint32_t>(kDefaultCostumeAttr));
            dc2_write_u32(rdram, udm + 0x4559Cu, static_cast<uint32_t>(kDefaultCostumeAttr >> 32u));
        }

        if (f50_7_trace_selected_map_enabled())
        {
            std::fprintf(stderr, "[F50.7:cosraw]");
            for (uint32_t i = 0u; i < 34u; ++i)
            {
                const uint32_t rec = dc2_read_u32(rdram, 0x0035CC40u + i * 4u);
                std::fprintf(stderr, " %u:%u/%u", i, rec & 0xFFFFu, (rec >> 16u) & 0xFFFFu);
            }
            std::fprintf(stderr, "\n");
        }

        const uint32_t scratch = savedSp - 0x200u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x24Au,
                                                costumeAttr, 6u, scratch + 0x00u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x322u,
                                                costumeAttr, 5u, scratch + 0x20u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x2B6u,
                                                costumeAttr, 7u, scratch + 0x40u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x24Au,
                                                costumeAttr, 9u, scratch + 0x60u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x322u,
                                                costumeAttr, 8u, scratch + 0x80u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x2B6u,
                                                costumeAttr, 10u, scratch + 0xA0u) ? 1u : 0u;

        // If the early debug-menu route cannot query item types yet, use the
        // known default costume IDs from cosbit_table. The Monica IDs match the
        // title debug route's direct equips; the Max IDs are the corresponding
        // first entries for the type-5/6/7 ranges.
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x322u, 111u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x24Au, 117u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x2B6u, 124u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x322u, 127u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x24Au, 133u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x2B6u, 266u) ? 1u : 0u;
    }

    chara0 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u) : 0u;
    chara1 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u) : 0u;
    const uint32_t activeAfter = udm ? f50_7_equip_id(rdram, udm, 0x44D96u) : 0xFFFFFFFFu;

    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr,
                     "[F50.7:equip] udm=0x%x repaired=%u seeds=%u active=%u->%u c0=(%u,%u,%u)->(%u,%u,%u) c1=(%u,%u,%u)->(%u,%u,%u)\n",
                     udm, repaired ? 1u : 0u, costumeSeeds, activeBefore, activeAfter,
                     c0MainBefore, c0SideBefore, c0SkinBefore,
                     f50_7_equip_id(rdram, chara0, 0x322u),
                     f50_7_equip_id(rdram, chara0, 0x24Au),
                     f50_7_equip_id(rdram, chara0, 0x2B6u),
                     c1MainBefore, c1SideBefore, c1SkinBefore,
                     f50_7_equip_id(rdram, chara1, 0x322u),
                     f50_7_equip_id(rdram, chara1, 0x24Au),
                     f50_7_equip_id(rdram, chara1, 0x2B6u));
    }

    std::memcpy(ctx->r, savedRegs, sizeof(savedRegs));
    ctx->pc = savedPc;
}

static const char *f50_7_fallback_item_path(uint32_t itemId)
{
    switch (itemId)
    {
    case 111u: return "mainchr/ha001.chr";
    case 117u: return "mainchr/sh001.chr";
    case 124u: return "mainchr/ac002.chr";
    case 127u: return "mainchr/ac005.chr";
    case 133u: return "mainchr/bo005.chr";
    case 266u: return "mainchr/c02fuku4.chr";
    default: return nullptr;
    }
}

static bool f50_7_guest_cstr_empty(uint8_t *rdram, uint32_t addr)
{
    const char *p = reinterpret_cast<const char *>(getConstMemPtr(rdram, addr));
    return p == nullptr || p[0] == '\0';
}

static void f50_7_write_item_path(uint8_t *rdram, const char *path)
{
    constexpr uint32_t kItemFilePath = 0x01E74190u;
    constexpr size_t kItemFilePathSize = 128u;
    uint8_t *dst = getMemPtr(rdram, kItemFilePath);
    if (!dst || !path)
        return;

    const size_t n = std::strlen(path);
    const size_t copyLen = (n < kItemFilePathSize - 1u) ? n : (kItemFilePathSize - 1u);
    std::memcpy(dst, path, copyLen);
    dst[copyLen] = 0u;
}

static void f50_7_get_item_file_path_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t itemId = getRegU32(ctx, 4) & 0xFFFFu;
    GetItemFilePath__Fii_0x195d40(rdram, ctx, runtime);

    constexpr uint32_t kItemFilePath = 0x01E74190u;
    const char *fallback = f50_7_fallback_item_path(itemId);
    if (!f50_6_debug_menu_enabled() || fallback == nullptr || !f50_7_guest_cstr_empty(rdram, getRegU32(ctx, 2)))
        return;

    f50_7_write_item_path(rdram, fallback);
    setReturnU32(ctx, kItemFilePath);
    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr, "[F50.7:itempath] id=%u -> '%s'\n", itemId, fallback);
    }
}

static void f50_7_random_circle_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t chara = self + 0x40u;

    uint32_t vt = self ? dc2_read_u32(rdram, chara) : 0u;
    uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0xD4u) : 0u;
    uint32_t repaired = 0u;
    if (self == 0x01EA4B20u && (vt == 0u || slot == 0u))
    {
        repaired = f50_7_repair_dng_main_static_objects(rdram);
        vt = dc2_read_u32(rdram, chara);
        slot = vt ? dc2_read_u32(rdram, vt + 0xD4u) : 0u;
    }

    if (f50_7_trace_selected_map_enabled() && (n <= 64u || repaired != 0u || slot == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:rcstep] n=%u self=0x%x chara=0x%x ra=0x%x vt=0x%x slot=0x%x repaired=0x%x\n",
                     n, self, chara, ra, vt, slot, repaired);
    }

    if (slot == 0u)
    {
        ctx->pc = ra;
        return;
    }
    if (!runtime->hasFunction(slot))
    {
        ctx->pc = slot;
        return;
    }

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, chara);
    f50_set_reg(ctx, 31, 0u);
    f50_run_guest_call(rdram, ctx, runtime, slot);

    const uint32_t postPc = ctx->pc;
    ctx->r[31] = savedRa;
    ctx->pc = postPc == 0u ? ra : postPc;
}

// F55: A/B probe for the dungeon free-map rectangle divergence (F53 blocker).
// Logs CalcGlidPutPos@0x1EA890 inputs (GLID grid shorts, view-offset floats at
// this+0x100/0x104) and outputs, to compare against live PCSX2 ground truth
// (view=(152,208), gridX 2..10, outputs on-screen 248..560 x 208..408).
static bool f55_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F55");
    return enabled;
}

static float f55_read_f32(uint8_t *rdram, uint32_t addr)
{
    const uint32_t bits = dc2_read_u32(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static void f55_calc_glid_put_pos_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t glid = getRegU32(ctx, 5);
    const uint32_t outX = getRegU32(ctx, 6);
    const uint32_t outY = getRegU32(ctx, 7);
    const uint32_t noView = getRegU32(ctx, 8); // 5th arg in $t0
    const uint32_t ra = getRegU32(ctx, 31);
    const bool log = f55_trace_enabled() && n <= 96u;

    int16_t type = 0, gx = 0, gy = 0, mode = 0;
    float viewX = 0.0f, viewY = 0.0f;
    uint32_t mgr = 0u, glidArr = 0u, glidCnt = 0u;
    uint32_t visible = 0u, glidA = 0u, glidB = 0u, texA = 0u, texB = 0u;
    float alpha = 0.0f, tgtX = 0.0f, tgtY = 0.0f;
    if (log)
    {
        if (glid != 0u)
        {
            type = static_cast<int16_t>(dc2_read_u32(rdram, glid) & 0xFFFFu);
            gx = static_cast<int16_t>((dc2_read_u32(rdram, glid) >> 16) & 0xFFFFu);
            gy = static_cast<int16_t>(dc2_read_u32(rdram, glid + 4u) & 0xFFFFu);
        }
        if (self != 0u)
        {
            mode = static_cast<int16_t>(dc2_read_u32(rdram, self + 0xCu) & 0xFFFFu);
            viewX = f55_read_f32(rdram, self + 0x100u);
            viewY = f55_read_f32(rdram, self + 0x104u);
            tgtX = f55_read_f32(rdram, self + 0x108u);
            tgtY = f55_read_f32(rdram, self + 0x10Cu);
            mgr = dc2_read_u32(rdram, self + 4u);
            visible = dc2_read_u32(rdram, self + 8u) & 0xFFu;   // gate 1 in CDngFreeMap::Draw
            alpha = f55_read_f32(rdram, self + 0xF0u);           // gate 2
            texB = dc2_read_u32(rdram, self + 0xD8u);            // gate 3 (map texture ptr)
            texA = dc2_read_u32(rdram, self + 0xD4u);
            glidA = dc2_read_u32(rdram, self + 0xC4u);           // player glid
            glidB = dc2_read_u32(rdram, self + 0xCCu);           // current-room glid
            if (mgr != 0u)
            {
                glidArr = dc2_read_u32(rdram, mgr + 4u);
                glidCnt = dc2_read_u32(rdram, mgr + 8u);
            }
        }
    }

    CalcGlidPutPos__11CDngFreeMapFP9GLID_INFORfRfi_0x1ea890(rdram, ctx, runtime);

    if (log)
    {
        const float ox = (outX != 0u) ? f55_read_f32(rdram, outX) : 0.0f;
        const float oy = (outY != 0u) ? f55_read_f32(rdram, outY) : 0.0f;
        std::fprintf(stderr,
                     "[F55:cgpp] n=%u ra=0x%x this=0x%x glid=0x%x noview=%u type=%d gx=%d gy=%d mode=%d "
                     "view=(%.2f,%.2f) tgt=(%.2f,%.2f) mgr=0x%x arr=0x%x cnt=%u "
                     "vis=%u alpha=%.2f texA=0x%x texB=0x%x glidA=0x%x glidB=0x%x -> out=(%.2f,%.2f)\n",
                     n, ra, self, glid, noView, type, gx, gy, mode,
                     viewX, viewY, tgtX, tgtY, mgr, glidArr, glidCnt,
                     visible, alpha, texA, texB, glidA, glidB, ox, oy);
    }
}

static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap)
{
    out[0] = '\0';
    if (addr == 0u)
        return;
    uint32_t i = 0u;
    for (; i + 1u < cap; ++i)
    {
        const uint8_t b = (uint8_t)(dc2_read_u32(rdram, addr + i) & 0xFFu);
        if (b == 0u)
            break;
        out[i] = (b >= 0x20u && b < 0x7Fu) ? (char)b : '.';
    }
    out[i] = '\0';
}

static void f55_init_end_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 8u)
        std::fprintf(stderr, "[F55:initend] n=%u this=0x%x ra=0x%x\n",
                     n, getRegU32(ctx, 4), getRegU32(ctx, 31));
    InitEnd__12CMenuTreeMapFv_0x1ef9f0(rdram, ctx, runtime);
}

static void f55_set_texture_info_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    SetTextureInfo__11CDngFreeMapFv_0x1eabe0(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 8u)
        std::fprintf(stderr,
                     "[F55:settex] n=%u this=0x%x -> d4=0x%x d8=0x%x dc=0x%x e0=0x%x (post pc=0x%x)\n",
                     n, self,
                     dc2_read_u32(rdram, self + 0xD4u), dc2_read_u32(rdram, self + 0xD8u),
                     dc2_read_u32(rdram, self + 0xDCu), dc2_read_u32(rdram, self + 0xE0u),
                     ctx->pc);
}

static void f55_get_pack_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t pack = getRegU32(ctx, 4);
    const uint32_t namePtr = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    GetPackFile__FPUiPcPi_0x149cd0(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 96u)
    {
        char nm[40];
        f55_read_cstr(rdram, namePtr, nm, sizeof(nm));
        std::fprintf(stderr, "[F55:packfile] n=%u pack=0x%x name='%s' ra=0x%x -> v0=0x%x\n",
                     n, pack, nm, ra, getRegU32(ctx, 2));
    }
}

static void f55_get_read_bg_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t idx = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    GetReadBGFile__Fi_0x148c70(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 64u)
        std::fprintf(stderr, "[F55:readbg] n=%u idx=%u ra=0x%x -> v0=0x%x\n",
                     n, idx, ra, getRegU32(ctx, 2));
}

static void f55_load_file_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t namePtr = getRegU32(ctx, 4);
    const uint32_t buf = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    char nm[64];
    char curdir[64];
    uint32_t headerNum = 0u, dataSector = 0u, foundIdx = 0xFFFFFFFFu;
    char e0[48], e1[48];
    e0[0] = e1[0] = '\0';
    const bool log = f55_trace_enabled() && n <= 64u;
    if (log)
    {
        f55_read_cstr(rdram, namePtr, nm, sizeof(nm));
        f55_read_cstr(rdram, 0x334390u, curdir, sizeof(curdir)); // CurrentDir
        headerNum = dc2_read_u32(rdram, 0x376D8Cu);              // header_num
        dataSector = dc2_read_u32(rdram, 0x376D94u);             // data_sector
        char full[128];
        std::snprintf(full, sizeof(full), "%s%s", curdir, nm);
        for (uint32_t i = 0u; i < headerNum && i < 16384u; ++i)
        {
            const uint32_t entryName = dc2_read_u32(rdram, 0x382680u + i * 16u);
            if (entryName == 0u)
                continue;
            char en[64];
            f55_read_cstr(rdram, entryName, en, sizeof(en));
            if (i == 0u)
                std::snprintf(e0, sizeof(e0), "%s", en);
            if (i == 1u)
                std::snprintf(e1, sizeof(e1), "%s", en);
            if (_stricmp(en, full) == 0)
            {
                foundIdx = i;
                break;
            }
        }
    }
    LoadFileBG__FPcP1Pi_0x148930(rdram, ctx, runtime);
    if (log)
    {
        std::fprintf(stderr,
                     "[F55:loadbg] n=%u name='%s' curdir='%s' headerNum=%u dataSector=%u foundIdx=%d "
                     "e0='%s' e1='%s' buf=0x%x ra=0x%x -> v0=0x%x (post pc=0x%x)\n",
                     n, nm, curdir, headerNum, dataSector, (int)foundIdx, e0, e1,
                     buf, ra, getRegU32(ctx, 2), ctx->pc);
    }
}

static void f55_init_read_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 32u)
        std::fprintf(stderr, "[F55:initreadbg] n=%u ra=0x%x\n", n, getRegU32(ctx, 31));
    InitReadBG__Fv_0x1488d0(rdram, ctx, runtime);
}

static void f55_start_read_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 32u)
        std::fprintf(stderr, "[F55:startreadbg] n=%u ra=0x%x\n", n, getRegU32(ctx, 31));
    StartReadBG__Fv_0x148cc0(rdram, ctx, runtime);
}

// G224 FIX (root: un-run __sinit class, same family as F50.4/F50.7/G12/G193):
// __sinit_dng_main.cpp@0x373CA0 constructs the two static dungeon CCameraControl
// objects (__ct__14CCameraControlFv@0x2EBE80 on 0x1EA5830 and 0x1EA5A20, asm
// 0x373F60-0x373F84). That initializer does not take effect on this route, so the
// free-roam camera object's +0x60 vtable pointer (0x376330, written by the ctor)
// stays 0. Every camera vtable dispatch then silently no-ops (the generated jalr
// treats target 0 as skip-call): DngMainDraw's per-frame Step__14CCameraControlFi
// (slot +0x08, the nextPos->pos commit) and DngMainKey's SetFollow (slot +0x20,
// the player-follow target) never run, leaving the free-roam camera frozen at its
// boot pose -- the G223 dungeon-0 "static gradient instead of 3D world" symptom.
// Fix: run the game's OWN recompiled ctor on each object (f50_run_guest_call
// resume-loop, G212 gold standard) before InitDungeonMain, exactly reproducing
// the static initializer's effect. Idempotent: only when +0x60 is still 0.
// Kill switch: DC2_G224_NO_CAMFIX=1.
static void g224_repair_dungeon_camera_controls(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G224_NO_CAMFIX");
    if (disabled)
        return;
    constexpr uint32_t kCamCtl[2] = {0x01EA5830u, 0x01EA5A20u};
    constexpr uint32_t kCamCtlVt60 = 0x00376330u;
    constexpr uint32_t kCamCtlCtor = 0x002EBE80u; // __ct__14CCameraControlFv
    for (uint32_t i = 0; i < 2u; ++i)
    {
        if (dc2_read_u32(rdram, kCamCtl[i] + 0x60u) != 0u)
            continue;
        // Preserve the wrapped function's incoming state around the guest call
        // (f50_callg clobbers a0-a3/ra and the callee clobbers v0/v1 + temps).
        const auto savedA0 = ctx->r[4];
        const auto savedA1 = ctx->r[5];
        const auto savedA2 = ctx->r[6];
        const auto savedA3 = ctx->r[7];
        const auto savedV0 = ctx->r[2];
        const auto savedV1 = ctx->r[3];
        const auto savedRa = ctx->r[31];
        const uint32_t savedPc = ctx->pc;
        f50_callg(rdram, ctx, runtime, kCamCtlCtor, kCamCtl[i], 0u, 0u, 0u);
        ctx->r[4] = savedA0;
        ctx->r[5] = savedA1;
        ctx->r[6] = savedA2;
        ctx->r[7] = savedA3;
        ctx->r[2] = savedV0;
        ctx->r[3] = savedV1;
        ctx->r[31] = savedRa;
        ctx->pc = savedPc;
        if (f48_4_trace_postcostume_enabled() || dc2_env_flag_enabled("DC2_TRACE_F59"))
        {
            std::fprintf(stderr, "[G224:camfix] constructed CCameraControl@0x%x vt60=0x%x (expect 0x%x)\n",
                         kCamCtl[i], dc2_read_u32(rdram, kCamCtl[i] + 0x60u), kCamCtlVt60);
        }
    }
}

static void f48_4_init_dungeon_main_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_init_log;
    g_f50_1_dungeon_active = true; // F50.1: arm placement-new probe for the dungeon path
    const uint32_t gp = getRegU32(ctx, 28);
    // F50.4: at InitDungeonMain ENTRY (before any dungeon code), read MainScene's
    // char count (scene+0x40) and the embedded CScene-vtable pointer (scene+0x10548)
    // that __sinit_mainloop@0x373580 sets to 0x375fe0 (__vt__6CScene). If both are 0
    // here, the static initializer never ran. Also dump the static __vt__6CScene
    // (0x375fe0, always loaded rodata) slots 0..0x14 to locate Initialize@0x282ea0.
    if (f48_4_trace_postcostume_enabled() && n == 1u)
    {
        const uint32_t sceneAbs = 0x01DD8260u; // &MainScene (from L158066 GetMainScene)
        const uint32_t vt = 0x00375FE0u;       // __vt__6CScene
        std::fprintf(stderr,
                     "[F50.4:initentry] count=%u vptr@+0x10548=0x%x | vt0=0x%x vt4=0x%x vt8=0x%x vtC=0x%x vt10=0x%x vt14=0x%x\n",
                     dc2_read_u32(rdram, sceneAbs + 0x40u),
                     dc2_read_u32(rdram, sceneAbs + 0x10548u),
                     dc2_read_u32(rdram, vt + 0x00u), dc2_read_u32(rdram, vt + 0x04u),
                     dc2_read_u32(rdram, vt + 0x08u), dc2_read_u32(rdram, vt + 0x0Cu),
                     dc2_read_u32(rdram, vt + 0x10u), dc2_read_u32(rdram, vt + 0x14u));
    }
    if (f48_4_trace_postcostume_enabled() && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=InitDungeonMain.enter n=%u frame=%u ra=0x%x loop=%u next=%u a0=0x%x d62c=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 4), dc2_read_u32(rdram, 0x01ECD62Cu));
    }
    // F50.4 FIX (always on, not gated): restore the MainScene CScene vtable
    // pointer before InitDungeonMain's scene-entry Initialize dispatch runs.
    f50_4_repair_main_scene_vtable(rdram);
    f50_7_repair_dng_main_static_objects(rdram);
    g224_repair_dungeon_camera_controls(rdram, ctx, runtime);
    f50_7_repair_debug_dungeon_equipment(rdram, ctx, runtime);
    InitDungeonMain__F13INIT_LOOP_ARG_0x1cc040(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=InitDungeonMain.exit n=%u frame=%u ret=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2));
    }
}
