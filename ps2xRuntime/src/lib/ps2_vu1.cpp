#include "runtime/ps2_vu1.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "ps2_log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// G141: measure-first perf scope (skill 17-performance-optimization.md), env-gated
// (DC2_PERF=1), aggregate-only print. Self-contained per-TU so no header/cross-TU
// wiring is needed; single-threaded call site (VU1Interpreter::run on the game thread).
// G141: global VU1-run nanosecond accumulator (external linkage) — includes the GS raster
// nested via XGKICK; mgEndFrame subtracts g_g141GsRasterNs to isolate VU1-interpreter-proper.
std::atomic<uint64_t> g_g141Vu1RunNs{0};
namespace {
struct G141PerfScope
{
    bool on;
    std::chrono::steady_clock::time_point t0;
    double *sum;
    uint32_t *win;
    const char *tag;
    uint32_t window;
    std::atomic<uint64_t> *accumNs = nullptr; // optional global ns accumulator
    // Optional: attribute the call's work-unit count (VU cycles, GS pixels, ...) so the
    // print can report ns-per-unit, not just ns-per-call (a call with 3 cycles and a call
    // with 30000 cycles cost wildly different amounts of real work).
    const uint32_t *unitPtr = nullptr;
    uint64_t *unitSum = nullptr;
    const char *unitLabel = nullptr;
    G141PerfScope(bool on_, double *sum_, uint32_t *win_, const char *tag_, uint32_t window_)
        : on(on_), sum(sum_), win(win_), tag(tag_), window(window_)
    {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~G141PerfScope()
    {
        if (!on) return;
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (accumNs)
            accumNs->fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                std::memory_order_relaxed);
        *sum += std::chrono::duration<double, std::milli>(elapsed).count();
        if (unitPtr && unitSum) *unitSum += *unitPtr;
        if (++(*win) >= window)
        {
            if (unitPtr && unitSum && *unitSum > 0ull)
            {
                std::fprintf(stderr, "[G141:perf] %s avgUs=%.2f window=%u %s=%llu avgNsPerUnit=%.2f\n",
                             tag, (*sum * 1000.0) / static_cast<double>(*win), *win, unitLabel,
                             static_cast<unsigned long long>(*unitSum),
                             (*sum * 1.0e6) / static_cast<double>(*unitSum));
                *unitSum = 0ull;
            }
            else
            {
                std::fprintf(stderr, "[G141:perf] %s avgUs=%.2f window=%u\n", tag,
                             (*sum * 1000.0) / static_cast<double>(*win), *win);
            }
            *sum = 0.0;
            *win = 0u;
        }
    }
};
} // namespace

// G89/G100: title-rock scene flag (loopNo==3 && TitleMap==0x8FC880), defined in
// ps2_gs_rasterizer.cpp, set per-frame by dc2_game_override.cpp::g67_title_scope. Used to
// title-scope the G100 native forced-draw so costume/dungeon VU1 programs are untouched.
extern std::atomic<bool> g_dc2TitleRockScope;
extern std::atomic<uint32_t> g_dc2G136TraceActive;

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

extern Dc2G137VuOrigin g_dc2G137VuOrigins[1024];

// G102: per-strip vertex history for near-plane clipping at the title cull gate. The gate
// fires once per kicking (advancing) strip vertex, so consecutive gate hits are consecutive
// strip vertices; we keep the previous vertex's screen position + q (=1/W) to clip an edge
// that crosses the near plane. Reset at the start of each title VU run. thread_local because
// guest VU execution is single-threaded per run but the interpreter object may be shared.
static thread_local float g_g102PrevScreen[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static thread_local bool g_g102PrevValid = false;
// The most recent IN-FRONT vertex (q>0) of the current strip: the clip anchor used when the
// immediate predecessor is itself behind the near plane (both-behind edges, which otherwise
// leave holes). Re-armed each title VU run alongside g_g102PrevValid.
static thread_local float g_g102LastFront[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static thread_local bool g_g102LastFrontValid = false;

namespace
{
    std::atomic<uint32_t> s_debugVu1XgkickCount{0};
    std::atomic<uint32_t> s_g22Vu1StopCount{0};
    std::atomic<uint32_t> s_g22Vu1XgkickOpCount{0};
    std::atomic<uint32_t> s_g22Vu1XgkickPacketCount{0};
    std::atomic<uint32_t> s_g22Vu1UnknownUpperCount{0};
    std::atomic<uint32_t> s_g22Vu1UnknownLowerCount{0};
    // G29: when >0, trace DIV/RSQRT operands in the model transform (set per model kick).
    std::atomic<int> s_g29DivTrace{0};
    // G87: VU1 Q-register pipeline latency. Real VU1 latches the DIV/SQRT result into Q only
    // after a fixed delay (DIV/SQRT = 7 cycles, RSQRT = 13); microcode that reads Q (MULq/ADDq/
    // SUBq) before then gets the PREVIOUS Q. The title rock lighting relies on this: the
    // point-light attenuation MULq at vu pc 0x1850 reads Q one instruction after an RSQRT at
    // 0x1840 (no WAITQ between) and expects the PIPELINED (older) Q — the old immediate model fed
    // it the fresh Q -> wrong attenuation -> point light (the only R source) ~0 -> green rock.
    // DIV/SQRT/RSQRT now stage into s_vuQPending+s_vuQDelay; the run loop ticks the delay down one
    // per instruction word and commits to m_state.q at 0; WAITQ commits immediately. Single-
    // threaded VU1 (guest thread) -> thread_local is sufficient and persists Q across MSCALs like
    // the HW register. Kill-switch DC2_VU1_NO_QLATENCY restores the immediate model.
    bool g87_q_latency_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_QLATENCY") == nullptr);
        return v;
    }
    thread_local float s_vuQPending = 0.0f;
    thread_local int   s_vuQDelay   = 0;
    // G200: real VU1's FDIV unit stalls a second DIV/SQRT/RSQRT issued while a result is still
    // in flight -- the machine waits until the FIRST result latches into Q, then starts the new
    // op (PCSX2 VUops.cpp: _vuTestFDIVStalls advances VU->cycle to the in-flight op's completion,
    // letting _vuFDIVflush latch VU->VI[REG_Q] BEFORE _vuFDIVAdd replaces the pipe entry). The
    // old single-slot pending model silently DROPPED the in-flight result on re-arm, leaving Q
    // stale-from-before-the-first-DIV for the whole next window. The town/map transform prologue
    // (back-to-back DIV @0x2020/0x2028, then ADDq.w @0x2058/0x2060 building the cull guard-box W
    // bounds at qw21/qw22) hit exactly this: stale Q -> corrupt W bracket -> behind-camera
    // vertices pass the ADC draw gate that real HW's map_0.gs freeze shows 100%-skipped (the
    // G195/G196 foreground water-sheet). Fix: commit the in-flight pending Q before arming the
    // new producer. Kill switch DC2_VU1_NO_QSTALL restores the drop-pending model.
    bool g200_qstall_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_QSTALL") == nullptr);
        return v;
    }
    // G138: VU1 MAC/STATUS flag pipeline (default ON, kill DC2_VU1_NO_MACPIPE). Real VU1 FMAC
    // results — and therefore the MAC flags FMEQ/FMAND/FMOR and the STATUS bits FSAND/FSEQ/FSOR
    // read — become visible ~4 instruction pairs AFTER the FMAC issues, not immediately. The
    // title transform packer's draw/cull gate depends on this: its FMEQ chain at 0x20c0/0x20c8/
    // 0x2108 must read the MAC flags of the guard-plane SUBs at 0x20a0/0x20a8 and the OPMSUB
    // winding test at 0x20c8 (4 pairs earlier), but the immediate model handed it the flags of
    // the interleaved fog MINIy/ADDx ops instead -> the chain could never equal VI3 (0xD0|qw30)
    // -> IBEQ @0x2128 never taken -> +2048 on every vertex -> ADC=1 -> the whole cavern culled
    // (the G70..G137 blue-void/ALLNODRAW line). HW's .gs shows the SAME batches through the SAME
    // microcode drawing their strip bodies selectively (G138 join: HW PRIMED/MIXED == runner
    // ALLNODRAW strips, geometry-identical), which is only possible with pipelined flags.
    // Model: a DEPTH-deep shift register of (mac,status) snapshots advanced once per executed
    // instruction pair; flag-consuming lower ops read the snapshot from DEPTH pairs ago. The
    // architectural m_state.mac/status stay immediate (they are the newest pipeline entry).
    // Same class of fix as the G87 Q-register latency. Depth tunable DC2_VU1_MACPIPE_DEPTH.
    bool g138_macpipe_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_MACPIPE") == nullptr);
        return v;
    }
    int g138_macpipe_depth()
    {
        static const int d = []() -> int {
            const char *e = std::getenv("DC2_VU1_MACPIPE_DEPTH");
            if (!e)
                return 4;
            const long v = std::strtol(e, nullptr, 0);
            return (v >= 1 && v <= 7) ? static_cast<int>(v) : 4;
        }();
        return d;
    }
    thread_local uint32_t s_vuMacPipe[8] = {};
    thread_local uint32_t s_vuStatusPipe[8] = {};
    thread_local uint32_t s_vuMacVisible = 0u;
    thread_local uint32_t s_vuStatusVisible = 0u;
    // G139: same-pair upper->lower VF hazard (default ON, kill DC2_VU1_NO_PAIRHAZ). Real VU1
    // FMAC results land ~4 cycles after issue, so a lower op in the SAME pair as an upper op
    // always reads the register state from BEFORE that upper op. The immediate model committed
    // the upper result first, so `SUB VF24.xyz,VF17,VF16 | SQ VF24 -> 5(VI6)` (title tri packer
    // 0x1fa8) stored the raw edge-vector float bits as the middle vertex XYZ instead of the
    // FTOI4 position from 4 pairs earlier (0x1f88) -> every tri's 2nd vertex exploded (the
    // "beam shard" screen-spanning triangles; .w survived via the .xyz dest mask, which is why
    // the ADC/fog stats still matched HW). Model: snapshot the upper's VF dest before
    // execUpper, expose the OLD value to execLower, then re-apply the upper's masked lanes
    // (the upper result lands later than the lower's same-cycle access, so the upper wins).
    bool g139_pairhaz_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_PAIRHAZ") == nullptr);
        return v;
    }
    // G39: when >0, trace loads (LQ/LQI/LQD/ILW/ILWR/MTIR/XTOP/XITOP) in the model kick so we
    // can see the vertex/matrix/translation source addresses and which read returns 0 on the
    // failing (2nd+) kicks. Armed per model kick; carries the kick index for labelling.
    std::atomic<int> s_g39LoadTrace{0};
    std::atomic<uint32_t> s_g39KickIdx{0};

    bool f50_12_vu_env_flag(const char *name)
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

    const std::vector<std::string> &g108_adc_replay_sequences()
    {
        static bool s_loaded = false;
        static std::vector<std::string> s_seq;
        if (s_loaded)
            return s_seq;
        s_loaded = true;

        const char *path = std::getenv("DC2_G108_ADC_REPLAY_FILE");
        if (!path || path[0] == '\0')
            return s_seq;

        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "[G108:replay] failed to open '%s'\n", path);
            return s_seq;
        }

        std::string line;
        while (std::getline(in, line))
        {
            std::string bits;
            bits.reserve(line.size());
            for (char ch : line)
            {
                if (ch == '0' || ch == '1')
                    bits.push_back(ch);
                else if (ch == '#')
                    break;
            }
            if (!bits.empty())
                s_seq.push_back(bits);
        }
        std::fprintf(stderr, "[G108:replay] loaded %zu ADC packet sequences from '%s'\n",
                     s_seq.size(), path);
        return s_seq;
    }

    bool f50_12_vu_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_TRACE_F50_12");
        return enabled;
    }

    bool g136_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_G136_TRACE");
        return enabled;
    }

    bool g136_trace_all_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_G136_ALL");
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

    uint32_t g136_qword_limit()
    {
        static const uint32_t value = std::min<uint32_t>(g136_env_u32("DC2_G136_QW", 4u), 16u);
        return value;
    }

    uint32_t g136_xg_limit()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_XG_LIMIT", 16u);
        return value;
    }

    uint32_t g136_xg_pc_filter()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_XG_PC", 0xFFFFFFFFu);
        return value;
    }

    bool g137_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_G137_TRACE");
        return enabled;
    }

    uint32_t g137_disp_limit()
    {
        static const uint32_t value = g136_env_u32("DC2_G137_DISP_LIMIT", 120u);
        return value;
    }

    std::atomic<uint32_t> s_g136XgLogged{0};

    bool g67_vu_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_TRACE_G67");
        return enabled;
    }

    bool g95_defer_gif_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_G95_DEFER_GIF");
        return enabled;
    }

    uint64_t f50_12_vu_load64(const uint8_t *data)
    {
        uint64_t value = 0u;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    bool f50_12_vu_is_paletted_psm(uint32_t psm)
    {
        return psm == GS_PSM_T4 ||
               psm == GS_PSM_T8 ||
               psm == GS_PSM_T4HL ||
               psm == GS_PSM_T4HH;
    }

    void f50_12_vu_log_tex0(uint32_t addr,
                            uint32_t totalBytes,
                            const char *source,
                            uint8_t regAddr,
                            uint64_t value,
                            uint32_t tagIndex,
                            uint32_t tagOffset,
                            uint32_t nloop,
                            uint8_t flg,
                            uint32_t nreg,
                            uint32_t loop,
                            uint32_t regSlot)
    {
        const uint32_t tbp = static_cast<uint32_t>(value & 0x3FFFu);
        const uint32_t tbw = static_cast<uint32_t>((value >> 14u) & 0x3Fu);
        const uint32_t psm = static_cast<uint32_t>((value >> 20u) & 0x3Fu);
        const uint32_t tw = static_cast<uint32_t>((value >> 26u) & 0xFu);
        const uint32_t th = static_cast<uint32_t>((value >> 30u) & 0xFu);
        const uint32_t cbp = static_cast<uint32_t>((value >> 37u) & 0x3FFFu);
        const uint32_t cpsm = static_cast<uint32_t>((value >> 51u) & 0xFu);
        const uint32_t csm = static_cast<uint32_t>((value >> 55u) & 0x1u);
        const uint32_t csa = static_cast<uint32_t>((value >> 56u) & 0x1Fu);
        const uint32_t cld = static_cast<uint32_t>((value >> 61u) & 0x7u);
        const bool paletted = f50_12_vu_is_paletted_psm(psm);
        const bool hit = tbp == 0x2580u || cbp == 0x2980u || cpsm == GS_PSM_CT16;
        if (!paletted && !hit)
            return;

        static std::atomic<uint32_t> s_f512VuTex0Count{0};
        static std::atomic<uint32_t> s_f512VuTex0HitCount{0};
        const uint32_t n = s_f512VuTex0Count.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t hitn = hit ? (s_f512VuTex0HitCount.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
        const bool shouldLog = n <= 128u ||
                               (hit && (hitn <= 256u || (hitn % 512u) == 0u)) ||
                               ((n % 2048u) == 0u);
        if (!shouldLog)
            return;

        std::fprintf(stderr,
                     "[F512:xgkick] n=%u hit=%u addr=0x%x totalBytes=0x%x source=%s reg=0x%02x raw=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u tag=%u tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u\n",
                     n, hitn, addr, totalBytes, source, static_cast<uint32_t>(regAddr),
                     static_cast<unsigned long long>(value),
                     tbp, tbw, psm, tw, th, cbp, cpsm, csm, csa, cld,
                     tagIndex, tagOffset, loop, regSlot,
                     static_cast<uint32_t>(flg), nloop, nreg);
    }

    void f50_12_vu_scan_register(uint32_t addr,
                                 uint32_t totalBytes,
                                 const char *source,
                                 uint8_t regAddr,
                                 uint64_t value,
                                 uint32_t tagIndex,
                                 uint32_t tagOffset,
                                 uint32_t nloop,
                                 uint8_t flg,
                                 uint32_t nreg,
                                 uint32_t loop,
                                 uint32_t regSlot)
    {
        if (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2)
            f50_12_vu_log_tex0(addr, totalBytes, source, regAddr, value, tagIndex, tagOffset, nloop, flg, nreg, loop, regSlot);
    }

    void f50_12_vu_scan_xgkick_packet(uint32_t addr, uint32_t totalBytes, const uint8_t *data)
    {
        if (!f50_12_vu_trace_enabled() || !data || totalBytes < 16u)
            return;

        uint32_t offset = 0u;
        uint32_t tagIndex = 0u;
        while (offset + 16u <= totalBytes && tagIndex < 256u)
        {
            const uint32_t tagOffset = offset;
            const uint64_t tagLo = f50_12_vu_load64(data + offset);
            const uint64_t tagHi = f50_12_vu_load64(data + offset + 8u);
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i)
                regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            if (flg == GIF_FMT_PACKED)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 16u > totalBytes)
                            return;
                        const uint64_t lo = f50_12_vu_load64(data + offset);
                        const uint64_t hi = f50_12_vu_load64(data + offset + 8u);
                        offset += 16u;
                        if (regs[r] == 0x0Eu)
                        {
                            f50_12_vu_scan_register(addr, totalBytes, "packed-ad",
                                                     static_cast<uint8_t>(hi & 0xFFu), lo,
                                                     tagIndex, tagOffset, nloop, flg, nreg, loop, r);
                        }
                        else
                        {
                            f50_12_vu_scan_register(addr, totalBytes, "packed-direct",
                                                     regs[r], lo,
                                                     tagIndex, tagOffset, nloop, flg, nreg, loop, r);
                        }
                    }
                }
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 8u > totalBytes)
                            return;
                        const uint64_t value = f50_12_vu_load64(data + offset);
                        offset += 8u;
                        f50_12_vu_scan_register(addr, totalBytes, "reglist",
                                                 regs[r], value,
                                                 tagIndex, tagOffset, nloop, flg, nreg, loop, r);
                    }
                }
                if (((nloop * nreg) & 1u) != 0u)
                {
                    if (offset + 8u > totalBytes)
                        return;
                    offset += 8u;
                }
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                if (imageBytes > static_cast<uint64_t>(totalBytes - offset))
                    return;
                offset += static_cast<uint32_t>(imageBytes);
            }
            else
            {
                return;
            }

            ++tagIndex;
        }
    }

    bool g67_vu_title_tbp(uint32_t tbp)
    {
        switch (tbp)
        {
        case 0x2720u:
        case 0x2760u:
        case 0x28a0u:
        case 0x2b20u:
        case 0x2f20u:
        case 0x3320u:
        case 0x3720u:
        case 0x3960u:
            return true;
        default:
            return false;
        }
    }

    void g67_vu_append_tbp(char *buf, size_t cap, int *p, uint32_t tbp, uint32_t psm, uint32_t cbp, uint32_t verts)
    {
        if (!buf || !p || *p >= static_cast<int>(cap))
            return;
        *p += std::snprintf(buf + *p, cap - static_cast<size_t>(*p),
                            " tbp=0x%x/psm=0x%x/cbp=0x%x verts=%u",
                            tbp, psm, cbp, verts);
    }

    void g67_vu_scan_xgkick_packet(uint32_t addr, uint32_t totalBytes, const uint8_t *data,
                                   uint32_t pc, uint32_t startPC, uint32_t itop)
    {
        if (!g67_vu_trace_enabled() || !data || totalBytes < 16u)
            return;

        uint32_t primLoops[8]{};
        uint32_t primTags[8]{};
        uint32_t vertsByTbp[8]{};
        uint32_t tbps[8]{};
        uint32_t psms[8]{};
        uint32_t cbps[8]{};
        // G83: per-tbp RGBA accumulation from the kicked (VU-output == GS-input) packet.
        // 0x1b68 is a passthrough copy packer, so this equals the EE-delivered per-vertex
        // colour -> splits "is RED already low in the EE packet" (this) from any VU-copy loss.
        uint64_t rSum[8]{}, gSum[8]{}, bSum[8]{}, aSum[8]{}, colN[8]{};
        uint32_t tbpCount = 0u;
        uint32_t tex0Count = 0u;
        uint32_t triVerts = 0u;
        uint32_t adcVerts = 0u;
        uint32_t regVerts = 0u;
        uint32_t curTbp = 0xFFFFFFFFu;
        uint32_t curPsm = 0xFFFFFFFFu;
        uint32_t curCbp = 0xFFFFFFFFu;
        uint32_t curPrim = 0xFFFFFFFFu;
        bool hit3960 = false;

        auto noteTex0 = [&](uint64_t value)
        {
            curTbp = static_cast<uint32_t>(value & 0x3FFFu);
            curPsm = static_cast<uint32_t>((value >> 20u) & 0x3Fu);
            curCbp = static_cast<uint32_t>((value >> 37u) & 0x3FFFu);
            ++tex0Count;
            if (curTbp == 0x3960u)
                hit3960 = true;
            if (g67_vu_title_tbp(curTbp))
            {
                uint32_t idx = 0xFFFFFFFFu;
                for (uint32_t i = 0u; i < tbpCount; ++i)
                {
                    if (tbps[i] == curTbp)
                    {
                        idx = i;
                        break;
                    }
                }
                if (idx == 0xFFFFFFFFu && tbpCount < 8u)
                {
                    idx = tbpCount++;
                    tbps[idx] = curTbp;
                    psms[idx] = curPsm;
                    cbps[idx] = curCbp;
                    vertsByTbp[idx] = 0u;
                }
            }
        };

        // G83: PACKED RGBAQ spread layout -> R=lo[0:7], G=lo[32:39], B=hi[0:7], A=hi[32:39].
        // The title rock geometry packets carry NO TEX0 (G80: textures come from the
        // mgEndDrawReloadTexture cursor, not in-packet), so tbp-keying never fires. Bucket
        // RGBA by PRIM type instead (rock = tstrip+trifan-heavy).
        (void)rSum; (void)gSum; (void)bSum; (void)aSum; (void)colN;
        uint64_t primRsum[8]{}, primGsum[8]{}, primBsum[8]{}, primAsum[8]{}, primColN[8]{};
        auto noteColor = [&](uint64_t lo, uint64_t hi)
        {
            const uint32_t p = curPrim & 7u;
            primRsum[p] += (lo & 0xFFu);
            primGsum[p] += ((lo >> 32u) & 0xFFu);
            primBsum[p] += (hi & 0xFFu);
            primAsum[p] += ((hi >> 32u) & 0xFFu);
            ++primColN[p];
        };

        // G84: per-PRIM VU-output XYZ screen-space range (12.4 fixed -> /16 px) to A/B
        // against HW (tools/g84_gs_geom.py) and decide if the title rock geometry is
        // stretched (wrong positions) vs merely mis-coloured.
        double primXmin[8], primXmax[8], primYmin[8], primYmax[8];
        double primSumX[8]{}, primSumY[8]{};
        uint64_t primZmin[8], primZmax[8], primXyN[8]{};
        // G84b: count verts inside the on-screen raw window (offset~2048 -> screen 0..512/448
        // is raw ~2048..2560 x / 2048..2496 y). Off-window = guard-band/clip extremes.
        uint64_t primOnscr[8]{};
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            primXmin[i] = 1e18; primXmax[i] = -1e18;
            primYmin[i] = 1e18; primYmax[i] = -1e18;
            primZmin[i] = ~0ull; primZmax[i] = 0u;
        }

        auto noteVertex = [&](uint64_t lo, uint64_t hi)
        {
            ++regVerts;
            const uint32_t p = curPrim & 7u;
            if (p == 3u || p == 4u || p == 5u)
            {
                ++triVerts;
                if (((hi >> 47u) & 1u) != 0u)
                    ++adcVerts;
                const double x = (double)(lo & 0xFFFFu) / 16.0;
                const double y = (double)((lo >> 32u) & 0xFFFFu) / 16.0;
                const uint64_t z = hi & 0xFFFFFFu;
                if (x < primXmin[p]) primXmin[p] = x;
                if (x > primXmax[p]) primXmax[p] = x;
                if (y < primYmin[p]) primYmin[p] = y;
                if (y > primYmax[p]) primYmax[p] = y;
                if (z < primZmin[p]) primZmin[p] = z;
                if (z > primZmax[p]) primZmax[p] = z;
                primSumX[p] += x; primSumY[p] += y;
                if (x >= 1900.0 && x <= 2700.0 && y >= 1950.0 && y <= 2550.0)
                    ++primOnscr[p];
                ++primXyN[p];
                for (uint32_t i = 0u; i < tbpCount; ++i)
                {
                    if (tbps[i] == curTbp)
                    {
                        ++vertsByTbp[i];
                        break;
                    }
                }
            }
        };

        uint32_t offset = 0u;
        uint32_t tagIndex = 0u;
        while (offset + 16u <= totalBytes && tagIndex < 256u)
        {
            const uint64_t tagLo = f50_12_vu_load64(data + offset);
            const uint64_t tagHi = f50_12_vu_load64(data + offset + 8u);
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            if (nreg > 16u)
                return;

            const bool pre = ((tagLo >> 46u) & 1u) != 0u;
            if (pre)
            {
                curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);
                const uint32_t p = curPrim & 7u;
                if (p < 8u)
                {
                    primLoops[p] += nloop;
                    ++primTags[p];
                }
            }

            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i)
                regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            if (flg == GIF_FMT_PACKED)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 16u > totalBytes)
                            goto done;
                        const uint64_t lo = f50_12_vu_load64(data + offset);
                        const uint64_t hi = f50_12_vu_load64(data + offset + 8u);
                        offset += 16u;
                        uint8_t reg = regs[r];
                        if (reg == 0x0Eu)
                            reg = static_cast<uint8_t>(hi & 0xFFu);
                        if (reg == GS_REG_TEX0_1 || reg == GS_REG_TEX0_2)
                            noteTex0(lo);
                        else if (reg == 0x01u) // RGBAQ
                            noteColor(lo, hi);
                        else if (reg == 0x04u || reg == 0x05u || reg == 0x0Cu || reg == 0x0Du)
                            noteVertex(lo, hi);
                    }
                }
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 8u > totalBytes)
                            goto done;
                        const uint64_t value = f50_12_vu_load64(data + offset);
                        offset += 8u;
                        const uint8_t reg = regs[r];
                        if (reg == GS_REG_TEX0_1 || reg == GS_REG_TEX0_2)
                            noteTex0(value);
                    }
                }
                if (((nloop * nreg) & 1u) != 0u)
                {
                    if (offset + 8u > totalBytes)
                        goto done;
                    offset += 8u;
                }
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                if (imageBytes > static_cast<uint64_t>(totalBytes - offset))
                    goto done;
                offset += static_cast<uint32_t>(imageBytes);
            }
            else
            {
                goto done;
            }

            ++tagIndex;
        }

    done:
        // G84: per-PRIM VU-output XYZ screen-space range, accumulated like G83. A/B vs HW
        // (tools/g84_gs_geom.py: HW tri/tstrip cluster X~2300-2380 Y~2110-2170 raw). Gated DC2_G84_XYZ.
        {
            static const bool s_g84xyz = (std::getenv("DC2_G84_XYZ") != nullptr);
            if (s_g84xyz && startPC == 0x10u)
            {
                static double gXmin[8], gXmax[8], gYmin[8], gYmax[8], gSumX[8], gSumY[8];
                static uint64_t gZmin[8], gZmax[8], gN[8], gOnscr[8];
                static bool s_init = false;
                static std::atomic<uint64_t> s_g84tick{0};
                if (!s_init)
                {
                    for (uint32_t p = 0u; p < 8u; ++p)
                    {
                        gXmin[p] = 1e18; gXmax[p] = -1e18; gYmin[p] = 1e18; gYmax[p] = -1e18;
                        gZmin[p] = ~0ull; gZmax[p] = 0u; gN[p] = 0u;
                        gSumX[p] = 0.0; gSumY[p] = 0.0; gOnscr[p] = 0u;
                    }
                    s_init = true;
                }
                for (uint32_t p = 0u; p < 8u; ++p)
                {
                    if (primXyN[p] == 0u) continue;
                    if (primXmin[p] < gXmin[p]) gXmin[p] = primXmin[p];
                    if (primXmax[p] > gXmax[p]) gXmax[p] = primXmax[p];
                    if (primYmin[p] < gYmin[p]) gYmin[p] = primYmin[p];
                    if (primYmax[p] > gYmax[p]) gYmax[p] = primYmax[p];
                    if (primZmin[p] < gZmin[p]) gZmin[p] = primZmin[p];
                    if (primZmax[p] > gZmax[p]) gZmax[p] = primZmax[p];
                    gSumX[p] += primSumX[p]; gSumY[p] += primSumY[p];
                    gOnscr[p] += primOnscr[p];
                    gN[p] += primXyN[p];
                }
                const uint64_t t = s_g84tick.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((t % 2000ull) == 0ull)
                {
                    for (uint32_t p = 0u; p < 8u; ++p)
                    {
                        if (gN[p] == 0u) continue;
                        const char *pn = (p == 3u) ? "tri" : (p == 4u) ? "tstrip"
                                       : (p == 5u) ? "trifan" : "other";
                        std::fprintf(stderr,
                                     "[G84:xyz] prim=%u(%s) verts=%llu avg(%.0f,%.0f) onscr=%llu(%.0f%%) X[%.0f,%.0f] Y[%.0f,%.0f] Z[%llu,%llu]\n",
                                     p, pn, (unsigned long long)gN[p],
                                     gSumX[p] / (double)gN[p], gSumY[p] / (double)gN[p],
                                     (unsigned long long)gOnscr[p],
                                     100.0 * (double)gOnscr[p] / (double)gN[p],
                                     gXmin[p], gXmax[p], gYmin[p], gYmax[p],
                                     (unsigned long long)gZmin[p], (unsigned long long)gZmax[p]);
                    }
                }
            }
        }
        // G83: accumulate per-PRIM VU-output RGBA for the title program BEFORE the throttle
        // returns below, so every title kick contributes (= EE-delivered colour; 0x1b68 is a
        // passthrough copy). Compare RED to HW rock avg ~66 (g82_gs_verts.py). Gated DC2_G83_RGBA.
        {
            static const bool s_g83rgba = (std::getenv("DC2_G83_RGBA") != nullptr);
            if (s_g83rgba && startPC == 0x10u)
            {
                static uint64_t gR[8]{}, gG[8]{}, gB[8]{}, gA[8]{}, gN[8]{};
                static std::atomic<uint64_t> s_g83tick{0};
                for (uint32_t pp = 0u; pp < 8u; ++pp)
                {
                    gR[pp] += primRsum[pp]; gG[pp] += primGsum[pp];
                    gB[pp] += primBsum[pp]; gA[pp] += primAsum[pp]; gN[pp] += primColN[pp];
                }
                const uint64_t t = s_g83tick.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((t % 2000ull) == 0ull)
                {
                    for (uint32_t pp = 0u; pp < 8u; ++pp)
                    {
                        if (gN[pp] == 0u) continue;
                        const char *pn = (pp == 3u) ? "tri" : (pp == 4u) ? "tstrip"
                                       : (pp == 5u) ? "trifan" : "other";
                        std::fprintf(stderr,
                                     "[G83:rgba] startPC=0x10 prim=%u(%s) verts=%llu avgRGBA=(%llu,%llu,%llu,%llu)\n",
                                     pp, pn, (unsigned long long)gN[pp],
                                     (unsigned long long)(gR[pp] / gN[pp]),
                                     (unsigned long long)(gG[pp] / gN[pp]),
                                     (unsigned long long)(gB[pp] / gN[pp]),
                                     (unsigned long long)(gA[pp] / gN[pp]));
                    }
                }
            }
        }

        const bool triPacket = (primLoops[3] | primLoops[4] | primLoops[5]) != 0u;
        const bool titleTex = tbpCount != 0u;
        if (!triPacket && !titleTex && !hit3960)
            return;

        static std::atomic<uint32_t> s_g67VuXgkick{0};
        static std::atomic<uint32_t> s_g67VuXgkick3960{0};
        const uint32_t n = s_g67VuXgkick.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t h = hit3960 ? (s_g67VuXgkick3960.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
        if (n > 256u && !hit3960 && (n % 512u) != 0u)
            return;

        char tbpBuf[512];
        int p = 0;
        for (uint32_t i = 0u; i < tbpCount; ++i)
            g67_vu_append_tbp(tbpBuf, sizeof(tbpBuf), &p, tbps[i], psms[i], cbps[i], vertsByTbp[i]);
        if (p <= 0)
            std::snprintf(tbpBuf, sizeof(tbpBuf), " none");

        std::fprintf(stderr,
                     "[G67:xgkick] n=%u hit3960=%u pc=0x%x startPC=0x%x itop=0x%x addr=0x%x bytes=0x%x "
                     "tex0=%u verts=%u triVerts=%u adcVerts=%u primLoops tri/tstrip/tfan=%u/%u/%u "
                     "primTags tri/tstrip/tfan=%u/%u/%u tbps:%s\n",
                     n, h, pc, startPC, itop, addr, totalBytes,
                     tex0Count, regVerts, triVerts, adcVerts,
                     primLoops[3], primLoops[4], primLoops[5],
                     primTags[3], primTags[4], primTags[5],
                     tbpBuf);

    }

    // ---- F51.7 VU1 transform diagnostics (env: DC2_TRACE_VU1) ----
    bool vu1_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_TRACE_VU1");
        return enabled;
    }

    bool g31_vu_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_TRACE_G31");
        return enabled;
    }

    bool g32_vu_trace_enabled()
    {
        static const bool enabled = f50_12_vu_env_flag("DC2_TRACE_G32");
        return enabled;
    }

    std::atomic<uint32_t> s_vu1ExecCount{0};
    std::atomic<uint32_t> s_vu1KickDumpCount{0};
    std::atomic<uint32_t> s_g31VuLqiCount{0};
    std::atomic<uint32_t> s_g32Count{0};

    void g22_log_unknown_upper(uint32_t pc, uint32_t instr, const char *group, uint32_t op, uint32_t sub)
    {
        if (!vu1_trace_enabled())
            return;
        const uint32_t n = s_g22Vu1UnknownUpperCount.fetch_add(1u, std::memory_order_relaxed);
        if (n >= 128u)
            return;
        const uint32_t dest = (instr >> 21u) & 0xFu;
        const uint32_t ft = (instr >> 16u) & 0x1Fu;
        const uint32_t fs = (instr >> 11u) & 0x1Fu;
        const uint32_t fd = (instr >> 6u) & 0x1Fu;
        std::fprintf(stderr,
                     "[G22:vu1unkU] n=%u pc=0x%x group=%s instr=0x%08x op=0x%x sub=0x%x dest=0x%x ft=%u fs=%u fd=%u\n",
                     n, pc, group, instr, op, sub, dest, ft, fs, fd);
    }

    void g22_log_unknown_lower(uint32_t pc, uint32_t instr, const char *group, uint32_t op, uint32_t sub)
    {
        if (!vu1_trace_enabled())
            return;
        const uint32_t n = s_g22Vu1UnknownLowerCount.fetch_add(1u, std::memory_order_relaxed);
        if (n >= 128u)
            return;
        const uint32_t it = (instr >> 16u) & 0x1Fu;
        const uint32_t is = (instr >> 11u) & 0x1Fu;
        const uint32_t id = (instr >> 6u) & 0x1Fu;
        const int16_t imm11 = static_cast<int16_t>(static_cast<int32_t>(instr << 21u) >> 21u);
        std::fprintf(stderr,
                     "[G22:vu1unkL] n=%u pc=0x%x group=%s instr=0x%08x op=0x%x sub=0x%x it=%u is=%u id=%u imm11=%d\n",
                     n, pc, group, instr, op, sub, it, is, id, static_cast<int>(imm11));
    }

    // Dump PRIM, TEX0.tbp and the first few decoded XYZ verts an XGKICK packet
    // emits, decoded EXACTLY as GS::writeRegisterPacked does (case 0x05/0x04/0x0D).
    // G214: watch a single texture page (e.g. the always-culled cap 0x35a0). When set, bypass the
    // 64-kick boot cap and print ONLY XGKICK packets whose TEX0.tbp matches -- so we can answer
    // whether the cap sub-mesh is ever XGKICK'd (=> GS-side cull) or never reaches the kick
    // (=> rejected inside the VU1 model program's guard/cull gate). 0xFFFFFFFF = disabled.
    static uint32_t g214_kick_watch_tbp()
    {
        static const uint32_t w = []() -> uint32_t {
            const char *e = std::getenv("DC2_G214_KICKWATCH");
            return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0xFFFFFFFFu;
        }();
        return w;
    }
    void vu1_dump_xgkick(uint32_t addr, uint32_t totalBytes, const uint8_t *data)
    {
        const uint32_t watch = g214_kick_watch_tbp();
        const bool watching = watch != 0xFFFFFFFFu;
        if ((!vu1_trace_enabled() && !watching) || !data || totalBytes < 16u)
            return;
        const uint32_t kick = s_vu1KickDumpCount.fetch_add(1u, std::memory_order_relaxed);
        if (!watching && kick >= 64u)
            return;

        uint32_t offset = 0u;
        uint32_t tagIndex = 0u;
        uint32_t curTbp = 0xFFFFFFFFu;
        uint32_t curPsm = 0xFFFFFFFFu;
        uint32_t prim = 0xFFFFFFFFu;
        int xyzPrinted = 0;
        uint32_t vertTotal = 0u;

        while (offset + 16u <= totalBytes && tagIndex < 256u)
        {
            const uint64_t tagLo = f50_12_vu_load64(data + offset);
            const uint64_t tagHi = f50_12_vu_load64(data + offset + 8u);
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            const bool pre = ((tagLo >> 46u) & 0x1u) != 0u;
            if (pre)
                prim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);

            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i)
                regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            if (flg == GIF_FMT_PACKED)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 16u > totalBytes)
                            goto done;
                        const uint64_t lo = f50_12_vu_load64(data + offset);
                        const uint64_t hi = f50_12_vu_load64(data + offset + 8u);
                        offset += 16u;
                        uint8_t rd = regs[r];
                        uint64_t dv = lo;
                        if (rd == 0x0Eu)
                        {
                            rd = static_cast<uint8_t>(hi & 0xFFu);
                            dv = lo;
                        }
                        if (rd == 0x06u) // TEX0_1
                        {
                            curTbp = static_cast<uint32_t>(dv & 0x3FFFu);
                            curPsm = static_cast<uint32_t>((dv >> 20u) & 0x3Fu);
                        }
                        else if (rd == 0x05u || rd == 0x0Du) // XYZ2 / XYZ3 packed
                        {
                            ++vertTotal;
                            if (xyzPrinted < 6 && (!watching || curTbp == watch))
                            {
                                const uint32_t x = static_cast<uint32_t>(lo & 0xFFFFu);
                                const uint32_t y = static_cast<uint32_t>((lo >> 32u) & 0xFFFFu);
                                const uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFFu);
                                std::fprintf(stderr,
                                             "[F51.7:xgkick] kick=%u v=%d reg=0x%02x x=%u(%.1f) y=%u(%.1f) z=0x%x lo=0x%016llx hi=0x%016llx\n",
                                             kick, xyzPrinted, rd, x, x / 16.0f, y, y / 16.0f, z,
                                             static_cast<unsigned long long>(lo),
                                             static_cast<unsigned long long>(hi));
                                ++xyzPrinted;
                            }
                        }
                    }
                }
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 8u > totalBytes)
                            goto done;
                        offset += 8u;
                    }
                }
                if (((nloop * nreg) & 1u) != 0u)
                {
                    if (offset + 8u > totalBytes)
                        goto done;
                    offset += 8u;
                }
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                if (imageBytes > static_cast<uint64_t>(totalBytes - offset))
                    goto done;
                offset += static_cast<uint32_t>(imageBytes);
            }
            else
            {
                goto done;
            }
            ++tagIndex;
        }
    done:
        if (!watching || curTbp == watch)
            std::fprintf(stderr,
                         "[F51.7:kick] kick=%u addr=0x%x bytes=0x%x prim=0x%x tbp=0x%x psm=0x%x verts=%u\n",
                         kick, addr, totalBytes, prim, curTbp, curPsm, vertTotal);
    }
}

// Instruction field extraction helpers
static inline uint8_t DEST(uint32_t i) { return (uint8_t)((i >> 21) & 0xF); }
static inline uint8_t FT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t FS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t FD(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline uint8_t BC(uint32_t i) { return (uint8_t)(i & 0x3); }
static inline uint8_t S2_FUNC(uint32_t i) { return (uint8_t)((((i >> 6) & 0x1F) << 2) | (i & 0x3)); }

// Lower instruction field helpers
static inline uint8_t LIT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t LIS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t LID(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline int16_t IMM11(uint32_t i) { return (int16_t)(int32_t)((int32_t)(i << 21) >> 21); }
static inline int16_t IMM15(uint32_t i)
{
    uint32_t lo11 = i & 0x7FF;
    uint32_t hi4 = (i >> 21) & 0xF;
    uint32_t raw = (hi4 << 11) | lo11;
    return (int16_t)(int32_t)((int32_t)(raw << 17) >> 17);
}

VU1Interpreter::VU1Interpreter()
{
    reset();
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f; // VF0.w = 1.0
    m_state.q = 1.0f;
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return vf[bc & 3];
}

// G71: VU1 MAC flag computation. Until now m_state.mac was NEVER written, only read by
// FMAND/FMEQ/FMOR — so every MAC-flag-gated branch evaluated against a constant 0. The
// title-map transform builds its per-vertex draw/cull decision from an FMEQ chain that
// samples the MAC flags of the fog-clamp / winding ops (VU pc 0x1d20/0x1f00.. -> IBEQ
// 0x1d48/0x1f68/0x2128 that skips the ADC `+2048`). With MAC==0 the FMEQ never matched ->
// every kicking vertex got the +2048 -> ADC=1 -> the whole rocky cavern culled (flat blue).
//
// HW MAC layout (per ps2tek; same as F54 VU0 note): 16 bits, four nibbles O[15:12] U[11:8]
// S[7:4] Z[3:0]; within each nibble the lanes are X=bit3 Y=bit2 Z=bit1 W=bit0. Only the
// DEST lanes are flagged; non-dest lanes read 0. Z=result is zero, S=sign bit set (negative,
// incl -0), U=denormal, O=inf/exp-saturated.
static bool s_vu1MacDisabled = (std::getenv("DC2_NO_VU1_MAC") != nullptr);
static const bool s_g106ParallelFlags = (std::getenv("DC2_G106_PARALLEL_FLAGS") != nullptr);
static thread_local bool s_g106UseLowerFlagRead = false;
static thread_local uint32_t s_g106LowerMacRead = 0u;
static thread_local uint32_t s_g106LowerStatusRead = 0u;
// G71/H2 probe: count how often a VU1 FMAC result lane is a denormal (HW flushes to signed 0) or
// inf/NaN (HW clamps to 0x7f7fffff) — i.e. cases where the missing `vuDouble` clamp (PCSX2
// VUops.cpp) would change the value. If DC2 never hits these, H2 is moot. Env DC2_G71_CLAMPCOUNT.
static const bool s_g71clampcount = (std::getenv("DC2_G71_CLAMPCOUNT") != nullptr);
static std::atomic<uint64_t> s_g71denorm{0}, s_g71infnan{0}, s_g71total{0};
static inline uint32_t vu1MakeMac(const float *result, uint8_t dest)
{
    uint32_t mac = 0u;
    for (int i = 0; i < 4; ++i)
    {
        if (!(dest & (0x8u >> i)))
            continue;            // only destination lanes are flagged
        const int b = 3 - i;     // X=bit3 .. W=bit0 within the nibble
        uint32_t bits;
        std::memcpy(&bits, &result[i], 4);
        const uint32_t exp = (bits >> 23) & 0xFFu;
        const uint32_t man = bits & 0x7FFFFFu;
        const bool sign = (bits >> 31) & 1u;
        if (exp == 0u && man == 0u)
            mac |= (1u << (b + 0));      // Z: zero (either sign)
        else if (exp == 0u)
            mac |= (1u << (b + 8));      // U: denormal underflow
        else if (exp == 0xFFu)
            mac |= (1u << (b + 12));     // O: overflow / inf
        if (sign)
            mac |= (1u << (b + 4));      // S: negative
        if (s_g71clampcount)
        {
            s_g71total.fetch_add(1u, std::memory_order_relaxed);
            if (exp == 0u && man != 0u) s_g71denorm.fetch_add(1u, std::memory_order_relaxed);
            else if (exp == 0xFFu)      s_g71infnan.fetch_add(1u, std::memory_order_relaxed);
            if ((s_g71total.load(std::memory_order_relaxed) % 2000000u) == 0u)
                std::fprintf(stderr, "[G71:clampcount] total=%llu denorm=%llu infnan=%llu\n",
                             (unsigned long long)s_g71total.load(),
                             (unsigned long long)s_g71denorm.load(),
                             (unsigned long long)s_g71infnan.load());
        }
    }
    return mac;
}
// G71/H1: derive the VU STATUS flag register from the MAC flags after each FMAC op (PCSX2
// VU_STAT_UPDATE + the sticky accumulate of VUops.cpp). status[3:0]=current Z/S/U/O present,
// [5:4]=D/I (kept), [11:6]=sticky (OR-accumulated). Was a sibling of the MAC bug: m_state.status
// was only ever written by FSSET, so FSAND/FSEQ/FSOR read a stale/zero value. DC2's title VU
// program does not read STATUS, but other VU1 programs may; this keeps it HW-correct.
static inline void vu1UpdateStatus(uint32_t &statusReg, uint32_t mac)
{
    uint32_t cur = 0u;
    if (mac & 0x000Fu) cur |= 0x1u; // Z present in any lane
    if (mac & 0x00F0u) cur |= 0x2u; // S
    if (mac & 0x0F00u) cur |= 0x4u; // U
    if (mac & 0xF000u) cur |= 0x8u; // O
    statusReg = (statusReg & 0xFF0u) | cur | (cur << 6); // keep D/I + sticky, set current, accumulate sticky
}
// G71/H2: optional VU1 result clamp (PCSX2 vuDouble / MAC_UPDATE). Off by default (DC2 renders
// without it); DC2_VU1_CLAMP enables. Flags are taken from the ORIGINAL result (so U/O are set),
// then the stored value is clamped: denormal -> signed zero, inf/NaN/exp-saturated -> +-0x7f7fffff.
// Caller must run this BEFORE applyDest so the clamped value is what gets stored.
static const bool s_vu1Clamp = (std::getenv("DC2_VU1_CLAMP") != nullptr);
static inline void vu1UpdateMac(uint32_t &macReg, uint32_t &statusReg, float *result, uint8_t dest)
{
    if (!s_vu1MacDisabled)
    {
        macReg = vu1MakeMac(result, dest);
        vu1UpdateStatus(statusReg, macReg);
    }
    if (s_vu1Clamp)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (!(dest & (0x8u >> i)))
                continue;
            uint32_t bits;
            std::memcpy(&bits, &result[i], 4);
            const uint32_t exp = (bits >> 23) & 0xFFu;
            if (exp == 0u)
                bits &= 0x80000000u;                 // denormal/zero -> signed zero
            else if (exp == 0xFFu)
                bits = (bits & 0x80000000u) | 0x7f7fffffu; // inf/NaN -> signed max
            else
                continue;
            std::memcpy(&result[i], &bits, 4);
        }
    }
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8)
        dst[0] = result[0]; // x
    if (dest & 0x4)
        dst[1] = result[1]; // y
    if (dest & 0x2)
        dst[2] = result[2]; // z
    if (dest & 0x1)
        dst[3] = result[3]; // w
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    // ACC-writing FMAC ops (ADDA/SUBA/MADDA/MSUBA/MULA/OPMULA/...) update MAC on HW too. Run the
    // MAC/clamp first so the (optional) clamped value is what lands in ACC.
    vu1UpdateMac(m_state.mac, m_state.status, const_cast<float *>(result), dest);
    applyDest(m_state.acc, result, dest);
}

// G65: current program startPC (single-threaded guest exec) so the XGKICK handler can
// attribute emitted prims to the program that built the packet. Bucketed under
// DC2_G65_KICKAGG to find which VU1 program emits the rock tristrips.
static uint32_t s_g65CurStartPC = 0u;

// G215: MAP-4 head/cap batch-cull discriminator. Per model-program execute (startPC=0x10)
// inside the character COutLineDraw window, count how much geometry the batch KICKS out vs how
// much vertex data was uploaded (XTOP window). Reset at execute() entry, bumped at each XGKICK,
// logged after run(). Single-threaded guest exec, same idiom as s_g65CurStartPC. Answers the
// G214 open question: does the head batch reach VU1 but get culled (kicks small/none while input
// present) or is it never submitted by the EE (no execute for it at all)? Default-off.
extern std::atomic<uint64_t> g_dc2PresentTick;        // ps2_runtime.cpp (host present tick mirror)
static const bool s_g215batch = (std::getenv("DC2_G215_BATCH") != nullptr);
static uint32_t s_g215KickCount = 0u;
static uint64_t s_g215KickBytes = 0ull;
static uint32_t s_g215EntryFbp0 = 0u;   // GS ctx0/ctx1 FRAME.fbp at execute entry — character RTT
static uint32_t s_g215EntryFbp1 = 0u;   // renders to fbp=0x139, so scope the batch trace on it

// G216: capture the projected vertex at the model program's final trifan clip gate.
static const bool s_g216clip = (std::getenv("DC2_G216_CLIP") != nullptr);
static const bool s_g216ForceGate = (std::getenv("DC2_G216_FORCE_GATE") != nullptr);
struct G216ClipSample
{
    float p[4];
    uint16_t vi1;
    uint16_t vi10;
};
struct G216ClipStats
{
    uint32_t count;
    uint32_t gateTaken;
    uint32_t gate1d48;
    uint32_t taken1d48;
    uint32_t gate1f68;
    uint32_t taken1f68;
    uint32_t nonFinite;
    uint32_t qNonPositive;
    uint32_t outsideGuard;
    float minv[4];
    float maxv[4];
    G216ClipSample samples[4];
    uint32_t sampleCount;
};
static bool s_g216CharExec = false;
static G216ClipStats s_g216Stats{};
static uint32_t s_g216Header[8]{};

// G217: join the character model's separate TEX0-state and geometry XGKICKs.  G214 proved
// that looking for TEX0 inside the geometry packet is insufficient: DC2 emits the bind and
// vertices in adjacent PATH1 packets.  This observer keeps the last PATH1 TEX0, then reports
// the exact page, packer PC, selector header, primitive, ADC distribution, and screen bounds
// for every kick in the fbp=0x139 model execute.  It never changes VU/GS state or packet bytes.
// Default off; DC2_G217_TICK_MIN/MAX bound the present-tick window and DC2_G217_LIMIT bounds log.
static const bool s_g217Join = (std::getenv("DC2_G217_JOIN") != nullptr);
static const uint64_t s_g217TickMin = g136_env_u32("DC2_G217_TICK_MIN", 0u);
static const uint64_t s_g217TickMax = g136_env_u32("DC2_G217_TICK_MAX", 0xFFFFFFFFu);
static const uint32_t s_g217Limit = g136_env_u32("DC2_G217_LIMIT", 4096u);
static bool s_g217CharExec = false;
static uint32_t s_g217ExecSeq = 0u;
static uint32_t s_g217KickSeq = 0u;
static uint64_t s_g217LastTex0 = 0u;
static uint32_t s_g217Header[8]{};
static const bool s_g217TagVu = (std::getenv("DC2_G217_TAG_VU") != nullptr);
static uint32_t s_g217ExactKind = 0u;
static std::atomic<uint32_t> s_g217Logged{0};

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t itop,
                             uint32_t maxCycles)
{
    s_g65CurStartPC = startPC;
    if (s_g215batch || s_g216clip || s_g216ForceGate || s_g217TagVu)
    {
        s_g215KickCount = 0u; s_g215KickBytes = 0ull;
        s_g215EntryFbp0 = gs.getContextFrame(0).fbp;
        s_g215EntryFbp1 = gs.getContextFrame(1).fbp;
    }
    if (s_g216clip || s_g216ForceGate)
    {
        const uint32_t fbp0 = gs.getContextFrame(0).fbp;
        const uint32_t fbp1 = gs.getContextFrame(1).fbp;
        s_g216CharExec = startPC == 0x10u && (fbp0 == 0x139u || fbp1 == 0x139u);
        s_g216Stats = {};
        std::memset(s_g216Header, 0, sizeof(s_g216Header));
        if (s_g216CharExec && memory)
        {
            const uint32_t top = memory->vif1_regs.top & 0x3FFu;
            for (uint32_t word = 0; word < 8u; ++word)
            {
                const uint32_t off = ((top * 16u) + word * 4u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 4u <= dataSize)
                    std::memcpy(&s_g216Header[word], vuData + off, 4u);
            }
        }
    }
    if (s_g217Join)
    {
        const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
        const uint32_t fbp0 = gs.getContextFrame(0).fbp;
        const uint32_t fbp1 = gs.getContextFrame(1).fbp;
        s_g217CharExec = startPC == 0x10u &&
                         (fbp0 == 0x139u || fbp1 == 0x139u) &&
                         tick >= s_g217TickMin && tick <= s_g217TickMax;
        s_g217KickSeq = 0u;
        std::memset(s_g217Header, 0, sizeof(s_g217Header));
        if (s_g217CharExec)
        {
            ++s_g217ExecSeq;
            if (memory)
            {
                const uint32_t top = memory->vif1_regs.top & 0x3FFu;
                for (uint32_t word = 0; word < 8u; ++word)
                {
                    const uint32_t off = ((top * 16u) + word * 4u) & (PS2_VU1_DATA_SIZE - 1u);
                    if (off + 4u <= dataSize)
                        std::memcpy(&s_g217Header[word], vuData + off, 4u);
                }
            }
        }
    }
    s_g217ExactKind = 0u;
    if (s_g217TagVu && vuData && 38u * 16u + 4u <= dataSize)
    {
        uint32_t selector = 0u;
        std::memcpy(&selector, vuData + 38u * 16u, sizeof(selector));
        if ((selector & 0x40000000u) != 0u) s_g217ExactKind = 1u;
        else if ((selector & 0x20000000u) != 0u) s_g217ExactKind = 2u;
    }
    m_state.pc = startPC;
    m_state.ebit = false;
    m_state.itop = itop;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    if (vu1_trace_enabled())
    {
        const uint32_t n = s_vu1ExecCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
        {
            std::fprintf(stderr, "[F51.7:exec] n=%u startPc=0x%x itop=0x%x dataSize=0x%x\n",
                         n, startPC, itop, dataSize);
            // G28: the latched double-buffer DATA top the model transform reads via XTOP.
            std::fprintf(stderr, "[G28:top] n=%u startPc=0x%x top=0x%x itop=0x%x\n",
                         n, startPC, memory ? (memory->vif1_regs.top & 0x3FFu) : 0u, itop);
        }
    }
    // G27: one-shot dump of the runaway model-transform loop body (~0xe00..0xe50) +
    // the integer registers at kick, so the next phase can decode why the loop counter
    // never satisfies its IBNE exit (transform loop runs to maxcycles -> no valid XGKICK).
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g27dump{0};
        if (s_g27dump.fetch_add(1u, std::memory_order_relaxed) < 10u)
        {
            const uint32_t top = memory ? (memory->vif1_regs.top & 0x3FFu) : 0u;
            auto dumpQw = [&](const char *tag, uint32_t qw)
            {
                const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 16u > dataSize) return;
                float f[4];
                for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                std::fprintf(stderr, "[G28:in] %s qw=0x%x off=0x%x = % .4g % .4g % .4g % .4g\n",
                             tag, qw, off, f[0], f[1], f[2], f[3]);
            };
            std::fprintf(stderr, "[G28:in] --- model kick top=0x%x itop=0x%x ---\n", top, itop);
            // G38: VF/VI register state AT ENTRY (persisted from the prior kick). A matrix held
            // in VF that is valid for early kicks but zero for later ones => clobber/no-reload.
            {
                char vfbuf[1024]; int p = 0;
                for (int r = 0; r < 32; ++r)
                    p += std::snprintf(vfbuf + p, sizeof(vfbuf) - p, "VF%d(% .3g,% .3g,% .3g,% .3g) ",
                                       r, m_state.vf[r][0], m_state.vf[r][1], m_state.vf[r][2], m_state.vf[r][3]);
                std::fprintf(stderr, "[G38:vfentry] %s\n", vfbuf);
                char vibuf[512]; int q = 0;
                for (int r = 0; r < 16; ++r)
                    q += std::snprintf(vibuf + q, sizeof(vibuf) - q, "VI%d=0x%x ", r,
                                       (unsigned)(uint16_t)m_state.vi[r]);
                std::fprintf(stderr, "[G38:vientry] %s\n", vibuf);
            }
            // input vertex buffer the transform reads (XTOP): count nonzero qwords in a wide
            // window so we see whether later batches' vertex buffers are mostly zero.
            uint32_t nzqw = 0u, firstZero = 0xFFFFFFFFu;
            for (uint32_t qw = top; qw < top + 0x60u; ++qw)
            {
                const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 16u > dataSize) break;
                bool nz = false;
                for (uint32_t b = 0; b < 16u; ++b) if (vuData[off + b]) { nz = true; break; }
                if (nz) ++nzqw; else if (firstZero == 0xFFFFFFFFu) firstZero = qw - top;
            }
            std::fprintf(stderr, "[G28:in] vtxwindow top=0x%x nonzeroQW=%u/0x60 firstZeroAt=+0x%x\n",
                         top, nzqw, firstZero);
            for (uint32_t qw = top; qw < top + 8u; ++qw) dumpQw("vtx", qw);
            // G39: the skinned path reads its bone-matrix palette from VU qw 0x3c (LQ 60(VIn)).
            // Dump qw 0x3c..0x47 (first 3 bone matrices) AT ENTRY to confirm whether the palette
            // is ever populated for the skinned (2nd+) kicks.
            for (uint32_t qw = 0x3cu; qw <= 0x47u; ++qw) dumpQw("pal", qw);
        }
    }
    // G38: one-shot dump of the MODEL program code (startPc=0x10) preamble + a bit of the
    // vertex loop, to see how the vertex address (XTOP -> VI -> LQ) and matrix are loaded.
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g38code{0};
        if (s_g38code.fetch_add(1u, std::memory_order_relaxed) == 0u)
        {
            // G39: dump the FULL resident program (setup 0x0 + model trampoline 0x10 jumps to
            // 0x320; transform divides at 0xc28/0xc68; GIF packing ~0x1b00) so it can be
            // disassembled offline to find the multi-kick vertex/matrix addressing bug.
            // G140: dump the WHOLE microcode (was 0x1c90 cap) — the trifan/clip emitters
            // live past 0x2180 and were never disassembled.
            for (uint32_t a = 0x0u; a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
        }
    }
    // G197: generic one-shot-per-distinct-startPC full microcode dump, unscoped (unlike G38's
    // startPC==0x10 title-model-only gate) -- needed to capture the map-piece/mgCVisualMDT
    // "VU program 0" microcode (G195/G196 block-trace) which uses a different startPC than the
    // title programs. Default-off (DC2_G197_VUDUMP); caps at 16 distinct startPC values so it
    // can't runaway across a long session.
    static const bool s_g197vudump = (std::getenv("DC2_G197_VUDUMP") != nullptr);
    if (s_g197vudump)
    {
        static std::atomic<uint32_t> s_g197Seen[16];
        bool doDump = false;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            uint32_t expected = 0u;
            const uint32_t tag = startPC + 1u;
            if (s_g197Seen[i].load(std::memory_order_relaxed) == tag) break;
            if (s_g197Seen[i].compare_exchange_strong(expected, tag, std::memory_order_relaxed))
            {
                doDump = true;
                break;
            }
            if (expected == tag) break;
        }
        if (doDump)
        {
            std::fprintf(stderr, "[G197:vudump] startPC=0x%x codeSize=0x%x\n", startPC, codeSize);
            for (uint32_t a = 0x0u; a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
            std::fflush(stderr);
        }
    }
    // G198: the gate chain's ILW.x VI8,30(VI0) @0x2058 reads a fixed data-memory slot (qw 30,
    // byte 0x1E0) whose x-component IORs into VI10 (with the 0xD0 constant) before the FMAND
    // cascade. Dump it once per kick (capped) to check whether it's a resident constant (same
    // every kick, same title vs map) or something per-object/per-draw upload seeds differently --
    // a plain wrong-constant bug would need no VU1 interpreter change at all. Default-off
    // (DC2_G198_GATE_TRACE, shares the gate-trace env with the VI dump above).
    {
        static const bool s_g198qw30 = (std::getenv("DC2_G198_GATE_TRACE") != nullptr);
        if (s_g198qw30 && startPC == 0x10u && vuData && dataSize > 30u * 16u + 4u)
        {
            static std::atomic<uint32_t> s_g198KickN{0};
            const uint32_t kn = s_g198KickN.fetch_add(1u, std::memory_order_relaxed);
            if (kn < 200u)
            {
                uint32_t qw30x = 0u;
                std::memcpy(&qw30x, vuData + 30u * 16u, 4);
                std::fprintf(stderr, "[G198:qw30] kick=%u qw30.x=0x%08x\n", kn, qw30x);
            }
        }
    }
    // G61: one-shot dump of the TITLE MAP GIF-packing program (kick pc=0x2d88, uploaded to
    // 0x2b20). HW emits this geometry as PRIM=tristrip(3) but the runner emits trifan(5); we
    // need the microcode that builds the output GIFtag PRIM field. Gate on a dedicated env so
    // it never perturbs the costume/setup dumps. Dump the whole GIF-pack region 0x2a00..0x2e90.
    static const bool s_g61vudump = (std::getenv("DC2_G61_VUDUMP") != nullptr);
    if (s_g61vudump)
    {
        static std::atomic<uint32_t> s_g61{0};
        // Dump only when the map GIF-pack region is actually populated (XGKICK lower op at
        // pc=0x2d88), so we skip the setup/costume executes. XGKICK lower encoding low byte
        // pattern: the dispatcher matches 0x6C in the decoded op; detect it by the raw lower
        // word's top opcode bits == 0x6C (lower special op field bits[31:25]>>1 ... ) — to be
        // robust just require non-zero code there and dump up to 4 distinct executes.
        if (s_g61.load(std::memory_order_relaxed) < 4u && 0x2d88u + 8u <= codeSize)
        {
            uint32_t loK = 0u; std::memcpy(&loK, vuCode + 0x2d88u, 4);
            if (loK != 0u)
            {
                s_g61.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G61:vudump] startPC=0x%x codeSize=0x%x lo@2d88=0x%08x\n",
                             startPC, codeSize, loK);
                for (uint32_t a = 0x2a00u; a <= 0x3300u && a + 8u <= codeSize; a += 8u)
                {
                    uint32_t lo = 0u, up = 0u;
                    std::memcpy(&lo, vuCode + a, 4);
                    std::memcpy(&up, vuCode + a + 4, 4);
                    std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
                }
                std::fflush(stderr);
            }
        }
    }
    // G29: the setup kick (startPc=0x0) computes the projection-scale constants. Dump its
    // microcode + input qw0..0xc at entry, then qw0x4..0x6 after run, to find where the
    // poisoned ~-FLT_MAX scale comes from (model verts -> -inf).
    bool g29SetupDump = false;
    if (vu1_trace_enabled() && startPC == 0x0u)
    {
        static std::atomic<uint32_t> s_g29{0};
        if (s_g29.fetch_add(1u, std::memory_order_relaxed) < 2u)
        {
            g29SetupDump = true;
            std::fprintf(stderr, "[G29:setup] --- setup kick top=0x%x itop=0x%x ---\n",
                         memory ? (memory->vif1_regs.top & 0x3FFu) : 0u, itop);
            for (uint32_t a = 0x0u; a <= 0xd0u && a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G29:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
            for (uint32_t qw = 0u; qw < 13u; ++qw)
            {
                const uint32_t off = qw * 16u;
                if (off + 16u > dataSize) break;
                float f[4];
                for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                std::fprintf(stderr, "[G29:in] qw=0x%x = % .4g % .4g % .4g % .4g\n",
                             qw, f[0], f[1], f[2], f[3]);
            }
        }
    }
    // G29: arm DIV/RSQRT tracing for the first 2 model kicks (find the -inf source).
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g29arm{0};
        const uint32_t kickIdx = s_g29arm.fetch_add(1u, std::memory_order_relaxed);
        s_g29DivTrace.store(kickIdx < 16u ? 200 : 0, std::memory_order_relaxed);
        // G39: arm the load trace for the first 6 model kicks (n=1 good, n=3+ bad). 120 load
        // logs per kick covers the preamble vertex/matrix pointer setup + first vertices.
        s_g39KickIdx.store(kickIdx, std::memory_order_relaxed);
        s_g39LoadTrace.store(kickIdx < 6u ? 120 : 0, std::memory_order_relaxed);
    }
    else
    {
        s_g29DivTrace.store(0, std::memory_order_relaxed);
        s_g39LoadTrace.store(0, std::memory_order_relaxed);
    }
    // G85: one-shot dump of the title-program (startPc=0x10) VU INPUT qwords 36..60 — the
    // light-colour matrix / ambient / light positions / fog. Maps the EE lightInfo (byte-
    // identical to HW, G84) to the VU qwords the lighting subs read (40/41/45-48) so the
    // R-vs-G/B channel routing of the directional term can be pinned. Env DC2_G85_IN, quiet.
    {
        static const bool s_g85in = (std::getenv("DC2_G85_IN") != nullptr);
        if (s_g85in && startPC == 0x10u && vuData)
        {
            static std::atomic<uint32_t> s_g85inN{0};
            const uint32_t kn = s_g85inN.fetch_add(1u, std::memory_order_relaxed);
            if (kn < 60u)
            {
                // selector at qw38.x (integer) + scan qw 8..60 for the light/colour data,
                // print only non-zero qwords so the rock's lighting signature is easy to spot.
                uint32_t sel = 0u;
                if (38u * 16u + 4u <= dataSize) std::memcpy(&sel, vuData + 38u * 16u, 4);
                std::fprintf(stderr, "[G85:in] kick=%u dataSize=0x%x sel(qw38)=0x%x\n", kn, dataSize, sel);
                for (uint32_t qw = 8u; qw <= 60u; ++qw)
                {
                    const uint32_t off = qw * 16u;
                    if (off + 16u > dataSize) break;
                    float f[4];
                    for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                    if (f[0] == 0.f && f[1] == 0.f && f[2] == 0.f && f[3] == 0.f) continue;
                    std::fprintf(stderr, "[G85:in]   qw=%u = % .4g % .4g % .4g % .4g\n",
                                 qw, f[0], f[1], f[2], f[3]);
                }
                std::fflush(stderr);
            }
        }
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (s_g217ExactKind != 0u)
    {
        uint32_t selector = 0u;
        if (vuData && 38u * 16u + 4u <= dataSize)
            std::memcpy(&selector, vuData + 38u * 16u, sizeof(selector));
        std::fprintf(stderr,
            "[G217:exactvu] tick=%llu kind=%u startPC=0x%x top=0x%x itop=0x%x selector=0x%08x kicks=%u bytes=%llu\n",
            static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
            s_g217ExactKind, startPC, memory ? (memory->vif1_regs.top & 0x3ffu) : 0u,
            itop, selector, s_g215KickCount,
            static_cast<unsigned long long>(s_g215KickBytes));
        std::fflush(stderr);
    }
    if (s_g216CharExec)
    {
        const G216ClipStats &st = s_g216Stats;
        const G216ClipSample &a = st.samples[0];
        const G216ClipSample &b = st.samples[st.sampleCount > 1u ? 1u : 0u];
        static std::atomic<uint32_t> s_g216seq{0};
        const uint32_t seq = s_g216seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G216:clip] seq=%u tick=%llu hdr=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x "
                     "g1d48=%u/%u g1f68=%u/%u g2128=%u/%u nonfin=%u qle0=%u outside=%u kicks=%u bytes=%llu "
                     "x=[%.3f,%.3f] y=[%.3f,%.3f] z=[%.3f,%.3f] q=[%.6g,%.6g] "
                     "s0=(%.3f,%.3f,%.3f,%.6g;%04x/%04x) s1=(%.3f,%.3f,%.3f,%.6g;%04x/%04x)\n",
                     seq, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     s_g216Header[0], s_g216Header[1], s_g216Header[2], s_g216Header[3],
                     s_g216Header[4], s_g216Header[5], s_g216Header[6], s_g216Header[7],
                     st.taken1d48, st.gate1d48, st.taken1f68, st.gate1f68,
                     st.gateTaken, st.count, st.nonFinite, st.qNonPositive, st.outsideGuard,
                     s_g215KickCount, (unsigned long long)s_g215KickBytes,
                     st.minv[0], st.maxv[0], st.minv[1], st.maxv[1],
                     st.minv[2], st.maxv[2], st.minv[3], st.maxv[3],
                     a.p[0], a.p[1], a.p[2], a.p[3], a.vi1, a.vi10,
                     b.p[0], b.p[1], b.p[2], b.p[3], b.vi1, b.vi10);
        std::fflush(stderr);
    }
    // G215: per-model-batch input-vs-kick tally for the character RTT (fbp=0x139). Distinguishes
    // an EE-skipped sub-mesh (no execute for it) from a VU1-culled one (input present, 0 kicks).
    if (s_g215batch && startPC == 0x10u &&
        (s_g215EntryFbp0 == 0x139u || s_g215EntryFbp1 == 0x139u))
    {
        const uint32_t top = memory ? (memory->vif1_regs.top & 0x3FFu) : 0u;
        uint32_t nzqw = 0u;
        for (uint32_t qw = top; qw < top + 0x200u; ++qw)
        {
            const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
            if (off + 16u > dataSize) break;
            bool nz = false;
            for (uint32_t b = 0; b < 16u; ++b) if (vuData[off + b]) { nz = true; break; }
            if (nz) ++nzqw;
        }
        static std::atomic<uint32_t> s_g215seq{0};
        const uint32_t seq = s_g215seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G215:batch] seq=%u tick=%llu fbp0=0x%x fbp1=0x%x top=0x%x itop=0x%x inNZqw=%u kicks=%u kickBytes=%llu\n",
                     seq, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     s_g215EntryFbp0, s_g215EntryFbp1, top, itop, nzqw,
                     s_g215KickCount, (unsigned long long)s_g215KickBytes);
        std::fflush(stderr);
    }
    if (g29SetupDump)
    {
        for (uint32_t qw = 4u; qw <= 6u; ++qw)
        {
            const uint32_t off = qw * 16u;
            if (off + 16u > dataSize) break;
            float f[4];
            for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
            std::fprintf(stderr, "[G29:out] qw=0x%x = % .4g % .4g % .4g % .4g\n",
                         qw, f[0], f[1], f[2], f[3]);
        }
    }
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t itop, uint32_t maxCycles)
{
    m_state.ebit = false;
    m_state.itop = itop;
    if (vu1_trace_enabled())
    {
        const uint32_t n = s_vu1ExecCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
            std::fprintf(stderr, "[F51.7:resume] n=%u pc=0x%x itop=0x%x\n", n, m_state.pc, itop);
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    static const bool s_g141PerfOn = (std::getenv("DC2_PERF") != nullptr);
    static double s_g141Sum = 0.0;
    static uint32_t s_g141Win = 0u;
    static uint64_t s_g141CycleSum = 0ull;
    G141PerfScope g141Scope(s_g141PerfOn, &s_g141Sum, &s_g141Win, "vu1.run", 2000u);
    g141Scope.accumNs = &g_g141Vu1RunNs;

    const uint32_t startPc = m_state.pc;
    uint32_t cycle = 0u;
    g141Scope.unitPtr = &cycle;
    g141Scope.unitSum = &s_g141CycleSum;
    g141Scope.unitLabel = "cycles";
    uint32_t lastPc = m_state.pc;
    uint32_t lastLower = 0u;
    uint32_t lastUpper = 0u;
    const char *stopReason = "maxcycles";
    uint32_t pendingBranchTarget = 0u;
    bool branchDelayActive = false;

    // G100: native title-rock forced draw (default-ON, kill DC2_G100_NO_FORCEDRAW). The
    // transform packers' +2048/ADC cull gate (IBEQ VI10,VI1 @0x1d48/0x2128) is structurally
    // never taken (VI10=208, VI1 in {0,1}) so 100% of the title rock verts get +2048 -> ADC=1
    // -> no-draw -> flat blue (G70-G74). Forcing VI1:=VI10 at the gate makes the branch taken
    // (skip +2048 = draw) so the native transform path renders the cavern/walls/water that the
    // retired G78/G80 copy band-aid could not. Title-scoped via g_dc2TitleRockScope (constant
    // for the duration of one VU program) so costume/dungeon are untouched; the off-screen
    // strip-restart that HW's ADC performs is approximated by the G89 rasterizer guard band.
    // Legacy diagnostic DC2_G72_FORCEDRAW still forces regardless of scope.
    // G138: RETIRED BY DEFAULT. The gate is no longer "structurally never taken" — that was a
    // double defect in the RUNNER, fixed at the root: (1) the lower-opcode table had FMEQ/FMAND
    // swapped (0x18/0x1A) so the FMAND guard-mask cascade ran as FMEQ 0/1 compares, and (2) MAC
    // flags were read un-pipelined so each FMAND saw the wrong FMAC's flags. With both fixed the
    // natural per-vertex ADC is bit-exact vs the HW reference (G138 join), and forcing the gate
    // OVER-draws (it overrides the faithful cull and double-draws the 0x1f68 3-vert loop).
    // Re-enable the old band-aid with DC2_G100_FORCE_DRAW=1 (plus DC2_VU1_NO_FMSWAPFIX=1 +
    // DC2_VU1_NO_MACPIPE=1 to reproduce the full pre-G138 render).
    static const bool s_g100native = (std::getenv("DC2_G100_FORCE_DRAW") != nullptr &&
                                      std::getenv("DC2_G100_NO_FORCEDRAW") == nullptr);
    static const bool s_g72force   = (std::getenv("DC2_G72_FORCEDRAW") != nullptr);
    // G101 A/B: disable the forced draw while KEEPING the rest of the G100 path (copy-off) so the
    // gate's NATURAL per-vertex cull (now that G71 computes the MAC flags VI7 feeds, and G99 fixed
    // the camera) can be measured against HW's selective ADC. Diagnostic only.
    static const bool s_gateNatural = (std::getenv("DC2_G101_GATE_NATURAL") != nullptr);
    const bool g100ForceTitleDraw =
        startPc == 0x10u &&
        (s_g72force ||
         (s_g100native && !s_gateNatural && g_dc2TitleRockScope.load(std::memory_order_relaxed)));

    // G102 attempted to patch individual behind-camera strip vertices in VU space. That cannot
    // represent true clipping for a shared tristrip vertex (each adjacent triangle needs its own
    // edge intersection), and its fallback anchor can connect unrelated batches. G104 supersedes
    // it with title-only triangle clipping in the GS rasterizer, so the VU-side vertex rewrite is
    // now opt-in for A/B only.
    static const bool s_g102clip =
        (std::getenv("DC2_G102_ENABLE_VU_CLIP") != nullptr &&
         std::getenv("DC2_G102_NO_CLIP") == nullptr);
    // G104 default path: let every title-rock vertex reach the GS unchanged and clip assembled
    // triangles there, where all three vertices are known. Kill with DC2_G104_NO_TRI_CLIP.
    static const bool s_g104triClip = (std::getenv("DC2_G104_NO_TRI_CLIP") == nullptr);
    static const float s_g102wnear = []() -> float {
        const char *e = std::getenv("DC2_G102_WNEAR");
        const float v = e ? static_cast<float>(std::atof(e)) : 4.0f;
        return (v > 0.0f) ? v : 4.0f;
    }();
    if (g100ForceTitleDraw)
    {
        // Reset the immediate-predecessor adjacency at the start of each title VU run/strip.
        // g_g102LastFront is deliberately NOT reset: it is only written by in-front title
        // vertices and only read in title scope, so persisting it across batches gives a valid
        // clip anchor to batches that OPEN behind the camera (otherwise those verts have no
        // anchor and leave holes). The near-static title camera makes a prior-batch front
        // vertex a good anchor.
        g_g102PrevValid = false;
    }

    // G141 perf: hoist the per-cycle correctness-machinery flags and thread_local base
    // pointers OUT of the hot loop. g87/g138/g139 enabled() are process-constant magic-static
    // bools (a guard check per call) and the pipeline state is thread_local (a TLS lookup per
    // access, ~8x/cycle for the MAC shift register). Both are invariant for the whole run on a
    // single thread, so resolving them ONCE here removes ~50% of VU1 time (measured: MAC-pipe
    // machinery ~108ns of ~218ns/cycle). Behavior-identical — same storage, same values; golden
    // title smoke must stay 211646. TLS addresses are stable for the thread's lifetime, and run()
    // is re-entered per MSCAL so each call re-caches its own thread's base.
    const bool s_qlatOn        = g87_q_latency_enabled();
    const bool s_macpipeOn     = g138_macpipe_enabled();
    const int  s_macpipeDepth  = s_macpipeOn ? g138_macpipe_depth() : 4;
    const bool s_pairhazOn     = g139_pairhaz_enabled();
    (void)s_qlatOn;
    uint32_t *const s_macPipeP     = s_vuMacPipe;
    uint32_t *const s_statusPipeP  = s_vuStatusPipe;
    uint32_t *const s_macVisP      = &s_vuMacVisible;
    uint32_t *const s_statusVisP   = &s_vuStatusVisible;
    int      *const s_qDelayP      = &s_vuQDelay;

    for (; cycle < maxCycles; ++cycle)
    {
        const uint32_t currentPc = m_state.pc;
        const int16_t g72_vi1_before = (int16_t)m_state.vi[1];
        if (s_g216CharExec && currentPc == 0x1d48u)
        {
            ++s_g216Stats.gate1d48;
            if ((uint16_t)m_state.vi[1] == (uint16_t)m_state.vi[10]) ++s_g216Stats.taken1d48;
        }
        if (s_g216CharExec && currentPc == 0x1f68u)
        {
            ++s_g216Stats.gate1f68;
            if ((uint16_t)m_state.vi[7] == (uint16_t)m_state.vi[10]) ++s_g216Stats.taken1f68;
        }
        if (s_g216CharExec && currentPc == 0x2128u)
        {
            G216ClipStats &st = s_g216Stats;
            const float *p = m_state.vf[16];
            if (st.count == 0u)
            {
                for (int lane = 0; lane < 4; ++lane)
                    st.minv[lane] = st.maxv[lane] = p[lane];
            }
            else
            {
                for (int lane = 0; lane < 4; ++lane)
                {
                    st.minv[lane] = std::min(st.minv[lane], p[lane]);
                    st.maxv[lane] = std::max(st.maxv[lane], p[lane]);
                }
            }
            bool finite = true;
            for (int lane = 0; lane < 4; ++lane)
                finite = finite && std::isfinite(p[lane]);
            if (!finite) ++st.nonFinite;
            if (p[3] <= 0.0f) ++st.qNonPositive;
            // G215 byte-matched these character-RTT guard limits against PCSX2.
            if (p[0] < 1766.4f || p[0] > 2329.6f ||
                p[1] < 1819.2f || p[1] > 2276.8f)
                ++st.outsideGuard;
            if ((uint16_t)m_state.vi[1] == (uint16_t)m_state.vi[10]) ++st.gateTaken;
            if (st.sampleCount < 4u)
            {
                G216ClipSample &sample = st.samples[st.sampleCount++];
                std::memcpy(sample.p, p, sizeof(sample.p));
                sample.vi1 = (uint16_t)m_state.vi[1];
                sample.vi10 = (uint16_t)m_state.vi[10];
            }
            ++st.count;
        }
        if (s_g216ForceGate && s_g216CharExec &&
            (currentPc == 0x1d48u || currentPc == 0x2128u))
            m_state.vi[1] = m_state.vi[10];
        // G72 force-draw experiment: the tristrip(0x1d48)/trifan(0x2128) packer cull gates
        // (IBEQ VI10,VI1) are never taken (VI10=208, VI1 in {0,1,3,23,...}) -> +2048 ADC=1 on
        // 100% of verts -> title flat blue. Force VI1:=VI10 just before the gate so the branch
        // is taken (skip +2048 = draw) to confirm the over-cull is the flat-blue root. Env
        // DC2_G72_FORCEDRAW; startPc==0x10 only; diagnostic, default off.
        // G100: the decision is hoisted to g100ForceTitleDraw (title-scoped, default-ON).
        static const bool s_g106NoForce1d48 = (std::getenv("DC2_G106_NO_FORCE_1D48") != nullptr);
        static const bool s_g106NoForce2128 = (std::getenv("DC2_G106_NO_FORCE_2128") != nullptr);
        const bool g106GateForceEnabled =
            !((currentPc == 0x1d48u && s_g106NoForce1d48) ||
              (currentPc == 0x2128u && s_g106NoForce2128));
        if (g100ForceTitleDraw &&
            g106GateForceEnabled &&
            (currentPc == 0x1d48u || currentPc == 0x2128u))
        {
            // G101 probe: dump the candidate position VFs at the gate so we can find which one
            // holds the transformed screen position and its on-screen range (to drive a SELECTIVE
            // force = HW per-vertex ADC). DC2_G101_VFPROBE, title-scoped, first 80 gate hits.
            static const bool s_vfprobe = (std::getenv("DC2_G101_VFPROBE") != nullptr);
            if (s_vfprobe)
            {
                static std::atomic<uint32_t> s_vn{0};
                const uint32_t k = s_vn.fetch_add(1u, std::memory_order_relaxed);
                if (k < 2000u)
                {
                    const float *p = m_state.vf[16];
                    std::fprintf(stderr, "[G101:vf] pc=0x%x x=%.0f y=%.0f z=%.0f q=%.4f\n",
                                 currentPc, p[0], p[1], p[2], p[3]);
                }
            }
            // G101 (selective per-vertex ADC = HW near-plane clip): the G100 forced draw skipped the
            // +2048/ADC cull for EVERY vertex, including verts BEHIND the camera (projected q=1/W<=0),
            // which map to flipped/extreme screen positions and swing wildly during the title camera
            // zoom -- the "vertex explosion". Force the draw ONLY for verts in FRONT of the camera;
            // leave behind-camera verts to the natural +2048 -> ADC=1 -> strip restart, exactly like
            // HW's near-plane clip. The projected vertex is VF16=(x,y,z,q); q=VF16.w. A stable binary
            // criterion (no screen-space threshold flicker) that removes only non-visible behind-
            // camera geometry (no new holes). DC2_G101_NO_QCLIP restores the G100 unconditional force;
            // DC2_G101_QMIN tunes the near-plane epsilon.
            static const bool s_noQClip = (std::getenv("DC2_G101_NO_QCLIP") != nullptr);
            static const float s_qmin = []() -> float {
                const char *e = std::getenv("DC2_G101_QMIN");
                return e ? static_cast<float>(std::atof(e)) : 0.0f;
            }();
            static const uint32_t s_g106Force2128Mac = []() -> uint32_t {
                const char *e = std::getenv("DC2_G106_FORCE_2128_MAC");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
            }();
            static const bool s_g108Qclip2128 = (std::getenv("DC2_G108_2128_QCLIP") != nullptr);
            static const bool s_g108Qclip2128Stat = (std::getenv("DC2_G108_2128_Q_STAT") != nullptr);
            float *cur = m_state.vf[16];
            const float qcur = cur[3];
            bool drawIt = false;
            // G102 stat: classify each gate vertex. cat0=front (drawn), cat1=behind clipped to
            // the near plane (drawn), cat2=behind clamped (no anchor, bounded+culled),
            // cat3=behind, no handling. DC2_G102_STAT prints every 20000.
            static const bool s_g102stat = (std::getenv("DC2_G102_STAT") != nullptr);
            static std::atomic<uint32_t> s_g102cat[4];
            int g102cat = 3;
            if (currentPc == 0x2128u && s_g106Force2128Mac != 0xFFFFFFFFu)
            {
                drawIt = ((m_state.mac & 0xFFFFu) == (s_g106Force2128Mac & 0xFFFFu));
                g102cat = drawIt ? 0 : 3;
            }
            else if (s_noQClip || (s_g104triClip && !(s_g108Qclip2128 && currentPc == 0x2128u)))
            {
                drawIt = true; // G100/G104 unconditional force draw; rasterizer clips q<=0 triangles.
                g102cat = (qcur > s_qmin) ? 0 : 3;
            }
            else if (qcur > s_qmin)
            {
                drawIt = true; // G101: vertex in front of the near plane
                g102cat = 0;
            }
            else if (s_g102clip && qcur < 0.0f)
            {
                // G102: this vertex is BEHIND the near plane, so its projected screen position is
                // exploded (q=1/W<0). Even if we cull its own kicking triangle, a tristrip reuses
                // it in the next 1-2 triangles, so the raw exploded position MUST be replaced or
                // it swims. Clip it ONTO the near plane along the edge to the nearest in-front
                // vertex (the immediate predecessor if it is in front, else the strip's most
                // recent in-front vertex) -> a finite, frame-stable position that keeps coverage.
                const float *anchor = nullptr;
                if (g_g102PrevValid && g_g102PrevScreen[3] > s_qmin)
                    anchor = g_g102PrevScreen;        // true adjacent edge (most accurate)
                else if (g_g102LastFrontValid)
                    anchor = g_g102LastFront;         // fallback: fills both-behind runs
                if (anchor)
                {
                    // screen = clip/W, so clip = screen*W with W = 1/q; W is clip-space-linear,
                    // interpolate it to W=Wnear, solve t, re-divide. cur[i]*Wc == clipC is finite
                    // (huge*tiny) so the math is stable despite the exploded screen position.
                    const float Wa = 1.0f / anchor[3];
                    const float Wc = 1.0f / qcur;        // < 0 (behind)
                    const float Wn = s_g102wnear;        // near-plane W (> 0)
                    const float denom = Wc - Wa;
                    float t = (denom != 0.0f) ? (Wn - Wa) / denom : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        const float clipA = anchor[i] * Wa;
                        const float clipC = cur[i] * Wc;
                        const float clipN = clipA + t * (clipC - clipA);
                        cur[i] = clipN / Wn;             // back to screen space
                    }
                    cur[3] = 1.0f / Wn;                  // q at the near plane (> 0)
                    drawIt = true;
                    g102cat = 1;
                }
                else
                {
                    // No in-front anchor yet (strip opens behind the camera). Cannot clip, but a
                    // bounded clamp keeps the position finite/stable (no swimming) instead of the
                    // raw 1e6 explosion; leave it culled (do not force draw).
                    cur[0] = (cur[0] < -2048.0f) ? -2048.0f : (cur[0] > 2560.0f) ? 2560.0f : cur[0];
                    cur[1] = (cur[1] < -2048.0f) ? -2048.0f : (cur[1] > 2560.0f) ? 2560.0f : cur[1];
                    g102cat = 2;
                }
                // Clamp any G102-modified screen XY into the guard band so the GS scissor (not the
                // guard-band cull) does the final on-screen clip -> the clipped triangle fills to
                // the screen edge instead of being guard-culled into a new hole.
                cur[0] = (cur[0] < -1000.0f) ? -1000.0f : (cur[0] > 1512.0f) ? 1512.0f : cur[0];
                cur[1] = (cur[1] < -1000.0f) ? -1000.0f : (cur[1] > 1432.0f) ? 1432.0f : cur[1];
                // Loop 2 (gate 0x2128) FTOI4s VF16 into the emit reg VF17 BEFORE this gate, so
                // also patch the already-computed VF17 = FTOI4(screen) = screen*16 (xy). Loops
                // that FTOI after the gate (0x1d48) re-derive VF17 from the patched VF16
                // (harmless). z/w left to the microcode.
                m_state.vf[17][0] = cur[0] * 16.0f;
                m_state.vf[17][1] = cur[1] * 16.0f;
            }
            if (s_g108Qclip2128Stat && currentPc == 0x2128u)
            {
                static std::atomic<uint64_t> s_g108Front{0};
                static std::atomic<uint64_t> s_g108Cull{0};
                static std::atomic<uint64_t> s_g108Tick{0};
                if (drawIt)
                    s_g108Front.fetch_add(1u, std::memory_order_relaxed);
                else
                    s_g108Cull.fetch_add(1u, std::memory_order_relaxed);
                const uint64_t t = s_g108Tick.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (t <= 24u || (t % 20000ull) == 0ull)
                    std::fprintf(stderr,
                                 "[G108:q2128] n=%llu q=% .6g draw=%u front=%llu cull=%llu\n",
                                 static_cast<unsigned long long>(t), qcur,
                                 static_cast<unsigned>(drawIt ? 1u : 0u),
                                 static_cast<unsigned long long>(s_g108Front.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(s_g108Cull.load(std::memory_order_relaxed)));
            }
            if (s_g102stat)
            {
                s_g102cat[g102cat].fetch_add(1u, std::memory_order_relaxed);
                static std::atomic<uint32_t> s_g102tick{0};
                if ((s_g102tick.fetch_add(1u, std::memory_order_relaxed) % 20000u) == 0u)
                    std::fprintf(stderr, "[G102:stat] front=%u clip=%u clamp=%u none=%u wnear=%.2f\n",
                                 s_g102cat[0].load(), s_g102cat[1].load(),
                                 s_g102cat[2].load(), s_g102cat[3].load(), s_g102wnear);
            }
            // Record this vertex (clipped/clamped or raw) as the predecessor for the next edge,
            // and track the most recent in-front vertex as the both-behind clip anchor.
            g_g102PrevScreen[0] = cur[0];
            g_g102PrevScreen[1] = cur[1];
            g_g102PrevScreen[2] = cur[2];
            g_g102PrevScreen[3] = cur[3];
            g_g102PrevValid = true;
            if (cur[3] > s_qmin)
            {
                g_g102LastFront[0] = cur[0];
                g_g102LastFront[1] = cur[1];
                g_g102LastFront[2] = cur[2];
                g_g102LastFront[3] = cur[3];
                g_g102LastFrontValid = true;
            }
            if (drawIt)
                m_state.vi[1] = m_state.vi[10];
        }
        // G75 route experiment: the title-batch dispatcher (0x0708..) routes to the +2048-gated
        // packers (0x1c50 tristrip etc.) because VI7=4&VI1 and VI8=1&VI1 are both nonzero (VI1 =
        // qword38 selector = 0x17). G74 proved those gated packers emit 100% ADC (never-takeable
        // gate) -> flat blue, while the no-gate packer 0x1b68 COPIES the EE input's per-vertex ADC
        // flag (ILWR VI7,(VI12) @0x1b98 -> &0x8000 -> ISWR @0x1bd8) and so would reproduce HW's
        // per-vertex strip-restart ADC. Force VI7=0 @0x0730 and VI8=0 @0x07b0 to route every
        // startPc=0x10 batch through 0x1b68 and test whether the rock then renders. Run with
        // DC2_G72_DISP to confirm p0x1b68 goes nonzero. Env DC2_G75_ROUTE1B68; diagnostic, off by default.
        {
            static const bool s_g75route = (std::getenv("DC2_G75_ROUTE1B68") != nullptr);
            if (s_g75route && startPc == 0x10u)
            {
                if (currentPc == 0x0730u) m_state.vi[7] = 0;
                else if (currentPc == 0x07b0u) m_state.vi[8] = 0;
            }
        }
        // G85: pin the directional N·L magnitude. The title rock green is the directional
        // light's genuine colour (0,80,125) (R=0) applied as ambient + colour*clamp(N·L);
        // the runner's G/B are ~2.7x HW so the N·L is too large. Dump the transformed normal
        // VF25 (+|N|^2) at pc=0x16f8 and the clamped directional dot VF26 at pc=0x1828 for the
        // first rock vertices. Env DC2_G85_NL; startPc==0x10 only; quiet by default.
        {
            static const bool s_g85nl = (std::getenv("DC2_G85_NL") != nullptr);
            if (s_g85nl && startPc == 0x10u)
            {
                static std::atomic<uint32_t> s_g85nlN{0};
                if (currentPc == 0x16f8u)
                {
                    const uint32_t k = s_g85nlN.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 24u)
                    {
                        const float *n = m_state.vf[25];
                        const float nlen2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
                        std::fprintf(stderr, "[G85:nl] k=%u N=(% .4g % .4g % .4g) |N|^2=% .4g\n",
                                     k, n[0], n[1], n[2], nlen2);
                        std::fflush(stderr);
                    }
                }
                else if (currentPc == 0x1828u)
                {
                    static std::atomic<uint32_t> s_g85vfN{0};
                    const uint32_t k = s_g85vfN.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 24u)
                    {
                        const float *c = m_state.vf[26];
                        std::fprintf(stderr, "[G85:dir] k=%u VF26(dir N.L clamped)=(% .4g % .4g % .4g % .4g)\n",
                                     k, c[0], c[1], c[2], c[3]);
                        std::fflush(stderr);
                    }
                }
            }
        }
        // G87: isolate the POINT-LIGHT contribution to the per-vertex colour. G86 proved the
        // title green is a per-vertex R-deficit (runner R<ambient 46.5) and the only R source is
        // the point light (qw49=(75,75,60)); directional light0=(0,80,125) has R=0. Capture, for
        // the first rock vertices: VF26 (ambient+directional, pc=0x18a0), the point term
        // VF24(N.L)/VF29(colour*atten)/VF31(atten) (pc=0x1970), and the FINAL stored colour VF17
        // (pc=0x19e8, the SQI). If VF26.R~46 but VF17.R<46, the point step is wrong/negative.
        // Env DC2_G87_PL; startPc==0x10 only; quiet by default.
        {
            static const bool s_g87 = (std::getenv("DC2_G87_PL") != nullptr);
            if (s_g87 && startPc == 0x10u)
            {
                if (currentPc == 0x18a0u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *c = m_state.vf[26];
                        std::fprintf(stderr, "[G87:ambdir] k=%u VF26=(% .3g % .3g % .3g % .3g)\n",
                                     k, c[0], c[1], c[2], c[3]); std::fflush(stderr); }
                }
                else if (currentPc == 0x1970u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *nl=m_state.vf[24],*ca=m_state.vf[29],*at=m_state.vf[31];
                        std::fprintf(stderr, "[G87:point] k=%u VF24nl=(% .3g % .3g % .3g % .3g) "
                                     "VF29ca=(% .3g % .3g % .3g) VF31at=(% .3g % .3g % .3g % .3g)\n",
                                     k, nl[0],nl[1],nl[2],nl[3], ca[0],ca[1],ca[2],
                                     at[0],at[1],at[2],at[3]); std::fflush(stderr); }
                }
                else if (currentPc == 0x19e8u)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
                    if (k < 16u) { const float *c = m_state.vf[17];
                        std::fprintf(stderr, "[G87:final] k=%u VF17=(% .3g % .3g % .3g % .3g)\n",
                                     k, c[0], c[1], c[2], c[3]); std::fflush(stderr); }
                }
            }
        }
        if (currentPc + 8 > codeSize)
        {
            stopReason = "pc-oob";
            break;
        }

        uint32_t lower, upper;
        std::memcpy(&lower, vuCode + currentPc, 4);
        std::memcpy(&upper, vuCode + currentPc + 4, 4);
        lastPc = currentPc;
        lastLower = lower;
        lastUpper = upper;

        // G68: ADC-source histogram. The title 3D map renders flat-blue because all its vertices
        // reach the GS with ADC=1 (bit15 of the stored vertex word). Histogram EVERY integer store
        // to VU memory (ISW opHi=0x05, ISWR opHi=0x40/s2=0x3F) by (pc -> bit15 set/clear count) for
        // the model program (startPc=0x10). The pc that writes many bit15=1 vertex words IS the
        // title's ADC source; comparing title vs costume localizes the divergence. Env DC2_G68_ADC.
        if (startPc == 0x10u)
        {
            static const bool s_g68adc = (std::getenv("DC2_G68_ADC") != nullptr);
            if (s_g68adc && vuData)
            {
                const uint32_t opHi68 = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
                // At each XGKICK (opHi 0x40, s2 0x6C), walk the kicked GIF packet in VU memory and
                // tally output-vertex ADC (bit15 of word3 of XYZF2/XYZ2) split by prim type, plus
                // dump the first few raw vertex word3 values so we can see the ACTUAL ADC source.
                if (opHi68 == 0x40u && S2_FUNC(lower) == 0x6Cu)
                {
                    const uint32_t is68 = (lower >> 11u) & 0x1Fu;
                    uint32_t off = (static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is68])) * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                    static std::atomic<uint32_t> s_kick{0}, s_dumped{0};
                    static std::atomic<uint32_t> s_adcSet{0}, s_adcClr{0};
                    const uint32_t kn = s_kick.fetch_add(1u, std::memory_order_relaxed);
                    uint32_t curPrim = 0xFFu; bool dumpThis = (s_dumped.load(std::memory_order_relaxed) < 6u);
                    uint32_t tag = 0u; uint32_t guard = 0u;
                    while (off + 16u <= dataSize && guard++ < 512u)
                    {
                        uint64_t lo64, hi64; std::memcpy(&lo64, vuData + off, 8); std::memcpy(&hi64, vuData + off + 8, 8);
                        const uint32_t nloop = static_cast<uint32_t>(lo64 & 0x7FFFu);
                        const uint32_t flg = static_cast<uint32_t>((lo64 >> 58u) & 3u);
                        uint32_t nreg = static_cast<uint32_t>((lo64 >> 60u) & 0xFu); if (!nreg) nreg = 16u;
                        const bool pre = ((lo64 >> 46u) & 1u) != 0u;
                        if (pre) curPrim = static_cast<uint32_t>((lo64 >> 47u) & 7u);
                        off += 16u;
                        if (flg != 0u) break;                 // only walk PACKED region
                        uint8_t regs[16]; for (uint32_t r = 0; r < nreg; ++r) regs[r] = (hi64 >> (r * 4u)) & 0xFu;
                        for (uint32_t lp = 0; lp < nloop && off + 16u <= dataSize; ++lp)
                            for (uint32_t r = 0; r < nreg; ++r, off += 16u)
                            {
                                if (off + 16u > dataSize) break;
                                const uint8_t rg = regs[r];
                                if (rg == 0x04u || rg == 0x05u) // XYZF2 / XYZ2
                                {
                                    uint64_t vhi; std::memcpy(&vhi, vuData + off + 8u, 8);
                                    const bool adc = ((vhi >> 47u) & 1u) != 0u;
                                    if (curPrim == 3u || curPrim == 4u || curPrim == 5u)
                                    { if (adc) s_adcSet.fetch_add(1u, std::memory_order_relaxed); else s_adcClr.fetch_add(1u, std::memory_order_relaxed); }
                                    if (dumpThis && s_dumped.load(std::memory_order_relaxed) < 6u)
                                    {
                                        const uint32_t w3 = static_cast<uint32_t>(vhi >> 32u);
                                        std::fprintf(stderr, "[G68:vtx] kick=%u prim=%u reg=0x%x adc=%u word3=0x%08x vhi=0x%016llx\n",
                                                     kn, curPrim, (unsigned)rg, (unsigned)(adc ? 1u : 0u), w3, (unsigned long long)vhi);
                                        s_dumped.fetch_add(1u, std::memory_order_relaxed);
                                    }
                                }
                            }
                    }
                    if ((kn % 2000u) == 0u)
                        std::fprintf(stderr, "[G68:adc] kick=%u outVtx ADC set=%u clr=%u\n",
                                     kn, s_adcSet.load(std::memory_order_relaxed), s_adcClr.load(std::memory_order_relaxed));
                }
            }
        }

        // G69: localize the title's per-vertex ADC SOURCE. G68 proved the title's output
        // vertices carry ADC=1 (word3 bit15) at 73% vs the costume's 26% (inverted). word3 is
        // cleanly 0x8000/0x0 (an integer flag), so the ADC word is written by an INTEGER store
        // (ISW opHi=0x05, or ISWR opHi=0x40/s2=0x3F). Histogram every such store by PC, splitting
        // bit15(value) set/clear and recording the dest-lane mask. The PC with many bit15-SET
        // stores into the W lane (dest&1 = word3) IS the ADC source. Reading that PC's microcode
        // (enable DC2_G61_VUDUMP in the same run for a consistent disasm) classifies the bug as
        // EE-data-copied (preceded by an ILWR of the input flag, like the costume) vs VU-computed
        // (preceded by a winding/cull compare). Env DC2_G69_ADC; startPc==0x10 only; quiet default.
        if (startPc == 0x10u)
        {
            static const bool s_g69adc = (std::getenv("DC2_G69_ADC") != nullptr);
            if (s_g69adc)
            {
                // G69c: one-shot dump of the ACTUALLY-EXECUTING title transform (FTOIs land at
                // 0x1d78/0x1f98/0x2158, past the G39 0x1c90 range) so we can see where the binary
                // per-vertex .w flag (0 / 2048.0) originates: an LQ from input data vs a VU clip/cull.
                static std::atomic<uint32_t> s_g69dump{0};
                if (currentPc >= 0x1c00u && currentPc <= 0x2200u &&
                    s_g69dump.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    for (uint32_t a = 0x1b00u; a <= 0x2200u && a + 8u <= codeSize; a += 8u)
                    {
                        uint32_t lo = 0u, up = 0u;
                        std::memcpy(&lo, vuCode + a, 4);
                        std::memcpy(&up, vuCode + a + 4, 4);
                        std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
                    }
                    std::fflush(stderr);
                }
                const uint8_t opHi69 = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
                const bool isIsw  = (opHi69 == 0x05u);
                const bool isIswr = (opHi69 == 0x40u && S2_FUNC(lower) == 0x3Fu);
                if (isIsw || isIswr)
                {
                    const uint8_t it69   = static_cast<uint8_t>((lower >> 16u) & 0x1Fu);
                    const uint8_t dest69 = static_cast<uint8_t>((lower >> 21u) & 0xFu);
                    const uint16_t val69 = static_cast<uint16_t>(m_state.vi[it69] & 0xFFFFu);
                    const bool b15 = (val69 & 0x8000u) != 0u;
                    static std::atomic<uint32_t> s_setC[2048];
                    static std::atomic<uint32_t> s_clrC[2048];
                    static std::atomic<uint32_t> s_destSeen[2048];
                    const uint32_t idx = (currentPc >> 3u) & 2047u;
                    if (b15) s_setC[idx].fetch_add(1u, std::memory_order_relaxed);
                    else     s_clrC[idx].fetch_add(1u, std::memory_order_relaxed);
                    s_destSeen[idx].fetch_or(dest69, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_tick69{0};
                    if ((s_tick69.fetch_add(1u, std::memory_order_relaxed) % 300000u) == 0u)
                    {
                        std::fprintf(stderr, "[G69:storehist] --- integer-store bit15 histogram by pc ---\n");
                        for (uint32_t i = 0; i < 2048u; ++i)
                        {
                            const uint32_t s = s_setC[i].load(std::memory_order_relaxed);
                            const uint32_t c = s_clrC[i].load(std::memory_order_relaxed);
                            if (s + c >= 2000u)
                                std::fprintf(stderr, "[G69:storehist] pc=0x%x dest=0x%x set=%u clr=%u\n",
                                             i << 3u, s_destSeen[i].load(std::memory_order_relaxed), s, c);
                        }
                    }
                }

                // G69b: the title ADC word3 = FTOI4.w of the projected position's .w lane (the
                // fog/clamp term), packed into the XYZF2 word3. The producing pc varies (the title
                // re-uploads VU microcode per map-part), so detect ANY upper FTOI0/4/12/15 that
                // computes the .w lane (dest&1) and tally bit15 of its integer result by pc, plus
                // sample the inputs (the source .w float, the scale, and VF22 clamp consts). This
                // localizes WHY VF18.w saturates to 0x8000 (=ADC=1) for ~73% of title vertices.
                {
                    const uint8_t upOp = static_cast<uint8_t>(upper & 0x3Fu);
                    if (upOp >= 0x3Cu)
                    {
                        const uint8_t vf = S2_FUNC(upper);
                        if (vf >= 0x14u && vf <= 0x17u) // FTOI0/4/12/15
                        {
                            const uint8_t dst = static_cast<uint8_t>((upper >> 21u) & 0xFu);
                            if (dst & 0x1u) // .w lane (= XYZF2 word3 = ADC)
                            {
                                const uint8_t fs = static_cast<uint8_t>((upper >> 11u) & 0x1Fu);
                                const float scale = (vf == 0x14u) ? 1.0f : (vf == 0x15u) ? 16.0f :
                                                    (vf == 0x16u) ? 4096.0f : 32768.0f;
                                const float win = m_state.vf[fs][3];
                                const int32_t wres = (int32_t)(win * scale);
                                const bool b15 = ((wres >> 15) & 1) != 0;
                                static std::atomic<uint32_t> s_fSet[2048];
                                static std::atomic<uint32_t> s_fClr[2048];
                                const uint32_t fidx = (currentPc >> 3u) & 2047u;
                                if (b15) s_fSet[fidx].fetch_add(1u, std::memory_order_relaxed);
                                else     s_fClr[fidx].fetch_add(1u, std::memory_order_relaxed);
                                static std::atomic<uint32_t> s_fs{0};
                                const uint32_t fi = s_fs.fetch_add(1u, std::memory_order_relaxed);
                                if (fi < 30u)
                                {
                                    const float *vfs = m_state.vf[fs];
                                    const float *v29 = m_state.vf[29];
                                    float q59[4] = {0,0,0,0};
                                    if (59u * 16u + 16u <= dataSize)
                                        std::memcpy(q59, vuData + 59u * 16u, 16);
                                    std::fprintf(stderr,
                                        "[G69:ftoi] n=%u pc=0x%x ftoi=0x%x src=VF%u srcW=% .6g scale=%g -> wres=0x%x adc=%u | VFsrc=% .5g % .5g % .5g % .5g VF29=% .5g % .5g % .5g % .5g qw59=% .5g % .5g % .5g % .5g\n",
                                        fi, currentPc, vf, fs, win, scale, (unsigned)wres, (unsigned)(b15 ? 1u : 0u),
                                        vfs[0], vfs[1], vfs[2], vfs[3], v29[0], v29[1], v29[2], v29[3],
                                        q59[0], q59[1], q59[2], q59[3]);
                                }
                                static std::atomic<uint32_t> s_ftick{0};
                                if ((s_ftick.fetch_add(1u, std::memory_order_relaxed) % 300000u) == 0u)
                                {
                                    std::fprintf(stderr, "[G69:ftoihist] --- FTOI .w-lane bit15 by pc ---\n");
                                    for (uint32_t i = 0; i < 2048u; ++i)
                                    {
                                        const uint32_t s = s_fSet[i].load(std::memory_order_relaxed);
                                        const uint32_t c = s_fClr[i].load(std::memory_order_relaxed);
                                        if (s + c >= 2000u)
                                            std::fprintf(stderr, "[G69:ftoihist] pc=0x%x set=%u clr=%u\n", i << 3u, s, c);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // G117: dedicated capture of the TRISTRIP packer emit (FTOI4 VF17,VF26 @ pc=0x2158, the
        // 0x2070 rock-tristrip loop). The shared G69 sampler (s_fs<30) fills with TRI-packer
        // (0x1f80..) samples and never reaches the tristrip emit. Here we sample 0x2158 directly to
        // answer the decisive G117 question: is the tristrip ADC uniform because VF26.w is PINNED?
        // Log the source VF26 (esp .w pre-FTOI4), the input vertex VF16 (esp .w), VF29 (qw59 cull
        // consts), and the resulting ADC bit. If VF16.w / VF26.w are constant across vertices, the
        // selectivity HW shows must come from a per-vertex input (.w) or a non-zero VF29 the runner
        // zeroes. Env DC2_G117_TS; startPc==0x10. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g117ts = (std::getenv("DC2_G117_TS") != nullptr);
            if (s_g117ts && currentPc == 0x2158u)
            {
                const float *v26 = m_state.vf[26];
                const float *v16 = m_state.vf[16];
                const float *v29 = m_state.vf[29];
                const int32_t wres = (int32_t)(v26[3] * 16.0f);
                const uint32_t adc = ((wres >> 15) & 1) != 0 ? 1u : 0u;
                // running min/max of the .w lanes to see if they vary across the strip
                static thread_local float s_v26wMin = 1e30f, s_v26wMax = -1e30f;
                static thread_local float s_v16wMin = 1e30f, s_v16wMax = -1e30f;
                if (v26[3] < s_v26wMin) s_v26wMin = v26[3];
                if (v26[3] > s_v26wMax) s_v26wMax = v26[3];
                if (v16[3] < s_v16wMin) s_v16wMin = v16[3];
                if (v16[3] > s_v16wMax) s_v16wMax = v16[3];
                static std::atomic<uint32_t> s_n117{0};
                const uint32_t nn = s_n117.fetch_add(1u, std::memory_order_relaxed);
                if (nn < 64u)
                    std::fprintf(stderr,
                        "[G117:ts] n=%u VF26=(% .4g % .4g % .4g % .4g) VF16w=% .4g adc=%u | VF29=(% .4g % .4g % .4g % .4g)\n",
                        nn, v26[0], v26[1], v26[2], v26[3], v16[3], adc,
                        v29[0], v29[1], v29[2], v29[3]);
                if ((nn % 50000u) == 0u && nn > 0u)
                    std::fprintf(stderr,
                        "[G117:tsrange] n=%u VF26.w=[% .4g,% .4g] VF16.w=[% .4g,% .4g] VF29=(% .4g % .4g % .4g % .4g)\n",
                        nn, s_v26wMin, s_v26wMax, s_v16wMin, s_v16wMax,
                        v29[0], v29[1], v29[2], v29[3]);
            }
        }

        // G132: the COPY packer (0x1b68) per-vertex ADC source. Decoded from the copy loop
        // (0x1be0-0x1c28): VF26.xyz = VF16.xyz (screen pos, MOVE @0x1c10); VF26.w =
        // max(min(VF29.x + VF29.w*VF16.w, VF29.y), VF29.z) (the fog/depth clamp, pipelined across
        // 0x1be8/0x1bf0/0x1c10); VF17 = FTOI4(VF26) @0x1c08; the XYZF2 store @0x1c28 packs VF17,
        // ADC = bit15 of VF17.w (= int(VF26.w*16) >= 0x8000 <=> VF26.w >= 2048). There is NO
        // unconditional +2048 here (unlike the 0x1ff0 transform tristrip), so with VF29(qword59)=0
        // the clamp collapses to 0 -> ADC=0 on EVERY copy vertex (= the G118 tally 648996/0
        // all-draw, the spanning-connector-tris root G131). Sample at the store (0x1c28): the
        // stored VF17 (int) gives the ACTUAL ADC; VF16.w is the EE-delivered per-vertex fog/.w the
        // copy is meant to pass through; VF29 is the clamp const. If VF16.w VARIES but VF29=0, the
        // fix is the qword59 upload (restore the passthrough clamp so VF16.w's ADC reaches the GS);
        // if VF16.w is also uniform/zero, the EE never wrote the per-vertex ADC into the copy input.
        // Env DC2_G132_COPYW; startPc==0x10. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g132 = (std::getenv("DC2_G132_COPYW") != nullptr);
            if (s_g132 && currentPc == 0x1c28u)
            {
                const float *v26 = m_state.vf[26];
                const float *v16 = m_state.vf[16];
                const float *v29 = m_state.vf[29];
                int32_t vi17w; std::memcpy(&vi17w, &m_state.vf[17][3], 4); // FTOI4 result (int)
                const uint32_t adc = ((vi17w >> 15) & 1) != 0 ? 1u : 0u;
                static thread_local float s_v16wMin = 1e30f, s_v16wMax = -1e30f;
                static thread_local float s_v26wMin = 1e30f, s_v26wMax = -1e30f;
                if (v16[3] < s_v16wMin) s_v16wMin = v16[3];
                if (v16[3] > s_v16wMax) s_v16wMax = v16[3];
                if (v26[3] < s_v26wMin) s_v26wMin = v26[3];
                if (v26[3] > s_v26wMax) s_v26wMax = v26[3];
                static std::atomic<uint32_t> s_n132{0};
                const uint32_t nn = s_n132.fetch_add(1u, std::memory_order_relaxed);
                if (nn < 48u)
                    std::fprintf(stderr,
                        "[G132:copyw] n=%u VF16=(% .4g % .4g % .4g % .6g) VF26.w=% .6g VF17.w(int)=%d adc=%u | VF29=(% .6g % .6g % .6g % .6g)\n",
                        nn, v16[0], v16[1], v16[2], v16[3], v26[3], vi17w, adc,
                        v29[0], v29[1], v29[2], v29[3]);
                if ((nn % 50000u) == 0u && nn > 0u)
                    std::fprintf(stderr,
                        "[G132:range] n=%u VF16.w=[% .6g,% .6g] VF26.w=[% .6g,% .6g] VF29=(% .6g % .6g % .6g % .6g)\n",
                        nn, s_v16wMin, s_v16wMax, s_v26wMin, s_v26wMax,
                        v29[0], v29[1], v29[2], v29[3]);
            }
        }

        // G70: confirm VU1 data qword 59 (VF29 = per-vertex W/ADC cull consts) is zero on the
        // title and dump the const region. The title flat-blue mechanism (decoded from the
        // executing transform 0x1e78-0x1fc0): sp06 = SUBAz.w ACC,VF0,VF29 -> ACC.w = 1 - VF29.z;
        // MADDw.w VF18 = ACC.w + VF29.w*W_raw; MINIy.w/MAXz.w clamp to [VF29.z, VF29.y]; then
        // ADDi.w VF18,VF18,I(=2048) on the KICKING vertex only; FTOI4 ->word3 ADC. With qword 59
        // = 0: ACC.w=1, clamp->0, +2048 -> 0x8000 -> ADC=1 on every kicking vertex => nothing
        // draws. Compare qw 21/22 (perspective divisors, must be non-zero) to prove a SELECTIVE
        // upload gap. Env DC2_G70; startPc==0x10; one-shot. Quiet by default.
        if (startPc == 0x10u)
        {
            static const bool s_g70 = (std::getenv("DC2_G70") != nullptr);
            if (s_g70)
            {
                static std::atomic<uint32_t> s_g70dump{0};
                if (currentPc >= 0x1e00u && currentPc <= 0x2000u &&
                    s_g70dump.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    static const uint32_t qws[] = {4u, 5u, 6u, 7u, 21u, 22u, 30u, 57u, 58u, 59u, 60u, 61u};
                    for (uint32_t i = 0; i < sizeof(qws) / sizeof(qws[0]); ++i)
                    {
                        const uint32_t q = qws[i];
                        float f[4] = {0, 0, 0, 0};
                        if (q * 16u + 16u <= dataSize)
                            std::memcpy(f, vuData + q * 16u, 16);
                        std::fprintf(stderr, "[G70:qw] qw%u = % .6g % .6g % .6g % .6g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                    std::fflush(stderr);
                }
            }
        }

        if (vu1_trace_enabled())
        {
            const uint8_t opHi = static_cast<uint8_t>((lower >> 25u) & 0x7Fu);
            if (opHi == 0x40u && S2_FUNC(lower) == 0x6Cu)
            {
                const uint32_t n = s_g22Vu1XgkickOpCount.fetch_add(1u, std::memory_order_relaxed);
                if (n < 96u)
                {
                    const uint32_t is = (lower >> 11u) & 0x1Fu;
                    const uint32_t vi = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is]));
                    std::fprintf(stderr,
                                 "[G22:vu1op] n=%u pc=0x%x cycle=%u lower=0x%08x upper=0x%08x is=VI%u vi=0x%x addr=0x%x itop=0x%x\n",
                                 n, currentPc, cycle, lower, upper, is, vi, vi * 16u, m_state.itop);
                }
            }
        }

        // G32: log the UPPER op paired with each lower in the model pack window so we can
        // see which op produces VF17 (XYZF2 position) / VF21 (RGBAQ) and from what source.
        if (g32_vu_trace_enabled() && currentPc >= 0x1b00u && currentPc <= 0x1c80u)
        {
            const uint32_t n = s_g32Count.load(std::memory_order_relaxed);
            if (false)
            {
                const uint8_t uOp = (uint8_t)((upper >> 25) & 0x3Fu); // bits 30:25 base op
                const uint8_t ud  = (uint8_t)((upper >> 6) & 0x1Fu);
                const uint8_t us  = (uint8_t)((upper >> 11) & 0x1Fu);
                const uint8_t ut  = (uint8_t)((upper >> 16) & 0x1Fu);
                const uint8_t udst = (uint8_t)((upper >> 21) & 0xFu);
                const uint8_t us2 = S2_FUNC(upper);
                std::fprintf(stderr,
                             "[G32:up] pc=0x%x upper=0x%08x op=0x%02x s2=0x%02x dst=0x%x vd=VF%u vs=VF%u vt=VF%u | VF17=(%.5g %.5g %.5g %.5g)\n",
                             currentPc, upper, (uint32_t)uOp, (uint32_t)us2, (uint32_t)udst,
                             (uint32_t)ud, (uint32_t)us, (uint32_t)ut,
                             m_state.vf[17][0], m_state.vf[17][1], m_state.vf[17][2], m_state.vf[17][3]);
            }
        }

        bool eBit = (upper >> 30) & 1;

        // G27: LOI is signaled by the I-bit (bit 31 of the UPPER word). When set,
        // the LOWER 32 bits are a 32-bit float immediate loaded into the I register
        // and must NOT be decoded as a lower instruction; the upper still executes.
        // The old code keyed LOI on a magic lower value (0x8000033C) that DC2's VU
        // programs never emit, so real LOI immediates (e.g. 0x457ff000 ~= 4093.0f)
        // were decoded as bogus lower ops (0x22/0x23), corrupting the model transform
        // and derailing the microprogram (runaway -> maxcycles, garbage GIF output).
        bool iBit = (upper >> 31) & 1;

        // G87: advance the Q pipeline one cycle BEFORE this word executes. A pending DIV/SQRT/
        // RSQRT result becomes visible in m_state.q when its delay expires; a producer in THIS
        // word (re)arms the delay afterwards (in execLower) so it counts from the next word.
        if (*s_qDelayP > 0 && --(*s_qDelayP) == 0)
            m_state.q = s_vuQPending;

        // G138: advance the MAC/STATUS flag pipeline one instruction pair. Flag-consuming
        // lower ops executed THIS pair (FMEQ/FMAND/FMOR/FSAND/FSEQ/FSOR) read the snapshot
        // from DEPTH pairs ago via s_vuMacVisible/s_vuStatusVisible. (G141: flag + TLS base
        // hoisted to loop-invariant locals; identical semantics.)
        if (s_macpipeOn)
        {
            const int g138d = s_macpipeDepth;
            *s_macVisP = s_macPipeP[g138d - 1];
            *s_statusVisP = s_statusPipeP[g138d - 1];
            for (int g138i = g138d - 1; g138i > 0; --g138i)
            {
                s_macPipeP[g138i] = s_macPipeP[g138i - 1];
                s_statusPipeP[g138i] = s_statusPipeP[g138i - 1];
            }
        }

        if (iBit)
        {
            std::memcpy(&m_state.i, &lower, 4);
            execUpper(upper);
        }
        else
        {
            const uint32_t g106MacBeforeUpper = m_state.mac;
            const uint32_t g106StatusBeforeUpper = m_state.status;
            // G139: locate the upper op's VF destination (fd for main-space ops; ft for
            // ITOF/FTOI/ABS in the special group; none for ACC/CLIP writers) so the lower
            // op of this pair can read the pre-upper value (see g139_pairhaz_enabled).
            float *g139dst = nullptr;
            float g139old[4], g139new[4];
            if (s_pairhazOn)
            {
                const uint8_t g139op = (uint8_t)(upper & 0x3Fu);
                if (g139op < 0x3Cu)
                {
                    g139dst = m_state.vf[(upper >> 6) & 0x1Fu];
                }
                else
                {
                    const uint8_t g139s2 = S2_FUNC(upper);
                    if ((g139s2 >= 0x10u && g139s2 <= 0x17u) || g139s2 == 0x1Du)
                        g139dst = m_state.vf[(upper >> 16) & 0x1Fu];
                }
                if (g139dst)
                    std::memcpy(g139old, g139dst, 16);
            }
            execUpper(upper);
            if (g139dst)
            {
                std::memcpy(g139new, g139dst, 16);
                std::memcpy(g139dst, g139old, 16);
            }
            if (s_g106ParallelFlags)
            {
                s_g106LowerMacRead = g106MacBeforeUpper;
                s_g106LowerStatusRead = g106StatusBeforeUpper;
                s_g106UseLowerFlagRead = true;
            }
            // G198: refuted G197's IALU-latency hypothesis by reading PCSX2's real _vuIALUflush/
            // _vuTestALUStalls (VUops.cpp) -- IALU-pipe bookkeeping only advances VU->cycle (a
            // timing counter used elsewhere for XGKICK pacing), it never delays or forwards a
            // stale VALUE: _vuILW/_vuIADD* write VU->VI[] eagerly every time regardless of the
            // ialu[] pipe state, so modeling that latency in DC2 (an interpreter with no true
            // out-of-order execution) would be a no-op for this bug. Also confirmed the PCSX2
            // DebugServer cannot do a live VU1-internal register A/B (g135's tool note: EE RAM/
            // scratchpad only). Redirect: trace the shared FMAND/IAND/IBEQ chain's actual inputs
            // (VI1/VI7/VI8/VI9/VI10 at pc=0x2128, the same gate G138 already fixed for the title)
            // for the map-piece startPC=0x10 program, so a title-vs-map comparison can be done
            // from log data instead. Default-off (DC2_G198_GATE_TRACE), capped at 4000 hits.
            static const bool s_g198GateOn = (std::getenv("DC2_G198_GATE_TRACE") != nullptr);
            if (s_g198GateOn && startPc == 0x10u && currentPc == 0x2128u)
            {
                static std::atomic<uint32_t> s_g198Hits{0};
                const uint32_t h = s_g198Hits.fetch_add(1u, std::memory_order_relaxed);
                if (h < 4000u)
                {
                    std::fprintf(stderr,
                        "[G198:gate] hit=%u vi1=0x%x vi7=0x%x vi8=0x%x vi9=0x%x vi10=0x%x taken=%d\n",
                        h, (unsigned)(uint16_t)m_state.vi[1], (unsigned)(uint16_t)m_state.vi[7],
                        (unsigned)(uint16_t)m_state.vi[8], (unsigned)(uint16_t)m_state.vi[9],
                        (unsigned)(uint16_t)m_state.vi[10],
                        (int)((int16_t)m_state.vi[1] == (int16_t)m_state.vi[10]));
                }
            }
            // G200: qw30's REAL producer is this same program's setup block (statically confirmed
            // from the G197 full dump: 0x170 OPMULA/OPMSUB cross of VF15xVF16, 0x180 MUL by VF17,
            // 0x198/0x1a0 ADD.x sum -> det in VF24.x, 0x1c0 FMAND VI9,0x80 on the det's S.x flag,
            // 0x1e0/0x200 VI7=0x0/0x20, 0x210 ISW.x VI7,30(VI0)) -- NOT an EE-side upload (G199's
            // info+0x270 refutation + this close G198's "EE-side producer" framing). Trace the det
            // inputs/outputs at the ISW so a host-side recompute can check sign consistency, and a
            // matrix-level A/B vs PCSX2 EE RAM becomes possible. Default-off (DC2_G200_DET_TRACE).
            // NOTE: startPC=0x10 enters at byte 0x10 -> JR to 0x320, SKIPPING the setup block;
            // the det/qw30 setup only runs under a different start (0x0 path). No startPc filter.
            static const bool s_g200DetOn = (std::getenv("DC2_G200_DET_TRACE") != nullptr);
            if (s_g200DetOn && currentPc == 0x210u)
            {
                static std::atomic<uint32_t> s_g200Hits{0};
                const uint32_t h = s_g200Hits.fetch_add(1u, std::memory_order_relaxed);
                if (h < 400u)
                {
                    uint32_t r15[4], r16[4], r17[4], d24;
                    std::memcpy(r15, m_state.vf[15], 16);
                    std::memcpy(r16, m_state.vf[16], 16);
                    std::memcpy(r17, m_state.vf[17], 16);
                    std::memcpy(&d24, &m_state.vf[24][0], 4);
                    std::fprintf(stderr,
                        "[G200:det] hit=%u start=0x%x vi7=0x%x det=%.9g d24=0x%08x"
                        " vf15=%08x,%08x,%08x,%08x vf16=%08x,%08x,%08x,%08x vf17=%08x,%08x,%08x,%08x\n",
                        h, startPc, (unsigned)(uint16_t)m_state.vi[7], m_state.vf[24][0], d24,
                        r15[0], r15[1], r15[2], r15[3],
                        r16[0], r16[1], r16[2], r16[3],
                        r17[0], r17[1], r17[2], r17[3]);
                }
            }
            execLower(lower, vuData, dataSize, gs, memory, upper);
            s_g106UseLowerFlagRead = false;
            if (g139dst)
            {
                // The upper result lands after the lower's same-cycle access: overlay the
                // upper's dest-masked lanes over whatever the lower left in the register.
                const uint8_t g139m = (uint8_t)((upper >> 21) & 0xFu);
                if (g139m & 0x8u) g139dst[0] = g139new[0];
                if (g139m & 0x4u) g139dst[1] = g139new[1];
                if (g139m & 0x2u) g139dst[2] = g139new[2];
                if (g139m & 0x1u) g139dst[3] = g139new[3];
            }
        }
        // G138: the newest pipeline entry is this pair's post-upper architectural flags.
        if (s_macpipeOn)
        {
            s_macPipeP[0] = m_state.mac;
            s_statusPipeP[0] = m_state.status;
        }
        const uint32_t postExecPc = m_state.pc;

        // G72: trace every change to VI1 in the title/model program (startPc=0x10). The
        // tristrip/trifan packer cull gate IBEQ VI10,VI1 needs VI1==208 to draw but VI1 reads 0.
        // Log pc + old/new VI1 to find VI1's source (input header vs computed). Env DC2_G72_VI1.
        {
            static const bool s_g72vi1 = (std::getenv("DC2_G72_VI1") != nullptr);
            if (s_g72vi1 && startPc == 0x10u && (int16_t)g72_vi1_before != (int16_t)m_state.vi[1])
            {
                static std::atomic<uint32_t> s_n{0};
                if (s_n.fetch_add(1u, std::memory_order_relaxed) < 80u)
                    std::fprintf(stderr, "[G72:vi1] pc=0x%x VI1 %d -> %d (lo=0x%08x)\n",
                                 currentPc, (int)(int16_t)g72_vi1_before,
                                 (int)(int16_t)m_state.vi[1], lower);
            }
        }

        // G71 probe: ground-truth the title-map cull gate. Logs MAC + the integer regs the
        // FMEQ/IBEQ chain reads at the decision PCs, and whether each branch was taken
        // (postExecPc != currentPc for a branch op). Env DC2_G71_PROBE; startPc==0x10 only.
        {
            static const bool s_g71probe = (std::getenv("DC2_G71_PROBE") != nullptr);
            if (s_g71probe && startPc == 0x10u)
            {
                static std::atomic<uint32_t> s_g71n{0};
                const bool atGate =
                    currentPc == 0x1d20u || currentPc == 0x1d28u || currentPc == 0x1d48u ||
                    currentPc == 0x1f00u || currentPc == 0x1f08u || currentPc == 0x1f50u ||
                    currentPc == 0x1f68u || currentPc == 0x2108u || currentPc == 0x2128u;
                if (atGate && s_g71n.fetch_add(1u, std::memory_order_relaxed) < 120u)
                {
                    const bool taken = (postExecPc != currentPc);
                    std::fprintf(stderr,
                        "[G71:gate] pc=0x%x mac=0x%04x VI1=%d VI7=%d VI8=%d VI9=%d VI10=%d VI11=%d taken=%d\n",
                        currentPc, (unsigned)(m_state.mac & 0xFFFFu),
                        (int)(int16_t)m_state.vi[1], (int)(int16_t)m_state.vi[7],
                        (int)(int16_t)m_state.vi[8], (int)(int16_t)m_state.vi[9],
                        (int)(int16_t)m_state.vi[10], (int)(int16_t)m_state.vi[11], (int)taken);
                }
            }
        }

        // G72 probe: the title VU program (startPc=0x10) has a per-batch DISPATCHER at
        // 0x0708..0x0810 that BALs to ONE of four GIF-pack subroutines based on the batch
        // header selectors VI1/VI7/VI8 (loaded by ILW VF1,1(VI12) etc):
        //   0x0730 IBEQ VI0,VI7 ->0x7b0 (VI7==0 path: -> 0x1c50 / 0x1b68)
        //   0x0760 IBEQ VI1,VI10(=4) ->0x790 (VI1==4 -> 0x1ff0; else -> 0x1dc0 = block2 +2048)
        //   0x07b0 IBEQ VI0,VI8 ->0x7e0 (VI8==0 -> 0x1b68; else -> 0x1c50)
        // G71 found the runner runs ONLY block2 (0x1dc0). This probe logs the selector values +
        // branch-taken at the decision PCs and COUNTS how often each of the 4 packers is entered,
        // so we can tell whether the EE feeds only block2-type batches (G66 EE-side delivery) or
        // the dispatch mis-routes. Env DC2_G72_DISP; startPc==0x10 only; quiet default.
        {
            static const bool s_g72disp = (std::getenv("DC2_G72_DISP") != nullptr);
            if (s_g72disp && startPc == 0x10u)
            {
                static std::atomic<uint64_t> s_p1b68{0}, s_p1c50{0}, s_p1dc0{0}, s_p1ff0{0};
                static std::atomic<uint32_t> s_g72n{0}, s_g72rep{0};
                if (currentPc == 0x1b68u) s_p1b68.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1c50u) s_p1c50.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1dc0u) s_p1dc0.fetch_add(1u, std::memory_order_relaxed);
                else if (currentPc == 0x1ff0u) s_p1ff0.fetch_add(1u, std::memory_order_relaxed);
                const bool atDisp =
                    currentPc == 0x0730u || currentPc == 0x0760u || currentPc == 0x07b0u;
                if (atDisp && s_g72n.fetch_add(1u, std::memory_order_relaxed) < 200u)
                {
                    const bool taken = (postExecPc != currentPc);
                    std::fprintf(stderr,
                        "[G72:disp] pc=0x%x VI1=%d VI7=%d VI8=%d VI10=%d VI12=%d taken=%d\n",
                        currentPc,
                        (int)(int16_t)m_state.vi[1], (int)(int16_t)m_state.vi[7],
                        (int)(int16_t)m_state.vi[8], (int)(int16_t)m_state.vi[10],
                        (int)(int16_t)m_state.vi[12], (int)taken);
                }
                // Per-vertex cull gates inside the tristrip (0x1d48) + trifan (0x2128) packers:
                // IBEQ VI10,VI1 -> skip the +2048 ADC (taken = DRAW; fall-through = +2048 = CULL).
                // Tally taken vs not for each, plus a few sample operand values.
                static std::atomic<uint64_t> s_tsDraw{0}, s_tsCull{0}, s_tfDraw{0}, s_tfCull{0};
                static std::atomic<uint32_t> s_gateSamp{0};
                if (currentPc == 0x1d48u || currentPc == 0x2128u)
                {
                    const bool taken = (postExecPc != currentPc); // taken = draw
                    if (currentPc == 0x1d48u) { if (taken) s_tsDraw.fetch_add(1u); else s_tsCull.fetch_add(1u); }
                    else { if (taken) s_tfDraw.fetch_add(1u); else s_tfCull.fetch_add(1u); }
                    if (s_gateSamp.fetch_add(1u, std::memory_order_relaxed) < 40u)
                        std::fprintf(stderr,
                            "[G72:gate] pc=0x%x VI10=%d VI1=%d VI3=%d mac=0x%04x taken(draw)=%d\n",
                            currentPc, (int)(int16_t)m_state.vi[10], (int)(int16_t)m_state.vi[1],
                            (int)(int16_t)m_state.vi[3], (unsigned)(m_state.mac & 0xFFFFu), (int)taken);
                    if (((s_tsDraw.load()+s_tsCull.load()+s_tfDraw.load()+s_tfCull.load()) % 20000u) == 0u)
                        std::fprintf(stderr,
                            "[G72:gatetally] tstrip draw=%llu cull=%llu | trifan draw=%llu cull=%llu\n",
                            (unsigned long long)s_tsDraw.load(), (unsigned long long)s_tsCull.load(),
                            (unsigned long long)s_tfDraw.load(), (unsigned long long)s_tfCull.load());
                }
                // periodic packer-selection tally
                if (currentPc == 0x0810u &&
                    (s_g72rep.fetch_add(1u, std::memory_order_relaxed) % 256u) == 0u)
                {
                    std::fprintf(stderr,
                        "[G72:packers] tristrip0x1c50=%llu trifan0x1ff0=%llu tri0x1dc0(+2048)=%llu p0x1b68=%llu\n",
                        (unsigned long long)s_p1c50.load(), (unsigned long long)s_p1ff0.load(),
                        (unsigned long long)s_p1dc0.load(), (unsigned long long)s_p1b68.load());
                }
            }
        }

        // G137: matched-packet dispatcher provenance. G136 proved the final copy XGKICK is all-draw;
        // this logs the live route decision before the copy packer overwrites VI1/VI7/VI8, and ties
        // the selector/header qwords back to the VIF UNPACK origin map. Default-off and only active
        // while G136 has armed a matched VIF stream.
        {
            static std::atomic<uint32_t> s_g137DispLogged{0};
            const uint32_t seq = g_dc2G136TraceActive.load(std::memory_order_relaxed);
            const bool atDisp = currentPc == 0x0730u || currentPc == 0x0760u || currentPc == 0x07b0u;
            const bool atPacker = currentPc == 0x1b68u || currentPc == 0x1c50u ||
                                  currentPc == 0x1dc0u || currentPc == 0x1ff0u;
            if (seq != 0u && g137_trace_enabled() && startPc == 0x10u &&
                (atDisp || atPacker))
            {
                const uint32_t n = s_g137DispLogged.fetch_add(1u, std::memory_order_relaxed);
                if (n < g137_disp_limit())
                {
                    uint32_t q38[4] = {};
                    uint32_t h0[4] = {};
                    uint32_t h1[4] = {};
                    const uint32_t vi12 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[12])) & 0x3FFu;
                    if (vuData && 0x26u * 16u + 16u <= dataSize)
                        std::memcpy(q38, vuData + 0x26u * 16u, sizeof(q38));
                    if (vuData && vi12 * 16u + 16u <= dataSize)
                        std::memcpy(h0, vuData + vi12 * 16u, sizeof(h0));
                    const uint32_t vi12Next = (vi12 + 1u) & 0x3FFu;
                    if (vuData && vi12Next * 16u + 16u <= dataSize)
                        std::memcpy(h1, vuData + vi12Next * 16u, sizeof(h1));

                    const Dc2G137VuOrigin &o38 = g_dc2G137VuOrigins[0x26u];
                    const Dc2G137VuOrigin &oh0 = g_dc2G137VuOrigins[vi12];
                    const Dc2G137VuOrigin &oh1 = g_dc2G137VuOrigins[vi12Next];
                    std::fprintf(stderr,
                                 "[G137:disp] seq=%u n=%u pc=0x%x post=0x%x kind=%s taken=%u vi1=0x%x vi7=0x%x vi8=0x%x vi10=0x%x vi12=0x%x q38=%08x %08x %08x %08x q38orig(v=%u cmd=0x%x src=0x%x raw=%08x %08x %08x %08x) h0=%08x %08x %08x %08x h0orig(v=%u cmd=0x%x src=0x%x) h1=%08x %08x %08x %08x h1orig(v=%u cmd=0x%x src=0x%x)\n",
                                 seq, n, currentPc, postExecPc, atPacker ? "packer" : "branch",
                                 (postExecPc != currentPc) ? 1u : 0u,
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[1])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[8])),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[10])),
                                 vi12,
                                 q38[0], q38[1], q38[2], q38[3],
                                 o38.valid, o38.cmdOff, o38.srcOff,
                                 o38.raw[0], o38.raw[1], o38.raw[2], o38.raw[3],
                                 h0[0], h0[1], h0[2], h0[3],
                                 oh0.valid, oh0.cmdOff, oh0.srcOff,
                                 h1[0], h1[1], h1[2], h1[3],
                                 oh1.valid, oh1.cmdOff, oh1.srcOff);
                }
            }
        }

        // G140: CLIP-route census (env DC2_G140_CLIP). The REAL trifan source is the
        // selector-bit1 (fc4) CLIP dispatcher at 0x600: bit1 batches go to the CLIP
        // packers 0x21b0 (tri) / 0x23e8 (tstrip) -> polygon clipper 0x2740 -> clipped
        // polys emitted as TRIFANS from the template at VU 780 (XGKICK 0x2d88/0x2f38).
        // HW's 1012 wall trifan verts come from here; the runner emits none. Count the
        // route entries + the FCAND/FCOR gates + the emitted fan nloop to find which
        // stage dies. Quiet unless enabled; startPc==0x10 only.
        {
            static const bool s_g140clip = (std::getenv("DC2_G140_CLIP") != nullptr);
            if (s_g140clip && startPc == 0x10u)
            {
                // one-shot pc trace: record 800 executed pcs starting at a clipper entry
                static std::atomic<int> s_trcState{0}; // 0=armed-wait 1=recording 2=done
                static uint16_t s_trc[800];
                static uint16_t s_trcVi11[800];
                static int s_trcN = 0;
                static std::atomic<uint32_t> s_trcSkip{0};
                int st = s_trcState.load(std::memory_order_relaxed);
                if (st == 0 && currentPc == 0x2740u &&
                    s_trcSkip.fetch_add(1u, std::memory_order_relaxed) == 0u)
                    s_trcState.store(1, std::memory_order_relaxed), st = 1;
                if (st == 1)
                {
                    s_trc[s_trcN] = (uint16_t)currentPc;
                    s_trcVi11[s_trcN] = (uint16_t)m_state.vi[11];
                    ++s_trcN;
                    if (s_trcN >= 800)
                    {
                        s_trcState.store(2, std::memory_order_relaxed);
                        for (int i = 0; i < 800; i += 8)
                            std::fprintf(stderr, "[G140:trc] %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d %04x/%d\n",
                                         s_trc[i], (int16_t)s_trcVi11[i], s_trc[i+1], (int16_t)s_trcVi11[i+1],
                                         s_trc[i+2], (int16_t)s_trcVi11[i+2], s_trc[i+3], (int16_t)s_trcVi11[i+3],
                                         s_trc[i+4], (int16_t)s_trcVi11[i+4], s_trc[i+5], (int16_t)s_trcVi11[i+5],
                                         s_trc[i+6], (int16_t)s_trcVi11[i+6], s_trc[i+7], (int16_t)s_trcVi11[i+7]);
                    }
                }
                static std::atomic<uint64_t> s_e21b0{0}, s_e23e8{0}, s_e2740{0}, s_k2d88{0}, s_k2f38{0};
                static std::atomic<uint64_t> s_fcandHit{0}, s_fcandMiss{0}, s_fcorRej{0}, s_fanVerts{0};
                static std::atomic<uint32_t> s_rep{0}, s_samp{0};
                switch (currentPc)
                {
                case 0x21b0u: s_e21b0.fetch_add(1u, std::memory_order_relaxed); break;
                case 0x23e8u: s_e23e8.fetch_add(1u, std::memory_order_relaxed); break;
                case 0x2740u:
                {
                    s_e2740.fetch_add(1u, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_entSamp{0};
                    if (s_entSamp.fetch_add(1u, std::memory_order_relaxed) < 10u)
                        std::fprintf(stderr,
                            "[G140:ent] v18=(%.4g,%.4g,%.4g,%.4g) v19=(%.4g,%.4g,%.4g,%.4g) v20=(%.4g,%.4g,%.4g,%.4g)\n",
                            m_state.vf[18][0], m_state.vf[18][1], m_state.vf[18][2], m_state.vf[18][3],
                            m_state.vf[19][0], m_state.vf[19][1], m_state.vf[19][2], m_state.vf[19][3],
                            m_state.vf[20][0], m_state.vf[20][1], m_state.vf[20][2], m_state.vf[20][3]);
                    break;
                }
                // per-plane pass-end bail census: IBEQ VI0,VI11 -> 0x2f48 pcs
                case 0x2988u: case 0x2a38u: case 0x2ae8u:
                case 0x2b98u: case 0x2c48u: case 0x2cf8u:
                {
                    static std::atomic<uint64_t> s_passZero[6]{}, s_passLive[6]{};
                    const uint32_t pi = currentPc == 0x2988u ? 0u : currentPc == 0x2a38u ? 1u :
                                        currentPc == 0x2ae8u ? 2u : currentPc == 0x2b98u ? 3u :
                                        currentPc == 0x2c48u ? 4u : 5u;
                    const int16_t vi11 = (int16_t)m_state.vi[11];
                    (vi11 == 0 ? s_passZero[pi] : s_passLive[pi]).fetch_add(1u, std::memory_order_relaxed);
                    static std::atomic<uint32_t> s_passRep{0};
                    if ((s_passRep.fetch_add(1u, std::memory_order_relaxed) % 4000u) == 0u)
                        std::fprintf(stderr,
                            "[G140:pass] z/l p0=%llu/%llu p1=%llu/%llu p2=%llu/%llu p3=%llu/%llu p4=%llu/%llu p5=%llu/%llu\n",
                            (unsigned long long)s_passZero[0].load(), (unsigned long long)s_passLive[0].load(),
                            (unsigned long long)s_passZero[1].load(), (unsigned long long)s_passLive[1].load(),
                            (unsigned long long)s_passZero[2].load(), (unsigned long long)s_passLive[2].load(),
                            (unsigned long long)s_passZero[3].load(), (unsigned long long)s_passLive[3].load(),
                            (unsigned long long)s_passZero[4].load(), (unsigned long long)s_passLive[4].load(),
                            (unsigned long long)s_passZero[5].load(), (unsigned long long)s_passLive[5].load());
                    break;
                }
                case 0x2d08u: // emit path entered (survived all 6 planes)
                {
                    static std::atomic<uint64_t> s_emit{0};
                    const uint64_t e = s_emit.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (e <= 24u || (e % 2000u) == 0u)
                        std::fprintf(stderr, "[G140:emit] n=%llu vi11=%d\n",
                                     (unsigned long long)e, (int)(int16_t)m_state.vi[11]);
                    break;
                }
                case 0x30d0u: // FCGET VI1 just executed
                {
                    static std::atomic<uint32_t> s_fcSamp{0};
                    if (s_fcSamp.fetch_add(1u, std::memory_order_relaxed) < 40u)
                        std::fprintf(stderr, "[G140:fcget] vi1=0x%03x vi5=0x%x vi6=0x%x clip=0x%06x\n",
                                     (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[5],
                                     (uint32_t)(uint16_t)m_state.vi[6], m_state.clip & 0xFFFFFFu);
                    break;
                }
                case 0x30e8u: case 0x3108u: // A/B inside-test IBEQ just executed
                {
                    static std::atomic<uint32_t> s_ibSamp{0};
                    if (s_ibSamp.fetch_add(1u, std::memory_order_relaxed) < 24u)
                        std::fprintf(stderr, "[G140:ib] pc=0x%x vi1=0x%x vi4=0x%x vi5=0x%x vi6=0x%x post=0x%x\n",
                                     currentPc, (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[4],
                                     (uint32_t)(uint16_t)m_state.vi[5], (uint32_t)(uint16_t)m_state.vi[6], postExecPc);
                    break;
                }
                case 0x30b0u: // both CLIPw executed; VF17=A pos, VF21=B pos
                {
                    static std::atomic<uint32_t> s_cwSamp{0};
                    if (s_cwSamp.fetch_add(1u, std::memory_order_relaxed) < 18u)
                        std::fprintf(stderr,
                            "[G140:cw] vi8=0x%x A=(%.4g,%.4g,%.4g,%.4g) B=(%.4g,%.4g,%.4g,%.4g) clip12=0x%03x\n",
                            (uint32_t)(uint16_t)m_state.vi[8],
                            m_state.vf[17][0], m_state.vf[17][1], m_state.vf[17][2], m_state.vf[17][3],
                            m_state.vf[21][0], m_state.vf[21][1], m_state.vf[21][2], m_state.vf[21][3],
                            m_state.clip & 0xFFFu);
                    break;
                }
                case 0x22d8u: case 0x2548u:
                    // FCAND VI1,0x3ffff just executed this pair (lower). VI1!=0 => tri touches a plane.
                    if (m_state.vi[1] != 0) s_fcandHit.fetch_add(1u, std::memory_order_relaxed);
                    else s_fcandMiss.fetch_add(1u, std::memory_order_relaxed);
                    break;
                case 0x22e0u: case 0x2550u:
                    break;
                case 0x2d88u:
                case 0x2f38u:
                {
                    (currentPc == 0x2d88u ? s_k2d88 : s_k2f38).fetch_add(1u, std::memory_order_relaxed);
                    // VI7 = kick addr (780). Read the fan giftag nloop being kicked.
                    const uint32_t kickQw = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])) & 0x3FFu;
                    if (vuData && kickQw * 16u + 16u <= dataSize)
                    {
                        uint32_t w0 = 0u, w1 = 0u;
                        std::memcpy(&w0, vuData + kickQw * 16u, 4);
                        std::memcpy(&w1, vuData + kickQw * 16u + 4u, 4);
                        const uint32_t nloop = w0 & 0x7FFFu;
                        s_fanVerts.fetch_add(nloop, std::memory_order_relaxed);
                        if (s_samp.fetch_add(1u, std::memory_order_relaxed) < 24u)
                            std::fprintf(stderr, "[G140:fan] pc=0x%x kickQw=0x%x w0=%08x w1=%08x nloop=%u\n",
                                         currentPc, kickQw, w0, w1, nloop);
                    }
                    break;
                }
                default: break;
                }
                if ((s_rep.fetch_add(1u, std::memory_order_relaxed) % 400000u) == 0u)
                    std::fprintf(stderr,
                        "[G140:clip] e21b0=%llu e23e8=%llu clip2740=%llu kick2d88=%llu kick2f38=%llu "
                        "fcandHit=%llu fcandMiss=%llu fanVerts=%llu\n",
                        (unsigned long long)s_e21b0.load(), (unsigned long long)s_e23e8.load(),
                        (unsigned long long)s_e2740.load(), (unsigned long long)s_k2d88.load(),
                        (unsigned long long)s_k2f38.load(), (unsigned long long)s_fcandHit.load(),
                        (unsigned long long)s_fcandMiss.load(), (unsigned long long)s_fanVerts.load());
            }
        }

        // Enforce VF0 invariant
        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        // Enforce VI0 invariant
        m_state.vi[0] = 0;

        uint32_t sequentialPC = currentPc + 8;
        if (sequentialPC >= codeSize)
            sequentialPC = 0;
        uint32_t nextPC = sequentialPC;

        if (branchDelayActive)
        {
            nextPC = pendingBranchTarget;
            branchDelayActive = false;
        }
        else if (postExecPc != currentPc)
        {
            pendingBranchTarget = postExecPc + 8;
            if (pendingBranchTarget >= codeSize)
                pendingBranchTarget = 0;
            branchDelayActive = true;
        }

        if (nextPC >= codeSize)
            nextPC = 0;
        m_state.pc = nextPC;

        if (m_state.ebit)
        {
            stopReason = "ebit-latched";
            break;
        }

        if (eBit)
            m_state.ebit = true;
    }

    if (vu1_trace_enabled())
    {
        const uint32_t n = s_g22Vu1StopCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 128u)
        {
            std::fprintf(stderr,
                         "[G22:vu1stop] n=%u startPc=0x%x cycles=%u reason=%s pc=0x%x lastPc=0x%x lastLower=0x%08x lastUpper=0x%08x ebit=%u itop=0x%x\n",
                         n, startPc, cycle, stopReason, m_state.pc, lastPc, lastLower, lastUpper,
                         m_state.ebit ? 1u : 0u, m_state.itop);
        }
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
void VU1Interpreter::execUpper(uint32_t instr)
{
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    const float *vs = m_state.vf[fs];
    const float *vt = m_state.vf[ft];
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > m_state.i) ? vs[c] : m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < m_state.i) ? vs[c] : m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * m_state.q;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * m_state.i;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] + vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = m_state.acc[c] - vs[c] * vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = m_state.acc[0] - vs[1] * vt[2];
        result[1] = m_state.acc[1] - vs[2] * vt[0];
        result[2] = m_state.acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        vu1UpdateMac(m_state.mac, m_state.status, result, dest);
        applyDest(vd, result, dest);
        return;

    // Special1 group (0x3C..0x3F with secondary field)
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t vuFunc = S2_FUNC(instr);

        if (vuFunc <= 0x0Fu)
        {
            const float bc = broadcast(vt, vuFunc & 3u);
            const uint8_t group = vuFunc >> 2u;
            for (int c = 0; c < 4; c++)
            {
                if (group == 0u) // ADDAx/y/z/w
                    result[c] = vs[c] + bc;
                else if (group == 1u) // SUBAx/y/z/w
                    result[c] = vs[c] - bc;
                else if (group == 2u) // MADDAx/y/z/w
                    result[c] = m_state.acc[c] + vs[c] * bc;
                else // MSUBAx/y/z/w
                    result[c] = m_state.acc[c] - vs[c] * bc;
            }
            applyDestAcc(result, dest);
            return;
        }

        if (vuFunc >= 0x10u && vuFunc <= 0x13u) // ITOF0/4/12/15
        {
            const float scale = (vuFunc == 0x10u) ? 1.0f :
                                (vuFunc == 0x11u) ? (1.0f / 16.0f) :
                                (vuFunc == 0x12u) ? (1.0f / 4096.0f) :
                                                     (1.0f / 32768.0f);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &vs[c], 4);
                result[c] = (float)iv * scale;
            }
            // G32: ITOF/FTOI/ABS are 2-operand VU ops; destination is ft (bits 20:16),
            // NOT fd. Writing fd left the model's XYZF2 source VF (ft) stale -> every packed
            // vertex shared one stale position -> model collapsed to a point. (VU1 is only
            // used by the skinned-model path; the dungeon/title use the VIF1 DIRECT path.)
            applyDest(m_state.vf[ft], result, dest);
            return;
        }

        if (vuFunc >= 0x14u && vuFunc <= 0x17u) // FTOI0/4/12/15
        {
            const float scale = (vuFunc == 0x14u) ? 1.0f :
                                (vuFunc == 0x15u) ? 16.0f :
                                (vuFunc == 0x16u) ? 4096.0f :
                                                     32768.0f;
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = (int32_t)(vs[c] * scale);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(m_state.vf[ft], result, dest); // G32: dest = ft, not fd
            return;
        }

        if (vuFunc >= 0x18u && vuFunc <= 0x1Bu) // MULAx/y/z/w
        {
            const float bc = broadcast(vt, vuFunc & 3u);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyDestAcc(result, dest);
            return;
        }

        switch (vuFunc)
        {
        case 0x1C: // MULAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(m_state.vf[ft], result, dest); // G32: ABS dest = ft, not fd
            return;
        case 0x1E: // MULAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x1F: // CLIPw
        {
            float w = std::fabs(vt[3]);
            uint32_t flags = 0;
            if (vs[0] > +w)
                flags |= 0x01;
            if (vs[0] < -w)
                flags |= 0x02;
            if (vs[1] > +w)
                flags |= 0x04;
            if (vs[1] < -w)
                flags |= 0x08;
            if (vs[2] > +w)
                flags |= 0x10;
            if (vs[2] < -w)
                flags |= 0x20;
            m_state.clip = (m_state.clip << 6) | flags;
            // G65: aggregate CLIPw outcomes per program startPC so we can tell whether the
            // title's geometry genuinely projects outside the ±W clip volume (all flags set =
            // wrong projection/camera) vs a downstream ADC bug. Sample a few (xyz,w).
            static const bool s_g65clip = (std::getenv("DC2_G65_CLIP") != nullptr);
            if (s_g65clip)
            {
                static std::atomic<uint64_t> s_tot{0}, s_clipped{0}, s_wneg{0}, s_samp{0};
                s_tot.fetch_add(1u, std::memory_order_relaxed);
                if (flags) s_clipped.fetch_add(1u, std::memory_order_relaxed);
                if (vt[3] <= 0.0f) s_wneg.fetch_add(1u, std::memory_order_relaxed);
                if (s_samp.fetch_add(1u, std::memory_order_relaxed) < 16u)
                    std::fprintf(stderr, "[G65:clip] startPC=0x%x flags=0x%02x vs=(% .3g % .3g % .3g) w=% .3g\n",
                                 s_g65CurStartPC, flags, vs[0], vs[1], vs[2], vt[3]);
                const uint64_t t = s_tot.load(std::memory_order_relaxed);
                if ((t % 20000u) == 0u)
                    std::fprintf(stderr, "[G65:clipagg] startPC=0x%x total=%llu clipped=%llu wNegOrZero=%llu\n",
                                 s_g65CurStartPC, (unsigned long long)t,
                                 (unsigned long long)s_clipped.load(), (unsigned long long)s_wneg.load());
            }
            return;
        }
        case 0x20: // ADDAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * m_state.q;
            applyDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * m_state.i;
            applyDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] + vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            for (int c = 0; c < 4; c++)
                result[c] = m_state.acc[c] - vs[c] * vt[c];
            applyDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = m_state.acc[3];
            applyDestAcc(result, dest);
            return;
        case 0x2F: // VNOP
            return;
        case 0x30: // MOVE
            applyDest(m_state.vf[ft], vs, dest);
            return;
        case 0x31: // MR32
        {
            float tmp[4] = {vs[1], vs[2], vs[3], vs[0]};
            applyDest(m_state.vf[ft], tmp, dest);
            return;
        }
        default:
            g22_log_unknown_upper(m_state.pc, instr, "special2", op, vuFunc);
            return;
        }

        uint8_t special = (instr >> 6) & 0x1F;
        uint8_t sop = (instr & 0x3) | ((instr >> 4) & 0x3C);
        (void)sop;

        switch (instr & 0x3F)
        {
        case 0x3C: // Special1 (ADDAx..ADDAw, SUBAx..SUBAw, MADDAx..MADDAw, MSUBAx..MSUBAw, etc.)
        {
            uint8_t funct = (instr >> 6) & 0x1F;
            uint8_t bc2 = (instr >> 0) & 0x3;
            (void)bc2;
            switch (funct)
            {
            case 0x00:
            case 0x01:
            case 0x02:
            case 0x03: // ADDAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x04:
            case 0x05:
            case 0x06:
            case 0x07: // SUBAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x08:
            case 0x09:
            case 0x0A:
            case 0x0B: // MADDAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x0C:
            case 0x0D:
            case 0x0E:
            case 0x0F: // MSUBAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x10: // ITOF0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv;
                }
                applyDest(vd, result, dest);
                return;
            case 0x11: // ITOF4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 16.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x12: // ITOF12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 4096.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x13: // ITOF15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 32768.0f;
                }
                applyDest(vd, result, dest);
                return;
            case 0x14: // FTOI0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)vs[c];
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x15: // FTOI4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 16.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x16: // FTOI12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 4096.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x17: // FTOI15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 32768.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                applyDest(vd, result, dest);
                return;
            case 0x18:
            case 0x19:
            case 0x1A:
            case 0x1B: // MULAbc
            {
                float bc = broadcast(vt, funct & 3);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x1C: // MULAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x1D: // ABS
                for (int c = 0; c < 4; c++)
                    result[c] = std::fabs(vs[c]);
                applyDest(vd, result, dest);
                return;
            case 0x1E: // MULAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x1F: // CLIP
            {
                float w = std::fabs(vt[3]);
                uint32_t flags = 0;
                if (vs[0] > +w)
                    flags |= 0x01;
                if (vs[0] < -w)
                    flags |= 0x02;
                if (vs[1] > +w)
                    flags |= 0x04;
                if (vs[1] < -w)
                    flags |= 0x08;
                if (vs[2] > +w)
                    flags |= 0x10;
                if (vs[2] < -w)
                    flags |= 0x20;
                m_state.clip = (m_state.clip << 6) | flags;
                return;
            }
            default:
                g22_log_unknown_upper(m_state.pc, instr, "special1", instr & 0x3Fu, funct);
                return;
            }
        }
        case 0x3D: // Special2 (ADDAq, MADDAq, ADDAi, MADDAi, ADDA, MADDA, MULA, OPMULA, ...)
        {
            uint8_t funct = (instr >> 6) & 0x1F;
            switch (funct)
            {
            case 0x00: // ADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x01: // MADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x02: // ADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x03: // MADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x04: // SUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x05: // MSUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x06: // SUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x07: // MSUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x08: // ADDA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x09: // MADDA
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] + vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0A: // MULA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0C: // SUBA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0D: // MSUBA
                for (int c = 0; c < 4; c++)
                    result[c] = m_state.acc[c] - vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x0E: // OPMULA
                result[0] = vs[1] * vt[2];
                result[1] = vs[2] * vt[0];
                result[2] = vs[0] * vt[1];
                result[3] = 0.0f;
                applyDestAcc(result, dest);
                return;
            case 0x0F: // NOP
                return;
            default:
                g22_log_unknown_upper(m_state.pc, instr, "special2", instr & 0x3Fu, funct);
                return;
            }
        }
        case 0x3E: // Special (more upper ops, rarely used)
            g22_log_unknown_upper(m_state.pc, instr, "special3e", op, special);
            return;
        case 0x3F: // Special (upper NOP typically)
            return;
        }
        return;
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33: // iadd-like upper? No, these are valid upper ops
    default:
        // NOP / unimplemented upper
        g22_log_unknown_upper(m_state.pc, instr, "primary", op, 0u);
        return;
    }
}

// ============================================================================
// Lower instructions
// ============================================================================
void VU1Interpreter::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    (void)upperInstr;
    if (instr == 0x00000000 || instr == 0x8000033C) // NOP
        return;

    uint8_t opHi = (instr >> 25) & 0x7F;

    // G133: copy-packer behind-camera SKIP. The copy packer (0x1b68) emits the wall tristrips with
    // ADC = the FOG clamp (structurally 0, G132), so it DRAWS the behind-camera spanning triangles HW
    // SKIPS (G133 drawn-vs-skipped extent A/B: HW draws only <1024px tris, skips the >512px/full-screen
    // ones). The copy input VF16 = (screenX, screenY, screenZ, 1/W) carries the q sign — VF16.w<0 =
    // behind camera (DC2_G132_COPYW range VF16.w=[-0.0034,128]). Capture the kicking vert's 1/W at the
    // copy loop's `MOVE VF26.xyz<-VF16` (pc 0x1c10), and at the XYZF2 store (pc 0x1c28) set the ADC bit
    // (word3 bit15) when 1/W<=0 so the GS skips the triangle ending at that vert = HW's near-plane
    // strip-restart, on the NATURAL copy route (vs the G100 forced 0x1ff0). 1-iter software-pipeline:
    // the store at 0x1c28 emits the VF26 set at the PREVIOUS 0x1c10, so consume the PREVIOUS stash;
    // the pipeline-edge first verts are strip primers (complete no triangle) so an off-by-one there is
    // harmless. Title-rock-scoped; opt-in DC2_G133_COPY_QSKIP (default off) to PROVE the diagnosis.
    static const bool s_g133qskip = (std::getenv("DC2_G133_COPY_QSKIP") != nullptr);
    static float s_g133qPrev = 1.0f, s_g133qCur = 1.0f;
    static std::atomic<uint64_t> s_g133skipN{0}, s_g133keepN{0};
    if (s_g133qskip && m_state.pc == 0x1c10u &&
        g_dc2TitleRockScope.load(std::memory_order_relaxed))
    {
        s_g133qPrev = s_g133qCur;
        s_g133qCur = m_state.vf[16][3];
    }

    // G32: watch the GIF-packing loop (model packs ST/RGBAQ/XYZF2 into the PATH1 output
    // buffer here). G31 showed varied transformed verts (LQI) collapse to an identical
    // packed XYZF2. Log every integer/store op in the packing PC window with the VI
    // pointer/value state so we can see whether the output pointer fails to advance or the
    // packed value is constant. Capped + env-gated (DC2_TRACE_G32).
    if (g32_vu_trace_enabled() && m_state.pc >= 0x1b00u && m_state.pc <= 0x1c80u)
    {
        const uint32_t n = s_g32Count.fetch_add(1u, std::memory_order_relaxed);
        if (n < 20000u)
        {
            const uint8_t it = LIT(instr);
            const uint8_t is = LIS(instr);
            const uint8_t dst = (instr >> 21) & 0xF;
            const uint8_t f6 = instr & 0x3F;
            const bool isSpecial2 = (opHi == 0x40u) && (f6 >= 0x3Cu);
            const uint8_t s2 = isSpecial2 ? S2_FUNC(instr) : 0xFFu;
            // SQI(0x35): store VF[is] -> (VI[it]++). Dump the VALUE being packed + dest addr.
            if (s2 == 0x35u && m_state.pc == 0x1c28u) // the XYZF2 store
            {
                int32_t ix, iy, iz;
                std::memcpy(&ix, &m_state.vf[is][0], 4);
                std::memcpy(&iy, &m_state.vf[is][1], 4);
                std::memcpy(&iz, &m_state.vf[is][2], 4);
                std::fprintf(stderr, "[G32:xyz] n=%u px=(%.2f,%.2f) z=%d ix=%d iy=%d\n",
                             n, ix / 16.0f, iy / 16.0f, iz, ix, iy);
            }
            else if (s2 == 0x34u) // LQI: load VF[it] <- (VI[is]++)
            {
                const uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                std::fprintf(stderr, "[G32:lqi] n=%u pc=0x%x LQI VF%u <- addr=0x%x (VI%u)\n",
                             n, m_state.pc, (uint32_t)it, addr, (uint32_t)is);
            }
            else if (s2 == 0x3Fu) // ISWR
            {
                const uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                std::fprintf(stderr, "[G32:iswr] n=%u pc=0x%x ISWR dst=0x%x VI%u=0x%x -> addr=0x%x (VI%u)\n",
                             n, m_state.pc, (uint32_t)dst, (uint32_t)it,
                             (uint32_t)(uint16_t)m_state.vi[it], addr, (uint32_t)is);
            }
        }
    }

    // G39: trace the loads / top reads that feed the model transform so the vertex / matrix /
    // translation source address is visible across consecutive kicks (n=1 good, n=3+ bad).
    if (s_g39LoadTrace.load(std::memory_order_relaxed) > 0)
    {
        const uint8_t it = LIT(instr);
        const uint8_t is = LIS(instr);
        const uint8_t funct = instr & 0x3Fu;
        const uint8_t s2 = (opHi == 0x40u && funct >= 0x3Cu) ? S2_FUNC(instr) : 0xFFu;
        const char *name = nullptr;
        uint32_t addr = 0xFFFFFFFFu;
        float fv[4] = {0, 0, 0, 0};
        int32_t iv = 0;
        bool haveQuad = false, haveInt = false;
        auto readQuad = [&](uint32_t a)
        { a &= (dataSize - 1u); if (a + 16u <= dataSize) { std::memcpy(fv, vuData + a, 16); haveQuad = true; } addr = a; };
        if (opHi == 0x00u) { name = "LQ"; readQuad(((uint32_t)(int32_t)(m_state.vi[is] + IMM11(instr))) * 16u); }
        else if (opHi == 0x04u) { name = "ILW"; readQuad(((uint32_t)(int32_t)(m_state.vi[is] + IMM11(instr))) * 16u); }
        else if (s2 == 0x34u) { name = "LQI"; readQuad(((uint32_t)(uint16_t)m_state.vi[is]) * 16u); }
        else if (s2 == 0x36u) { name = "LQD"; readQuad(((uint32_t)(uint16_t)(m_state.vi[is] - 1)) * 16u); }
        else if (s2 == 0x3Eu) { name = "ILWR"; readQuad(((uint32_t)(uint16_t)m_state.vi[is]) * 16u); }
        else if (s2 == 0x3Cu) { name = "MTIR"; std::memcpy(&iv, &m_state.vf[is][(instr >> 21) & 0x3], 4); haveInt = true; }
        else if (s2 == 0x68u) { name = "XTOP"; iv = (int32_t)(memory ? (memory->vif1_regs.top & 0x3FFu) : 0u); haveInt = true; }
        else if (s2 == 0x69u) { name = "XITOP"; iv = (int32_t)(m_state.itop & 0x3FFu); haveInt = true; }
        if (name)
        {
            s_g39LoadTrace.fetch_sub(1, std::memory_order_relaxed);
            if (haveQuad)
                std::fprintf(stderr, "[G39:ld] k=%u pc=0x%x %s VF%u<-addr=0x%x (VI%u=0x%x) = % .4g % .4g % .4g % .4g\n",
                             s_g39KickIdx.load(std::memory_order_relaxed), m_state.pc, name, (unsigned)it,
                             addr, (unsigned)is, (unsigned)(uint16_t)m_state.vi[is], fv[0], fv[1], fv[2], fv[3]);
            else if (haveInt)
                std::fprintf(stderr, "[G39:ld] k=%u pc=0x%x %s VI%u<-0x%x (VI%u=0x%x)\n",
                             s_g39KickIdx.load(std::memory_order_relaxed), m_state.pc, name, (unsigned)it,
                             (unsigned)iv, (unsigned)is, (unsigned)(uint16_t)m_state.vi[is]);
        }
    }

    // The lower instruction encoding uses bits 31:25 for the primary opcode
    switch (opHi)
    {
    case 0x00: // LQ (Load Quadword from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(m_state.vf[it], tmp, dest);
        }
        return;
    }
    case 0x01: // SQ (Store Quadword to VU data memory)
    {
        uint8_t is = LIS(instr);
        uint8_t it = LIT(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[it] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            if (dest & 0x8)
                tmp[0] = m_state.vf[is][0];
            if (dest & 0x4)
                tmp[1] = m_state.vf[is][1];
            if (dest & 0x2)
                tmp[2] = m_state.vf[is][2];
            if (dest & 0x1)
                tmp[3] = m_state.vf[is][3];
            std::memcpy(vuData + addr, tmp, 16);
        }
        return;
    }
    case 0x04: // ILW (Integer Load Word from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            int comp = 0;
            if (dest & 0x8)
                comp = 0;
            else if (dest & 0x4)
                comp = 1;
            else if (dest & 0x2)
                comp = 2;
            else
                comp = 3;
            uint32_t v;
            std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (it != 0)
                m_state.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return;
    }
    case 0x05: // ISW (Integer Store Word to VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t val = (uint32_t)(uint16_t)(m_state.vi[it] & 0xFFFF);
            if (dest & 0x8)
                std::memcpy(vuData + addr + 0, &val, 4);
            if (dest & 0x4)
                std::memcpy(vuData + addr + 4, &val, 4);
            if (dest & 0x2)
                std::memcpy(vuData + addr + 8, &val, 4);
            if (dest & 0x1)
                std::memcpy(vuData + addr + 12, &val, 4);
        }
        return;
    }
    case 0x08: // IADDIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] + imm);
        return;
    }
    case 0x09: // ISUBIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] - imm);
        return;
    }
    case 0x10: // FCEQ
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & 0xFFFFFF) == imm24) ? 1 : 0;
        return;
    }
    case 0x11: // FCSET
    {
        m_state.clip = instr & 0xFFFFFF;
        return;
    }
    case 0x12: // FCAND
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & imm24) != 0) ? 1 : 0;
        return;
    }
    case 0x13: // FCOR
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip | imm24) == 0xFFFFFF) ? 1 : 0;
        return;
    }
    case 0x14: // FSEQ
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = ((statusRead & 0xFFF) == imm12) ? 1 : 0;
        return;
    }
    case 0x15: // FSSET
    {
        m_state.status = (instr >> 6) & 0xFC0;
        return;
    }
    case 0x16: // FSAND
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = (int32_t)(statusRead & imm12);
        return;
    }
    case 0x17: // FSOR
    {
        uint16_t imm12 = instr & 0xFFF;
        const uint32_t statusRead = g138_macpipe_enabled() ? s_vuStatusVisible
                                    : (s_g106UseLowerFlagRead ? s_g106LowerStatusRead : m_state.status);
        if (1 != 0)
            m_state.vi[1] = ((statusRead | imm12) == 0xFFF) ? 1 : 0;
        return;
    }
    // G138: the MAC-flag lower opcodes were mis-mapped vs the real VU lower-opcode table
    // (PCSX2 VUops.cpp _LOWER_OPCODE[128]): 0x18=FMEQ, 0x1A=FMAND, 0x1B=FMOR, 0x1C=FCGET.
    // The old runner had 0x18=FMAND / 0x1A=FMEQ (SWAPPED) and FMOR parked on 0x1C. Every
    // "FMEQ" in the G70..G137 title-gate disassemblies was therefore really FMAND: the
    // transform packers' draw gate is an FMAND mask cascade (VI = mac & 0xD0 accumulated
    // over the guard-plane SUBs, winding bit 0x20 IORed in, IBEQ against 0xD0|qw30 = draw
    // iff inside-guard AND front-facing) — impossible under FMEQ's 0/1 results, which is
    // why the gate never fired and the whole cavern emitted ADC=1 (blue void / G100 band-
    // aid). Fix is global (all VU1 programs decoded through the swapped slots). Kill-switch
    // DC2_VU1_NO_FMSWAPFIX restores the old mapping for A/B.
    case 0x18: // real VU: FMEQ (old runner: FMAND)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (it != 0)
        {
            if (s_fmFixed)
                m_state.vi[it] = ((macRead & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
            else
                m_state.vi[it] = (int32_t)(macRead & (uint32_t)(uint16_t)m_state.vi[is]);
        }
        return;
    }
    case 0x1A: // real VU: FMAND (old runner: FMEQ)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (it != 0)
        {
            if (s_fmFixed)
                m_state.vi[it] = (int32_t)(macRead & (uint32_t)(uint16_t)m_state.vi[is]);
            else
                m_state.vi[it] = ((macRead & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
        }
        return;
    }
    case 0x1B: // real VU: FMOR (old runner: unmapped)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                 : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
        if (s_fmFixed && it != 0)
            m_state.vi[it] = (int32_t)(macRead | (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    case 0x1C: // real VU: FCGET (old runner: FMOR)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        static const bool s_fmFixed = (std::getenv("DC2_VU1_NO_FMSWAPFIX") == nullptr);
        if (s_fmFixed)
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(m_state.clip & 0xFFFu);
        }
        else
        {
            const uint32_t macRead = g138_macpipe_enabled() ? s_vuMacVisible
                                     : (s_g106UseLowerFlagRead ? s_g106LowerMacRead : m_state.mac);
            if (it != 0)
                m_state.vi[it] = (int32_t)(macRead | (uint32_t)(uint16_t)m_state.vi[is]);
        }
        return;
    }
    case 0x20: // B (unconditional branch)
    {
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        // The run loop detects this target and applies it after the delay slot.
        m_state.pc = target - 8;
        return;
    }
    case 0x21: // BAL (Branch and link)
    {
        uint8_t it = LIT(instr);
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.pc = target - 8;
        return;
    }
    case 0x24: // JR
    {
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & 0x3FFF;
        m_state.pc = target - 8;
        return;
    }
    case 0x25: // JALR
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & 0x3FFF;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.pc = target - 8;
        return;
    }
    case 0x28: // IBEQ
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] == (int16_t)m_state.vi[it])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x29: // IBNE
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] != (int16_t)m_state.vi[it])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2C: // IBLTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] < 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2D: // IBGTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] > 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2E: // IBLEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] <= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }
    case 0x2F: // IBGEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is] >= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.pc = target - 8;
        }
        return;
    }

    case 0x40: // Lower special (opcode in bits 5:0)
    {
        uint8_t funct = instr & 0x3F;
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t id = LID(instr);
        uint8_t dest = (instr >> 21) & 0xF;

        switch (funct)
        {
        case 0x30: // IADD
            if (id != 0)
                m_state.vi[id] = (int16_t)(m_state.vi[is] + m_state.vi[it]);
            return;
        case 0x31: // ISUB
            if (id != 0)
                m_state.vi[id] = (int16_t)(m_state.vi[is] - m_state.vi[it]);
            return;
        case 0x32: // IADDI
        {
            int16_t imm5 = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27);
            if (it != 0)
                m_state.vi[it] = (int16_t)(m_state.vi[is] + imm5);
            return;
        }
        case 0x34: // IAND
        {
            // G64: the title-map rock batches gate on IAND VI4,VI1,VI5 (high enable
            // 0x80/0x40/0x200/0x100) @0x30d8 and IAND VI4,VI1,VI6 (low enable 2/1/8/4)
            // @0x30f8/0x3168, inside the per-strip emit subroutine 0x3088. VI1 = the
            // per-mesh batch-enable flag from the VIF-unpacked control header
            // (ILWR.z VI1,(VI14) @0x2fc8) -- NOT qw0x26 (that gates a different post-kick
            // op @0x2e98). If VI1 lacks the high bits the full tristrip emit is skipped.
            // G64: repair the title/costume MAP VU1 program's per-mesh prim-type ENABLE flag.
            // The map render program emits geometry in 6 flag-gated batches via the emit
            // subroutine at 0x3088: `IAND VI4,VI1,VI5` @0x30d8 (high enable, VI5 masks
            // 0x40..0x800) and `IAND VI4,VI1,VI6` @0x30f8/0x3168 (low enable, VI6 2..0x20).
            // VI1 = the per-mesh enable mask (loaded by the header loader from the VIF-unpacked
            // control header). Headless it is 0 for EVERY mesh -> every IAND is 0 -> all batches
            // gated off -> only the degenerate trifan/flush path reaches the GS (the green swirl;
            // the rock tristrips/tris vanish). Repair: at each gate, OR the batch's own tested
            // mask (vi[it]) into VI1 so the batch passes its gate. The per-batch COUNT word VI11
            // (`IBEQ VI11,VI0` preamble check) still skips empty batches, so this only un-gates
            // the tristrip/tri batches the EE packet already carries (G62: 52 tristrip + 10 tri
            // templates present) -- it cannot synthesise geometry. Scoped to the 3 exact gate PCs
            // inside the map emit subroutine. Kill: DC2_G64_NO_ENABLE_FIX. Diagnostic (pre-repair
            // VI1): DC2_G63_GATE -> [G63:gate].
            // G140 (2026-07-06): G64 RETIRED BY DEFAULT — the premise was wrong. 0x3088 is NOT a
            // "per-mesh enable gate": it is the polygon CLIPPER's per-edge inside/outside test
            // (VI1 = FCGET clip flags of edge verts A/B; VI5/VI6 = the plane bit for A/B). ORing
            // the mask into VI1 marks EVERY vertex OUTSIDE every plane -> the Sutherland-Hodgman
            // passes always end with 0 verts -> the fan emitters (XGKICK 780 @0x2d88/0x2f38)
            // NEVER fire -> the clipped bottom strip + water pool trifans (HW 1012 verts/frame)
            // were missing from the title. With G64 off the runner emits the same alternating
            // empty/real trifan giftags as HW (00008000/0000800N 302ec000). The geometry G64
            // was credited with (rock tristrips) is actually gated by the transform packers'
            // FMAND cascade, fixed at the root by G138/G139. Re-enable: DC2_G64_FORCE_ENABLE_FIX.
            if (m_state.pc == 0x30d8u || m_state.pc == 0x30f8u || m_state.pc == 0x3168u)
            {
                static const bool s_g63gate = (std::getenv("DC2_G63_GATE") != nullptr);
                static const bool s_g64noEnable = (std::getenv("DC2_G64_FORCE_ENABLE_FIX") == nullptr);
                if (s_g63gate)
                {
                    static std::atomic<uint32_t> s_g63n{0};
                    if (s_g63n.fetch_add(1u, std::memory_order_relaxed) < 96u)
                        std::fprintf(stderr,
                            "[G63:gate] pc=0x%x VI1=0x%x VI5=0x%x VI6=0x%x VI11=0x%x\n",
                            m_state.pc,
                            (uint32_t)(uint16_t)m_state.vi[1], (uint32_t)(uint16_t)m_state.vi[5],
                            (uint32_t)(uint16_t)m_state.vi[6], (uint32_t)(uint16_t)m_state.vi[11]);
                    // G65: aggregate across ALL gate hits (not just first 96) so we can tell
                    // whether the emit subroutine EVER sees a nonzero per-batch count VI11.
                    static std::atomic<uint32_t> s_tot{0}, s_nz{0}, s_max{0};
                    const uint32_t v11 = (uint32_t)(uint16_t)m_state.vi[11];
                    s_tot.fetch_add(1u, std::memory_order_relaxed);
                    if (v11) s_nz.fetch_add(1u, std::memory_order_relaxed);
                    uint32_t pm = s_max.load(std::memory_order_relaxed);
                    while (v11 > pm && !s_max.compare_exchange_weak(pm, v11)) {}
                    const uint32_t tot = s_tot.load(std::memory_order_relaxed);
                    if ((tot % 2000u) == 0u)
                        std::fprintf(stderr, "[G65:gateagg] totalGateHits=%u nonzeroVI11=%u maxVI11=0x%x\n",
                                     tot, s_nz.load(std::memory_order_relaxed), s_max.load(std::memory_order_relaxed));
                }
                if (!s_g64noEnable && is == 1u)
                    m_state.vi[1] = (int32_t)(uint16_t)((uint16_t)m_state.vi[1] | (uint16_t)m_state.vi[it]);
            }
            if (id != 0)
                m_state.vi[id] = m_state.vi[is] & m_state.vi[it];
            return;
        }
        case 0x35: // IOR
            if (id != 0)
                m_state.vi[id] = m_state.vi[is] | m_state.vi[it];
            return;

        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F: // Lower special2 subtables
        {
            const uint8_t vuFunc = S2_FUNC(instr);
            switch (vuFunc)
            {
            case 0x30: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[is], 16);
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x31: // MR32 (xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[is][1], m_state.vf[is][2], m_state.vf[is][3], m_state.vf[is][0]};
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x34: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (g31_vu_trace_enabled())
                    {
                        const uint32_t n = s_g31VuLqiCount.fetch_add(1u, std::memory_order_relaxed);
                        if (n < 192u)
                        {
                            std::fprintf(stderr,
                                         "[G31:lqi] n=%u pc=0x%x fn=0x34 it=VF%u is=VI%u viBefore=0x%x addr=0x%x dest=0x%x v=(% .4g,% .4g,% .4g,% .4g)\n",
                                         n, m_state.pc, static_cast<uint32_t>(it), static_cast<uint32_t>(is),
                                         static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                         addr, static_cast<uint32_t>(dest),
                                         tmp[0], tmp[1], tmp[2], tmp[3]);
                        }
                    }
                    applyDest(m_state.vf[it], tmp, dest);
                }
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] + 1);
                return;
            }
            case 0x35: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    // G133: copy-packer behind-camera skip — the copy XYZF2 store is this SQI at pc
                    // 0x1c28 (S2_FUNC==0x35). Set ADC=word3 bit15 when the kicking vert's 1/W<=0
                    // (captured at 0x1c10). Skips the spanning connector triangle HW omits.
                    if (s_g133qskip && m_state.pc == 0x1c28u && (dest & 0x1) &&
                        g_dc2TitleRockScope.load(std::memory_order_relaxed))
                    {
                        if (s_g133qPrev <= 0.0f)
                        {
                            int32_t w3; std::memcpy(&w3, &tmp[3], 4);
                            w3 |= 0x8000; std::memcpy(&tmp[3], &w3, 4);
                            s_g133skipN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        else
                            s_g133keepN.fetch_add(1u, std::memory_order_relaxed);
                        static std::atomic<uint64_t> s_t{0};
                        if ((s_t.fetch_add(1u, std::memory_order_relaxed) % 200000u) == 0u)
                            std::fprintf(stderr, "[G133:qskip] skip=%llu keep=%llu\n",
                                         (unsigned long long)s_g133skipN.load(),
                                         (unsigned long long)s_g133keepN.load());
                    }
                    std::memcpy(vuData + addr, tmp, 16);
                }
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] + 1);
                return;
            }
            case 0x36: // LQD (Load Quadword, pre-decrement)
            {
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[it], tmp, dest);
                }
                return;
            }
            case 0x37: // SQD (Store Quadword, pre-decrement)
            {
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                return;
            }
            case 0x38: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = m_state.vf[it][ftf];
                float qv = (den != 0.0f)
                    ? (num / den)
                    : ((num >= 0.0f) ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max());
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                if (s_g29DivTrace.load(std::memory_order_relaxed) > 0)
                {
                    s_g29DivTrace.fetch_sub(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G29:div] op=0x38 pc=0x%x num=% .4g den=% .4g q=% .4g  it=VF%d ftf=%d vf[it]=(% .4g % .4g % .4g % .4g)\n",
                                 m_state.pc, num, den, qv, it, ftf,
                                 m_state.vf[it][0], m_state.vf[it][1], m_state.vf[it][2], m_state.vf[it][3]);
                    // G38: one-shot full VF dump at first GOOD (den!=0) and first BAD (den==0)
                    // divide, so the matrix regs zeroed in the bad case stand out.
                    static std::atomic<int> s_dumpGood{0}, s_dumpBad{0};
                    const bool bad = (den == 0.0f);
                    if ((bad ? s_dumpBad : s_dumpGood).exchange(1) == 0)
                    {
                        char b[1200]; int p = 0;
                        for (int r = 0; r < 32; ++r)
                            p += std::snprintf(b + p, sizeof(b) - p, "VF%d(% .3g,% .3g,% .3g,% .3g) ",
                                               r, m_state.vf[r][0], m_state.vf[r][1], m_state.vf[r][2], m_state.vf[r][3]);
                        std::fprintf(stderr, "[G38:vfdiv %s] %s\n", bad ? "BAD" : "GOOD", b);
                    }
                }
                return;
            }
            case 0x39: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                float val = m_state.vf[it][ftf];
                float qv = std::sqrt(std::fabs(val));
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x3A: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = std::sqrt(std::fabs(m_state.vf[it][ftf]));
                float qv = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::max();
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 13; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x3B: // WAITQ
                if (g87_q_latency_enabled() && s_vuQDelay > 0) { m_state.q = s_vuQPending; s_vuQDelay = 0; }
                return;
            case 0x3C: // MTIR
            {
                int comp = (instr >> 21) & 0x3;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[is][comp], 4);
                if (it != 0)
                    m_state.vi[it] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x3D: // MFIR
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[is] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x3E: // ILWR
            {
                if (it == 0)
                    return;
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t value = 0u;
                    if (dest & 0x8)
                        std::memcpy(&value, vuData + addr + 0u, 4);
                    if (dest & 0x4)
                        std::memcpy(&value, vuData + addr + 4u, 4);
                    if (dest & 0x2)
                        std::memcpy(&value, vuData + addr + 8u, 4);
                    if (dest & 0x1)
                        std::memcpy(&value, vuData + addr + 12u, 4);
                    m_state.vi[it] = (int32_t)(int16_t)(value & 0xFFFFu);
                }
                return;
            }
            case 0x3F: // ISWR
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t value = (uint32_t)(uint16_t)(m_state.vi[it] & 0xFFFF);
                    if (dest & 0x8)
                        std::memcpy(vuData + addr + 0u, &value, 4);
                    if (dest & 0x4)
                        std::memcpy(vuData + addr + 4u, &value, 4);
                    if (dest & 0x2)
                        std::memcpy(vuData + addr + 8u, &value, 4);
                    if (dest & 0x1)
                        std::memcpy(vuData + addr + 12u, &value, 4);
                }
                return;
            }
            case 0x40: // RNEXT
            case 0x41: // RGET
            case 0x42: // RINIT
            case 0x43: // RXOR
                return;
            case 0x64: // MFP
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x68: // XTOP - double-buffer DATA top (VIF1 TOPS latched at MSCAL); the model
                       // transform reads its vertex/count buffer relative to this. G28: was
                       // returning ITOP (0) -> reads from VU addr 0 -> runaway loop.
                if (it != 0)
                    m_state.vi[it] = (int32_t)(memory ? (memory->vif1_regs.top & 0x3FFu)
                                                      : (m_state.itop & 0x3FFu));
                return;
            case 0x69: // XITOP - INTEGER top (VIF1 ITOP)
                if (it != 0)
                    m_state.vi[it] = (int32_t)(m_state.itop & 0x3FFu);
                return;
            case 0x6C: // XGKICK
                goto legacy_xgkick;
            case 0x70: // ESADD
                m_state.p = m_state.vf[is][0] * m_state.vf[is][0] +
                            m_state.vf[is][1] * m_state.vf[is][1] +
                            m_state.vf[is][2] * m_state.vf[is][2];
                return;
            case 0x71: // ERSADD
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                m_state.p = (s != 0.0f) ? (1.0f / s) : 0.0f;
                return;
            }
            case 0x72: // ELENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                m_state.p = std::sqrt(std::fabs(s));
                return;
            }
            case 0x73: // ERLENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] +
                          m_state.vf[is][1] * m_state.vf[is][1] +
                          m_state.vf[is][2] * m_state.vf[is][2];
                float len = std::sqrt(std::fabs(s));
                m_state.p = (len != 0.0f) ? (1.0f / len) : 0.0f;
                return;
            }
            case 0x76: // ESUM
                m_state.p = m_state.vf[is][0] + m_state.vf[is][1] + m_state.vf[is][2] + m_state.vf[is][3];
                return;
            case 0x78: // ESQRT
            {
                int fsf = (instr >> 21) & 0x3;
                m_state.p = std::sqrt(std::fabs(m_state.vf[is][fsf]));
                return;
            }
            case 0x79: // ERSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                float root = std::sqrt(std::fabs(m_state.vf[is][fsf]));
                m_state.p = (root != 0.0f) ? (1.0f / root) : 0.0f;
                return;
            }
            case 0x7A: // ERCPR
            {
                int fsf = (instr >> 21) & 0x3;
                float val = m_state.vf[is][fsf];
                m_state.p = (val != 0.0f) ? (1.0f / val) : 0.0f;
                return;
            }
            case 0x7B: // WAITP
            case 0x74: // EATANxy
            case 0x75: // EATANxz
            case 0x77: // reserved
            case 0x7C: // ESIN
            case 0x7D: // EATAN
            case 0x7E: // EEXP
                return;
            default:
                g22_log_unknown_lower(m_state.pc, instr, "lower-special2", opHi, vuFunc);
                return;
            }

            uint8_t funct2 = (instr >> 6) & 0x1F;
            switch (funct2)
            {
            case 0x00: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[is], 16);
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x01: // MR32 (rotate right by 32 bits = shift xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[is][1], m_state.vf[is][2], m_state.vf[is][3], m_state.vf[is][0]};
                applyDest(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x03: // MFIR (Move From Integer Register)
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[is] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            case 0x04: // MTIR (Move To Integer Register)
            {
                int comp = 0;
                if (dest & 0x8)
                    comp = 0;
                else if (dest & 0x4)
                    comp = 1;
                else if (dest & 0x2)
                    comp = 2;
                else
                    comp = 3;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[is][comp], 4);
                if (it != 0)
                    m_state.vi[it] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x05: // RNEXT
                return;
            case 0x06: // RGET
                return;
            case 0x07: // RINIT
                return;
            case 0x10: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (g31_vu_trace_enabled())
                    {
                        const uint32_t n = s_g31VuLqiCount.fetch_add(1u, std::memory_order_relaxed);
                        if (n < 192u)
                        {
                            std::fprintf(stderr,
                                         "[G31:lqi] n=%u pc=0x%x fn=0x10 it=VF%u is=VI%u viBefore=0x%x addr=0x%x dest=0x%x v=(% .4g,% .4g,% .4g,% .4g)\n",
                                         n, m_state.pc, static_cast<uint32_t>(it), static_cast<uint32_t>(is),
                                         static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                         addr, static_cast<uint32_t>(dest),
                                         tmp[0], tmp[1], tmp[2], tmp[3]);
                        }
                    }
                    applyDest(m_state.vf[it], tmp, dest);
                }
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] + 1);
                return;
            }
            case 0x11: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    // G133: copy-packer behind-camera skip — at the XYZF2 store (pc 0x1c28, VF17),
                    // set ADC=word3 bit15 when the kicking vert's 1/W<=0 (behind camera). Skips the
                    // spanning connector triangle HW omits. Title-rock-scoped + opt-in.
                    if (s_g133qskip && m_state.pc == 0x1c28u && is == 17u && (dest & 0x1) &&
                        g_dc2TitleRockScope.load(std::memory_order_relaxed))
                    {
                        if (s_g133qPrev <= 0.0f)
                        {
                            int32_t w3; std::memcpy(&w3, &tmp[3], 4);
                            w3 |= 0x8000; std::memcpy(&tmp[3], &w3, 4);
                            s_g133skipN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        else
                            s_g133keepN.fetch_add(1u, std::memory_order_relaxed);
                        static std::atomic<uint64_t> s_t{0};
                        if ((s_t.fetch_add(1u, std::memory_order_relaxed) % 200000u) == 0u)
                            std::fprintf(stderr, "[G133:qskip] skip=%llu keep=%llu\n",
                                         (unsigned long long)s_g133skipN.load(),
                                         (unsigned long long)s_g133keepN.load());
                    }
                    std::memcpy(vuData + addr, tmp, 16);
                }
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] + 1);
                return;
            }
            case 0x12: // LQD (Load Quadword, pre-decrement)
            {
                if (is != 0)
                    m_state.vi[is] = (int16_t)(m_state.vi[is] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[it], tmp, dest);
                }
                return;
            }
            case 0x13: // SQD (Store Quadword, pre-decrement)
            {
                if (it != 0)
                    m_state.vi[it] = (int16_t)(m_state.vi[it] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                return;
            }
            case 0x14: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = m_state.vf[it][ftf];
                float qv = (den != 0.0f)
                    ? (num / den)
                    : ((num >= 0.0f) ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max());
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                if (s_g29DivTrace.load(std::memory_order_relaxed) > 0)
                {
                    s_g29DivTrace.fetch_sub(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G29:div] op=0x14 pc=0x%x num=% .4g den=% .4g q=% .4g\n",
                                 m_state.pc, num, den, qv);
                }
                return;
            }
            case 0x15: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                float val = m_state.vf[it][ftf];
                float qv = std::sqrt(std::fabs(val));
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 7; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x16: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = std::sqrt(std::fabs(m_state.vf[it][ftf]));
                float qv = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::max();
                if (g87_q_latency_enabled()) { if (g200_qstall_enabled() && s_vuQDelay > 0) m_state.q = s_vuQPending; s_vuQPending = qv; s_vuQDelay = 13; } // G200 FDIV busy-stall
                else m_state.q = qv;
                return;
            }
            case 0x17: // WAITQ
                if (g87_q_latency_enabled() && s_vuQDelay > 0) { m_state.q = s_vuQPending; s_vuQDelay = 0; }
                return;
            case 0x18: // ESADD
                return;
            case 0x19: // ERSADD
                return;
            case 0x1B: // ELENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                m_state.p = std::sqrt(s);
                return;
            }
            case 0x1C: // ERCPR
            {
                int fsf = (instr >> 21) & 0x3;
                float val = m_state.vf[is][fsf];
                m_state.p = (val != 0.0f) ? (1.0f / val) : std::numeric_limits<float>::max();
                return;
            }
            case 0x1D: // ERLENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                float len = std::sqrt(s);
                m_state.p = (len != 0.0f) ? (1.0f / len) : std::numeric_limits<float>::max();
                return;
            }
            case 0x1E: // WAITP
                return;
            case 0x1A: // EATAN / EATANxy / EATANxz
                return;
            case 0x1F: // MFP (Move From P register)
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[it], result, dest);
                return;
            }
            default:
                g22_log_unknown_lower(m_state.pc, instr, "lower-special2", opHi, funct2);
                return;
            }
        }
        case 0x7D: // legacy-unreachable XGKICK body; real dispatch is full function 0x6C.
        legacy_xgkick:
        {
            if (!vuData || dataSize < 16u)
                return;

            auto wrapOffset = [&](uint32_t off) -> uint32_t
            {
                return off % dataSize;
            };

            auto read64Wrap = [&](uint32_t off) -> uint64_t
            {
                uint8_t bytes[8];
                for (uint32_t i = 0; i < 8u; ++i)
                {
                    bytes[i] = vuData[wrapOffset(off + i)];
                }
                uint64_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                return value;
            };

            uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is]) * 16u;
            addr = wrapOffset(addr);
            // G62: at each XGKICK dump the GIFtag PRIM currently in VU memory at the candidate
            // template qwords (0x35c/0x398 = what the map per-strip copy subroutine reads;
            // 0x3c/0xf0 = where the EE templates were UNPACKED). If 0x35c/0x398 hold trifan(5)
            // while 0x3c/0xf0 hold the correct tristrip(4), the EE template never reached the
            // address VU1 reads (a VIF dest / addressing bug). Env DC2_G62_VUTPL.
            static const bool s_g62vutpl = (std::getenv("DC2_G62_VUTPL") != nullptr);
            if (s_g62vutpl)
            {
                static std::atomic<uint32_t> s_g62v{0};
                if (s_g62v.fetch_add(1u, std::memory_order_relaxed) < 24u)
                {
                    const uint32_t tplQw[4] = {0x35cu, 0x398u, 0x3cu, 0xf0u};
                    char buf[256]; int bp = 0;
                    bp += std::snprintf(buf + bp, sizeof(buf) - bp, "[G62:vutpl] kickAddr=0x%x ", addr);
                    for (int t = 0; t < 4; ++t)
                    {
                        const uint64_t lo = read64Wrap(wrapOffset(tplQw[t] * 16u));
                        const uint64_t hi = read64Wrap(wrapOffset(tplQw[t] * 16u + 8u));
                        const uint32_t prim = (uint32_t)((lo >> 47) & 0x7FFu);
                        bp += std::snprintf(buf + bp, sizeof(buf) - bp,
                                            "qw0x%x:prim=0x%x(t%u,pre%u,nl%u,hi=0x%llx) ",
                                            tplQw[t], prim, prim & 7u, (unsigned)((lo >> 46) & 1u),
                                            (unsigned)(lo & 0x7FFFu),
                                            (unsigned long long)(hi & 0xFFFFu));
                    }
                    // G63: also dump the batch-gating flag word (VU qw 0x26, read by the title-map
                    // program as `ILW.x VI1,38(VI0)`) and count words (qw 0x1e/0x1d), which gate
                    // which prim-type batches render (IAND VI1,{0x80,0x40,0x200,0x100}).
                    const uint64_t f26lo = read64Wrap(wrapOffset(0x26u * 16u));
                    const uint64_t f26hi = read64Wrap(wrapOffset(0x26u * 16u + 8u));
                    const uint64_t f1elo = read64Wrap(wrapOffset(0x1eu * 16u));
                    const uint64_t f1dlo = read64Wrap(wrapOffset(0x1du * 16u));
                    std::fprintf(stderr,
                                 "[G63:flags] qw0x26=%08x %08x %08x %08x  qw0x1e.x=%u qw0x1d.x=%u  &0x80=%u &0x40=%u &0x200=%u &0x100=%u\n",
                                 (uint32_t)(f26lo & 0xFFFFFFFFu), (uint32_t)(f26lo >> 32),
                                 (uint32_t)(f26hi & 0xFFFFFFFFu), (uint32_t)(f26hi >> 32),
                                 (uint32_t)(f1elo & 0xFFFFFFFFu), (uint32_t)(f1dlo & 0xFFFFFFFFu),
                                 (unsigned)((f26lo & 0x80u) != 0u), (unsigned)((f26lo & 0x40u) != 0u),
                                 (unsigned)((f26lo & 0x200u) != 0u), (unsigned)((f26lo & 0x100u) != 0u));
                }
            }
            if (vu1_trace_enabled())
            {
                const uint32_t n = s_g22Vu1XgkickPacketCount.fetch_add(1u, std::memory_order_relaxed);
                if (n < 96u)
                {
                    uint64_t tagLo = 0u;
                    uint64_t tagHi = 0u;
                    if (dataSize >= 16u)
                    {
                        tagLo = read64Wrap(addr);
                        tagHi = read64Wrap(addr + 8u);
                    }
                    std::fprintf(stderr,
                                 "[G22:xgkick] n=%u pc=0x%x is=VI%u vi=0x%x addr=0x%x tagLo=0x%016llx tagHi=0x%016llx itop=0x%x\n",
                                 n, m_state.pc, static_cast<uint32_t>(is),
                                 static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[is])),
                                 addr,
                                 static_cast<unsigned long long>(tagLo),
                                 static_cast<unsigned long long>(tagHi),
                                 m_state.itop);

                    // G28: decode the first PACKED tristrip vertex (ST,RGBAQ,XYZF2 => XYZF2 at
                    // tag+48) screen XY (12.4 fixed) to confirm the transform lands on-screen.
                    {
                        const uint8_t flg0 = (uint8_t)((tagLo >> 58) & 0x3u);
                        const uint32_t nreg0 = (uint32_t)((tagLo >> 60) & 0xFu);
                        if (flg0 == 0u && nreg0 == 3u && dataSize >= 64u && n < 4u)
                        {
                            // Raw dump of vert0's 3 regs (ST,RGBAQ,XYZF2) lo/hi to find the
                            // real XYZF2 layout / whether positions are genuinely zero.
                            std::fprintf(stderr,
                                "[G28:raw] n=%u ST=%016llx_%016llx RGBAQ=%016llx_%016llx XYZF2=%016llx_%016llx\n",
                                n,
                                (unsigned long long)read64Wrap(wrapOffset(addr + 24u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 16u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 40u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 32u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 56u)),
                                (unsigned long long)read64Wrap(wrapOffset(addr + 48u)));
                        }
                    }

                    // G24: map where real data sits in VU1 data mem (16-byte qwords) so we can
                    // tell "no input delivered" from "input present but transformed to NaN".
                    uint32_t nzQw = 0u, firstNz[6] = {0,0,0,0,0,0}, nFirst = 0u;
                    for (uint32_t off = 0u; off + 16u <= dataSize; off += 16u)
                    {
                        bool nz = false;
                        for (uint32_t b = 0u; b < 16u; ++b) { if (vuData[off + b] != 0u) { nz = true; break; } }
                        if (nz) { if (nFirst < 6u) firstNz[nFirst++] = off; ++nzQw; }
                    }
                    std::fprintf(stderr,
                                 "[G24:vumap] nzQw=%u/%u firstNz=0x%x,0x%x,0x%x,0x%x,0x%x,0x%x readAddr=0x%x\n",
                                 nzQw, dataSize / 16u, firstNz[0], firstNz[1], firstNz[2],
                                 firstNz[3], firstNz[4], firstNz[5], addr);
                }
            }
            // G65: attribute the prim types emitted by THIS XGKICK to the program that built
            // the packet (s_g65CurStartPC). Walk the giftag chain, tally NLOOP per PRIM type
            // into a per-startPC bucket, print periodically. Reveals which VU1 program emits
            // the rock tristrips (PRIM type 4) and whether the title ever runs it.
            static const bool s_g65kickagg = (std::getenv("DC2_G65_KICKAGG") != nullptr);
            if (s_g65kickagg)
            {
                struct Bucket { uint32_t pc; uint64_t prim[8]; uint64_t kicks; };
                static Bucket s_b[24]; static uint32_t s_bn = 0; static uint64_t s_kc = 0;
                Bucket* bk = nullptr;
                for (uint32_t i = 0; i < s_bn; ++i) if (s_b[i].pc == s_g65CurStartPC) { bk = &s_b[i]; break; }
                if (!bk && s_bn < 24u) { bk = &s_b[s_bn++]; bk->pc = s_g65CurStartPC; for (int j=0;j<8;++j) bk->prim[j]=0; bk->kicks=0; }
                if (bk)
                {
                    bk->kicks++;
                    uint32_t off = addr; bool d2 = false;
                    for (int s2 = 0; s2 < 256 && !d2; ++s2)
                    {
                        uint64_t lo = read64Wrap(off);
                        uint32_t nl = (uint32_t)(lo & 0x7FFFu);
                        uint8_t fg = (uint8_t)((lo >> 58) & 0x3u);
                        uint32_t nr = (uint32_t)((lo >> 60) & 0xFu); if (nr == 0u) nr = 16u;
                        bool pre = ((lo >> 46) & 1u) != 0u;
                        uint32_t prim = (uint32_t)((lo >> 47) & 0x7u);
                        bool eop2 = ((lo >> 15) & 1u) != 0u;
                        if (pre) bk->prim[prim] += nl;
                        uint32_t ps = 16u;
                        if (fg == 0u) ps += nl * nr * 16u;
                        else if (fg == 1u) { uint32_t rg = nl*nr; ps += rg*8u; if (rg&1u) ps += 8u; }
                        else if (fg == 2u) ps += nl * 16u;
                        if (ps == 0u) break;
                        off = wrapOffset(off + ps);
                        if (eop2) d2 = true;
                    }
                }
                if ((++s_kc % 4000u) == 0u)
                {
                    for (uint32_t i = 0; i < s_bn; ++i)
                        std::fprintf(stderr, "[G65:kickagg] startPC=0x%x kicks=%llu prim[pt0..7]= %llu %llu %llu %llu %llu %llu %llu %llu\n",
                                     s_b[i].pc, (unsigned long long)s_b[i].kicks,
                                     (unsigned long long)s_b[i].prim[0],(unsigned long long)s_b[i].prim[1],
                                     (unsigned long long)s_b[i].prim[2],(unsigned long long)s_b[i].prim[3],
                                     (unsigned long long)s_b[i].prim[4],(unsigned long long)s_b[i].prim[5],
                                     (unsigned long long)s_b[i].prim[6],(unsigned long long)s_b[i].prim[7]);
                }
            }

            // G118: live per-PRIM per-ADC tally of THIS XGKICK's emitted packet — the
            // runner-side equivalent of tools/g69_hw_adc.py on ref/dumps/new_game.gs. Walks the
            // kicked GIFtag chain in VU memory and counts XYZF2/XYZ2 vertices by (prim, ADC bit
            // 111 = hi>>47&1). Answers the decisive G118 question: does the runner's NATURAL
            // (DC2_G100_NO_FORCEDRAW=1) tri packer (0x1dc0) reproduce HW's ~85%-draw "0,0,1"
            // pattern and the tristrip ~60%-nodraw primer pattern, and is trifan(pt5) genuinely
            // zero? Env DC2_G118_ADC; startPc==0x10 only; quiet by default.
            static const bool s_g118adc = (std::getenv("DC2_G118_ADC") != nullptr);
            if (s_g118adc && s_g65CurStartPC == 0x10u)
            {
                static std::atomic<uint64_t> s_adcSet[8] = {};
                static std::atomic<uint64_t> s_adcClr[8] = {};
                static std::atomic<uint64_t> s_g118kc{0};
                // Per-PACKER tally keyed by the XGKICK pc: 0x2180=tristrip(0x1ff0),
                // 0x1fd0=tri(0x1dc0), 0x1da0=trifan(0x1c50), 0x1c30=copy(0x1b68). Resolves the
                // copy-vs-transform question: which packer's verts actually DRAW (ADC=0) on the
                // runner, and is the 0x1ff0 transform-tristrip 100% nodraw (= the cavern walls
                // invisible) while the drawn tristrips come from the copy packer.
                static std::atomic<uint64_t> s_pkSet[4] = {}; // [tristrip,tri,trifan,copy]
                static std::atomic<uint64_t> s_pkClr[4] = {};
                // G119: per-packer screen-position stats of DRAWING (ADC=0) verts. XYZF2 packed:
                // X=lo[0:15], Y=lo[16:31] in 12.4 fixed (px = raw/16, screen centred ~XYOFFSET).
                // "on-screen" window (generous): raw X in [0x4000,0xD000], Y in [0x4000,0xC000].
                // Resolves the copy-vs-transform crux: which packer's drawn verts are visible.
                static std::atomic<uint64_t> s_pkOn[4] = {};   // drawing verts on-screen
                static std::atomic<uint64_t> s_pkOff[4] = {};  // drawing verts off-screen
                static std::atomic<uint64_t> s_pkSX[4] = {};   // sum X (drawing verts, for centroid)
                static std::atomic<uint64_t> s_pkSY[4] = {};   // sum Y
                static std::atomic<uint64_t> s_pkAX[4] = {};   // sum X over ALL verts (incl nodraw)
                static std::atomic<uint64_t> s_pkAY[4] = {};   // sum Y over ALL verts
                static std::atomic<uint64_t> s_pkAN[4] = {};   // count ALL verts
                // G132: per-(packer x GS-prim) ADC tally. Resolves which packer emits which GS prim
                // (e.g. does the copy packer 0x1b68 emit prim=tri(3) all-draw, while the drawing
                // tristrips(4) come from a different packer?). [packer][prim] -> draw(ADC0)/nodraw.
                static std::atomic<uint64_t> s_pkPrimDraw[4][8] = {};
                static std::atomic<uint64_t> s_pkPrimNo[4][8] = {};
                // G119: per-STRIP structure of the tristrip packer (pkIdx==0, pc=0x2180). For each
                // tristrip GIFtag, nloop = strip length and the per-vertex loop index l = strip
                // position. Histogram strip-length and ADC-by-position so the runner's 0x1ff0 output
                // is DIRECTLY comparable to tools/g117_strip_adc.py on ref/dumps/new_game.gs (HW:
                // 1296 strips len 4-7, pos0/1 92% nodraw primers, pos2+ ~45%). Settles whether the
                // runner builds HW's short-strip structure (=> fix is per-position ADC in 0x1ff0) or
                // a divergent grouping (=> EE packet bug). Env DC2_G119_STRIP; quiet by default.
                static const bool s_g119strip = (std::getenv("DC2_G119_STRIP") != nullptr);
                static std::atomic<uint64_t> s_posSet[32] = {}; // ADC=1 (nodraw) by strip position
                static std::atomic<uint64_t> s_posClr[32] = {}; // ADC=0 (draw)   by strip position
                static std::atomic<uint64_t> s_lenHist[33] = {}; // strip-length histogram (cap 32)
                static std::atomic<uint64_t> s_stripN{0};       // total tristrip GIFtags seen
                static std::atomic<uint64_t> s_copyLenHist[33] = {}; // G132 copy-packer tristrip lens
                static std::atomic<uint64_t> s_copyStripN{0};        // G132 copy tristrip GIFtags
                static std::atomic<uint64_t> s_copyExt[7] = {};      // G132 copy tristrip extent buckets px
                int pkIdx = -1;
                switch (m_state.pc) { case 0x2180u: pkIdx=0; break; case 0x1fd0u: pkIdx=1; break;
                                      case 0x1da0u: pkIdx=2; break; case 0x1c30u: pkIdx=3; break; }
                uint32_t off = addr; bool eopDone = false; uint32_t curPrim = 0u;
                for (int s2 = 0; s2 < 256 && !eopDone; ++s2)
                {
                    uint64_t lo = read64Wrap(off);
                    uint32_t nl = (uint32_t)(lo & 0x7FFFu);
                    uint8_t fg = (uint8_t)((lo >> 58) & 0x3u);
                    uint32_t nr = (uint32_t)((lo >> 60) & 0xFu); if (nr == 0u) nr = 16u;
                    bool pre = ((lo >> 46) & 1u) != 0u;
                    bool eop2 = ((lo >> 15) & 1u) != 0u;
                    uint64_t rd = read64Wrap(off + 8u); // REGS descriptor (bits 64..127)
                    if (pre) curPrim = (uint32_t)((lo >> 47) & 0x7u);
                    uint32_t voff = off + 16u;
                    if (fg == 0u)
                    {
                        // G119: this GIFtag's vertex count == strip length for the tristrip packer.
                        if (s_g119strip && pkIdx == 0 && nl >= 3u)
                        {
                            s_lenHist[nl < 32u ? nl : 32u].fetch_add(1u, std::memory_order_relaxed);
                            s_stripN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        // G132: COPY-packer tristrip GIFtag length histogram. If the copy packer
                        // (pkIdx==3) emits ONE long tristrip GIFtag = many strips concatenated, every
                        // strip boundary (no restart ADC, copy ADC structurally 0) draws a spanning
                        // connector triangle = the green streaks. Short (4-7) = one real strip each.
                        if (s_g119strip && pkIdx == 3 && curPrim == 4u && nl >= 3u)
                        {
                            s_copyLenHist[nl < 32u ? nl : 32u].fetch_add(1u, std::memory_order_relaxed);
                            s_copyStripN.fetch_add(1u, std::memory_order_relaxed);
                        }
                        // G132: per-copy-tristrip-GIFtag screen bbox (spanning vs local test).
                        const uint32_t gifPrim = curPrim;
                        int cgx0 = 0x7fffffff, cgx1 = -1, cgy0 = 0x7fffffff, cgy1 = -1;
                        for (uint32_t l = 0; l < nl; ++l)
                            for (uint32_t r = 0; r < nr; ++r)
                            {
                                uint32_t rr = (uint32_t)((rd >> (r * 4u)) & 0xFu);
                                if (rr == 0x00u) curPrim = (uint32_t)(read64Wrap(voff) & 0x7u);
                                else if (rr == 0x04u || rr == 0x05u)
                                {
                                    uint64_t hi = read64Wrap(voff + 8u);
                                    uint32_t a = (uint32_t)((hi >> 47) & 1u);
                                    if (s_g119strip && pkIdx == 3 && gifPrim == 4u)
                                    {
                                        uint64_t vb = read64Wrap(voff);
                                        int bx = (int)(vb & 0xFFFFu), by = (int)((vb >> 32) & 0xFFFFu);
                                        if (bx < cgx0) cgx0 = bx; if (bx > cgx1) cgx1 = bx;
                                        if (by < cgy0) cgy0 = by; if (by > cgy1) cgy1 = by;
                                    }
                                    if (a) s_adcSet[curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                    else   s_adcClr[curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                    if (pkIdx >= 0) { if (a) s_pkSet[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                                      else   s_pkClr[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                                      // G132: per-(packer x prim)
                                                      if (a) s_pkPrimNo[pkIdx][curPrim & 7u].fetch_add(1u, std::memory_order_relaxed);
                                                      else   s_pkPrimDraw[pkIdx][curPrim & 7u].fetch_add(1u, std::memory_order_relaxed); }
                                    // G119: screen-position per packer. ALL verts -> centroid (so
                                    // 0x1ff0's on-screen nodraw cavern has a reference); DRAWING
                                    // verts -> on/off-screen.
                                    if (s_g119strip && pkIdx >= 0)
                                    {
                                        uint64_t vlo = read64Wrap(voff);
                                        // PACKED XYZF2: X=lo[0:15], Y=lo[32:47] (12.4 fixed).
                                        uint32_t vx = (uint32_t)(vlo & 0xFFFFu);
                                        uint32_t vy = (uint32_t)((vlo >> 32) & 0xFFFFu);
                                        s_pkAX[pkIdx].fetch_add(vx, std::memory_order_relaxed);
                                        s_pkAY[pkIdx].fetch_add(vy, std::memory_order_relaxed);
                                        s_pkAN[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                        if (a == 0u)
                                        {
                                            s_pkSX[pkIdx].fetch_add(vx, std::memory_order_relaxed);
                                            s_pkSY[pkIdx].fetch_add(vy, std::memory_order_relaxed);
                                            bool on = (vx >= 0x4000u && vx <= 0xD000u && vy >= 0x4000u && vy <= 0xC000u);
                                            if (on) s_pkOn[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                            else    s_pkOff[pkIdx].fetch_add(1u, std::memory_order_relaxed);
                                        }
                                    }
                                    // G119: ADC by strip position (l) for the tristrip packer.
                                    if (s_g119strip && pkIdx == 0 && nl >= 3u)
                                    {
                                        uint32_t p = l < 32u ? l : 31u;
                                        if (a) s_posSet[p].fetch_add(1u, std::memory_order_relaxed);
                                        else   s_posClr[p].fetch_add(1u, std::memory_order_relaxed);
                                    }
                                }
                                voff += 16u;
                            }
                        // G132: finalize this copy-tristrip GIFtag's extent (px) -> bucket.
                        if (s_g119strip && pkIdx == 3 && gifPrim == 4u && cgx1 >= 0 && nl >= 3u)
                        {
                            int ext = (cgx1 - cgx0); int ey = (cgy1 - cgy0); if (ey > ext) ext = ey;
                            ext /= 16; // 12.4 -> px
                            uint32_t b = ext < 64 ? 0u : ext < 128 ? 1u : ext < 256 ? 2u : ext < 512 ? 3u :
                                         ext < 1024 ? 4u : ext < 2048 ? 5u : 6u;
                            s_copyExt[b].fetch_add(1u, std::memory_order_relaxed);
                        }
                    }
                    uint32_t ps = 16u;
                    if (fg == 0u) ps += nl * nr * 16u;
                    else if (fg == 1u) { uint32_t rg = nl * nr; ps += rg * 8u; if (rg & 1u) ps += 8u; }
                    else if (fg == 2u) ps += nl * 16u;
                    if (ps == 0u) break;
                    off = wrapOffset(off + ps);
                    if (eop2) eopDone = true;
                }
                if ((s_g118kc.fetch_add(1u, std::memory_order_relaxed) % 4000u) == 0u)
                {
                    std::fprintf(stderr,
                        "[G118:adc] tri(set/clr)=%llu/%llu tristrip=%llu/%llu trifan=%llu/%llu\n",
                        (unsigned long long)s_adcSet[3].load(), (unsigned long long)s_adcClr[3].load(),
                        (unsigned long long)s_adcSet[4].load(), (unsigned long long)s_adcClr[4].load(),
                        (unsigned long long)s_adcSet[5].load(), (unsigned long long)s_adcClr[5].load());
                    std::fprintf(stderr,
                        "[G118:pkr] 0x1ff0tstrip(set/clr)=%llu/%llu 0x1dc0tri=%llu/%llu 0x1c50tfan=%llu/%llu 0x1b68copy=%llu/%llu\n",
                        (unsigned long long)s_pkSet[0].load(), (unsigned long long)s_pkClr[0].load(),
                        (unsigned long long)s_pkSet[1].load(), (unsigned long long)s_pkClr[1].load(),
                        (unsigned long long)s_pkSet[2].load(), (unsigned long long)s_pkClr[2].load(),
                        (unsigned long long)s_pkSet[3].load(), (unsigned long long)s_pkClr[3].load());
                    {
                        // G132: per-(packer x prim) draw/nodraw. prim 3=tri 4=tristrip 5=trifan.
                        const char* pkn132[4] = {"0x1ff0","0x1dc0","0x1c50","0x1b68"};
                        for (int k = 0; k < 4; ++k)
                            std::fprintf(stderr,
                                "[G132:pkprim] %s tri(d/n)=%llu/%llu tstrip=%llu/%llu tfan=%llu/%llu\n",
                                pkn132[k],
                                (unsigned long long)s_pkPrimDraw[k][3].load(), (unsigned long long)s_pkPrimNo[k][3].load(),
                                (unsigned long long)s_pkPrimDraw[k][4].load(), (unsigned long long)s_pkPrimNo[k][4].load(),
                                (unsigned long long)s_pkPrimDraw[k][5].load(), (unsigned long long)s_pkPrimNo[k][5].load());
                    }
                    if (s_g119strip)
                    {
                        // Per-packer DRAWING-vert screen position: on/off-screen + centroid (px).
                        const char* pkn[4] = {"0x1ff0tstrip","0x1dc0tri","0x1c50tfan","0x1b68copy"};
                        for (int k = 0; k < 4; ++k)
                        {
                            uint64_t on = s_pkOn[k].load(), off = s_pkOff[k].load(), t = on + off;
                            double cx = t ? (double)s_pkSX[k].load()/(double)t/16.0 : 0.0;
                            double cy = t ? (double)s_pkSY[k].load()/(double)t/16.0 : 0.0;
                            uint64_t an = s_pkAN[k].load();
                            double ax = an ? (double)s_pkAX[k].load()/(double)an/16.0 : 0.0;
                            double ay = an ? (double)s_pkAY[k].load()/(double)an/16.0 : 0.0;
                            std::fprintf(stderr, "[G119:pos2] %-12s draw=%llu drawOn=%.0f%% drawCentroid=(%.0f,%.0f) ALLn=%llu allCentroid=(%.0f,%.0f)\n",
                                         pkn[k], (unsigned long long)t, t ? 100.0*(double)on/(double)t : 0.0, cx, cy,
                                         (unsigned long long)an, ax, ay);
                        }
                        // Strip-length histogram (matches g117_strip_adc.py len histogram on HW).
                        std::fprintf(stderr, "[G119:len] tstripGIFtags=%llu lenHist(3..16,>16):",
                                     (unsigned long long)s_stripN.load());
                        for (uint32_t k = 3; k <= 16; ++k)
                            std::fprintf(stderr, " %u:%llu", k, (unsigned long long)s_lenHist[k].load());
                        uint64_t gt16 = 0; for (uint32_t k = 17; k <= 32; ++k) gt16 += s_lenHist[k].load();
                        std::fprintf(stderr, " >16:%llu\n", (unsigned long long)gt16);
                        // G132: COPY-packer tristrip GIFtag length histogram (spanning-connector test).
                        std::fprintf(stderr, "[G132:copylen] copyTstripGIFtags=%llu lenHist(3..16,>16):",
                                     (unsigned long long)s_copyStripN.load());
                        for (uint32_t k = 3; k <= 16; ++k)
                            std::fprintf(stderr, " %u:%llu", k, (unsigned long long)s_copyLenHist[k].load());
                        uint64_t cgt16 = 0; for (uint32_t k = 17; k <= 32; ++k) cgt16 += s_copyLenHist[k].load();
                        std::fprintf(stderr, " >16:%llu\n", (unsigned long long)cgt16);
                        // G132: copy-tristrip screen-extent histogram (spanning test). Buckets px:
                        // <64 <128 <256 <512 <1024 <2048 >=2048. Local strips => mostly <128.
                        std::fprintf(stderr, "[G132:copyext] px <64:%llu <128:%llu <256:%llu <512:%llu <1024:%llu <2048:%llu >=2048:%llu\n",
                                     (unsigned long long)s_copyExt[0].load(), (unsigned long long)s_copyExt[1].load(),
                                     (unsigned long long)s_copyExt[2].load(), (unsigned long long)s_copyExt[3].load(),
                                     (unsigned long long)s_copyExt[4].load(), (unsigned long long)s_copyExt[5].load(),
                                     (unsigned long long)s_copyExt[6].load());
                        // ADC=1(nodraw)% by strip position pos0..pos9.
                        std::fprintf(stderr, "[G119:pos] nodraw%% by strip position:");
                        for (uint32_t p = 0; p < 10; ++p)
                        {
                            uint64_t s = s_posSet[p].load(), c = s_posClr[p].load(), t = s + c;
                            double pct = t ? (100.0 * (double)s / (double)t) : 0.0;
                            std::fprintf(stderr, " pos%u:%.0f%%(n=%llu)", p, pct, (unsigned long long)t);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }

            uint32_t pktOff = addr;
            uint32_t totalBytes = 0u;
            bool done = false;

            for (int safety = 0; safety < 256 && !done; ++safety)
            {
                uint64_t tagLo = read64Wrap(pktOff);
                uint32_t nloop = (uint32_t)(tagLo & 0x7FFFu);
                uint8_t flg = (uint8_t)((tagLo >> 58) & 0x3u);
                uint32_t nreg = (uint32_t)((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                bool eop = ((tagLo >> 15) & 0x1ull) != 0ull;

                uint32_t pktSize = 16u;
                if (flg == 0u)
                {
                    pktSize += nloop * nreg * 16u;
                }
                else if (flg == 1u)
                {
                    uint32_t regs = nloop * nreg;
                    pktSize += regs * 8u;
                    if ((regs & 1u) != 0u)
                        pktSize += 8u;
                }
                else if (flg == 2u)
                {
                    pktSize += nloop * 16u;
                }

                if (pktSize == 0u)
                    break;

                totalBytes += pktSize;
                pktOff = wrapOffset(pktOff + pktSize);
                if (eop)
                    done = true;
            }

            if (totalBytes == 0u)
                return;

            const uint32_t debugIndex = s_debugVu1XgkickCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[vu1:xgkick] idx=" << debugIndex
                                                << " addr=0x" << std::hex << addr
                                                << " totalBytes=0x" << totalBytes
                                                << std::dec
                                                << " wrap=" << static_cast<uint32_t>((addr + totalBytes > dataSize) ? 1u : 0u)
                                                << std::endl);
            }

            // G96: title rock TEX0/geometry de-interleaving fix (root of G91/G95). Each title
            // map-part batch UNPACKs its intended TEX0 into VU data memory (state packet), but the
            // forced copy packer's XGKICK never re-emits it, so every rock batch samples whatever
            // the up-front texture reload last bound -> the wall pages (0x2b20/0x2f20/0x3320) are
            // bound to the GS but no geometry is ever drawn while they are active (blue voids /
            // "missing models"). Title-rock-scoped: locate this batch's TEX0 in VU data memory and
            // PREPEND a TEX0_1 A+D GIF register write to the kicked packet, reproducing the HW
            // per-batch bind. DC2_G96_TEXSCAN reports the VU offset; DC2_G96_TEX0BIND applies.
            extern std::atomic<bool> g_dc2TitleRockScope;
            static const bool s_g96bind = (std::getenv("DC2_G96_TEX0BIND") != nullptr);
            static const bool s_g96scan = (std::getenv("DC2_G96_TEXSCAN") != nullptr);
            static const uint32_t s_g107StripAdcFirst = []() -> uint32_t {
                const char *e = std::getenv("DC2_G107_STRIP_ADC_FIRST");
                if (!e)
                    return 0u;
                const long v = std::strtol(e, nullptr, 0);
                return (v > 0) ? static_cast<uint32_t>((v > 64) ? 64 : v) : 0u;
            }();
            static const uint32_t s_g107StripAdcTarget = []() -> uint32_t {
                const char *e = std::getenv("DC2_G107_STRIP_ADC_TBP");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
            }();
            uint64_t g96Tex0 = 0u;
            uint32_t g96Off = 0xffffffffu;
            if ((s_g96bind || s_g96scan || s_g107StripAdcFirst != 0u || g136_trace_enabled()) && vuData &&
                g_dc2TitleRockScope.load(std::memory_order_relaxed))
            {
                for (uint32_t off = 0u; off + 8u <= dataSize; off += 4u)
                {
                    uint64_t q;
                    std::memcpy(&q, vuData + off, 8);
                    const uint32_t tbp = (uint32_t)(q & 0x3FFFu);
                    const uint32_t psm = (uint32_t)((q >> 20) & 0x3Fu);
                    if (psm == 0x13u && tbp >= 0x2720u && tbp <= 0x3960u)
                    {
                        g96Tex0 = q;
                        g96Off = off;
                        break;
                    }
                }
                if (s_g96scan)
                {
                    static std::atomic<uint32_t> s_g96n{0};
                    const uint32_t sn = s_g96n.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 96u)
                        std::fprintf(stderr, "[G96:texscan] kickAddr=0x%x off=0x%x tex0=0x%llx tbp=0x%x\n",
                                     addr, g96Off, (unsigned long long)g96Tex0,
                                     (unsigned)(g96Tex0 & 0x3FFFu));
                    // One-shot hexdump of the low VU data region (where the state packet UNPACKs
                    // matrices/TEX0/light) so we can eyeball the per-batch TEX0 location.
                    if (sn < 4u)
                    {
                        for (uint32_t b = 0u; b + 16u <= 0x180u && b + 16u <= dataSize; b += 16u)
                            std::fprintf(stderr, "[G96:vumem] kick%u 0x%03x: %016llx %016llx\n",
                                         sn, b,
                                         (unsigned long long)(*(uint64_t *)(vuData + b)),
                                         (unsigned long long)(*(uint64_t *)(vuData + b + 8u)));
                    }
                }
            }

            // G98: per-kick world (LW) matrix probe. HW gives each title map-part its own LW
            // matrix in its state packet; the suspicion is the runner uploads all parts' state
            // up front (de-interleaved from geometry) so every kick's VU memory ends up with the
            // SAME (last) part's matrix -> 11/12 parts transform off-screen. Dump the world-matrix
            // rows (VU 0x80/0xa0 rotation, 0xb0 translation) + combined-matrix row0 per kick so a
            // python pass can confirm whether all kicks share one matrix. DC2_G98_MTX, title scope.
            {
                static const bool s_g98mtx = (std::getenv("DC2_G98_MTX") != nullptr);
                if (s_g98mtx && vuData &&
                    g_dc2TitleRockScope.load(std::memory_order_relaxed))
                {
                    static std::atomic<uint32_t> s_g98n{0};
                    const uint32_t sn = s_g98n.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 96u)
                    {
                        auto F = [&](uint32_t o) -> float {
                            float f; std::memcpy(&f, vuData + o, 4); return f;
                        };
                        std::fprintf(stderr,
                            "[G98:mtx] kick=%u worldRot=(%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f) "
                            "comb0=(%.1f,%.1f,%.1f,%.3f)\n",
                            sn, F(0x80), F(0x88), F(0xa0), F(0xb0), F(0xb4), F(0xb8),
                            F(0x40), F(0x44), F(0x48), F(0x4c));
                    }
                }
            }

            auto g136DumpVuQwords = [&](uint32_t seq, const char *label, uint32_t qwBase, uint32_t qwords)
            {
                const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    const uint32_t qw = (qwBase + q) & 0x3FFu;
                    const uint32_t byteOff = qw * 16u;
                    const uint64_t lo = read64Wrap(byteOff);
                    const uint64_t hi = read64Wrap(byteOff + 8u);
                    uint32_t w[4] = {
                        static_cast<uint32_t>(lo & 0xFFFFFFFFu),
                        static_cast<uint32_t>(lo >> 32u),
                        static_cast<uint32_t>(hi & 0xFFFFFFFFu),
                        static_cast<uint32_t>(hi >> 32u),
                    };
                    float f[4] = {};
                    std::memcpy(f, w, sizeof(f));
                    std::fprintf(stderr,
                                 "[G136:vuqw] seq=%u %s qw=0x%x raw=%08x %08x %08x %08x f=(% .6g % .6g % .6g % .6g)\n",
                                 seq, label, qw, w[0], w[1], w[2], w[3],
                                 f[0], f[1], f[2], f[3]);
                }
            };

            auto g137DumpOrigin = [&](uint32_t seq, const char *label, uint32_t qwBase, uint32_t qwords)
            {
                if (!g137_trace_enabled())
                    return;

                const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    const uint32_t qw = (qwBase + q) & 0x3FFu;
                    const Dc2G137VuOrigin &origin = g_dc2G137VuOrigins[qw];
                    std::fprintf(stderr,
                                 "[G137:origin] seq=%u %s qw=0x%x valid=%u oseq=%u cmdOff=0x%x srcOff=0x%x op=0x%02x imm=0x%04x dest=0x%x write=%u srcIdx=%u cl=%u wl=%u mode=%u tops=0x%x top=0x%x raw=%08x %08x %08x %08x src=%08x %08x %08x %08x\n",
                                 seq, label, qw, origin.valid, origin.seq,
                                 origin.cmdOff, origin.srcOff, origin.opcode, origin.imm,
                                 origin.destVec, origin.writeIndex, origin.srcIndex,
                                 origin.cl, origin.wl, origin.mode, origin.tops, origin.top,
                                 origin.raw[0], origin.raw[1], origin.raw[2], origin.raw[3],
                                 origin.srcRaw[0], origin.srcRaw[1], origin.srcRaw[2], origin.srcRaw[3]);
                }
            };

            auto g136DumpPacketQwords = [&](uint32_t seq, const uint8_t *pkt, uint32_t bytes)
            {
                const uint32_t capped = std::min<uint32_t>(bytes / 16u, g136_qword_limit());
                for (uint32_t q = 0u; q < capped; ++q)
                {
                    uint32_t w[4] = {};
                    std::memcpy(w, pkt + q * 16u, sizeof(w));
                    std::fprintf(stderr,
                                 "[G136:xgqw] seq=%u q=%u %08x %08x %08x %08x\n",
                                 seq, q, w[0], w[1], w[2], w[3]);
                }
            };

            auto g217JoinXgkick = [&](const uint8_t *pkt, uint32_t bytes)
            {
                if (!s_g217Join || !pkt || bytes < 16u)
                    return;

                const uint64_t tex0Before = s_g217LastTex0;
                uint64_t curTex0 = tex0Before;
                uint64_t geomTex0 = 0u;
                uint32_t texWrites = 0u;
                uint32_t tags = 0u;
                uint32_t invalid = 0u;
                uint32_t curPrim = 0u;
                uint32_t verts[8] = {};
                uint32_t adcDraw[8] = {};
                uint32_t adcNoDraw[8] = {};
                uint32_t minX = 0xFFFFFFFFu, minY = 0xFFFFFFFFu;
                uint32_t maxX = 0u, maxY = 0u;
                uint32_t off = 0u;

                auto bindTex0 = [&](uint64_t value)
                {
                    curTex0 = value;
                    ++texWrites;
                };
                auto visitVertex = [&](uint64_t lo, uint64_t hi)
                {
                    const uint32_t prim = curPrim & 7u;
                    if (prim < 8u)
                    {
                        ++verts[prim];
                        if (((hi >> 47u) & 1ull) != 0ull)
                            ++adcNoDraw[prim];
                        else
                            ++adcDraw[prim];
                    }
                    if (geomTex0 == 0u)
                        geomTex0 = curTex0;
                    const uint32_t x = static_cast<uint32_t>(lo & 0xFFFFu);
                    const uint32_t y = static_cast<uint32_t>((lo >> 32u) & 0xFFFFu);
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                };

                for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                {
                    uint64_t tagLo = 0u, tagHi = 0u;
                    std::memcpy(&tagLo, pkt + off, 8u);
                    std::memcpy(&tagHi, pkt + off + 8u, 8u);
                    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
                    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                    if (nreg == 0u) nreg = 16u;
                    if (((tagLo >> 46u) & 1ull) != 0ull)
                        curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);
                    ++tags;
                    uint32_t dataOff = off + 16u;

                    if (flg == 0u)
                    {
                        for (uint32_t lp = 0u; lp < nloop; ++lp)
                        {
                            for (uint32_t r = 0u; r < nreg; ++r)
                            {
                                if (dataOff + 16u > bytes) { ++invalid; goto g217_done; }
                                const uint8_t rr = static_cast<uint8_t>((tagHi >> ((r & 0xFu) * 4u)) & 0xFu);
                                uint64_t lo = 0u, hi = 0u;
                                std::memcpy(&lo, pkt + dataOff, 8u);
                                std::memcpy(&hi, pkt + dataOff + 8u, 8u);
                                if (rr == 0x0Eu)
                                {
                                    const uint32_t adReg = static_cast<uint32_t>(hi & 0xFFu);
                                    if (adReg == 0x00u) curPrim = static_cast<uint32_t>(lo & 7u);
                                    else if (adReg == 0x06u || adReg == 0x07u) bindTex0(lo);
                                }
                                else if (rr == 0x00u) curPrim = static_cast<uint32_t>(lo & 7u);
                                else if (rr == 0x06u || rr == 0x07u) bindTex0(lo);
                                else if (rr == 0x04u || rr == 0x05u || rr == 0x0Du) visitVertex(lo, hi);
                                dataOff += 16u;
                            }
                        }
                    }
                    else if (flg == 1u)
                    {
                        const uint32_t count = nloop * nreg;
                        for (uint32_t k = 0u; k < count; ++k)
                        {
                            if (dataOff + 8u > bytes) { ++invalid; goto g217_done; }
                            const uint8_t rr = static_cast<uint8_t>((tagHi >> (((k % nreg) & 0xFu) * 4u)) & 0xFu);
                            uint64_t value = 0u;
                            std::memcpy(&value, pkt + dataOff, 8u);
                            if (rr == 0x00u) curPrim = static_cast<uint32_t>(value & 7u);
                            else if (rr == 0x06u || rr == 0x07u) bindTex0(value);
                            dataOff += 8u;
                        }
                        if ((count & 1u) != 0u) dataOff += 8u;
                    }
                    else if (flg == 2u)
                    {
                        const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                        if (imageBytes > static_cast<uint64_t>(bytes - dataOff)) { ++invalid; goto g217_done; }
                        dataOff += static_cast<uint32_t>(imageBytes);
                    }
                    else
                    {
                        ++invalid;
                        goto g217_done;
                    }

                    if (dataOff <= off || dataOff > bytes) { ++invalid; break; }
                    off = dataOff;
                    if (((tagLo >> 15u) & 1ull) != 0ull)
                        break;
                }

            g217_done:
                s_g217LastTex0 = curTex0;
                if (s_g217TagVu && s_g217ExactKind == 0u)
                    return;
                if (!s_g217CharExec)
                    return;
                const uint32_t logIndex = s_g217Logged.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (s_g217Limit != 0u && logIndex > s_g217Limit)
                    return;
                const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
                const uint64_t effectiveTex0 = geomTex0 != 0u ? geomTex0 : curTex0;
                const uint32_t totalVerts = verts[3] + verts[4] + verts[5];
                const uint32_t bboxMinX = minX == 0xFFFFFFFFu ? 0u : minX;
                const uint32_t bboxMinY = minY == 0xFFFFFFFFu ? 0u : minY;
                std::fprintf(stderr,
                    "[G217:join] n=%u tick=%llu exec=%u kick=%u exact=%u pc=0x%x bytes=0x%x tags=%u invalid=%u "
                    "texWrites=%u texBefore=0x%llx texAfter=0x%llx geomTex=0x%llx tbp=0x%x psm=0x%x "
                    "verts=%u tri=%u/%u tstrip=%u/%u tfan=%u/%u bbox=%u,%u..%u,%u "
                    "hdr=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x\n",
                    logIndex, static_cast<unsigned long long>(tick), s_g217ExecSeq, ++s_g217KickSeq,
                    s_g217ExactKind, m_state.pc, bytes, tags, invalid, texWrites,
                    static_cast<unsigned long long>(tex0Before),
                    static_cast<unsigned long long>(curTex0),
                    static_cast<unsigned long long>(effectiveTex0),
                    static_cast<uint32_t>(effectiveTex0 & 0x3FFFu),
                    static_cast<uint32_t>((effectiveTex0 >> 20u) & 0x3Fu), totalVerts,
                    adcDraw[3], adcNoDraw[3], adcDraw[4], adcNoDraw[4], adcDraw[5], adcNoDraw[5],
                    bboxMinX, bboxMinY, maxX, maxY,
                    s_g217Header[0], s_g217Header[1], s_g217Header[2], s_g217Header[3],
                    s_g217Header[4], s_g217Header[5], s_g217Header[6], s_g217Header[7]);
            };

            auto g136DumpXgkick = [&](const uint8_t *pkt, uint32_t bytes)
            {
                if (!pkt || bytes < 16u || !g136_trace_enabled())
                    return;
                const uint32_t activeSeq = g_dc2G136TraceActive.load(std::memory_order_relaxed);
                if (activeSeq == 0u && !g136_trace_all_enabled())
                    return;
                const bool titleScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
                const uint32_t pcFilter = g136_xg_pc_filter();
                const bool interestingPc = (pcFilter != 0xFFFFFFFFu)
                    ? (m_state.pc == pcFilter)
                    : (m_state.pc == 0x1c30u || m_state.pc == 0x1fd0u ||
                       m_state.pc == 0x1da0u || m_state.pc == 0x2180u);
                // G216: the same decoder is useful for the character RTT even though the
                // historical G136 scope was title-only. Both paths remain diagnostic-only.
                if ((!titleScope && !s_g216CharExec) || !interestingPc)
                    return;

                const uint32_t limit = g136_xg_limit();
                const uint32_t logIndex = s_g136XgLogged.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (limit != 0u && logIndex > limit)
                    return;
                const uint32_t seq = (activeSeq != 0u) ? activeSeq : 0xFFFFFFFFu;

                uint64_t primLoops[8] = {};
                uint64_t adcDraw[8] = {};
                uint64_t adcNoDraw[8] = {};
                uint64_t tstripPosDraw[10] = {};
                uint64_t tstripPosNoDraw[10] = {};
                uint64_t tstripLen[17] = {};
                uint32_t tags = 0u;
                uint32_t invalid = 0u;
                uint32_t curPrim = 0u;
                uint32_t off = 0u;

                for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                {
                    uint64_t tagLo = 0u;
                    uint64_t tagHi = 0u;
                    std::memcpy(&tagLo, pkt + off, 8u);
                    std::memcpy(&tagHi, pkt + off + 8u, 8u);
                    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
                    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    const bool pre = ((tagLo >> 46u) & 1ull) != 0ull;
                    const bool eop = ((tagLo >> 15u) & 1ull) != 0ull;
                    if (pre)
                        curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);

                    uint32_t pktSize = 16u;
                    if (flg == 0u)
                        pktSize += nloop * nreg * 16u;
                    else if (flg == 1u)
                    {
                        const uint32_t regs = nloop * nreg;
                        pktSize += regs * 8u;
                        if ((regs & 1u) != 0u)
                            pktSize += 8u;
                    }
                    else if (flg == 2u)
                        pktSize += nloop * 16u;

                    if (pktSize == 0u || off + pktSize > bytes)
                    {
                        ++invalid;
                        break;
                    }

                    if (tags < 6u)
                    {
                        std::fprintf(stderr,
                                     "[G136:xgtag] seq=%u tag=%u off=0x%x nloop=%u flg=%u nreg=%u pre=%u prim=%u eop=%u regs=0x%016llx size=0x%x\n",
                                     seq, tags, off, nloop, flg, nreg, pre ? 1u : 0u,
                                     curPrim, eop ? 1u : 0u,
                                     static_cast<unsigned long long>(tagHi), pktSize);
                    }
                    ++tags;
                    if (curPrim < 8u)
                        primLoops[curPrim] += nloop;

                    if (flg == 0u)
                    {
                        uint8_t regs[16] = {};
                        for (uint32_t r = 0u; r < nreg && r < 16u; ++r)
                            regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                        if ((curPrim & 7u) == 4u)
                        {
                            const uint32_t lenBucket = (nloop < 16u) ? nloop : 16u;
                            tstripLen[lenBucket] += 1u;
                        }

                        uint32_t dataOff = off + 16u;
                        uint32_t vertexIndex = 0u;
                        for (uint32_t lp = 0u; lp < nloop; ++lp)
                        {
                            for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                            {
                                if (dataOff + 16u > off + pktSize)
                                    break;
                                const uint8_t rr = regs[r & 0xFu];
                                uint64_t lo = 0u;
                                uint64_t hi = 0u;
                                std::memcpy(&lo, pkt + dataOff, 8u);
                                std::memcpy(&hi, pkt + dataOff + 8u, 8u);
                                if (rr == 0x0Eu)
                                {
                                    const uint32_t adReg = static_cast<uint32_t>(hi & 0xFFu);
                                    if (adReg == 0u)
                                        curPrim = static_cast<uint32_t>(lo & 0x7u);
                                }
                                else if (rr == 0x00u)
                                {
                                    curPrim = static_cast<uint32_t>(lo & 0x7u);
                                }
                                else if (rr == 0x04u || rr == 0x05u)
                                {
                                    const uint32_t prim = curPrim & 7u;
                                    const bool adc = ((hi >> 47u) & 1ull) != 0ull;
                                    if (prim < 8u)
                                    {
                                        if (adc)
                                            ++adcNoDraw[prim];
                                        else
                                            ++adcDraw[prim];
                                    }
                                    if (prim == 4u)
                                    {
                                        const uint32_t pos = (vertexIndex < 9u) ? vertexIndex : 9u;
                                        if (adc)
                                            ++tstripPosNoDraw[pos];
                                        else
                                            ++tstripPosDraw[pos];
                                    }
                                    ++vertexIndex;
                                }
                            }
                        }
                    }

                    if (eop)
                        break;
                    off += pktSize;
                }

                const uint32_t vi2 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[2]));
                const uint32_t vi12 = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[12]));
                std::fprintf(stderr,
                             "[G136:xgsummary] seq=%u n=%u pc=0x%x startPC=0x%x addr=0x%x bytes=0x%x tags=%u invalid=%u vi2=0x%x vi12=0x%x vi1=0x%x vi7=0x%x vi8=0x%x vi10=0x%x itop=0x%x tex0=0x%llx tbp=0x%x texOff=0x%x primLoops tri=%llu tstrip=%llu tfan=%llu adcDraw/No tri=%llu/%llu tstrip=%llu/%llu tfan=%llu/%llu\n",
                             seq, logIndex, m_state.pc, s_g65CurStartPC, addr, bytes,
                             tags, invalid, vi2, vi12,
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[1])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[7])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[8])),
                             static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[10])),
                             m_state.itop,
                             static_cast<unsigned long long>(g96Tex0),
                             static_cast<uint32_t>(g96Tex0 & 0x3FFFu), g96Off,
                             static_cast<unsigned long long>(primLoops[3]),
                             static_cast<unsigned long long>(primLoops[4]),
                             static_cast<unsigned long long>(primLoops[5]),
                             static_cast<unsigned long long>(adcDraw[3]),
                             static_cast<unsigned long long>(adcNoDraw[3]),
                             static_cast<unsigned long long>(adcDraw[4]),
                             static_cast<unsigned long long>(adcNoDraw[4]),
                             static_cast<unsigned long long>(adcDraw[5]),
                             static_cast<unsigned long long>(adcNoDraw[5]));
                std::fprintf(stderr,
                             "[G136:xgstrip] seq=%u len3=%llu len4=%llu len5=%llu len6=%llu len7=%llu len8=%llu len9=%llu len10plus=%llu pos0=%llu/%llu pos1=%llu/%llu pos2=%llu/%llu pos3=%llu/%llu pos4=%llu/%llu pos5plus=%llu/%llu\n",
                             seq,
                             static_cast<unsigned long long>(tstripLen[3]),
                             static_cast<unsigned long long>(tstripLen[4]),
                             static_cast<unsigned long long>(tstripLen[5]),
                             static_cast<unsigned long long>(tstripLen[6]),
                             static_cast<unsigned long long>(tstripLen[7]),
                             static_cast<unsigned long long>(tstripLen[8]),
                             static_cast<unsigned long long>(tstripLen[9]),
                             static_cast<unsigned long long>(tstripLen[10] + tstripLen[11] + tstripLen[12] + tstripLen[13] + tstripLen[14] + tstripLen[15] + tstripLen[16]),
                             static_cast<unsigned long long>(tstripPosDraw[0]), static_cast<unsigned long long>(tstripPosNoDraw[0]),
                             static_cast<unsigned long long>(tstripPosDraw[1]), static_cast<unsigned long long>(tstripPosNoDraw[1]),
                             static_cast<unsigned long long>(tstripPosDraw[2]), static_cast<unsigned long long>(tstripPosNoDraw[2]),
                             static_cast<unsigned long long>(tstripPosDraw[3]), static_cast<unsigned long long>(tstripPosNoDraw[3]),
                             static_cast<unsigned long long>(tstripPosDraw[4]), static_cast<unsigned long long>(tstripPosNoDraw[4]),
                             static_cast<unsigned long long>(tstripPosDraw[5] + tstripPosDraw[6] + tstripPosDraw[7] + tstripPosDraw[8] + tstripPosDraw[9]),
                             static_cast<unsigned long long>(tstripPosNoDraw[5] + tstripPosNoDraw[6] + tstripPosNoDraw[7] + tstripPosNoDraw[8] + tstripPosNoDraw[9]));
                g136DumpVuQwords(seq, "VI2", vi2, g136_qword_limit());
                g136DumpVuQwords(seq, "VI12", vi12, g136_qword_limit());
                g136DumpVuQwords(seq, "QW59", 59u, 1u);
                g137DumpOrigin(seq, "VI2", vi2, g136_qword_limit());
                g137DumpOrigin(seq, "VI12", vi12, g136_qword_limit());
                g137DumpOrigin(seq, "QW59", 59u, 1u);
                g136DumpPacketQwords(seq, pkt, bytes);
            };

            auto g96Submit = [&](const uint8_t *pkt, uint32_t bytes)
            {
                extern std::atomic<bool> g_dc2TitleRockScope;
                // G138: publish the packer PC for the PATH1 record the GS-stream dump is
                // about to write (see g138DumpSubmit in ps2_memory.cpp).
                {
                    extern std::atomic<uint32_t> g_dc2G138XgPc;
                    g_dc2G138XgPc.store(m_state.pc, std::memory_order_relaxed);
                }
                static const bool s_g106pc2180Tfan = (std::getenv("DC2_G106_PC2180_TFAN") != nullptr);
                static const bool s_g106pc2180Stat = (std::getenv("DC2_G106_PC2180_STAT") != nullptr);
                static const bool s_g107StripAdcStat = (std::getenv("DC2_G107_STRIP_ADC_STAT") != nullptr);
                static const bool s_g108AdcReplayStat = (std::getenv("DC2_G108_ADC_REPLAY_STAT") != nullptr);
                static const bool s_g108AdcReplayMatchLen = (std::getenv("DC2_G108_ADC_REPLAY_MATCH_LEN") != nullptr);
                static const uint32_t s_g134CopyExtentSkipPx = []() -> uint32_t {
                    const char *e = std::getenv("DC2_G134_COPY_EXTENT_SKIP");
                    if (!e)
                        return 0u;
                    if (e[0] == '\0' || std::strcmp(e, "1") == 0)
                        return 1024u;
                    if (std::strcmp(e, "0") == 0 || std::strcmp(e, "false") == 0 ||
                        std::strcmp(e, "FALSE") == 0 || std::strcmp(e, "off") == 0 ||
                        std::strcmp(e, "OFF") == 0)
                        return 0u;
                    const unsigned long v = std::strtoul(e, nullptr, 0);
                    return (v > 0ul && v < 8192ul) ? static_cast<uint32_t>(v) : 1024u;
                }();
                static const bool s_g134CopyExtentStat =
                    (std::getenv("DC2_G134_COPY_EXTENT_STAT") != nullptr);
                const bool titleScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
                const std::vector<std::string> &g108Replay = g108_adc_replay_sequences();
                const uint32_t g107Tbp =
                    (g96Tex0 != 0u) ? static_cast<uint32_t>(g96Tex0 & 0x3FFFu) : 0xFFFFFFFFu;
                const bool g107TbpMatch =
                    s_g107StripAdcTarget == 0xFFFFFFFFu || g107Tbp == s_g107StripAdcTarget;
                std::vector<uint8_t> g106Patched;
                if ((s_g106pc2180Tfan || (s_g107StripAdcFirst != 0u && g107TbpMatch) || !g108Replay.empty()) &&
                    m_state.pc == 0x2180u &&
                    titleScope)
                {
                    g106Patched.assign(pkt, pkt + bytes);
                    uint32_t off = 0u;
                    uint32_t patchedTags = 0u;
                    uint32_t patchedAdcVerts = 0u;
                    uint32_t replayPackets = 0u;
                    uint32_t replayVerts = 0u;
                    uint32_t replaySet = 0u;
                    uint32_t replayClear = 0u;
                    uint32_t replayMismatch = 0u;
                    for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                    {
                        uint64_t tagLo = 0u;
                        uint64_t tagHi = 0u;
                        std::memcpy(&tagLo, g106Patched.data() + off, 8u);
                        std::memcpy(&tagHi, g106Patched.data() + off + 8u, 8u);
                        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
                        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                        if (nreg == 0u)
                            nreg = 16u;
                        const bool pre = ((tagLo >> 46u) & 0x1ull) != 0ull;
                        const bool eop = ((tagLo >> 15u) & 0x1ull) != 0ull;
                        const uint32_t originalPrimType =
                            pre ? static_cast<uint32_t>((tagLo >> 47u) & 0x7u) : 0xFFFFFFFFu;
                        uint32_t pktSize = 16u;
                        if (flg == 0u)
                            pktSize += nloop * nreg * 16u;
                        else if (flg == 1u)
                        {
                            const uint32_t regs = nloop * nreg;
                            pktSize += regs * 8u;
                            if ((regs & 1u) != 0u)
                                pktSize += 8u;
                        }
                        else if (flg == 2u)
                            pktSize += nloop * 16u;

                        if (s_g107StripAdcFirst != 0u &&
                            g107TbpMatch &&
                            originalPrimType == 4u &&
                            flg == 0u &&
                            off + pktSize <= bytes)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                            uint32_t dataOff = off + 16u;
                            uint32_t vertexIndex = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x04u || rr == 0x05u)
                                    {
                                        if (vertexIndex < s_g107StripAdcFirst)
                                        {
                                            uint64_t vhi = 0u;
                                            std::memcpy(&vhi, g106Patched.data() + dataOff + 8u, 8u);
                                            if (((vhi >> 47u) & 1ull) == 0ull)
                                                ++patchedAdcVerts;
                                            vhi |= (1ull << 47u);
                                            std::memcpy(g106Patched.data() + dataOff + 8u, &vhi, 8u);
                                        }
                                        ++vertexIndex;
                                    }
                                }
                            }
                        }
                        if (!g108Replay.empty() &&
                            originalPrimType == 4u &&
                            flg == 0u &&
                            off + pktSize <= bytes)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                            uint32_t packetVerts = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                                for (uint32_t r = 0u; r < nreg; ++r)
                                    if (regs[r] == 0x04u || regs[r] == 0x05u)
                                        ++packetVerts;
                            static std::atomic<uint64_t> s_g108ReplayCursor{0};
                            uint64_t seqNo = s_g108ReplayCursor.load(std::memory_order_relaxed);
                            uint64_t advance = 0u;
                            if (s_g108AdcReplayMatchLen)
                            {
                                for (; advance < g108Replay.size(); ++advance)
                                {
                                    const std::string &candidate =
                                        g108Replay[static_cast<size_t>((seqNo + advance) % g108Replay.size())];
                                    if (candidate.size() == packetVerts)
                                        break;
                                }
                                if (advance == g108Replay.size())
                                    advance = 0u;
                            }
                            const std::string &pat =
                                g108Replay[static_cast<size_t>((seqNo + advance) % g108Replay.size())];
                            s_g108ReplayCursor.store(seqNo + advance + 1u, std::memory_order_relaxed);
                            uint32_t dataOff = off + 16u;
                            uint32_t vertexIndex = 0u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x04u || rr == 0x05u)
                                    {
                                        uint64_t vhi = 0u;
                                        std::memcpy(&vhi, g106Patched.data() + dataOff + 8u, 8u);
                                        const bool wantAdc =
                                            vertexIndex < pat.size() && pat[vertexIndex] == '1';
                                        const bool oldAdc = ((vhi >> 47u) & 1ull) != 0ull;
                                        if (wantAdc)
                                        {
                                            vhi |= (1ull << 47u);
                                            if (!oldAdc)
                                                ++replaySet;
                                        }
                                        else
                                        {
                                            vhi &= ~(1ull << 47u);
                                            if (oldAdc)
                                                ++replayClear;
                                        }
                                        std::memcpy(g106Patched.data() + dataOff + 8u, &vhi, 8u);
                                        ++vertexIndex;
                                    }
                                }
                            }
                            if (vertexIndex != pat.size())
                                ++replayMismatch;
                            ++replayPackets;
                            replayVerts += vertexIndex;
                        }

                        if (pre)
                        {
                            uint32_t prim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);
                            if (s_g106pc2180Tfan && (prim & 0x7u) == 4u)
                            {
                                prim = (prim & ~0x7u) | 5u;
                                tagLo = (tagLo & ~(0x7FFull << 47u)) |
                                        (static_cast<uint64_t>(prim) << 47u);
                                std::memcpy(g106Patched.data() + off, &tagLo, 8u);
                                ++patchedTags;
                            }
                        }
                        if (pktSize == 0u || off + pktSize > bytes || eop)
                            break;
                        off += pktSize;
                    }
                    if (s_g106pc2180Stat)
                    {
                        static std::atomic<uint64_t> s_g106PatchCalls{0};
                        const uint64_t n = s_g106PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G106:pc2180] n=%llu bytes=0x%x patchedTags=%u\n",
                                         static_cast<unsigned long long>(n), bytes, patchedTags);
                    }
                    if (s_g107StripAdcStat && patchedAdcVerts != 0u)
                    {
                        static std::atomic<uint64_t> s_g107PatchCalls{0};
                        const uint64_t n = s_g107PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G107:stripadc] n=%llu bytes=0x%x tbp=0x%x first=%u patchedVerts=%u\n",
                                         static_cast<unsigned long long>(n), bytes,
                                         g107Tbp, s_g107StripAdcFirst, patchedAdcVerts);
                    }
                    if (s_g108AdcReplayStat && replayPackets != 0u)
                    {
                        static std::atomic<uint64_t> s_g108ReplayCalls{0};
                        const uint64_t n = s_g108ReplayCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G108:replay] n=%llu bytes=0x%x packets=%u verts=%u set=%u clear=%u mismatch=%u seqTotal=%zu\n",
                                         static_cast<unsigned long long>(n), bytes, replayPackets,
                                         replayVerts, replaySet, replayClear, replayMismatch,
                                         g108Replay.size());
                    }
                    pkt = g106Patched.data();
                }

                // G134: packet-level copy-packer extent restart diagnostic. Live HW confirmed the
                // four bit2=0 title descriptors still route through the copy path and their EE
                // vtable extension emits no geometry, so the remaining divergence is the copy
                // XGKICK packet itself. Default-off: set ADC on the vertex completing any title
                // copy tristrip triangle whose screen-space extent crosses the configured pixel
                // threshold (DC2_G134_COPY_EXTENT_SKIP, "1" = 1024px).
                std::vector<uint8_t> g134Patched;
                if (s_g134CopyExtentSkipPx != 0u &&
                    m_state.pc == 0x1c30u &&
                    titleScope)
                {
                    g134Patched.assign(pkt, pkt + bytes);
                    const uint32_t thresholdRaw = s_g134CopyExtentSkipPx * 16u;
                    uint32_t off = 0u;
                    uint32_t stripTags = 0u;
                    uint32_t triSeen = 0u;
                    uint32_t patchedVerts = 0u;
                    uint32_t existingAdc = 0u;
                    uint32_t invalidTags = 0u;
                    uint32_t curPrim = 0u;
                    for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                    {
                        uint64_t tagLo = 0u;
                        uint64_t tagHi = 0u;
                        std::memcpy(&tagLo, g134Patched.data() + off, 8u);
                        std::memcpy(&tagHi, g134Patched.data() + off + 8u, 8u);
                        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
                        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
                        if (nreg == 0u)
                            nreg = 16u;
                        const bool pre = ((tagLo >> 46u) & 0x1ull) != 0ull;
                        const bool eop = ((tagLo >> 15u) & 0x1ull) != 0ull;
                        if (pre)
                            curPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7u);
                        uint32_t pktSize = 16u;
                        if (flg == 0u)
                            pktSize += nloop * nreg * 16u;
                        else if (flg == 1u)
                        {
                            const uint32_t regs = nloop * nreg;
                            pktSize += regs * 8u;
                            if ((regs & 1u) != 0u)
                                pktSize += 8u;
                        }
                        else if (flg == 2u)
                            pktSize += nloop * 16u;

                        if (pktSize == 0u || off + pktSize > bytes)
                        {
                            ++invalidTags;
                            break;
                        }

                        if (flg == 0u)
                        {
                            uint8_t regs[16] = {};
                            for (uint32_t r = 0u; r < nreg; ++r)
                                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);

                            struct G134Vertex
                            {
                                uint32_t x;
                                uint32_t y;
                            };
                            G134Vertex prev2[2] = {};
                            uint32_t vertexIndex = 0u;
                            uint32_t activePrim = curPrim;
                            bool countedStripTag = false;
                            uint32_t dataOff = off + 16u;
                            for (uint32_t lp = 0u; lp < nloop; ++lp)
                            {
                                for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                {
                                    if (dataOff + 16u > off + pktSize)
                                        break;
                                    const uint8_t rr = regs[r];
                                    if (rr == 0x00u)
                                    {
                                        uint64_t primData = 0u;
                                        std::memcpy(&primData, g134Patched.data() + dataOff, 8u);
                                        activePrim = static_cast<uint32_t>(primData & 0x7u);
                                        curPrim = activePrim;
                                        prev2[0] = {};
                                        prev2[1] = {};
                                        vertexIndex = 0u;
                                        continue;
                                    }
                                    if (rr != 0x04u && rr != 0x05u)
                                        continue;
                                    if ((activePrim & 0x7u) != 4u)
                                        continue;
                                    if (!countedStripTag)
                                    {
                                        ++stripTags;
                                        countedStripTag = true;
                                    }

                                    uint64_t vlo = 0u;
                                    uint64_t vhi = 0u;
                                    std::memcpy(&vlo, g134Patched.data() + dataOff, 8u);
                                    std::memcpy(&vhi, g134Patched.data() + dataOff + 8u, 8u);
                                    const G134Vertex cur = {
                                        static_cast<uint32_t>(vlo & 0xFFFFu),
                                        static_cast<uint32_t>((vlo >> 32u) & 0xFFFFu)
                                    };

                                    if (vertexIndex >= 2u)
                                    {
                                        ++triSeen;
                                        uint32_t minX = prev2[0].x;
                                        uint32_t maxX = prev2[0].x;
                                        uint32_t minY = prev2[0].y;
                                        uint32_t maxY = prev2[0].y;
                                        if (prev2[1].x < minX) minX = prev2[1].x;
                                        if (cur.x < minX) minX = cur.x;
                                        if (prev2[1].x > maxX) maxX = prev2[1].x;
                                        if (cur.x > maxX) maxX = cur.x;
                                        if (prev2[1].y < minY) minY = prev2[1].y;
                                        if (cur.y < minY) minY = cur.y;
                                        if (prev2[1].y > maxY) maxY = prev2[1].y;
                                        if (cur.y > maxY) maxY = cur.y;
                                        const uint32_t extentRaw =
                                            ((maxX - minX) > (maxY - minY)) ? (maxX - minX) : (maxY - minY);
                                        if (extentRaw >= thresholdRaw)
                                        {
                                            const bool oldAdc = ((vhi >> 47u) & 1ull) != 0ull;
                                            if (oldAdc)
                                                ++existingAdc;
                                            else
                                                ++patchedVerts;
                                            vhi |= (1ull << 47u);
                                            std::memcpy(g134Patched.data() + dataOff + 8u, &vhi, 8u);
                                        }
                                    }
                                    prev2[0] = prev2[1];
                                    prev2[1] = cur;
                                    ++vertexIndex;
                                }
                            }
                        }

                        if (eop)
                            break;
                        off += pktSize;
                    }

                    if (s_g134CopyExtentStat || patchedVerts != 0u)
                    {
                        static std::atomic<uint64_t> s_g134PatchCalls{0};
                        static std::atomic<uint64_t> s_g134PatchVerts{0};
                        const uint64_t n = s_g134PatchCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint64_t totalPatch =
                            s_g134PatchVerts.fetch_add(patchedVerts, std::memory_order_relaxed) + patchedVerts;
                        if (n <= 96u || (n & 0x3FFull) == 0ull)
                            std::fprintf(stderr,
                                         "[G134:copyextskip] n=%llu bytes=0x%x threshPx=%u tags=%u tri=%u patched=%u existing=%u invalid=%u totalPatched=%llu\n",
                                         static_cast<unsigned long long>(n), bytes, s_g134CopyExtentSkipPx,
                                         stripTags, triSeen, patchedVerts, existingAdc, invalidTags,
                                         static_cast<unsigned long long>(totalPatch));
                    }
                    pkt = g134Patched.data();
                }

                if (s_g96bind && g96Tex0 != 0u)
                {
                    // Prepend GIFtag(NLOOP=1,EOP=0,NREG=1,REGS=A+D) + (TEX0 | reg 0x06) + original.
                    // EOP=0 so the GS continues into the original geometry tags after the bind.
                    std::vector<uint8_t> buf((size_t)bytes + 32u);
                    const uint64_t tagLo = 1ull | (1ull << 60); // nloop=1, nreg=1, eop=0
                    const uint64_t tagHi = 0x0Eull;             // packed A+D
                    const uint64_t adData = g96Tex0;
                    const uint64_t adAddr = 0x06ull;            // GS TEX0_1
                    std::memcpy(buf.data() + 0u, &tagLo, 8);
                    std::memcpy(buf.data() + 8u, &tagHi, 8);
                    std::memcpy(buf.data() + 16u, &adData, 8);
                    std::memcpy(buf.data() + 24u, &adAddr, 8);
                    std::memcpy(buf.data() + 32u, pkt, bytes);
                    if (memory)
                        memory->submitGifPacket(GifPathId::Path1, buf.data(), bytes + 32u, !g95_defer_gif_enabled());
                    else
                        gs.processGIFPacket(buf.data(), bytes + 32u);
                }
                else
                {
                    // G217 A/B: the missing MAP-4 FixMDT head/cap use the copy packer.  If a
                    // character-RTT packed tag marks every vertex ADC/no-draw, preserve the two
                    // strip-restart vertices and clear ADC on the remaining vertices.  This is
                    // diagnostic-only and deliberately broad within fbp=0x139; it determines
                    // whether the residual is output ADC versus projected position/submission.
                    static const bool s_g217ForceCopyDraw =
                        (std::getenv("DC2_G217_FORCE_COPY_DRAW") != nullptr);
                    std::vector<uint8_t> g217Patched;
                    if (s_g217ForceCopyDraw && s_g217CharExec && m_state.pc == 0x1c30u)
                    {
                        g217Patched.assign(pkt, pkt + bytes);
                        uint32_t off = 0u;
                        uint32_t changed = 0u;
                        for (uint32_t safety = 0u; safety < 256u && off + 16u <= bytes; ++safety)
                        {
                            uint64_t tagLo = 0u, tagHi = 0u;
                            std::memcpy(&tagLo, g217Patched.data() + off, 8u);
                            std::memcpy(&tagHi, g217Patched.data() + off + 8u, 8u);
                            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7fffu);
                            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 3u);
                            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xfu);
                            if (nreg == 0u) nreg = 16u;
                            uint32_t pktSize = 16u;
                            if (flg == 0u) pktSize += nloop * nreg * 16u;
                            else if (flg == 1u)
                            {
                                const uint32_t regs = nloop * nreg;
                                pktSize += regs * 8u + ((regs & 1u) ? 8u : 0u);
                            }
                            else if (flg == 2u) pktSize += nloop * 16u;
                            if (pktSize == 0u || off + pktSize > bytes) break;

                            if (flg == 0u)
                            {
                                uint32_t vertexCount = 0u;
                                uint32_t adcCount = 0u;
                                uint32_t dataOff = off + 16u;
                                for (uint32_t lp = 0u; lp < nloop; ++lp)
                                {
                                    for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                    {
                                        const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xfu);
                                        if (reg != 0x04u && reg != 0x05u && reg != 0x0du) continue;
                                        uint64_t hi = 0u;
                                        std::memcpy(&hi, g217Patched.data() + dataOff + 8u, 8u);
                                        ++vertexCount;
                                        if (((hi >> 47u) & 1ull) != 0ull) ++adcCount;
                                    }
                                }
                                if (vertexCount >= 3u && adcCount == vertexCount)
                                {
                                    dataOff = off + 16u;
                                    uint32_t vertexIndex = 0u;
                                    for (uint32_t lp = 0u; lp < nloop; ++lp)
                                    {
                                        for (uint32_t r = 0u; r < nreg; ++r, dataOff += 16u)
                                        {
                                            const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xfu);
                                            if (reg != 0x04u && reg != 0x05u && reg != 0x0du) continue;
                                            if (vertexIndex++ < 2u) continue;
                                            uint64_t hi = 0u;
                                            std::memcpy(&hi, g217Patched.data() + dataOff + 8u, 8u);
                                            hi &= ~(1ull << 47u);
                                            std::memcpy(g217Patched.data() + dataOff + 8u, &hi, 8u);
                                            ++changed;
                                        }
                                    }
                                }
                            }
                            if (((tagLo >> 15u) & 1ull) != 0ull) break;
                            off += pktSize;
                        }
                        if (changed != 0u)
                        {
                            static std::atomic<uint32_t> s_g217ForceLogs{0};
                            const uint32_t n = s_g217ForceLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (n <= 128u)
                                std::fprintf(stderr, "[G217:forcecopy] n=%u pc=0x%x changed=%u bytes=0x%x\n",
                                             n, m_state.pc, changed, bytes);
                            pkt = g217Patched.data();
                        }
                    }
                    if (memory)
                        memory->submitGifPacket(GifPathId::Path1, pkt, bytes, !g95_defer_gif_enabled());
                    else
                        gs.processGIFPacket(pkt, bytes);
                }
            };

            if (s_g215batch || s_g216clip || s_g216ForceGate || s_g217TagVu)
            {
                ++s_g215KickCount;
                s_g215KickBytes += totalBytes;
            }
            if (addr + totalBytes <= dataSize)
            {
                g217JoinXgkick(vuData + addr, totalBytes);
                g136DumpXgkick(vuData + addr, totalBytes);
                f50_12_vu_scan_xgkick_packet(addr, totalBytes, vuData + addr);
                g67_vu_scan_xgkick_packet(addr, totalBytes, vuData + addr, m_state.pc, s_g65CurStartPC, m_state.itop);
                vu1_dump_xgkick(addr, totalBytes, vuData + addr);
                g96Submit(vuData + addr, totalBytes);
            }
            else
            {
                std::vector<uint8_t> wrappedPacket(totalBytes);
                for (uint32_t i = 0; i < totalBytes; ++i)
                {
                    wrappedPacket[i] = vuData[wrapOffset(addr + i)];
                }

                g217JoinXgkick(wrappedPacket.data(), totalBytes);
                g136DumpXgkick(wrappedPacket.data(), totalBytes);
                f50_12_vu_scan_xgkick_packet(addr, totalBytes, wrappedPacket.data());
                g67_vu_scan_xgkick_packet(addr, totalBytes, wrappedPacket.data(), m_state.pc, s_g65CurStartPC, m_state.itop);
                vu1_dump_xgkick(addr, totalBytes, wrappedPacket.data());
                g96Submit(wrappedPacket.data(), totalBytes);
            }
            return;
        }
        case 0x7E: // legacy-unreachable XTOP - DATA top (VIF1 TOPS), not ITOP
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(memory ? (memory->vif1_regs.top & 0x3FFu)
                                                  : (m_state.itop & 0x3FFu));
            return;
        }
        case 0x7F: // legacy-unreachable XITOP - INTEGER top
        {
            if (it != 0)
                m_state.vi[it] = (int32_t)(m_state.itop & 0x3FFu);
            return;
        }
        default:
            g22_log_unknown_lower(m_state.pc, instr, "lower-special", opHi, funct);
            return;
        }
    }
    default:
        g22_log_unknown_lower(m_state.pc, instr, "primary", opHi, 0u);
        break;
    }
}
