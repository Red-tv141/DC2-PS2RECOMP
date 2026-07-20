#include "runtime/ps2_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// G217: armed by the default-off exact head-object wrapper in dc2_game_override.cpp and
// consumed by the matching scratchpad -> RAM DMA below.  Kept out of a shared header because
// this is a temporary cross-layer diagnostic, not runtime API.
extern std::atomic<uint32_t> g_dc2G217HeadDmaPacket;
extern std::atomic<uint32_t> g_dc2G217HeadDmaSelf;
extern std::atomic<uint32_t> g_dc2G217HeadDmaKind;
extern std::atomic<uint32_t> g_dc2G217DirectPackets[128];

std::atomic<uint32_t> g_dc2G136TraceActive{0};
std::atomic<uint32_t> g_dc2G136TraceSeq{0};
// G138: last VU1 XGKICK program counter (packer entry), published by ps2_vu1.cpp just
// before it submits the kicked packet on PATH1, so the G138 GS-stream dump can attribute
// each PATH1 record to its VU packer without changing the submit signature.
std::atomic<uint32_t> g_dc2G138XgPc{0};
std::atomic<uint64_t> g_g146GifSubmitNs{0};
// G138: title scope flag (defined in dc2_game_override.cpp).
extern std::atomic<bool> g_dc2TitleRockScope;

namespace
{
    constexpr uint32_t G217_RECENT_DMA_COUNT = 256u;
    std::atomic<uint32_t> s_g217RecentDma[G217_RECENT_DMA_COUNT]{};
    std::atomic<uint32_t> s_g217RecentDmaWrite{0u};

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

    // G268: env vars are process-constant in every DC2 harness; the VIF1 chain-tag walker
    // called getenv() per tag/candidate check (G217/G227 sites, up to 16384 tags per chain).
    // Read once by default; DC2_G268_LIVE_ENVREAD=1 restores the per-call read (A/B control).
    bool g268LiveEnvRead()
    {
        static const bool live = envFlagEnabled("DC2_G268_LIVE_ENVREAD");
        return live;
    }

    bool g217TagVuEnabled()
    {
        static const bool cached = envFlagEnabled("DC2_G217_TAG_VU");
        return g268LiveEnvRead() ? envFlagEnabled("DC2_G217_TAG_VU") : cached;
    }

    bool g227TraceEnabled()
    {
        static const bool cached = envFlagEnabled("DC2_TRACE_G227");
        return g268LiveEnvRead() ? envFlagEnabled("DC2_TRACE_G227") : cached;
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

    uint32_t g136_target_packet()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_PACKET", 0x00439e10u);
        return value & 0x1FFFFFFFu;
    }

    uint32_t g136_target_bulk()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_BULK", 0x00907e10u);
        return value & 0x1FFFFFFFu;
    }

    uint32_t g136_trace_limit()
    {
        static const uint32_t value = g136_env_u32("DC2_G136_LIMIT", 2u);
        return value;
    }

    uint32_t g136_qword_limit()
    {
        static const uint32_t value = std::min<uint32_t>(g136_env_u32("DC2_G136_QW", 4u), 16u);
        return value;
    }

    std::atomic<uint32_t> s_g136PendingChains{0};
    std::atomic<uint32_t> s_g136PendingSeq{0};
    std::atomic<uint32_t> s_g136ArmedChains{0};

    bool g136_addr_matches(uint32_t addr, uint32_t target)
    {
        return (addr & 0x1FFFFFFFu) == target;
    }

    void g136_log_qwords(const uint8_t *base, uint32_t maxBytes, uint32_t qwords,
                         const char *label, uint32_t seq, uint32_t addr)
    {
        const uint32_t capped = std::min<uint32_t>(qwords, g136_qword_limit());
        for (uint32_t q = 0u; q < capped; ++q)
        {
            const uint32_t off = q * 16u;
            if (off + 16u > maxBytes)
                break;
            uint32_t w[4] = {};
            std::memcpy(w, base + off, sizeof(w));
            std::fprintf(stderr,
                         "[G136:%s] seq=%u base=0x%x q=%u %08x %08x %08x %08x\n",
                         label, seq, addr, q, w[0], w[1], w[2], w[3]);
        }
    }

    // F51.7: scan a GIF packet submitted on a given path; report PRIM / TEX0.tbp /
    // first decoded XYZ2 verts. Helps locate which path delivers the dungeon map
    // (tbp=0x3220) and what XYZ it carries.
    void f517_scan_submit(int pathId, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!vu1_trace_enabled() || !data || sizeBytes < 16u)
            return;
        static std::atomic<uint32_t> s_scan{0};
        static std::atomic<uint32_t> s_mapHit{0};

        uint32_t offset = 0u, tagIndex = 0u;
        uint32_t curTbp = 0xFFFFFFFFu, curPsm = 0xFFFFFFFFu, prim = 0xFFFFFFFFu;
        uint32_t verts = 0u;
        uint32_t firstX = 0u, firstY = 0u, firstZ = 0u; bool haveVert = false;
        auto ld = [&](uint32_t off) -> uint64_t { uint64_t v=0; std::memcpy(&v, data+off, 8); return v; };

        while (offset + 16u <= sizeBytes && tagIndex < 512u)
        {
            const uint64_t tagLo = ld(offset);
            const uint64_t tagHi = ld(offset + 8u);
            offset += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
            if (nreg == 0u) nreg = 16u;
            if (((tagLo >> 46u) & 0x1u) != 0u) prim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);
            uint8_t regs[16];
            for (uint32_t i = 0u; i < nreg; ++i) regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

            if (flg == 0u) // PACKED
            {
                for (uint32_t loop = 0u; loop < nloop; ++loop)
                    for (uint32_t r = 0u; r < nreg; ++r)
                    {
                        if (offset + 16u > sizeBytes) goto done;
                        const uint64_t lo = ld(offset), hi = ld(offset + 8u);
                        offset += 16u;
                        uint8_t rd = regs[r];
                        uint64_t dv = lo;
                        if (rd == 0x0Eu) { rd = static_cast<uint8_t>(hi & 0xFFu); }
                        if (rd == 0x06u) { curTbp = static_cast<uint32_t>(dv & 0x3FFFu); curPsm = static_cast<uint32_t>((dv >> 20u) & 0x3Fu); }
                        else if (rd == 0x05u || rd == 0x0Du) { ++verts; if (!haveVert) { firstX=(uint32_t)(lo&0xFFFFu); firstY=(uint32_t)((lo>>32)&0xFFFFu); firstZ=(uint32_t)(hi&0xFFFFFFFFu); haveVert=true; } }
                    }
            }
            else if (flg == 1u) // REGLIST
            {
                uint32_t total = nloop * nreg;
                uint32_t adv = ((total + 1u) / 2u) * 16u; // 2 regs per qword
                if (offset + adv > sizeBytes) goto done;
                offset += adv;
            }
            else if (flg == 2u) // IMAGE
            {
                const uint64_t ib = static_cast<uint64_t>(nloop) * 16ull;
                if (ib > static_cast<uint64_t>(sizeBytes - offset)) goto done;
                offset += static_cast<uint32_t>(ib);
            }
            else goto done;
            ++tagIndex;
        }
    done:
        const bool mapHit = (curTbp == 0x3220u);
        const uint32_t n = s_scan.fetch_add(1u, std::memory_order_relaxed);
        const bool logIt = (n < 48u) || (mapHit && (s_mapHit.fetch_add(1u, std::memory_order_relaxed) < 64u));
        if (!logIt) return;
        const char *pname = (pathId == 1) ? "P1" : (pathId == 2) ? "P2" : "P3";
        std::fprintf(stderr,
                     "[F51.7:submit] n=%u path=%s bytes=%u prim=0x%x tbp=0x%x psm=0x%x verts=%u v0=(%.1f,%.1f,z=0x%x) map=%u\n",
                     n, pname, sizeBytes, prim, curTbp, curPsm, verts,
                     firstX / 16.0f, firstY / 16.0f, firstZ, mapHit ? 1u : 0u);
    }

    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return addr >= PS2_GS_PRIV_REG_BASE && addr < PS2_GS_PRIV_REG_BASE + PS2_GS_PRIV_REG_SIZE;
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        case 0x1000:
            return &gs.csr;
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
