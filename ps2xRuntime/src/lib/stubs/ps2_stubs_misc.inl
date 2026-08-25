// ===== G657 P1: 375 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
// Phase B — ISO bridge: defined in ps2_runtime.cpp, inside namespace ps2_stubs.
bool isoFindFileForStubs(const char *isoPath, uint32_t *lbaOut, uint32_t *sizeOut);


































void iopGetArea(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, kIopHeapBase);
}




void Pad_init(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnS32(ctx, 1);
}

void Pad_set(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnS32(ctx, 1);
}


namespace
{
    std::mutex g_mcStateMutex;
    int32_t g_mcNextFd = 1;
    int32_t g_mcLastResult = 0;
}






































































static void writeU32AtGp(uint8_t *rdram, uint32_t gp, int32_t offset, uint32_t value)
{
    const uint32_t addr = gp + static_cast<uint32_t>(offset);
    if (uint8_t *p = getMemPtr(rdram, addr))
        *reinterpret_cast<uint32_t *>(p) = value;
}



static constexpr uint32_t kFontBase     = 0x176148u;
static constexpr uint32_t kFontEntrySz = 0x24u;















// G374: the sceMc* family lives in src/lib/Kernel/Stubs/MemoryCard.cpp (host-backed,
// real files under mc0/). The duplicate no-op copies that used to sit here were linked
// ambiguously via /FORCE:MULTIPLE and are deleted. Do not re-add them here.





static void mpegGuestWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    if (uint8_t *p = getMemPtr(rdram, addr))
        *reinterpret_cast<uint32_t *>(p) = value;
}
static void mpegGuestWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    if (uint8_t *p = getMemPtr(rdram, addr))
    {
        *reinterpret_cast<uint32_t *>(p) = static_cast<uint32_t>(value);
        *reinterpret_cast<uint32_t *>(p + 4) = static_cast<uint32_t>(value >> 32);
    }
}




























































namespace
{
    struct Ps2SifDmaTransfer
    {
        uint32_t src = 0;
        uint32_t dest = 0;
        int32_t size = 0;
        int32_t attr = 0;
    };
    static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

    std::mutex g_sifDmaTransferMutex;
    uint32_t g_nextSifDmaTransferId = 1u;
    std::mutex g_sifCmdStateMutex;
    std::unordered_map<uint32_t, uint32_t> g_sifRegs;
    std::unordered_map<uint32_t, uint32_t> g_sifSregs;
    std::unordered_map<uint32_t, uint32_t> g_sifCmdHandlers;
    uint32_t g_sifCmdBuffer = 0u;
    uint32_t g_sifSysCmdBuffer = 0u;
    bool g_sifCmdInitialized = false;
    uint32_t g_sifGetRegLogCount = 0u;
    uint32_t g_sifSetRegLogCount = 0u;

    constexpr uint32_t kSifRegBootStatus = 0x4u;
    constexpr uint32_t kSifRegMainAddr = 0x80000000u;
    constexpr uint32_t kSifRegSubAddr = 0x80000001u;
    constexpr uint32_t kSifRegMsCom = 0x80000002u;
    constexpr uint32_t kSifBootReadyMask = 0x00020000u;

    void seedDefaultSifRegsLocked()
    {
        g_sifRegs.clear();
        g_sifSregs.clear();
        g_sifCmdHandlers.clear();
        g_sifCmdBuffer = 0u;
        g_sifSysCmdBuffer = 0u;
        g_sifCmdInitialized = false;
        g_sifGetRegLogCount = 0u;
        g_sifSetRegLogCount = 0u;

        g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
        g_sifRegs[kSifRegMainAddr] = 0u;
        g_sifRegs[kSifRegSubAddr] = 0u;
        g_sifRegs[kSifRegMsCom] = 0u;
    }

    bool shouldTraceSifReg(uint32_t reg)
    {
        switch (reg)
        {
        case 0x2u:
        case 0x4u:
        case 0x80000000u:
        case 0x80000001u:
        case 0x80000002u:
            return true;
        default:
            return false;
        }
    }

    struct SifStateInitializer
    {
        SifStateInitializer()
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            seedDefaultSifRegsLocked();
        }
    } g_sifStateInitializer;

    uint32_t allocateSifDmaTransferId()
    {
        std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
        uint32_t id = g_nextSifDmaTransferId++;
        if (id == 0u)
        {
            id = g_nextSifDmaTransferId++;
        }
        return id;
    }

    bool isCopyableGuestAddress(uint32_t addr)
    {
        if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
        {
            return true;
        }

        if (addr < 0x20000000u)
        {
            return true;
        }

        if (addr >= 0x20000000u && addr < 0x40000000u)
        {
            return true;
        }

        if (addr >= 0x80000000u && addr < 0xC0000000u)
        {
            return true;
        }

        return false;
    }

    bool canCopyGuestByteRange(const uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
    {
        if (!rdram)
        {
            return false;
        }

        if (sizeBytes == 0u)
        {
            return true;
        }

        for (uint32_t i = 0u; i < sizeBytes; ++i)
        {
            const uint32_t srcByteAddr = srcAddr + i;
            const uint32_t dstByteAddr = dstAddr + i;

            if (!isCopyableGuestAddress(srcByteAddr) || !isCopyableGuestAddress(dstByteAddr))
            {
                return false;
            }

            const uint8_t *src = getConstMemPtr(rdram, srcByteAddr);
            const uint8_t *dst = getConstMemPtr(rdram, dstByteAddr);
            if (!src || !dst)
            {
                return false;
            }
        }

        return true;
    }

    bool copyGuestByteRange(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
    {
        if (!canCopyGuestByteRange(rdram, dstAddr, srcAddr, sizeBytes))
        {
            return false;
        }

        if (sizeBytes == 0u)
        {
            return true;
        }

        const uint64_t srcBegin = srcAddr;
        const uint64_t srcEnd = srcBegin + static_cast<uint64_t>(sizeBytes);
        const uint64_t dstBegin = dstAddr;
        const bool copyBackward = (dstBegin > srcBegin) && (dstBegin < srcEnd);

        if (copyBackward)
        {
            for (uint32_t i = sizeBytes; i > 0u; --i)
            {
                const uint32_t index = i - 1u;
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + index);
                uint8_t *dst = getMemPtr(rdram, dstAddr + index);
                if (!src || !dst)
                {
                    return false;
                }
                *dst = *src;
            }
            return true;
        }

        for (uint32_t i = 0; i < sizeBytes; ++i)
        {
            const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
            uint8_t *dst = getMemPtr(rdram, dstAddr + i);
            if (!src || !dst)
            {
                return false;
            }
            *dst = *src;
        }
        return true;
    }
}









































































































































namespace
{
    bool readVuVec4f(uint8_t *rdram, uint32_t addr, float (&out)[4])
    {
        const uint8_t *ptr = getConstMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }
        std::memcpy(out, ptr, sizeof(out));
        return true;
    }

    bool writeVuVec4f(uint8_t *rdram, uint32_t addr, const float (&in)[4])
    {
        uint8_t *ptr = getMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }
        std::memcpy(ptr, in, sizeof(in));
        return true;
    }

    bool readVuVec4i(uint8_t *rdram, uint32_t addr, int32_t (&out)[4])
    {
        const uint8_t *ptr = getConstMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }
        std::memcpy(out, ptr, sizeof(out));
        return true;
    }

    bool writeVuVec4i(uint8_t *rdram, uint32_t addr, const int32_t (&in)[4])
    {
        uint8_t *ptr = getMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }
        std::memcpy(ptr, in, sizeof(in));
        return true;
    }
}























































void setMpegCompatLayout(const PS2MpegCompatLayout &layout)
{
}

