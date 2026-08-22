static uint32_t dc2_read_u32(uint8_t *rdram, uint32_t addr);
static uint64_t dc2_read_u64(uint8_t *rdram, uint32_t addr);
static float dc2_read_f32(uint8_t *rdram, uint32_t addr);
static void dc2_write_u32(uint8_t *rdram, uint32_t addr, uint32_t value);
static void dc2_write_u16(uint8_t *rdram, uint32_t addr, uint16_t value);
static inline void f50_set_reg(R5900Context *ctx, int reg, uint32_t value);

static uint32_t g_f40_frame_counter = 0u;
static bool     g_f40_active        = false;
static uint16_t g_f40_pressed       = 0u;
// F66: in-dungeon free-roam input. The live pad path is pad_button_read_stub
// (override of 0x14a3d0), which the title input (g_f40_*) drives ג€” but that clock
// freezes in-dungeon and the stub hardcodes the analog sticks to centre. The
// present-loop driver f66_drive_dungeon_pad sets these so pad_button_read_stub can
// emit the held buttons + a deflected LEFT STICK (free-roam reads the analog stick).
static bool     g_f66_dungeon_active = false;
static uint16_t g_f66_dungeon_buttons= 0u;
static uint8_t  g_f66_dungeon_lx     = 0x80u;
static uint8_t  g_f66_dungeon_ly     = 0x80u;
// G7: live host controller (XInput via raylib). Snapshot published once per host
// present frame by g7_poll_live_pad (main thread); consumed by the guest pad read
// (read_pad_stub / dc2_pad_mask / dc2_write_pad_status) on the EE thread. When
// g_pad_live_connected, the live controller OWNS input (buttons + all four analog
// axes), overriding the scripted F40/F66 paths. Disabled when DC2_PAD_INPUT is set
// explicitly (deterministic tests) or DC2_NO_XINPUT=1. Marker: G7_XINPUT_LIVE.
static bool     g_pad_live_connected = false;
static uint16_t g_pad_live_mask      = 0u;     // active-high scePad bits
static uint8_t  g_pad_live_lx        = 0x80u;
static uint8_t  g_pad_live_ly        = 0x80u;
static uint8_t  g_pad_live_rx        = 0x80u;
static uint8_t  g_pad_live_ry        = 0x80u;
// G49: scripted RIGHT-STICK deflection for HEADLESS validation. The Select-Costume model
// preview rotates with the RIGHT analog stick (the right stick is hardcoded to centre 0x80
// in the headless pad path, so DC2_PAD_INPUT's digital buttons can navigate menus but cannot
// turn the model). Env DC2_RSTICK uses the same "start..end:Dir[+Dir];..." range syntax as
// DC2_PAD_INPUT, with Dir in {RLeft,RRight,RUp,RDown}; driven off the F40 scriptFrame path and
// applied in dc2_write_pad_status when no live pad is connected. Default 0x80 => inert.
static uint8_t  g_f40_rx             = 0x80u;
static uint8_t  g_f40_ry             = 0x80u;
// G374: scripted LEFT-STICK deflection (DC2_LSTICK). replay_from_rec.py only ever emitted
// the digital buttons and the right stick, so a recorded session that WALKED the player
// with the left analog stick replayed as a player who never moved. Env DC2_LSTICK uses the
// same scriptFrame range syntax as DC2_PAD_INPUT but carries the RAW recorded axis bytes:
//   "start..end:0xLX,0xLY;..."  (0x80 = centre). Default inert.
static bool     g_f40_lstick_active  = false;
static uint8_t  g_f40_lx             = 0x80u;
static uint8_t  g_f40_ly             = 0x80u;
static uint32_t g_f48_2_log_count   = 0u;
static uint32_t g_f48_3_log_count   = 0u;
static bool dc2_env_flag_enabled_uncached(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr &&
           value[0] != '\0' &&
           std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

// ===== G639: process-wide memo for dc2_env_flag_enabled (bring-up DEFAULT OFF) =====
//
// PREMISE. G619 cached the SIX flags one hot override read per scene node, which was 18.9% of the
// EE thread on `dungeon1`. It did not touch the other ~150 call sites, and the residual is still
// measurable: the EE PC sampler on `s05` (this phase) reads **`getenv` 2.07% + `memcmp` 1.85%** of
// every sample on that thread. `getenv` is not cheap — the UCRT implementation takes the
// environment lock and linearly scans the environment block, and this harness sets a large one.
//
// EXACTNESS rests on the SAME premise G619 and G268 already ship: the environment is
// process-constant here (nothing in the tree calls `putenv`/`_putenv`/`SetEnvironmentVariable`, and
// every harness sets variables in the parent shell before `Start-Process`). The memo therefore
// honours G619's own control — `DC2_G619_LIVE_ENVREAD=1` disables it, so that arm keeps meaning
// "every read is live".
//
// ⭐ KEYED ON THE NAME POINTER, NOT THE STRING. Every call site passes a distinct string LITERAL, so
// pointer identity is a valid key and a hit costs one compare instead of a lock plus a scan. The one
// caller that passes a variable (`g619EnvFlag`) forwards a literal it received, so its pointer is
// stable too. ⛔ A caller that ever passes a REUSED BUFFER (a `std::string::c_str()`, a stack array)
// would alias two different names onto one slot — grep before adding one.
//
// Slots publish `value` before `name` with a release store and are read with an acquire load, so a
// racing reader that observes the name observes the value. Two threads computing the same slot is
// benign: they compute the same answer from the same immutable environment.
static const bool g_g639EnvMemoOn =
    (std::getenv("DC2_G639_NO_ENVMEMO") == nullptr) &&
    (std::getenv("DC2_G639_ENVMEMO") != nullptr) &&
    (std::getenv("DC2_G619_LIVE_ENVREAD") == nullptr);

struct G639EnvSlot
{
    std::atomic<const char *> name{nullptr};
    bool value = false;
};
static constexpr uint32_t kG639EnvSlots = 256u; // >> the ~150 distinct flag literals in this TU set
static G639EnvSlot g_g639EnvSlots[kG639EnvSlots];

static bool dc2_env_flag_enabled(const char *name)
{
    if (!g_g639EnvMemoOn || name == nullptr)
        return dc2_env_flag_enabled_uncached(name);
    const uintptr_t key = reinterpret_cast<uintptr_t>(name);
    uint32_t idx = static_cast<uint32_t>((key >> 4) ^ (key >> 12)) & (kG639EnvSlots - 1u);
    for (uint32_t probe = 0; probe < kG639EnvSlots; ++probe)
    {
        const char *have = g_g639EnvSlots[idx].name.load(std::memory_order_acquire);
        if (have == name)
            return g_g639EnvSlots[idx].value;
        if (have == nullptr)
        {
            const bool v = dc2_env_flag_enabled_uncached(name);
            g_g639EnvSlots[idx].value = v;
            g_g639EnvSlots[idx].name.store(name, std::memory_order_release);
            return v;
        }
        idx = (idx + 1u) & (kG639EnvSlots - 1u);
    }
    return dc2_env_flag_enabled_uncached(name); // table full: fail OPEN to the live read
}

static bool f33_trace_drawprim_enabled()
{
    static const bool enabled = []() {
        return dc2_env_flag_enabled("DC2_TRACE_DRAWPRIM");
    }();
    return enabled;
}

static bool dc2_phase_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_PHASE_TRACE");
    return enabled;
}

// 60fps patch (perf-arc baseline, NOT default-on): raw PS2 cheat-device code
// "20376C50 00000001" = 32-bit write of 0x00000001 to guest address 0x00376C50.
// Opt-in via DC2_PATCH_60FPS=1; reapplied every guest frame (mgEndFrame tick) the
// same way a real cheat-device/pnach "extended" write survives the game's own
// resets of the value, rather than a one-shot boot write.
static void dc2_apply_60fps_patch(uint8_t *rdram)
{
    static const bool enabled = dc2_env_flag_enabled("DC2_PATCH_60FPS");
    if (!enabled)
        return;
    dc2_write_u32(rdram, 0x00376C50u, 0x00000001u);
}

static uint32_t g154_env_u32(const char *name, uint32_t defValue, uint32_t minValue, uint32_t maxValue)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0')
        return defValue;

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    if (end == value)
        return defValue;

    uint32_t out = static_cast<uint32_t>(parsed);
    if (out < minValue)
        out = minValue;
    if (out > maxValue)
        out = maxValue;
    return out;
}

static std::string g154_trim(std::string value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    return value;
}

struct G154Watch
{
    uint32_t addr = 0u;
    uint32_t size = 4u;
    std::string label;
    uint64_t last = 0ull;
    uint64_t changes = 0ull;
    bool haveLast = false;
};

static uint32_t g154_watch_size(uint32_t raw)
{
    if (raw == 1u || raw == 2u || raw == 4u || raw == 8u)
        return raw;
    return 4u;
}

static std::vector<G154Watch> g154_parse_watches()
{
    std::vector<G154Watch> watches;
    const char *env = std::getenv("DC2_G154_WATCH");
    if (!env || env[0] == '\0')
        return watches;

    const std::string spec(env);
    size_t pos = 0u;
    while (pos < spec.size() && watches.size() < 16u)
    {
        const size_t comma = spec.find(',', pos);
        const size_t end = (comma == std::string::npos) ? spec.size() : comma;
        std::string item = g154_trim(spec.substr(pos, end - pos));
        pos = (comma == std::string::npos) ? spec.size() : comma + 1u;
        if (item.empty())
            continue;

        size_t colon0 = item.find(':');
        size_t colon1 = (colon0 == std::string::npos) ? std::string::npos : item.find(':', colon0 + 1u);
        std::string addrText = g154_trim(item.substr(0u, colon0));
        if (addrText.empty())
            continue;

        char *addrEnd = nullptr;
        const unsigned long addr = std::strtoul(addrText.c_str(), &addrEnd, 0);
        if (addrEnd == addrText.c_str())
            continue;

        G154Watch watch;
        watch.addr = static_cast<uint32_t>(addr);
        if (colon0 != std::string::npos)
        {
            const size_t sizeEnd = (colon1 == std::string::npos) ? item.size() : colon1;
            std::string sizeText = g154_trim(item.substr(colon0 + 1u, sizeEnd - colon0 - 1u));
            if (!sizeText.empty())
            {
                char *sizeParseEnd = nullptr;
                const unsigned long parsedSize = std::strtoul(sizeText.c_str(), &sizeParseEnd, 0);
                if (sizeParseEnd != sizeText.c_str())
                    watch.size = g154_watch_size(static_cast<uint32_t>(parsedSize));
            }
        }
        if (colon1 != std::string::npos)
            watch.label = g154_trim(item.substr(colon1 + 1u));
        watches.push_back(watch);
    }
    return watches;
}

static uint64_t g154_read_le(uint8_t *rdram, uint32_t addr, uint32_t size)
{
    uint64_t value = 0ull;
    for (uint32_t i = 0u; i < size; ++i)
    {
        const uint8_t *src = getConstMemPtr(rdram, addr + i);
        if (src)
            value |= static_cast<uint64_t>(*src) << (i * 8u);
    }
    return value;
}

static bool g154_should_log_watch(uint64_t changes)
{
    if (dc2_env_flag_enabled("DC2_G154_WATCH_VERBOSE"))
        return true;
    return changes <= 64ull || (changes & (changes - 1ull)) == 0ull;
}

static void g154_observe_frame(uint8_t *rdram,
                               uint32_t n,
                               uint32_t scriptFrame,
                               uint32_t loopNo,
                               uint32_t nextLoopNo)
{
    static const bool s_obs = dc2_env_flag_enabled("DC2_G154_OBS");
    static const uint32_t s_every = g154_env_u32("DC2_G154_EVERY", 60u, 1u, 3600u);
    static std::vector<G154Watch> s_watches = g154_parse_watches();
    static bool s_reportedInit = false;

    if (!s_obs && s_watches.empty())
        return;

    if (!s_reportedInit)
    {
        const char *tilebin = std::getenv("DC2_G144_TILEBIN");
        const bool tilebinDisabled =
            dc2_env_flag_enabled("DC2_G144_NO_TILEBIN") ||
            (tilebin && (tilebin[0] == '\0' ||
                         std::strcmp(tilebin, "0") == 0 ||
                         std::strcmp(tilebin, "false") == 0 ||
                         std::strcmp(tilebin, "FALSE") == 0 ||
                         std::strcmp(tilebin, "off") == 0 ||
                         std::strcmp(tilebin, "OFF") == 0));
        std::fprintf(stderr,
                     "[G154:init] obs=%u every=%u watches=%u mtgs=%u tilebin=%s\n",
                     s_obs ? 1u : 0u,
                     s_every,
                     static_cast<unsigned>(s_watches.size()),
                     g150_mtgs_enabled() ? 1u : 0u,
                     tilebinDisabled ? "off" : ((tilebin && tilebin[0] != '\0') ? tilebin : "8(default)"));
        s_reportedInit = true;
    }

    static bool s_havePerfBase = false;
    static std::chrono::steady_clock::time_point s_windowStart{};
    static uint32_t s_windowFrames = 0u;
    static uint64_t s_lastG144MidNs = 0ull;
    static uint64_t s_lastG144UploadNs = 0ull;
    static uint64_t s_lastG144FrameNs = 0ull;
    static uint64_t s_lastG144MidCount = 0ull;
    static uint64_t s_lastG144UploadCount = 0ull;
    static uint64_t s_lastG144FrameCount = 0ull;

    const auto now = std::chrono::steady_clock::now();
    if (!s_havePerfBase)
    {
        s_windowStart = now;
        s_lastG144MidNs = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
        s_lastG144UploadNs = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
        s_lastG144FrameNs = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
        s_lastG144MidCount = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
        s_lastG144UploadCount = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
        s_lastG144FrameCount = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
        s_havePerfBase = true;
    }
    else if (s_obs)
    {
        ++s_windowFrames;
        if (s_windowFrames >= s_every)
        {
            const double elapsedMs = std::chrono::duration<double, std::milli>(now - s_windowStart).count();
            const double avgFrameMs = elapsedMs / static_cast<double>(s_windowFrames);
            const double fps = 1000.0 / std::max(0.001, avgFrameMs);
            const int32_t dngStatus = static_cast<int32_t>(dc2_read_u32(rdram, 0x01E9F6E0u));
            const uint64_t midNs = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
            const uint64_t uploadNs = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
            const uint64_t frameNs = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
            const uint64_t midCount = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
            const uint64_t uploadCount = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
            const uint64_t frameCount = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
            const double midMs = static_cast<double>(midNs - s_lastG144MidNs) / 1.0e6;
            const double uploadMs = static_cast<double>(uploadNs - s_lastG144UploadNs) / 1.0e6;
            const double frameMs = static_cast<double>(frameNs - s_lastG144FrameNs) / 1.0e6;
            const double w = static_cast<double>(s_windowFrames);

            std::fprintf(stderr,
                         "[G154:perf] n=%u frame=%u window=%u avgFrameMs=%.2f fps=%.2f loop=%u->%u DngStatus=%d mtgs=%u g144(mid=%.2fms/%.2fx upload=%.2fms/%.2fx frame=%.2fms/%.2fx) watches=%u\n",
                         n,
                         scriptFrame,
                         s_windowFrames,
                         avgFrameMs,
                         fps,
                         loopNo,
                         nextLoopNo,
                         dngStatus,
                         g150_mtgs_enabled() ? 1u : 0u,
                         midMs / w,
                         static_cast<double>(midCount - s_lastG144MidCount) / w,
                         uploadMs / w,
                         static_cast<double>(uploadCount - s_lastG144UploadCount) / w,
                         frameMs / w,
                         static_cast<double>(frameCount - s_lastG144FrameCount) / w,
                         static_cast<unsigned>(s_watches.size()));

            // G434: per-window workload invariant for the drawPrimitive deletion probe. Same
            // window and denominator as [G154:perf], so kicks/f is directly comparable arm to arm.
            {
                static const bool s_g434Inv = (std::getenv("DC2_G434_INV") != nullptr);
                if (s_g434Inv)
                {
                    static uint64_t s_lastKicks = 0ull, s_lastDrawKicks = 0ull, s_lastL2l = 0ull;
                    const uint64_t kicks = ::g_g434Kicks.load(std::memory_order_relaxed);
                    const uint64_t draws = ::g_g434DrawKicks.load(std::memory_order_relaxed);
                    const uint64_t l2l = ::g_g434L2lEdges.load(std::memory_order_relaxed);
                    std::fprintf(stderr,
                                 "[G434:inv] kicks/f=%.1f drawKicks/f=%.1f l2lEdges/f=%.1f window=%u\n",
                                 static_cast<double>(kicks - s_lastKicks) / w,
                                 static_cast<double>(draws - s_lastDrawKicks) / w,
                                 static_cast<double>(l2l - s_lastL2l) / w,
                                 s_windowFrames);
                    s_lastKicks = kicks;
                    s_lastDrawKicks = draws;
                    s_lastL2l = l2l;
                }
            }

            s_windowStart = now;
            s_windowFrames = 0u;
            s_lastG144MidNs = midNs;
            s_lastG144UploadNs = uploadNs;
            s_lastG144FrameNs = frameNs;
            s_lastG144MidCount = midCount;
            s_lastG144UploadCount = uploadCount;
            s_lastG144FrameCount = frameCount;
        }
    }

    for (G154Watch &watch : s_watches)
    {
        const uint64_t value = g154_read_le(rdram, watch.addr, watch.size);
        const char *label = watch.label.empty() ? "-" : watch.label.c_str();
        if (!watch.haveLast)
        {
            std::fprintf(stderr,
                         "[G154:watch] n=%u frame=%u loop=%u->%u addr=0x%08x size=%u label=%s init=0x%llx\n",
                         n,
                         scriptFrame,
                         loopNo,
                         nextLoopNo,
                         watch.addr,
                         watch.size,
                         label,
                         static_cast<unsigned long long>(value));
            watch.haveLast = true;
            watch.last = value;
        }
        else if (value != watch.last)
        {
            ++watch.changes;
            if (g154_should_log_watch(watch.changes))
            {
                std::fprintf(stderr,
                             "[G154:watch] n=%u frame=%u loop=%u->%u addr=0x%08x size=%u label=%s old=0x%llx new=0x%llx changes=%llu\n",
                             n,
                             scriptFrame,
                             loopNo,
                             nextLoopNo,
                             watch.addr,
                             watch.size,
                             label,
                             static_cast<unsigned long long>(watch.last),
                             static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(watch.changes));
            }
            watch.last = value;
        }
    }
}

// G570: low-overhead attribution for the isolated jump/dash spike class. This runs only at the
// once-per-frame mgEndFrame hook and snapshots counters that already exist for the light census.
// It emits the spike plus its two predecessors so the depth-two frame pipeline cannot hide a
// producer one frame earlier. Normal frames stay quiet; periodic summaries retain the base rate.
struct G570Counters
{
    uint64_t eeCpuNs = 0ull;
    uint64_t vuBusyNs = 0ull;
    uint64_t vuKicks = 0ull;
    uint64_t gsWindowNs = 0ull;
    uint64_t gsTotalNs = 0ull;
    uint64_t collectNs = 0ull;
    uint64_t collectStallNs = 0ull;
    uint64_t collectCalls = 0ull;
    uint64_t vifNs = 0ull;
    uint64_t gifNs = 0ull;
    uint64_t imageNs = 0ull;
    uint64_t localNs = 0ull;
    uint64_t uploadNs = 0ull;
    uint64_t uploadCount = 0ull;
    uint64_t packetNs = 0ull;
    uint64_t packetCount = 0ull;
    uint64_t drawNs = 0ull;
    uint64_t drawCount = 0ull;
    uint64_t imageBytes = 0ull;
    uint64_t backendNs[4]{};
    uint64_t backendCount[4]{};
};

struct G570FrameSample
{
    uint32_t n = 0u;
    uint32_t scriptFrame = 0u;
    uint32_t loopNo = 0u;
    int32_t dngStatus = 0;
    double frameMs = 0.0;
    G570Counters delta{};
};

struct G570Accum
{
    uint64_t frames = 0ull;
    double frameMs = 0.0;
    G570Counters sum{};
};

static G570Counters g570_capture_counters()
{
    G570Counters out;
    out.eeCpuNs = g182ThreadCpuNs();
    out.vuBusyNs = g297WorkerBusyNs();
    out.vuKicks = g297WorkerKicksRun();
    out.gsWindowNs = g303_gs_worker_busy_ns();
    out.gsTotalNs = g332_gs_worker_total_ns();
    out.collectNs = g297GsCollectTotalNs();
    out.collectStallNs = g297GsCollectStallNs();
    out.collectCalls = g297GsCollectCalls();
    out.vifNs = g_g146Vif1Ns.load(std::memory_order_relaxed);
    out.gifNs = g_g146GifSubmitNs.load(std::memory_order_relaxed);
    out.imageNs = g_g146GsImageNs.load(std::memory_order_relaxed);
    out.localNs = g_g146GsLocalNs.load(std::memory_order_relaxed);
    out.uploadNs = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
    out.uploadCount = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
    out.packetNs = g_g147GsGifPacketNs.load(std::memory_order_relaxed);
    out.packetCount = g_g147GsGifPacketCount.load(std::memory_order_relaxed);
    out.drawNs = g_g147DrawPrimitiveNs.load(std::memory_order_relaxed);
    out.drawCount = g_g147DrawPrimitiveCount.load(std::memory_order_relaxed);
    out.imageBytes = g_g147ImageBytes.load(std::memory_order_relaxed);
    g332_backend_snapshot(out.backendNs, out.backendCount);
    return out;
}

static uint64_t g570_counter_delta(uint64_t now, uint64_t before)
{
    return now >= before ? now - before : 0ull;
}

static G570Counters g570_counter_delta(const G570Counters &now, const G570Counters &before)
{
    G570Counters out;
    out.eeCpuNs = g570_counter_delta(now.eeCpuNs, before.eeCpuNs);
    out.vuBusyNs = g570_counter_delta(now.vuBusyNs, before.vuBusyNs);
    out.vuKicks = g570_counter_delta(now.vuKicks, before.vuKicks);
    out.gsWindowNs = g570_counter_delta(now.gsWindowNs, before.gsWindowNs);
    out.gsTotalNs = g570_counter_delta(now.gsTotalNs, before.gsTotalNs);
    out.collectNs = g570_counter_delta(now.collectNs, before.collectNs);
    out.collectStallNs = g570_counter_delta(now.collectStallNs, before.collectStallNs);
    out.collectCalls = g570_counter_delta(now.collectCalls, before.collectCalls);
    out.vifNs = g570_counter_delta(now.vifNs, before.vifNs);
    out.gifNs = g570_counter_delta(now.gifNs, before.gifNs);
    out.imageNs = g570_counter_delta(now.imageNs, before.imageNs);
    out.localNs = g570_counter_delta(now.localNs, before.localNs);
    out.uploadNs = g570_counter_delta(now.uploadNs, before.uploadNs);
    out.uploadCount = g570_counter_delta(now.uploadCount, before.uploadCount);
    out.packetNs = g570_counter_delta(now.packetNs, before.packetNs);
    out.packetCount = g570_counter_delta(now.packetCount, before.packetCount);
    out.drawNs = g570_counter_delta(now.drawNs, before.drawNs);
    out.drawCount = g570_counter_delta(now.drawCount, before.drawCount);
    out.imageBytes = g570_counter_delta(now.imageBytes, before.imageBytes);
    for (int i = 0; i < 4; ++i)
    {
        out.backendNs[i] = g570_counter_delta(now.backendNs[i], before.backendNs[i]);
        out.backendCount[i] = g570_counter_delta(now.backendCount[i], before.backendCount[i]);
    }
    return out;
}

static void g570_accumulate(G570Accum &acc, const G570FrameSample &sample)
{
    ++acc.frames;
    acc.frameMs += sample.frameMs;
    const G570Counters &in = sample.delta;
    G570Counters &out = acc.sum;
    out.eeCpuNs += in.eeCpuNs;
    out.vuBusyNs += in.vuBusyNs;
    out.vuKicks += in.vuKicks;
    out.gsWindowNs += in.gsWindowNs;
    out.gsTotalNs += in.gsTotalNs;
    out.collectNs += in.collectNs;
    out.collectStallNs += in.collectStallNs;
    out.collectCalls += in.collectCalls;
    out.vifNs += in.vifNs;
    out.gifNs += in.gifNs;
    out.imageNs += in.imageNs;
    out.localNs += in.localNs;
    out.uploadNs += in.uploadNs;
    out.uploadCount += in.uploadCount;
    out.packetNs += in.packetNs;
    out.packetCount += in.packetCount;
    out.drawNs += in.drawNs;
    out.drawCount += in.drawCount;
    out.imageBytes += in.imageBytes;
    for (int i = 0; i < 4; ++i)
    {
        out.backendNs[i] += in.backendNs[i];
        out.backendCount[i] += in.backendCount[i];
    }
}

static double g570_ms_per_frame(uint64_t ns, uint64_t frames)
{
    return frames ? static_cast<double>(ns) / 1.0e6 / static_cast<double>(frames) : 0.0;
}

static double g570_count_per_frame(uint64_t count, uint64_t frames)
{
    return frames ? static_cast<double>(count) / static_cast<double>(frames) : 0.0;
}

static void g570_print_sample(uint32_t spikeN, int rel, const G570FrameSample &sample)
{
    const G570Counters &d = sample.delta;
    const uint64_t backendNs = d.backendNs[0] + d.backendNs[1] + d.backendNs[2] + d.backendNs[3];
    const uint64_t frontNs = d.gsTotalNs > backendNs ? d.gsTotalNs - backendNs : 0ull;
    const uint64_t gsOtherNs = d.gsTotalNs > d.gsWindowNs ? d.gsTotalNs - d.gsWindowNs : 0ull;
    std::fprintf(stderr,
                 "[G570:frame] spike=%u rel=%d n=%u script=%u ms=%.2f loop=%u dng=%d "
                 "| eeCpu=%.2f vu=%.2f/%llu gs=%.2f(win=%.2f other=%.2f front=%.2f) "
                 "collect=%.2f stall=%.2f calls=%llu "
                 "| be(render=%.2f/%llu rb=%.2f/%llu copy=%.2f/%llu other=%.2f/%llu) "
                 "| gif(packet=%.2f/%llu draw=%.2f/%llu imageKB=%.1f) "
                 "upload=%.2f/%llu vif=%.2f submit=%.2f image=%.2f local=%.2f\n",
                 spikeN, rel, sample.n, sample.scriptFrame, sample.frameMs,
                 sample.loopNo, sample.dngStatus,
                 d.eeCpuNs / 1.0e6, d.vuBusyNs / 1.0e6,
                 static_cast<unsigned long long>(d.vuKicks),
                 d.gsTotalNs / 1.0e6, d.gsWindowNs / 1.0e6, gsOtherNs / 1.0e6,
                 frontNs / 1.0e6, d.collectNs / 1.0e6, d.collectStallNs / 1.0e6,
                 static_cast<unsigned long long>(d.collectCalls),
                 d.backendNs[0] / 1.0e6,
                 static_cast<unsigned long long>(d.backendCount[0]),
                 d.backendNs[1] / 1.0e6,
                 static_cast<unsigned long long>(d.backendCount[1]),
                 d.backendNs[2] / 1.0e6,
                 static_cast<unsigned long long>(d.backendCount[2]),
                 d.backendNs[3] / 1.0e6,
                 static_cast<unsigned long long>(d.backendCount[3]),
                 d.packetNs / 1.0e6,
                 static_cast<unsigned long long>(d.packetCount),
                 d.drawNs / 1.0e6,
                 static_cast<unsigned long long>(d.drawCount),
                 static_cast<double>(d.imageBytes) / 1024.0,
                 d.uploadNs / 1.0e6,
                 static_cast<unsigned long long>(d.uploadCount),
                 d.vifNs / 1.0e6, d.gifNs / 1.0e6, d.imageNs / 1.0e6,
                 d.localNs / 1.0e6);
}

static void g570_print_summary(uint32_t n, double thresholdMs,
                               const G570Accum &spike, const G570Accum &normal)
{
    const uint64_t spikeBackend = spike.sum.backendNs[0] + spike.sum.backendNs[1] +
                                  spike.sum.backendNs[2] + spike.sum.backendNs[3];
    const uint64_t normalBackend = normal.sum.backendNs[0] + normal.sum.backendNs[1] +
                                   normal.sum.backendNs[2] + normal.sum.backendNs[3];
    const uint64_t spikeFront = spike.sum.gsTotalNs > spikeBackend
                                    ? spike.sum.gsTotalNs - spikeBackend : 0ull;
    const uint64_t normalFront = normal.sum.gsTotalNs > normalBackend
                                     ? normal.sum.gsTotalNs - normalBackend : 0ull;
    std::fprintf(stderr,
                 "[G570:summary] n=%u threshold=%.1f spike=%llu normal=%llu "
                 "| frame=%.2f/%.2f ee=%.2f/%.2f vu=%.2f/%.2f gs=%.2f/%.2f "
                 "front=%.2f/%.2f rb=%.2f/%.2f packet=%.2f/%.2f draw=%.2f/%.2f "
                 "upload=%.2f/%.2f calls=%.1f/%.1f\n",
                 n, thresholdMs,
                 static_cast<unsigned long long>(spike.frames),
                 static_cast<unsigned long long>(normal.frames),
                 spike.frames ? spike.frameMs / static_cast<double>(spike.frames) : 0.0,
                 normal.frames ? normal.frameMs / static_cast<double>(normal.frames) : 0.0,
                 g570_ms_per_frame(spike.sum.eeCpuNs, spike.frames),
                 g570_ms_per_frame(normal.sum.eeCpuNs, normal.frames),
                 g570_ms_per_frame(spike.sum.vuBusyNs, spike.frames),
                 g570_ms_per_frame(normal.sum.vuBusyNs, normal.frames),
                 g570_ms_per_frame(spike.sum.gsTotalNs, spike.frames),
                 g570_ms_per_frame(normal.sum.gsTotalNs, normal.frames),
                 g570_ms_per_frame(spikeFront, spike.frames),
                 g570_ms_per_frame(normalFront, normal.frames),
                 g570_ms_per_frame(spike.sum.backendNs[1], spike.frames),
                 g570_ms_per_frame(normal.sum.backendNs[1], normal.frames),
                 g570_ms_per_frame(spike.sum.packetNs, spike.frames),
                 g570_ms_per_frame(normal.sum.packetNs, normal.frames),
                 g570_ms_per_frame(spike.sum.drawNs, spike.frames),
                 g570_ms_per_frame(normal.sum.drawNs, normal.frames),
                 g570_ms_per_frame(spike.sum.uploadNs, spike.frames),
                 g570_ms_per_frame(normal.sum.uploadNs, normal.frames),
                 g570_count_per_frame(spike.sum.collectCalls, spike.frames),
                 g570_count_per_frame(normal.sum.collectCalls, normal.frames));
}

static void g570_observe_frame(uint8_t *rdram, uint32_t n, uint32_t scriptFrame, uint32_t loopNo)
{
    static const bool s_on = dc2_env_flag_enabled("DC2_G570_FRAME");
    if (!s_on)
        return;

    static const double s_thresholdMs = []() {
        const char *value = std::getenv("DC2_G570_SPIKE_MS");
        const double parsed = value ? std::strtod(value, nullptr) : 90.0;
        return std::max(16.0, std::min(2000.0, parsed));
    }();
    static const uint32_t s_summaryEvery =
        g154_env_u32("DC2_G570_SUMMARY_EVERY", 300u, 30u, 3600u);
    static bool s_haveLast = false;
    static auto s_lastTs = std::chrono::steady_clock::time_point{};
    static G570Counters s_lastCounters{};
    static G570FrameSample s_ring[3]{};
    static uint32_t s_ringWrite = 0u;
    static uint32_t s_ringCount = 0u;
    static G570Accum s_spike{};
    static G570Accum s_normal{};
    static uint32_t s_summaryFrames = 0u;

    const auto now = std::chrono::steady_clock::now();
    const G570Counters counters = g570_capture_counters();
    if (!s_haveLast)
    {
        s_haveLast = true;
        s_lastTs = now;
        s_lastCounters = counters;
        std::fprintf(stderr,
                     "[G570:init] threshold=%.1f summaryEvery=%u g332=%u g146=%u g147=%u\n",
                     s_thresholdMs, s_summaryEvery,
                     std::getenv("DC2_G332_CENSUS") ? 1u : 0u,
                     std::getenv("DC2_G146_PERF") ? 1u : 0u,
                     std::getenv("DC2_G147_PERF") ? 1u : 0u);
        return;
    }

    G570FrameSample sample;
    sample.n = n;
    sample.scriptFrame = scriptFrame;
    sample.loopNo = loopNo;
    sample.dngStatus = static_cast<int32_t>(dc2_read_u32(rdram, 0x01E9F6E0u));
    sample.frameMs = std::chrono::duration<double, std::milli>(now - s_lastTs).count();
    sample.delta = g570_counter_delta(counters, s_lastCounters);
    s_lastTs = now;
    s_lastCounters = counters;

    s_ring[s_ringWrite] = sample;
    s_ringWrite = (s_ringWrite + 1u) % 3u;
    s_ringCount = std::min<uint32_t>(3u, s_ringCount + 1u);

    const bool spike = sample.frameMs >= s_thresholdMs;
    g570_accumulate(spike ? s_spike : s_normal, sample);
    ++s_summaryFrames;
    if (spike)
    {
        for (uint32_t i = 0u; i < s_ringCount; ++i)
        {
            const uint32_t index = (s_ringWrite + 3u - s_ringCount + i) % 3u;
            const int rel = static_cast<int>(i) - static_cast<int>(s_ringCount) + 1;
            g570_print_sample(sample.n, rel, s_ring[index]);
        }
    }
    if (s_summaryFrames >= s_summaryEvery)
    {
        g570_print_summary(n, s_thresholdMs, s_spike, s_normal);
        s_spike = {};
        s_normal = {};
        s_summaryFrames = 0u;
    }
}

static bool f39_trace_map_entry_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MAP_ENTRY");
    return enabled;
}

static bool f50_6_debug_menu_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_DEBUG_MENU");
    return enabled;
}

static bool f50_7_trace_selected_map_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_7");
    return enabled;
}

static bool f50_9_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_9");
    return enabled;
}

static bool f50_10_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_10");
    return enabled;
}

static bool f50_11_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_11");
    return enabled;
}

static bool f51_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_T8_UPLOAD");
    return enabled;
}

static bool f51_should_log()
{
    static uint32_t s_logs = 0u;
    if (s_logs >= 96u)
        return false;
    ++s_logs;
    return true;
}

static bool g59_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G59");
    return enabled;
}

static bool f48_trace_title_camera_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_TITLE_CAMERA");
    return enabled;
}

static bool f49_trace_waitvsync_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_WAITVSYNC");
    return enabled;
}

static bool f49_trace_vsync_callback_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_VSYNC_CB");
    return enabled;
}

static bool f49_trace_syncv_set_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_SYNCVCB_SET");
    return enabled;
}

static bool f49_trace_syncv_call_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_SYNCV_CALL");
    return enabled;
}

static bool f49_trace_loop_timing_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_LOOP_TIMING");
    return enabled;
}

static bool f49_trace_mainloop_hotspots_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MAINLOOP_HOT");
    return enabled;
}

static bool f49_trace_title_path_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_TITLE_PATH");
    return enabled;
}

static bool f47_trace_menu_state_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MENU_STATE");
    return enabled;
}

static bool f48_2_trace_charbg_enabled()
{
    static const bool enabled =
        dc2_env_flag_enabled("DC2_TRACE_F48_2") ||
        dc2_env_flag_enabled("DC2_TRACE_MENU_STATE");
    return enabled;
}

static bool f48_3_trace_costume_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F48_3");
    return enabled;
}

static bool f49_remap_title_ret5_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_F49_REMAP_RET5");
    return enabled;
}

static int32_t f49_waitvsync_env_max_count()
{
    static const int32_t parsed = []() -> int32_t
    {
        const char *value = std::getenv("DC2_WAITVSYNC_MAX_COUNT");
        if (value == nullptr || *value == '\0')
            return -1;

        char *end = nullptr;
        const long v = std::strtol(value, &end, 10);
        if (end == value || (end && *end != '\0') || v < 0L || v > 1024L)
            return -1;
        return static_cast<int32_t>(v);
    }();
    return parsed;
}

static bool f29_should_log(uint32_t n, uint32_t step = 60u)
{
    if (!dc2_phase_trace_enabled())
        return false;
    return n <= 12u || (step != 0u && (n % step) == 0u);
}
