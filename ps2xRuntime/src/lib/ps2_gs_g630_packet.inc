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
    std::vector<uint32_t> expected; // G716 diagnostic only: independent CPU decode of raw bytes
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

// ==================================================================================================
// G714: ONE same-format GS local->local transfer, resolved, executed entirely GPU-side against the
// persistent raw GS VRAM image (2048x512 R32UI, row p == guest page p — G630's representation).
//
// The packet carries the RESOLVED OPERATION, never GS registers: the front end has already decided
// which guest pages are CPU-authoritative (`patchPages`/`patchWords`, a straight page memcpy — no
// per-pixel work), which pages a resident CT32 target owns (`fboIn`, pulled GPU->GPU), and which
// rows the copy's result must be pushed back into (`fboOut`). Worker order is:
//
//     patch guest pages -> raw     (glTexSubImage2D runs, G631 coalesced)
//     fboIn:  FBO  -> raw          (compute; a target owns those pages, guest VRAM is stale there)
//     copy:   raw  -> raw          (compute; one invocation per DESTINATION word, no write races)
//     fboOut: raw  -> FBO          (compute; the G630 view program, unchanged)
//
// Nothing in that chain reads back to the CPU, so the transfer costs no `g261Materialize`.
// ==================================================================================================
struct G714L2lPacket
{
    uint32_t g716Mode = 0u; // 1: row handoff, 2: raw memory copy; guest patch precedes ingestion
    std::vector<uint32_t> g716Protected; // pages excluded from FBO ingestion
    // One-time tables. Empty on every packet after the backend has latched them.
    std::vector<uint32_t> ct32PageWords; // exact 64x32 CT32 page word map (shared with G630)
    std::vector<uint32_t> fwd;           // texel index -> byte offset within its 8 KiB page
    std::vector<uint32_t> inv;           // element index -> texel index (the inverse of `fwd`)
    uint32_t tableKey = 0xFFFFFFFFu;     // the PSM `fwd`/`inv` describe; 0xFFFFFFFF = "unchanged"

    std::vector<uint32_t> patchPages;    // ascending guest-authoritative pages, applied after fboIn
    std::vector<uint32_t> patchWords;    // exactly 2048 words per patchPages entry, same order

    // 5 ints per entry: fbp, fbw, fbW, rowLo, rowHi (GS rows, both inclusive, 32-aligned bands).
    std::vector<int32_t> fboIn;
    std::vector<int32_t> fboOut;

    uint32_t sbp = 0u, dbp = 0u;             // BITBLTBUF base, in blocks
    uint32_t sPagesPerRow = 1u, dPagesPerRow = 1u;
    uint32_t pw = 0u, ph = 0u, bpp = 0u;     // page geometry of the (single, shared) PSM
    uint32_t ssax = 0u, ssay = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint32_t revX = 0u, revY = 0u;           // TRXPOS traversal
    uint32_t dPgX0 = 0u, dPgY0 = 0u, dPgNx = 0u, dPgNy = 0u; // destination page grid
    // G715 diagnostic (DC2_G715_IDENTONLY=1): run ONLY `g714In` then `g714Out` on the same bands —
    // no guest patch, no copy — while the front end lets the shipped CPU path do the entire
    // transfer. `g714Out o g714In` must be the identity, so a pixel change under this arm isolates
    // the FBO<->raw boundary from the copy, the patch, the publication skip and the admission.
    uint32_t identOnly = 0u;
    // ⭐ G715 VRAMDST. The shipped renderer never puts an l2l destination's bytes into the resident
    // FBO — `kG627PromotedDefaultOn == false`, so the CPU body writes guest VRAM, the tag bumps
    // those pages' generations, and G536 hands the pages to guest VRAM. Reproduce that: read the
    // destination page span back out of raw and let the front end place exactly the transfer's
    // bytes in guest VRAM, leaving the FBO's rendered colour alone. `noFboOut` drops leg (4).
    std::vector<uint32_t> *dstOut = nullptr; // front-end owned; filled with pages dstPgLo..+Count
    uint32_t dstPgLo = 0u, dstPgCount = 0u;
    uint32_t noFboOut = 0u;
};
