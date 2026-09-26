// ===== G768: the GENERAL GPU PAGE VIEW packet (shared by the rasterizer front end and the GL backend) =====
//
// One view = one logical texture (texW x texH, RGBA8, exactly what g178DecodeWhole would produce)
// assembled on the GL worker from the pages that hold its bytes:
//
//   * a resident FBO page (owner / current dirty resident), read with the G675 centre tap, or
//   * the GPU MIRROR of guest VRAM: a persistent 1024x1024 RGBA8 texture holding every guest page in
//     CT32 logical page layout (page p at ((p & 15) * 64, (p >> 4) * 32)). A page is uploaded into
//     the mirror only when its guest write generation moved since its last upload, so a static
//     index texture costs one page upload per change, not one per view build.
//
// A per-format PAGE LUT maps every texel of one format page to {CT32 word x, word y, bit-in-word}
// of the same page: the composition of PixelStorageTraits<psm>::Read's own address arithmetic with
// the CT32 inverse. Every PS2 storage format is page-periodic, so one LUT per format serves every
// texture of that format at any page-aligned base. Indexed formats resolve through a CLUT texture
// whose entries are either CPU-decoded constants (CLUT pages current in guest VRAM — the exact
// g178LookupClutFixed values) or GPU gathers from the FBO that holds the CLUT bytes.
//
// Included by BOTH ps2_gs_rasterizer.cpp (front end) and ps2_gs_gpu_raster.cpp (backend), at true
// global scope, so the packet is one type.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

enum G768ViewFmt : int
{
    kG768FmtCt16 = 0, // 16-bit colour (PSMCT16 / PSMCT16S): decodePSMCT16 + TEXA
    kG768FmtIdx8 = 1, // 8-bit index (PSMT8 / PSMT8H) through the CLUT
    kG768FmtIdx4 = 2, // 4-bit index (PSMT4 / PSMT4HL / PSMT4HH) through the CLUT
    kG768FmtCt32 = 3, // 32-bit colour, raw (PSMCT32)
    kG768FmtCt24 = 4, // 24-bit colour + TEXA alpha (PSMCT24)
};
enum G768SrcKind : int
{
    kG768SrcFbo = 0,    // a resident FBO (key), GS logical coordinates, centre tap at scale S
    kG768SrcMirror = 1, // the guest-VRAM mirror texture (unscaled, top-down)
};
struct G768ViewPacket
{
    uint64_t texKey = 0u;
    uint64_t serial = 0u; // front-end build serial (the backend publishes the last one executed)
    int texW = 0, texH = 0;
    int fmt = kG768FmtIdx8;
    int lutId = 0;                             // one LUT per storage format
    int lutW = 0, lutH = 0;
    const std::vector<uint32_t> *lut = nullptr; // static storage owned by the front end
    bool full = true;                          // false: patch rects into an existing view
    // Per page run: {srcKind, srcKey, srcX, srcY, dstX, dstY, w, h} (8 ints; GS logical coords).
    std::vector<int32_t> rects;
    // Mirror uploads applied BEFORE anything else in the packet: page ids and 2048 CT32 words each
    // (x + 64 * y, the page's logical CT32 layout).
    std::vector<uint32_t> mirPages;
    std::vector<uint32_t> mirPixels;
    // CLUT (indexed formats only). clutConst holds the final RGBA of every entry the CPU could
    // decode exactly; clutEnt holds, per entry, {src (-1 = constant, else 0..3), x, y, mode} with
    // mode 0 = CT32 word, 1 = CT24 (+TEXA), 2 = CT16 low half, 3 = CT16 high half (+decode, TEXA).
    int clutN = 0;
    bool clutGpu = false;
    uint64_t clutSig = 0u; // 0 = never reuse; else the backend skips the CLUT pass when unchanged
    std::vector<uint32_t> clutConst;
    std::vector<int32_t> clutEnt;
    int clutSrcKind[4] = {-1, -1, -1, -1};
    int clutSrcKey[4] = {0, 0, 0, 0};
    int ta0 = 0, ta1 = 0, aem = 0;
};
