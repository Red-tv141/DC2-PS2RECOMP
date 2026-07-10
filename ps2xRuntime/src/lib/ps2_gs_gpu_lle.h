// G178: bespoke LLE GPU rasterizer — PRIVATE interface between the batch front-end
// (ps2_gs_rasterizer.cpp: entry validation, vertex translation, texture decode, VRAM swizzle
// read/write) and the GL backend (ps2_gs_gpu_raster.cpp: persistent dedicated GPU thread, FBOs,
// GPU-resident texture cache, draw submission, readback).
//
// This header is included ONLY by those two .cpp files (both in the ps2_runtime lib target); it
// is deliberately NOT under include/runtime/ — nothing in the generated dc2_game target can see
// it, so editing it never triggers the 30h full rebuild the public headers would.
//
// Everything here is opt-in behind DC2_G178_GPU=1 and default-off. See
// plans/gpu-raster-arc-plan.md and plans/phase-G178-fix-log.md.
#pragma once

#include <cstdint>
#include <vector>

// One translated vertex. x/y are screen pixels (GS window coords minus XYOFFSET, pixel centers at
// +0.5 like the CPU rasterizer); z is the raw GS Z value (uint bits, normalized on the GPU).
// sq/tq/iq encode texture coords uniformly for both addressing modes (interpolated
// `noperspective`, i.e. linearly in screen space — exactly the CPU rasterizer's convention):
//   STQ (fst=0): sq = s/|q|, tq = t/|q|, iq = 1/|q|      → frag uv = (sq/iq, tq/iq)  [normalized]
//   UV  (fst=1): sq = (u/16)/texW, tq = (v/16)/texH, iq = 1
struct G178Vtx
{
    float x, y, z;
    float sq, tq, iq;
    uint8_t r, g, b, a;
};

// One state-batched draw run over a contiguous vertex range (triangles, 3 verts each).
struct G178Draw
{
    int firstVtx = 0;
    int vtxCount = 0;
    uint64_t texKey = 0; // 0 = untextured (vertex color only)
    uint8_t blend = 0;   // 0=off (Cv=Cs), 1=standard (Cs-Cd)*As+Cd, 2=additive Cs*As+Cd
    uint8_t tfx = 0;     // GS TFX (0 modulate / 1 decal / 2 highlight / 3 highlight2)
    uint8_t tcc = 0;
    uint8_t depthFunc = 0; // 0=disabled, 1=ALWAYS, 2=GEQUAL, 3=GREATER
    bool depthWrite = false;
    bool bilinear = false;
    uint8_t wrapU = 0, wrapV = 0; // 0 repeat, 1 clamp-to-edge
    uint16_t scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0; // GS scissor (inclusive, top-origin)
};

// A texture the backend must (re)upload before drawing this batch (decoded RGBA8, linear,
// row 0 = texel row 0). Replaces any previous texture with the same key.
struct G178TexUpload
{
    uint64_t key = 0;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

// One flush's worth of work. Submitted synchronously; on return `readback` holds the FBO color
// contents (RGBA8, GL row order = BOTTOM row first — the front-end owns all row flipping).
struct G178Batch
{
    uint32_t fbp = 0;
    int fbW = 0, fbH = 0;
    bool clearDepth = false;
    bool uploadFb = false;              // VRAM framebuffer copy is newer than the FBO
    std::vector<uint32_t> fbPixels;     // fbW*fbH RGBA when uploadFb (GL row order)
    std::vector<G178TexUpload> texUploads;
    std::vector<G178Vtx> verts;
    std::vector<G178Draw> draws;
    std::vector<uint32_t> readback;     // out (resized by the backend)
};

// Backend (ps2_gs_gpu_raster.cpp). submit() blocks the calling thread until rendering+readback
// are complete; returns false if the backend never started / failed (caller must CPU-fallback).
bool g178_backend_ready();
bool g178_backend_submit(G178Batch &batch);
bool g178_backend_has_tex(uint64_t key); // still resident (not evicted)?
