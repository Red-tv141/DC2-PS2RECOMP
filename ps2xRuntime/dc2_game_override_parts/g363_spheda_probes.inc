// G363: Spheda (mini-game) HUD asset-path diagnostics.
//
// Symptom: entering Spheda draws the HUD sprites at the correct screen positions but
// with dungeon-wall texels. Packet A/B (tools/g363_hud_census.py, runner .gs vs
// ref/dumps/spheda.gs) shows hardware binds TEX0 tbp=0x2720 tbw=8 T8 tw=9 th=8
// cbp=0x3fbc for the HUD sprite group while the runner emits NO TEX0 for that group
// at all (it keeps the previous bind, tbw=4/tw=8) -> mgCTextureManager::GetTexture
// ("sphida_bar") returned NULL, i.e. the texture bank was never loaded.
//
// The runner's stderr for that route contains exactly the "dungeon/sphida/" asset set
// opened with NO directory prefix:
//   fioOpen error: fopen failed for 'D:\ps2r\dc2\eng_sphida_bar.img'
//   ... rball.chr / bball.chr / rgool.chr / rgool2.chr / gool_a.mds
//
// These probes answer, with no behavior change, WHY the prefix is missing:
//   [G363:setdir]  every SetCurrentDir__FPc@0x148760 call (arg string + resulting CurrentDir)
// (0x148930 / 0x149370 already carry the F55 / F39 probes -- do not re-register them,
//  the later registration silently discards the earlier one.)
// Gate: DC2_G363_PATHTRACE=1. Default OFF; the probes call the real bodies unchanged.

// Guest globals (from ref/functions asm):
//   CurrentDir     = 0x00334390 (char[])
//   TopDir         = 0x00334290 (char[])
//   DefaultFileDev = gp(0x0037E4F0) - 0x7FD8 = 0x00376518 (int)
static constexpr uint32_t kG363CurrentDir = 0x00334390u;
static constexpr uint32_t kG363TopDir = 0x00334290u;
static constexpr uint32_t kG363DefaultFileDev = 0x00376518u;

static bool g363_trace_enabled()
{
    static const bool on = dc2_env_flag_enabled("DC2_G363_PATHTRACE");
    return on;
}

static std::string g363_guest_str(uint8_t *rdram, uint32_t addr, uint32_t maxLen = 128u)
{
    if (addr == 0u)
        return std::string("<null>");
    std::string s;
    for (uint32_t i = 0u; i < maxLen; ++i)
    {
        const char c = (char)rdram[(addr + i) & 0x1fffffffu];
        if (c == '\0')
            break;
        s.push_back(c);
    }
    return s;
}

static uint32_t g363_guest_u32(uint8_t *rdram, uint32_t addr)
{
    uint32_t v = 0u;
    std::memcpy(&v, rdram + (addr & 0x1fffffffu), sizeof(v));
    return v;
}

static void g363_setcurrentdir_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t argAddr = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    std::string arg;
    if (g363_trace_enabled())
        arg = g363_guest_str(rdram, argAddr);
    SetCurrentDir__FPc_0x148760(rdram, ctx, runtime);
    if (g363_trace_enabled())
    {
        static uint32_t s_n = 0u;
        if (++s_n <= 400u)
            std::fprintf(stderr, "[G363:setdir] n=%u ra=0x%x arg='%s' -> CurrentDir='%s' TopDir='%s'\n",
                         s_n, ra, arg.c_str(),
                         g363_guest_str(rdram, kG363CurrentDir).c_str(),
                         g363_guest_str(rdram, kG363TopDir).c_str());
    }
}

static void g363_register_probes(PS2Runtime &runtime)
{
    runtime.registerFunction(0x00148760u, g363_setcurrentdir_probe); // SetCurrentDir__FPc
}
