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
static uint32_t g_f48_2_log_count   = 0u;
static uint32_t g_f48_3_log_count   = 0u;
static bool dc2_env_flag_enabled(const char *name)
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
