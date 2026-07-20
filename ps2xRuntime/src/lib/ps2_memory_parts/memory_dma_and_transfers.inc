                if (fromSpr && armedG217 != 0u &&
                    (madr & 0x0fffffffu) == armedG217 &&
                    g_dc2G217HeadDmaPacket.compare_exchange_strong(
                        armedG217, 0u,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    const uint32_t headKind =
                        g_dc2G217HeadDmaKind.load(std::memory_order_relaxed);
                    const uint32_t r = madr & PS2_RAM_MASK;
                    const uint32_t recentSlot = s_g217RecentDmaWrite.fetch_add(
                        1u, std::memory_order_relaxed) % G217_RECENT_DMA_COUNT;
                    s_g217RecentDma[recentSlot].store(
                        madr & 0x0fffffffu, std::memory_order_release);
                    uint32_t taggedSelectors = 0u;
                    if (g217TagVuEnabled() && copied >= 20u)
                    {
                        const uint32_t tagBit = (headKind & 1u) ? 0x40000000u :
                                                (headKind & 2u) ? 0x20000000u : 0u;
                        uint32_t *words = reinterpret_cast<uint32_t *>(m_rdram + r);
                        const uint32_t wordCount = copied / 4u;
                        for (uint32_t i = 0u; tagBit != 0u && i + 1u < wordCount; ++i)
                        {
                            if (words[i] == 0x6c010026u)
                            {
                                words[i + 1u] |= tagBit;
                                ++taggedSelectors;
                            }
                        }
                    }
                    uint32_t first[4]{};
                    uint32_t last[4]{};
                    if (copied >= 16u && r + copied <= PS2_RAM_SIZE)
                    {
                        std::memcpy(first, m_rdram + r, sizeof(first));
                        std::memcpy(last, m_rdram + r + copied - sizeof(last), sizeof(last));
                    }
                    std::fprintf(stderr,
                        "[G217:headdma] self=0x%08x kind=%u madr=0x%08x sadr=0x%08x qwc=%u copied=%u tagged=%u first=%08x,%08x,%08x,%08x last=%08x,%08x,%08x,%08x\n",
                        g_dc2G217HeadDmaSelf.load(std::memory_order_relaxed),
                        headKind, madr, sadr, qwc, copied, taggedSelectors,
                        first[0], first[1], first[2], first[3],
                        last[0], last[1], last[2], last[3]);
                    std::fflush(stderr);
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
                    // G217: MAP-4's character RTT legitimately exceeds 4096 nested/local
                    // DMAtags.  The old guard truncated the chain at exactly the point where
                    // the static-view head/cap packets were linked; rotating the model moved
                    // them a few tags earlier and made them appear.  Keep a generous runaway
                    // guard, but do not discard valid packets solely because the scene is
                    // complex.
                    const int kMaxChainTags = 16384;
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

                    // G217: the exact head/cap render-info DMA can optionally stamp an
                    // otherwise-unused selector bit immediately after its V4_32 UNPACK.
                    // Look for that stamp in each source payload while assembling the
                    // VIF1 chain.  This distinguishes "the packet was never linked" from
                    // a later VIF/VU unpacking failure without changing default behavior.
                    auto g217TraceTaggedPayload = [&](uint32_t srcAddr, uint32_t qwCount,
                                                      uint32_t dmaTagAddr, uint32_t dmaId)
                    {
                        if (channelBase != 0x10009000u ||
                            !g217TagVuEnabled() || qwCount == 0u)
                            return;

                        try
                        {
                            const bool scratch = isScratchpad(srcAddr);
                            const uint32_t phys = translateAddress(srcAddr);
                            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
                            const uint32_t maxSize = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                            const uint64_t requested64 = static_cast<uint64_t>(qwCount) * 16ull;
                            if (phys >= maxSize || requested64 < 8ull)
                                return;
                            const uint32_t bytes = std::min<uint32_t>(
                                static_cast<uint32_t>(std::min<uint64_t>(requested64, 0xffffffffull)),
                                maxSize - phys);
                            const uint32_t wordCount = bytes / 4u;
                            const uint32_t *words = reinterpret_cast<const uint32_t *>(base + phys);
                            for (uint32_t i = 0u; i + 1u < wordCount; ++i)
                            {
                                if (words[i] != 0x6c010026u)
                                    continue;
                                const uint32_t selector = words[i + 1u];
                                const uint32_t exactBits = selector & 0x60000000u;
                                if (exactBits == 0u)
                                    continue;
                                static std::atomic<uint32_t> s_g217VifConsume{0u};
                                const uint32_t n = s_g217VifConsume.fetch_add(
                                    1u, std::memory_order_relaxed) + 1u;
                                if (n <= 192u || (n % 512u) == 0u)
                                {
                                    std::fprintf(stderr,
                                        "[G217:vifconsume] n=%u tagAddr=0x%08x id=%u src=0x%08x qwc=%u word=%u selector=0x%08x exact=0x%08x spr=%u\n",
                                        n, dmaTagAddr, dmaId, srcAddr, qwCount, i,
                                        selector, exactBits, scratch ? 1u : 0u);
                                    std::fflush(stderr);
                                }
                            }
                        }
                        catch (...)
                        {
                        }
                    };

                    auto g217RecentExactDma = [&](uint32_t candidate) -> bool
                    {
                        if (!g217TagVuEnabled())
                            return false;
                        const uint32_t phys = candidate & 0x0fffffffu;
                        if (phys == 0u)
                            return false;
                        for (uint32_t i = 0u; i < G217_RECENT_DMA_COUNT; ++i)
                            if (s_g217RecentDma[i].load(std::memory_order_acquire) == phys)
                                return true;
                        return false;
                    };

                    auto g217RecentDirectPacket = [&](uint32_t candidate) -> bool
                    {
                        if (!g217TagVuEnabled())
                            return false;
                        const uint32_t phys = candidate & 0x0fffffffu;
                        if (phys == 0u)
                            return false;
                        for (uint32_t i = 0u; i < 128u; ++i)
                            if (g_dc2G217DirectPackets[i].load(std::memory_order_acquire) == phys)
                                return true;
                        return false;
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

                        if (channelBase == 0x10009000u)
                        {
                            const bool hitTag = g217RecentExactDma(currentTagAddr);
                            const bool hitAddr = g217RecentExactDma(addr);
                            const bool hitData = g217RecentExactDma(dataAddr);
                            if (hitTag || hitAddr || hitData)
                            {
                                static std::atomic<uint32_t> s_g217VifLink{0u};
                                const uint32_t n = s_g217VifLink.fetch_add(
                                    1u, std::memory_order_relaxed) + 1u;
                                if (n <= 256u || (n % 512u) == 0u)
                                {
                                    std::fprintf(stderr,
                                        "[G217:viflink] n=%u tagIndex=%d tagAddr=0x%08x id=%u qwc=%u addr=0x%08x dataAddr=0x%08x next=0x%08x hitTag=%u hitAddr=%u hitData=%u\n",
                                        n, tagsProcessed, currentTagAddr, id, tagQwc, addr,
                                        dataAddr, tagAddr, hitTag ? 1u : 0u,
                                        hitAddr ? 1u : 0u, hitData ? 1u : 0u);
                                    std::fflush(stderr);
                                }
                            }

                            const bool directTag = g217RecentDirectPacket(currentTagAddr);
                            const bool directAddr = g217RecentDirectPacket(addr);
                            const bool directData = g217RecentDirectPacket(dataAddr);
                            if (directTag || directAddr || directData)
                            {
                                static std::atomic<uint32_t> s_g217VifDirect{0u};
                                const uint32_t n = s_g217VifDirect.fetch_add(
                                    1u, std::memory_order_relaxed) + 1u;
                                if (n <= 256u || (n % 512u) == 0u)
                                {
                                    std::fprintf(stderr,
                                        "[G217:vifdirect] n=%u tagIndex=%d tagAddr=0x%08x id=%u qwc=%u addr=0x%08x dataAddr=0x%08x next=0x%08x hitTag=%u hitAddr=%u hitData=%u\n",
                                        n, tagsProcessed, currentTagAddr, id, tagQwc, addr,
                                        dataAddr, tagAddr, directTag ? 1u : 0u,
                                        directAddr ? 1u : 0u, directData ? 1u : 0u);
                                    std::fflush(stderr);
                                }
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
                                g217TraceTaggedPayload(dataAddr, tagQwc, currentTagAddr, id);
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

                    // G227: read-only high-water-mark tracker for the VIF1 chain-tag walker
                    // (kMaxChainTags=16384 since G217). Confirms whether a given scene's chain
                    // ever approaches the cutoff, without needing to re-derive G217's evidence
                    // trail from scratch. Env: DC2_TRACE_G227.
                    if (channelBase == 0x10009000u && g227TraceEnabled())
                    {
                        static std::atomic<int> s_g227MaxTags{0};
                        int prevMax = s_g227MaxTags.load(std::memory_order_relaxed);
                        while (tagsProcessed > prevMax &&
                               !s_g227MaxTags.compare_exchange_weak(prevMax, tagsProcessed, std::memory_order_relaxed))
                        {
                        }
                        if (tagsProcessed > prevMax)
                        {
                            std::fprintf(stderr, "[G227:viftags] new max tagsProcessed=%d (limit=%d)\n",
                                         tagsProcessed, kMaxChainTags);
                        }
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
