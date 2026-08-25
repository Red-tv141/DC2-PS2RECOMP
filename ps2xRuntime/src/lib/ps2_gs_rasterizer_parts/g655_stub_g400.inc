// G655 P17: closed surface-stage sampler. Keep the environment integer helper for a build that
// independently enables G401's composite probe.
static constexpr bool g400StageOn() { return false; }
static uint64_t g400EnvU64(const char *name, uint64_t fallback)
{
    const char *e = std::getenv(name);
    return (e && *e) ? static_cast<uint64_t>(std::strtoull(e, nullptr, 0)) : fallback;
}
template <typename... T> static void g400NoteDrawTarget(T &&...) {}
