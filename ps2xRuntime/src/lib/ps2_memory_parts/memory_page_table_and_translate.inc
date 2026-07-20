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
// G234: cross-TU gate for deferred-start dumping (see g138DumpSubmit); the present
// loop in ps2_runtime.cpp flips it once DC2_G138_GSDUMP_AT_TICK is reached.
std::atomic<bool> g_dc2G138DumpGateOpen{true};
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
        // G234: optional deferred-start gate (DC2_G138_GSDUMP_AT_TICK=<present tick>).
        // The present loop opens the gate once the requested host tick is reached, so a
        // long scripted route can dump ONLY the scene of interest instead of the whole
        // session. Gate defaults open (old behavior) when the env is absent.
        if (!g_dc2G138DumpGateOpen.load(std::memory_order_relaxed))
            return;
        // G194: DC2_G138_GSDUMP_ALL=1 lifts the title-scope gate so non-title routes
        // (town/edit map, dungeon) can be packet-dumped too. Default keeps the old
        // title-scoped behavior.
        static const bool dumpAllScopes = []() {
            const char *e = std::getenv("DC2_G138_GSDUMP_ALL");
            return e && e[0] && !(e[0] == '0' && !e[1]);
        }();
        if (!dumpAllScopes && !g_dc2TitleRockScope.load(std::memory_order_relaxed))
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
