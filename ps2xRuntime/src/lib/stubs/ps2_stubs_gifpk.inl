// ===== G657 P1: 25 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
// sceGifPk* / sceVif1Pk* packet-builder library implementations.
//
// These mirror the packet context layout used by SCUS_972.13's libgraph/libdma
// helpers.  The game passes context[1] to sceDmaSend as the DMA chain start, so
// the context order must be:
//   +0 current write pointer
//   +4 buffer start
//   +8 open DMA tag pointer
//   +12 open GIFtag pointer for sceGifPk, or open DIRECT VIFcode for sceVif1Pk
//   +20 open GIFtag pointer inside a VIF1 DIRECT block
//
// VIF1 source-chain tags are compact: the DMA tag occupies the low 64 bits of
// the first qword, and the upper 64 bits may contain the first two VIF words.

namespace
{
    constexpr uint32_t kPkPtr = 0u;
    constexpr uint32_t kPkBuf = 4u;
    constexpr uint32_t kPkDmaTag = 8u;
    constexpr uint32_t kGifPkGifTag = 12u;
    constexpr uint32_t kVif1PkDirect = 12u;
    constexpr uint32_t kVif1PkGifTag = 20u;

    inline uint32_t pkRead32(uint8_t *rdram, uint32_t base, uint32_t field)
    {
        return Ps2FastRead32(rdram, base + field);
    }

    inline void pkWrite32(uint8_t *rdram, uint32_t base, uint32_t field, uint32_t v)
    {
        Ps2FastWrite32(rdram, base + field, v);
    }

    inline void pkPatchNloop(uint8_t *rdram, uint32_t ps2Addr, uint32_t nloop)
    {
        uint64_t qw = Ps2FastRead64(rdram, ps2Addr);
        qw = (qw & ~static_cast<uint64_t>(0x7FFFu)) | (static_cast<uint64_t>(nloop) & 0x7FFFu);
        Ps2FastWrite64(rdram, ps2Addr, qw);
    }

    inline void pkWriteGifTag(uint8_t *rdram, uint32_t pktPtr, uint64_t tagLo, uint64_t tagHi)
    {
        Ps2FastWrite64(rdram, pktPtr, tagLo & ~static_cast<uint64_t>(0x7FFFu));
        Ps2FastWrite64(rdram, pktPtr + 8u, tagHi);
    }

    uint32_t pkAlignToQword(uint8_t *rdram, uint32_t ptr)
    {
        while ((ptr & 0xCu) != 0u)
        {
            Ps2FastWrite32(rdram, ptr, 0u);
            ptr += 4u;
        }
        return ptr;
    }

    void pkTerminate(uint8_t *rdram, uint32_t c)
    {
        uint32_t ptr = pkAlignToQword(rdram, pkRead32(rdram, c, kPkPtr));
        const uint32_t dmaTag = pkRead32(rdram, c, kPkDmaTag);
        if (dmaTag != 0u && ptr >= dmaTag + 16u)
        {
            const uint32_t qwc = ((ptr - dmaTag) >> 4) - 1u;
            Ps2FastWrite32(rdram, dmaTag, Ps2FastRead32(rdram, dmaTag) + qwc);
        }

        pkWrite32(rdram, c, kPkDmaTag, 0u);
        pkWrite32(rdram, c, kPkPtr, ptr);
    }

    void pkCloseGifTag(uint8_t *rdram, uint32_t c, uint32_t tagField)
    {
        const uint32_t ptag = pkRead32(rdram, c, tagField);
        uint32_t ptr = pkRead32(rdram, c, kPkPtr);

        if (ptag != 0u && ptr >= ptag + 16u)
        {
            const uint64_t tagLo = Ps2FastRead64(rdram, ptag);
            uint32_t entries = ((ptr - ptag) >> 3) - 2u;
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 0x3u);

            if (flg != 1u)
                entries >>= 1;

            if (flg != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>(tagLo >> 60);
                if (nreg == 0u)
                    nreg = 16u;
                entries = (entries + nreg - 1u) / nreg;
            }

            pkPatchNloop(rdram, ptag, entries);
        }

        ptr = pkAlignToQword(rdram, ptr);
        pkWrite32(rdram, c, tagField, 0u);
        pkWrite32(rdram, c, kPkPtr, ptr);
    }

    void pkAlignWords(uint8_t *rdram, uint32_t c, uint32_t alignShift, uint32_t wordOffset)
    {
        uint32_t ptr = pkRead32(rdram, c, kPkPtr);
        const uint32_t bits = (alignShift + 2u) & 0x1Fu;
        const uint32_t mask = bits == 0u ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        uint32_t target = (ptr & ~mask) + wordOffset * 4u;
        if (target < ptr)
            target += mask + 1u;

        while (ptr < target)
        {
            Ps2FastWrite32(rdram, ptr, 0u);
            ptr += 4u;
            pkWrite32(rdram, c, kPkPtr, ptr);
        }
    }

    void pkOpenGifTag(uint8_t *rdram, R5900Context *ctx, uint32_t tagField)
    {
        const uint32_t c = getRegU32(ctx, 4);
        if (c == 0u)
            return;

        const uint64_t lo = static_cast<uint64_t>(GPR_U64(ctx, 5));
        const uint64_t hi = static_cast<uint64_t>(GPR_U64(ctx, 6));
        const uint32_t p = pkRead32(rdram, c, kPkPtr);
        pkWriteGifTag(rdram, p, lo, hi);
        pkWrite32(rdram, c, kPkPtr, p + 16u);
        pkWrite32(rdram, c, tagField, p);
    }

    void pkStartCnt(uint8_t *rdram, uint32_t c)
    {
        pkTerminate(rdram, c);
        const uint32_t p = pkRead32(rdram, c, kPkPtr);
        pkWrite32(rdram, c, kPkDmaTag, p);
        Ps2FastWrite32(rdram, p, 0x10000000u);
        Ps2FastWrite32(rdram, p + 4u, 0u);
        Ps2FastWrite32(rdram, p + 8u, 0u);
        Ps2FastWrite32(rdram, p + 12u, 0u);
        pkWrite32(rdram, c, kPkPtr, p + 16u);
    }

    void pkEmitRef(uint8_t *rdram, uint32_t c, uint32_t addr, uint32_t qwc)
    {
        pkTerminate(rdram, c);
        const uint32_t p = pkRead32(rdram, c, kPkPtr);
        Ps2FastWrite32(rdram, p, 0x30000000u | (qwc & 0xFFFFu));
        Ps2FastWrite32(rdram, p + 4u, addr & 0x9FFFFFFFu);
        Ps2FastWrite32(rdram, p + 8u, 0u);
        Ps2FastWrite32(rdram, p + 12u, 0u);
        pkWrite32(rdram, c, kPkPtr, p + 16u);
    }

    void pkEmitAd(uint8_t *rdram, uint32_t c, uint32_t reg, uint64_t value)
    {
        const uint32_t p = pkRead32(rdram, c, kPkPtr);
        Ps2FastWrite64(rdram, p, value);
        Ps2FastWrite32(rdram, p + 8u, reg);
        Ps2FastWrite32(rdram, p + 12u, 0u);
        pkWrite32(rdram, c, kPkPtr, p + 16u);
    }
}

// =============================================================================
// sceGifPk* - build GIF source-chain packets in a caller-supplied buffer
// =============================================================================













// =============================================================================
// sceVif1Pk* - build compact VIF1 source-chain packets
// =============================================================================













