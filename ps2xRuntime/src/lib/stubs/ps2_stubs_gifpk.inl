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

void sceGifPkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    const uint32_t b = getRegU32(ctx, 5);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkWrite32(rdram, c, kPkPtr, b);
    pkWrite32(rdram, c, kPkBuf, b);
    pkWrite32(rdram, c, kPkDmaTag, 0u);
    pkWrite32(rdram, c, kGifPkGifTag, 0u);
    setReturnU32(ctx, b);
}

void sceGifPkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t b = pkRead32(rdram, c, kPkBuf);
    pkWrite32(rdram, c, kPkPtr, b);
    pkWrite32(rdram, c, kPkDmaTag, 0u);
    pkWrite32(rdram, c, kGifPkGifTag, 0u);
    setReturnU32(ctx, b);
}

void sceGifPkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    setReturnU32(ctx, pkRead32(rdram, c, kPkPtr));
}

void sceGifPkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkDmaTag, p);
    Ps2FastWrite32(rdram, p, 0x70000000u | (getRegU32(ctx, 7) & 0x0FFFFFFFu));
    Ps2FastWrite32(rdram, p + 4u, 0u);
    Ps2FastWrite32(rdram, p + 8u, getRegU32(ctx, 5));
    Ps2FastWrite32(rdram, p + 12u, getRegU32(ctx, 6));
    pkWrite32(rdram, c, kPkPtr, p + 16u);
    setReturnU32(ctx, p + 16u);
}

void sceGifPkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkDmaTag, p);
    Ps2FastWrite32(rdram, p, 0x10000000u | (getRegU32(ctx, 7) & 0x0FFFFFFFu));
    Ps2FastWrite32(rdram, p + 4u, 0u);
    Ps2FastWrite32(rdram, p + 8u, getRegU32(ctx, 5));
    Ps2FastWrite32(rdram, p + 12u, getRegU32(ctx, 6));
    pkWrite32(rdram, c, kPkPtr, p + 16u);
    setReturnS32(ctx, 0);
}

void sceGifPkRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    const uint32_t addr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
    const uint32_t qwc = getRegU32(ctx, 6) & 0xFFFFu;
    Ps2FastWrite32(rdram, p, 0x30000000u | qwc);
    Ps2FastWrite32(rdram, p + 4u, addr);
    Ps2FastWrite32(rdram, p + 8u, getRegU32(ctx, 7));
    Ps2FastWrite32(rdram, p + 12u, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 16u);
    setReturnS32(ctx, 0);
}

void sceGifPkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    pkOpenGifTag(rdram, ctx, kGifPkGifTag);
    setReturnS32(ctx, 0);
}

void sceGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkCloseGifTag(rdram, c, kGifPkGifTag);
    setReturnS32(ctx, 0);
}

void sceGifPkAddGsData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint64_t data = static_cast<uint64_t>(GPR_U64(ctx, 5));
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    Ps2FastWrite64(rdram, p, data);
    pkWrite32(rdram, c, kPkPtr, p + 8u);
    setReturnS32(ctx, 0);
}

void sceGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint64_t data = static_cast<uint64_t>(GPR_U64(ctx, 6));
    const uint32_t addr = getRegU32(ctx, 5);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    Ps2FastWrite64(rdram, p, data);
    Ps2FastWrite32(rdram, p + 8u, addr);
    Ps2FastWrite32(rdram, p + 12u, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 16u);
    setReturnS32(ctx, 0);
}

void sceGifPkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    const uint32_t words = getRegU32(ctx, 5);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkPtr, p + words * 4u);
    setReturnU32(ctx, p);
}

void sceGifPkRefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t dbp = getRegU32(ctx, 5) & 0x3FFFu;
    const uint32_t dpsm = getRegU32(ctx, 6) & 0x3Fu;
    const uint32_t dbw = getRegU32(ctx, 7) & 0x3Fu;
    uint32_t srcAddr = getRegU32(ctx, 8);
    uint32_t remainingQwc = getRegU32(ctx, 9);
    const uint32_t dsax = getRegU32(ctx, 10) & 0x7FFu;
    const uint32_t dsay = getRegU32(ctx, 11) & 0x7FFu;
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t rrw = Ps2FastRead32(rdram, sp + 0x00u) & 0xFFFu;
    const uint32_t rrh = Ps2FastRead32(rdram, sp + 0x08u) & 0xFFFu;

    uint32_t chunks = 0u;
    {
        static std::atomic<uint32_t> s_f31RefLoadCount{0};
        const uint32_t n = s_f31RefLoadCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u || (n % 128u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:refload] n=%u c=0x%08x dbp=0x%x dbw=%u dpsm=0x%x src=0x%08x qwc=%u ds=(%u,%u) rr=(%u,%u)\n",
                         n, c, dbp, dbw, dpsm, srcAddr, remainingQwc, dsax, dsay, rrw, rrh);
        }
    }

    pkStartCnt(rdram, c);
    const uint32_t adTag = pkRead32(rdram, c, kPkPtr);
    Ps2FastWrite64(rdram, adTag, 1ull << 60);
    Ps2FastWrite64(rdram, adTag + 8u, 0x0Eu);
    pkWrite32(rdram, c, kPkPtr, adTag + 16u);
    pkWrite32(rdram, c, kGifPkGifTag, adTag);

    const uint64_t bitbltbuf =
        (static_cast<uint64_t>(dbp) << 32) |
        (static_cast<uint64_t>(dbw) << 48) |
        (static_cast<uint64_t>(dpsm) << 56);
    const uint64_t trxpos =
        (static_cast<uint64_t>(dsax) << 32) |
        (static_cast<uint64_t>(dsay) << 48);
    const uint64_t trxreg =
        static_cast<uint64_t>(rrw) |
        (static_cast<uint64_t>(rrh) << 32);

    pkEmitAd(rdram, c, 0x50u, bitbltbuf);
    pkEmitAd(rdram, c, 0x51u, trxpos);
    pkEmitAd(rdram, c, 0x52u, trxreg);
    pkEmitAd(rdram, c, 0x53u, 0u);
    pkCloseGifTag(rdram, c, kGifPkGifTag);

    while (remainingQwc != 0u)
    {
        const uint32_t chunkQwc = std::min<uint32_t>(remainingQwc, 0x7FFFu);
        pkStartCnt(rdram, c);

        const uint32_t imageTagAddr = pkRead32(rdram, c, kPkPtr);
        const bool finalChunk = (chunkQwc == remainingQwc);
        const uint64_t imageTag =
            static_cast<uint64_t>(chunkQwc & 0x7FFFu) |
            (finalChunk ? (1ull << 15) : 0ull) |
            (2ull << 58);
        Ps2FastWrite64(rdram, imageTagAddr, imageTag);
        Ps2FastWrite64(rdram, imageTagAddr + 8u, 0u);
        pkWrite32(rdram, c, kPkPtr, imageTagAddr + 16u);

        pkEmitRef(rdram, c, srcAddr, chunkQwc);
        srcAddr += chunkQwc * 16u;
        remainingQwc -= chunkQwc;
        ++chunks;
    }

    {
        static std::atomic<uint32_t> s_f31RefLoadDoneCount{0};
        const uint32_t n = s_f31RefLoadDoneCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u || (n % 128u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:refload.done] n=%u chunks=%u ptr=0x%08x\n",
                         n, chunks, pkRead32(rdram, c, kPkPtr));
        }
    }

    setReturnS32(ctx, 0);
}

// =============================================================================
// sceVif1Pk* - build compact VIF1 source-chain packets
// =============================================================================

void sceVif1PkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    const uint32_t b = getRegU32(ctx, 5);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkWrite32(rdram, c, kPkPtr, b);
    pkWrite32(rdram, c, kPkBuf, b);
    pkWrite32(rdram, c, kPkDmaTag, 0u);
    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    pkWrite32(rdram, c, kVif1PkGifTag, 0u);
    setReturnU32(ctx, b);
}

void sceVif1PkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t b = pkRead32(rdram, c, kPkBuf);
    pkWrite32(rdram, c, kPkPtr, b);
    pkWrite32(rdram, c, kPkDmaTag, 0u);
    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    pkWrite32(rdram, c, kVif1PkGifTag, 0u);
    setReturnU32(ctx, b);
}

void sceVif1PkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    setReturnU32(ctx, pkRead32(rdram, c, kPkPtr));
}

void sceVif1PkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkDmaTag, p);
    Ps2FastWrite32(rdram, p, 0x10000000u | (getRegU32(ctx, 5) & 0x0FFFFFFFu));
    Ps2FastWrite32(rdram, p + 4u, 0u);
    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 8u);
    setReturnS32(ctx, 0);
}

void sceVif1PkCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    const uint32_t target = getRegU32(ctx, 5);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkDmaTag, p);
    Ps2FastWrite32(rdram, p, 0x50000000u | (getRegU32(ctx, 6) & 0x0FFFFFFFu));
    Ps2FastWrite32(rdram, p + 4u, target & 0x9FFFFFFFu);
    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 8u);
    setReturnS32(ctx, 0);
}

void sceVif1PkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkTerminate(rdram, c);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkDmaTag, p);
    Ps2FastWrite32(rdram, p, 0x70000000u | (getRegU32(ctx, 5) & 0x0FFFFFFFu));
    Ps2FastWrite32(rdram, p + 4u, 0u);
    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 8u);
    setReturnU32(ctx, p + 8u);
}

void sceVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkAlignWords(rdram, c, 2u, 3u);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    const uint32_t cmd = getRegU32(ctx, 5) != 0u ? 0xD0000000u : 0x50000000u;
    Ps2FastWrite32(rdram, p, cmd);
    pkWrite32(rdram, c, kPkPtr, p + 4u);
    pkWrite32(rdram, c, kVif1PkDirect, p);
    setReturnS32(ctx, 0);
}

void sceVif1PkCloseDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t direct = pkRead32(rdram, c, kVif1PkDirect);
    if (direct != 0u)
    {
        const uint32_t ptr = pkRead32(rdram, c, kPkPtr);
        const uint32_t qwc = (ptr >= direct + 4u) ? (((ptr - 4u - direct) >> 2) >> 2) : 0u;
        Ps2FastWrite32(rdram, direct, Ps2FastRead32(rdram, direct) + qwc);
    }

    pkWrite32(rdram, c, kVif1PkDirect, 0u);
    setReturnS32(ctx, 0);
}

void sceVif1PkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    pkOpenGifTag(rdram, ctx, kVif1PkGifTag);
    setReturnS32(ctx, 0);
}

void sceVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkCloseGifTag(rdram, c, kVif1PkGifTag);
    setReturnS32(ctx, 0);
}

void sceVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint64_t data = static_cast<uint64_t>(GPR_U64(ctx, 6));
    const uint32_t addr = getRegU32(ctx, 5);
    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    Ps2FastWrite64(rdram, p, data);
    Ps2FastWrite32(rdram, p + 8u, addr);
    Ps2FastWrite32(rdram, p + 12u, 0u);
    pkWrite32(rdram, c, kPkPtr, p + 16u);
    setReturnS32(ctx, 0);
}

void sceVif1PkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    const uint32_t words = getRegU32(ctx, 5);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    const uint32_t p = pkRead32(rdram, c, kPkPtr);
    pkWrite32(rdram, c, kPkPtr, p + words * 4u);
    setReturnU32(ctx, p);
}

void sceVif1PkAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t c = getRegU32(ctx, 4);
    if (c == 0u) { setReturnS32(ctx, 0); return; }

    pkAlignWords(rdram, c, getRegU32(ctx, 5), getRegU32(ctx, 6));
    setReturnS32(ctx, 0);
}
