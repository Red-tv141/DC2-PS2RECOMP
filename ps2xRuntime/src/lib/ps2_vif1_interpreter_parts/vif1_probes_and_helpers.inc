// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ps2_log.h"

// [G124] Ordered-interleave probe: inject the VIF1-stream MSCAL/MSCNT into the GS-side
// G123 BIND/DRAWRUN log so the bind/kick interleave is visible (DC2_G123_SEQ, title scope).
void g124_note_mscal(uint32_t startPC, const char *kind);
void g124_note_stream(uint32_t sizeBytes);
extern std::atomic<uint32_t> g_dc2G136TraceActive;
std::atomic<uint64_t> g_g146Vif1Ns{0};

// G301 MTVU: record a VU-RAM write (UNPACK/MPG/fog) as an ordered event so the VU1 worker replays it
// onto its PERSISTENT VU RAM in program order (fixes the G297 per-kick-snapshot cross-kick drop).
// No-op unless MTVU is active; VIF1 still writes live RAM (EE shadow) as before. Defined in ps2_runtime.cpp.
void g297NoteVuWrite(bool isCode, uint32_t off, const uint8_t *src, uint32_t bytes);

// G317: explicit GIF-order fence provenance for the default-off Path-2 census.
// Values mirror g317_path2_fence_census.inc without changing a public header.
void g317_note_gif_fence(uint32_t reason);
static constexpr uint32_t kG317VifFlush  = 1u << 0u;
static constexpr uint32_t kG317VifFlusha = 1u << 1u;
static constexpr uint32_t kG317VifMscal  = 1u << 2u;
static constexpr uint32_t kG317VifMscalf = 1u << 3u;
static constexpr uint32_t kG317VifMscnt  = 1u << 4u;

struct Dc2G137VuOrigin
{
    uint32_t valid;
    uint32_t seq;
    uint32_t cmdOff;
    uint32_t srcOff;
    uint32_t opcode;
    uint32_t imm;
    uint32_t destVec;
    uint32_t writeIndex;
    uint32_t srcIndex;
    uint32_t cl;
    uint32_t wl;
    uint32_t mode;
    uint32_t tops;
    uint32_t top;
    uint32_t raw[4];
    uint32_t srcRaw[4];
};

Dc2G137VuOrigin g_dc2G137VuOrigins[1024] = {};

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    struct G146PerfScope
    {
        bool on = false;
        std::chrono::steady_clock::time_point t0;
        std::atomic<uint64_t> *dst = nullptr;

        explicit G146PerfScope(std::atomic<uint64_t> &target)
        {
        static const bool s_on = (std::getenv("DC2_PERF") != nullptr) ||
                                 (std::getenv("DC2_G146_PERF") != nullptr) ||
                                 (std::getenv("DC2_G147_PERF") != nullptr);
            on = s_on;
            dst = &target;
            if (on)
                t0 = std::chrono::steady_clock::now();
        }

        ~G146PerfScope()
        {
            if (!on || !dst)
                return;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            dst->fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
        }
    };

    bool envFlagEnabled(const char *name)
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

    bool phaseDiagnosticsEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_PHASE_TRACE");
        return enabled;
    }

    bool f50_12_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_F50_12");
        return enabled;
    }

    bool vu1_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_VU1");
        return enabled;
    }

    bool g95_defer_gif_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G95_DEFER_GIF");
        return enabled;
    }

    bool g95_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G95");
        return enabled;
    }

    bool g136_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G136_TRACE");
        return enabled;
    }

    uint32_t g136_env_u32(const char *name, uint32_t fallback)
    {
        const char *value = std::getenv(name);
        if (!value || value[0] == '\0')
            return fallback;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value)
            return fallback;
        return static_cast<uint32_t>(parsed);
    }

    uint32_t g136_command_limit()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_CMDS", 180u);
        return value;
    }

    uint32_t g136_qword_limit()
    {
        static const uint32_t value = std::min<uint32_t>(g136_env_u32("DC2_G136_QW", 4u), 16u);
        return value;
    }

    bool g137_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G137_TRACE");
        return enabled;
    }

    void g136_log_qwords(const char *kind, uint32_t seq, uint32_t baseOff,
                         const uint8_t *data, uint32_t sizeBytes, uint32_t qwords)
    {
        if (!data || sizeBytes < 16u)
            return;
        const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
        for (uint32_t q = 0u; q < capped; ++q)
        {
            const uint32_t off = q * 16u;
            if (off + 16u > sizeBytes)
                break;
            uint32_t w[4] = {};
            std::memcpy(w, data + off, sizeof(w));
            std::fprintf(stderr,
                         "[G136:%s] seq=%u off=0x%x q=%u %08x %08x %08x %08x\n",
                         kind, seq, baseOff + off, q, w[0], w[1], w[2], w[3]);
        }
    }

    void g137_clear_origins()
    {
        std::memset(g_dc2G137VuOrigins, 0, sizeof(g_dc2G137VuOrigins));
    }

    void g137_record_origin(uint32_t seq, uint32_t destVec, uint32_t cmdOff, uint32_t srcOff,
                            uint32_t opcode, uint32_t imm, uint32_t writeIndex, uint32_t srcIndex,
                            uint32_t cl, uint32_t wl, uint32_t mode, uint32_t tops, uint32_t top,
                            const uint32_t raw[4], const uint8_t *srcVec, uint32_t srcBytes)
    {
        if (destVec >= 1024u || !raw)
            return;

        Dc2G137VuOrigin &origin = g_dc2G137VuOrigins[destVec];
        origin.valid = 1u;
        origin.seq = seq;
        origin.cmdOff = cmdOff;
        origin.srcOff = srcOff;
        origin.opcode = opcode;
        origin.imm = imm;
        origin.destVec = destVec;
        origin.writeIndex = writeIndex;
        origin.srcIndex = srcIndex;
        origin.cl = cl;
        origin.wl = wl;
        origin.mode = mode;
        origin.tops = tops;
        origin.top = top;
        std::memcpy(origin.raw, raw, sizeof(origin.raw));
        std::memset(origin.srcRaw, 0, sizeof(origin.srcRaw));
        if (srcVec && srcBytes > 0u)
            std::memcpy(origin.srcRaw, srcVec, std::min<uint32_t>(srcBytes, sizeof(origin.srcRaw)));
    }

    std::atomic<uint32_t> s_f517Vif1Calls{0};
    std::atomic<uint32_t> s_f517Mpg{0};
    std::atomic<uint32_t> s_f517Unpack{0};
    std::atomic<uint32_t> s_f517Mscal{0};
    std::atomic<uint32_t> s_f517Mscnt{0};
    std::atomic<uint32_t> s_f517Direct{0};
    std::atomic<uint32_t> s_f517Ops{0};
    std::atomic<uint32_t> s_f517RawMscalBytes{0};
    // G17: raw scan for a MSCNT (0x17) opcode word anywhere in the assembled stream.
    // A nonzero count means the kick word IS present in chainBuf (parse-swallow);
    // a zero count means it never reached processVIF1Data (assembly drop).
    std::atomic<uint32_t> s_f517RawMscntBytes{0};
    std::atomic<uint32_t> s_f517V432Unpack{0};
    // G39: armed when the first model MSCAL (startPc=0x10) fires, so the unpack trace logs the
    // costume-draw unpacks (incl. the bone-matrix palette to qw 0x3c) instead of being consumed
    // by the title/menu unpacks. s_g39UnpkLog caps the post-arm logging.
    std::atomic<uint32_t> s_g39Armed{0};
    std::atomic<uint32_t> s_g39UnpkLog{0};
    // G39: one-shot ordered trace of the costume-model VIF stream (BASE/OFFSET/UNPACK/MSCAL)
    // armed on the first BASE imm=0x3c (the Max model framing) so the delivery order of the
    // bone palette vs the per-mesh vertex unpacks vs the kicks is unambiguous.
    std::atomic<int> s_g39SeqArmed{0}; // 0=waiting, 1=active, 2=done
    std::atomic<uint32_t> s_g39SeqN{0};
    void g39_seq(const char *kind, uint32_t a, uint32_t b, uint32_t c)
    {
        if (s_g39SeqArmed.load(std::memory_order_relaxed) != 1) return;
        const uint32_t n = s_g39SeqN.fetch_add(1u, std::memory_order_relaxed);
        if (n < 200u)
            std::fprintf(stderr, "[G39:seq] #%u %-5s a=0x%x b=0x%x c=0x%x\n", n, kind, a, b, c);
        else
            s_g39SeqArmed.store(2, std::memory_order_relaxed);
    }

    std::atomic<uint32_t> s_debugVu1KickCount{0};
    std::atomic<uint32_t> s_debugVif1OpcodeCount{0};
    std::atomic<uint32_t> s_g22SynthVu1Kick{0};
    std::atomic<uint32_t> s_g22MpgTraceCount{0};
    constexpr uint8_t kGifFmtImage = 2u;
    constexpr uint32_t kDc2ResidentVu1Pc = 0x320u;

    enum class G22Vu1KickMode : uint8_t
    {
        Off,
        Stream,
        Unpack,
        Resume,
        Upload,
        AllStreams,
    };

    // G268: env vars are process-constant in every DC2 harness; these two ran getenv() (plus
    // strcmp chains) per VIF1 packet parse / per VU1 kick. Parsed once by default;
    // DC2_G268_LIVE_ENVREAD=1 restores the historical per-call read (A/B control).
    bool g268LiveEnvRead()
    {
        static const bool live = envFlagEnabled("DC2_G268_LIVE_ENVREAD");
        return live;
    }

    G22Vu1KickMode g22_vu1_kick_mode_uncached()
    {
        const char *value = std::getenv("DC2_G22_VU1_KICK");
        if (value == nullptr || value[0] == '\0' ||
            std::strcmp(value, "0") == 0 ||
            std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "FALSE") == 0 ||
            std::strcmp(value, "off") == 0 ||
            std::strcmp(value, "OFF") == 0)
        {
            return G22Vu1KickMode::Off;
        }

        if (std::strcmp(value, "unpack") == 0 ||
            std::strcmp(value, "UNPACK") == 0 ||
            std::strcmp(value, "candidate") == 0 ||
            std::strcmp(value, "CANDIDATE") == 0)
        {
            return G22Vu1KickMode::Unpack;
        }

        if (std::strcmp(value, "all") == 0 ||
            std::strcmp(value, "ALL") == 0)
        {
            return G22Vu1KickMode::AllStreams;
        }

        if (std::strcmp(value, "resume") == 0 ||
            std::strcmp(value, "RESUME") == 0 ||
            std::strcmp(value, "mscnt") == 0 ||
            std::strcmp(value, "MSCNT") == 0)
        {
            return G22Vu1KickMode::Resume;
        }

        if (std::strcmp(value, "upload") == 0 ||
            std::strcmp(value, "UPLOAD") == 0 ||
            std::strcmp(value, "mpg") == 0 ||
            std::strcmp(value, "MPG") == 0)
        {
            return G22Vu1KickMode::Upload;
        }

        return G22Vu1KickMode::Stream;
    }

    G22Vu1KickMode g22_vu1_kick_mode()
    {
        if (g268LiveEnvRead())
            return g22_vu1_kick_mode_uncached();
        static const G22Vu1KickMode cached = g22_vu1_kick_mode_uncached();
        return cached;
    }

    struct G22XgkickScan
    {
        uint32_t count = 0u;
        uint32_t firstPc = 0u;
        uint32_t firstLower = 0u;
        uint32_t firstUpper = 0u;
        uint32_t firstIs = 0u;
    };

    G22XgkickScan g22_scan_xgkick_ops(const uint8_t *code, uint32_t codeBytes, uint32_t basePc)
    {
        G22XgkickScan scan;
        if (!code)
            return scan;

        const uint32_t instructionCount = codeBytes / 8u;
        for (uint32_t i = 0u; i < instructionCount; ++i)
        {
            uint32_t lower = 0u;
            uint32_t upper = 0u;
            std::memcpy(&lower, code + i * 8u, sizeof(lower));
            std::memcpy(&upper, code + i * 8u + 4u, sizeof(upper));

            const uint8_t opHi = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
            const uint8_t vuFunc = static_cast<uint8_t>((((lower >> 6u) & 0x1Fu) << 2u) | (lower & 0x3u));
            if (opHi != 0x40u || vuFunc != 0x6Cu)
                continue;

            if (scan.count == 0u)
            {
                scan.firstPc = basePc + i * 8u;
                scan.firstLower = lower;
                scan.firstUpper = upper;
                scan.firstIs = (lower >> 11u) & 0x1Fu;
            }
            ++scan.count;
        }
        return scan;
    }

    bool g22_vu1_program_ready(const uint8_t *vuCode)
    {
        if (!vuCode)
            return false;

        const uint32_t begin = kDc2ResidentVu1Pc;
        const uint32_t end = begin + 64u;
        for (uint32_t off = begin; off < end; off += 4u)
        {
            uint32_t word = 0u;
            std::memcpy(&word, vuCode + off, sizeof(word));
            if (word != 0u)
                return true;
        }
        return false;
    }

    uint32_t g22_vu1_start_pc_override_uncached(uint32_t fallback)
    {
        const char *value = std::getenv("DC2_G22_VU1_START_PC");
        if (!value || value[0] == '\0')
            return fallback;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value)
            return fallback;
        return static_cast<uint32_t>(parsed) & ~7u;
    }

    uint32_t g22_vu1_start_pc_override(uint32_t fallback)
    {
        if (g268LiveEnvRead())
            return g22_vu1_start_pc_override_uncached(fallback);
        // Sentinel survives the uncached path verbatim only when the override is unset/invalid
        // (a set value is masked with ~7u and cannot equal it).
        static const uint32_t cachedRaw = g22_vu1_start_pc_override_uncached(0xFFFFFFFFu);
        return (cachedRaw == 0xFFFFFFFFu) ? fallback : cachedRaw;
    }

    uint32_t gifImageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != kGifFmtImage)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}
