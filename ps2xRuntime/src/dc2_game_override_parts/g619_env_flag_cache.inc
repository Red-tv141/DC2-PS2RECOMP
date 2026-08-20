// ===== G619: process-constant env-flag cache for the EE-side override probes =====
//
// WHY THIS EXISTS. The EE thread is the pole on `dungeon1` post-G618 (`EE cpu 30.61` against
// `GS own 26.71` in the spike window), and the G503 PC sampler pointed at it read
// **`getenv` = 20.7% of every sample on that thread** — the largest single item, larger than the
// whole VIF1 parser. The caller attribution ([G619:eecaller], resolved against the linker /MAP)
// named one function: `g78_frame_clipinbox_fix`, the override on `mgClipInBoxW@0x12F380`, at
// **18.9% of the thread**. That override runs once per scene node per frame and executed SIX
// uncached `std::getenv` calls on every invocation — `DC2_G88_VP`, `DC2_G92_MTX`,
// `DC2_G92_FORCEMTX`, `DC2_G90_TEXMGR`, `DC2_G81_CAM`, `DC2_G82_LIGHT`. Every one of them is a
// DEFAULT-OFF diagnostic left behind by the G81–G92 title-render investigation, which measured
// the title screen and never saw the per-node cost in a dungeon.
//
// `getenv` is not cheap: the UCRT implementation takes the environment lock and walks the
// environment block comparing names, so each call is a lock plus a linear scan — and DC2's
// harness sets a large environment.
//
// EXACTNESS. Caching is exact because the environment is process-constant here: nothing in the
// tree calls `putenv`/`_putenv`/`SetEnvironmentVariable`, and every harness sets the variables in
// the parent shell before `Start-Process`. This is the same premise G268 already ships for the
// VIF1/VU1 kick-mode reads, and it carries the same control:
//
//   DC2_G619_LIVE_ENVREAD=1  -> restore the historical per-call `getenv` (the A/B control arm).
//
// These are NAMESPACE-scope statics, deliberately, not function-local ones: a function-local
// `static const bool` is a per-access thread-safe-static guard on MSVC (an `_Init_global_epoch`
// TLS read plus a compare on EVERY call), which is the very tax this file exists to remove.
// Dynamic init at load time has no guard — the read is a plain load of a global.

static const bool g_g619LiveEnvRead = (std::getenv("DC2_G619_LIVE_ENVREAD") != nullptr);

// The cached values. One `getenv` each, at process load.
static const bool g_g619G88Vp = (std::getenv("DC2_G88_VP") != nullptr);
static const bool g_g619G92Mtx = (std::getenv("DC2_G92_MTX") != nullptr);
static const bool g_g619G92ForceMtx = (std::getenv("DC2_G92_FORCEMTX") != nullptr);
static const bool g_g619G90TexMgr = (std::getenv("DC2_G90_TEXMGR") != nullptr);
static const bool g_g619G90Reload = (std::getenv("DC2_G90_RELOAD") != nullptr);
static const bool g_g619G81Cam = (std::getenv("DC2_G81_CAM") != nullptr);
static const bool g_g619G82Light = (std::getenv("DC2_G82_LIGHT") != nullptr);
static const bool g_g619G120Sel = (std::getenv("DC2_G120_SEL") != nullptr);
static const bool g_g619G115Sel = (std::getenv("DC2_G115_SEL") != nullptr);
static const bool g_g619G199Qw30 = (std::getenv("DC2_G199_QW30") != nullptr);
static const bool g_g619G93Lw = (std::getenv("DC2_G93_LW") != nullptr);
static const bool g_g619G91Flush = (std::getenv("DC2_G91_FLUSH") != nullptr);
static const bool g_g619G195BlockTrace = dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE");

// Read a cached flag, honouring the live-read control. `name` is only touched on the control arm,
// so the shipped path is a load and a test.
static inline bool g619EnvSet(const bool cached, const char *name)
{
    return g_g619LiveEnvRead ? (std::getenv(name) != nullptr) : cached;
}

// Same, for the `dc2_env_flag_enabled` spelling (which additionally rejects 0/false/off).
static inline bool g619EnvFlag(const bool cached, const char *name)
{
    return g_g619LiveEnvRead ? dc2_env_flag_enabled(name) : cached;
}
