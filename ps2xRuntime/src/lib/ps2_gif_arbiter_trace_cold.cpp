#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
bool envFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 && std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 && std::strcmp(value, "OFF") != 0;
}

const char *pathName(GifPathId id)
{
    switch (id)
    {
    case GifPathId::Path1: return "path1";
    case GifPathId::Path2: return "path2";
    case GifPathId::Path3: return "path3";
    default: return "path?";
    }
}

uint64_t load64(const uint8_t *data)
{
    uint64_t value = 0u;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool paletted(uint32_t psm)
{
    return psm == GS_PSM_T4 || psm == GS_PSM_T8 ||
           psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

void dumpPacket(const GifArbiterPacket &pkt, uint32_t drainIndex, const char *reason)
{
    static std::atomic<uint32_t> count{0u};
    const uint32_t dump = count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (dump > 4u || pkt.data.size() < 16u)
        return;

    const uint8_t *data = pkt.data.data();
    const uint32_t size = static_cast<uint32_t>(pkt.data.size());
    uint32_t offset = 0u, item = 0u;
    while (offset + 16u <= size && item < 48u)
    {
        const uint32_t tagOffset = offset;
        const uint64_t tagLo = load64(data + offset);
        const uint64_t tagHi = load64(data + offset + 8u);
        offset += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7fffu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 15u);
        if (nreg == 0u) nreg = 16u;
        uint8_t regs[16]{};
        for (uint32_t i = 0u; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 15u);
        std::fprintf(stderr,
                     "[F512:gifdump] dump=%u drain=%u path=%s reason=%s tagOff=0x%x "
                     "tagLo=0x%016llx tagHi=0x%016llx flg=%u nloop=%u nreg=%u size=%u\n",
                     dump, drainIndex, pathName(pkt.pathId), reason, tagOffset,
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi), static_cast<uint32_t>(flg),
                     nloop, nreg, size);
        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0u; loop < nloop && item < 48u; ++loop)
                for (uint32_t r = 0u; r < nreg && item < 48u; ++r)
                {
                    if (offset + 16u > size) return;
                    const uint64_t lo = load64(data + offset);
                    const uint64_t hi = load64(data + offset + 8u);
                    offset += 16u;
                    const uint8_t reg = regs[r] == 0x0eu ? static_cast<uint8_t>(hi) : regs[r];
                    std::fprintf(stderr,
                                 "[F512:gifdump] dump=%u item=%u loop=%u slot=%u desc=0x%02x "
                                 "reg=0x%02x lo=0x%016llx hi=0x%016llx\n",
                                 dump, item++, loop, r, static_cast<uint32_t>(regs[r]),
                                 static_cast<uint32_t>(reg), static_cast<unsigned long long>(lo),
                                 static_cast<unsigned long long>(hi));
                }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            const uint64_t bytes = static_cast<uint64_t>(nloop) * nreg * 8u +
                                   (((nloop * nreg) & 1u) ? 8u : 0u);
            if (bytes > size - offset) return;
            offset += static_cast<uint32_t>(bytes);
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            const uint64_t bytes = static_cast<uint64_t>(nloop) * 16u;
            if (bytes > size - offset) return;
            offset += static_cast<uint32_t>(bytes);
        }
        else
            return;
    }
}

void logRegister(const GifArbiterPacket &pkt, uint32_t drainIndex, const char *source,
                 uint8_t reg, uint64_t value, uint32_t tagIndex, uint32_t tagOffset,
                 uint32_t nloop, uint8_t flg, uint32_t nreg, uint32_t loop, uint32_t slot)
{
    if (reg == GS_REG_TEX0_1 || reg == GS_REG_TEX0_2)
    {
        const uint32_t tbp = static_cast<uint32_t>(value & 0x3fffu);
        const uint32_t tbw = static_cast<uint32_t>((value >> 14u) & 0x3fu);
        const uint32_t psm = static_cast<uint32_t>((value >> 20u) & 0x3fu);
        const uint32_t tw = static_cast<uint32_t>((value >> 26u) & 0x0fu);
        const uint32_t th = static_cast<uint32_t>((value >> 30u) & 0x0fu);
        const uint32_t cbp = static_cast<uint32_t>((value >> 37u) & 0x3fffu);
        const uint32_t cpsm = static_cast<uint32_t>((value >> 51u) & 0x0fu);
        const uint32_t csm = static_cast<uint32_t>((value >> 55u) & 1u);
        const uint32_t csa = static_cast<uint32_t>((value >> 56u) & 0x1fu);
        const uint32_t cld = static_cast<uint32_t>((value >> 61u) & 7u);
        const bool hit = tbp == 0x2580u || cbp == 0x2980u || cpsm == GS_PSM_CT16;
        const bool title = tbp == 0x1a00u && psm == GS_PSM_T4HH && cbp == 0x3fe0u;
        if (!paletted(psm) && !hit) return;
        if (hit) dumpPacket(pkt, drainIndex, "tex0-hit");
        static std::atomic<uint32_t> total{0u}, titleN{0u}, otherN{0u}, hitN{0u};
        const uint32_t n = total.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t bucket = (title ? titleN : otherN).fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t hn = hit ? hitN.fetch_add(1u, std::memory_order_relaxed) + 1u : 0u;
        const bool print = title ? (bucket <= 12u || bucket % 2048u == 0u)
                                 : (bucket <= 512u || (hit && (hn <= 256u || hn % 512u == 0u)) ||
                                    bucket % 2048u == 0u);
        if (!print) return;
        std::fprintf(stderr,
                     "[F512:giftex0] n=%u bucket=%u title=%u hit=%u drain=%u path=%s source=%s "
                     "reg=0x%02x raw=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x "
                     "cpsm=0x%x csm=%u csa=%u cld=%u size=%u tag=%u tagOff=0x%x loop=%u slot=%u "
                     "flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                     n, bucket, title ? 1u : 0u, hn, drainIndex, pathName(pkt.pathId), source,
                     static_cast<uint32_t>(reg), static_cast<unsigned long long>(value), tbp, tbw,
                     psm, tw, th, cbp, cpsm, csm, csa, cld, static_cast<uint32_t>(pkt.data.size()),
                     tagIndex, tagOffset, loop, slot, static_cast<uint32_t>(flg), nloop, nreg,
                     pkt.path2DirectHl ? 1u : 0u, pkt.path3Image ? 1u : 0u);
        return;
    }
    if (reg == GS_REG_TEX2_1 || reg == GS_REG_TEX2_2)
    {
        const uint32_t psm = static_cast<uint32_t>((value >> 20u) & 0x3fu);
        const uint32_t cbp = static_cast<uint32_t>((value >> 37u) & 0x3fffu);
        const uint32_t cpsm = static_cast<uint32_t>((value >> 51u) & 0x0fu);
        const uint32_t csm = static_cast<uint32_t>((value >> 55u) & 1u);
        const uint32_t csa = static_cast<uint32_t>((value >> 56u) & 0x1fu);
        const uint32_t cld = static_cast<uint32_t>((value >> 61u) & 7u);
        if (!paletted(psm) && cbp != 0x2980u && cpsm != GS_PSM_CT16) return;
        static std::atomic<uint32_t> count{0u};
        const uint32_t n = count.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n > 128u && n % 1024u != 0u) return;
        std::fprintf(stderr,
                     "[F512:giftex2] n=%u drain=%u path=%s source=%s reg=0x%02x raw=0x%016llx "
                     "psm=0x%x cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u size=%u tag=%u "
                     "tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                     n, drainIndex, pathName(pkt.pathId), source, static_cast<uint32_t>(reg),
                     static_cast<unsigned long long>(value), psm, cbp, cpsm, csm, csa, cld,
                     static_cast<uint32_t>(pkt.data.size()), tagIndex, tagOffset, loop, slot,
                     static_cast<uint32_t>(flg), nloop, nreg, pkt.path2DirectHl ? 1u : 0u,
                     pkt.path3Image ? 1u : 0u);
        return;
    }
    if (reg != GS_REG_BITBLTBUF) return;
    const uint32_t sbp = static_cast<uint32_t>(value & 0x3fffu);
    const uint32_t sbw = static_cast<uint32_t>((value >> 16u) & 0x3fu);
    const uint32_t spsm = static_cast<uint32_t>((value >> 24u) & 0x3fu);
    const uint32_t dbp = static_cast<uint32_t>((value >> 32u) & 0x3fffu);
    const uint32_t dbw = static_cast<uint32_t>((value >> 48u) & 0x3fu);
    const uint32_t dpsm = static_cast<uint32_t>((value >> 56u) & 0x3fu);
    const bool hit = dbp == 0x2580u || dbp == 0x2980u || dpsm == GS_PSM_CT16;
    static std::atomic<uint32_t> count{0u}, hitCount{0u};
    const uint32_t n = count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint32_t hn = hit ? hitCount.fetch_add(1u, std::memory_order_relaxed) + 1u : 0u;
    if (n > 96u && !hit && n % 512u != 0u) return;
    std::fprintf(stderr,
                 "[F512:gifbitblt] n=%u hit=%u drain=%u path=%s source=%s raw=0x%016llx "
                 "sbp=0x%x sbw=%u spsm=0x%x dbp=0x%x dbw=%u dpsm=0x%x size=%u tag=%u "
                 "tagOff=0x%x loop=%u slot=%u flg=%u nloop=%u nreg=%u directhl=%u path3image=%u\n",
                 n, hn, drainIndex, pathName(pkt.pathId), source,
                 static_cast<unsigned long long>(value), sbp, sbw, spsm, dbp, dbw, dpsm,
                 static_cast<uint32_t>(pkt.data.size()), tagIndex, tagOffset, loop, slot,
                 static_cast<uint32_t>(flg), nloop, nreg, pkt.path2DirectHl ? 1u : 0u,
                 pkt.path3Image ? 1u : 0u);
}
} // namespace

extern const bool g652GifTraceOn = envFlag("DC2_TRACE_F50_12");

void g652ScanGifPacketCold(const GifArbiterPacket &pkt, uint32_t drainIndex)
{
    if (pkt.data.empty()) return;
    const uint8_t *data = pkt.data.data();
    const uint32_t size = static_cast<uint32_t>(pkt.data.size());
    uint32_t offset = 0u, tagIndex = 0u;
    while (offset + 16u <= size && tagIndex < 256u)
    {
        const uint32_t tagOffset = offset;
        const uint64_t tagLo = load64(data + offset);
        const uint64_t tagHi = load64(data + offset + 8u);
        offset += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7fffu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 15u);
        if (nreg == 0u) nreg = 16u;
        uint8_t regs[16]{};
        for (uint32_t i = 0u; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 15u);
        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0u; loop < nloop; ++loop)
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (offset + 16u > size) return;
                    const uint64_t lo = load64(data + offset);
                    const uint64_t hi = load64(data + offset + 8u);
                    offset += 16u;
                    const bool ad = regs[r] == 0x0eu;
                    logRegister(pkt, drainIndex, ad ? "packed-ad" : "packed-direct",
                                ad ? static_cast<uint8_t>(hi) : regs[r], lo, tagIndex, tagOffset,
                                nloop, flg, nreg, loop, r);
                }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0u; loop < nloop; ++loop)
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (offset + 8u > size) return;
                    const uint64_t value = load64(data + offset);
                    offset += 8u;
                    logRegister(pkt, drainIndex, "reglist", regs[r], value, tagIndex, tagOffset,
                                nloop, flg, nreg, loop, r);
                }
            if (((nloop * nreg) & 1u) != 0u)
            {
                if (offset + 8u > size) return;
                offset += 8u;
            }
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            const uint64_t bytes = static_cast<uint64_t>(nloop) * 16u;
            if (bytes > size - offset) return;
            offset += static_cast<uint32_t>(bytes);
        }
        else
            return;
        ++tagIndex;
    }
}
