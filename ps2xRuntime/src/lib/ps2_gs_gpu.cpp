#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_memory.h"
#include "ps2_log.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

// [G37] Shared GS-order sequence counter for the costume RTT-lifecycle trace (uploads here +
// RTT draws/composites in the rasterizer). Global so both translation units can use it.
std::atomic<uint32_t> g_g37_seq{0};
std::atomic<int> g_g37_armed{0};
bool g_g37_seq_trace = (std::getenv("DC2_G37_SEQ") != nullptr);
std::atomic<uint64_t> g_g146GsImageNs{0};
std::atomic<uint64_t> g_g146GsLocalNs{0};
std::atomic<uint64_t> g_g147GsGifPacketNs{0};
std::atomic<uint64_t> g_g147GsGifPacketCount{0};
std::atomic<uint64_t> g_g147DrawPrimitiveNs{0};
std::atomic<uint64_t> g_g147DrawPrimitiveCount{0};
std::atomic<uint64_t> g_g147GifTags{0};
std::atomic<uint64_t> g_g147PackedRegs{0};
std::atomic<uint64_t> g_g147ReglistRegs{0};
std::atomic<uint64_t> g_g147ImageBytes{0};
// G171: per-packed-regDesc (0x0-0xF) timing/count, measured around each writeRegisterPacked call
// in the packet parse loop. Default-off (DC2_G171_STAT) — isolates which register type(s) inside
// the ~28ms/frame "parserOther" serial GIF parse (G156 finding #3) actually dominate.
std::atomic<uint64_t> g_g171RegNs[16]{};
std::atomic<uint64_t> g_g171RegCount[16]{};
// G171 follow-up: A+D (packed desc 0x0E) writes an arbitrary GS register by address (hi&0xFF);
// bucket those separately by address to find which register dominates the 0x0E cost.
std::atomic<uint64_t> g_g171AdNs[256]{};
std::atomic<uint64_t> g_g171AdCount[256]{};

namespace
{
    bool g146PerfEnabled()
    {
        static const bool s_on = (std::getenv("DC2_PERF") != nullptr) ||
                                 (std::getenv("DC2_G146_PERF") != nullptr) ||
                                 (std::getenv("DC2_G147_PERF") != nullptr);
        return s_on;
    }

    bool g147PerfEnabled()
    {
        static const bool s_on = (std::getenv("DC2_G147_PERF") != nullptr);
        return s_on;
    }

    bool g171PerfEnabled()
    {
        static const bool s_on = (std::getenv("DC2_G171_STAT") != nullptr);
        return s_on;
    }

    struct G146PerfScope
    {
        bool on = false;
        std::chrono::steady_clock::time_point t0;
        std::atomic<uint64_t> *dst = nullptr;

        explicit G146PerfScope(std::atomic<uint64_t> &target)
        {
            static const bool s_on = g146PerfEnabled();
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

    struct G147CountedPerfScope
    {
        bool on = false;
        std::chrono::steady_clock::time_point t0;
        std::atomic<uint64_t> *dstNs = nullptr;
        std::atomic<uint64_t> *dstCount = nullptr;

        G147CountedPerfScope(std::atomic<uint64_t> &targetNs, std::atomic<uint64_t> &targetCount)
        {
            static const bool s_on = g147PerfEnabled();
            on = s_on;
            dstNs = &targetNs;
            dstCount = &targetCount;
            if (on)
                t0 = std::chrono::steady_clock::now();
        }

        ~G147CountedPerfScope()
        {
            if (!on || !dstNs || !dstCount)
                return;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            dstNs->fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
            dstCount->fetch_add(1u, std::memory_order_relaxed);
        }
    };

    // [G35] GIF register-sequence capture for the costume model composite. Armed when a CT32
    // 0x2720 TEX0 is bound; logs the subsequent TEX0/PRIM/XYZ register writes so we can see the
    // exact interleaving that clobbers the CT32 bind back to T8 before the sprite's vertices.
    std::atomic<int> g_g34SeqWindow{0};
    std::atomic<uint32_t> g_g34SeqCap{0};

    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    bool isHostPresentableUploadPsm(uint8_t psm)
    {
        return psm == GS_PSM_CT32 ||
               psm == GS_PSM_CT24 ||
               psm == GS_PSM_CT16 ||
               psm == GS_PSM_CT16S;
    }

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

    bool uploadPresentationFallbackEnabled()
    {
        static const bool enabled =
            envFlagEnabled("DC2_UPLOAD_PREVIEW") ||
            envFlagEnabled("DC2_FORCE_VISIBLE");
        return enabled;
    }

    // G268: diagnostic/lever env vars are process-constant in every DC2 harness, but several
    // per-vertex/per-packet hot paths still called getenv() on EVERY invocation (Windows CRT
    // getenv = env lock + linear scan, microseconds). The G268 MAP-0 census measured packed
    // XYZF2 at 5.0us/call vs 25ns for RGBAQ (whose handler has no getenv) — ~2.4us/vertex of
    // pure environment lookup in vertexKick, ~66ms/frame. Hot sites now read once;
    // DC2_G268_LIVE_ENVREAD=1 restores the historical per-call read (same-binary A/B control).
    bool g268LiveEnvRead()
    {
        static const bool live = envFlagEnabled("DC2_G268_LIVE_ENVREAD");
        return live;
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

    bool f50_12_latch_should_log(uint32_t n)
    {
        return n <= 32u || (n % 60u) == 0u;
    }

    enum class G195PresentMode : int
    {
        Default = 0,
        Crt1Only = 1,
        Crt2Only = 2,
        FieldPairCrt2Even = 3,
        FieldPairCrt1Even = 4,
    };

    G195PresentMode g195PresentationMode()
    {
        static const G195PresentMode mode = []() {
            const char *value = std::getenv("DC2_G195_PRESENT_MODE");
            if (!value || value[0] == '\0')
            {
                return G195PresentMode::Default;
            }

            if (std::strcmp(value, "crt1") == 0 || std::strcmp(value, "1") == 0)
            {
                return G195PresentMode::Crt1Only;
            }
            if (std::strcmp(value, "crt2") == 0 || std::strcmp(value, "2") == 0)
            {
                return G195PresentMode::Crt2Only;
            }
            if (std::strcmp(value, "fieldpair") == 0 || std::strcmp(value, "pair") == 0 ||
                std::strcmp(value, "3") == 0)
            {
                return G195PresentMode::FieldPairCrt2Even;
            }
            if (std::strcmp(value, "fieldpair-swap") == 0 || std::strcmp(value, "pair-swap") == 0 ||
                std::strcmp(value, "4") == 0)
            {
                return G195PresentMode::FieldPairCrt1Even;
            }
            return G195PresentMode::Default;
        }();
        return mode;
    }

    const char *g195PresentationModeName(G195PresentMode mode)
    {
        switch (mode)
        {
        case G195PresentMode::Crt1Only:
            return "crt1";
        case G195PresentMode::Crt2Only:
            return "crt2";
        case G195PresentMode::FieldPairCrt2Even:
            return "fieldpair";
        case G195PresentMode::FieldPairCrt1Even:
            return "fieldpair-swap";
        case G195PresentMode::Default:
        default:
            return "default";
        }
    }

    bool f33AdTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_AD");
        return enabled;
    }

    bool renderQualityTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_RENDER_QUALITY");
        return enabled;
    }

    bool t8UploadTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_T8_UPLOAD");
        return enabled;
    }

    // G14: tally GS primitive types + sample triangle screen coords, to see whether the
    // costume 3D Max model emits triangles (vs only the 2D UI sprites). Quiet by default.
    bool g14TraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G14");
        return enabled;
    }

    bool g31TraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G31");
        return enabled;
    }

    bool g105TitleRockTbp(uint32_t tbp)
    {
        return tbp >= 0x2720u && tbp <= 0x3960u;
    }

    float g105AdcGuard()
    {
        static const float value = []() -> float {
            const char *e = std::getenv("DC2_G105_ADC_GUARD");
            const float v = e ? static_cast<float>(std::atof(e)) : 384.0f;
            return (std::isfinite(v) && v >= 0.0f) ? v : 384.0f;
        }();
        return value;
    }

    bool g105SynthAdcEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G105_SYNTH_ADC");
        return enabled;
    }

    bool g105ResetStripEnabled()
    {
        static const bool enabled = std::getenv("DC2_G105_KEEP_QUEUE") == nullptr;
        return enabled;
    }

    bool f51T8AliasShouldLog()
    {
        static std::atomic<uint32_t> s_f51T8AliasLogs{0};
        return s_f51T8AliasLogs.fetch_add(1u, std::memory_order_relaxed) < 16u;
    }

    bool f51PendingDropShouldLog()
    {
        static std::atomic<uint32_t> s_f51PendingDropLogs{0};
        return s_f51PendingDropLogs.fetch_add(1u, std::memory_order_relaxed) < 8u;
    }

    bool looksLikePackedAdGifTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return false;

        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        std::memcpy(&tagHi, data + 8u, sizeof(tagHi));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;

        if (flg != GIF_FMT_PACKED || nloop == 0u || nreg != 1u)
            return false;

        const uint32_t packetBytes = 16u + nloop * 16u;
        return packetBytes <= sizeBytes && ((tagHi & 0xFu) == 0xEu);
    }

    bool frameRegisterTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_FRAME_REGS");
        return enabled;
    }

    bool f33TraceAdRegister(uint8_t addr)
    {
        switch (addr)
        {
        case GS_REG_PRIM:
        case GS_REG_RGBAQ:
        case GS_REG_ST:
        case GS_REG_UV:
        case GS_REG_XYZ2:
        case GS_REG_XYZ3:
        case GS_REG_XYZF2:
        case GS_REG_XYZF3:
        case GS_REG_TEX0_1:
        case GS_REG_TEX0_2:
        case GS_REG_TEX1_1:
        case GS_REG_TEX1_2:
        case GS_REG_TEX2_1:
        case GS_REG_TEX2_2:
        case GS_REG_TEXCLUT:
        case GS_REG_TEXA:
        case GS_REG_TEXFLUSH:
        case GS_REG_PRMODECONT:
        case GS_REG_PRMODE:
        case GS_REG_FRAME_1:
        case GS_REG_FRAME_2:
        case GS_REG_XYOFFSET_1:
        case GS_REG_XYOFFSET_2:
        case GS_REG_SCISSOR_1:
        case GS_REG_SCISSOR_2:
        case GS_REG_ALPHA_1:
        case GS_REG_ALPHA_2:
        case GS_REG_TEST_1:
        case GS_REG_TEST_2:
            return true;
        default:
            return false;
        }
    }

    void f33TraceAdWrite(uint32_t n, uint8_t addr, uint64_t value)
    {
        std::fprintf(stderr,
                     "[F33:ad] n=%u reg=0x%02x value=0x%016llx",
                     n, addr, static_cast<unsigned long long>(value));
        if (addr == GS_REG_PRIM)
        {
            std::fprintf(stderr,
                         " prim(type=%u iip=%u tme=%u abe=%u fst=%u ctxt=%u)",
                         static_cast<uint32_t>(value & 0x7u),
                         static_cast<uint32_t>((value >> 3u) & 1u),
                         static_cast<uint32_t>((value >> 4u) & 1u),
                         static_cast<uint32_t>((value >> 6u) & 1u),
                         static_cast<uint32_t>((value >> 8u) & 1u),
                         static_cast<uint32_t>((value >> 9u) & 1u));
        }
        else if (addr == GS_REG_RGBAQ)
        {
            std::fprintf(stderr,
                         " rgba=%02x,%02x,%02x,%02x q=0x%08x",
                         static_cast<uint32_t>(value & 0xFFu),
                         static_cast<uint32_t>((value >> 8u) & 0xFFu),
                         static_cast<uint32_t>((value >> 16u) & 0xFFu),
                         static_cast<uint32_t>((value >> 24u) & 0xFFu),
                         static_cast<uint32_t>((value >> 32u) & 0xFFFFFFFFu));
        }
        else if (addr == GS_REG_TEX0_1 || addr == GS_REG_TEX0_2)
        {
            std::fprintf(stderr,
                         " tex0(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x cpsm=0x%x csa=%u)",
                         static_cast<uint32_t>(value & 0x3FFFu),
                         static_cast<uint32_t>((value >> 14u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 20u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 26u) & 0xFu),
                         static_cast<uint32_t>((value >> 30u) & 0xFu),
                         static_cast<uint32_t>((value >> 37u) & 0x3FFFu),
                         static_cast<uint32_t>((value >> 51u) & 0xFu),
                         static_cast<uint32_t>((value >> 56u) & 0x1Fu));
        }
        else if (addr == GS_REG_UV)
        {
            std::fprintf(stderr,
                         " uv=%u,%u",
                         static_cast<uint32_t>(value & 0xFFFFu),
                         static_cast<uint32_t>((value >> 16u) & 0xFFFFu));
        }
        else if (addr == GS_REG_XYZ2 || addr == GS_REG_XYZ3)
        {
            std::fprintf(stderr,
                         " xyz=%u,%u,0x%x",
                         static_cast<uint32_t>(value & 0xFFFFu),
                         static_cast<uint32_t>((value >> 16u) & 0xFFFFu),
                         static_cast<uint32_t>((value >> 32u) & 0xFFFFFFFFu));
        }
        std::fprintf(stderr, "\n");
    }

    struct F31UploadPresentationCandidate
    {
        bool valid = false;
        GSFrameReg frame{};
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t sourceOriginX = 0u;
        uint32_t sourceOriginY = 0u;
        uint32_t nonzeroBytes = 0u;
        uint64_t score = 0u;
        std::vector<uint8_t> pixels;
    };

    F31UploadPresentationCandidate s_f31UploadPresentationCandidate{};

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dx = static_cast<uint32_t>((display64 >> 0) & 0x0FFFu);
        const uint32_t dy = static_cast<uint32_t>((display64 >> 12) & 0x07FFu);
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;

        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }

        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        GSDisplayReadOrigin origin{};
        origin.x = static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu);
        origin.y = static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu);
        return origin;
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSTransferTraversal
    {
        bool reverseX = false;
        bool reverseY = false;
    };

    GSTransferTraversal decodeTransferTraversal(uint8_t dir)
    {
        GSTransferTraversal traversal{};
        switch (dir & 0x3u)
        {
        case 1u:
            traversal.reverseY = true;
            break;
        case 2u:
            traversal.reverseX = true;
            break;
        case 3u:
            traversal.reverseX = true;
            traversal.reverseY = true;
            break;
        default:
            break;
        }
        return traversal;
    }

    uint32_t transferCoord(uint32_t start, uint32_t extent, uint32_t index, bool reverse)
    {
        if (reverse && extent != 0u)
        {
            return start + (extent - 1u - index);
        }
        return start + index;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        GSPmodeState pmode{};
        pmode.enableCrt1 = (pmode64 & 0x1ull) != 0ull;
        pmode.enableCrt2 = (pmode64 & 0x2ull) != 0ull;
        pmode.mmod = ((pmode64 >> 5) & 0x1ull) != 0ull;
        pmode.amod = ((pmode64 >> 6) & 0x1ull) != 0ull;
        pmode.slbg = ((pmode64 >> 7) & 0x1ull) != 0ull;
        pmode.alp = static_cast<uint8_t>((pmode64 >> 8) & 0xFFu);
        return pmode;
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode264)
    {
        GSSmode2State smode2{};
        smode2.interlaced = (smode264 & 0x1ull) != 0ull;
        smode2.frameMode = ((smode264 >> 1) & 0x1ull) != 0ull;
        return smode2;
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
        {
            return;
        }

        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
            {
                sourceY = height - 1u;
            }

            const uint8_t *srcRow = source.data() + (sourceY * kHostFrameWidth * 4u);
            uint8_t *dstRow = pixels.data() + (y * kHostFrameWidth * 4u);
            std::memcpy(dstRow, srcRow, width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        if (pixels.empty() || width == 0u || height == 0u)
        {
            return;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 4u + 3u] = 255u;
            }
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t r = row[x * 4u + 0u];
                const uint8_t g = row[x * 4u + 1u];
                const uint8_t b = row[x * 4u + 2u];
                if (r != 0u || g != 0u || b != 0u)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    bool clearFramebufferRect(uint8_t *vram,
                              uint32_t vramSize,
                              const GSContext &ctx,
                              uint32_t rgba)
    {
        if (!vram || vramSize == 0u || ctx.frame.fbw == 0u)
        {
            return false;
        }

        const uint32_t stride = GSInternal::fbStride(ctx.frame.fbw, ctx.frame.psm);
        if (stride == 0u)
        {
            return false;
        }

        const int x0 = std::max<int>(0, ctx.scissor.x0);
        const int x1 = std::max<int>(x0, ctx.scissor.x1);
        const int y0 = std::max<int>(0, ctx.scissor.y0);
        const int y1 = std::max<int>(y0, ctx.scissor.y1);
        const uint32_t base = ctx.frame.fbp * 8192u;

        uint8_t r = static_cast<uint8_t>(rgba & 0xFFu);
        uint8_t g = static_cast<uint8_t>((rgba >> 8) & 0xFFu);
        uint8_t b = static_cast<uint8_t>((rgba >> 16) & 0xFFu);
        uint8_t a = static_cast<uint8_t>((rgba >> 24) & 0xFFu);
        if ((ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
        {
            const uint32_t srcPixel =
                static_cast<uint32_t>(r) |
                (static_cast<uint32_t>(g) << 8) |
                (static_cast<uint32_t>(b) << 16) |
                (static_cast<uint32_t>(a) << 24);
            const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const uint32_t off =
                        GSPSMCT32::addrPSMCT32(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                               widthBlocks,
                                               static_cast<uint32_t>(x),
                                               static_cast<uint32_t>(y));
                    if (off + 4u > vramSize)
                    {
                        return true;
                    }

                    uint32_t pixel = srcPixel;
                    if (ctx.frame.fbmsk != 0u)
                    {
                        uint32_t existing = 0u;
                        std::memcpy(&existing, vram + off, sizeof(existing));
                        pixel = (pixel & ~ctx.frame.fbmsk) | (existing & ctx.frame.fbmsk);
                    }
                    // G219 (default-off, DC2_G219_SKYPX=1): rect-clear watch, zoom work-page window.
                    {
                        static const bool s_g219cw = (std::getenv("DC2_G219_SKYPX") != nullptr);
                        if (s_g219cw && off >= 0x278400u && off < 0x278440u)
                        {
                            static std::atomic<uint32_t> s_g219cwn{0};
                            const uint32_t n = s_g219cwn.fetch_add(1u, std::memory_order_relaxed);
                            if (n < 200u || (n % 512u) == 0u)
                                std::fprintf(stderr,
                                    "[G219:rcx] n=%u off=0x%x xy=(%d,%d) fbp=0x%x val=%08x\n",
                                    n, off, x, y, ctx.frame.fbp, pixel);
                        }
                    }
                    std::memcpy(vram + off, &pixel, sizeof(pixel));
                }
            }
            return true;
        }

        if (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S)
        {
            const uint16_t srcPixel = encodeFramePixelPSMCT16(r, g, b, a);
            const uint16_t mask = static_cast<uint16_t>(ctx.frame.fbmsk & 0xFFFFu);
            const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
            const uint32_t basePtr = GSInternal::framePageBaseToBlock(ctx.frame.fbp);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const uint32_t off = addrPSMCT16Family(basePtr,
                                                           widthBlocks,
                                                           ctx.frame.psm,
                                                           static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y));
                    if (off + 2u > vramSize)
                    {
                        return true;
                    }

                    uint16_t pixel = srcPixel;
                    if (mask != 0u)
                    {
                        uint16_t existing = 0u;
                        std::memcpy(&existing, vram + off, sizeof(existing));
                        pixel = static_cast<uint16_t>((pixel & ~mask) | (existing & mask));
                    }
                    std::memcpy(vram + off, &pixel, sizeof(pixel));
                }
            }
            return true;
        }

        return false;
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
    uint32_t s_pendingImageBytes = 0u;

    // [G123] Ordered TEX0-bind vs geometry-draw interleave log (title scope, DC2_G123_SEQ).
    // G122 proved the per-batch TEX0 register WRITES match HW (same pages incl. 0x3960, same
    // order) yet the cavern geometry never DRAWS under 0x3960 (HW draws the whole cavern there).
    // This log emits, in GS-processing order, every rock-range TEX0 BIND and the aggregated
    // tri-class DRAW run that follows it, so the actual bind/kick pairing is visible: does any
    // geometry draw right after the 0x3960 bind (HW) or only after a later/dominant bind (runner
    // de-interleave)? `g123_hw_seq.py` produces the same sequence from the .gs ground truth.
    bool g123SeqEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G123_SEQ");
        return enabled;
    }
    // [G124] event cap tunable so a FULL title frame can be captured (default 2000).
    uint32_t g123Cap()
    {
        static const uint32_t cap = []() -> uint32_t {
            const char *v = std::getenv("DC2_G123_CAP");
            if (v && v[0]) { const long n = std::strtol(v, nullptr, 10); if (n > 0) return (uint32_t)n; }
            return 2000u;
        }();
        return cap;
    }
    std::mutex s_g123Mtx;
    std::atomic<uint32_t> s_g123Seq{0};
    uint32_t s_g123RunTbp = 0xFFFFFFFFu;
    uint32_t s_g123RunPrim = 0xFFFFFFFFu;
    uint64_t s_g123RunDraw = 0u;
    uint64_t s_g123RunNodraw = 0u;
    uint32_t s_g123Events = 0u;
    // Caller must hold s_g123Mtx.
    void g123FlushRun()
    {
        if (s_g123RunTbp != 0xFFFFFFFFu && (s_g123RunDraw || s_g123RunNodraw) &&
            s_g123Events < g123Cap())
        {
            static const char *kPrim[8] =
                {"pt", "ln", "lnst", "tri", "tstrip", "tfan", "spr", "?"};
            const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G123:seq] %u DRAWRUN tbp=0x%x prim=%s draw=%llu nodraw=%llu\n",
                         seq, s_g123RunTbp, kPrim[s_g123RunPrim & 7u],
                         (unsigned long long)s_g123RunDraw,
                         (unsigned long long)s_g123RunNodraw);
            ++s_g123Events;
        }
        s_g123RunTbp = 0xFFFFFFFFu;
        s_g123RunPrim = 0xFFFFFFFFu;
        s_g123RunDraw = 0u;
        s_g123RunNodraw = 0u;
    }

    bool supportsFormatAwareLocalCopy(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        case GS_PSM_T8:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return true;
        default:
            return false;
        }
    }

    uint32_t readTransferPixel(const uint8_t *vram,
                               uint32_t vramSize,
                               uint32_t basePtr,
                               uint8_t widthBlocks,
                               uint8_t psm,
                               uint32_t x,
                               uint32_t y)
    {
        const uint32_t width = (widthBlocks != 0u) ? static_cast<uint32_t>(widthBlocks) : 1u;
        const uint32_t base = basePtr * 256u;

        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 4u > vramSize)
                return 0u;
            uint32_t value = 0u;
            std::memcpy(&value, vram + off, sizeof(value));
            return value;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 3u > vramSize)
                return 0u;
            return static_cast<uint32_t>(vram[off + 0u]) |
                   (static_cast<uint32_t>(vram[off + 1u]) << 8) |
                   (static_cast<uint32_t>(vram[off + 2u]) << 16);
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            const uint32_t off = addrPSMCT16Family(basePtr, width, psm, x, y);
            if (off + 2u > vramSize)
                return 0u;
            uint16_t value = 0u;
            std::memcpy(&value, vram + off, sizeof(value));
            return value;
        }
        case GS_PSM_T8:
        {
            const uint32_t off = GSPSMT8::addrPSMT8(basePtr, width, x, y);
            return (off < vramSize) ? vram[off] : 0u;
        }
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 4u > vramSize)
                return 0u;
            const uint8_t high = vram[off + 3u];
            return (psm == GS_PSM_T4HH) ? ((high >> 4u) & 0x0Fu) : (high & 0x0Fu);
        }
        case GS_PSM_T4:
        {
            const uint32_t nibbleAddr = GSPSMT4::addrPSMT4(basePtr, width, x, y);
            const uint32_t byteOff = nibbleAddr >> 1;
            if (byteOff >= vramSize)
                return 0u;
            const int shift = static_cast<int>((nibbleAddr & 1u) << 2);
            return static_cast<uint32_t>((vram[byteOff] >> shift) & 0x0Fu);
        }
        default:
            return 0u;
        }
    }

    void writeTransferPixel(uint8_t *vram,
                            uint32_t vramSize,
                            uint32_t basePtr,
                            uint8_t widthBlocks,
                            uint8_t psm,
                            uint32_t x,
                            uint32_t y,
                            uint32_t value)
    {
        const uint32_t width = (widthBlocks != 0u) ? static_cast<uint32_t>(widthBlocks) : 1u;
        const uint32_t base = basePtr * 256u;

        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 4u > vramSize)
                return;
            // G219 (default-off, DC2_G219_SKYPX=1): transfer-write watch for the zoom work-page
            // clobber window (see [G219:wpx] in the rasterizer).
            {
                static const bool s_g219tw = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219tw && off >= 0x278400u && off < 0x278440u)
                {
                    static std::atomic<uint32_t> s_g219twn{0};
                    const uint32_t n = s_g219twn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 200u || (n % 512u) == 0u)
                        std::fprintf(stderr,
                            "[G219:twx] n=%u off=0x%x xy=(%u,%u) dbp=0x%x dbw=%u psm=0x%x val=%08x\n",
                            n, off, x, y, basePtr, width, psm, value);
                }
            }
            std::memcpy(vram + off, &value, sizeof(value));
            return;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 3u > vramSize)
                return;
            vram[off + 0u] = static_cast<uint8_t>(value & 0xFFu);
            vram[off + 1u] = static_cast<uint8_t>((value >> 8) & 0xFFu);
            vram[off + 2u] = static_cast<uint8_t>((value >> 16) & 0xFFu);
            return;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            const uint32_t off = addrPSMCT16Family(basePtr, width, psm, x, y);
            if (off + 2u > vramSize)
                return;
            const uint16_t value16 = static_cast<uint16_t>(value & 0xFFFFu);
            std::memcpy(vram + off, &value16, sizeof(value16));
            return;
        }
        case GS_PSM_T8:
        {
            const uint32_t off = GSPSMT8::addrPSMT8(basePtr, width, x, y);
            if (off < vramSize)
                vram[off] = static_cast<uint8_t>(value & 0xFFu);
            return;
        }
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(basePtr, width, x, y);
            if (off + 4u > vramSize)
                return;
            const uint8_t nibble = static_cast<uint8_t>(value & 0x0Fu);
            uint8_t &high = vram[off + 3u];
            if (psm == GS_PSM_T4HH)
                high = static_cast<uint8_t>((high & 0x0Fu) | (nibble << 4u));
            else
                high = static_cast<uint8_t>((high & 0xF0u) | nibble);
            return;
        }
        case GS_PSM_T4:
        {
            const uint32_t nibbleAddr = GSPSMT4::addrPSMT4(basePtr, width, x, y);
            const uint32_t byteOff = nibbleAddr >> 1;
            if (byteOff >= vramSize)
                return;
            const uint8_t nibble = static_cast<uint8_t>(value & 0x0Fu);
            uint8_t &dst = vram[byteOff];
            if ((nibbleAddr & 1u) != 0u)
                dst = static_cast<uint8_t>((dst & 0x0Fu) | (nibble << 4));
            else
                dst = static_cast<uint8_t>((dst & 0xF0u) | nibble);
            return;
        }
        default:
            return;
        }
    }
}

using namespace GSInternal;

// [G124] VIF1-stream MSCAL marker injected into the G123 ordered interleave log.
// Called synchronously from the VIF1 MSCAL/MSCNT handler right BEFORE VU1 runs (and
// therefore before the XGKICK drains to the GS), so the kick appears in GS-processing
// order between the TEX0 binds and the draws it produces. G123 (GS side) showed the
// cavern's prim-3 tri collapses onto 0x2720 instead of 0x3960, but could not show
// whether kicks INTERLEAVE with binds (a bind that draws nothing leaves no DRAWRUN).
// This marker resolves it: if the cavern MSCAL appears right after a lone 0x3960 BIND
// (HW) vs only after a run of batched binds ending at 0x2720 (runner de-interleave),
// we know whether the EE stream batches binds before kicks. Same DC2_G123_SEQ gate.
// Defined at GLOBAL scope (after `using namespace GSInternal`) so the GSInternal helpers
// resolve via the using-directive and the GLOBAL g_dc2TitleRockScope extern binds to its
// real definition (an extern inside the namespace would bind to GSInternal:: instead).
extern std::atomic<bool> g_dc2TitleRockScope;
void g124_note_mscal(uint32_t startPC, const char *kind)
{
    if (!g123SeqEnabled() || !g_dc2TitleRockScope.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lk(s_g123Mtx);
    g123FlushRun();
    if (s_g123Events < g123Cap())
    {
        const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[G123:seq] %u %s pc=0x%x\n", seq, kind, startPC);
        ++s_g123Events;
    }
}

// [G124] VIF1 DMA-transfer boundary marker, injected into the same ordered log. Lets us
// tell whether the offending TEX0 rebind (e.g. 0x3720) and the geometry MSCAL live in the
// SAME VIF1 transfer as the 0x3960 bind (→ EE draw-build emits binds/kicks in this order,
// HW would too unless its input state diverges) or in a SEPARATE transfer (→ a runtime
// DMA-channel/GIF-path interleave the runner serializes differently than HW's arbiter).
void g124_note_stream(uint32_t sizeBytes)
{
    if (!g123SeqEnabled() || !g_dc2TitleRockScope.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lk(s_g123Mtx);
    g123FlushRun();
    if (s_g123Events < g123Cap())
    {
        const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[G123:seq] %u STREAM size=%u\n", seq, sizeBytes);
        ++s_g123Events;
    }
}

// G176: presentation-latch generation counter. The host present thread's UploadFrame
// re-did the full snapshot copy + UpdateTexture GPU upload every host tick (~60Hz) even
// though the latched snapshot only changes when latchHostPresentationFrame runs (once per
// guest frame at the mgEndFrame boundary since G175, ~6.5Hz on the title). The counter
// lets present skip that redundant work when the snapshot is unchanged. Incremented AFTER
// the snapshot fields are written, still under s_g189HostPresentMutex (the bump-guard in
// latchHostPresentationFrame), so a reader that observes a new generation and then takes
// s_g189HostPresentMutex for the copy always sees the snapshot that generation describes (a
// reader racing an in-progress latch at worst re-uploads one duplicate frame, never misses one).
// File-static + extern accessor — same no-header idiom as g150_*/g175_*.
static std::atomic<uint64_t> s_g176LatchGeneration{0ull};

// G189 (2026-07-10): a DEDICATED mutex for the published host-presentation snapshot
// (m_hostPresentationFrame + its w/h/fbp/preferred fields + m_hasHostPresentationFrame),
// separate from the general recursive GS::m_stateMutex. Root cause: under
// DC2_G157_PIPELINE=1 on a cheap/idle screen, the GS worker thread calls writeRegister()
// (which locks m_stateMutex) at very high frequency while it's almost never idle; the present
// thread's copyLatchedHostPresentationFrame() also needed m_stateMutex just to read this one
// small published snapshot, and lost the race against the worker's near-constant lock churn
// for seconds to minutes at a stretch (a lock-convoy/starvation hang, not a deadlock — see
// plans/phase-G189-fix-log.md). This snapshot's real writer/reader relationship is:
// latchHostPresentationFrame() (worker thread, ~once per guest frame) writes it,
// copyLatchedHostPresentationFrame() (present thread, ~once per host tick) reads it, and
// GS::reset() clears it once at startup — none of that needs to be serialized against the
// GS worker's own VRAM/register-write traffic (the worker is documented elsewhere as the SOLE
// writer of GS VRAM/state, so latchHostPresentationFrame's own VRAM reads need no mutual
// exclusion against itself; the presentation registers it reads from m_privRegs are already
// written by the EE thread WITHOUT any mutex, per ps2_memory.cpp's writeIORegister — gated
// only by the G157 credit-wait timing, not by m_stateMutex). Splitting this into its own
// low-traffic mutex removes the present thread from contending with writeRegister()'s
// per-register calls entirely; it now only ever contends with the far rarer per-frame latch.
static std::mutex s_g189HostPresentMutex;

uint64_t g176_present_latch_generation()
{
    return s_g176LatchGeneration.load(std::memory_order_acquire);
}

GS::GS()
{
    // F62: build the GSMem swizzle lookup tables once before any upload/sample
    // routes through GSMem::Write*/Read* (PR #132 wiring).
    GSMem::InitLookupTables();
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_privRegs = privRegs;
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_hwregX = 0;
    m_hwregY = 0;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    // G189: the host-presentation snapshot fields are guarded by the dedicated
    // s_g189HostPresentMutex, not m_stateMutex (already held above) — nest the lock rather
    // than reordering, since this is the only site that ever takes both (safe: every other
    // site takes at most one of the two, so there is no lock-order-inversion risk).
    {
        std::lock_guard<std::mutex> presentLock(s_g189HostPresentMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }
    // G176: reset clears the snapshot — advance the generation so present re-reads it.
    s_g176LatchGeneration.fetch_add(1ull, std::memory_order_release);
    s_pendingImageBytes = 0u;
    s_f31UploadPresentationCandidate = {};

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_vram || m_vramSize == 0)
        return;
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.resize(m_vramSize);
    std::memcpy(m_displaySnapshot.data(), m_vram, m_vramSize);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

bool GS::copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha,
                                     bool useLocalMemoryLayout,
                                     bool frameBaseIsPages,
                                     uint32_t sourceOriginX,
                                     uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
    {
        return false;
    }

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);

    const uint32_t baseBytes = frameBaseIsPages ? (frame.fbp * 8192u) : (frame.fbp * 256u);
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;

    if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
    {
        const uint32_t srcPixelBytes = (frame.psm == GS_PSM_CT24) ? 3u : 4u;
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(basePtr, fbwBlocks, srcX, srcY);
                    if (srcOff + srcPixelBytes > m_vramSize)
                    {
                        return false;
                    }

                    dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                    dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                    dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                    dstRow[x * 4u + 3u] =
                        (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dstRow = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * srcPixelBytes);
                if (srcOff + srcPixelBytes > m_vramSize)
                {
                    return false;
                }

                dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                dstRow[x * 4u + 3u] =
                    (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
            }
        }
        return true;
    }

    if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
    {
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t dstOff = y * kHostFrameWidth * 4u;
                uint8_t *dst = outPixels.data() + dstOff;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;
                    const uint32_t srcOff = addrPSMCT16Family(basePtr, fbwBlocks, frame.psm, srcX, srcY);
                    if (srcOff + sizeof(uint16_t) > m_vramSize)
                    {
                        return false;
                    }

                    uint16_t pixel = 0u;
                    std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                    const uint32_t r = pixel & 31u;
                    const uint32_t g = (pixel >> 5) & 31u;
                    const uint32_t b = (pixel >> 10) & 31u;
                    dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                    dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                    dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                    dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dst = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * 2u);
                if (srcOff + sizeof(uint16_t) > m_vramSize)
                {
                    return false;
                }

                uint16_t pixel = 0u;
                std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                const uint32_t r = pixel & 31u;
                const uint32_t g = (pixel >> 5) & 31u;
                const uint32_t b = (pixel >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
            }
        }
        return true;
    }

    return false;
}

void GS::latchHostPresentationFrame()
{
    // G189: dedicated mutex, not m_stateMutex — see s_g189HostPresentMutex's comment above.
    std::lock_guard<std::mutex> lock(s_g189HostPresentMutex);
    // Bump on EVERY exit path (success, dual-merge, clear-on-invalid) — any call can
    // change the snapshot. Destructs before the lock_guard releases s_g189HostPresentMutex.
    struct G176LatchBump
    {
        ~G176LatchBump() { s_g176LatchGeneration.fetch_add(1ull, std::memory_order_release); }
    } g176Bump;
    (void)g176Bump;

    // PHASE F30: latch state probe, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30Latch{0};
        const uint32_t n = s_f30Latch.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (n <= 6u || (n % 60u) == 0u)
        {
            const uint64_t dispfb1 = m_privRegs ? m_privRegs->dispfb1 : 0ull;
            const uint64_t dispfb2 = m_privRegs ? m_privRegs->dispfb2 : 0ull;
            const uint64_t pmode   = m_privRegs ? m_privRegs->pmode   : 0ull;
            std::fprintf(stderr,
                         "[F30:latch] n=%u pmode=0x%llx dispfb1=0x%llx dispfb2=0x%llx "
                         "ctx0.frame.fbp=0x%x fbw=%u psm=%u "
                         "ctx1.frame.fbp=0x%x fbw=%u psm=%u\n",
                         n,
                         static_cast<unsigned long long>(pmode),
                         static_cast<unsigned long long>(dispfb1),
                         static_cast<unsigned long long>(dispfb2),
                         m_ctx[0].frame.fbp, m_ctx[0].frame.fbw, m_ctx[0].frame.psm,
                         m_ctx[1].frame.fbp, m_ctx[1].frame.fbw, m_ctx[1].frame.psm);
        }
    }

    if (!m_privRegs || !m_vram || m_vramSize == 0u)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    const bool applyFieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (ps2_syscalls::GetCurrentVSyncTick() & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const GSDisplayReadOrigin displayOrigin1 = decodeDisplayReadOrigin(m_privRegs->dispfb1);
    const GSDisplayReadOrigin displayOrigin2 = decodeDisplayReadOrigin(m_privRegs->dispfb2);

    uint32_t width1 = 0u;
    uint32_t height1 = 0u;
    uint32_t width2 = 0u;
    uint32_t height2 = 0u;
    decodeDisplaySize(m_privRegs->display1, width1, height1);
    decodeDisplaySize(m_privRegs->display2, width2, height2);

    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);
    uint32_t f50_12_latch_n = 0u;
    if (f50_12_trace_enabled())
    {
        static std::atomic<uint32_t> s_f50_12_latch{0};
        f50_12_latch_n = s_f50_12_latch.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (f50_12_latch_should_log(f50_12_latch_n))
        {
            std::fprintf(stderr,
                         "[F512:latch] n=%u valid1=%u valid2=%u pmode=0x%llx smode2=0x%llx "
                         "disp1(fbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u) "
                         "disp2(fbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u) "
                         "ctx0(fbp=0x%x fbw=%u psm=0x%x) ctx1(fbp=0x%x fbw=%u psm=0x%x) "
                         "preferred=%u prefSrc(fbp=0x%x fbw=%u psm=0x%x) prefDst=0x%x\n",
                         f50_12_latch_n,
                         validCrt1 ? 1u : 0u,
                         validCrt2 ? 1u : 0u,
                         static_cast<unsigned long long>(m_privRegs->pmode),
                         static_cast<unsigned long long>(m_privRegs->smode2),
                         displayFrame1.fbp, displayFrame1.fbw, static_cast<uint32_t>(displayFrame1.psm),
                         displayOrigin1.x, displayOrigin1.y, width1, height1,
                         displayFrame2.fbp, displayFrame2.fbw, static_cast<uint32_t>(displayFrame2.psm),
                         displayOrigin2.x, displayOrigin2.y, width2, height2,
                         m_ctx[0].frame.fbp, m_ctx[0].frame.fbw, static_cast<uint32_t>(m_ctx[0].frame.psm),
                         m_ctx[1].frame.fbp, m_ctx[1].frame.fbw, static_cast<uint32_t>(m_ctx[1].frame.psm),
                         m_hasPreferredDisplaySource ? 1u : 0u,
                         m_preferredDisplaySourceFrame.fbp,
                         m_preferredDisplaySourceFrame.fbw,
                         static_cast<uint32_t>(m_preferredDisplaySourceFrame.psm),
                         m_preferredDisplayDestFbp);
        }
    }

    auto copyDisplaySource = [&](const GSFrameReg &displayFrame,
                                 const GSDisplayReadOrigin &displayOrigin,
                                 uint32_t width,
                                 uint32_t height,
                                 bool allowPreferred,
                                 bool preserveAlpha,
                                 GSFrameReg &selectedFrame,
                                 std::vector<uint8_t> &scratch,
                                 bool &usedPreferred) -> bool
    {
        selectedFrame = displayFrame;
        scratch.clear();
        usedPreferred = false;

        if (allowPreferred &&
            m_hasPreferredDisplaySource &&
            m_preferredDisplayDestFbp == displayFrame.fbp &&
            (m_preferredDisplaySourceFrame.fbw != 0u || m_preferredDisplaySourceFrame.fbp != displayFrame.fbp))
        {
            if (copyFrameToHostRgbaUnlocked(m_preferredDisplaySourceFrame,
                                            width,
                                            height,
                                            scratch,
                                            preserveAlpha,
                                            true,
                                            false,
                                            0u,
                                            0u))
            {
                selectedFrame = m_preferredDisplaySourceFrame;
                usedPreferred = true;
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=preferred displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u nonblack=%u\n",
                                 f50_12_latch_n,
                                 displayFrame.fbp,
                                 selectedFrame.fbp,
                                 selectedFrame.fbw,
                                 static_cast<uint32_t>(selectedFrame.psm),
                                 width,
                                 height,
                                 countNonBlackPixels(scratch, width, height));
                }
            }
            else if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchcopy] n=%u stage=preferred-fail displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u\n",
                             f50_12_latch_n,
                             displayFrame.fbp,
                             m_preferredDisplaySourceFrame.fbp,
                             m_preferredDisplaySourceFrame.fbw,
                             static_cast<uint32_t>(m_preferredDisplaySourceFrame.psm),
                             width,
                             height);
            }
        }

        if (scratch.empty())
        {
            if (!copyFrameToHostRgbaUnlocked(displayFrame,
                                             width,
                                             height,
                                             scratch,
                                             preserveAlpha,
                                             true,
                                             true,
                                             displayOrigin.x,
                                             displayOrigin.y))
            {
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=display-fail displayFbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u\n",
                                 f50_12_latch_n,
                                 displayFrame.fbp,
                                 displayFrame.fbw,
                                 static_cast<uint32_t>(displayFrame.psm),
                                 displayOrigin.x,
                                 displayOrigin.y,
                                 width,
                                 height);
                }
                return false;
            }
            if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchcopy] n=%u stage=display displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u nonblack=%u\n",
                             f50_12_latch_n,
                             displayFrame.fbp,
                             displayFrame.fbp,
                             displayFrame.fbw,
                             static_cast<uint32_t>(displayFrame.psm),
                             displayOrigin.x,
                             displayOrigin.y,
                             width,
                             height,
                             countNonBlackPixels(scratch, width, height));
            }
        }

        if (!usedPreferred && countNonBlackPixels(scratch, width, height) == 0u)
        {
            for (int contextIndex = 0; contextIndex < 2; ++contextIndex)
            {
                const GSFrameReg &candidate = m_ctx[contextIndex].frame;
                if (candidate.fbp == selectedFrame.fbp &&
                    candidate.fbw == selectedFrame.fbw &&
                    candidate.psm == selectedFrame.psm)
                {
                    continue;
                }

                std::vector<uint8_t> candidatePixels;
                if (!copyFrameToHostRgbaUnlocked(candidate,
                                                 width,
                                                 height,
                                                 candidatePixels,
                                                 preserveAlpha,
                                                 true,
                                                 true,
                                                 0u,
                                                 0u))
                {
                    continue;
                }

                const uint32_t candidateNonBlack = countNonBlackPixels(candidatePixels, width, height);
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=context%d displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u nonblack=%u\n",
                                 f50_12_latch_n,
                                 contextIndex,
                                 displayFrame.fbp,
                                 candidate.fbp,
                                 candidate.fbw,
                                 static_cast<uint32_t>(candidate.psm),
                                 width,
                                 height,
                                 candidateNonBlack);
                }

                if (candidateNonBlack == 0u)
                {
                    continue;
                }

                selectedFrame = candidate;
                scratch.swap(candidatePixels);
                break;
            }

            if (uploadPresentationFallbackEnabled() &&
                countNonBlackPixels(scratch, width, height) == 0u &&
                s_f31UploadPresentationCandidate.valid)
            {
                const F31UploadPresentationCandidate &candidate = s_f31UploadPresentationCandidate;
                const uint32_t candidateWidth = std::min<uint32_t>(width, candidate.width);
                const uint32_t candidateHeight = std::min<uint32_t>(height, candidate.height);
                if (candidateWidth != 0u &&
                    candidateHeight != 0u &&
                    !candidate.pixels.empty() &&
                    countNonBlackPixels(candidate.pixels, candidateWidth, candidateHeight) != 0u)
                {
                    if (phaseDiagnosticsEnabled())
                    {
                        static std::atomic<uint32_t> s_f31UploadPresentCount{0};
                        const uint32_t n = s_f31UploadPresentCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 12u || (n % 60u) == 0u)
                        {
                            std::fprintf(stderr,
                                         "[F31:upload-present] n=%u displayFbp=0x%x srcDbp=0x%x dbw=%u psm=0x%x size=(%u,%u) nonzero=%u score=%llu\n",
                                         n,
                                         displayFrame.fbp,
                                         candidate.frame.fbp,
                                         candidate.frame.fbw,
                                         static_cast<uint32_t>(candidate.frame.psm),
                                         candidate.width,
                                         candidate.height,
                                         candidate.nonzeroBytes,
                                         static_cast<unsigned long long>(candidate.score));
                        }
                    }
                    selectedFrame = candidate.frame;
                    scratch = candidate.pixels;
                    usedPreferred = true;
                }
            }
        }

        return true;
    };

    if (!validCrt1 && !validCrt2)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (validCrt1 && validCrt2)
    {
        GSFrameReg selectedFrame1{};
        GSFrameReg selectedFrame2{};
        std::vector<uint8_t> rc1;
        std::vector<uint8_t> rc2;
        bool usedPreferred1 = false;
        bool usedPreferred2 = false;

        const bool copiedCrt1 = copyDisplaySource(displayFrame1, displayOrigin1, width1, height1, false, true, selectedFrame1, rc1, usedPreferred1);
        const bool copiedCrt2 = copyDisplaySource(displayFrame2, displayOrigin2, width2, height2, false, true, selectedFrame2, rc2, usedPreferred2);

        if (copiedCrt1 && copiedCrt2)
        {
            const G195PresentMode g195Mode = g195PresentationMode();
            if (g195Mode == G195PresentMode::Crt1Only || g195Mode == G195PresentMode::Crt2Only)
            {
                const bool useCrt1 = (g195Mode == G195PresentMode::Crt1Only);
                std::vector<uint8_t> selected = useCrt1 ? rc1 : rc2;
                const uint32_t width = useCrt1 ? width1 : width2;
                const uint32_t height = useCrt1 ? height1 : height2;
                if (applyFieldMode)
                {
                    applyFieldPresentation(selected, width, height, oddField);
                }
                normalizePresentationAlpha(selected, width, height);

                m_hostPresentationFrame.swap(selected);
                m_hostPresentationWidth = width;
                m_hostPresentationHeight = height;
                m_hostPresentationDisplayFbp = useCrt1 ? displayFrame1.fbp : displayFrame2.fbp;
                m_hostPresentationSourceFbp = useCrt1 ? selectedFrame1.fbp : selectedFrame2.fbp;
                m_hostPresentationUsedPreferred = false;
                m_hasHostPresentationFrame = true;
                static std::atomic<uint32_t> s_g195PresentLog{0};
                const uint32_t pn = s_g195PresentLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (pn <= 16u || (pn % 60u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G195:present] n=%u mode=%s applyField=%u odd=%u "
                                 "dispFbp=0x%x sourceFbp=0x%x size=%ux%u nonblack=%u\n",
                                 pn,
                                 g195PresentationModeName(g195Mode),
                                 applyFieldMode ? 1u : 0u,
                                 oddField ? 1u : 0u,
                                 m_hostPresentationDisplayFbp,
                                 m_hostPresentationSourceFbp,
                                 m_hostPresentationWidth,
                                 m_hostPresentationHeight,
                                 countNonBlackPixels(m_hostPresentationFrame,
                                                     m_hostPresentationWidth,
                                                     m_hostPresentationHeight));
                }
                return;
            }

            if (g195Mode == G195PresentMode::FieldPairCrt2Even ||
                g195Mode == G195PresentMode::FieldPairCrt1Even)
            {
                const uint32_t width = std::max(width1, width2);
                const uint32_t height = std::max(height1, height2);
                const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
                const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
                const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);

                std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
                for (uint32_t y = 0; y < height; ++y)
                {
                    const bool evenLine = (y & 1u) == 0u;
                    const bool useCrt1 = (g195Mode == G195PresentMode::FieldPairCrt1Even)
                                             ? evenLine
                                             : !evenLine;
                    const std::vector<uint8_t> &src = useCrt1 ? rc1 : rc2;
                    const uint32_t srcWidth = useCrt1 ? width1 : width2;
                    const uint32_t srcHeight = useCrt1 ? height1 : height2;
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        dstRow[x * 4u + 0u] = bgR;
                        dstRow[x * 4u + 1u] = bgG;
                        dstRow[x * 4u + 2u] = bgB;
                        dstRow[x * 4u + 3u] = 255u;
                    }

                    if (srcWidth == 0u || srcHeight == 0u || src.empty())
                    {
                        continue;
                    }

                    uint32_t sourceY = (y >> 1u) << 1u;
                    if (sourceY >= srcHeight)
                    {
                        sourceY = srcHeight - 1u;
                    }

                    const uint8_t *srcRow = src.data() + (sourceY * kHostFrameWidth * 4u);
                    const uint32_t copyWidth = std::min(width, srcWidth);
                    for (uint32_t x = 0; x < copyWidth; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }

                m_hostPresentationFrame.swap(merged);
                m_hostPresentationWidth = width;
                m_hostPresentationHeight = height;
                m_hostPresentationDisplayFbp = displayFrame1.fbp;
                m_hostPresentationSourceFbp = selectedFrame1.fbp;
                m_hostPresentationUsedPreferred = false;
                m_hasHostPresentationFrame = true;
                static std::atomic<uint32_t> s_g195PresentLog{0};
                const uint32_t pn = s_g195PresentLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (pn <= 16u || (pn % 60u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G195:present] n=%u mode=%s applyField=%u odd=%u "
                                 "disp1Org=%u,%u disp2Org=%u,%u size=%ux%u nonblack=%u\n",
                                 pn,
                                 g195PresentationModeName(g195Mode),
                                 applyFieldMode ? 1u : 0u,
                                 oddField ? 1u : 0u,
                                 displayOrigin1.x,
                                 displayOrigin1.y,
                                 displayOrigin2.x,
                                 displayOrigin2.y,
                                 m_hostPresentationWidth,
                                 m_hostPresentationHeight,
                                 countNonBlackPixels(m_hostPresentationFrame,
                                                     m_hostPresentationWidth,
                                                     m_hostPresentationHeight));
                }
                return;
            }

            const uint32_t width = std::max(width1, width2);
            const uint32_t height = std::max(height1, height2);
            const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
            const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
            const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);
            const uint8_t bgA = pmode.alp;

            std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    dstRow[x * 4u + 0u] = bgR;
                    dstRow[x * 4u + 1u] = bgG;
                    dstRow[x * 4u + 2u] = bgB;
                    dstRow[x * 4u + 3u] = bgA;
                }
            }

            if (!pmode.slbg)
            {
                for (uint32_t y = 0; y < height2; ++y)
                {
                    const uint8_t *srcRow = rc2.data() + (y * kHostFrameWidth * 4u);
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width2; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
                    }
                }
            }

            for (uint32_t y = 0; y < height1; ++y)
            {
                const uint8_t *srcRow = rc1.data() + (y * kHostFrameWidth * 4u);
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t srcR = srcRow[x * 4u + 0u];
                    const uint8_t srcG = srcRow[x * 4u + 1u];
                    const uint8_t srcB = srcRow[x * 4u + 2u];
                    const uint8_t srcA = srcRow[x * 4u + 3u];
                    const uint8_t dstR = dstRow[x * 4u + 0u];
                    const uint8_t dstG = dstRow[x * 4u + 1u];
                    const uint8_t dstB = dstRow[x * 4u + 2u];
                    const uint8_t dstA = dstRow[x * 4u + 3u];
                    const uint32_t factor = pmode.mmod
                                                ? static_cast<uint32_t>(pmode.alp)
                                                : std::min<uint32_t>(255u, static_cast<uint32_t>(srcA) * 2u);

                    dstRow[x * 4u + 0u] = blendPresentationChannel(srcR, dstR, factor);
                    dstRow[x * 4u + 1u] = blendPresentationChannel(srcG, dstG, factor);
                    dstRow[x * 4u + 2u] = blendPresentationChannel(srcB, dstB, factor);
                    dstRow[x * 4u + 3u] = pmode.amod ? dstA : srcA;
                }
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 4u + 3u] = 255u;
                }
            }

            if (applyFieldMode)
            {
                applyFieldPresentation(merged, width, height, oddField);
            }

            m_hostPresentationFrame.swap(merged);
            m_hostPresentationWidth = width;
            m_hostPresentationHeight = height;
            m_hostPresentationDisplayFbp = displayFrame1.fbp;
            m_hostPresentationSourceFbp = selectedFrame1.fbp;
            m_hostPresentationUsedPreferred = false;
            m_hasHostPresentationFrame = true;
            if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchdone] n=%u mode=dual displayFbp=0x%x sourceFbp=0x%x usedPreferred=0 size=%ux%u nonblack=%u\n",
                             f50_12_latch_n,
                             m_hostPresentationDisplayFbp,
                             m_hostPresentationSourceFbp,
                             m_hostPresentationWidth,
                             m_hostPresentationHeight,
                             countNonBlackPixels(m_hostPresentationFrame, m_hostPresentationWidth, m_hostPresentationHeight));
            }
            return;
        }
    }

    const GSFrameReg &displayFrame = validCrt1 ? displayFrame1 : displayFrame2;
    const uint32_t width = validCrt1 ? width1 : width2;
    const uint32_t height = validCrt1 ? height1 : height2;

    GSFrameReg selectedFrame = displayFrame;
    std::vector<uint8_t> scratch;
    bool usedPreferred = false;
    const GSDisplayReadOrigin &displayOrigin = validCrt1 ? displayOrigin1 : displayOrigin2;
    if (!copyDisplaySource(displayFrame, displayOrigin, width, height, true, false, selectedFrame, scratch, usedPreferred))
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = displayFrame.fbp;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (applyFieldMode)
    {
        applyFieldPresentation(scratch, width, height, oddField);
    }

    normalizePresentationAlpha(scratch, width, height);

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = displayFrame.fbp;
    m_hostPresentationSourceFbp = selectedFrame.fbp;
    m_hostPresentationUsedPreferred = usedPreferred;
    m_hasHostPresentationFrame = true;
    if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
    {
        std::fprintf(stderr,
                     "[F512:latchdone] n=%u mode=single displayFbp=0x%x sourceFbp=0x%x usedPreferred=%u size=%ux%u nonblack=%u\n",
                     f50_12_latch_n,
                     m_hostPresentationDisplayFbp,
                     m_hostPresentationSourceFbp,
                     m_hostPresentationUsedPreferred ? 1u : 0u,
                     m_hostPresentationWidth,
                     m_hostPresentationHeight,
                     countNonBlackPixels(m_hostPresentationFrame, m_hostPresentationWidth, m_hostPresentationHeight));
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    // G189: dedicated mutex, not m_stateMutex — see s_g189HostPresentMutex's comment above.
    std::lock_guard<std::mutex> lock(s_g189HostPresentMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.assign(packedRowBytes * static_cast<size_t>(outHeight), 0u);
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    G147CountedPerfScope g147GifScope(g_g147GsGifPacketNs, g_g147GsGifPacketCount);
    const bool g147On = g147PerfEnabled();
    uint64_t g147Tags = 0ull;
    uint64_t g147PackedRegs = 0ull;
    uint64_t g147ReglistRegs = 0ull;
    uint64_t g147ImageBytesLocal = 0ull;
    auto g147FlushCounters = [&]() {
        if (!g147On)
            return;
        if (g147Tags)
            g_g147GifTags.fetch_add(g147Tags, std::memory_order_relaxed);
        if (g147PackedRegs)
            g_g147PackedRegs.fetch_add(g147PackedRegs, std::memory_order_relaxed);
        if (g147ReglistRegs)
            g_g147ReglistRegs.fetch_add(g147ReglistRegs, std::memory_order_relaxed);
        if (g147ImageBytesLocal)
            g_g147ImageBytes.fetch_add(g147ImageBytesLocal, std::memory_order_relaxed);
    };

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_vram)
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    if (s_pendingImageBytes == 0u && sizeBytes >= 16)
    {
        const uint64_t tagLo = loadLE64(data);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        if (flg == GIF_FMT_PACKED)
        {
            m_hwregX = 0;
            m_hwregY = 0;
        }
    }

    uint32_t offset = 0;
    if (s_pendingImageBytes != 0u)
    {
        if (looksLikePackedAdGifTag(data, sizeBytes))
        {
            if (t8UploadTraceEnabled() && f51PendingDropShouldLog())
            {
                uint64_t tagLo = 0u;
                uint64_t tagHi = 0u;
                std::memcpy(&tagLo, data, sizeof(tagLo));
                std::memcpy(&tagHi, data + 8u, sizeof(tagHi));
                std::fprintf(stderr,
                             "[F51:pendingdrop] pending=%u size=%u tagLo=0x%016llx tagHi=0x%016llx bitblt(dbp=0x%x dbw=%u dpsm=0x%x)\n",
                             s_pendingImageBytes, sizeBytes,
                             static_cast<unsigned long long>(tagLo),
                             static_cast<unsigned long long>(tagHi),
                             m_bitbltbuf.dbp,
                             static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
            }
            s_pendingImageBytes = 0u;
        }
    }

    if (s_pendingImageBytes != 0u)
    {
        const uint32_t imageBytes = std::min<uint32_t>(s_pendingImageBytes, sizeBytes);
        {
            static const bool g12up = envFlagEnabled("DC2_TRACE_G12_UPLOAD");
            const uint32_t db = m_bitbltbuf.dbp;
            if (g12up && (db == 0x2aa0u || (db >= 0x2700u && db <= 0x3400u)))
                std::fprintf(stderr, "[G12:cont] dbp=0x%x consume=%u remain=%u\n",
                             db, imageBytes, s_pendingImageBytes - imageBytes);
        }
        processImageData(data, imageBytes);
        g147ImageBytesLocal += imageBytes;
        s_pendingImageBytes -= imageBytes;
        offset = imageBytes;
        if (s_pendingImageBytes != 0u)
        {
            g147FlushCounters();
            return;
        }
    }

    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        ++g147Tags;

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegister(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            m_hwregX = 0;
            m_hwregY = 0;
            const bool g171On = g171PerfEnabled();
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    if (g171On)
                    {
                        const auto g171T0 = std::chrono::steady_clock::now();
                        writeRegisterPacked(regs[r], lo, hi);
                        const auto g171Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - g171T0).count();
                        const uint8_t g171Desc = regs[r] & 0xFu;
                        g_g171RegNs[g171Desc].fetch_add(static_cast<uint64_t>(g171Ns), std::memory_order_relaxed);
                        g_g171RegCount[g171Desc].fetch_add(1u, std::memory_order_relaxed);
                        if (g171Desc == 0x0Eu)
                        {
                            const uint8_t g171AdAddr = static_cast<uint8_t>(hi & 0xFFu);
                            g_g171AdNs[g171AdAddr].fetch_add(static_cast<uint64_t>(g171Ns), std::memory_order_relaxed);
                            g_g171AdCount[g171AdAddr].fetch_add(1u, std::memory_order_relaxed);
                            // G171 follow-up: identify WHAT is being drawn on the slow A+D XYZ2/XYZF2
                            // vertex-kick path (candidate: non-deferred 2D sprite raster, since G144
                            // only defers textured TRIS). Rate-limited trace on expensive calls only.
                            if ((g171AdAddr == 0x04u || g171AdAddr == 0x05u) && g171Ns > 5000)
                            {
                                static std::atomic<uint32_t> s_g171SlowCount{0};
                                const uint32_t sn = s_g171SlowCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (sn <= 40u || (sn % 200u) == 0u)
                                {
                                    const GSContext &sctx = m_ctx[m_prim.ctxt & 1u];
                                    const GSVertex &vA = m_vtxQueue[(m_vtxCount + kMaxVerts - 1) % kMaxVerts];
                                    const GSVertex &vB = m_vtxQueue[(m_vtxCount + kMaxVerts - 2) % kMaxVerts];
                                    std::fprintf(stderr,
                                                 "[G171:slowad] sn=%u addr=0x%02x ns=%lld prim=%u tme=%u abe=%u fbp=0x%x fbw=%u psm=0x%x scissor=(%d,%d)-(%d,%d) vtx=(%.1f,%.1f)-(%.1f,%.1f)\n",
                                                 sn, g171AdAddr, static_cast<long long>(g171Ns),
                                                 static_cast<uint32_t>(m_prim.type),
                                                 (unsigned)m_prim.tme, (unsigned)m_prim.abe,
                                                 sctx.frame.fbp, sctx.frame.fbw, static_cast<uint32_t>(sctx.frame.psm),
                                                 sctx.scissor.x0, sctx.scissor.y0, sctx.scissor.x1, sctx.scissor.y1,
                                                 vA.x, vA.y, vB.x, vB.y);
                                }
                            }
                        }
                    }
                    else
                    {
                        writeRegisterPacked(regs[r], lo, hi);
                    }
                    ++g147PackedRegs;
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                    {
                        g147FlushCounters();
                        return;
                    }
                    writeRegister(regs[r], loadLE64(data + offset));
                    offset += 8;
                    ++g147ReglistRegs;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            const uint32_t expectedImageBytes = nloop * 16u;
            uint32_t imageBytes = expectedImageBytes;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            if (phaseDiagnosticsEnabled())
            {
                static std::atomic<uint32_t> s_f31ImageCount{0};
                const uint32_t n = s_f31ImageCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u || (n % 128u) == 0u)
                {
                    uint32_t nonzeroBytes = 0u;
                    for (uint32_t i = 0; i < imageBytes; ++i)
                    {
                        if (data[offset + i] != 0u)
                            ++nonzeroBytes;
                    }
                    std::fprintf(stderr,
                                 "[F31:image] n=%u nloop=%u bytes=%u nonzero=%u trxdir=%u dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u)\n",
                                 n, nloop, imageBytes, nonzeroBytes, m_trxdir,
                                 m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                                 static_cast<uint32_t>(m_bitbltbuf.dpsm),
                                 static_cast<uint32_t>(m_trxreg.rrw),
                                 static_cast<uint32_t>(m_trxreg.rrh),
                                 static_cast<uint32_t>(m_trxpos.dsax),
                                 static_cast<uint32_t>(m_trxpos.dsay));
                }
            }
            // G12: uncapped marker for uploads into the costume common-menu-frame /
            // fukusel region (block 0x2700-0x3400). Confirms whether block 0x2aa0
            // (the row-bar atlas, slot[64], src=0xde7890 on HW) ever receives pixels.
            static const bool g12up = envFlagEnabled("DC2_TRACE_G12_UPLOAD");
            if (g12up)
            {
                const uint32_t db = m_bitbltbuf.dbp;
                if (db == 0x2aa0u || (db >= 0x2700u && db <= 0x3400u))
                {
                    uint32_t nz = 0u;
                    for (uint32_t i = 0; i < imageBytes; ++i) if (data[offset + i]) ++nz;
                    std::fprintf(stderr, "[G12:upload] dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) nloop=%u expect=%u bytes=%u nz=%u sizeLeft=%u\n",
                                 db, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm,
                                 (unsigned)m_trxreg.rrw, (unsigned)m_trxreg.rrh,
                                 (unsigned)m_trxpos.dsax, (unsigned)m_trxpos.dsay,
                                 nloop, expectedImageBytes, imageBytes, nz, sizeBytes - offset);
                }
            }
            // G66: tally distinct upload destination blocks so we can tell whether the rock
            // texture (HW tbp=0x3960 PSMT8) is ever uploaded to VRAM on the runner.
            if (g14TraceEnabled())
            {
                static std::mutex s_g66UpMtx;
                struct UpRec { uint32_t dbp; uint32_t dpsm; uint64_t n; uint64_t nz; };
                static UpRec s_up[64]{}; static uint32_t s_upN = 0;
                uint32_t nz = 0u; for (uint32_t i = 0; i < imageBytes; ++i) if (data[offset + i]) ++nz;
                std::lock_guard<std::mutex> lk(s_g66UpMtx);
                uint32_t idx = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < s_upN; ++i) if (s_up[i].dbp == m_bitbltbuf.dbp && s_up[i].dpsm == m_bitbltbuf.dpsm) { idx = i; break; }
                if (idx == 0xFFFFFFFFu && s_upN < 64u) { idx = s_upN++; s_up[idx].dbp = m_bitbltbuf.dbp; s_up[idx].dpsm = m_bitbltbuf.dpsm; }
                if (idx != 0xFFFFFFFFu) { s_up[idx].n++; s_up[idx].nz += nz; }
                static uint64_t s_upTick = 0;
                if ((++s_upTick % 4000ull) == 0ull)
                    for (uint32_t i = 0; i < s_upN; ++i)
                        std::fprintf(stderr, "[G66:up] dbp=0x%x dpsm=0x%x n=%llu nz=%llu\n",
                            s_up[i].dbp, s_up[i].dpsm, (unsigned long long)s_up[i].n, (unsigned long long)s_up[i].nz);
            }
            processImageData(data + offset, imageBytes);
            g147ImageBytesLocal += imageBytes;
            offset += imageBytes;
            if (imageBytes < expectedImageBytes)
            {
                s_pendingImageBytes = expectedImageBytes - imageBytes;
                g147FlushCounters();
                return;
            }
        }
    }
    g147FlushCounters();
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegister(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
    {
        // G52: the Select-Costume Max model (skinned, fbp=0x139 RTT) occasionally emits a
        // FULLY-ZERO RGBAQ for a skin vertex — a VU1/packet artifact. HW's red_vest.gs NEVER
        // emits a zero RGBAQ for this model (every model vertex has alpha=0x80, color>=0x50),
        // so a zero here is spurious; absent the bogus write the GS color register would simply
        // RETAIN the previous (valid) vertex color. Triangles interpolating from these zero
        // verts produced the long-deferred dark "face/hand speckle". Scope: only the costume
        // model context (fbp=0x139 textured tristrip). Kill switch DC2_G52_HOLDCOLOR=0.
        static const bool g52holdOff = envFlagEnabled("DC2_G52_NO_HOLDCOLOR");
        const bool g52ZeroRgbaq = (lo == 0u && hi == 0u);
        const bool g52ModelCtx = (m_prim.type == GS_PRIM_TRISTRIP &&
                                  m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u);
        if (!(!g52holdOff && g52ZeroRgbaq && g52ModelCtx))
        {
            m_curR = static_cast<uint8_t>(lo & 0xFF);
            m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
            m_curB = static_cast<uint8_t>(hi & 0xFF);
            m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        }
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && m_prim.type == GS_PRIM_TRISTRIP &&
                m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52c{0};
                const uint32_t n = s_g52c.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr, "[G52:rgba] n=%u col=(%u,%u,%u,%u) lo=0x%016llx hi=0x%016llx\n",
                                 n, (unsigned)m_curR, (unsigned)m_curG, (unsigned)m_curB, (unsigned)m_curA,
                                 static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
            }
        }
        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31PackedRgbaCount{0};
            const uint32_t n = s_f31PackedRgbaCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool rgbNz = (m_curR | m_curG | m_curB) != 0u;
            if (n <= 32u || rgbNz || (n % 1024u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:packed-rgbaq] n=%u rgba=%02x,%02x,%02x,%02x lo=0x%016llx hi=0x%016llx\n",
                             n, m_curR, m_curG, m_curB, m_curA,
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
        break;
    }
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0xFFFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0xFFFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        { static const bool s_fd = envFlagEnabled("DC2_G65_FORCEDRAW"); if (s_fd) adk = false; }
        bool g105Restart = false;
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (!adk && g105SynthAdcEnabled() &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                m_prim.type == GS_PRIM_TRISTRIP &&
                m_prim.tme &&
                ctx.frame.fbp != 0x139u &&
                g105TitleRockTbp(ctx.tex0.tbp0))
            {
                const float guard = g105AdcGuard();
                const float sx = static_cast<float>(x) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofx >> 4);
                const float sy = static_cast<float>(y) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofy >> 4);
                const bool outside =
                    sx < static_cast<float>(ctx.scissor.x0) - guard ||
                    sx > static_cast<float>(ctx.scissor.x1) + guard ||
                    sy < static_cast<float>(ctx.scissor.y0) - guard ||
                    sy > static_cast<float>(ctx.scissor.y1) + guard;
                if (outside)
                {
                    adk = true;
                    g105Restart = g105ResetStripEnabled();
                    static const bool s_stat = envFlagEnabled("DC2_G105_ADC_STAT");
                    if (s_stat)
                    {
                        static std::atomic<uint64_t> s_forced{0}, s_seen{0};
                        const uint64_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t fcnt = s_forced.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if ((n & 0x1FFFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G105:adc] seen=%llu forced=%llu guard=%.0f tbp=0x%x xy=(%.1f,%.1f)\n",
                                         static_cast<unsigned long long>(n),
                                         static_cast<unsigned long long>(fcnt),
                                         guard, ctx.tex0.tbp0, sx, sy);
                    }
                }
            }
        }
        {
            static const bool g52seq = envFlagEnabled("DC2_G52_SEQ");
            if (g52seq && m_prim.type == GS_PRIM_TRISTRIP &&
                m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g52{0};
                const uint32_t n = s_g52.fetch_add(1u, std::memory_order_relaxed);
                if (n < 400u)
                    std::fprintf(stderr,
                                 "[G52:seq] n=%u adc=%u xy=(%.1f,%.1f) col=(%u,%u,%u,%u) tbp=0x%x hi=0x%016llx\n",
                                 n, (unsigned)(adk ? 1u : 0u), x / 16.0f, y / 16.0f,
                                 (unsigned)m_curR, (unsigned)m_curG, (unsigned)m_curB, (unsigned)m_curA,
                                 m_ctx[m_prim.ctxt & 1u].tex0.tbp0,
                                 static_cast<unsigned long long>(hi));
            }
        }
        if (g31TraceEnabled() &&
            (m_prim.type == GS_PRIM_TRIANGLE ||
             m_prim.type == GS_PRIM_TRISTRIP ||
             m_prim.type == GS_PRIM_TRIFAN))
        {
            static std::atomic<uint32_t> s_g31PackedXyzf2Count{0};
            const uint32_t n = s_g31PackedXyzf2Count.fetch_add(1u, std::memory_order_relaxed);
            if (n < 160u)
            {
                std::fprintf(stderr,
                             "[G31:xyzf2] n=%u prim=%u slot=%u countBefore=%u adc=%u x=%u(%.1f) y=%u(%.1f) z=0x%x f=%u lo=0x%016llx hi=0x%016llx\n",
                             n,
                             static_cast<uint32_t>(m_prim.type),
                             static_cast<uint32_t>(m_vtxCount % kMaxVerts),
                             m_vtxCount,
                             static_cast<uint32_t>(adk ? 1u : 0u),
                             static_cast<uint32_t>(x), x / 16.0f,
                             static_cast<uint32_t>(y), y / 16.0f,
                             z,
                             static_cast<uint32_t>(f),
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi));
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        if (g105Restart)
            m_vtxCount = 0;
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        { static const bool s_fd = envFlagEnabled("DC2_G65_FORCEDRAW"); if (s_fd) adk = false; }
        bool g105Restart = false;
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (!adk && g105SynthAdcEnabled() &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                m_prim.type == GS_PRIM_TRISTRIP &&
                m_prim.tme &&
                ctx.frame.fbp != 0x139u &&
                g105TitleRockTbp(ctx.tex0.tbp0))
            {
                const float guard = g105AdcGuard();
                const float sx = static_cast<float>(x) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofx >> 4);
                const float sy = static_cast<float>(y) / 16.0f -
                                 static_cast<float>(ctx.xyoffset.ofy >> 4);
                const bool outside =
                    sx < static_cast<float>(ctx.scissor.x0) - guard ||
                    sx > static_cast<float>(ctx.scissor.x1) + guard ||
                    sy < static_cast<float>(ctx.scissor.y0) - guard ||
                    sy > static_cast<float>(ctx.scissor.y1) + guard;
                if (outside)
                {
                    adk = true;
                    g105Restart = g105ResetStripEnabled();
                    static const bool s_stat = envFlagEnabled("DC2_G105_ADC_STAT");
                    if (s_stat)
                    {
                        static std::atomic<uint64_t> s_forced{0}, s_seen{0};
                        const uint64_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t fcnt = s_forced.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if ((n & 0x1FFFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G105:adc] seen=%llu forced=%llu guard=%.0f tbp=0x%x xy=(%.1f,%.1f)\n",
                                         static_cast<unsigned long long>(n),
                                         static_cast<unsigned long long>(fcnt),
                                         guard, ctx.tex0.tbp0, sx, sy);
                    }
                }
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        if (g105Restart)
            m_vtxCount = 0;
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        if (f33AdTraceEnabled() && f33TraceAdRegister(addr))
        {
            static std::atomic<uint32_t> s_f33AdCount{0};
            const uint32_t n = s_f33AdCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 1024u)
                f33TraceAdWrite(n, addr, lo);
        }
        writeRegister(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegister(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // PHASE F30: gsreg probe — track RGBAQ separately to catch color sets
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30GsReg{0};
        static std::atomic<uint32_t> s_f30Rgbaq{0};
        static std::atomic<uint32_t> s_f30RgbaqNz{0};
        const uint32_t n = s_f30GsReg.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (regAddr == 0x01u)
        {
            const uint32_t k = s_f30Rgbaq.fetch_add(1, std::memory_order_relaxed) + 1u;
            const uint32_t lo = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            const bool rgbNz = (lo & 0x00FFFFFFu) != 0u;
            if (rgbNz)
                s_f30RgbaqNz.fetch_add(1, std::memory_order_relaxed);
            if (k <= 8u || (k & 0x3FFu) == 0u || rgbNz)
            {
                std::fprintf(stderr,
                             "[F30:rgbaq] k=%u value=0x%016llx rgbNz=%u nzTotal=%u\n",
                             k, static_cast<unsigned long long>(value),
                             rgbNz ? 1u : 0u, s_f30RgbaqNz.load());
            }
        }
        if (regAddr >= 0x50u && regAddr <= 0x53u)
        {
            static std::atomic<uint32_t> s_f31UploadRegCount{0};
            const uint32_t k = s_f31UploadRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (k <= 64u || (k % 256u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:upload-reg] k=%u reg=0x%02x value=0x%016llx\n",
                             k, regAddr, static_cast<unsigned long long>(value));
            }
        }
        if (n <= 12u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                         "[F30:gsreg] n=%u reg=0x%02x lo=0x%08x\n",
                         n, regAddr, static_cast<uint32_t>(value & 0xFFFFFFFFu));
        }
    }
    if (phaseDiagnosticsEnabled() && regAddr == GS_REG_PRIM)
    {
        static std::atomic<uint32_t> s_f31PrimRegCount{0};
        const uint32_t n = s_f31PrimRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 4096u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:prim-reg] n=%u value=0x%llx prmodecont=%u type=%u iip=%u tme=%u abe=%u fst=%u ctxt=%u\n",
                         n, static_cast<unsigned long long>(value),
                         m_prmodecont ? 1u : 0u,
                         static_cast<uint32_t>(value & 0x7u),
                         static_cast<uint32_t>((value >> 3u) & 1u),
                         static_cast<uint32_t>((value >> 4u) & 1u),
                         static_cast<uint32_t>((value >> 6u) & 1u),
                         static_cast<uint32_t>((value >> 8u) & 1u),
                         static_cast<uint32_t>((value >> 9u) & 1u));
        }
    }
    else if (phaseDiagnosticsEnabled() && (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2))
    {
        static std::atomic<uint32_t> s_f31Tex0RegCount{0};
        const uint32_t n = s_f31Tex0RegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 1024u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:tex0-reg] n=%u reg=0x%02x value=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u\n",
                         n, regAddr, static_cast<unsigned long long>(value),
                         static_cast<uint32_t>(value & 0x3FFFu),
                         static_cast<uint32_t>((value >> 14u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 20u) & 0x3Fu),
                         static_cast<uint32_t>((value >> 26u) & 0xFu),
                         static_cast<uint32_t>((value >> 30u) & 0xFu),
                         static_cast<uint32_t>((value >> 34u) & 0x1u),
                         static_cast<uint32_t>((value >> 35u) & 0x3u),
                         static_cast<uint32_t>((value >> 37u) & 0x3FFFu),
                         static_cast<uint32_t>((value >> 51u) & 0xFu),
                         static_cast<uint32_t>((value >> 55u) & 0x1u),
                         static_cast<uint32_t>((value >> 56u) & 0x1Fu),
                         static_cast<uint32_t>((value >> 61u) & 0x7u));
        }
    }
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_prim.type = static_cast<GSPrimType>(value & 0x7);
        if (m_prmodecont)
        {
            m_prim.iip = ((value >> 3) & 1) != 0;
            m_prim.tme = ((value >> 4) & 1) != 0;
            m_prim.fge = ((value >> 5) & 1) != 0;
            m_prim.abe = ((value >> 6) & 1) != 0;
            m_prim.aa1 = ((value >> 7) & 1) != 0;
            m_prim.fst = ((value >> 8) & 1) != 0;
            m_prim.ctxt = ((value >> 9) & 1) != 0;
            m_prim.fix = ((value >> 10) & 1) != 0;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        if (renderQualityTraceEnabled() && g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
            g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
            g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G35:seq] PRIM type=%u ctxt=%u tme=%u abe=%u\n",
                         (unsigned)m_prim.type, (unsigned)m_prim.ctxt,
                         (unsigned)m_prim.tme, (unsigned)m_prim.abe);
        }
        break;
    }
    case GS_REG_RGBAQ:
    {
        // G52: hold last valid color on a spurious fully-zero model RGBAQ (see writeRegisterPacked
        // case 0x01). Covers the A+D / REGLIST color path. Scope: costume model fbp=0x139 tristrip.
        static const bool g52holdOff = envFlagEnabled("DC2_G52_NO_HOLDCOLOR");
        const bool g52ModelCtx = (m_prim.type == GS_PRIM_TRISTRIP &&
                                  m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u);
        if (!g52holdOff && g52ModelCtx && (value & 0xFFFFFFFFull) == 0ull)
            break;
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0xFFFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0xFFFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        if (renderQualityTraceEnabled() && g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
            g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            const auto &sc = m_ctx[m_prim.ctxt & 1u].tex0;
            g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
            g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G35:seq] XYZF2 xy=(%.1f,%.1f) curTex0(tbp=0x%x psm=0x%x) ctxt=%u%s\n",
                         vtx.x, vtx.y, sc.tbp0, (unsigned)sc.psm, (unsigned)m_prim.ctxt,
                         (sc.tbp0 == 0x2720u) ? ((sc.psm == 0u) ? "  <-- verts under CT32 (GOOD)" : "  <-- verts under T8 (BUG)") : "");
        }
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        // [G34] Does the costume composite's sprite geometry reach the GS? Log vertices
        // kicked while the active context binds the CT32 0x2720 RTT (the composite).
        if (renderQualityTraceEnabled())
        {
            const auto &ct = m_ctx[m_prim.ctxt & 1u].tex0;
            if (m_prim.tme && ct.tbp0 == 0x2720u && ct.psm == 0u)
            {
                static std::atomic<uint32_t> s_g34v{0};
                const uint32_t vn = s_g34v.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (vn <= 64u)
                    std::fprintf(stderr, "[G34:vtx2720ct32] vn=%u xyz2 vtxCount=%u xy=(%.1f,%.1f) prim=%u ctxt=%u uv=(%u,%u)\n",
                                 vn, m_vtxCount, vtx.x, vtx.y,
                                 (unsigned)m_prim.type, (unsigned)m_prim.ctxt,
                                 (unsigned)m_curU, (unsigned)m_curV);
            }
            if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0 &&
                g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
            {
                const auto &sc = m_ctx[m_prim.ctxt & 1u].tex0;
                g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] XYZ2 xy=(%.1f,%.1f) curTex0(tbp=0x%x psm=0x%x) ctxt=%u%s\n",
                             vtx.x, vtx.y, sc.tbp0, (unsigned)sc.psm, (unsigned)m_prim.ctxt,
                             (sc.tbp0 == 0x2720u) ? ((sc.psm == 0u) ? "  <-- verts under CT32 (GOOD)" : "  <-- verts under T8 (BUG)") : "");
            }
        }
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        // [G91] Ground-truth TEX0 register-write stream for the title rock. Logs every rock-range
        // TEX0 bind in DMA/GIF stream order (title scope) so we can tell whether the wall pages
        // (0x2b20/0x2f20/0x3320) are ever WRITTEN to the GS context at all (vs the geometry just
        // inheriting a stale block-0 late texture). DC2_G91_TEX0.
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            static const bool s_g91tex0 = (std::getenv("DC2_G91_TEX0") != nullptr);
            if (s_g91tex0 && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                t.tbp0 >= 0x2720u && t.tbp0 <= 0x3960u)
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (k <= 4000u)
                    std::fprintf(stderr, "[G91:tex0] k=%u ci=%d tbp=0x%x psm=0x%x cbp=0x%x tw=%u th=%u\n",
                        k, ci, t.tbp0, (unsigned)t.psm, t.cbp, (unsigned)t.tw, (unsigned)t.th);
            }
        }
        // [G123] ordered interleave: emit the rock-range BIND event (and flush the pending
        // draw-run that preceded it) so the bind/kick pairing is visible vs the .gs ground truth.
        {
            extern std::atomic<bool> g_dc2TitleRockScope;
            if (g123SeqEnabled() && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
                g105TitleRockTbp(t.tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                g123FlushRun();
                if (s_g123Events < g123Cap())
                {
                    const uint32_t seq = s_g123Seq.fetch_add(1u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G123:seq] %u BIND tbp=0x%x psm=0x%x ci=%d\n",
                                 seq, t.tbp0, (unsigned)t.psm, ci);
                    ++s_g123Events;
                }
            }
        }
        // [G34] Count every TEX0 register bind of block 0x2720 (the model-RTT / brick
        // time-share) by psm. Tells definitively whether the costume model composite ever
        // emits a CT32 (psm=0) bind to GS, vs only the T8 brick (psm=0x13). ct32==0 after
        // a costume run => the composite's CT32 sprite never reaches the GS at all.
        if (renderQualityTraceEnabled() && t.tbp0 == 0x2720u)
        {
            static std::atomic<uint32_t> s_g34ct32{0};
            static std::atomic<uint32_t> s_g34t8{0};
            static std::atomic<uint32_t> s_g34oth{0};
            std::atomic<uint32_t> *c = (t.psm == 0u) ? &s_g34ct32
                                       : ((t.psm == GS_PSM_T8) ? &s_g34t8 : &s_g34oth);
            const uint32_t bn = c->fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((t.psm == 0u && bn <= 32u) || (bn % 8192u) == 0u)
                std::fprintf(stderr,
                    "[G34:tex0bind2720] psm=0x%x tbw=%u tw=%u th=%u bn=%u (ct32=%u t8=%u oth=%u)\n",
                    (unsigned)t.psm, (unsigned)t.tbw, (unsigned)t.tw, (unsigned)t.th, bn,
                    s_g34ct32.load(std::memory_order_relaxed),
                    s_g34t8.load(std::memory_order_relaxed),
                    s_g34oth.load(std::memory_order_relaxed));
        }
        // [G35] GIF register-sequence capture: arm on a CT32 0x2720 bind, then log every
        // following TEX0 (catching the T8 0x2720 clobber), bounded by the window + global cap.
        if (renderQualityTraceEnabled() && g_g34SeqCap.load(std::memory_order_relaxed) < 1600u)
        {
            const int seqCi = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
            if (t.tbp0 == 0x2720u && t.psm == 0u)
            {
                g_g34SeqWindow.store(70, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x2720 psm=0x0 tbw=%u  (CT32 ARM)\n",
                             seqCi, (unsigned)t.tbw);
            }
            else if (g_g34SeqWindow.load(std::memory_order_relaxed) > 0)
            {
                g_g34SeqWindow.fetch_sub(1, std::memory_order_relaxed);
                g_g34SeqCap.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G35:seq] TEX0 ci=%d tbp=0x%x psm=0x%x tbw=%u%s\n",
                             seqCi, t.tbp0, (unsigned)t.psm, (unsigned)t.tbw,
                             (t.tbp0 == 0x2720u && t.psm != 0u) ? "   <-- T8 0x2720 CLOBBER" : "");
            }
        }
        // F50.9: uncapped histogram of paletted (PSMT4/T8) TEX0 binds per context, to see
        // whether materials ever bind the uploaded pages (0x2820/0x2f20/...) or only the
        // empty 0x2580/0x2980 the dungeon draw samples.
        if (renderQualityTraceEnabled() &&
            (t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH))
        {
            // Skip the title 4HH spam (tbp=0x1a00) so the cap captures dungeon-steady-state binds.
            if (t.tbp0 != 0x1a00u)
            {
                static std::atomic<uint32_t> s_f509Tex0Bind{0};
                const uint32_t n = s_f509Tex0Bind.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 300u)
                    std::fprintf(stderr, "[F509:tex0bind] n=%u ci=%d tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u\n",
                                 n, ci, t.tbp0, (unsigned)t.tbw, (unsigned)t.psm,
                                 t.cbp, (unsigned)t.cpsm, (unsigned)t.cld);
            }
        }
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
        m_prmodecont = (value & 1) != 0;
        break;
    case GS_REG_PRMODE:
        if (!m_prmodecont)
        {
            m_prim.iip = ((value >> 3) & 1) != 0;
            m_prim.tme = ((value >> 4) & 1) != 0;
            m_prim.fge = ((value >> 5) & 1) != 0;
            m_prim.abe = ((value >> 6) & 1) != 0;
            m_prim.aa1 = ((value >> 7) & 1) != 0;
            m_prim.fst = ((value >> 8) & 1) != 0;
            m_prim.ctxt = ((value >> 9) & 1) != 0;
            m_prim.fix = ((value >> 10) & 1) != 0;
        }
        break;
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        const GSFrameReg prev = m_ctx[ci].frame;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        if (frameRegisterTraceEnabled())
        {
            static std::atomic<uint32_t> s_f49FrameRegCount{0};
            const uint32_t n = s_f49FrameRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool zeroed = (m_ctx[ci].frame.fbp == 0u || m_ctx[ci].frame.fbw == 0u);
            const bool changed = prev.fbp != m_ctx[ci].frame.fbp ||
                                 prev.fbw != m_ctx[ci].frame.fbw ||
                                 prev.psm != m_ctx[ci].frame.psm ||
                                 prev.fbmsk != m_ctx[ci].frame.fbmsk;
            if ((changed && (n <= 256u || (n % 512u) == 0u)) || zeroed)
            {
                std::fprintf(stderr,
                             "[F49:frame] n=%u reg=0x%02x ctx=%d raw=0x%016llx prev(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) new(fbp=0x%x fbw=%u psm=0x%x msk=0x%08x) zero=%u primCtxt=%u bitblt(dbp=0x%x dbw=%u dpsm=0x%x)\n",
                             n, regAddr, ci, static_cast<unsigned long long>(value),
                             prev.fbp, prev.fbw, static_cast<uint32_t>(prev.psm), prev.fbmsk,
                             m_ctx[ci].frame.fbp, m_ctx[ci].frame.fbw, static_cast<uint32_t>(m_ctx[ci].frame.psm), m_ctx[ci].frame.fbmsk,
                             zeroed ? 1u : 0u,
                             m_prim.ctxt ? 1u : 0u,
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw), static_cast<uint32_t>(m_bitbltbuf.dpsm));
            }
        }
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf = value;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        const uint32_t prevDbp = m_bitbltbuf.dbp;
        const uint8_t prevDbw = m_bitbltbuf.dbw;
        const uint8_t prevDpsm = m_bitbltbuf.dpsm;
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        if (frameRegisterTraceEnabled())
        {
            static std::atomic<uint32_t> s_f49BitbltRegCount{0};
            const uint32_t n = s_f49BitbltRegCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool changed = prevDbp != m_bitbltbuf.dbp ||
                                 prevDbw != m_bitbltbuf.dbw ||
                                 prevDpsm != m_bitbltbuf.dpsm;
            if (changed && (n <= 128u || (n % 256u) == 0u))
            {
                std::fprintf(stderr,
                             "[F49:bitblt] n=%u raw=0x%016llx dst=0x%x/%u/0x%x src=0x%x/%u/0x%x primCtxt=%u frame0=0x%x/%u frame1=0x%x/%u\n",
                             n, static_cast<unsigned long long>(value),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw), static_cast<uint32_t>(m_bitbltbuf.dpsm),
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw), static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_prim.ctxt ? 1u : 0u,
                             m_ctx[0].frame.fbp, m_ctx[0].frame.fbw,
                             m_ctx[1].frame.fbp, m_ctx[1].frame.fbw);
            }
        }
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37BitbltCount{0};
            const uint32_t n = s_f37BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:bitblt] n=%u raw=0x%016llx sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n, static_cast<unsigned long long>(value),
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw),
                             static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
        }
        if (t8UploadTraceEnabled())
        {
            static std::atomic<uint32_t> s_f43BitbltCount{0};
            const uint32_t n = s_f43BitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 128u)
                std::fprintf(stderr, "[F43:t8] n=%u sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x\n",
                             n,
                             m_bitbltbuf.sbp, static_cast<uint32_t>(m_bitbltbuf.sbw),
                             static_cast<uint32_t>(m_bitbltbuf.spsm),
                             m_bitbltbuf.dbp, static_cast<uint32_t>(m_bitbltbuf.dbw),
                             static_cast<uint32_t>(m_bitbltbuf.dpsm));
        }
        // F50.9: UNCAPPED check — does ANY BITBLTBUF ever target the pages the dungeon
        // paletted draws actually sample (tbp=0x2580 texture, cbp=0x2980 CLUT)? If never,
        // the upload-destination vs draw-reference addressing genuinely diverges.
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f509Tex2580{0};
            static std::atomic<uint32_t> s_f509Clut2980{0};
            static std::atomic<uint32_t> s_f509Total{0};
            const uint32_t tot = s_f509Total.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (m_bitbltbuf.dbp == 0x2580u)
            {
                const uint32_t h = s_f509Tex2580.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2580] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm);
            }
            if (m_bitbltbuf.dbp == 0x2980u)
            {
                const uint32_t h = s_f509Clut2980.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (h <= 8u)
                    std::fprintf(stderr, "[F509:hit2980] hit=%u dbp=0x%x dbw=%u dpsm=0x%x\n",
                                 h, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm);
            }
            if ((tot % 256u) == 0u)
                std::fprintf(stderr, "[F509:bbtally] total=%u hit2580=%u hit2980=%u\n",
                             tot, s_f509Tex2580.load(), s_f509Clut2980.load());
        }
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37TrxregCount{0};
            const uint32_t n = s_f37TrxregCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:trxreg] n=%u rrw=%u rrh=%u\n", n, m_trxreg.rrw, m_trxreg.rrh);
        }
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        m_hwregX = 0;
        m_hwregY = 0;
        if (renderQualityTraceEnabled())
        {
            static std::atomic<uint32_t> s_f37TrxdirCount{0};
            const uint32_t n = s_f37TrxdirCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
                std::fprintf(stderr, "[F37:trxdir] n=%u dir=%u dbp=0x%x dpsm=0x%x rr=(%u,%u)\n",
                             n, m_trxdir, m_bitbltbuf.dbp,
                             static_cast<uint32_t>(m_bitbltbuf.dpsm),
                             m_trxreg.rrw, m_trxreg.rrh);
        }

        if (m_trxdir == 2 && m_vram)
        {
            performLocalToLocalTransfer();
        }
        else if (m_trxdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_TEXFLUSH:
    case GS_REG_SCANMSK:
    case GS_REG_FOGCOL:
    case GS_REG_DIMX:
    case GS_REG_DTHE:
    case GS_REG_COLCLAMP:
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
        break;
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr |= 0x1;
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_privRegs)
            m_privRegs->csr |= 0x2;
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }
}

// G144/G145: defined in ps2_gs_rasterizer.cpp; drains pending tile-binning deferred triangles
// before transfers that overlap VRAM they depend on. Unknown ranges fall back to flushing.
// G149: bump the de-swizzle texture-cache VRAM generation so stale decoded buffers are invalidated.
// G260: native-renderer command graph (opt-in DC2_G260_NR=1) — when active, transfers must take
// the RANGE-tested barrier path (the graph executes only on real dependency edges).
extern bool g260NrEnabled();
extern void g149BumpVramGen();
extern void g144FlushPendingUpload();
extern void g144FlushPendingUploadRange(uint32_t dbp,
                                        uint8_t dbw,
                                        uint8_t dpsm,
                                        uint32_t dsax,
                                        uint32_t dsay,
                                        uint32_t rrw,
                                        uint32_t rrh);
extern void g144FlushPendingLocalTransferRange(uint32_t sbp,
                                               uint8_t sbw,
                                               uint8_t spsm,
                                               uint32_t ssax,
                                               uint32_t ssay,
                                               uint32_t dbp,
                                               uint8_t dbw,
                                               uint8_t dpsm,
                                               uint32_t dsax,
                                               uint32_t dsay,
                                               uint32_t rrw,
                                               uint32_t rrh);
// G242: when opt-in GPU guest-depth ownership is active, materialize an overlapping
// GPU-resident depth surface before a transfer reads its guest-VRAM source. No-op otherwise.
extern void g242PrepareVramReadRect(uint8_t *vram,
                                    uint32_t bpBlocks,
                                    uint32_t bw64,
                                    uint32_t psm,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t w,
                                    uint32_t h);
extern bool g242GuestDepthEnabled();

void GS::performLocalToLocalTransfer()
{
    G146PerfScope g146Scope(g_g146GsLocalNs);
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on)
        {
            static thread_local char s_g219tag[80];
            std::snprintf(s_g219tag, sizeof(s_g219tag), "l2l(sbp=0x%x dbp=0x%x rr=%ux%u)",
                          m_bitbltbuf.sbp, m_bitbltbuf.dbp, m_trxreg.rrw, m_trxreg.rrh);
            g219Sentinel(m_vram, m_vramSize, s_g219tag);
        }
    }

    static const bool s_g145EnvDirtyUpload = envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                             !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD");
    const bool g145DirtyUpload = (g268LiveEnvRead()
                                      ? (envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                         !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD"))
                                      : s_g145EnvDirtyUpload) ||
                                 g260NrEnabled();
    if (!g145DirtyUpload)
        g144FlushPendingUpload();

    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = m_bitbltbuf.sbw;
    uint8_t spsm = m_bitbltbuf.spsm;
    uint32_t dbp = m_bitbltbuf.dbp;
    uint8_t dbw = m_bitbltbuf.dbw;
    uint8_t dpsm = m_bitbltbuf.dpsm;

    if (sbw == 0)
        sbw = 1;
    if (dbw == 0)
        dbw = 1;

    const uint32_t rrw = m_trxreg.rrw;
    const uint32_t rrh = m_trxreg.rrh;
    const uint32_t ssax = m_trxpos.ssax;
    const uint32_t ssay = m_trxpos.ssay;
    const uint32_t dsax = m_trxpos.dsax;
    const uint32_t dsay = m_trxpos.dsay;
    const GSTransferTraversal traversal = decodeTransferTraversal(m_trxpos.dir);
    const bool formatAware = (spsm == dpsm) && supportsFormatAwareLocalCopy(spsm);

    if (rrw == 0u || rrh == 0u)
    {
        return;
    }

    if (g145DirtyUpload)
    {
        g144FlushPendingLocalTransferRange(sbp, sbw, spsm, ssax, ssay,
                                           dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    }

    // G242: the range/full G144 barrier above must run first so every deferred draw
    // that can feed this copy has reached its GPU depth surface before we read VRAM.
    g242PrepareVramReadRect(m_vram, sbp, sbw, spsm, ssax, ssay, rrw, rrh);

    g149BumpVramGen(); // VRAM about to change: invalidate the de-swizzle texture cache
    // G178/G242: notify only after pending draws and source depth have been resolved.
    // The hook materializes an overlapping GPU-owned destination before the CPU copy writes it.
    {
        extern void g178NoteVramWriteRect(uint8_t *, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t, uint32_t);
        g178NoteVramWriteRect(m_vram, dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    }

    PS2_IF_AGRESSIVE_LOGS({
        if ((spsm == GS_PSM_T4 || dpsm == GS_PSM_T4) &&
            s_debugLocalCopyCount.fetch_add(1u, std::memory_order_relaxed) < 96u)
        {
            RUNTIME_LOG("[gs:l2l] sbp=" << sbp
                                        << " dbp=" << dbp
                                        << " sbw=" << static_cast<uint32_t>(sbw)
                                        << " dbw=" << static_cast<uint32_t>(dbw)
                                        << " spsm=0x" << std::hex << static_cast<uint32_t>(spsm)
                                        << " dpsm=0x" << static_cast<uint32_t>(dpsm) << std::dec
                                        << " ss=(" << ssax << "," << ssay << ")"
                                        << " ds=(" << dsax << "," << dsay << ")"
                                        << " rr=(" << rrw << "," << rrh << ")"
                                        << " dir=" << static_cast<uint32_t>(m_trxpos.dir)
                                        << " formatAware=" << (formatAware ? 1 : 0) << std::endl);
        }
    });

    if (formatAware)
    {
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t srcY = transferCoord(ssay, rrh, row, traversal.reverseY);
            const uint32_t dstY = transferCoord(dsay, rrh, row, traversal.reverseY);
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t srcX = transferCoord(ssax, rrw, col, traversal.reverseX);
                const uint32_t dstX = transferCoord(dsax, rrw, col, traversal.reverseX);
                const uint32_t pixel =
                    readTransferPixel(m_vram, m_vramSize, sbp, sbw, spsm, srcX, srcY);
                writeTransferPixel(m_vram, m_vramSize, dbp, dbw, dpsm, dstX, dstY, pixel);
            }
        }
    }
    else
    {
        const uint32_t srcBase = sbp * 256u;
        const uint32_t dstBase = dbp * 256u;
        uint32_t srcBpp = bitsPerPixel(spsm) / 8u;
        uint32_t dstBpp = bitsPerPixel(dpsm) / 8u;
        if (srcBpp == 0)
            srcBpp = 4;
        if (dstBpp == 0)
            dstBpp = 4;
        const uint32_t srcStride = static_cast<uint32_t>(sbw) * 64u * srcBpp;
        const uint32_t dstStride = static_cast<uint32_t>(dbw) * 64u * dstBpp;
        const uint32_t copyBpp = (srcBpp < dstBpp) ? srcBpp : dstBpp;

        uint8_t pixelBytes[4] = {};
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t srcY = transferCoord(ssay, rrh, row, traversal.reverseY);
            const uint32_t dstY = transferCoord(dsay, rrh, row, traversal.reverseY);
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t srcX = transferCoord(ssax, rrw, col, traversal.reverseX);
                const uint32_t dstX = transferCoord(dsax, rrw, col, traversal.reverseX);
                const uint32_t srcOff = srcBase + srcY * srcStride + srcX * srcBpp;
                const uint32_t dstOff = dstBase + dstY * dstStride + dstX * dstBpp;
                if (srcOff + copyBpp > m_vramSize || dstOff + copyBpp > m_vramSize)
                {
                    continue;
                }

                std::memcpy(pixelBytes, m_vram + srcOff, copyBpp);
                std::memcpy(m_vram + dstOff, pixelBytes, copyBpp);
            }
        }
    }

    if (sbp == 0u && (dbp == 0u || dbp == 0x20u) && rrw >= 640u && rrh >= 512u)
    {
        m_lastDisplayBaseBytes = (dbp == 0x20u) ? 8192u : 0u;
        snapshotVRAM();
    }
}

void GS::vertexKick(bool drawing)
{
    // G196: raw per-kick ADC/drawing trace for tbp=0x2a20 (the G195 foreground-sheet suspect),
    // scoped identically to the existing G195:fgtri draw-time trace, so the two can be diffed
    // directly against the real map_0.gs freeze-dump's vertex/ADC stream. Default-off.
    // G268: this ran getenv() on EVERY vertex kick (~2.4us/vertex, ~66ms/frame on MAP-0).
    static const bool s_g196AdcTrace = envFlagEnabled("DC2_G196_ADC_TRACE");
    if (g268LiveEnvRead() ? envFlagEnabled("DC2_G196_ADC_TRACE") : s_g196AdcTrace)
    {
        const GSContext &ctx196 = m_ctx[m_prim.ctxt & 1u];
        if (m_prim.tme && ctx196.tex0.tbp0 == 0x2a20u &&
            (ctx196.frame.fbp == 0u || ctx196.frame.fbp == 0x68u))
        {
            static std::atomic<uint32_t> s_g196{0};
            const uint32_t n = s_g196.fetch_add(1u, std::memory_order_relaxed);
            if (n < 512u)
            {
                // G200: also log the freshly-written vertex position so the runner's output
                // coords can be compared against the real map_0.gs freeze's XYZ2 stream for the
                // same tbp (positions-diverge vs pure-gate-diverge discriminator).
                const GSVertex &v196 = m_vtxQueue[m_vtxCount % kMaxVerts];
                std::fprintf(stderr,
                    "[G196:kick] n=%u prim=%u drawing=%u vtxCount=%u fbp=0x%x x=%.3f y=%.3f z=%.1f q=%g\n",
                    n, static_cast<uint32_t>(m_prim.type),
                    static_cast<uint32_t>(drawing ? 1u : 0u), m_vtxCount, ctx196.frame.fbp,
                    v196.x, v196.y, v196.z, v196.q);
            }
        }
    }

    // G107 A/B: ADC is "drawing kick disabled". For title-rock strips, test the stricter
    // interpretation that an ADC vertex must not advance the primitive queue either.
    if (!drawing)
    {
        extern std::atomic<bool> g_dc2TitleRockScope;
        static const bool s_g107NoAdvance = envFlagEnabled("DC2_G107_ADC_NO_ADVANCE");
        if (s_g107NoAdvance &&
            g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
            m_prim.type == GS_PRIM_TRISTRIP)
        {
            const GSContext &ctx = m_ctx[m_prim.ctxt & 1u];
            if (m_prim.tme && ctx.frame.fbp != 0x139u && g105TitleRockTbp(ctx.tex0.tbp0))
            {
                static const bool s_stat = envFlagEnabled("DC2_G107_ADC_STAT");
                if (s_stat)
                {
                    static std::atomic<uint64_t> s_skip{0};
                    const uint64_t n = s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 32u || (n & 0x3FFull) == 0ull)
                        std::fprintf(stderr,
                                     "[G107:adcskip] n=%llu prim=%u tbp=0x%x vtxCount=%u\n",
                                     static_cast<unsigned long long>(n),
                                     static_cast<uint32_t>(m_prim.type),
                                     ctx.tex0.tbp0, m_vtxCount);
                }
                return;
            }
        }
    }

    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    // [G123] ordered interleave: accumulate this tri-class kick into the current draw-run keyed
    // by the bound TEX0; a tbp change flushes the run. Pairs with the BIND events above.
    if (g123SeqEnabled())
    {
        const uint32_t tt123 = (uint32_t)m_prim.type & 7u;
        const bool triClass123 = (tt123 == GS_PRIM_TRIANGLE || tt123 == GS_PRIM_TRISTRIP ||
                                  tt123 == GS_PRIM_TRIFAN);
        extern std::atomic<bool> g_dc2TitleRockScope;
        if (triClass123 && m_prim.tme &&
            g_dc2TitleRockScope.load(std::memory_order_relaxed))
        {
            const GSContext &ctx123 = m_ctx[m_prim.ctxt & 1u];
            if (g105TitleRockTbp(ctx123.tex0.tbp0))
            {
                std::lock_guard<std::mutex> lk(s_g123Mtx);
                if (ctx123.tex0.tbp0 != s_g123RunTbp || tt123 != s_g123RunPrim)
                    g123FlushRun();
                s_g123RunTbp = ctx123.tex0.tbp0;
                s_g123RunPrim = tt123;
                if (drawing)
                    ++s_g123RunDraw;
                else
                    ++s_g123RunNodraw;
            }
        }
    }

    if (m_vtxCount < needed)
        return;

    // G65: tally EVERY vertexKick by prim type AND by drawing flag, so we can tell whether
    // the title's tristrips reach the GS at all (and with drawing=true) vs are lost upstream.
    if (g14TraceEnabled())
    {
        static std::atomic<uint64_t> s_all[8]{}, s_nodraw[8]{};
        const uint32_t tt = (uint32_t)m_prim.type & 7u;
        s_all[tt].fetch_add(1u, std::memory_order_relaxed);
        if (!drawing) s_nodraw[tt].fetch_add(1u, std::memory_order_relaxed);

        // G66: per-tbp draw/nodraw tally for tri-class prims. Distinguishes "rock geometry
        // (tbp=0x3960) never emitted" (H1) vs "emitted but ADC=1 / texture missing / occluded"
        // (H2). HW new_game.gs: rock = tbp=0x3960 PSMT8, 608 tstrip+184 tfan+128 tri / frame.
        {
            static std::mutex s_g66Mtx;
            struct TbpRec { uint32_t tbp; uint32_t tme; uint64_t draw; uint64_t nodraw; };
            static TbpRec s_g66[64]{};
            static uint32_t s_g66N = 0;
            const bool triClass = (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP || tt == GS_PRIM_TRIFAN);
            if (triClass)
            {
                const uint32_t tbp = m_prim.tme ? m_ctx[m_prim.ctxt & 1u].tex0.tbp0 : 0xFFFFFFFFu;
                std::lock_guard<std::mutex> lk(s_g66Mtx);
                uint32_t idx = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < s_g66N; ++i)
                    if (s_g66[i].tbp == tbp) { idx = i; break; }
                if (idx == 0xFFFFFFFFu && s_g66N < 64u) { idx = s_g66N++; s_g66[idx].tbp = tbp; s_g66[idx].tme = m_prim.tme; }
                if (idx != 0xFFFFFFFFu) { if (drawing) s_g66[idx].draw++; else s_g66[idx].nodraw++; }
                static uint64_t s_g66Tick = 0;
                if ((++s_g66Tick % 20000ull) == 0ull)
                {
                    for (uint32_t i = 0; i < s_g66N; ++i)
                        std::fprintf(stderr, "[G66:tbp] tbp=0x%x tme=%u draw=%llu nodraw=%llu\n",
                            s_g66[i].tbp, s_g66[i].tme,
                            (unsigned long long)s_g66[i].draw, (unsigned long long)s_g66[i].nodraw);
                }
            }
        }
        static std::atomic<uint64_t> s_n{0};
        const uint64_t vkn = s_n.fetch_add(1u, std::memory_order_relaxed);
        // G65: steady-state (late) sample of triangle-class coords + tme, skipping the early
        // boot/intro geometry that contaminates the capped per-sample traces. Window at high
        // vkn so we see the actual title menu rock geometry, not boot frames.
        if (vkn >= 80000u && vkn < 80360u &&
            (tt == GS_PRIM_TRIANGLE || tt == GS_PRIM_TRISTRIP) && m_vtxCount >= 3u)
        {
            const GSVertex& a = m_vtxQueue[(m_vtxCount-3) % kMaxVerts];
            const GSVertex& b = m_vtxQueue[(m_vtxCount-2) % kMaxVerts];
            const GSVertex& c = m_vtxQueue[(m_vtxCount-1) % kMaxVerts];
            std::fprintf(stderr, "[G65:late] t=%u draw=%u tme=%u tbp=0x%x v0=(%.0f,%.0f,%.0f) v1=(%.0f,%.0f,%.0f) v2=(%.0f,%.0f,%.0f)\n",
                tt, (unsigned)(drawing?1u:0u), (unsigned)m_prim.tme, m_ctx[m_prim.ctxt&1u].tex0.tbp0,
                a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z);
        }
        if ((vkn % 20000u) == 0u)
            std::fprintf(stderr, "[G65:vk] ALL t0..7= %llu %llu %llu %llu %llu %llu %llu %llu | NODRAW tri/tstrip/tfan= %llu %llu %llu\n",
                (unsigned long long)s_all[0].load(),(unsigned long long)s_all[1].load(),(unsigned long long)s_all[2].load(),
                (unsigned long long)s_all[3].load(),(unsigned long long)s_all[4].load(),(unsigned long long)s_all[5].load(),
                (unsigned long long)s_all[6].load(),(unsigned long long)s_all[7].load(),
                (unsigned long long)s_nodraw[3].load(),(unsigned long long)s_nodraw[4].load(),(unsigned long long)s_nodraw[5].load());
    }

    if (drawing && g14TraceEnabled())
    {
        static std::atomic<uint32_t> s_pt[8]{};
        const uint32_t t = (uint32_t)m_prim.type & 7u;
        const uint32_t c = s_pt[t].fetch_add(1u, std::memory_order_relaxed);
        const bool tri = (t == GS_PRIM_TRIANGLE || t == GS_PRIM_TRISTRIP || t == GS_PRIM_TRIFAN);
        if (tri && c < 40u)
        {
            std::fprintf(stderr, "[G14:tri] type=%u n=%u tme=%u v0=(%.1f,%.1f,%.1f) v1=(%.1f,%.1f,%.1f) v2=(%.1f,%.1f,%.1f)\n",
                         t, c, (uint32_t)m_prim.tme,
                         m_vtxQueue[0].x, m_vtxQueue[0].y, m_vtxQueue[0].z,
                         m_vtxQueue[1].x, m_vtxQueue[1].y, m_vtxQueue[1].z,
                         m_vtxQueue[2].x, m_vtxQueue[2].y, m_vtxQueue[2].z);
            std::fflush(stderr);
        }
        if (((c + 1u) % 5000u) == 0u || (tri && c == 0u))
        {
            std::fprintf(stderr, "[G14:primsum] point=%u line=%u lstrip=%u tri=%u tstrip=%u tfan=%u sprite=%u\n",
                         s_pt[GS_PRIM_POINT].load(), s_pt[GS_PRIM_LINE].load(), s_pt[GS_PRIM_LINESTRIP].load(),
                         s_pt[GS_PRIM_TRIANGLE].load(), s_pt[GS_PRIM_TRISTRIP].load(), s_pt[GS_PRIM_TRIFAN].load(),
                         s_pt[GS_PRIM_SPRITE].load());
            std::fflush(stderr);
        }
    }

    if (drawing)
    {
        G147CountedPerfScope g147DrawScope(g_g147DrawPrimitiveNs, g_g147DrawPrimitiveCount);
        m_rasterizer.drawPrimitive(this);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    G146PerfScope g146Scope(g_g146GsImageNs);
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on)
        {
            static thread_local char s_g219tag[64];
            std::snprintf(s_g219tag, sizeof(s_g219tag), "upload(dbp=0x%x dpsm=0x%x)",
                          m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dpsm);
            g219Sentinel(m_vram, m_vramSize, s_g219tag);
        }
    }

    static const bool s_g145EnvDirtyUpload = envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                             !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD");
    const bool g145DirtyUpload = (g268LiveEnvRead()
                                      ? (envFlagEnabled("DC2_G145_DIRTY_UPLOAD") &&
                                         !envFlagEnabled("DC2_G145_NO_DIRTY_UPLOAD"))
                                      : s_g145EnvDirtyUpload) ||
                                 g260NrEnabled();
    if (!g145DirtyUpload)
        g144FlushPendingUpload();

    // G145 opt-in: the deferred-triangle upload barrier is range-checked after host-to-local is confirmed.
    if (renderQualityTraceEnabled())
    {
        static std::atomic<int> s_f37PidEntry{0};
        const int n = s_f37PidEntry.fetch_add(1, std::memory_order_relaxed);
        const bool isT4HH = (m_bitbltbuf.dpsm == GS_PSM_T4HH || m_bitbltbuf.dpsm == GS_PSM_T4HL);
        // F50.9: also surface the dungeon paletted-texture/CLUT uploads (PSMT4=0x14,
        // PSMT8=0x13, and PSMCT32 CLUTs with dbw==1) which arrive after the first 24
        // (all-T4HH/title) calls, so we can confirm processImageData actually runs for them.
        const bool isDungeonShaped = (m_bitbltbuf.dpsm == GS_PSM_T4 ||
                                      m_bitbltbuf.dpsm == GS_PSM_T8 ||
                                      (m_bitbltbuf.dpsm == GS_PSM_CT32 && m_bitbltbuf.dbw == 1u));
        static std::atomic<int> s_f37PidDng{0};
        const int dng = isDungeonShaped ? s_f37PidDng.fetch_add(1, std::memory_order_relaxed) : 0;
        if (n < 24 || isT4HH || (isDungeonShaped && dng < 48))
        {
            uint32_t nz = 0;
            const uint32_t cap = std::min<uint32_t>(sizeBytes, 256u);
            for (uint32_t i = 0; i < cap; ++i) if (data[i]) ++nz;
            std::fprintf(stderr, "[F37:pid] n=%d trxdir=%u dpsm=0x%x dbp=0x%x dbw=%u rr=(%u,%u) bytes=%u nz=%u pending=%u\n",
                n, m_trxdir, (unsigned)m_bitbltbuf.dpsm, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw,
                m_trxreg.rrw, m_trxreg.rrh, sizeBytes, nz, s_pendingImageBytes);
        }
    }
    if (m_trxdir != 0 || !m_vram)
        return;

    // G145: flush only if this upload overlaps a pending deferred draw's texture/CLUT/target.
    if (g145DirtyUpload)
    {
        g144FlushPendingUploadRange(m_bitbltbuf.dbp,
                                    m_bitbltbuf.dbw,
                                    m_bitbltbuf.dpsm,
                                    m_trxpos.dsax,
                                    m_trxpos.dsay,
                                    m_trxreg.rrw,
                                    m_trxreg.rrh);
    }

    g149BumpVramGen(); // host-to-local upload about to write VRAM: invalidate the texture cache
    // G178/G242: notify only after the pending-draw barrier above has established GS order.
    {
        extern void g178NoteVramWriteRect(uint8_t *, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t, uint32_t);
        g178NoteVramWriteRect(m_vram, m_bitbltbuf.dbp, m_bitbltbuf.dbw, m_bitbltbuf.dpsm,
                              m_trxpos.dsax, m_trxpos.dsay, m_trxreg.rrw, m_trxreg.rrh);
    }

    // [G37:seq] GS-order trace of writes to the 0x2720 work page (uploads), shared with the
    // rasterizer's RTT-draw + composite events, to pin what clobbers the model RTT before the
    // display composite samples it. Gated by DC2_G37_SEQ.
    if (g_g37_seq_trace && g_g37_armed.load(std::memory_order_relaxed) == 1 &&
        m_bitbltbuf.dbp >= 0x2700u && m_bitbltbuf.dbp <= 0x2740u)
    {
        const uint32_t s = g_g37_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (s <= 120u)
        {
            std::fprintf(stderr, "[G37:seq] s=%u UPLOAD dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) bytes=%u\n",
                         s, m_bitbltbuf.dbp, (unsigned)m_bitbltbuf.dbw, (unsigned)m_bitbltbuf.dpsm,
                         m_trxreg.rrw, m_trxreg.rrh, sizeBytes);
            std::fflush(stderr);
        }
    }

    uint32_t dbp = m_bitbltbuf.dbp;
    uint8_t dbw = m_bitbltbuf.dbw;
    uint8_t dpsm = m_bitbltbuf.dpsm;

    // G2: capture the raw host->local source bytes for the font/banner pages so the
    // CT32-alias upload + T8 read swizzle can be solved offline. DC2_DUMP_FONTSRC=1.
    {
        static const bool s_dumpSrc = []() {
            const char *e = std::getenv("DC2_DUMP_FONTSRC");
            return e && e[0] != '0';
        }();
        if (s_dumpSrc && (dbp == 0x2720u || dbp == 0x2f20u || dbp == 0x3220u) && sizeBytes != 0u)
        {
            static std::atomic<int> s_n2720{0}, s_n2f20{0}, s_n3220{0};
            std::atomic<int> *cnt = (dbp == 0x2720u) ? &s_n2720 : (dbp == 0x2f20u) ? &s_n2f20 : &s_n3220;
            int idx = cnt->fetch_add(1, std::memory_order_relaxed);
            if (idx < 8)
            {
                char p[128];
                std::snprintf(p, sizeof(p), "captures/src_%x_%d_dbw%u_rr%ux%u_ds%ux%u.bin",
                              dbp, idx, (unsigned)dbw, m_trxreg.rrw, m_trxreg.rrh, m_trxpos.dsax, m_trxpos.dsay);
                FILE *fp = std::fopen(p, "wb");
                if (fp) { std::fwrite(data, 1, sizeBytes, fp); std::fclose(fp); }
                std::fprintf(stderr, "[G2:srcdump] %s bytes=%u dpsm=0x%x\n", p, sizeBytes, (unsigned)dpsm);
            }
        }
    }

    if (dbw == 0)
        dbw = 1;
    uint32_t base = dbp * 256u;
    uint32_t bpp = bitsPerPixel(dpsm);
    uint32_t stridePixels = static_cast<uint32_t>(dbw) * 64u;

    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t dsax = m_trxpos.dsax;
    uint32_t dsay = m_trxpos.dsay;

    // G155 (perf, bit-exact): the upload-preview candidate below (this full-buffer nonzero scan
    // plus, for CT* uploads, a 512x448x4 RGBA assign+copy) is consumed ONLY by the default-off host
    // upload-preview fallback (uploadPresentationFallbackEnabled -> the [F31:upload-present] latch)
    // and by DC2_PHASE_TRACE traces. It never touches VRAM. In the normal render path it is pure
    // dead work — the title streams ~1.9 MB of host->local image data/frame, so this scan alone is a
    // ~1.9M byte compares/frame tax on the (MTGS worker) upload budget for no visible effect. Skip
    // the scan unless a consumer is active; nonzeroBytes then stays 0 so the candidate block below is
    // naturally skipped (bit-exact). Kill switch DC2_G155_NO_SKIP_PREVIEW forces the old scan.
    uint32_t nonzeroBytes = 0u;
    static const bool s_g155PreviewNeeded =
        uploadPresentationFallbackEnabled() || phaseDiagnosticsEnabled() ||
        envFlagEnabled("DC2_G155_NO_SKIP_PREVIEW");
    if (s_g155PreviewNeeded)
    {
        for (uint32_t i = 0; i < sizeBytes; ++i)
        {
            if (data[i] != 0u)
                ++nonzeroBytes;
        }
    }

    if (nonzeroBytes != 0u && isHostPresentableUploadPsm(dpsm) && rrw != 0u && rrh != 0u)
    {
        const uint32_t candidateWidth = std::min<uint32_t>(rrw, kHostFrameWidth);
        const uint32_t candidateHeight = std::min<uint32_t>(rrh, kHostFrameHeight);
        const uint64_t candidateScore =
            static_cast<uint64_t>(candidateWidth) *
            static_cast<uint64_t>(candidateHeight) *
            static_cast<uint64_t>(nonzeroBytes);
        if (!s_f31UploadPresentationCandidate.valid ||
            candidateScore > s_f31UploadPresentationCandidate.score)
        {
            s_f31UploadPresentationCandidate.valid = true;
            s_f31UploadPresentationCandidate.frame.fbp = dbp;
            s_f31UploadPresentationCandidate.frame.fbw = dbw;
            s_f31UploadPresentationCandidate.frame.psm = dpsm;
            s_f31UploadPresentationCandidate.frame.fbmsk = 0u;
            s_f31UploadPresentationCandidate.width = candidateWidth;
            s_f31UploadPresentationCandidate.height = candidateHeight;
            s_f31UploadPresentationCandidate.sourceOriginX = dsax;
            s_f31UploadPresentationCandidate.sourceOriginY = dsay;
            s_f31UploadPresentationCandidate.nonzeroBytes = nonzeroBytes;
            s_f31UploadPresentationCandidate.score = candidateScore;
            s_f31UploadPresentationCandidate.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);

            if (dpsm == GS_PSM_CT32)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 4u;
                        if (srcOff + 4u > sizeBytes)
                            break;
                        dstRow[x * 4u + 0u] = data[srcOff + 0u];
                        dstRow[x * 4u + 1u] = data[srcOff + 1u];
                        dstRow[x * 4u + 2u] = data[srcOff + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }
            else if (dpsm == GS_PSM_CT24)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 3u;
                        if (srcOff + 3u > sizeBytes)
                            break;
                        dstRow[x * 4u + 0u] = data[srcOff + 0u];
                        dstRow[x * 4u + 1u] = data[srcOff + 1u];
                        dstRow[x * 4u + 2u] = data[srcOff + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }
            else if (dpsm == GS_PSM_CT16 || dpsm == GS_PSM_CT16S)
            {
                for (uint32_t y = 0u; y < candidateHeight; ++y)
                {
                    uint8_t *dstRow = s_f31UploadPresentationCandidate.pixels.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0u; x < candidateWidth; ++x)
                    {
                        const uint32_t srcOff = (y * rrw + x) * 2u;
                        if (srcOff + sizeof(uint16_t) > sizeBytes)
                            break;
                        uint16_t pixel = 0u;
                        std::memcpy(&pixel, data + srcOff, sizeof(pixel));
                        const uint32_t r = pixel & 31u;
                        const uint32_t g = (pixel >> 5) & 31u;
                        const uint32_t b = (pixel >> 10) & 31u;
                        dstRow[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                        dstRow[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                        dstRow[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }
            }

            if (phaseDiagnosticsEnabled())
            {
                static std::atomic<uint32_t> s_f31UploadCandidateCount{0};
                const uint32_t n = s_f31UploadCandidateCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u || (n % 64u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[F31:upload-candidate] n=%u dbp=0x%x dbw=%u psm=0x%x size=(%u,%u) ds=(%u,%u) nonzero=%u score=%llu\n",
                                 n,
                                 dbp,
                                 static_cast<uint32_t>(dbw),
                                 static_cast<uint32_t>(dpsm),
                                 candidateWidth,
                                 candidateHeight,
                                 dsax,
                                 dsay,
                                 nonzeroBytes,
                                 static_cast<unsigned long long>(candidateScore));
                }
            }
        }
    }

    if (phaseDiagnosticsEnabled() && sizeBytes != 0u)
    {
        static std::atomic<uint32_t> s_f31ImageWriteCount{0};
        const uint32_t n = s_f31ImageWriteCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:imgwrite] n=%u bytes=%u nonzero=%u dbp=0x%x dbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) hw=(%u,%u)\n",
                         n, sizeBytes, nonzeroBytes, dbp, static_cast<uint32_t>(dbw),
                         static_cast<uint32_t>(dpsm), rrw, rrh, dsax, dsay,
                         m_hwregX, m_hwregY);
        }
    }

    if (dpsm == GS_PSM_T4HL || dpsm == GS_PSM_T4HH)
    {
        if (dpsm == GS_PSM_T4HH && dbp == 0x1A00u && envFlagEnabled("DC2_DUMP_FONT"))
        {
            static uint32_t s_g5SourceBytes = 0u;
            const uint32_t sourceBytes = (rrw * rrh + 1u) / 2u;
            if (s_g5SourceBytes < sourceBytes)
            {
                const uint32_t bytesToWrite = std::min<uint32_t>(sizeBytes, sourceBytes - s_g5SourceBytes);
                FILE *fp = std::fopen("captures/font4hh_source.bin",
                                      (s_g5SourceBytes == 0u) ? "wb" : "ab");
                if (fp)
                {
                    std::fwrite(data, 1, bytesToWrite, fp);
                    std::fclose(fp);
                }
                s_g5SourceBytes += bytesToWrite;
                std::fprintf(stderr, "[G5:font4hh-src] wrote=%u total=%u expected=%u\n",
                             bytesToWrite, s_g5SourceBytes, sourceBytes);
            }
        }

        if (renderQualityTraceEnabled())
        {
            static std::atomic<int> s_f37T4hhCount{0};
            const int n = s_f37T4hhCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 16)
            {
                uint32_t nz = 0;
                for (uint32_t i = 0; i < sizeBytes; ++i) if (data[i]) ++nz;
                std::fprintf(stderr, "[F37:t4hh-write] call=%d dpsm=0x%x dbp=0x%x dbw=%u bytes=%u nonzero=%u hwx=%u hwy=%u rrw=%u rrh=%u\n",
                    n, static_cast<uint32_t>(dpsm), dbp, static_cast<uint32_t>(dbw),
                    sizeBytes, nz, m_hwregX, m_hwregY, rrw, rrh);
            }
        }
        // Host-to-local transfers use the format's transfer bpp. T4HL/T4HH are packed
        // 4-bit streams even though their destination nibble aliases a CT32 word.
        auto writeT4Nibble = [&](uint8_t nibble)
        {
            if (m_hwregY >= rrh)
                return;

            if (dpsm == GS_PSM_T4HH)
            {
                GSMem::WriteP4HH(m_vram, dbp, dbw, dsax + m_hwregX, dsay + m_hwregY,
                                 static_cast<uint32_t>(nibble & 0x0Fu));
            }
            else
            {
                GSMem::WriteP4HL(m_vram, dbp, dbw, dsax + m_hwregX, dsay + m_hwregY,
                                 static_cast<uint32_t>(nibble & 0x0Fu));
            }

            ++m_hwregX;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        };

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            const uint8_t srcByte = data[offset++];
            const uint32_t xBefore = m_hwregX;

            writeT4Nibble(static_cast<uint8_t>(srcByte & 0x0Fu));
            if ((xBefore + 1u) < rrw && m_hwregY < rrh)
            {
                writeT4Nibble(static_cast<uint8_t>((srcByte >> 4u) & 0x0Fu));
            }
        }
    }
    else if (bpp == 4)
    {
        // F62 (PR #132): PSMT4 upload via the GSMem swizzle module. GSMem takes the
        // buffer width in PAGES; PSMT4 pages are 128 wide, so pass dbw>>1 (the same
        // halving the local GSPSMT4::addrPSMT4 did internally) — byte-identical.
        const uint32_t pageBwT4 = ((static_cast<uint32_t>(dbw) >> 1u) != 0u)
                                      ? (static_cast<uint32_t>(dbw) >> 1u)
                                      : 1u;
        auto writeT4Nibble = [&](uint8_t nibble)
        {
            if (m_hwregY >= rrh)
                return;
            GSMem::WriteP4(m_vram, dbp, pageBwT4, dsax + m_hwregX, dsay + m_hwregY,
                           static_cast<uint32_t>(nibble & 0x0Fu));
            ++m_hwregX;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        };

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            const uint8_t srcByte = data[offset++];
            const uint32_t srcLo = srcByte & 0x0Fu;
            const uint32_t srcHi = (srcByte >> 4) & 0x0Fu;
            const uint32_t xBefore = m_hwregX;

            writeT4Nibble(static_cast<uint8_t>(srcLo));
            if ((xBefore + 1u) < rrw && m_hwregY < rrh)
            {
                writeT4Nibble(static_cast<uint8_t>(srcHi));
            }
        }
    }
    else if (dpsm == GS_PSM_T8)
    {
        // F62 (PR #132): PSMT8 upload via GSMem. PSMT8 pages are 128 wide → dbw>>1
        // (same halving GSPSMT8::addrPSMT8 did internally) — byte-identical.
        const uint32_t pageBwT8 = ((static_cast<uint32_t>(dbw) >> 1u) != 0u)
                                      ? (static_cast<uint32_t>(dbw) >> 1u)
                                      : 1u;
        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t pixelsToCopy = std::min<uint32_t>(pixelsLeft, sizeBytes - offset);
            if (pixelsToCopy == 0)
            {
                break;
            }

            for (uint32_t i = 0; i < pixelsToCopy; ++i)
            {
                const uint32_t vx = dsax + m_hwregX + i;
                const uint32_t vy = dsay + m_hwregY;
                GSMem::WriteP8(m_vram, dbp, pageBwT8, vx, vy,
                               static_cast<uint32_t>(data[offset + i]));
            }

            offset += pixelsToCopy;
            m_hwregX += pixelsToCopy;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        }
    }
    else if (dpsm == GS_PSM_CT24 || dpsm == GS_PSM_Z24)
    {
        // F62 (PR #132): 24-bit upload via GSMem. CT24/Z24 share the 32-bit page
        // layout, so write through WriteCT32 (GSMem CT32 == local addrPSMCT32) and
        // keep the pre-F62 quirk of storing alpha=0x80 in the unused upper byte.
        const uint32_t pageBw24 = (static_cast<uint32_t>(dbw) != 0u)
                                      ? static_cast<uint32_t>(dbw)
                                      : 1u;
        uint32_t transferBpp = 3;

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t srcBytesLeft = pixelsLeft * transferBpp;
            uint32_t bytesAvail = sizeBytes - offset;
            uint32_t pixelsToCopy = pixelsLeft;
            if (srcBytesLeft > bytesAvail)
                pixelsToCopy = bytesAvail / transferBpp;

            if (pixelsToCopy == 0)
                break;

            for (uint32_t p = 0; p < pixelsToCopy; ++p)
            {
                const uint32_t vx = dsax + m_hwregX + p;
                const uint32_t vy = dsay + m_hwregY;
                const uint32_t c = static_cast<uint32_t>(data[offset + p * 3u + 0u]) |
                                   (static_cast<uint32_t>(data[offset + p * 3u + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + p * 3u + 2u]) << 16u) |
                                   (0x80u << 24u);
                GSMem::WriteCT32(m_vram, dbp, pageBw24, vx, vy, c);
            }

            offset += pixelsToCopy * transferBpp;
            m_hwregX += pixelsToCopy;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }
        }
    }
    else
    {
        // F62 (PR #132): remaining formats via GSMem. CT32/Z32 were already swizzled
        // (GSMem CT32/Z32 == local addrPSMCT32, byte-identical). The 16-bit family
        // (CT16/CT16S/Z16/Z16S) was previously written LINEARLY here while the sampler
        // reads it SWIZZLED — GSMem now swizzles the upload so the two agree. T8H stores
        // the index in the upper byte of a 32-bit word. Unknown psm keeps the old linear
        // copy as a safety fallback.
        const uint32_t pageBwOther = (static_cast<uint32_t>(dbw) != 0u)
                                         ? static_cast<uint32_t>(dbw)
                                         : 1u;
        uint32_t bytesPerPixel = bpp / 8u;
        if (bytesPerPixel == 0)
            bytesPerPixel = 4;
        uint32_t strideBytes = stridePixels * bytesPerPixel;

        uint32_t offset = 0;
        while (offset < sizeBytes && m_hwregY < rrh)
        {
            uint32_t dstY = dsay + m_hwregY;
            uint32_t pixelsLeft = rrw - m_hwregX;
            uint32_t bytesLeft = pixelsLeft * bytesPerPixel;
            uint32_t bytesAvail = sizeBytes - offset;
            if (bytesLeft > bytesAvail)
                bytesLeft = (bytesAvail / bytesPerPixel) * bytesPerPixel;

            uint32_t pixelsCopied = bytesLeft / bytesPerPixel;

            for (uint32_t p = 0; p < pixelsCopied; ++p)
            {
                const uint32_t vx = dsax + m_hwregX + p;
                const uint32_t vy = dstY;
                const uint8_t *src = data + offset + p * bytesPerPixel;
                switch (dpsm)
                {
                case GS_PSM_CT32:
                {
                    uint32_t c; std::memcpy(&c, src, 4u);
                    GSMem::WriteCT32(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z32:
                {
                    uint32_t c; std::memcpy(&c, src, 4u);
                    GSMem::WriteZ32(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_CT16:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteCT16(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_CT16S:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteCT16S(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z16:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteZ16(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_Z16S:
                {
                    uint16_t c; std::memcpy(&c, src, 2u);
                    GSMem::WriteZ16S(m_vram, dbp, pageBwOther, vx, vy, c);
                    break;
                }
                case GS_PSM_T8H:
                    GSMem::WriteP8H(m_vram, dbp, pageBwOther, vx, vy,
                                    static_cast<uint32_t>(src[0]));
                    break;
                default:
                {
                    const uint32_t dstOff = base + vy * strideBytes +
                                            (dsax + m_hwregX + p) * bytesPerPixel;
                    if (dstOff + bytesPerPixel <= m_vramSize)
                        std::memcpy(m_vram + dstOff, src, bytesPerPixel);
                    break;
                }
                }
            }

            offset += bytesLeft;
            m_hwregX += pixelsCopied;
            if (m_hwregX >= rrw)
            {
                m_hwregX = 0;
                ++m_hwregY;
            }

            if (bytesLeft == 0)
                break;
        }
    }

    if (t8UploadTraceEnabled() &&
        dpsm == GS_PSM_CT32 &&
        (dbp == 0x2720u || dbp == 0x3220u) &&
        dbw != 0u &&
        rrw != 0u &&
        rrh != 0u &&
        sizeBytes != 0u &&
        f51T8AliasShouldLog())
    {
        const uint32_t t8Tbw = static_cast<uint32_t>(dbw) * 2u;
        const uint32_t sampleW = std::min<uint32_t>(rrw * 2u, 256u);
        const uint32_t sampleH = std::min<uint32_t>(rrh * 2u, 256u);
        uint32_t nzT8 = 0u;
        uint32_t firstOff = 0u;
        uint32_t firstX = 0u;
        uint32_t firstY = 0u;
        uint8_t firstByte = 0u;
        bool haveFirst = false;

        for (uint32_t y = 0u; y < sampleH; ++y)
        {
            for (uint32_t x = 0u; x < sampleW; ++x)
            {
                const uint32_t off = GSPSMT8::addrPSMT8(dbp, t8Tbw, dsax * 2u + x, dsay * 2u + y);
                if (off < m_vramSize && m_vram[off] != 0u)
                {
                    ++nzT8;
                    if (!haveFirst)
                    {
                        firstOff = off;
                        firstX = x;
                        firstY = y;
                        firstByte = m_vram[off];
                        haveFirst = true;
                    }
                }
            }
        }

        std::fprintf(stderr,
                     "[F51:t8alias] dbp=0x%x ct32dbw=%u t8tbw=%u dpsm=0x%x rr=(%u,%u) ds=(%u,%u) bytes=%u srcNz=%u t8Sample=%ux%u nzT8=%u first=(%u,%u off=0x%x byte=0x%02x)\n",
                     dbp, static_cast<uint32_t>(dbw), t8Tbw, static_cast<uint32_t>(dpsm),
                     rrw, rrh, dsax, dsay, sizeBytes, nonzeroBytes, sampleW, sampleH,
                     nzT8, firstX, firstY, firstOff, firstByte);
    }

    // G3: raw VRAM page-region snapshot immediately after this upload completes.
    // Pairs with the at-sample snapshot in the rasterizer to detect a later overwrite
    // of the font page (0x2720 == RTT fbp=0x139). DC2_DUMP_VRAMRGN=1.
    {
        static const bool s_dumpRgn = []() {
            const char *e = std::getenv("DC2_DUMP_VRAMRGN");
            return e && e[0] != '0';
        }();
        if (s_dumpRgn && (dbp == 0x2720u || dbp == 0x2f20u || dbp == 0x3220u) && sizeBytes != 0u)
        {
            static std::atomic<int> s_rc2720{0}, s_rc2f20{0}, s_rc3220{0};
            std::atomic<int> *rc = (dbp == 0x2720u) ? &s_rc2720 : (dbp == 0x2f20u) ? &s_rc2f20 : &s_rc3220;
            const int idx = rc->fetch_add(1, std::memory_order_relaxed);
            if (idx < 6)
            {
                const uint32_t rbase = dbp * 256u;
                const uint32_t rlen = 0x40000u;
                if (rbase + rlen <= m_vramSize)
                {
                    char p[128];
                    std::snprintf(p, sizeof(p), "captures/vram_postup_%x_%d.bin", dbp, idx);
                    FILE *fp = std::fopen(p, "wb");
                    if (fp) { std::fwrite(m_vram + rbase, 1, rlen, fp); std::fclose(fp); }
                    std::fprintf(stderr, "[G3:postup] %s rr=(%u,%u)\n", p, rrw, rrh);
                }
            }
        }
    }

    // F50.10: immediately read VRAM back through the SAME addressing the sampler uses,
    // to prove whether the write landed (independent of any later overwrite/timing).
    if (renderQualityTraceEnabled() && rrw != 0u && rrh != 0u)
    {
        const bool isPT4 = (dpsm == GS_PSM_T4);
        const bool isCT32clut = (dpsm == GS_PSM_CT32 && dbw == 1u);
        if (isPT4 || isCT32clut)
        {
            static std::atomic<int> s_f510rb{0};
            const int rb = s_f510rb.fetch_add(1, std::memory_order_relaxed);
            if (rb < 40)
            {
                const uint32_t sampW = std::min<uint32_t>(rrw, 64u);
                const uint32_t sampH = std::min<uint32_t>(rrh, 64u);
                uint32_t nzBack = 0u;
                if (isPT4)
                {
                    for (uint32_t y = 0; y < sampH; ++y)
                        for (uint32_t x = 0; x < sampW; ++x)
                        {
                            const uint32_t na = GSPSMT4::addrPSMT4(dbp, (dbw != 0 ? dbw : 1u), dsax + x, dsay + y);
                            const uint32_t bo = na >> 1;
                            if (bo < m_vramSize && m_vram[bo] != 0u) ++nzBack;
                        }
                }
                else
                {
                    for (uint32_t y = 0; y < sampH; ++y)
                        for (uint32_t x = 0; x < sampW; ++x)
                        {
                            const uint32_t off = GSPSMCT32::addrPSMCT32(dbp, dbw, dsax + x, dsay + y);
                            if (off + 4u <= m_vramSize)
                            {
                                uint32_t w; std::memcpy(&w, m_vram + off, 4);
                                if (w != 0u) ++nzBack;
                            }
                        }
                }
                std::fprintf(stderr,
                             "[F510:readback] rb=%d dpsm=0x%x dbp=0x%x dbw=%u ds=(%u,%u) rr=(%u,%u) sampled=%ux%u nzInVram=%u\n",
                             rb, (unsigned)dpsm, dbp, (unsigned)dbw, dsax, dsay, rrw, rrh, sampW, sampH, nzBack);
            }
        }
    }
}

void GS::performLocalToHostToBuffer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = m_bitbltbuf.sbw;
    uint8_t spsm = m_bitbltbuf.spsm;

    if (sbw == 0)
        sbw = 1;
    uint32_t base = sbp * 256u;
    uint32_t bpp = bitsPerPixel(spsm);
    uint32_t stridePixels = static_cast<uint32_t>(sbw) * 64u;

    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t ssax = m_trxpos.ssax;
    uint32_t ssay = m_trxpos.ssay;

    // G242: establish GS submission order first, then publish any persistent depth aliased by the
    // source.  Keep the historical default-off path byte-for-byte unchanged.
    if (g242GuestDepthEnabled())
        g144FlushPendingUpload();
    g242PrepareVramReadRect(m_vram, sbp, sbw, spsm, ssax, ssay, rrw, rrh);

    if (bpp == 4)
    {
        uint32_t rowBytes = (rrw + 1u) / 2u;
        if (rowBytes == 0)
            rowBytes = 1;
        m_localToHostBuffer.reserve(rowBytes * rrh);
        uint32_t widthBlocks = static_cast<uint32_t>(sbw);
        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                uint32_t vx = ssax + x;
                uint32_t vy = ssay + y;
                uint32_t nibbleAddr = GSPSMT4::addrPSMT4(sbp, widthBlocks, vx, vy);
                uint32_t byteOff = nibbleAddr >> 1;
                uint8_t nibble = 0;
                if (byteOff < m_vramSize)
                {
                    int shift = static_cast<int>((nibbleAddr & 1u) << 2);
                    nibble = static_cast<uint8_t>((m_vram[byteOff] >> shift) & 0x0Fu);
                }
                if (x & 1u)
                    m_localToHostBuffer.back() = static_cast<uint8_t>((m_localToHostBuffer.back() & 0x0Fu) | (nibble << 4));
                else
                    m_localToHostBuffer.push_back(nibble);
            }
        }
    }
    else if (spsm == GS_PSM_T8)
    {
        m_localToHostBuffer.reserve(rrw * rrh);
        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                const uint32_t src = GSPSMT8::addrPSMT8(sbp, sbw, ssax + x, ssay + y);
                m_localToHostBuffer.push_back((src < m_vramSize) ? m_vram[src] : 0u);
            }
        }
    }
    else if (spsm == GS_PSM_CT24 || spsm == GS_PSM_Z24)
    {
        uint32_t transferBpp = 3;
        m_localToHostBuffer.reserve(rrw * rrh * transferBpp);

        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                uint32_t srcOff = GSPSMCT32::addrPSMCT32(sbp, sbw, ssax + x, ssay + y);
                if (srcOff + 4 <= m_vramSize)
                {
                    m_localToHostBuffer.push_back(m_vram[srcOff + 0]);
                    m_localToHostBuffer.push_back(m_vram[srcOff + 1]);
                    m_localToHostBuffer.push_back(m_vram[srcOff + 2]);
                }
            }
        }
    }
    else
    {
        uint32_t bytesPerPixel = bpp / 8u;
        if (bytesPerPixel == 0)
            bytesPerPixel = 4;
        uint32_t strideBytes = stridePixels * bytesPerPixel;
        uint32_t rowBytes = rrw * bytesPerPixel;
        m_localToHostBuffer.reserve(rowBytes * rrh);

        for (uint32_t y = 0; y < rrh; ++y)
        {
            if (spsm == GS_PSM_CT32 || spsm == GS_PSM_Z32)
            {
                for (uint32_t x = 0; x < rrw; ++x)
                {
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(sbp, sbw, ssax + x, ssay + y);
                    if (srcOff + 4u <= m_vramSize)
                    {
                        m_localToHostBuffer.push_back(m_vram[srcOff + 0u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 1u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 2u]);
                        m_localToHostBuffer.push_back(m_vram[srcOff + 3u]);
                    }
                }
            }
            else
            {
                uint32_t srcOff = base + (ssay + y) * strideBytes + ssax * bytesPerPixel;
                if (srcOff + rowBytes <= m_vramSize)
                {
                    for (uint32_t i = 0; i < rowBytes; ++i)
                        m_localToHostBuffer.push_back(m_vram[srcOff + i]);
                }
            }
        }
    }
}

bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(m_vram, m_vramSize, m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(m_vram, m_vramSize, activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!dst || maxBytes == 0)
        return 0;
    size_t avail = m_localToHostBuffer.size() - m_localToHostReadPos;
    if (avail == 0)
        return 0;
    size_t toCopy = (avail < maxBytes) ? avail : static_cast<size_t>(maxBytes);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, toCopy);
    m_localToHostReadPos += toCopy;
    return static_cast<uint32_t>(toCopy);
}
