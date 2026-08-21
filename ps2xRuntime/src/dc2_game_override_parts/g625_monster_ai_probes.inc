// ===================== G625: MONSTER PROJECTILE / DAMAGE-PATH PROBES (DEFAULT OFF) ==============
//
// Symptom under investigation: in `dungeon1` the Man-Eating Grass never damages the player — the
// player's HP sits at 32.0/32.0 for a whole 120 s run, while the same scene on PCSX2 takes the
// player 32 -> 21 -> 7 -> 6 -> 0 within seconds.
//
// What the G625 lookupFunction census (runtime_init_and_signals.inc) already established:
//   * the MONSTER script fires: `_ESM_CREATE` 34x with `_ESM_SET_VECT1/2`, `_ESM_SET_TARGET_ID`,
//     `_ESM_SET_VALUE` and `_GET_BASE_ATTACK` beside it, and `_SET_DMG2` 40x;
//   * `CreateEffSpt` succeeds 182x and the effect scripts run (`_SPT_*` in the thousands);
//   * but NO effect script ever calls `_CREATE_DAMAGE`, `_COLPRIM_CREATE`, `_DMG_SET_DAMAGE` or
//     `_ES_CREATE` — every one of those VM commands reads 0, and they are reachable ONLY through
//     the script VM's indirect ext table, so a 0 there is a real absence, not a dispatch artefact.
//
// These probes close the remaining gap between "a damage volume was entered" and "the player lost
// HP", on observables that exist identically in the runner and in PCSX2 RAM:
//   * `[G625:colprim]` — CColPrimMan @0x01EA0710 is 0x40 slots of 0x110 bytes starting at +0x10,
//     with the slot's live flag at +0xc (so this+i*0x110+0x1c). ActivePrimNum counts exactly that.
//   * `[G625:esm]` — the effect-script NAME the monster asks for, and whether `_ESM_CREATE`
//     accepted it (v0) — RS_STACKDATA is {u32 type, u32 value} pairs, so the name pointer is at
//     stack[0]+4 and the mode is $a1.
//
// Enable: DC2_G625_HP=1 (per-frame lines) and/or DC2_G625_ESM=1 (per-shot lines).

// (the recompiled body's extern lives at file scope in dc2_game_override.cpp — this .inc is
// included inside an anonymous namespace, where an `extern` would declare a NEW internal symbol)

static constexpr uint32_t kG625ColPrimMan = 0x01EA0710u;
static constexpr uint32_t kG625ColPrimSlots = 0x40u;
static constexpr uint32_t kG625ColPrimStride = 0x110u;

// ActivePrimNum__11CColPrimManFv, read straight out of guest RAM so the same expression can be
// evaluated against a paused PCSX2 without running any guest code.
static uint32_t g625_active_colprims(uint8_t *rdram)
{
    uint32_t live = 0u;
    for (uint32_t i = 0; i < kG625ColPrimSlots; ++i)
    {
        if (dc2_read_u32(rdram, kG625ColPrimMan + i * kG625ColPrimStride + 0x1Cu) != 0u)
            ++live;
    }
    return live;
}

// _EFF_SCRIPT instance list: EffScriptMan (gp-0x6134) -> +0x1188 head, +0x144 next.
// Per instance: +0x08 CCharacter2 · +0x24 level · +0x2c sprite count · +0x30 name ·
// +0xa4 program index (-1 = "resume the suspended script") · +0x13c state (2/3 = SKIPPED by Step).
// There are TWO effect-script managers. The scene's is the one `EffScriptMan` (gp-0x6134) points
// at; the MONSTER one is *(ActiveMonster(gp-0x7248) + 0xfff0) and is the manager _ESM_CREATE spawns
// the poison-spit projectile into.
static uint32_t g625_monster_eff_man(uint8_t *rdram)
{
    const uint32_t am = dc2_read_u32(rdram, 0x003772A8u);
    if (am <= 0x80000u || am >= 0x2000000u)
        return 0u;
    return dc2_read_u32(rdram, am + 0xFFF0u);
}

static uint32_t g625_eff_count(uint8_t *rdram, uint32_t man)
{
    if (man <= 0x80000u || man >= 0x2000000u)
        return 0u;
    uint32_t inst = dc2_read_u32(rdram, man + 0x1188u), i = 0;
    while (inst > 0x80000u && inst < 0x2000000u && i < 256u)
    {
        ++i;
        inst = dc2_read_u32(rdram, inst + 0x144u);
    }
    return i;
}

static void g625_dump_eff_man(uint8_t *rdram, uint32_t n, const char *tag, uint32_t man)
{
    if (man <= 0x80000u || man >= 0x2000000u)
        return;
    uint32_t inst = dc2_read_u32(rdram, man + 0x1188u);
    uint32_t i = 0;
    for (; inst > 0x80000u && inst < 0x2000000u && i < 48u; ++i)
    {
        char name[24];
        uint32_t k = 0;
        for (; k + 1u < sizeof(name); ++k)
        {
            const uint8_t c = rdram[(inst + 0x30u + k) & 0x01FFFFFFu];
            if (c == 0u)
                break;
            name[k] = static_cast<char>(c);
        }
        name[k] = '\0';
        std::fprintf(stderr,
                     "[G625:effinst] n=%u man=%s i=%u inst=0x%x name=\"%s\" lvl=%d prog=%d state=%d "
                     "chr=0x%x spr=%d\n",
                     n, tag, i, inst, name, (int32_t)dc2_read_u32(rdram, inst + 0x24u),
                     (int32_t)dc2_read_u32(rdram, inst + 0xA4u),
                     (int32_t)dc2_read_u32(rdram, inst + 0x13Cu),
                     dc2_read_u32(rdram, inst + 0x08u),
                     (int32_t)dc2_read_u32(rdram, inst + 0x2Cu));
        inst = dc2_read_u32(rdram, inst + 0x144u);
    }
}

// Integrity watch: BuildBase writes `*base = baseId` as the header's first word, so recording
// (ptr,id) at build time gives a cheap corruption oracle for every effect-script base.
static uint32_t g_g625BasePtr[64];
static int32_t g_g625BaseId[64];
static uint32_t g_g625BaseN = 0u;

static uint32_t g625_bases_bad(uint8_t *rdram)
{
    uint32_t bad = 0u;
    for (uint32_t i = 0; i < g_g625BaseN; ++i)
    {
        if ((int32_t)dc2_read_u32(rdram, g_g625BasePtr[i]) != g_g625BaseId[i])
            ++bad;
    }
    return bad;
}

// Called every guest frame while tracing: reports the first frame each recorded base header stops
// reading back its own id, plus the 32 bytes that replaced it.
static void g625_watch_bases(uint8_t *rdram, uint32_t n)
{
    static uint8_t s_reported[64] = {0};
    for (uint32_t i = 0; i < g_g625BaseN; ++i)
    {
        const int32_t now = (int32_t)dc2_read_u32(rdram, g_g625BasePtr[i]);
        if (now == g_g625BaseId[i] || s_reported[i])
            continue;
        s_reported[i] = 1u;
        std::fprintf(stderr, "[G625:clobber] n=%u slot=%u ptr=0x%x wantId=%d gotId=%d bytes=",
                     n, i, g_g625BasePtr[i], g_g625BaseId[i], now);
        for (uint32_t k = 0; k < 32u; ++k)
            std::fprintf(stderr, "%02x", rdram[(g_g625BasePtr[i] + k) & 0x01FFFFFFu]);
        std::fprintf(stderr, "\n");
    }
}

static bool g625_esm_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G625_ESM");
    return enabled;
}

static void g625_esm_create_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    uint32_t stack = 0u, mode = 0u, namePtr = 0u;
    if (trace)
    {
        stack = getRegU32(ctx, 4);
        mode = getRegU32(ctx, 5);
        namePtr = (stack != 0u) ? dc2_read_u32(rdram, stack + 4u) : 0u;
    }
    ps2__ESM_CREATE__FP12RS_STACKDATAi_0x1e6620(rdram, ctx, runtime);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    if (s_lines >= 400u)
        return;
    ++s_lines;
    char name[64];
    uint32_t k = 0;
    if (namePtr > 0x80000u && namePtr < 0x2000000u)
    {
        for (; k + 1u < sizeof(name); ++k)
        {
            const uint8_t c = rdram[(namePtr + k) & 0x01FFFFFFu];
            if (c == 0u)
                break;
            name[k] = static_cast<char>(c);
        }
    }
    name[k] = '\0';
    std::fprintf(stderr, "[G625:esm] n=%u mode=%u name=\"%s\" ret=%u colprims=%u\n",
                 s_lines, mode, name, getRegU32(ctx, 2), g625_active_colprims(rdram));
    g625_dump_eff_man(rdram, s_lines, "scene", dc2_read_u32(rdram, 0x003783BCu));
    g625_dump_eff_man(rdram, s_lines, "mons", g625_monster_eff_man(rdram));
}

// CreateEffSpt(name, chrid, flag) is where an effect-script instance is minted. It has five
// distinct failure exits (no bases loaded / base name not found / no memory manager / no free
// per-chara slot / StartStackMode failed), each of which only prints to the game's own printf and
// then returns 0. 182 of these run per `dungeon1` run but only ~150 instances ever execute a
// script program, so the 32 that do not are the interesting population.
static void g625_create_effspt_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    uint32_t namePtr = 0u, chrid = 0u, flag = 0u;
    if (trace)
    {
        namePtr = getRegU32(ctx, 5);
        chrid = getRegU32(ctx, 6);
        flag = getRegU32(ctx, 7);
    }
    CreateEffSpt__16CEffectScriptManFPcii_0x2e1260(rdram, ctx, runtime);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    if (s_lines >= 400u)
        return;
    ++s_lines;
    char name[64];
    uint32_t k = 0;
    if (namePtr > 0x80000u && namePtr < 0x2000000u)
    {
        for (; k + 1u < sizeof(name); ++k)
        {
            const uint8_t c = rdram[(namePtr + k) & 0x01FFFFFFu];
            if (c == 0u)
                break;
            name[k] = static_cast<char>(c);
        }
    }
    name[k] = '\0';
    std::fprintf(stderr, "[G625:effspt] n=%u name=\"%s\" chrid=%d flag=%u ret=0x%x badBases=%u/%u\n",
                 s_lines, name, static_cast<int32_t>(chrid), flag, getRegU32(ctx, 2),
                 g625_bases_bad(rdram), g_g625BaseN);
}

// BuildBase mints the per-base header with `*base = baseId` at the head. Reading that word back the
// instant BuildBase returns separates "BuildBase produced a bad base" from "a later allocation
// overwrote a good one".
static void g625_buildbase_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    uint32_t man = 0u, baseId = 0u, mem = 0u, slotArg = 0u;
    if (trace)
    {
        man = getRegU32(ctx, 4);
        baseId = getRegU32(ctx, 5);
        mem = dc2_read_u32(rdram, getRegU32(ctx, 29) + 0x10u); // param_7 arrives on the stack
        slotArg = getRegU32(ctx, 29);
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    }
    BuildBase__16CEffectScriptManFiP1iP1iP9mgCMemoryi_0x2e0420(rdram, ctx, runtime);
    if (trace)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    if (s_lines >= 120u)
        return;
    ++s_lines;
    uint32_t found = 0u, word0 = 0u, slot = 0xFFFFFFFFu;
    if (man > 0x80000u && man < 0x2000000u)
    {
        for (uint32_t i = 0; i < 0x40u; ++i)
        {
            const uint32_t b = dc2_read_u32(rdram, man + 0x80u + i * 4u);
            if (b > 0x80000u && b < 0x2000000u && dc2_read_u32(rdram, b) == baseId)
            {
                found = b;
                word0 = dc2_read_u32(rdram, b);
                slot = i;
                break;
            }
        }
    }
    if (found && g_g625BaseN < 64u)
    {
        g_g625BasePtr[g_g625BaseN] = found;
        g_g625BaseId[g_g625BaseN] = (int32_t)baseId;
        ++g_g625BaseN;
    }
    std::fprintf(stderr,
                 "[G625:build] n=%u man=0x%x base=%d ret=%d slot=%d ptr=0x%x word0=%d sp=0x%x "
                 "memArg=0x%x count=%d\n",
                 s_lines, man, (int32_t)baseId, (int32_t)getRegU32(ctx, 2), (int32_t)slot, found,
                 (int32_t)word0, slotArg, mem,
                 (man > 0x80000u && man < 0x2000000u)
                     ? (int32_t)dc2_read_u32(rdram, man + 0x180u) : -1);
}

// The int-keyed CreateEffSpt is where the instance is actually allocated and linked into the
// manager's list. Logging its own manager pointer settles whether the poison instance goes into
// the same manager Step() walks.
static void g625_create_effspt_i_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    uint32_t man = 0u, base = 0u, chrid = 0u, flag = 0u;
    if (trace)
    {
        man = getRegU32(ctx, 4);
        base = getRegU32(ctx, 5);
        chrid = getRegU32(ctx, 6);
        flag = getRegU32(ctx, 7);
    }
    // G224's rule: a wrapped call that can be preempted returns to the probe with $v0 holding a
    // MID-CALL value, so a traced read has to suppress preemption to see a completed result.
    if (trace)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    CreateEffSpt__16CEffectScriptManFiii_0x2e0d60(rdram, ctx, runtime);
    if (trace)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    if (s_lines >= 300u)
        return;
    ++s_lines;
    const uint32_t inst = getRegU32(ctx, 2);
    std::fprintf(stderr,
                 "[G625:mint] n=%u man=0x%x base=%d chrid=%d flag=%u inst=0x%x head=0x%x count=%u "
                 "sceneMan=0x%x monsMan=0x%x prog=%d state=%d\n",
                 s_lines, man, (int32_t)base, (int32_t)chrid, flag, inst,
                 (man > 0x80000u && man < 0x2000000u) ? dc2_read_u32(rdram, man + 0x1188u) : 0u,
                 g625_eff_count(rdram, man), dc2_read_u32(rdram, 0x003783BCu),
                 g625_monster_eff_man(rdram),
                 (inst > 0x80000u && inst < 0x2000000u)
                     ? (int32_t)dc2_read_u32(rdram, inst + 0xA4u) : -999,
                 (inst > 0x80000u && inst < 0x2000000u)
                     ? (int32_t)dc2_read_u32(rdram, inst + 0x13Cu) : -999);
    // Which of CreateEffSpt's five exits fired: +0x180 is the loaded-base count, +0x80[0..0x3f]
    // the loaded base pointers (each base's word0 is its base id), +0x04 the memory manager.
    if (inst == 0u && man > 0x80000u && man < 0x2000000u)
    {
        std::fprintf(stderr, "[G625:mint]   fail baseCount=%d mem=0x%x loaded=",
                     (int32_t)dc2_read_u32(rdram, man + 0x180u), dc2_read_u32(rdram, man + 4u));
        for (uint32_t i = 0; i < 0x40u; ++i)
        {
            const uint32_t b = dc2_read_u32(rdram, man + 0x80u + i * 4u);
            if (b != 0u)
                std::fprintf(stderr, "[%u]0x%x=%d,", i, b,
                             (b > 0x80000u && b < 0x2000000u)
                                 ? (int32_t)dc2_read_u32(rdram, b) : -999);
        }
        std::fprintf(stderr, "\n");
    }
}

// ThinkHost mints the CColPrim only while the chara's CURRENT MOTION FRAME (CActionChara+0x388,
// what the vtable+0x108 GetNowFrame returns) lies inside the entered slot's [lo,hi). _SET_MOS runs
// every frame for every monster, so it is the cheapest place to watch that frame against the
// windows _SET_DMG2 entered.
static void g625_set_mos_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    ps2__SET_MOS__FP12RS_STACKDATAi_0x1e5f20(rdram, ctx, runtime);
    if (!g625_esm_trace_enabled())
        return;
    static uint32_t s_calls = 0u, s_lines = 0u;
    if ((++s_calls % 20u) != 0u || s_lines >= 600u)
        return;
    const uint32_t mon = dc2_read_u32(rdram, 0x00377360u);
    if (mon <= 0x80000u || mon >= 0x2000000u)
        return;
    const uint8_t used = rdram[(mon + 0xBD8u) & 0x01FFFFFFu];
    if (used == 0u)
        return;
    ++s_lines;
    std::fprintf(stderr, "[G625:mosfr] n=%u mon=0x%x frame=%.3f wait=%.3f used=%u colprims=%u",
                 s_lines, mon, dc2_read_f32(rdram, mon + 0x388u),
                 dc2_read_f32(rdram, mon + 0x38Cu), used, g625_active_colprims(rdram));
    for (uint32_t i = 0; i < 11u; ++i)
    {
        const uint32_t slot = mon + 0xA20u + i * 0x28u;
        if (!rdram[slot & 0x01FFFFFFu])
            continue;
        std::fprintf(stderr, " | s%u[%.1f,%.1f)prim=0x%x", i,
                     dc2_read_f32(rdram, slot + 0x18u), dc2_read_f32(rdram, slot + 0x1Cu),
                     dc2_read_u32(rdram, slot + 0x24u));
    }
    std::fprintf(stderr, "\n");
}

// _SET_DMG2 is the monster script's "enter a damage volume on this bone for this slice of this
// motion" command. It forwards to EntryDamage2, which returns 0 — i.e. enters NOTHING — when both
// GetWaitToFrame calls return 0.0, and ThinkHost only mints a CColPrim while the chara's current
// motion frame lies inside [+0xa38, +0xa3c). So `ret=0` here means the damage volume was never
// entered, and `lo==hi` means it was entered with an empty window.
static void g625_set_dmg2_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    uint32_t stack = 0u, argc = 0u;
    if (trace)
    {
        stack = getRegU32(ctx, 4);
        argc = getRegU32(ctx, 5);
    }
    ps2__SET_DMG2__FP12RS_STACKDATAi_0x1e32f0(rdram, ctx, runtime);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    if (s_lines >= 400u)
        return;
    ++s_lines;
    // nowMonster = *(u32*)(gp-0x7190); its 11 damage slots start at +0xa20, stride 0x28:
    //   +0x00 live flag · +0x04 motion-name ptr · +0x18 window lo · +0x1c window hi · +0x24 prim
    const uint32_t nowMonster = dc2_read_u32(rdram, 0x00377360u);
    std::fprintf(stderr, "[G625:dmg2] n=%u argc=%u ret=%u mon=0x%x used=%u colprims=%u\n",
                 s_lines, argc, getRegU32(ctx, 2), nowMonster,
                 (nowMonster > 0x80000u && nowMonster < 0x2000000u)
                     ? rdram[(nowMonster + 0xBD8u) & 0x01FFFFFFu] : 0u,
                 g625_active_colprims(rdram));
    if (nowMonster > 0x80000u && nowMonster < 0x2000000u)
    {
        for (uint32_t i = 0; i < 11u; ++i)
        {
            const uint32_t slot = nowMonster + 0xA20u + i * 0x28u;
            const uint8_t live = rdram[slot & 0x01FFFFFFu];
            if (!live)
                continue;
            std::fprintf(stderr, "[G625:dmg2]   slot%u lo=%.3f hi=%.3f mot=0x%x prim=0x%x\n",
                         i, dc2_read_f32(rdram, slot + 0x18u), dc2_read_f32(rdram, slot + 0x1Cu),
                         dc2_read_u32(rdram, slot + 0x04u), dc2_read_u32(rdram, slot + 0x24u));
        }
    }
}

// The dispatch ring that caught the base-header clobber ended on the audio path
// (ezTransToIOP2 -> FlushCache -> sceSifSetDma), so log that transfer's EE-side source range and
// flag any overlap with a recorded effect-script base header.
static void g625_eztrans_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g625_esm_trace_enabled();
    const uint32_t src = getRegU32(ctx, 4);
    const uint32_t dst = getRegU32(ctx, 5);
    const uint32_t size = getRegU32(ctx, 6);
    ezTransToIOP2__FPvPvi_0x18b310(rdram, ctx, runtime);
    if (!trace)
        return;
    static uint32_t s_lines = 0u;
    bool hit = false;
    for (uint32_t i = 0; i < g_g625BaseN; ++i)
    {
        if (g_g625BasePtr[i] >= src && g_g625BasePtr[i] < src + size)
            hit = true;
    }
    if (!hit && s_lines >= 60u)
        return;
    ++s_lines;
    std::fprintf(stderr, "[G625:eztrans] n=%u eeSrc=0x%x iopDst=0x%x size=0x%x overlapsBase=%d\n",
                 s_lines, src, dst, size, hit ? 1 : 0);
}
