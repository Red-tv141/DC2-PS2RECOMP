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

    G22Vu1KickMode g22_vu1_kick_mode()
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

    uint32_t g22_vu1_start_pc_override(uint32_t fallback)
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

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (!m_rdram || !m_gsVRAM || sizeBytes == 0u)
        return;
    if (srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    G146PerfScope g146Scope(g_g146Vif1Ns);

    if (!data || !m_gsVRAM || sizeBytes == 0u)
        return;

    g124_note_stream(sizeBytes); // [G124] DMA-transfer boundary in the ordered interleave log
    const uint32_t g136Seq = g136_trace_enabled()
        ? g_dc2G136TraceActive.load(std::memory_order_relaxed)
        : 0u;
    const bool g136Active = g136Seq != 0u;
    uint32_t g136LoggedCommands = 0u;
    uint32_t g136LoggedPayloads = 0u;
    if (g136Active)
    {
        std::fprintf(stderr, "[G136:vifstart] seq=%u size=%u\n", g136Seq, sizeBytes);
        g136_log_qwords("vifhead", g136Seq, 0u, data, sizeBytes, g136_qword_limit());
        if (g137_trace_enabled())
            g137_clear_origins();
    }

    auto g95DrainDeferredGif = [&](const char *reason)
    {
        if (!g95_defer_gif_enabled() || !m_gifArbiter)
            return;
        if (g95_trace_enabled())
        {
            static std::atomic<uint32_t> s_g95Drain{0};
            const uint32_t n = s_g95Drain.fetch_add(1u, std::memory_order_relaxed);
            if (n < 128u || (n % 1024u) == 0u)
                std::fprintf(stderr, "[G95:gifdrain] n=%u reason=%s\n", n, reason);
        }
        m_gifArbiter->drain();
    };

    // G17: first clean-MSCNT offset in this stream (filled by the trace scan, used to log the
    // parser's command boundaries around the kick so we can see what consumes it).
    uint32_t g17FirstMscntOff = 0xFFFFFFFFu;
    // G25: first clean-MSCAL offset (the costume/character model kick is MSCAL, not MSCNT).
    uint32_t g17FirstMscalOff = 0xFFFFFFFFu;

    if (vu1_trace_enabled())
    {
        const uint32_t n = s_f517Vif1Calls.fetch_add(1u, std::memory_order_relaxed);
        // Crude raw scan: count 4-byte words whose top opcode byte is MSCAL/MSCALF.
        // Over-counts (payload bytes may match) but a 0 means MSCAL is nowhere in the stream.
        for (uint32_t p = 0; p + 4u <= sizeBytes; p += 4u)
        {
            uint32_t w = 0u;
            std::memcpy(&w, data + p, 4);
            const uint8_t op = static_cast<uint8_t>((w >> 24) & 0x7Fu);
            if (op == 0x14u || op == 0x15u)
            {
                s_f517RawMscalBytes.fetch_add(1u, std::memory_order_relaxed);
                // G25: a clean MSCAL VIFcode = num field (23:16)==0 and a plausible micro
                // start address (imm < 0x800 instructions). The model's per-draw kick.
                if (g17FirstMscalOff == 0xFFFFFFFFu &&
                    ((w >> 16) & 0xFFu) == 0u && (w & 0xFFFFu) < 0x800u)
                    g17FirstMscalOff = p;
            }
            // A genuine MSCNT VIFcode has num==0 and imm==0 (kick continues resident prog).
            // Match the clean word to avoid payload noise that merely has 0x17 in the top byte.
            if (op == 0x17u && (w & 0x00FFFFFFu) == 0u)
            {
                if (g17FirstMscntOff == 0xFFFFFFFFu)
                    g17FirstMscntOff = p;
                const uint32_t r = s_f517RawMscntBytes.fetch_add(1u, std::memory_order_relaxed);
                if (r < 24u)
                {
                    uint32_t prev = 0u, prev2 = 0u;
                    if (p >= 4u) std::memcpy(&prev, data + p - 4u, 4);
                    if (p >= 8u) std::memcpy(&prev2, data + p - 8u, 4);
                    std::fprintf(stderr, "[G17:rawmscnt] call=%u off=0x%x word=0x%08x prev=0x%08x prev2=0x%08x size=%u\n",
                                 n, p, w, prev, prev2, sizeBytes);
                }
            }
        }
        // G25: announce streams that DO contain a clean MSCAL kick, so we can confirm the
        // offset exists (and whether it is later swallowed / reached).
        if (g17FirstMscalOff != 0xFFFFFFFFu && sizeBytes > 0x1000u)
        {
            static std::atomic<uint32_t> s_g25found{0};
            if (s_g25found.fetch_add(1u, std::memory_order_relaxed) < 8u)
            {
                uint32_t mw = 0u; std::memcpy(&mw, data + g17FirstMscalOff, 4);
                std::fprintf(stderr, "[G25:mscalfound] call=%u mscalOff=0x%x word=0x%08x size=%u\n",
                             n, g17FirstMscalOff, mw, sizeBytes);
            }
        }
        if (n < 64u)
        {
            uint64_t firstQw = 0u;
            if (sizeBytes >= 8u)
                std::memcpy(&firstQw, data, sizeof(firstQw));
            std::fprintf(stderr, "[F51.7:vif1] n=%u size=%u firstQw=0x%016llx\n",
                         n, sizeBytes, static_cast<unsigned long long>(firstQw));
        }
    }

    // PHASE F30: vif1cmd probe, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30Vif1Cmd{0};
        const uint32_t n = s_f30Vif1Cmd.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (n <= 12u || (n % 64u) == 0u)
        {
            uint64_t firstQw = 0u;
            if (sizeBytes >= 8u)
                std::memcpy(&firstQw, data, sizeof(firstQw));
            std::fprintf(stderr,
                         "[F30:vif1cmd] n=%u size=%u firstQw=0x%016llx\n",
                         n, sizeBytes, static_cast<unsigned long long>(firstQw));
        }
    }

    auto recomputeVif1Tops = [&]()
    {
        const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
        const uint32_t base = vif1_regs.base & 0x3FFu;
        const uint32_t ofst = vif1_regs.ofst & 0x3FFu;
        vif1_regs.tops = dbf ? ((base + ofst) & 0x3FFu) : base;
    };

    uint32_t pos = 0;
    // G27: window dump following a model BASE (small imm framing) — logs the exact command
    // sequence after the known-aligned model section-1 so we can see where the cursor desyncs
    // before the section-2 MSCAL. Gated on DC2_TRACE_VU1; capped to 8 windows.
    uint32_t g27Win = 0u;
    static std::atomic<uint32_t> s_g27Starts{0};
    // G17: track the last command parsed + where the loop ultimately stops, so we can tell
    // whether the parser desyncs / breaks early before reaching a MSCNT kick deep in the chain.
    uint8_t g17LastOpcode = 0xFF;
    uint32_t g17LastOpcodePos = 0;
    const char *g17BreakReason = "loop-end";
    // G17: ring buffer of the last commands parsed, dumped when a bogus over-long DIRECT
    // (the symptom of an upstream desync) swallows the kick.
    static constexpr uint32_t kG17Ring = 16u;
    uint32_t g17RingPos[kG17Ring] = {0};
    uint32_t g17RingCmd[kG17Ring] = {0};
    uint32_t g17RingConsumed[kG17Ring] = {0};
    uint32_t g17RingIdx = 0;
    uint32_t g17PrevPosForConsumed = 0;
    const G22Vu1KickMode g22KickMode = g22_vu1_kick_mode();
    uint32_t g22DirectCommands = 0u;
    uint32_t g22V432Unpacks = 0u;
    uint32_t g22LargeUnpacks = 0u;
    uint32_t g22Mpg320Uploads = 0u;
    auto g22KickResidentVu1 = [&](const char *reason, uint32_t cmdPos, uint8_t sourceOpcode,
                                  uint32_t writeCount, uint32_t vuAddr, uint32_t startPc)
    {
        if (g22KickMode == G22Vu1KickMode::Off || !m_vu1MscalCallback ||
            !g22_vu1_program_ready(m_vu1Code))
        {
            return;
        }

        const uint32_t kickIndex = s_g22SynthVu1Kick.fetch_add(1u, std::memory_order_relaxed);
        const bool resumeResident = (g22KickMode == G22Vu1KickMode::Resume) &&
                                    (kickIndex > 0u) &&
                                    static_cast<bool>(m_vu1MscntCallback);
        if (vu1_trace_enabled() && kickIndex < 128u)
        {
            std::fprintf(stderr,
                         "[G22:synthkick] n=%u kind=%s reason=%s size=%u cmdPos=0x%x op=0x%02x count=%u vuAddr=0x%x startPc=0x%x itop=0x%x tops=0x%x base=0x%x ofst=0x%x direct=%u v432=%u large=%u mpg320=%u\n",
                         kickIndex, resumeResident ? "resume" : "execute", reason, sizeBytes, cmdPos, static_cast<uint32_t>(sourceOpcode),
                         writeCount, vuAddr, startPc, vif1_regs.itop & 0x3FFu, vif1_regs.tops & 0x3FFu,
                         vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu,
                         g22DirectCommands, g22V432Unpacks, g22LargeUnpacks, g22Mpg320Uploads);
        }

        if (resumeResident)
            m_vu1MscntCallback(vif1_regs.itop & 0x3FFu);
        else
            m_vu1MscalCallback(startPc, vif1_regs.itop & 0x3FFu);
    };
    auto submitPath2ImageChunk = [&](const uint8_t *payload, uint32_t chunkQw, bool directHl)
    {
        if (!payload || chunkQw == 0u)
            return;

        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31VifImageDirect{0};
            const uint32_t n = s_f31VifImageDirect.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                uint64_t firstDw = 0u;
                std::memcpy(&firstDw, payload, sizeof(firstDw));
                std::fprintf(stderr,
                             "[F31:vif-image-direct] n=%u qwc=%u pendingBefore=%u hl=%u firstDw=0x%016llx\n",
                             n, chunkQw, m_vif1PendingPath2ImageQwc, directHl ? 1u : 0u,
                             static_cast<unsigned long long>(firstDw));
            }
        }

        // The GS already consumed the IMAGE tag that established its pending transfer.
        // A second tag here would be consumed as the first qword of pixel data.
        submitGifPacket(GifPathId::Path2,
                        payload,
                        chunkQw * 16u,
                        !g95_defer_gif_enabled(),
                        directHl);
    };

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            uint32_t nextCmd = 0u;
            std::memcpy(&nextCmd, data + pos, sizeof(nextCmd));
            const uint8_t nextOpcode = static_cast<uint8_t>((nextCmd >> 24) & 0x7Fu);
            uint32_t nextCmd2 = 0u;
            if (pos + 8u <= sizeBytes)
                std::memcpy(&nextCmd2, data + pos + 4u, sizeof(nextCmd2));
            const uint8_t nextOpcode2 = static_cast<uint8_t>((nextCmd2 >> 24) & 0x7Fu);
            const bool nextLooksLikeDirectWrapper =
                nextOpcode == VIF_DIRECT ||
                nextOpcode == VIF_DIRECTHL ||
                (nextOpcode == VIF_NOP && (nextOpcode2 == VIF_DIRECT || nextOpcode2 == VIF_DIRECTHL));
            if (nextLooksLikeDirectWrapper)
            {
                // Some compact chains put the next image chunk behind another VIF DIRECT.
                // Let the command parser see that wrapper instead of treating it as pixels.
                if (vu1_trace_enabled() && sizeBytes > 0x10000u)
                {
                    static std::atomic<uint32_t> s_g17wrap{0};
                    if (s_g17wrap.fetch_add(1u, std::memory_order_relaxed) < 8u)
                        std::fprintf(stderr,
                                     "[G17:wrapbail] pos=0x%x pending=%u nextCmd=0x%08x nextCmd2=0x%08x\n",
                                     pos, m_vif1PendingPath2ImageQwc, nextCmd, nextCmd2);
                }
            }
            else
            {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            submitPath2ImageChunk(data + pos, chunkQw, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
            }
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        g17LastOpcode = opcode;
        g17LastOpcodePos = pos - 4u;
        if (vu1_trace_enabled())
        {
            // backfill the consumed-span of the previous ring entry
            const uint32_t prevIdx = (g17RingIdx + kG17Ring - 1u) % kG17Ring;
            g17RingConsumed[prevIdx] = (pos - 4u) - g17PrevPosForConsumed;
            g17RingPos[g17RingIdx] = pos - 4u;
            g17RingCmd[g17RingIdx] = cmd;
            g17RingConsumed[g17RingIdx] = 0u;
            g17RingIdx = (g17RingIdx + 1u) % kG17Ring;
            g17PrevPosForConsumed = pos - 4u;
        }
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;

        if (g136Active && opcode != VIF_NOP && g136LoggedCommands < g136_command_limit())
        {
            ++g136LoggedCommands;
            std::fprintf(stderr,
                         "[G136:vifcmd] seq=%u off=0x%x op=0x%02x imm=0x%04x num=%u tops=0x%x top=0x%x base=0x%x ofst=0x%x itop=0x%x cycle=0x%04x\n",
                         g136Seq, pos - 4u, static_cast<uint32_t>(opcode), imm,
                         static_cast<uint32_t>(num), vif1_regs.tops & 0x3FFu,
                         vif1_regs.top & 0x3FFu, vif1_regs.base & 0x3FFu,
                         vif1_regs.ofst & 0x3FFu, vif1_regs.itop & 0x3FFu,
                         vif1_regs.cycle & 0xFFFFu);
        }

        if (vu1_trace_enabled() && g27Win > 0u)
        {
            std::fprintf(stderr, "[G27] pos=0x%x op=0x%02x imm=0x%04x num=%u cl=%u wl=%u\n",
                         pos - 4u, static_cast<uint32_t>(opcode), imm,
                         static_cast<uint32_t>(num),
                         vif1_regs.cycle & 0xFFu, (vif1_regs.cycle >> 8) & 0xFFu);
            --g27Win;
        }

        if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu)
        {
            const uint32_t cmdPos = pos - 4u;
            if (cmdPos + 80u >= g17FirstMscntOff && cmdPos <= g17FirstMscntOff + 16u)
            {
                static std::atomic<uint32_t> s_g17win{0};
                const uint32_t w = s_g17win.fetch_add(1u, std::memory_order_relaxed);
                if (w < 80u)
                    std::fprintf(stderr,
                                 "[G17:win] cmdPos=0x%x op=0x%02x imm=0x%04x num=%u (mscntOff=0x%x)\n",
                                 cmdPos, static_cast<uint32_t>(opcode), imm,
                                 static_cast<uint32_t>(num), g17FirstMscntOff);
            }
        }
        const bool irq = (cmd & 0x80000000u) != 0u;
        if (vu1_trace_enabled())
        {
            s_f517Ops.fetch_add(1u, std::memory_order_relaxed);
            if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
                s_f517Direct.fetch_add(1u, std::memory_order_relaxed);
        }

        const uint32_t opcodeIndex = s_debugVif1OpcodeCount.fetch_add(1, std::memory_order_relaxed);
        if (opcodeIndex < 160u)
        {
            RUNTIME_LOG("[vif1:cmd] idx=" << opcodeIndex
                                          << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                          << " imm=0x" << imm
                                          << std::dec
                                          << " num=" << static_cast<uint32_t>(num)
                                          << " irq=" << static_cast<uint32_t>(irq ? 1u : 0u)
                                          << std::endl);
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // HW/PCSX2: OFFSET sets OFST=imm, clears DBF, and resets TOPS=BASE. It does NOT
            // modify BASE. The old code set base=oldTops, which made the double-buffer TOPS
            // MARCH upward each mesh (0x3c->0xf0->0x1a4...) instead of ping-ponging, so the
            // model's VU input buffer eventually collided with its XGKICK output buffer
            // (0x1a4 qw == 0x1a40 byte) -> the transform read its own giftags -> -inf verts. (G28)
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            recomputeVif1Tops();          // tops = base (DBF now 0)
            g39_seq("OFST", imm & 0x3FFu, vif1_regs.base & 0x3FFu, vif1_regs.tops & 0x3FFu);
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g25ofs{0};
                if (s_g25ofs.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr, "[G25:base/ofst] OFFSET imm=0x%x base=0x%x size=%u\n",
                                 imm & 0x3FFu, vif1_regs.base & 0x3FFu, sizeBytes);
            }
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            vif1_regs.base = imm & 0x3FFu;
            recomputeVif1Tops();
            if (vu1_trace_enabled() && (imm & 0x3FFu) == 0x3cu &&
                s_g39SeqArmed.load(std::memory_order_relaxed) == 0)
            {
                s_g39SeqN.store(0u, std::memory_order_relaxed);
                s_g39SeqArmed.store(1, std::memory_order_relaxed);
            }
            g39_seq("BASE", imm & 0x3FFu, 0u, 0u);
            if (vu1_trace_enabled() && g27Win == 0u &&
                (imm & 0x3FFu) != 0u && (imm & 0x3FFu) < 0x100u)
            {
                const uint32_t s = s_g27Starts.fetch_add(1u, std::memory_order_relaxed);
                if (s < 8u)
                {
                    g27Win = 48u;
                    std::fprintf(stderr, "[G27:start] BASE imm=0x%x pos=0x%x size=%u\n",
                                 imm & 0x3FFu, pos - 4u, sizeBytes);
                }
            }
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g25base{0};
                if (s_g25base.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr, "[G25:base/ofst] BASE imm=0x%x size=%u\n",
                                 imm & 0x3FFu, sizeBytes);
            }
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif1_regs.itop = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            if (opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
                g95DrainDeferredGif((opcode == VIF_FLUSH) ? "FLUSH" : "FLUSHA");
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            if (opcode == VIF_MSCALF)
                g95DrainDeferredGif("MSCALF-pre");
            // G28: latch TOP = current TOPS (the buffer the preceding UNPACK filled) BEFORE
            // toggling the double-buffer flag. The VU microprogram reads its vertex/count input
            // via XTOP (= this TOP). Without it XTOP returned ITOP (0 for the model) so the
            // transform read vertices from VU addr 0 -> garbage loop bound -> runaway to maxcycles.
            vif1_regs.top = vif1_regs.tops & 0x3FFu;
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            uint32_t startPC = (uint32_t)imm * 8u;
            g39_seq("MSCAL", startPC, vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu);
            if (startPC == 0x10u && s_g39Armed.exchange(1u, std::memory_order_relaxed) == 0u)
                s_g39UnpkLog.store(600u, std::memory_order_relaxed);
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_f517Mscal.fetch_add(1u, std::memory_order_relaxed);
                if (n < 48u)
                    std::fprintf(stderr, "[F51.7:mscal] n=%u imm=0x%x startPc=0x%x itop=0x%x cbHooked=%u\n",
                                 n, imm, startPC, vif1_regs.itop, m_vu1MscalCallback ? 1u : 0u);
            }
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscal] idx=" << kickIndex
                                                << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                                << " imm=0x" << imm
                                                << " startPc=0x" << startPC
                                                << " itop=0x" << vif1_regs.itop
                                                << std::dec << std::endl);
            }
            g124_note_mscal(startPC, "MSCAL"); // [G124] interleave marker (pre-kick)
            if (g136Active)
            {
                std::fprintf(stderr,
                             "[G136:mscal] seq=%u cmdOff=0x%x startPc=0x%x imm=0x%x itop=0x%x top=0x%x tops=0x%x cb=%u\n",
                             g136Seq, pos - 4u, startPC, imm, vif1_regs.itop & 0x3FFu,
                             vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu,
                             m_vu1MscalCallback ? 1u : 0u);
            }
            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, vif1_regs.itop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_f517Mscnt.fetch_add(1u, std::memory_order_relaxed);
                if (n < 16u)
                    std::fprintf(stderr, "[F51.7:mscnt] n=%u itop=0x%x cbHooked=%u\n",
                                 n, vif1_regs.itop, m_vu1MscntCallback ? 1u : 0u);
            }
            vif1_regs.top = vif1_regs.tops & 0x3FFu; // G28: latch TOP before DBF toggle (XTOP source)
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscnt] idx=" << kickIndex
                                                << " itop=0x" << std::hex << vif1_regs.itop
                                                << " pc=resume"
                                                << std::dec << std::endl);
            }
            g124_note_mscal(0xFFFFu, "MSCNT"); // [G124] interleave marker (pre-kick)
            if (g136Active)
            {
                std::fprintf(stderr,
                             "[G136:mscnt] seq=%u cmdOff=0x%x itop=0x%x top=0x%x tops=0x%x cb=%u\n",
                             g136Seq, pos - 4u, vif1_regs.itop & 0x3FFu,
                             vif1_regs.top & 0x3FFu, vif1_regs.tops & 0x3FFu,
                             m_vu1MscntCallback ? 1u : 0u);
            }
            if (m_vu1MscntCallback)
                m_vu1MscntCallback(vif1_regs.itop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            if (vu1_trace_enabled())
                s_f517Mpg.fetch_add(1u, std::memory_order_relaxed);
            uint32_t destAddr = (uint32_t)imm * 8u;
            if (destAddr == kDc2ResidentVu1Pc)
                ++g22Mpg320Uploads;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            const uint32_t availableMpgBytes = (pos < sizeBytes) ? std::min<uint32_t>(mpgBytes, sizeBytes - pos) : 0u;
            const G22XgkickScan xgkickScan = g22_scan_xgkick_ops(data + pos, availableMpgBytes, destAddr);
            if (vu1_trace_enabled() && (destAddr == kDc2ResidentVu1Pc || xgkickScan.count != 0u))
            {
                const uint32_t traceIndex = s_g22MpgTraceCount.fetch_add(1u, std::memory_order_relaxed);
                if (traceIndex < 64u)
                {
                    uint32_t firstLower = 0u;
                    uint32_t firstUpper = 0u;
                    if (availableMpgBytes >= 8u)
                    {
                        std::memcpy(&firstLower, data + pos, sizeof(firstLower));
                        std::memcpy(&firstUpper, data + pos + 4u, sizeof(firstUpper));
                    }
                    std::fprintf(stderr,
                                 "[G22:mpg] n=%u streamSize=%u cmdPos=0x%x dest=0x%x num=%u instr=%u bytes=%u avail=%u xgkick=%u firstPc=0x%x firstIs=%u firstLower=0x%08x firstUpper=0x%08x headLower=0x%08x headUpper=0x%08x\n",
                                 traceIndex, sizeBytes, g17LastOpcodePos, destAddr,
                                 static_cast<uint32_t>(num), instructionCount, mpgBytes, availableMpgBytes,
                                 xgkickScan.count, xgkickScan.firstPc, xgkickScan.firstIs,
                                 xgkickScan.firstLower, xgkickScan.firstUpper,
                                 firstLower, firstUpper);
                }
            }
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
            }
            if (g22KickMode == G22Vu1KickMode::Upload &&
                xgkickScan.count != 0u)
            {
                const uint32_t startPc = g22_vu1_start_pc_override(destAddr);
                g22KickResidentVu1("mpg-upload", g17LastOpcodePos, opcode, instructionCount, destAddr, startPc);
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            ++g22DirectCommands;
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (vu1_trace_enabled() && imm >= 0x100u && sizeBytes > 0x10000u)
            {
                static std::atomic<uint32_t> s_g17big{0};
                if (s_g17big.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    std::fprintf(stderr, "[G17:bigdirect] cmdPos=0x%x imm=0x%04x pending=%u gifdata@0x%x:",
                                 pos - 4u, imm, m_vif1PendingPath2ImageQwc, pos);
                    for (uint32_t k = 0; k < 16u && pos + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + pos + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                const uint32_t originalQwCount = qwCount;
                if (g136Active && g136LoggedPayloads < g136_command_limit())
                {
                    ++g136LoggedPayloads;
                    uint64_t gifLo = 0u;
                    uint64_t gifHi = 0u;
                    if (pos + 8u <= sizeBytes)
                        std::memcpy(&gifLo, data + pos, sizeof(gifLo));
                    if (pos + 16u <= sizeBytes)
                        std::memcpy(&gifHi, data + pos + 8u, sizeof(gifHi));
                    const uint32_t nloop = static_cast<uint32_t>(gifLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((gifLo >> 58u) & 0x3u);
                    const uint32_t nreg = static_cast<uint32_t>((gifLo >> 60u) & 0xFu);
                    const uint32_t prim = static_cast<uint32_t>((gifLo >> 47u) & 0x7FFu);
                    const uint32_t pre = static_cast<uint32_t>((gifLo >> 46u) & 0x1u);
                    std::fprintf(stderr,
                                 "[G136:direct] seq=%u cmdOff=0x%x payloadOff=0x%x qwc=%u imm=0x%04x hl=%u truncated=%u pendingImage=%u gif(nloop=%u flg=%u nreg=%u pre=%u prim=0x%x regs=0x%016llx)\n",
                                 g136Seq, pos - 4u, pos, qwCount, imm, directHl ? 1u : 0u,
                                 truncated ? 1u : 0u, m_vif1PendingPath2ImageQwc,
                                 nloop, flg, nreg, pre, prim,
                                 static_cast<unsigned long long>(gifHi));
                    g136_log_qwords("directqw", g136Seq, pos, data + pos,
                                    (sizeBytes > pos) ? (sizeBytes - pos) : 0u, qwCount);
                }
                // PHASE F30: path2submit probe, retained only for explicit phase tracing.
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f30Path2{0};
                    const uint32_t n = s_f30Path2.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (n <= 12u || (n % 64u) == 0u)
                    {
                        uint64_t firstDw = 0u;
                        std::memcpy(&firstDw, data + pos, sizeof(firstDw));
                        std::fprintf(stderr,
                                     "[F30:path2submit] n=%u qwc=%u hl=%u firstDw=0x%016llx\n",
                                     n, qwCount, directHl ? 1u : 0u,
                                     static_cast<unsigned long long>(firstDw));
                    }
                }
                if (f50_12_trace_enabled())
                {
                    static std::atomic<uint32_t> s_f512VifDirect{0};
                    const uint32_t n = s_f512VifDirect.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 192u || (n % 1024u) == 0u)
                    {
                        uint64_t gifLo = 0u;
                        uint64_t gifHi = 0u;
                        if (pos + 8u <= sizeBytes)
                            std::memcpy(&gifLo, data + pos, sizeof(gifLo));
                        if (pos + 16u <= sizeBytes)
                            std::memcpy(&gifHi, data + pos + 8u, sizeof(gifHi));
                        std::fprintf(stderr,
                                     "[F512:vifdirect] n=%u streamSize=%u cmdOff=0x%x payloadOff=0x%x qwc=%u availQw=%u hl=%u truncated=%u pendingImage=%u gifLo=0x%016llx gifHi=0x%016llx\n",
                                     n, sizeBytes, pos - 4u, pos, qwCount, availableQw,
                                     directHl ? 1u : 0u, truncated ? 1u : 0u,
                                     m_vif1PendingPath2ImageQwc,
                                     static_cast<unsigned long long>(gifLo),
                                     static_cast<unsigned long long>(gifHi));
                    }
                }

                uint32_t payloadPos = pos;
                uint32_t payloadQw = qwCount;
                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, payloadQw);
                    submitPath2ImageChunk(data + payloadPos, chunkQw, m_vif1PendingPath2DirectHl);
                    payloadPos += chunkQw * 16u;
                    payloadQw -= chunkQw;
                    m_vif1PendingPath2ImageQwc -= chunkQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                    {
                        m_vif1PendingPath2DirectHl = false;
                    }
                }

                if (payloadQw > 0u)
                {
                    submitGifPacket(GifPathId::Path2, data + payloadPos, payloadQw * 16u, !g95_defer_gif_enabled(), directHl);

                    const uint32_t imageQw = gifImageQwcFromTag(data + payloadPos, payloadQw * 16u);
                    if (imageQw != 0u)
                    {
                        const uint32_t inlineImageQw = (payloadQw > 0u) ? (payloadQw - 1u) : 0u;
                        if (imageQw > inlineImageQw)
                        {
                            if (vu1_trace_enabled() && sizeBytes > 0x10000u)
                            {
                                static std::atomic<uint32_t> s_g17setp{0};
                                if (s_g17setp.fetch_add(1u, std::memory_order_relaxed) < 8u)
                                {
                                    uint64_t glo = 0u, ghi = 0u;
                                    std::memcpy(&glo, data + payloadPos, 8);
                                    if (payloadQw >= 1u) std::memcpy(&ghi, data + payloadPos + 8u, 8);
                                    const uint8_t flg = static_cast<uint8_t>((glo >> 58) & 0x3u);
                                    const uint8_t nreg = static_cast<uint8_t>((glo >> 60) & 0xFu);
                                    std::fprintf(stderr,
                                                 "[G17:setpending] cmdPos=0x%x payloadQw=%u imageQw=%u flg=%u nreg=%u gifLo=0x%016llx gifHi=0x%016llx newPending=%u\n",
                                                 g17LastOpcodePos, payloadQw, imageQw, flg, nreg,
                                                 static_cast<unsigned long long>(glo),
                                                 static_cast<unsigned long long>(ghi),
                                                 imageQw - inlineImageQw);
                                }
                            }
                            m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                            m_vif1PendingPath2DirectHl = directHl;
                        }
                    }
                }
                qwCount = originalQwCount;
            }

            pos += qwCount * 16;
            if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscntOff && g17FirstMscntOff < pos)
            {
                static std::atomic<uint32_t> s_g17dir{0};
                if (s_g17dir.fetch_add(1u, std::memory_order_relaxed) < 4u)
                {
                    std::fprintf(stderr,
                                 "[G17:swallow] DIRECT cmdPos=0x%x imm=0x%04x qwc=%u span=[0x%x,0x%x) mscntOff=0x%x hl=%u\n",
                                 g17LastOpcodePos, imm, qwCount, g17LastOpcodePos, pos,
                                 g17FirstMscntOff, (opcode == VIF_DIRECTHL) ? 1u : 0u);
                    // dump the DIRECT cmd word + first GIFtag (16 words from cmdPos)
                    std::fprintf(stderr, "[G17:dump@cmd]");
                    for (uint32_t k = 0; k < 16u && g17LastOpcodePos + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + g17LastOpcodePos + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump 12 words around the MSCNT site
                    const uint32_t base = (g17FirstMscntOff >= 16u) ? (g17FirstMscntOff - 16u) : 0u;
                    std::fprintf(stderr, "[G17:dump@mscnt base=0x%x]", base);
                    for (uint32_t k = 0; k < 12u && base + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + base + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump the head of the stream (should be clean STCYCL/MPG/UNPACK setup)
                    std::fprintf(stderr, "[G17:head]");
                    for (uint32_t k = 0; k < 48u && k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    std::fprintf(stderr, "\n");
                    // dump the ring of preceding commands (oldest→newest)
                    std::fprintf(stderr, "[G17:ring]\n");
                    for (uint32_t k = 0; k < kG17Ring; ++k)
                    {
                        const uint32_t ri = (g17RingIdx + k) % kG17Ring;
                        std::fprintf(stderr, "   pos=0x%06x cmd=0x%08x op=0x%02x imm=0x%04x consumed=%u\n",
                                     g17RingPos[ri], g17RingCmd[ri],
                                     static_cast<uint32_t>((g17RingCmd[ri] >> 24) & 0x7Fu),
                                     g17RingCmd[ri] & 0xFFFFu, g17RingConsumed[ri]);
                    }
                }
            }
            // G25: the model kick is MSCAL — detect a DIRECT whose span swallows it.
            if (vu1_trace_enabled() && g17FirstMscalOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscalOff && g17FirstMscalOff < pos)
            {
                static std::atomic<uint32_t> s_g25dir{0};
                if (s_g25dir.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    uint32_t mw = 0u; std::memcpy(&mw, data + g17FirstMscalOff, 4);
                    std::fprintf(stderr,
                                 "[G25:mscalswallow] DIRECT cmdPos=0x%x imm=0x%04x qwc=%u span=[0x%x,0x%x) mscalOff=0x%x mscalWord=0x%08x size=%u hl=%u\n",
                                 g17LastOpcodePos, imm, qwCount, g17LastOpcodePos, pos,
                                 g17FirstMscalOff, mw, sizeBytes, (opcode == VIF_DIRECTHL) ? 1u : 0u);
                    // Bytes leading INTO the swallowing DIRECT command (is it a real cmd or
                    // a data word the parser landed on after an upstream desync?).
                    const uint32_t base2 = (g17LastOpcodePos >= 64u) ? (g17LastOpcodePos - 64u) : 0u;
                    std::fprintf(stderr, "[G25:cmdctx base=0x%x]", base2);
                    for (uint32_t k = 0; k < 20u && base2 + k * 4u + 4u <= sizeBytes; ++k)
                    {
                        uint32_t dw = 0u; std::memcpy(&dw, data + base2 + k * 4u, 4);
                        std::fprintf(stderr, " %08x", dw);
                    }
                    // The 16-command ring leading up to the swallow (oldest -> newest): the
                    // op/consumed sequence shows where the parser sized a command wrong.
                    std::fprintf(stderr, "\n[G25:ring]\n");
                    for (uint32_t k = 0; k < kG17Ring; ++k)
                    {
                        const uint32_t ri = (g17RingIdx + k) % kG17Ring;
                        std::fprintf(stderr, "   pos=0x%06x cmd=0x%08x op=0x%02x imm=0x%04x consumed=%u\n",
                                     g17RingPos[ri], g17RingCmd[ri],
                                     static_cast<uint32_t>((g17RingCmd[ri] >> 24) & 0x7Fu),
                                     g17RingCmd[ri] & 0xFFFFu, g17RingConsumed[ri]);
                    }
                }
            }
            if (truncated)
            {
                g17BreakReason = "direct-truncated";
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            if (vu1_trace_enabled())
                s_f517Unpack.fetch_add(1u, std::memory_order_relaxed);
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;
            const bool g22IsV432 = (opcode == 0x6Cu);
            const bool g22IsLargeUnpack = (writeVectorCount >= 64u);
            if (g22IsV432)
                ++g22V432Unpacks;
            if (g22IsLargeUnpack)
                ++g22LargeUnpacks;

            // G17: for big model UNPACKs, log sizing + scan the consumed span for a VU1 kick
            // (0x14/0x15/0x17) that would indicate our source-size is too large (fill-mode
            // desync swallows the kick after the vertices).
            if (vu1_trace_enabled() && g22IsV432)
                s_f517V432Unpack.fetch_add(1u, std::memory_order_relaxed);
            if (vu1_trace_enabled() && writeVectorCount >= 64u)
            {
                static std::atomic<uint32_t> s_g17unpk{0};
                if (s_g17unpk.fetch_add(1u, std::memory_order_relaxed) < 24u)
                {
                    uint32_t firstKickOff = 0xFFFFFFFFu, firstKickWord = 0u;
                    for (uint32_t q = 4u; q + 4u <= totalBytes && pos + q + 4u <= sizeBytes; q += 4u)
                    {
                        uint32_t kw = 0u; std::memcpy(&kw, data + pos + q, 4);
                        const uint8_t kop = static_cast<uint8_t>((kw >> 24) & 0x7Fu);
                        if ((kop == 0x14u || kop == 0x15u || kop == 0x17u) && (kw & 0x00FFFFFFu) == 0u)
                        { firstKickOff = q; firstKickWord = kw; break; }
                    }
                    uint32_t nextW = 0u;
                    if (pos + totalBytes + 4u <= sizeBytes) std::memcpy(&nextW, data + pos + totalBytes, 4);
                    std::fprintf(stderr,
                                 "[G17:unpk] cmdPos=0x%x op=0x%02x num=%u cl=%u wl=%u srcVec=%u totalBytes=%u nextWord=0x%08x kickInSpan@%d=0x%08x\n",
                                 pos - 4u, static_cast<uint32_t>(opcode), writeVectorCount,
                                 cl, wl, sourceVectorCount, totalBytes, nextW,
                                 (firstKickOff == 0xFFFFFFFFu) ? -1 : static_cast<int>(firstKickOff),
                                 firstKickWord);
                }
            }

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;
            g39_seq("UNPK", vuAddr, writeVectorCount, (imm & 0x8000u) ? 1u : 0u);
            // G238: capture the map per-vertex COLOUR array's SOURCE bytes. Colour is unpacked as
            // V4-8 (vn=3, vl=2 => 4 bytes/vec, opcode 0x6E). Gated DC2_G238_COLSRC + the G138 map
            // scene gate; dump the first few source vectors so we can see whether the EE data is
            // already white (255,255,255,128 => EE build bug) or teal (unpack-scale bug).
            {
                static const bool s_g238col = (std::getenv("DC2_G238_COLSRC") != nullptr);
                extern std::atomic<bool> g_dc2G138DumpGateOpen;
                // Only V4 unpacks (vn==3) are candidate colour/pos/st streams; log all vl formats
                // with the first vector decoded both as bytes and as f32 so the colour stream
                // (values 0/128/255) is identifiable regardless of pack width.
                if (s_g238col && writeVectorCount >= 16u &&
                    g_dc2G138DumpGateOpen.load(std::memory_order_relaxed))
                {
                    static std::atomic<uint32_t> s_g238u{0};
                    const uint32_t un = s_g238u.fetch_add(1u, std::memory_order_relaxed);
                    if (un < 12u && pos + bytesPerVector * 3u <= sizeBytes)
                    {
                        const uint32_t nv = (writeVectorCount < 12u) ? writeVectorCount : 12u;
                        std::fprintf(stderr, "[G238:colsrc] u=%u vn=%u vl=%u bpv=%u dest=0x%x num=%u v:",
                                     un, (uint32_t)vn, (uint32_t)vl, bytesPerVector, vuAddr, writeVectorCount);
                        for (uint32_t v = 0; v < nv; ++v)
                        {
                            const uint32_t o = pos + v * bytesPerVector;
                            if (vl == 0u) { float f[4]; std::memcpy(f, data+o, 16); std::fprintf(stderr, " (%.0f,%.0f,%.0f,%.0f)", f[0],f[1],f[2],f[3]); }
                            else if (vl == 1u) { int16_t s[4]; std::memcpy(s, data+o, 8); std::fprintf(stderr, " (%d,%d,%d,%d)", s[0],s[1],s[2],s[3]); }
                            else { std::fprintf(stderr, " (%u,%u,%u,%u)", data[o],data[o+1],data[o+2],data[o+3]); }
                        }
                        std::fprintf(stderr, "\n"); std::fflush(stderr);
                    }
                }
            }
            if (g136Active && g136LoggedPayloads < g136_command_limit())
            {
                ++g136LoggedPayloads;
                const uint32_t lastDest = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                std::fprintf(stderr,
                             "[G136:unpack] seq=%u cmdOff=0x%x srcOff=0x%x op=0x%02x imm=0x%04x dest=0x%x..0x%x rel=%u num=%u srcVec=%u bytes=%u vn=%u vl=%u bpv=%u cl=%u wl=%u mode=%u tops=0x%x\n",
                             g136Seq, pos - 4u, pos, static_cast<uint32_t>(opcode), imm,
                             vuAddr, lastDest & 0x3FFu, (imm & 0x8000u) ? 1u : 0u,
                             writeVectorCount, sourceVectorCount, totalBytes,
                             static_cast<uint32_t>(vn), static_cast<uint32_t>(vl),
                             bytesPerVector, cl, wl, vif1_regs.mode & 3u,
                             vif1_regs.tops & 0x3FFu);
                g136_log_qwords("unpacksrc", g136Seq, pos, data + pos,
                                (sizeBytes > pos) ? (sizeBytes - pos) : 0u,
                                totalBytes / 16u);
            }
            // G62: localize EE-vs-VIF for the title-map wrong-prim bug. Scan the SOURCE qwords of
            // each UNPACK for triangle-class GIFtag templates (PRE=1, prim type in {3 tri,4 tristrip,
            // 5 trifan}) and print the prim the EE packet carries BEFORE unpack. Compare to the
            // post-unpack VU template / output prim (trifan 0x5D): source already trifan => EE build
            // bug; source tristrip/tri but output trifan => VIF unpack mangles it. Env DC2_G62_TPLSRC.
            static const bool s_g62tplsrc = (std::getenv("DC2_G62_TPLSRC") != nullptr);
            if (s_g62tplsrc)
            {
                static std::atomic<uint32_t> s_g62{0};
                const uint32_t scanQw = (writeVectorCount < 16u) ? writeVectorCount : 16u;
                for (uint32_t q = 0u; q < scanQw && pos + (q + 1u) * 16u <= sizeBytes; ++q)
                {
                    uint64_t tagLo = 0u, tagHi = 0u;
                    std::memcpy(&tagLo, data + pos + q * 16u, 8);
                    std::memcpy(&tagHi, data + pos + q * 16u + 8u, 8);
                    const bool pre = ((tagLo >> 46) & 1u) != 0u;
                    const uint32_t prim = static_cast<uint32_t>((tagLo >> 47) & 0x7FFu);
                    const uint32_t ptype = prim & 7u;
                    if (pre && (ptype == 3u || ptype == 4u || ptype == 5u) &&
                        s_g62.fetch_add(1u, std::memory_order_relaxed) < 256u)
                    {
                        std::fprintf(stderr,
                                     "[G62:tplsrc] dest=0x%x op=0x%02x num=%u +qw%u prim=0x%x type=%u nloop=%u tagLo=0x%016llx tagHi=0x%016llx\n",
                                     vuAddr, static_cast<uint32_t>(opcode), writeVectorCount, q,
                                     prim, ptype, static_cast<uint32_t>(tagLo & 0x7FFFu),
                                     static_cast<unsigned long long>(tagLo),
                                     static_cast<unsigned long long>(tagHi));
                    }
                }
            }
            // G39: for the skinned-model bone-matrix palette unpack (absolute dest qw 0x3c, large
            // num) dump the SOURCE bytes as floats. Distinguishes a zero EE source (game did not
            // compute the bone matrices headless) from a runtime unpack/cycle drop.
            if (vu1_trace_enabled() && vuAddr == 0x3cu && writeVectorCount >= 20u)
            {
                static std::atomic<uint32_t> s_g39pal{0};
                if (s_g39pal.fetch_add(1u, std::memory_order_relaxed) < 3u)
                {
                    std::fprintf(stderr, "[G39:palsrc] dest=0x%x num=%u cl=%u wl=%u rel=%u bytes=%u\n",
                                 vuAddr, writeVectorCount, cl, wl, (imm & 0x8000u) ? 1u : 0u, totalBytes);
                    for (uint32_t q = 0u; q < 8u && pos + (q + 1u) * 16u <= sizeBytes; ++q)
                    {
                        float f[4] = {0, 0, 0, 0};
                        std::memcpy(f, data + pos + q * 16u, 16);
                        std::fprintf(stderr, "[G39:palsrc] +qw%u = % .4g % .4g % .4g % .4g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                }
            }

            // G38: trace UNPACK destination vs the double-buffer TOPS so the write/read buffer
            // interleaving across multiple MSCAL kicks is visible (find why later kicks read 0).
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g38unpk{0};
                if (s_g38unpk.fetch_add(1u, std::memory_order_relaxed) < 60u)
                    std::fprintf(stderr, "[G38:unpk] dest=0x%x imm=0x%x rel=%u tops=0x%x num=%u op=0x%02x bytes=%u\n",
                                 vuAddr, imm & 0xFFFFu, (imm & 0x8000u) ? 1u : 0u,
                                 vif1_regs.tops & 0x3FFu, writeVectorCount, (uint32_t)opcode, totalBytes);
                // G39: log every unpack during the costume model draw (armed at first model kick)
                // so the bone-matrix palette upload (dest qw ~0x3c) and the per-mesh vertex unpacks
                // are visible. Flags whether the palette is ever written in the runner stream.
                const bool armedLog = s_g39UnpkLog.load(std::memory_order_relaxed) > 0u;
                // Always catch a substantial unpack into the palette/work region (qw 0x38..0x180),
                // even if it happens once at model-load before any model kick (before arming).
                static std::atomic<uint32_t> s_g39PalLog{0};
                const bool palLog = (vuAddr >= 0x38u && vuAddr <= 0x180u && writeVectorCount >= 4u &&
                                     s_g39PalLog.fetch_add(1u, std::memory_order_relaxed) < 200u);
                if (armedLog || palLog)
                {
                    if (armedLog) s_g39UnpkLog.fetch_sub(1u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G39:unpk]%s dest=0x%x rel=%u tops=0x%x num=%u cl=%u wl=%u op=0x%02x bytes=%u\n",
                                 palLog ? "PAL" : "", vuAddr, (imm & 0x8000u) ? 1u : 0u, vif1_regs.tops & 0x3FFu,
                                 writeVectorCount, cl, wl, (uint32_t)opcode, totalBytes);
                }
            }

            // G29: catch the UNPACK that fills the low matrix/constants region (qw0..0xc) where
            // the poisoned ~-FLT_MAX projection terms live. Dump the SOURCE as floats to tell a
            // bad-source (game/EE) from a bad-decode (runtime unpack).
            if (vu1_trace_enabled() && vuAddr <= 0x0Cu)
            {
                static std::atomic<uint32_t> s_g29unpk{0};
                if (s_g29unpk.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    std::fprintf(stderr, "[G29:matunpk] dest=0x%x imm=0x%x op=0x%02x num=%u cl=%u wl=%u flg=%u bytes=%u\n",
                                 vuAddr, imm, (uint32_t)opcode, writeVectorCount, cl, wl,
                                 (imm & 0x8000u) ? 1u : 0u, totalBytes);
                    const uint32_t nshow = (totalBytes / 4u) < 28u ? (totalBytes / 4u) : 28u;
                    for (uint32_t w = 0u; w < nshow && pos + (w + 1u) * 4u <= sizeBytes; w += 4u)
                    {
                        float f[4] = {0,0,0,0};
                        for (uint32_t k = 0; k < 4u && pos + (w + k + 1u) * 4u <= sizeBytes; ++k)
                            std::memcpy(&f[k], data + pos + (w + k) * 4u, 4);
                        std::fprintf(stderr, "[G29:matsrc] +%u = % .4g % .4g % .4g % .4g\n",
                                     w, f[0], f[1], f[2], f[3]);
                    }
                    // G30: dump the guest render_info global (base 0x380ec0): focal[0] + the
                    // projection input matrix at +0x10..0x4f that CreateRenderInfoPacket feeds
                    // into mgMulMatrix to produce VU qw0x4..0x7. If THIS is already -FLT_MAX the
                    // bug is the projection setup (SetRenderInfo); if it is sane the bug is in the
                    // packet-build mgMulMatrix (COP2 class). HW ref: focal=800, rows
                    // (800,0,0,0)/(-61.4,-994.3,50.08,-0.03)/(-2047,..)/(2.05e5,..,4.99e7,100).
                    if (m_rdram)
                    {
                        const uint32_t riBase = 0x00380ec0u & 0x1FFFFFFFu;
                        float focal = 0.0f; std::memcpy(&focal, m_rdram + riBase, 4);
                        std::fprintf(stderr, "[G30:ri] focal=% .6g\n", focal);
                        for (uint32_t r = 0u; r < 4u; ++r)
                        {
                            float m[4] = {0,0,0,0};
                            std::memcpy(m, m_rdram + riBase + 0x10u + r * 16u, sizeof(m));
                            std::fprintf(stderr, "[G30:ri] +0x%02x = % .6g % .6g % .6g % .6g\n",
                                         0x10u + r * 16u, m[0], m[1], m[2], m[3]);
                        }
                    }
                }
            }

            // G70: find the EE source of VU1 data qword 59 (VF29 = the per-vertex W/ADC cull
            // consts). The title reads qword 59 = 0 -> degenerate cull -> flat blue (see
            // ps2_vu1.cpp [G70:qw]). Log any UNPACK whose dest range covers qword 59 (or the
            // const qwords 21/22/30) with its EE source address + the source floats for slot 59,
            // so the same EE bytes can be read on PCSX2 (HW) for an A/B, and so we can see
            // whether qword 59 is uploaded at all on the title. Env DC2_G70; quiet by default.
            {
                static const bool s_g70vif = (std::getenv("DC2_G70") != nullptr);
                if (s_g70vif)
                {
                    const uint32_t lastDest = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                    const bool covers59 = (vuAddr <= 59u && lastDest >= 59u);
                    const bool nearConst = (vuAddr <= 60u && lastDest >= 21u);
                    if (covers59 || nearConst)
                    {
                        static std::atomic<uint32_t> s_g70vn{0};
                        if (s_g70vn.fetch_add(1u, std::memory_order_relaxed) < 120u)
                        {
                            const uint32_t eeSrc = m_rdram ? (uint32_t)(((data + pos) - m_rdram) & 0x1FFFFFFFu) : 0u;
                            float s59[4] = {0, 0, 0, 0};
                            if (covers59 && m_rdram && bytesPerVector == 16u)
                            {
                                const uint32_t srcOff = pos + (59u - vuAddr) * 16u;
                                for (uint32_t k = 0; k < 4u && srcOff + (k + 1u) * 4u <= sizeBytes; ++k)
                                    std::memcpy(&s59[k], data + srcOff + k * 4u, 4);
                            }
                            std::fprintf(stderr,
                                         "[G70:unpk] dest=0x%x..0x%x covers59=%u abs=%u eeSrc=0x%x num=%u cl=%u wl=%u bpv=%u tops=0x%x src59=% .6g % .6g % .6g % .6g\n",
                                         vuAddr, lastDest, covers59 ? 1u : 0u, (imm & 0x8000u) ? 1u : 0u, eeSrc,
                                         writeVectorCount, cl, wl, bytesPerVector, vif1_regs.tops & 0x3FFu,
                                         s59[0], s59[1], s59[2], s59[3]);
                        }
                    }
                }
            }

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            const bool g137Active = g136Active && g137_trace_enabled();
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    uint32_t srcVecIndex = 0xFFFFFFFFu;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVecIndex = srcIndex;
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        if (g137Active)
                        {
                            uint32_t rawCopy[4] = {};
                            std::memcpy(rawCopy, m_vu1Data + destOff, sizeof(rawCopy));
                            g137_record_origin(g136Seq, destVec, pos - 4u,
                                                srcVec ? static_cast<uint32_t>(pos + (srcVec - srcBase)) : 0xFFFFFFFFu,
                                                static_cast<uint32_t>(opcode), imm, writeIndex, srcVecIndex,
                                                cl, wl, vif1_regs.mode & 3u,
                                                vif1_regs.tops & 0x3FFu, vif1_regs.top & 0x3FFu,
                                                rawCopy, srcVec, bytesPerVector);
                        }
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                    if (g137Active)
                    {
                        g137_record_origin(g136Seq, destVec, pos - 4u,
                                            srcVec ? static_cast<uint32_t>(pos + (srcVec - srcBase)) : 0xFFFFFFFFu,
                                            static_cast<uint32_t>(opcode), imm, writeIndex, srcVecIndex,
                                            cl, wl, vif1_regs.mode & 3u,
                                            vif1_regs.tops & 0x3FFu, vif1_regs.top & 0x3FFu,
                                            lanes, srcVec, bytesPerVector);
                    }
                }
            }

            // G70: title flat-blue ROOT FIX (experimental, gated DC2_G70_FOG). The title's
            // per-vertex FOG const VF29 = VU data qword 59 is uploaded ZERO on the runner (the
            // headless title fog setup never runs SetFogParam, so the mgFlushRenderInfo source
            // globals 0x381e9c/a0/a4/a8 stay 0). With VF29=0 the VU's word3 packing saturates ->
            // the GS reads ADC=1 on ~73% of title verts (HW 32%) -> the whole rock cavern culls
            // -> flat blue. HW's qword 59 (read live from PCSX2 via those globals) =
            // (-191.25, 255, 0, 535500). Inject it when an UNPACK covering qword 59 carries an
            // all-zero source, to measure whether correct fog restores the title geometry.
            if (m_vu1Data)
            {
                static const bool s_g70fog = (std::getenv("DC2_G70_FOG") != nullptr);
                if (s_g70fog)
                {
                    const uint32_t lastDest70 = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                    if (vuAddr <= 59u && lastDest70 >= 59u && 59u * 16u + 16u <= PS2_VU1_DATA_SIZE)
                    {
                        float cur[4] = {0, 0, 0, 0};
                        std::memcpy(cur, m_vu1Data + 59u * 16u, 16);
                        if (cur[0] == 0.0f && cur[1] == 0.0f && cur[2] == 0.0f && cur[3] == 0.0f)
                        {
                            const float hwFog[4] = {-191.25f, 255.0f, 0.0f, 535500.0f};
                            std::memcpy(m_vu1Data + 59u * 16u, hwFog, 16);
                        }
                    }
                }
            }
            // G39: dump VU memory at the palette base (qw 0x3c) right after the bone-palette
            // unpack writes, to confirm whether the data actually landed in VU data RAM.
            if (vu1_trace_enabled() && vuAddr == 0x3cu && writeVectorCount >= 20u && m_vu1Data)
            {
                static std::atomic<uint32_t> s_g39paldst{0};
                if (s_g39paldst.fetch_add(1u, std::memory_order_relaxed) < 3u)
                {
                    for (uint32_t q = 0x3cu; q < 0x44u; ++q)
                    {
                        float f[4] = {0, 0, 0, 0};
                        std::memcpy(f, m_vu1Data + q * 16u, 16);
                        std::fprintf(stderr, "[G39:paldst] vu qw0x%x = % .4g % .4g % .4g % .4g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                }
            }
            pos += totalBytes;

            if (g22KickMode == G22Vu1KickMode::Unpack &&
                (g22IsV432 || (g22IsLargeUnpack && sizeBytes <= 0x10000u)))
            {
                g22KickResidentVu1("unpack", g17LastOpcodePos, opcode, writeVectorCount, vuAddr,
                                    g22_vu1_start_pc_override(kDc2ResidentVu1Pc));
            }

            if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscntOff && g17FirstMscntOff < pos)
            {
                static std::atomic<uint32_t> s_g17unp{0};
                if (s_g17unp.fetch_add(1u, std::memory_order_relaxed) < 32u)
                    std::fprintf(stderr,
                                 "[G17:swallow] UNPACK cmdPos=0x%x op=0x%02x imm=0x%04x num=%u bytes=%u span=[0x%x,0x%x) mscntOff=0x%x\n",
                                 g17LastOpcodePos, static_cast<uint32_t>(opcode), imm,
                                 static_cast<uint32_t>(num), totalBytes, g17LastOpcodePos, pos,
                                 g17FirstMscntOff);
            }

            if (pos > sizeBytes)
            {
                g17BreakReason = "unpack-overrun";
                break;
            }
            continue;
        }
        else
        {
            continue;
        }
    }

    if (g22KickMode == G22Vu1KickMode::Stream ||
        g22KickMode == G22Vu1KickMode::Resume ||
        g22KickMode == G22Vu1KickMode::AllStreams)
    {
        const bool deformLikeStream =
            sizeBytes >= 512u &&
            sizeBytes <= 0x10000u &&
            g22DirectCommands > 0u &&
            (g22V432Unpacks > 0u || g22LargeUnpacks > 0u);

        if (g22KickMode == G22Vu1KickMode::AllStreams || deformLikeStream)
        {
            g22KickResidentVu1((g22KickMode == G22Vu1KickMode::AllStreams) ? "all-stream" : "stream",
                               0xFFFFFFFFu, 0u, 0u, 0u,
                               g22_vu1_start_pc_override(kDc2ResidentVu1Pc));
        }
    }

    if (g136Active)
    {
        std::fprintf(stderr,
                     "[G136:vifend] seq=%u stopPos=0x%x size=%u reason=%s direct=%u v432=%u large=%u mpg320=%u loggedCmds=%u loggedPayloads=%u\n",
                     g136Seq, pos, sizeBytes, g17BreakReason, g22DirectCommands,
                     g22V432Unpacks, g22LargeUnpacks, g22Mpg320Uploads,
                     g136LoggedCommands, g136LoggedPayloads);
    }

    if (vu1_trace_enabled())
    {
        // Log only the streams big enough to plausibly carry a model chain, when the parser
        // stopped well short of the end (early break / desync) — i.e. a swallowed kick.
        static std::atomic<uint32_t> s_g17end{0};
        if (sizeBytes > 0x10000u && pos + 64u < sizeBytes)
        {
            const uint32_t e = s_g17end.fetch_add(1u, std::memory_order_relaxed);
            if (e < 32u)
                std::fprintf(stderr,
                             "[G17:parseend] e=%u stopPos=0x%x size=%u reason=%s lastOp=0x%02x lastOpPos=0x%x\n",
                             e, pos, sizeBytes, g17BreakReason,
                             static_cast<uint32_t>(g17LastOpcode), g17LastOpcodePos);
        }
    }

    if (vu1_trace_enabled())
    {
        static std::atomic<uint32_t> s_sum{0};
        const uint32_t n = s_sum.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u || (n % 256u) == 0u)
            std::fprintf(stderr, "[F51.7:vifsum] call=%u ops=%u mpg=%u unpack=%u direct=%u mscal=%u mscnt=%u rawMscalWords=%u rawMscntWords=%u\n",
                         n,
                         s_f517Ops.load(std::memory_order_relaxed),
                         s_f517Mpg.load(std::memory_order_relaxed),
                         s_f517Unpack.load(std::memory_order_relaxed),
                         s_f517Direct.load(std::memory_order_relaxed),
                         s_f517Mscal.load(std::memory_order_relaxed),
                         s_f517Mscnt.load(std::memory_order_relaxed),
                         s_f517RawMscalBytes.load(std::memory_order_relaxed),
                         s_f517RawMscntBytes.load(std::memory_order_relaxed));
        std::fprintf(stderr, "[G17:v432] count=%u\n", s_f517V432Unpack.load(std::memory_order_relaxed));
    }

    g95DrainDeferredGif("vif-end");
}
