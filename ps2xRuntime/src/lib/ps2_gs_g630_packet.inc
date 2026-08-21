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
