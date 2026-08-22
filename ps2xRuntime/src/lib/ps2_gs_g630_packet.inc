// G630 private cross-TU packet ABI. Included by the rasterizer front-end and GL backend .cpp
// files; kept out of the frozen public headers so this phase remains an incremental build.
#pragma once

#include <cstdint>
#include <vector>

struct G630ReplayPacket
{
    std::vector<uint32_t> recs;
    std::vector<uint32_t> texDesc;
    std::vector<uint32_t> texData;
    std::vector<uint32_t> tileOff;
    std::vector<uint32_t> tileRef;
    std::vector<uint32_t> seedVram;   // exactly 4 MiB, first submit only
    std::vector<uint32_t> patchPages; // 8 KiB page indices
    std::vector<uint32_t> patchWords; // 2048 words per patchPages entry
    std::vector<uint32_t> depthSeed;  // private-Z mirror, only after CPU authority changed
    int width = 0, height = 0, tilesX = 0;
    int block = 0, fbw = 0;
    bool wantDepth = false;
};

struct G630ViewPacket
{
    std::vector<uint32_t> pageWords; // one-time exact 64x32 CT32 page word map
    std::vector<uint32_t> patchPages;
    std::vector<uint32_t> patchWords;
    uint32_t fbp = 0u;
    int fbw = 0, width = 0, height = 0, originY = 0, rows = 0;
};

struct G630UploadPacket
{
    std::vector<uint32_t> pixels;
    uint32_t block = 0u;
    int fbw = 0, x = 0, y = 0, width = 0, height = 0;
};

struct G630T8Packet
{
    std::vector<uint32_t> mapWords;   // one-time exact 128x64 PSMT8 page permutation
    std::vector<uint32_t> clut;
    std::vector<uint32_t> patchPages;
    std::vector<uint32_t> patchWords;
    uint64_t texKey = 0u;
    uint32_t tbp = 0u, tbw = 0u;
    int width = 0, height = 0;
};

// G634: a CT32/CT24 TEXTURE decoded straight out of the persistent raw-VRAM image, addressed by
// TEX0 (tbp0/tbw/psm) rather than by FRAME. The output is exactly texW x texH with GS row 0 at
// output row 0, so the consumer's sampler sees a normal texture object and every wrap/round rule
// downstream keeps working against the DECLARED texture dimensions. There is no resident surface
// in the contract at all: the only inputs are the raw image, the one-time CT32 page word map, and
// whichever guest pages are still CPU-authoritative (carried as patches, exactly like G630's T8).
struct G634RawViewPacket
{
    std::vector<uint32_t> pageWords;  // one-time exact 64x32 CT32 page word map (shared with G630)
    std::vector<uint32_t> patchPages;
    std::vector<uint32_t> patchWords;
    std::vector<uint32_t> expected;   // DC2_G634_VERIFY only: CPU reference, texW*texH RGBA words
    uint64_t texKey = 0u;
    uint32_t tbp = 0u, tbw = 0u;
    uint32_t psm = 0u;                // GS_PSM_CT32 (0) or GS_PSM_CT24 (1)
    uint32_t ta0 = 0u, aem = 0u;      // TEXA, used only for CT24
    int width = 0, height = 0;
    // The oracle runs entirely on the GL worker and accumulates into backend-side counters, so no
    // front-end storage has to outlive the asynchronous submit.
    bool verify = false;
};

// G634b: a Host->Local IMAGE transfer of ANY pixel format, expressed as raw-VRAM word deltas.
// `ops` holds (wordIndex, value, mask) triples, ONE PER DISTINCT WORD, and the GPU applies
// `dst = (dst & ~mask) | (value & mask)`. Format-agnostic by construction: the front end computes
// the bit range each texel occupies with the SHIPPED per-PSM address helpers, so no per-format
// shader is involved and sub-word formats (PSMT8/PSMT4/PSMCT24/PSMCT16) are exact.
struct G634RawUploadPacket
{
    std::vector<uint32_t> ops;
    uint32_t psm = 0u;
    bool verify = false;
};
