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
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (virtualAddress >= 0x20000000u && virtualAddress < 0x40000000u)
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (virtualAddress >= 0x80000000 && virtualAddress < 0xC0000000)
    {
        return virtualAddress & 0x1FFFFFFF;
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (virtualAddress >= 0xC0000000)
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint32_t off = address & 7;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (address >= PS2_IO_BASE && address < (PS2_IO_BASE + PS2_IO_SIZE))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (reg)
        {
            uint32_t off = address & 7;
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u && off == 0u)
            {
                // CSR low dword: bits 0..1 are write-one-to-clear status bits.
                constexpr uint32_t kW1cMask = 0x3u;
                uint64_t current = *reg;
                uint32_t oldLow = static_cast<uint32_t>(current & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                current = (current & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                current &= ~static_cast<uint64_t>(value & kW1cMask);
                *reg = current;
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFULL << (off * 8);
                uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
                *reg = newVal;
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (reg)
        {
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u)
            {
                // CSR: bits 0..1 are write-one-to-clear status bits.
                constexpr uint64_t kW1cMask = 0x3ull;
                uint64_t next = (*reg & kW1cMask) | (value & ~kW1cMask);
                next &= ~(value & kW1cMask);
                *reg = next;
            }
            else
            {
                *reg = value;
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (isGsPrivReg(address))
    {
        m_ioRegisters[address] = value;
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint32_t off = address & 7u;
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u && off == 0u)
            {
                constexpr uint32_t kW1cMask = 0x3u;
                uint64_t current = *reg;
                uint32_t oldLow = static_cast<uint32_t>(current & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                current = (current & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                current &= ~static_cast<uint64_t>(value & kW1cMask);
                *reg = current;
            }
            else
            {
                const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            // =================================================================
            // PHASE G26 — EE scratchpad DMA channels: SPR_FROM (ch8 @0x1000D000,
            // scratchpad -> RAM) and SPR_TO (ch9 @0x1000D400, RAM -> scratchpad).
            // DC2's mg library stages each deform/character model VIF packet in the
            // EE scratchpad (GetScrPad@0x13e3b0), then copies it into its reserved
            // slot in the RAM-resident mgVif1Packet DMA chain via a fromSPR DMA
            // (SendDMA@0x13e3d0), kicked by a single byte store to CHCR+1 (STR bit).
            // That chain is streamed to VIF1/VU1 each frame. Without this copy the
            // model packet (BASE/OFFSET/UNPACK verts/MSCAL) never lands in the chain
            // -> VIF1 never sees it -> no XGKICK -> invisible 3D models. Registers:
            // SADR(+0x80)=scratchpad end, MADR(+0x10)=RAM end, QWC(+0x20)=qwords.
            // Confirmed live vs PCSX2 at the Select-Costume screen (G26).
            // =================================================================
            if (channelBase == 0x1000D000u || channelBase == 0x1000D400u)
            {
                const bool fromSpr = (channelBase == 0x1000D000u); // ch8: SPR -> RAM
                const uint32_t sadr = m_ioRegisters[channelBase + 0x80u];
                const uint32_t mode = (value >> 2) & 0x3u;
                uint32_t copied = 0u;
                // mg stages fixed-size packets and uses normal (non-chain) mode.
                if (qwc > 0u && mode == 0u)
                {
                    const uint32_t sprOff = isScratchpad(sadr)
                                                ? ps2ScratchpadOffset(sadr)
                                                : (sadr & (PS2_SCRATCHPAD_SIZE - 1u));
                    const uint32_t ramOff = madr & PS2_RAM_MASK;
                    uint64_t bytes64 = static_cast<uint64_t>(qwc) * 16ull;
                    uint32_t n = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu
                                                           : static_cast<uint32_t>(bytes64);
                    // Clamp to both buffers (defensive; SADR/MADR are guest-supplied).
                    if (sprOff >= PS2_SCRATCHPAD_SIZE) n = 0u;
                    else if (sprOff + n > PS2_SCRATCHPAD_SIZE) n = PS2_SCRATCHPAD_SIZE - sprOff;
                    if (ramOff >= PS2_RAM_SIZE) n = 0u;
                    else if (ramOff + n > PS2_RAM_SIZE) n = PS2_RAM_SIZE - ramOff;
                    if (n > 0u)
                    {
                        if (fromSpr)
                            std::memcpy(m_rdram + ramOff, m_scratchpad + sprOff, n);
                        else
                            std::memcpy(m_scratchpad + sprOff, m_rdram + ramOff, n);
                        copied = n;
                    }
                }
                // Mark the channel transfer complete: clear STR, drain QWC.
                m_ioRegisters[channelBase] = value & ~0x100u;
                m_ioRegisters[channelBase + 0x20u] = 0u;

                if (vu1_trace_enabled())
                {
                    static std::atomic<uint32_t> s_g26n{0};
                    const uint32_t gn = s_g26n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (gn <= 48u || (gn % 256u) == 0u)
                    {
                        uint32_t dst0 = 0u;
                        const uint32_t r = madr & PS2_RAM_MASK;
                        if (fromSpr && copied >= 4u && r + 4u <= PS2_RAM_SIZE)
                            std::memcpy(&dst0, m_rdram + r, sizeof(dst0));
                        std::fprintf(stderr,
                                     "[G26:spr] n=%u ch=%s madr=0x%08x sadr=0x%08x qwc=%u mode=%u copied=%u dst0=0x%08x\n",
                                     gn, fromSpr ? "FROM" : "TO", madr, sadr, qwc, mode, copied, dst0);
                    }
                }
                return true;
            }

            // PHASE F30: chcr1 probe (VIF1 DMA channel = 0x10009000)
            if (phaseDiagnosticsEnabled() && channelBase == 0x10009000)
            {
                static std::atomic<uint32_t> s_f30Chcr1{0};
                const uint32_t n = s_f30Chcr1.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 12u || (n % 64u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[F30:chcr1] n=%u chcr=0x%08x madr=0x%08x qwc=%u mode=%u\n",
                                 n, value, madr, qwc, (value >> 2) & 0x3u);
                }
            }
            if (f50_12_trace_enabled() && channelBase == 0x10009000)
            {
                static std::atomic<uint32_t> s_f512VifDma{0};
                const uint32_t n = s_f512VifDma.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 96u || (n % 512u) == 0u)
                {
                    const uint32_t tadr = m_ioRegisters[channelBase + 0x30];
                    const uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    const uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    std::fprintf(stderr,
                                 "[F512:vifdma] n=%u chcr=0x%08x madr=0x%08x qwc=%u tadr=0x%08x asr0=0x%08x asr1=0x%08x mode=%u tie=%u\n",
                                 n, value, madr, qwc, tadr, asr0, asr1,
                                 (value >> 2) & 0x3u,
                                 (value & (1u << 7)) ? 1u : 0u);
                }
            }

            if ((channelBase == 0x1000A000 || channelBase == 0x10009000) && m_gsVRAM)
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000)
                        m_pendingVif1Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    const int kMaxChainTags = 4096;
                    std::vector<uint8_t> chainBuf;
                    const bool g136Enabled = g136_trace_enabled() && channelBase == 0x10009000u;
                    const uint32_t g136Packet = g136_target_packet();
                    const uint32_t g136Bulk = g136_target_bulk();
                    bool g136ChainArmed = false;
                    uint32_t g136Seq = 0u;

                    auto g136TryArm = [&]() -> bool
                    {
                        if (!g136Enabled)
                            return false;
                        if (g136ChainArmed)
                            return true;
                        const uint32_t n = s_g136ArmedChains.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        const uint32_t limit = g136_trace_limit();
                        if (limit != 0u && n > limit)
                            return false;
                        g136Seq = g_dc2G136TraceSeq.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        g136ChainArmed = true;
                        return true;
                    };

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0; 
                        src = translateAddress(srcAddr); 
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }
                        // PR131: wrap around the physical buffer (DMAC ring) instead
                        // of truncating, so a chain that crosses the RAM/SPR boundary
                        // is reassembled rather than dropped.
                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVif1TagHi = [&](uint32_t dmaTagAddr, uint32_t id, uint32_t qwCount, bool appendZeroTagHi) -> bool
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(dmaTagAddr);
                        tagPhys = translateAddress(dmaTagAddr);
                        
                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return false;

                        uint64_t tagHi = 0u;
                        std::memcpy(&tagHi, localBase + tagPhys + 8u, sizeof(tagHi));
                        if (!appendZeroTagHi && tagHi == 0u)
                            return false;

                        // VIF1 packet helpers embed 8 bytes of VIF stream in the DMAtag's upper half.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                        if (phaseDiagnosticsEnabled())
                        {
                            static std::atomic<uint32_t> s_f31VifChainTagHi{0};
                            const uint32_t n = s_f31VifChainTagHi.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (n <= 64u || (n % 512u) == 0u)
                            {
                                std::fprintf(stderr,
                                             "[F31:vif-chain-taghi] n=%u id=%u qwc=%u force=%u hi=0x%016llx\n",
                                             n, id, qwCount, appendZeroTagHi ? 1u : 0u,
                                             static_cast<unsigned long long>(tagHi));
                            }
                        }
                        return true;
                    };

                    int tagsProcessed = 0;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        ++tagsProcessed;

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (f50_12_trace_enabled() && channelBase == 0x10009000u)
                        {
                            static std::atomic<uint32_t> s_f512VifChain{0};
                            const uint32_t n = s_f512VifChain.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (n <= 192u || (n % 1024u) == 0u)
                            {
                                uint64_t tagHi = 0u;
                                std::memcpy(&tagHi, tp + 8u, sizeof(tagHi));
                                std::fprintf(stderr,
                                             "[F512:vifchain] n=%u tagIndex=%d tagAddr=0x%08x phys=0x%08x spr=%u id=%u qwc=%u addr=0x%08x dataAddr=0x%08x nextTag=0x%08x payload=%u irq=%u end=%u hi=0x%016llx\n",
                                             n, tagsProcessed, currentTagAddr, physTag, tagInSPR ? 1u : 0u,
                                             id, tagQwc, addr, dataAddr, tagAddr,
                                             hasPayload ? 1u : 0u, irq ? 1u : 0u, endChain ? 1u : 0u,
                                             static_cast<unsigned long long>(tagHi));
                            }
                        }

                        if (g136Enabled)
                        {
                            const bool packetHit =
                                g136_addr_matches(currentTagAddr, g136Packet) ||
                                g136_addr_matches(dataAddr, g136Packet) ||
                                g136_addr_matches(addr, g136Packet);
                            const bool bulkHit =
                                g136_addr_matches(currentTagAddr, g136Bulk) ||
                                g136_addr_matches(dataAddr, g136Bulk) ||
                                g136_addr_matches(addr, g136Bulk);
                            if ((packetHit || bulkHit) && g136TryArm())
                            {
                                uint64_t tagHi = 0u;
                                std::memcpy(&tagHi, tp + 8u, sizeof(tagHi));
                                std::fprintf(stderr,
                                             "[G136:vifchain] seq=%u tagIndex=%d tagAddr=0x%08x id=%u qwc=%u addr=0x%08x dataAddr=0x%08x nextTag=0x%08x hitPacket=%u hitBulk=%u hi=0x%016llx\n",
                                             g136Seq, tagsProcessed, currentTagAddr, id, tagQwc, addr, dataAddr,
                                             tagAddr, packetHit ? 1u : 0u, bulkHit ? 1u : 0u,
                                             static_cast<unsigned long long>(tagHi));

                                if (hasPayload && tagQwc != 0u)
                                {
                                    try
                                    {
                                        const bool dataScratch = isScratchpad(dataAddr);
                                        const uint32_t physData = translateAddress(dataAddr);
                                        const uint8_t *dataBase = dataScratch ? m_scratchpad : m_rdram;
                                        const uint32_t dataMax = dataScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                        if (physData < dataMax)
                                        {
                                            const uint32_t available = dataMax - physData;
                                            const uint32_t payloadBytes = std::min<uint32_t>(
                                                static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(tagQwc) * 16ull, 0xFFFFFFFFull)),
                                                available);
                                            g136_log_qwords(dataBase + physData, payloadBytes, tagQwc,
                                                           "srcqw", g136Seq, dataAddr);
                                        }
                                    }
                                    catch (...)
                                    {
                                        std::fprintf(stderr,
                                                     "[G136:srcqw] seq=%u base=0x%x unreadable\n",
                                                     g136Seq, dataAddr);
                                    }
                                }
                            }
                        }

                        if (hasPayload)
                        {
                            const bool vif1Chain = channelBase == 0x10009000u;
                            const bool compactVif1LocalPayload =
                                vif1Chain && (id == 1u || id == 2u || id == 5u || id == 6u || id == 7u);
                            if (vif1Chain)
                            {
                                appendVif1TagHi(currentTagAddr, id, tagQwc, compactVif1LocalPayload);
                                appendData(dataAddr, tagQwc);
                            }
                            else
                                appendData(dataAddr, tagQwc);
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            appendVif1TagHi(currentTagAddr, id, tagQwc, false);
                        }
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }

                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        if (g136ChainArmed && channelBase == 0x10009000u)
                        {
                            s_g136PendingChains.fetch_add(1u, std::memory_order_relaxed);
                            s_g136PendingSeq.store(g136Seq, std::memory_order_relaxed);
                            std::fprintf(stderr,
                                         "[G136:arm] seq=%u streamBytes=%zu packet=0x%x bulk=0x%x pending=%u\n",
                                         g136Seq, chainBuf.size(), g136Packet, g136Bulk,
                                         s_g136PendingChains.load(std::memory_order_relaxed));
                            g136_log_qwords(chainBuf.data(),
                                           static_cast<uint32_t>(std::min<size_t>(chainBuf.size(), 0xFFFFFFFFu)),
                                           static_cast<uint32_t>(chainBuf.size() / 16u),
                                           "streamqw", g136Seq, 0u);
                        }
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            uint32_t g136Seq = 0u;
            if (g136_trace_enabled())
            {
                uint32_t pending = s_g136PendingChains.load(std::memory_order_relaxed);
                while (pending != 0u &&
                       !s_g136PendingChains.compare_exchange_weak(pending, pending - 1u,
                                                                  std::memory_order_relaxed,
                                                                  std::memory_order_relaxed))
                {
                }
                if (pending != 0u)
                {
                    g136Seq = s_g136PendingSeq.exchange(0u, std::memory_order_relaxed);
                    if (g136Seq == 0u)
                        g136Seq = g_dc2G136TraceSeq.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    g_dc2G136TraceActive.store(g136Seq, std::memory_order_relaxed);
                    std::fprintf(stderr,
                                 "[G136:process] seq=%u streamBytes=%zu\n",
                                 g136Seq, p.chainData.size());
                }
            }
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
            if (g136Seq != 0u)
                g_dc2G136TraceActive.store(0u, std::memory_order_relaxed);
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    if (m_gifArbiter)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

// ---------------------------------------------------------------------------
// G138: title-scoped GS-stream dump (default OFF, DC2_G138_GSDUMP=<path>).
// Writes every submitted GIF packet into a minimal PCSX2-v9-shaped .gs container
// (44-byte header + 0x2000 zero "priv regs" + type-0 transfer records) so the
// existing tools/g1xx_hw_*.py parsers can consume the RUNNER stream directly and
// A/B it against ref/dumps/new_game_via_debug.gs. PATH1 records are preceded by
// a synthetic 32-byte A+D NOP record carrying the VU packer PC (reg addr 0x0f is
// a GS NOP, ignored by the old parsers, decoded by tools/g138_hw_slice.py).
// Bounded by DC2_G138_GSDUMP_MAX records (default 200000).
namespace
{
    struct Dc2G138GsDump
    {
        FILE *file = nullptr;
        uint32_t records = 0u;
        uint32_t maxRecords = 200000u;
        bool closed = false;
    };

    Dc2G138GsDump &g138Dump()
    {
        static Dc2G138GsDump d;
        return d;
    }

    const char *g138DumpPath()
    {
        static const char *path = std::getenv("DC2_G138_GSDUMP");
        return path;
    }

    void g138WriteRecord(uint8_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        Dc2G138GsDump &d = g138Dump();
        if (d.closed)
            return;
        if (!d.file)
        {
            const char *p = g138DumpPath();
            if (!p || !p[0])
            {
                d.closed = true;
                return;
            }
            static const uint32_t maxEnv = []() -> uint32_t {
                const char *e = std::getenv("DC2_G138_GSDUMP_MAX");
                if (!e)
                    return 200000u;
                const long v = std::strtol(e, nullptr, 0);
                return (v > 0) ? static_cast<uint32_t>(v) : 200000u;
            }();
            d.maxRecords = maxEnv;
            d.file = std::fopen(p, "wb");
            if (!d.file)
            {
                std::fprintf(stderr, "[G138:gsdump] fopen failed: %s\n", p);
                d.closed = true;
                return;
            }
            // 8-byte magic + 9 u32 header fields (sv,ss,so,sz,crc,sw,sh,sho,shs).
            // sho=36/shs=0 => state offset 44, ss=0 => records begin at 44+0x2000,
            // matching what parse_header() in the g1xx tools computes.
            uint8_t hdr[44] = {};
            std::memcpy(hdr, "G138GSD\0", 8);
            uint32_t fields[9] = {1u, 0u, 0u, 0u, 0u, 640u, 448u, 36u, 0u};
            std::memcpy(hdr + 8, fields, sizeof(fields));
            std::fwrite(hdr, 1, sizeof(hdr), d.file);
            static const uint8_t zeros[0x800] = {};
            for (int i = 0; i < 4; ++i)
                std::fwrite(zeros, 1, sizeof(zeros), d.file);
            std::fprintf(stderr, "[G138:gsdump] writing %s (max %u records)\n", p, d.maxRecords);
        }
        if (d.records >= d.maxRecords)
        {
            std::fclose(d.file);
            d.file = nullptr;
            d.closed = true;
            std::fprintf(stderr, "[G138:gsdump] record cap %u reached, file closed\n", d.maxRecords);
            return;
        }
        const uint8_t type = 0u;
        const int32_t size = static_cast<int32_t>(sizeBytes);
        std::fwrite(&type, 1, 1, d.file);
        std::fwrite(&path, 1, 1, d.file);
        std::fwrite(&size, 4, 1, d.file);
        std::fwrite(data, 1, sizeBytes, d.file);
        ++d.records;
        if ((d.records & 0x3FFu) == 0u)
            std::fflush(d.file);
    }

    void g138DumpSubmit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!g138DumpPath())
            return;
        if (!g_dc2TitleRockScope.load(std::memory_order_relaxed))
            return;
        // PCSX2 dump path byte convention: 0=P1(old) 1=P2 2=P3 3=P1(new).
        uint8_t path = 2u;
        if (pathId == GifPathId::Path1)
            path = 3u;
        else if (pathId == GifPathId::Path2)
            path = 1u;
        if (pathId == GifPathId::Path1)
        {
            const uint32_t pc = g_dc2G138XgPc.load(std::memory_order_relaxed);
            uint8_t marker[32] = {};
            const uint64_t tagLo = 1ull | (1ull << 15) | (1ull << 60); // nloop=1 eop=1 nreg=1
            const uint64_t tagHi = 0x0Eull;                            // PACKED A+D
            const uint64_t adData = pc;
            const uint64_t adAddr = 0x0Full;                           // GS NOP register
            std::memcpy(marker + 0, &tagLo, 8);
            std::memcpy(marker + 8, &tagHi, 8);
            std::memcpy(marker + 16, &adData, 8);
            std::memcpy(marker + 24, &adAddr, 8);
            g138WriteRecord(path, marker, sizeof(marker));
        }
        g138WriteRecord(path, data, sizeBytes);
    }
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    G146PerfScope g146Scope(g_g146GifSubmitNs);

    if (!data || sizeBytes < 16)
        return;

    g138DumpSubmit(pathId, data, sizeBytes);

    if (vu1_trace_enabled())
    {
        const int pid = (pathId == GifPathId::Path1) ? 1 : (pathId == GifPathId::Path2) ? 2 : 3;
        f517_scan_submit(pid, data, sizeBytes);
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (isGsPrivReg(address))
    {
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint32_t off = address & 7u;
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }
    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000000 && address < 0x10000100)
        {
            if ((address & 0xF) == 0x00)
            {
                return 0;
            }
        }

        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
