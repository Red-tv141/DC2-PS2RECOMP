#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_frame_dump.h"
#include "runtime/ps2_gs_gpu.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>

namespace ps2_stubs
{
    void resetSifState();
}

void (*g_g7_poll_live_pad_hook)() = nullptr;
void (*g_g55_title_draw_probe_hook)(uint8_t *rdram) = nullptr;
bool (*g_f66_drive_dungeon_pad_hook)(uint32_t) = nullptr;


// ============================================================================
// F60 spin diagnostics (global linkage; referenced by extern in the primitive
// TUs). A guest EE busy-wait that never returns hammers exactly one of these
// poll/wait primitives. The host present loop stays alive during the stall and
// samples per-interval deltas (see "[F60:spin]" below) so we can identify the
// spinning primitive and the guest return address it is polling from.
// All quiet/no-op unless DC2_TRACE_F60 is set.
// ============================================================================
std::atomic<uint64_t> g_f60_checkStatRpc{0};
std::atomic<uint64_t> g_f60_pollSema{0};
std::atomic<uint64_t> g_f60_dmaStat{0};
std::atomic<uint64_t> g_f60_gsSyncV{0};
std::atomic<uint64_t> g_f60_waitVSyncTick{0};
std::atomic<uint64_t> g_f60_waitSemaBlocked{0};
std::atomic<uint32_t> g_f60_checkStatRpc_ra{0};
std::atomic<uint32_t> g_f60_checkStatRpc_client{0};
std::atomic<uint32_t> g_f60_pollSema_ra{0};
std::atomic<uint32_t> g_f60_waitSema_ra{0};
std::atomic<int32_t> g_f60_waitSema_sid{0};

// ============================================================================
// Syscall-dispatch path audit (env-gated DC2_TRACE_SYSCALL_PATH; quiet by
// default). Counts how each EE syscall resolves: encoded immediate, $v1 (the
// canonical EE number reg), or nothing (→ [Syscall TODO]). F64.5 removed the
// legacy $v0 fallback after this probe measured 0 hits for DC2; anything that
// now lands in [Syscall TODO] is a real missing/mis-emitted syscall to fix.
// ============================================================================
std::atomic<uint64_t> g_scpath_v1{0};   // resolved via $v1 (canonical)
std::atomic<uint64_t> g_scpath_enc{0};  // resolved via encoded immediate
std::atomic<uint64_t> g_scpath_todo{0}; // resolved by nothing (→ [Syscall TODO])

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif

static bool runtimeEnvFlagEnabled(const char *name)
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

static uint32_t dc2Vu1CycleBudget()
{
    static const uint32_t budget = []()
    {
        const char *value = std::getenv("DC2_G22_VU1_CYCLES");
        if (!value || value[0] == '\0')
            return 65536u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value || parsed == 0ul)
            return 65536u;
        if (parsed > 1048576ul)
            return 1048576u;
        return static_cast<uint32_t>(parsed);
    }();
    return budget;
}

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
                                 { m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                 m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                 m_gs, &m_memory, startPC, itop, dc2Vu1CycleBudget()); });
    m_memory.setVu1MscntCallback([this](uint32_t itop)
                                 { m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                m_gs, &m_memory, itop, dc2Vu1CycleBudget()); });
    m_iop.init(rdram);
    m_iop.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        // G158a (default OFF, DC2_G158_GPURASTER=1): one-shot synthetic FBO/shader/readback
        // round-trip proof on the just-created GL context. No effect on real rendering; see
        // plans/phase-G158-fix-log.md. A no-op single boolean read when unset.
        // G159: capture this (main-thread) HGLRC/HDC BEFORE the MTGS worker thread can exist, so
        // g158RunWorkerContextTest() (ps2_gif_arbiter.cpp, runs later on the worker thread) can
        // create a shared second context against it. Also a no-op when the env var is unset.
        extern void g158CaptureMainContext();
        extern void g158RunSelfTest();
        g158CaptureMainContext();
        g158RunSelfTest();
        // G160 (default OFF, same DC2_G158_GPURASTER switch): G159 found sharing fails while the
        // main context is current elsewhere; this releases it, shares+tests on a fresh dedicated
        // GPU thread, then restores it here before raylib's own frame loop starts. Blocking
        // (one-time startup stall) and a no-op when unset. See plans/phase-G160-fix-log.md.
        extern void g158StartDedicatedGpuThread();
        g158StartDedicatedGpuThread();
        // G162 (default OFF, needs DC2_G158_GPURASTER=1 AND DC2_G162_GPU_DECODE=1): starts a
        // SECOND, PERSISTENT dedicated GPU thread (same release-before-share pattern G160 just
        // validated above) that stays alive for the process lifetime, draining PSMT8 texture
        // de-swizzle+CLUT decode jobs submitted from G149's texture-cache build site. A no-op
        // when either env var is unset. See plans/phase-G162-fix-log.md.
        extern void g162StartPersistentDecoder();
        g162StartPersistentDecoder();
        // G178 (default ON since 2026-07-10, kill DC2_G178_NO_GPU=1): starts the persistent LLE
        // GPU rasterizer backend thread (same release-before-share pattern as G160/G162 above;
        // the main context capture above also fires for this lever). See
        // plans/gpu-raster-arc-plan.md and plans/phase-G178-fix-log.md.
        extern void g178StartPersistentBackend();
        g178StartPersistentBackend();

        SetTargetFPS(60);

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    if (std::getenv("DC2_TRACE_F58") != nullptr)
    {
        std::fprintf(stderr,
                     "[F58:elfheap] maxLoadedRdramEnd=0x%x paddedEnd=0x%x suggestedHeapBase=0x%x "
                     "suggestedBase=0x%x hardLimit=0x%x RAM=0x%x\n",
                     maxLoadedRdramEnd, paddedEnd, suggestedHeapBase, m_guestHeapSuggestedBase,
                     (unsigned)std::min(kGuestHeapHardLimit, PS2_RAM_SIZE), (unsigned)PS2_RAM_SIZE);
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    ps2_game_overrides::applyMatching(*this, elfPath, m_cpuContext.pc);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

void PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    m_functionTable[address] = func;
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        return true;
    }

    return false;
}

// G186 watch state (see lookupFunction / the first-bad-pc handler)
struct G186WatchChange
{
    uint32_t oldVal, newVal, prevAddr, nextAddr;
};
static G186WatchChange g_g186Ring[64];
static uint32_t g_g186RingN = 0u;
static const uint32_t g_g186WatchAddr = []() -> uint32_t {
    const char *e = std::getenv("DC2_G186_WATCH");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
}();

// G186 sp-balance trampoline (opt-in DC2_G186_SPBAL=1): every dispatched call is
// wrapped so a callee that returns with a shifted guest $sp is named directly.
// (Guest thread switches / longjmp-class flows legitimately change sp — read the
// log with that in mind; the leak signature is a REPEATED same-address entry.)
static const bool g_g186SpBal = (std::getenv("DC2_G186_SPBAL") != nullptr);
bool g_g186SpBalArmed = false; // set by the G186 EditDraw probe (dc2_game_override.cpp)
struct G186SpEnt
{
    PS2Runtime::RecompiledFunction fn;
    uint32_t addr;
};
static thread_local std::vector<G186SpEnt> g_g186SpStack;
static void g186SpBalTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    G186SpEnt e = g_g186SpStack.back();
    g_g186SpStack.pop_back();
    const uint32_t spIn = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    e.fn(rdram, ctx, runtime);
    const uint32_t spOut = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    if (spOut != spIn)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = ++s_n;
        if (g_g186SpBalArmed && n <= 8192u)
            std::fprintf(stderr, "[G186:spbal] fn=0x%x spIn=0x%x spOut=0x%x delta=%d n=%u\n",
                         e.addr, spIn, spOut, static_cast<int32_t>(spOut - spIn), n);
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    // G58: detect the function that clobbers TitleMapDraw's saved-$ra slot (0x1fff800).
    static const bool s_g58 = (std::getenv("DC2_TRACE_G58") != nullptr);
    if (s_g58 && g_g58Rdram && address == 0x002A2280u)
    {
        // One-shot title-scene camera-state dump on TitleMapDraw entry. HW (live): scene
        // 0x1dd8260, count=8, slot0 +0x00=0x4 +0x34=0x785f70 (camera obj, vt 0x374e70).
        static int s_camDumps = 0;
        if (s_camDumps < 6)
        {
            ++s_camDumps;
            const uint32_t scene = readGuestU32Wrapped(g_g58Rdram, 0x00377EDCu);
            const uint32_t count = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2044u) : 0u;
            const uint32_t camId = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2E54u) : 0u;
            const uint32_t slot0v = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2048u) : 0u;
            const uint32_t slot0o = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2048u + 0x34u) : 0u;
            const uint32_t camObjVt = readGuestU32Wrapped(g_g58Rdram, 0x00785F70u + 0x60u);
            const uint32_t camObjW0 = readGuestU32Wrapped(g_g58Rdram, 0x00785F70u);
            std::cerr << "[G58:camstate] scene=0x" << std::hex << scene
                      << " count=" << std::dec << count
                      << " camId=" << camId
                      << " slot0.valid=0x" << std::hex << slot0v
                      << " slot0.obj=0x" << slot0o
                      << " | 0x785f70[0]=0x" << camObjW0
                      << " 0x785f70+0x60(vt)=0x" << camObjVt
                      << std::dec << std::endl;
        }
    }
    if (s_g58 && g_g58Rdram)
    {
        const uint32_t slot = readGuestU32Wrapped(g_g58Rdram, 0x01FFF800u);
        if (slot != g_g58LastSlot)
        {
            std::cerr << "[G58:write] 0x1fff800 0x" << std::hex << g_g58LastSlot
                      << " -> 0x" << slot
                      << " after fn@0x" << g_g58PrevPc
                      << " (next=0x" << address << ")" << std::dec << std::endl;
            g_g58LastSlot = slot;
        }
        g_g58PrevPc = address;
    }

    // F64 DISCOVERY: capture the set of INDIRECTLY-called guest functions while the
    // dungeon-entrance event is parked (DngStatus==2, CRunScript done==0). The
    // audio-gated wait command (op-0x15 ext native fn) is reached via the indirect
    // call in ext__10CRunScript@0x1870bc, so it shows up here. Cross-ref the printed
    // addresses against ref/decompiled.txt to name the sound-state query. Gated
    // DC2_TRACE_F64 (default OFF, quiet otherwise).
    {
        static const bool s_f64disc = (std::getenv("DC2_TRACE_F64") != nullptr);
        if (s_f64disc)
        {
            uint8_t *rd = m_memory.getRDRAM();
            if (rd)
            {
                int32_t status;
                uint32_t done, dngMap;
                std::memcpy(&status, rd + (0x01E9F6E0u & 0x01FFFFFFu), 4);
                std::memcpy(&done, rd + (0x01ece40cu & 0x01FFFFFFu), 4);
                std::memcpy(&dngMap, rd + (0x003772A4u & 0x01FFFFFFu), 4);
                if (status == 2 && done == 0u && dngMap != 0u)
                {
                    static uint32_t s_seen[128] = {0};
                    static uint32_t s_n = 0u;
                    bool found = false;
                    for (uint32_t i = 0; i < s_n; ++i)
                        if (s_seen[i] == address) { found = true; break; }
                    if (!found && s_n < 128u)
                    {
                        s_seen[s_n++] = address;
                        std::fprintf(stderr, "[F64:indcall] addr=0x%x distinct=%u\n", address, s_n);
                    }
                }
            }
        }
    }

    // G186 (opt-in DC2_G186_WATCH=<hex guest addr>): watch one guest word at every
    // dispatch boundary, keep a RING of the most recent changes, and dump it from the
    // first-bad-pc handler. Same technique as the G58 saved-$ra clobber hunt, but
    // address-configurable and crash-anchored (streaming hit its cap long before the
    // crash; only the last few changes before first-bad-pc matter).
    if (g_g186WatchAddr)
    {
        static uint32_t s_lastVal = 0u;
        static uint32_t s_prevAddr = 0u;
        uint8_t *rd = m_memory.getRDRAM();
        if (rd)
        {
            const uint32_t v = readGuestU32Wrapped(rd, g_g186WatchAddr);
            if (v != s_lastVal)
            {
                G186WatchChange &c = g_g186Ring[g_g186RingN++ & 63u];
                c.oldVal = s_lastVal;
                c.newVal = v;
                c.prevAddr = s_prevAddr;
                c.nextAddr = address;
                if (g_g186RingN <= 32u)
                    std::fprintf(stderr, "[G186:watch] 0x%x: 0x%x -> 0x%x after=0x%x next=0x%x\n",
                                 g_g186WatchAddr, s_lastVal, v, s_prevAddr, address);
                s_lastVal = v;
            }
        }
        // Sentinel ring entries mark scope: EditDraw / DngMainDraw / EditLoop entries.
        if (address == 0x001AE3D0u || address == 0x001CF090u || address == 0x001ABCF0u)
        {
            G186WatchChange &c = g_g186Ring[g_g186RingN++ & 63u];
            c.oldVal = 0xEDEDEDEDu;
            c.newVal = 0xEDEDEDEDu;
            c.prevAddr = s_prevAddr;
            c.nextAddr = address;
        }
        s_prevAddr = address;
    }

    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        if (g_g186SpBal)
        {
            g_g186SpStack.push_back({it->second, address});
            return g186SpBalTrampoline;
        }
        return it->second;
    }

    std::cerr << "Warning: Function at address 0x" << std::hex << address << std::dec << " not found" << std::endl;

    static RecompiledFunction defaultFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
        const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
        const uint32_t gp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) : 0u;
        const uint32_t a0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) : 0u;
        const uint32_t a1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) : 0u;
        const uint32_t v0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0)) : 0u;
        const uint32_t v1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0)) : 0u;

        if (ctx && runtime)
        {
            thread_local uint32_t s_recoverCount = 0u;
            thread_local bool s_loggedContext = false;
            const uint32_t pc = ctx->pc;
            const bool hasPcFunction = runtime->hasFunction(pc);

            if (!hasPcFunction && s_recoverCount < 8192u)
            {
                if (!s_loggedContext)
                {
                    std::ostringstream stackDump;
                    if (rdram)
                    {
                        stackDump << " [stack]";
                        for (uint32_t off = 0u; off < 0x40u; off += 4u)
                        {
                            const uint32_t slot = readGuestU32Wrapped(rdram, sp + off);
                            stackDump << " +" << std::hex << off << "=0x" << slot;
                        }
                    }
                    std::cerr << "[dispatch:first-bad-pc] bad=0x" << std::hex << pc
                              << " ra=0x" << ra
                              << " sp=0x" << sp
                              << " gp=0x" << gp
                              << " v0=0x" << v0
                              << " v1=0x" << v1
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " trace=" << formatDispatchHistory()
                              << stackDump.str()
                              << std::dec << std::endl;
                    s_loggedContext = true;

                    // G186: dump the watch ring (most recent changes of the watched word)
                    if (g_g186WatchAddr)
                    {
                        const uint32_t n = g_g186RingN < 64u ? g_g186RingN : 64u;
                        for (uint32_t i = 0; i < n; ++i)
                        {
                            const G186WatchChange &c = g_g186Ring[(g_g186RingN - n + i) & 63u];
                            std::fprintf(stderr, "[G186:ring] %u/%u 0x%x: 0x%x -> 0x%x after=0x%x next=0x%x\n",
                                         i + 1, n, g_g186WatchAddr, c.oldVal, c.newVal, c.prevAddr, c.nextAddr);
                        }
                    }

                    // G186 (opt-in DC2_G186_BADPTR_DUMP=1): dump the entire guest RAM at the
                    // moment of the FIRST bad indirect-call target so offline tooling can
                    // locate which structure held the garbage pointer and A/B the same slots
                    // against a paused PCSX2. One-shot per process.
                    static const bool s_g186dump = (std::getenv("DC2_G186_BADPTR_DUMP") != nullptr);
                    if (s_g186dump && rdram)
                    {
                        static std::atomic<bool> s_dumped{false};
                        bool expected = false;
                        if (s_dumped.compare_exchange_strong(expected, true))
                        {
                            const uint32_t t9 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[25], 0));
                            if (FILE *fp = std::fopen("captures/g186_badpc_ram.bin", "wb"))
                            {
                                std::fwrite(rdram, 1u, 0x02000000u, fp);
                                std::fclose(fp);
                                std::fprintf(stderr, "[G186:dump] wrote captures/g186_badpc_ram.bin bad=0x%x t9=0x%x\n", pc, t9);
                            }
                            else
                            {
                                std::fprintf(stderr, "[G186:dump] FAILED to open captures/g186_badpc_ram.bin (t9=0x%x)\n", t9);
                            }
                        }
                    }
                }

                uint32_t recoveryPc = 0u;
                if (ra != 0u && runtime->hasFunction(ra))
                {
                    recoveryPc = ra;
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }

                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 256u)
                    {
                        std::cerr << "[dispatch:recover-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }

            if (hasPcFunction)
            {
                s_recoverCount = 0u;
                s_loggedContext = false;
            }
            else if (pc < 0x00100000u && ra == pc && s_recoverCount < 4096u)
            {
                uint32_t recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }
                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 128u)
                    {
                        std::cerr << "[dispatch:recover-low-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }
        }

        std::ostringstream oss;
        oss << "Error: Called unimplemented function at address 0x" << std::hex << (ctx ? ctx->pc : 0u)
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " hostTid=" << std::this_thread::get_id()
            << " pcTrace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_defaultFnLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_defaultFnLogMutex);
            std::cerr << oss.str() << std::endl;
        }

        runtime->requestStop();
    };

    return defaultFunction;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    static std::unordered_map<uint32_t, int> seen;
    int &count = seen[address];
    if (count < 3)
    {
        RUNTIME_LOG("[VU0] microprogram @0x" << std::hex << address
                                             << " pc=0x" << ctx->pc
                                             << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0))
                                             << std::dec << std::endl);
    }
    ++count;

    // Seed status so dependent code sees success.
    ctx->vu0_clip_flags = 0;
    ctx->vu0_clip_flags2 = 0;
    ctx->vu0_mac_flags = 0;
    ctx->vu0_status = 0;
    ctx->vu0_q = 1.0f;
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    static const bool s_scpath = (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr);

    // EE syscall number = the syscall instruction's encoded immediate when present,
    // else $v1 ($3), the canonical EE kernel syscall-number register. The legacy
    // $v0 ($2) fallback was REMOVED in F64.5 (upstream PR #131 alignment): $v0 is the
    // EE return-value register, never the number, so the fallback could only fire on a
    // stale-$v0 collision and silently run the WRONG syscall, masking a missing one.
    // Measured 0 hits for DC2 (probe DC2_TRACE_SYSCALL_PATH). A genuinely missing
    // syscall now lands in [Syscall TODO] (which logs both $v1 and $v0) — fix at root.
    // NOTE: do NOT also take PR #131's 0x5A/0x5B remap; DC2 calls QueryBootMode (0x5A)
    // and GetThreadTLS (0x5B) — see plans/syscall-v0-fallback-investigation.md.
    const bool fromEncoded = (encodedSyscallId != 0u);
    const uint32_t syscallId = fromEncoded ? encodedSyscallId : getRegU32(ctx, 3); // $v1

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        if (s_scpath)
        {
            const uint64_t n = (fromEncoded ? g_scpath_enc : g_scpath_v1)
                                   .fetch_add(1, std::memory_order_relaxed);
            if (n < 80u)
            {
                std::cerr << "[SCPATH:" << (fromEncoded ? "enc" : "v1") << "] num=0x"
                          << std::hex << syscallId
                          << " ra=0x" << getRegU32(ctx, 31)
                          << " pc=0x" << ctx->pc << std::dec << std::endl;
            }
        }
        return;
    }

    if (s_scpath)
    {
        const uint64_t n = g_scpath_todo.fetch_add(1, std::memory_order_relaxed);
        if (n < 200u)
        {
            std::cerr << "[SCPATH:todo]"
                      << " v1=0x" << std::hex << getRegU32(ctx, 3)
                      << " v0=0x" << getRegU32(ctx, 2)
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " pc=0x" << ctx->pc
                      << std::dec << std::endl;
        }
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// G1: allocator-family coherence trace (env-gated DC2_TRACE_ALLOC, quiet by
// default). Every C/C++ allocation entry point (malloc/free + _malloc_r/_free_r/
// _calloc_r, operator new/delete via jal 0x1264a0/0x1264c8) is proven to land in
// THIS single host free-list. The counters confirm the live path is the runtime
// heap; if the dead newlib internals (sbrk@0x1107e0 -> TODO_NAMED "sbrk", or
// malloc_extend_top) ever fire, they log separately -> mixed routing would show.
namespace
{
    std::atomic<uint64_t> g_allocTraceAllocs{0};
    std::atomic<uint64_t> g_allocTraceFrees{0};
    bool allocTraceEnabled()
    {
        static const bool s_on = (std::getenv("DC2_TRACE_ALLOC") != nullptr);
        return s_on;
    }
    void allocTraceTick(const char *what, uint32_t arg0, uint32_t arg1)
    {
        if (!allocTraceEnabled())
        {
            return;
        }
        const uint64_t a = g_allocTraceAllocs.load(std::memory_order_relaxed);
        const uint64_t f = g_allocTraceFrees.load(std::memory_order_relaxed);
        const uint64_t sum = a + f;
        if (sum <= 4u || (sum & 0xFFu) == 0u)
        {
            std::fprintf(stderr, "[G1:alloc] %s a0=0x%x a1=0x%x | allocs=%llu frees=%llu live=%lld\n",
                         what, arg0, arg1,
                         (unsigned long long)a, (unsigned long long)f,
                         (long long)((int64_t)a - (int64_t)f));
        }
    }
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        g_allocTraceAllocs.fetch_add(1, std::memory_order_relaxed);
        allocTraceTick("malloc", size, alignment);
        return alignedAddr;
    }

    // F58: guest-heap OOM diagnostics (env-gated DC2_TRACE_F58). malloc / _malloc_r /
    // calloc and thus C++ operator new (__nw__FUi@0x1005D0) all route here; a 0 return
    // makes __nw throw std::bad_alloc -> terminate -> abort -> exit (the silent F57
    // floor-entrance termination). Dump size + heap occupancy to classify a bogus
    // oversized request vs genuine exhaustion vs fragmentation. Quiet by default.
    static const bool s_f58Trace = (std::getenv("DC2_TRACE_F58") != nullptr);
    if (s_f58Trace)
    {
        uint64_t totalFree = 0u, largestFree = 0u, totalUsed = 0u;
        uint32_t freeBlocks = 0u, usedBlocks = 0u;
        for (const auto &blk : m_guestHeapBlocks)
        {
            if (blk.free)
            {
                totalFree += blk.size;
                if (blk.size > largestFree) largestFree = blk.size;
                ++freeBlocks;
            }
            else
            {
                totalUsed += blk.size;
                ++usedBlocks;
            }
        }
        std::fprintf(stderr,
                     "[F58:oom] guestMalloc FAIL size=0x%x align=0x%x allocSize=0x%x | "
                     "base=0x%x end=0x%x limit=0x%x | free=0x%llx largestFree=0x%llx "
                     "used=0x%llx blocks=%u(free=%u used=%u)\n",
                     size, alignment, allocSize, m_guestHeapBase, m_guestHeapEnd,
                     m_guestHeapLimit, (unsigned long long)totalFree,
                     (unsigned long long)largestFree, (unsigned long long)totalUsed,
                     (unsigned)m_guestHeapBlocks.size(), freeBlocks, usedBlocks);
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    g_allocTraceFrees.fetch_add(1, std::memory_order_relaxed);
    allocTraceTick("free", guestAddr, 0u);
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

// PR131: top of the configured guest-heap window (vs guestHeapEnd = current
// bump). Additive getter; SetupHeap/EndOfHeap keep F58 behavior.
uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t lastPc = std::numeric_limits<uint32_t>::max();
    uint32_t samePcCount = 0;
    constexpr uint32_t kSamePcYieldInterval = 0x4000u;

    g_g58Rdram = rdram; // G58: enable the 0x1fff800 saved-$ra watch in lookupFunction.

    while (!isStopRequested())
    {
        const uint32_t pc = ctx->pc;

        if (pc == lastPc)
        {
            ++samePcCount;
            if ((samePcCount % kSamePcYieldInterval) == 0u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    RUNTIME_LOG("CPU is doing some work at PC 0x" << std::hex << pc << ". PC not updating.");
                });
                std::this_thread::yield();
            }
        }
        else
        {
            samePcCount = 0;
            lastPc = pc;
        }

        m_debugPc.store(pc, std::memory_order_relaxed);
        m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), std::memory_order_relaxed);
        m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)), std::memory_order_relaxed);
        m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)), std::memory_order_relaxed);

        RecompiledFunction fn = lookupFunction(pc);
        const uint32_t dispatchedPc = pc;
        const uint32_t dispatchedRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));

        {
            GuestExecutionScope guestExecution(this);
            fn(rdram, ctx, this);
        }

        // G58: capture the function whose execution left pc=0x9f84a0 (the robust title crash).
        static const bool s_g58bad = (std::getenv("DC2_TRACE_G58") != nullptr);
        if (s_g58bad && ctx->pc == 0x009F84A0u)
        {
            static int s_g58BadHits = 0;
            if (s_g58BadHits < 4)
            {
                ++s_g58BadHits;
                const uint32_t r31 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
                std::cerr << "[G58:badjump] fn@0x" << std::hex << dispatchedPc
                          << " (enteredRa=0x" << dispatchedRa << ") left pc=0x9f84a0"
                          << " r31=0x" << r31 << " sp=0x" << sp
                          << " *(sp+0xA0)=0x" << readGuestU32Wrapped(rdram, sp + 0xA0u)
                          << std::dec << std::endl;
            }
        }

        if (ctx->pc == 0u)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            std::cerr << "[dispatch:pc-zero] from=0x" << std::hex << dispatchedPc
                      << " fromRa=0x" << dispatchedRa
                      << " ra=0x" << ra
                      << " sp=0x" << sp
                      << " gp=0x" << gp
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;

            // PC=0 means this guest thread returned (usually via jr $ra with RA=0).
            // Do not request a global runtime stop here: other guest threads may still run.
            break;
        }
    }
}

void PS2Runtime::enterGuestExecution()
{
    m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
    m_guestExecutionMutex.lock();
    m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
    ++g_guestExecutionDepths[this];
}

void PS2Runtime::leaveGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return;
    }

    --it->second;
    m_guestExecutionMutex.unlock();
    if (it->second == 0u)
    {
        g_guestExecutionDepths.erase(it);
    }
}

uint32_t PS2Runtime::releaseGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return 0u;
    }

    const uint32_t depth = it->second;
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionMutex.unlock();
    }
    g_guestExecutionDepths.erase(it);
    return depth;
}

void PS2Runtime::reacquireGuestExecution(uint32_t depth)
{
    if (depth == 0u)
    {
        return;
    }

    uint32_t &heldDepth = g_guestExecutionDepths[this];
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
        m_guestExecutionMutex.lock();
        m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
        ++heldDepth;
    }
}

// G57: scoped back-edge preemption suppression. The title-frame draw runs through
// override functions (f48_title_map_draw_probe / g56_begindraw_tap) that call the
// recompiled bodies DIRECTLY. When a recompiled loop yields mid-iteration via
// shouldPreemptGuestExecution() it returns a mid-function resume PC; the normal
// dispatch loop re-enters via the registered resume-PC table, but an override that
// invokes the body as a plain C++ call swallows that resume — leaving e.g.
// BeginDraw__14mgCDrawManager half-initialized (mgr[4]/mgr[6] null) so the deferred
// flush dereferences null and crashes. Suppressing preemption around the title draw
// makes those bodies run to completion in a single call. Scoped (RAII inc/dec around
// the title draw only), so other guest threads are unaffected outside that window.
std::atomic<int> g_dc2PreemptSuppressDepth{0};

// PHASE G214: extern-visible mirror of the host present-loop tick (g_g189PresentTick lives
// in an anonymous namespace and cannot be externed). Lets a game-override diagnostic probe
// correlate an event (e.g. a collapsed skin matrix) with the dumped frame_NNNNNN.ppm number.
std::atomic<uint64_t> g_dc2PresentTick{0};

bool PS2Runtime::shouldPreemptGuestExecution()
{
    // G57 experiment: allow disabling back-edge preemption entirely to confirm
    // a mid-loop preemption return is the corruption source. Env-gated.
    static const bool s_noPreempt = [] {
        const char *v = std::getenv("DC2_G57_NO_PREEMPT");
        return v && v[0] && v[0] != '0';
    }();
    if (s_noPreempt)
        return false;

    // G57 fix: scoped suppression set by the title-draw override.
    if (g_dc2PreemptSuppressDepth.load(std::memory_order_relaxed) > 0)
        return false;

    thread_local uint32_t s_backEdgeYieldCounter = 0u;
    const uint32_t waiterCount = m_guestExecutionWaiters.load(std::memory_order_acquire);
    const uint32_t yieldInterval = (waiterCount != 0u) ? 64u : 100u;
    if (++s_backEdgeYieldCounter < yieldInterval)
    {
        return false;
    }

    s_backEdgeYieldCounter = 0u;
    return true;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    ps2_syscalls::notifyRuntimeStop();
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

// F50.3: registry of live guest worker-thread contexts (external linkage; no header
// edit). Worker contexts are stack-locals inside StartThread's lambda, so the host
// hang watchdog cannot otherwise see them. StartThread registers/unregisters each
// worker here; the DC2_TRACE_HANG watchdog in run() samples every thread's live
// per-instruction PC to pinpoint an intra-function (no-call) spin.
std::mutex g_guestThreadCtxMutex;
std::unordered_map<int, R5900Context *> g_guestThreadCtxs;

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    ps2_syscalls::resetSoundDriverRpcState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetGsSyncVCallbackState();
    ps2_stubs::resetMpegStubState();
    ps2_syscalls::initializeGuestKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    g_activeThreads.store(1, std::memory_order_relaxed);
    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            dispatchLoop(m_memory.getRDRAM(), &m_cpuContext);
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
        gameThreadFinished.store(true, std::memory_order_release); });

    ps2_syscalls::EnsureVSyncWorkerRunning(m_memory.getRDRAM(), this);

    g189StartWatchdogOnce();
    uint64_t tick = 0;
    while (!isStopRequested() && g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        tick++;
        g_g189PresentTick.store(tick, std::memory_order_relaxed);
        g_dc2PresentTick.store(tick, std::memory_order_relaxed);
        g_g189PresentStage.store(0, std::memory_order_relaxed);
        PS2_IF_AGRESSIVE_LOGS({
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const int activeThreads = g_activeThreads.load(std::memory_order_relaxed);

                std::cout << "[run:tick] tick=" << tick
                          << " pc=0x" << std::hex << dbgPc
                          << " ra=0x" << dbgRa
                          << " sp=0x" << dbgSp
                          << " gp=0x" << dbgGp
                          << " dispfb1=0x" << gs.dispfb1
                          << " display1=0x" << gs.display1
                          << std::dec
                          << " activeThreads=" << activeThreads
                          << " dma=" << curDma
                          << " gif=" << curGif
                          << " gsw=" << curGs
                          << " vif=" << curVif
                          << std::endl;
            }
        });
        // F50.2: env-gated hang watchdog. The game thread runs dispatchLoop; when a
        // recompiled function spins on an internal backward branch (no sub-calls), it
        // never returns to dispatchLoop, so m_debugPc freezes at that function's entry
        // PC. Sample it here on the host present-thread to surface the spinning function.
        {
            static const bool s_hangWatch = (std::getenv("DC2_TRACE_HANG") != nullptr);
            if (s_hangWatch && (tick % 240) == 0)
            {
                static uint32_t s_lastPc = 0xFFFFFFFFu;
                static uint32_t s_freeze = 0u;
                // Sample the LIVE per-instruction PC (recomp sets ctx->pc before every
                // instruction). m_debugPc only updates at dispatchLoop granularity, which
                // is useless here because recomp functions nest inline (the whole game
                // runs as one deep C++ call tree from the entry dispatch). Reading the
                // live context PC pinpoints an intra-function (no-call) spin exactly.
                const uint32_t livePc = m_cpuContext.pc;
                const uint32_t wpc = m_debugPc.load(std::memory_order_relaxed);
                if (livePc == s_lastPc) { ++s_freeze; } else { s_freeze = 0u; s_lastPc = livePc; }
                std::cerr << "[F50.2:watch] tick=" << std::dec << tick
                          << " livePc=0x" << std::hex << livePc
                          << " dispPc=0x" << wpc
                          << " ra=0x" << m_debugRa.load(std::memory_order_relaxed)
                          << " sp=0x" << m_debugSp.load(std::memory_order_relaxed)
                          << " gp=0x" << m_debugGp.load(std::memory_order_relaxed)
                          << std::dec << " freezeN=" << s_freeze << std::endl;

                // F50.3: also sample every live guest worker thread's per-instruction PC.
                // The spinning worker (sound/ADX thread) holds a stack-local context that
                // m_cpuContext/m_debugPc never see; this exposes its exact spin instruction.
                std::lock_guard<std::mutex> wlock(g_guestThreadCtxMutex);
                for (auto &kv : g_guestThreadCtxs)
                {
                    if (kv.second == nullptr) continue;
                    std::cerr << "[F50.3:thread] tick=" << std::dec << tick
                              << " tid=" << kv.first
                              << " pc=0x" << std::hex << kv.second->pc
                              << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[31], 0))
                              << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[29], 0))
                              << " a0=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[4], 0))
                              << " a1=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[5], 0))
                              << std::dec << std::endl;
                }
            }
        }
        // G167 (MEASURE ONLY, DC2_G167_STARVE_MAIN=1, default off): skip the main thread's own
        // GPU work (UploadFrame's UpdateTexture + DrawTexturePro) to test whether G165's
        // cross-thread-GPU-contention hypothesis for the T8 GPU-decoder thread is correct -- if
        // the decoder's draw/readback times (DC2_G164_PROFILE) drop when the main thread stops
        // touching the GPU, the hypothesis is confirmed; if unchanged, something else dominates.
        // Diagnostic only, never shipped as a real presentation path -- window goes black/frozen
        // while set. See plans/phase-G167-fix-log.md.
        static const bool s_g167StarveMain = (std::getenv("DC2_G167_STARVE_MAIN") != nullptr);
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        g_g189PresentStage.store(1, std::memory_order_relaxed);
        if (!s_g167StarveMain)
            UploadFrame(frameTex, this, presentWidth, presentHeight);
        g_g189PresentStage.store(2, std::memory_order_relaxed);

        BeginDrawing();
        ClearBackground(BLACK);
        g_g189PresentStage.store(3, std::memory_order_relaxed);
        if (!s_g167StarveMain)
        {
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }

        if (dc2HudEnabled())
        {
            const int hudX = 8;
            const int hudY = 8;
            DrawRectangle(hudX - 4, hudY - 4, 320, 70, Fade(BLACK, 0.55f));

            char line1[64];
            char line2[128];
            char line3[64];
            std::snprintf(line1, sizeof(line1), "tick=%llu",
                          static_cast<unsigned long long>(tick));
            std::snprintf(line2, sizeof(line2),
                          "dma=%llu gif=%llu gsw=%llu vif=%llu",
                          static_cast<unsigned long long>(m_memory.dmaStartCount()),
                          static_cast<unsigned long long>(m_memory.gifCopyCount()),
                          static_cast<unsigned long long>(m_memory.gsWriteCount()),
                          static_cast<unsigned long long>(m_memory.vifWriteCount()));
            std::snprintf(line3, sizeof(line3), "threads=%d",
                          g_activeThreads.load(std::memory_order_relaxed));

            DrawText(line1, hudX, hudY, 20, RAYWHITE);
            DrawText(line2, hudX, hudY + 20, 18, LIME);
            DrawText(line3, hudX, hudY + 42, 18, SKYBLUE);
        }

        static const bool s_scpathSummary = (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr);
        if (s_scpathSummary && (tick != 0) && (tick % 120 == 0)) {
            std::cerr << "[SCPATH:summary] tick=" << tick
                      << " enc=" << g_scpath_enc.load(std::memory_order_relaxed)
                      << " v1=" << g_scpath_v1.load(std::memory_order_relaxed)
                      << " todo=" << g_scpath_todo.load(std::memory_order_relaxed)
                      << std::endl;
        }

        static bool s_forceBlank = false;
        if (IsKeyPressed(KEY_F11)) {
            s_forceBlank = !s_forceBlank;
        }
        static const uint64_t s_frameDumpEvery = []() -> uint64_t {
            const char *e = std::getenv("DC2_FRAME_DUMP_EVERY");
            const long v = e ? std::strtol(e, nullptr, 10) : 60;
            return v > 0 ? static_cast<uint64_t>(v) : 60ull;
        }();
        const bool periodic = dc2FrameDumpEnabled() && (tick != 0) && (tick % s_frameDumpEvery == 0);
        const bool manual = IsKeyPressed(KEY_F12);
        if (periodic || manual) {
            dc2::dumpFramePpm(tick, s_forceBlank || manual, this);
            // F59: correlate each dumped frame with the dungeon render state so the
            // bright(event)->dim(post-event) capture transition can be tied to
            // DngStatus / DngMainMap / lightScale. Gated on DC2_TRACE_F59.
            static const bool s_f59 = (std::getenv("DC2_TRACE_F59") != nullptr);
            if (s_f59) {
                uint8_t *rd = m_memory.getRDRAM();
                auto rd32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const int32_t status = (int32_t)rd32(0x01E9F6E0u);
                const uint32_t dngMap = rd32(0x003772A4u);
                const uint32_t bas = rd32(0x003772A0u);
                const uint32_t scene = rd32(0x0037729Cu);
                const uint32_t scaleU = bas ? rd32(bas + 0x6cu) : 0u;
                float scale; std::memcpy(&scale, &scaleU, 4);
                const uint32_t flags = bas ? rd32(bas + 0x08u) : 0u;
                const int32_t mapIdx = scene ? (int32_t)rd32(scene + 0x2e5cu) : 0xDEAD;
                std::fprintf(stderr,
                    "[F59:dump] tick=%llu DngStatus=%d DngMainMap=0x%x scale=%.3f flags=0x%x mapIdx=%d\n",
                    (unsigned long long)tick, status, dngMap, scale, flags, mapIdx);
            }
        }

        // F60: sample the poll/wait-primitive counters from the host present
        // loop (which keeps running even while the guest EE thread is stalled in
        // a busy-wait). Whichever delta explodes identifies the spin. Throttled
        // to ~every 30 present frames (~0.5s). Gated DC2_TRACE_F60.
        {
            static const bool s_f60 = (std::getenv("DC2_TRACE_F60") != nullptr);
            if (s_f60) {
                static uint64_t s_f60LastTick = 0;
                static uint64_t s_f60Prev[5] = {0, 0, 0, 0, 0};
                if (tick - s_f60LastTick >= 30) {
                    s_f60LastTick = tick;
                    const uint64_t cur[5] = {
                        g_f60_checkStatRpc.load(std::memory_order_relaxed),
                        g_f60_pollSema.load(std::memory_order_relaxed),
                        g_f60_dmaStat.load(std::memory_order_relaxed),
                        g_f60_gsSyncV.load(std::memory_order_relaxed),
                        g_f60_waitVSyncTick.load(std::memory_order_relaxed),
                    };
                    uint8_t *rd = m_memory.getRDRAM();
                    int32_t status;
                    std::memcpy(&status, rd + (0x01E9F6E0u & 0x01FFFFFFu), 4);
                    // Sample the EE context PC twice ~1ms apart: if it does not
                    // move while the render is frozen, the guest EE thread is
                    // pinned in a pure-memory busy-wait at this PC (no syscall);
                    // if it moves, the loop is calling something each iteration.
                    const uint32_t pcA = m_cpuContext.pc;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    const uint32_t pcB = m_cpuContext.pc;
                    // CAutoMapGen@0x1ea0480 procedural-map state. If tmplCnt==0 /
                    // template[0].parts==0, CreatRoom can never succeed and
                    // RandomMapMainProc's do/while(iVar5==0) spins forever.
                    auto rd32b = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                    const uint32_t tmplCnt = rd32b(0x1ea0480u + 0x1c8u);
                    // newlib rand seed lives at *(_impure_ptr)+0x58; _impure_ptr
                    // global = 0x333b84. If the seed does not advance between two
                    // host samples while the generator spins, rand() is stuck
                    // (constant) => iRand constant => every room places at the same
                    // cell => CreatRoom overlap-fails forever.
                    const uint32_t impurePtr = rd32b(0x333b84u);
                    const uint32_t seedA = impurePtr ? rd32b(impurePtr + 0x58u) : 0xDEADu;
                    // (the 1ms sleep already happened between pcA and pcB above)
                    const uint32_t seedB = impurePtr ? rd32b(impurePtr + 0x58u) : 0xDEADu;
                    std::fprintf(stderr,
                        "[F60:spin] tick=%llu status=%d pcA=0x%x pcB=0x%x moved=%d | dCheckRpc=%lld dPollSema=%lld dDmaStat=%lld dGsSyncV=%lld dWaitVS=%lld | tmplCnt=%u seedMoved=%d\n",
                        (unsigned long long)tick, status, pcA, pcB, (pcA != pcB) ? 1 : 0,
                        (long long)(cur[0]-s_f60Prev[0]), (long long)(cur[1]-s_f60Prev[1]),
                        (long long)(cur[2]-s_f60Prev[2]), (long long)(cur[3]-s_f60Prev[3]),
                        (long long)(cur[4]-s_f60Prev[4]),
                        tmplCnt, (seedA != seedB) ? 1 : 0);
                    for (int i = 0; i < 5; ++i) s_f60Prev[i] = cur[i];
                }
            }
        }

        // F64: watch the costume-select load phase (KeyStep@0x2bcc60 gates input on
        // MenuCosutumeLoadPhase; must reach its settled value before the menu accepts
        // a press, and resets to 1 on each costume reload). Gated DC2_TRACE_F64.
        {
            static const bool s_f64c = (std::getenv("DC2_TRACE_F64") != nullptr);
            if (s_f64c) {
                static uint64_t s_f64cLast = 0;
                static uint32_t s_f64cPrev = 0xFFFFFFFFu;
                uint8_t *rd = m_memory.getRDRAM();
                auto c32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const uint32_t ph = c32(0x01ecd62cu);
                if (ph != s_f64cPrev || tick - s_f64cLast >= 30) {
                    s_f64cLast = tick; s_f64cPrev = ph;
                    std::fprintf(stderr, "[F64:costume] tick=%llu ph62c=%u ph630=%u ph628=%u ca5c=%u\n",
                                 (unsigned long long)tick, ph, c32(0x01ecd630u), c32(0x01ecd628u), c32(0x01ecca5cu));
                }
            }
        }

        // F64 FIX: the dungeon-entrance EVENT (DngStatus==2) never completes headless,
        // G225 (2026-07-12) CORRECTION of the F64 story: the entrance script's generic
        // wait sub-function (strBase+0x378) is NOT an infinite "wait until the director
        // skips me" loop — it is a plain per-frame COUNTDOWN (op-0x7 SUB: var0 -= 1,
        // exit when var0 < 0; decoded live from PCSX2 guest memory). The whole entrance
        // cutscene runs to its final dialogue box naturally with no audio at all
        // (verified: forge-off runner frame matches ref/dumps/dungeon_0_cutscene_end.png
        // pixel-for-pixel in composition). F64's 120-iteration (~2s) done==0 detector
        // fired long before any multi-thousand-tick countdown could finish, which is what
        // truncated the cutscene and dropped the player at the staging position (G224
        // post-fix symptom). The only genuinely indefinite park is a message-window /
        // end-of-script wait (real PCSX2 parks at the same vmcode offset +0x40E0 with
        // done==0 until player input).
        //
        // So the stall detector is now PROGRESS-AWARE: it only counts consecutive present
        // iterations where the script's current vmcode_t* (+0x38) has NOT moved. Any
        // advance resets the counter. Threshold default 10800 iterations (longest
        // legitimate frozen-vmcode park measured on this route is ~3600, the countdown);
        // override via DC2_F64_STALL_TICKS (e.g. =120 for the old fast-skip behavior in
        // headless harnesses that only need free-roam). The forge action itself is
        // unchanged (halt VM + done=1 + RunMainEvent completion block — see F64 fix-log
        // for why the subtler alternatives failed).
        // Default ON; set DC2_DISABLE_EVENT_SKIP=1 to A/B the raw behavior.
        {
            static const bool s_f64disabled = (std::getenv("DC2_DISABLE_EVENT_SKIP") != nullptr);
            static const bool s_f64log = (std::getenv("DC2_TRACE_F64") != nullptr);
            if (!s_f64disabled) {
                uint8_t *rd = m_memory.getRDRAM();
                auto rr = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                auto wr = [&](uint32_t a, uint32_t v){ std::memcpy(rd + (a & 0x01FFFFFFu), &v, 4); };
                const int32_t status = (int32_t)rr(0x01E9F6E0u);
                const uint32_t script = 0x01ece3d0u;
                const uint32_t done   = rr(script + 0x3cu);     // DAT_01ece40c
                const uint32_t dngMap = rr(0x003772A4u);        // DngMainMap (floor loaded)
                // Gate to the DUNGEON ENTRANCE: map loaded (DngMainMap != 0). status==2
                // alone also matches the intro/menu events that run BEFORE the map loads.
                const bool stuckEvent = (status == 2 && dngMap != 0u && done == 0u);
                static uint32_t s_f64stuck = 0u;
                static bool s_f64forced = false;
                static uint32_t s_g225LastVm = 0u;
                static const uint32_t s_g225Threshold = [](){
                    const char *e = std::getenv("DC2_F64_STALL_TICKS");
                    if (e) { long v = std::strtol(e, nullptr, 10); if (v > 0) return (uint32_t)v; }
                    return 10800u; }();
                const uint32_t vm = rr(script + 0x38u);
                if (stuckEvent) {
                    // G225: count only while the script makes NO progress (vmcode frozen).
                    if (vm == s_g225LastVm) ++s_f64stuck; else s_f64stuck = 0u;
                    s_g225LastVm = vm;
                } else { s_f64stuck = 0u; s_g225LastVm = 0u; s_f64forced = false; }
                if (stuckEvent && s_f64stuck >= s_g225Threshold && !s_f64forced) {
                    s_f64forced = true;
                    // Halt the event script VM the way its own op-0xf end opcode does
                    // (current vmcode_t* +0x38 = 0, done +0x3c = 1, return latch = 0)...
                    wr(script + 0x38u, 0u);   // VM halts (resume__10CRunScript no-ops)
                    wr(script + 0x3cu, 1u);   // DAT_01ece40c = done
                    wr(0x01ece4fcu, 0u);      // DAT_01ece4fc return latch clear
                    // ...then reproduce RunMainEvent@0x1D1360's "EventLoop returned 1"
                    // completion block directly (DngStatus=0 + scene reset), since with
                    // the script halted EventLoop should return 1 but does not settle
                    // here headless. This is the documented free-roam-entry state.
                    wr(0x01E9F6E0u, 0u);      // DngStatus = 0 (free-roam)
                    const uint32_t bas = rr(0x003772A0u); // BattleAreaScene
                    if (bas) {
                        std::memcpy(rd + ((bas + 0x46u) & 0x01FFFFFFu), "\0\0", 2); // +0x46 (u16) = 0
                        const uint32_t f = rr(bas + 0x08u) & 0xFFFFFBFFu;           // clear bit 0x400
                        wr(bas + 0x08u, f);
                    }
                    const uint32_t dms = rr(0x0037729Cu); // DngMainScene
                    if (dms) wr(dms + 0x2e54u, 0u);       // cam idx = 0
                    if (s_f64log)
                        std::fprintf(stderr, "[F64:done][G225] forcing event end (frozen=%u thr=%u vm=0x%x) bas=0x%x dms=0x%x -> DngStatus=0\n",
                                     s_f64stuck, s_g225Threshold, vm, bas, dms);
                }
            }
        }

        // F65: dim free-roam render. After F64 forces the entrance event to complete,
        // DngStatus reaches 0 but the frame is dim (~50000 nz vs the event's ~630000).
        // DngMainDraw@0x1CF090 draws the 3D world for DngStatus in {0,2,3} gated on
        // (DngMainMap!=0 && (BattleAreaScene+8 & 0x80)==0); sky on (& 0x20)==0; lighting
        // scaled by BattleAreaScene+0x6c; camera = GetCamera(DngMainScene, +0x2e54);
        // map = GetMap(DngMainScene, +0x2e5c). Real-HW free-roam (PCSX2): flags=0x0,
        // light=1.0, cam=0, mapIdx=0. Dump the runner's values at status 0 to A/B.
        // Gated DC2_TRACE_F65.
        {
            static const bool s_f65 = (std::getenv("DC2_TRACE_F65") != nullptr);
            if (s_f65) {
                static uint64_t s_f65Last = 0;
                uint8_t *rd = m_memory.getRDRAM();
                auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const int32_t status = (int32_t)g32(0x01E9F6E0u);
                if (status == 0 && tick - s_f65Last >= 30) {
                    s_f65Last = tick;
                    const uint32_t bas = g32(0x003772A0u);
                    const uint32_t dms = g32(0x0037729Cu);
                    const uint32_t dmap = g32(0x003772A4u);
                    const uint32_t flags = bas ? g32(bas + 0x08u) : 0u;
                    const uint32_t f46 = bas ? (g32(bas + 0x44u) >> 16) : 0u;
                    uint32_t lu = bas ? g32(bas + 0x6cu) : 0u; float light; std::memcpy(&light, &lu, 4);
                    const int32_t cam = dms ? (int32_t)g32(dms + 0x2e54u) : -1;
                    const int32_t mapIdx = dms ? (int32_t)g32(dms + 0x2e5cu) : -1;
                    const uint32_t mapDrawn = dmap ? g32(dmap + 0xc4u) : 0u;
                    std::fprintf(stderr,
                        "[F65:dim] tick=%llu DngStatus=0 DngMainMap=0x%x flags=0x%x +0x46=0x%x light=%.3f cam=%d mapIdx=%d mapDrawn=%u\n",
                        (unsigned long long)tick, dmap, flags, f46, light, cam, mapIdx, mapDrawn);
                }
            }
        }

        // F66: free-roam player movement. After F65 the dungeon free-roam renders,
        // but injected movement input did not translate the player/camera. Real-HW
        // PCSX2 free-roam: MainChara(*0x003772C8) position @ +0xe0 = (785,12,3200),
        // vtable=0x3756f0; gates DAT_01e9f6e8=0, BattleAreaScene+8=0, +0x54=0.
        // Movement runs via RunScript__12CActionChara@DngMainKey gated on
        // (DAT_01e9f6e8==0 && DebugPause==0 && (BattleAreaScene+8 & 4)==0). Dump the
        // runner's MainChara ptr/pos + gates at status 0 and track the position delta
        // so we can tell "player frozen" from "player moves, camera doesn't follow".
        // Gated DC2_TRACE_F66.
        {
            static const bool s_f66 = (std::getenv("DC2_TRACE_F66") != nullptr);
            if (s_f66) {
                static uint64_t s_f66Last = 0;
                uint8_t *rd = m_memory.getRDRAM();
                auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                auto gf  = [&](uint32_t a){ float f; std::memcpy(&f, rd + (a & 0x01FFFFFFu), 4); return f; };
                const int32_t status = (int32_t)g32(0x01E9F6E0u);
                if (status == 0 && tick - s_f66Last >= 15) {
                    s_f66Last = tick;
                    const uint32_t mc  = g32(0x003772C8u);   // MainChara
                    const uint32_t bas = g32(0x003772A0u);
                    const uint32_t e8  = g32(0x01E9F6E8u);
                    const uint32_t dbgP= g32(0x00377288u);   // DebugPause (gp-0x7268, gate @0x1d1444)
                    const uint32_t vt  = mc ? g32(mc + 0x00u) : 0u;
                    const float px = mc ? gf(mc + 0xe0u) : 0.f;
                    const float py = mc ? gf(mc + 0xe4u) : 0.f;
                    const float pz = mc ? gf(mc + 0xe8u) : 0.f;
                    const uint32_t flags = bas ? g32(bas + 0x08u) : 0u;
                    const uint32_t b54   = bas ? g32(bas + 0x54u) : 0u;
                    static float s_px=0,s_py=0,s_pz=0; static bool s_have=false;
                    float dx=px-s_px, dy=py-s_py, dz=pz-s_pz;
                    float dmag = s_have ? std::sqrt(dx*dx+dy*dy+dz*dz) : 0.f;
                    s_px=px; s_py=py; s_pz=pz; s_have=true;
                    // CGamePad@0x3d76e0 analog: GetLX reads +0xc, GetLY +0x10 (raw
                    // stick byte, centre 0x80). Confirms our scePad override reaches
                    // the object the player movement reads from.
                    const int32_t cgLX = (int32_t)g32(0x3D76E0u + 0xcu); // LX (PAD_STATUS+8)
                    const int32_t cgLY = (int32_t)g32(0x3D76E0u + 0x8u); // LY (PAD_STATUS+4)
                    std::fprintf(stderr,
                        "[F66:move] tick=%llu mc=0x%x vt=0x%x pos=(%.2f,%.2f,%.2f) dmove=%.3f | e8=0x%x dbgPause=0x%x flags=0x%x +0x54=0x%x cgLX=%d cgLY=%d\n",
                        (unsigned long long)tick, mc, vt, px, py, pz, dmag, e8, dbgP, flags, b54, cgLX, cgLY);
                }
            }
        }

        // F66: drive in-dungeon free-roam pad input from the present loop. The
        // title/menu input clock (mgEndFrame-override-derived scriptFrame) freezes
        // in-dungeon, so DC2_PAD_INPUT cannot reach free-roam; this additive path
        // uses DC2_DUNGEON_PAD with its own counter that starts at free-roam entry
        // (DngStatus==0 with the floor actually loaded) and ticks every present
        // frame. Inert unless DC2_DUNGEON_PAD is set.
        // G7: poll the live host controller (XInput via raylib) once per present
        // frame on this (raylib/main) thread and publish the snapshot the guest pad
        // read consumes. Inert (live disabled) when DC2_PAD_INPUT is set explicitly
        // or DC2_NO_XINPUT=1, so deterministic automation is unaffected.
        {
            if (g_g7_poll_live_pad_hook)
                g_g7_poll_live_pad_hook();
        }

        // G55: observe the title map's steady-state draw state from the present loop
        // (the TitleMapDraw@0x2a2280 override is only consulted on INDIRECT dispatch, so
        // it cannot sample per-frame). Reads TitleMap draw-list count + map-level box gate.
        // Gated DC2_TRACE_G53; inert otherwise.
        {
            if (g_g55_title_draw_probe_hook)
                g_g55_title_draw_probe_hook(m_memory.getRDRAM());
        }

        {
            uint8_t *rd = m_memory.getRDRAM();
            auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
            const int32_t status = (int32_t)g32(0x01E9F6E0u);
            const uint32_t mc   = g32(0x003772C8u); // MainChara
            const uint32_t dmap = g32(0x003772A4u); // DngMainMap
            static uint32_t s_dframe = 0u;
            static bool s_freeroamSeen = false;
            const bool freeroam = (status == 0 && mc != 0u && dmap != 0u);
            if (freeroam)
            {
                if (!s_freeroamSeen) { s_freeroamSeen = true; s_dframe = 0u; }
                if (g_f66_drive_dungeon_pad_hook)
                    g_f66_drive_dungeon_pad_hook(s_dframe++);
            }
            else
            {
                s_freeroamSeen = false;
            }
        }

        // F63: dump the dungeon-entrance EVENT state machine during the freeze.
        // EventLoop@0x2555E0 parks on DAT_01ece504 (3=StreamOpenState/SIF sound
        // wait, 2=fade, 1=skip) and advances the script via resume__10CRunScript
        // gated by DAT_01ece500; the CRunScript done flag is DAT_01ece40c (+0x3c).
        // Gated DC2_TRACE_F63.
        {
            static const bool s_f63e = (std::getenv("DC2_TRACE_F63") != nullptr);
            if (s_f63e) {
                static uint64_t s_f63eLast = 0;
                if (tick - s_f63eLast >= 30) {
                    s_f63eLast = tick;
                    uint8_t *rd = m_memory.getRDRAM();
                    auto r32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                    const int32_t status = (int32_t)r32(0x01E9F6E0u);
                    const uint32_t d500 = r32(0x01ece500u); // event sub-state
                    const uint32_t d504 = r32(0x01ece504u); // stream/fade wait selector
                    const uint32_t d508 = r32(0x01ece508u);
                    const uint32_t d40c = r32(0x01ece40cu); // CRunScript done flag (+0x3c)
                    const uint32_t d4fc = r32(0x01ece4fcu); // EventLoop return code latch
                    const uint32_t script = 0x01ece3d0u;
                    const uint32_t vmcode = r32(script + 0x38u); // current vmcode_t* (exe runs while != 0)
                    const uint32_t yield  = r32(script + 0x50u);
                    const uint32_t op0 = vmcode ? r32(vmcode + 0u) : 0u; // opcode
                    const uint32_t op1 = vmcode ? r32(vmcode + 4u) : 0u;
                    const uint32_t op2 = vmcode ? r32(vmcode + 8u) : 0u;
                    std::fprintf(stderr,
                        "[F63:evt] tick=%llu DngStatus=%d d500=%u d504=%u d508=0x%x done(d40c)=%u retLatch(d4fc)=0x%x vmcode=0x%x yield=0x%x op=[0x%x 0x%x 0x%x]\n",
                        (unsigned long long)tick, status, d500, d504, d508, d40c, d4fc, vmcode, yield, op0, op1, op2);
                    // One-shot script disassembly around the parked vmcode: each
                    // instruction is 0xc bytes [op,arg1,arg2]. Resolve opcode-0x13
                    // (call native) to a name via strBase=+0x48 (string pool).
                    static uint32_t s_f63PrevVm = 0;
                    static uint32_t s_f63Stable = 0;
                    static uint32_t s_f63DumpedVm = 0;
                    static uint32_t s_f63DumpCount = 0;
                    if (vmcode != 0 && vmcode == s_f63PrevVm && d40c == 0)
                        ++s_f63Stable;
                    else
                        s_f63Stable = 0;
                    s_f63PrevVm = vmcode;
                    // Dump once per NEWLY-stable park location (stable >= 6 samples
                    // == ~180 ticks of an unchanging script pc == the real stall, not
                    // a transient). Re-fires if the park moves, capped at 4 dumps.
                    const bool parked = (s_f63Stable >= 6u);
                    if (parked && vmcode != s_f63DumpedVm && status == 2 && tick > 600 && s_f63DumpCount < 4u) {
                        s_f63DumpedVm = vmcode;
                        ++s_f63DumpCount;
                        const uint32_t strBase = r32(script + 0x48u);
                        const uint32_t cmdTab  = r32(script + 0x08u); // ext-command fn table
                        std::fprintf(stderr, "[F63:script] vmcode=0x%x strBase=0x%x funcTab=0x%x cmdTab=0x%x\n",
                                     vmcode, strBase, r32(script + 0x34u), cmdTab);
                        // Track the first int pushed in the current arg group so an
                        // op-0x15 (ext) can be resolved to its command id (= first
                        // pushed int) and native fn (= cmdTab[id]). F64.
                        uint32_t groupFirstInt = 0xffffffffu;
                        bool haveGroupFirst = false;
                        for (uint32_t i = 0; i < 48u; ++i) {
                            const uint32_t a = vmcode + i * 0xcu;
                            const uint32_t o = r32(a), q1 = r32(a + 4u), q2 = r32(a + 8u);
                            char nm[64]; nm[0] = 0;
                            // op3/a1==3 = push_str(strBase+arg2).
                            if (o == 0x3u && q1 == 3u && strBase) {
                                const uint32_t fd = strBase + q2;
                                for (uint32_t k = 0; k < 32u; ++k) {
                                    uint8_t ch = rd[(fd + k) & 0x01FFFFFFu];
                                    if (ch < 0x20 || ch > 0x7e) { nm[k] = 0; break; }
                                    nm[k] = (char)ch; nm[k+1] = 0;
                                }
                            }
                            // op3/a1==1 = push_int; remember first of the group.
                            if (o == 0x3u && q1 == 1u && !haveGroupFirst) {
                                groupFirstInt = q2; haveGroupFirst = true;
                            }
                            // op-0x15 = ext: id = first pushed int, fn = cmdTab[id].
                            if (o == 0x15u) {
                                const uint32_t id = haveGroupFirst ? groupFirstInt : 0xffffffffu;
                                const uint32_t fn = (cmdTab && id != 0xffffffffu) ? r32(cmdTab + id * 4u) : 0u;
                                std::snprintf(nm, sizeof(nm), "EXT id=0x%x fn=0x%x", id, fn);
                                haveGroupFirst = false; groupFirstInt = 0xffffffffu;
                            }
                            std::fprintf(stderr, "[F63:script]  +0x%02x a=0x%x op=0x%x a1=0x%x a2=0x%x %s\n",
                                         i * 0xcu, a, o, q1, q2, nm);
                        }
                        // F64: also dump the generic WAIT sub-function at
                        // strBase+0x378 (0x8ec4a8..0x8ec538, an op-0x10 back-jump
                        // loop) where the event blocks. Resolve its op-0x15 ext
                        // commands (the audio/condition query) and op-0x11/0x12
                        // conditional exit.
                        {
                            const uint32_t wbase = strBase + 0x378u;
                            std::fprintf(stderr, "[F64:wait] dumping wait loop @0x%x\n", wbase);
                            uint32_t gFirst = 0xffffffffu; bool haveG = false;
                            for (uint32_t i = 0; i < 24u; ++i) {
                                const uint32_t a = wbase + i * 0xcu;
                                const uint32_t o = r32(a), q1 = r32(a + 4u), q2 = r32(a + 8u);
                                char nm[64]; nm[0] = 0;
                                if (o == 0x3u && q1 == 3u && strBase) {
                                    const uint32_t fd = strBase + q2;
                                    for (uint32_t k = 0; k < 32u; ++k) {
                                        uint8_t ch = rd[(fd + k) & 0x01FFFFFFu];
                                        if (ch < 0x20 || ch > 0x7e) { nm[k] = 0; break; }
                                        nm[k] = (char)ch; nm[k+1] = 0;
                                    }
                                }
                                if (o == 0x3u && q1 == 1u && !haveG) { gFirst = q2; haveG = true; }
                                if (o == 0x15u) {
                                    const uint32_t id = haveG ? gFirst : 0xffffffffu;
                                    const uint32_t fn = (cmdTab && id != 0xffffffffu) ? r32(cmdTab + id * 4u) : 0u;
                                    std::snprintf(nm, sizeof(nm), "EXT id=0x%x fn=0x%x", id, fn);
                                    haveG = false; gFirst = 0xffffffffu;
                                }
                                if (o == 0x13u) { // script call: target = strBase + funcdata[0]
                                    const uint32_t fdp = strBase + q2;
                                    const uint32_t tgt = strBase + r32(fdp);
                                    std::snprintf(nm, sizeof(nm), "CALL ->0x%x", tgt);
                                }
                                std::fprintf(stderr, "[F64:wait]  +0x%02x a=0x%x op=0x%x a1=0x%x a2=0x%x %s\n",
                                             i * 0xcu, a, o, q1, q2, nm);
                            }
                        }
                    }
                }
            }
        }

        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();

    // G150 MTGS: drain + join the GS worker thread BEFORE the game thread join / GS teardown. Also
    // unblocks the game thread if it is parked in the frame-barrier backpressure wait. No-op unless
    // DC2_G150_MTGS=1. Declared here (external linkage, defined in ps2_gif_arbiter.cpp).
    extern void g150_shutdown();
    g150_shutdown();

    const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gameThreadFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < joinDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (gameThread.joinable())
    {
        if (gameThreadFinished.load(std::memory_order_acquire))
        {
            gameThread.join();
        }
        else
        {
            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;
            gameThread.detach();
        }
    }

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < workerDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        requestStop();
        const auto finalWorkerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
               std::chrono::steady_clock::now() < finalWorkerDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (g_activeThreads.load(std::memory_order_relaxed) == 0)
    {
        ps2_syscalls::joinAllGuestHostThreads();
    }
    else
    {
        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"
                  << std::endl;
        ps2_syscalls::detachAllGuestHostThreads();
    }

    UnloadTexture(frameTex);
    CloseWindow();

    if (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr)
    {
        std::cerr << "[SCPATH:summary]"
                  << " enc=" << g_scpath_enc.load(std::memory_order_relaxed)
                  << " v1=" << g_scpath_v1.load(std::memory_order_relaxed)
                  << " todo=" << g_scpath_todo.load(std::memory_order_relaxed)
                  << std::endl;
    }

    const int remainingThreads = g_activeThreads.load(std::memory_order_relaxed);
    RUNTIME_LOG("[run] exiting loop, activeThreads=" << remainingThreads);
    if (remainingThreads > 0)
    {
        std::cerr << "[run] warning: " << remainingThreads
                  << " guest worker thread(s) still active during shutdown." << std::endl;
    }
}
