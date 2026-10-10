// G790 (measurement only) — the per-process random stream (plans/phase-G790-ledger.md).
//
// title_init_rand (0x29f050) is `srand(mgGetVSyncCount())`: the seed of the guest's only random
// stream is the number of guest vblanks that have elapsed when the title initialises, which depends
// on how fast this host boots. Every process therefore replays a scripted route with its own random
// stream (monster AI, hit rolls, the battle's outcome, the pose and camera of the death hold the
// Combat and Dungeon1 routes end in).
//
//   DC2_G790_SEED_LOG=1   print `[G790:seed] natural=<vsync count> used=<seed>` at every call
//   DC2_G790_SEED=<n>     seed the stream with n instead of the vblank count (implies the log)
//
// Neither set: nothing is registered and title_init_rand runs as generated.
namespace g790_seed {
constexpr uint32_t kInitRand = 0x0029f050u, kVsyncCount = 0x00141310u, kSrand = 0x00128398u;

static bool pinned(uint32_t &value)
{
    static const char *const s = std::getenv("DC2_G790_SEED");
    if (s == nullptr || s[0] == '\0')
        return false;
    value = static_cast<uint32_t>(std::strtoul(s, nullptr, 0));
    return true;
}

// Replaces the whole body: v0 = mgGetVSyncCount(); srand(v0 or the pinned seed). Both callees are
// leaves that return through $ra, which is left as the caller's, so each returns with pc == ra.
static void initRand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    runtime->lookupFunction(kVsyncCount)(rdram, ctx, runtime);
    const uint32_t natural = getRegU32(ctx, 2);
    uint32_t seed = natural;
    pinned(seed);
    ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(seed)));
    ctx->pc = kSrand;
    runtime->lookupFunction(kSrand)(rdram, ctx, runtime);
    std::fprintf(stderr, "[G790:seed] natural=%u used=%u\n", natural, seed);
    ctx->pc = ra;
}
} // namespace g790_seed

static void g790_seed_register(PS2Runtime &runtime)
{
    uint32_t unused = 0u;
    if (!g790_seed::pinned(unused) && std::getenv("DC2_G790_SEED_LOG") == nullptr)
        return;
    runtime.registerFunction(g790_seed::kInitRand, g790_seed::initRand);
    std::fprintf(stderr, "[G790:seed] armed\n");
}
