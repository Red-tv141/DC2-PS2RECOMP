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
// G285: exact full-page CT32 local-copy fast-path counters. Quiet unless DC2_G285_STAT=1.
std::atomic<uint64_t> g_g285L2lCalls{0};
std::atomic<uint64_t> g_g285L2lEligible{0};
std::atomic<uint64_t> g_g285L2lFast{0};
std::atomic<uint64_t> g_g285L2lBytes{0};
std::atomic<uint64_t> g_g285L2lLegacyNs{0};
std::atomic<uint64_t> g_g285L2lFastNs{0};

// G289 private .cpp-to-.cpp bridge. The native-copy owner is audited against the exact normal
// presentation source rectangle immediately before this TU reads guest VRAM.
void g289_prepare_presentation_read(uint8_t *vram,
                                    uint32_t fbp, uint32_t fbw, uint32_t psm,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    bool useLocalMemoryLayout, bool frameBaseIsPages);

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

