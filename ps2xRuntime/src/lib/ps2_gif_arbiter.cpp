#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"
#include "ps2_log.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

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
