#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"
#include "ps2_log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// G297 MTVU bridges (defined in ps2_runtime.cpp). No-ops unless DC2_G297_MTVU=1.
extern bool g297HasPendingKicks();
extern void g297OnWindowHandoff();
extern void g297CollectWindowPackets(std::vector<std::vector<uint8_t>> &out);
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// G156: worker thread CPU-time (user+kernel) so we can compare it against wall-time and prove
// whether the MTGS worker's ~130 ms/frame "busy" is real CPU work or scheduler/HT stall.
static uint64_t g156ThreadCpuNs()
{
    FILETIME cr, ex, kt, ut;
    if (!GetThreadTimes(GetCurrentThread(), &cr, &ex, &kt, &ut))
        return 0ull;
    const uint64_t k = ((uint64_t)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
    const uint64_t u = ((uint64_t)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
    return (k + u) * 100ull; // FILETIME is 100 ns units → ns
}
#else
static uint64_t g156ThreadCpuNs() { return 0ull; }
#endif

namespace
{
    std::atomic<uint32_t> s_debugGifArbiterSubmitCount{0};
    std::atomic<uint32_t> s_debugGifArbiterDrainCount{0};
    uint32_t s_path3PendingImageQwc = 0u;

    const char *pathName(GifPathId id)
    {
        switch (id)
        {
        case GifPathId::Path1:
            return "path1";
        case GifPathId::Path2:
            return "path2";
        case GifPathId::Path3:
            return "path3";
        default:
            return "path?";
        }
    }

    bool f50_12_env_flag(const char *name)
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

    bool f50_12_env_disabled(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr &&
               (value[0] == '\0' ||
                std::strcmp(value, "0") == 0 ||
                std::strcmp(value, "false") == 0 ||
                std::strcmp(value, "FALSE") == 0 ||
                std::strcmp(value, "off") == 0 ||
                std::strcmp(value, "OFF") == 0);
    }

    bool f50_12_trace_enabled()
    {
        static const bool enabled = f50_12_env_flag("DC2_TRACE_F50_12");
        return enabled;
    }

    uint64_t f50_12_load64(const uint8_t *data)
    {
        uint64_t value = 0u;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    bool f50_12_is_paletted_psm(uint32_t psm)
    {
        return psm == GS_PSM_T4 ||
               psm == GS_PSM_T8 ||
               psm == GS_PSM_T4HL ||
               psm == GS_PSM_T4HH;
    }

    void f50_12_dump_ad_packet(const GifArbiterPacket &pkt, uint32_t drainIndex, const char *reason)
    {
        static std::atomic<uint32_t> s_f512GifDumpCount{0};
        const uint32_t dump = s_f512GifDumpCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (dump > 4u || pkt.data.size() < 16u)
            return;

        const uint8_t *data = pkt.data.data();
        const uint32_t sizeBytes = static_cast<uint32_t>(pkt.data.size());
        uint32_t offset = 0u;
        uint32_t item = 0u;
        while (offset + 16u <= sizeBytes && item < 48u)
        {
            const uint32_t tagOffset = offset;
            const uint64_t tagLo = f50_12_load64(data + offset);
            const uint64_t tagHi = f50_12_load64(data + offset + 8u);
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i)
                regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            std::fprintf(stderr,
                         "[F512:gifdump] dump=%u drain=%u path=%s reason=%s tagOff=0x%x tagLo=0x%016llx tagHi=0x%016llx flg=%u nloop=%u nreg=%u size=%u\n",
                         dump, drainIndex, pathName(pkt.pathId), reason, tagOffset,
                         static_cast<unsigned long long>(tagLo),
                         static_cast<unsigned long long>(tagHi),
                         static_cast<uint32_t>(flg), nloop, nreg, sizeBytes);

            if (flg == GIF_FMT_PACKED)
            {
                for (uint32_t loop = 0u; loop < nloop && item < 48u; ++loop)
                {
                    for (uint32_t r = 0u; r < nreg && item < 48u; ++r)
                    {
                        if (offset + 16u > sizeBytes)
                            return;
                        const uint64_t lo = f50_12_load64(data + offset);
                        const uint64_t hi = f50_12_load64(data + offset + 8u);
                        offset += 16u;
                        const uint8_t reg = (regs[r] == 0x0Eu) ? static_cast<uint8_t>(hi & 0xFFu) : regs[r];
                        std::fprintf(stderr,
                                     "[F512:gifdump] dump=%u item=%u loop=%u slot=%u desc=0x%02x reg=0x%02x lo=0x%016llx hi=0x%016llx\n",
                                     dump, item, loop, r, static_cast<uint32_t>(regs[r]), static_cast<uint32_t>(reg),
                                     static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
                        ++item;
                    }
                }
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                const uint32_t values = nloop * nreg;
                const uint32_t bytes = values * 8u + ((values & 1u) ? 8u : 0u);
                if (bytes > sizeBytes - offset)
                    return;
                offset += bytes;
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                if (imageBytes > static_cast<uint64_t>(sizeBytes - offset))
                    return;
                offset += static_cast<uint32_t>(imageBytes);
            }
            else
            {
                return;
            }
        }
    }

    void f50_12_log_tex0(const GifArbiterPacket &pkt,
                         uint32_t drainIndex,
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
        const bool paletted = f50_12_is_paletted_psm(psm);
        const bool hit = tbp == 0x2580u || cbp == 0x2980u || cpsm == GS_PSM_CT16;
        const bool title4hh = tbp == 0x1A00u && psm == GS_PSM_T4HH && cbp == 0x3FE0u;
        if (!paletted && !hit)
            return;
        if (hit)
            f50_12_dump_ad_packet(pkt, drainIndex, "tex0-hit");

        static std::atomic<uint32_t> s_f512GifTex0Count{0};
        static std::atomic<uint32_t> s_f512GifTex0TitleCount{0};
        static std::atomic<uint32_t> s_f512GifTex0NonTitleCount{0};
        static std::atomic<uint32_t> s_f512GifTex0HitCount{0};
        const uint32_t n = s_f512GifTex0Count.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t bucket = title4hh
                                    ? (s_f512GifTex0TitleCount.fetch_add(1u, std::memory_order_relaxed) + 1u)
                                    : (s_f512GifTex0NonTitleCount.fetch_add(1u, std::memory_order_relaxed) + 1u);
        const uint32_t hitn = hit ? (s_f512GifTex0HitCount.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
        const bool shouldLog = title4hh
                                   ? (bucket <= 12u || (bucket % 2048u) == 0u)
                                   : (bucket <= 512u ||
                                      (hit && (hitn <= 256u || (hitn % 512u) == 0u)) ||
                                      ((bucket % 2048u) == 0u));
        if (!shouldLog)
            return;

        std::fprintf(stderr,
                     "[F512:giftex0] n=%u bucket=%u title=%u hit=%u drain=%u path=%s source=%s reg=0x%02x raw=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u size=%u tag=%u tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                     n, bucket, static_cast<uint32_t>(title4hh ? 1u : 0u), hitn,
                     drainIndex, pathName(pkt.pathId), source,
                     static_cast<uint32_t>(regAddr), static_cast<unsigned long long>(value),
                     tbp, tbw, psm, tw, th, cbp, cpsm, csm, csa, cld,
                     static_cast<uint32_t>(pkt.data.size()), tagIndex, tagOffset,
                     loop, regSlot, static_cast<uint32_t>(flg), nloop, nreg,
                     static_cast<uint32_t>(pkt.path2DirectHl ? 1u : 0u),
                     static_cast<uint32_t>(pkt.path3Image ? 1u : 0u));
    }

    void f50_12_log_tex2(const GifArbiterPacket &pkt,
                         uint32_t drainIndex,
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
        const uint32_t psm = static_cast<uint32_t>((value >> 20u) & 0x3Fu);
        const uint32_t cbp = static_cast<uint32_t>((value >> 37u) & 0x3FFFu);
        const uint32_t cpsm = static_cast<uint32_t>((value >> 51u) & 0xFu);
        const uint32_t csm = static_cast<uint32_t>((value >> 55u) & 0x1u);
        const uint32_t csa = static_cast<uint32_t>((value >> 56u) & 0x1Fu);
        const uint32_t cld = static_cast<uint32_t>((value >> 61u) & 0x7u);
        const bool interesting = f50_12_is_paletted_psm(psm) || cbp == 0x2980u || cpsm == GS_PSM_CT16;
        if (!interesting)
            return;

        static std::atomic<uint32_t> s_f512GifTex2Count{0};
        const uint32_t n = s_f512GifTex2Count.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n > 128u && (n % 1024u) != 0u)
            return;

        std::fprintf(stderr,
                     "[F512:giftex2] n=%u drain=%u path=%s source=%s reg=0x%02x raw=0x%016llx psm=0x%x cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u size=%u tag=%u tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                     n, drainIndex, pathName(pkt.pathId), source,
                     static_cast<uint32_t>(regAddr), static_cast<unsigned long long>(value),
                     psm, cbp, cpsm, csm, csa, cld,
                     static_cast<uint32_t>(pkt.data.size()), tagIndex, tagOffset,
                     loop, regSlot, static_cast<uint32_t>(flg), nloop, nreg,
                     static_cast<uint32_t>(pkt.path2DirectHl ? 1u : 0u),
                     static_cast<uint32_t>(pkt.path3Image ? 1u : 0u));
    }

    void f50_12_log_bitbltbuf(const GifArbiterPacket &pkt,
                              uint32_t drainIndex,
                              const char *source,
                              uint64_t value,
                              uint32_t tagIndex,
                              uint32_t tagOffset,
                              uint32_t nloop,
                              uint8_t flg,
                              uint32_t nreg,
                              uint32_t loop,
                              uint32_t regSlot)
    {
        const uint32_t sbp = static_cast<uint32_t>(value & 0x3FFFu);
        const uint32_t sbw = static_cast<uint32_t>((value >> 16u) & 0x3Fu);
        const uint32_t spsm = static_cast<uint32_t>((value >> 24u) & 0x3Fu);
        const uint32_t dbp = static_cast<uint32_t>((value >> 32u) & 0x3FFFu);
        const uint32_t dbw = static_cast<uint32_t>((value >> 48u) & 0x3Fu);
        const uint32_t dpsm = static_cast<uint32_t>((value >> 56u) & 0x3Fu);
        const bool hit = dbp == 0x2580u || dbp == 0x2980u || dpsm == GS_PSM_CT16;

        static std::atomic<uint32_t> s_f512GifBitbltCount{0};
        static std::atomic<uint32_t> s_f512GifBitbltHitCount{0};
        const uint32_t n = s_f512GifBitbltCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t hitn = hit ? (s_f512GifBitbltHitCount.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
        if (n > 96u && !hit && (n % 512u) != 0u)
            return;

        std::fprintf(stderr,
                     "[F512:gifbitblt] n=%u hit=%u drain=%u path=%s source=%s raw=0x%016llx sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x size=%u tag=%u tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                     n, hitn, drainIndex, pathName(pkt.pathId), source,
                     static_cast<unsigned long long>(value),
                     sbp, sbw, spsm, dbp, dbw, dpsm,
                     static_cast<uint32_t>(pkt.data.size()), tagIndex, tagOffset,
                     loop, regSlot, static_cast<uint32_t>(flg), nloop, nreg,
                     static_cast<uint32_t>(pkt.path2DirectHl ? 1u : 0u),
                     static_cast<uint32_t>(pkt.path3Image ? 1u : 0u));
    }

    void f50_12_scan_register(const GifArbiterPacket &pkt,
                              uint32_t drainIndex,
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
        {
            f50_12_log_tex0(pkt, drainIndex, source, regAddr, value, tagIndex, tagOffset, nloop, flg, nreg, loop, regSlot);
        }
        else if (regAddr == GS_REG_TEX2_1 || regAddr == GS_REG_TEX2_2)
        {
            f50_12_log_tex2(pkt, drainIndex, source, regAddr, value, tagIndex, tagOffset, nloop, flg, nreg, loop, regSlot);
        }
        else if (regAddr == GS_REG_BITBLTBUF)
        {
            f50_12_log_bitbltbuf(pkt, drainIndex, source, value, tagIndex, tagOffset, nloop, flg, nreg, loop, regSlot);
        }
    }

    void f50_12_scan_gif_packet(const GifArbiterPacket &pkt, uint32_t drainIndex)
    {
        if (!f50_12_trace_enabled() || pkt.data.empty())
            return;

        const uint8_t *data = pkt.data.data();
        const uint32_t sizeBytes = static_cast<uint32_t>(pkt.data.size());
        uint32_t offset = 0u;
        uint32_t tagIndex = 0u;
        while (offset + 16u <= sizeBytes && tagIndex < 256u)
        {
            const uint32_t tagOffset = offset;
            const uint64_t tagLo = f50_12_load64(data + offset);
            const uint64_t tagHi = f50_12_load64(data + offset + 8u);
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
                        if (offset + 16u > sizeBytes)
                            return;
                        const uint64_t lo = f50_12_load64(data + offset);
                        const uint64_t hi = f50_12_load64(data + offset + 8u);
                        offset += 16u;
                        if (regs[r] == 0x0Eu)
                        {
                            f50_12_scan_register(pkt, drainIndex, "packed-ad", static_cast<uint8_t>(hi & 0xFFu),
                                                 lo, tagIndex, tagOffset, nloop, flg, nreg, loop, r);
                        }
                        else
                        {
                            f50_12_scan_register(pkt, drainIndex, "packed-direct", regs[r],
                                                 lo, tagIndex, tagOffset, nloop, flg, nreg, loop, r);
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
                        if (offset + 8u > sizeBytes)
                            return;
                        const uint64_t value = f50_12_load64(data + offset);
                        offset += 8u;
                        f50_12_scan_register(pkt, drainIndex, "reglist", regs[r],
                                             value, tagIndex, tagOffset, nloop, flg, nreg, loop, r);
                    }
                }
                if (((nloop * nreg) & 1u) != 0u)
                {
                    if (offset + 8u > sizeBytes)
                        return;
                    offset += 8u;
                }
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                const uint64_t imageBytes = static_cast<uint64_t>(nloop) * 16ull;
                if (imageBytes > static_cast<uint64_t>(sizeBytes - offset))
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
}

// ===== G150 MTGS (multi-threaded GS) — DEFAULT OFF (DC2_G150_MTGS=1) =====
#include "ps2_gif_arbiter_parts/g317_path2_fence_census.inc"

// Architectural perf lever (user chose "big lever", 2026-07-07): move the whole GS-drain
// (processGIFPacket → register writes + drawPrimitive raster + processImageData) off the guest
// EE thread onto a dedicated GS worker thread, so the EE thread (VIF unpack + VU1 interp + packet
// copy) overlaps the GS work of the previous frame. Canonical PCSX2-style MTGS.
//
// Bit-exact by construction in the default (golden) config: DC2_G95_DEFER_GIF is OFF, so every
// submitGifPacket drains immediately (queue of 1) → each drain() window is one packet and the
// arbiter's path sort never reorders. The worker processes windows STRICTLY FIFO (== EE submission
// order) and is the SOLE writer of GS VRAM/state, so the VRAM mutation sequence is byte-identical to
// the synchronous path — just on another thread. The present thread reads only the latch snapshot
// (m_stateMutex-guarded, produced by this worker at the frame barrier), so it never races a partial
// frame. Overlap is bounded to ~1 frame by the frame-barrier backpressure (no unbounded run-ahead,
// no memory blowup, ≤1 frame added input latency — the named trade-off).
namespace
{
    // G151 (diagnostic, DEFAULT OFF, DC2_G151_STAT=1): measure the MTGS worker's per-frame busy
    // time so the compounding ceiling (MTGS+G144) is grounded in the actual worker breakdown rather
    // than inference. Accumulated in the worker loop, printed from frameDrain() (the per-frame EE hook).
    std::atomic<uint64_t> g_g151WorkerBusyNs{0};
    std::atomic<uint64_t> g_g151WorkerCpuNs{0}; // G156: worker CPU time (user+kernel) over the window
    std::atomic<uint64_t> g_g151WindowCount{0};
    std::atomic<uint64_t> g_g151PktCount{0};

    // G189 (diagnostic watchdog, DEFAULT OFF, DC2_G189_WATCHDOG=1): a real, reproducible hang
    // under DC2_G157_PIPELINE=1 was found on the recorded town->inventory route (present-thread
    // tick count freezes permanently a few seconds after the inventory circular-map screen
    // opens; matches the user's live RTSS "drops to ~2Hz" report). fprintf-based tracing of the
    // frame-boundary closure PERTURBED the timing enough to avoid the hang (3/3 no-trace repro
    // vs 0/1 with fprintf tracing) -- so this uses only relaxed atomic stores (no I/O) on every
    // hot-path breadcrumb; a dedicated watchdog thread does the one-shot diagnostic print only
    // after the present thread has visibly stopped advancing. See plans/phase-G189-fix-log.md.
    std::atomic<int> g_g189WorkerStage{0};   // 0 idle/waiting-for-item, 1 popped-window, 2 running-window,
                                              // 3 popped-boundary/enter-flush, 4 enter-latch, 5 boundary-done, 6 apply
    std::atomic<uint64_t> g_g189WorkerN{0};  // last mgEndFrame n whose closure the worker is/was running
    std::atomic<int> g_g189EEStage{0};       // 0 running, 1 wait-enqueueWindow-space, 2 wait-frameBoundary-credit,
                                              // 3 wait-registerSlot

    // G184 (diagnostic, DEFAULT OFF, DC2_G184_BOUNDARY_STAT=1): under DC2_G157_PIPELINE=1, the
    // frame-boundary closure (G144 trailing flush + GS::latchHostPresentationFrame, which under
    // DC2_G178_GPU=1 includes the GPU-thread's readback-to-VRAM) runs HERE, on the worker thread,
    // via the isFrameBoundary branch below -- unlike the normal packet-processing branch (which
    // already has the G151 busy/cpu timers above), this closure has NO wall-time instrumentation
    // at all today. G182 found EE 92-93% inside mgEndFrame but could not see this piece (it never
    // runs on the EE thread when pipelined); this measures it directly instead of guessing.
    std::atomic<uint64_t> g_g184BoundaryNs{0};
    std::atomic<uint64_t> g_g184BoundaryCount{0};

    // G316 (diagnosis only, default off): current-binary, mutually-exclusive GS-worker census.
    // G312 proved that per-draw timing can manufacture a pole. G316 therefore measures root work
    // at worker-window/boundary/apply granularity and samples path groups for one frame in 30.
    // The sampled leaf sum is exact: MTVU merge + stable sort + three path runs + residual loop work.
    // DC2_G316_EMPTY=1 runs identical sampling timestamps without leaf atomic updates.
    enum G316PathIndex : uint32_t
    {
        kG316Path1 = 0u,
        kG316Path2 = 1u,
        kG316Path3 = 2u,
    };

    std::atomic<uint64_t> g_g316WindowNs{0};
    std::atomic<uint64_t> g_g316BoundaryNs{0};
    std::atomic<uint64_t> g_g316ApplyNs{0};
    std::atomic<uint64_t> g_g316MergeNs{0};
    std::atomic<uint64_t> g_g316SortNs{0};
    std::atomic<uint64_t> g_g316PathNs[3]{};
    std::atomic<uint64_t> g_g316Windows{0};
    std::atomic<uint64_t> g_g316Boundaries{0};
    std::atomic<uint64_t> g_g316Applies{0};
    std::atomic<uint64_t> g_g316PathPackets[3]{};
    // Leaf timing is armed for one full worker frame every 30 boundaries. This keeps the
    // diagnostic below G312's per-packet self-cost threshold while retaining a real path split.
    std::atomic<uint64_t> g_g316SampleWindowNs{0};
    std::atomic<uint64_t> g_g316SampleBoundaryNs{0};
    std::atomic<uint64_t> g_g316SampleApplyNs{0};
    std::atomic<uint64_t> g_g316SampledFrames{0};

    bool g316TimingOn()
    {
        static const bool s_on = f50_12_env_flag("DC2_G316_CENSUS") ||
                                 f50_12_env_flag("DC2_G316_EMPTY");
        return s_on;
    }

    bool g316RecordOn()
    {
        static const bool s_on = f50_12_env_flag("DC2_G316_CENSUS");
        return s_on;
    }

    static uint64_t g316ElapsedNs(std::chrono::steady_clock::time_point t0)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    static void g316ReportBoundary()
    {
        if (!g316TimingOn())
            return;

        const uint64_t frame = g_g316Boundaries.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((frame % 60u) != 0u)
            return;

        static uint64_t s_lastWindowNs = 0u, s_lastBoundaryNs = 0u, s_lastApplyNs = 0u;
        static uint64_t s_lastMergeNs = 0u, s_lastSortNs = 0u;
        static uint64_t s_lastSampleWindowNs = 0u, s_lastSampleBoundaryNs = 0u,
                        s_lastSampleApplyNs = 0u, s_lastSampledFrames = 0u;
        static uint64_t s_lastPathNs[3]{};
        static uint64_t s_lastWindows = 0u, s_lastApplies = 0u, s_lastPathPackets[3]{};

        const uint64_t windowNs = g_g316WindowNs.load(std::memory_order_relaxed);
        const uint64_t boundaryNs = g_g316BoundaryNs.load(std::memory_order_relaxed);
        const uint64_t applyNs = g_g316ApplyNs.load(std::memory_order_relaxed);
        const uint64_t mergeNs = g_g316MergeNs.load(std::memory_order_relaxed);
        const uint64_t sortNs = g_g316SortNs.load(std::memory_order_relaxed);
        const uint64_t windows = g_g316Windows.load(std::memory_order_relaxed);
        const uint64_t applies = g_g316Applies.load(std::memory_order_relaxed);
        const uint64_t sampleWindowNs = g_g316SampleWindowNs.load(std::memory_order_relaxed);
        const uint64_t sampleBoundaryNs = g_g316SampleBoundaryNs.load(std::memory_order_relaxed);
        const uint64_t sampleApplyNs = g_g316SampleApplyNs.load(std::memory_order_relaxed);
        const uint64_t sampledFrames = g_g316SampledFrames.load(std::memory_order_relaxed);

        uint64_t pathNs[3]{};
        uint64_t pathPackets[3]{};
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            pathNs[i] = g_g316PathNs[i].load(std::memory_order_relaxed);
            pathPackets[i] = g_g316PathPackets[i].load(std::memory_order_relaxed);
        }

        const uint64_t dWindowNs = windowNs - s_lastWindowNs;
        const uint64_t dBoundaryNs = boundaryNs - s_lastBoundaryNs;
        const uint64_t dApplyNs = applyNs - s_lastApplyNs;
        const uint64_t dMergeNs = mergeNs - s_lastMergeNs;
        const uint64_t dSortNs = sortNs - s_lastSortNs;
        const uint64_t dWindows = windows - s_lastWindows;
        const uint64_t dApplies = applies - s_lastApplies;
        const uint64_t dSampleWindowNs = sampleWindowNs - s_lastSampleWindowNs;
        const uint64_t dSampleBoundaryNs = sampleBoundaryNs - s_lastSampleBoundaryNs;
        const uint64_t dSampleApplyNs = sampleApplyNs - s_lastSampleApplyNs;
        const uint64_t dSampledFrames = sampledFrames - s_lastSampledFrames;
        uint64_t dPathNs[3]{};
        uint64_t dPathPackets[3]{};
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            dPathNs[i] = pathNs[i] - s_lastPathNs[i];
            dPathPackets[i] = pathPackets[i] - s_lastPathPackets[i];
            s_lastPathNs[i] = pathNs[i];
            s_lastPathPackets[i] = pathPackets[i];
        }

        s_lastWindowNs = windowNs;
        s_lastBoundaryNs = boundaryNs;
        s_lastApplyNs = applyNs;
        s_lastMergeNs = mergeNs;
        s_lastSortNs = sortNs;
        s_lastWindows = windows;
        s_lastApplies = applies;
        s_lastSampleWindowNs = sampleWindowNs;
        s_lastSampleBoundaryNs = sampleBoundaryNs;
        s_lastSampleApplyNs = sampleApplyNs;
        s_lastSampledFrames = sampledFrames;

        const uint64_t leafWindowNs = dMergeNs + dSortNs + dPathNs[kG316Path1] +
                                      dPathNs[kG316Path2] + dPathNs[kG316Path3];
        const uint64_t sampleResidualNs = dSampleWindowNs >= leafWindowNs
                                              ? dSampleWindowNs - leafWindowNs
                                              : 0u;
        const uint64_t rootWorkerNs = dWindowNs + dBoundaryNs + dApplyNs;
        const uint64_t sampleWorkerNs = dSampleWindowNs + dSampleBoundaryNs + dSampleApplyNs;
        const uint64_t sampleReconciledNs = leafWindowNs + sampleResidualNs +
                                             dSampleBoundaryNs + dSampleApplyNs;
        const double rootDenom = 60.0 * 1.0e6;
        const double sampleDenom = static_cast<double>(dSampledFrames) * 1.0e6;

        std::fprintf(stderr,
                     "[G316:worker] mode=%s frames=60 root=%.3fms/f "
                     "[window=%.3f boundary=%.3f apply=%.3f/%llu windows=%.2f] "
                     "samples=%llu sample=%.3fms/f [merge=%.3f sort=%.3f p1=%.3f/%llu "
                     "p2=%.3f/%llu p3=%.3f/%llu other=%.3f boundary=%.3f apply=%.3f] "
                     "reconcile=%.3fms/f\n",
                     g316RecordOn() ? "census" : "empty", rootWorkerNs / rootDenom,
                     dWindowNs / rootDenom, dBoundaryNs / rootDenom, dApplyNs / rootDenom,
                     static_cast<unsigned long long>(dApplies), static_cast<double>(dWindows) / 60.0,
                     static_cast<unsigned long long>(dSampledFrames), sampleWorkerNs / sampleDenom,
                     dMergeNs / sampleDenom, dSortNs / sampleDenom,
                     dPathNs[kG316Path1] / sampleDenom,
                     static_cast<unsigned long long>(dPathPackets[kG316Path1]),
                     dPathNs[kG316Path2] / sampleDenom,
                     static_cast<unsigned long long>(dPathPackets[kG316Path2]),
                     dPathNs[kG316Path3] / sampleDenom,
                     static_cast<unsigned long long>(dPathPackets[kG316Path3]),
                     sampleResidualNs / sampleDenom, dSampleBoundaryNs / sampleDenom,
                     dSampleApplyNs / sampleDenom, sampleReconciledNs / sampleDenom);
    }

    // G157: one queue item is either a draw window (packets) or a frame-boundary marker (the
    // G144-flush + present-latch closure). Both travel through the SAME FIFO deque, so the
    // marker is guaranteed to be processed strictly after all of its frame's windows and
    // strictly before the next frame's — draw-order/latch-order correctness comes from queue
    // ordering alone, no separate synchronization needed for that part.
    struct QueueItem
    {
        std::vector<GifArbiterPacket> packets; // a draw window; unused when isFrameBoundary/isApply
        std::function<void()> frameBoundaryFn; // set when isFrameBoundary or isApply
        bool isFrameBoundary = false;
        bool isApply = false; // G177: plain in-order closure, no frame-marker accounting
        uint64_t g317FenceEpoch = 0u;
        uint32_t g317FenceReasons = 0u;
        bool g317HadQueuedSuccessor = false;
    };

    class G150Mtgs
    {
    public:
        static G150Mtgs &instance()
        {
            static G150Mtgs m;
            return m;
        }

        // EE thread: hand one drain() window (already path-sorted-equivalent when 1 packet) to the
        // GS worker, FIFO. In-frame backpressure caps queued windows so one slow frame can't blow up.
        void enqueueWindow(const GifArbiter::ProcessPacketFn &fn, std::vector<GifArbiterPacket> &&pkts,
                           bool force = false)
        {
            // G297: `force` materializes a window even with no arbiter packets, because its real
            // content (VU1 XGKICK Path1) arrives via the side-channel batch in processWindow. Keeps
            // the batch/window pop 1:1. When MTVU is off, force is only ever set for non-empty pkts,
            // so behavior is identical to the original empty-skip.
            if (pkts.empty() && !force)
                return;
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_processFn)
                m_processFn = fn;
            startLocked();
            g_g189EEStage.store(1, std::memory_order_relaxed);
            m_cvSpace.wait(lk, [&] { return m_stop || m_items.size() < kMaxWindows; });
            g_g189EEStage.store(0, std::memory_order_relaxed);
            if (m_stop)
                return;
            QueueItem item;
            item.packets = std::move(pkts);
            g317StampItem(item);
            m_items.push_back(std::move(item));
            m_cvWork.notify_one();
        }

        // G157 (DC2_G157_PIPELINE=1): non-blocking hand-off of the frame-boundary closure
        // (G144 flush + present latch), bounded to kPipelineDepth frames "in flight". Unlike
        // frameDrain() this does NOT wait for the closure to finish — only for a free credit —
        // which is what lets the EE thread start producing the next frame while the worker is
        // still finishing this one. The credit is released by the worker in worker() once it has
        // actually RUN the closure (see there), which is also what makes waitRegisterSlot() below
        // correct: gs_regs can't be overwritten for the next frame until this frame's latch read it.
        void enqueueFrameBoundary(std::function<void()> onComplete)
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_started)
            {
                // Worker never started (no packets submitted this run) -> nothing to pipeline
                // against; run the closure inline so it still executes.
                lk.unlock();
                if (onComplete)
                    onComplete();
                return;
            }
            g_g189EEStage.store(2, std::memory_order_relaxed);
            m_cvFrameSlot.wait(lk, [&] { return m_stop || m_pendingFrameMarkers < kPipelineDepth; });
            g_g189EEStage.store(0, std::memory_order_relaxed);
            if (m_stop)
                return;
            ++m_pendingFrameMarkers;
            QueueItem item;
            item.isFrameBoundary = true;
            item.frameBoundaryFn = std::move(onComplete);
            g317StampItem(item);
            m_items.push_back(std::move(item));
            m_cvWork.notify_one();
        }

        // G177: EE thread — hand an arbitrary closure to the worker FIFO, ordered like a draw
        // window (strictly after everything already queued, strictly before anything queued
        // later — including this frame's boundary latch). Used for the sceGsSwapDBuff dispenv
        // privileged-register writes: they must land between this frame's last draw window and
        // its boundary latch, but must NOT stall the EE (routing them through writeIORegister's
        // waitRegisterSlot() gate was measured to serialize the G157 pipeline, 9.3→6.8 fps).
        // No frame-marker accounting: this is not a boundary, just an ordered side-effect.
        // Returns false when the worker isn't running — caller applies inline (single-threaded).
        bool enqueueApply(std::function<void()> fn)
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_started)
                return false;
            m_cvSpace.wait(lk, [&] { return m_stop || m_items.size() < kMaxWindows; });
            if (m_stop)
                return false;
            QueueItem item;
            item.isApply = true;
            item.frameBoundaryFn = std::move(fn);
            g317StampItem(item);
            m_items.push_back(std::move(item));
            m_cvWork.notify_one();
            return true;
        }

        // G157: gate for GS-privileged-register writes (called from ps2_memory.cpp). Blocks only
        // if the EE thread is about to overwrite a NEW frame's display registers while the worker
        // hasn't yet confirmed it finished presenting the PREVIOUS frame — bounding how far ahead
        // the EE can race relative to the one piece of frame state that bypasses this FIFO
        // entirely (gs_regs is written directly, not via a GIF packet). No-op/instant in the
        // common case (worker already caught up).
        void waitRegisterSlot()
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_started)
                return;
            g_g189EEStage.store(3, std::memory_order_relaxed);
            m_cvFrameSlot.wait(lk, [&] { return m_stop || m_pendingFrameMarkers < kPipelineDepth; });
            g_g189EEStage.store(0, std::memory_order_relaxed);
        }

        // EE thread frame boundary (mgEndFrame): block until the worker has fully drained this frame's
        // draw windows (queue empty AND not mid-window), i.e. frame N's VRAM is complete. The CALLER
        // (EE thread) then runs the present latch itself — reading live m_privRegs + vsync tick exactly
        // as the synchronous baseline does, over complete VRAM → presentation is byte-consistent with
        // baseline. Perf is preserved because the EE-side per-frame work (~51 ms) overlaps the worker's
        // draw of the SAME frame (~183 ms) as windows stream in, so the frame is still worker-bound.
        void frameDrain()
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_started)
                return;
            m_cvIdle.wait(lk, [&] { return m_stop || (m_items.empty() && !m_busy); });

            // G151 diagnostic: worker busy ms/frame over a 30-frame window (this is the frame boundary
            // on the EE thread — the worker is idle here). Grounds the MTGS+G144 ceiling analysis.
            static const bool s_stat = (std::getenv("DC2_G151_STAT") != nullptr);
            if (s_stat)
            {
                static uint64_t s_frame = 0, s_lastBusy = 0, s_lastWin = 0, s_lastPkt = 0, s_lastCpu = 0;
                ++s_frame;
                if ((s_frame % 30u) == 0u)
                {
                    const uint64_t busy = g_g151WorkerBusyNs.load(std::memory_order_relaxed);
                    const uint64_t cpu = g_g151WorkerCpuNs.load(std::memory_order_relaxed);
                    const uint64_t win = g_g151WindowCount.load(std::memory_order_relaxed);
                    const uint64_t pkt = g_g151PktCount.load(std::memory_order_relaxed);
                    const double ms = (busy - s_lastBusy) / 1e6 / 30.0;
                    const double cpuMs = (cpu - s_lastCpu) / 1e6 / 30.0;
                    std::fprintf(stderr,
                                 "[G151:worker] frame=%llu window=30 busyMs/f=%.1f cpuMs/f=%.1f (%.0f%% onCPU) windows/f=%.1f pkts/f=%.1f\n",
                                 (unsigned long long)s_frame, ms, cpuMs,
                                 ms > 0.001 ? 100.0 * cpuMs / ms : 0.0,
                                 (double)(win - s_lastWin) / 30.0, (double)(pkt - s_lastPkt) / 30.0);
                    s_lastBusy = busy; s_lastWin = win; s_lastPkt = pkt; s_lastCpu = cpu;
                }
            }
        }

        // EE/present thread: block until the worker has processed everything (store-image readback,
        // GS sync syscalls, shutdown).
        void waitIdle()
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (!m_started)
                return;
            m_cvIdle.wait(lk, [&] { return m_stop || (m_items.empty() && !m_busy); });
        }

        void shutdown()
        {
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                if (!m_started)
                    return;
                m_cvIdle.wait(lk, [&] { return m_items.empty() && !m_busy; });
                m_stop = true;
            }
            m_cvWork.notify_all();
            m_cvSpace.notify_all();
            m_cvFrameSlot.notify_all();
            if (m_thread.joinable())
                m_thread.join();
        }

        ~G150Mtgs() { shutdown(); }

    private:
        static constexpr size_t kMaxWindows = 8192;
        static constexpr int kPipelineDepth = 1; // G157: max frames the EE may run ahead of the worker

        static void g317StampItem(QueueItem &item)
        {
            if (!g317FenceCensusOn())
                return;
            item.g317FenceEpoch = g_g317VifFenceEpoch.load(std::memory_order_acquire);
            item.g317FenceReasons = g_g317VifFenceReasons.exchange(0u, std::memory_order_acq_rel);
        }

        void g317CloseRun()
        {
            if (m_g317RunPackets == 0u)
                return;
            ++m_g317Stats.runs;
            m_g317Stats.maxRun = std::max<uint64_t>(m_g317Stats.maxRun, m_g317RunPackets);
            ++m_g317Stats.runBins[g317RunBin(m_g317RunPackets)];
            m_g317RunPackets = 0u;
        }

        void g317Barrier(G317BarrierIndex barrier)
        {
            g317CloseRun();
            ++m_g317Stats.barriers[barrier];
        }

        void g317ObserveItemFence(const QueueItem &item)
        {
            if (!g317FenceCensusOn() || item.g317FenceEpoch == m_g317LastFenceEpoch)
                return;

            g317CloseRun();
            m_g317Stats.explicitEpochs += item.g317FenceEpoch - m_g317LastFenceEpoch;
            m_g317LastFenceEpoch = item.g317FenceEpoch;
            const uint32_t reasons = item.g317FenceReasons;
            if (reasons & kG317VifFlush)  ++m_g317Stats.barriers[kG317BarrierFlush];
            if (reasons & kG317VifFlusha) ++m_g317Stats.barriers[kG317BarrierFlusha];
            if (reasons & kG317VifMscal)  ++m_g317Stats.barriers[kG317BarrierMscal];
            if (reasons & kG317VifMscalf) ++m_g317Stats.barriers[kG317BarrierMscalf];
            if (reasons & kG317VifMscnt)  ++m_g317Stats.barriers[kG317BarrierMscnt];
        }

        bool g317ObservePacket(const GifArbiterPacket &pkt, bool firstEligibleInWindow,
                               bool hadQueuedSuccessor)
        {
            // Track IMAGE continuation across every GIF path. A non-Path-2
            // packet is still a barrier, but it may start or consume global
            // IMAGE state that makes following Path-2 bytes ineligible.
            const G317Path2Class cls = g317ClassifyPath2Packet(pkt, m_g317Path2Scan);
            if (pkt.pathId == GifPathId::Path1)
            {
                g317Barrier(kG317BarrierPath1);
                return false;
            }
            if (pkt.pathId == GifPathId::Path3)
            {
                g317Barrier(kG317BarrierPath3);
                return false;
            }
            if (pkt.pathId != GifPathId::Path2)
            {
                g317Barrier(kG317BarrierUnknown);
                return false;
            }

            if (cls == G317Path2Class::Image)
            {
                g317Barrier(kG317BarrierImage);
                return false;
            }
            if (cls == G317Path2Class::DirectHl)
            {
                g317Barrier(kG317BarrierDirectHl);
                return false;
            }
            if (cls == G317Path2Class::Unknown)
            {
                g317Barrier(kG317BarrierUnknown);
                return false;
            }

            ++m_g317RunPackets;
            ++m_g317Stats.eligiblePackets;
            m_g317Stats.eligibleBytes += pkt.data.size();
            if (firstEligibleInWindow)
            {
                ++m_g317Stats.eligibleWindows;
                if (hadQueuedSuccessor)
                    ++m_g317Stats.readyWindows;
            }
            return true;
        }

        void g317ReportFrame()
        {
            if (!g317FenceCensusOn())
                return;
            g317Barrier(kG317BarrierFrame);
            m_g317Path2Scan = {};
            if ((++m_g317Stats.frames % 60u) != 0u)
                return;

            const double avgRun = m_g317Stats.runs != 0u
                                      ? static_cast<double>(m_g317Stats.eligiblePackets) /
                                            static_cast<double>(m_g317Stats.runs)
                                      : 0.0;
            const double readyPct = m_g317Stats.eligibleWindows != 0u
                                        ? 100.0 * static_cast<double>(m_g317Stats.readyWindows) /
                                              static_cast<double>(m_g317Stats.eligibleWindows)
                                        : 0.0;
            std::fprintf(stderr,
                         "[G317:fence] frames=60 runs=%llu eligible=%llu bytes=%llu "
                         "avg=%.2f max=%llu bins=[%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                         "ready=%llu/%llu(%.1f%%) epochs=%llu "
                         "bar=[flush=%llu flusha=%llu mscal=%llu mscalf=%llu mscnt=%llu "
                         "p1=%llu p3=%llu image=%llu directhl=%llu unknown=%llu apply=%llu frame=%llu] "
                         "pendingImageQwc=%llu\n",
                         static_cast<unsigned long long>(m_g317Stats.runs),
                         static_cast<unsigned long long>(m_g317Stats.eligiblePackets),
                         static_cast<unsigned long long>(m_g317Stats.eligibleBytes), avgRun,
                         static_cast<unsigned long long>(m_g317Stats.maxRun),
                         static_cast<unsigned long long>(m_g317Stats.runBins[0]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[1]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[2]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[3]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[4]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[5]),
                         static_cast<unsigned long long>(m_g317Stats.runBins[6]),
                         static_cast<unsigned long long>(m_g317Stats.readyWindows),
                         static_cast<unsigned long long>(m_g317Stats.eligibleWindows), readyPct,
                         static_cast<unsigned long long>(m_g317Stats.explicitEpochs),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierFlush]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierFlusha]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierMscal]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierMscalf]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierMscnt]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierPath1]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierPath3]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierImage]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierDirectHl]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierUnknown]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierApply]),
                         static_cast<unsigned long long>(m_g317Stats.barriers[kG317BarrierFrame]),
                         static_cast<unsigned long long>(m_g317Path2Scan.pendingImageQwc));
            m_g317Stats = {};
        }

        void startLocked()
        {
            if (!m_started)
            {
                m_started = true;
                m_thread = std::thread([this] { worker(); });
            }
        }

        void worker()
        {
            // G159 (default OFF, DC2_G158_GPURASTER=1; needs DC2_G150_MTGS=1 to run this thread
            // at all): empirical GL-context-sharing probe, run once before the worker's normal
            // GS-drain loop starts. Isolated from real rendering -- see
            // ps2_gs_gpu_raster.cpp's g158RunWorkerContextTest() and plans/phase-G158-fix-log.md.
            extern bool g158_gpu_raster_enabled();
            extern void g158RunWorkerContextTest();
            if (g158_gpu_raster_enabled())
                g158RunWorkerContextTest();

            std::unique_lock<std::mutex> lk(m_mtx);
            for (;;)
            {
                g_g189WorkerStage.store(0, std::memory_order_relaxed);
                m_cvWork.wait(lk, [&] { return m_stop || !m_items.empty(); });
                if (m_stop && m_items.empty())
                    return;
                QueueItem item = std::move(m_items.front());
                m_items.pop_front();
                item.g317HadQueuedSuccessor = !m_items.empty();
                m_busy = true;
                m_cvSpace.notify_all(); // a window slot freed
                lk.unlock();

                if (item.isApply)
                {
                    g317ObserveItemFence(item);
                    if (g317FenceCensusOn())
                        g317Barrier(kG317BarrierApply);
                    g_g189WorkerStage.store(2, std::memory_order_relaxed);
                    // G177: ordered side-effect (dispenv register writes). No marker/credit to
                    // release — just run it at its FIFO position.
                    const bool g316On = g316TimingOn();
                    const bool g316Sample = g316On && m_g316LeafSample;
                    const auto g316T0 = g316On ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                    try
                    {
                        if (item.frameBoundaryFn)
                            item.frameBoundaryFn();
                    }
                    catch (const std::exception &e)
                    {
                        std::fprintf(stderr, "[G177:apply] exception: %s\n", e.what());
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[G177:apply] unknown exception\n");
                    }
                    if (g316On)
                    {
                        const uint64_t g316ApplyNs = g316ElapsedNs(g316T0);
                        g_g316ApplyNs.fetch_add(g316ApplyNs, std::memory_order_relaxed);
                        if (g316Sample)
                            g_g316SampleApplyNs.fetch_add(g316ApplyNs, std::memory_order_relaxed);
                        g_g316Applies.fetch_add(1u, std::memory_order_relaxed);
                    }
                    lk.lock();
                    m_busy = false;
                    if (m_items.empty())
                        m_cvIdle.notify_all();
                    continue;
                }

                if (item.isFrameBoundary)
                {
                    g317ObserveItemFence(item);
                    g_g189WorkerStage.store(3, std::memory_order_relaxed);
                    // G157: this frame's draws are all ahead of us in FIFO order and have already
                    // been processed, so VRAM is complete; the register-write gate
                    // (waitRegisterSlot(), consulted from ps2_memory.cpp) has been holding off any
                    // EE write to the 6 presentation registers since this marker was enqueued, so
                    // gs_regs still belongs to THIS frame. Running the closure (G144 flush + the
                    // UNMODIFIED GS::latchHostPresentationFrame()) here is therefore correct.
                    static const bool s_g184Stat = (std::getenv("DC2_G184_BOUNDARY_STAT") != nullptr);
                    const bool g316On = g316TimingOn();
                    const bool g316Sample = g316On && m_g316LeafSample;
                    const auto g184T0 = s_g184Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    const auto g316T0 = g316On ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                    try
                    {
                        if (item.frameBoundaryFn)
                            item.frameBoundaryFn();
                    }
                    catch (const std::exception &e)
                    {
                        std::fprintf(stderr, "[G157:pipeline] frame-boundary exception: %s\n", e.what());
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[G157:pipeline] frame-boundary unknown exception\n");
                    }
                    if (s_g184Stat)
                    {
                        const uint64_t ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g184T0).count());
                        const uint64_t total = g_g184BoundaryNs.fetch_add(ns, std::memory_order_relaxed) + ns;
                        static uint64_t s_frame = 0, s_last = 0;
                        if ((++s_frame % 30u) == 0u)
                        {
                            const double ms = (total - s_last) / 1.0e6 / 30.0;
                            std::fprintf(stderr, "[G184:boundary] frame=%llu window=30 boundaryMs/f=%.2f\n",
                                         (unsigned long long)s_frame, ms);
                            s_last = total;
                        }
                    }
                    g_g184BoundaryCount.fetch_add(1u, std::memory_order_relaxed);
                    if (g316On)
                    {
                        const uint64_t g316BoundaryNs = g316ElapsedNs(g316T0);
                        g_g316BoundaryNs.fetch_add(g316BoundaryNs, std::memory_order_relaxed);
                        if (g316Sample)
                        {
                            g_g316SampleBoundaryNs.fetch_add(g316BoundaryNs, std::memory_order_relaxed);
                            g_g316SampledFrames.fetch_add(1u, std::memory_order_relaxed);
                        }
                        g316ReportBoundary();
                        // Arm the NEXT complete worker frame once every 30 boundaries. The current
                        // frame's sample (if any) was fully accounted before this state changes.
                        m_g316LeafSample = ((++m_g316BoundaryN % 30u) == 0u);
                    }
                    g317ReportFrame();

                    lk.lock();
                    m_busy = false;
                    --m_pendingFrameMarkers;
                    // Release the EE thread: it may now write the next frame's presentation
                    // registers (waitRegisterSlot()) and/or enqueue the next frame's own marker.
                    m_cvFrameSlot.notify_all();
                    if (m_items.empty())
                        m_cvIdle.notify_all();
                    continue;
                }

                g_g189WorkerStage.store(1, std::memory_order_relaxed);
                static const bool s_g151cpu = (std::getenv("DC2_G151_STAT") != nullptr);
                const size_t g151NPkt = item.packets.size();
                const auto g151T0 = std::chrono::steady_clock::now();
                const uint64_t g151Cpu0 = s_g151cpu ? g156ThreadCpuNs() : 0ull;

                // Pipeline error-handling (vault: "identify fatal errors before defining the
                // architecture" — Pipeline Architecture Style): a bad GIF packet must not take down
                // the whole process via an unhandled throw on this worker (the guest/game thread is
                // wrapped the same way in ps2_runtime.cpp). Swallow + continue so the pipeline drains.
                try
                {
                    g317ObserveItemFence(item);
                    processWindow(item.packets, item.g317HadQueuedSuccessor);
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[G150:mtgs] GS worker exception: %s\n", e.what());
                }
                catch (...)
                {
                    std::fprintf(stderr, "[G150:mtgs] GS worker unknown exception\n");
                }

                const auto g151T1 = std::chrono::steady_clock::now();
                const uint64_t g151Cpu1 = s_g151cpu ? g156ThreadCpuNs() : 0ull;
                g_g151WorkerBusyNs.fetch_add(
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(g151T1 - g151T0).count()),
                    std::memory_order_relaxed);
                g_g151WorkerCpuNs.fetch_add(g151Cpu1 - g151Cpu0, std::memory_order_relaxed);
                g_g151WindowCount.fetch_add(1u, std::memory_order_relaxed);
                g_g151PktCount.fetch_add(static_cast<uint64_t>(g151NPkt), std::memory_order_relaxed);

                lk.lock();
                m_busy = false;
                if (m_items.empty())
                    m_cvIdle.notify_all();
            }
        }

        void processWindow(std::vector<GifArbiterPacket> &q, bool g317HadQueuedSuccessor)
        {
            const bool g316On = g316TimingOn();
            const bool g316Record = g316RecordOn();
            const bool g316Sample = g316On && m_g316LeafSample;
            const auto g316WindowT0 = g316On ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
            // G297 MTVU: pull this window's off-thread VU1 XGKICK output (Path1) and merge it into
            // the window BEFORE the sort. Waits per-kick only if VU1 hasn't caught up (backpressure).
            // No-op unless MTVU is active. Under MTVU, XGKICK does not submit to the arbiter, so q
            // holds no Path1 packets; the stable sort places these kicks (in kick order) ahead of
            // Path2/Path3 exactly as the synchronous baseline's path sort would.
            {
                const auto g316MergeT0 = g316Sample ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                std::vector<std::vector<uint8_t>> g297pk;
                g297CollectWindowPackets(g297pk);
                for (auto &pk : g297pk)
                {
                    GifArbiterPacket p;
                    p.pathId = GifPathId::Path1;
                    p.path2DirectHl = false;
                    p.path3Image = false;
                    p.data = std::move(pk);
                    q.push_back(std::move(p));
                }
                if (g316Sample)
                {
                    const uint64_t g316MergeNs = g316ElapsedNs(g316MergeT0);
                    if (g316Record)
                        g_g316MergeNs.fetch_add(g316MergeNs, std::memory_order_relaxed);
                }
            }
            // Identical grouping/order to GifArbiter::drain(): stable path sort, then process in order.
            const auto g316SortT0 = g316Sample ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
            std::stable_sort(q.begin(), q.end(),
                             [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                             {
                                 if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                                 {
                                     if (a.path3Image && b.path2DirectHl)
                                         return true;
                                     if (a.path2DirectHl && b.path3Image)
                                         return false;
                                 }
                                 return static_cast<uint8_t>(a.pathId) < static_cast<uint8_t>(b.pathId);
                             });
            if (g316Sample)
            {
                const uint64_t g316SortNs = g316ElapsedNs(g316SortT0);
                if (g316Record)
                    g_g316SortNs.fetch_add(g316SortNs, std::memory_order_relaxed);
            }
            if (!g316Sample)
            {
                // Default and non-sampled diagnostic frames retain the original dispatch loop.
                bool g317FirstEligibleInWindow = true;
                for (auto &pkt : q)
                {
                    if (g317FenceCensusOn())
                    {
                        if (g317ObservePacket(pkt, g317FirstEligibleInWindow, g317HadQueuedSuccessor))
                            g317FirstEligibleInWindow = false;
                    }
                    if (!pkt.data.empty() && m_processFn)
                        m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
                }
            }
            else
            {
                // The stable sort makes each path a contiguous run. Time the run, not individual
                // packet dispatches: G312 proved a packet/draw timer changes this renderer's result.
                bool g317FirstEligibleInWindow = true;
                for (size_t begin = 0u; begin < q.size();)
                {
                    const GifPathId pathId = q[begin].pathId;
                    const auto g316PathT0 = std::chrono::steady_clock::now();
                    uint64_t g316PathPackets = 0u;
                    size_t end = begin;
                    do
                    {
                        auto &pkt = q[end];
                        if (g317FenceCensusOn())
                        {
                            if (g317ObservePacket(pkt, g317FirstEligibleInWindow, g317HadQueuedSuccessor))
                                g317FirstEligibleInWindow = false;
                        }
                        if (!pkt.data.empty() && m_processFn)
                        {
                            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
                            ++g316PathPackets;
                        }
                        ++end;
                    }
                    while (end < q.size() && q[end].pathId == pathId);

                    if (g316Record)
                    {
                        const uint32_t path = static_cast<uint32_t>(pathId);
                        if (path >= 1u && path <= 3u)
                        {
                            const uint32_t index = path - 1u;
                            g_g316PathNs[index].fetch_add(g316ElapsedNs(g316PathT0), std::memory_order_relaxed);
                            g_g316PathPackets[index].fetch_add(g316PathPackets, std::memory_order_relaxed);
                        }
                    }
                    else
                    {
                        // Empty arm: preserve the same sampled-frame timestamp cadence.
                        (void)g316ElapsedNs(g316PathT0);
                    }
                    begin = end;
                }
            }
            if (g316On)
            {
                const uint64_t g316WindowNs = g316ElapsedNs(g316WindowT0);
                g_g316WindowNs.fetch_add(g316WindowNs, std::memory_order_relaxed);
                if (g316Sample)
                    g_g316SampleWindowNs.fetch_add(g316WindowNs, std::memory_order_relaxed);
                g_g316Windows.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        std::mutex m_mtx;
        std::condition_variable m_cvWork, m_cvSpace, m_cvIdle;
        std::condition_variable m_cvFrameSlot; // G157: pipeline-depth credit + register-write gate
        std::deque<QueueItem> m_items;
        int m_pendingFrameMarkers = 0; // G157: frame markers enqueued but not yet processed
        std::thread m_thread;
        GifArbiter::ProcessPacketFn m_processFn;
        bool m_started = false;
        bool m_stop = false;
        bool m_busy = false;
        bool m_g316LeafSample = false; // worker-thread only; one complete frame every 30 boundaries
        uint64_t m_g316BoundaryN = 0u;
        G317CensusStats m_g317Stats{};
        G317Path2ScanState m_g317Path2Scan{};
        uint64_t m_g317LastFenceEpoch = 0u;
        uint64_t m_g317RunPackets = 0u;
    };
} // namespace

bool g150_mtgs_enabled()
{
    static const bool on = !f50_12_env_flag("DC2_G150_NO_MTGS") &&
                           !f50_12_env_disabled("DC2_G150_MTGS");
    return on;
}

// G177: ordered-closure hand-off for FIFO-bypass register writes (see G150Mtgs::enqueueApply).
// Returns false when MTGS is off or the worker isn't running — caller applies inline.
bool g150_enqueue_apply(std::function<void()> fn)
{
    if (!g150_mtgs_enabled())
        return false;
    return G150Mtgs::instance().enqueueApply(std::move(fn));
}

// G157 (default ON since 2026-07-10, requires MTGS which is itself default-on): pipeline the EE
// and the GS worker across the frame boundary instead of frameDrain()'s full block every frame.
// See the G150Mtgs::enqueueFrameBoundary/waitRegisterSlot comments for the design. G174's CLUT/
// TEX0 race is fixed (G177), the G189 present-thread hang on idle screens is fixed (dedicated
// host-presentation mutex), and the dungeon-3D soak is clean (G187). Kill via
// DC2_G157_NO_PIPELINE=1 or DC2_G157_PIPELINE=0.
bool g150_pipeline_enabled()
{
    static const bool on = g150_mtgs_enabled() &&
                           !f50_12_env_flag("DC2_G157_NO_PIPELINE") &&
                           !f50_12_env_disabled("DC2_G157_PIPELINE");
    return on;
}

// Called from ps2_memory.cpp's writeIORegister for the 6 presentation registers only. See
// G150Mtgs::waitRegisterSlot.
void g150_pipeline_wait_register_slot()
{
    if (g150_pipeline_enabled())
        G150Mtgs::instance().waitRegisterSlot();
}

// Present-latch at the frame boundary.
// - MTGS off: runs the closure inline (byte-identical to the pre-G150 path).
// - MTGS on, pipelining off: first block until the GS worker has finished this frame's draws
//   (frameDrain), then run the closure on THIS (EE) thread — reads live display registers + vsync
//   exactly as the synchronous baseline, over complete VRAM (G150-v2, unchanged).
// - MTGS on, pipelining on (DC2_G157_PIPELINE=1): hand the closure to the worker as a
//   frame-boundary marker and return immediately — the EE does NOT wait. The closure still runs
//   with correct registers (gated by waitRegisterSlot()) and complete VRAM (FIFO ordering), just
//   later and on the worker thread instead of here.
// G175: count of guest frame-boundary latches (mgEndFrame barrier). UploadFrame's present-thread
// re-latch is only a boot fallback until the first boundary latch exists; after that the boundary
// snapshot is the sole presentation source (the per-tick re-latch read VRAM mid-frame and caused
// the RTT-transition black/stale ping-pong).
static std::atomic<uint64_t> s_g175FrameBoundaryCount{0};

uint64_t g175_frame_boundary_count()
{
    return s_g175FrameBoundaryCount.load(std::memory_order_relaxed);
}

// G303: expose the MTGS worker's TOTAL per-window busy time (processWindow, accumulated since G151)
// so the GS worker's whole-frame occupancy can be compared to the GSimage subset and the VU1-worker
// busy time for post-MTVU pole attribution. File-scope read of the anon-namespace counter.
uint64_t g303_gs_worker_busy_ns() { return g_g151WorkerBusyNs.load(std::memory_order_relaxed); }

void g150_frame_barrier(std::function<void()> latch)
{
    s_g175FrameBoundaryCount.fetch_add(1u, std::memory_order_relaxed);
    if (g150_mtgs_enabled())
    {
        if (g150_pipeline_enabled())
        {
            G150Mtgs::instance().enqueueFrameBoundary(std::move(latch));
            return; // ownership of the closure moved into the marker; do not also run it here
        }
        G150Mtgs::instance().frameDrain();
    }
    if (latch)
        latch();
}

void g150_wait_idle()
{
    if (g150_mtgs_enabled())
        G150Mtgs::instance().waitIdle();
}

void g150_shutdown()
{
    if (g150_mtgs_enabled())
        G150Mtgs::instance().shutdown();
}

// G189 diagnostic accessors (see the g_g189* atomics above). Called from the closure lambda in
// dc2_game_override.cpp and from the present-thread watchdog in ps2_runtime.cpp.
void g189_set_closure_stage(int stage, uint32_t n)
{
    // stage: 1=enter-flush, 2=enter-latch, 3=done -> mapped onto g_g189WorkerStage 4/5/6 so it
    // stays distinguishable from the coarse worker-loop stages (0/1/2/3) set in worker().
    g_g189WorkerStage.store(3 + stage, std::memory_order_relaxed);
    g_g189WorkerN.store(n, std::memory_order_relaxed);
}

int g189_worker_stage() { return g_g189WorkerStage.load(std::memory_order_relaxed); }
uint64_t g189_worker_n() { return g_g189WorkerN.load(std::memory_order_relaxed); }
int g189_ee_stage() { return g_g189EEStage.load(std::memory_order_relaxed); }

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
    s_path3PendingImageQwc = 0u;
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

namespace
{
    uint32_t imageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != 2u)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    const uint32_t debugIndex = s_debugGifArbiterSubmitCount.fetch_add(1, std::memory_order_relaxed);
    if (debugIndex < 96u)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        RUNTIME_LOG("[gif:submit] idx=" << debugIndex
                                        << " path=" << pathName(pathId)
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " directhl=" << static_cast<uint32_t>(path2DirectHl ? 1u : 0u)
                                        << std::endl);
    }

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = false;
    if (pathId == GifPathId::Path3)
    {
        const uint32_t packetQwc = sizeBytes / 16u;
        if (s_path3PendingImageQwc != 0u)
        {
            pkt.path3Image = true;
            const uint32_t chunkQwc = std::min<uint32_t>(s_path3PendingImageQwc, packetQwc);
            s_path3PendingImageQwc -= chunkQwc;
        }
        else
        {
            const uint32_t imageQwc = imageQwcFromTag(data, sizeBytes);
            if (imageQwc != 0u)
            {
                pkt.path3Image = true;
                const uint32_t inlineImageQwc = (packetQwc > 0u) ? (packetQwc - 1u) : 0u;
                if (imageQwc > inlineImageQwc)
                    s_path3PendingImageQwc = imageQwc - inlineImageQwc;
            }
        }
    }
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    // G150 MTGS: hand this drain window to the GS worker thread (FIFO) and return without touching
    // GS state on the EE thread. Bit-exact — see the G150Mtgs comment above.
    if (g150_mtgs_enabled())
    {
        // G297 MTVU: seal this window's VU1 kick batch (1:1 with the window the worker will pop in
        // processWindow) so kick output lands in its own window in order. A geometry-only VIF chain
        // has empty m_queue but pending kicks → still materialize the window (force) so its batch is
        // consumed and lockstep holds. No-op unless MTVU is active (haveKicks stays false).
        const bool haveKicks = g297HasPendingKicks();
        if (!m_queue.empty() || haveKicks)
        {
            g297OnWindowHandoff();
            G150Mtgs::instance().enqueueWindow(m_processFn, std::move(m_queue), /*force=*/true);
            m_queue.clear();
        }
        return;
    }

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            const uint32_t debugIndex = s_debugGifArbiterDrainCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 96u)
            {
                uint64_t tagLo = 0;
                std::memcpy(&tagLo, pkt.data.data(), sizeof(tagLo));
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                RUNTIME_LOG("[gif:drain] idx=" << debugIndex
                                               << " path=" << pathName(pkt.pathId)
                                               << " size=" << pkt.data.size()
                                               << " nloop=" << nloop
                                               << " flg=" << static_cast<uint32_t>(flg)
                                               << " nreg=" << nreg
                                               << " directhl=" << static_cast<uint32_t>(pkt.path2DirectHl ? 1u : 0u)
                                               << " path3image=" << static_cast<uint32_t>(pkt.path3Image ? 1u : 0u)
                                               << std::endl);
            }
            f50_12_scan_gif_packet(pkt, debugIndex);
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
