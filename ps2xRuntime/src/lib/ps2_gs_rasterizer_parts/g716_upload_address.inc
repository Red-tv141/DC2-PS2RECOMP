#pragma once
// Shared, shipped transfer addressing. One word/mask for the pixel being written.
static bool g634UploadTexelBits(uint32_t block, uint32_t width, uint32_t psm,
                                uint32_t X, uint32_t Y, uint32_t &word, uint32_t &mask)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0xFFFFFFFFu; return (off & 3u) == 0u;
    }
    case GS_PSM_CT24:
    case GS_PSM_Z24:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0x00FFFFFFu; return (off & 3u) == 0u;
    }
    case GS_PSM_CT16:
    case GS_PSM_Z16:
    {
        const uint32_t off = GSPSMCT16::addrPSMCT16(block, width, X, Y);
        if ((off & 1u) != 0u) return false;
        word = off >> 2u; mask = 0x0000FFFFu << ((off & 2u) * 8u); return true;
    }
    case GS_PSM_CT16S:
    case GS_PSM_Z16S:
    {
        const uint32_t off = GSPSMCT16::addrPSMCT16S(block, width, X, Y);
        if ((off & 1u) != 0u) return false;
        word = off >> 2u; mask = 0x0000FFFFu << ((off & 2u) * 8u); return true;
    }
    case GS_PSM_T8:
    {
        const uint32_t off = GSPSMT8::addrPSMT8(block, width, X, Y);
        word = off >> 2u; mask = 0xFFu << ((off & 3u) * 8u); return true;
    }
    case GS_PSM_T4HL:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0x0F000000u; return (off & 3u) == 0u;
    }
    case GS_PSM_T4HH:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0xF0000000u; return (off & 3u) == 0u;
    }
    case GS_PSM_T4:
    {
        const uint32_t nib = GSPSMT4::addrPSMT4(block, width, X, Y);
        const uint32_t byteOff = nib >> 1u;
        word = byteOff >> 2u;
        mask = 0x0Fu << (((byteOff & 3u) * 8u) + ((nib & 1u) * 4u));
        return true;
    }
    default:
        return false;
    }
}

