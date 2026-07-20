static bool dc2HudEnabled()
{
    static const bool enabled = runtimeEnvFlagEnabled("DC2_HUD");
    return enabled;
}

static bool dc2FrameDumpEnabled()
{
    static const bool enabled = runtimeEnvFlagEnabled("DC2_FRAME_DUMP");
    return enabled;
}

struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    // F58: top-of-RAM slice reserved for the EE main stack (sp init = RAM-0x10) and the
    // async callback stacks (vsync/alarm/interrupt, 0x4000 each). The guest C heap sits
    // BELOW this. The old 0x01F00000 hard cap sat *below* DC2's _end (0x1F64E00), so the
    // guest's SetupHeap(base=_end, size=all-remaining) request collapsed to an empty heap
    // and every malloc/operator-new failed (-> std::bad_alloc abort during the floor load).
    constexpr uint32_t kGuestHeapStackReserve = 0x00040000u; // 256 KB
    constexpr uint32_t kGuestHeapHardLimit = PS2_RAM_SIZE - kGuestHeapStackReserve; // 0x01FC0000

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct HostFrameProbePoint
    {
        uint32_t x;
        uint32_t y;
    };

    constexpr HostFrameProbePoint kGhostProbePoints[] = {
        {220u, 176u},
        {260u, 208u},
        {320u, 208u},
        {260u, 240u},
        {320u, 240u},
        {260u, 272u},
        {320u, 272u},
    };

    uint32_t sampleHostFramePixel(const std::vector<uint8_t> &pixels,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t x,
                                  uint32_t y)
    {
        if (x >= width || y >= height)
        {
            return 0u;
        }

        const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
        if (offset + 4u > pixels.size())
        {
            return 0u;
        }

        return static_cast<uint32_t>(pixels[offset + 0u]) |
               (static_cast<uint32_t>(pixels[offset + 1u]) << 8) |
               (static_cast<uint32_t>(pixels[offset + 2u]) << 16) |
               (static_cast<uint32_t>(pixels[offset + 3u]) << 24);
    }

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    thread_local std::unordered_map<PS2Runtime *, uint32_t> g_guestExecutionDepths;

    // G58: fine-grained watch on TitleMapDraw's saved-$ra slot (0x1fff800). The title loop
    // dies after one frame because that slot is clobbered (0x2a1b88 -> 0x30003 -> 0x9f84a0)
    // by a callee whose frame overlaps it; the epilogue ld $ra then jr-jumps into data.
    // lookupFunction() runs before every dispatched call (nested included), so polling there
    // attributes each change to the function that just ran. Gated DC2_TRACE_G58; quiet by
    // default (g_g58Rdram stays null until the dispatch loop sets it).
    uint8_t *g_g58Rdram = nullptr;
    thread_local uint32_t g_g58LastSlot = 0xCAFEBABEu;
    thread_local uint32_t g_g58PrevPc = 0u;

    std::string formatDispatchHistory();

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }

        // F50.2: env-gated hang locator. When DC2_TRACE_HANG is set, periodically
        // dump the dispatch ring so a no-crash spin loop reveals its repeating cycle.
        static const bool s_hangTrace = (std::getenv("DC2_TRACE_HANG") != nullptr);
        if (s_hangTrace)
        {
            static uint64_t s_calls = 0u;
            static uint32_t s_dumps = 0u;
            if ((++s_calls % 2000000ull) == 0ull && s_dumps < 40u)
            {
                ++s_dumps;
                std::cerr << "[F50.2:hang] calls=" << std::dec << s_calls
                          << " ring=" << formatDispatchHistory() << std::endl;
            }
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectDispatchRecoveryPc(const PS2Runtime *runtime)
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return 0u;
        }

        uint32_t firstHigh = 0u;
        for (uint32_t step = 1u; step <= count; ++step)
        {
            const uint32_t idx = (h.next + h.pcs.size() - step) % static_cast<uint32_t>(h.pcs.size());
            const uint32_t pc = h.pcs[idx];
            if (pc < 0x00100000u)
            {
                continue;
            }
            if (runtime && !runtime->hasFunction(pc))
            {
                continue;
            }

            if (firstHigh == 0u)
            {
                firstHigh = pc;
                continue;
            }

            return pc;
        }

        return firstHigh;
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    uint32_t readGuestU32Wrapped(const uint8_t *rdram, uint32_t addr)
    {
        if (!rdram)
        {
            return 0;
        }

        uint32_t value = 0;
        value |= static_cast<uint32_t>(rdram[(addr + 0u) & PS2_RAM_MASK]) << 0;
        value |= static_cast<uint32_t>(rdram[(addr + 1u) & PS2_RAM_MASK]) << 8;
        value |= static_cast<uint32_t>(rdram[(addr + 2u) & PS2_RAM_MASK]) << 16;
        value |= static_cast<uint32_t>(rdram[(addr + 3u) & PS2_RAM_MASK]) << 24;
        return value;
    }

    uint64_t readGuestU64Wrapped(const uint8_t *rdram, uint32_t addr)
    {
        const uint64_t lo = readGuestU32Wrapped(rdram, addr);
        const uint64_t hi = readGuestU32Wrapped(rdram, addr + 4u);
        return lo | (hi << 32);
    }

    uint32_t selectStackRecoveryPc(const uint8_t *rdram, const R5900Context *ctx, const PS2Runtime *runtime)
    {
        if (!rdram || !ctx || !runtime)
        {
            return 0u;
        }

        const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
        constexpr uint32_t kScanBytes = 0x200u;

        for (uint32_t offset = 0u; offset < kScanBytes; offset += 8u)
        {
            const uint32_t slotAddr = sp + offset;
            const uint32_t ra32 = static_cast<uint32_t>(readGuestU64Wrapped(rdram, slotAddr));
            if (ra32 < 0x00100000u)
            {
                continue;
            }
            if (!runtime->hasFunction(ra32))
            {
                continue;
            }
            return ra32;
        }

        for (uint32_t offset = 0u; offset < kScanBytes; offset += 4u)
        {
            const uint32_t slotAddr = sp + offset;
            const uint32_t ra32 = readGuestU32Wrapped(rdram, slotAddr);
            if (ra32 < 0x00100000u)
            {
                continue;
            }
            if (!runtime->hasFunction(ra32))
            {
                continue;
            }
            return ra32;
        }

        return 0u;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

PS2Runtime::GuestExecutionScope::GuestExecutionScope(PS2Runtime *runtime) noexcept
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_runtime->enterGuestExecution();
    }
}

PS2Runtime::GuestExecutionScope::~GuestExecutionScope()
{
    if (m_runtime)
    {
        m_runtime->leaveGuestExecution();
    }
}

PS2Runtime::GuestExecutionReleaseScope::GuestExecutionReleaseScope(PS2Runtime *runtime) noexcept
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_depth = m_runtime->releaseGuestExecution();
    }
}

PS2Runtime::GuestExecutionReleaseScope::~GuestExecutionReleaseScope()
{
    if (m_runtime && m_depth != 0u)
    {
        m_runtime->reacquireGuestExecution(m_depth);
    }
}

// G189 (diagnostic watchdog, DEFAULT OFF, DC2_G189_WATCHDOG=1): a real, reproducible hang under
// DC2_G157_PIPELINE=1 was found on the recorded town->inventory route -- the present thread's
// tick counter freezes permanently a few seconds after the inventory circular-map screen opens
// (matches the user's live RTSS "drops to ~2Hz" report). See ps2_gif_arbiter.cpp for the
// matching worker/EE-thread breadcrumbs and plans/phase-G189-fix-log.md for the investigation.
namespace
{
    std::atomic<uint64_t> g_g189PresentTick{0};
    std::atomic<int> g_g189PresentStage{0}; // 0 top-of-loop, 1 pre-copy, 2 post-copy/pre-upload, 3 post-upload

    void g189StartWatchdogOnce()
    {
        static const bool s_on = (std::getenv("DC2_G189_WATCHDOG") != nullptr);
        if (!s_on)
            return;
        static std::once_flag s_started;
        std::call_once(s_started, [] {
            std::thread([] {
                extern int g189_worker_stage();
                extern uint64_t g189_worker_n();
                extern int g189_ee_stage();
                uint64_t lastTick = std::numeric_limits<uint64_t>::max();
                auto lastChange = std::chrono::steady_clock::now();
                bool reported = false;
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    const uint64_t tick = g_g189PresentTick.load(std::memory_order_relaxed);
                    const auto now = std::chrono::steady_clock::now();
                    if (tick != lastTick)
                    {
                        lastTick = tick;
                        lastChange = now;
                        reported = false;
                        continue;
                    }
                    const double stuckMs = std::chrono::duration<double, std::milli>(now - lastChange).count();
                    if (stuckMs > 2000.0 && !reported)
                    {
                        std::fprintf(stderr,
                                     "[G189:watchdog] present tick STUCK at %llu for %.0fms | "
                                     "presentStage=%d workerStage=%d workerN=%llu eeStage=%d\n",
                                     (unsigned long long)tick, stuckMs,
                                     g_g189PresentStage.load(std::memory_order_relaxed),
                                     g189_worker_stage(),
                                     (unsigned long long)g189_worker_n(),
                                     g189_ee_stage());
                        reported = true;
                    }
                }
            }).detach();
        });
    }
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;

    // G157: under pipelined MTGS the worker owns latching (ps2_gif_arbiter.cpp frame-boundary
    // marker, gated so it always sees complete VRAM + still-correct registers). Calling
    // latchHostPresentationFrame() here too could race the worker's in-progress draws for the
    // NEXT frame, so present must only ever read the already-published snapshot in that mode.
    // G175: the same race exists in EVERY mode, not just pipelined MTGS — this present-thread
    // re-latch fires ~10x per guest frame and reads VRAM mid-frame while the guest (serial) or
    // GS worker (MTGS) is still building it. On RTT-composited transitions (costume-screen exit
    // fade) that latched just-cleared/partially-composited buffers: the nonzero-pixel ping-pong
    // (full->partial->black->full) documented in PS2_PROJECT_STATE.md Known Issues #2. The guest
    // frame-boundary latch (g150_frame_barrier at the mgEndFrame hook, all modes) is the sole
    // valid presentation source; keep the per-tick latch only as a boot fallback until the first
    // boundary latch exists. DC2_G175_TICK_RELATCH=1 restores the old per-tick behavior.
    extern bool g150_pipeline_enabled();
    extern uint64_t g175_frame_boundary_count();
    static const bool s_g175TickRelatch = [] {
        const char *v = std::getenv("DC2_G175_TICK_RELATCH");
        return v && *v && *v != '0';
    }();
    const uint64_t currentTick = ps2_syscalls::GetCurrentVSyncTick();
    if (!s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick)
    {
        if (!g150_pipeline_enabled() &&
            (s_g175TickRelatch || g175_frame_boundary_count() == 0ull))
            rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }

    // G176 (GPU-raster arc option (a)): the latched snapshot only changes when a latch
    // runs (guest frame boundary, ~6.5Hz on the title; per-tick only during boot fallback
    // or DC2_G175_TICK_RELATCH=1), but this function ran the full m_stateMutex-held
    // snapshot copy + convert + whole-frame UpdateTexture every host present tick (~60Hz)
    // — ~90% redundant GPU upload + the exact main-thread GPU touch G167 measured as the
    // cross-thread contention driver, plus per-tick m_stateMutex contention against the
    // GS worker. Skip all of it when the latch generation is unchanged; frameTex already
    // holds this snapshot and DrawTexturePro keeps presenting it every tick.
    // Kill-switch DC2_G176_TICK_UPLOAD=1 restores the per-tick upload.
    extern uint64_t g176_present_latch_generation();
    static const bool s_g176TickUpload = [] {
        const char *v = std::getenv("DC2_G176_TICK_UPLOAD");
        return v && *v && *v != '0';
    }();
    static const bool s_g176Stat = [] {
        const char *v = std::getenv("DC2_G176_STAT");
        return v && *v && *v != '0';
    }();
    static uint64_t s_g176LastUploadedGen = std::numeric_limits<uint64_t>::max();
    static uint32_t s_g176LastOutWidth = FB_WIDTH;
    static uint32_t s_g176LastOutHeight = DEFAULT_DISPLAY_HEIGHT;
    static uint64_t s_g176Skips = 0ull;
    static uint64_t s_g176Uploads = 0ull;
    // Read the generation BEFORE the copy: if a latch lands in between, we copy the newer
    // snapshot but record the older generation, so the next tick re-uploads once
    // (harmless duplicate). Reading after could record a generation whose snapshot we
    // never copied (missed frame).
    const uint64_t g176Gen = g176_present_latch_generation();
    if (s_g176Stat && ((s_g176Skips + s_g176Uploads) % 600ull) == 599ull)
    {
        std::fprintf(stderr, "[G176:stat] ticks=%llu uploads=%llu skips=%llu gen=%llu\n",
                     static_cast<unsigned long long>(s_g176Skips + s_g176Uploads + 1ull),
                     static_cast<unsigned long long>(s_g176Uploads),
                     static_cast<unsigned long long>(s_g176Skips),
                     static_cast<unsigned long long>(g176Gen));
    }
    if (!s_g176TickUpload && g176Gen == s_g176LastUploadedGen)
    {
        ++s_g176Skips;
        outWidth = s_g176LastOutWidth;
        outHeight = s_g176LastOutHeight;
        return;
    }
    ++s_g176Uploads;

    std::vector<uint8_t> scratch;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_g176LastUploadedGen = g176Gen;
        s_g176LastOutWidth = outWidth;
        s_g176LastOutHeight = outHeight;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        static uint32_t s_probeDebugCount = 0u;
        if (s_probeDebugCount < 32u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred)
        {
            std::cout << "[frame:probe] idx=" << s_probeDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u);
            for (const auto &probe : kGhostProbePoints)
            {
                if (probe.x >= width || probe.y >= height)
                {
                    continue;
                }

                const uint32_t pixel = sampleHostFramePixel(scratch, width, height, probe.x, probe.y);
                std::cout << " host[" << probe.x << "," << probe.y << "]=0x"
                          << std::hex << pixel << std::dec;
            }
            std::cout << std::endl;
            ++s_probeDebugCount;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::vector<uint8_t> uploadBuffer(DEFAULT_FB_SIZE, 0u);
    if (!scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > scratch.size() ||
                dstOffset + copyRowBytes > uploadBuffer.size())
            {
                break;
            }
            std::memcpy(uploadBuffer.data() + dstOffset, scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_g176LastUploadedGen = g176Gen;
    s_g176LastOutWidth = outWidth;
    s_g176LastOutHeight = outHeight;
}

PS2Runtime::PS2Runtime()
{
    std::memset(&m_cpuContext, 0, sizeof(m_cpuContext));

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);

    // G40: VU0 vf0 is hardwired to (x,y,z,w)=(0,0,0,1) on real hardware. The
    // recompiled COP2 macro ops read ctx->vu0_vf[0] for that constant (e.g.
    // mgInversMatrix@0x1302d0 does `vdiv $Q,$vf0w,$vf15x` -> Q = vf0.w / det),
    // but the context memset above leaves vf0 = (0,0,0,0). With vf0.w == 0 the
    // determinant reciprocal collapses to 0, so every matrix inverse comes out
    // ALL-ZERO -> the skinned-character bone-matrix palette is zero -> skinned
    // meshes collapse (the costume Max model; any skinned NPC/player). The
    // dungeon map never exposed this because it transforms via VIF1 DIRECT, not
    // the VU0 macro path. Nothing in the runtime or the recompiled ELF ever
    // writes vf0, so pinning it once here is sufficient and permanent.
    // _mm_set_ps(w,z,y,x): lane0=x=0, lane1=y=0, lane2=z=0, lane3=w=1.
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_functionTable.clear();

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        ps2_syscalls::detachAllGuestHostThreads();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();

        m_functionTable.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t itop)
                                 {
                                     // G297 MTVU: hand the kick to the VU1 worker (default-off).
                                     if (g297MtvuActive())
                                     {
                                         G297Mtvu::instance().wire(&m_vu1, &m_memory, &m_gs);
                                         G297Mtvu::instance().enqueueKick(
                                             startPC, itop, m_memory.vif1_regs.top & 0x3FFu,
                                             dc2Vu1CycleBudget(), false);
                                         return;
                                     }
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, itop, dc2Vu1CycleBudget()); });
    m_memory.setVu1MscntCallback([this](uint32_t itop)
                                 {
                                     if (g297MtvuActive())
                                     {
                                         G297Mtvu::instance().wire(&m_vu1, &m_memory, &m_gs);
                                         G297Mtvu::instance().enqueueKick(
                                             0u, itop, m_memory.vif1_regs.top & 0x3FFu,
                                             dc2Vu1CycleBudget(), true);
                                         return;
                                     }
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, itop, dc2Vu1CycleBudget()); });
    m_iop.init(rdram);
    m_iop.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

