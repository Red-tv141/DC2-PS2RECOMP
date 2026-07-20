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

// G297 MTVU: when non-null (set on the VU1 worker thread before m_vu1.execute/resume), each XGKICK
// Path1 packet is captured into this per-kick list instead of being submitted to the GifArbiter.
// The EE thread later splices the list into the arbiter in kick-order (GifArbiter::submit is
// single-writer / EE-only). Null on the EE thread → the legacy synchronous submit path is used, so
// the default (MTVU-off) build is byte-identical. thread_local: only the worker sees it set.
thread_local std::vector<std::vector<uint8_t>> *g_g297KickCapture = nullptr;
// G297: the double-buffer DATA top (VIF1 TOPS latched at the kick's MSCAL) snapshotted on the EE at
// enqueue time. The interpreter reads XTOP live from memory->vif1_regs.top, which the EE mutates at
// the NEXT MSCAL — on the worker that would read the wrong buffer half. When g_g297KickCapture is
// set (worker context) the XTOP read uses this captured value instead. Masked (& 0x3FF) at capture.
thread_local uint32_t g_g297KickTop = 0u;

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
    // G239: VU1 P-register/EFU pipeline. EFU results are not architecturally visible until
    // their instruction-specific latency expires. A second EFU instruction (or WAITP) stalls
    // the unit and publishes the older pending result before continuing; MFP itself does not
    // stall and therefore reads the previous P while a newer result is in flight. DC2's dungeon
    // lighting loop deliberately issues ERSQRT at the unit's throughput boundary, then executes
    // MFP one pair later. This pipeline is required for the architectural P value to match VU1;
    // DC2_VU1_NO_PPIPE restores the old immediate-result model for focused A/B verification.
    bool g239_p_pipeline_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_PPIPE") == nullptr);
        return v;
    }
    thread_local float s_vuPPending = 0.0f;
    thread_local int   s_vuPDelay   = 0;

    void g239_stage_p(float &architecturalP, float result, int latency)
    {
        if (!g239_p_pipeline_enabled())
        {
            architecturalP = result;
            s_vuPDelay = 0;
            return;
        }

        // PCSX2's _vuTestEFUStalls releases the busy stall one cycle before the nominal
        // latency, then flushes the old pipe entry before _vuEFUAdd replaces it. For value
        // semantics in this single-pair interpreter, that is exactly an old-P commit here.
        if (s_vuPDelay > 0)
            architecturalP = s_vuPPending;
        s_vuPPending = result;
        s_vuPDelay = latency;
    }

    void g239_wait_p(float &architecturalP)
    {
        if (g239_p_pipeline_enabled() && s_vuPDelay > 0)
        {
            architecturalP = s_vuPPending;
            s_vuPDelay = 0;
        }
    }
    // G239: lower FDIV/EFU waits and busy producers are tested before the parallel upper slot
    // executes on VU1. This matters for pairs such as `MULq ... | WAITQ`: WAITQ publishes the
    // pending Q first, then MULq consumes it. The old sequential upper-then-lower interpreter
    // multiplied by the stale Q and could turn a normalized lighting basis into ~FLT_MAX.
    // Kill switch retained solely for the matched route A/B.
    bool g239_scalar_prestall_enabled()
    {
        static const bool v = (std::getenv("DC2_VU1_NO_SCALAR_PRESTALL") == nullptr);
        return v;
    }
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

