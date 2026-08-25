// ===== G657 P1: 45 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
namespace
{
    uint32_t sanitizeMemTransferSize(uint32_t size, const char *op)
    {
        constexpr uint32_t kMaxTransfer = PS2_RAM_SIZE;
        if (size <= kMaxTransfer)
        {
            return size;
        }

        static std::mutex s_warnMutex;
        static std::unordered_map<std::string, uint32_t> s_warnCounts;
        uint32_t warnCount = 0u;
        {
            std::lock_guard<std::mutex> lock(s_warnMutex);
            warnCount = ++s_warnCounts[op ? op : "memop"];
        }
        if (warnCount <= 16u)
        {
            std::cerr << "[" << (op ? op : "memop") << "] size clamp from 0x"
                      << std::hex << size << " to 0x" << kMaxTransfer
                      << std::dec << std::endl;
        }
        return kMaxTransfer;
    }

    uint32_t guestContiguousBytes(uint32_t guestAddr)
    {
        uint32_t offset = 0u;
        bool scratch = false;
        if (!ps2ResolveGuestPointer(guestAddr, offset, scratch))
        {
            return 0u;
        }
        if (scratch)
        {
            return (offset < PS2_SCRATCHPAD_SIZE) ? (PS2_SCRATCHPAD_SIZE - offset) : 0u;
        }
        return (offset < PS2_RAM_SIZE) ? (PS2_RAM_SIZE - offset) : 0u;
    }
}













































