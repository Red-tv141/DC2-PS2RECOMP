#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_memory.h"
#include "ps2_log.h"
#include "ps2_gs_gpu_lle.h" // G178: private front-end<->backend interface (default-off lever)
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <vector>

using namespace GSInternal;

// Private live PSMT8 address helper (implemented in ps2_gs_memory.cpp, intentionally absent
// from the public generated-target header).
namespace GSMem
{
std::uint32_t AddressP8(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
}

// G264 private .cpp-to-.cpp bridge: alias-aware partial FBO patch. Deliberately kept out of
// ps2_gs_gpu_lle.h so no generated-target header is touched.
bool g264_backend_write_color_rect(uint32_t fbp, int texWidth, int texHeight,
                                   int glX, int glY, int width, int rows,
                                   const std::vector<uint32_t> &in);

// G281 private .cpp-to-.cpp bridge: build one decoded RGBA texture view directly from a
// resident CT32 target FBO. `mapRgba` encodes {srcX,srcGlY,byteLane} per PSMT8 texel; the
// backend combines those bytes with `clutRgba` on-GPU and installs the result under `texKey`.
// Kept out of ps2_gs_gpu_lle.h for the project's no-header-rebuild rule.
bool g281_backend_prepare_t8_view(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                                  int texW, int texH,
                                  const std::vector<uint32_t> &mapRgba,
                                  const std::vector<uint32_t> &clutRgba,
                                  bool verify, uint64_t &verifyBad);

// G309 private .cpp-to-.cpp bridge: compose the 128 physical CT32 pages of the measured
// 0x2720/tbw=8 atlas from the five live target FBOs in one GPU pass. `pageMap` is an 8x16
// RGBA8 map packed as {owner+1, source-page-column, source-page-row, 255}. When `readback`
// is true, `out` receives the 512x512 composite in GL bottom-first row order for the exact
// bring-up verifier. Kept out of ps2_gs_gpu_lle.h for the project's no-header-rebuild rule.
bool g309_backend_build_authoritative_composite(
    const std::vector<uint32_t> &pageMap,
    const std::vector<uint32_t> &basePixels,
    bool readback, std::vector<uint32_t> &out);

// G310 private .cpp-to-.cpp bridges. Initial/rebuild publication uses G309's exact one-pass
// page compositor to populate one persistent ordinary 512x512 logical atlas. Stable incremental
// changes can still use exact guest uploads and FBO copies. No public/generated header changes.
bool g310_backend_init_logical(uint32_t seedFbp,
                               const std::vector<uint32_t> &guestPixels);
bool g310_backend_copy_logical_pages(uint32_t srcFbp,
                                     const std::vector<int32_t> &rects6);
bool g310_backend_build_logical_composite(
    const std::vector<uint32_t> &pageMap,
    const std::vector<uint32_t> &basePixels);

// G141: measure-first perf scope (skill 17-performance-optimization.md), env-gated
// (DC2_PERF=1), aggregate-only print. Self-contained per-TU; drawPrimitive runs on the
// single game thread, no cross-thread sharing needed.
// G141: global GS-rasterization nanosecond accumulator (external linkage) so mgEndFrame can
// read a per-frame GS total and compute the non-overlapping GS-vs-VU1-vs-other split. The GS
// raster runs SYNCHRONOUSLY inside VU1 XGKICK, so vu1.run time double-counts it — this global
// lets the frame decomposition subtract it out.
std::atomic<uint64_t> g_g141GsRasterNs{0};
// G141 sub-profile: rasterizer coverage + primitive-type split (DC2_PERF). bboxPx = total
// bounding-box pixels iterated, insidePx = passed the barycentric edge test, drawnPx = reached
// writePixel. coverage = insidePx/bboxPx exposes bbox-scan waste; drawnPx/frame is the true
// per-pixel raster load. triCalls/sprCalls tell whether 3D tris or 2D sprites dominate.
std::atomic<uint64_t> g_g141TriCalls{0}, g_g141SpriteCalls{0};
std::atomic<uint64_t> g_g141BboxPx{0}, g_g141InsidePx{0}, g_g141DrawnPx{0};
std::atomic<uint64_t> g_g146G144FlushMidNs{0}, g_g146G144FlushUploadNs{0}, g_g146G144FlushFrameNs{0};
std::atomic<uint64_t> g_g146G144FlushMidCount{0}, g_g146G144FlushUploadCount{0}, g_g146G144FlushFrameCount{0};
// G148 (perf DIAGNOSIS, DEFAULT OFF, DC2_G148_SAMPLESTAT=1): decide whether a per-texture
// de-swizzle/decode cache would help the sampler. The dominant sampler cost is the swizzled
// texel fetch (ReadP8 + CLUT); the G142 texel-quad cache already skips the 4-tap refetch when
// consecutive pixels share the SAME (u0,v0) taps. If the quad-HIT rate is high the swizzle reads
// are already coherent and a de-swizzle cache buys little; if the MISS rate is high (perspective /
// minified textures change taps every pixel) the swizzle+CLUT reads dominate and de-swizzling the
// whole bound texture into a linear cache-hot RGBA buffer is the lever. These counters expose that
// ratio + the point-sample leaf-read count. Measurement only (bit-identical output).
std::atomic<uint64_t> g_g148TexSamples{0};   // total g141FastSample calls (textured tri pixels, fast path)
std::atomic<uint64_t> g_g148QuadHit{0};      // bilinear: reused the cached 2x2 tap quad (coherent)
std::atomic<uint64_t> g_g148QuadMiss{0};     // bilinear: rebuilt the quad (4x leaf read)
std::atomic<uint64_t> g_g148PointSamp{0};    // point-filter samples (1x leaf read each)
std::atomic<uint64_t> g_g148LeafReads{0};    // total g141SamplePoint leaf reads (ReadP8/readTexel + CLUT)

// G149 (perf, DEFAULT OFF, DC2_G149_TEXCACHE=1): shared de-swizzle/decode texture cache. G148
// measured the texel-quad cache hit at only ~10% -> ~1.18M swizzled leaf reads/frame ARE the ~50 ms
// sampler. This decodes the bound texture ONCE into a linear CLUT-pre-applied RGBA buffer so each
// g141SamplePoint becomes a single decoded[v*texW+u] read instead of swizzle-address math + a CLUT
// resolve. Bit-exact BY CONSTRUCTION: the buffer is filled with the SAME leaf expression the
// uncached path uses (verified per-pixel by DC2_G149_TCVERIFY). PROCESS-WIDE (not thread_local): the
// G144 tilebin flush replays one texture's tris across up to 8 disjoint scanline bands on different
// threads, so a thread_local decode would rebuild the texture up to 8x and can net-lose; the cache is
// keyed on the full texture descriptor + a VRAM GENERATION counter (bumped on every image upload /
// local transfer), built ONCE under a mutex per (key,gen), read lock-free by all band lanes.
std::atomic<uint64_t> g_g149VramGen{1};  // bumped by g149BumpVramGen() on every VRAM write
std::atomic<uint64_t> g_g149Hits{0};     // cache hits (decoded buffer reused)
std::atomic<uint64_t> g_g149Builds{0};   // decode builds (one per distinct texture/gen)
std::atomic<uint64_t> g_g149Ineligible{0}; // fast-path tris that could not use the cache
std::atomic<uint64_t> g_g162GpuHits{0};     // G162: T8 decode builds that used the GPU path
std::atomic<uint64_t> g_g162GpuFallback{0}; // G162: T8 decode builds that fell back to CPU
namespace {
struct G149Key
{
    uint32_t tbp0, tbw, cbp;
    uint32_t tw, th;
    uint32_t psm, cpsm, csm, csa;
    uint32_t texa;   // packed aem/ta0/ta1 (affects applyTexa for CT16/CT24)
    uint64_t gen;
    bool operator==(const G149Key &o) const
    {
        return tbp0 == o.tbp0 && tbw == o.tbw && cbp == o.cbp && tw == o.tw && th == o.th &&
               psm == o.psm && cpsm == o.cpsm && csm == o.csm && csa == o.csa &&
               texa == o.texa && gen == o.gen;
    }
};
struct G149KeyHash
{
    size_t operator()(const G149Key &k) const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix(k.tbp0); mix((static_cast<uint64_t>(k.tbw) << 32) | k.cbp);
        mix((static_cast<uint64_t>(k.tw) << 32) | k.th);
        mix((static_cast<uint64_t>(k.psm) << 24) | (k.cpsm << 16) | (k.csm << 8) | k.csa);
        mix(k.texa); mix(k.gen);
        return static_cast<size_t>(h);
    }
};
// LAZY-fill decoded texture: `px` is the linear CLUT-pre-applied RGBA buffer, `valid[i]` marks which
// texels have been decoded yet. Allocated (not decoded) at defer time; each texel is decoded ON FIRST
// SAMPLE and memoized, so only SAMPLED texels ever cost a swizzle+CLUT read — and that cost is shared
// across all 8 tilebin band lanes and every triangle using the texture (vs the eager variant that
// decoded the WHOLE texture even when tris sample a fraction, a measured net loss). The concurrent
// band writes are a BENIGN race: within one (key,gen) VRAM is constant so every thread decodes the
// SAME value for a given texel; aligned 32-bit `px[i]` and byte `valid[i]` stores don't tear on x86.
struct G149Tex
{
    std::vector<uint32_t> px;
    std::vector<uint8_t> valid;
};
using G149Buf = std::shared_ptr<G149Tex>;
// Guest-thread-only cache map: the G144 tilebin defer path (drawPrimitive, !t_g144InReplay) is the
// SOLE accessor, so no mutex is needed. Entries hold a shared_ptr also stored in the G144Entry, so
// band-replay threads use the buffer lock-free for the whole flush even if this map is later cleared.
// Keyed on the texture descriptor + VRAM generation; cleared when the generation advances.
std::unordered_map<G149Key, G149Buf, G149KeyHash> g_g149Map;
uint64_t g_g149MapGen = 0;
}
// Called from the GS upload / local-transfer paths (ps2_gs_gpu.cpp) — any write to VRAM bumps the
// generation so stale decoded buffers are never reused. Cheap monotonic increment.
void g149BumpVramGen()
{
    g_g149VramGen.fetch_add(1u, std::memory_order_relaxed);
}
// Lock-free pointers into a lazily-filled decoded texture, valid only during a G144 band replay of an
// eligible tri (set by the replay closures before drawTriangle; null otherwise). Read/written in
// g141SamplePoint. Each band lane has its own copy of these pointers; the shared_ptr in the G144Entry
// keeps the underlying buffers alive for the whole flush.
static thread_local uint32_t *t_g149Px = nullptr;
static thread_local uint8_t *t_g149Valid = nullptr;
namespace {
struct G146PerfScope
{
    bool on = false;
    std::chrono::steady_clock::time_point t0;
    std::atomic<uint64_t> *ns = nullptr;
    std::atomic<uint64_t> *count = nullptr;

    G146PerfScope(std::atomic<uint64_t> &targetNs, std::atomic<uint64_t> &targetCount)
    {
        static const bool s_on = (std::getenv("DC2_PERF") != nullptr) ||
                                 (std::getenv("DC2_G146_PERF") != nullptr) ||
                                 (std::getenv("DC2_G147_PERF") != nullptr);
        on = s_on;
        ns = &targetNs;
        count = &targetCount;
        if (on)
            t0 = std::chrono::steady_clock::now();
    }

    ~G146PerfScope()
    {
        if (!on || !ns || !count)
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ns->fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
        count->fetch_add(1u, std::memory_order_relaxed);
    }
};

struct G141PerfScope
{
    bool on;
    std::chrono::steady_clock::time_point t0;
    double *sum;
    uint32_t *win;
    const char *tag;
    uint32_t window;
    std::atomic<uint64_t> *accumNs = nullptr; // optional global ns accumulator
    G141PerfScope(bool on_, double *sum_, uint32_t *win_, const char *tag_, uint32_t window_)
        : on(on_), sum(sum_), win(win_), tag(tag_), window(window_)
    {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~G141PerfScope()
    {
        if (!on) return;
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (accumNs)
            accumNs->fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                std::memory_order_relaxed);
        *sum += std::chrono::duration<double, std::milli>(elapsed).count();
        if (++(*win) >= window)
        {
            std::fprintf(stderr, "[G141:perf] %s avgUs=%.2f window=%u\n", tag,
                         (*sum * 1000.0) / static_cast<double>(*win), *win);
            *sum = 0.0;
            *win = 0u;
        }
    }
};
} // namespace

// G89: set by dc2_game_override.cpp::g67_title_scope each frame; gates the title-rock guard-band
// cull in drawTriangle so it never fires outside the title-map scene. Defined here (external
// linkage) and referenced via `extern` from the override.
std::atomic<bool> g_dc2TitleRockScope{false};
// G202: set by dc2_game_override.cpp once the edit/town loop has a live scene, map, and camera.
// The GS state alone (fbp 0/0x68, Z24, zbp 0xd0) also appears during transitions/loading, so the
// town Z promotion must be game-state scoped to avoid clearing those frames.
std::atomic<bool> g_dc2TownDepthScope{false};
static thread_local bool t_g144TitleRockScopeOverride = false;
static thread_local bool t_g144TitleRockScope = false;
static thread_local bool t_g144TownDepthScopeOverride = false;
static thread_local bool t_g144TownDepthScope = false;

static bool g144EffectiveTitleRockScope()
{
    return t_g144TitleRockScopeOverride
               ? t_g144TitleRockScope
               : g_dc2TitleRockScope.load(std::memory_order_relaxed);
}

static bool g144EffectiveTownDepthScope()
{
    return t_g144TownDepthScopeOverride
               ? t_g144TownDepthScope
               : g_dc2TownDepthScope.load(std::memory_order_relaxed);
}
// G90: the logical title block currently being flushed by mgEndDraw (set by the override). Lets
// the G88:geo probe tag each drawn rock triangle with its source block. -1 = none.
std::atomic<int> g_dc2TitleCurBlock{-1};
std::atomic<int> g_g34_in_divsprite{0};


namespace
{

    bool envFlagEnabled(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr &&
               value[0] != '\0' &&
               std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0 &&
               std::strcmp(value, "FALSE") != 0 &&
               std::strcmp(value, "off") != 0 &&
               std::strcmp(value, "OFF") != 0;
    }

    bool envFlagDisabled(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr &&
               (value[0] == '\0' ||
                std::strcmp(value, "0") == 0 ||
                std::strcmp(value, "false") == 0 ||
                std::strcmp(value, "FALSE") == 0 ||
                std::strcmp(value, "off") == 0 ||
                std::strcmp(value, "OFF") == 0);
    }

    // G268: env vars are process-constant in every DC2 harness; per-draw/per-flush hot paths
    // must not call getenv() per invocation (Windows CRT getenv = env lock + linear scan —
    // and the banded replay lanes serialize on that lock). Hot sites read once by default;
    // DC2_G268_LIVE_ENVREAD=1 restores the historical per-call read (same-binary A/B control).
    bool g268LiveEnvRead()
    {
        static const bool live = envFlagEnabled("DC2_G268_LIVE_ENVREAD");
        return live;
    }

    // G268: cached DC2_G242_STAT (was a per-call getenv on depth-sync/alias paths).
    bool g242StatEnabled()
    {
        static const bool cached = envFlagEnabled("DC2_G242_STAT");
        return g268LiveEnvRead() ? envFlagEnabled("DC2_G242_STAT") : cached;
    }

    bool phaseDiagnosticsEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_PHASE_TRACE");
        return enabled;
    }

    bool renderQualityTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_RENDER_QUALITY");
        return enabled;
    }

    bool g195DofTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G195_DOF_TRACE");
        return enabled;
    }

    bool g195ForegroundTraceTarget(const GSContext &ctx, int x, int y)
    {
        static const bool enabled = envFlagEnabled("DC2_G195_FG_TRACE");
        if (!enabled)
            return false;
        if (ctx.frame.fbp != 0u && ctx.frame.fbp != 0x68u)
            return false;
        return (x == 256 && y == 260) ||
               (x == 256 && y == 300) ||
               (x == 256 && y == 350) ||
               (x == 120 && y == 300) ||
               (x == 400 && y == 300);
    }

    bool g43FaceUvTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G43_FACE_UV");
        return enabled;
    }

    bool g43FacePixelTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G43_FACE_PIXEL");
        return enabled;
    }

    bool g44FaceZTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_G44_FACE_Z");
        return enabled;
    }

    bool g44FaceZTargetTbp(uint32_t tbp)
    {
        return tbp == 0x3420u || tbp == 0x3460u || tbp == 0x35a0u || tbp == 0x34a0u;
    }

    // G50 (head fidelity): the costume head's VISIBLE BASE meshes — hat/cap (0x3420), face skin
    // (0x3480), hair (0x34c0), body skin (0x3460). Identified by skip-page A/B (DC2_G43_SKIP_TBP):
    // removing each removes that part. These are opaque base geometry, NOT deferring decals, so the
    // G48 dark-skip must NOT skip their dark fragments over the black RTT (doing so punched black
    // holes in the face skin = "speckle dots showing the wall through them", and dropped the hat's
    // dark brim = "parts of the hat invisible"). Skip stays ON for the actual decal pages
    // (0x34a0/0x35a0 etc.) which should defer to the skin. Env override: DC2_G50_NOEXEMPT=1 disables
    // the exemption (restores the G48 global-in-head-band behavior) for A/B.
    bool g50HeadBaseMeshPage(uint32_t tbp)
    {
        return tbp == 0x3420u || tbp == 0x3460u || tbp == 0x3480u || tbp == 0x34c0u;
    }

    // G50: per-texture-page tally of costume model (fbp=0x139) triangles — total vs degenerate
    // (collapsed -> dropped) and the screen bounding box of drawn vs degenerate triangles. Tells
    // which tbp = legs/boots and whether the lower-body under-render is a collapse-and-drop, a
    // mispositioning, or neither. Env-gated DC2_G50_PARTS, quiet by default.
    bool g50PartsEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G50_PARTS");
        return enabled;
    }

    struct G50PartStat
    {
        uint64_t nTotal = 0;
        uint64_t nDegen = 0;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;     // drawn (non-degenerate)
        float dMinX = 1e9f, dMinY = 1e9f, dMaxX = -1e9f, dMaxY = -1e9f; // degenerate
    };
    std::mutex g_g50Mutex;
    std::map<uint32_t, G50PartStat> g_g50Parts;