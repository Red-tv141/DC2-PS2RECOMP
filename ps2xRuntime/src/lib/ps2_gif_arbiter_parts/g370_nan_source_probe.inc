// G370 — locate the PRODUCER of the cutscene "gray screen" poison (diagnostic, default-off).
//
// G369 established that during the first-cutscene gray window every scene triangle reaches the
// GS with all vertices at (0,0) and a NaN `q`, that the VIF unpack source floats are clean, and
// that no VU1 `vf` lane ever goes non-finite. Both of those are NEGATIVE probes, so the layer
// attribution ("EE-built GIF packet") is still inference.
//
// This probe closes that gap at the single convergence point every GS packet passes through
// (`PS2Memory::submitGifPacket`), where BOTH facts we need are still available:
//   * the delivery PATH (1 = VU1 XGKICK, 2 = VIF1 DIRECT, 3 = EE/GIF DMA), and
//   * the EE physical source address, when the packet buffer lies inside RDRAM.
// It walks the GIFtag chain and reports the first qwords whose `q` is non-finite:
//   - PACKED  reg 0x02 (ST)     -> q = float at qword+8
//   - PACKED  reg 0x0E (A+D)    -> addr 0x01 (RGBAQ) q = float at qword+4
//   - REGLIST reg 0x02 (ST)     -> q is NOT carried (S,T only) — skipped
// plus the XY of the nearest following XYZ(F)2/3 so a "(0,0) + NaN q" vertex is identifiable
// from one line, and the offending GIFtag's nloop/nreg/prim (a runaway nloop from garbage would
// show up here as well).
//
// Enable with DC2_G370_QSRC=1. Nothing is touched when off (one static bool test per packet on
// a path that already memcpys the whole packet).

// Lives in the GIF-arbiter TU (file scope) but is CALLED from PS2Memory::submitGifPacket, which
// sits inside a part file that starts mid-function — hence external linkage + an extern decl on
// the memory side instead of a local include.
#include <cmath>
#include "runtime/ps2_memory.h"

namespace
{
    bool g370QSrcEnabled()
    {
        static const bool s_on = (std::getenv("DC2_G370_QSRC") != nullptr);
        return s_on;
    }

    inline bool g370NonFinite(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, 4);
        return !std::isfinite(f);
    }

    inline float g370AsFloat(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    // Per-path aggregate so a periodic line proves whether a path is *ever* the carrier, and the
    // early per-hit detail lines are not mistaken for the whole picture (G291 trap).
    std::atomic<uint64_t> g_g370Hits[4]{};
    std::atomic<uint64_t> g_g370Packets[4]{};
    std::atomic<uint64_t> g_g370Rejected[4]{};

} // namespace

void g370ScanPacket(int pathId, const uint8_t *data, uint32_t sizeBytes, const uint8_t *rdram)
{
    {
        if (!g370QSrcEnabled() || !data || sizeBytes < 16u)
            return;
        const int pi = (pathId >= 1 && pathId <= 3) ? pathId : 0;
        g_g370Packets[pi].fetch_add(1u, std::memory_order_relaxed);

        // Validation pass first. A 16 KB PATH2 texture-upload chunk parsed as a tag chain yields
        // garbage "tags" (nloop=21074, prim=0x4a4) and therefore garbage NaN hits; only scan
        // packets whose whole GIFtag chain fits the buffer exactly. Without this gate the probe
        // reports thousands of false positives from IMAGE payload bytes.
        {
            uint32_t vo = 0u, vguard = 0u;
            bool ok = true;
            while (vo + 16u <= sizeBytes && ++vguard < 4096u)
            {
                uint64_t tl;
                std::memcpy(&tl, data + vo, 8);
                const uint32_t nl = static_cast<uint32_t>(tl & 0x7FFFull);
                const bool ep = ((tl >> 15) & 1ull) != 0ull;
                const uint32_t fl = static_cast<uint32_t>((tl >> 58) & 0x3ull);
                uint32_t nr = static_cast<uint32_t>((tl >> 60) & 0xFull);
                if (nr == 0u)
                    nr = 16u;
                uint64_t qw = 0u;
                if (fl == 0u)      qw = static_cast<uint64_t>(nl) * nr;
                else if (fl == 1u) qw = (static_cast<uint64_t>(nl) * nr + 1ull) / 2ull;
                else if (fl == 2u) qw = nl;
                vo += 16u;
                const uint64_t need = qw * 16ull;
                if (vo + need > sizeBytes) { ok = false; break; }
                vo += static_cast<uint32_t>(need);
                if (ep)
                    break;
            }
            if (!ok || vo > sizeBytes)
            {
                g_g370Rejected[pi].fetch_add(1u, std::memory_order_relaxed);
                return;
            }
        }

        uint32_t off = 0u;
        uint32_t guard = 0u;
        while (off + 16u <= sizeBytes && ++guard < 4096u)
        {
            uint64_t tagLo, tagHi;
            std::memcpy(&tagLo, data + off, 8);
            std::memcpy(&tagHi, data + off + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFull);
            const bool eop = ((tagLo >> 15) & 1ull) != 0ull;
            const uint32_t prim = static_cast<uint32_t>((tagLo >> 47) & 0x7FFull);
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 0x3ull);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFull);
            if (nreg == 0u)
                nreg = 16u;
            off += 16u;

            uint64_t dataQw = 0u;
            if (flg == 0u)
                dataQw = static_cast<uint64_t>(nloop) * nreg;
            else if (flg == 1u)
                dataQw = (static_cast<uint64_t>(nloop) * nreg + 1ull) / 2ull;
            else if (flg == 2u)
                dataQw = nloop;
            else
                dataQw = 0u;

            if (flg == 0u)
            {
                // PACKED: walk the reg descriptors, remember the last non-finite q, and pair it
                // with the XY of the vertex qword that consumes it.
                bool pendingNan = false;
                uint32_t nanBits = 0u;
                const char *nanFrom = "";
                for (uint64_t i = 0; i < static_cast<uint64_t>(nloop) * nreg; ++i)
                {
                    const uint32_t qwOff = off + static_cast<uint32_t>(i) * 16u;
                    if (qwOff + 16u > sizeBytes)
                        break;
                    const uint32_t reg = static_cast<uint32_t>((tagHi >> ((i % nreg) * 4u)) & 0xFull);
                    if (reg == 0x02u) // ST: q in the upper 64 bits, low word
                    {
                        uint32_t qb;
                        std::memcpy(&qb, data + qwOff + 8u, 4);
                        if (g370NonFinite(qb)) { pendingNan = true; nanBits = qb; nanFrom = "ST"; }
                    }
                    else if (reg == 0x0Eu) // A+D
                    {
                        const uint8_t addr = data[qwOff + 8u];
                        if (addr == 0x01u) // RGBAQ: q = bits 32..63
                        {
                            uint32_t qb;
                            std::memcpy(&qb, data + qwOff + 4u, 4);
                            if (g370NonFinite(qb)) { pendingNan = true; nanBits = qb; nanFrom = "RGBAQ"; }
                        }
                    }
                    else if (reg == 0x04u || reg == 0x05u || reg == 0x0Cu || reg == 0x0Du)
                    {
                        // XYZF2/XYZ2/XYZF3/XYZ3 (PACKED descriptors 0x04/0x05 and 0x0C/0x0D)
                        if (!pendingNan)
                            continue;
                        uint64_t lo;
                        std::memcpy(&lo, data + qwOff, 8);
                        const uint32_t x = static_cast<uint32_t>(lo & 0xFFFFull);
                        const uint32_t y = static_cast<uint32_t>((lo >> 32) & 0xFFFFull);
                        const uint64_t hits = g_g370Hits[pi].fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (hits <= 24u || (hits % 5000u) == 0u)
                        {
                            const bool inRam = (rdram != nullptr && data >= rdram && data < rdram + PS2_RAM_SIZE);
                            std::fprintf(stderr,
                                         "[G370:qsrc] hit=%llu path=%d src=%s%08x pktOff=0x%x from=%s "
                                         "qbits=0x%08x q=%g xy12.4=(%u,%u) xy=(%.2f,%.2f) "
                                         "tag(nloop=%u nreg=%u prim=0x%x flg=%u eop=%u) bytes=%u\n",
                                         (unsigned long long)hits, pathId,
                                         inRam ? "ee:0x" : "buf:+0x",
                                         inRam ? static_cast<uint32_t>(data - rdram) : 0u,
                                         qwOff, nanFrom, nanBits, g370AsFloat(nanBits),
                                         x, y, static_cast<float>(x) / 16.0f, static_cast<float>(y) / 16.0f,
                                         nloop, nreg, prim, flg, eop ? 1u : 0u, sizeBytes);
                        }
                        pendingNan = false;
                    }
                }
            }

            off += static_cast<uint32_t>(dataQw * 16ull);
            if (eop)
                break;
        }

        // Periodic aggregate: which path carries the poison, over the whole run.
        static std::atomic<uint64_t> s_tick{0};
        if ((s_tick.fetch_add(1u, std::memory_order_relaxed) % 20000ull) == 0ull)
            std::fprintf(stderr, "[G370:agg] p1=%llu/%llu p2=%llu/%llu p3=%llu/%llu (nanQ/packets)\n",
                         (unsigned long long)g_g370Hits[1].load(), (unsigned long long)g_g370Packets[1].load(),
                         (unsigned long long)g_g370Hits[2].load(), (unsigned long long)g_g370Packets[2].load(),
                         (unsigned long long)g_g370Hits[3].load(), (unsigned long long)g_g370Packets[3].load());
    }
}
