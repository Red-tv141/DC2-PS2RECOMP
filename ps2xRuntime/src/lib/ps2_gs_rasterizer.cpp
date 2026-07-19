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

    // PHASE G214 (default-off, DC2_G214_CAPTRACE=1): per-present-tick triangle counts for the
    // individual head sub-mesh texture pages, so the "head present but cap missing" window the
    // user observed and the flicker's temporal shape (rapid per-frame vs contiguous stretch) are
    // directly visible. Each page is an independent VU1 batch; G213 showed cap(0x35a0)+face(0x3420)
    // drop as whole batches (0 tris) upstream of the GS. Prints one line per tick that touches the
    // fbp=0x139 character RTT.
    bool g214CapTraceEnabled()
    {
        static const bool e = envFlagEnabled("DC2_G214_CAPTRACE");
        return e;
    }
    std::mutex g_g214Mutex;
    uint64_t g_g214Tick = 0;
    // pages: cap 0x35a0, face 0x3420, hair 0x34c0, head-base 0x3480, body 0x3460, neck-decal 0x34a0
    uint64_t g_g214Cap = 0, g_g214Face = 0, g_g214Hair = 0, g_g214Head = 0, g_g214Body = 0, g_g214Neck = 0;
    void g214CapFlushLocked(uint64_t newTick)
    {
        if (g_g214Tick != 0 && (g_g214Cap | g_g214Face | g_g214Hair | g_g214Head | g_g214Body | g_g214Neck))
        {
            std::fprintf(stderr,
                "[G214:cap] tick=%llu cap35a0=%llu face3420=%llu hair34c0=%llu head3480=%llu body3460=%llu neck34a0=%llu\n",
                (unsigned long long)g_g214Tick,
                (unsigned long long)g_g214Cap, (unsigned long long)g_g214Face,
                (unsigned long long)g_g214Hair, (unsigned long long)g_g214Head,
                (unsigned long long)g_g214Body, (unsigned long long)g_g214Neck);
        }
        g_g214Tick = newTick;
        g_g214Cap = g_g214Face = g_g214Hair = g_g214Head = g_g214Body = g_g214Neck = 0;
    }

    void g50Record(uint32_t tbp, bool degen,
                   float x0, float y0, float x1, float y1, float x2, float y2)
    {
        std::lock_guard<std::mutex> lk(g_g50Mutex);
        G50PartStat &s = g_g50Parts[tbp];
        s.nTotal++;
        float mnx = std::min({x0, x1, x2}), mxx = std::max({x0, x1, x2});
        float mny = std::min({y0, y1, y2}), mxy = std::max({y0, y1, y2});
        if (degen)
        {
            s.nDegen++;
            s.dMinX = std::min(s.dMinX, mnx);
            s.dMaxX = std::max(s.dMaxX, mxx);
            s.dMinY = std::min(s.dMinY, mny);
            s.dMaxY = std::max(s.dMaxY, mxy);
        }
        else
        {
            s.minX = std::min(s.minX, mnx);
            s.maxX = std::max(s.maxX, mxx);
            s.minY = std::min(s.minY, mny);
            s.maxY = std::max(s.maxY, mxy);
        }
    }

    void g50DumpMaybe()
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % 4000ull != 0ull)
            return;
        std::lock_guard<std::mutex> lk(g_g50Mutex);
        std::fprintf(stderr, "[G50:parts] === snapshot @%llu fbp139 model tris ===\n",
                     (unsigned long long)n);
        for (auto &kv : g_g50Parts)
        {
            const G50PartStat &s = kv.second;
            std::fprintf(stderr,
                         "[G50:parts] tbp=0x%04x total=%llu degen=%llu (%.0f%%) "
                         "drawn=(%.0f,%.0f)-(%.0f,%.0f) degen=(%.0f,%.0f)-(%.0f,%.0f)\n",
                         kv.first, (unsigned long long)s.nTotal, (unsigned long long)s.nDegen,
                         s.nTotal ? 100.0 * (double)s.nDegen / (double)s.nTotal : 0.0,
                         s.minX, s.minY, s.maxX, s.maxY,
                         s.dMinX, s.dMinY, s.dMaxX, s.dMaxY);
        }
    }

    uint32_t g43SkipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G43_SKIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    bool f50_12_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_F50_12");
        return enabled;
    }

    uint64_t f50_12_tex0_raw(const GSTex0Reg &tex)
    {
        return static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) |
               (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 14) |
               (static_cast<uint64_t>(tex.psm & 0x3Fu) << 20) |
               (static_cast<uint64_t>(tex.tw & 0xFu) << 26) |
               (static_cast<uint64_t>(tex.th & 0xFu) << 30) |
               (static_cast<uint64_t>(tex.tcc & 0x1u) << 34) |
               (static_cast<uint64_t>(tex.tfx & 0x3u) << 35) |
               (static_cast<uint64_t>(tex.cbp & 0x3FFFu) << 37) |
               (static_cast<uint64_t>(tex.cpsm & 0xFu) << 51) |
               (static_cast<uint64_t>(tex.csm & 0x1u) << 55) |
               (static_cast<uint64_t>(tex.csa & 0x1Fu) << 56) |
               (static_cast<uint64_t>(tex.cld & 0x7u) << 61);
    }

    void g195LogForegroundPixel(const GSContext &ctx,
                                int x,
                                int y,
                                uint32_t off,
                                uint32_t bytesPerPixel,
                                uint32_t primType,
                                bool primTme,
                                bool primAbe,
                                bool primFst,
                                bool primCtxt,
                                bool pabe,
                                uint32_t dstBefore,
                                uint8_t srcR,
                                uint8_t srcG,
                                uint8_t srcB,
                                uint8_t srcA,
                                uint32_t finalRgba)
    {
        static std::atomic<uint32_t> s_count{0};
        const uint32_t n = s_count.fetch_add(1u, std::memory_order_relaxed);
        if (n >= 1024u)
            return;
        std::fprintf(stderr,
            "[G195:fgpx] n=%u blk=%d xy=(%d,%d) fbp=0x%x fbw=%u fpsm=0x%x off=0x%x bpp=%u "
            "prim=%u tme=%u abe=%u fst=%u ctxt=%u tbp=0x%x tbw=%u tpsm=0x%x tw=%u th=%u "
            "cbp=0x%x csa=%u tex0=0x%016llx alpha=0x%llx test=0x%llx zbuf=0x%llx fba=0x%llx pabe=%u "
            "src=%02x,%02x,%02x,%02x dst=%08x final=%08x scissor=(%d,%d)-(%d,%d)\n",
            n, g_dc2TitleCurBlock.load(std::memory_order_relaxed),
            x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
            static_cast<uint32_t>(ctx.frame.psm), off, bytesPerPixel, primType,
            primTme ? 1u : 0u, primAbe ? 1u : 0u, primFst ? 1u : 0u,
            primCtxt ? 1u : 0u, ctx.tex0.tbp0,
            static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.psm),
            static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
            ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.csa),
            static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
            static_cast<unsigned long long>(ctx.alpha),
            static_cast<unsigned long long>(ctx.test),
            static_cast<unsigned long long>(ctx.zbuf),
            static_cast<unsigned long long>(ctx.fba),
            pabe ? 1u : 0u,
            srcR, srcG, srcB, srcA, dstBefore, finalRgba,
            ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
    }

    struct F50_12FramebufferBucket
    {
        std::atomic<uint64_t> commits{0};
        std::atomic<uint64_t> rgbNz{0};
        std::atomic<uint64_t> rgbNzLogged{0};
    };

    void f50_12_log_framebuffer_commit(const GSContext &ctx,
                                       int x,
                                       int y,
                                       uint32_t off,
                                       uint32_t bytesPerPixel,
                                       uint32_t finalRgba,
                                       uint32_t primType,
                                       bool primTme,
                                       bool primAbe,
                                       bool primFst,
                                       bool primCtxt)
    {
        if (!f50_12_trace_enabled())
            return;

        static F50_12FramebufferBucket s_buckets[4];
        const uint32_t fbp = ctx.frame.fbp;
        const uint32_t bucketIndex =
            (fbp == 0u) ? 0u :
            (fbp == 0x68u) ? 1u :
            (fbp == 0x139u) ? 2u : 3u;
        const char *bucketName =
            (bucketIndex == 0u) ? "fbp0" :
            (bucketIndex == 1u) ? "fbp68" :
            (bucketIndex == 2u) ? "fbp139" : "other";

        F50_12FramebufferBucket &bucket = s_buckets[bucketIndex];
        const uint64_t bucketN = bucket.commits.fetch_add(1, std::memory_order_relaxed) + 1ull;
        const bool rgbNz = (finalRgba & 0x00FFFFFFu) != 0u;
        uint64_t rgbNzTotal = bucket.rgbNz.load(std::memory_order_relaxed);
        bool shouldLog = bucketN <= 16ull || (bucketN & 0xFFFFFull) == 0ull;
        if (rgbNz)
        {
            rgbNzTotal = bucket.rgbNz.fetch_add(1, std::memory_order_relaxed) + 1ull;
            const uint64_t logged = bucket.rgbNzLogged.fetch_add(1, std::memory_order_relaxed) + 1ull;
            shouldLog = shouldLog || logged <= 16ull || (logged & 0xFFFFull) == 0ull;
        }

        if (!shouldLog)
            return;

        const auto &tex = ctx.tex0;
        std::fprintf(stderr,
                     "[F512:pxfbp] bucket=%s bucketN=%llu rgbNz=%llu fbp=0x%x fbw=%u fpsm=0x%x "
                     "prim=%u tme=%u abe=%u fst=%u ctxt=%u tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x "
                     "xy=(%d,%d) off=0x%x bpp=%u final=0x%08x\n",
                     bucketName,
                     static_cast<unsigned long long>(bucketN),
                     static_cast<unsigned long long>(rgbNzTotal),
                     fbp,
                     ctx.frame.fbw,
                     static_cast<uint32_t>(ctx.frame.psm),
                     primType,
                     primTme ? 1u : 0u,
                     primAbe ? 1u : 0u,
                     primFst ? 1u : 0u,
                     primCtxt ? 1u : 0u,
                     static_cast<unsigned long long>(f50_12_tex0_raw(tex)),
                     tex.tbp0,
                     tex.tbw,
                     static_cast<uint32_t>(tex.psm),
                     tex.cbp,
                     static_cast<uint32_t>(tex.cpsm),
                     x,
                     y,
                     off,
                     bytesPerPixel,
                     finalRgba);
    }

    // PHASE F50.8: per-sample texel-color histogram. Quiet unless DC2_TRACE_RENDER_QUALITY.
    // Answers: are textured dungeon draws sampling RGB=0 (black), and is it the paletted path?
    void f508LogSample(uint32_t psm, uint32_t color)
    {
        static std::atomic<uint64_t> total{0};
        static std::atomic<uint64_t> rgbZero{0};
        static std::atomic<uint64_t> palTotal{0};
        static std::atomic<uint64_t> palZero{0};
        const bool paletted = (psm == GS_PSM_T4 || psm == GS_PSM_T4HL ||
                               psm == GS_PSM_T4HH || psm == GS_PSM_T8);
        const bool z = (color & 0x00FFFFFFu) == 0u;
        const uint64_t t = total.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (z)
            rgbZero.fetch_add(1, std::memory_order_relaxed);
        if (paletted)
        {
            palTotal.fetch_add(1, std::memory_order_relaxed);
            if (z)
                palZero.fetch_add(1, std::memory_order_relaxed);
        }
        if ((t & 0xFFFFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F508:texsum] total=%llu rgbZero=%llu palTotal=%llu palZero=%llu\n",
                         static_cast<unsigned long long>(t),
                         static_cast<unsigned long long>(rgbZero.load()),
                         static_cast<unsigned long long>(palTotal.load()),
                         static_cast<unsigned long long>(palZero.load()));
        }
    }

    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    thread_local bool g_g104TriClipReentry = false;

    bool g104TriClipEnabled()
    {
        // G139: RETIRED by default. The G138 FM-swap/MAC-pipeline root fix plus the G139
        // same-pair VF hazard fix make the natural VU gate + positions HW-faithful; the
        // rasterizer-side near-plane tri clip is no longer needed (A/B g100_g139nb_keepz.png
        // == g100_g139fix.png). Re-enable with DC2_G104_FORCE_TRI_CLIP=1.
        static const bool enabled = std::getenv("DC2_G104_FORCE_TRI_CLIP") != nullptr;
        return enabled;
    }

    float g104Wnear()
    {
        static const float value = []() -> float {
            const char *e = std::getenv("DC2_G104_WNEAR");
            const float v = e ? static_cast<float>(std::atof(e)) : 4.0f;
            return (std::isfinite(v) && v > 0.0f) ? v : 4.0f;
        }();
        return value;
    }

    bool g104ScreenClipEnabled()
    {
        // G125 (DECISIVE, default-ON homog): the title camera/projection matrix matches HW
        // byte-for-byte (G124's "X-scale" was refuted), so the title cavern transforms correctly;
        // the residual defect is the near-plane clip of straddling tristrips. The old DEFAULT
        // screen-space lerp interpolated screen XY of a behind vertex whose 12.4-fixed position is
        // already saturated -> wrong intersection -> dark "sheets". The homogeneous path
        // reconstructs clip = screen*W (q=1/W preserved in GSVertex) and interpolates in clip
        // space = the mathematically correct intersection. Make homog the DEFAULT; restore the old
        // screen-space clip with DC2_G125_NO_HOMOG_CLIP=1 (or DC2_G104_SCREEN_CLIP=1).
        static const bool screen =
            envFlagEnabled("DC2_G125_NO_HOMOG_CLIP") ||
            envFlagEnabled("DC2_G104_SCREEN_CLIP");
        return screen;
    }

    float g104Qnear()
    {
        static const float value = []() -> float {
            const char *e = std::getenv("DC2_G104_QNEAR");
            const float v = e ? static_cast<float>(std::atof(e)) : 1.0e-4f;
            return (std::isfinite(v) && v > 0.0f) ? v : 1.0e-4f;
        }();
        return value;
    }

    bool g104TitleRockTriangle(bool primTme, uint32_t fbp, uint32_t tbp)
    {
        return g144EffectiveTitleRockScope() &&
               primTme &&
               fbp != 0x139u &&
               tbp >= 0x2720u &&
               tbp <= 0x3960u;
    }

    bool g106TitleZEnabled()
    {
        // G125 (default-ON): the title cavern has real depth. Without a depth test the back
        // geometry overdraws the bright front mural in painter order -> dark/muddy center (the
        // mural vanished even when the geometry+clip were correct). A private, per-frame-cleared,
        // non-aliasing title Z buffer (g106TitleZPrivateEnabled) restores HW-correct ordering so
        // the front mural shows. Honors the guest's own ZTE/ZTST. Tightly title-scoped
        // (g106TitleZScope: title-rock tris, fbp 0/0x68). Kill with DC2_G125_NO_TITLE_Z=1.
        static const bool enabled = !envFlagEnabled("DC2_G125_NO_TITLE_Z");
        return enabled;
    }

    bool g106TitleZPrivateEnabled()
    {
        static const bool enabled = (std::getenv("DC2_G106_TITLE_Z_VRAM") == nullptr);
        return enabled;
    }

    bool g106TitleZTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G106_TITLE_Z_STAT");
        return enabled;
    }

    bool g202TownZEnabled()
    {
        // G202 (default-ON): normal town/field display targets use the guest's real ZBUF/TEST
        // state instead of painter's order. DC2_G202_NO_TOWN_Z kills the promoted behavior; the
        // older G195 opt-in still forces it for A/B against the original diagnostic.
        static const bool enabled =
            !envFlagEnabled("DC2_G202_NO_TOWN_Z") || envFlagEnabled("DC2_G195_TOWN_Z");
        return enabled;
    }

    bool g202TownZTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G202_TOWN_Z_STAT");
        return enabled;
    }

    bool g202TownZForced()
    {
        static const bool enabled = envFlagEnabled("DC2_G195_TOWN_Z");
        return enabled;
    }

    bool g203UniversalZEnabled()
    {
        // G203 (default-ON): honor the guest's real ZBUF/TEST for EVERY draw + primitive against the
        // real VRAM Z buffer, and let the game's own full-screen ztst=ALWAYS,zmsk=0 clear-sprite
        // populate it (drawSprite now writes Z). Replaces the per-screen Z whitelist — title
        // private-Z (g106), costume private-Z (fbp 0x139), and the town game-state scope + artificial
        // Z-page clear (g202) — which was not HW-faithful and leaked: g202's blind clear of zbp 0xd0
        // (= VRAM block 0x1a00, the debug/menu font staging per G5) corrupted inventory/loading
        // textures whenever the town scope stayed active into a menu. Kill DC2_G203_LEGACY_Z=1 to
        // restore the old scoped/private paths for A/B. DC2_NO_ZTEST=1 still disables Z globally.
        static const bool enabled = !envFlagEnabled("DC2_G203_LEGACY_Z");
        return enabled;
    }

    bool g202TownZScope(uint32_t fbp, uint32_t zbpPage, uint32_t zpsm)
    {
        return g202TownZEnabled() &&
               (g202TownZForced() || g144EffectiveTownDepthScope()) &&
               !g144EffectiveTitleRockScope() &&
               (fbp == 0u || fbp == 0x68u) &&
               zbpPage == 0xd0u &&
               zpsm == 0x1u; // Z24, matching the MAP-0 HW dump's town scene.
    }

    void g202TownZClear(uint8_t *vram, uint32_t fbp, uint32_t zbpBlock, uint32_t zbw)
    {
        if (vram == nullptr)
            return;
        const uint32_t width = std::max(1u, std::min(1024u, zbw * 64u));
        constexpr uint32_t kHeight = 512u;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < width; ++x)
                GSMem::WriteZ24(vram, zbpBlock, zbw, x, y, 0u);
        if (g202TownZTraceEnabled())
        {
            static std::atomic<uint32_t> s_clearTrace{0};
            const uint32_t n = s_clearTrace.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u || (n % 120u) == 0u)
                std::fprintf(stderr, "[G202:zclear] n=%u fbp=0x%x zbp=0x%x zbw=%u size=%ux%u\n",
                             n, fbp, zbpBlock, zbw, width, kHeight);
        }
    }

    void g202TownZClearIfNeeded(uint8_t *vram, uint32_t fbp, uint32_t zbuf, uint32_t fbw)
    {
        const uint32_t zbpPage = static_cast<uint32_t>(zbuf & 0x1FFu);
        const uint32_t zpsm = static_cast<uint32_t>((zbuf >> 24) & 0xFu);
        if (!g202TownZScope(fbp, zbpPage, zpsm))
            return;
        const uint32_t zbw = (fbw != 0u) ? fbw : 8u;
        const uint32_t key = (fbp & 0x1FFu) | (zbpPage << 9) | ((zbw & 0x3Fu) << 18) | (zpsm << 24);
        static std::atomic<uint32_t> s_lastKey{0xFFFFFFFFu};
        const uint32_t prev = s_lastKey.exchange(key, std::memory_order_relaxed);
        if (prev == key)
            return;
        g202TownZClear(vram, fbp, zbpPage << 5, zbw);
    }

    bool g106TitleZScope(bool primTme, uint32_t fbp, uint32_t tbp)
    {
        return g106TitleZEnabled() &&
               (fbp == 0u || fbp == 0x68u) &&
               g104TitleRockTriangle(primTme, fbp, tbp);
    }

    constexpr uint32_t kG106TitleZW = 512u;
    constexpr uint32_t kG106TitleZH = 512u;

    uint32_t g106TitleZIndex(uint32_t fbp)
    {
        return (fbp == 0x68u) ? 1u : 0u;
    }

    std::vector<uint32_t> &g106TitleZBuf(uint32_t fbp)
    {
        static std::vector<uint32_t> s_buf0(kG106TitleZW * kG106TitleZH, 0u);
        static std::vector<uint32_t> s_buf68(kG106TitleZW * kG106TitleZH, 0u);
        return (g106TitleZIndex(fbp) == 0u) ? s_buf0 : s_buf68;
    }

    void g106TitleZClear(uint32_t fbp)
    {
        auto &buf = g106TitleZBuf(fbp);
        std::fill(buf.begin(), buf.end(), 0u);
    }

    void g106TitleZClearIfNeeded(uint32_t fbp)
    {
        static std::atomic<uint32_t> s_lastFbp{0xFFFFFFFFu};
        const uint32_t prev = s_lastFbp.exchange(fbp, std::memory_order_relaxed);
        if (prev == fbp)
            return;
        g106TitleZClear(fbp);
        if (g106TitleZTraceEnabled())
            std::fprintf(stderr, "[G106:zclear] fbp=0x%x private=%u\n",
                         fbp, g106TitleZPrivateEnabled() ? 1u : 0u);
    }

    uint32_t g106TitleZRead(int x, int y, uint32_t fbp)
    {
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= kG106TitleZW ||
            static_cast<uint32_t>(y) >= kG106TitleZH)
            return 0u;
        return g106TitleZBuf(fbp)[static_cast<uint32_t>(y) * kG106TitleZW + static_cast<uint32_t>(x)];
    }

    void g106TitleZWrite(int x, int y, uint32_t fbp, uint32_t z)
    {
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= kG106TitleZW ||
            static_cast<uint32_t>(y) >= kG106TitleZH)
            return;
        g106TitleZBuf(fbp)[static_cast<uint32_t>(y) * kG106TitleZW + static_cast<uint32_t>(x)] = z;
    }

    uint32_t g106SkipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G106_SKIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    int g106SkipPrim()
    {
        static const int value = []() {
            const char *e = std::getenv("DC2_G106_SKIP_PRIM");
            return e ? std::atoi(e) : -1;
        }();
        return value;
    }

    bool g106SkipStatEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G106_SKIP_STAT");
        return enabled;
    }

    uint32_t g106NoTriClipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G106_NO_TRICLIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    int g106NoTriClipPrim()
    {
        static const int value = []() {
            const char *e = std::getenv("DC2_G106_NO_TRICLIP_PRIM");
            return e ? std::atoi(e) : -1;
        }();
        return value;
    }

    bool g106NoTriClipTriangle(bool primTme, uint32_t primType, uint32_t fbp, uint32_t tbp)
    {
        const uint32_t noClipTbp = g106NoTriClipTbp();
        const int noClipPrim = g106NoTriClipPrim();
        if (noClipTbp == 0xFFFFFFFFu && noClipPrim < 0)
            return false;
        if (!g104TitleRockTriangle(primTme, fbp, tbp))
            return false;
        if (noClipTbp != 0xFFFFFFFFu && tbp != noClipTbp)
            return false;
        if (noClipPrim >= 0 && static_cast<uint32_t>(noClipPrim) != primType)
            return false;
        return true;
    }

    bool g106ShouldSkipTriangle(bool primTme, uint32_t primType, uint32_t fbp, uint32_t tbp)
    {
        const uint32_t skipTbp = g106SkipTbp();
        const int skipPrim = g106SkipPrim();
        if (skipTbp == 0xFFFFFFFFu && skipPrim < 0)
            return false;
        if (!g104TitleRockTriangle(primTme, fbp, tbp))
            return false;
        if (skipTbp != 0xFFFFFFFFu && tbp != skipTbp)
            return false;
        if (skipPrim >= 0 && static_cast<uint32_t>(skipPrim) != primType)
            return false;
        return true;
    }

    bool g104InsideQ(const GSVertex &v)
    {
        // G126: "inside" = in FRONT of the near plane, i.e. view-space depth W=1/q >= wNear.
        // The old test (q>0) also accepted verts BETWEEN the camera and the near plane (tiny
        // W, huge q) whose projected screen XY is extreme -> "stretched" triangles spanning the
        // screen. Treat those as outside so the near-plane clip pulls the edge onto the plane
        // (W=wNear) instead of passing the triangle stretched. q<=0 (behind camera) stays
        // outside. Restore the old behaviour with DC2_G126_NO_NEAR_CULL=1.
        if (!std::isfinite(v.q) || v.q <= 0.0f)
            return false;
        static const bool s_noNearCull = envFlagEnabled("DC2_G126_NO_NEAR_CULL");
        if (s_noNearCull)
            return true;
        const float wNear = g104Wnear();
        const float qNear = (wNear > 0.0f) ? (1.0f / wNear) : 1.0e9f; // q at the near plane
        return v.q <= qNear;
    }

    float g104SafeW(float q)
    {
        if (!std::isfinite(q))
            return (q < 0.0f) ? -1.0e8f : 1.0e8f;
        if (std::fabs(q) < 1.0e-8f)
            return (q < 0.0f) ? -1.0e8f : 1.0e8f;
        return 1.0f / q;
    }

    float g104Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    uint8_t g104LerpU8(uint8_t a, uint8_t b, float t)
    {
        return clampU8(static_cast<int>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t)));
    }

    uint16_t g104LerpU16(uint16_t a, uint16_t b, float t)
    {
        const int v = static_cast<int>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
        return static_cast<uint16_t>(clampInt(v, 0, 0xFFFF));
    }

    GSVertex g104IntersectNearW(const GSVertex &a, const GSVertex &b, float wNear)
    {
        if (g104ScreenClipEnabled())
        {
            const float qNear = g104Qnear();
            const float denomQ = b.q - a.q;
            float t = (std::fabs(denomQ) > 1.0e-8f) ? ((qNear - a.q) / denomQ) : 0.0f;
            t = std::max(0.0f, std::min(1.0f, t));
            GSVertex out{};
            out.x = g104Lerp(a.x, b.x, t);
            out.y = g104Lerp(a.y, b.y, t);
            out.z = g104Lerp(a.z, b.z, t);
            out.r = g104LerpU8(a.r, b.r, t);
            out.g = g104LerpU8(a.g, b.g, t);
            out.b = g104LerpU8(a.b, b.b, t);
            out.a = g104LerpU8(a.a, b.a, t);
            out.q = qNear;
            out.s = g104Lerp(a.s, b.s, t);
            out.t = g104Lerp(a.t, b.t, t);
            out.u = g104LerpU16(a.u, b.u, t);
            out.v = g104LerpU16(a.v, b.v, t);
            out.fog = g104LerpU8(a.fog, b.fog, t);
            return out;
        }

        const float wa = g104SafeW(a.q);
        const float wb = g104SafeW(b.q);
        const float denom = wb - wa;
        float t = (std::fabs(denom) > 1.0e-8f) ? ((wNear - wa) / denom) : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));

        GSVertex out{};
        auto clipLerp = [&](float av, float bv) -> float {
            const float ac = std::isfinite(av) ? (av * wa) : 0.0f;
            const float bc = std::isfinite(bv) ? (bv * wb) : 0.0f;
            return (ac + (bc - ac) * t) / wNear;
        };

        out.x = clipLerp(a.x, b.x);
        out.y = clipLerp(a.y, b.y);
        out.z = clipLerp(a.z, b.z);
        out.r = g104LerpU8(a.r, b.r, t);
        out.g = g104LerpU8(a.g, b.g, t);
        out.b = g104LerpU8(a.b, b.b, t);
        out.a = g104LerpU8(a.a, b.a, t);
        out.q = 1.0f / wNear;
        out.s = g104Lerp(a.s, b.s, t);
        out.t = g104Lerp(a.t, b.t, t);
        out.u = g104LerpU16(a.u, b.u, t);
        out.v = g104LerpU16(a.v, b.v, t);
        out.fog = g104LerpU8(a.fog, b.fog, t);
        return out;
    }

    uint32_t decodePSMCT16(uint16_t pixel)
    {
        const uint32_t r = ((pixel >> 0) & 0x1Fu) << 3;
        const uint32_t g = ((pixel >> 5) & 0x1Fu) << 3;
        const uint32_t b = ((pixel >> 10) & 0x1Fu) << 3;
        const uint32_t a = (pixel & 0x8000u) ? 0x80u : 0u;
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint16_t encodePSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};
    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool writeDepth;
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false, false};
        case 2: // ZB_ONLY
            return {false, true, false};
        case 3: // RGB_ONLY
            return {true, false, true};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        if (sourcePsm == GS_PSM_T4 ||
            sourcePsm == GS_PSM_T4HL ||
            sourcePsm == GS_PSM_T4HH)
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);
            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        else if ((sourcePsm == GS_PSM_T8 || sourcePsm == GS_PSM_T8H) && csm == 0u)
        {
            // F62: T8H shares the T8 CSM1 CLUT swizzle (spec-correct; the old code
            // only swizzled plain T8).
            clutIndex = swizzleClutIndexCSM1(clutIndex);
        }

        return clutIndex;
    }

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    // G45: private off-screen Z buffer for the costume model RTT (fbp=0x139). The game's Z block
    // (0x1a00) aliases menu text VRAM in the runner's flat 4MB, so storing model Z there both
    // corrupts the text and is corrupted BY the per-frame text sprites -> model flicker. This
    // buffer aliases nothing; it is cleared fully once per costume frame (costumeZClear) and the
    // model's GEQUAL test/write route here. 512x512 covers the whole RTT at native res.
    constexpr uint32_t kCostumeZW = 512u;
    constexpr uint32_t kCostumeZH = 512u;
    inline std::vector<uint32_t> &costumeZBuf()
    {
        static std::vector<uint32_t> buf(kCostumeZW * kCostumeZH, 0u);
        return buf;
    }
    inline uint32_t costumeZRead(int x, int y)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return 0u;
        return costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)];
    }
    inline void costumeZWrite(int x, int y, uint32_t z)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return;
        costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)] = z;
    }
    inline void costumeZClear()
    {
        std::fill(costumeZBuf().begin(), costumeZBuf().end(), 0u);
    }

    int wrapTextureCoordinate(int coordinate, uint8_t mode, int size, int regionMin, int regionMax)
    {
        switch (mode)
        {
        case 0: // REPEAT
            return coordinate & (size - 1);
        case 1: // CLAMP
            return clampInt(coordinate, 0, size - 1);
        case 2: // REGION_CLAMP
            return clampInt(coordinate, regionMin, regionMax);
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) &
                                     static_cast<uint32_t>(regionMin)) |
                                    static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

// G143 (perf, DEFAULT OFF): persistent worker pool that rasterizes DISJOINT scanline ranges of a
// single triangle concurrently. A dispatch (`run`) splits [y0,y1] into `lanes` contiguous chunks;
// lane 0 runs on the CALLER, lanes 1..lanes-1 on workers; `run` blocks until all lanes finish, so
// the triangle is fully drawn before drawTriangle returns (cross-triangle order preserved). Single
// caller only (the guest thread inside a VU1 XGKICK) — dispatches never overlap. No allocation on
// the hot path. Deadlock-safe: workers sleep on a generation counter; the barrier is a pending
// countdown. Correct/exact ONLY when the lane job touches disjoint pixels + thread-local scratch.
class GSRowPool
{
public:
    static GSRowPool &instance()
    {
        static GSRowPool p;
        return p;
    }
    int laneCap() const { return static_cast<int>(m_threads.size()) + 1; }

    void run(int y0, int y1, int lanes, const std::function<void(int, int)> &job)
    {
        lanes = std::max(1, std::min(lanes, laneCap()));
        if (lanes <= 1 || y1 < y0)
        {
            if (y1 >= y0)
                job(y0, y1);
            return;
        }
        const int total = y1 - y0 + 1;
        auto bound = [&](int i) { return y0 + static_cast<int>(static_cast<int64_t>(total) * i / lanes); };

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_job = &job;
            m_y0 = y0;
            m_total = total;
            m_lanes = lanes;
            m_nextLane.store(1, std::memory_order_relaxed);
            m_pending = static_cast<int>(m_threads.size()); // every worker wakes and reports
            ++m_gen;
        }
        m_cvWork.notify_all();

        // Caller runs lane 0 itself (no wasted thread).
        job(bound(0), bound(1) - 1);

        std::unique_lock<std::mutex> lk(m_mtx);
        m_cvDone.wait(lk, [&] { return m_pending == 0; });
        m_job = nullptr;
    }

private:
    GSRowPool()
    {
        unsigned hc = std::thread::hardware_concurrency();
        int n = (hc > 1u) ? static_cast<int>(hc) - 1 : 0;
        n = std::min(n, 15);
        for (int i = 0; i < n; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }
    ~GSRowPool()
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
            ++m_gen;
        }
        m_cvWork.notify_all();
        for (auto &t : m_threads)
            if (t.joinable())
                t.join();
    }
    void workerLoop()
    {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lk(m_mtx);
        for (;;)
        {
            m_cvWork.wait(lk, [&] { return m_stop || m_gen != seen; });
            if (m_stop)
                return;
            seen = m_gen;
            const std::function<void(int, int)> *job = m_job;
            const int y0 = m_y0, total = m_total, lanes = m_lanes;
            for (;;)
            {
                const int lane = m_nextLane.fetch_add(1, std::memory_order_relaxed);
                if (lane >= lanes)
                    break;
                const int a = y0 + static_cast<int>(static_cast<int64_t>(total) * lane / lanes);
                const int b = y0 + static_cast<int>(static_cast<int64_t>(total) * (lane + 1) / lanes) - 1;
                lk.unlock();
                if (job && b >= a)
                    (*job)(a, b);
                lk.lock();
            }
            if (--m_pending == 0)
                m_cvDone.notify_one();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mtx;
    std::condition_variable m_cvWork, m_cvDone;
    const std::function<void(int, int)> *m_job = nullptr;
    int m_y0 = 0, m_total = 0, m_lanes = 1, m_pending = 0;
    std::atomic<int> m_nextLane{0};
    uint64_t m_gen = 0;
    bool m_stop = false;
};

// ===== G144 FRAME-LEVEL TILE-BINNING (G168 DEFAULT 8 LANES, DC2_G144_NO_TILEBIN disables) =====
// Defer deferrable triangles (fast-path textured tris to a display framebuffer, non-RTT) into a
// display list instead of rasterizing each immediately; at a flush (a non-deferrable primitive, an
// fbp change, or frame end) rasterize the WHOLE list with T threads, each owning a disjoint
// horizontal BAND of scanlines and replaying every overlapping entry IN SUBMISSION ORDER clipped to
// its band. Bit-exact by the SAME argument as G143 (disjoint rows ⇒ disjoint FB/Z writes; in-order
// per band ⇒ Z/blend order preserved), but the parallelism now spans the WHOLE FRAME's triangle
// stream (one dispatch, thousands of tris/thread) instead of one triangle — so it is not limited by
// per-triangle size the way G143's intra-triangle row split is. A full POD snapshot of the exact GS
// draw state drawTriangle reads (ctx/prim/texa/texclut/pabe/3 verts) makes each entry self-contained.
struct G144Entry
{
    GSContext ctx;
    GSPrimReg prim;
    GSTexaReg texa;
    GSTexClutReg texclut;
    bool pabe;
    bool titleRockScope;
    bool townDepthScope;
    GSVertex v[3];
    int minY, maxY; // screen-space bbox rows (post-scissor) for band-overlap prefilter
    G149Buf decoded; // G149: pre-decoded texture buffer (null unless texcache eligible); kept alive here
};
static std::vector<G144Entry> g_g144List;       // reused across frames (clear keeps capacity)
// G219 diagnostic snapshot buffer (see the [G219:cap]/[G219:dif] probes; inert unless
// DC2_G219_SKYPX=1).
uint8_t g_g219Snap[0x8000];
// G219 transition sentinel: called at raster/transfer choke points with a location tag; logs
// whenever the watched work-page qword (0x278420, blue after the zoom screen-copy) changes value,
// bracketing WHICH processing step the change happened inside. Inert unless DC2_G219_SKYPX=1.
static std::atomic<uint64_t> g_g219SentinelVal{0xDEADBEEFDEADBEEFull};

#if defined(_WIN32)
// G219 write-watchpoint (default-off, DC2_G219_GUARD=1 in addition to DC2_G219_SKYPX=1): make the
// host page holding VRAM offset 0x278400 read-only once the sentinel sees the post-copy content;
// the first write then faults, the vectored handler logs the writer's instruction address + thread
// and restores write access. Identifies the zeroing writer definitively (every prior software
// choke-point instrumentation missed it).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace
{
    uint8_t *g_g219GuardBase = nullptr;   // host address of the guarded page
    std::atomic<bool> g_g219GuardArmed{false};

    LONG CALLBACK g219VectoredHandler(PEXCEPTION_POINTERS info)
    {
        if (!info || !info->ExceptionRecord ||
            info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;
        const ULONG_PTR isWrite = info->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR addr = info->ExceptionRecord->ExceptionInformation[1];
        uint8_t *base = g_g219GuardBase;
        if (!base || addr < reinterpret_cast<ULONG_PTR>(base) ||
            addr >= reinterpret_cast<ULONG_PTR>(base) + 0x1000u)
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD oldProt = 0;
        VirtualProtect(base, 0x1000u, PAGE_READWRITE, &oldProt);
        g_g219GuardArmed.store(false, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_hits{0};
        const uint32_t n = s_hits.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
        {
            HMODULE mod = nullptr;
            const void *rip = info->ExceptionRecord->ExceptionAddress;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod);
            char name[260] = {0};
            if (mod)
                GetModuleFileNameA(mod, name, sizeof(name) - 1);
            std::fprintf(stderr,
                "[G219:guard] n=%u tid=%lu write=%llu vramOff=0x%llx rip=%p module=%s base=%p delta=0x%llx\n",
                n, (unsigned long)GetCurrentThreadId(),
                (unsigned long long)isWrite,
                (unsigned long long)(addr - reinterpret_cast<ULONG_PTR>(base) + 0x278000u),
                rip, name[0] ? name : "?", (void *)mod,
                mod ? (unsigned long long)(reinterpret_cast<ULONG_PTR>(rip) -
                                           reinterpret_cast<ULONG_PTR>(mod))
                    : 0ull);
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void g219GuardArm(uint8_t *vramBase)
    {
        static const bool s_on = (std::getenv("DC2_G219_GUARD") != nullptr);
        if (!s_on || !vramBase)
            return;
        static bool s_handler = []() {
            AddVectoredExceptionHandler(1, g219VectoredHandler);
            return true;
        }();
        (void)s_handler;
        bool expected = false;
        if (!g_g219GuardArmed.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            return;
        g_g219GuardBase = vramBase + 0x278000u;
        DWORD oldProt = 0;
        VirtualProtect(g_g219GuardBase, 0x1000u, PAGE_READONLY, &oldProt);
    }
}
#else
namespace
{
    void g219GuardArm(uint8_t *) {}
}
#endif
void g219Sentinel(const uint8_t *vram, uint32_t vramSize, const char *where)
{
    static const bool s_on = (std::getenv("DC2_G219_SKYPX") != nullptr);
    if (!s_on || !vram || 0x278420u + 8u > vramSize)
        return;
    // Ring of the last 8 sentinel visits (single GS worker thread proven by the tid trace), so a
    // detected transition can name its exact predecessor event, not just the detector.
    static char s_ring[8][96];
    static uint32_t s_ringPos = 0;
    uint64_t v = 0;
    std::memcpy(&v, vram + 0x278420u, 8);
    const uint64_t prev = g_g219SentinelVal.exchange(v, std::memory_order_relaxed);
    if (prev != v && prev != 0xDEADBEEFDEADBEEFull)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 600u)
        {
            const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            std::fprintf(stderr, "[G219:trans] n=%u tid=%04x at=%s old=%016llx new=%016llx\n",
                         n, (unsigned)(tid & 0xFFFFu), where,
                         (unsigned long long)prev, (unsigned long long)v);
            if (n < 40u)
                for (uint32_t k = 0; k < 8u; ++k)
                    std::fprintf(stderr, "[G219:ring] -%u %s\n", 8u - k,
                                 s_ring[(s_ringPos + k) & 7u]);
        }
    }
    std::snprintf(s_ring[s_ringPos & 7u], sizeof(s_ring[0]), "%s v=%016llx",
                  where, (unsigned long long)v);
    s_ringPos = (s_ringPos + 1u) & 7u;
    g219GuardArm(const_cast<uint8_t *>(vram));
}
static int g_g144Threads = -1;                  // -1 = env not read yet; 0/1 = off; >1 = lanes
static uint32_t g_g144BlockFbp = 0xFFFFFFFFu;   // fbp of the current deferred block (flush on change)
static GS *g_g144LastGs = nullptr;              // live GS for VRAM at flush (set each capture)
static uint8_t *g_g144LastVram = nullptr;       // member-safe frame-boundary alias, set beside LastGs
// Flush closures, assigned ONCE inside GSRasterizer::drawPrimitive so they keep that member's friend
// access to GS/private drawTriangle even when invoked later from g144FlushPending() (frame end).
static std::function<void()> g_g144FlushFn;     // PARALLEL band replay (mid-frame, on guest thread)
static std::function<void()> g_g144FlushSeqFn;  // SEQUENTIAL drain (frame end; no pool, full range)
// Replay control (thread_local: each band lane its own; set only during a flush).
static thread_local bool t_g144InReplay = false;
static thread_local bool t_g144Banded = false;
static thread_local int t_g144BandY0 = 0;
static thread_local int t_g144BandY1 = 0;
static thread_local size_t t_g258EntryIndex = static_cast<size_t>(-1);

// ===== G248 MEASURE ONLY (DEFAULT OFF, DC2_G248_STAT=1) =====
// G247 found that A+D XYZ2 kicks to the character-RTT work targets dominate map_0's
// packed-register time, but that outer timing includes any pending display-list drain and
// GPU-depth synchronization before drawSprite() starts.  Time the sprite body itself here and
// aggregate by target / texture / blend state.  Pixel coverage is computed from the already-
// clipped loop rectangle; no per-pixel instrumentation is added.  This keeps the discriminator
// quiet and cheap enough to answer whether G248 should target fill work or flush fragmentation.
static const bool g_g248Stat = (std::getenv("DC2_G248_STAT") != nullptr);
static constexpr uint32_t kG248TargetCount = 5u;
static constexpr uint32_t kG248StateCount = 4u; // bit0=tme, bit1=abe
static constexpr uint32_t kG248BucketCount = kG248TargetCount * kG248StateCount;
static std::atomic<uint64_t> g_g248SpriteCalls[kG248BucketCount]{};
static std::atomic<uint64_t> g_g248SpritePixels[kG248BucketCount]{};
static std::atomic<uint64_t> g_g248SpriteNs[kG248BucketCount]{};

static int g248TargetIndex(uint32_t fbp)
{
    switch (fbp)
    {
    case 0x139u: return 0;
    case 0x13cu: return 1;
    case 0x143u: return 2;
    case 0x146u: return 3;
    case 0x155u: return 4;
    default:     return -1;
    }
}

// G255 (DEFAULT OFF): promote the already-isolated G252 RTT batches through the existing
// all-or-nothing G178 GPU backend without crossing any upload, local-copy, or FBP boundary.
// The important RTT ownership rule is that a GPU color write must advance the same page
// generations used by the texture cache; otherwise a later display composite can legally reuse
// the pre-RTT texture.  DC2_G255_VERIFY adds an exact same-run oracle: save the pre-GPU target,
// capture the GPU result, restore the target, run the proven CPU replay, then compare every pixel.
// G261 (DEFAULT ON since G266; stack kill DC2_G26X_NO_NATIVE=1): GPU-resident RTT waves on the G260 command-graph
// substrate. RTT batches render through the G255 backend but SKIP the per-flush
// readback+swizzle-writeback; the display composite samples the producer's FBO texture directly;
// guest VRAM materializes only at a real CPU-consumer edge (CPU band replay overlap, inline
// primitive overlap, transfer overlap, local→host read). Ownership contract: every CPU write to
// a resident target's pages MUST pass a g261 materialize hook first; an escaped writer trips the
// gen invariant and residency is dropped loudly (VRAM stays authoritative), never read back over
// newer guest bytes. Implies DC2_G260_NR and the G255 RTT routing; declines under the exact
// oracles and the separate G242/G256 ownership arms (their contracts assume per-flush readback).
static bool g261WaveOn()
{
    static const bool on = []() {
        // G266 PROMOTION (2026-07-16): the native-renderer wave stack (G260 graph + G261 waves +
        // G262 widening + G263 bilinear bind + G264 upload mirror + G265 display-Z + G266 fbw
        // split) is DEFAULT-ON after the full G253 promotion matrix passed on the final binary.
        // Rollback: DC2_G26X_NO_NATIVE=1 kills the whole stack; DC2_G261_NO_WAVE=1 kills waves
        // only (NR then follows its own DC2_G260_NR flag). DC2_G261_WAVE=1 stays accepted (it is
        // now redundant); DC2_G261_WAVE=0 also disables.
        if (envFlagEnabled("DC2_G26X_NO_NATIVE") || envFlagEnabled("DC2_G261_NO_WAVE") ||
            envFlagDisabled("DC2_G261_WAVE"))
            return false;
        // Exact same-run oracles compare VRAM after every flush — residency would break their
        // contract, so they force the readback path (G255 semantics) instead of engaging waves.
        if (envFlagEnabled("DC2_G255_VERIFY") || envFlagEnabled("DC2_G248_VERIFY"))
            return false;
        // Separate architecture arms with their own ownership models.
        if (envFlagEnabled("DC2_G242_GPU_DEPTH") || envFlagEnabled("DC2_G256_EXACT_RTT"))
            return false;
        // Waves ride the G144 capture machinery and the G178 backend.
        if (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
            return false;
        if (envFlagEnabled("DC2_G178_NO_GPU") || envFlagDisabled("DC2_G178_GPU"))
            return false;
        // Diagnostics that read guest VRAM outside the materialize hooks (capture-time texture
        // decode, raw-VRAM dumps) keep the readback-per-flush contract instead.
        if (std::getenv("DC2_G149_TEXCACHE") != nullptr ||
            std::getenv("DC2_G38_VRAMDUMP") != nullptr)
            return false;
        return true;
    }();
    return on;
}

static bool g255GpuRttOn()
{
    // G256 is the exact-arithmetic successor arm and implicitly selects the same proven G255
    // dependency/eligibility path.  Requiring two environment switches made it too easy to run
    // the exact shader without ever exercising it.
    // G261 waves route the same five targets through the same backend path (with readback
    // deferred to real consumer edges), so the wave lever implies this gate too.
    static const bool on = envFlagEnabled("DC2_G255_GPU_RTT") ||
                           envFlagEnabled("DC2_G256_EXACT_RTT") ||
                           g261WaveOn();
    return on;
}

static bool g256ExactRttOn()
{
    static const bool on = envFlagEnabled("DC2_G256_EXACT_RTT");
    return on;
}

// ===== G262 RTT-family classifier widening (default-on since G266; requires waves) =====
// The G262 census split G261's rttRej(classify=1836): ~89% is the universal-Z depth reject
// INSIDE g178ClassifyEntry (character-body triangles: zte=1 ztst=GEQUAL zmsk=0 zpsm=Z24 —
// G261's wave-level "depth=0" counter was unreachable because it required DC2_G242_GPU_DEPTH),
// ~11% an additive (Cs-0)*(FIX=128)>>7+Cd blend whose entries also Z-test. Widening therefore
// means: per-wave NON-PERSISTENT guest depth (fresh Z-window upload each wave + immediate
// readback after execution — no cross-flush GPU Z ownership, so the G242/G249 dirty-generation
// trap cannot occur by construction) plus the additive blend mode, both scoped to the five RTT
// targets only. Riding the wave lever inherits every verify/exact/G242/dump decline.
static bool g262WideOn()
{
    // G266 promotion: default-on when waves are on. Kill: DC2_G262_NO_WIDE=1 (or the wave/stack
    // kills upstream). DC2_G262_WIDE=1 stays accepted (redundant); =0 disables.
    static const bool on = !envFlagEnabled("DC2_G262_NO_WIDE") &&
                           !envFlagDisabled("DC2_G262_WIDE") && g261WaveOn();
    return on;
}

// ===== G265 display-batch non-persistent guest depth (opt-in) =====
// Reuse G262's wave-granular Z upload/readback for display targets while keeping color on the
// normal G178 readback path. VRAM remains authoritative for both color and Z after every batch;
// unlike G242, this creates no persistent display-depth ownership to escape across CPU writers.
static bool g265DisplayZWaveOn()
{
    // G266 promotion: default-on when waves+widening are on. Kill: DC2_G265_NO_DISPLAY_ZWAVE=1
    // (or the wave/stack kills upstream). DC2_G265_DISPLAY_ZWAVE=1 stays accepted; =0 disables.
    static const bool on = !envFlagEnabled("DC2_G265_NO_DISPLAY_ZWAVE") &&
                           !envFlagDisabled("DC2_G265_DISPLAY_ZWAVE") && g262WideOn();
    return on;
}

static bool g265CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G265_CENSUS");
    return on;
}

static bool g258TraceOn()
{
    static const bool on = envFlagEnabled("DC2_G258_TRACE");
    return on;
}

static bool g255VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G255_VERIFY");
    return on;
}

static bool g286VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G286_VERIFY");
    return on;
}

struct G255VerifyState
{
    bool pending = false;
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint32_t fbBlock = 0;
    int fbW = 0;
    int rowLo = 0;
    int rowHi = -1;
    size_t entries = 0;
    uint32_t prim = 0;
    uint32_t tme = 0;
    uint32_t abe = 0;
    uint64_t alpha = 0;
    uint32_t tbp = 0;
    uint32_t tpsm = 0;
    uint32_t tfx = 0;
    uint64_t tex1 = 0;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    uint16_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uint64_t traceBatch = 0;
    std::vector<uint32_t> before;
    std::vector<uint32_t> gpu;
};

static G255VerifyState g_g255Verify;
static std::atomic<uint64_t> g_g255Attempts[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Gpu[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Checks[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Bad[kG248TargetCount]{};
static std::atomic<uint64_t> g_g286VerifyChecks{0}, g_g286VerifyBad{0};

static void g255Report()
{
    static const bool s_stat = envFlagEnabled("DC2_G255_STAT") || g255VerifyOn();
    if (!s_stat)
        return;
    std::fprintf(stderr, "[G255:stat]");
    static constexpr uint32_t kFbp[kG248TargetCount] = {
        0x139u, 0x13cu, 0x143u, 0x146u, 0x155u
    };
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        std::fprintf(stderr, " %03x(a=%llu gpu=%llu chk=%llu bad=%llu)", kFbp[i],
                     static_cast<unsigned long long>(g_g255Attempts[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Gpu[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Checks[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Bad[i].load(std::memory_order_relaxed)));
    std::fprintf(stderr, "\n");
}

static void g255NoteAttempt(uint32_t fbp)
{
    const int i = g248TargetIndex(fbp);
    if (i < 0)
        return;
    const uint64_t n = g_g255Attempts[i].fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 256u) == 0u)
        g255Report();
}

static void g255BeginVerify(uint8_t *vram, uint32_t vramSize, uint32_t fbp, uint32_t fbw,
                            int fbW, int rowLo, int rowHi)
{
    g_g255Verify.pending = false;
    if (!(g255VerifyOn() || (g286VerifyOn() && fbp == 0x13bu)) ||
        !vram || rowHi < rowLo)
        return;
    G255VerifyState &s = g_g255Verify;
    s.fbp = fbp;
    s.fbw = fbw;
    s.fbBlock = GSInternal::framePageBaseToBlock(fbp);
    s.fbW = fbW;
    s.rowLo = rowLo;
    s.rowHi = rowHi;
    s.entries = g_g144List.size();
    if (g258TraceOn() && fbp == 0x146u)
    {
        static uint64_t s_g258Batch = 0;
        s.traceBatch = ++s_g258Batch;
    }
    else
    {
        s.traceBatch = 0;
    }
    if (!g_g144List.empty())
    {
        const G144Entry &e = g_g144List.front();
        s.prim = static_cast<uint32_t>(e.prim.type);
        s.tme = e.prim.tme ? 1u : 0u;
        s.abe = e.prim.abe ? 1u : 0u;
        s.alpha = e.ctx.alpha;
        s.tbp = e.ctx.tex0.tbp0;
        s.tpsm = e.ctx.tex0.psm;
        s.tfx = e.ctx.tex0.tfx;
        s.tex1 = e.ctx.tex1;
        s.x0 = e.v[0].x; s.y0 = e.v[0].y;
        s.x1 = e.v[1].x; s.y1 = e.v[1].y;
        s.u0 = e.v[0].u; s.v0 = e.v[0].v;
        s.u1 = e.v[1].u; s.v1 = e.v[1].v;
    }
    const size_t count = static_cast<size_t>(rowHi - rowLo + 1) * static_cast<size_t>(fbW);
    s.before.resize(count);
    s.gpu.resize(count);
    for (int y = rowLo; y <= rowHi; ++y)
        for (int x = 0; x < fbW; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            uint32_t px = 0;
            if (off + 4u <= vramSize)
                std::memcpy(&px, vram + off, 4);
            s.before[static_cast<size_t>(y - rowLo) * fbW + x] = px;
        }
}

static void g255StageGpuVerify(uint8_t *vram, uint32_t vramSize,
                               const std::vector<uint32_t> &readback, int fbH)
{
    G255VerifyState &s = g_g255Verify;
    if (!(g255VerifyOn() || (g286VerifyOn() && s.fbp == 0x13bu)) ||
        !vram || s.before.empty() ||
        readback.size() != static_cast<size_t>(s.fbW) * static_cast<size_t>(fbH))
        return;
    for (int y = s.rowLo; y <= s.rowHi; ++y)
        for (int x = 0; x < s.fbW; ++x)
        {
            const size_t i = static_cast<size_t>(y - s.rowLo) * s.fbW + x;
            const size_t gpuI = static_cast<size_t>(fbH - 1 - y) * s.fbW + x;
            s.gpu[i] = readback[gpuI];
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, s.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            if (off + 4u <= vramSize)
                std::memcpy(vram + off, &s.before[i], 4);
        }
    s.pending = true;
}

static void g255CompareCpuVerify(uint8_t *vram, uint32_t vramSize)
{
    G255VerifyState &s = g_g255Verify;
    if (!s.pending || !vram)
        return;
    uint64_t bad = 0;
    uint64_t badRgb = 0;
    uint64_t badAlpha = 0;
    uint64_t coverage = 0;
    int firstX = -1, firstY = -1;
    uint32_t firstCpu = 0, firstGpu = 0;
    for (int y = s.rowLo; y <= s.rowHi; ++y)
        for (int x = 0; x < s.fbW; ++x)
        {
            const size_t i = static_cast<size_t>(y - s.rowLo) * s.fbW + x;
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, s.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            uint32_t cpu = 0;
            if (off + 4u <= vramSize)
                std::memcpy(&cpu, vram + off, 4);
            if (cpu != s.gpu[i])
            {
                if (bad == 0)
                {
                    firstX = x; firstY = y; firstCpu = cpu; firstGpu = s.gpu[i];
                }
                ++bad;
                if ((cpu & 0x00FFFFFFu) != (s.gpu[i] & 0x00FFFFFFu))
                    ++badRgb;
                if ((cpu & 0xFF000000u) != (s.gpu[i] & 0xFF000000u))
                    ++badAlpha;
                if (((cpu & 0x00FFFFFFu) == 0u) != ((s.gpu[i] & 0x00FFFFFFu) == 0u))
                    ++coverage;
            }
        }
    const int ti = g248TargetIndex(s.fbp);
    if (ti >= 0)
    {
        const uint64_t n = g_g255Checks[ti].fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0)
            g_g255Bad[ti].fetch_add(bad, std::memory_order_relaxed);
        if (bad != 0 || (n % 64u) == 0u)
            std::fprintf(stderr,
                         "[G255:verify] fbp=0x%x n=%llu rows=%d..%d pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "batch(entries=%zu prim=%u tme=%u abe=%u alpha=%016llx tbp=0x%x psm=0x%x tfx=%u) "
                         "spr=(%.1f,%.1f u=%u,%u)-(%.1f,%.1f u=%u,%u) tex1=%llx "
                         "first=(%d,%d cpu=%08x gpu=%08x)\n",
                         s.fbp, static_cast<unsigned long long>(n), s.rowLo, s.rowHi,
                         s.gpu.size(), static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(badRgb),
                         static_cast<unsigned long long>(badAlpha),
                         static_cast<unsigned long long>(coverage),
                         s.entries, s.prim, s.tme, s.abe,
                         static_cast<unsigned long long>(s.alpha), s.tbp, s.tpsm, s.tfx,
                         s.x0, s.y0, static_cast<uint32_t>(s.u0), static_cast<uint32_t>(s.v0),
                         s.x1, s.y1, static_cast<uint32_t>(s.u1), static_cast<uint32_t>(s.v1),
                         static_cast<unsigned long long>(s.tex1),
                         firstX, firstY, firstCpu, firstGpu);
    }
    else if (g286VerifyOn() && s.fbp == 0x13bu)
    {
        const uint64_t n =
            g_g286VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g286VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (bad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G286:verify] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "rows=%d..%d entries=%zu prim=%u tme=%u tbp=0x%x psm=0x%x "
                         "first=(%d,%d cpu=%08x gpu=%08x) cumulativeBad=%llu\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme, s.tbp, s.tpsm,
                         firstX, firstY, firstCpu, firstGpu,
                         (unsigned long long)g_g286VerifyBad.load(std::memory_order_relaxed));
    }
    s.pending = false;
}

// G253 (diagnostic only, default off): attribute G252's mid-frame drains without changing
// capture, replay, or barrier behavior.  The compact transition matrix answers whether the
// remaining cost is target switching (which needs a dependency-aware mixed-target design) or
// non-deferrable primitives (which may admit a narrower eligibility change).
static int g253BarrierFbpBucket(uint32_t fbp)
{
    if (fbp == 0x0u) return 0;
    if (fbp == 0x68u) return 1;
    const int rtt = g248TargetIndex(fbp);
    return rtt >= 0 ? (2 + rtt) : 7;
}

static void g253NoteMidBarrier(bool fbpSwitch, uint32_t fromFbp, uint32_t toFbp,
                               size_t pendingEntries, uint32_t primType, bool tme, bool abe)
{
    static const bool s_on = envFlagEnabled("DC2_G253_BARRIER_STAT");
    if (!s_on)
        return;

    static std::atomic<uint64_t> s_seq{0};
    static std::atomic<uint64_t> s_fbpSwitches{0};
    static std::atomic<uint64_t> s_nonDeferrable{0};
    static std::atomic<uint64_t> s_entries{0};
    static std::atomic<uint64_t> s_transition[8][8]{};
    static std::atomic<uint64_t> s_nonDefClass[8][4]{};
    static constexpr const char *kNames[8] = {
        "000", "068", "139", "13c", "143", "146", "155", "other"
    };

    if (fbpSwitch)
        s_fbpSwitches.fetch_add(1u, std::memory_order_relaxed);
    else
        s_nonDeferrable.fetch_add(1u, std::memory_order_relaxed);
    s_entries.fetch_add(static_cast<uint64_t>(pendingEntries), std::memory_order_relaxed);
    s_transition[g253BarrierFbpBucket(fromFbp)][g253BarrierFbpBucket(toFbp)]
        .fetch_add(1u, std::memory_order_relaxed);
    if (!fbpSwitch)
    {
        const uint32_t state = (tme ? 1u : 0u) | (abe ? 2u : 0u);
        s_nonDefClass[std::min<uint32_t>(primType, 7u)][state]
            .fetch_add(1u, std::memory_order_relaxed);
    }

    const uint64_t seq = s_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((seq % 1024u) != 0u)
        return;

    const uint64_t switches = s_fbpSwitches.exchange(0u, std::memory_order_relaxed);
    const uint64_t nonDef = s_nonDeferrable.exchange(0u, std::memory_order_relaxed);
    const uint64_t entries = s_entries.exchange(0u, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G253:barrier] n=%llu fbpSwitch=%llu nondef=%llu entries=%llu avgEntries=%.2f\n",
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned long long>(switches),
                 static_cast<unsigned long long>(nonDef),
                 static_cast<unsigned long long>(entries),
                 static_cast<double>(entries) / 1024.0);
    for (int from = 0; from < 8; ++from)
    {
        for (int to = 0; to < 8; ++to)
        {
            const uint64_t count = s_transition[from][to].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G253:transition] %s->%s count=%llu\n",
                             kNames[from], kNames[to],
                             static_cast<unsigned long long>(count));
        }
    }
    for (int prim = 0; prim < 8; ++prim)
    {
        for (int state = 0; state < 4; ++state)
        {
            const uint64_t count = s_nonDefClass[prim][state].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G253:nondef] prim=%d tme=%d abe=%d count=%llu\n",
                             prim, state & 1, (state >> 1) & 1,
                             static_cast<unsigned long long>(count));
        }
    }
}

class G248SpriteScope
{
public:
    G248SpriteScope(uint32_t fbp, bool tme, bool abe)
    {
        const int target = g_g248Stat ? g248TargetIndex(fbp) : -1;
        if (target < 0)
            return;
        m_bucket = target * static_cast<int>(kG248StateCount) +
                   (tme ? 1 : 0) + (abe ? 2 : 0);
        m_start = std::chrono::steady_clock::now();
    }

    ~G248SpriteScope()
    {
        if (m_bucket < 0)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_start).count());
        g_g248SpriteCalls[m_bucket].fetch_add(1u, std::memory_order_relaxed);
        g_g248SpritePixels[m_bucket].fetch_add(m_pixels, std::memory_order_relaxed);
        g_g248SpriteNs[m_bucket].fetch_add(ns, std::memory_order_relaxed);
    }

    void setPixels(uint64_t pixels) { m_pixels = pixels; }

private:
    int m_bucket = -1;
    uint64_t m_pixels = 0u;
    std::chrono::steady_clock::time_point m_start{};
};

static void g248Report()
{
    if (!g_g248Stat)
        return;
    static uint64_t s_frame = 0u;
    static uint64_t s_lastCalls[kG248BucketCount]{};
    static uint64_t s_lastPixels[kG248BucketCount]{};
    static uint64_t s_lastNs[kG248BucketCount]{};
    if ((++s_frame % 30u) != 0u)
        return;

    static constexpr uint32_t kTargets[kG248TargetCount] = {
        0x139u, 0x13cu, 0x143u, 0x146u, 0x155u
    };
    uint64_t totalCalls = 0u, totalPixels = 0u, totalNs = 0u;
    for (uint32_t target = 0u; target < kG248TargetCount; ++target)
    {
        for (uint32_t state = 0u; state < kG248StateCount; ++state)
        {
            const uint32_t bucket = target * kG248StateCount + state;
            const uint64_t calls = g_g248SpriteCalls[bucket].load(std::memory_order_relaxed);
            const uint64_t pixels = g_g248SpritePixels[bucket].load(std::memory_order_relaxed);
            const uint64_t ns = g_g248SpriteNs[bucket].load(std::memory_order_relaxed);
            const uint64_t dc = calls - s_lastCalls[bucket];
            const uint64_t dp = pixels - s_lastPixels[bucket];
            const uint64_t dn = ns - s_lastNs[bucket];
            s_lastCalls[bucket] = calls;
            s_lastPixels[bucket] = pixels;
            s_lastNs[bucket] = ns;
            if (dc == 0u)
                continue;
            totalCalls += dc;
            totalPixels += dp;
            totalNs += dn;
            std::fprintf(stderr,
                         "[G248:rttspr] frame=%llu fbp=0x%x tme=%u abe=%u calls/f=%.2f "
                         "pixels/f=%.0f ms/f=%.3f ns/px=%.1f us/call=%.1f\n",
                         static_cast<unsigned long long>(s_frame), kTargets[target],
                         state & 1u, (state >> 1u) & 1u,
                         static_cast<double>(dc) / 30.0,
                         static_cast<double>(dp) / 30.0,
                         static_cast<double>(dn) / 30.0e6,
                         dp ? static_cast<double>(dn) / static_cast<double>(dp) : 0.0,
                         static_cast<double>(dn) / static_cast<double>(dc) / 1.0e3);
        }
    }
    std::fprintf(stderr,
                 "[G248:rttspr-total] frame=%llu calls/f=%.2f pixels/f=%.0f ms/f=%.3f "
                 "ns/px=%.1f us/call=%.1f\n",
                 static_cast<unsigned long long>(s_frame),
                 static_cast<double>(totalCalls) / 30.0,
                 static_cast<double>(totalPixels) / 30.0,
                 static_cast<double>(totalNs) / 30.0e6,
                 totalPixels ? static_cast<double>(totalNs) / static_cast<double>(totalPixels) : 0.0,
                 totalCalls ? static_cast<double>(totalNs) / static_cast<double>(totalCalls) / 1.0e3 : 0.0);
}

// ===== G156 probe (DEFAULT OFF, DC2_G156_STAT=1) — attribute the ~78 ms worker drawPrimitive. =====
// G155 measured mid-flush=0, so the 78 ms billed to drawPrimitive under MTGS+G144 is NOT flush time.
// It is either the deferred-snapshot cost (cheap, should be ~ms) OR inline serial raster of tris that
// FAIL deferrable144 (drawTriangle on the single worker thread). This probe counts deferred vs inline
// tris, TIMES the inline raster, and records WHY each inline tri was not deferrable — so G157 knows
// whether the lever is "widen the defer eligibility → parallel 8-band replay" or "the sampler itself".
static const bool g_g156Stat = (std::getenv("DC2_G156_STAT") != nullptr);
static std::atomic<uint64_t> g_g156DeferredTri{0}; // tris deferred to g_g144List (parallel raster)
static std::atomic<uint64_t> g_g156InlineTri{0};   // tris rasterized inline on the calling thread
static std::atomic<uint64_t> g_g156InlineTriNs{0}; // wall-time of that inline drawTriangle
static std::atomic<uint64_t> g_g156CaptureNs{0};   // wall-time of the deferred snapshot+push_back body
static std::atomic<uint64_t> g_g156PrologueNs{0};  // wall-time from drawPrimitive entry to the capture
static std::atomic<uint64_t> g_g156NoTme{0};       // inline reason: untextured (tme=0)
static std::atomic<uint64_t> g_g156BadFbp{0};      // inline reason: fbp not 0x0/0x68 (RTT/other target)
static std::atomic<uint64_t> g_g156Diag{0};        // inline reason: a diagnostic trace forced eager
static void g156Report()
{
    std::fprintf(stderr,
                 "[G156:draw] deferTri=%llu prologueMs=%.1f captureMs=%.1f (%.2f us/tri) inlineTri=%llu inlineMs=%.1f (%.2f us/tri) | notme=%llu badfbp=%llu diag=%llu\n",
                 (unsigned long long)g_g156DeferredTri.load(std::memory_order_relaxed),
                 g_g156PrologueNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156CaptureNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156DeferredTri.load(std::memory_order_relaxed)
                     ? (double)g_g156CaptureNs.load(std::memory_order_relaxed) / 1e3 /
                           (double)g_g156DeferredTri.load(std::memory_order_relaxed)
                     : 0.0,
                 (unsigned long long)g_g156InlineTri.load(std::memory_order_relaxed),
                 g_g156InlineTriNs.load(std::memory_order_relaxed) / 1e6,
                 g_g156InlineTri.load(std::memory_order_relaxed)
                     ? (double)g_g156InlineTriNs.load(std::memory_order_relaxed) / 1e3 /
                           (double)g_g156InlineTri.load(std::memory_order_relaxed)
                     : 0.0,
                 (unsigned long long)g_g156NoTme.load(std::memory_order_relaxed),
                 (unsigned long long)g_g156BadFbp.load(std::memory_order_relaxed),
                 (unsigned long long)g_g156Diag.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

struct G144BlockRange
{
    uint32_t first = 0;
    uint32_t last = 0;
    bool valid = false;
};

static bool g144PsmPageDims(uint8_t psm, uint32_t &pageW, uint32_t &pageH, bool &halfWidthBw)
{
    halfWidthBw = false;
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        pageW = 64u;
        pageH = 32u;
        return true;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        pageW = 64u;
        pageH = 64u;
        return true;
    case GS_PSM_T8:
        pageW = 128u;
        pageH = 64u;
        halfWidthBw = true;
        return true;
    case GS_PSM_T4:
        pageW = 128u;
        pageH = 128u;
        halfWidthBw = true;
        return true;
    default:
        return false;
    }
}

static G144BlockRange g144FullVramRange()
{
    return {0u, 0x3FFFu, true};
}

static G144BlockRange g144RangeForRect(uint32_t baseBlock,
                                       uint8_t psm,
                                       uint32_t bw,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t w,
                                       uint32_t h)
{
    if (w == 0u || h == 0u)
        return {};

    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    if (!g144PsmPageDims(psm, pageW, pageH, halfWidthBw))
        return g144FullVramRange();

    uint32_t pageBw = (bw != 0u) ? bw : 1u;
    if (halfWidthBw)
        pageBw = std::max<uint32_t>((pageBw + 1u) >> 1u, 1u);

    const uint64_t x0Page = static_cast<uint64_t>(x) / pageW;
    const uint64_t x1Page = (static_cast<uint64_t>(x) + static_cast<uint64_t>(w) - 1u) / pageW;
    const uint64_t y0Page = static_cast<uint64_t>(y) / pageH;
    const uint64_t y1Page = (static_cast<uint64_t>(y) + static_cast<uint64_t>(h) - 1u) / pageH;

    const uint64_t firstPage = y0Page * pageBw + x0Page;
    const uint64_t lastPage = y1Page * pageBw + x1Page;
    const uint64_t firstBlock64 = static_cast<uint64_t>(baseBlock) + firstPage * 32u;
    const uint64_t lastBlock64 = static_cast<uint64_t>(baseBlock) + lastPage * 32u + 31u;

    if (firstBlock64 > 0x3FFFu)
        return {};
    return {static_cast<uint32_t>(firstBlock64),
            static_cast<uint32_t>(std::min<uint64_t>(lastBlock64, 0x3FFFu)),
            true};
}

static bool g144RangeOverlaps(const G144BlockRange &a, const G144BlockRange &b)
{
    return a.valid && b.valid && a.first <= b.last && b.first <= a.last;
}

static bool g144IsPalettedPsm(uint8_t psm)
{
    return psm == GS_PSM_T8 || psm == GS_PSM_T8H ||
           psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

static G144BlockRange g144TextureRange(const G144Entry &e)
{
    const GSTex0Reg &tex = e.ctx.tex0;
    const uint32_t texW = 1u << std::min<uint8_t>(tex.tw, 15u);
    const uint32_t texH = 1u << std::min<uint8_t>(tex.th, 15u);
    return g144RangeForRect(tex.tbp0, tex.psm, tex.tbw, 0u, 0u, texW, texH);
}

static G144BlockRange g144ClutRange(const G144Entry &e)
{
    const GSTex0Reg &tex = e.ctx.tex0;
    if (!g144IsPalettedPsm(tex.psm))
        return {};

    const uint32_t clutBw = (e.texclut.cbw != 0u) ? static_cast<uint32_t>(e.texclut.cbw) : 1u;
    // The sampler may swizzle CSM1/CSA indices. Use the whole 16x16 CLUT tile so dirty-checking
    // cannot miss a palette write because of index remapping.
    return g144RangeForRect(tex.cbp, tex.cpsm, clutBw,
                            static_cast<uint32_t>(e.texclut.cou),
                            static_cast<uint32_t>(e.texclut.cov),
                            16u, 16u);
}

static G144BlockRange g144TargetRange(const G144Entry &e)
{
    const GSContext &ctx = e.ctx;
    const uint32_t baseBlock = framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t x0 = static_cast<uint32_t>(ctx.scissor.x0);
    const uint32_t y0 = static_cast<uint32_t>(ctx.scissor.y0);
    const uint32_t w = (ctx.scissor.x1 >= ctx.scissor.x0)
                           ? (static_cast<uint32_t>(ctx.scissor.x1 - ctx.scissor.x0) + 1u)
                           : 0u;
    const uint32_t h = (ctx.scissor.y1 >= ctx.scissor.y0)
                           ? (static_cast<uint32_t>(ctx.scissor.y1 - ctx.scissor.y0) + 1u)
                           : 0u;
    return g144RangeForRect(baseBlock, ctx.frame.psm, ctx.frame.fbw, x0, y0, w, h);
}

static bool g144PendingNeedsFlushForRanges(const G144BlockRange &src, const G144BlockRange &dst)
{
    if (g_g144List.empty())
        return false;

    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange target = g144TargetRange(e);
        if (g144RangeOverlaps(dst, target) || g144RangeOverlaps(src, target))
            return true;

        if (g144RangeOverlaps(dst, g144TextureRange(e)) ||
            g144RangeOverlaps(dst, g144ClutRange(e)))
        {
            return true;
        }
    }

    return false;
}

// G254 (DIAGNOSTIC ONLY, DEFAULT OFF): census candidate cross-target dependencies. Both behavior
// prototypes were rejected: triangle widening caused a temporal model dropout, while isolated
// predicted-safe FBP crossing was performance-neutral and produced large black MAP-0 regions.
// The range model is informative but not a permission oracle; all FBP drains remain.
enum G254Dependency : uint32_t
{
    G254_DEP_NONE = 0u,
    G254_DEP_UNKNOWN_RANGE = 1u << 0,
    G254_DEP_TARGET_ALIAS = 1u << 1,
    G254_DEP_EARLIER_WRITE_READ = 1u << 2,
    G254_DEP_LATER_WRITE_READ = 1u << 3,
    G254_DEP_DEPTH_ALIAS = 1u << 4,
    G254_DEP_LEGACY_Z = 1u << 5,
};

struct G254Surface
{
    uint32_t base = 0u;
    uint32_t bw = 0u;
    uint8_t psm = 0u;
    G144BlockRange range{};
    bool active = false;
    bool writable = false;
};

static G254Surface g254ColorSurface(const G144Entry &e)
{
    G254Surface s;
    s.base = framePageBaseToBlock(e.ctx.frame.fbp);
    s.bw = e.ctx.frame.fbw;
    s.psm = e.ctx.frame.psm;
    s.range = g144TargetRange(e);
    s.active = true;
    s.writable = e.ctx.frame.fbmsk != 0xFFFFFFFFu;
    return s;
}

static G254Surface g254DepthSurface(const G144Entry &e)
{
    G254Surface s;
    const uint32_t zte = static_cast<uint32_t>((e.ctx.test >> 16u) & 1u);
    const uint32_t ztst = static_cast<uint32_t>((e.ctx.test >> 17u) & 3u);
    const bool zmsk = ((e.ctx.zbuf >> 32u) & 1u) != 0u;
    if (zte == 0u)
        return s;
    s.base = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu) << 5u;
    s.bw = e.ctx.frame.fbw;
    s.psm = static_cast<uint8_t>((e.ctx.zbuf >> 24u) & 0xFu);
    const uint32_t x0 = static_cast<uint32_t>(e.ctx.scissor.x0);
    const uint32_t y0 = static_cast<uint32_t>(e.ctx.scissor.y0);
    const uint32_t w = (e.ctx.scissor.x1 >= e.ctx.scissor.x0)
                           ? static_cast<uint32_t>(e.ctx.scissor.x1 - e.ctx.scissor.x0) + 1u
                           : 0u;
    const uint32_t h = (e.ctx.scissor.y1 >= e.ctx.scissor.y0)
                           ? static_cast<uint32_t>(e.ctx.scissor.y1 - e.ctx.scissor.y0) + 1u
                           : 0u;
    s.range = g144RangeForRect(s.base, s.psm, s.bw, x0, y0, w, h);
    s.active = true;
    s.writable = !zmsk && ztst != 0u;
    return s;
}

static bool g254SameSurface(const G254Surface &a, const G254Surface &b)
{
    return a.active && b.active && a.base == b.base && a.bw == b.bw && a.psm == b.psm;
}

static bool g254KnownSurface(const G254Surface &s)
{
    if (!s.active)
        return true;
    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    return s.range.valid && g144PsmPageDims(s.psm, pageW, pageH, halfWidthBw);
}

static bool g254KnownTextureReads(const G144Entry &e,
                                  G144BlockRange &texture,
                                  G144BlockRange &clut)
{
    if (!e.prim.tme)
    {
        texture = {};
        clut = {};
        return true;
    }
    uint32_t pageW = 0u, pageH = 0u;
    bool halfWidthBw = false;
    if (!g144PsmPageDims(e.ctx.tex0.psm, pageW, pageH, halfWidthBw))
        return false;
    texture = g144TextureRange(e);
    if (!texture.valid)
        return false;
    clut = g144ClutRange(e);
    if (g144IsPalettedPsm(e.ctx.tex0.psm) &&
        (!g144PsmPageDims(e.ctx.tex0.cpsm, pageW, pageH, halfWidthBw) || !clut.valid))
    {
        return false;
    }
    return true;
}

static uint32_t g254DependencyMask(const G144Entry &candidate)
{
    if (g_g144List.empty())
        return G254_DEP_NONE;
    if (!g203UniversalZEnabled())
        return G254_DEP_LEGACY_Z;

    const G254Surface nextColor = g254ColorSurface(candidate);
    const G254Surface nextDepth = g254DepthSurface(candidate);
    G144BlockRange nextTexture, nextClut;
    if (!g254KnownSurface(nextColor) || !g254KnownSurface(nextDepth) ||
        !g254KnownTextureReads(candidate, nextTexture, nextClut))
    {
        return G254_DEP_UNKNOWN_RANGE;
    }

    uint32_t mask = G254_DEP_NONE;
    for (const G144Entry &pending : g_g144List)
    {
        const G254Surface prevColor = g254ColorSurface(pending);
        const G254Surface prevDepth = g254DepthSurface(pending);
        G144BlockRange prevTexture, prevClut;
        if (!g254KnownSurface(prevColor) || !g254KnownSurface(prevDepth) ||
            !g254KnownTextureReads(pending, prevTexture, prevClut))
        {
            mask |= G254_DEP_UNKNOWN_RANGE;
            continue;
        }

        if ((prevColor.writable || nextColor.writable) &&
            g144RangeOverlaps(prevColor.range, nextColor.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevColor.writable || nextDepth.writable) &&
            g144RangeOverlaps(prevColor.range, nextDepth.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevDepth.writable || nextColor.writable) &&
            g144RangeOverlaps(prevDepth.range, nextColor.range))
        {
            mask |= G254_DEP_TARGET_ALIAS;
        }
        if ((prevDepth.writable || nextDepth.writable) &&
            g144RangeOverlaps(prevDepth.range, nextDepth.range) &&
            !g254SameSurface(prevDepth, nextDepth))
        {
            mask |= G254_DEP_DEPTH_ALIAS;
        }
        if ((prevColor.writable &&
             (g144RangeOverlaps(prevColor.range, nextTexture) ||
              g144RangeOverlaps(prevColor.range, nextClut))) ||
            (prevDepth.writable &&
             (g144RangeOverlaps(prevDepth.range, nextTexture) ||
              g144RangeOverlaps(prevDepth.range, nextClut))))
        {
            mask |= G254_DEP_EARLIER_WRITE_READ;
        }
        if ((nextColor.writable &&
             (g144RangeOverlaps(nextColor.range, prevTexture) ||
              g144RangeOverlaps(nextColor.range, prevClut))) ||
            (nextDepth.writable &&
             (g144RangeOverlaps(nextDepth.range, prevTexture) ||
              g144RangeOverlaps(nextDepth.range, prevClut))))
        {
            mask |= G254_DEP_LATER_WRITE_READ;
        }
    }
    return mask;
}

static bool g254DependencyStatEnabled()
{
    static const bool s_on = envFlagEnabled("DC2_G254_DEP_STAT");
    return s_on;
}

static void g254NoteSwitch(uint32_t mask, uint32_t fromFbp, uint32_t toFbp, size_t pendingEntries)
{
    if (!g254DependencyStatEnabled())
        return;
    static std::atomic<uint64_t> s_seq{0};
    static std::atomic<uint64_t> s_safe{0};
    static std::atomic<uint64_t> s_reason[6]{};
    static std::atomic<uint64_t> s_entries{0};
    static std::atomic<uint64_t> s_transition[8][8]{};

    if (mask == G254_DEP_NONE)
        s_safe.fetch_add(1u, std::memory_order_relaxed);
    for (uint32_t bit = 0; bit < 6u; ++bit)
        if ((mask & (1u << bit)) != 0u)
            s_reason[bit].fetch_add(1u, std::memory_order_relaxed);
    s_entries.fetch_add(static_cast<uint64_t>(pendingEntries), std::memory_order_relaxed);
    s_transition[g253BarrierFbpBucket(fromFbp)][g253BarrierFbpBucket(toFbp)]
        .fetch_add(1u, std::memory_order_relaxed);

    const uint64_t seq = s_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((seq % 512u) != 0u)
        return;
    static constexpr const char *kReason[6] = {
        "unknown", "targetAlias", "earlierWriteRead", "laterWriteRead", "depthAlias", "legacyZ"
    };
    const uint64_t safe = s_safe.exchange(0u, std::memory_order_relaxed);
    const uint64_t entries = s_entries.exchange(0u, std::memory_order_relaxed);
    std::fprintf(stderr, "[G254:dep] n=%llu safe=%llu blocked=%llu entries=%llu avgEntries=%.2f\n",
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned long long>(safe),
                 static_cast<unsigned long long>(512u - safe),
                 static_cast<unsigned long long>(entries),
                 static_cast<double>(entries) / 512.0);
    for (uint32_t bit = 0; bit < 6u; ++bit)
    {
        const uint64_t count = s_reason[bit].exchange(0u, std::memory_order_relaxed);
        if (count != 0u)
            std::fprintf(stderr, "[G254:reason] %s=%llu\n", kReason[bit],
                         static_cast<unsigned long long>(count));
    }
    for (int from = 0; from < 8; ++from)
        for (int to = 0; to < 8; ++to)
        {
            const uint64_t count = s_transition[from][to].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G254:transition] from=%d to=%d count=%llu\n", from, to,
                             static_cast<unsigned long long>(count));
        }
}

static void g144DirtyStat(const char *kind, bool flushed)
{
    static const bool s_stat = (std::getenv("DC2_G145_DIRTY_STAT") != nullptr);
    if (!s_stat)
        return;

    static std::atomic<uint64_t> s_flush{0};
    static std::atomic<uint64_t> s_skip{0};
    const uint64_t f = flushed ? (s_flush.fetch_add(1u, std::memory_order_relaxed) + 1u)
                               : s_flush.load(std::memory_order_relaxed);
    const uint64_t s = flushed ? s_skip.load(std::memory_order_relaxed)
                               : (s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u);
    if ((flushed && f <= 32u) || (!flushed && s <= 32u) || (((f + s) & 0x3FFull) == 0ull))
    {
        std::fprintf(stderr, "[G145:dirty] kind=%s action=%s flush=%llu skip=%llu\n",
                     kind, flushed ? "flush" : "skip",
                     static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(s));
    }
}

// G166: extracted so BOTH the direct (single-texture) path and the batch-fallback path (any GPU
// batch failure) can share the exact same, already-proven CPU decode expression -- never
// duplicated inline. Identical math to the original inline T8 case (ReadP8 -> lookupCLUT).
static void g149DecodeT8Cpu(GSRasterizer *self, GS *gs, uint32_t tbp0, uint32_t pageBwT8,
                            uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t texPsm,
                            uint8_t *vram, int texW, int texH, uint32_t *outPx)
{
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, tbp0, pageBwT8,
                                    static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
            outPx[static_cast<size_t>(vv) * static_cast<size_t>(texW) + static_cast<size_t>(uu)] =
                self->lookupCLUT(gs, idx, cbp, cpsm, csm, csa, texPsm);
        }
}

// G166 (default OFF, needs DC2_G162_GPU_DECODE): a T8 cache-miss with GPU decode enabled defers
// its actual pixel fill to ONE per-frame batch (see g162ResolvePendingT8Batch below) instead of a
// synchronous per-texture GPU round-trip -- G164/G165 isolated cross-thread draw+readback
// synchronization, not buffer management, as the dominant remaining GPU-decode cost, and batching
// amortizes that synchronization over the whole frame's requests instead of paying it ~8x/frame.
// SAFE BY CONSTRUCTION: the placeholder `tex` is inserted into g_g149Map immediately (so later
// triangles in the same frame needing the SAME texture get a normal cache hit, unchanged), but its
// `px` is only filled at RESOLVE time -- which is guaranteed to run before any replay reads it,
// because it is the first statement of both G144 flush closures (g_g144FlushFn/g_g144FlushSeqFn),
// and replay of a frame's G144Entry list can only happen via one of those closures.
struct G162PendingT8
{
    uint32_t tbp0 = 0, pageBwT8 = 0;
    int texW = 0, texH = 0;
    uint32_t clut256[256]{};
    uint32_t cbp = 0; uint8_t cpsm = 0, csm = 0, csa = 0, texPsm = 0;
    GSRasterizer *self = nullptr; GS *gs = nullptr; uint8_t *vram = nullptr; // CPU-fallback params
    G149Buf tex;
};
static std::vector<G162PendingT8> g_g162Pending;

// G242's guest-depth ownership helpers are defined below the G178 front-end.  Keep them as a
// private .cpp-to-.cpp contract: CPU readers must materialize overlapping GPU-owned depth before
// they inspect VRAM bytes, while the default-off path is a single cached flag check.
void g242PrepareVramReadAll(uint8_t *vram);
void g242PrepareVramReadRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Resolves ALL pending T8 batch requests in one shot: tries the GPU batch first; on ANY failure
// (decoder not ready, batch too large for the atlas, etc) falls back to the proven CPU decode for
// every pending entry individually -- correctness never depends on the batch succeeding. Called
// from the START of both G144 flush closures (before they read any e.decoded), so this must
// complete before this function returns.
static void g162ResolvePendingT8Batch()
{
    if (g_g162Pending.empty())
        return;
    // Pending T8 requests dereference their captured VRAM pointer below (GPU upload or CPU
    // fallback).  A prior G242 batch may own aliased Z pages, so resolve that ownership first.
    g242PrepareVramReadAll(g_g162Pending[0].vram);
    extern bool g162DecodeT8Batch(int, const uint32_t *, const uint32_t *, const int *, const int *,
                                  const uint32_t *, const uint8_t *, size_t, uint32_t **);

    const int count = static_cast<int>(g_g162Pending.size());
    std::vector<uint32_t> tbp0Arr(count), tbwArr(count), clutFlat(static_cast<size_t>(count) * 256u);
    std::vector<int> texWArr(count), texHArr(count);
    std::vector<uint32_t *> outPxArr(count);
    for (int i = 0; i < count; ++i)
    {
        const G162PendingT8 &p = g_g162Pending[static_cast<size_t>(i)];
        tbp0Arr[i] = p.tbp0; tbwArr[i] = p.pageBwT8;
        texWArr[i] = p.texW; texHArr[i] = p.texH;
        std::memcpy(&clutFlat[static_cast<size_t>(i) * 256u], p.clut256, sizeof(p.clut256));
        outPxArr[i] = p.tex->px.data();
    }
    const bool ok = g162DecodeT8Batch(count, tbp0Arr.data(), tbwArr.data(), texWArr.data(),
                                      texHArr.data(), clutFlat.data(),
                                      g_g162Pending[0].vram, static_cast<size_t>(GSMem::MEMORY_SIZE),
                                      outPxArr.data());
    if (ok)
    {
        g_g162GpuHits.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
    }
    else
    {
        g_g162GpuFallback.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
        for (const G162PendingT8 &p : g_g162Pending)
            g149DecodeT8Cpu(p.self, p.gs, p.tbp0, p.pageBwT8, p.cbp, p.cpsm, p.csm, p.csa, p.texPsm,
                            p.vram, p.texW, p.texH, p.tex->px.data());
    }
    g_g162Pending.clear();
}

// G149: fetch (or ALLOCATE, lazily) the decoded-texture buffer for the texture bound in `ctx`, at
// DEFER time on the single guest thread. Returns null if the tri is not texcache-eligible (mirrors
// g141FastSampleTri's PSM/tbw gate), uses a REGION wrap mode, or exceeds the cap. The buffer is NOT
// decoded here — only allocated + zero-`valid`; texels are decoded lazily on first sample (see
// g141SamplePoint), so only sampled texels ever cost a swizzle+CLUT read. Memoized in g_g149Map keyed
// on descriptor + VRAM generation; the map is cleared when the generation advances (any VRAM upload).
// Guest-thread-only (defer is pre-replay) ⇒ no mutex. `self`/`vram`/`texa` are unused now (decode is
// deferred to sample time) but kept for signature stability.
static G149Buf g149BuildDecoded(GSRasterizer *self, GS *gs, const GSContext &ctx,
                                const GSTexaReg &texa, uint8_t *vram)
{
    (void)self; (void)gs; (void)vram;
    static const bool s_on = (std::getenv("DC2_G149_TEXCACHE") != nullptr);
    if (!s_on)
        return nullptr;
    static const size_t s_cap = []() {
        const char *e = std::getenv("DC2_G149_CAP");
        const size_t c = e ? static_cast<size_t>(std::strtoull(e, nullptr, 0)) : 262144u;
        return c ? c : 262144u;
    }();
    static const bool s_t8GsmemDefault =
        !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
    static const bool s_t8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);

    const uint8_t psm = ctx.tex0.psm;
    const bool fastPsm =
        psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S ||
        psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH ||
        (psm == GS_PSM_T8 && s_t8GsmemDefault && !s_t8AliasSet);
    if (!fastPsm || ctx.tex0.tbw == 0u)
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }
    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    if (!((wrapU == 0u || wrapU == 1u) && (wrapV == 0u || wrapV == 1u)))
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr; // REGION_CLAMP/REPEAT can index outside [0,texW): let the normal sampler run
    }
    const int texW = 1 << ctx.tex0.tw;
    const int texH = 1 << ctx.tex0.th;
    const size_t texels = static_cast<size_t>(texW) * static_cast<size_t>(texH);
    if (texels == 0 || texels > s_cap)
    {
        g_g149Ineligible.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }

    // G242: texture storage and CLUT storage can alias real guest Z (notably the 0x1a00 work
    // pages).  Materialize only on an actual overlap before even consulting the decoded cache;
    // the sync bumps g_g149VramGen, so an alias can never return a stale cached decode.
    g242PrepareVramReadRect(vram, ctx.tex0.tbp0, ctx.tex0.tbw, psm, 0u, 0u,
                            static_cast<uint32_t>(texW), static_cast<uint32_t>(texH));
    if (psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
        g242PrepareVramReadRect(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                0u, 0u, 64u, 32u);

    G149Key key;
    key.tbp0 = ctx.tex0.tbp0; key.tbw = ctx.tex0.tbw; key.cbp = ctx.tex0.cbp;
    key.tw = static_cast<uint32_t>(texW); key.th = static_cast<uint32_t>(texH);
    key.psm = psm; key.cpsm = ctx.tex0.cpsm; key.csm = ctx.tex0.csm; key.csa = ctx.tex0.csa;
    key.texa = (static_cast<uint32_t>(texa.aem ? 1u : 0u) << 16) |
               (static_cast<uint32_t>(texa.ta1) << 8) | static_cast<uint32_t>(texa.ta0);
    key.gen = g_g149VramGen.load(std::memory_order_relaxed);

    if (g_g149MapGen != key.gen) { g_g149Map.clear(); g_g149MapGen = key.gen; }
    auto it = g_g149Map.find(key);
    if (it != g_g149Map.end())
    {
        g_g149Hits.fetch_add(1u, std::memory_order_relaxed);
        return it->second;
    }

    // EAGER decode: fill the whole texture ONCE now (guest thread), so band-replay reads are pure
    // read-only linear lookups (no cross-thread writes → stable on the parallel path). Bit-exact:
    // filled with the SAME leaf expression g141SamplePoint uses (readTexel*/ReadP8 → lookupCLUT →
    // applyTexa) over uu,vv in [0,texW)x[0,texH). (A lazy per-texel fill would decode only sampled
    // texels but requires cross-band writes to the shared buffer, which proved unstable — see fix-log.)
    auto tex = std::make_shared<G149Tex>();
    tex->px.resize(texels);
    tex->valid.assign(texels, 1u); // eager: every texel already decoded
    const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;

    // G166 (default OFF, DC2_G162_GPU_DECODE=1, also needs DC2_G158_GPURASTER=1): defer this T8
    // texture's decode to the per-frame batch resolved at G144's flush point (see
    // g162ResolvePendingT8Batch above) instead of a synchronous per-texture GPU round-trip.
    // `pageBwT8` (not the raw ctx.tex0.tbw) is passed through unchanged -- same value ReadP8 uses
    // in the CPU fallback, so both paths address VRAM identically. `tex` is cached NOW (px filled
    // in later, before any replay can read it) so later triangles in this frame needing the same
    // texture get a normal cache hit. Bit-exactness verified by the EXISTING `DC2_G149_TCVERIFY`
    // per-pixel A/B (zero new verify code): if this path is wrong, TCVERIFY's compare shows it.
    if (psm == GS_PSM_T8)
    {
        static const bool s_gpuDecode = (std::getenv("DC2_G162_GPU_DECODE") != nullptr);
        if (s_gpuDecode)
        {
            G162PendingT8 pend;
            pend.tbp0 = ctx.tex0.tbp0; pend.pageBwT8 = pageBwT8;
            pend.texW = texW; pend.texH = texH;
            for (int i = 0; i < 256; ++i)
                pend.clut256[i] = self->lookupCLUT(gs, static_cast<uint8_t>(i), ctx.tex0.cbp, ctx.tex0.cpsm,
                                                   ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            pend.cbp = ctx.tex0.cbp; pend.cpsm = ctx.tex0.cpsm; pend.csm = ctx.tex0.csm;
            pend.csa = ctx.tex0.csa; pend.texPsm = ctx.tex0.psm;
            pend.self = self; pend.gs = gs; pend.vram = vram;
            pend.tex = tex;
            g_g162Pending.push_back(std::move(pend));
            g_g149Builds.fetch_add(1u, std::memory_order_relaxed);
            g_g149Map.emplace(key, tex);
            return tex; // px filled by g162ResolvePendingT8Batch() before replay reads it
        }
    }

    uint32_t *d = tex->px.data();
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            uint32_t c;
            switch (psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_CT24:
                c = applyTexa(texa, psm, self->readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
                c = applyTexa(texa, psm, self->readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_T8:
            {
                const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, ctx.tex0.tbp0, pageBwT8,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
                c = self->lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                break;
            }
            default: // T4 / T4HL / T4HH
            {
                const uint32_t idx = self->readTexelPSMT4(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv);
                c = self->lookupCLUT(gs, static_cast<uint8_t>(idx), ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                break;
            }
            }
            d[static_cast<size_t>(vv) * static_cast<size_t>(texW) + static_cast<size_t>(uu)] = c;
        }
    g_g149Builds.fetch_add(1u, std::memory_order_relaxed);
    g_g149Map.emplace(key, tex);
    return tex;
}

// ============================== G178: LLE GPU rasterizer FRONT-END ==============================
// (default OFF, DC2_G178_GPU=1; backend = ps2_gs_gpu_raster.cpp via ps2_gs_gpu_lle.h)
//
// Owns everything that needs this TU's proven GS semantics: per-entry eligibility validation,
// fixed-point→float vertex translation (exact drawTriangle/drawSprite conventions), eager
// whole-texture decode (the SAME leaf expressions as g149BuildDecoded), the per-page VRAM write
// generations that keep the backend's GPU-resident texture cache honest, and the swizzled VRAM
// read/write for framebuffer upload + readback (the SAME addrPSMCT32 calls writePixel uses).
// A flush either renders ENTIRELY on the GPU (then reads back into VRAM, so presentation/dumps/
// any downstream VRAM reader is unaffected) or falls back ENTIRELY to the proven CPU band replay
// — never mixed within one flush.
namespace
{
bool g178GpuOn()
{
    // G178 promoted to DEFAULT-ON (2026-07-10) — kill via DC2_G178_NO_GPU=1 or DC2_G178_GPU=0.
    static const bool on = !envFlagEnabled("DC2_G178_NO_GPU") && !envFlagDisabled("DC2_G178_GPU");
    return on;
}

// G249: G246's default-on promotion was retired after a multi-frame title test exposed repeated
// dirty-generation invariant failures: CPU VRAM writes can advance the guest Z-page generation
// while G242 still owns newer GPU depth. Keep the bridge available for explicit barrier work,
// but never enter an ownership model whose own invariant is already false on the title route.
// DC2_G242_GPU_DEPTH=1 opts in; DC2_G242_NO_GPU_DEPTH=1 remains the explicit kill switch.
bool g242GpuDepthOn()
{
    static const bool on = envFlagEnabled("DC2_G242_GPU_DEPTH") &&
                           !envFlagEnabled("DC2_G242_NO_GPU_DEPTH");
    return on;
}

// ===== G260 NATIVE-RENDERER SLICE 1: frame-scope command graph (OPT-IN, DC2_G260_NR=1) =====
// First executable foundation of the native host renderer (plans/arc-native-renderer.md).
// Today every FBP switch and every non-deferrable primitive DRAINS the single-target deferred
// list (22 mid drains/frame on MAP-0), and RTT-family triangles are structurally excluded from
// deferral entirely (the G247 gap), rasterizing inline scalar on the GS thread. This layer
// replaces "drain at every boundary" with "record across boundaries, execute at real dependency
// edges":
//   - RECORD: draws append to the open per-target batch exactly as G144 does (same G144Entry,
//     same capture-time semantics). A target switch CLOSES the open batch into the frame graph
//     (an O(1) vector move) instead of draining it. RTT-family triangles (all five G248 targets)
//     become recordable, joining the same batches as the G252 RTT sprites.
//   - DEPENDENCY EDGES: each batch carries conservative block-interval read/write sets (color+Z
//     writes, texture+CLUT reads; unknown → overlaps-everything). A VRAM mutation (host upload,
//     local-to-local copy) or VRAM read (local-to-host) executes the pending graph only when its
//     range actually intersects a pending set — the WAR/RAW edge — or is unresolvable. Everything
//     else records through without synchronization.
//   - EXECUTE: batches run strictly in SUBMISSION ORDER through the existing proven flush
//     closures (per-batch whole-flush GPU attempt via g178TryFlushGpu, else the 8-lane band CPU
//     replay). No reordering and no cross-batch merging is ever attempted (that is the refuted
//     G254 shortcut); the graph only DELAYS execution, so the VRAM evolution order observed by
//     every reader is identical to the immediate-drain baseline.
// Backend neutrality: the recorded command unit (state snapshot + vertices + read/write sets +
// executor choice per batch) carries no GL types; the OpenGL 4.6 backend is one executor behind
// g178TryFlushGpu, the banded CPU replay is the other (oracle/fallback). Logical-vs-physical
// resolution separation (pillar 4) keys off the batch target dims (fbW/512 logical); the host
// scale factor multiplies only inside the backend submit — scale=1 in this slice.
// Interactions: requires G144 tile-binning (rides its machinery); declines to engage when the
// G242 GPU-depth or G255/G256 GPU-RTT experiments are active (separate architecture arms).
// Kill: unset DC2_G260_NR (default off). Stats: DC2_G260_STAT=1.
} // namespace — the fwd decl below must be GLOBAL scope to match the later global definition
// fwd (defined later in this file): replayed-batch write notes need it before its definition.
void g178NoteVramWriteRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
namespace
{

bool g260NrOn()
{
    static const bool s_on = []() {
        // G261 waves are the GPU executor built ON this substrate — the wave lever implies NR.
        if (!envFlagEnabled("DC2_G260_NR") && !g261WaveOn())
            return false;
        if (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
            return false; // NR records through the G144 capture/replay machinery
        // Keep one architectural variable per arm: the standalone G242/G255/G256 experiments
        // carry their own ownership models and never combine with NR. G261 is the exception by
        // design (it IS the ownership model for GPU-resident RTT on this substrate).
        if (g242GpuDepthOn() || (g255GpuRttOn() && !g261WaveOn()))
            return false;
        return true;
    }();
    return s_on;
}

// G270 (DEFAULT OFF, DC2_G270_LINE_WAVE=1): record the display LINE/LINESTRIP
// border work in the native-renderer graph instead of treating every segment as an inline
// primitive. G268's inclusive dispatch timer attributed ~61.5 ms/frame to A+D XYZ2 draws, while
// G171 showed that the first line in each group was paying the pending graph drain. This lever
// removes that artificial barrier. The GPU executor expands the existing integer Bresenham walk
// into ordered 1x1 quads, preserving the CPU rasterizer's exact pixel coordinates and colors.
// Keep this opt-in until CPU fallback, GPU composition, and downstream presentation all pass.
bool g270LineWaveOn()
{
    static const bool s_on = g260NrOn() && envFlagEnabled("DC2_G270_LINE_WAVE") &&
                             !envFlagEnabled("DC2_G270_NO_LINE_WAVE");
    return s_on;
}

bool g270LineType(uint32_t type)
{
    return type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP;
}

static const bool g_g270Stat = envFlagEnabled("DC2_G270_STAT");
static std::atomic<uint64_t> g_g270Captured{0}, g_g270Pixels{0};

void g270NoteCapture(const GSVertex &v0, const GSVertex &v1)
{
    if (!g_g270Stat)
        return;
    const uint64_t n = g_g270Captured.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const int dx = std::abs(static_cast<int>(v1.x) - static_cast<int>(v0.x));
    const int dy = std::abs(static_cast<int>(v1.y) - static_cast<int>(v0.y));
    g_g270Pixels.fetch_add(static_cast<uint64_t>(std::max(dx, dy)) + 1u,
                           std::memory_order_relaxed);
    if ((n & 0xFFu) == 0u)
        std::fprintf(stderr, "[G270:line] captured=%llu pixels=%llu\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_g270Pixels.load(std::memory_order_relaxed)));
}

// G266: FBW is part of the target identity. MAP-0's fbp=0x139 batch carries 1,059 fbw=2
// character-RTT draws plus ONE trailing fbw=8 display-grab sprite; the width mix is a
// guaranteed whole-flush GPU reject (mixed-target check), sending the whole wave to the scalar
// CPU replay every frame. Split the batch at a width switch exactly like an fbp switch. Engaged
// under NR (graph close, O(1)) and under the G255 verify oracle (legacy drain — so the exact
// CPU-shadow harness can exercise the newly GPU-routed batches). Default path unchanged.
// Kill: DC2_G266_NO_FBW_SPLIT=1.
bool g266FbwSplitOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G266_NO_FBW_SPLIT"))
            return false;
        return g260NrOn() || (g255GpuRttOn() && g255VerifyOn());
    }();
    return s_on;
}

// G286 experiment: admit an ordinary CT32 work target into the frame-scope command graph without
// granting it persistent FBO ownership. The selected 0x13b target is a recurring full-screen
// MAP-0 producer that currently rasterizes inline on the GS thread (~62 ms for its terminal
// sprite). It physically aliases the G261 RTT family, so the GPU attempt must first publish every
// overlapping resident target exactly as the proven CPU replay does. The result is read back into
// VRAM in the same flush and its exact color window advances page generations; downstream
// texture/transfer/presentation consumers therefore keep today's VRAM-authoritative contract.
// G287 PROMOTION (2026-07-18): default-on after fixing the backend's missing texture bind/combine
// state and first-use incomplete sampler, then passing a 210-batch exact oracle plus normal-output
// MAP-0 composition. Kill via DC2_G287_NO_TRANSIENT_TARGET=1 (the G286 kill remains accepted);
// DC2_G286_TRANSIENT_TARGET=1 is retained as a redundant compatibility opt-in.
bool g286TransientTargetOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G287_NO_TRANSIENT_TARGET") &&
        !envFlagDisabled("DC2_G287_TRANSIENT_TARGET") &&
        !envFlagEnabled("DC2_G286_NO_TRANSIENT_TARGET") &&
        !envFlagDisabled("DC2_G286_TRANSIENT_TARGET");
    return s_on && g260NrOn();
}

bool g286TransientTarget(uint32_t fbp)
{
    return g286TransientTargetOn() && fbp == 0x13bu;
}

static bool g286StatOn()
{
    static const bool s_on =
        envFlagEnabled("DC2_G287_STAT") || envFlagEnabled("DC2_G286_STAT");
    return s_on;
}

// G289 bring-up: pin the exact consumer-side alias shape before granting any GPU-local-copy
// authority. Diagnostic only; the transfer-side census already proved display CT32 -> dbp=0x2760.
static bool g289CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_CENSUS");
    return s_on;
}

static std::atomic<uint64_t> g_g286Captured{0}, g_g286GpuAttempts{0},
    g_g286GpuSuccess{0}, g_g286RowsPublished{0};

// G272 display-color residency helpers are defined after the G178 framebuffer snapshot state.
// The nested G260 scheduler namespace uses these .cpp-local bridges without a public header.
bool g272DisplayColorWaveOn();
bool g272DirtyInfo(int index, uint32_t &fbp, uint32_t &fbw, G144BlockRange &range);
void g272MaterializeIndex(int index, int cause);
void g272MaterializeAll(int cause);
void g272MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown, int cause);
void g272SetDisplayBatch(uint32_t fbp, uint32_t fbw);

namespace
{
    // Conservative set of disjoint block intervals. Fixed capacity; overflow merges into the
    // nearest interval (over-coverage is safe: it can only force an extra execute, never skip
    // a required one). `unknown` poisons the set → overlaps everything.
    struct G260RangeSet
    {
        G144BlockRange r[8];
        int n = 0;
        bool unknown = false;

        void reset()
        {
            n = 0;
            unknown = false;
        }
        void add(const G144BlockRange &b)
        {
            if (unknown)
                return;
            if (!b.valid)
                return; // empty rect (zero area) — nothing read/written
            // merge into an overlapping/adjacent interval if any
            for (int i = 0; i < n; ++i)
            {
                if (b.first <= r[i].last + 1u && r[i].first <= b.last + 1u)
                {
                    r[i].first = std::min(r[i].first, b.first);
                    r[i].last = std::max(r[i].last, b.last);
                    return;
                }
            }
            if (n < 8)
            {
                r[n++] = b;
                return;
            }
            // capacity: merge into the interval with the smallest resulting span growth
            int best = 0;
            uint64_t bestGrow = ~0ull;
            for (int i = 0; i < n; ++i)
            {
                const uint32_t lo = std::min(r[i].first, b.first);
                const uint32_t hi = std::max(r[i].last, b.last);
                const uint64_t grow = static_cast<uint64_t>(hi - lo) -
                                      (r[i].last - r[i].first);
                if (grow < bestGrow)
                {
                    bestGrow = grow;
                    best = i;
                }
            }
            r[best].first = std::min(r[best].first, b.first);
            r[best].last = std::max(r[best].last, b.last);
        }
        void markUnknown() { unknown = true; }
        bool overlaps(const G144BlockRange &b) const
        {
            if (unknown)
                return true;
            if (!b.valid)
                return false;
            for (int i = 0; i < n; ++i)
                if (g144RangeOverlaps(r[i], b))
                    return true;
            return false;
        }
    };

    struct G260Batch
    {
        uint32_t fbp = 0xFFFFFFFFu;
        std::vector<G144Entry> entries;
        G260RangeSet wr; // color target + active Z (read-modify-write surfaces)
        G260RangeSet rd; // texture + CLUT source pages
    };

    // Before executing a recorded batch, resolve any dirty display FBO that the batch will sample
    // as ordinary VRAM, alias through another target, or reinterpret with a different FBW. A same-
    // layout write to the same display target may continue directly in its persistent FBO.
    void g272PrepareBatch(const G260Batch &b)
    {
        if (!g272DisplayColorWaveOn() || b.entries.empty())
            return;
        const uint32_t nextFbw = b.entries.front().ctx.frame.fbw;
        for (int di = 0; di < 2; ++di)
        {
            uint32_t dirtyFbp = 0u, dirtyFbw = 0u;
            G144BlockRange dirtyRange{};
            if (!g272DirtyInfo(di, dirtyFbp, dirtyFbw, dirtyRange))
                continue;
            bool depthAlias = false;
            for (const G144Entry &e : b.entries)
            {
                const G254Surface depth = g254DepthSurface(e);
                if (depth.active && depth.range.valid &&
                    g144RangeOverlaps(depth.range, dirtyRange))
                {
                    depthAlias = true;
                    break;
                }
            }
            const bool readsDirty = b.rd.overlaps(dirtyRange);
            const bool otherWrite = b.wr.overlaps(dirtyRange) &&
                                    (b.fbp != dirtyFbp || nextFbw != dirtyFbw || depthAlias);
            if (readsDirty || otherWrite)
                g272MaterializeIndex(di, 1); // recorded GPU consumer/alias edge
        }
    }

    std::vector<G260Batch> g_g260Graph;                 // closed batches, submission order
    std::vector<std::vector<G144Entry>> g_g260Pool;     // recycled entry storage
    G260RangeSet g_g260OpenWr;                          // aggregates of the OPEN batch (g_g144List)
    G260RangeSet g_g260OpenRd;
    // stats (quiet unless DC2_G260_STAT=1)
    uint64_t g_g260ExecByCause[8] = {};
    uint64_t g_g260BatchesExecuted = 0;
    uint64_t g_g260EntriesExecuted = 0;
    uint64_t g_g260Closes = 0;
    uint64_t g_g260UploadSkips = 0;
    size_t g_g260MaxGraph = 0;

    // G290 (probe, quiet unless DC2_G290_PROBE=1): drain-execution decomposition. The G273
    // profile times only SUCCESSFUL GPU flushes; this splits the whole per-batch execution —
    // batch preparation, T8 resolve, GPU attempt (success vs reject), pre-replay publication,
    // CPU band replay, and post-replay generation notes — so the unattributed remainder of the
    // g144 upload drain can be located. Accumulated single-threaded on the drain thread.
    bool g290ProbeOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G290_PROBE");
        return s_on;
    }
    uint64_t g_g290PrepNs = 0, g_g290ResolveNs = 0, g_g290GpuOkNs = 0, g_g290GpuFailNs = 0,
             g_g290PubNs = 0, g_g290ReplayNs = 0, g_g290NoteNs = 0;
    uint64_t g_g290GpuOk = 0, g_g290GpuFail = 0, g_g290CpuEntries = 0, g_g290Drains = 0;

    // G271 (DEFAULT OFF, DC2_G271_PREFIX_BARRIER=1): an upload that conflicts with one recorded
    // batch only needs submission-order execution through the LAST conflicting batch. Independent
    // suffix batches may remain recorded across the upload. No reordering is introduced: this is
    // the prefix form of the same G260 range dependency proof, not a new ownership model.
    bool g271PrefixBarrierOn()
    {
        static const bool s_on = g260NrOn() && envFlagEnabled("DC2_G271_PREFIX_BARRIER") &&
                                 !envFlagEnabled("DC2_G271_NO_PREFIX_BARRIER");
        return s_on;
    }
    uint64_t g_g271Execs = 0, g_g271Batches = 0, g_g271SuffixBatches = 0;

    enum G260Cause : int
    {
        kG260Boundary = 0,
        kG260Upload = 1,
        kG260LocalCopy = 2,
        kG260LocalHost = 3,
        kG260NonDef = 4,
        kG260Runaway = 5,
        kG260Legacy = 6,
    };

    bool g260StatOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G260_STAT");
        return s_on;
    }

    // Rect range with fail-closed semantics for the dependency sets: a non-empty rect whose
    // range could not be resolved (block overflow past 0x3FFF etc.) must poison as UNKNOWN,
    // not silently become empty like the raw helper's {} return.
    void g260AddRectOrUnknown(G260RangeSet &set, uint32_t baseBlock, uint8_t psm, uint32_t bw,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (w == 0u || h == 0u)
            return;
        const G144BlockRange rg = g144RangeForRect(baseBlock, psm, bw, x, y, w, h);
        if (!rg.valid)
            set.markUnknown();
        else
            set.add(rg);
    }

    // Accumulate one just-recorded entry's read/write ranges into the open-batch sets.
    void g260NoteAppend(const G144Entry &e)
    {
        // color target (written and, under blending/fbmsk, read back) — scissor-bounded
        const G144BlockRange color = g144TargetRange(e);
        if (!color.valid &&
            e.ctx.scissor.x1 >= e.ctx.scissor.x0 && e.ctx.scissor.y1 >= e.ctx.scissor.y0)
            g_g260OpenWr.markUnknown();
        else
            g_g260OpenWr.add(color);
        // active Z surface (read for ZTST, written unless ZMSK) — treat as write-class
        const G254Surface depth = g254DepthSurface(e);
        if (depth.active)
        {
            if (!depth.range.valid)
                g_g260OpenWr.markUnknown();
            else
                g_g260OpenWr.add(depth.range);
        }
        // texture + CLUT reads
        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
                g_g260OpenRd.markUnknown();
            else
                g_g260OpenRd.add(tex);
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                    g_g260OpenRd.markUnknown();
                else
                    g_g260OpenRd.add(clut);
            }
        }
    }

    bool g260HasPending()
    {
        return !g_g260Graph.empty() || !g_g144List.empty();
    }

    // Close the open batch (g_g144List) into the graph. O(1): vector move, no rasterization.
    void g260CloseOpenBatch()
    {
        if (g_g144List.empty())
            return;
        G260Batch b;
        if (!g_g260Pool.empty())
        {
            b.entries = std::move(g_g260Pool.back());
            g_g260Pool.pop_back();
            b.entries.clear();
        }
        b.entries.swap(g_g144List);
        b.fbp = g_g144BlockFbp;
        b.wr = g_g260OpenWr;
        b.rd = g_g260OpenRd;
        g_g260OpenWr.reset();
        g_g260OpenRd.reset();
        g_g260Graph.push_back(std::move(b));
        ++g_g260Closes;
        if (g_g260Graph.size() > g_g260MaxGraph)
            g_g260MaxGraph = g_g260Graph.size();
    }

    void g260StatReport()
    {
        if (!g260StatOn())
            return;
        static uint64_t s_tick = 0;
        if ((++s_tick % 240ull) != 0ull)
            return;
        std::fprintf(stderr,
                     "[G260:stat] exec(boundary=%llu upload=%llu l2l=%llu l2h=%llu nondef=%llu "
                     "runaway=%llu legacy=%llu) closes=%llu batches=%llu entries=%llu maxGraph=%zu "
                     "uploadSkips=%llu\n",
                     (unsigned long long)g_g260ExecByCause[kG260Boundary],
                     (unsigned long long)g_g260ExecByCause[kG260Upload],
                     (unsigned long long)g_g260ExecByCause[kG260LocalCopy],
                     (unsigned long long)g_g260ExecByCause[kG260LocalHost],
                     (unsigned long long)g_g260ExecByCause[kG260NonDef],
                     (unsigned long long)g_g260ExecByCause[kG260Runaway],
                     (unsigned long long)g_g260ExecByCause[kG260Legacy],
                     (unsigned long long)g_g260Closes,
                     (unsigned long long)g_g260BatchesExecuted,
                     (unsigned long long)g_g260EntriesExecuted,
                     g_g260MaxGraph,
                     (unsigned long long)g_g260UploadSkips);
    }

    // Execute a submission-order prefix of the CLOSED graph. Each batch runs through the SAME
    // proven flush closure the immediate drain uses (whole-flush GPU attempt, else 8-lane band
    // CPU replay). Remaining suffix batches retain their original order and captured state.
    void g260ExecutePrefix(int cause, bool allowParallel, size_t prefixCount)
    {
        if (t_g144InReplay)
            return; // replay lanes never re-enter the scheduler
        prefixCount = std::min(prefixCount, g_g260Graph.size());
        if (prefixCount == 0u)
            return;
        ++g_g260ExecByCause[cause & 7];
        static const bool s_seqEnv = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &fn =
            (allowParallel && !s_seqEnv && g_g144FlushFn) ? g_g144FlushFn : g_g144FlushSeqFn;
        for (size_t bi = 0; bi < prefixCount; ++bi)
        {
            G260Batch &b = g_g260Graph[bi];
            if (b.entries.empty())
                continue;
            g272PrepareBatch(b);
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            ++g_g260BatchesExecuted;
            g_g260EntriesExecuted += g_g144List.size();
            // Ownership rule (the G255 lesson, applied to CPU replay): a replayed batch's color/Z
            // writes must advance the SAME page generations an inline draw would (the inline
            // drawPrimitive-end hook is skipped during replay via t_g144InReplay). Without this,
            // deferred RTT-triangle output freezes its pages' generations and a GPU-classified
            // display batch legally reuses a stale cached texture of those pages — the persistent
            // missing-body-part failure found during G260 bring-up. Bump BEFORE execution order
            // consumers evaluate gens (they only read at their own later execution).
            // G261 waves: this pre-execution bump would force a stale-VRAM uploadFb into a
            // GPU-resident target (VRAM has NOT changed when the batch executes on the GPU).
            // Under waves the equivalent bump happens only on the CPU-replay path, inside the
            // flush closures (g261NoteCpuRttWrites, after materialize-before-CPU-write ran).
            if (g178GpuOn() && !g261WaveOn())
            {
                uint8_t *vram260 = g_g144LastVram; // member-safe alias (set beside g_g144LastGs)
                for (size_t ei = 0; vram260 && ei < g_g144List.size(); ++ei)
                {
                    const G144Entry &e = g_g144List[ei];
                    const GSContext &c = e.ctx;
                    // Scope: RTT-family targets only. In the baseline, RTT triangles rasterized
                    // INLINE and bumped these pages per draw (that inline bump is what kept the
                    // GPU texture cache honest for the composite); deferred DISPLAY draws never
                    // bumped, and replicating a display bump here only churns the fb-snapshot
                    // gens into constant re-uploads (measured as a full perf giveback).
                    if (g248TargetIndex(c.frame.fbp) < 0)
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRect(vram260,
                                              framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRect(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                    }
                }
            }
            if (fn)
                fn(); // drains + clears g_g144List
            else
                g_g144List.clear(); // cannot happen (entries imply assigned closures); fail safe
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g144List.swap(b.entries); // reclaim the (now empty) storage's capacity
            g_g260Pool.push_back(std::move(b.entries));
        }
        g_g260Graph.erase(g_g260Graph.begin(), g_g260Graph.begin() + prefixCount);
        g_g144BlockFbp = 0xFFFFFFFFu;
        g260StatReport();
    }

    // Execute the whole pending graph (closed batches then the open batch) strictly in
    // submission order. allowParallel selects the band-pool closure; callers pass false only
    // from contexts where the pool is not proven (non-pipelined frame boundary on the EE thread).
    void g260ExecuteAll(int cause, bool allowParallel)
    {
        if (t_g144InReplay)
            return;
        g260CloseOpenBatch();
        if (g_g260Graph.empty())
            return;
        ++g_g260ExecByCause[cause & 7];
        static const bool s_seqEnv = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &fn =
            (allowParallel && !s_seqEnv && g_g144FlushFn) ? g_g144FlushFn : g_g144FlushSeqFn;
        // Keep the pre-G271 whole-graph loop on every established/default path. Prefix execution
        // uses its separate opt-in helper only; this avoids changing vector lifetime/capacity or
        // open-batch state for the promoted G260-G266 stack.
        const bool g290Probe = g290ProbeOn();
        for (auto &b : g_g260Graph)
        {
            if (b.entries.empty())
                continue;
            const auto g290Tp0 = g290Probe ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            g272PrepareBatch(b);
            if (g290Probe)
                g_g290PrepNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g290Tp0)
                        .count());
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            ++g_g260BatchesExecuted;
            g_g260EntriesExecuted += g_g144List.size();
            if (g178GpuOn() && !g261WaveOn())
            {
                uint8_t *vram260 = g_g144LastVram;
                for (size_t ei = 0; vram260 && ei < g_g144List.size(); ++ei)
                {
                    const G144Entry &e = g_g144List[ei];
                    const GSContext &c = e.ctx;
                    if (g248TargetIndex(c.frame.fbp) < 0)
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRect(vram260, framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRect(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                    }
                }
            }
            if (fn)
                fn();
            else
                g_g144List.clear();
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g144List.swap(b.entries);
            g_g260Pool.push_back(std::move(b.entries));
        }
        g_g260Graph.clear();
        g_g144BlockFbp = 0xFFFFFFFFu;
        if (g290Probe && (++g_g290Drains % 100u) == 0u)
            std::fprintf(stderr,
                         "[G290:probe] drains=%llu prep=%.1f resolve=%.1f gpuOk=%.1f/%llu "
                         "gpuFail=%.1f/%llu pub=%.1f replay=%.1f/%llue note=%.1f (cumMs)\n",
                         (unsigned long long)g_g290Drains, g_g290PrepNs / 1e6,
                         g_g290ResolveNs / 1e6, g_g290GpuOkNs / 1e6,
                         (unsigned long long)g_g290GpuOk, g_g290GpuFailNs / 1e6,
                         (unsigned long long)g_g290GpuFail, g_g290PubNs / 1e6,
                         g_g290ReplayNs / 1e6, (unsigned long long)g_g290CpuEntries,
                         g_g290NoteNs / 1e6);
        g260StatReport();
    }

    bool g260RangeSetsConflict(const G260RangeSet &wr, const G260RangeSet &rd,
                               const G144BlockRange &src, const G144BlockRange &dst,
                               bool srcUnknown, bool dstUnknown)
    {
        if (srcUnknown || dstUnknown)
            return true;
        if (src.valid && wr.overlaps(src))
            return true;
        return dst.valid && (wr.overlaps(dst) || rd.overlaps(dst));
    }

    // G271: close the open batch, find the last conflicting batch, and execute exactly that
    // prefix. Every earlier batch executes too, preserving submission order; every retained
    // suffix is proven disjoint from the VRAM event's source/destination ranges.
    void g271ExecuteConflictPrefix(const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown, int cause,
                                   bool allowParallel)
    {
        g260CloseOpenBatch();
        if (g_g260Graph.empty())
            return;
        static const bool s_noSkip = envFlagEnabled("DC2_G260_NO_SKIP");
        size_t prefixCount = 0u;
        if (s_noSkip || srcUnknown || dstUnknown)
        {
            prefixCount = g_g260Graph.size();
        }
        else
        {
            for (size_t i = 0; i < g_g260Graph.size(); ++i)
                if (g260RangeSetsConflict(g_g260Graph[i].wr, g_g260Graph[i].rd,
                                          src, dst, false, false))
                    prefixCount = i + 1u;
        }
        if (prefixCount == 0u)
            return; // defensive: the caller's pre-check should have found no edge
        const size_t before = g_g260Graph.size();
        ++g_g271Execs;
        g_g271Batches += prefixCount;
        g_g271SuffixBatches += before - prefixCount;
        g260ExecutePrefix(cause, allowParallel, prefixCount);
        if (g260StatOn() && (g_g271Execs % 120u) == 0u)
            std::fprintf(stderr,
                         "[G271:prefix] exec=%llu batches=%llu retainedSuffix=%llu pending=%zu\n",
                         static_cast<unsigned long long>(g_g271Execs),
                         static_cast<unsigned long long>(g_g271Batches),
                         static_cast<unsigned long long>(g_g271SuffixBatches),
                         g_g260Graph.size());
    }

    // Would a VRAM mutation of [src→]dst intersect any pending recorded work? src matters only
    // for reads of pending WRITE surfaces (a copy must see rasterized pixels); dst conflicts
    // with both pending writes (target alias / WAW+WAR) and pending reads (texture/CLUT WAR).
    bool g260VramEventNeedsExecute(const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown)
    {
        if (!g260HasPending())
            return false;
        // DC2_G260_NO_SKIP=1 (bisect lever): treat every VRAM transfer as a dependency edge —
        // discriminates "range-test false negative" from any other delayed-execution defect.
        static const bool s_noSkip = envFlagEnabled("DC2_G260_NO_SKIP");
        if (s_noSkip)
            return true;
        if (srcUnknown || dstUnknown)
            return true;
        for (const auto &b : g_g260Graph)
            if (g260RangeSetsConflict(b.wr, b.rd, src, dst, false, false))
                return true;
        if (!g_g144List.empty() &&
            g260RangeSetsConflict(g_g260OpenWr, g_g260OpenRd, src, dst, false, false))
            return true;
        return false;
    }
} // namespace

constexpr uint32_t kG178PageCount = 512; // 4MB VRAM / 8KB GS page
std::atomic<uint64_t> g_g178PageGen[kG178PageCount]; // zero-initialized

// page geometry (pixels per GS page) by PSM — used only to compute conservative page RANGES.
void g178PageDims(uint32_t psm, uint32_t &pw, uint32_t &ph)
{
    switch (psm)
    {
    case GS_PSM_T8: pw = 128; ph = 64; break;
    case GS_PSM_T4: pw = 128; ph = 128; break;
    case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
        pw = 64; ph = 64; break;
    default: pw = 64; ph = 32; break; // CT32/CT24/Z32/Z24
    }
}

// Sum of write-generations over a conservative page range for a rect at BLOCK base `bp`.
// Any VRAM write into the range changes the sum (each bump is a monotonic increment).
uint64_t g178GenSumRect(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t pw, ph;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t py0 = y / ph, py1 = (y + std::max(1u, h) - 1u) / ph;
    const uint32_t px0 = x / pw, px1 = (x + std::max(1u, w) - 1u) / pw;
    const uint32_t basePage = bpBlocks >> 5;
    uint64_t sum = 0;
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1; ++px)
        {
            // +1 slack page for non-page-aligned block bases (over-coverage is safe: it can only
            // cause a spurious re-decode, never a stale texture).
            const uint32_t pg = basePage + py * widthPages + px;
            sum += g_g178PageGen[pg & (kG178PageCount - 1)].load(std::memory_order_relaxed);
            sum += g_g178PageGen[(pg + 1) & (kG178PageCount - 1)].load(std::memory_order_relaxed);
        }
    return sum;
}

// G277-R: slack-free gen sum for a PAGE-ALIGNED CT32 window (G273's measured display tuple).
// g178GenSumRect deliberately reads each page's +1 slack neighbour (safe for texture re-decode,
// where over-coverage only costs a spurious decode). For the G276 display fb snapshot that slack
// is actively harmful: the rows 0..415 window's last page (207) reads page 208 = zbp 0xd0's first
// Z page, so every display Z-wave readback bump spuriously invalidated the snapshot — and with
// G276's deferred readback the forced stale-VRAM re-upload erased the unpublished FBO rows (the
// MAP-0 Max-body flicker). A page-aligned base with exact-multiple dims tiles exactly; no slack
// is needed for correctness here because the snapshot only reasons about rows it covers.
static uint64_t g277GenSumAligned(uint32_t fbBlock, uint32_t fbw, uint32_t rows)
{
    const uint32_t basePage = fbBlock >> 5;               // caller guarantees (fbBlock & 31) == 0
    const uint32_t widthPages = std::max(1u, fbw);        // CT32 page = 64 px wide
    const uint32_t pageRows = rows / 32u;                 // caller guarantees rows % 32 == 0
    uint64_t sum = 0;
    for (uint32_t pr = 0; pr < pageRows; ++pr)
        for (uint32_t pc = 0; pc < widthPages; ++pc)
            sum += g_g178PageGen[(basePage + pr * widthPages + pc) & (kG178PageCount - 1)]
                       .load(std::memory_order_relaxed);
    return sum;
}

// G276 pending-display state (declared before g178BumpRectImpl so the G277 mover probe can read
// it; the lever/flush logic lives later with the other G276 code).
struct G276PendDisplay
{
    bool active = false;
    uint32_t fbp = 0u, fbw = 0u, fbBlock = 0u;
    int fbW = 0;
    int rowLo = 1 << 30, rowHi = -1;
};
static G276PendDisplay s_g276Pend;

void g178BumpRectImpl(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // G272 diagnostic: identify every generation publisher that touches a display range while its
    // FBO is ahead of VRAM. This is required to distinguish conservative page-range noise from an
    // actual escaped writer before the display residency arm can ever be promoted.
    static const bool s_g272Stat = envFlagEnabled("DC2_G272_STAT");
    if (s_g272Stat && g272DisplayColorWaveOn())
    {
        const G144BlockRange bump = g144RangeForRect(bpBlocks, static_cast<uint8_t>(psm), bw64,
                                                     x, y, w, h);
        for (int di = 0; di < 2; ++di)
        {
            uint32_t dirtyFbp = 0u, dirtyFbw = 0u;
            G144BlockRange dirty{};
            if (bump.valid && g272DirtyInfo(di, dirtyFbp, dirtyFbw, dirty) &&
                g144RangeOverlaps(bump, dirty))
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
                if (n < 160u)
                    std::fprintf(stderr,
                                 "[G272:bump] n=%u dirtyFbp=0x%x bp=0x%x bw=%u psm=0x%x rect=(%u,%u %ux%u)\n",
                                 n, dirtyFbp, bpBlocks, bw64, psm, x, y, w, h);
            }
        }
    }
    uint32_t pw, ph;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t py0 = y / ph, py1 = (y + std::max(1u, h) - 1u) / ph;
    const uint32_t px0 = x / pw, px1 = (x + std::max(1u, w) - 1u) / pw;
    const uint32_t basePage = bpBlocks >> 5;
    // G277-R mover probe: report every bump whose page walk (incl. the +1 slack) can touch the
    // 0x68 display chunk-12 pages 200..207 — pins the writer that trips publish-before-upload.
    {
        static const bool s_g277gm = envFlagEnabled("DC2_G277_GENMOVE");
        if (s_g277gm)
        {
            const uint32_t pgLo = basePage + py0 * widthPages + px0;
            const uint32_t pgHi = basePage + py1 * widthPages + px1 + 1u; // +1 slack
            if (pgLo <= 207u && pgHi >= 200u)
            {
                // Split the census: the frequent full-scissor inline-hook bump (pgLo=104) vs any
                // NARROW bump (pgLo>=160) — only bumps landing while the 0x68 pending union is
                // armed can trip publish-before-upload.
                static std::atomic<uint32_t> s_nFull{0}, s_nNarrow{0};
                const bool narrow = pgLo >= 160u;
                const uint32_t n = narrow
                    ? s_nNarrow.fetch_add(1u, std::memory_order_relaxed)
                    : s_nFull.fetch_add(1u, std::memory_order_relaxed);
                if (n < (narrow ? 48u : 8u))
                    std::fprintf(stderr,
                                 "[G277:bump200] %s n=%u pend=%d bp=0x%x bw=%u psm=0x%x rect=(%u,%u %ux%u) pages=%u..%u\n",
                                 narrow ? "NARROW" : "full", n,
                                 (s_g276Pend.active && s_g276Pend.fbp == 0x68u) ? 1 : 0,
                                 bpBlocks, bw64, psm, x, y, w, h, pgLo, pgHi);
            }
        }
    }
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            g_g178PageGen[pg & (kG178PageCount - 1)].fetch_add(1u, std::memory_order_relaxed);
            g_g178PageGen[(pg + 1) & (kG178PageCount - 1)].fetch_add(1u, std::memory_order_relaxed);
        }
}

// FNV-1a over the full texture descriptor → the backend's cache key. Bit 63 forced so 0 stays
// reserved for "untextured".
uint64_t g178TexKey(const GSContext &ctx, const GSTexaReg &texa,
                    const GSTexClutReg &texclut)
{
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { hsh ^= v; hsh *= 1099511628211ull; };
    mix(ctx.tex0.tbp0);
    mix((static_cast<uint64_t>(ctx.tex0.tbw) << 32) | ctx.tex0.cbp);
    mix((static_cast<uint64_t>(ctx.tex0.tw) << 40) | (static_cast<uint64_t>(ctx.tex0.th) << 32) |
        (static_cast<uint64_t>(ctx.tex0.psm) << 24) | (static_cast<uint64_t>(ctx.tex0.cpsm) << 16) |
        (static_cast<uint64_t>(ctx.tex0.csm) << 8) | ctx.tex0.csa);
    mix((static_cast<uint64_t>(texa.aem ? 1u : 0u) << 16) |
        (static_cast<uint64_t>(texa.ta1) << 8) | texa.ta0);
    // lookupCLUT addresses CBP through the captured TEXCLUT window.  DC2's RTT effect atlases
    // deliberately reuse one TBP/CBP descriptor with different CBW/COU/COV windows; omitting the
    // window aliases those distinct decoded textures to one backend cache entry.
    mix((static_cast<uint64_t>(texclut.cbw) << 32) |
        (static_cast<uint64_t>(texclut.cou) << 16) | texclut.cov);
    return hsh | (1ull << 63);
}

// Front-end registry entry: what the backend has resident, the page-gen sum its decode saw, and
// a CONTENT hash of the source pages — DC2 re-uploads texture data every frame ("transfer is
// cheaper than space"), so gen-only invalidation would re-decode everything every frame; the
// hash detects byte-identical re-uploads and keeps the GPU copy resident.
struct G178TexReg
{
    uint64_t genSum = 0;
    uint64_t contentHash = 0;
    int w = 0, h = 0;
};
std::unordered_map<uint64_t, G178TexReg> g_g178TexReg;

// Collect the distinct VRAM page indices a texture's conservative range covers (same walk as
// g178GenSumRect, deduped), then FNV-hash those pages' raw bytes. ~8-32 pages for title textures
// — a sequential ~64-256KB hash, far cheaper than a swizzle+CLUT whole-texture decode.
uint64_t g178HashPages(const uint8_t *vram, uint32_t vramSize,
                       uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                       uint32_t w, uint32_t h,
                       uint32_t clutBpBlocks)
{
    bool mark[kG178PageCount] = {};
    auto collect = [&](uint32_t bp, uint32_t bw, uint32_t p, uint32_t ww, uint32_t hh) {
        uint32_t pw, ph;
        g178PageDims(p, pw, ph);
        const uint32_t widthPages = std::max(1u, (std::max(1u, bw) * 64u) / pw);
        const uint32_t py1 = (std::max(1u, hh) - 1u) / ph;
        const uint32_t px1 = (std::max(1u, ww) - 1u) / pw;
        const uint32_t basePage = bp >> 5;
        for (uint32_t py = 0; py <= py1; ++py)
            for (uint32_t px = 0; px <= px1; ++px)
            {
                const uint32_t pg = basePage + py * widthPages + px;
                mark[pg & (kG178PageCount - 1)] = true;
                mark[(pg + 1) & (kG178PageCount - 1)] = true;
            }
    };
    collect(bpBlocks, bw64, psm, w, h);
    collect(clutBpBlocks, 1, GS_PSM_CT32, 64, 32);

    uint64_t hsh = 1469598103934665603ull;
    for (uint32_t pg = 0; pg < kG178PageCount; ++pg)
    {
        if (!mark[pg])
            continue;
        const size_t off = static_cast<size_t>(pg) * 8192u;
        if (off + 8192u > vramSize)
            continue;
        const uint8_t *p = vram + off;
        for (size_t i = 0; i < 8192u; i += 8)
        {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            hsh ^= v;
            hsh *= 1099511628211ull;
        }
    }
    return hsh;
}

// Per-fbp framebuffer snapshot state: when the fb pages' gen sum is unchanged since our last
// readback (and the row window is inside what we have ever uploaded/read back), the FBO already
// equals VRAM and the upload can be skipped.
struct G178FbSnap
{
    uint64_t genSum = 0;
    int rowLo = 1 << 30, rowHi = -1; // rows ever covered by an upload/readback
    bool valid = false;
};
std::map<uint32_t, G178FbSnap> g_g178FbSnap;

// G272 (DEFAULT OFF, DC2_G272_DISPLAY_COLOR_WAVE=1): extend the native renderer's existing
// residency contract to the two CT32 display targets. Unlike the RTT-only G261 records, these
// entries are materialized at presentation as well as at transfer/CPU/texture dependency edges.
// This removes synchronous full-FBO readback from intermediate display batches while keeping
// guest VRAM authoritative whenever normal downstream presentation or a CPU reader needs it.
struct G272DisplayRes
{
    uint32_t fbp = 0u;
    uint32_t fbw = 0u;
    int fbW = 0;
    bool dirty = false;
    int dirtyLo = 512;
    int dirtyHi = -1;
};
G272DisplayRes g_g272Display[2] = {{0x0u}, {0x68u}};
uint32_t g_g272VramSize = 0u;
thread_local bool t_g272DisplayBatch = false;
static std::atomic<uint64_t> g_g272Waves{0}, g_g272Materializes{0}, g_g272Failures{0},
    g_g272GenMoves{0};

bool g272DisplayColorWaveOn()
{
    static const bool s_on = g260NrOn() && g261WaveOn() &&
                             envFlagEnabled("DC2_G272_DISPLAY_COLOR_WAVE") &&
                             !envFlagEnabled("DC2_G272_NO_DISPLAY_COLOR_WAVE");
    return s_on;
}

static int g272DisplayIndex(uint32_t fbp)
{
    return fbp == 0x0u ? 0 : (fbp == 0x68u ? 1 : -1);
}

void g272SetDisplayBatch(uint32_t fbp, uint32_t fbw)
{
    t_g272DisplayBatch = g272DisplayColorWaveOn() && g272DisplayIndex(fbp) >= 0;
    if (!t_g272DisplayBatch)
        return;
    G272DisplayRes &r = g_g272Display[g272DisplayIndex(fbp)];
    if (r.dirty && r.fbw != fbw)
        g272MaterializeIndex(g272DisplayIndex(fbp), 2); // layout change before FBO recreation
}

bool g272DirtyInfo(int index, uint32_t &fbp, uint32_t &fbw, G144BlockRange &range)
{
    if (index < 0 || index >= 2 || !g_g272Display[index].dirty)
        return false;
    const G272DisplayRes &r = g_g272Display[index];
    fbp = r.fbp;
    fbw = r.fbw;
    range = g144RangeForRect(framePageBaseToBlock(r.fbp), GS_PSM_CT32, r.fbw,
                             0u, static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW),
                             static_cast<uint32_t>(r.dirtyHi - r.dirtyLo + 1));
    return range.valid;
}

static void g272MarkDirty(uint32_t fbp, uint32_t fbw, int fbW, int rowLo, int rowHi,
                          uint32_t vramSize)
{
    const int index = g272DisplayIndex(fbp);
    if (index < 0)
        return;
    G272DisplayRes &r = g_g272Display[index];
    r.fbw = fbw;
    r.fbW = fbW;
    r.dirty = true;
    r.dirtyLo = std::min(r.dirtyLo, rowLo);
    r.dirtyHi = std::max(r.dirtyHi, rowHi);
    g_g272VramSize = vramSize;
    g_g272Waves.fetch_add(1u, std::memory_order_relaxed);
}

void g272MaterializeIndex(int index, int cause)
{
    if (index < 0 || index >= 2)
        return;
    G272DisplayRes &r = g_g272Display[index];
    if (!r.dirty)
        return;
    const int rows = r.dirtyHi - r.dirtyLo + 1;
    const int glY = 512 - 1 - r.dirtyHi;
    uint8_t *vram = g_g144LastVram;
    static std::vector<uint32_t> s_px;
    const bool ok = vram != nullptr && g_g272VramSize != 0u && r.fbW > 0 && rows > 0 &&
                    g178_backend_read_color(r.fbp, r.fbW, 512, glY, rows, s_px) &&
                    s_px.size() == static_cast<size_t>(r.fbW) * rows;
    if (ok)
    {
        const uint32_t fbBlock = framePageBaseToBlock(r.fbp);
        for (int y = r.dirtyLo; y <= r.dirtyHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(511 - y - glY) * r.fbW;
            for (int x = 0; x < r.fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(
                    fbBlock, r.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                if (off + 4u <= g_g272VramSize)
                    std::memcpy(vram + off, &s_px[srcRow + x], 4u);
            }
        }
        g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                         static_cast<uint32_t>(r.dirtyLo), static_cast<uint32_t>(r.fbW),
                         static_cast<uint32_t>(rows));
        g149BumpVramGen();
        G178FbSnap &snap = g_g178FbSnap[r.fbp];
        snap.valid = true;
        snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                     static_cast<uint32_t>(r.fbW), 512u);
        g_g272Materializes.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        std::fprintf(stderr,
                     "[G272:materialize-fail] fbp=0x%x rows=%d..%d fbW=%d cause=%d\n",
                     r.fbp, r.dirtyLo, r.dirtyHi, r.fbW, cause);
        G178FbSnap &snap = g_g178FbSnap[r.fbp];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        g_g272Failures.fetch_add(1u, std::memory_order_relaxed);
    }
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    static const bool s_stat = envFlagEnabled("DC2_G272_STAT");
    const uint64_t mats = g_g272Materializes.load(std::memory_order_relaxed);
    if (s_stat && mats != 0u && (mats % 120u) == 0u)
        std::fprintf(stderr, "[G272:stat] waves=%llu materialize=%llu fail=%llu genMoves=%llu\n",
                     static_cast<unsigned long long>(g_g272Waves.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(mats),
                     static_cast<unsigned long long>(g_g272Failures.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g272GenMoves.load(std::memory_order_relaxed)));
}

void g272MaterializeAll(int cause)
{
    if (!g272DisplayColorWaveOn())
        return;
    g272MaterializeIndex(0, cause);
    g272MaterializeIndex(1, cause);
}

void g272MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown, int cause)
{
    if (!g272DisplayColorWaveOn())
        return;
    if (srcUnknown || dstUnknown)
    {
        g272MaterializeAll(cause);
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange range{};
        if (g272DirtyInfo(i, fbp, fbw, range) &&
            ((src.valid && g144RangeOverlaps(range, src)) ||
             (dst.valid && g144RangeOverlaps(range, dst))))
            g272MaterializeIndex(i, cause);
    }
}

// G178 stats ([G178:stat] every 300 flushes when enabled).
std::atomic<uint64_t> g_g178Flushes{0}, g_g178GpuFlushes{0}, g_g178Fallbacks{0};
std::atomic<uint64_t> g_g178TexDecodes{0}, g_g178TexHits{0}, g_g178TexHashHits{0};
std::atomic<uint64_t> g_g178FbUploads{0}, g_g178FbUploadSkips{0};
std::atomic<uint64_t> g_g178RejectState{0}, g_g178RejectBlend{0}, g_g178RejectTex{0};

// GSRasterizer::lookupCLUT reads the CLUT WINDOW (cbw/cou/cov) from `gs->m_texclut` and the
// alpha-key from `gs->m_texa` -- both LIVE state, correct for CPU inline/replay callers (which
// install the entry's own snapshot as active state first) but WRONG for g178DecodeWhole, which
// runs at flush time against whatever the game's live CLUT window has moved on to since this
// entry was captured (e.g. a later glyph's differently-positioned CLUT slot inside a shared CLUT
// bank). Same bug class as the T4HH nibble-plane fix above, second instance: replicate
// lookupCLUT's logic here using the entry's OWN captured `texclut`/`texa`.
uint32_t g178LookupClutFixed(uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut,
                             uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa,
                             uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, csm, csa, sourcePsm);
    const uint32_t clutWidth = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(texclut.cov) + (clutIndex >> 4);
    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(texa, cpsm, GSMem::ReadCT32(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(texa, cpsm, GSMem::ReadCT24(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16(vram, cbp, clutWidth, clutX, clutY))));
    case GS_PSM_CT16S:
        return applyTexa(texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16S(vram, cbp, clutWidth, clutX, clutY))));
    default:
        break;
    }
    return 0xFFFF00FFu;
}

// Eager whole-texture decode — the SAME leaf expressions as g149BuildDecoded's eager loop
// (readTexel*/ReadP8 → lookupCLUT), so GPU sampling sees byte-identical texels to the CPU path.
void g178DecodeWhole(GSRasterizer *self, GS *gs, uint8_t *vram, const GSContext &ctx,
                     const GSTexaReg &texa, const GSTexClutReg &texclut, int texW, int texH,
                     uint32_t *outPx)
{
    const uint8_t psm = ctx.tex0.psm;
    const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
    const auto decodeRows = [&](int rowLo, int rowHi, uint32_t *dst) {
        for (int vv = rowLo; vv <= rowHi; ++vv)
            for (int uu = 0; uu < texW; ++uu)
        {
            uint32_t c;
            switch (psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_CT24:
                c = applyTexa(texa, psm, self->readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
                c = applyTexa(texa, psm, self->readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_T8:
            {
                const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, ctx.tex0.tbp0, pageBwT8,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
                c = g178LookupClutFixed(vram, texa, texclut, idx, ctx.tex0.cbp, ctx.tex0.cpsm,
                                        ctx.tex0.csm, ctx.tex0.csa, psm);
                break;
            }
            default: // T4 / T4HL / T4HH
            {
                // readTexelPSMT4 derives the HH/HL/plain split from gs->activeContext().tex0.psm
                // (the LIVE GS state) -- correct for the CPU inline/replay paths (which install the
                // entry's ctx as active before sampling) but WRONG here: g178DecodeWhole runs at
                // flush time against whatever the game's active tex0.psm has moved on to since this
                // entry was captured, silently reading the wrong nibble plane for T4HH/T4HL sources
                // (e.g. the costume value-bar text atlas, tbp0=0x1a00 psm=T4HH per G5). Dispatch on
                // this entry's OWN captured `psm` instead, calling GSMem's readers directly.
                uint32_t idx;
                if (psm == GS_PSM_T4HH)
                    idx = GSMem::ReadP4HH(vram, ctx.tex0.tbp0, ctx.tex0.tbw,
                                          static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                else if (psm == GS_PSM_T4HL)
                    idx = GSMem::ReadP4HL(vram, ctx.tex0.tbp0, ctx.tex0.tbw,
                                          static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                else
                {
                    const uint32_t pageBwT4 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
                    idx = GSMem::ReadP4(vram, ctx.tex0.tbp0, pageBwT4,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                }
                c = g178LookupClutFixed(vram, texa, texclut, static_cast<uint8_t>(idx), ctx.tex0.cbp,
                                        ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, psm);
                break;
            }
            }
            dst[static_cast<size_t>(vv) * static_cast<size_t>(texW) +
                static_cast<size_t>(uu)] = c;
        }
    };

    // G298 promotion (default on): whole-texture decode is a read-only VRAM/CLUT transform
    // into independent RGBA rows. Reuse the joined row pool and complete every row before the
    // upload is appended to the backend batch. Keep combined and stage-local kill switches.
    static const bool s_g298ParTexDecode =
        !envFlagEnabled("DC2_G298_NO_PAR_PREP") &&
        !envFlagEnabled("DC2_G298_NO_PAR_TEXDECODE") &&
        !envFlagDisabled("DC2_G298_PAR_TEXDECODE");
    static const int s_g298TexDecodeLanes = [] {
        const char *value = std::getenv("DC2_G298_TEXDECODE_LANES");
        if (value == nullptr)
            return 8;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    const int pixels = texW * texH;
    const int lanes =
        s_g298ParTexDecode && texH >= 8 && pixels >= 4096
            ? s_g298TexDecodeLanes
            : 1;
    const auto joinedDecode = [&](int rowLo, int rowHi) {
        decodeRows(rowLo, rowHi, outPx);
    };
    GSRowPool::instance().run(0, texH - 1, lanes, joinedDecode);

    static const bool s_g298Verify =
        envFlagEnabled("DC2_G298_VERIFY_TEXDECODE");
    if (s_g298ParTexDecode && s_g298Verify && texW > 0 && texH > 0)
    {
        static std::vector<uint32_t> s_shadow;
        s_shadow.resize(static_cast<size_t>(texW) * texH);
        decodeRows(0, texH - 1, s_shadow.data());
        uint64_t bad = 0u;
        for (size_t i = 0; i < s_shadow.size(); ++i)
            bad += outPx[i] != s_shadow[i] ? 1u : 0u;
        static std::atomic<uint64_t> s_calls{0u}, s_pixels{0u}, s_bad{0u};
        const uint64_t n =
            s_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t totalPixels =
            s_pixels.fetch_add(static_cast<uint64_t>(pixels),
                               std::memory_order_relaxed) +
            static_cast<uint64_t>(pixels);
        const uint64_t totalBad =
            s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
        if (bad != 0u || n <= 8u || (n % 1024u) == 0u)
            std::fprintf(stderr,
                         "[G298:decode-verify] calls=%llu pixels=%llu bad=%llu "
                         "last=%dx%d lanes=%d bad=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(totalPixels),
                         static_cast<unsigned long long>(totalBad),
                         texW, texH, lanes,
                         static_cast<unsigned long long>(bad));
    }
}

// Per-entry validation + draw-state derivation. Returns false → whole flush falls back to CPU.
struct G178EntryState
{
    uint64_t texKey = 0;
    int texW = 0, texH = 0;
    uint8_t blend = 0, tfx = 0, tcc = 0;
    uint8_t depthFunc = 0;
    bool depthWrite = false, bilinear = false;
    uint8_t wrapU = 0, wrapV = 0;
    uint16_t scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
    uint16_t zbpPage = 0;
    uint8_t zpsm = 0;
    bool guestDepth = false;
    bool skip = false; // valid but draws nothing (e.g. ZTST=NEVER)
};

// ===== G262 RTT-family classifier reject census (opt-in, DC2_G262_CENSUS=1) =====
// G261's rttRej(classify=1836) lumps every g178ClassifyEntry reject site together, and the
// wave-level depth counter is only reachable under DC2_G242_GPU_DEPTH (waves decline it), so
// "depth=0" cannot distinguish state-shape rejects from the universal-Z depth reject INSIDE
// classify. This census splits the sites and logs deduplicated state shapes for the five RTT
// targets only, so the widening decision attacks the real dominant shape.
enum G262RejSite : int
{
    kG262RejFrame = 0,   // frame psm/fbmsk/fbw/fba/pabe
    kG262RejATest = 1,   // DATE or non-no-op alpha test
    kG262RejScissor = 2, // scissor sanity/bounds
    kG262RejBlend = 3,   // unsupported ALPHA a/b/c/d combo
    kG262RejTex = 4,     // tex psm/tbw/wrap/size
    kG262RejZPsm = 5,    // G242 path, non-Z24 zpsm
    kG262RejDepth = 6,   // universal-Z real Z test/write without GPU depth
    kG262RejLegacyZ = 7, // legacy town/title Z scopes
    kG262RejSiteCount = 8
};
static const char *const kG262SiteName[kG262RejSiteCount] = {
    "frame", "atest", "scissor", "blend", "tex", "zpsm", "depth", "legacyz"};
static std::atomic<uint64_t> g_g262Rej[kG262RejSiteCount] = {};
static std::atomic<uint64_t> g_g262RttSeen{0};
// Shape map is only touched on the flush-owning thread (same ownership as g_g178FbSnap).
static std::map<uint64_t, uint64_t> g_g262Shapes;

// G265 display reject census. Keep it separate from G262's RTT-only counters so enabling the
// diagnostic cannot change the historical RTT denominator or shape reports.
static std::atomic<uint64_t> g_g265Seen{0};
static std::atomic<uint64_t> g_g265Rej[kG262RejSiteCount] = {};
static std::unordered_set<uint64_t> g_g265Shapes;
static std::atomic<uint64_t> g_g265DepthBatches{0}, g_g265ZReadbacks{0};

static bool g265DisplayTarget(uint32_t fbp)
{
    return fbp == 0u || fbp == 0x68u;
}

static void g265NoteSeen(const G144Entry &e)
{
    if (g265CensusOn() && g265DisplayTarget(e.ctx.frame.fbp))
        g_g265Seen.fetch_add(1u, std::memory_order_relaxed);
}

static void g265NoteReject(int site, const G144Entry &e)
{
    if (!g265CensusOn() || !g265DisplayTarget(e.ctx.frame.fbp))
        return;
    g_g265Rej[site].fetch_add(1u, std::memory_order_relaxed);
    const GSContext &ctx = e.ctx;
    const uint64_t ts = ctx.test;
    const uint64_t al = ctx.alpha;
    uint64_t key = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(site)); mix(ctx.frame.fbp); mix(e.prim.type);
    mix(e.prim.tme); mix(e.prim.abe); mix(ctx.frame.fbw); mix(ctx.frame.psm);
    mix(ctx.frame.fbmsk); mix(ctx.fba); mix(e.pabe); mix(ts); mix(ctx.zbuf); mix(al);
    mix(ctx.tex0.psm); mix(ctx.tex0.tbw); mix(ctx.clamp);
    const bool fresh = g_g265Shapes.insert(key).second;
    static std::atomic<uint32_t> s_log{0};
    if (fresh && s_log.fetch_add(1u, std::memory_order_relaxed) < 48u)
        std::fprintf(stderr,
                     "[G265:rej] site=%s fbp=%03x prim=%u tme=%u abe=%u abcd=%u%u%u%u "
                     "fix=%u ate=%u atst=%u aref=%u afail=%u date=%u tpsm=0x%x tbw=%u "
                     "wrap=%u%u zte=%u ztst=%u zmsk=%u zpsm=%u fbmskNZ=%u fba=%u pabe=%u\n",
                     kG262SiteName[site], ctx.frame.fbp, static_cast<unsigned>(e.prim.type),
                     e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                     static_cast<unsigned>(al & 3u), static_cast<unsigned>((al >> 2u) & 3u),
                     static_cast<unsigned>((al >> 4u) & 3u),
                     static_cast<unsigned>((al >> 6u) & 3u),
                     static_cast<unsigned>((al >> 32u) & 0xffu),
                     static_cast<unsigned>(ts & 1u), static_cast<unsigned>((ts >> 1u) & 7u),
                     static_cast<unsigned>((ts >> 4u) & 0xffu),
                     static_cast<unsigned>((ts >> 12u) & 3u),
                     static_cast<unsigned>((ts >> 14u) & 1u), ctx.tex0.psm,
                     static_cast<unsigned>(ctx.tex0.tbw),
                     static_cast<unsigned>(ctx.clamp & 3u),
                     static_cast<unsigned>((ctx.clamp >> 2u) & 3u),
                     static_cast<unsigned>((ts >> 16u) & 1u),
                     static_cast<unsigned>((ts >> 17u) & 3u),
                     static_cast<unsigned>((ctx.zbuf >> 32u) & 1u),
                     static_cast<unsigned>((ctx.zbuf >> 24u) & 0xfu),
                     ctx.frame.fbmsk != 0u ? 1u : 0u,
                     (ctx.fba & 1ull) != 0ull ? 1u : 0u, e.pabe ? 1u : 0u);
}

static void g265Report()
{
    if (!g265CensusOn())
        return;
    std::fprintf(stderr, "[G265:census] seen=%llu shapes=%zu rej(",
                 (unsigned long long)g_g265Seen.load(std::memory_order_relaxed),
                 g_g265Shapes.size());
    for (int i = 0; i < kG262RejSiteCount; ++i)
        std::fprintf(stderr, "%s%s=%llu", i ? " " : "", kG262SiteName[i],
                     (unsigned long long)g_g265Rej[i].load(std::memory_order_relaxed));
    std::fprintf(stderr, ") zwave=%llu zrb=%llu\n",
                 (unsigned long long)g_g265DepthBatches.load(std::memory_order_relaxed),
                 (unsigned long long)g_g265ZReadbacks.load(std::memory_order_relaxed));
}

static bool g262CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G262_CENSUS");
    return s_on;
}

static void g262Report()
{
    if (!g262CensusOn())
        return;
    std::fprintf(stderr, "[G262:census] seen=%llu rej(",
                 (unsigned long long)g_g262RttSeen.load(std::memory_order_relaxed));
    for (int i = 0; i < kG262RejSiteCount; ++i)
        std::fprintf(stderr, "%s%s=%llu", i ? " " : "", kG262SiteName[i],
                     (unsigned long long)g_g262Rej[i].load(std::memory_order_relaxed));
    std::fprintf(stderr, ")\n");
    for (const auto &kv : g_g262Shapes)
    {
        const uint64_t k = kv.first;
        static constexpr uint32_t kFbp[5] = {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
        std::fprintf(stderr,
                     "[G262:shape] n=%llu site=%s fbp=%03x prim=%u tme=%u abe=%u abcd=%u%u%u%u "
                     "fix=%u ate=%u atst=%u aref=%u afail=%u date=%u tpsm=0x%02x wu=%u wv=%u "
                     "zte=%u ztst=%u zmsk=%u zpsm=%u fbmskNZ=%u fba=%u pabe=%u\n",
                     (unsigned long long)kv.second,
                     kG262SiteName[(k >> 60) & 0x7u],
                     kFbp[(k >> 57) & 0x7u],
                     (unsigned)((k >> 54) & 0x7u),  // prim.type
                     (unsigned)((k >> 33) & 1u),    // tme
                     (unsigned)((k >> 34) & 1u),    // abe
                     (unsigned)((k >> 35) & 3u), (unsigned)((k >> 37) & 3u), // a,b
                     (unsigned)((k >> 39) & 3u), (unsigned)((k >> 41) & 3u), // c,d
                     (unsigned)((k >> 43) & 0xFFu), // fix
                     (unsigned)((k >> 18) & 1u),    // ate
                     (unsigned)((k >> 19) & 7u),    // atst
                     (unsigned)((k >> 22) & 0xFFu), // aref
                     (unsigned)((k >> 30) & 3u),    // afail
                     (unsigned)((k >> 32) & 1u),    // date
                     (unsigned)((k >> 8) & 0x3Fu),  // tex psm
                     (unsigned)((k >> 14) & 3u), (unsigned)((k >> 16) & 3u), // wrapU, wrapV
                     (unsigned)((k >> 1) & 1u),     // zte
                     (unsigned)((k >> 2) & 3u),     // ztst
                     (unsigned)(k & 1u),            // zmsk
                     (unsigned)((k >> 4) & 0xFu),   // zpsm
                     (unsigned)((k >> 51) & 1u),    // fbmsk != 0
                     (unsigned)((k >> 52) & 1u),    // fba
                     (unsigned)((k >> 53) & 1u));   // pabe
    }
}

// ===== G266 post-classify consumer census (opt-in, DC2_G266_CENSUS=1; diagnostics only) =====
// G265 closed the display classifier (MAP-0 rejects zero) yet 1,024-wave checkpoints still drain
// RTT residency through mat(cpu=237 l2l=151 tex=113). This census records, at the consumer edges
// themselves: (a) WHY the flushed batch that forces a CPU-replay materialize left the GPU path
// (flush reject reason + classify site) and what batch/overlap shape it is; (b) the exact
// source/destination roles and layouts of every local→local copy that drains a resident target;
// (c) the state shape of every rejected FBO-direct texture bind that still materializes.
// Shape maps are touched only on the flush-owner threads (same contract as g_g178FbSnap).
static bool g266CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G266_CENSUS");
    return s_on;
}
static bool g282CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_CENSUS");
    return s_on;
}
enum G266FlushRej : int
{
    kG266FrNone = 0,        // GPU attempt succeeded (or none recorded yet)
    kG266FrOff = 1,         // GPU path off / backend not ready / empty list
    kG266FrCpuOnlyRtt = 2,  // RTT target without G255 routing (G252 CPU-only rule)
    kG266FrFbW = 3,         // frame width out of range
    kG266FrMixedTgt = 4,    // mixed fbp/fbw inside one flush
    kG266FrClassify = 5,    // g178ClassifyEntry reject (site in g_g266ClassifySite)
    kG266FrMixedZ = 6,      // two guest-Z tuples in one flush
    kG266FrRttDepth = 7,    // RTT guest depth without G262 widening
    kG266FrColorZAlias = 8, // color rows alias the Z rows
    kG266FrDepthSetup = 9,  // guest-depth setup fallback (rows/overlap-sync/G242 invariant)
    kG266FrTexAliasZ = 10,  // texture/CLUT pages alias the live Z window (or their depth sync)
    kG266FrSubmit = 11,     // backend submit failed
    kG266FrZReadback = 12,  // per-wave Z readback failed
};
static int g_g266FlushRej = kG266FrNone; // why the LAST g178TryFlushGpu returned false
static int g_g266ClassifySite = -1;      // kG262Rej* site of the latest classify reject
static const char *const kG266FrName[] = {
    "none", "off", "cpuonlyrtt", "fbw", "mixedtgt", "classify", "mixedz", "rttdepth",
    "colorzalias", "depthsetup", "texaliasz", "submit", "zreadback"};
static std::map<uint64_t, uint64_t> g_g266CpuShapes;
static std::map<uint64_t, uint64_t> g_g266L2lShapes;
static std::map<uint64_t, uint64_t> g_g266TexShapes;
static std::atomic<uint64_t> g_g266CpuN{0}, g_g266L2lN{0}, g_g266TexN{0};

static void g262NoteSeen(const G144Entry &e)
{
    if (!g262CensusOn() || g248TargetIndex(e.ctx.frame.fbp) < 0)
        return;
    g_g262RttSeen.fetch_add(1u, std::memory_order_relaxed);
}

static void g262NoteReject(int site, const G144Entry &e)
{
    // G266: remember the site unconditionally (single int, flush-owner threads only) so the
    // consumer census can attribute a classify-driven CPU replay to its exact reject site.
    g_g266ClassifySite = site;
    g265NoteReject(site, e);
    if (!g262CensusOn() || g248TargetIndex(e.ctx.frame.fbp) < 0)
        return;
    const uint64_t n = g_g262Rej[site].fetch_add(1u, std::memory_order_relaxed) + 1u;
    const GSContext &ctx = e.ctx;
    const uint64_t ts = ctx.test;
    const uint64_t al = ctx.alpha;
    uint64_t k = 0;
    k |= ((ctx.zbuf >> 32) & 1ull);                              // 0: zmsk
    k |= ((ts >> 16) & 1ull) << 1;                               // 1: zte
    k |= ((ts >> 17) & 3ull) << 2;                               // 2-3: ztst
    k |= ((ctx.zbuf >> 24) & 0xFull) << 4;                       // 4-7: zpsm
    k |= (e.prim.tme ? (static_cast<uint64_t>(ctx.tex0.psm) & 0x3Full) : 0x3Full) << 8; // 8-13
    k |= (static_cast<uint64_t>(ctx.clamp) & 3ull) << 14;        // 14-15: wrapU
    k |= ((static_cast<uint64_t>(ctx.clamp) >> 2) & 3ull) << 16; // 16-17: wrapV
    k |= (ts & 1ull) << 18;                                      // 18: ate
    k |= ((ts >> 1) & 7ull) << 19;                               // 19-21: atst
    k |= ((ts >> 4) & 0xFFull) << 22;                            // 22-29: aref
    k |= ((ts >> 12) & 3ull) << 30;                              // 30-31: afail
    k |= ((ts >> 14) & 1ull) << 32;                              // 32: date
    k |= (e.prim.tme ? 1ull : 0ull) << 33;                       // 33: tme
    k |= (e.prim.abe ? 1ull : 0ull) << 34;                       // 34: abe
    k |= (al & 3ull) << 35;                                      // 35-36: a
    k |= ((al >> 2) & 3ull) << 37;                               // 37-38: b
    k |= ((al >> 4) & 3ull) << 39;                               // 39-40: c
    k |= ((al >> 6) & 3ull) << 41;                               // 41-42: d
    k |= ((al >> 32) & 0xFFull) << 43;                           // 43-50: fix
    k |= ((ctx.frame.fbmsk != 0u) ? 1ull : 0ull) << 51;          // 51
    k |= ((ctx.fba & 1ull) != 0ull ? 1ull : 0ull) << 52;         // 52
    k |= (e.pabe ? 1ull : 0ull) << 53;                           // 53
    k |= (static_cast<uint64_t>(e.prim.type) & 0x7ull) << 54;    // 54-56
    k |= (static_cast<uint64_t>(g248TargetIndex(ctx.frame.fbp)) & 0x7ull) << 57; // 57-59
    k |= (static_cast<uint64_t>(site) & 0x7ull) << 60;           // 60-62
    ++g_g262Shapes[k];
    if ((n & 0x7FFull) == 0ull) // periodic report every 2048 RTT rejects at this site
        g262Report();
}

bool g178ClassifyEntry(const G144Entry &e, G178EntryState &st)
{
    g265NoteSeen(e);
    g262NoteSeen(e);
    const GSContext &ctx = e.ctx;
    // Frame target: CT32 display buffer, no channel masking, no dest-alpha tricks.
    if (ctx.frame.psm != GS_PSM_CT32 || ctx.frame.fbmsk != 0u || ctx.frame.fbw == 0u ||
        (ctx.fba & 1ull) != 0ull || e.pabe)
    {
        g262NoteReject(kG262RejFrame, e);
        g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const uint64_t ts = ctx.test;
    const uint32_t ate = static_cast<uint32_t>(ts & 1u);
    const uint32_t atst = static_cast<uint32_t>((ts >> 1) & 7u);
    const uint32_t aref = static_cast<uint32_t>((ts >> 4) & 0xffu);
    const uint32_t afail = static_cast<uint32_t>((ts >> 12) & 3u);
    const uint32_t date = static_cast<uint32_t>((ts >> 14) & 1u);
    // Alpha test is normally required to be a structural no-op. G265 admits only the two
    // MAP-0 display shapes proven by census: GEQUAL AREF={1,64,127}, AFAIL=KEEP. Shader discard is
    // exact for KEEP because it suppresses framebuffer and depth writes together; every other
    // ATST/AFAIL/DATE combination retains the CPU fallback (G240's independent FB/Z contract).
    uint8_t g265AlphaMode = 0u;
    const bool alphaNoOp = ate == 0u || atst == 1u || (atst == 5u && aref == 0u);
    if (date != 0u || !alphaNoOp)
    {
        const bool g265Keep = date == 0u && g265DisplayZWaveOn() &&
                              g265DisplayTarget(ctx.frame.fbp) && ate != 0u && atst == 5u &&
                              (aref == 1u || aref == 64u || aref == 127u) && afail == 0u;
        if (!g265Keep)
        {
            g262NoteReject(kG262RejATest, e);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const uint8_t refCode = aref == 1u ? 0u : (aref == 64u ? 1u : 2u);
        g265AlphaMode = static_cast<uint8_t>(0x20u | (refCode << 6u));
    }
    // Scissor (inclusive) sanity; FBO rows capped at 512.
    if (ctx.scissor.x1 < ctx.scissor.x0 || ctx.scissor.y1 < ctx.scissor.y0 ||
        ctx.scissor.y1 >= 512u || ctx.scissor.x1 >= 1024u)
    {
        g262NoteReject(kG262RejScissor, e);
        g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    st.scX0 = ctx.scissor.x0; st.scY0 = ctx.scissor.y0;
    st.scX1 = ctx.scissor.x1; st.scY1 = ctx.scissor.y1;

    // Blend classification (writePixel's ((A-B)*C>>7)+D with the census-confirmed combos).
    if (!e.prim.abe)
        st.blend = 0;
    else
    {
        const uint64_t al = ctx.alpha;
        const uint32_t a = static_cast<uint32_t>(al & 3u), b = static_cast<uint32_t>((al >> 2) & 3u);
        const uint32_t c = static_cast<uint32_t>((al >> 4) & 3u), d = static_cast<uint32_t>((al >> 6) & 3u);
        if (a == b && d == 0u)
            st.blend = 0; // (X-X)*C + Cs == Cs
        else if (a == 0u && b == 1u && c == 0u && d == 1u)
            st.blend = 1; // (Cs-Cd)*As + Cd
        else if (a == 0u && b == 2u && c == 0u && d == 1u)
            st.blend = 2; // Cs*As + Cd
        else if (a == 0u && b == 2u && c == 2u && d == 1u && ((al >> 32) & 0xFFull) == 128ull &&
                 g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0)
            st.blend = 3; // (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd (G262 census shape, RTT waves only)
        else if (a == 2u && b == 0u && c == 2u && d == 1u && ((al >> 32) & 0xFFull) == 128ull &&
                 g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0)
            st.blend = 4; // (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs (G262 census shape, RTT waves only)
        else if (a == 2u && b == 1u && c == 0u && d == 1u &&
                 g265DisplayZWaveOn() && g265DisplayTarget(ctx.frame.fbp))
            st.blend = 5; // G265 census: (0-Cd)*As>>7 + Cd == Cd*(1-As)
        else
        {
            g262NoteReject(kG262RejBlend, e);
            g_g178RejectBlend.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    }

    // Texture state.
    if (e.prim.tme)
    {
        const uint8_t psm = ctx.tex0.psm;
        const bool okPsm = psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 ||
                           psm == GS_PSM_CT16S || psm == GS_PSM_T8 || psm == GS_PSM_T4 ||
                           psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const uint8_t wu = static_cast<uint8_t>(ctx.clamp & 3u);
        const uint8_t wv = static_cast<uint8_t>((ctx.clamp >> 2) & 3u);
        st.texW = 1 << ctx.tex0.tw;
        st.texH = 1 << ctx.tex0.th;
        if (!okPsm || ctx.tex0.tbw == 0u || wu > 1u || wv > 1u ||
            st.texW > 1024 || st.texH > 1024)
        {
            g262NoteReject(kG262RejTex, e);
            g_g178RejectTex.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        st.texKey = g178TexKey(ctx, e.texa, e.texclut);
        st.tfx = ctx.tex0.tfx;
        st.tcc = ctx.tex0.tcc;
        // G255: CPU drawSprite selects linear filtering from MMAG OR the MMIN linear/mipmap
        // classes. Looking at MMAG alone made the T8 RTT downsample/effect sprites point-sampled
        // on GPU while their proven CPU replay was bilinear.
        const bool g286ExactEntry = g286TransientTarget(ctx.frame.fbp);
        const bool g255RttEntry =
            (g255GpuRttOn() && g248TargetIndex(ctx.frame.fbp) >= 0) || g286ExactEntry;
        st.bilinear = g255RttEntry
            ? tex1UsesLinearFilter(ctx.tex1)
            : (((ctx.tex1 >> 5u) & 1u) != 0u);
        // G179: GL_REPEAT wraps the final NORMALIZED [0,1] UV -- under GL_NEAREST, a sample that
        // floating-point-rounds to land exactly on (or a hair past) the wrap seam reads the
        // texture's OPPOSITE edge instead of clamping, bleeding a stray row/column of an unrelated
        // atlas cell into the draw (found on the debug-menu PSMT4HH font, which is REPEAT-wrapped
        // per G5, and is always point-sampled per G8 -- DC2's 2D UI/text is never bilinear). GS
        // hardware itself only cares about wrap for genuine out-of-declared-range addressing, which
        // point-sampled UI glyphs never actually need (each glyph's own UV stays within one atlas
        // cell) -- CLAMP_TO_EDGE removes the seam-bleed risk with no behavior change for in-range
        // sampling. Bilinear-sampled REPEAT textures (tiled background art) are unaffected.
        // G256's explicit texel fetch applies the CPU sampler's wrap mode itself, including point
        // samples.  Preserve G179's clamp-at-the-normalized-seam convention for the legacy shader.
        const bool exactSampler = (g256ExactRttOn() && g255RttEntry) || g286ExactEntry;
        st.wrapU = exactSampler ? wu : (st.bilinear ? wu : 1u);
        st.wrapV = exactSampler ? wv : (st.bilinear ? wv : 1u);
    }

    // G270 line entries deliberately retain drawLine's semantics: no texture and no depth
    // read/write. Alpha test, blend, scissor, and framebuffer validation above still apply. A
    // defensive gate keeps an accidentally captured textured/off-target line on CPU fallback.
    if (g270LineType(static_cast<uint32_t>(e.prim.type)))
    {
        if (!g270LineWaveOn() || e.prim.tme ||
            (ctx.frame.fbp != 0x0u && ctx.frame.fbp != 0x68u))
        {
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        st.depthFunc = g265AlphaMode;
        st.depthWrite = false;
        return true;
    }

    // Depth: the CPU rasterizer's SHIPPED title model — Z applies only to title-rock-scope tris
    // (private per-fbp Z, g106TitleZScope); sprites never touch Z (drawSprite has no Z code).
    st.depthFunc = g265AlphaMode;
    st.depthWrite = false;
    static const bool s_zTestDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const uint32_t zte = static_cast<uint32_t>((ts >> 16) & 1u);
    const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32) & 1ull) != 0ull;
    if (g203UniversalZEnabled())
    {
        // G203 correctness-first: under universal-Z the CPU VRAM Z buffer is authoritative for ALL
        // targets and prims (incl. the game's clear-sprite). The G178 GPU path reads back color
        // only, not the live VRAM Z page, so any entry that reads OR writes real Z must run on the
        // CPU replay to stay coherent. Trivial ztst=ALWAYS + zmsk (no test, no write) draws remain
        // GPU-eligible (e.g. UI/HUD sprites). GPU depth parity for universal-Z is a G204 follow-up.
        const bool zWrites = (zte != 0u) && !zmsk && (ztst != 0u);
        const bool zTests  = (zte != 0u) && (ztst != 1u); // NEVER/GEQUAL/GREATER all need Z
        if (!s_zTestDisabled && (zWrites || zTests))
        {
            // G262: widened RTT-family entries take the same guestDepth derivation as G242, but
            // the flush executes it as a NON-PERSISTENT per-wave Z round-trip (no ownership).
            const bool g262Rtt = g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0;
            const bool g265Display = g265DisplayZWaveOn() && g265DisplayTarget(ctx.frame.fbp);
            if (g242GpuDepthOn() || g262Rtt || g265Display)
            {
                const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
                const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
                // DC2's validated 3D paths all use PSMZ24. Other formats remain on the proven CPU
                // replay until they have their own exact float/packing verification.
                if (zpsm != 0x1u)
                {
                    g262NoteReject(kG262RejZPsm, e);
                    g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                if (ztst == 0u)
                {
                    st.skip = true; // NEVER neither draws nor writes Z.
                    return true;
                }
                st.guestDepth = true;
                st.zbpPage = static_cast<uint16_t>(zbpPage);
                st.zpsm = static_cast<uint8_t>(zpsm);
                // High bit 0x10 is private to G242: clamp interpolated fragment Z to uint24 in
                // the shader before applying the existing ALWAYS/GEQUAL/GREATER mapping.
                st.depthFunc = static_cast<uint8_t>(g265AlphaMode | 0x10u |
                    ((ztst == 1u) ? 1u : (ztst == 2u ? 2u : 3u)));
                st.depthWrite = !zmsk;
                return true;
            }
            g262NoteReject(kG262RejDepth, e);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    // Legacy (DC2_G203_LEGACY_Z=1): the old per-scope GPU-depth model.
    if (e.prim.type != GS_PRIM_SPRITE)
    {
        const bool rockScope = e.titleRockScope && e.prim.tme && ctx.frame.fbp != 0x139u &&
                               ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u;
        const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
        const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
        if (zte != 0u && g202TownZScope(ctx.frame.fbp, zbpPage, zpsm))
        {
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false; // G202 town Z needs real VRAM Z read/write; GPU path readbacks color only.
        }
        const bool zEnabled = !s_zTestDisabled && g106TitleZEnabled() && rockScope && zte != 0u;
        if (zEnabled)
        {
            if (zpsm != 0u)
            {
                g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                return false; // title is PSMZ32; other Z formats untested on this path
            }
            if (ztst == 0u)
            {
                st.skip = true; // NEVER: draws nothing
                return true;
            }
            st.depthFunc = static_cast<uint8_t>(g265AlphaMode |
                ((ztst == 1u) ? 1u : (ztst == 2u ? 2u : 3u)));
            st.depthWrite = !zmsk;
        }
    }
    return true;
}

bool g178StateEqual(const G178EntryState &x, const G178EntryState &y)
{
    return x.texKey == y.texKey && x.blend == y.blend && x.tfx == y.tfx && x.tcc == y.tcc &&
           x.depthFunc == y.depthFunc && x.depthWrite == y.depthWrite &&
           x.bilinear == y.bilinear && x.wrapU == y.wrapU && x.wrapV == y.wrapV &&
           x.scX0 == y.scX0 && x.scY0 == y.scY0 && x.scX1 == y.scX1 && x.scY1 == y.scY1;
}

void g178PushVertex(std::vector<G178Vtx> &out, const GSVertex &v, int ofx, int ofy,
                    const GSVertex &colorSrc, bool tme, bool fst, int texW, int texH,
                    float uvShiftU, float uvShiftV)
{
    G178Vtx o;
    o.x = v.x - static_cast<float>(ofx);
    o.y = v.y - static_cast<float>(ofy);
    o.z = v.z;
    o.r = colorSrc.r; o.g = colorSrc.g; o.b = colorSrc.b; o.a = colorSrc.a;
    if (!tme)
    {
        o.sq = 0.0f; o.tq = 0.0f; o.iq = 1.0f;
    }
    else if (fst)
    {
        o.sq = (static_cast<float>(v.u) / 16.0f + uvShiftU) / static_cast<float>(texW);
        o.tq = (static_cast<float>(v.v) / 16.0f + uvShiftV) / static_cast<float>(texH);
        o.iq = 1.0f;
    }
    else
    {
        // G222 CPU parity (drawTriangle STQ path): raw S/T/Q per vertex; the FS divides the
        // noperspective-lerped .xy by the lerped .z — the GS's own hyperbolic interpolation,
        // matching the G222 CPU sampler (u = lerp(S)/lerp(Q)). DC2_G222_AFFINE_TEX=1 restores
        // the pre-G222 affine s/|q| feed (parity with the old CPU sampler).
        static const bool s_g222Affine = envFlagEnabled("DC2_G222_AFFINE_TEX");
        if (s_g222Affine)
        {
            const float invQ = 1.0f / fabsQ(v.q);
            o.sq = v.s * invQ;
            o.tq = v.t * invQ;
            o.iq = 1.0f;
        }
        else
        {
            o.sq = v.s;
            o.tq = v.t;
            o.iq = fabsQ(v.q); // zero-guard only (returns q signed); FS max() guards the divide
        }
    }
    out.push_back(o);
}

// G270: turn the exact CPU Bresenham result into GL triangles. Each visited integer pixel becomes
// one [x,x+1]x[y,y+1] quad, so the native backend's existing triangle path preserves scissor,
// alpha-test, blend, target ownership, and submission order without relying on host line rules.
void g270AppendLinePixels(std::vector<G178Vtx> &out, const G144Entry &e)
{
    const GSVertex &v0 = e.v[0];
    const GSVertex &v1 = e.v[1];
    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    const int x1 = static_cast<int>(v1.x) - ofx;
    const int y1 = static_cast<int>(v1.y) - ofy;
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        const float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        GSVertex color = v1;
        if (e.prim.iip)
        {
            color.r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            color.g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            color.b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            color.a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }

        GSVertex c00 = color, c10 = color, c11 = color, c01 = color;
        const float rawX = static_cast<float>(x0 + ofx);
        const float rawY = static_cast<float>(y0 + ofy);
        c00.x = rawX;        c00.y = rawY;
        c10.x = rawX + 1.0f; c10.y = rawY;
        c11.x = rawX + 1.0f; c11.y = rawY + 1.0f;
        c01.x = rawX;        c01.y = rawY + 1.0f;
        c00.z = c10.z = c11.z = c01.z = v1.z;
        g178PushVertex(out, c00, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c10, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c11, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c00, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c11, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c01, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);

        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        ++step;
    }
}
} // namespace

// G242 private side channel implemented in ps2_gs_gpu_raster.cpp. Kept out of headers so this
// phase only rebuilds the two runtime translation units.
bool g242_backend_submit_depth(G178Batch &batch, uint64_t depthKey,
                               const std::vector<float> *depthUpload,
                               int depthY, int depthRows);
bool g275_backend_submit_depth_readback(G178Batch &batch, uint64_t depthKey,
                                        const std::vector<float> *depthUpload,
                                        int depthY, int depthRows,
                                        int readY, int readRows,
                                        std::vector<float> &depthReadback);
bool g242_backend_read_depth(uint64_t depthKey, int width, int height,
                             int depthY, int depthRows, std::vector<float> &depthReadback);

namespace
{
struct G242DepthState
{
    uint64_t key = 0u;
    uint32_t zbpPage = 0u;
    uint32_t zpsm = 0u;
    uint32_t fbw = 0u;
    int fbW = 0;
    bool valid = false;
    bool dirty = false;
    int dirtyLo = 512;
    int dirtyHi = -1;
    uint64_t guestGen = 0u;
};

std::unordered_map<uint64_t, G242DepthState> g_g242DepthStates;
std::atomic<uint64_t> g_g242DepthUploads{0};
std::atomic<uint64_t> g_g242DepthSyncs{0};
std::atomic<uint64_t> g_g242InvariantFails{0};

uint64_t g242DepthKey(uint32_t zbpPage, uint32_t zpsm, uint32_t fbw)
{
    return (1ull << 63u) | static_cast<uint64_t>(zbpPage & 0x1FFu) |
           (static_cast<uint64_t>(zpsm & 0xFu) << 9u) |
           (static_cast<uint64_t>(fbw & 0x3Fu) << 16u);
}

uint64_t g242GuestGen(const G242DepthState &s)
{
    return g178GenSumRect(s.zbpPage << 5u, s.fbw, GS_PSM_Z24, 0u, 0u,
                          static_cast<uint32_t>(s.fbW), 512u);
}

bool g242SyncDepthState(uint8_t *vram, G242DepthState &s)
{
    if (!s.valid || !s.dirty)
        return true;
    if (vram == nullptr || s.dirtyHi < s.dirtyLo)
        return false;
    const int syncLo = s.dirtyLo;
    const int syncHi = s.dirtyHi;
    const int rows = syncHi - syncLo + 1;
    const int glY = 512 - 1 - syncHi;
    static std::vector<float> readback;
    readback.clear();
    if (!g242_backend_read_depth(s.key, s.fbW, 512, glY, rows, readback) ||
        readback.size() != static_cast<size_t>(s.fbW) * rows)
    {
        std::fprintf(stderr, "[G242:sync] FAIL key=0x%llx rows=%d..%d\n",
                     static_cast<unsigned long long>(s.key), s.dirtyLo, s.dirtyHi);
        return false;
    }
    constexpr double kDepthToZ = 4294967296.0;
    const uint32_t zbpBlock = s.zbpPage << 5u;
    for (int y = s.dirtyLo; y <= s.dirtyHi; ++y)
    {
        const size_t srcRow = static_cast<size_t>(s.dirtyHi - y) * s.fbW;
        for (int x = 0; x < s.fbW; ++x)
        {
            double scaled = static_cast<double>(readback[srcRow + x]) * kDepthToZ;
            if (scaled < 0.0) scaled = 0.0;
            if (scaled > 16777215.0) scaled = 16777215.0;
            GSMem::WriteZ24(vram, zbpBlock, s.fbw, static_cast<uint32_t>(x),
                            static_cast<uint32_t>(y), static_cast<uint32_t>(scaled));
        }
    }
    g178BumpRectImpl(zbpBlock, s.fbw, GS_PSM_Z24, 0u,
                     static_cast<uint32_t>(s.dirtyLo), static_cast<uint32_t>(s.fbW),
                     static_cast<uint32_t>(rows));
    // Decoded CPU textures use their own coarse VRAM generation.  A Z page may also be texture or
    // CLUT storage, so publishing GPU depth bytes must invalidate that cache as well.
    g149BumpVramGen();
    s.guestGen = g242GuestGen(s);
    s.dirty = false;
    s.dirtyLo = 512;
    s.dirtyHi = -1;
    const uint64_t syncNo = g_g242DepthSyncs.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g242StatEnabled() && syncNo <= 32u)
        std::fprintf(stderr, "[G242:sync] n=%llu key=0x%llx zbp=0x%x fbw=%u rows=%d..%d\n",
                     static_cast<unsigned long long>(syncNo),
                     static_cast<unsigned long long>(s.key), s.zbpPage, s.fbw,
                     syncLo, syncHi);
    return true;
}

bool g242SyncAllDepth(uint8_t *vram)
{
    if (!g242GpuDepthOn())
        return true;
    bool ok = true;
    for (auto &kv : g_g242DepthStates)
        ok = g242SyncDepthState(vram, kv.second) && ok;
    return ok;
}

void g242MarkRectPages(std::array<uint8_t, kG178PageCount> &pages,
                       uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u)
        return;
    uint32_t pw = 0u, ph = 0u;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t basePage = bpBlocks >> 5u;
    const uint32_t px0 = x / pw, px1 = (x + w - 1u) / pw;
    const uint32_t py0 = y / ph, py1 = (y + h - 1u) / ph;
    for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
        for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            pages[pg & (kG178PageCount - 1u)] = 1u;
            pages[(pg + 1u) & (kG178PageCount - 1u)] = 1u;
        }
}

bool g242RectsOverlap(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
                      uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                      uint32_t bBp, uint32_t bBw, uint32_t bPsm,
                      uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    std::array<uint8_t, kG178PageCount> a{};
    std::array<uint8_t, kG178PageCount> b{};
    g242MarkRectPages(a, aBp, aBw, aPsm, ax, ay, aw, ah);
    g242MarkRectPages(b, bBp, bBw, bPsm, bx, by, bw, bh);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0u && b[i] != 0u)
            return true;
    return false;
}

// G273: exact page-set overlap for PAGE-ALIGNED bases. g242MarkRectPages deliberately marks pg+1
// as slack for non-page-aligned block bases, but applying that slack unconditionally turns the
// measured MAP-0 pair into a false alias: fbp=0x68 color rows use pages 104..207 and ZBP=0xd0
// starts at page 208. Separate GL color/depth attachments are valid for these disjoint pages.
// Unknown/non-aligned layouts never call this helper and retain the conservative G242 answer.
bool g273AlignedRectsOverlapExact(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
                                  uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                                  uint32_t bBp, uint32_t bBw, uint32_t bPsm,
                                  uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    if ((aBp & 31u) != 0u || (bBp & 31u) != 0u || aw == 0u || ah == 0u || bw == 0u || bh == 0u)
        return true; // fail closed; caller should normally have declined this helper
    auto markExact = [](std::array<uint8_t, kG178PageCount> &pages,
                        uint32_t bp, uint32_t width64, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        uint32_t pw = 0u, ph = 0u;
        g178PageDims(psm, pw, ph);
        const uint32_t widthPages = std::max(1u, (std::max(1u, width64) * 64u) / pw);
        const uint32_t basePage = bp >> 5u;
        const uint32_t px0 = x / pw, px1 = (x + w - 1u) / pw;
        const uint32_t py0 = y / ph, py1 = (y + h - 1u) / ph;
        for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
            for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
                pages[(basePage + py * widthPages + px) & (kG178PageCount - 1u)] = 1u;
    };
    std::array<uint8_t, kG178PageCount> a{};
    std::array<uint8_t, kG178PageCount> b{};
    markExact(a, aBp, aBw, aPsm, ax, ay, aw, ah);
    markExact(b, bBp, bBw, bPsm, bx, by, bw, bh);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0u && b[i] != 0u)
            return true;
    return false;
}

bool g242StateOverlapsRect(const G242DepthState &s,
                           uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t depthY = s.dirty && s.dirtyHi >= s.dirtyLo
                                ? static_cast<uint32_t>(s.dirtyLo)
                                : 0u;
    const uint32_t depthH = s.dirty && s.dirtyHi >= s.dirtyLo
                                ? static_cast<uint32_t>(s.dirtyHi - s.dirtyLo + 1)
                                : 512u;
    return g242RectsOverlap(s.zbpPage << 5u, s.fbw, GS_PSM_Z24,
                            0u, depthY, static_cast<uint32_t>(s.fbW), depthH,
                            bpBlocks, bw64, psm, x, y, w, h);
}

bool g242SyncOverlappingDepth(uint8_t *vram,
                              uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint64_t ignoreKey = 0u)
{
    if (!g242GpuDepthOn())
        return true;
    bool ok = true;
    for (auto &kv : g_g242DepthStates)
    {
        G242DepthState &s = kv.second;
        if (s.key == ignoreKey || !s.dirty ||
            !g242StateOverlapsRect(s, bpBlocks, bw64, psm, x, y, w, h))
            continue;
        ok = g242SyncDepthState(vram, s) && ok;
    }
    return ok;
}

bool g242WriteOverlapsDirtyDepth(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g242GpuDepthOn())
        return false;
    for (const auto &kv : g_g242DepthStates)
    {
        const G242DepthState &s = kv.second;
        // A CPU write anywhere in this logical Z surface makes the full-surface guest generation
        // diverge.  Even when it misses the dirty row interval, publish that interval first so the
        // subsequent clean mismatch can be handled by a normal full upload (never an invariant
        // failure or a lossy merge).
        if (s.dirty && g242RectsOverlap(s.zbpPage << 5u, s.fbw, GS_PSM_Z24,
                                        0u, 0u, static_cast<uint32_t>(s.fbW), 512u,
                                        bpBlocks, bw64, psm, x, y, w, h))
            return true;
    }
    return false;
}
} // namespace

void g242PrepareVramReadAll(uint8_t *vram)
{
    if (!g242SyncAllDepth(vram))
        std::fprintf(stderr, "[G242:barrier] FAIL all-depth materialization\n");
}

bool g242GuestDepthEnabled()
{
    return g242GpuDepthOn();
}

void g242PrepareVramReadRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g242SyncOverlappingDepth(vram, bpBlocks, bw64, psm, x, y, w, h))
        std::fprintf(stderr, "[G242:barrier] FAIL read bp=0x%x psm=0x%x\n", bpBlocks, psm);
}

// Extern hook called immediately before upload/local-transfer writes and immediately after CPU
// raster writes.  Pre-write callers materialize aliased GPU-owned depth; every caller bumps the
// page generations so a clean persistent depth texture is re-uploaded on its next use.
void g178NoteVramWriteRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g178GpuOn())
        return;
    if (g242WriteOverlapsDirtyDepth(bpBlocks, bw64, psm, x, y, w, h))
        g242PrepareVramReadRect(vram, bpBlocks, bw64, psm, x, y, w, h);
    g178BumpRectImpl(bpBlocks, bw64, psm, x, y, w, h);
}

// ===== G261 GPU-RESIDENT RTT WAVES (DEFAULT-ON since G266; see g261WaveOn above) =====
// Residency state for the five G248 RTT targets. `dirty` = the target's FBO holds rows guest
// VRAM does not (a wave rendered with readback skipped). Ownership contract: every CPU
// read/write of those pages must call a materialize hook FIRST; the hooks below cover the four
// real consumer edges (CPU band replay, inline primitive, VRAM transfers, local→host reads).
// All state is touched only on the threads that already own the G144 flush machinery (GS worker
// under MTGS / EE thread at the frame barrier — never concurrently), same as g_g178FbSnap.
struct G261Res
{
    bool dirty = false;
    int dirtyLo = 512, dirtyHi = -1; // GS rows the FBO has and VRAM lacks
    int covLo = 512, covHi = -1;     // GS rows DEFINED in the FBO (upload/readback/render union)
    uint32_t fbw = 0;
    int fbW = 0;
    uint64_t genAtSkip = 0;          // gen sum over the ROW WINDOW when readback was last skipped
    // Row-window-scoped residency footprint. A whole-target (512-row) range is refuted: at RTT
    // block density (targets 3 pages apart, texture pages ~0x34e0 close behind 0x2720) it
    // swallows the neighbor targets and the per-frame texture uploads, materializing every wave
    // within milliseconds (G261 bring-up measured mat.up=409/window from exactly this).
    int winLo = 512, winHi = -1;     // rows the residency claims (cov ∪ dirty)
    G144BlockRange range{};          // block range of the claimed row window (overlap tests)
    // G264: rows the guest re-uploaded into VRAM while this target was resident, deferred for
    // an address-exact VRAM→FBO patch at the next consumer/flush edge instead of a materialize
    // round-trip. Per-pixel bits are required because DC2 intentionally aliases the same pages
    // through different DBP/DBW and FRAME/FBW layouts; a source rect is not generally a target
    // rect, and coalescing disjoint uploads into a row bounding box can clobber dirty FBO pixels.
    bool mirPending = false;
    int mirLo = 512, mirHi = -1;
    std::array<std::array<uint64_t, 8>, 512> mirBits{}; // max target width = 512 pixels
    // G291: cumulative per-pixel "upload re-covered since the last wave write to this row" map
    // (same tx/ty geometry as mirBits, but NOT cleared by the mirror flush). A 64x32 CT32 page
    // tile whose every pixel bit is set has been fully re-written in guest VRAM by noted
    // uploads AFTER the last GPU render of those rows, so VRAM >= FBO for that page and a
    // texture/CLUT read overlapping only such pages needs no read-triggered materialization
    // (the materialize round-trip — mirror VRAM->FBO then readback FBO->VRAM — is the identity
    // on VRAM for a fully re-covered page). Bits are cleared on every wave render of the rows,
    // on any residency reset/width flip, and are meaningful only while g291CovFbw == fbw.
    std::array<std::array<uint64_t, 8>, 512> g291Cov{};
    uint32_t g291CovFbw = 0;
};
static constexpr uint32_t kG261Fbp[kG248TargetCount] = {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
static G261Res g_g261Res[kG248TargetCount];
static uint32_t g_g261VramSize = 0; // set beside the wave flush (GS::m_vramSize is private)
enum G261MatCause : int
{
    kG261MatCpuReplay = 0,
    kG261MatInline = 1,
    kG261MatUpload = 2,
    kG261MatLocal = 3,
    kG261MatL2H = 4,
    kG261MatTexAlias = 5,
    kG261MatResync = 6,
    kG261MatFail = 7,
};
static std::atomic<uint64_t> g_g261Waves{0}, g_g261SkippedRb{0}, g_g261FboBinds{0},
    g_g261FboRejects{0}, g_g261Invariants{0}, g_g261MatUnknownRange{0};
// G285: page-exact destructive local-copy consumption. The copy replaces every dirty page after
// one partially covered page-row, so only that prefix row needs publication.
static std::atomic<uint64_t> g_g285L2lConsumes{0}, g_g285L2lRowsPublished{0},
    g_g285L2lRowsElided{0};
static std::atomic<uint64_t> g_g263FboBindBi{0}; // G263: bilinear-admitted FBO-direct binds
// G267: display-TRISTRIP bilinear tap/page census. The behavior prototype was neutral after its
// required alias/lifecycle repairs and was removed; these counters remain diagnostics-only.
static std::atomic<uint64_t> g_g267TriCandidates{0}, g_g267TriEligible{0},
    g_g267TriFootReject{0}, g_g267TriAliasReject{0};
// G264 upload-into-FBO mirror telemetry: uploads deferred to a mirror, mirrors executed,
// mirror failures (residency dropped loudly).
static std::atomic<uint64_t> g_g264MirNoted{0}, g_g264MirFlushed{0}, g_g264MirFail{0};
// ===== G280 GPU PHYSICAL-ALIAS PAGE VIEW (default-off bring-up, DC2_G280_ALIAS_VIEW=1) =====
// G267 proved the display TRISTRIP consumer of the 0x139 character RTT samples one physical CT32
// page (0x2920) that is NEWER in the dirty sibling target 0x146 — the same GS page rendered under
// two FRAME/FBW layouts (fbw=8 vs fbw=2). Today that consumer pays cause-5 materializations of
// BOTH targets plus a full 512x512 VRAM texture decode. G280 instead composes the sampled view on
// the GPU: the aliased page tiles are copied FBO->FBO (bit-exact 64x32 rects — CT32 within-page
// pixel order is layout-independent, only page SELECTION depends on FBW), the consumer binds the
// producer FBO directly (existing G261/G263 srcFbp path), and VRAM publication stays exactly on
// today's consumer edges.
//
// Ownership/versioning model:
//  - Physical-page authority is unchanged: a dirty target's FBO owns its pages until its existing
//    materialization edge publishes them; VRAM owns everything else. G280 adds NO publication.
//  - Each target gets a monotonic contentGen, bumped whenever its FBO CONTENT changes (wave
//    render, G264 mirror patch, uploadFb). An overlay entry records the src contentGen at copy
//    time; a moved gen makes the copy stale (re-resolved at the next consumer, restored from
//    VRAM before any dst publication).
//  - Overlay tiles in the dst FBO are a READ CACHE, never dst-owned: before the dst target
//    materializes (ANY cause), every live overlay tile is overwritten with the current guest
//    VRAM bytes and dropped, so dst never publishes alias-page bytes whose authority is the src
//    FBO or VRAM. Restore failure is loud and drops residency without a readback (G264 pattern).
//  - Ordered invalidation: dst waves rendering rows that overlap an overlay tile drop the entry
//    (the blit reproduced the physical page state at resolve time, so later dst draws layer on
//    top exactly as GS command order did); src waves/mirrors bump contentGen (stale -> re-copy);
//    CPU/transfer writers keep their existing materialize/mirror hooks, which land in the
//    restore-and-drop paths above.
//  - Unsupported consumers and presentation are untouched: both targets stay dirty and publish
//    at exactly today's consumer edges; the RTT family is never presented directly.
// Admitted alias tuples are evidence-limited (G267 census): dst 0x139 (ti 0) <- src 0x146 (ti 3).
// Everything else fails closed to the legacy bind/materialize loop.
struct G280Ovl
{
    uint32_t pageBlock = 0u; // absolute GS block address of the physical page (multiple of 32)
    uint8_t srcTi = 0u;
    uint64_t srcGen = 0u;    // src contentGen when the tile was copied
    uint64_t pageEpoch = 0u; // G283: physical-page epoch when resolved (authority-off => 0)
    uint16_t dstRow = 0u, dstCol = 0u; // page coords under the DST layout (row*32 / col*64 px)
    uint16_t srcRow = 0u, srcCol = 0u; // page coords under the SRC layout
};
static std::vector<G280Ovl> g_g280Ovl[kG248TargetCount]; // per DST target; flush-owner thread only
static uint64_t g_g280ContentGen[kG248TargetCount] = {};
static std::atomic<uint64_t> g_g280Binds{0}, g_g280Copies{0}, g_g280CopyFail{0},
    g_g280Restores{0}, g_g280RestoreFail{0}, g_g280Drops{0}, g_g280SibSkips{0},
    g_g280RejFoot{0}, g_g280RejTuple{0}, g_g280RejPage{0}, g_g280VerifyBad{0};
// G281: PSMT8 view over the resident 0x13c CT32 target. The view is a decoded RGBA texture
// produced entirely on the G178 worker from the source FBO + current guest CLUT; no guest-VRAM
// color publication occurs. It is deliberately a child of the G280 alias/view lever.
static std::atomic<uint64_t> g_g281Candidates{0}, g_g281Views{0}, g_g281BuildFail{0},
    g_g281RejShape{0}, g_g281RejPage{0}, g_g281RejClut{0}, g_g281VerifyBad{0};
// G282: exact captured coverage + write-side page ownership for the measured
// fbp=0x139/fbw=8 atlas batch. A page-aligned leading clear makes its touched pages independent
// of their old alias owners; after the GPU batch, exact CT32 page copies fan the new bytes back
// to every resident sibling view before any of those views can publish stale data.
struct G282Fanout
{
    uint32_t pageBlock = 0u;
    uint8_t dstTi = 0u;
    uint16_t srcRow = 0u, srcCol = 0u;
    uint16_t dstRow = 0u, dstCol = 0u;
};
static std::atomic<uint64_t> g_g282Candidates{0}, g_g282TightBatches{0},
    g_g282AliasSaved{0}, g_g282FanoutPages{0}, g_g282FanoutFail{0},
    g_g282RejShape{0}, g_g282RejOwner{0}, g_g282VerifyBad{0};
static bool g280AliasOn();
static bool g281ViewOn();
static bool g282TightRowsOn();
static bool g294OwnerTokenRequested(); // G294 combined-arm request (defined near g280 bodies)
static bool g294OwnerTokenOn();         // G294 owner-token active (rides g280 alias-view)
static bool g280PrepareMaterialize(int ti); // restore+drop overlays before dst publication
static void g280DropOverlays(int ti, uint64_t *dropCounter = nullptr);
static void g280NoteFboContentChange(int ti, int rowLo, int rowHi);
static void g280NoteMirrorPatch(int ti, int lo, int hi);
static void g280HygienePass();
static void g280Report();
// Why RTT batches fall to the CPU under waves (the residency-killing consumers): entry classify
// rejects vs the color-only guest-depth fail-close. Pins the follow-up phase's target.
static std::atomic<uint64_t> g_g261RttClassifyRej{0}, g_g261RttDepthRej{0};
// G262 widened-wave telemetry: waves carrying per-wave guest depth, and how many of those
// actually paid the immediate Z readback (write-enabled batches only).
static std::atomic<uint64_t> g_g262DepthWaves{0}, g_g262ZReadbacks{0};
// G274 measurement: wall-time spent in the per-wave Z round-trip swizzle loops (upload:
// ReadZ24->float; readback: backend glReadPixels + WriteZ24). Diagnostics only, cached gate.
static std::atomic<uint64_t> g_g274ZUpNs{0}, g_g274ZRbNs{0};
// G274 residency-ceiling census: was the previous same-key Z readback->VRAM->reupload redundant
// (no intervening VRAM-Z writer)? If the whole-target Z gen is unchanged since our last writeback,
// nobody consumed VRAM Z between waves and a resident GPU depth would have elided the round-trip.
static std::unordered_map<uint64_t, uint64_t> g_g274LastZGen; // depthKey -> gen after last writeback
static std::atomic<uint64_t> g_g274ZRedundant{0}, g_g274ZNecessary{0};
// G278: within-drain Z-readback coalescing. The backend depth texture is authoritative only until
// the next proven VRAM-Z consumer (or the drain/presentation boundary); it never crosses a frame.
// The pending union stores enough layout identity to publish without consulting mutable GS state.
struct G278PendDepth
{
    bool active = false;
    bool display = false;
    uint64_t key = 0u;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
    uint32_t zbpPage = 0u, fbw = 0u;
    int fbW = 0;
    int rowLo = 512, rowHi = -1;
    std::array<uint64_t, 8> staleRows{}; // CPU-written rows outside dirty union; backend is stale
};
static G278PendDepth s_g278Pend; // flush-owner thread only (same contract as s_g276Pend)
static std::atomic<uint64_t> g_g278Deferred{0}, g_g278Flushes{0}, g_g278Saved{0},
    g_g278FlushRows{0}, g_g278UploadSkips{0}, g_g278FlushFail{0},
    g_g278InvariantFail{0}, g_g278StaleMarks{0}, g_g278Reinits{0};
static std::atomic<uint64_t> g_g278FlushByCause[9] = {};
// G274: total wall-time inside the synchronous backend submit (all waves) + count.
static std::atomic<uint64_t> g_g274SubNs{0}, g_g274SubN{0};
// G275: one synchronous worker job now owns the render and its mandatory immediate Z readback.
// The CPU swizzle/publication remains before this flush returns; only the redundant queue/future
// boundary and scratch-FBO reattachment are removed.
static std::atomic<uint64_t> g_g275FusedJobs{0}, g_g275ReadWaitsElided{0};
static std::atomic<uint64_t> g_g275FusedNs{0};
// G276 measure-first probe (default-off, DC2_G276_PROBE=1): split the synchronous submit time by
// batch category and measure the color-readback window span, to decide whether the full-FBO color
// glReadPixels (renderBatch reads 0..fbH but only rowLo..rowHi is swizzled back) is a real cost.
// Buckets indexed [skipReadback][guestDepthWrites].
static std::atomic<uint64_t> g_g276SubNs[2][2] = {};
static std::atomic<uint64_t> g_g276SubN[2][2] = {};
static std::atomic<uint64_t> g_g276ColorRbN{0}, g_g276ColorRbWinRows{0}, g_g276ColorRbFullRows{0};
static bool g276ProbeOn()
{
    static const bool s_on = envFlagEnabled("DC2_G276_PROBE");
    return s_on;
}
// G276 within-drain display-color readback coalescing lever + pending state + counters (defined
// here so g261Report can print them; the flush body is defined after g261Materialize).
static bool g276DisplayRbOn()
{
    // G276 PROMOTION (2026-07-17): DEFAULT-ON. Coalesces consecutive same-target display-color
    // readbacks within a drain (the persistent FBO accumulates; the gen-snapshot already suppresses
    // re-upload for a same-target continuation, and the deferred readback bumps no color gens), so a
    // K-batch same-target run pays ONE full-FBO glReadPixels instead of K. VRAM is authoritative at
    // exactly today's boundaries (each g272MaterializeAll consumer edge + drain end/present + any
    // different-target flush), so the cross-frame G242/G249/G272 ownership trap cannot occur.
    // Rides the native wave stack (g261WaveOn already declines under every exact oracle, the
    // G242/G256 ownership arms, the tile-bin/GPU kills, and the VRAM-dump diagnostics). Kill:
    // DC2_G276_NO_DISPLAY_RB_COALESCE=1 (or DC2_G276_DISPLAY_RB_COALESCE=0); the whole-stack kill
    // DC2_G26X_NO_NATIVE=1 also disables it.
    static const bool s_on = !envFlagEnabled("DC2_G276_NO_DISPLAY_RB_COALESCE") &&
                             !envFlagDisabled("DC2_G276_DISPLAY_RB_COALESCE");
    return s_on && g261WaveOn();
}
// G284 experiment: extends the G276 within-drain display-color readback
// coalescing ACROSS the ranged transfer edges (cause 7 host->local upload dst, cause 8 local->local
// src+dst) when the transfer provably misses the pending display rows. The upload edge is NOT a
// display-color consumer; before G284 it force-flushed the pending display readback unconditionally,
// so every streaming-texture upload (~3.9 MB/frame on MAP-0) paid a full-FBO display glReadPixels.
// The DEPTH readback already skips this exact edge when disjoint (g278FlushPendingDepthForRanges);
// G284 is the display twin. Bit-exact by the same disjointness proof G273/G278 use: if the transfer
// dst/src does not overlap the pending display rows, publishing the display readback now vs. at the
// next real consumer edge (drain-end / present / different-target / CPU-replay / inline / an
// OVERLAPPING l2l display->work-page copy) yields identical guest VRAM. Fail-closed: unknown ranges,
// any overlap, or degenerate pending take the original unconditional flush. Rides G276 (so it
// auto-declines under every oracle/kill G276 already declines under, incl. DC2_G26X_NO_NATIVE=1).
// G289 PROMOTION (2026-07-18): DEFAULT-ON only as an atomic pair with G289 ownership. G284 alone
// lost MAP-2's left cliff because its range-local proof did not cover the later physical-alias
// consumer. G289 now owns every skipped display->work copy or fails closed to publication, so
// either kill disables both halves and restores the G287-safe baseline.
static bool g289PolicyOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G289_NO_GPU_LOCAL_COPY") &&
        !envFlagDisabled("DC2_G289_GPU_LOCAL_COPY");
    return s_on;
}
static bool g284UploadCoalesceOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G284_NO_UPLOAD_COALESCE") &&
        !envFlagDisabled("DC2_G284_UPLOAD_COALESCE");
    return s_on && g289PolicyOn() && g276DisplayRbOn();
}
static bool g284StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G284_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g284UploadSkipped{0}, g_g284UploadFlushed{0};
// G289: one versioned GPU-local-copy owner for the measured display CT32 -> work-page transfers.
// It is deliberately coupled to G284 because the source must still be authoritative in the
// display FBO. Default-on after exact oracle, hardware-reference route matrix, and timing gates.
static bool g289LocalCopyOn()
{
    return g289PolicyOn() && g284UploadCoalesceOn();
}
static bool g289VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_VERIFY");
    return s_on && g289LocalCopyOn();
}
static bool g289StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_STAT") || g289VerifyOn();
    return s_on;
}
static bool g289RawVramDiagOn()
{
    static const bool s_on =
        envFlagEnabled("DC2_DUMP_VRAMRGN") ||
        envFlagEnabled("DC2_G37_RTTDUMP") ||
        envFlagEnabled("DC2_G38_VRAMDUMP") ||
        envFlagEnabled("DC2_G86_TEXDUMP") ||
        envFlagEnabled("DC2_G194_VRAM_DUMP") ||
        envFlagEnabled("DC2_G231_GEMDUMP") ||
        envFlagEnabled("DC2_G232_RTTDUMP");
    return s_on;
}
static constexpr uint32_t kG289ZeroFbpToken = 0xfffffffdu;
static constexpr uint32_t kG289AliasCt32Token = 0xfffffffcu;
static constexpr uint32_t kG289AliasCt24Token = 0xfffffffau;
static constexpr uint32_t kG289WorkCt24Token = 0xfffffff9u;
static constexpr uint32_t kG289WorkCt32Token = 0xfffffff8u;
struct G289CopyOwner
{
    bool active = false;
    uint64_t version = 0u;
    uint32_t srcFbp = 0u;
    uint32_t dstFbp = 0u;
    uint32_t firstOwnedBlock = 0u;
    uint32_t lastOwnedBlock = 0u;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0u;
};
static G289CopyOwner s_g289Owner;
struct G289PendingUploadOverwrite
{
    bool active = false;
    uint64_t ownerVersion = 0u;
    uint32_t dbp = 0u;
    uint8_t dbw = 0u;
    uint8_t dpsm = 0u;
    uint32_t dsax = 0u;
    uint32_t dsay = 0u;
    uint32_t rrw = 0u;
    uint32_t rrh = 0u;
    uint32_t replacedThroughBlock = 0u;
};
static G289PendingUploadOverwrite s_g289PendingUpload;
static std::atomic<uint64_t> g_g289Direct{0}, g_g289Copies{0}, g_g289CopyFail{0},
    g_g289Consumes{0}, g_g289Carries{0}, g_g289OwnerViews{0}, g_g289OwnerViewFail{0},
    g_g289OwnerBinds{0}, g_g289Materializes{0}, g_g289MatFail{0},
    g_g289UploadOverwrites{0}, g_g289SuffixPublishes{0},
    g_g289InlineCarries{0}, g_g289FrameCarries{0},
    g_g289VerifyChecks{0}, g_g289VerifyBad{0},
    g_g289Unexpected{0};
// G288 bring-up: G284 can leave display color newer in the persistent FBO while guest VRAM stays
// stale across a disjoint upload. CPU replay already publishes at cause 3 before reading VRAM, but
// a later GPU batch can read target/Z/texture/CLUT bytes directly without crossing any G276 edge.
// Audit that whole batch before its first VRAM read and publish only when its physical block ranges
// overlap the pending display owner (unknown ranges fail closed). Opt-in until MAP-2 reference
// composition and the G284 payoff both pass.
static bool g288GpuConsumerOn()
{
    static const bool s_on = envFlagEnabled("DC2_G288_GPU_CONSUMER") &&
                             !envFlagEnabled("DC2_G288_NO_GPU_CONSUMER");
    return s_on && g284UploadCoalesceOn();
}
static bool g288StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G288_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g288GpuAudits{0}, g_g288GpuPublishes{0},
    g_g288GpuDisjoint{0}, g_g288GpuUnknown{0};
// (G276PendDisplay struct + s_g276Pend moved above g178BumpRectImpl for the G277 mover probe.)
static std::atomic<uint64_t> g_g276Deferred{0}, g_g276Flushes{0}, g_g276FlushRows{0},
    g_g276FlushFail{0};
// G277-R repair counters (MAP-0 Max body flicker, user-reported on the G276 default):
// publish-before-upload fail-safe fires + exact-window adoptions.
static std::atomic<uint64_t> g_g277UploadPublish{0}, g_g277ExactGenWin{0};
// G277-R mover probe (default-off, DC2_G277_GENMOVE=1): per-32-row-chunk gen sums for fbp 0x68
// captured at snapshot anchor, compared at each publish-before-upload event to pin WHICH rows'
// pages a mystery writer bumped between same-target deferred batches.
static bool g277GenMoveOn()
{
    static const bool s_on = envFlagEnabled("DC2_G277_GENMOVE");
    return s_on;
}
static uint64_t s_g277Chunk[13] = {};   // rows 32i..32i+31, i=0..12 (rows 0..415)
static bool s_g277ChunkValid = false;   // flush-owner thread only
static bool g277ExactDisplayGenOn()
{
    // G277-R (2026-07-17): the display fb snapshot's FULL-HEIGHT CT32 gen window for fbp 0x68
    // (pages 104..231) overlaps zbp 0xd0's Z pages (208..231), so every per-wave Z readback
    // writeback bump spuriously invalidated the snapshot. Pre-G276 that only re-uploaded identical
    // bytes; with G276's deferred readback it re-uploaded STALE VRAM over unpublished FBO rows and
    // erased Max's body (dense MAP-0: 484/1009 frames body-absent). G273 proved color rows 0..415
    // occupy pages 104..207, disjoint from Z pages 208+; the snapshot window now uses that exact
    // row bound for the measured tuple whenever all coverage stays <=415 (anything taller falls
    // back to the conservative full height). A/B kill: DC2_G277_NO_EXACT_DISPLAY_GEN=1.
    static const bool s_on = !envFlagEnabled("DC2_G277_NO_EXACT_DISPLAY_GEN");
    return s_on;
}
// G277 measure-first census (default-off, DC2_G277_CENSUS=1): sizes range-exact consumer edges.
// Hypothesis: most G276 pending-display flushes are PHANTOM consumers — fully-scissored offscreen
// inline prims (the sceGsSwapDBuff sync linestrips draw nothing) and range-skipped l2h upload
// edges whose destination never touches the pending display rows — and the same exactness would
// make within-drain Z-readback coalescing viable (G274's 44% redundancy counted writers only;
// reads don't bump gens, so the READ-side consumer census is this one). Counters only, no
// behavior change; all runs on the flush-owner thread (same single-thread contract as s_g276Pend).
static bool g277CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G277_CENSUS");
    return s_on;
}
static bool g278DepthRbOn()
{
    // G278 PROMOTION CANDIDATE (2026-07-17): default-on while riding the already-oracle-gated
    // native wave stack. The G277 census remains behavior-pure by disabling this lever whenever
    // it is requested. Kill: DC2_G278_NO_Z_RB_COALESCE=1 (or DC2_G278_Z_RB_COALESCE=0).
    static const bool s_on = !envFlagEnabled("DC2_G278_NO_Z_RB_COALESCE") &&
                             !envFlagDisabled("DC2_G278_Z_RB_COALESCE");
    return s_on && g261WaveOn() && !g277CensusOn();
}
static G144BlockRange g278PendingDepthWholeRange();
static bool g278FlushPendingDepth(int cause);
static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown);
static std::atomic<uint64_t> g_g277FlushByCause[9] = {};          // g276 flushes that had pending
static std::atomic<uint64_t> g_g277EdgeCalls[9] = {}, g_g277EdgeSkippable[9] = {};
static std::atomic<uint64_t> g_g277InlEmpty{0}, g_g277InlNoOvl{0}, g_g277InlOvl{0}, g_g277InlZ{0};
static std::atomic<uint64_t> g_g277ZRb{0}, g_g277ZRbCoalescable{0};
static bool s_g277ZRunLive = false;   // flush-owner thread only
static uint64_t s_g277ZRunKey = 0u;
static G144BlockRange s_g277ZRunRange; // whole-target block range of the live run's Z key
static bool g274ZTimeOn()
{
    static const bool s_on = envFlagEnabled("DC2_G274_ZTIME");
    return s_on;
}
// G274 behavior lever (default-off): skip the redundant per-wave Z RE-UPLOAD when the persistent
// backend depth texture for this key is provably current (whole-target Z gen unchanged since our
// last writeback => resident GPU depth == VRAM Z, bit-exact by construction). VRAM stays
// authoritative (the readback still runs every write-wave), so there is NO deferred-ownership
// mechanism and the G242/G249 trap cannot occur. Rides the native stack; auto-off under the
// oracles that require the per-flush readback contract.
static std::atomic<uint64_t> g_g274ZUploadsSkipped{0};
static bool g274ZResidentOn()
{
    // G274 PROMOTION (2026-07-17): DEFAULT-ON. Elides the redundant per-wave Z RE-UPLOAD when the
    // whole-target Z gen is unchanged since our last writeback (resident backend depth == VRAM Z,
    // bit-exact). VRAM stays authoritative (the readback still runs every write-wave), so there is
    // NO deferred-ownership mechanism and the G242/G249 trap cannot occur. Requires the native
    // wave stack (g261WaveOn already declines under every exact oracle, the G242/G256 ownership
    // arms, the tile-bin/GPU kills, and the VRAM-dump diagnostics). Kill: DC2_G274_NO_ZRESIDENT=1
    // (or DC2_G274_ZRESIDENT=0); the whole-stack kill DC2_G26X_NO_NATIVE=1 also disables it.
    static const bool s_on = !envFlagEnabled("DC2_G274_NO_ZRESIDENT") &&
                             !envFlagDisabled("DC2_G274_ZRESIDENT");
    return s_on && g261WaveOn();
}

static bool g275FusedZReadOn()
{
    static const bool s_on = g261WaveOn() && envFlagEnabled("DC2_G275_FUSED_ZREAD");
    return s_on;
}

static bool g275StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G275_STAT");
    return s_on;
}

static int g275ZSwizzleLanes()
{
    static const int s_lanes = [] {
        // G275 PROMOTION (2026-07-17): DEFAULT 8. PSMZ24 is a one-to-one mapping from logical
        // pixels to independent 32-bit VRAM words, so disjoint guest-row lanes can pack/unpack in
        // parallel without changing RMW semantics or ownership timing. The existing persistent
        // row pool is joined before upload/submission or generation publication proceeds.
        if (envFlagEnabled("DC2_G275_NO_PAR_ZSWIZZLE"))
            return 1;
        const char *value = std::getenv("DC2_G275_ZSWIZZLE_LANES");
        if (value == nullptr)
            return 8;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    return g261WaveOn() ? s_lanes : 1;
}
// Per-key "backend depth texture has been fully uploaded once" (backend contract: a fresh
// texture's first upload must cover the whole target). Committed only after a successful submit.
static std::unordered_set<uint64_t> g_g262DepthInit;
static std::atomic<uint64_t> g_g261Mat[8] = {};
static std::atomic<uint64_t> g_g261MatByTarget[kG248TargetCount] = {};
// G279 diagnosis (default-off, DC2_G279_PROFILE=1): wall-time split for the remaining
// GPU-residency publication paths. Counts/rows are bucketed by the existing G261 cause enum;
// display publication uses the existing 0..8 consumer-edge causes. This is behavior-pure and
// cached so the normal renderer pays one predictable branch at each real materialization.
static std::atomic<uint64_t> g_g279MatNs[8] = {}, g_g279MatReadNs[8] = {},
    g_g279MatRows[8] = {};
static std::atomic<uint64_t> g_g279DisplayNs[9] = {}, g_g279DisplayReadNs[9] = {},
    g_g279DisplayN[9] = {};

static bool g279ProfileOn()
{
    static const bool s_on = envFlagEnabled("DC2_G279_PROFILE");
    return s_on;
}

static bool g261StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G261_STAT");
    return s_on;
}

static bool g267CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G267_CENSUS");
    return s_on;
}

static void g266Report(); // defined below g261AnyDirty (reads the G261 residency state)

// G291 (bring-up opt-in DC2_G291_ATLAS=1; kills DC2_G291_NO_ATLAS=1 / DC2_G291_ATLAS=0):
// persistent-residency consumer-completeness for the 0x139 work-block atlas. The G290-ON census
// measured the exact relocation: admitting the seeded fbp=0x139/fbw=8 batch keeps the whole
// atlas dirty-resident, and texture consumers (T8 materials at 0x34a0/0x3460/0x34e0, the T8
// 0x143<-0x13c view source) then force kG261MatTexAlias materializations of ~4x more rows
// (c5 3.7s -> 11.8s per run) — even though the game re-UPLOADS those exact texture pages every
// frame (3.9 MB/f) before sampling them, so guest VRAM is already current for the pages the
// consumers read. G291 tracks cumulative per-pixel upload re-coverage: a page whose pixels were
// FULLY re-covered by noted uploads since the last wave write has VRAM >= FBO by construction
// (the G264 mirror map records the exact CT32 byte->target-pixel map of every upload), so a
// texture/CLUT read overlapping ONLY such pages needs no materialization. Writers still publish
// through every existing edge; the skip applies to the READ-triggered materialize only, and
// fails closed on any page not provably current or any layout change. G292 audited the G280
// alias-view write paths and retained exact tile-coverage invalidation diagnostics, but the
// combined behavior remains forbidden: true multi-owner pages need a per-target authority token,
// which G283's page-global epoch does not provide. G291 implies the G290 seeded admission.
static bool g291AtlasOn()
{
    // G294 combined arm: the owner-token makes the g280 alias-view exact on multi-owner pages, so
    // G291's upload-re-coverage retire (excluded from G280 since G292) can now run ALONGSIDE it —
    // this is the consumer-chain retire that makes the atlas residency actually pay. g294 also
    // implies the retire (and, via g290ExactRowsOn, the seeded admission) so the whole paying arm
    // is one behavior lever: DC2_G294_OWNER_TOKEN=1.
    static const bool s_on = (envFlagEnabled("DC2_G291_ATLAS") || g294OwnerTokenRequested()) &&
                             !envFlagEnabled("DC2_G291_NO_ATLAS") &&
                             !envFlagDisabled("DC2_G291_ATLAS");
    return s_on && (!g280AliasOn() || g294OwnerTokenOn());
}
static bool g291StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G291_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g291TexSkips{0}, g_g291TexMats{0}, g_g291CurPagesSet{0},
    g_g291CurPagesClr{0}, g_g291LayoutResets{0};

// G292: any GPU-side alias copy makes this target tile potentially newer than guest VRAM.
// Clear only the exact CT32 64x32 page tile; unrelated uploaded pages in the same row retain
// their G291 proof. Overlay restoration later makes FBO==VRAM again, but leaving the tile clear
// is the conservative direction until a noted upload re-covers it.
static void g291ClearCopiedTile(int ti, uint32_t pageRow, uint32_t pageCol)
{
    if (!g291AtlasOn() || ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    G261Res &r = g_g261Res[ti];
    if (r.g291CovFbw == 0u)
        return;
    if (pageRow >= 16u || pageCol >= 8u)
    {
        r.g291Cov = {};
        r.g291CovFbw = 0u;
        g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t y0 = pageRow * 32u;
    for (uint32_t y = y0; y < y0 + 32u; ++y)
        r.g291Cov[y][pageCol] = 0u;
    g_g291CurPagesClr.fetch_add(64u * 32u, std::memory_order_relaxed);
}

static void g261Report()
{
    if (!g261StatOn() && !g279ProfileOn())
        return;
    if (g291StatOn())
        std::fprintf(stderr,
                     "[G291:stat] skips=%llu mats=%llu covSetPx=%llu covClrPx=%llu "
                     "layoutResets=%llu\n",
                     (unsigned long long)g_g291TexSkips.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291TexMats.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesSet.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesClr.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291LayoutResets.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G261:stat] waves=%llu rbSkip=%llu fboBind=%llu fboBindBi=%llu fboRej=%llu inv=%llu unk=%llu "
                 "mat(cpu=%llu inl=%llu up=%llu l2l=%llu l2h=%llu tex=%llu rsync=%llu fail=%llu) "
                 "mir(n=%llu f=%llu x=%llu) g267(cand=%llu elig=%llu foot=%llu alias=%llu)\n",
                 (unsigned long long)g_g261Waves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261SkippedRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboBinds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g263FboBindBi.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboRejects.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Invariants.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261MatUnknownRange.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirNoted.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFlushed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriCandidates.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriEligible.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriFootReject.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriAliasReject.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G261:res]");
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        std::fprintf(stderr, " %03x(mat=%llu win=%d..%d dirty=%d)", kG261Fbp[i],
                     (unsigned long long)g_g261MatByTarget[i].load(std::memory_order_relaxed),
                     g_g261Res[i].winLo, g_g261Res[i].winHi, g_g261Res[i].dirty ? 1 : 0);
    std::fprintf(stderr, " rttRej(classify=%llu depth=%llu) g262(zwave=%llu zrb=%llu)\n",
                 (unsigned long long)g_g261RttClassifyRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261RttDepthRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262DepthWaves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262ZReadbacks.load(std::memory_order_relaxed));
    g280Report();
    if (g274ZTimeOn())
        std::fprintf(stderr,
                     "[G274:ztime] zUpMs=%.1f zRbMs=%.1f redundant=%llu necessary=%llu "
                     "(cumulative swizzle round-trip; redundant=elidable by depth residency)\n",
                     (double)g_g274ZUpNs.load(std::memory_order_relaxed) / 1e6,
                     (double)g_g274ZRbNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274ZRedundant.load(std::memory_order_relaxed),
                     (unsigned long long)g_g274ZNecessary.load(std::memory_order_relaxed));
    if (g274ZTimeOn())
        std::fprintf(stderr, "[G274:submit] submitMs=%.1f submitN=%llu (synchronous backend waves)\n",
                     (double)g_g274SubNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274SubN.load(std::memory_order_relaxed));
    if (g274ZTimeOn() || g274ZResidentOn())
        std::fprintf(stderr, "[G274:zresident] uploadsSkipped=%llu (redundant Z re-uploads elided)\n",
                     (unsigned long long)g_g274ZUploadsSkipped.load(std::memory_order_relaxed));
    if (g275StatOn())
        std::fprintf(stderr,
                     "[G275:fused] jobs=%llu readWaitsElided=%llu submitReadMs=%.1f "
                     "zSwizzleLanes=%d\n",
                     (unsigned long long)g_g275FusedJobs.load(std::memory_order_relaxed),
                     (unsigned long long)g_g275ReadWaitsElided.load(std::memory_order_relaxed),
                     (double)g_g275FusedNs.load(std::memory_order_relaxed) / 1e6,
                     g275ZSwizzleLanes());
    if (g276ProbeOn())
    {
        auto ms = [](const std::atomic<uint64_t> &a) {
            return (double)a.load(std::memory_order_relaxed) / 1e6;
        };
        auto cnt = [](const std::atomic<uint64_t> &a) {
            return (unsigned long long)a.load(std::memory_order_relaxed);
        };
        std::fprintf(stderr,
                     "[G276:submit] rttSkip(depthW=%.1fms/%llu noZ=%.1fms/%llu) "
                     "readback(colorZ=%.1fms/%llu color=%.1fms/%llu)\n",
                     ms(g_g276SubNs[1][1]), cnt(g_g276SubN[1][1]),
                     ms(g_g276SubNs[1][0]), cnt(g_g276SubN[1][0]),
                     ms(g_g276SubNs[0][1]), cnt(g_g276SubN[0][1]),
                     ms(g_g276SubNs[0][0]), cnt(g_g276SubN[0][0]));
        const unsigned long long win = cnt(g_g276ColorRbWinRows);
        const unsigned long long full = cnt(g_g276ColorRbFullRows);
        std::fprintf(stderr,
                     "[G276:colorrb] n=%llu winRows=%llu fullRows=%llu span=%.1f%% "
                     "(rows actually swizzled vs full-FBO glReadPixels)\n",
                     cnt(g_g276ColorRbN), win, full,
                     full ? 100.0 * (double)win / (double)full : 0.0);
    }
    if (g276DisplayRbOn() || g276ProbeOn())
    {
        const unsigned long long def = g_g276Deferred.load(std::memory_order_relaxed);
        const unsigned long long fl = g_g276Flushes.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G276:coalesce] deferred=%llu flushes=%llu rows=%llu fail=%llu "
                     "readbacksSaved=%llu uploadPublish=%llu exactGenWin=%llu\n",
                     def, fl, (unsigned long long)g_g276FlushRows.load(std::memory_order_relaxed),
                     (unsigned long long)g_g276FlushFail.load(std::memory_order_relaxed),
                     def > fl ? def - fl : 0ull,
                     (unsigned long long)g_g277UploadPublish.load(std::memory_order_relaxed),
                     (unsigned long long)g_g277ExactGenWin.load(std::memory_order_relaxed));
    }
    if (g279ProfileOn())
    {
        std::fprintf(stderr, "[G279:mat]");
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llur", i,
                         (double)g_g279MatNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279MatReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279MatRows[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/rows)\n[G279:display]");
        for (int i = 0; i < 9; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llu", i,
                         (double)g_g279DisplayNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279DisplayReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279DisplayN[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/count)\n");
    }
    if (g284StatOn())
    {
        std::fprintf(stderr, "[G284:stat] uploadCoalesce=%d skipped=%llu flushed=%llu\n",
                     g284UploadCoalesceOn() ? 1 : 0,
                     (unsigned long long)g_g284UploadSkipped.load(std::memory_order_relaxed),
                     (unsigned long long)g_g284UploadFlushed.load(std::memory_order_relaxed));
    }
    if (g288StatOn())
    {
        std::fprintf(stderr,
                     "[G288:stat] audits=%llu publishes=%llu disjoint=%llu unknown=%llu\n",
                     (unsigned long long)g_g288GpuAudits.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuDisjoint.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuUnknown.load(std::memory_order_relaxed));
    }
    if (g289StatOn())
    {
        std::fprintf(stderr,
                     "[G289:stat] direct=%llu copy=%llu copyFail=%llu consume=%llu carry=%llu "
                     "view=%llu viewFail=%llu bind=%llu mat=%llu matFail=%llu unexpected=%llu "
                     "upOverwrite=%llu inlineCarry=%llu frameCarry=%llu suffixPub=%llu "
                     "verify=%llu bad=%llu "
                     "active=%u version=%llu owned=%x..%x\n",
                     (unsigned long long)g_g289Direct.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Copies.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289CopyFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Consumes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Carries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViews.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViewFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerBinds.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Materializes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289MatFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Unexpected.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289UploadOverwrites.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289InlineCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289FrameCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289SuffixPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyChecks.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyBad.load(std::memory_order_relaxed),
                     s_g289Owner.active ? 1u : 0u,
                     (unsigned long long)s_g289Owner.version,
                     s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock);
    }
    g262Report();
    g265Report();
    g266Report();
}

static bool g261AnyDirty()
{
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        if (g_g261Res[i].dirty)
            return true;
    return false;
}

// G266: a CPU band replay is about to materialize resident target `ti` because entry `e`
// overlaps it. hitBits: 1=color 2=depth 4=texture 8=CLUT 16=unknown-range.
static void g266NoteCpuMat(const G144Entry &e, uint32_t ti, uint32_t hitBits)
{
    if (!g266CensusOn())
        return;
    g_g266CpuN.fetch_add(1u, std::memory_order_relaxed);
    const GSContext &ctx = e.ctx;
    const bool zOn = ((ctx.test >> 16) & 1u) != 0u && ((ctx.zbuf >> 32) & 1ull) == 0ull;
    uint64_t k = 0;
    k |= static_cast<uint64_t>(ctx.frame.fbp & 0x1FFu);                               // 0-8
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 9;                              // 9-11
    k |= static_cast<uint64_t>(e.prim.tme ? 1u : 0u) << 12;                           // 12
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.psm & 0x3Fu) : 0x3Fu) << 13;    // 13-18
    k |= static_cast<uint64_t>(ctx.zbuf & 0x1FFu) << 19;                              // 19-27
    k |= static_cast<uint64_t>(zOn ? 1u : 0u) << 28;                                  // 28
    k |= static_cast<uint64_t>(hitBits & 0x1Fu) << 29;                                // 29-33
    k |= static_cast<uint64_t>(ti & 0x7u) << 34;                                      // 34-36
    k |= static_cast<uint64_t>(g_g266FlushRej & 0xFu) << 37;                          // 37-40
    k |= static_cast<uint64_t>((g_g266FlushRej == kG266FrClassify
                                    ? g_g266ClassifySite : 0xF) & 0xFu) << 41;        // 41-44
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.tbp0 & 0x3FFFu) : 0u) << 45;    // 45-58
    ++g_g266CpuShapes[k];
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
        std::fprintf(stderr,
                     "[G266:cpu] fbp=%03x prim=%u tme=%u tpsm=0x%x tbp=0x%x zbp=%03x zOn=%u "
                     "hit=0x%x t=%03x rej=%s site=%s\n",
                     ctx.frame.fbp, static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                     e.prim.tme ? ctx.tex0.psm : 0u, e.prim.tme ? ctx.tex0.tbp0 : 0u,
                     static_cast<unsigned>(ctx.zbuf & 0x1FFu), zOn ? 1u : 0u, hitBits,
                     kG261Fbp[ti], kG266FrName[g_g266FlushRej],
                     (g_g266FlushRej == kG266FrClassify && g_g266ClassifySite >= 0 &&
                      g_g266ClassifySite < kG262RejSiteCount)
                         ? kG262SiteName[g_g266ClassifySite]
                         : "-");
}

// G266: a local→local copy is about to drain resident targets. Record each target's exact role
// (source/destination/unknown-range) plus both transfer layouts — the G264 lesson says the fix
// (if any) must map addresses under BOTH swizzles, so the census must carry both.
static void g266NoteLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                              uint32_t ssax, uint32_t ssay,
                              uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                              uint32_t dsax, uint32_t dsay,
                              uint32_t rrw, uint32_t rrh,
                              const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown)
{
    if (!g266CensusOn() || !g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        const bool unknown = srcUnknown || dstUnknown || !r.range.valid;
        const bool hitSrc = !unknown && src.valid && g144RangeOverlaps(r.range, src);
        const bool hitDst = !unknown && dst.valid && g144RangeOverlaps(r.range, dst);
        if (!unknown && !hitSrc && !hitDst)
            continue; // this target survives the copy — not a drain
        g_g266L2lN.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t role = unknown ? 4u : ((hitSrc ? 1u : 0u) | (hitDst ? 2u : 0u));
        uint64_t k = 0;
        k |= static_cast<uint64_t>(sbp & 0x3FFFu);       // 0-13
        k |= static_cast<uint64_t>(spsm & 0x3Fu) << 14;  // 14-19
        k |= static_cast<uint64_t>(sbw & 0x3Fu) << 20;   // 20-25
        k |= static_cast<uint64_t>(dbp & 0x3FFFu) << 26; // 26-39
        k |= static_cast<uint64_t>(dpsm & 0x3Fu) << 40;  // 40-45
        k |= static_cast<uint64_t>(dbw & 0x3Fu) << 46;   // 46-51
        k |= static_cast<uint64_t>(i & 0x7u) << 52;      // 52-54
        k |= static_cast<uint64_t>(role & 0x7u) << 55;   // 55-57
        ++g_g266L2lShapes[k];
        static std::atomic<uint32_t> s_log{0};
        if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
            std::fprintf(stderr,
                         "[G266:l2l] s(bp=0x%x bw=%u psm=0x%x at %u,%u) d(bp=0x%x bw=%u psm=0x%x "
                         "at %u,%u) rect=%ux%u t=%03x role=%s dirty=%d..%d cov=%d..%d\n",
                         sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                         dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                         kG261Fbp[i],
                         role == 4u ? "unk" : role == 3u ? "src+dst"
                                            : role == 1u ? "src" : "dst",
                         r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    }
}

// G266: a GPU batch samples a resident target but the FBO-direct bind was rejected — record the
// consumer's exact state shape (the [G262:bindrej] print is capped; this aggregates all of them).
static void g266NoteTexBindMat(uint32_t dstFbp, uint32_t ti, const G144Entry &e,
                               const G178EntryState &st)
{
    if (!g266CensusOn())
        return;
    g_g266TexN.fetch_add(1u, std::memory_order_relaxed);
    uint64_t k = 0;
    k |= static_cast<uint64_t>(dstFbp & 0x1FFu);                 // 0-8
    k |= static_cast<uint64_t>(ti & 0x7u) << 9;                  // 9-11
    k |= static_cast<uint64_t>(e.ctx.tex0.psm & 0x3Fu) << 12;    // 12-17
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 18;        // 18-20
    k |= static_cast<uint64_t>(e.prim.fst ? 1u : 0u) << 21;      // 21
    k |= static_cast<uint64_t>(st.bilinear ? 1u : 0u) << 22;     // 22
    k |= static_cast<uint64_t>(st.wrapU & 0x3u) << 23;           // 23-24
    k |= static_cast<uint64_t>(st.wrapV & 0x3u) << 25;           // 25-26
    k |= static_cast<uint64_t>(e.ctx.tex0.tbp0 & 0x3FFFu) << 27; // 27-40
    k |= static_cast<uint64_t>(e.ctx.tex0.tbw & 0x3Fu) << 41;    // 41-46
    ++g_g266TexShapes[k];
}

static void g266Report()
{
    if (!g266CensusOn())
        return;
    std::fprintf(stderr, "[G266:census] cpu=%llu l2l=%llu tex=%llu shapes(%zu/%zu/%zu)\n",
                 (unsigned long long)g_g266CpuN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266L2lN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266TexN.load(std::memory_order_relaxed),
                 g_g266CpuShapes.size(), g_g266L2lShapes.size(), g_g266TexShapes.size());
    for (const auto &kv : g_g266CpuShapes)
    {
        const uint64_t k = kv.first;
        const int rej = static_cast<int>((k >> 37) & 0xFu);
        const int site = static_cast<int>((k >> 41) & 0xFu);
        std::fprintf(stderr,
                     "[G266:cpuShape] n=%llu fbp=%03x prim=%u tme=%u tpsm=0x%02x tbp=0x%x "
                     "zbp=%03x zOn=%u hit=0x%x t=%03x rej=%s site=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), (unsigned)((k >> 9) & 0x7u),
                     (unsigned)((k >> 12) & 1u), (unsigned)((k >> 13) & 0x3Fu),
                     (unsigned)((k >> 45) & 0x3FFFu), (unsigned)((k >> 19) & 0x1FFu),
                     (unsigned)((k >> 28) & 1u), (unsigned)((k >> 29) & 0x1Fu),
                     kG261Fbp[(k >> 34) & 0x7u],
                     kG266FrName[rej],
                     (rej == kG266FrClassify && site < kG262RejSiteCount)
                         ? kG262SiteName[site] : "-");
    }
    for (const auto &kv : g_g266L2lShapes)
    {
        const uint64_t k = kv.first;
        const uint32_t role = static_cast<uint32_t>((k >> 55) & 0x7u);
        std::fprintf(stderr,
                     "[G266:l2lShape] n=%llu s(bp=0x%x bw=%u psm=0x%02x) d(bp=0x%x bw=%u "
                     "psm=0x%02x) t=%03x role=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x3FFFu), (unsigned)((k >> 20) & 0x3Fu),
                     (unsigned)((k >> 14) & 0x3Fu),
                     (unsigned)((k >> 26) & 0x3FFFu), (unsigned)((k >> 46) & 0x3Fu),
                     (unsigned)((k >> 40) & 0x3Fu),
                     kG261Fbp[(k >> 52) & 0x7u],
                     role == 4u ? "unk" : role == 3u ? "src+dst"
                                        : role == 1u ? "src" : "dst");
    }
    for (const auto &kv : g_g266TexShapes)
    {
        const uint64_t k = kv.first;
        std::fprintf(stderr,
                     "[G266:texShape] n=%llu dstFbp=%03x t=%03x tpsm=0x%02x tbp=0x%x tbw=%u "
                     "prim=%u fst=%u bilin=%u wrap=%u%u\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), kG261Fbp[(k >> 9) & 0x7u],
                     (unsigned)((k >> 12) & 0x3Fu), (unsigned)((k >> 27) & 0x3FFFu),
                     (unsigned)((k >> 41) & 0x3Fu), (unsigned)((k >> 18) & 0x7u),
                     (unsigned)((k >> 21) & 1u), (unsigned)((k >> 22) & 1u),
                     (unsigned)((k >> 23) & 0x3u), (unsigned)((k >> 25) & 0x3u));
    }
}

// Recompute the claimed row window (cov ∪ dirty) and its block range + gen baseline. Rows
// OUTSIDE the window belong to VRAM alone (never uploaded to or read back from the FBO), so
// writes there are free — no materialize, no invariant. The gen baseline is scoped to the same
// window so unrelated same-fbp-page-range traffic cannot trip it falsely.
static void g261UpdateWindow(int ti)
{
    G261Res &r = g_g261Res[ti];
    int lo = r.covLo, hi = r.covHi;
    if (r.dirty)
    {
        lo = std::min(lo, r.dirtyLo);
        hi = std::max(hi, r.dirtyHi);
    }
    r.winLo = lo;
    r.winHi = hi;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[ti]);
    if (hi >= lo && r.fbW > 0)
    {
        r.range = g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                                   static_cast<uint32_t>(lo),
                                   static_cast<uint32_t>(r.fbW),
                                   static_cast<uint32_t>(hi - lo + 1));
        r.genAtSkip = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                                     static_cast<uint32_t>(lo),
                                     static_cast<uint32_t>(r.fbW),
                                     static_cast<uint32_t>(hi - lo + 1));
    }
    else
    {
        r.range = G144BlockRange{};
        r.genAtSkip = 0;
    }
}

static uint64_t g261WindowGenNow(const G261Res &r, uint32_t fbp)
{
    if (r.winHi < r.winLo || r.fbW <= 0)
        return 0;
    return g178GenSumRect(framePageBaseToBlock(fbp), r.fbw, GS_PSM_CT32, 0u,
                          static_cast<uint32_t>(r.winLo),
                          static_cast<uint32_t>(r.fbW),
                          static_cast<uint32_t>(r.winHi - r.winLo + 1));
}

// G264 (default-on since G266): route guest re-uploads INTO the resident FBO instead of
// materializing around them (pillar-3 GPU-resident upload; G261 blocker 2). The upload hook
// defers eligible rects to `mirPending` rows; this flush reads those rows from VRAM (the guest
// upload has completed — the GIF stream is serial) and glTexSubImage2Ds them into the FBO,
// keeping the residency alive across the guest's per-frame scratch-page upload cycle.
static bool g264On()
{
    // G266 promotion: default-on (only consulted on wave paths, so the wave/stack kills gate it
    // upstream). Kill: DC2_G264_NO_UP_FBO=1. DC2_G264_UP_FBO=1 stays accepted; =0 disables.
    static const bool s_on = !envFlagEnabled("DC2_G264_NO_UP_FBO") &&
                             !envFlagDisabled("DC2_G264_UP_FBO");
    return s_on;
}

// Push pending mirror rows VRAM→FBO. Returns false on backend failure, in which case the FBO
// no longer matches guest intent for those rows — the CALLER must drop the residency WITHOUT a
// readback (a readback would clobber the newer VRAM upload bytes with stale FBO content).
static bool g264FlushMirror(int ti)
{
    G261Res &r = g_g261Res[ti];
    if (!r.mirPending)
        return true;
    const int lo = r.mirLo, hi = r.mirHi;
    const uint32_t fbp = kG261Fbp[ti];
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange mirrorRange =
        (hi >= lo && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(std::max(0, lo)),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(hi - lo + 1))
            : G144BlockRange{};
    if (!g278FlushPendingDepthForRanges(
            1, mirrorRange, G144BlockRange{},
            hi >= lo && r.fbW > 0 && !mirrorRange.valid, false))
        return false; // deferred Z could not be published; do not decode stale VRAM into the FBO
    r.mirPending = false;
    r.mirLo = 512;
    r.mirHi = -1;
    uint8_t *vram = g_g144LastVram;
    auto clearBits = [&]() {
        if (hi >= lo)
            for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
                r.mirBits[static_cast<size_t>(y)].fill(0u);
    };
    if (vram == nullptr || hi < lo || lo < 0 || hi >= 512 ||
        r.fbW <= 0 || r.fbW > 512 || g_g261VramSize == 0u)
    {
        clearBits();
        return false;
    }
    static std::vector<uint32_t> s_g264Px; // single-threaded (flush-owner threads only)
    bool anyPatch = false;
    int patchLo = 512, patchHi = -1;
    uint32_t patchRects = 0u;
    for (int y = lo; y <= hi;)
    {
        const auto &rowMask = r.mirBits[static_cast<size_t>(y)];
        bool rowEmpty = true;
        for (uint64_t word : rowMask)
            rowEmpty = rowEmpty && (word == 0u);
        if (rowEmpty)
        {
            ++y;
            continue;
        }

        // Adjacent rows with the same target-pixel mask become one glTexSubImage2D rectangle
        // per horizontal run. This keeps the common full-page aliases to 1-2 queue jobs while
        // remaining exact for arbitrary CT32 swizzle aliases and disjoint pending uploads.
        int groupHi = y;
        while (groupHi < hi &&
               r.mirBits[static_cast<size_t>(groupHi + 1)] == rowMask)
            ++groupHi;
        const int rows = groupHi - y + 1;
        for (int x = 0; x < r.fbW;)
        {
            const auto bitSet = [&](int px) {
                return (rowMask[static_cast<size_t>(px >> 6)] &
                        (uint64_t{1} << (px & 63))) != 0u;
            };
            while (x < r.fbW && !bitSet(x))
                ++x;
            if (x >= r.fbW)
                break;
            const int x0 = x;
            while (x < r.fbW && bitSet(x))
                ++x;
            const int width = x - x0;
            const int glY = 511 - groupHi;
            s_g264Px.assign(static_cast<size_t>(width) * rows, 0u);
            bool packed = true;
            for (int gy = y; gy <= groupHi && packed; ++gy)
            {
                // GL input rows are bottom-first: highest GS y is the first buffer row.
                const size_t dstRow = static_cast<size_t>(groupHi - gy) * width;
                for (int gx = x0; gx < x0 + width; ++gx)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, r.fbw, static_cast<uint32_t>(gx), static_cast<uint32_t>(gy));
                    if (off + 4u > g_g261VramSize)
                    {
                        packed = false;
                        break;
                    }
                    std::memcpy(&s_g264Px[dstRow + static_cast<size_t>(gx - x0)],
                                vram + off, 4);
                }
            }
            if (!packed ||
                !g264_backend_write_color_rect(fbp, r.fbW, 512, x0, glY,
                                                width, rows, s_g264Px))
            {
                clearBits();
                g_g264MirFail.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[G264:mirror-fail] fbp=0x%x rect=%d,%d %dx%d fbW=%d packed=%u\n",
                             fbp, x0, y, width, rows, r.fbW, packed ? 1u : 0u);
                return false;
            }
            anyPatch = true;
            ++patchRects;
            patchLo = std::min(patchLo, y);
            patchHi = std::max(patchHi, groupHi);
        }
        y = groupHi + 1;
    }
    clearBits();
    if (!anyPatch)
        return false;
    if (patchLo < r.covLo) r.covLo = patchLo;
    if (patchHi > r.covHi) r.covHi = patchHi;
    g261UpdateWindow(ti); // re-anchor claimed rows + gen baseline past the upload's own bumps
    // FBO == VRAM again for the mirrored rows; keep the snapshot coherent if one is live.
    G178FbSnap &snap = g_g178FbSnap[fbp];
    if (snap.valid)
    {
        if (lo < snap.rowLo) snap.rowLo = lo;
        if (hi > snap.rowHi) snap.rowHi = hi;
        snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                     static_cast<uint32_t>(r.fbW), 512u);
    }
    if (g262CensusOn())
    {
        static std::atomic<uint32_t> s_g264PatchLog{0};
        if (s_g264PatchLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr, "[G264:patch] fbp=0x%x rows=%d..%d rects=%u fbW=%d\n",
                         fbp, patchLo, patchHi, patchRects, r.fbW);
    }
    g_g264MirFlushed.fetch_add(1u, std::memory_order_relaxed);
    // G280: the mirror wrote VRAM-authoritative bytes into these FBO rows — re-sync + drop any
    // alias-overlay tiles they touch and stale every overlay copied FROM this target.
    g280NoteMirrorPatch(ti, patchLo, patchHi);
    return true;
}

// G264: flush every pending upload mirror before a GPU flush touches classify/invariant/bind
// state. A mirror failure means the FBO is stale for rows VRAM now owns — drop that residency
// WITHOUT a readback (loud, bounded; the fresh VRAM upload bytes must win).
static void g264FlushMirrorsAtFlush()
{
    if (!g264On())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.mirPending)
            continue;
        if (!g264FlushMirror(static_cast<int>(i)))
        {
            g280DropOverlays(static_cast<int>(i)); // residency dies; nothing publishes them
            r.dirty = false;
            r.dirtyLo = 512;
            r.dirtyHi = -1;
            g261UpdateWindow(static_cast<int>(i));
            G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
            snap.valid = false;
            snap.genSum = 0;
            snap.rowLo = 1 << 30;
            snap.rowHi = -1;
        }
    }
}

// Read the dirty row window back from the target's FBO into guest VRAM, advance the page write
// generations (this is when VRAM bytes actually change), and refresh the fb snapshot so the next
// GPU flush of this target does not re-upload bytes the FBO already equals. On ANY failure the
// residency is dropped loudly with VRAM left as-is (bounded stale-pixel glitch, never a hang or
// a silent overwrite of newer guest bytes).
static void g261Materialize(int ti, int cause)
{
    G261Res &r = g_g261Res[ti];
    if (!r.dirty)
    {
        // Not dirty but a mirror may still be pending (e.g. noted just before an invariant
        // drop): flush it so the FBO does not keep stale rows for a later wave.
        if (r.mirPending)
            g264FlushMirror(ti);
        return;
    }
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    const uint32_t fbp = kG261Fbp[ti];
    const int rows = r.dirtyHi - r.dirtyLo + 1;
    uint8_t *vram = g_g144LastVram;
    bool ok = false;
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange dirtyRange =
        (rows > 0 && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(r.dirtyLo),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(rows))
            : G144BlockRange{};
    const bool depthPublished = g278FlushPendingDepthForRanges(
        1, G144BlockRange{}, dirtyRange, false,
        rows > 0 && r.fbW > 0 && !dirtyRange.valid);
    static std::vector<uint32_t> s_g261Px; // single-threaded (flush-owner threads only)
    // G264: bring the FBO current with any pending upload mirror FIRST — the readback below
    // must not clobber newer VRAM upload bytes with pre-upload FBO content. Mirror failure ⇒
    // ok stays false ⇒ the loud drop path below runs with VRAM untouched (upload preserved).
    // G280: alias-overlay tiles are re-synced from VRAM (and dropped) BEFORE the readback so
    // this publication never writes alias-page bytes the dst view does not own; a restore
    // failure takes the same loud no-readback drop path as a mirror failure.
    if (depthPublished && g280PrepareMaterialize(ti) && g264FlushMirror(ti) &&
        vram != nullptr && rows > 0 && r.fbW > 0 && g_g261VramSize != 0u)
    {
        const int glY = 512 - 1 - r.dirtyHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const bool readOk = g178_backend_read_color(fbp, r.fbW, 512, glY, rows, s_g261Px);
        if (g279Profile)
            g_g279MatReadNs[cause & 7].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g261Px.size() == static_cast<size_t>(r.fbW) * rows;
        if (ok)
        {
            for (int y = r.dirtyLo; y <= r.dirtyHi; ++y)
            {
                // s_g261Px rows are GL window rows starting at glY (bottom-first):
                // GS row y ↔ GL row (511-y) ↔ buffer row (511-y-glY).
                const size_t srcRow = static_cast<size_t>(511 - y - glY) * r.fbW;
                for (int x = 0; x < r.fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw,
                                                                static_cast<uint32_t>(x),
                                                                static_cast<uint32_t>(y));
                    if (off + 4u <= g_g261VramSize)
                        std::memcpy(vram + off, &s_g261Px[srcRow + x], 4);
                }
            }
            g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW), static_cast<uint32_t>(rows));
            // FBO still equals VRAM for this target: refresh the snapshot past our own bump.
            G178FbSnap &snap = g_g178FbSnap[fbp];
            snap.valid = true;
            if (r.covLo < snap.rowLo) snap.rowLo = r.covLo;
            if (r.covHi > snap.rowHi) snap.rowHi = r.covHi;
            snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                         static_cast<uint32_t>(r.fbW), 512u);
        }
    }
    if (!ok)
    {
        std::fprintf(stderr,
                     "[G261:materialize-fail] fbp=0x%x rows=%d..%d fbW=%d cause=%d — residency "
                     "dropped, VRAM stays authoritative\n",
                     fbp, r.dirtyLo, r.dirtyHi, r.fbW, cause);
        g_g261Mat[kG261MatFail].fetch_add(1u, std::memory_order_relaxed);
        // FBO↔VRAM equality is unknown now — force a fresh upload. Reset in place (callers may
        // hold live references into g_g178FbSnap; erase would invalidate them).
        G178FbSnap &failSnap = g_g178FbSnap[fbp];
        failSnap.valid = false;
        failSnap.genSum = 0;
        failSnap.rowLo = 1 << 30;
        failSnap.rowHi = -1;
    }
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    g261UpdateWindow(ti); // window shrinks to cov rows; gen baseline re-anchored past our bump
    g_g261MatByTarget[ti & 7].fetch_add(1u, std::memory_order_relaxed);
    if (g279Profile)
    {
        g_g279MatNs[cause & 7].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279MatRows[cause & 7].fetch_add(
            static_cast<uint64_t>(std::max(rows, 0)), std::memory_order_relaxed);
    }
    const uint64_t n = g_g261Mat[cause & 7].fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 512u) == 0u)
        g261Report();
}

// ---- G276: within-drain display-color readback coalescing -------------------------------------
// Steady MAP-0 pays ~9 full-FBO color glReadPixels/frame (~30 ms/f, ~13% of the frame): each
// display batch (fbp 0x0/0x68) blocks on its own readback even when the very next batch draws into
// the SAME persistent FBO with no intervening VRAM-color consumer. The FBO accumulates across those
// batches, and the gen-snapshot already suppresses a re-upload for a same-target continuation (the
// readback does not bump color gens), so a consecutive same-target run needs exactly ONE readback,
// not K. G276 defers the readback (renderBatch skips the glReadPixels; the draw still lands in the
// FBO), records the pending union, and publishes it (one row-windowed g178_backend_read_color +
// swizzle -> VRAM) at the next different target OR any g272MaterializeAll consumer edge (GPU drain,
// CPU replay, inline primitive, drain end, l2h/l2l upload) — the same edge set G272 established.
// Scope is WITHIN A DRAIN (no persistent cross-frame residency), so the G242/G249/G272 cross-frame
// ownership trap cannot occur; VRAM is authoritative at exactly the boundaries it is today.
// (Lever g276DisplayRbOn(), the G276PendDisplay state, and its counters are declared earlier so
// g261Report can print them; only the flush body lives here where its readback helpers are visible.)

// Publish the pending coalesced display readback (union rows) into guest VRAM. Mirrors the per-batch
// color writeback exactly: row-windowed FBO readback, swizzle to VRAM, and NO color-gen bump (so the
// snapshot for a later same-target batch still equals current gens and re-upload stays suppressed —
// identical to today's per-batch display path). On any failure the snapshot is invalidated so the
// next flush re-uploads from VRAM (bounded stale-pixel, never a silent overwrite of newer bytes).
// Defined at global scope (region 4683..7342), static/internal linkage; called only from sites that
// are lexically after this point (the different-target site in g178TryFlushGpu and each
// g272MaterializeAll consumer-edge call site), so no forward declaration is needed.
static void g276FlushPendingDisplay(int cause)
{
    if (!s_g276Pend.active)
        return;
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    // Preserve the historical per-batch publication order when color and Z physically alias:
    // G262 wrote Z back before color. Range-disjoint owners remain independently coalescable.
    const G144BlockRange displayRange = g144RangeForRect(
        s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
        static_cast<uint32_t>(s_g276Pend.rowLo),
        static_cast<uint32_t>(s_g276Pend.fbW),
        static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
    if (s_g278Pend.active)
    {
        const G144BlockRange depthWhole = g278PendingDepthWholeRange();
        if (!depthWhole.valid || !displayRange.valid ||
            g144RangeOverlaps(depthWhole, displayRange))
        {
            if (!g278FlushPendingDepth(cause))
                return;
        }
    }
    if (!g278FlushPendingDepthForRanges(
            cause, G144BlockRange{}, displayRange, false, !displayRange.valid))
        return;
    if (g277CensusOn() && cause >= 0 && cause < 9)
        g_g277FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    G276PendDisplay p = s_g276Pend;
    s_g276Pend.active = false;
    s_g276Pend.rowLo = 1 << 30;
    s_g276Pend.rowHi = -1;
    uint8_t *vram = g_g144LastVram;
    const uint32_t vramSize = g_g261VramSize;
    const int rows = p.rowHi - p.rowLo + 1;
    bool ok = false;
    static std::vector<uint32_t> s_g276Px; // single-threaded flush-owner path (as g261Materialize)
    if (vram != nullptr && vramSize != 0u && p.fbW > 0 && rows > 0)
    {
        const int glY = 512 - 1 - p.rowHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const bool readOk = g178_backend_read_color(p.fbp, p.fbW, 512, glY, rows, s_g276Px);
        if (g279Profile && cause >= 0 && cause < 9)
            g_g279DisplayReadNs[cause].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g276Px.size() == static_cast<size_t>(p.fbW) * rows;
        if (ok)
        {
            for (int y = p.rowLo; y <= p.rowHi; ++y)
            {
                const size_t srcRow = static_cast<size_t>(511 - y - glY) * p.fbW;
                for (int x = 0; x < p.fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(p.fbBlock, p.fbw,
                                                                static_cast<uint32_t>(x),
                                                                static_cast<uint32_t>(y));
                    if (off + 4u <= vramSize)
                        std::memcpy(vram + off, &s_g276Px[srcRow + x], 4);
                }
            }
        }
    }
    if (ok)
    {
        g_g276Flushes.fetch_add(1u, std::memory_order_relaxed);
        g_g276FlushRows.fetch_add(static_cast<uint64_t>(rows), std::memory_order_relaxed);
    }
    else
    {
        // FBO<->VRAM relation now unknown for this target — force a fresh upload next flush.
        g_g276FlushFail.fetch_add(1u, std::memory_order_relaxed);
        G178FbSnap &snap = g_g178FbSnap[p.fbp];
        snap.valid = false;
        snap.genSum = 0;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    if (g279Profile && cause >= 0 && cause < 9)
    {
        g_g279DisplayNs[cause].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279DisplayN[cause].fetch_add(1u, std::memory_order_relaxed);
    }
}

// ---- G277 census helpers (counters only, DC2_G277_CENSUS=1; state by s_g276Pend) ---------------
// Block range of the CURRENT pending coalesced display readback (invalid when none).
static G144BlockRange g277PendingDisplayRange()
{
    if (!s_g276Pend.active || s_g276Pend.fbW <= 0 || s_g276Pend.rowHi < s_g276Pend.rowLo)
        return {};
    return g144RangeForRect(s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
                            static_cast<uint32_t>(s_g276Pend.rowLo),
                            static_cast<uint32_t>(s_g276Pend.fbW),
                            static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
}

// G284: publish the pending coalesced display readback at a RANGED transfer edge (cause 7 l2h dst,
// cause 8 l2l src+dst) only if that transfer could observe or overwrite the pending display rows.
// Lever-off, unknown range, degenerate pending, or overlap => the original unconditional G276 flush
// (bit-identical to pre-G284). A provably-disjoint transfer keeps the display readback coalesced for
// the next REAL consumer edge — exactly how G278 already gates the depth readback at this same edge.
static void g276FlushPendingDisplayForRanges(int cause, const G144BlockRange &src,
                                             const G144BlockRange &dst, bool srcUnknown,
                                             bool dstUnknown)
{
    if (!s_g276Pend.active)
        return;
    if (g284UploadCoalesceOn() && !srcUnknown && !dstUnknown)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const bool overlaps = (src.valid && g144RangeOverlaps(pend, src)) ||
                              (dst.valid && g144RangeOverlaps(pend, dst));
        if (pend.valid && !overlaps)
        {
            if (g284StatOn())
                g_g284UploadSkipped.fetch_add(1u, std::memory_order_relaxed);
            return; // disjoint: keep the display readback coalesced
        }
    }
    if (g284StatOn())
        g_g284UploadFlushed.fetch_add(1u, std::memory_order_relaxed);
    g276FlushPendingDisplay(cause);
}

// G288: consumer-complete bridge for a G284-deferred display owner into the next native GPU
// batch. g178TryFlushGpu calls this before framebuffer upload, texture/CLUT decode, or depth setup.
// Same-target color writes deliberately continue in the persistent FBO (the proven G276 path);
// every other physical read/write range is checked against the unpublished display rows.
static void g288PublishDisplayForGpuBatch()
{
    if (!g288GpuConsumerOn() || !s_g276Pend.active || g_g144List.empty())
        return;

    g_g288GpuAudits.fetch_add(1u, std::memory_order_relaxed);
    const G144BlockRange pending = g277PendingDisplayRange();
    bool overlap = false;
    bool unknown = !pending.valid;
    uint32_t reason = unknown ? 0x10u : 0u; // 1=color, 2=depth, 4=texture, 8=CLUT, 16=unknown
    size_t hitEntry = 0u;

    for (size_t i = 0; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool sameDisplay =
            e.ctx.frame.fbp == s_g276Pend.fbp && e.ctx.frame.fbw == s_g276Pend.fbw;

        if (!sameDisplay)
        {
            const G144BlockRange color = g144TargetRange(e);
            const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                                  e.ctx.scissor.y1 >= e.ctx.scissor.y0;
            if (nonEmpty && !color.valid)
            {
                unknown = true;
                reason |= 0x11u;
            }
            else if (color.valid && g144RangeOverlaps(pending, color))
            {
                overlap = true;
                reason |= 0x1u;
            }
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(pending, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(pending, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(pending, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }

    if (!overlap && !unknown)
    {
        g_g288GpuDisjoint.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if (unknown)
        g_g288GpuUnknown.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t n =
        g_g288GpuPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g288StatOn() && (n <= 64u || (n % 256u) == 0u))
    {
        const G144Entry &e = g_g144List[std::min(hitEntry, g_g144List.size() - 1u)];
        std::fprintf(stderr,
                     "[G288:gpu-consumer] n=%llu reason=0x%x entry=%zu/%zu "
                     "pend(fbp=0x%x fbw=%u rows=%d..%d blocks=%x..%x) "
                     "draw(fbp=0x%x fbw=%u tme=%u tbp=0x%x psm=0x%x cbp=0x%x)\n",
                     static_cast<unsigned long long>(n), reason, hitEntry, g_g144List.size(),
                     s_g276Pend.fbp, s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                     pending.first, pending.last, e.ctx.frame.fbp, e.ctx.frame.fbw,
                     e.prim.tme ? 1u : 0u, e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.cbp);
    }
    g276FlushPendingDisplay(0);
}

// Private .cpp-to-.cpp bridge; keeping G289 out of the shared LLE header avoids widening the
// backend surface for one tightly admitted owner.
extern bool g289_backend_copy_display_to_work(uint32_t srcFbp, uint32_t dstFbp);
extern bool g289_backend_build_work_alias_view();
static bool g289Resolve14aBeforeDirect();

// The exact display-grab sprite previously proven by G288's direct-source prototype. The one-row
// bilinear halo is safe because CLAMP repeats GS row 415; no undefined FBO row is sampled.
static uint32_t g289DirectDisplayToken()
{
    if (!g289LocalCopyOn() || !s_g276Pend.active ||
        s_g276Pend.fbW != 512 || s_g276Pend.fbw != 8u ||
        s_g276Pend.rowLo != 0 || s_g276Pend.rowHi != 415 ||
        (s_g276Pend.fbp != 0u && s_g276Pend.fbp != 0x68u) ||
        g_g144List.size() != 1u)
        return 0u;

    const G144Entry &e = g_g144List[0];
    const uint32_t sourceBlock = framePageBaseToBlock(s_g276Pend.fbp);
    const bool exact =
        e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 447u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && e.prim.fst &&
        e.ctx.tex0.tbp0 == sourceBlock && e.ctx.tex0.tbw == 8u &&
        e.ctx.tex0.psm == GS_PSM_CT32 &&
        e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u &&
        tex1UsesLinearFilter(e.ctx.tex1) &&
        e.v[0].u == 0u && e.v[0].v == 0u &&
        e.v[1].u == 512u * 16u && e.v[1].v == 416u * 16u;
    if (!exact)
        return 0u;
    if (s_g289Owner.active && !g289Resolve14aBeforeDirect())
        return 0u;
    return s_g276Pend.fbp == 0u ? kG289ZeroFbpToken : s_g276Pend.fbp;
}

static G144BlockRange g289OwnerRange()
{
    G144BlockRange out;
    out.first = s_g289Owner.firstOwnedBlock;
    out.last = s_g289Owner.lastOwnedBlock;
    out.valid = s_g289Owner.active && out.first <= out.last;
    return out;
}

static bool g289MaterializeOwner(int cause)
{
    if (!s_g289Owner.active)
        return true;
    G289CopyOwner owner = s_g289Owner;
    s_g289Owner.active = false;
    s_g289PendingUpload.active = false;

    static std::vector<uint32_t> s_px;
    const bool ok =
        owner.vram != nullptr && owner.vramSize != 0u &&
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 416, s_px) &&
        s_px.size() == 512u * 416u;
    if (ok)
    {
        const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
        for (uint32_t y = 0u; y < 416u; ++y)
        {
            const size_t srcRow = static_cast<size_t>(415u - y) * 512u;
            for (uint32_t x = 0u; x < 512u; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
                const uint32_t block = off >> 8u;
                if (block >= owner.firstOwnedBlock && block <= owner.lastOwnedBlock &&
                    off + 4u <= owner.vramSize)
                    std::memcpy(owner.vram + off, &s_px[srcRow + x], 4u);
            }
        }
        g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 0u, 512u, 416u);
        g_g289Materializes.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        g_g289MatFail.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G289:materialize-fail] version=%llu cause=%d vram=%p size=%u\n",
                     static_cast<unsigned long long>(owner.version), cause,
                     static_cast<void *>(owner.vram), owner.vramSize);
    }
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    if (g289StatOn())
    {
        const uint64_t n =
            g_g289Materializes.load(std::memory_order_relaxed) +
            g_g289MatFail.load(std::memory_order_relaxed);
        if (n <= 8u || (n % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:materialize] n=%llu version=%llu dst=%03x cause=%d ok=%u\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(owner.version), owner.dstFbp,
                         cause, ok ? 1u : 0u);
    }
    return ok;
}

// The measured 512x384 PSMT4 uploads and following CT32 page uploads replace complete physical
// pages at the front of the 0x14a owner. When one intersects that prefix without a gap, guest VRAM
// owns the replaced pages after IMAGE completes and the FBO keeps owning the untouched suffix.
static bool g289PrepareUploadOverwrite(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                       uint32_t dsax, uint32_t dsay,
                                       uint32_t rrw, uint32_t rrh,
                                       const G144BlockRange &dst)
{
    const bool exactT4 =
        dbw == 8u && dpsm == GS_PSM_T4 &&
        dsax == 0u && dsay == 0u && rrw == 512u && rrh == 384u;
    const bool exactCt32 =
        dbw == 2u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 128u && rrh == 128u;
    const bool exactCt32Tail =
        dbw == 1u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 64u && rrh == 64u;
    if (!s_g289Owner.active || s_g289Owner.dstFbp != 0x14au ||
        (!exactT4 && !exactCt32 && !exactCt32Tail) ||
        !dst.valid || dst.first > s_g289Owner.firstOwnedBlock ||
        dst.last < s_g289Owner.firstOwnedBlock ||
        s_g289Owner.lastOwnedBlock != 0x363fu)
        return false;
    s_g289PendingUpload.active = true;
    s_g289PendingUpload.ownerVersion = s_g289Owner.version;
    s_g289PendingUpload.dbp = dbp;
    s_g289PendingUpload.dbw = dbw;
    s_g289PendingUpload.dpsm = dpsm;
    s_g289PendingUpload.dsax = dsax;
    s_g289PendingUpload.dsay = dsay;
    s_g289PendingUpload.rrw = rrw;
    s_g289PendingUpload.rrh = rrh;
    s_g289PendingUpload.replacedThroughBlock = dst.last;
    return true;
}

// Called after GS::processImageData has written the complete exact image.
void g289NoteUploadComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                            uint32_t dsax, uint32_t dsay,
                            uint32_t rrw, uint32_t rrh)
{
    if (!s_g289PendingUpload.active)
        return;
    const bool exact =
        s_g289Owner.active &&
        s_g289Owner.version == s_g289PendingUpload.ownerVersion &&
        s_g289Owner.dstFbp == 0x14au &&
        dbp == s_g289PendingUpload.dbp &&
        dbw == s_g289PendingUpload.dbw &&
        dpsm == s_g289PendingUpload.dpsm &&
        dsax == s_g289PendingUpload.dsax &&
        dsay == s_g289PendingUpload.dsay &&
        rrw == s_g289PendingUpload.rrw &&
        rrh == s_g289PendingUpload.rrh;
    if (!exact)
    {
        s_g289PendingUpload.active = false;
        return;
    }
    s_g289Owner.firstOwnedBlock =
        std::max(s_g289Owner.firstOwnedBlock,
                 s_g289PendingUpload.replacedThroughBlock + 1u);
    s_g289PendingUpload.active = false;
    const bool fullyReplaced =
        s_g289Owner.firstOwnedBlock > s_g289Owner.lastOwnedBlock;
    if (fullyReplaced)
    {
        s_g289Owner.active = false;
        G178FbSnap &snap = g_g178FbSnap[s_g289Owner.dstFbp];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    const uint64_t n =
        g_g289UploadOverwrites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:upload-overwrite] n=%llu version=%llu "
                     "upload=%x..%x owner=%x..%x full=%u\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(s_g289Owner.version),
                     dbp, s_g289PendingUpload.replacedThroughBlock,
                     s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock,
                     fullyReplaced ? 1u : 0u);
}

// The following exact fbp=0x139 display sprite overwrites physical pages 0x2720..0x341f.
// For the partially overwritten 0x14a owner, only page 0x3420 onward can survive. Publishing
// GS rows 320..415 covers that complete suffix (plus harmless bounded over-work), after which
// the normal direct sprite may execute with no unresolved alias owner.
static bool g289Resolve14aBeforeDirect()
{
    if (!s_g289Owner.active || s_g289Owner.dstFbp != 0x14au ||
        s_g289Owner.firstOwnedBlock < 0x29a0u ||
        s_g289Owner.firstOwnedBlock > s_g289Owner.lastOwnedBlock ||
        s_g289Owner.lastOwnedBlock != 0x363fu ||
        s_g289Owner.vram == nullptr || s_g289Owner.vramSize == 0u)
        return false;

    G289CopyOwner owner = s_g289Owner;
    static std::vector<uint32_t> s_px;
    const bool readOk =
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 96, s_px) &&
        s_px.size() == 512u * 96u;
    if (!readOk)
        return g289MaterializeOwner(12);

    const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
    for (uint32_t y = 320u; y < 416u; ++y)
    {
        const size_t srcRow = static_cast<size_t>(415u - y) * 512u;
        for (uint32_t x = 0u; x < 512u; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
            const uint32_t block = off >> 8u;
            if (block >= owner.firstOwnedBlock && block <= owner.lastOwnedBlock &&
                off + 4u <= owner.vramSize)
                std::memcpy(owner.vram + off, &s_px[srcRow + x], 4u);
        }
    }
    g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 320u, 512u, 96u);
    s_g289Owner.active = false;
    s_g289PendingUpload.active = false;
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    const uint64_t n =
        g_g289SuffixPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:suffix-publish] n=%llu version=%llu dst=14a rows=320..415\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(owner.version));
    return true;
}

static void g289MaterializeForRanges(const G144BlockRange &src,
                                     const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (!s_g289Owner.active)
        return;
    const G144BlockRange owner = g289OwnerRange();
    if (srcUnknown || dstUnknown || !owner.valid ||
        (src.valid && g144RangeOverlaps(owner, src)) ||
        (dst.valid && g144RangeOverlaps(owner, dst)))
        (void)g289MaterializeOwner(cause);
}

// Private .cpp-to-.cpp presentation bridge. Normal presentation may read the display frames,
// preferred source, or a non-black context fallback. Audit the exact source rectangle at the
// point of read; an overlap or layout we cannot prove materializes before the host copy.
void g289_prepare_presentation_read(uint8_t *vram,
                                    uint32_t fbp, uint32_t fbw, uint32_t psm,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    bool useLocalMemoryLayout, bool frameBaseIsPages)
{
    if (!s_g289Owner.active || w == 0u || h == 0u)
        return;
    if (vram != s_g289Owner.vram || !useLocalMemoryLayout)
    {
        (void)g289MaterializeOwner(13);
        return;
    }
    const uint32_t baseBlock = frameBaseIsPages ? framePageBaseToBlock(fbp) : fbp;
    const G144BlockRange read =
        g144RangeForRect(baseBlock, static_cast<uint8_t>(psm),
                         fbw != 0u ? fbw : 1u, x, y, w, h);
    g289MaterializeForRanges(read, G144BlockRange{}, !read.valid, false, 13);
}

struct G289InlineAudit
{
    bool carry = false;
    bool empty = false;
    bool unknown = false;
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    G144BlockRange color{}, depth{}, texture{}, clut{};
};

// Range-exact inline consumer edge. A clipped-out primitive touches nothing; otherwise the
// scissor-clipped target/depth bbox and whole texture/CLUT footprints must all provably miss the
// surviving owner. Unknown ranges and every overlap retain fail-closed cause-4 materialization.
static G289InlineAudit g289AuditInline(const GSPrimReg &prim, const GSVertex *verts,
                                       const GSContext &ctx, const GSTexClutReg &texclut)
{
    G289InlineAudit out;
    int count = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: count = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: count = 2; break;
    default: count = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minX = verts[0].x, maxX = verts[0].x;
    float minY = verts[0].y, maxY = verts[0].y;
    for (int i = 1; i < count; ++i)
    {
        minX = std::min(minX, verts[i].x);
        maxX = std::max(maxX, verts[i].x);
        minY = std::min(minY, verts[i].y);
        maxY = std::max(maxY, verts[i].y);
    }
    out.x0 = std::max(static_cast<int>(std::floor(minX)) - ofx,
                      static_cast<int>(ctx.scissor.x0));
    out.x1 = std::min(static_cast<int>(std::ceil(maxX)) - ofx,
                      static_cast<int>(ctx.scissor.x1));
    out.y0 = std::max(static_cast<int>(std::floor(minY)) - ofy,
                      static_cast<int>(ctx.scissor.y0));
    out.y1 = std::min(static_cast<int>(std::ceil(maxY)) - ofy,
                      static_cast<int>(ctx.scissor.y1));
    if (out.x1 < out.x0 || out.y1 < out.y0)
    {
        out.empty = true;
        out.carry = true;
        return out;
    }

    const uint32_t x = static_cast<uint32_t>(out.x0);
    const uint32_t y = static_cast<uint32_t>(out.y0);
    const uint32_t w = static_cast<uint32_t>(out.x1 - out.x0 + 1);
    const uint32_t h = static_cast<uint32_t>(out.y1 - out.y0 + 1);
    out.color = g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                                 ctx.frame.fbw, x, y, w, h);
    const bool depthActive = ((ctx.test >> 16u) & 1u) != 0u ||
                             ((ctx.zbuf >> 32u) & 1u) == 0u;
    if (depthActive)
        out.depth = g144RangeForRect(
            static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5u,
            static_cast<uint8_t>((ctx.zbuf >> 24u) & 0xFu), ctx.frame.fbw,
            x, y, w, h);
    if (prim.tme)
    {
        G144Entry cand{};
        cand.ctx = ctx;
        cand.prim = prim;
        cand.texclut = texclut;
        out.texture = g144TextureRange(cand);
        if (g144IsPalettedPsm(ctx.tex0.psm))
            out.clut = g144ClutRange(cand);
    }
    out.unknown = !out.color.valid ||
                  (depthActive && !out.depth.valid) ||
                  (prim.tme && !out.texture.valid) ||
                  (prim.tme && g144IsPalettedPsm(ctx.tex0.psm) && !out.clut.valid);
    const G144BlockRange owner = g289OwnerRange();
    out.carry = !out.unknown && owner.valid &&
                !g144RangeOverlaps(owner, out.color) &&
                !g144RangeOverlaps(owner, out.depth) &&
                !g144RangeOverlaps(owner, out.texture) &&
                !g144RangeOverlaps(owner, out.clut);
    return out;
}

static bool g289CanDeferLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                  uint32_t ssax, uint32_t ssay,
                                  uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay,
                                  uint32_t rrw, uint32_t rrh)
{
    const bool can = g289LocalCopyOn() && !s_g289Owner.active && s_g276Pend.active &&
           s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
           s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
           framePageBaseToBlock(s_g276Pend.fbp) == sbp &&
           sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
           (dbp == framePageBaseToBlock(0x13bu) ||
            dbp == framePageBaseToBlock(0x14au)) &&
           dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
           rrw == 512u && rrh == 416u;
    if (!can && g289LocalCopyOn() && s_g276Pend.active && g289StatOn())
    {
        static std::atomic<uint32_t> s_logs{0};
        const uint32_t n = s_logs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 16u)
            std::fprintf(
                stderr,
                "[G289:defer-reject] n=%u owner=%u pend=%x/%u/%d..%d/%d "
                "copy(src=%x/%u/%x/%u,%u dst=%x/%u/%x/%u,%u size=%ux%u)\n",
                n + 1u, s_g289Owner.active ? 1u : 0u, s_g276Pend.fbp,
                s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                s_g276Pend.fbW, sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh);
    }
    return can;
}

// Called by GS::performLocalToLocalTransfer after all graph/range barriers have completed. A false
// result leaves the caller on its unchanged legacy copy. A true result means the exact logical
// write lives in fbp=0x13b's FBO until the immediately measured G287 consumer or a fail-closed
// materialization edge.
bool g289TryGpuLocalCopy(uint8_t *vram, uint32_t vramSize,
                         uint32_t sbp, uint8_t sbw, uint8_t spsm,
                         uint32_t ssax, uint32_t ssay,
                         uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                         uint32_t dsax, uint32_t dsay,
                         uint32_t rrw, uint32_t rrh)
{
    if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                               dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
        return false;

    const uint32_t sourceFbp = s_g276Pend.fbp;
    const uint32_t destFbp = dbp >> 5u;
    if (!g289_backend_copy_display_to_work(sourceFbp, destFbp))
    {
        const uint64_t n =
            g_g289CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        std::fprintf(stderr,
                     "[G289:copy-fail] n=%llu src=0x%x dst=0x%x; legacy fallback\n",
                     static_cast<unsigned long long>(n), sourceFbp, destFbp);
        g276FlushPendingDisplay(8);
        return false;
    }

    s_g289Owner.active = true;
    ++s_g289Owner.version;
    s_g289Owner.srcFbp = sourceFbp;
    s_g289Owner.dstFbp = destFbp;
    const G144BlockRange ownerRange =
        g144RangeForRect(dbp, GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
    s_g289Owner.firstOwnedBlock = ownerRange.first;
    s_g289Owner.lastOwnedBlock = ownerRange.last;
    s_g289Owner.vram = vram;
    s_g289Owner.vramSize = vramSize;
    s_g289PendingUpload.active = false;
    const uint64_t copyN =
        g_g289Copies.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (copyN <= 16u || (copyN % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:copy] n=%llu version=%llu src=0x%x dst=0x%x rows=0..415\n",
                     static_cast<unsigned long long>(copyN),
                     static_cast<unsigned long long>(s_g289Owner.version), sourceFbp,
                     destFbp);

    if (g289VerifyOn())
    {
        // Make the legacy source bytes authoritative, materialize the GPU copy, then compare the
        // two exact CT32 transfer views pixel-for-pixel. This deliberately disables the payoff.
        g276FlushPendingDisplay(8);
        const bool matOk = g289MaterializeOwner(9);
        uint64_t bad = matOk ? 0u : 512u * 416u;
        const uint32_t srcBlock = framePageBaseToBlock(sourceFbp);
        const uint32_t dstBlock = framePageBaseToBlock(destFbp);
        if (matOk)
        {
            for (uint32_t y = 0u; y < 416u; ++y)
                for (uint32_t x = 0u; x < 512u; ++x)
                {
                    uint32_t srcPx = 0u, dstPx = 0u;
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(srcBlock, 8u, x, y);
                    const uint32_t dstOff = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
                    if (srcOff + 4u > vramSize || dstOff + 4u > vramSize)
                    {
                        ++bad;
                        continue;
                    }
                    std::memcpy(&srcPx, vram + srcOff, 4u);
                    std::memcpy(&dstPx, vram + dstOff, 4u);
                    if (srcPx != dstPx)
                        ++bad;
                }
        }
        const uint64_t checks =
            g_g289VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g289VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (checks <= 16u || bad != 0u || (checks % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:verify] checks=%llu bad=%llu totalBad=%llu firstUse=%u\n",
                         static_cast<unsigned long long>(checks),
                         static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(
                             g_g289VerifyBad.load(std::memory_order_relaxed)),
                         checks == 1u ? 1u : 0u);
    }
    return true;
}

static bool g289CanConsumeOwner(uint8_t *vram, uint32_t vramSize)
{
    if (!s_g289Owner.active || s_g289Owner.vram != vram ||
        s_g289Owner.vramSize != vramSize || s_g289Owner.dstFbp != 0x13bu ||
        g_g144List.size() != 1u)
    {
        if (s_g289Owner.active && g289StatOn())
        {
            static std::atomic<uint32_t> s_earlyRejectLogs{0};
            const uint32_t n = s_earlyRejectLogs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 8u)
                std::fprintf(
                    stderr,
                    "[G289:consume-early-reject] n=%u ownerVram=%p/%u vram=%p/%u entries=%zu\n",
                    n + 1u, static_cast<void *>(s_g289Owner.vram), s_g289Owner.vramSize,
                    static_cast<void *>(vram), vramSize, g_g144List.size());
        }
        return false;
    }
    const G144Entry &e = g_g144List[0];
    const G144BlockRange tex = g144TextureRange(e);
    const G144BlockRange clut = g144ClutRange(e);
    const G144BlockRange owner = g289OwnerRange();
    const bool exact =
        e.ctx.frame.fbp == 0x13bu && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 415u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && !e.prim.abe &&
        e.ctx.tex0.tbp0 == 0x2720u && e.ctx.tex0.tbw == 2u &&
        e.ctx.tex0.psm == GS_PSM_T8 && e.ctx.tex0.tw == 7u && e.ctx.tex0.th == 7u &&
        e.ctx.tex0.cbp == 0x3fbcu && e.ctx.tex0.cpsm == GS_PSM_CT32 &&
        owner.valid && tex.valid && clut.valid &&
        !g144RangeOverlaps(owner, tex) && !g144RangeOverlaps(owner, clut);
    if (!exact && g289StatOn())
    {
        static std::atomic<uint32_t> s_rejectLogs{0};
        const uint32_t n = s_rejectLogs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(
                stderr,
                "[G289:consume-reject] n=%u frame=%03x/%u/%x sc=%u,%u..%u,%u "
                "prim=%u tme=%u abe=%u tex=%x/%u/%x/%u/%u clut=%x/%x "
                "range(owner=%x..%x tex=%x..%x clut=%x..%x)\n",
                n + 1u, e.ctx.frame.fbp, static_cast<unsigned>(e.ctx.frame.fbw),
                e.ctx.frame.psm, e.ctx.scissor.x0, e.ctx.scissor.y0,
                e.ctx.scissor.x1, e.ctx.scissor.y1, static_cast<unsigned>(e.prim.type),
                e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u, e.ctx.tex0.tbp0,
                static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm,
                static_cast<unsigned>(e.ctx.tex0.tw), static_cast<unsigned>(e.ctx.tex0.th),
                e.ctx.tex0.cbp, e.ctx.tex0.cpsm, owner.first, owner.last,
                tex.first, tex.last, clut.first, clut.last);
    }
    return exact;
}

// A versioned work-page owner may cross arbitrary native batches, not just the immediately next
// flush. Keep it GPU-local only when every color/depth/texture/CLUT range is known and disjoint;
// overlap or an unresolvable range fails closed to guest-VRAM materialization.
static bool g289BatchDisjointOwner(uint32_t &reason, size_t &hitEntry)
{
    const G144BlockRange owner = g289OwnerRange();
    reason = owner.valid ? 0u : 0x10u;
    hitEntry = 0u;
    bool overlap = false;
    bool unknown = !owner.valid;
    for (size_t i = 0u; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                              e.ctx.scissor.y1 >= e.ctx.scissor.y0;
        const G144BlockRange color = g144TargetRange(e);
        if (nonEmpty && !color.valid)
        {
            unknown = true;
            reason |= 0x11u;
        }
        else if (color.valid && g144RangeOverlaps(owner, color))
        {
            overlap = true;
            reason |= 0x1u;
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(owner, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(owner, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(owner, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }
    return !overlap && !unknown;
}

static bool g289OwnerAliasTuple(const G144Entry &e)
{
    const bool triangle =
        e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
        e.prim.type == GS_PRIM_TRIFAN;
    const bool format =
        e.ctx.tex0.psm == GS_PSM_CT32 || e.ctx.tex0.psm == GS_PSM_CT24;
    const bool ct24Alpha =
        e.ctx.tex0.psm != GS_PSM_CT24 ||
        (e.ctx.tex0.tcc == 1u && e.texa.ta0 == 128u &&
         !e.texa.aem && e.texa.ta1 == 128u);
    return triangle && e.prim.tme && e.prim.fst && format && ct24Alpha &&
           (e.ctx.tex0.tbp0 == 0x2720u || e.ctx.tex0.tbp0 == 0x2760u) &&
           e.ctx.tex0.tbw == 8u && e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u;
}

// The measured final-composition batch may read the copied work pages through two CT32-page
// aliases. This coarse pass runs before classification and admits no bytes yet; it only defers
// fail-closed materialization until exact per-entry tap footprints are available.
static bool g289CanTryOwnerView(uint8_t *vram, uint32_t vramSize)
{
    if (!s_g289Owner.active || s_g289Owner.vram != vram ||
        s_g289Owner.dstFbp != 0x13bu ||
        s_g289Owner.vramSize != vramSize || g_g144List.empty())
        return false;
    const uint32_t fbp = g_g144List[0].ctx.frame.fbp;
    if (fbp != 0u && fbp != 0x68u)
        return false;
    const G144BlockRange owner = g289OwnerRange();
    if (!owner.valid)
        return false;
    bool hit = false;
    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        if (!color.valid || g144RangeOverlaps(owner, color) ||
            (depth.active && (!depth.range.valid || g144RangeOverlaps(owner, depth.range))))
            return false;
        if (!e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        if (!tex.valid)
            return false;
        if (g144RangeOverlaps(owner, tex))
        {
            if (!g289OwnerAliasTuple(e))
                return false;
            hit = true;
        }
        if (g144IsPalettedPsm(e.ctx.tex0.psm))
        {
            const G144BlockRange clut = g144ClutRange(e);
            if (!clut.valid || g144RangeOverlaps(owner, clut))
                return false;
        }
    }
    return hit;
}

struct G289ConsumeGuard
{
    bool armed = false;
    ~G289ConsumeGuard()
    {
        if (armed && s_g289Owner.active)
        {
            g_g289Unexpected.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(10);
        }
    }
};

struct G289OwnerViewGuard
{
    bool armed = false;
    ~G289OwnerViewGuard()
    {
        if (armed && s_g289Owner.active)
        {
            g_g289OwnerViewFail.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(11);
        }
    }
};

struct G289DirectGuard
{
    bool armed = false;
    ~G289DirectGuard()
    {
        if (armed && s_g276Pend.active)
            g276FlushPendingDisplay(0);
    }
};

static void g277CensusPrintMaybe()
{
    static uint64_t s_events = 0; // flush-owner thread only
    if ((++s_events % 4096u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G277:census] g276flush(c0=%llu c3=%llu c4=%llu c5=%llu c6=%llu c7=%llu c8=%llu) "
                 "inl(empty=%llu noovl=%llu ovl=%llu z=%llu) "
                 "e7(n=%llu skip=%llu) e8(n=%llu skip=%llu) "
                 "zrb(n=%llu coalescable=%llu)\n",
                 (unsigned long long)g_g277FlushByCause[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlEmpty.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlNoOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlZ.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRbCoalescable.load(std::memory_order_relaxed));
}

// Hard consumer edge (cause 3 CPU replay, 5 frame boundary, 6 un-ranged upload): always a real
// VRAM consumer — breaks the simulated Z-coalescing run.
static void g277CensusHardEdge(int cause)
{
    if (!g277CensusOn())
        return;
    if (cause >= 0 && cause < 9)
        g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    s_g277ZRunLive = false;
}

// Ranged transfer edge (cause 7 l2h dst, cause 8 l2l src+dst): skippable for the pending display
// flush when the transfer provably misses the pending rows; breaks the Z run only on Z overlap.
static void g277CensusRangeEdge(int cause, const G144BlockRange &src, const G144BlockRange &dst,
                                bool srcUnknown, bool dstUnknown)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    if (srcUnknown || dstUnknown)
    {
        s_g277ZRunLive = false;
        g277CensusPrintMaybe();
        return;
    }
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        if (pend.valid && !g144RangeOverlaps(pend, src) && !g144RangeOverlaps(pend, dst))
            g_g277EdgeSkippable[cause].fetch_add(1u, std::memory_order_relaxed);
    }
    if (s_g277ZRunLive &&
        (g144RangeOverlaps(s_g277ZRunRange, src) || g144RangeOverlaps(s_g277ZRunRange, dst)))
        s_g277ZRunLive = false;
    g277CensusPrintMaybe();
}

// Inline-primitive consumer edge (cause 4). Classifies by the prim's CLIPPED screen bbox: a fully
// scissored-out prim (the offscreen sceGsSwapDBuff sync linestrips, vtx x∈[1792,2303]) rasterizes
// nothing, reads nothing, writes nothing — a phantom consumer. Non-empty prims are classified by
// block-range overlap (target/Z/texture/CLUT vs the pending display rows), and break the simulated
// Z run only when they actually test/write Z.
static void g277CensusInlineEdge(const GSPrimReg &prim, const GSVertex *verts,
                                 const GSContext &ctx, const GSTexClutReg &texclut)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[4].fetch_add(1u, std::memory_order_relaxed);
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x, minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x); maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y); maxYf = std::max(maxYf, verts[i].y);
    }
    const int bx0 = static_cast<int>(std::floor(minXf)) - ofx;
    const int bx1 = static_cast<int>(std::ceil(maxXf)) - ofx;
    const int by0 = static_cast<int>(std::floor(minYf)) - ofy;
    const int by1 = static_cast<int>(std::ceil(maxYf)) - ofy;
    const int cx0 = std::max(bx0, static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(bx1, static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(by0, static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(by1, static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
    {
        g_g277InlEmpty.fetch_add(1u, std::memory_order_relaxed); // phantom: draws nothing
        g277CensusPrintMaybe();
        return;
    }
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    const uint64_t ts = ctx.test;
    const bool zte = ((ts >> 16) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32) & 1u) != 0u;
    if ((zte && ztst >= 2u) || !zmsk)
    {
        g_g277InlZ.fetch_add(1u, std::memory_order_relaxed);
        if (s_g277ZRunLive)
        {
            const uint32_t zbpBlock = (static_cast<uint32_t>(ctx.zbuf) & 0x1FFu) << 5u;
            const G144BlockRange zr = g144RangeForRect(zbpBlock, GS_PSM_Z24, ctx.frame.fbw,
                                                       static_cast<uint32_t>(cx0),
                                                       static_cast<uint32_t>(cy0), w, h);
            if (!zr.valid || g144RangeOverlaps(s_g277ZRunRange, zr))
                s_g277ZRunLive = false;
        }
    }
    bool ovl = false;
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const uint32_t fbBlock = framePageBaseToBlock(ctx.frame.fbp);
        const G144BlockRange tgt = g144RangeForRect(fbBlock, ctx.frame.psm, ctx.frame.fbw,
                                                    static_cast<uint32_t>(cx0),
                                                    static_cast<uint32_t>(cy0), w, h);
        ovl = !pend.valid || !tgt.valid || g144RangeOverlaps(pend, tgt);
        if (!ovl && prim.tme)
        {
            const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
            const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
            const G144BlockRange tex =
                g144RangeForRect(ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw, 0u, 0u, texW, texH);
            ovl = !tex.valid || g144RangeOverlaps(pend, tex);
            if (!ovl && g144IsPalettedPsm(ctx.tex0.psm))
            {
                const uint32_t clutBw = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
                const G144BlockRange clut =
                    g144RangeForRect(ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                                     static_cast<uint32_t>(texclut.cou),
                                     static_cast<uint32_t>(texclut.cov), 16u, 16u);
                ovl = !clut.valid || g144RangeOverlaps(pend, clut);
            }
        }
        if (ovl)
            g_g277InlOvl.fetch_add(1u, std::memory_order_relaxed);
        else
            g_g277InlNoOvl.fetch_add(1u, std::memory_order_relaxed);
    }
    g277CensusPrintMaybe();
}

// ---- G278: within-drain depth-readback coalescing ----------------------------------------------
// A G262 write wave leaves its Z result in the per-key backend depth texture and extends this
// pending row union. Same-key waves render directly against that texture, without uploading stale
// VRAM. The union is published before the first actual VRAM-Z consumer and unconditionally at the
// drain/presentation boundary. This is deliberately NOT G242 ownership: no state crosses a frame.
static G144BlockRange g278PendingDepthRange()
{
    if (!s_g278Pend.active || s_g278Pend.fbW <= 0 ||
        s_g278Pend.rowHi < s_g278Pend.rowLo)
        return {};
    return g144RangeForRect(s_g278Pend.zbpPage << 5u, GS_PSM_Z24,
                            s_g278Pend.fbw, 0u,
                            static_cast<uint32_t>(s_g278Pend.rowLo),
                            static_cast<uint32_t>(s_g278Pend.fbW),
                            static_cast<uint32_t>(
                                s_g278Pend.rowHi - s_g278Pend.rowLo + 1));
}

static G144BlockRange g278PendingDepthWholeRange()
{
    if (!s_g278Pend.active || s_g278Pend.fbW <= 0)
        return {};
    return g144RangeForRect(s_g278Pend.zbpPage << 5u, GS_PSM_Z24,
                            s_g278Pend.fbw, 0u, 0u,
                            static_cast<uint32_t>(s_g278Pend.fbW), 512u);
}

static bool g278StaleRowsAny(const G278PendDepth &p)
{
    for (uint64_t bits : p.staleRows)
        if (bits != 0u)
            return true;
    return false;
}

static bool g278StaleRowsOverlap(int lo, int hi)
{
    if (!s_g278Pend.active || hi < lo)
        return false;
    lo = std::max(0, lo);
    hi = std::min(511, hi);
    for (int y = lo; y <= hi; ++y)
        if ((s_g278Pend.staleRows[static_cast<size_t>(y >> 6)] >>
             static_cast<uint32_t>(y & 63)) & 1u)
            return true;
    return false;
}

static void g278MarkStaleRowsForRange(const G144BlockRange &write)
{
    if (!s_g278Pend.active || !write.valid)
        return;
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!whole.valid || !g144RangeOverlaps(whole, write))
        return;
    const uint32_t firstBlock = std::max(whole.first, write.first);
    const uint32_t lastBlock = std::min(whole.last, write.last);
    const uint32_t firstPage = (firstBlock - whole.first) >> 5u;
    const uint32_t lastPage = (lastBlock - whole.first) >> 5u;
    const uint32_t pagesPerRow = std::max(1u, s_g278Pend.fbw);
    const int lo = static_cast<int>(firstPage / pagesPerRow) * 32;
    const int hi = std::min(
        511, static_cast<int>(lastPage / pagesPerRow) * 32 + 31);
    for (int y = lo; y <= hi; ++y)
        s_g278Pend.staleRows[static_cast<size_t>(y >> 6)] |=
            1ull << static_cast<uint32_t>(y & 63);
    g_g278StaleMarks.fetch_add(1u, std::memory_order_relaxed);
}

static bool g278StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G278_STAT");
    return s_on;
}

static void g278Report(uint64_t flushes)
{
    if (!g278StatOn() || (flushes > 8u && (flushes % 256u) != 0u))
        return;
    std::fprintf(
        stderr,
        "[G278:zrb] deferred=%llu flushes=%llu saved=%llu rows=%llu uploadSkip=%llu "
        "fail=%llu inv=%llu stale=%llu reinit=%llu "
        "causes(0=%llu 1=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu)\n",
        (unsigned long long)g_g278Deferred.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Flushes.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Saved.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushRows.load(std::memory_order_relaxed),
        (unsigned long long)g_g278UploadSkips.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278InvariantFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278StaleMarks.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Reinits.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[0].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[1].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[3].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[4].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[5].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[6].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[7].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[8].load(std::memory_order_relaxed));
}

static bool g278FlushPendingDepth(int cause)
{
    if (!s_g278Pend.active)
        return true;
    const G278PendDepth p = s_g278Pend;
    const int rows = p.rowHi - p.rowLo + 1;
    const int glY = 512 - 1 - p.rowHi;
    static std::vector<float> s_readback; // flush-owner thread only
    s_readback.clear();
    const auto t0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    if (p.vram == nullptr || p.vramSize == 0u || p.fbW <= 0 || rows <= 0 ||
        !g242_backend_read_depth(p.key, p.fbW, 512, glY, rows, s_readback) ||
        s_readback.size() != static_cast<size_t>(p.fbW) * rows)
    {
        const uint64_t n = g_g278FlushFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
            std::fprintf(stderr,
                         "[G278:depth] PUBLISH FAIL n=%llu cause=%d key=0x%llx "
                         "zbp=0x%x rows=%d..%d fbW=%d\n",
                         (unsigned long long)n, cause,
                         (unsigned long long)p.key, p.zbpPage,
                         p.rowLo, p.rowHi, p.fbW);
        // Keep the pending owner armed so a transient backend failure can be retried before a
        // fallback/consumer proceeds. Any observed failure blocks promotion.
        return false;
    }

    constexpr double kDepthToZ = 4294967296.0;
    const uint32_t zbpBlock = p.zbpPage << 5u;
    const auto unpackDepthRows = [&](int laneLo, int laneHi) {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(p.rowHi - y) * p.fbW;
            for (int x = 0; x < p.fbW; ++x)
            {
                double scaled = static_cast<double>(
                                    s_readback[srcRow + static_cast<size_t>(x)]) *
                                kDepthToZ;
                if (scaled < 0.0) scaled = 0.0;
                if (scaled > 16777215.0) scaled = 16777215.0;
                GSMem::WriteZ24(p.vram, zbpBlock, p.fbw,
                                static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y),
                                static_cast<uint32_t>(scaled));
            }
        }
    };
    GSRowPool::instance().run(p.rowLo, p.rowHi,
                              rows >= 8 ? g275ZSwizzleLanes() : 1,
                              unpackDepthRows);
    g178BumpRectImpl(zbpBlock, p.fbw, GS_PSM_Z24, 0u,
                     static_cast<uint32_t>(p.rowLo),
                     static_cast<uint32_t>(p.fbW),
                     static_cast<uint32_t>(rows));
    g149BumpVramGen();
    if (g274ZTimeOn())
        g_g274ZRbNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count(),
            std::memory_order_relaxed);
    if (g278StaleRowsAny(p))
    {
        // An out-of-union CPU write left other backend rows stale. Force the next use to perform
        // the backend contract's full initialization; a whole-gen equality stamp would otherwise
        // let G274 incorrectly skip over those newer VRAM rows.
        g_g274LastZGen.erase(p.key);
        g_g262DepthInit.erase(p.key);
        g_g278Reinits.fetch_add(1u, std::memory_order_relaxed);
    }
    else if (g274ZTimeOn() || g274ZResidentOn() || g278DepthRbOn())
        g_g274LastZGen[p.key] =
            g178GenSumRect(zbpBlock, p.fbw, GS_PSM_Z24, 0u, 0u,
                           static_cast<uint32_t>(p.fbW), 512u);
    g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
    if (p.display)
        g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);

    s_g278Pend = {};
    s_g278Pend.rowLo = 512;
    s_g278Pend.rowHi = -1;
    const uint64_t n =
        g_g278Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g278FlushRows.fetch_add(static_cast<uint64_t>(rows),
                              std::memory_order_relaxed);
    if (cause >= 0 && cause < 9)
        g_g278FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    g278Report(n);
    return true;
}

static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown)
{
    if (!s_g278Pend.active)
        return true;
    if (srcUnknown || dstUnknown)
        return g278FlushPendingDepth(cause);
    const G144BlockRange dirty = g278PendingDepthRange();
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!dirty.valid || !whole.valid)
        return g278FlushPendingDepth(cause); // unknown owner window fails closed
    // Reads only require unpublished rows. A write that touches dirty rows must preserve the
    // historical GPU-Z-before-CPU-write order, so publish first. A write elsewhere in the same
    // target can proceed without forcing a readback: remember its rows as backend-stale and force
    // a full re-init when this owner ends (or publish before a same-key wave reaches those rows).
    if ((src.valid && g144RangeOverlaps(dirty, src)) ||
        (dst.valid && g144RangeOverlaps(dirty, dst)))
        return g278FlushPendingDepth(cause);
    if (dst.valid && g144RangeOverlaps(whole, dst))
        g278MarkStaleRowsForRange(dst);
    return true;
}

static bool g278FlushPendingDepthForDisplayOwners(int cause)
{
    if (!s_g278Pend.active)
        return true;
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!whole.valid)
        return g278FlushPendingDepth(cause);
    for (int di = 0; di < 2; ++di)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange dirty{};
        if (g272DirtyInfo(di, fbp, fbw, dirty) &&
            (!dirty.valid || g144RangeOverlaps(whole, dirty)))
            return g278FlushPendingDepth(cause);
    }
    if (s_g276Pend.active)
    {
        const G144BlockRange display = g277PendingDisplayRange();
        if (!display.valid || g144RangeOverlaps(whole, display))
            return g278FlushPendingDepth(cause);
    }
    return true;
}

// Exact inline edge: fully clipped primitives consume nothing. Otherwise publish only when the
// CPU primitive's target, active Z, texture, or CLUT range can touch unpublished Z bytes.
static bool g278FlushPendingDepthForInline(const GSPrimReg &prim,
                                           const GSVertex *verts,
                                           const GSContext &ctx,
                                           const GSTexClutReg &texclut)
{
    if (!s_g278Pend.active)
        return true;
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x;
    float minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x);
        maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y);
        maxYf = std::max(maxYf, verts[i].y);
    }
    const int cx0 = std::max(static_cast<int>(std::floor(minXf)) - ofx,
                             static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(static_cast<int>(std::ceil(maxXf)) - ofx,
                             static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(static_cast<int>(std::floor(minYf)) - ofy,
                             static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(static_cast<int>(std::ceil(maxYf)) - ofy,
                             static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
        return true;

    const G144BlockRange dirty = g278PendingDepthRange();
    const G144BlockRange whole = g278PendingDepthWholeRange();
    if (!dirty.valid || !whole.valid)
        return g278FlushPendingDepth(4);
    const uint32_t x = static_cast<uint32_t>(cx0);
    const uint32_t y = static_cast<uint32_t>(cy0);
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    const auto readsPending = [&](const G144BlockRange &r) {
        return !r.valid || g144RangeOverlaps(dirty, r);
    };
    const auto writesOwner = [&](const G144BlockRange &r) {
        return !r.valid || g144RangeOverlaps(whole, r);
    };

    const G144BlockRange target =
        g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                         ctx.frame.fbw, x, y, w, h);
    bool mustPublish = writesOwner(target);
    const uint64_t test = ctx.test;
    const bool zte = ((test >> 16u) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17u) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32u) & 1u) != 0u;
    const bool depthReads = zte && ztst >= 2u;
    const bool depthWrites = !zmsk;
    if (!mustPublish && (depthReads || depthWrites))
    {
        const uint32_t zbpBlock =
            (static_cast<uint32_t>(ctx.zbuf) & 0x1FFu) << 5u;
        const uint32_t zpsm =
            static_cast<uint32_t>((ctx.zbuf >> 24u) & 0xFu);
        const G144BlockRange zr = g144RangeForRect(
            zbpBlock, zpsm, ctx.frame.fbw, x, y, w, h);
        mustPublish = (depthReads && readsPending(zr)) ||
                      (depthWrites && writesOwner(zr));
    }
    if (!mustPublish && prim.tme)
    {
        const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
        const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
        mustPublish = readsPending(g144RangeForRect(
            ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw,
            0u, 0u, texW, texH));
        if (!mustPublish && g144IsPalettedPsm(ctx.tex0.psm))
        {
            const uint32_t clutBw =
                texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            mustPublish = readsPending(g144RangeForRect(
                ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                static_cast<uint32_t>(texclut.cou),
                static_cast<uint32_t>(texclut.cov), 16u, 16u));
        }
    }
    return !mustPublish || g278FlushPendingDepth(4);
}

// G263: exact GL-tap footprint admission for a BILINEAR FST sprite that samples a resident RTT
// target (the G262 census shape: the display composite, prim=6, CT32, tbp=0x2720, bilin=1 was the
// SOLE bind rejector). GL_LINEAR taps floor(s-0.5)/+1 around every sample; samples are the GL
// pixel-center evaluations of the pushed varyings (raw corner UVs, drawSprite's left-edge
// -0.5*gradient shift, linear in screen space) — the same 4-tap convention the CPU bilinear
// sampler applies (sampleU = texUf - 0.5, floor, +1), so GL and the CPU oracle read identical
// taps wherever both are defined. Admission requires every tap that can carry NONZERO weight to
// land on an FBO-DEFINED texel: rows [covLo..covHi], full width. One fold is allowed per axis:
// tap -1 under GS CLAMP folds onto texel 0 on both GL (CLAMP_TO_EDGE) and the CPU sampler, exact
// when row/col 0 is itself defined. Everything else fails closed to the materialize path:
// REPEAT consumers with edge taps (GL would clamp where the CPU wraps), taps past the covered
// rows (FBO content undefined there), taps past the DECLARED texture dims (the CPU folds at
// texW/texH, the fbW×512 FBO space does not).
static bool g263BilinearSpriteBind(const G144Entry &e, const G178EntryState &st,
                                   const G261Res &r, int *outTaps = nullptr)
{
    const GSVertex &q0 = e.v[0];
    const GSVertex &q1 = e.v[1];
    int tapLo[2] = {0, 0}, tapHi[2] = {0, 0};
    for (int a = 0; a < 2; ++a)
    {
        const float p0 = a ? q0.y : q0.x, p1 = a ? q1.y : q1.x;
        const float t0 = static_cast<float>(a ? q0.v : q0.u) / 16.0f;
        const float t1 = static_cast<float>(a ? q1.v : q1.u) / 16.0f;
        const float lo = std::min(p0, p1), hi = std::max(p0, p1);
        // First/last GL pixel centers covered by [lo, hi). XYOFFSET is integer at this level
        // (ofx = reg >> 4), so subtracting it keeps the +0.5 center grid — evaluate in raw coords.
        const float cF = std::ceil(lo - 0.5f) + 0.5f;
        const float cL = std::ceil(hi - 0.5f) - 0.5f;
        if (!(cL >= cF) || p1 == p0)
            return false; // degenerate/empty axis — nothing provable, materialize instead
        const float g = (t1 - t0) / (p1 - p0);
        const float shift = -0.5f * g; // drawSprite left-edge (0.0) bias, exactly as pushed
        const float sA = t0 + (cF - p0) * g + shift;
        const float sB = t0 + (cL - p0) * g + shift;
        const float sLo = std::min(sA, sB), sHi = std::max(sA, sB);
        tapLo[a] = static_cast<int>(std::floor(sLo - 0.5f - 0.001f));
        // ceil() keeps an exactly-integral edge in place (that +1 neighbour has weight 0); the
        // epsilon biases toward INCLUDING a tap, i.e. toward rejection, never toward admission.
        tapHi[a] = static_cast<int>(std::ceil(sHi - 0.5f + 0.001f));
    }
    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (tapLo[0] >= 0 || (clampU && tapLo[0] == -1)) && tapHi[0] <= maxCol;
    const bool vOk = (tapLo[1] >= r.covLo || (clampV && r.covLo == 0 && tapLo[1] == -1)) &&
                     tapHi[1] <= maxRow;
    if (!(uOk && vOk) && g262CensusOn())
    {
        static std::atomic<uint32_t> s_g263RejLog{0};
        if (s_g263RejLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr,
                         "[G263:rej] xy0=(%.2f,%.2f) xy1=(%.2f,%.2f) uv0=(%u,%u) uv1=(%u,%u) "
                         "tapU=%d..%d tapV=%d..%d cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u\n",
                         q0.x, q0.y, q1.x, q1.y, q0.u, q0.v, q1.u, q1.v,
                         tapLo[0], tapHi[0], tapLo[1], tapHi[1], r.covLo, r.covHi, r.fbW,
                         st.texW, st.texH, static_cast<unsigned>(st.wrapU),
                         static_cast<unsigned>(st.wrapV));
    }
    if (uOk && vOk && outTaps != nullptr)
    {
        outTaps[0] = tapLo[0];
        outTaps[1] = tapHi[0];
        outTaps[2] = tapLo[1];
        outTaps[3] = tapHi[1];
    }
    return uOk && vOk;
}

// G267: conservative tap footprint for an affine FST triangle. Every interpolated UV inside a
// triangle lies inside the per-axis convex hull of its three vertex UVs, so expanding that hull
// to the two GL_LINEAR taps is a safe (possibly over-wide) bound. Unlike the sprite helper above,
// this does not try to exploit exclusive right/bottom endpoints. The returned GS block range is
// used to reject a direct bind whenever another dirty resident target could contribute any sampled
// byte. That alias screen is load-bearing: binding one producer FBO while silently ignoring a dirty
// sibling would make an internal residency oracle pass but corrupt final composition.
struct G267TriFootprint
{
    int tapLoU = 0, tapHiU = -1;
    int tapLoV = 0, tapHiV = -1;
    uint32_t sampleX0 = 0, sampleY0 = 0, sampleX1 = 0, sampleY1 = 0;
    G144BlockRange sampleRange{};
};

static bool g267BilinearTriFootprint(const G144Entry &e, const G178EntryState &st,
                                     const G261Res &r, G267TriFootprint &out)
{
    float uvLo[2] = {FLT_MAX, FLT_MAX};
    float uvHi[2] = {-FLT_MAX, -FLT_MAX};
    for (int k = 0; k < 3; ++k)
    {
        const float u = static_cast<float>(e.v[k].u) / 16.0f;
        const float v = static_cast<float>(e.v[k].v) / 16.0f;
        if (!std::isfinite(u) || !std::isfinite(v))
            return false;
        uvLo[0] = std::min(uvLo[0], u); uvHi[0] = std::max(uvHi[0], u);
        uvLo[1] = std::min(uvLo[1], v); uvHi[1] = std::max(uvHi[1], v);
    }

    // GL_LINEAR samples floor(coord - 0.5) and its +1 neighbour. Epsilons only widen the
    // footprint, biasing uncertain half ties toward rejection rather than unsafe admission.
    out.tapLoU = static_cast<int>(std::floor(uvLo[0] - 0.5f - 0.001f));
    out.tapHiU = static_cast<int>(std::ceil (uvHi[0] - 0.5f + 0.001f));
    out.tapLoV = static_cast<int>(std::floor(uvLo[1] - 0.5f - 0.001f));
    out.tapHiV = static_cast<int>(std::ceil (uvHi[1] - 0.5f + 0.001f));

    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (out.tapLoU >= 0 || (clampU && out.tapLoU == -1)) &&
                     out.tapHiU <= maxCol;
    const bool vOk = (out.tapLoV >= r.covLo ||
                      (clampV && r.covLo == 0 && out.tapLoV == -1)) &&
                     out.tapHiV <= maxRow;
    if (!(uOk && vOk))
        return false;

    const uint32_t x0 = static_cast<uint32_t>(std::max(0, out.tapLoU));
    const uint32_t y0 = static_cast<uint32_t>(std::max(0, out.tapLoV));
    const uint32_t x1 = static_cast<uint32_t>(out.tapHiU);
    const uint32_t y1 = static_cast<uint32_t>(out.tapHiV);
    out.sampleX0 = x0; out.sampleY0 = y0; out.sampleX1 = x1; out.sampleY1 = y1;
    out.sampleRange = g144RangeForRect(e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.tbw,
                                       x0, y0, x1 - x0 + 1u, y1 - y0 + 1u);
    return out.sampleRange.valid;
}

// The generic G144BlockRange is intentionally conservative: it collapses a 2-D page footprint
// to one contiguous min/max interval. For G267's narrow U=0..16 strip over many page rows that
// interval falsely includes the seven unsampled CT32 page columns between each real column-0
// page. Enumerate the exact sampled CT32 pages before declaring a dirty sibling alias.
static bool g267SamplePagesOverlap(const G144Entry &e, const G267TriFootprint &fp,
                                   const G144BlockRange &resident)
{
    if (!resident.valid || e.ctx.tex0.psm != GS_PSM_CT32)
        return true; // fail closed for unknown range/format
    const uint32_t pageBw = std::max<uint32_t>(1u, e.ctx.tex0.tbw);
    const uint32_t xp0 = fp.sampleX0 / 64u, xp1 = fp.sampleX1 / 64u;
    const uint32_t yp0 = fp.sampleY0 / 32u, yp1 = fp.sampleY1 / 32u;
    for (uint32_t yp = yp0; yp <= yp1; ++yp)
    {
        for (uint32_t xp = xp0; xp <= xp1; ++xp)
        {
            const uint64_t first64 = static_cast<uint64_t>(e.ctx.tex0.tbp0) +
                                     (static_cast<uint64_t>(yp) * pageBw + xp) * 32u;
            if (first64 > 0x3FFFu)
                return true;
            const G144BlockRange page{static_cast<uint32_t>(first64),
                                      static_cast<uint32_t>(std::min<uint64_t>(first64 + 31u,
                                                                               0x3FFFu)),
                                      true};
            if (g144RangeOverlaps(page, resident))
                return true;
        }
    }
    return false;
}

// G294 (default-off, DC2_G294_OWNER_TOKEN=1; kill DC2_G294_NO_OWNER_TOKEN=1): per-physical-page
// authoritative-owner token. Requesting it implies the whole G280/G281/G282/G283 view+fanout+epoch
// stack so a same-binary A/B needs one behavior lever. Declared free here (env-only, no gate deps)
// so the request chains below and g283AuthorityRequested can all reference it.
static bool g294OwnerTokenRequested()
{
    if (envFlagEnabled("DC2_G294_NO_OWNER_TOKEN") || envFlagDisabled("DC2_G294_OWNER_TOKEN"))
        return false;
    return envFlagEnabled("DC2_G294_OWNER_TOKEN");
}

// ===== G280 bodies (state + forward decls beside the G264 telemetry above) =====
static bool g280AliasOn()
{
    // Bring-up default: OFF. Enable with DC2_G280_ALIAS_VIEW=1; DC2_G280_NO_ALIAS_VIEW=1 (or
    // =0) always disables. Rides the wave stack, so every established oracle/diagnostic decline
    // in g261WaveOn applies; additionally declines under both consumer-edge censuses so their
    // measurements stay behavior-pure (the G267/G277 census must observe the legacy cadence).
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G280_NO_ALIAS_VIEW") || envFlagDisabled("DC2_G280_ALIAS_VIEW"))
            return false;
        // G281 is a paying child configuration of the G280 page-view mechanism. Requesting it
        // implies G280 so a same-binary A/B needs one behavior lever, while the G280 kill still
        // tears down the complete view stack.
        const bool g282Requested =
            envFlagEnabled("DC2_G282_TIGHT_ROWS") &&
            !envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") &&
            !envFlagDisabled("DC2_G282_TIGHT_ROWS");
        // G283 authority is a paying child of the whole view/fan-out stack: requesting it implies
        // G282 (and thus G281/G280) so a same-binary A/B needs one behavior lever.
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G280_ALIAS_VIEW") ||
               envFlagEnabled("DC2_G281_T8_VIEW") ||
               g282Requested || g283Requested || g294OwnerTokenRequested();
    }();
    return s_on && g261WaveOn() && !g267CensusOn() && !g277CensusOn();
}

static bool g281ViewOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G281_NO_T8_VIEW") ||
            envFlagDisabled("DC2_G281_T8_VIEW"))
            return false;
        const bool g282Requested =
            envFlagEnabled("DC2_G282_TIGHT_ROWS") &&
            !envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") &&
            !envFlagDisabled("DC2_G282_TIGHT_ROWS");
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G281_T8_VIEW") ||
               g282Requested || g283Requested;
    }();
    return s_on && g280AliasOn();
}

static bool g282TightRowsOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") ||
            envFlagDisabled("DC2_G282_TIGHT_ROWS"))
            return false;
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G282_TIGHT_ROWS") || g283Requested;
    }();
    return s_on && g281ViewOn();
}

// G290 (bring-up opt-in DC2_G290_EXACT_ROWS=1; kills DC2_G290_NO_EXACT_ROWS=1 /
// DC2_G290_EXACT_ROWS=0): exact vertex-bound rows for the measured fbp=0x139/fbw=8 CT32 batch
// with READ-ONLY zbp=0xd0 Z24 depth. The legacy scissor row union (448 rows) makes the
// conservative color/Z page test a FALSE alias (color pages 313..319 vs Z page row 13); the
// per-entry capture bounds prove rows 0..415 → color pages 313..416 and Z pages 208..311 are
// physically disjoint (same false-alias class G273 removed for the 0x68 display tuple).
// Independent of the default-off G280/G281/G282 alias-view stack.
// Every 64x32 page tile of resident target `fbp` whose block interval overlaps `rd` must be
// fully upload-re-covered (all 2048 pixel bits set in g291Cov) for the read to skip
// materialization. Fail-closed on any layout mismatch, invalid range, or partial coverage.
static bool g291PagesCurrent(const G261Res &r, uint32_t fbp, const G144BlockRange &rd)
{
    if (!rd.valid || r.g291CovFbw == 0u || r.g291CovFbw != r.fbw ||
        r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64)
    {
        static std::atomic<uint32_t> s_shapeLog{0};
        if (g291StatOn() && s_shapeLog.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr,
                         "[G291:fail-shape] t=%03x valid=%d covFbw=%u fbw=%u fbW=%d\n",
                         fbp, rd.valid ? 1 : 0, r.g291CovFbw, r.fbw, r.fbW);
        return false;
    }
    const uint32_t base = framePageBaseToBlock(fbp);
    const uint32_t pages = 16u * r.fbw; // 512 rows / 32 per page-row
    for (uint32_t p = 0; p < pages; ++p)
    {
        const uint32_t b0 = base + p * 32u;
        const uint32_t b1 = b0 + 31u;
        if (b1 < rd.first || b0 > rd.last)
            continue; // page outside the read
        const uint32_t py = p / r.fbw;
        const uint32_t px = p % r.fbw;
        const uint32_t rowLo = py * 32u;
        const uint32_t word = px; // 64 pixels per uint64 word, page cols are word-aligned
        for (uint32_t y = rowLo; y < rowLo + 32u; ++y)
            if (r.g291Cov[y][word] != ~uint64_t{0})
            {
                static std::atomic<uint32_t> s_pageLog{0};
                if (g291StatOn() && fbp == 0x139u &&
                    s_pageLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr,
                                 "[G291:fail-page] t=%03x rd=0x%x..0x%x page=%u py=%u px=%u "
                                 "y=%u cov=0x%016llx\n",
                                 fbp, rd.first, rd.last, p, py, px, y,
                                 (unsigned long long)r.g291Cov[y][word]);
                return false;
            }
    }
    return true;
}
static bool g290ExactRowsOn()
{
    static const bool s_on = (envFlagEnabled("DC2_G290_EXACT_ROWS") || g291AtlasOn()) &&
                             !envFlagEnabled("DC2_G290_NO_EXACT_ROWS") &&
                             !envFlagDisabled("DC2_G290_EXACT_ROWS");
    return s_on;
}
static bool g290StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G290_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g290ExactAdmit{0}, g_g290ExactRejOverlap{0}, g_g290ExactRejShape{0};

// ===== G283 PHYSICAL-PAGE AUTHORITY / EPOCH (default-off, DC2_G283_AUTHORITY=1) =====
// G280/G281/G282 track per-TARGET contentGen, which cannot express that TWO different resident
// targets alias the SAME physical GS page (CT32 pages are shared VRAM addressed under different
// FRAME/FBW layouts). G282's write-side fan-out proved a real payoff (removed 205 CPU
// materializations at 3,072 waves) but was UNSAFE with more than one dirty sibling owning a page:
// an independent later write to one alias left the other alias's overlay/copy stale, and the exact
// G280 verifier caught a full mismatched page 0x2860 (0x13c vs 0x139). G283 adds a per-PHYSICAL-PAGE
// monotonic epoch bumped on EVERY write to ANY aliasing target; G280 overlays and G282 fan-out
// records store the epoch at resolve time and are treated as stale (re-resolved or VRAM-re-synced)
// the moment any peer alias writes the page. This is the arbitration that lets multi-owner fan-out
// hold exactly, so the measured G282 batch can keep the five targets resident without a stale peer.
// The mechanism is authority-off inert: g283NoteTargetPages/g283PageEpoch short-circuit and the
// fan-out keeps G282's single-owner fail-close.
static bool g283AuthorityRequested()
{
    if (envFlagEnabled("DC2_G283_NO_AUTHORITY") || envFlagDisabled("DC2_G283_AUTHORITY"))
        return false;
    return envFlagEnabled("DC2_G283_AUTHORITY");
}
static bool g283AuthorityOn()
{
    // G294 turns the epoch ON (cross-pass overlay staleness) WITHOUT g282's write-side fanout:
    // the owner-token gives in-pass source selection, the epoch gives cross-pass invalidation, and
    // skipping the fanout (which pre-equalizes aliases and forces G291 skips=0) leaves the retire
    // free to pay. g282PrepareFanout still only runs under g282TightRowsOn (false under g294).
    static const bool s_req = g283AuthorityRequested() || g294OwnerTokenRequested();
    return s_req && (g282TightRowsOn() || g294OwnerTokenRequested());
}
static bool g283StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G283_STAT");
    return s_on;
}
// pageBlock (32-block-aligned) -> monotonic epoch. Absent == 0. Flush-owner thread only (same
// contract as g_g280Ovl / g_g280ContentGen). At most 512 CT32 pages exist below 0x4000, so the
// map is tiny and never crosses a frame boundary in an unbounded way.
static std::unordered_map<uint32_t, uint64_t> g_g283Epoch;
static std::atomic<uint64_t> g_g283Bumps{0}, g_g283Stale{0}, g_g283MultiOwner{0},
    g_g283FanoutPages{0};
static uint64_t g283PageEpoch(uint32_t pageBlock)
{
    if (!g283AuthorityOn())
        return 0u;
    auto it = g_g283Epoch.find(pageBlock);
    return it == g_g283Epoch.end() ? 0u : it->second;
}
static uint64_t g283BumpPageEpoch(uint32_t pageBlock)
{
    uint64_t &e = g_g283Epoch[pageBlock];
    ++e;
    g_g283Bumps.fetch_add(1u, std::memory_order_relaxed);
    return e;
}

// ===== G294 per-physical-page authoritative-owner token (default-off, DC2_G294_OWNER_TOKEN=1) =====
// G283's page epoch proves "someone wrote this page since resolve" but not WHO. With a REAL
// multi-owner page (G292: physical page 0x2860 aliased by 0x13c and 0x139 with divergent content),
// g280TryEntry copied EVERY dirty sibling overlapping the page into the same dst tile
// (last-writer-wins) and the exact verifier caught a full-page mismatch. G294 records the CURRENT
// authoritative owner target per physical CT32 page — the last target to INDEPENDENTLY write it
// (wave render or VRAM upload; a g280/g282 COPY does not restamp) — plus a monotonic equivalence
// version. g280 then copies a page ONLY from its authoritative owner; a non-owner (divergent)
// source is skipped and the dst keeps existing VRAM/overlay bytes (fail-closed). A g282 fanout
// leaves owner=source, so its byte-identical destinations share the owner's version implicitly; the
// next independent write to any of them re-stamps owner to that writer, staling the equivalents.
struct G294Owner
{
    int32_t ownerTi = -1;
    uint64_t equivVer = 0u;
};
static std::unordered_map<uint32_t, G294Owner> g_g294Owner; // pageBlock -> authoritative owner
static uint64_t g_g294EquivSeq = 0u; // monotonic version source (flush-owner thread only)
static std::atomic<uint64_t> g_g294Stamps{0}, g_g294OwnerSel{0}, g_g294NonOwnerSkip{0};
static bool g294OwnerTokenOn()
{
    // Rides the G280 alias-view (NOT G283): the owner-token REPLACES G282's write-side fanout as
    // the multi-owner arbiter. G283's fanout pre-equalizes aliases so multiOwner never diverges
    // (exact but neutral — it forces G291 skips=0, G292); the owner-token instead lets g280's
    // read-side copy pick the one authoritative source, leaving G291's upload-re-coverage retire
    // free to pay. commitOvl replaces the overlay per pageBlock (most-recent owner wins), so no
    // per-overlay equivalence version is needed for exactness.
    static const bool s_req = g294OwnerTokenRequested();
    return s_req && g280AliasOn();
}
static bool g294StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G294_STAT");
    return s_on;
}
// An INDEPENDENT write (render/upload) into target ti is the fresh authority for pageBlock.
static void g294StampOwner(int ti, uint32_t pageBlock)
{
    if (!g294OwnerTokenOn())
        return;
    G294Owner &o = g_g294Owner[pageBlock];
    o.ownerTi = ti;
    o.equivVer = ++g_g294EquivSeq;
    g_g294Stamps.fetch_add(1u, std::memory_order_relaxed);
}
// Current authoritative owner target of pageBlock, or -1 if none/authority-off.
static int g294PageOwner(uint32_t pageBlock)
{
    if (!g294OwnerTokenOn())
        return -1;
    auto it = g_g294Owner.find(pageBlock);
    return it == g_g294Owner.end() ? -1 : it->second.ownerTi;
}

// A resident target's FBO content changed over GS rows [rowLo..rowHi]. Bump the epoch of every
// physical CT32 page the target defines in those rows, so any overlay/fan-out copy of the SAME
// physical page made from a DIFFERENT alias is provably stale by epoch mismatch. Cheap and gated:
// authority-off returns immediately, and the measured family is at most 13x8 pages.
static void g283NoteTargetPages(int ti, int rowLo, int rowHi)
{
    if ((!g283AuthorityOn() && !g294OwnerTokenOn()) ||
        ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    const bool g283On = g283AuthorityOn(); // epoch bumps only under G283; owner stamps under G294
    const G261Res &r = g_g261Res[ti];
    if (r.fbw == 0u || r.fbW <= 0)
        return;
    const uint32_t base = framePageBaseToBlock(kG261Fbp[ti]);
    const int pcols = r.fbW / 64;
    const int lo = std::max(0, rowLo);
    const int hi = std::min(511, rowHi);
    for (int y = (lo / 32) * 32; y <= hi; y += 32)
    {
        const uint32_t prow = static_cast<uint32_t>(y) / 32u;
        for (int col = 0; col < pcols; ++col)
        {
            const uint64_t first64 = static_cast<uint64_t>(base) +
                (static_cast<uint64_t>(prow) * r.fbw + static_cast<uint32_t>(col)) * 32u;
            if (first64 <= 0x3FFFu)
            {
                if (g283On)
                    g283BumpPageEpoch(static_cast<uint32_t>(first64));
                g294StampOwner(ti, static_cast<uint32_t>(first64)); // G294: fresh authority
            }
        }
    }
}

static bool g282StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_STAT");
    return s_on;
}

static bool g282VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_VERIFY");
    return s_on;
}

static bool g281StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G281_STAT");
    return s_on;
}

static bool g281VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G281_VERIFY");
    return s_on;
}

static bool g280StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G280_STAT");
    return s_on;
}

static uint64_t g281ViewKey(uint64_t base)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(base);
    mix(0x4732383154385649ull); // "G281T8VI": distinct from guest-VRAM decoded textures
    return h | (1ull << 63);
}

// Exact physical-page ownership test shared by the G281 read view and G282 write fanout.
// G261's range is an interval around a row window and can include layout holes; map the page
// through the target's real CT32 page grid before calling it resident-owned.
static bool g280DirtyTargetOwnsPage(int ti, uint32_t pageBlock,
                                    uint32_t *pageRow = nullptr,
                                    uint32_t *pageCol = nullptr)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return false;
    const G261Res &r = g_g261Res[ti];
    if (!r.dirty || r.fbw == 0u || r.fbW <= 0 ||
        r.dirtyHi < r.dirtyLo || !r.range.valid)
        return false;
    const uint32_t base = framePageBaseToBlock(kG261Fbp[ti]);
    if (pageBlock < base || ((pageBlock - base) & 31u) != 0u)
        return false;
    const uint32_t off = (pageBlock - base) >> 5u;
    const uint32_t row = off / r.fbw;
    const uint32_t col = off % r.fbw;
    const int y0 = static_cast<int>(row) * 32;
    if (static_cast<int>(col + 1u) * 64 > r.fbW || y0 >= 512 ||
        y0 + 31 < r.dirtyLo || y0 > r.dirtyHi)
        return false;
    const G144BlockRange page{pageBlock, pageBlock + 31u, true};
    if (!g144RangeOverlaps(page, r.range))
        return false;
    if (pageRow) *pageRow = row;
    if (pageCol) *pageCol = col;
    return true;
}

// Invert one CT32 page's 2048 word positions once. PSMT8 bytes and CT32 pixels share physical
// GS pages but NOT within-page geometry; the view therefore starts with the real AddressP8
// result, then maps that byte through this exact CT32 inverse instead of assuming tile order.
struct G281Ct32Inverse
{
    std::array<int16_t, 2048> x{};
    std::array<int16_t, 2048> y{};
    bool ok = true;
    G281Ct32Inverse()
    {
        x.fill(-1);
        y.fill(-1);
        for (uint32_t yy = 0; yy < 32u; ++yy)
            for (uint32_t xx = 0; xx < 64u; ++xx)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, xx, yy);
                const uint32_t word = (off & 8191u) >> 2u;
                if (word >= x.size() || x[word] >= 0)
                {
                    ok = false;
                    continue;
                }
                x[word] = static_cast<int16_t>(xx);
                y[word] = static_cast<int16_t>(yy);
            }
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] < 0 || y[i] < 0)
                ok = false;
    }
};

// Build the exact per-texel source map for the measured PSMT8 view. Each RGBA8 map texel packs:
// R=src X, G=src GL-Y low byte, B.bit0=GL-Y bit8, B.bits1..2=byte lane. The source is restricted
// to one dirty CT32 FBO, and every physical page must be wholly outside all other dirty owners.
static bool g281BuildT8Map(const G144Entry &e, int srcTi, int texW, int texH,
                           std::vector<uint32_t> &out)
{
    static const G281Ct32Inverse s_inv;
    if (!s_inv.ok || srcTi < 0 || srcTi >= static_cast<int>(kG248TargetCount) ||
        texW <= 0 || texH <= 0 || texW > 256 || texH > 512)
        return false;
    const G261Res &src = g_g261Res[srcTi];
    if (!src.dirty || !src.range.valid || src.fbw == 0u || src.fbW <= 0 || src.fbW > 256)
        return false;
    const uint32_t srcBase = framePageBaseToBlock(kG261Fbp[srcTi]);
    const uint32_t pageBwT8 = std::max<uint32_t>(1u, e.ctx.tex0.tbw >> 1u);
    out.resize(static_cast<size_t>(texW) * texH);
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint32_t off = GSMem::AddressP8(
                e.ctx.tex0.tbp0, pageBwT8,
                static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
            const uint32_t pageBlock = (off >> 13u) << 5u;
            const G144BlockRange page{pageBlock, pageBlock + 31u, true};
            if (page.last > 0x3FFFu || page.first < src.range.first ||
                page.last > src.range.last || pageBlock < srcBase ||
                ((pageBlock - srcBase) & 31u) != 0u)
                return false;
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                if (static_cast<int>(t) == srcTi || !g_g261Res[t].dirty)
                    continue;
                if (!g_g261Res[t].range.valid ||
                    g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock))
                    return false;
            }

            const uint32_t pageOff = (pageBlock - srcBase) >> 5u;
            const uint32_t pageRow = pageOff / src.fbw;
            const uint32_t pageCol = pageOff % src.fbw;
            const uint32_t word = (off & 8191u) >> 2u;
            if (word >= s_inv.x.size())
                return false;
            const int sx = static_cast<int>(pageCol) * 64 + s_inv.x[word];
            const int sy = static_cast<int>(pageRow) * 32 + s_inv.y[word];
            if (sx < 0 || sx >= src.fbW || sy < src.covLo || sy > src.covHi ||
                sy < 0 || sy >= 512)
                return false;
            const uint32_t glY = static_cast<uint32_t>(511 - sy);
            const uint32_t lane = off & 3u;
            const uint32_t packed = static_cast<uint32_t>(sx) |
                                    ((glY & 0xFFu) << 8u) |
                                    ((((glY >> 8u) & 1u) | (lane << 1u)) << 16u) |
                                    0xFF000000u;
            out[static_cast<size_t>(vv) * texW + static_cast<size_t>(uu)] = packed;
        }
    return true;
}

// Evidence-limited admission for the consumer G280 identified. This deliberately does not
// generalize to arbitrary PSMT8/CT32 aliases: shape, source layout, page ownership, and CLUT
// independence must all match the current-binary census or the legacy materialize path wins.
static int g281TryEntry(uint32_t batchFbp, const G144Entry &e,
                        const G178EntryState &st)
{
    // The retained G262 census identifies this as the FST bilinear SPRITE consumer
    // (prim=6, cov=0..127), not the earlier shorthand's TRI_STRIP description.
    if (batchFbp != 0x143u || !e.prim.tme || !e.prim.fst ||
        e.prim.type != GS_PRIM_SPRITE || !st.bilinear ||
        e.ctx.tex0.psm != GS_PSM_T8 || e.ctx.tex0.tbp0 != 0x2820u ||
        e.ctx.tex0.tbw != 2u || st.texW != 128 || st.texH != 128)
    {
        g_g281RejShape.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    g_g281Candidates.fetch_add(1u, std::memory_order_relaxed);
    const int srcTi = g248TargetIndex(0x13cu);
    if (srcTi < 0 || !g_g261Res[srcTi].dirty || g_g261Res[srcTi].fbw != 2u ||
        g_g261Res[srcTi].fbW != 128)
    {
        g_g281RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid)
    {
        g_g281RejClut.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            (!g_g261Res[t].range.valid ||
             g144RangeOverlaps(clut, g_g261Res[t].range)))
        {
            g_g281RejClut.fetch_add(1u, std::memory_order_relaxed);
            return -1;
        }
    static std::vector<uint32_t> s_probeMap;
    if (!g281BuildT8Map(e, srcTi, st.texW, st.texH, s_probeMap))
    {
        g_g281RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    return srcTi;
}

static bool g280VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G280_VERIFY");
    return s_on;
}

static void g280DropOverlays(int ti, uint64_t *)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount) || g_g280Ovl[ti].empty())
        return;
    g_g280Drops.fetch_add(g_g280Ovl[ti].size(), std::memory_order_relaxed);
    g_g280Ovl[ti].clear();
}

// Overwrite one overlay page tile in the DST FBO with the current guest VRAM bytes (the page
// authority whenever the src FBO does not own it). 64x32 CT32 tile packed in GL bottom-first
// row order through the DST layout's swizzle — the exact inverse of g261Materialize's writeback.
static bool g280RestoreOverlayTile(int dstTi, const G280Ovl &o)
{
    const G261Res &r = g_g261Res[dstTi];
    uint8_t *vram = g_g144LastVram;
    const int x0 = static_cast<int>(o.dstCol) * 64;
    const int y0 = static_cast<int>(o.dstRow) * 32;
    if (vram == nullptr || g_g261VramSize == 0u || r.fbW <= 0 || r.fbw == 0u ||
        x0 + 64 > r.fbW || y0 + 32 > 512)
        return false;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[dstTi]);
    static std::vector<uint32_t> s_px; // flush-owner thread only
    s_px.assign(64u * 32u, 0u);
    for (int gy = 0; gy < 32; ++gy)
    {
        const size_t bufRow = static_cast<size_t>(31 - gy) * 64u; // GL rows are bottom-first
        for (int gx = 0; gx < 64; ++gx)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw,
                                                        static_cast<uint32_t>(x0 + gx),
                                                        static_cast<uint32_t>(y0 + gy));
            if (off + 4u > g_g261VramSize)
                return false;
            std::memcpy(&s_px[bufRow + static_cast<size_t>(gx)], vram + off, 4);
        }
    }
    return g264_backend_write_color_rect(kG261Fbp[dstTi], r.fbW, 512, x0, 512 - 32 - y0,
                                         64, 32, s_px);
}

// The DST target is about to publish its dirty rows to VRAM. Overlay tiles are a read cache of
// pages the dst view does NOT own — re-sync them from VRAM first so the publication cannot
// write alias-page bytes whose authority is the src FBO (still dirty, publishes at its own
// edge) or VRAM itself. Returns false on any restore failure: the caller must take the loud
// residency-drop path WITHOUT a readback (same contract as a G264 mirror failure).
static bool g280PrepareMaterialize(int ti)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return true;
    auto &ovl = g_g280Ovl[ti];
    if (ovl.empty())
        return true;
    bool ok = true;
    for (const G280Ovl &o : ovl)
    {
        if (g280RestoreOverlayTile(ti, o))
            g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            ok = false;
            const uint64_t n = g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
                std::fprintf(stderr, "[G280:restore-fail] n=%llu dst=%03x page=0x%x\n",
                             static_cast<unsigned long long>(n), kG261Fbp[ti], o.pageBlock);
        }
    }
    g_g280Drops.fetch_add(ovl.size(), std::memory_order_relaxed);
    ovl.clear();
    ++g_g280ContentGen[ti]; // the restores changed the dst FBO content
    return ok;
}

// A target's FBO content changed through a wave render or a full fb upload over rows
// [rowLo..rowHi]. Bump its version (staling every overlay copied FROM it) and drop any of ITS
// OWN overlay tiles the write overlaps: the resolve reproduced the physical page state of its
// moment, so later dst draws layer on top exactly as GS command order would — the tile becomes
// ordinary dst content and publishes with the dst view from now on.
static void g280NoteFboContentChange(int ti, int rowLo, int rowHi)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    ++g_g280ContentGen[ti];
    g283NoteTargetPages(ti, rowLo, rowHi); // G283: this write is the new authority for its pages
    auto &ovl = g_g280Ovl[ti];
    for (size_t k = ovl.size(); k-- > 0;)
    {
        const int ty0 = static_cast<int>(ovl[k].dstRow) * 32;
        if (ty0 + 31 >= rowLo && ty0 <= rowHi)
        {
            g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
            ovl[k] = ovl.back();
            ovl.pop_back();
        }
    }
}

// A G264 mirror patched VRAM-authoritative upload bytes into this target's FBO rows [lo..hi].
// Overlay tiles the patch overlaps are re-synced from VRAM and dropped: VRAM is the newest
// authority for the patched pixels and the tile may now be a mix of mirror and resolve bytes.
static void g280NoteMirrorPatch(int ti, int lo, int hi)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    ++g_g280ContentGen[ti];
    g283NoteTargetPages(ti, lo, hi); // G283: VRAM-authoritative mirror is the new page authority
    auto &ovl = g_g280Ovl[ti];
    for (size_t k = ovl.size(); k-- > 0;)
    {
        const int ty0 = static_cast<int>(ovl[k].dstRow) * 32;
        if (ty0 + 31 < lo || ty0 > hi)
            continue;
        if (g280RestoreOverlayTile(ti, ovl[k]))
            g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            const uint64_t n = g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
                std::fprintf(stderr, "[G280:restore-fail] mirror n=%llu dst=%03x page=0x%x\n",
                             static_cast<unsigned long long>(n), kG261Fbp[ti],
                             ovl[k].pageBlock);
        }
        g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
        ovl[k] = ovl.back();
        ovl.pop_back();
    }
}

// Once per flush while overlays exist: any entry whose src target is no longer dirty (VRAM
// became the page authority when the src published) or whose src contentGen moved (the copy is
// stale) is re-synced from VRAM and dropped. A consumer the admission path handles will simply
// re-resolve fresh; a legacy-path bind in between then reads exactly what it reads today
// without G280 (VRAM-consistent bytes) — never a stale resolve.
static void g280HygienePass()
{
    for (uint32_t ti = 0; ti < kG248TargetCount; ++ti)
    {
        auto &ovl = g_g280Ovl[ti];
        if (ovl.empty())
            continue;
        for (size_t k = ovl.size(); k-- > 0;)
        {
            const G280Ovl &o = ovl[k];
            if (o.srcTi < kG248TargetCount && g_g261Res[o.srcTi].dirty &&
                o.srcGen == g_g280ContentGen[o.srcTi] &&
                (!g283AuthorityOn() || o.pageEpoch == g283PageEpoch(o.pageBlock)))
                continue; // still current (G283: physical-page authority unchanged too)
            if (g283AuthorityOn() && o.srcTi < kG248TargetCount &&
                g_g261Res[o.srcTi].dirty && o.srcGen == g_g280ContentGen[o.srcTi])
                g_g283Stale.fetch_add(1u, std::memory_order_relaxed); // peer alias wrote the page
            if (g280RestoreOverlayTile(static_cast<int>(ti), o))
                g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
            else
            {
                const uint64_t n =
                    g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u)
                    std::fprintf(stderr,
                                 "[G280:restore-fail] hygiene n=%llu dst=%03x page=0x%x\n",
                                 static_cast<unsigned long long>(n), kG261Fbp[ti],
                                 o.pageBlock);
            }
            g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
            ovl[k] = ovl.back();
            ovl.pop_back();
        }
        ++g_g280ContentGen[ti];
    }
}

// Conservative inclusive texel tap bounds {u0,u1,v0,v1} for the admitted FST consumer shapes,
// proven against the DST residency exactly like the existing bind rules (NEAREST texel extents,
// G263 bilinear sprite taps, G267 bilinear triangle hull). Returns false for any other shape.
static bool g280TapBounds(const G144Entry &e, const G178EntryState &st, const G261Res &r,
                          int taps[4])
{
    const bool triType = e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
                         e.prim.type == GS_PRIM_TRIFAN;
    if (e.prim.type == GS_PRIM_SPRITE)
    {
        if (st.bilinear)
            return g263BilinearSpriteBind(e, st, r, taps);
        int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
        for (int k = 0; k < 2; ++k)
        {
            const int uu = static_cast<int>(e.v[k].u >> 4);
            const int vv = static_cast<int>(e.v[k].v >> 4);
            u0 = std::min(u0, uu); u1 = std::max(u1, uu);
            v0 = std::min(v0, vv); v1 = std::max(v1, vv);
        }
        // Exclusive right/bottom endpoints (drawSprite): max tap is u1-1 / v1-1.
        if (!(u0 >= 0 && u1 <= r.fbW && v0 >= r.covLo && v1 <= r.covHi + 1) ||
            u1 - 1 < u0 || v1 - 1 < v0)
            return false;
        taps[0] = u0; taps[1] = u1 - 1; taps[2] = v0; taps[3] = v1 - 1;
        return true;
    }
    if (triType)
    {
        if (st.bilinear)
        {
            G267TriFootprint fp;
            if (!g267BilinearTriFootprint(e, st, r, fp))
                return false;
            taps[0] = fp.tapLoU; taps[1] = fp.tapHiU;
            taps[2] = fp.tapLoV; taps[3] = fp.tapHiV;
            return true;
        }
        int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
        for (int k = 0; k < 3; ++k)
        {
            const int uu = static_cast<int>(e.v[k].u >> 4);
            const int vv = static_cast<int>(e.v[k].v >> 4);
            u0 = std::min(u0, uu); u1 = std::max(u1, uu);
            v0 = std::min(v0, vv); v1 = std::max(v1, vv);
        }
        if (!(u0 >= 0 && u1 <= r.fbW - 1 && v0 >= r.covLo && v1 <= r.covHi))
            return false;
        taps[0] = u0; taps[1] = u1; taps[2] = v0; taps[3] = v1;
        return true;
    }
    return false;
}

static uint32_t g289OwnerSourceToken(const G144Entry &e)
{
    const bool ct24 = e.ctx.tex0.psm == GS_PSM_CT24;
    if (e.ctx.tex0.tbp0 == 0x2720u)
        return ct24 ? kG289AliasCt24Token : kG289AliasCt32Token;
    if (e.ctx.tex0.tbp0 == 0x2760u)
        return ct24 ? kG289WorkCt24Token : kG289WorkCt32Token;
    return 0u;
}

static bool g289IsOwnerSourceToken(uint32_t token)
{
    return token == kG289AliasCt32Token || token == kG289AliasCt24Token ||
           token == kG289WorkCt24Token || token == kG289WorkCt32Token;
}

static bool g289TriangleHasArea(const G144Entry &e)
{
    const int64_t ax = static_cast<int64_t>(e.v[1].x) - e.v[0].x;
    const int64_t ay = static_cast<int64_t>(e.v[1].y) - e.v[0].y;
    const int64_t bx = static_cast<int64_t>(e.v[2].x) - e.v[0].x;
    const int64_t by = static_cast<int64_t>(e.v[2].y) - e.v[0].y;
    return ax * by - ay * bx != 0;
}

static bool g289OwnerTapBounds(const G144Entry &e, const G178EntryState &st, int taps[4])
{
    float loU = FLT_MAX, hiU = -FLT_MAX, loV = FLT_MAX, hiV = -FLT_MAX;
    for (int k = 0; k < 3; ++k)
    {
        const float u = static_cast<float>(e.v[k].u) / 16.0f;
        const float v = static_cast<float>(e.v[k].v) / 16.0f;
        if (!std::isfinite(u) || !std::isfinite(v))
            return false;
        loU = std::min(loU, u); hiU = std::max(hiU, u);
        loV = std::min(loV, v); hiV = std::max(hiV, v);
    }
    if (st.bilinear)
    {
        taps[0] = static_cast<int>(std::floor(loU - 0.5f - 0.001f));
        taps[1] = static_cast<int>(std::ceil (hiU - 0.5f + 0.001f));
        taps[2] = static_cast<int>(std::floor(loV - 0.5f - 0.001f));
        taps[3] = static_cast<int>(std::ceil (hiV - 0.5f + 0.001f));
    }
    else
    {
        taps[0] = static_cast<int>(std::floor(loU));
        taps[1] = static_cast<int>(std::floor(hiU));
        taps[2] = static_cast<int>(std::floor(loV));
        taps[3] = static_cast<int>(std::floor(hiV));
    }
    const bool clampU = (e.ctx.clamp & 3u) == 1u;
    const bool clampV = ((e.ctx.clamp >> 2u) & 3u) == 1u;
    if (clampU)
    {
        taps[0] = std::clamp(taps[0], 0, 511);
        taps[1] = std::clamp(taps[1], 0, 511);
    }
    if (clampV)
    {
        taps[2] = std::clamp(taps[2], 0, 511);
        taps[3] = std::clamp(taps[3], 0, 511);
    }
    return taps[0] <= taps[1] && taps[2] <= taps[3];
}

// Exact second-stage admission for the final-composition owner view. Every actual texture tap
// must name one of the 104 copied CT32 pages (0x2760..0x345f). Degenerate triangles sample
// nothing and may carry the source token without a footprint. The 0x2720 alias needs a private
// synthetic FBO view; it never mutates the real 0x139 target.
static bool g289PrepareOwnerViewSources(const std::vector<G178EntryState> &states,
                                        std::vector<uint32_t> &sources,
                                        uint64_t &binds, bool &builtAlias)
{
    sources.assign(g_g144List.size(), 0u);
    binds = 0u;
    builtAlias = false;
    const auto reject = [&](const char *stage, size_t i, const G144Entry *e,
                            const int *taps, uint64_t page) {
        if (g289StatOn())
        {
            static std::atomic<uint32_t> s_logs{0};
            const uint32_t n = s_logs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(
                    stderr,
                    "[G289:view-detail] n=%u stage=%s entry=%zu/%zu "
                    "tex=%x/%u/%x uv=(%u,%u)(%u,%u)(%u,%u) "
                    "taps=%d..%d,%d..%d page=%llx\n",
                    n + 1u, stage, i, g_g144List.size(),
                    e ? e->ctx.tex0.tbp0 : 0u,
                    e ? static_cast<unsigned>(e->ctx.tex0.tbw) : 0u,
                    e ? e->ctx.tex0.psm : 0u,
                    e ? static_cast<unsigned>(e->v[0].u) : 0u,
                    e ? static_cast<unsigned>(e->v[0].v) : 0u,
                    e ? static_cast<unsigned>(e->v[1].u) : 0u,
                    e ? static_cast<unsigned>(e->v[1].v) : 0u,
                    e ? static_cast<unsigned>(e->v[2].u) : 0u,
                    e ? static_cast<unsigned>(e->v[2].v) : 0u,
                    taps ? taps[0] : 0, taps ? taps[1] : 0,
                    taps ? taps[2] : 0, taps ? taps[3] : 0,
                    static_cast<unsigned long long>(page));
        }
        return false;
    };
    bool needAlias = false;
    const G144BlockRange owner = g289OwnerRange();
    if (!owner.valid)
        return reject("owner", 0u, nullptr, nullptr, 0u);

    for (size_t i = 0u; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        const G178EntryState &st = states[i];
        if (st.skip || !e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        if (!tex.valid || !g144RangeOverlaps(owner, tex))
            continue;
        if (!g289OwnerAliasTuple(e))
            return reject("tuple", i, &e, nullptr, 0u);
        const uint32_t token = g289OwnerSourceToken(e);
        if (token == 0u)
            return reject("token", i, &e, nullptr, 0u);
        if (g289TriangleHasArea(e))
        {
            int taps[4];
            if (!g289OwnerTapBounds(e, st, taps))
                return reject("footprint", i, &e, taps, 0u);
            bool xPages[8] = {}, yPages[16] = {};
            const auto markPages = [](int lo, int hi, int pageSize, int pageCount,
                                      bool clamp, bool *pages) {
                if (clamp)
                {
                    lo = std::clamp(lo, 0, 511);
                    hi = std::clamp(hi, 0, 511);
                }
                const int span = hi - lo + 1;
                if (!clamp && span >= 512)
                {
                    for (int p = 0; p < pageCount; ++p)
                        pages[p] = true;
                    return;
                }
                for (int coord = lo; coord <= hi; ++coord)
                {
                    int wrapped = coord;
                    if (!clamp)
                    {
                        wrapped %= 512;
                        if (wrapped < 0)
                            wrapped += 512;
                    }
                    pages[wrapped / pageSize] = true;
                }
            };
            markPages(taps[0], taps[1], 64, 8, st.wrapU == 1u, xPages);
            markPages(taps[2], taps[3], 32, 16, st.wrapV == 1u, yPages);
            for (uint32_t yp = 0u; yp < 16u; ++yp)
                for (uint32_t xp = 0u; xp < 8u; ++xp)
                {
                    if (!yPages[yp] || !xPages[xp])
                        continue;
                    const uint64_t page = static_cast<uint64_t>(e.ctx.tex0.tbp0) +
                        (static_cast<uint64_t>(yp) * 8u + xp) * 32u;
                    if (page < owner.first || page + 31u > owner.last)
                        return reject("page", i, &e, taps, page);
                }
        }
        sources[i] = token;
        needAlias |= e.ctx.tex0.tbp0 == 0x2720u;
        ++binds;
    }
    if (binds == 0u)
        return reject("empty", 0u, nullptr, nullptr, 0u);
    if (needAlias)
    {
        if (!g289_backend_build_work_alias_view())
            return reject("backend", 0u, nullptr, nullptr, 0u);
        builtAlias = true;
    }
    return true;
}

// Attempt to fully handle one display-batch consumer entry that samples a dirty RTT target:
// admit the FBO-direct bind (adding the previously CPU-only bilinear TRISTRIP shape) and
// resolve every dirty-sibling physical-page alias with GPU->GPU page-tile copies. Returns the
// producer fbp to bind, or 0 to fall back to the legacy per-target bind/materialize loop
// (today's behavior, bit-exact). Fail-closed on: unproven footprints, unknown sibling windows,
// non-admitted alias tuples, misaligned/out-of-range pages, src tiles not provably
// FBO-defined, and backend copy failure — none of which mutate any state before returning 0.
static uint32_t g280TryEntry(uint32_t batchFbp, const G144Entry &e, const G178EntryState &st)
{
    if (g248TargetIndex(batchFbp) >= 0)
        return 0u; // display consumers only; RTT-family internal consumers keep existing rules
    if (!e.prim.tme || !e.prim.fst || e.ctx.tex0.psm != GS_PSM_CT32)
        return 0u;
    int dstTi = -1;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            e.ctx.tex0.tbp0 == framePageBaseToBlock(kG261Fbp[t]) &&
            e.ctx.tex0.tbw == g_g261Res[t].fbw)
        {
            dstTi = static_cast<int>(t);
            break;
        }
    if (dstTi < 0)
        return 0u;
    const G261Res &dst = g_g261Res[dstTi];
    int taps[4];
    if (!g280TapBounds(e, st, dst, taps))
    {
        g_g280RejFoot.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    const uint32_t dstBase = e.ctx.tex0.tbp0;
    const uint32_t dstBw = std::max<uint32_t>(1u, e.ctx.tex0.tbw);
    const uint32_t xp0 = static_cast<uint32_t>(std::max(taps[0], 0)) / 64u;
    const uint32_t xp1 = static_cast<uint32_t>(std::max(taps[1], 0)) / 64u;
    const uint32_t yp0 = static_cast<uint32_t>(std::max(taps[2], 0)) / 32u;
    const uint32_t yp1 = static_cast<uint32_t>(std::max(taps[3], 0)) / 32u;
    static std::vector<G280Ovl> s_newOvl; // overlay records committed on copy success
    s_newOvl.clear();
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        if (static_cast<int>(t) == dstTi || !g_g261Res[t].dirty)
            continue;
        const G261Res &src = g_g261Res[t];
        if (!src.range.valid)
        {
            g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
            return 0u; // unknown sibling window — fail closed
        }
        bool anyOverlap = false;
        for (uint32_t yp = yp0; yp <= yp1; ++yp)
            for (uint32_t xp = xp0; xp <= xp1; ++xp)
            {
                const uint64_t first64 = static_cast<uint64_t>(dstBase) +
                                         (static_cast<uint64_t>(yp) * dstBw + xp) * 32u;
                if (first64 > 0x3FFFu)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const G144BlockRange page{
                    static_cast<uint32_t>(first64),
                    static_cast<uint32_t>(std::min<uint64_t>(first64 + 31u, 0x3FFFu)), true};
                if (!g144RangeOverlaps(page, src.range))
                    continue;
                anyOverlap = true;
                // Evidence-limited tuple admission: dst is the 0x139 CT32 atlas view; src is
                // any of ITS four measured CT32 work blocks (G267 proved 0x146 page 0x2920;
                // the G280 bindrej census proved 0x13c page 0x2820 in the same strip today —
                // the mapping below is identical for every family member because CT32
                // within-page pixel order is layout-independent).
                if (kG261Fbp[static_cast<uint32_t>(dstTi)] != 0x139u)
                {
                    g_g280RejTuple.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const uint32_t pageBlock = static_cast<uint32_t>(first64);
                const uint32_t srcBaseBlk = framePageBaseToBlock(kG261Fbp[t]);
                if (pageBlock < srcBaseBlk || ((pageBlock - srcBaseBlk) & 31u) != 0u ||
                    src.fbw == 0u || src.fbW <= 0)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const uint32_t srcOff = (pageBlock - srcBaseBlk) >> 5u;
                const uint32_t srcRow = srcOff / src.fbw;
                const uint32_t srcCol = srcOff % src.fbw;
                const int sy0 = static_cast<int>(srcRow) * 32;
                if (static_cast<int>(srcCol + 1u) * 64 > src.fbW || sy0 + 32 > 512 ||
                    sy0 < src.covLo || sy0 + 31 > src.covHi)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u; // src tile not provably FBO-defined under the src view
                }
                // G294: copy this page ONLY from its current authoritative owner. A DIVERGENT
                // sibling (owner recorded and it is a different target — e.g. the dst 0x139 or a
                // peer wrote the page more recently) is skipped, and the dst keeps its existing
                // overlay/VRAM bytes for the page (fail-closed). This is the exact fix for G292's
                // multi-owner page-0x2860 mismatch, where two aliasing siblings both copied in and
                // the later copy overwrote the earlier. owner<0 (unknown) keeps the pre-G294
                // single-owner behavior (already bit-exact, G280 vfyBad=0 without a multi-owner).
                if (g294OwnerTokenOn())
                {
                    const int owner = g294PageOwner(pageBlock);
                    if (owner >= 0 && owner != static_cast<int>(t))
                    {
                        g_g294NonOwnerSkip.fetch_add(1u, std::memory_order_relaxed);
                        continue; // authoritative owner is elsewhere
                    }
                    if (owner == static_cast<int>(t))
                        g_g294OwnerSel.fetch_add(1u, std::memory_order_relaxed);
                }
                bool current = false;
                for (const G280Ovl &o : g_g280Ovl[dstTi])
                    if (o.pageBlock == pageBlock && o.srcTi == static_cast<uint8_t>(t) &&
                        o.srcGen == g_g280ContentGen[t] &&
                        (!g283AuthorityOn() || o.pageEpoch == g283PageEpoch(pageBlock)))
                    {
                        current = true;
                        break;
                    }
                if (current)
                    continue;
                G280Ovl o;
                o.pageBlock = pageBlock;
                o.srcTi = static_cast<uint8_t>(t);
                o.srcGen = g_g280ContentGen[t];
                o.pageEpoch = g283PageEpoch(pageBlock); // G283: authority epoch at resolve time
                o.dstRow = static_cast<uint16_t>(yp);
                o.dstCol = static_cast<uint16_t>(xp);
                o.srcRow = static_cast<uint16_t>(srcRow);
                o.srcCol = static_cast<uint16_t>(srcCol);
                s_newOvl.push_back(o);
            }
        if (!anyOverlap)
            g_g280SibSkips.fetch_add(1u, std::memory_order_relaxed);
    }
    bool copiesOk = true;
    if (!s_newOvl.empty())
    {
        // One batched copy job per distinct src target (a strip can alias several work blocks
        // of the same atlas — the census measured 0x13c AND 0x146 in one footprint). Overlay
        // records are committed for EVERY attempted job, including a failed one: the tiles'
        // dst FBO content is then unknown, and the committed record is exactly what forces a
        // VRAM re-sync before any dst publication or later bind (hygiene/materialize paths).
        static std::vector<int32_t> s_rects;
        auto commitOvl = [&](uint8_t srcT) {
            for (const G280Ovl &o : s_newOvl)
            {
                if (o.srcTi != srcT)
                    continue;
                // G292: this GPU copy changed the dst tile without changing guest VRAM.
                g291ClearCopiedTile(dstTi, o.dstRow, o.dstCol);
                bool replaced = false;
                for (G280Ovl &ex : g_g280Ovl[dstTi])
                    if (ex.pageBlock == o.pageBlock)
                    {
                        ex = o;
                        replaced = true;
                        break;
                    }
                if (!replaced)
                    g_g280Ovl[dstTi].push_back(o);
            }
        };
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            s_rects.clear();
            for (const G280Ovl &o : s_newOvl)
            {
                if (o.srcTi != static_cast<uint8_t>(t))
                    continue;
                s_rects.push_back(static_cast<int32_t>(o.srcCol) * 64);
                s_rects.push_back(512 - 32 - static_cast<int32_t>(o.srcRow) * 32);
                s_rects.push_back(static_cast<int32_t>(o.dstCol) * 64);
                s_rects.push_back(512 - 32 - static_cast<int32_t>(o.dstRow) * 32);
                s_rects.push_back(64);
                s_rects.push_back(32);
            }
            if (s_rects.empty())
                continue;
            if (!g280_backend_copy_color_rects(kG261Fbp[t],
                                               kG261Fbp[static_cast<uint32_t>(dstTi)],
                                               s_rects))
            {
                copiesOk = false;
                const uint64_t n =
                    g_g280CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u)
                    std::fprintf(stderr,
                                 "[G280:copy-fail] n=%llu dst=%03x src=%03x rects=%zu\n",
                                 static_cast<unsigned long long>(n),
                                 kG261Fbp[static_cast<uint32_t>(dstTi)], kG261Fbp[t],
                                 s_rects.size() / 6u);
                // Stale the failed tiles so no freshness test can call them current, then
                // force an immediate VRAM re-sync of everything just committed.
                for (G280Ovl &o : s_newOvl)
                    if (o.srcTi == static_cast<uint8_t>(t))
                        o.srcGen = g_g280ContentGen[t] - 1u;
            }
            commitOvl(static_cast<uint8_t>(t));
        }
        if (!copiesOk)
        {
            g280HygienePass(); // restores the failed/unknown tiles from VRAM and drops them
            return 0u;         // legacy path then reproduces today's materialize behavior
        }
        g_g280Copies.fetch_add(s_newOvl.size(), std::memory_order_relaxed);
        if (g280VerifyOn())
        {
            // Exactness check of the mapping/copy itself: both FBO tiles must be bit-identical.
            static std::vector<uint32_t> s_vs, s_vd;
            for (const G280Ovl &o : s_newOvl)
            {
                const G261Res &src = g_g261Res[o.srcTi];
                const int sGlY = 512 - 32 - static_cast<int>(o.srcRow) * 32;
                const int dGlY = 512 - 32 - static_cast<int>(o.dstRow) * 32;
                if (!g178_backend_read_color(kG261Fbp[o.srcTi], src.fbW, 512, sGlY, 32, s_vs) ||
                    !g178_backend_read_color(kG261Fbp[static_cast<uint32_t>(dstTi)], dst.fbW,
                                             512, dGlY, 32, s_vd))
                    continue;
                uint64_t bad = 0;
                for (int ry = 0; ry < 32; ++ry)
                    for (int rx = 0; rx < 64; ++rx)
                        if (s_vs[static_cast<size_t>(ry) * src.fbW +
                                 static_cast<size_t>(o.srcCol) * 64u + rx] !=
                            s_vd[static_cast<size_t>(ry) * dst.fbW +
                                 static_cast<size_t>(o.dstCol) * 64u + rx])
                            ++bad;
                if (bad != 0u)
                {
                    const uint64_t tot =
                        g_g280VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
                    std::fprintf(stderr,
                                 "[G280:vfy] MISMATCH page=0x%x bad=%llu tot=%llu "
                                 "src=%03x(%u,%u) dst=%03x(%u,%u)\n",
                                 o.pageBlock, static_cast<unsigned long long>(bad),
                                 static_cast<unsigned long long>(tot), kG261Fbp[o.srcTi],
                                 o.srcCol, o.srcRow,
                                 kG261Fbp[static_cast<uint32_t>(dstTi)], o.dstCol, o.dstRow);
                }
            }
        }
    }
    g_g280Binds.fetch_add(1u, std::memory_order_relaxed);
    return kG261Fbp[static_cast<uint32_t>(dstTi)];
}

static bool g282EntryBounds(const G144Entry &e, const G178EntryState &st,
                            int &x0, int &y0, int &x1, int &y1)
{
    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    if (e.prim.type == GS_PRIM_SPRITE)
    {
        int ax = static_cast<int>(e.v[0].x) - ofx;
        int ay = static_cast<int>(e.v[0].y) - ofy;
        int bx = static_cast<int>(e.v[1].x) - ofx;
        int by = static_cast<int>(e.v[1].y) - ofy;
        if (ax > bx) std::swap(ax, bx);
        if (ay > by) std::swap(ay, by);
        x0 = ax;
        y0 = ay;
        x1 = ax + std::max(1, bx - ax) - 1;
        y1 = ay + std::max(1, by - ay) - 1;
    }
    else if (e.prim.type == GS_PRIM_TRIANGLE ||
             e.prim.type == GS_PRIM_TRISTRIP ||
             e.prim.type == GS_PRIM_TRIFAN)
    {
        float minX = e.v[0].x, maxX = e.v[0].x;
        float minY = e.v[0].y, maxY = e.v[0].y;
        for (int k = 1; k < 3; ++k)
        {
            minX = std::min(minX, e.v[k].x);
            maxX = std::max(maxX, e.v[k].x);
            minY = std::min(minY, e.v[k].y);
            maxY = std::max(maxY, e.v[k].y);
        }
        x0 = static_cast<int>(std::floor(minX)) - ofx;
        y0 = static_cast<int>(std::floor(minY)) - ofy;
        x1 = static_cast<int>(std::ceil(maxX)) - ofx;
        y1 = static_cast<int>(std::ceil(maxY)) - ofy;
    }
    else
        return false;
    x0 = std::max(x0, static_cast<int>(st.scX0));
    y0 = std::max(y0, static_cast<int>(st.scY0));
    x1 = std::min(x1, static_cast<int>(st.scX1));
    y1 = std::min(y1, static_cast<int>(st.scY1));
    return x1 >= x0 && y1 >= y0;
}

// Structural admission, not a frame-count fingerprint: the first primitive must completely
// replace the thirteen measured CT32 page rows, and every later draw must stay inside that clear.
// Thus neither old color aliases nor untouched pixels are inputs to the new page contents.
static bool g282PrepareFanout(const std::vector<G178EntryState> &states,
                              bool guestDepth, bool guestDepthWrites,
                              uint32_t guestZbpPage, uint32_t guestZpsm,
                              std::vector<G282Fanout> &out)
{
    auto logReject = [&](const char *why, size_t i, int x0, int y0, int x1, int y1) {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (g282StatOn() && n <= 16u)
            std::fprintf(stderr,
                         "[G282:admit-rej] n=%llu why=%s i=%zu bounds=%d,%d..%d,%d entries=%zu\n",
                         static_cast<unsigned long long>(n), why, i,
                         x0, y0, x1, y1, g_g144List.size());
    };
    out.clear();
    if (!guestDepth || guestDepthWrites || guestZbpPage != 0x0d0u ||
        guestZpsm != 1u || g_g144List.size() < 2u ||
        states.size() != g_g144List.size())
    {
        logReject("family", 0u, 0, 0, -1, -1);
        return false;
    }
    const G144Entry &clear = g_g144List.front();
    const G178EntryState &clearSt = states.front();
    int clearX0 = 0, clearY0 = 0, clearX1 = -1, clearY1 = -1;
    if (clear.prim.type != GS_PRIM_SPRITE || clear.prim.tme || clear.prim.abe ||
        clear.ctx.frame.fbmsk != 0u || clearSt.guestDepth || clearSt.skip ||
        clear.v[0].r != 0 || clear.v[0].g != 0 ||
        clear.v[0].b != 0 || clear.v[0].a != 0 ||
        clear.v[1].r != 0 || clear.v[1].g != 0 ||
        clear.v[1].b != 0 || clear.v[1].a != 0 ||
        !g282EntryBounds(clear, clearSt, clearX0, clearY0, clearX1, clearY1) ||
        clearX0 != 0 || clearY0 != 0 || clearX1 != 511 || clearY1 != 415)
    {
        logReject("clear", 0u, clearX0, clearY0, clearX1, clearY1);
        return false;
    }
    for (size_t i = 1; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        const G178EntryState &st = states[i];
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        if (st.skip)
            continue;
        if (e.prim.type != GS_PRIM_TRIANGLE &&
            e.prim.type != GS_PRIM_TRISTRIP &&
            e.prim.type != GS_PRIM_TRIFAN)
        {
            logReject("prim", i, x0, y0, x1, y1);
            return false;
        }
        if (e.prim.tme || !e.prim.abe || !st.guestDepth || st.depthWrite)
        {
            logReject("draw", i, x0, y0, x1, y1);
            return false;
        }
        if (!g282EntryBounds(e, st, x0, y0, x1, y1))
            continue; // fully scissored primitive: no color write and no depth read
        if (x0 < clearX0 || x1 > clearX1 || y0 < 32 || y1 > 447)
        {
            logReject("bounds", i, x0, y0, x1, y1);
            return false;
        }
    }

    const int srcTi = g248TargetIndex(0x139u);
    if (srcTi < 0)
        return false;
    // A row-scoped source write will drop its overlay records. Fail closed if one exists in the
    // affected rows but outside the fully-overwritten page rectangle.
    for (const G280Ovl &o : g_g280Ovl[srcTi])
        if (static_cast<int>(o.dstRow) * 32 <= clearY1 &&
            static_cast<int>(o.dstRow) * 32 + 31 >= clearY0 &&
            (static_cast<int>(o.dstCol) * 64 < clearX0 ||
             static_cast<int>(o.dstCol) * 64 + 63 > clearX1))
        {
            g_g282RejOwner.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }

    // G283: with the physical-page authority epoch, more than one dirty sibling may own a page.
    // Fan the completed 0x139 batch bytes to EVERY dirty owner (they are all byte-identical after
    // the copy) and let the epoch gate invalidate whichever alias is next written independently.
    // Authority OFF keeps G282's proven single-owner fail-close (bit-exact default behavior).
    const bool authority = g283AuthorityOn();
    const uint32_t srcBase = framePageBaseToBlock(0x139u);
    for (uint32_t py = 0; py < 13u; ++py)
        for (uint32_t px = 0; px < 8u; ++px)
        {
            const uint32_t pageBlock = srcBase + (py * 8u + px) * 32u;
            int owners = 0;
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                if (static_cast<int>(t) == srcTi)
                    continue;
                uint32_t row = 0u, col = 0u;
                if (!g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock, &row, &col))
                    continue;
                if (owners >= 1 && !authority)
                {
                    // Multiple dirty sibling publishers for one physical page need the G283
                    // authority mechanism; without it, admit only the measured single owner.
                    out.clear();
                    g_g282RejOwner.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                if (owners >= 1)
                    g_g283MultiOwner.fetch_add(1u, std::memory_order_relaxed);
                ++owners;
                G282Fanout f;
                f.pageBlock = pageBlock;
                f.dstTi = static_cast<uint8_t>(t);
                f.srcRow = static_cast<uint16_t>(py);
                f.srcCol = static_cast<uint16_t>(px);
                f.dstRow = static_cast<uint16_t>(row);
                f.dstCol = static_cast<uint16_t>(col);
                out.push_back(f);
            }
        }
    return true;
}

static bool g282ExecuteFanout(const std::vector<G282Fanout> &plan)
{
    static std::vector<int32_t> s_rects;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        s_rects.clear();
        for (const G282Fanout &f : plan)
        {
            if (f.dstTi != static_cast<uint8_t>(t))
                continue;
            s_rects.push_back(static_cast<int32_t>(f.srcCol) * 64);
            s_rects.push_back(512 - 32 - static_cast<int32_t>(f.srcRow) * 32);
            s_rects.push_back(static_cast<int32_t>(f.dstCol) * 64);
            s_rects.push_back(512 - 32 - static_cast<int32_t>(f.dstRow) * 32);
            s_rects.push_back(64);
            s_rects.push_back(32);
        }
        if (s_rects.empty())
            continue;
        if (!g280_backend_copy_color_rects(0x139u, kG261Fbp[t], s_rects))
        {
            const uint64_t n =
                g_g282FanoutFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::fprintf(stderr,
                         "[G282:fanout-fail] n=%llu dst=%03x rects=%zu\n",
                         static_cast<unsigned long long>(n), kG261Fbp[t],
                         s_rects.size() / 6u);
            return false;
        }
    }
    bool verifyOk = true;
    if (g282VerifyOn())
    {
        static std::vector<uint32_t> s_src, s_dst;
        for (const G282Fanout &f : plan)
        {
            const G261Res &dst = g_g261Res[f.dstTi];
            const int srcGlY = 512 - 32 - static_cast<int>(f.srcRow) * 32;
            const int dstGlY = 512 - 32 - static_cast<int>(f.dstRow) * 32;
            if (!g178_backend_read_color(0x139u, 512, 512, srcGlY, 32, s_src) ||
                !g178_backend_read_color(kG261Fbp[f.dstTi], dst.fbW, 512,
                                         dstGlY, 32, s_dst))
                continue;
            uint64_t bad = 0u;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x)
                    if (s_src[static_cast<size_t>(y) * 512u +
                              static_cast<size_t>(f.srcCol) * 64u + x] !=
                        s_dst[static_cast<size_t>(y) * dst.fbW +
                              static_cast<size_t>(f.dstCol) * 64u + x])
                        ++bad;
            if (bad != 0u)
            {
                verifyOk = false;
                const uint64_t tot =
                    g_g282VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
                std::fprintf(stderr,
                             "[G282:vfy] page=0x%x dst=%03x bad=%llu tot=%llu\n",
                             f.pageBlock, kG261Fbp[f.dstTi],
                             static_cast<unsigned long long>(bad),
                             static_cast<unsigned long long>(tot));
            }
        }
    }
    return verifyOk;
}

static void g282CommitFanout(const std::vector<G282Fanout> &plan)
{
    bool touched[kG248TargetCount] = {};
    for (const G282Fanout &f : plan)
    {
        const int ti = static_cast<int>(f.dstTi);
        G261Res &r = g_g261Res[ti];
        const int y0 = static_cast<int>(f.dstRow) * 32;
        r.dirty = true;
        r.dirtyLo = std::min(r.dirtyLo, y0);
        r.dirtyHi = std::max(r.dirtyHi, y0 + 31);
        r.covLo = std::min(r.covLo, y0);
        r.covHi = std::max(r.covHi, y0 + 31);
        // G292: fan-out copied the new 0x139 bytes into this sibling FBO tile; any prior
        // upload-re-coverage proof for the destination tile is now stale.
        g291ClearCopiedTile(ti, f.dstRow, f.dstCol);
        auto &ovl = g_g280Ovl[ti];
        for (size_t k = ovl.size(); k-- > 0;)
            if (ovl[k].pageBlock == f.pageBlock)
            {
                g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
                ovl[k] = ovl.back();
                ovl.pop_back();
            }
        touched[ti] = true;
        // G283: the completed 0x139 batch is the authoritative writer of this physical page; the
        // fresh epoch stales every alias overlay/copy so a later independent peer write is caught.
        if (g283AuthorityOn())
            g283BumpPageEpoch(f.pageBlock);
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (touched[t])
        {
            ++g_g280ContentGen[t];
            g261UpdateWindow(static_cast<int>(t));
        }
    g_g282FanoutPages.fetch_add(plan.size(), std::memory_order_relaxed);
    g_g283FanoutPages.fetch_add(plan.size(), std::memory_order_relaxed);
    if (g283StatOn())
    {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 12u || (n % 256u) == 0u)
            std::fprintf(stderr,
                         "[G283:fanout] n=%llu pages=%zu multiOwnerCum=%llu epochBumps=%llu "
                         "vfyBad=%llu\n",
                         static_cast<unsigned long long>(n), plan.size(),
                         (unsigned long long)g_g283MultiOwner.load(std::memory_order_relaxed),
                         (unsigned long long)g_g283Bumps.load(std::memory_order_relaxed),
                         (unsigned long long)g_g282VerifyBad.load(std::memory_order_relaxed));
    }
}

// After the source wave's normal row invalidation, reclassify each fanned page as a current
// read-cache overlay from its sibling owner. Both FBO tiles contain identical post-batch bytes;
// choosing the narrow sibling as authority prevents the wide 0x139 view from blocking G281.
static void g282CommitSourceOverlays(const std::vector<G282Fanout> &plan)
{
    const int srcTi = g248TargetIndex(0x139u);
    if (srcTi < 0)
        return;
    auto &ovl = g_g280Ovl[srcTi];
    for (const G282Fanout &f : plan)
    {
        G280Ovl o;
        o.pageBlock = f.pageBlock;
        o.srcTi = f.dstTi;
        o.srcGen = g_g280ContentGen[f.dstTi];
        o.pageEpoch = g283PageEpoch(f.pageBlock); // G283: current physical-page authority epoch
        o.dstRow = f.srcRow;
        o.dstCol = f.srcCol;
        o.srcRow = f.dstRow;
        o.srcCol = f.dstCol;
        bool replaced = false;
        for (G280Ovl &ex : ovl)
            if (ex.pageBlock == o.pageBlock)
            {
                ex = o;
                replaced = true;
                break;
            }
        if (!replaced)
            ovl.push_back(o);
    }
}

static void g282DropResidencyAfterFanoutFailure()
{
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        g280DropOverlays(static_cast<int>(t));
        G261Res &r = g_g261Res[t];
        r.dirty = false;
        r.dirtyLo = 512;
        r.dirtyHi = -1;
        r.covLo = 512;
        r.covHi = -1;
        r.mirPending = false;
        r.mirLo = 512;
        r.mirHi = -1;
        r.mirBits = {};
        r.g291Cov = {};
        r.g291CovFbw = 0u;
        g261UpdateWindow(static_cast<int>(t));
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[t]];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
}

static void g280Report()
{
    if (!g280AliasOn() && !g280StatOn() && !g281StatOn() &&
        g_g280Binds.load(std::memory_order_relaxed) == 0u)
        return;
    size_t live = 0;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        live += g_g280Ovl[t].size();
    std::fprintf(stderr,
                 "[G280:stat] binds=%llu copies=%llu copyFail=%llu restores=%llu "
                 "restoreFail=%llu drops=%llu sibSkip=%llu "
                 "rej(foot=%llu tuple=%llu page=%llu) vfyBad=%llu ovlLive=%zu\n",
                 (unsigned long long)g_g280Binds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Copies.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280CopyFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Restores.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RestoreFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Drops.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280SibSkips.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejFoot.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejTuple.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejPage.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280VerifyBad.load(std::memory_order_relaxed),
                 live);
    if (g281ViewOn() || g281StatOn() ||
        g_g281Views.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G281:stat] cand=%llu views=%llu buildFail=%llu "
                     "rej(shape=%llu page=%llu clut=%llu) vfyBad=%llu\n",
                     (unsigned long long)g_g281Candidates.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281Views.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281BuildFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejShape.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejPage.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejClut.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281VerifyBad.load(std::memory_order_relaxed));
    if (g282TightRowsOn() || g282StatOn() ||
        g_g282TightBatches.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G282:stat] cand=%llu tight=%llu aliasSaved=%llu "
                     "fanout=%llu fanoutFail=%llu rej(shape=%llu owner=%llu) vfyBad=%llu\n",
                     (unsigned long long)g_g282Candidates.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282TightBatches.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282AliasSaved.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282FanoutPages.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282FanoutFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282RejShape.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282RejOwner.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282VerifyBad.load(std::memory_order_relaxed));
    if (g283AuthorityOn() || g283StatOn() ||
        g_g283Bumps.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G283:stat] epochBumps=%llu stale=%llu multiOwner=%llu fanoutPages=%llu "
                     "pages=%zu\n",
                     (unsigned long long)g_g283Bumps.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283Stale.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283MultiOwner.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283FanoutPages.load(std::memory_order_relaxed),
                     g_g283Epoch.size());
    if (g294StatOn() || g294OwnerTokenOn())
        std::fprintf(stderr,
                     "[G294:stat] ownerToken=%d stamps=%llu ownerSel=%llu nonOwnerSkip=%llu "
                     "owners=%zu\n",
                     g294OwnerTokenOn() ? 1 : 0,
                     (unsigned long long)g_g294Stamps.load(std::memory_order_relaxed),
                     (unsigned long long)g_g294OwnerSel.load(std::memory_order_relaxed),
                     (unsigned long long)g_g294NonOwnerSkip.load(std::memory_order_relaxed),
                     g_g294Owner.size());
}

static void g261MaterializeAllDirty(int cause)
{
    if (!g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        if (g_g261Res[i].dirty)
            g261Materialize(static_cast<int>(i), cause);
    // Bring-up observability: this route rarely reaches the 512-materialize / 1024-wave report
    // triggers in a short capture, so emit a periodic report at a drain-boundary cadence when a
    // stat lever is on. Default runs never call this branch (gate returns false).
    if (g280StatOn() || g283StatOn())
    {
        static std::atomic<uint64_t> s_n{0};
        if ((s_n.fetch_add(1u, std::memory_order_relaxed) % 240u) == 0u)
            g261Report();
    }
}

// A VRAM transfer is about to read `src` and/or write `dst`: materialize any resident target it
// touches BEFORE the caller mutates/reads guest bytes. Unknown ranges fail closed (all targets).
static void g261MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause,
                                     uint32_t g264SkipMask = 0u)
{
    if (!g261WaveOn() || !g261AnyDirty())
        return;
    if (srcUnknown || dstUnknown)
    {
        g_g261MatUnknownRange.fetch_add(1u, std::memory_order_relaxed);
        g261MaterializeAllDirty(cause);
        return;
    }
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        if (g264SkipMask & (1u << i))
            continue; // G264: this overlap was deferred to an upload mirror instead
        if (!r.range.valid ||
            (src.valid && g144RangeOverlaps(r.range, src)) ||
            (dst.valid && g144RangeOverlaps(r.range, dst)))
            g261Materialize(static_cast<int>(i), cause);
    }
}

// G264: examine a host→local upload rect BEFORE it writes VRAM. Any dirty resident target it
// overlaps either gets the rect deferred as a mirror (returned in the mask — the caller skips
// its materialize) or falls through to the normal materialize. Fail-closed eligibility: CT32,
// matching buffer width, page-row-aligned base inside the target, rows inside 0..511, and
// full-width whenever the rows intersect DIRTY rows (a column-partial rect inside wave-rendered
// rows cannot be reproduced from VRAM alone — VRAM lacks the wave output in the other columns).
// Rows outside the dirty interval are VRAM-authoritative, so whole-row mirrors are exact there.
// G285: a full-page CT32 local copy can destructively consume a dirty resident destination.
// Publish the one partially covered page-row, then let the caller replace every remaining dirty
// page. Admission is generic GS-page geometry, never screen/game state. Source overlap fails
// closed because source bytes must be published before the copy reads them. G280 overlays also
// fail closed because their independent page authority is a separate mechanism.
static uint32_t g285PrepareL2lConsume(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                     uint32_t ssax, uint32_t ssay,
                                     uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                     uint32_t dsax, uint32_t dsay,
                                     uint32_t rrw, uint32_t rrh,
                                     const G144BlockRange &src,
                                     const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown)
{
    static const bool s_on = !envFlagEnabled("DC2_G285_NO_L2L_PAGE_COPY") &&
                             !envFlagDisabled("DC2_G285_L2L_PAGE_COPY");
    static const bool s_stat = envFlagEnabled("DC2_G285_STAT");
    if (!s_on || !g261WaveOn() || g280AliasOn() || !g261AnyDirty() ||
        srcUnknown || dstUnknown || !src.valid || !dst.valid ||
        spsm != GS_PSM_CT32 || dpsm != GS_PSM_CT32 ||
        sbw == 0u || dbw == 0u || sbw != dbw ||
        (sbp & 31u) != 0u || (dbp & 31u) != 0u ||
        ssax != 0u || ssay != 0u || dsax != 0u || dsay != 0u ||
        rrw != static_cast<uint32_t>(dbw) * 64u ||
        rrh == 0u || (rrh & 31u) != 0u)
        return 0u;

    constexpr uint64_t kPageBytes = 8192ull;
    const uint64_t copyBytes = static_cast<uint64_t>(rrw) * rrh * 4ull;
    const uint64_t srcBase = static_cast<uint64_t>(sbp) * 256ull;
    const uint64_t dstBase = static_cast<uint64_t>(dbp) * 256ull;
    const uint64_t srcEnd = srcBase + copyBytes;
    const uint64_t dstEnd = dstBase + copyBytes;
    if (srcEnd > g_g261VramSize || dstEnd > g_g261VramSize ||
        !(srcEnd <= dstBase || dstEnd <= srcBase))
        return 0u;

    uint32_t consumedMask = 0u;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty || !r.range.valid || r.fbw != dbw ||
            r.fbW != static_cast<int>(rrw) ||
            (r.dirtyLo & 31) != 0 || ((r.dirtyHi + 1) & 31) != 0 ||
            r.dirtyHi - r.dirtyLo + 1 < 64 ||
            g144RangeOverlaps(r.range, src) || !g144RangeOverlaps(r.range, dst))
            continue;

        const uint64_t targetBase =
            static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[i])) * 256ull;
        const uint64_t pageRowBytes = static_cast<uint64_t>(r.fbw) * kPageBytes;
        const uint64_t dirtyStart =
            targetBase + static_cast<uint64_t>(r.dirtyLo / 32) * pageRowBytes;
        const uint64_t dirtyEnd =
            targetBase + static_cast<uint64_t>((r.dirtyHi + 1) / 32) * pageRowBytes;
        // Begin page-aligned inside the first dirty row, cover the rest of that row, and reach
        // through the dirty suffix. Publishing the whole first row is safe bounded over-work.
        if (dstBase <= dirtyStart || dstBase >= dirtyStart + pageRowBytes ||
            ((dstBase - dirtyStart) % kPageBytes) != 0u || dstEnd < dirtyEnd)
            continue;

        const int originalRows = r.dirtyHi - r.dirtyLo + 1;
        r.dirtyHi = r.dirtyLo + 31;
        g261Materialize(static_cast<int>(i), kG261MatLocal);
        // Remaining FBO rows are deliberately stale until the caller copies. Never let the
        // partial publication masquerade as a whole-target upload snapshot.
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
        const bool prefixPublished = snap.valid;
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        if (!prefixPublished)
            continue; // g261Materialize already emitted its loud bounded failure and dropped state
        consumedMask |= 1u << i;
        const uint64_t n =
            g_g285L2lConsumes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g285L2lRowsPublished.fetch_add(32u, std::memory_order_relaxed);
        g_g285L2lRowsElided.fetch_add(
            static_cast<uint64_t>(originalRows - 32), std::memory_order_relaxed);
        if (s_stat && (n <= 8u || (n % 128u) == 0u))
            std::fprintf(stderr,
                         "[G285:consume] n=%llu fbp=%03x publishRows=32 elidedRows=%d "
                         "dst=0x%x bytes=%llu\n",
                         static_cast<unsigned long long>(n), kG261Fbp[i],
                         originalRows - 32, dbp,
                         static_cast<unsigned long long>(copyBytes));
    }
    return consumedMask;
}

static uint32_t g264NoteUploadForMirror(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                        uint32_t dsax, uint32_t dsay,
                                        uint32_t rrw, uint32_t rrh)
{
    if (!g264On() || !g261WaveOn() || rrw == 0u || rrh == 0u)
        return 0u;
    const G144BlockRange up = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!up.valid)
        return 0u;
    // Only exact CT32 byte replacement is supported. The common DC2 upload is 128x128 (16K
    // pixels); cap the mapper at one full 512x512 surface so malformed state fails closed.
    const uint64_t uploadPixels64 = static_cast<uint64_t>(rrw) * rrh;
    auto logReject = [&](const char *why, uint32_t ti, const G261Res &r, uint32_t hits) {
        if (!g262CensusOn())
            return;
        static std::atomic<uint32_t> s_g264RejectLog{0};
        if (s_g264RejectLog.fetch_add(1u, std::memory_order_relaxed) >= 32u)
            return;
        std::fprintf(stderr,
                     "[G264:rej] why=%s dbp=0x%x dbw=%u psm=0x%x sax=%u say=%u rect=%ux%u "
                     "blocks=%x..%x -> t=%03x fbw=%u fbW=%d hits=%u "
                     "dirty=%d..%d cov=%d..%d\n",
                     why, dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                     up.first, up.last, kG261Fbp[ti], r.fbw, r.fbW, hits,
                     r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    };

    // Invert the CT32 swizzle inside one 64x32 page once. Surface page selection itself is
    // linear in bytes; this table converts an uploaded byte address into the coordinates each
    // resident FRAME/FBW interpretation assigns to it.
    struct G264InvPage
    {
        std::array<uint16_t, 2048> x{};
        std::array<uint16_t, 2048> y{};
        bool valid = false;
    };
    static const G264InvPage s_inv = []() {
        G264InvPage out;
        out.x.fill(0xffffu);
        out.y.fill(0xffffu);
        out.valid = true;
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, y);
                if ((off & 3u) != 0u || off >= 8192u)
                {
                    out.valid = false;
                    continue;
                }
                const uint32_t word = off >> 2u;
                if (out.x[word] != 0xffffu)
                {
                    out.valid = false;
                    continue;
                }
                out.x[word] = static_cast<uint16_t>(x);
                out.y[word] = static_cast<uint16_t>(y);
            }
        for (uint32_t word = 0; word < 2048u; ++word)
            out.valid = out.valid && out.x[word] != 0xffffu;
        return out;
    }();

    if (dpsm != GS_PSM_CT32 || dbw == 0u || uploadPixels64 > 512u * 512u || !s_inv.valid)
    {
        for (uint32_t i = 0; i < kG248TargetCount; ++i)
        {
            const G261Res &r = g_g261Res[i];
            if (r.dirty && r.range.valid && g144RangeOverlaps(r.range, up))
                logReject("shape", i, r, 0u);
        }
        return 0u;
    }

    // Compute the exact set of byte addresses the CT32 IMAGE transfer replaces. This is the
    // stable bridge across different DBP/DBW and FRAME/FBW views of the same VRAM pages.
    static std::vector<uint32_t> s_uploadOffsets; // single flush-owner thread by G144 contract
    s_uploadOffsets.resize(static_cast<size_t>(uploadPixels64));
    size_t op = 0;
    for (uint32_t sy = 0; sy < rrh; ++sy)
        for (uint32_t sx = 0; sx < rrw; ++sx)
            s_uploadOffsets[op++] = GSPSMCT32::addrPSMCT32(
                dbp, dbw, dsax + sx, dsay + sy);

    uint32_t mask = 0u;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty || !r.range.valid || !g144RangeOverlaps(r.range, up))
            continue;
        if (r.fbw == 0u || r.fbW <= 0 || r.fbW > 512)
        {
            logReject("target-shape", i, r, 0u);
            continue;
        }
        const uint64_t targetBase = static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[i])) * 256u;
        const uint64_t targetBytes = static_cast<uint64_t>(r.fbw) * 16u * 8192u;
        const bool g291On = g291AtlasOn();
        if (g291On && r.g291CovFbw != r.fbw)
        {
            r.g291Cov = {};
            r.g291CovFbw = r.fbw;
            g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        }
        uint32_t hits = 0u;
        uint32_t newHits = 0u;
        int hitLo = 512, hitHi = -1;
        for (uint32_t off : s_uploadOffsets)
        {
            const uint64_t off64 = off;
            if (off64 < targetBase || off64 + 4u > targetBase + targetBytes)
                continue;
            const uint64_t rel = off64 - targetBase;
            const uint32_t page = static_cast<uint32_t>(rel / 8192u);
            const uint32_t word = static_cast<uint32_t>((rel % 8192u) >> 2u);
            const uint32_t tx = (page % r.fbw) * 64u + s_inv.x[word];
            const uint32_t ty = (page / r.fbw) * 32u + s_inv.y[word];
            if (tx >= static_cast<uint32_t>(r.fbW) || ty >= 512u)
                continue;
            ++hits;
            auto &row = r.mirBits[ty];
            const uint64_t bit = uint64_t{1} << (tx & 63u);
            uint64_t &wordRef = row[tx >> 6u];
            if ((wordRef & bit) == 0u)
            {
                wordRef |= bit;
                ++newHits;
            }
            if (g291On)
            {
                // G291: cumulative coverage — the upload just wrote this pixel's bytes to
                // guest VRAM, so VRAM is current for it until the next wave render of the row.
                r.g291Cov[ty][tx >> 6u] |= bit;
                g_g291CurPagesSet.fetch_add(1u, std::memory_order_relaxed);
            }
            hitLo = std::min(hitLo, static_cast<int>(ty));
            hitHi = std::max(hitHi, static_cast<int>(ty));
        }
        if (hits == 0u)
        {
            logReject("alias-none", i, r, hits);
            continue;
        }
        r.mirPending = true;
        if (hitLo < r.mirLo) r.mirLo = hitLo;
        if (hitHi > r.mirHi) r.mirHi = hitHi;
        mask |= (1u << i);
        g_g264MirNoted.fetch_add(1u, std::memory_order_relaxed);
        {
            // G291 bring-up diag: which uploads land in the seeded batch's row-13 texture band.
            static std::atomic<uint32_t> s_g291HiLog{0};
            if (g291StatOn() && dbp >= 0x3400u && kG261Fbp[i] == 0x139u &&
                s_g291HiLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                std::fprintf(stderr,
                             "[G291:upnote] dbp=0x%x dbw=%u rect=%ux%u rows=%d..%d px=%u new=%u\n",
                             dbp, static_cast<unsigned>(dbw), rrw, rrh, hitLo, hitHi,
                             hits, newHits);
        }
        if (g262CensusOn())
        {
            static std::atomic<uint32_t> s_g264UpLog{0};
            if (s_g264UpLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
                std::fprintf(stderr,
                             "[G264:up] dbp=0x%x dbw=%u psm=0x%x sax=%u say=%u rect=%ux%u -> "
                             "t=%03x aliasPx=%u new=%u rows=%d..%d dirty=%d..%d cov=%d..%d\n",
                             dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                             kG261Fbp[i], hits, newHits, hitLo, hitHi,
                             r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
        }
    }
    return mask;
}

// The flush closures are about to run the CPU band replay for g_g144List (the GPU attempt
// failed or was ineligible). Any entry whose target/Z/texture/CLUT pages touch a resident
// target needs those pages materialized first — the replay reads and writes guest VRAM directly.
static void g261PrepareCpuReplay()
{
    if (!g261WaveOn() || !g261AnyDirty() || g_g144List.empty())
        return;
    for (const G144Entry &e : g_g144List)
    {
        if (!g261AnyDirty())
            return;
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        const bool tme = e.prim.tme;
        const G144BlockRange tex = tme ? g144TextureRange(e) : G144BlockRange{};
        const G144BlockRange clut =
            (tme && g144IsPalettedPsm(e.ctx.tex0.psm)) ? g144ClutRange(e) : G144BlockRange{};
        const bool anyUnknown = !color.valid || (depth.active && !depth.range.valid) ||
                                (tme && !tex.valid);
        for (uint32_t i = 0; i < kG248TargetCount; ++i)
        {
            G261Res &r = g_g261Res[i];
            if (!r.dirty)
                continue;
            const bool hitUnknown = anyUnknown || !r.range.valid;
            const bool hitColor = g144RangeOverlaps(r.range, color);
            const bool hitDepth = depth.active && g144RangeOverlaps(r.range, depth.range);
            const bool hitTex = tme && g144RangeOverlaps(r.range, tex);
            const bool hitClut = clut.valid && g144RangeOverlaps(r.range, clut);
            if (hitUnknown || hitColor || hitDepth || hitTex || hitClut)
            {
                g266NoteCpuMat(e, i, (hitColor ? 1u : 0u) | (hitDepth ? 2u : 0u) |
                                         (hitTex ? 4u : 0u) | (hitClut ? 8u : 0u) |
                                         (hitUnknown ? 16u : 0u));
                g261Materialize(static_cast<int>(i), kG261MatCpuReplay);
            }
        }
    }
}

// A CPU band replay just wrote g_g144List into guest VRAM. Under waves the G260 pre-execution
// generation bump is disabled (it would poison GPU-resident targets with stale-VRAM uploads),
// so publish the RTT-family write generations HERE instead — the exact bump an inline draw
// would have done, after the replay actually changed the bytes.
static void g261NoteCpuRttWrites(uint8_t *vram)
{
    if (!g261WaveOn() || vram == nullptr)
        return;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        if (g248TargetIndex(c.frame.fbp) < 0)
            continue;
        if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
        {
            const uint32_t w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
            const uint32_t h = static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1);
            g178NoteVramWriteRect(vram, framePageBaseToBlock(c.frame.fbp),
                                  c.frame.fbw, c.frame.psm,
                                  c.scissor.x0, c.scissor.y0, w, h);
            if (((c.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRect(vram,
                                      static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                      c.frame.fbw,
                                      static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                      c.scissor.x0, c.scissor.y0, w, h);
        }
    }
}

// G286 transient targets deliberately do not join the persistent G261 ownership table, but a
// rejected GPU batch still falls back to the deferred CPU replay. Publish those exact CPU writes
// too, so a later texture/readback consumer cannot retain a generation from before the fallback.
static void g286NoteCpuTransientWrites(uint8_t *vram)
{
    if (!g286TransientTargetOn() || vram == nullptr)
        return;
    for (const G144Entry &e : g_g144List)
    {
        const GSContext &c = e.ctx;
        if (!g286TransientTarget(c.frame.fbp) ||
            c.scissor.x1 < c.scissor.x0 || c.scissor.y1 < c.scissor.y0)
            continue;
        const uint32_t w = static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1);
        const uint32_t h = static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1);
        g178NoteVramWriteRect(vram, framePageBaseToBlock(c.frame.fbp),
                              c.frame.fbw, c.frame.psm,
                              c.scissor.x0, c.scissor.y0, w, h);
        if (((c.zbuf >> 32u) & 1u) == 0u)
            g178NoteVramWriteRect(vram,
                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                  c.frame.fbw,
                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                  c.scissor.x0, c.scissor.y0, w, h);
    }
}

// An inline (non-recordable) primitive is about to rasterize on the CPU with the LIVE GS state:
// materialize any resident target its frame/Z/texture/CLUT pages touch. Reuses the exact G254
// range helpers via a candidate entry so PSM/scissor semantics can never diverge from the
// recorded-entry tests.
static void g261PrepareInline(const GSContext &ctx, const GSPrimReg &prim,
                              const GSTexClutReg &texclut)
{
    if (!g261WaveOn() || !g261AnyDirty())
        return;
    G144Entry cand{};
    cand.ctx = ctx;
    cand.prim = prim;
    cand.texclut = texclut;
    const G144BlockRange color = g144TargetRange(cand);
    const G254Surface depth = g254DepthSurface(cand);
    const bool tme = prim.tme;
    const G144BlockRange tex = tme ? g144TextureRange(cand) : G144BlockRange{};
    const G144BlockRange clut =
        (tme && g144IsPalettedPsm(ctx.tex0.psm)) ? g144ClutRange(cand) : G144BlockRange{};
    const bool unknown = (!color.valid &&
                          ctx.scissor.x1 >= ctx.scissor.x0 && ctx.scissor.y1 >= ctx.scissor.y0) ||
                         (depth.active && !depth.range.valid) || (tme && !tex.valid);
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        if (unknown || !r.range.valid ||
            (color.valid && g144RangeOverlaps(r.range, color)) ||
            (depth.active && depth.range.valid && g144RangeOverlaps(r.range, depth.range)) ||
            (tex.valid && g144RangeOverlaps(r.range, tex)) ||
            (clut.valid && g144RangeOverlaps(r.range, clut)))
            g261Materialize(static_cast<int>(i), kG261MatInline);
    }
}

// The whole-flush GPU attempt. Called from BOTH G144 flush closures (worker thread mid-frame /
// EE thread at the frame barrier — never concurrently, see the barrier contract in
// dc2_game_override.cpp). Returns true when the batch was rendered on the GPU AND its pixels are
// already written back into guest VRAM — the caller must then skip the CPU replay and clear the
// list. Any unsupported entry → false (caller runs the proven CPU replay for the WHOLE flush).
static bool g178TryFlushGpu(GSRasterizer *self, GS *gs, uint8_t *vram, uint32_t vramSize)
{
    if (!g178GpuOn() || g_g144List.empty() || vram == nullptr)
    {
        g_g266FlushRej = kG266FrOff;
        return false;
    }
    if (!g178_backend_ready())
    {
        g_g266FlushRej = kG266FrOff;
        return false;
    }
    g_g266FlushRej = kG266FrNone;
    g_g178Flushes.fetch_add(1u, std::memory_order_relaxed);
    // Resolve the exact 0x14a alias suffix before the generic owner-conflict audit: this batch
    // itself is the proven destructive prefix writer.
    const uint32_t g289DirectDisplaySrc = g289DirectDisplayToken();
    bool g289ConsumeOwner = g289CanConsumeOwner(vram, vramSize);
    bool g289TryOwnerView = false;
    if (s_g289Owner.active && !g289ConsumeOwner)
    {
        if (g289CanTryOwnerView(vram, vramSize))
            g289TryOwnerView = true;
        else
        {
            uint32_t reason = 0u;
            size_t hitEntry = 0u;
            if (g289BatchDisjointOwner(reason, hitEntry))
            {
                const uint64_t n =
                    g_g289Carries.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 8u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:carry] n=%llu version=%llu entries=%zu\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(s_g289Owner.version),
                                 g_g144List.size());
            }
            else
            {
                const uint64_t n =
                    g_g289Unexpected.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 8u || (n % 256u) == 0u))
                {
                    const G144Entry &e =
                        g_g144List[std::min(hitEntry, g_g144List.size() - 1u)];
                    const G144BlockRange color = g144TargetRange(e);
                    const G144BlockRange tex =
                        e.prim.tme ? g144TextureRange(e) : G144BlockRange{};
                    const G144BlockRange clut =
                        (e.prim.tme && g144IsPalettedPsm(e.ctx.tex0.psm))
                            ? g144ClutRange(e) : G144BlockRange{};
                    std::fprintf(
                        stderr,
                        "[G289:owner-conflict] n=%llu version=%llu reason=0x%x "
                        "entry=%zu/%zu draw(fbp=%03x/%u/%x sc=%u,%u..%u,%u "
                        "prim=%u tme=%u abe=%u fst=%u tex=%x/%u/%x/%u/%u "
                        "tfx=%u tcc=%u clut=%x/%x texa=%u/%u/%u "
                        "uv=(%u,%u)(%u,%u)(%u,%u)) "
                        "ranges(color=%x..%x tex=%x..%x clut=%x..%x)\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(s_g289Owner.version), reason,
                        hitEntry, g_g144List.size(), e.ctx.frame.fbp,
                        static_cast<unsigned>(e.ctx.frame.fbw), e.ctx.frame.psm,
                        e.ctx.scissor.x0, e.ctx.scissor.y0, e.ctx.scissor.x1,
                        e.ctx.scissor.y1, static_cast<unsigned>(e.prim.type),
                        e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                        e.prim.fst ? 1u : 0u, e.ctx.tex0.tbp0,
                        static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm,
                        static_cast<unsigned>(e.ctx.tex0.tw),
                        static_cast<unsigned>(e.ctx.tex0.th),
                        static_cast<unsigned>(e.ctx.tex0.tfx),
                        static_cast<unsigned>(e.ctx.tex0.tcc), e.ctx.tex0.cbp,
                        e.ctx.tex0.cpsm, static_cast<unsigned>(e.texa.ta0),
                        e.texa.aem ? 1u : 0u, static_cast<unsigned>(e.texa.ta1),
                        static_cast<unsigned>(e.v[0].u), static_cast<unsigned>(e.v[0].v),
                        static_cast<unsigned>(e.v[1].u), static_cast<unsigned>(e.v[1].v),
                        static_cast<unsigned>(e.v[2].u), static_cast<unsigned>(e.v[2].v),
                        color.first, color.last, tex.first, tex.last,
                        clut.first, clut.last);
                }
                (void)g289MaterializeOwner(10);
            }
        }
    }
    G289ConsumeGuard g289Guard{g289ConsumeOwner};
    G289OwnerViewGuard g289OwnerViewGuard{g289TryOwnerView};
    G289DirectGuard g289DirectGuard{g289DirectDisplaySrc != 0u};
    // G288: a G284-deferred display owner must be published before this native batch can read
    // overlapping guest-VRAM bytes through framebuffer upload, depth, texture, or CLUT paths.
    // G289's one exact display-grab sprite consumes that same owner directly from its FBO.
    if (g289DirectDisplaySrc == 0u)
        g288PublishDisplayForGpuBatch();
    // G273 diagnostic: split the proven GPU flush into front-end preparation, synchronous backend
    // submit, and post-submit VRAM/depth publication. Quiet unless DC2_G273_PROFILE=1.
    static const bool s_g273Profile = envFlagEnabled("DC2_G273_PROFILE");
    // G298 diagnosis: split G273's large front-end "pre" bucket at architecture boundaries.
    // Aggregate-only, cached, and behavior-inert unless explicitly requested.
    static const bool s_g298Profile = envFlagEnabled("DC2_G298_PROFILE");
    const auto g273T0 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto g298T0 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};

    const uint32_t fbp = g_g144List[0].ctx.frame.fbp;
    const bool g286Transient = g286TransientTarget(fbp);
    if (g286Transient)
    {
        g_g286GpuAttempts.fetch_add(1u, std::memory_order_relaxed);
        if (g289CensusOn())
        {
            static std::atomic<uint64_t> s_g289ConsumerN{0};
            const uint64_t n =
                s_g289ConsumerN.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                const G144BlockRange copied = g144RangeForRect(
                    framePageBaseToBlock(0x13bu), GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
                std::fprintf(stderr,
                             "[G289:consumer] n=%llu entries=%zu copied=%x..%x pend=%u\n",
                             static_cast<unsigned long long>(n), g_g144List.size(),
                             copied.first, copied.last, s_g276Pend.active ? 1u : 0u);
                for (size_t i = 0; i < g_g144List.size(); ++i)
                {
                    const G144Entry &e = g_g144List[i];
                    const G144BlockRange tex =
                        e.prim.tme ? g144TextureRange(e) : G144BlockRange{};
                    const G144BlockRange clut =
                        (e.prim.tme && g144IsPalettedPsm(e.ctx.tex0.psm))
                            ? g144ClutRange(e) : G144BlockRange{};
                    const G144BlockRange target = g144TargetRange(e);
                    std::fprintf(
                        stderr,
                        "[G289:entry] n=%llu i=%zu prim=%u tme=%u "
                        "tex(tbp=%x tbw=%u psm=%x wh=%ux%u range=%x..%x hit=%u) "
                        "clut(cbp=%x cpsm=%x range=%x..%x hit=%u) "
                        "target=%x..%x hit=%u\n",
                        static_cast<unsigned long long>(n), i,
                        static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                        e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.tbw),
                        e.ctx.tex0.psm,
                        1u << std::min<uint8_t>(e.ctx.tex0.tw, 15u),
                        1u << std::min<uint8_t>(e.ctx.tex0.th, 15u),
                        tex.first, tex.last, g144RangeOverlaps(copied, tex) ? 1u : 0u,
                        e.ctx.tex0.cbp, e.ctx.tex0.cpsm, clut.first, clut.last,
                        g144RangeOverlaps(copied, clut) ? 1u : 0u,
                        target.first, target.last,
                        g144RangeOverlaps(copied, target) ? 1u : 0u);
                }
            }
        }
        // 0x13b begins inside the physical 0x139 family. Match the existing CPU-replay
        // ownership edge before any FBO upload or texture decode can observe stale VRAM.
        g261PrepareCpuReplay();
    }
    const bool g255Rtt = g255GpuRttOn() && g248TargetIndex(fbp) >= 0;
    // G261: GPU-resident RTT wave — this RTT batch renders with readback deferred to a real
    // CPU-consumer edge instead of the per-flush readPixels+swizzle-writeback round-trip.
    const bool g261Wave = g255Rtt && g261WaveOn();
    if (g261WaveOn())
    {
        g_g261VramSize = vramSize; // materialize needs the size; GS::m_vramSize is private
        // G264: bring resident FBOs current with any deferred upload mirrors before the
        // invariant/uploadFb/FBO-bind logic below reasons about them.
        g264FlushMirrorsAtFlush();
    }
    if (g255Rtt)
        g255NoteAttempt(fbp);
    // G252: RTT band deferral is intentionally CPU-only during bring-up.  The CPU replay keeps
    // the proven sampler/blend/Z implementation and merely partitions disjoint rows.  A future
    // GPU RTT phase must opt in separately after explicit write->sample barrier verification.
    static const bool s_g252GpuRttEnv = (std::getenv("DC2_G252_GPU_RTT") != nullptr);
    const bool g252GpuRttEnv = g268LiveEnvRead()
                                   ? (std::getenv("DC2_G252_GPU_RTT") != nullptr)
                                   : s_g252GpuRttEnv;
    if (g248TargetIndex(fbp) >= 0 && !g252GpuRttEnv && !g255Rtt)
    {
        g_g266FlushRej = kG266FrCpuOnlyRtt;
        return false;
    }
    const uint32_t fbw = g_g144List[0].ctx.frame.fbw;
    const int fbW = static_cast<int>(fbw) * 64;
    constexpr int kFbH = 512;
    if (fbW <= 0 || fbW > 1024)
    {
        g_g266FlushRej = kG266FrFbW;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }
    const bool g272Wave = g272DisplayColorWaveOn() && t_g272DisplayBatch &&
                          g272DisplayIndex(fbp) >= 0;

    // G266: FBW is part of the RESIDENCY identity too. DC2 draws fbp=0x139 through both fbw=2
    // (character work blocks) and fbw=8 (display-grab sprite) every frame, and the backend
    // recreates the FBO on any dimension change — nothing defined survives a width flip.
    // Publish dirty rows under the OLD layout first (materialize reads back with r.fbw), then
    // drop coverage/window/snapshot so the new-width wave starts from a fresh VRAM upload.
    // Pending upload mirrors are dropped after flushing under the old geometry: their rows are
    // VRAM-authoritative by construction and the redefined FBO re-uploads from VRAM anyway.
    if (g261Wave)
    {
        const int g266Ti = g248TargetIndex(fbp);
        G261Res &g266R = g_g261Res[g266Ti];
        if (g266R.fbW > 0 && g266R.fbw != fbw && (g266R.dirty || g266R.covHi >= g266R.covLo))
        {
            if (g266R.dirty)
                g261Materialize(g266Ti, kG261MatResync); // flushes the mirror itself first
            else if (g266R.mirPending)
                g264FlushMirror(g266Ti);
            g266R.mirPending = false;
            g266R.mirLo = 512;
            g266R.mirHi = -1;
            g266R.mirBits = {};
            g266R.covLo = 512;
            g266R.covHi = -1;
            g266R.fbw = fbw;
            g266R.fbW = fbW;
            g261UpdateWindow(g266Ti);
            G178FbSnap &g266Snap = g_g178FbSnap[fbp];
            g266Snap.valid = false;
            g266Snap.genSum = 0;
            g266Snap.rowLo = 1 << 30;
            g266Snap.rowHi = -1;
        }
    }

    // Pass 1: validate every entry + derive draw state. All-or-nothing.
    static std::vector<G178EntryState> s_states;
    s_states.clear();
    s_states.reserve(g_g144List.size());
    int rowLo = kFbH, rowHi = -1;
    int depthRowLo = kFbH, depthRowHi = -1;
    int g282RowLo = kFbH, g282RowHi = -1;
    int g282DepthLo = kFbH, g282DepthHi = -1;
    const bool g282Candidate =
        g282TightRowsOn() && fbp == 0x139u && fbw == 8u;
    // G290: accumulate the same exact per-entry row unions WITHOUT engaging the default-off
    // G282 tight/seeding/fan-out machinery (g282MeasuredFamily stays gated on g282Candidate).
    const bool g290Candidate =
        !g282Candidate && g290ExactRowsOn() && fbp == 0x139u && fbw == 8u;
    bool guestDepth = false;
    bool guestDepthWrites = false;
    uint32_t guestZbpPage = 0u;
    uint32_t guestZpsm = 0u;
    for (const G144Entry &e : g_g144List)
    {
        if (e.ctx.frame.fbp != fbp || e.ctx.frame.fbw != fbw)
        {
            static std::atomic<uint32_t> s_aliasColorLog{0};
            const uint32_t n = s_aliasColorLog.fetch_add(1u, std::memory_order_relaxed);
            if (g242StatEnabled() && n < 24u)
                std::fprintf(stderr,
                             "[G242:alias] color-depth n=%u fbp=0x%x zbp=0x%x fbRows=%d..%d zRows=%d..%d fbw=%u\n",
                             n + 1u, fbp, guestZbpPage, rowLo, rowHi,
                             depthRowLo, depthRowHi, fbw);
            g_g266FlushRej = kG266FrMixedTgt;
            // G266: the batch boundary splits on FBP only, so this is expected to be a same-fbp
            // FBW mix — capture both tuples to pin the exact mix shape.
            if (g266CensusOn())
            {
                static std::atomic<uint32_t> s_g266MixLog{0};
                if (s_g266MixLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
                    std::fprintf(stderr,
                                 "[G266:mix] first(fbp=%03x fbw=%u) entry(fbp=%03x fbw=%u prim=%u "
                                 "tme=%u tbp=0x%x tpsm=0x%x) idx=%zu n=%zu\n",
                                 fbp, fbw, e.ctx.frame.fbp, e.ctx.frame.fbw,
                                 static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                                 e.ctx.tex0.tbp0, e.ctx.tex0.psm,
                                 static_cast<size_t>(&e - g_g144List.data()), g_g144List.size());
            }
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false; // mixed targets in one flush — shouldn't happen (block flush on fbp change)
        }
        G178EntryState st;
        if (!g178ClassifyEntry(e, st))
        {
            g_g266FlushRej = kG266FrClassify; // g_g266ClassifySite set inside g262NoteReject
            if (g261Wave)
                g_g261RttClassifyRej.fetch_add(1u, std::memory_order_relaxed);
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear(); // the CPU replay about to run writes VRAM the gens don't track
            return false;
        }
        // G282: G144 capture already computed conservative primitive rows with the same
        // floor/ceil/scissor rules as drawTriangle/drawSprite. Keep a parallel tight union for
        // the measured alias batch; the legacy scissor union stays intact until all entries pass.
        const int g282EntryLo = std::max(static_cast<int>(st.scY0), e.minY);
        const int g282EntryHi = std::min(static_cast<int>(st.scY1), e.maxY);
        if (st.guestDepth)
        {
            if (!guestDepth)
            {
                guestDepth = true;
                guestZbpPage = st.zbpPage;
                guestZpsm = st.zpsm;
            }
            else if (guestZbpPage != st.zbpPage || guestZpsm != st.zpsm)
            {
                // One GL attachment cannot represent two guest Z targets in one all-or-nothing
                // flush. Preserve exact ordering by sending the whole list to the CPU replay.
                g_g266FlushRej = kG266FrMixedZ;
                g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                g_g178FbSnap.clear();
                return false;
            }
            guestDepthWrites = guestDepthWrites || st.depthWrite;
            if (!st.skip)
            {
                depthRowLo = std::min(depthRowLo, static_cast<int>(st.scY0));
                depthRowHi = std::max(depthRowHi, static_cast<int>(st.scY1));
                if ((g282Candidate || g290Candidate) && g282EntryHi >= g282EntryLo)
                {
                    g282DepthLo = std::min(g282DepthLo, g282EntryLo);
                    g282DepthHi = std::max(g282DepthHi, g282EntryHi);
                }
            }
        }
        s_states.push_back(st);
        if (!st.skip)
        {
            rowLo = std::min(rowLo, static_cast<int>(st.scY0));
            rowHi = std::max(rowHi, static_cast<int>(st.scY1));
            if ((g282Candidate || g290Candidate) && g282EntryHi >= g282EntryLo)
            {
                g282RowLo = std::min(g282RowLo, g282EntryLo);
                g282RowHi = std::max(g282RowHi, g282EntryHi);
            }
        }
    }
    static std::vector<uint32_t> s_g289OwnerSrc;
    bool g289OwnerViewReady = false;
    if (g289TryOwnerView)
    {
        uint64_t binds = 0u;
        bool builtAlias = false;
        if (g289PrepareOwnerViewSources(s_states, s_g289OwnerSrc, binds, builtAlias))
        {
            g289OwnerViewReady = true;
            const uint64_t n =
                g_g289OwnerViews.fetch_add(1u, std::memory_order_relaxed) + 1u;
            g_g289OwnerBinds.fetch_add(binds, std::memory_order_relaxed);
            if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                std::fprintf(stderr,
                             "[G289:owner-view] n=%llu version=%llu entries=%zu binds=%llu "
                             "alias=%u\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(s_g289Owner.version),
                             g_g144List.size(), static_cast<unsigned long long>(binds),
                             builtAlias ? 1u : 0u);
        }
        else
        {
            s_g289OwnerSrc.clear();
            g289TryOwnerView = false;
            g289OwnerViewGuard.armed = false;
            const uint64_t n =
                g_g289OwnerViewFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                std::fprintf(stderr,
                             "[G289:owner-view-reject] n=%llu version=%llu entries=%zu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(s_g289Owner.version),
                             g_g144List.size());
            (void)g289MaterializeOwner(11);
        }
    }
    else
        s_g289OwnerSrc.clear();
    if (rowHi < rowLo)
        return true; // everything skipped — nothing to draw, nothing to read back
    const auto g298T1 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const int g282LegacyRowLo = rowLo, g282LegacyRowHi = rowHi;
    const int g282LegacyDepthLo = depthRowLo, g282LegacyDepthHi = depthRowHi;
    static std::vector<G282Fanout> s_g282Fanout;
    s_g282Fanout.clear();
    const bool g282MeasuredFamily =
        g282Candidate && guestDepth && !guestDepthWrites &&
        guestZbpPage == 0x0d0u && guestZpsm == 1u;
    if (g282MeasuredFamily)
        g_g282Candidates.fetch_add(1u, std::memory_order_relaxed);
    const bool g282Prepared =
        g282MeasuredFamily &&
        g282PrepareFanout(s_states, guestDepth, guestDepthWrites,
                          guestZbpPage, guestZpsm, s_g282Fanout);
    if (g282MeasuredFamily && !g282Prepared)
        g_g282RejShape.fetch_add(1u, std::memory_order_relaxed);
    const bool g282Tight =
        g282Prepared && g282RowHi >= g282RowLo &&
        (!guestDepth || g282DepthHi >= g282DepthLo);
    if (g282Tight)
    {
        rowLo = g282RowLo;
        rowHi = g282RowHi;
        if (guestDepth)
        {
            depthRowLo = g282DepthLo;
            depthRowHi = g282DepthHi;
        }
        g_g282TightBatches.fetch_add(1u, std::memory_order_relaxed);
    }
    // G290: standalone seeded admission for the measured 0x139 batch (fbw=8, CT32, zbp=0xd0
    // Z24, READ-ONLY Z — every depth entry has ZMSK=1, so no Z readback can ever publish the
    // seeded rows). The G282 structural proof (g282PrepareFanout, fan-out list must stay empty
    // without the alias-view stack) establishes: entry 0 is an exact opaque zero 512x416 clear,
    // every later draw writes color only at rows 32..447 inside the clear, and depth is never
    // written. The physical color/Z page alias (color rows 0..31 == Z rows 416..447, x>=64)
    // therefore holds exactly the post-clear zero bytes at every Z read, which the seeded depth
    // upload below reproduces independent of prior VRAM content (first use included). Exact
    // vertex-bound rows are adopted so upload/readback/residency windows match real coverage.
    bool g290Seeded = false;
    if (g290Candidate && !g282Tight &&
        g_g144List[0].ctx.frame.psm == 0u && guestDepth && !guestDepthWrites &&
        guestZbpPage == 0x0d0u && guestZpsm == 1u &&
        g282RowHi >= g282RowLo && g282DepthHi >= g282DepthLo &&
        g282RowLo == 0 && g282RowHi <= 447 && g282DepthLo >= 32 && g282DepthHi <= 447)
    {
        static std::vector<G282Fanout> s_emptyPlan;
        s_emptyPlan.clear();
        if (g282PrepareFanout(s_states, guestDepth, guestDepthWrites,
                              guestZbpPage, guestZpsm, s_emptyPlan) &&
            s_emptyPlan.empty())
        {
            g290Seeded = true;
            rowLo = g282RowLo;
            rowHi = g282RowHi;
            depthRowLo = g282DepthLo;
            depthRowHi = g282DepthHi;
        }
        else
            g_g290ExactRejShape.fetch_add(1u, std::memory_order_relaxed);
    }
    if (g255Rtt && guestDepth && !g262WideOn())
    {
        // G255 owns RTT color only. Real-Z batches retain the proven CPU replay until a separate
        // persistent-depth design can satisfy the G249 dirty-generation invariant. G262 widening
        // lifts this via the NON-persistent per-wave Z round-trip below (no ownership to break).
        g_g266FlushRej = kG266FrRttDepth;
        if (g261Wave)
            g_g261RttDepthRej.fetch_add(1u, std::memory_order_relaxed);
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }

    const uint32_t fbBlock = GSInternal::framePageBaseToBlock(fbp);
    if (guestDepth)
    {
        const uint32_t zbpBlock = guestZbpPage << 5u;
        // Separate GL attachments cannot emulate simultaneous color/depth access to the same GS
        // storage.  Keep this rare alias configuration on the proven CPU path.
        const bool conservativeAlias = g242RectsOverlap(
            fbBlock, fbw, GS_PSM_CT32, 0u, static_cast<uint32_t>(rowLo),
            static_cast<uint32_t>(fbW), static_cast<uint32_t>(rowHi - rowLo + 1),
            zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
            static_cast<uint32_t>(fbW), static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
        // G273 promotion: the only relaxed tuple is the measured page-aligned display pair below.
        // The exact marker proves color pages 104..207 and Z pages 208+ are disjoint; every other
        // layout retains G242's conservative slack. Ride the promoted native display-Z stack so
        // its master kill/oracle declines remain authoritative. Per-phase rollback accepts either
        // DC2_G273_NO_EXACT_COLORZ_ALIAS=1 or DC2_G273_EXACT_COLORZ_ALIAS=0.
        static const bool s_g273ExactAlias = !envFlagEnabled("DC2_G273_NO_EXACT_COLORZ_ALIAS") &&
                                              !envFlagDisabled("DC2_G273_EXACT_COLORZ_ALIAS") &&
                                              g265DisplayZWaveOn();
        const bool measuredPair = s_g273ExactAlias && fbp == 0x68u && fbBlock == 0x0d00u &&
                                  zbpBlock == 0x1a00u && fbw == 8u;
        const bool rawColorZAlias = measuredPair
            ? g273AlignedRectsOverlapExact(
                  fbBlock, fbw, GS_PSM_CT32, 0u, static_cast<uint32_t>(rowLo),
                  static_cast<uint32_t>(fbW), static_cast<uint32_t>(rowHi - rowLo + 1),
                  zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                  static_cast<uint32_t>(fbW), static_cast<uint32_t>(depthRowHi - depthRowLo + 1))
            : conservativeAlias;
        // G290: the structurally-proven seeded batch represents the color/Z page alias exactly
        // (post-clear zeros seeded into the depth upload below), so the alias is not a reject.
        if (g290Seeded)
        {
            const uint64_t n =
                g_g290ExactAdmit.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g290StatOn() && (n <= 8u || (n % 120u) == 0u))
                std::fprintf(stderr,
                             "[G290:seeded] admit=%llu alias=%u rows(c=%d..%d z=%d..%d) "
                             "entries=%zu rejShape=%llu\n",
                             (unsigned long long)n, rawColorZAlias ? 1u : 0u, rowLo, rowHi,
                             depthRowLo, depthRowHi, g_g144List.size(),
                             (unsigned long long)g_g290ExactRejShape.load(
                                 std::memory_order_relaxed));
        }
        // G282's only real overlap is color page row 0 (blocks 313..319) aliasing Z page row
        // 13, cols 1..7. The structurally-proven leading black clear fills those whole CT32
        // pages before any depth-tested draw, and every later color write starts at row 32, so
        // pre-seeding those Z upload pages to zero exactly represents the intra-batch GS alias.
        const bool g282SeededColorZ =
            g282Tight && rawColorZAlias && rowLo == 0 && rowHi <= 447 &&
            depthRowLo >= 32 && depthRowHi <= 447;
        const bool colorZAlias = rawColorZAlias && !g282SeededColorZ && !g290Seeded;
        const bool g282LegacyAlias =
            g282Tight && g242RectsOverlap(
                fbBlock, fbw, GS_PSM_CT32, 0u,
                static_cast<uint32_t>(g282LegacyRowLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(g282LegacyRowHi - g282LegacyRowLo + 1),
                zbpBlock, fbw, GS_PSM_Z24, 0u,
                static_cast<uint32_t>(g282LegacyDepthLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(g282LegacyDepthHi - g282LegacyDepthLo + 1));
        if (g282LegacyAlias && !colorZAlias)
            g_g282AliasSaved.fetch_add(1u, std::memory_order_relaxed);
        if (measuredPair && conservativeAlias && !colorZAlias)
        {
            static std::atomic<uint64_t> s_admit{0};
            const uint64_t n = s_admit.fetch_add(1u, std::memory_order_relaxed) + 1u;
            static const bool s_stat = envFlagEnabled("DC2_G273_STAT");
            if (s_stat && (n <= 16u || (n % 120u) == 0u))
                std::fprintf(stderr,
                             "[G273:alias] admit=%llu colorPages<=207 zPages>=208 rows(c=%d..%d z=%d..%d)\n",
                             static_cast<unsigned long long>(n), rowLo, rowHi,
                             depthRowLo, depthRowHi);
        }
        if (colorZAlias)
        {
            // G282: G266 attributed the one-per-frame fbp=0x139/fbw=8 CPU publication to this
            // branch, while the drain-wide G178 census showed only ZTST=ALWAYS + ZMSK=1 entries
            // for that target (a depth no-op). Log at the rejecting branch itself so concurrent
            // drain diagnostics cannot misattribute another thread's shared reject code.
            if (g282CensusOn())
            {
                static std::atomic<uint64_t> s_aliasN{0};
                const uint64_t n = s_aliasN.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 8u || (n % 120u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G282:colorz] n=%llu fbp=%03x fbw=%u rows=%d..%d "
                                 "zbp=%03x zpsm=%u zrows=%d..%d entries=%zu "
                                 "legacy(c=%d..%d z=%d..%d) tight=%u\n",
                                 (unsigned long long)n, fbp, fbw, rowLo, rowHi,
                                 guestZbpPage, guestZpsm, depthRowLo, depthRowHi,
                                 g_g144List.size(), g282LegacyRowLo, g282LegacyRowHi,
                                 g282LegacyDepthLo, g282LegacyDepthHi, g282Tight ? 1u : 0u);
                    if (n <= 2u)
                    {
                        for (size_t i = 0; i < std::min<size_t>(g_g144List.size(), 16u); ++i)
                        {
                            const G144Entry &e = g_g144List[i];
                            const G178EntryState &st = s_states[i];
                            const uint64_t ts = e.ctx.test;
                            std::fprintf(
                                stderr,
                                "[G282:entry] n=%llu i=%zu prim=%u tme=%u abe=%u "
                                "test=%llx(zte=%u ztst=%u) zbuf=%llx(zmsk=%u) "
                                "guestZ=%u zwrite=%u skip=%u sc=%u,%u..%u,%u "
                                "v0=(%.1f,%.1f,z=%.0f) v1=(%.1f,%.1f,z=%.0f)\n",
                                (unsigned long long)n, i,
                                static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                                e.prim.abe ? 1u : 0u, (unsigned long long)ts,
                                static_cast<unsigned>((ts >> 16u) & 1u),
                                static_cast<unsigned>((ts >> 17u) & 3u),
                                (unsigned long long)e.ctx.zbuf,
                                static_cast<unsigned>((e.ctx.zbuf >> 32u) & 1u),
                                st.guestDepth ? 1u : 0u, st.depthWrite ? 1u : 0u,
                                st.skip ? 1u : 0u, st.scX0, st.scY0, st.scX1, st.scY1,
                                e.v[0].x, e.v[0].y, e.v[0].z,
                                e.v[1].x, e.v[1].y, e.v[1].z);
                        }
                    }
                }
            }
            g_g266FlushRej = kG266FrColorZAlias;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
    }

    // G278 pending-depth consumer audit before ANY pass-2 VRAM reads. A same-key guest-depth
    // continuation consumes the resident backend texture directly and is safe; a cross-key wave
    // must first publish because its upload reads VRAM Z. Texture/CLUT reads are tested against
    // the unpublished row union; an exact disjoint color write may continue, then marks its depth
    // rows backend-stale when it reaches VRAM. Unknown layouts fail closed to publication.
    // CPU-fallback returns below publish again at cause 3.
    if (s_g278Pend.active)
    {
        bool mustPublish = s_g278Pend.vram != vram ||
                           s_g278Pend.vramSize != vramSize;
        // G290: the seeded batch is owner-INELIGIBLE (its color write range physically overlaps
        // its Z read range), so it must never fall through to a full VRAM Z upload while a
        // same-key pending owner holds newer backend rows. A pending union confined to rows
        // 0..415 is safe to CONTINUE (the seeded rows 416..447 are re-established by the partial
        // upload below and row 416+ is never in the union); any wider pending union publishes.
        if (g290Seeded && s_g278Pend.rowHi > 415)
            mustPublish = true;
        if (!mustPublish && guestDepth)
        {
            const uint64_t nextKey = g242DepthKey(guestZbpPage, guestZpsm, fbw);
            mustPublish = g242GpuDepthOn() || nextKey != s_g278Pend.key;
            if (!mustPublish)
            {
                const int prospectiveLo = std::min(s_g278Pend.rowLo, depthRowLo);
                const int prospectiveHi = std::max(s_g278Pend.rowHi, depthRowHi);
                mustPublish = g278StaleRowsOverlap(prospectiveLo, prospectiveHi);
            }
        }
        const G144BlockRange dirty = g278PendingDepthRange();
        const G144BlockRange whole = g278PendingDepthWholeRange();
        if (!mustPublish && (!dirty.valid || !whole.valid))
            mustPublish = true;
        if (!mustPublish)
        {
            const G144BlockRange target = g144RangeForRect(
                fbBlock, GS_PSM_CT32, fbw, 0u,
                static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(rowHi - rowLo + 1));
            mustPublish = !target.valid || g144RangeOverlaps(dirty, target);
        }
        if (!mustPublish)
        {
            for (size_t i = 0; i < g_g144List.size(); ++i)
            {
                const G144Entry &e = g_g144List[i];
                if (s_states[i].skip || !e.prim.tme)
                    continue;
                const G144BlockRange tex = g144TextureRange(e);
                if (!tex.valid || g144RangeOverlaps(dirty, tex))
                {
                    mustPublish = true;
                    break;
                }
                if (g144IsPalettedPsm(e.ctx.tex0.psm))
                {
                    const G144BlockRange clut = g144ClutRange(e);
                    if (!clut.valid || g144RangeOverlaps(dirty, clut))
                    {
                        mustPublish = true;
                        break;
                    }
                }
            }
        }
        if (!mustPublish && s_g276Pend.active)
        {
            const G144BlockRange display = g277PendingDisplayRange();
            mustPublish = !display.valid || g144RangeOverlaps(whole, display);
        }
        if (mustPublish && !g278FlushPendingDepth(0))
        {
            g_g266FlushRej = kG266FrZReadback;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
    }

    // The CPU framebuffer upload below must not observe stale bytes from an aliased persistent Z
    // resource.  This is normally a no-op (display color and Z occupy disjoint GS pages).
    if (!g242SyncOverlappingDepth(vram, fbBlock, fbw, GS_PSM_CT32, 0u,
                                  static_cast<uint32_t>(rowLo),
                                  static_cast<uint32_t>(fbW),
                                  static_cast<uint32_t>(rowHi - rowLo + 1)))
    {
        g_g266FlushRej = kG266FrDepthSetup;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear();
        return false;
    }

    // Pass 2: build the batch.
    static G178Batch s_batch; // reused across flushes (keeps vector capacity)
    s_batch.fbp = fbp;
    s_batch.fbW = fbW;
    s_batch.fbH = kFbH;
    s_batch.texUploads.clear();
    s_batch.verts.clear();
    s_batch.draws.clear();
    // G276: coalesce consecutive same-target display-color readbacks. A display batch (0x0/0x68)
    // that is not already a G261/G272 wave defers its color readback: skip it here (the draw still
    // lands in the persistent FBO), and publish the pending union at the next different target or a
    // consumer edge. If a pending readback exists for a DIFFERENT target/width, publish it first so
    // that stale target is never read by this batch.
    const bool g276Coalesce = g276DisplayRbOn() && g265DisplayTarget(fbp) &&
                              !g261Wave && !g272Wave && !g255Rtt;
    if (g276Coalesce && s_g276Pend.active &&
        (s_g276Pend.fbp != fbp || s_g276Pend.fbw != fbw))
        g276FlushPendingDisplay(0);
    // G261: waves leave the result in the FBO (no readPixels, no swizzle-writeback); the raw-
    // alpha RTT store rides the batch flag so the backend does not depend on the G255 env var.
    s_batch.skipReadback = g261Wave || g272Wave || g276Coalesce;
    s_batch.rttRawAlpha = g255Rtt || g286Transient;

    // Depth-clear on target change: mirror g106TitleZClearIfNeeded's fbp-transition semantics and
    // keep the CPU private-Z state machine in sync for any CPU-fallback flush in between.
    {
        static uint32_t s_lastZFbp = 0xFFFFFFFFu;
        // Guest depth already contains the game's clears and persists independently of color-FBP
        // changes.  Synthetic GL clears are only valid for the legacy private-depth path.
        s_batch.clearDepth = !guestDepth && (s_lastZFbp != fbp);
        s_lastZFbp = fbp;
        g106TitleZClearIfNeeded(fbp);
    }

    // Framebuffer upload, GEN-GATED: skipped when (a) the fb pages' write-gen sum is unchanged
    // since our last readback for this fbp (no CPU upload/inline draw touched them — GPU inline
    // writes bump via the drawPrimitive hook, BITBLTs via the ps2_gs_gpu.cpp hooks, and a CPU-
    // fallback flush invalidates the snapshot wholesale below), AND (b) this batch's row window
    // is inside the rows we have ever uploaded/read back (FBO rows outside that are undefined).
    // GL row order = bottom-first: buffer row r = GS row (kFbH-1-r).
    const int rows = rowHi - rowLo + 1;
    G178FbSnap &snap = g_g178FbSnap[fbp];
    // G277-R exact snapshot gen window: for the G273-measured display tuple, color rows 0..415 map
    // to pages 104..207, provably disjoint from zbp 0xd0's Z pages 208+ (which the full 512-row
    // window falsely included — every Z-wave writeback bump then clobbered G276-deferred FBO rows
    // with a stale-VRAM re-upload: the MAP-0 Max body flicker). Adopt the exact bound only while
    // every row this snapshot reasons about stays <=415; otherwise conservative full height.
    int g277GenRows = kFbH;
    if (g277ExactDisplayGenOn() && fbp == 0x68u && fbBlock == 0x0d00u && fbw == 8u &&
        rowHi <= 415 && (!snap.valid || snap.rowHi <= 415))
    {
        g277GenRows = 416;
        g_g277ExactGenWin.fetch_add(1u, std::memory_order_relaxed);
    }
    // Exact tuple: slack-free aligned sum (g178GenSumRect's +1 slack page would still read Z page
    // 208 through color page 207 and defeat the window — see g277GenSumAligned).
    uint64_t fbGenNow = (g277GenRows == 416)
        ? g277GenSumAligned(fbBlock, fbw, 416u)
        : g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0, 0,
                         static_cast<uint32_t>(fbW), kFbH);
    // G261: a GPU-dirty target must NEVER be overwritten by an uploadFb from (stale) VRAM. If
    // the pages' generations moved while dirty, a CPU writer escaped the materialize hooks —
    // drop residency loudly and let VRAM stay authoritative (the G242/G249 fail-loud pattern).
    // If this batch renders outside the FBO-defined rows, materialize first (VRAM is current
    // for non-dirty rows) and take the normal upload path.
    bool g261ForceNoUpload = false;
    if (g261Wave)
    {
        G261Res &r = g_g261Res[g248TargetIndex(fbp)];
        if (r.dirty)
        {
            const uint64_t g261WinNow = g261WindowGenNow(r, fbp);
            if (r.genAtSkip != g261WinNow)
            {
                const uint64_t n = g_g261Invariants.fetch_add(1u, std::memory_order_relaxed) + 1u;
                std::fprintf(stderr,
                             "[G261:invariant] escaped writer fbp=0x%x genAtSkip=%llu genNow=%llu n=%llu\n",
                             fbp, (unsigned long long)r.genAtSkip,
                             (unsigned long long)g261WinNow, (unsigned long long)n);
                g280DropOverlays(g248TargetIndex(fbp)); // residency dies; nothing publishes them
                r.dirty = false;
                r.dirtyLo = 512;
                r.dirtyHi = -1;
                // Unknown FBO↔VRAM relation — force a fresh upload. Reset IN PLACE (a `snap`
                // reference into the map is live in this scope; erase would invalidate it).
                snap.valid = false;
                snap.genSum = 0;
                snap.rowLo = 1 << 30;
                snap.rowHi = -1;
            }
            else if (rowLo >= r.covLo && rowHi <= r.covHi)
            {
                g261ForceNoUpload = true;
                if (!snap.valid)
                {
                    // A CPU-fallback elsewhere cleared the snapshot map. The ownership hooks
                    // guarantee no CPU write touched THESE pages while dirty, so the FBO is
                    // still the newest content — restore the snapshot instead of re-uploading.
                    snap.valid = true;
                    snap.genSum = fbGenNow;
                    snap.rowLo = r.covLo;
                    snap.rowHi = r.covHi;
                }
            }
            else
            {
                g261Materialize(g248TargetIndex(fbp), kG261MatResync);
                fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0, 0,
                                          static_cast<uint32_t>(fbW), kFbH);
            }
        }
    }
    s_batch.uploadFb = !g261ForceNoUpload && !g289ConsumeOwner &&
                       !(snap.valid && snap.genSum == fbGenNow &&
                         rowLo >= snap.rowLo && rowHi <= snap.rowHi);
    if (s_batch.uploadFb && s_g276Pend.active && s_g276Pend.fbp == fbp)
    {
        // G277-R fail-safe: the FBO holds G276-deferred rows that exist NOWHERE else. Re-uploading
        // (stale) VRAM now would erase them (the Max-body flicker mechanism). Publish the pending
        // union first so the upload below reads current bytes; correctness identical to the
        // pre-G276 per-batch readback ordering. Loud counter — with the exact gen window this
        // should be rare; a hot count means a new unhandled generation mover.
        if (g277GenMoveOn() && fbp == 0x68u && s_g277ChunkValid)
        {
            char buf[160];
            int off = 0;
            for (int i = 0; i < 13; ++i)
            {
                const uint64_t cur = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u,
                                                    static_cast<uint32_t>(i * 32),
                                                    static_cast<uint32_t>(fbW), 32u);
                if (cur != s_g277Chunk[i] && off < 140)
                    off += std::snprintf(buf + off, sizeof(buf) - off, " %d(+%lld)", i,
                                         static_cast<long long>(cur - s_g277Chunk[i]));
            }
            static std::atomic<uint32_t> s_gmLog{0};
            if (s_gmLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                std::fprintf(stderr, "[G277:genmove] movedChunks:%s (rows=chunk*32..+31)\n",
                             off ? buf : " none");
        }
        g276FlushPendingDisplay(2);
        const uint64_t n = g_g277UploadPublish.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 8u)
            std::fprintf(stderr,
                         "[G277:upload-publish] n=%llu fbp=0x%x rows=%d..%d snap(v=%d gen%s)\n",
                         static_cast<unsigned long long>(n), fbp, rowLo, rowHi,
                         snap.valid ? 1 : 0,
                         (snap.valid && snap.genSum != fbGenNow) ? "MOVED" : "=");
    }
    if (s_batch.uploadFb)
    {
        g_g178FbUploads.fetch_add(1u, std::memory_order_relaxed);
        s_batch.fbPixels.assign(static_cast<size_t>(fbW) * kFbH, 0u);
        // Upload the union of this batch's window and everything previously covered, so the
        // covered-row invariant stays true after a single upload.
        const int upLo = snap.valid ? std::min(rowLo, snap.rowLo) : rowLo;
        const int upHi = snap.valid ? std::max(rowHi, snap.rowHi) : rowHi;
        // G290: the structurally-proven leading 512x416 opaque clear (fbmsk=0, no blend)
        // overwrites every FBO pixel in rows 0..415 before any draw reads destination color,
        // so uploading those rows from VRAM is pure churn — only rows the clear does not cover
        // need real VRAM bytes. Coverage invariant unchanged: rows 0..415 are valid post-clear.
        const int upLoEff = g290Seeded ? std::max(upLo, 416) : upLo;
        // G298 promotion (default on): CT32 maps each logical pixel to one independent 32-bit
        // staging element. Split disjoint guest-row ranges across the existing joined row pool;
        // VRAM is read-only here and the pool joins before backend submission, snapshot
        // publication, or any ownership transition. Keep combined and stage-local kill switches.
        static const bool s_g298ParFbPack =
            !envFlagEnabled("DC2_G298_NO_PAR_PREP") &&
            !envFlagEnabled("DC2_G298_NO_PAR_FBPACK") &&
            !envFlagDisabled("DC2_G298_PAR_FBPACK");
        static const int s_g298FbPackLanes = [] {
            const char *value = std::getenv("DC2_G298_FBPACK_LANES");
            if (value == nullptr)
                return 8;
            return std::max(1, std::min(16, std::atoi(value)));
        }();
        const auto packFbRows = [&](int laneLo, int laneHi) {
            for (int y = laneLo; y <= laneHi; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, fbw, static_cast<uint32_t>(x),
                        static_cast<uint32_t>(y));
                    uint32_t px = 0;
                    if (off + 4u <= vramSize)
                        std::memcpy(&px, vram + off, 4);
                    s_batch.fbPixels[dstRow + x] = px;
                }
            }
        };
        const int packRows = upHi >= upLoEff ? (upHi - upLoEff + 1) : 0;
        const int packLanes =
            s_g298ParFbPack && packRows >= 8 ? s_g298FbPackLanes : 1;
        GSRowPool::instance().run(upLoEff, upHi, packLanes, packFbRows);

        // Exact same-run oracle for bring-up: independently reread every packed CT32 pixel using
        // the serial address path after the joined dispatch. This is diagnostic-only because it
        // deliberately repeats the staging work.
        static const bool s_g298Verify =
            envFlagEnabled("DC2_G298_VERIFY_FBPACK");
        if (s_g298ParFbPack && s_g298Verify && packRows > 0)
        {
            uint64_t bad = 0u;
            for (int y = upLoEff; y <= upHi; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, fbw, static_cast<uint32_t>(x),
                        static_cast<uint32_t>(y));
                    uint32_t expected = 0u;
                    if (off + 4u <= vramSize)
                        std::memcpy(&expected, vram + off, 4);
                    bad += s_batch.fbPixels[dstRow + x] != expected ? 1u : 0u;
                }
            }
            static std::atomic<uint64_t> s_packs{0u}, s_pixels{0u}, s_bad{0u};
            const uint64_t n =
                s_packs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint64_t pixels = static_cast<uint64_t>(packRows) *
                                    static_cast<uint64_t>(fbW);
            const uint64_t totalPixels =
                s_pixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
            const uint64_t totalBad =
                s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
            if (bad != 0u || n <= 8u || (n % 256u) == 0u)
                std::fprintf(stderr,
                             "[G298:verify] packs=%llu pixels=%llu bad=%llu "
                             "last(rows=%d lanes=%d bad=%llu)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(totalPixels),
                             static_cast<unsigned long long>(totalBad),
                             packRows, packLanes,
                             static_cast<unsigned long long>(bad));
        }
        snap.rowLo = upLo;
        snap.rowHi = upHi;
    }
    else
    {
        g_g178FbUploadSkips.fetch_add(1u, std::memory_order_relaxed);
        s_batch.fbPixels.clear();
    }

    // G242: keep guest Z resident across GPU flushes. Upload the full validated 512-row target only
    // on first use or after an overlapping guest write; CPU/transfer/frame boundaries materialize
    // just the dirty row union back to VRAM. Float(raw * 2^-32) is exact for uint24.
    static std::vector<float> s_depthUpload;
    int depthGlY = 0;
    int depthRows = 0;
    uint64_t depthKey = 0u;
    uint64_t stagedGuestGen = 0u;
    bool stagedDepthUpload = false;
    const std::vector<float> *depthUpload = nullptr;
    G242DepthState *depthState = nullptr;
    bool g262WaveDepth = false;
    bool g278PendingSameKey = false;
    bool g278OwnerEligible = false;
    G144BlockRange g278ColorWrite = g144RangeForRect(
        fbBlock, GS_PSM_CT32, fbw, 0u,
        static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
        static_cast<uint32_t>(rowHi - rowLo + 1));
    if (guestDepth)
    {
        if (guestZpsm != 0x1u || depthRowHi < depthRowLo)
        {
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        depthKey = g242DepthKey(guestZbpPage, guestZpsm, fbw);
        g262WaveDepth = !g242GpuDepthOn(); // classify admits guestDepth without G242 only via G262
        if (g262WaveDepth)
        {
            const G144BlockRange depthWrite = g144RangeForRect(
                guestZbpPage << 5u, GS_PSM_Z24, fbw, 0u,
                static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            // Separate color/depth backend resources cannot preserve GS physical aliasing across
            // a simultaneous physical write. Exact disjoint row windows are safe: an immediate
            // color publication marks the other depth rows stale below, while retained color is
            // ordered by the G261/G272/G276 materialization barriers.
            g278OwnerEligible =
                g278ColorWrite.valid && depthWrite.valid &&
                !g144RangeOverlaps(g278ColorWrite, depthWrite);
        }
        g278PendingSameKey =
            g278DepthRbOn() && g278OwnerEligible && s_g278Pend.active &&
            s_g278Pend.key == depthKey && s_g278Pend.vram == vram &&
            s_g278Pend.vramSize == vramSize;
        // G277 census: a cross-key Z wave re-reads VRAM Z for its own window upload — it is a real
        // consumer of the previous key's deferred readback (conservative lower bound: G274 may
        // actually skip that upload when the other key's gen is unchanged).
        if (g277CensusOn() && s_g277ZRunLive && depthKey != s_g277ZRunKey)
            s_g277ZRunLive = false;
        if (g262WaveDepth)
        {
            // G262 per-wave NON-PERSISTENT guest depth: upload exactly the depth row window from
            // VRAM every wave and read it back immediately after execution (below). VRAM stays
            // authoritative between waves — no G242DepthState, no dirty flag, no generation
            // invariant, no boundary-sync obligation. This is the wave-granular barrier the arc
            // prescribes (coarser than G256's refuted per-draw barriers by the whole batch).
            // A fresh backend depth texture has undefined rows and its first upload must cover
            // the whole target (backend contract). Track first-use per key front-end-side; if
            // the backend ever recreates a texture behind our back the submit fail-closes to
            // the CPU replay (validateBatchInputs), which is safe.
            const bool fullInit = g_g262DepthInit.count(depthKey) == 0u;
            const uint32_t zbpBlock = guestZbpPage << 5;
            // G274: whole-target Z gen. If unchanged since our last writeback for this key, no
            // writer touched VRAM Z between waves, so the resident backend depth texture already
            // equals VRAM Z (last readback wrote GPU->VRAM; VRAM unchanged; GPU untouched) and the
            // re-upload is pure churn. Computed when the census OR the residency lever is active.
            uint64_t zGenNow = 0u;
            bool zGenValid = false;
            if (g274ZTimeOn() || g274ZResidentOn())
            {
                zGenNow = g178GenSumRect(zbpBlock, fbw, GS_PSM_Z24, 0u, 0u,
                                         static_cast<uint32_t>(fbW), 512u);
                zGenValid = true;
            }
            bool g274SkipUpload = false;
            bool g290PartialSeed = false;
            {
                auto itPrev = g_g274LastZGen.find(depthKey);
                const bool prevKnown = zGenValid && itPrev != g_g274LastZGen.end();
                const bool unchanged = prevKnown && itPrev->second == zGenNow;
                if (g274ZTimeOn() && prevKnown)
                {
                    if (unchanged) g_g274ZRedundant.fetch_add(1u, std::memory_order_relaxed);
                    else           g_g274ZNecessary.fetch_add(1u, std::memory_order_relaxed);
                }
                // Skip only when NOT a first-init (the resident texture already holds every row for
                // this key) AND VRAM Z is provably unchanged. Bit-exact by construction.
                if (g278PendingSameKey)
                {
                    // VRAM is intentionally stale while G278 owns this key. The backend texture
                    // is the sole current copy, so an upload would clobber newer depth. A pending
                    // owner can exist only after a successful submit/full-init for this key.
                    g274SkipUpload = true;
                    g_g278UploadSkips.fetch_add(1u, std::memory_order_relaxed);
                    // G290: the seeded batch may CONTINUE on the resident rows (the owner union
                    // is confined to rows 0..415 — the wider case published above), but its
                    // aliased rows 416..447 must be re-established to the post-clear state with
                    // a partial upload; the resident copy holds a previous batch's seed window.
                    if (g290Seeded)
                        g290PartialSeed = true;
                }
                else if (g274ZResidentOn() && !fullInit && unchanged && !g282Tight &&
                         !g290Seeded)
                    g274SkipUpload = true;
            }
            if (g290PartialSeed)
            {
                // Rows 416..447 only: x0..63 (physical page 312, VRAM-authoritative — outside
                // the 0x139 color family and never in a pending display-Z row union) from VRAM,
                // x64..511 the structural post-clear zero seed. Rows 0..415 stay resident.
                constexpr int kSeedLo = 416, kSeedHi = 447;
                depthRows = kSeedHi - kSeedLo + 1;
                depthGlY = kFbH - 1 - kSeedHi;
                s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
                constexpr float kZToDepth = 1.0f / 4294967296.0f;
                const uint32_t zbpBlockSeed = guestZbpPage << 5;
                for (int y = kSeedLo; y <= kSeedHi; ++y)
                {
                    const size_t dstRow = static_cast<size_t>(kSeedHi - y) * fbW;
                    for (int x = 0; x < 64; ++x)
                    {
                        const uint32_t z = GSMem::ReadZ24(vram, zbpBlockSeed, fbw,
                                                          static_cast<uint32_t>(x),
                                                          static_cast<uint32_t>(y));
                        s_depthUpload[dstRow + x] = static_cast<float>(z) * kZToDepth;
                    }
                    for (int x = 64; x < fbW; ++x)
                        s_depthUpload[dstRow + static_cast<size_t>(x)] = 0.0f;
                }
                depthUpload = &s_depthUpload;
            }
            else if (g274SkipUpload)
            {
                // renderBatch skips glTexSubImage2D when depthUpload==nullptr and renders against
                // the persistent depth texture as-is. depthRows/depthGlY are then unused.
                depthUpload = nullptr;
                depthRows = depthRowHi - depthRowLo + 1;
                depthGlY = kFbH - 1 - depthRowHi;
                g_g274ZUploadsSkipped.fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                const int upLoZ = fullInit ? 0 : depthRowLo;
                const int upHiZ = fullInit ? (kFbH - 1) : depthRowHi;
                depthRows = upHiZ - upLoZ + 1;
                depthGlY = kFbH - 1 - upHiZ;
                s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
                constexpr float kZToDepth = 1.0f / 4294967296.0f;
                const auto g274ZUpT0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                const auto packDepthRows = [&](int laneLo, int laneHi) {
                    for (int y = laneLo; y <= laneHi; ++y)
                    {
                        const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                        for (int x = 0; x < fbW; ++x)
                        {
                            const uint32_t z = GSMem::ReadZ24(
                                vram, zbpBlock, fbw,
                                static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y));
                            s_depthUpload[dstRow + x] =
                                static_cast<float>(z) * kZToDepth;
                        }
                    }
                };
                const int swizzleLanes =
                    depthRows >= 8 ? g275ZSwizzleLanes() : 1;
                GSRowPool::instance().run(upLoZ, upHiZ, swizzleLanes,
                                          packDepthRows);
                if (g282Tight || g290Seeded)
                {
                    // The batch's leading black CT32 clear writes physical blocks 313..319,
                    // which are this Z layout's y=416..447, x=64..511 pages. Seed the separate
                    // backend depth attachment with the post-clear value before later draws.
                    const int seedLo = std::max(upLoZ, 416);
                    const int seedHi = std::min(upHiZ, 447);
                    for (int y = seedLo; y <= seedHi; ++y)
                    {
                        const size_t dstRow = static_cast<size_t>(upHiZ - y) * fbW;
                        for (int x = 64; x < fbW; ++x)
                            s_depthUpload[dstRow + static_cast<size_t>(x)] = 0.0f;
                    }
                }
                if (g274ZTimeOn())
                    g_g274ZUpNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - g274ZUpT0).count(),
                                          std::memory_order_relaxed);
                depthUpload = &s_depthUpload;
            }
            g_g262DepthWaves.fetch_add(1u, std::memory_order_relaxed);
            if (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp))
                g_g265DepthBatches.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
        G242DepthState &state = g_g242DepthStates[depthKey];
        state.key = depthKey;
        state.zbpPage = guestZbpPage;
        state.zpsm = guestZpsm;
        state.fbw = fbw;
        state.fbW = fbW;
        depthState = &state;
        // A different persistent Z tuple may map the same physical GS pages.  Publish that older
        // owner before this tuple becomes active; never allow two overlapping dirty owners.
        if (!g242SyncOverlappingDepth(vram, guestZbpPage << 5u, fbw, GS_PSM_Z24,
                                      0u, 0u, static_cast<uint32_t>(fbW), kFbH, depthKey))
        {
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        uint64_t guestGenNow = g242GuestGen(state);
        if (state.dirty && state.guestGen != guestGenNow)
        {
            // A CPU writer escaped its mandatory pre-write barrier.  Reading the GPU copy back now
            // would overwrite newer guest bytes, so discard ownership and fall back loudly.
            const uint64_t n = g_g242InvariantFails.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::fprintf(stderr,
                         "[G242:invariant] dirty generation mismatch n=%llu key=0x%llx old=%llu new=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(depthKey),
                         static_cast<unsigned long long>(state.guestGen),
                         static_cast<unsigned long long>(guestGenNow));
            state.valid = false;
            state.dirty = false;
            state.dirtyLo = kFbH;
            state.dirtyHi = -1;
            g_g266FlushRej = kG266FrDepthSetup;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        const bool needUpload = !state.valid || state.guestGen != guestGenNow;
        depthRows = kFbH;
        depthGlY = 0;
        if (needUpload)
        {
            s_depthUpload.resize(static_cast<size_t>(fbW) * depthRows);
            const uint32_t zbpBlock = guestZbpPage << 5;
            constexpr float kZToDepth = 1.0f / 4294967296.0f;
            for (int y = 0; y < kFbH; ++y)
            {
                const size_t dstRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    const uint32_t z = GSMem::ReadZ24(vram, zbpBlock, fbw,
                                                       static_cast<uint32_t>(x),
                                                       static_cast<uint32_t>(y));
                    s_depthUpload[dstRow + x] = static_cast<float>(z) * kZToDepth;
                }
            }
            depthUpload = &s_depthUpload;
            stagedDepthUpload = true;
            stagedGuestGen = guestGenNow;
            g_g242DepthUploads.fetch_add(1u, std::memory_order_relaxed);
        }
        } // end G242 persistent-depth path (G262 per-wave path above)
    }

    const auto g298T2 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // G261 FBO-bind pre-pass: entries that sample a GPU-dirty RTT target either bind the
    // producer's FBO color texture directly (skip VRAM decode entirely — the pixels never
    // round-tripped through VRAM) or, when the access shape is not provably in-range/aligned,
    // force that target's materialization and fall through to the normal decode path.
    static std::vector<uint32_t> s_g261FboSrc;
    static std::vector<uint64_t> s_g281ViewKey;
    static std::vector<int8_t> s_g281ViewSrc;
    bool g261AnyBind = false;
    if ((g261WaveOn() && g261AnyDirty()) || g289DirectDisplaySrc != 0u ||
        g289OwnerViewReady)
    {
        s_g261FboSrc.assign(g_g144List.size(), 0u);
        s_g281ViewKey.assign(g_g144List.size(), 0u);
        s_g281ViewSrc.assign(g_g144List.size(), -1);
        // G280: re-sync stale/orphaned alias-overlay tiles from VRAM before any bind decision
        // this flush makes (including legacy-path binds) can sample them.
        if (g280AliasOn())
            g280HygienePass();
        for (size_t i = 0; i < g_g144List.size(); ++i)
        {
            const G178EntryState &st = s_states[i];
            const G144Entry &e = g_g144List[i];
            if (g289DirectDisplaySrc != 0u && i == 0u)
            {
                s_g261FboSrc[i] = g289DirectDisplaySrc;
                g261AnyBind = true;
                const uint64_t n =
                    g_g289Direct.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:direct] n=%llu src=0x%x dst=0x139 rows=0..415 "
                                 "clampHalo=1\n",
                                 static_cast<unsigned long long>(n),
                                 g289DirectDisplaySrc == kG289ZeroFbpToken
                                     ? 0u : g289DirectDisplaySrc);
                continue;
            }
            if (g289OwnerViewReady && s_g289OwnerSrc[i] != 0u)
            {
                s_g261FboSrc[i] = s_g289OwnerSrc[i];
                g261AnyBind = true;
                continue;
            }
            if (st.texKey == 0 || st.skip || !e.prim.tme)
                continue;

            // G281: close the measured format-crossing consumer before the generic overlap loop
            // materializes 0x13c. Admission proves the exact PSMT8 byte map belongs solely to
            // that dirty FBO and that the CLUT remains VRAM-authoritative. The decoded GPU view
            // is built later with the other texture work.
            if (g281ViewOn() && fbp == 0x143u &&
                e.ctx.tex0.psm == GS_PSM_T8 && e.ctx.tex0.tbp0 == 0x2820u)
            {
                const int srcTi = g281TryEntry(fbp, e, st);
                if (srcTi >= 0)
                {
                    s_g281ViewKey[i] = g281ViewKey(st.texKey);
                    s_g281ViewSrc[i] = static_cast<int8_t>(srcTi);
                    continue;
                }
            }

            // G267 diagnostic: characterize the stable display TRISTRIP consumer with exact
            // bilinear taps and exact CT32 sampled pages. Behavior is intentionally unchanged:
            // the repaired prototype was neutral and was removed at phase close.
            if (g267CensusOn() && g265DisplayTarget(fbp) &&
                e.prim.type == GS_PRIM_TRISTRIP && e.prim.fst && st.bilinear &&
                e.ctx.tex0.psm == GS_PSM_CT32)
            {
                int srcTi = -1;
                for (uint32_t t = 0; t < kG248TargetCount; ++t)
                {
                    const G261Res &r = g_g261Res[t];
                    if (r.dirty && e.ctx.tex0.tbp0 == framePageBaseToBlock(kG261Fbp[t]) &&
                        e.ctx.tex0.tbw == r.fbw)
                    {
                        srcTi = static_cast<int>(t);
                        break;
                    }
                }
                if (srcTi >= 0)
                {
                    g_g267TriCandidates.fetch_add(1u, std::memory_order_relaxed);
                    const G261Res &src = g_g261Res[srcTi];
                    G267TriFootprint fp;
                    const bool footprintOk = g267BilinearTriFootprint(e, st, src, fp);
                    uint32_t aliasMask = 0u;
                    if (footprintOk)
                    {
                        for (uint32_t t = 0; t < kG248TargetCount; ++t)
                        {
                            if (static_cast<int>(t) == srcTi || !g_g261Res[t].dirty)
                                continue;
                            if (g267SamplePagesOverlap(e, fp, g_g261Res[t].range))
                                aliasMask |= 1u << t;
                        }
                    }

                    if (!footprintOk)
                        g_g267TriFootReject.fetch_add(1u, std::memory_order_relaxed);
                    else if (aliasMask != 0u)
                        g_g267TriAliasReject.fetch_add(1u, std::memory_order_relaxed);
                    else
                        g_g267TriEligible.fetch_add(1u, std::memory_order_relaxed);

                    static std::atomic<uint32_t> s_g267Log{0};
                    if (s_g267Log.fetch_add(1u, std::memory_order_relaxed) < 24u)
                        std::fprintf(stderr,
                                     "[G267:tri] dst=%03x src=%03x ok=%u alias=0x%x "
                                     "uv=(%u,%u)(%u,%u)(%u,%u) taps=%d..%d,%d..%d "
                                     "range=%04x..%04x cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u\n",
                                     fbp, kG261Fbp[srcTi], footprintOk ? 1u : 0u,
                                     aliasMask, e.v[0].u, e.v[0].v, e.v[1].u, e.v[1].v,
                                     e.v[2].u, e.v[2].v, fp.tapLoU, fp.tapHiU,
                                     fp.tapLoV, fp.tapHiV,
                                     fp.sampleRange.valid ? fp.sampleRange.first : 0u,
                                     fp.sampleRange.valid ? fp.sampleRange.last : 0u,
                                     src.covLo, src.covHi, src.fbW, st.texW, st.texH,
                                     static_cast<unsigned>(st.wrapU),
                                     static_cast<unsigned>(st.wrapV));
                }
            }
            // G280: fully handle the entry when the bind is provable AND every dirty-sibling
            // physical-page alias resolves with a GPU->GPU page-tile copy (the G267 family).
            // Success replaces the whole legacy per-target loop below for this entry — the
            // sibling's newer pages are already IN the bound FBO, so no materialization is
            // needed. Any rejection falls through to the legacy loop unchanged.
            if (g280AliasOn())
            {
                const uint32_t g280Fbp = g280TryEntry(fbp, e, st);
                if (g280Fbp != 0u)
                {
                    s_g261FboSrc[i] = g280Fbp;
                    g261AnyBind = true;
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
            }
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                G261Res &r = g_g261Res[t];
                if (!r.dirty)
                    continue;
                const uint32_t tBlock = framePageBaseToBlock(kG261Fbp[t]);
                bool bind = false;
                if (e.ctx.tex0.psm == GS_PSM_CT32 && e.ctx.tex0.tbp0 == tBlock &&
                    e.ctx.tex0.tbw == r.fbw && e.prim.fst &&
                    (e.prim.type == GS_PRIM_SPRITE || e.prim.type == GS_PRIM_TRIANGLE ||
                     e.prim.type == GS_PRIM_TRISTRIP || e.prim.type == GS_PRIM_TRIFAN))
                {
                    if (!st.bilinear)
                    {
                    // FST texel extent across the primitive's vertices must lie inside the
                    // FBO-defined row/column window (NEAREST taps only touch [u0..u1) texels).
                    const int nv = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
                    int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
                    for (int k = 0; k < nv; ++k)
                    {
                        const int uu = static_cast<int>(e.v[k].u >> 4);
                        const int vv = static_cast<int>(e.v[k].v >> 4);
                        u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                        v0 = std::min(v0, vv); v1 = std::max(v1, vv);
                    }
                    if (e.prim.type == GS_PRIM_SPRITE)
                    {
                        // Sprite endpoints are exclusive right/bottom (max tap = v1-1 texels).
                        bind = u0 >= 0 && u1 <= r.fbW && v0 >= r.covLo && v1 <= r.covHi + 1;
                    }
                    else
                    {
                        // Triangle interpolation can reach the vertex texel itself — require it
                        // to be inside the defined window outright.
                        bind = u0 >= 0 && u1 <= r.fbW - 1 && v0 >= r.covLo && v1 <= r.covHi;
                    }
                    }
                    else if (e.prim.type == GS_PRIM_SPRITE && g248TargetIndex(fbp) < 0)
                    {
                        // G263: the display composite sprite samples the character RTT with
                        // bilinear filtering — G262 measured it as the ONLY bind rejector
                        // (2728 mat(tex)/run, solely on bilin=1). Admit it when the exact tap
                        // footprint stays FBO-defined; the backend already selects GL_LINEAR
                        // from d.bilinear on the srcFbp path. RTT-family consumers keep the
                        // NEAREST-only rule until a census shape demands more.
                        static const bool s_g263Off = envFlagEnabled("DC2_G263_NO_BILIN_BIND");
                        if (!s_g263Off)
                        {
                            bind = g263BilinearSpriteBind(e, st, r);
                            if (bind)
                                g_g263FboBindBi.fetch_add(1u, std::memory_order_relaxed);
                        }
                    }
                }
                if (bind)
                {
                    s_g261FboSrc[i] = kG261Fbp[t];
                    g261AnyBind = true;
                    g_g261FboBinds.fetch_add(1u, std::memory_order_relaxed);
                }
                else
                {
                    // Any other overlap of this texture/CLUT with the resident target's pages
                    // needs real VRAM bytes — materialize before the decode loop reads them.
                    const G144BlockRange tex = g144TextureRange(e);
                    const G144BlockRange clut = g144IsPalettedPsm(e.ctx.tex0.psm)
                                                    ? g144ClutRange(e)
                                                    : G144BlockRange{};
                    if (!tex.valid || !r.range.valid ||
                        g144RangeOverlaps(r.range, tex) ||
                        (clut.valid && g144RangeOverlaps(r.range, clut)))
                    {
                        // G291: the read needs VRAM bytes, but if every overlapped 64x32 page
                        // of this resident target was FULLY re-covered by noted uploads since
                        // its last wave render, VRAM is already current for exactly the pages
                        // the read touches — the materialize round-trip would be the identity
                        // there. CLUT reads use paletted PSMs whose range test is the same
                        // block interval. Fails closed on any partial page.
                        if (g291AtlasOn() && tex.valid && r.range.valid &&
                            g291PagesCurrent(r, kG261Fbp[t], tex) &&
                            (!clut.valid || g291PagesCurrent(r, kG261Fbp[t], clut)))
                        {
                            const uint64_t n = g_g291TexSkips.fetch_add(
                                                   1u, std::memory_order_relaxed) + 1u;
                            if (g291StatOn() && (n <= 8u || (n % 4096u) == 0u))
                                std::fprintf(stderr,
                                             "[G291:skip] n=%llu t=%03x tex=0x%x..0x%x "
                                             "mats=%llu\n",
                                             (unsigned long long)n, kG261Fbp[t],
                                             tex.first, tex.last,
                                             (unsigned long long)g_g291TexMats.load(
                                                 std::memory_order_relaxed));
                            continue;
                        }
                        if (g291AtlasOn())
                            g_g291TexMats.fetch_add(1u, std::memory_order_relaxed);
                        // G262 diagnosis: WHY did the direct-bind test fail for a real consumer?
                        static std::atomic<uint32_t> s_g262BindRejLog{0};
                        if (g262CensusOn() &&
                            s_g262BindRejLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                            std::fprintf(stderr,
                                         "[G262:bindrej] dstFbp=0x%x src=%03x psm=0x%x tbp=0x%x "
                                         "tbw=%u rfbw=%u fst=%u bilin=%u prim=%u cov=%d..%d "
                                         "wrap=%u%u tex=%dx%d\n",
                                         fbp, kG261Fbp[t], e.ctx.tex0.psm, e.ctx.tex0.tbp0,
                                         e.ctx.tex0.tbw, r.fbw, e.prim.fst ? 1u : 0u,
                                         st.bilinear ? 1u : 0u,
                                         static_cast<unsigned>(e.prim.type), r.covLo, r.covHi,
                                         static_cast<unsigned>(st.wrapU),
                                         static_cast<unsigned>(st.wrapV), st.texW, st.texH);
                        g_g261FboRejects.fetch_add(1u, std::memory_order_relaxed);
                        g266NoteTexBindMat(fbp, t, e, st);
                        g261Materialize(static_cast<int>(t), kG261MatTexAlias);
                    }
                }
            }
        }
    }
    else
    {
        s_g261FboSrc.clear();
        s_g281ViewKey.clear();
        s_g281ViewSrc.clear();
    }
    const bool g261HaveFboSrc = g261AnyBind; // s_g261FboSrc sized to the list only when true

    const auto g298T3 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // Textures: decode + upload anything not resident or invalidated by page-gen changes.
    static std::unordered_set<uint64_t> s_batchKeys;
    s_batchKeys.clear();
    for (size_t i = 0; i < g_g144List.size(); ++i)
    {
        const G178EntryState &st = s_states[i];
        if (g261HaveFboSrc && s_g261FboSrc[i] != 0u)
            continue; // G261: sampled straight from the producer's FBO — no VRAM decode
        const uint64_t effectiveKey =
            (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                ? s_g281ViewKey[i] : st.texKey;
        if (effectiveKey == 0u || st.skip || s_batchKeys.count(effectiveKey))
            continue;
        s_batchKeys.insert(effectiveKey);
        const G144Entry &e = g_g144List[i];
        const GSContext &ctx = e.ctx;
        const bool paletted = ctx.tex0.psm == GS_PSM_T8 || ctx.tex0.psm == GS_PSM_T4 ||
                              ctx.tex0.psm == GS_PSM_T4HL || ctx.tex0.psm == GS_PSM_T4HH;

        if (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
        {
            const int srcTi = s_g281ViewSrc[i];
            static std::vector<uint32_t> s_map, s_clut;
            const bool srcOk = srcTi >= 0;
            const bool syncOk =
                srcOk && g242SyncOverlappingDepth(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                                  0u, 0u, 64u, 32u);
            const bool mapOk =
                syncOk && g281BuildT8Map(e, srcTi, st.texW, st.texH, s_map);
            bool ok = mapOk;
            if (ok)
            {
                s_clut.resize(256u);
                for (uint32_t ci = 0; ci < 256u; ++ci)
                    s_clut[ci] = g178LookupClutFixed(
                        vram, e.texa, e.texclut, static_cast<uint8_t>(ci),
                        ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa,
                        ctx.tex0.psm);
                uint64_t bad = 0u;
                ok = g281_backend_prepare_t8_view(
                    s_g281ViewKey[i], kG261Fbp[static_cast<uint32_t>(srcTi)],
                    g_g261Res[srcTi].fbW, st.texW, st.texH, s_map, s_clut,
                    g281VerifyOn(), bad);
                if (bad != 0u)
                    g_g281VerifyBad.fetch_add(bad, std::memory_order_relaxed);
            }
            if (ok)
            {
                g_g281Views.fetch_add(1u, std::memory_order_relaxed);
                continue;
            }

            // Fail closed before any CPU decode/replay can observe stale guest bytes. Publishing
            // the source restores the exact legacy path; clear every entry sharing this view.
            const uint64_t buildFail =
                g_g281BuildFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g282StatOn() && buildFail <= 8u)
                std::fprintf(stderr,
                             "[G282:g281-fail] n=%llu src=%d srcOk=%d sync=%d map=%d "
                             "dirty=%d range=%d win=%d..%d cov=%d..%d\n",
                             static_cast<unsigned long long>(buildFail), srcTi,
                             srcOk ? 1 : 0, syncOk ? 1 : 0, mapOk ? 1 : 0,
                             srcOk && g_g261Res[srcTi].dirty ? 1 : 0,
                             srcOk && g_g261Res[srcTi].range.valid ? 1 : 0,
                             srcOk ? g_g261Res[srcTi].dirtyLo : -1,
                             srcOk ? g_g261Res[srcTi].dirtyHi : -1,
                             srcOk ? g_g261Res[srcTi].covLo : -1,
                             srcOk ? g_g261Res[srcTi].covHi : -1);
            if (srcTi >= 0)
                g261Materialize(srcTi, kG261MatTexAlias);
            const uint64_t failedKey = s_g281ViewKey[i];
            for (size_t j = 0; j < s_g281ViewKey.size(); ++j)
                if (s_g281ViewKey[j] == failedKey)
                {
                    s_g281ViewKey[j] = 0u;
                    s_g281ViewSrc[j] = -1;
                }
            s_batchKeys.erase(failedKey);
            s_batchKeys.insert(st.texKey);
            // Continue below through the ordinary guest-VRAM texture registry/decode.
        }

        if (guestDepth)
        {
            const uint32_t zbpBlock = guestZbpPage << 5u;
            const bool texAliasesZ = g242RectsOverlap(
                ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u, 0u,
                static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH),
                zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            const bool clutAliasesZ = paletted && g242RectsOverlap(
                ctx.tex0.cbp, 1u, GS_PSM_CT32, 0u, 0u, 64u, 32u,
                zbpBlock, fbw, GS_PSM_Z24, 0u, static_cast<uint32_t>(depthRowLo),
                static_cast<uint32_t>(fbW),
                static_cast<uint32_t>(depthRowHi - depthRowLo + 1));
            if (texAliasesZ || clutAliasesZ)
            {
                static std::atomic<uint32_t> s_aliasTexLog{0};
                const uint32_t n = s_aliasTexLog.fetch_add(1u, std::memory_order_relaxed);
                if (g242StatEnabled() && n < 24u)
                    std::fprintf(stderr,
                                 "[G242:alias] texture-depth n=%u tbp=0x%x psm=0x%x cbp=0x%x zbp=0x%x zRows=%d..%d tex=%dx%d texHit=%u clutHit=%u\n",
                                 n + 1u, ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.cbp,
                                 guestZbpPage, depthRowLo, depthRowHi, st.texW, st.texH,
                                 texAliasesZ ? 1u : 0u, clutAliasesZ ? 1u : 0u);
                g_g266FlushRej = kG266FrTexAliasZ;
                g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                g_g178RejectTex.fetch_add(1u, std::memory_order_relaxed);
                g_g178FbSnap.clear();
                return false;
            }
        }
        if (!g242SyncOverlappingDepth(vram, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                                      0u, 0u, static_cast<uint32_t>(st.texW),
                                      static_cast<uint32_t>(st.texH)) ||
            (paletted && !g242SyncOverlappingDepth(vram, ctx.tex0.cbp, 1u, GS_PSM_CT32,
                                                   0u, 0u, 64u, 32u)))
        {
            g_g266FlushRej = kG266FrTexAliasZ;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        const uint64_t genNow =
            g178GenSumRect(ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0, 0,
                           static_cast<uint32_t>(st.texW), static_cast<uint32_t>(st.texH)) +
            g178GenSumRect(ctx.tex0.cbp, 1, GS_PSM_CT32, 0, 0, 64, 32); // CLUT page(s)
        auto it = g_g178TexReg.find(st.texKey);
        const bool backendHas = g178_backend_has_tex(st.texKey);
        if (it != g_g178TexReg.end() && backendHas && it->second.genSum == genNow)
        {
            g_g178TexHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        // Gen mismatch (or unknown): DC2 re-uploads texture data every frame, usually byte-
        // identical — hash the source pages before paying the swizzle+CLUT decode. Hash-equal +
        // still resident → refresh the gen stamp and keep the GPU copy.
        const uint64_t hashNow = g178HashPages(vram, vramSize, ctx.tex0.tbp0, ctx.tex0.tbw,
                                               ctx.tex0.psm, static_cast<uint32_t>(st.texW),
                                               static_cast<uint32_t>(st.texH), ctx.tex0.cbp);
        if (it != g_g178TexReg.end() && backendHas && it->second.contentHash == hashNow)
        {
            it->second.genSum = genNow;
            g_g178TexHashHits.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        G178TexUpload up;
        up.key = st.texKey;
        up.w = st.texW;
        up.h = st.texH;
        up.px.resize(static_cast<size_t>(st.texW) * st.texH);
        g178DecodeWhole(self, gs, vram, ctx, e.texa, e.texclut, st.texW, st.texH, up.px.data());
        s_batch.texUploads.push_back(std::move(up));
        g_g178TexReg[st.texKey] = G178TexReg{genNow, hashNow, st.texW, st.texH};
        g_g178TexDecodes.fetch_add(1u, std::memory_order_relaxed);
    }

    const auto g298T4 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // Vertices + state-batched draw runs (submission order preserved — no sorting; blending
    // depends on order).
    for (size_t i = 0; i < g_g144List.size(); ++i)
    {
        const G178EntryState &st = s_states[i];
        if (st.skip)
            continue;
        const G144Entry &e = g_g144List[i];
        const int ofx = e.ctx.xyoffset.ofx >> 4;
        const int ofy = e.ctx.xyoffset.ofy >> 4;
        const int first = static_cast<int>(s_batch.verts.size());
        // G261 FBO-bound entry: normalize FST UVs against the PRODUCER target's full fbW x 512
        // space instead of the declared texture dims; the V axis is flipped after the push
        // (GS row y lives at GL row 511-y in the persistent FBO).
        const bool g261Bound = g261HaveFboSrc && s_g261FboSrc[i] != 0u;
        const uint64_t drawTexKey =
            (!s_g281ViewKey.empty() && s_g281ViewKey[i] != 0u)
                ? s_g281ViewKey[i] : st.texKey;
        int pushTexW = st.texW, pushTexH = st.texH;
        if (g261Bound)
        {
            if (s_g261FboSrc[i] == kG289ZeroFbpToken ||
                g289IsOwnerSourceToken(s_g261FboSrc[i]) ||
                s_g261FboSrc[i] == 0x68u)
            {
                pushTexW = 512;
                pushTexH = 512;
            }
            else
            {
                const int srcTi = g248TargetIndex(s_g261FboSrc[i]);
                pushTexW = g_g261Res[srcTi].fbW;
                pushTexH = 512;
            }
        }
        if (g270LineType(static_cast<uint32_t>(e.prim.type)))
        {
            g270AppendLinePixels(s_batch.verts, e);
        }
        else if (e.prim.type == GS_PRIM_SPRITE)
        {
            // drawSprite: rect [min..max) exclusive right/bottom, FLAT color from v1, UV linearly
            // interpolated with the G8 LEFT-EDGE (0.0) bias — GL samples at pixel centers (+0.5),
            // so shift the UVs by -0.5 * (texel step per pixel) to reproduce the CPU convention.
            const GSVertex &a = e.v[0];
            const GSVertex &b = e.v[1];
            GSVertex q0 = e.v[0], q1 = e.v[1];
            if (g255Rtt || g286Transient)
            {
                // drawSprite first truncates screen positions, sorts the bounds, and expands a
                // zero span to one pixel.  The RTT effects deliberately submit full rectangles
                // plus degenerate edge/corner sprites; raw GL triangles silently drop the latter.
                int sx0 = static_cast<int>(a.x) - ofx;
                int sy0 = static_cast<int>(a.y) - ofy;
                int sx1 = static_cast<int>(b.x) - ofx;
                int sy1 = static_cast<int>(b.y) - ofy;
                if (sx0 > sx1) std::swap(sx0, sx1);
                if (sy0 > sy1) std::swap(sy0, sy1);
                q0.x = static_cast<float>(sx0 + ofx);
                q0.y = static_cast<float>(sy0 + ofy);
                q1.x = static_cast<float>(sx0 + std::max(1, sx1 - sx0) + ofx);
                q1.y = static_cast<float>(sy0 + std::max(1, sy1 - sy0) + ofy);
                if (e.prim.fst)
                {
                    // CPU FST sprites derive endpoints with integer `u >> 4` / `v >> 4` before
                    // interpolation. Preserve that truncation instead of feeding fractional 12.4
                    // endpoints into GL.
                    q0.u = static_cast<uint16_t>((q0.u >> 4u) << 4u);
                    q0.v = static_cast<uint16_t>((q0.v >> 4u) << 4u);
                    q1.u = static_cast<uint16_t>((q1.u >> 4u) << 4u);
                    q1.v = static_cast<uint16_t>((q1.v >> 4u) << 4u);
                }
            }
            float uvShiftU = 0.0f, uvShiftV = 0.0f;
            if (e.prim.tme && e.prim.fst)
            {
                const float dxp = q1.x - q0.x; // positions are already pixel-unit floats (kick /16)
                const float dyp = q1.y - q0.y;
                if (dxp != 0.0f)
                    uvShiftU = -0.5f * (static_cast<float>(q1.u) - static_cast<float>(q0.u)) / 16.0f / dxp;
                if (dyp != 0.0f)
                    uvShiftV = -0.5f * (static_cast<float>(q1.v) - static_cast<float>(q0.v)) / 16.0f / dyp;
            }
            // Two triangles covering the rect; UVs pair with the ORIGINAL corners (axis-aligned).
            GSVertex c00 = q0, c11 = q1, c10 = q0, c01 = q0;
            c10.x = q1.x; c10.u = q1.u; // (x1, y0)
            c01.y = q1.y; c01.v = q1.v; // (x0, y1)
            // The GS uses the second kick's Z as one flat value for a sprite. Interpolating the
            // two endpoint Z values would turn a clear sprite into a depth ramp.
            c00.z = c10.z = c11.z = c01.z = b.z;
            const bool tme = e.prim.tme, fst = e.prim.fst;
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c10, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c00, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c11, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
            g178PushVertex(s_batch.verts, c01, ofx, ofy, b, tme, fst, pushTexW, pushTexH, uvShiftU, uvShiftV);
        }
        else
        {
            // Triangle: gouraud (iip=1) uses per-vertex color; flat uses v2's (drawTriangle).
            const bool tme = e.prim.tme, fst = e.prim.fst, iip = e.prim.iip;
            for (int k = 0; k < 3; ++k)
                g178PushVertex(s_batch.verts, e.v[k], ofx, ofy, iip ? e.v[k] : e.v[2],
                               tme, fst, pushTexW, pushTexH, 0.0f, 0.0f);
        }
        const int count = static_cast<int>(s_batch.verts.size()) - first;
        if (g261Bound)
        {
            // GS texel row v ↔ FBO GL row 511-v: flip the normalized T coordinate. Linear in
            // screen space, so a per-vertex flip is exact under noperspective interpolation.
            for (int k = first; k < first + count; ++k)
                s_batch.verts[static_cast<size_t>(k)].tq =
                    1.0f - s_batch.verts[static_cast<size_t>(k)].tq;
        }
        if (!g256ExactRttOn() && !s_batch.draws.empty() &&
            g178StateEqual(s_states[i], s_states[i - 1]) &&
            (!g261HaveFboSrc || s_g261FboSrc[i] == s_g261FboSrc[i - 1]) &&
            s_batch.draws.back().texKey == (g261Bound ? 0u : drawTexKey) &&
            s_batch.draws.back().firstVtx + s_batch.draws.back().vtxCount == first)
        {
            s_batch.draws.back().vtxCount += count;
        }
        else
        {
            G178Draw d;
            d.firstVtx = first;
            d.vtxCount = count;
            d.texKey = g261Bound ? 0u : drawTexKey;
            d.srcFbp = g261Bound ? s_g261FboSrc[i] : 0u;
            d.blend = st.blend;
            d.tfx = st.tfx;
            d.tcc = st.tcc;
            d.depthFunc = st.depthFunc;
            d.depthWrite = st.depthWrite;
            d.bilinear = st.bilinear;
            d.wrapU = st.wrapU;
            d.wrapV = st.wrapV;
            d.scX0 = st.scX0; d.scY0 = st.scY0; d.scX1 = st.scX1; d.scY1 = st.scY1;
            s_batch.draws.push_back(d);
        }
    }

    const auto g298T5 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // G255 verification is intentionally color-only. Any batch that needs real guest depth has
    // already failed closed unless the separately opt-in G242 ownership model is active; do not
    // combine those two experiments in one oracle.
    if ((g255Rtt || (g286Transient && g286VerifyOn())) && !guestDepth)
        g255BeginVerify(vram, vramSize, fbp, fbw, fbW, rowLo, rowHi);

    const auto g273T1 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const int g275ReadRows = g262WaveDepth ? (depthRowHi - depthRowLo + 1) : 0;
    const int g275ReadGlY = g262WaveDepth ? (kFbH - 1 - depthRowHi) : 0;
    const bool g278DeferDepth =
        g278DepthRbOn() && g262WaveDepth && g278OwnerEligible && guestDepthWrites &&
        !g275FusedZReadOn();
    const bool g275FuseReadback =
        g262WaveDepth && guestDepthWrites && g275FusedZReadOn() &&
        !g278DeferDepth;
    static std::vector<float> s_g262DepthReadback;
    if (g262WaveDepth && guestDepthWrites && !g278DeferDepth)
        s_g262DepthReadback.clear();
    // G274: time EVERY synchronous backend submit (RTT color/depth waves AND display batches),
    // not only G273's display profile. This is the blocking future to the GPU worker; the
    // hypothesis is that per-wave submit/sync latency dominates the pending-graph drain.
    const bool g276Probe = g276ProbeOn();
    const auto g274SubT0 = (g274ZTimeOn() || g276Probe) ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    const auto g275FusedT0 = g275FuseReadback ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    const bool submitted = g275FuseReadback
        ? g275_backend_submit_depth_readback(
              s_batch, depthKey, depthUpload, depthGlY, depthRows,
              g275ReadGlY, g275ReadRows, s_g262DepthReadback)
        : (guestDepth
               ? g242_backend_submit_depth(s_batch, depthKey, depthUpload,
                                           depthGlY, depthRows)
               : g178_backend_submit(s_batch));
    if (g275FuseReadback)
    {
        g_g275FusedNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g275FusedT0).count(),
            std::memory_order_relaxed);
        if (submitted)
        {
            g_g275FusedJobs.fetch_add(1u, std::memory_order_relaxed);
            g_g275ReadWaitsElided.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    if (g274ZTimeOn() || g276Probe)
    {
        const uint64_t subNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - g274SubT0).count();
        g_g274SubNs.fetch_add(subNs, std::memory_order_relaxed);
        g_g274SubN.fetch_add(1u, std::memory_order_relaxed);
        if (g276Probe)
        {
            const int si = s_batch.skipReadback ? 1 : 0;
            const int di = guestDepthWrites ? 1 : 0;
            g_g276SubNs[si][di].fetch_add(subNs, std::memory_order_relaxed);
            g_g276SubN[si][di].fetch_add(1u, std::memory_order_relaxed);
        }
    }
    const auto g273T2 = s_g273Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto g298T6 = s_g298Profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (!submitted)
    {
        // A G281 view kept its producer newer in the FBO. If the consumer batch cannot submit
        // and will replay on the CPU, publish that producer first so CPU sampling sees the same
        // bytes. This is a failure-only completion edge; successful batches remain readback-free.
        if (!s_g281ViewSrc.empty())
        {
            bool done[kG248TargetCount] = {};
            for (int8_t srcTi : s_g281ViewSrc)
                if (srcTi >= 0 && srcTi < static_cast<int8_t>(kG248TargetCount) &&
                    !done[static_cast<uint8_t>(srcTi)])
                {
                    done[static_cast<uint8_t>(srcTi)] = true;
                    g261Materialize(srcTi, kG261MatTexAlias);
                }
        }
        g_g266FlushRej = kG266FrSubmit;
        g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
        g_g178FbSnap.clear(); // the CPU replay about to run writes VRAM the gens don't track
        return false;
    }
    if (g282Tight)
    {
        // The leading clear made every planned source page independent of its old contents.
        // Publish the completed GPU result to the resident sibling views now, while command
        // order is exact and before any of them can materialize stale bytes.
        if (!g282ExecuteFanout(s_g282Fanout))
        {
            g282DropResidencyAfterFanoutFailure();
            g_g266FlushRej = kG266FrSubmit;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        g282CommitFanout(s_g282Fanout);
    }
    if (depthState && stagedDepthUpload)
    {
        // Commit ownership only after the backend accepted and completed the whole batch.
        depthState->valid = true;
        depthState->guestGen = stagedGuestGen;
    }
    if (g262WaveDepth)
        g_g262DepthInit.insert(depthKey); // full-init (or window update) reached the texture
    if (g262WaveDepth && guestDepthWrites && !g278DeferDepth)
    {
        // G262: materialize the wave's Z writes into guest VRAM immediately (the wave-granular
        // barrier). Between waves VRAM Z stays authoritative, so nothing persists on the GPU and
        // there is no ownership invariant to trip. Same float→uint24 packing as g242SyncDepthState.
        // Readback covers only the depth ROW WINDOW (scissor confines Z writes to it), even when
        // the upload was a full first-init.
        const int rbRows = g275ReadRows;
        const int rbGlY = g275ReadGlY;
        const auto g274ZRbT0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
        if ((!g275FuseReadback &&
             !g242_backend_read_depth(depthKey, fbW, kFbH, rbGlY, rbRows,
                                      s_g262DepthReadback)) ||
            s_g262DepthReadback.size() != static_cast<size_t>(fbW) * rbRows)
        {
            // Fail loudly and fall back: the FBO result is orphaned (snapshot cleared → next
            // flush re-uploads from VRAM) and the CPU replay re-renders color AND Z into VRAM,
            // so guest state stays coherent.
            std::fprintf(stderr, "[G262:depth] Z readback FAIL fbp=0x%x rows=%d..%d\n",
                         fbp, depthRowLo, depthRowHi);
            g_g266FlushRej = kG266FrZReadback;
            g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
            g_g178FbSnap.clear();
            return false;
        }
        constexpr double kDepthToZ = 4294967296.0;
        const uint32_t zbpBlock = guestZbpPage << 5u;
        const auto unpackDepthRows = [&](int laneLo, int laneHi) {
            for (int y = laneLo; y <= laneHi; ++y)
            {
                const size_t srcRow = static_cast<size_t>(depthRowHi - y) * fbW;
                for (int x = 0; x < fbW; ++x)
                {
                    double scaled =
                        static_cast<double>(s_g262DepthReadback[srcRow + x]) *
                        kDepthToZ;
                    if (scaled < 0.0) scaled = 0.0;
                    if (scaled > 16777215.0) scaled = 16777215.0;
                    GSMem::WriteZ24(vram, zbpBlock, fbw,
                                    static_cast<uint32_t>(x),
                                    static_cast<uint32_t>(y),
                                    static_cast<uint32_t>(scaled));
                }
            }
        };
        const int swizzleLanes =
            rbRows >= 8 ? g275ZSwizzleLanes() : 1;
        GSRowPool::instance().run(depthRowLo, depthRowHi, swizzleLanes,
                                  unpackDepthRows);
        // Publish the Z bytes exactly as an inline CPU Z writer would (the G260 ownership rule):
        // page write-generations for range/gen consumers, plus the coarse decoded-texture gen
        // (a Z page can alias texture/CLUT storage).
        g178BumpRectImpl(zbpBlock, fbw, GS_PSM_Z24, 0u,
                         static_cast<uint32_t>(depthRowLo), static_cast<uint32_t>(fbW),
                         static_cast<uint32_t>(rbRows));
        g149BumpVramGen();
        if (g274ZTimeOn())
            g_g274ZRbNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - g274ZRbT0).count(),
                                  std::memory_order_relaxed);
        if (g274ZTimeOn() || g274ZResidentOn())
            // Baseline the whole-target Z gen AFTER our own writeback bump so the next same-key
            // upload can tell whether any OTHER writer touched VRAM Z in between. This readback
            // just synced GPU->VRAM, so the resident texture now equals VRAM at this gen.
            g_g274LastZGen[depthKey] = g178GenSumRect(zbpBlock, fbw, GS_PSM_Z24, 0u, 0u,
                                                      static_cast<uint32_t>(fbW), 512u);
        g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
        if (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp))
            g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
        if (g277CensusOn())
        {
            // Simulated within-drain Z-readback coalescing: this readback is coalescable when the
            // previous same-key readback's run is still live (no real VRAM-Z consumer edge and no
            // cross-key Z wave in between — a different-key wave re-reads VRAM Z for its upload).
            g_g277ZRb.fetch_add(1u, std::memory_order_relaxed);
            if (s_g277ZRunLive && s_g277ZRunKey == depthKey)
                g_g277ZRbCoalescable.fetch_add(1u, std::memory_order_relaxed);
            s_g277ZRunLive = true;
            s_g277ZRunKey = depthKey;
            s_g277ZRunRange = g144RangeForRect(zbpBlock, GS_PSM_Z24, fbw, 0u, 0u,
                                               static_cast<uint32_t>(fbW), 512u);
        }
    }
    else if (g278DeferDepth)
    {
        const bool continuing = s_g278Pend.active;
        if (continuing &&
            (s_g278Pend.key != depthKey || s_g278Pend.vram != vram ||
             s_g278Pend.vramSize != vramSize))
        {
            const uint64_t n =
                g_g278InvariantFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::fprintf(stderr,
                         "[G278:invariant] pending owner changed after submit n=%llu "
                         "old=0x%llx new=0x%llx\n",
                         (unsigned long long)n,
                         (unsigned long long)s_g278Pend.key,
                         (unsigned long long)depthKey);
            if (!g278FlushPendingDepth(0))
            {
                g_g266FlushRej = kG266FrZReadback;
                g_g178Fallbacks.fetch_add(1u, std::memory_order_relaxed);
                g_g178FbSnap.clear();
                return false;
            }
        }
        if (!s_g278Pend.active)
        {
            s_g278Pend.active = true;
            s_g278Pend.key = depthKey;
            s_g278Pend.vram = vram;
            s_g278Pend.vramSize = vramSize;
            s_g278Pend.zbpPage = guestZbpPage;
            s_g278Pend.fbw = fbw;
            s_g278Pend.fbW = fbW;
            s_g278Pend.rowLo = depthRowLo;
            s_g278Pend.rowHi = depthRowHi;
        }
        else
        {
            s_g278Pend.rowLo = std::min(s_g278Pend.rowLo, depthRowLo);
            s_g278Pend.rowHi = std::max(s_g278Pend.rowHi, depthRowHi);
            g_g278Saved.fetch_add(1u, std::memory_order_relaxed);
        }
        s_g278Pend.display =
            s_g278Pend.display ||
            (!g255Rtt && g265DisplayZWaveOn() && g265DisplayTarget(fbp));
        const uint64_t deferred =
            g_g278Deferred.fetch_add(1u, std::memory_order_relaxed) + 1u;
        // Timeout-driven harnesses do not run process-exit reporting. Keep the stat useful even
        // when coalescing holds the flush count below its normal 256-event print cadence.
        if (g278StatOn() && (deferred % 256u) == 0u)
            g278Report(0u);
    }

    // Write the rendered rows back into guest VRAM (same addressing as writePixel) so the
    // present latch / frame dumps / any guest read sees the GPU result.
    if (s_batch.readback.size() == static_cast<size_t>(fbW) * kFbH)
    {
        if (g276ProbeOn())
        {
            g_g276ColorRbN.fetch_add(1u, std::memory_order_relaxed);
            g_g276ColorRbWinRows.fetch_add(
                static_cast<uint64_t>(rowHi - rowLo + 1), std::memory_order_relaxed);
            g_g276ColorRbFullRows.fetch_add(static_cast<uint64_t>(kFbH),
                                            std::memory_order_relaxed);
        }
        for (int y = rowLo; y <= rowHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(kFbH - 1 - y) * fbW;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                            static_cast<uint32_t>(x),
                                                            static_cast<uint32_t>(y));
                if (off + 4u <= vramSize)
                    std::memcpy(vram + off, &s_batch.readback[srcRow + x], 4);
            }
        }
        // The color and depth backends are distinct even when their GS storage aliases. The
        // admitted write windows are disjoint, so this cannot overwrite pending dirty Z rows;
        // remember any other affected Z rows so a later same-key wave cannot use stale depth.
        if (s_g278Pend.active && g278ColorWrite.valid)
            g278MarkStaleRowsForRange(g278ColorWrite);
        if (g286Transient)
        {
            // Unlike display color, this work target is consumed later as ordinary VRAM. Publish
            // the same page generations an inline draw would advance, after bytes really changed.
            g178BumpRectImpl(fbBlock, fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                             static_cast<uint32_t>(rowHi - rowLo + 1));
            fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                                      static_cast<uint32_t>(fbW), kFbH);
            const uint64_t done =
                g_g286GpuSuccess.fetch_add(1u, std::memory_order_relaxed) + 1u;
            g_g286RowsPublished.fetch_add(
                static_cast<uint64_t>(rowHi - rowLo + 1), std::memory_order_relaxed);
            if (g286StatOn() && (done <= 8u || (done % 120u) == 0u))
                std::fprintf(stderr,
                             "[G287:transient] gpu=%llu attempts=%llu captured=%llu "
                             "rows=%llu fbp=%03x fbw=%u win=%d..%d entries=%zu\n",
                             (unsigned long long)done,
                             (unsigned long long)g_g286GpuAttempts.load(std::memory_order_relaxed),
                             (unsigned long long)g_g286Captured.load(std::memory_order_relaxed),
                             (unsigned long long)g_g286RowsPublished.load(std::memory_order_relaxed),
                             fbp, fbw, rowLo, rowHi, g_g144List.size());
            if (g289ConsumeOwner)
            {
                const uint64_t version = s_g289Owner.version;
                s_g289Owner.active = false;
                s_g289PendingUpload.active = false;
                g289Guard.armed = false;
                const uint64_t n =
                    g_g289Consumes.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
                    std::fprintf(stderr,
                                 "[G289:consume] n=%llu version=%llu fbp=0x13b rows=0..415\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(version));
            }
        }
    }
    if (g272Wave)
    {
        // Display color remains in the persistent FBO until an actual VRAM consumer or the
        // presentation boundary. Page generations advance only when materialization changes VRAM.
        g272MarkDirty(fbp, fbw, fbW, rowLo, rowHi, vramSize);
    }
    if (g255Rtt)
    {
        const int ti = g248TargetIndex(fbp);
        if (g261Wave)
        {
            // G261 wave: VRAM did NOT change (readback skipped), so the page generations stay
            // untouched — the ownership edge is carried by the residency record instead. The
            // generations advance when (and only when) the rows materialize at a real CPU
            // consumer edge, which is when the VRAM bytes actually change.
            G261Res &r = g_g261Res[ti];
            r.fbw = fbw;
            r.fbW = fbW;
            r.dirty = true;
            if (rowLo < r.dirtyLo) r.dirtyLo = rowLo;
            if (rowHi > r.dirtyHi) r.dirtyHi = rowHi;
            if (g291AtlasOn() && r.g291CovFbw != 0u)
            {
                // G291: the wave just rendered these rows on the GPU — the FBO is newer than
                // VRAM for every pixel it may have written. Conservative whole-row clear
                // (scissor columns ignored, fail-closed direction).
                for (int y = rowLo; y <= rowHi && y < 512; ++y)
                    r.g291Cov[static_cast<size_t>(y)].fill(0u);
            }
            // G280: this wave changed the target's FBO content — stale any alias copies made
            // FROM it and drop any of its OWN overlay tiles the rendered rows overlap (the
            // draw layered over the resolved page state exactly as GS command order would).
            if (s_batch.uploadFb)
                g280NoteFboContentChange(ti, 0, 511);
            g280NoteFboContentChange(ti, rowLo, rowHi);
            if (g282Tight)
                g282CommitSourceOverlays(s_g282Fanout);
            // window/range/gen baseline refresh happens after the snapshot row update below
            // (cov must include this flush's rendered rows first).
            g_g261SkippedRb.fetch_add(1u, std::memory_order_relaxed);
            const uint64_t nWaves =
                g_g261Waves.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((nWaves % 1024u) == 0u ||
                (g283StatOn() && (nWaves % 128u) == 0u)) // G283 bring-up observability
                g261Report();
        }
        else
        {
            // The RTT color write is a texture-producer event. Advance the same page generations
            // used by G178's texture registry before a later display-target composite asks
            // whether its GPU copy is still resident. This is the ownership edge the dormant
            // G252 GPU switch lacked.
            g178BumpRectImpl(fbBlock, fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(rowLo), static_cast<uint32_t>(fbW),
                             static_cast<uint32_t>(rows));
            fbGenNow = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                                      static_cast<uint32_t>(fbW), kFbH);
        }
        if (ti >= 0)
            g_g255Gpu[ti].fetch_add(1u, std::memory_order_relaxed);
    }
    if (depthState)
    {
        if (guestDepthWrites)
        {
            depthState->dirty = true;
            depthState->dirtyLo = std::min(depthState->dirtyLo, depthRowLo);
            depthState->dirtyHi = std::max(depthState->dirtyHi, depthRowHi);
        }
        static std::atomic<uint64_t> s_g242DepthBatches{0};
        static std::atomic<uint64_t> s_g242DepthPixels{0};
        const uint64_t nDepth = s_g242DepthBatches.fetch_add(1u, std::memory_order_relaxed) + 1u;
        s_g242DepthPixels.fetch_add(static_cast<uint64_t>(fbW) *
                                        static_cast<uint64_t>(depthRowHi - depthRowLo + 1),
                                    std::memory_order_relaxed);
        static const bool s_g242Stat = envFlagEnabled("DC2_G242_STAT");
        if (s_g242Stat && (nDepth % 120u) == 0u)
            std::fprintf(stderr,
                "[G242:depth] batches=%llu pixels=%llu uploads=%llu syncs=%llu "
                "last(zbp=0x%x rows=%d..%d write=%u dirty=%u)\n",
                static_cast<unsigned long long>(nDepth),
                static_cast<unsigned long long>(s_g242DepthPixels.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_g242DepthUploads.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_g242DepthSyncs.load(std::memory_order_relaxed)),
                guestZbpPage, depthRowLo, depthRowHi, guestDepthWrites ? 1u : 0u,
                 depthState->dirty ? 1u : 0u);
    }
    if (g272Wave || (g276Coalesce && g262WaveDepth && guestDepthWrites))
    {
        // G265 can publish its non-persistent Z result after fbGenNow was sampled for the upload
        // decision. Normal color readback is written after that Z publication and is therefore the
        // effective winner for any conservative page alias; retained FBO color has the same order.
        // Re-anchor the snapshot now so the next display batch cannot upload pre-wave VRAM over it.
        // G277-R: G276-deferred batches need the same re-anchor for their OWN Z writeback bump —
        // without it the next same-target batch re-uploads stale VRAM over unpublished FBO rows
        // (the Max-body flicker). Same-thread flush ownership makes the re-sample exact: only this
        // flush's own writeback bumped between the sample above and here. Uses the same gen window
        // as the upload decision so the two sums stay comparable.
        const uint64_t postDepthGen = (g277GenRows == 416)
            ? g277GenSumAligned(fbBlock, fbw, 416u)
            : g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u, 0u,
                             static_cast<uint32_t>(fbW), kFbH);
        if (postDepthGen != fbGenNow)
            g_g272GenMoves.fetch_add(1u, std::memory_order_relaxed);
        fbGenNow = postDepthGen;
    }
    // Color FBO == VRAM for this fbp now (depth may remain GPU-owned until the next boundary).
    // G261/G272 wave: FBO is AHEAD of VRAM instead of equal — snap.genSum == current VRAM gens
    // still means "do not upload stale VRAM over the FBO"; the residency record carries the
    // dirty-row obligation until a consumer edge materializes it.
    snap.genSum = fbGenNow;
    snap.valid = true;
    if (rowLo < snap.rowLo) snap.rowLo = rowLo;
    if (rowHi > snap.rowHi) snap.rowHi = rowHi;
    if (g277GenMoveOn() && fbp == 0x68u)
    {
        for (int i = 0; i < 13; ++i)
            s_g277Chunk[i] = g178GenSumRect(fbBlock, fbw, GS_PSM_CT32, 0u,
                                            static_cast<uint32_t>(i * 32),
                                            static_cast<uint32_t>(fbW), 32u);
        s_g277ChunkValid = true;
    }
    if (g261Wave)
    {
        const int ti = g248TargetIndex(fbp);
        G261Res &r = g_g261Res[ti];
        r.covLo = snap.rowLo;
        r.covHi = snap.rowHi;
        g261UpdateWindow(ti); // re-anchor claimed rows + gen baseline for the new residency state
    }
    if (g276Coalesce)
    {
        // The batch's draws are now in the persistent FBO but its color readback was deferred.
        // Extend the pending union (same target, guaranteed by the different-target flush above).
        s_g276Pend.active = true;
        s_g276Pend.fbp = fbp;
        s_g276Pend.fbw = fbw;
        s_g276Pend.fbBlock = fbBlock;
        s_g276Pend.fbW = fbW;
        if (rowLo < s_g276Pend.rowLo) s_g276Pend.rowLo = rowLo;
        if (rowHi > s_g276Pend.rowHi) s_g276Pend.rowHi = rowHi;
        g_g276Deferred.fetch_add(1u, std::memory_order_relaxed);
    }

    const uint64_t nGpu = g_g178GpuFlushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (s_g298Profile)
    {
        static uint64_t s_classNs = 0u, s_fbzNs = 0u, s_depsNs = 0u, s_texNs = 0u,
                        s_vertsNs = 0u, s_submitNs = 0u, s_postNs = 0u;
        const auto g298T7 = std::chrono::steady_clock::now();
        const auto ns = [](auto a, auto b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        s_classNs += ns(g298T0, g298T1);
        s_fbzNs += ns(g298T1, g298T2);
        s_depsNs += ns(g298T2, g298T3);
        s_texNs += ns(g298T3, g298T4);
        s_vertsNs += ns(g298T4, g298T5);
        s_submitNs += ns(g298T5, g298T6);
        s_postNs += ns(g298T6, g298T7);
        if ((nGpu % 300u) == 0u)
        {
            const double denom = 1.0e6 * static_cast<double>(nGpu);
            std::fprintf(stderr,
                         "[G298:profile] gpu=%llu avgMs(class=%.3f fbz=%.3f deps=%.3f "
                         "tex=%.3f verts=%.3f submit=%.3f post=%.3f)\n",
                         static_cast<unsigned long long>(nGpu),
                         static_cast<double>(s_classNs) / denom,
                         static_cast<double>(s_fbzNs) / denom,
                         static_cast<double>(s_depsNs) / denom,
                         static_cast<double>(s_texNs) / denom,
                         static_cast<double>(s_vertsNs) / denom,
                         static_cast<double>(s_submitNs) / denom,
                         static_cast<double>(s_postNs) / denom);
        }
    }
    if (s_g273Profile)
    {
        static uint64_t s_preNs = 0u, s_submitNs = 0u, s_postNs = 0u;
        static uint64_t s_entries = 0u, s_verts = 0u, s_draws = 0u;
        const auto g273T3 = std::chrono::steady_clock::now();
        s_preNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T1 - g273T0).count());
        s_submitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T2 - g273T1).count());
        s_postNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            g273T3 - g273T2).count());
        s_entries += g_g144List.size();
        s_verts += s_batch.verts.size();
        s_draws += s_batch.draws.size();
        if ((nGpu % 300u) == 0u)
            std::fprintf(stderr,
                         "[G273:profile] gpu=%llu avgMs(pre=%.3f submit=%.3f post=%.3f) avg(entries=%.1f verts=%.1f draws=%.1f)\n",
                         static_cast<unsigned long long>(nGpu),
                         static_cast<double>(s_preNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_submitNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_postNs) / 1.0e6 / nGpu,
                         static_cast<double>(s_entries) / nGpu,
                         static_cast<double>(s_verts) / nGpu,
                         static_cast<double>(s_draws) / nGpu);
    }
    static const bool s_stat = (std::getenv("DC2_G178_STAT") != nullptr);
    if (s_stat && (nGpu % 300u) == 0u)
        std::fprintf(stderr,
            "[G178:stat] flushes=%llu gpu=%llu fallback=%llu texDecodes=%llu texHits=%llu "
            "texHashHits=%llu fbUp=%llu fbSkip=%llu rej(state=%llu blend=%llu tex=%llu) "
            "lastBatch(verts=%zu draws=%zu texUps=%zu rows=%d)\n",
            (unsigned long long)g_g178Flushes.load(std::memory_order_relaxed),
            (unsigned long long)nGpu,
            (unsigned long long)g_g178Fallbacks.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexDecodes.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexHits.load(std::memory_order_relaxed),
            (unsigned long long)g_g178TexHashHits.load(std::memory_order_relaxed),
            (unsigned long long)g_g178FbUploads.load(std::memory_order_relaxed),
            (unsigned long long)g_g178FbUploadSkips.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectState.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectBlend.load(std::memory_order_relaxed),
            (unsigned long long)g_g178RejectTex.load(std::memory_order_relaxed),
            s_batch.verts.size(), s_batch.draws.size(), s_batch.texUploads.size(), rows);
    if (g255Rtt && !guestDepth && g255VerifyOn())
    {
        // Leave the proven CPU replay authoritative in verification mode. The GPU result is kept
        // only as the comparison oracle; invalidate this FBO snapshot so the next GPU submission
        // uploads the CPU result rather than assuming the restored target is still resident.
        g255StageGpuVerify(vram, vramSize, s_batch.readback, kFbH);
        g_g178FbSnap.erase(fbp);
        return false;
    }
    if (g286Transient && !guestDepth && g286VerifyOn())
    {
        // Same-run exact oracle: keep GPU pixels only as the comparison arm, restore the
        // pre-batch target, then let the established CPU band replay remain authoritative.
        g255StageGpuVerify(vram, vramSize, s_batch.readback, kFbH);
        g_g178FbSnap.erase(fbp);
        return false;
    }
    g289OwnerViewGuard.armed = false;
    g289DirectGuard.armed = false;
    return true;
}

// A whole-flush CPU replay bypasses drawPrimitive's normal post-draw notification.  Publish one
// conservative full-target generation bump per distinct color/Z tuple after replay, so persistent
// G242 depth and G178 textures can never reuse bytes the CPU just changed.
static void g242NoteCpuReplayWrites(uint8_t *vram)
{
    if (!g242GpuDepthOn() || vram == nullptr || g_g144List.empty())
        return;
    std::unordered_set<uint64_t> colorTargets;
    std::unordered_set<uint64_t> depthTargets;
    for (const G144Entry &e : g_g144List)
    {
        const uint32_t fbBlock = GSInternal::framePageBaseToBlock(e.ctx.frame.fbp);
        const uint32_t fbw = e.ctx.frame.fbw;
        const uint32_t fpsm = e.ctx.frame.psm;
        const uint64_t colorKey = static_cast<uint64_t>(fbBlock) |
                                  (static_cast<uint64_t>(fbw) << 16u) |
                                  (static_cast<uint64_t>(fpsm) << 24u);
        if (colorTargets.insert(colorKey).second)
            g178NoteVramWriteRect(vram, fbBlock, fbw, fpsm, 0u, 0u,
                                  std::max(1u, fbw) * 64u, 512u);

        const bool zmsk = ((e.ctx.zbuf >> 32u) & 1u) != 0u;
        if (!zmsk)
        {
            const uint32_t zbpBlock = static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu) << 5u;
            const uint32_t zpsm = static_cast<uint32_t>((e.ctx.zbuf >> 24u) & 0xFu);
            const uint64_t depthKey = static_cast<uint64_t>(zbpBlock) |
                                      (static_cast<uint64_t>(fbw) << 16u) |
                                      (static_cast<uint64_t>(zpsm) << 24u);
            if (depthTargets.insert(depthKey).second)
                g178NoteVramWriteRect(vram, zbpBlock, fbw, zpsm, 0u, 0u,
                                      std::max(1u, fbw) * 64u, 512u);
        }
    }
}

// ===== G161 (MEASURE ONLY, DEFAULT OFF, DC2_G161_STAT=1) — census the G158 plan's eligibility =====
// gate against REAL G144-deferred title geometry, before writing any GPU shader/commit code. The
// signed-off G158 plan (~/.claude/plans/snoopy-roaming-blanket.md) scoped GPU raster to a narrow
// predicate specifically to dodge the two hardest risk classes (CLUT/swizzle decode in a shader,
// and PS2-fixed-point-Z vs GPU-depth-float correctness): textured tri, abe==0 (no blend), fbp
// already known 0x0/0x68 at this call site (G144's deferrable144 already enforced that), PSM in
// {CT32,CT24,CT16,CT16S} (no CLUT4/T8/T4 -- palette lookup deliberately excluded), wrap in
// {REPEAT,CLAMP}, texW*texH <= cap, alpha-test disabled (ctx.test bit0 = ATE). This function does
// NOT touch VRAM, GL, or any new thread -- it only counts, at the exact site G144 already captures
// deferred entries, whether each one *would* pass every clause of that gate. Answers the decisive
// open question before any GPU-thread/shader engineering: is there any eligible geometry at all in
// the title workload (suspected near-zero -- prior phases record the cavern as PSMT8, which this
// gate deliberately excludes)? Same measure-before-build sequencing as G148 (measured the sampler
// before G149 built -- and later refuted -- the CPU decode cache).
namespace
{
std::atomic<uint64_t> g_g161Total{0};
std::atomic<uint64_t> g_g161Eligible{0};
std::atomic<uint64_t> g_g161BadPsm{0};
std::atomic<uint64_t> g_g161BadWrap{0};
std::atomic<uint64_t> g_g161Blend{0};
std::atomic<uint64_t> g_g161Atest{0};
std::atomic<uint64_t> g_g161TooBig{0};

void g161Report()
{
    const uint64_t tot = g_g161Total.load(std::memory_order_relaxed);
    const uint64_t elig = g_g161Eligible.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G161:elig] eligible=%llu total=%llu (%.1f%%) badPsm=%llu badWrap=%llu blend=%llu atest=%llu tooBig=%llu\n",
                 (unsigned long long)elig, (unsigned long long)tot,
                 tot ? 100.0 * static_cast<double>(elig) / static_cast<double>(tot) : 0.0,
                 (unsigned long long)g_g161BadPsm.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161BadWrap.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161Blend.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161Atest.load(std::memory_order_relaxed),
                 (unsigned long long)g_g161TooBig.load(std::memory_order_relaxed));
    std::fflush(stderr);
}
} // namespace

static void g161CensusEligibility(const GSContext &ctx, bool abe)
{
    static const bool s_on = (std::getenv("DC2_G161_STAT") != nullptr);
    if (!s_on)
        return;
    static const size_t s_cap = []() {
        const char *e = std::getenv("DC2_G158_CAP");
        const size_t c = e ? static_cast<size_t>(std::strtoull(e, nullptr, 0)) : 262144u;
        return c ? c : 262144u;
    }();

    const uint64_t tot = g_g161Total.fetch_add(1u, std::memory_order_relaxed) + 1u;
    bool ok = true;
    if (abe) { g_g161Blend.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    if ((ctx.test & 0x1u) != 0u) { g_g161Atest.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    const uint8_t psm = ctx.tex0.psm;
    const bool goodPsm = psm == GS_PSM_CT32 || psm == GS_PSM_CT24 ||
                         psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    if (!goodPsm) { g_g161BadPsm.fetch_add(1u, std::memory_order_relaxed); ok = false; }
    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    if (!((wrapU == 0u || wrapU == 1u) && (wrapV == 0u || wrapV == 1u)))
    {
        g_g161BadWrap.fetch_add(1u, std::memory_order_relaxed);
        ok = false;
    }
    const size_t texels = (static_cast<size_t>(1) << ctx.tex0.tw) * (static_cast<size_t>(1) << ctx.tex0.th);
    if (texels == 0 || texels > s_cap) { g_g161TooBig.fetch_add(1u, std::memory_order_relaxed); ok = false; }

    if (ok)
        g_g161Eligible.fetch_add(1u, std::memory_order_relaxed);
    if ((tot % 8000u) == 0u)
        g161Report();
}

// [G232] gem-draw pixel accounting (DC2_G232_RTTDUMP=1, default-off). drawPrimitive arms
// this for a gem-red 0x34e0 draw; the immediately following drawTriangle consumes it and
// tallies where the pixels die (bbox -> inside -> ztest -> writePixel). Serial-path only.
static std::atomic<int> g_g232TriArm{0};
static std::atomic<uint32_t> g_g232PxBbox{0}, g_g232PxInside{0}, g_g232PxZfail{0};
static std::atomic<uint32_t> g_g232PxWrite{0}, g_g232PxRedWrite{0}, g_g232PxZlog{0};
// [G232] watched pixel = latest gem-draw centroid; drawTriangle logs EVERY fbp=0x139 draw
// touching it (tbp, zi vs zdst, pass) -> full per-frame Z history at the gem's screen pixel.
static std::atomic<int> g_g232GemX{-1}, g_g232GemY{-1};

void GSRasterizer::drawPrimitive(GS *gs)
{
    static const bool s_g141PerfOn = (std::getenv("DC2_PERF") != nullptr);
    static double s_g141Sum = 0.0;
    static uint32_t s_g141Win = 0u;
    G141PerfScope g141Scope(s_g141PerfOn, &s_g141Sum, &s_g141Win, "gs.drawPrimitive", 4000u);
    g141Scope.accumNs = &g_g141GsRasterNs;
    // G219 transition sentinel (inert unless DC2_G219_SKYPX=1).
    {
        static const bool s_g219on = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219on && !t_g144InReplay)
        {
            extern void g219Sentinel(const uint8_t *, uint32_t, const char *);
            static thread_local char s_g219tag[96];
            const auto &g219ctx = gs->activeContext();
            std::snprintf(s_g219tag, sizeof(s_g219tag), "drawprim(prim=%u tme=%u fbp=0x%x tbp=0x%x)",
                          static_cast<uint32_t>(gs->m_prim.type),
                          static_cast<uint32_t>(gs->m_prim.tme),
                          g219ctx.frame.fbp, g219ctx.tex0.tbp0);
            g219Sentinel(gs->m_vram, gs->m_vramSize, s_g219tag);
        }
    }
    const std::chrono::steady_clock::time_point g156FnT0 =
        g_g156Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    const auto &ctx = gs->activeContext();
    // G45: clear the costume model's PRIVATE Z buffer exactly once per costume frame, on the
    // fbp -> 0x139 transition (the first draw of each frame's model RTT pass). The old clear was
    // gated on the full-RTT black-sprite signature, which does NOT fire every frame in the
    // interactive/zoom pose (Red Vest) -> stale private Z from the prior frame rejected current
    // model triangles via GEQUAL -> the model shredded/flickered. The model render is a
    // contiguous block of fbp=0x139 draws (the composite that follows samples 0x2720 but targets
    // a non-0x139 fbp), so a prev!=0x139 && cur==0x139 edge fires once per frame. Costs one 1MB
    // memset/frame and only matters for the costume RTT; harmless if 0x139 is reused otherwise.
    {
        static std::atomic<uint32_t> s_lastDrawFbp{0xFFFFFFFFu};
        static std::atomic<uint32_t> s_costumeFrameDisplay{0xFFFFFFFFu};
        static const bool s_noZClear = (std::getenv("DC2_NO_ZTEST") != nullptr);
        // G49 kill switch: DC2_G49_PERENTRY restores the old (G47) per-0x139-entry Z clear.
        static const bool s_perEntryZClear = (std::getenv("DC2_G49_PERENTRY") != nullptr);
        const uint32_t prevFbp = s_lastDrawFbp.exchange(ctx.frame.fbp, std::memory_order_relaxed);
        bool didClear = false;
        // G49: the costume model is NOT one contiguous 0x139 block — it is drawn in MANY 0x139
        // segments per frame, each bracketed by a draw to the current display buffer (0x68/0x0,
        // which double-buffers per rendered frame). The old G47 clear fired on EVERY 0x139 entry,
        // so it wiped the private Z between the head sub-meshes (skin -> decal -> cap). With Z
        // wiped, GEQUAL reads 0 and the farther cap/hair (tbp 0x35a0, behind the face) no longer
        // fails the depth test -> it draws DARK over the face (the G48 "streaks" residual). Fix:
        // clear the private Z only ONCE per rendered frame, detected by the display buffer we
        // interleave with CHANGING (a double-buffer flip). Z then persists across the frame's
        // 0x139 segments, so the cap/back faces correctly fail GEQUAL behind the skin. The
        // black-fill-sprite clear (below, ~line 870) remains as a once-per-frame safety net for
        // any frame whose display buffer does not flip.
        if (!s_noZClear && ctx.frame.fbp == 0x139u && prevFbp != 0x139u)
        {
            if (s_perEntryZClear)
            {
                costumeZClear();
                didClear = true;
            }
            else
            {
                const uint32_t lastDisp = s_costumeFrameDisplay.load(std::memory_order_relaxed);
                if (prevFbp != lastDisp)
                {
                    costumeZClear();
                    s_costumeFrameDisplay.store(prevFbp, std::memory_order_relaxed);
                    didClear = true;
                }
            }
        }
        // G49 diag: trace the fbp transition sequence + costume Z clears. Env-gated, quiet.
        static const bool s_g49diag = (std::getenv("DC2_G49_DIAG") != nullptr);
        if (s_g49diag && (prevFbp != ctx.frame.fbp))
        {
            static std::atomic<uint32_t> s_g49seq{0};
            const uint32_t sn = s_g49seq.fetch_add(1u, std::memory_order_relaxed);
            if (sn < 400u)
                std::fprintf(stderr, "[G49:fbp] sn=%u 0x%x->0x%x clear=%u\n",
                             sn, prevFbp, ctx.frame.fbp, (unsigned)didClear);
        }
    }
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f31PrimCount{0};
        static std::atomic<uint32_t> s_f31TexturedPrimCount{0};
        const uint32_t n = s_f31PrimCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t texturedN = gs->m_prim.tme
                                       ? (s_f31TexturedPrimCount.fetch_add(1u, std::memory_order_relaxed) + 1u)
                                       : 0u;
        if (n <= 64u || (texturedN != 0u && texturedN <= 128u) || (n % 1024u) == 0u)
        {
            std::fprintf(stderr,
                         "[F31:prim] n=%u type=%u tme=%u iip=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x tex=(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u) rgba0=%02x,%02x,%02x,%02x rgba1=%02x,%02x,%02x,%02x rgba2=%02x,%02x,%02x,%02x uv0=%u,%u uv1=%u,%u uv2=%u,%u\n",
                         n,
                         static_cast<uint32_t>(gs->m_prim.type),
                         static_cast<uint32_t>(gs->m_prim.tme),
                         static_cast<uint32_t>(gs->m_prim.iip),
                         static_cast<uint32_t>(gs->m_prim.abe),
                         static_cast<uint32_t>(gs->m_prim.fst),
                         static_cast<uint32_t>(gs->m_prim.ctxt),
                         ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                         static_cast<uint32_t>(ctx.frame.psm),
                         ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                         static_cast<uint32_t>(ctx.tex0.psm),
                         static_cast<uint32_t>(ctx.tex0.tw),
                         static_cast<uint32_t>(ctx.tex0.th),
                         static_cast<uint32_t>(ctx.tex0.tcc),
                         static_cast<uint32_t>(ctx.tex0.tfx),
                         ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                         static_cast<uint32_t>(ctx.tex0.csm),
                         static_cast<uint32_t>(ctx.tex0.csa),
                         gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                         gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a,
                         gs->m_vtxQueue[2].r, gs->m_vtxQueue[2].g, gs->m_vtxQueue[2].b, gs->m_vtxQueue[2].a,
                         static_cast<uint32_t>(gs->m_vtxQueue[0].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[0].v >> 4),
                         static_cast<uint32_t>(gs->m_vtxQueue[1].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[1].v >> 4),
                         static_cast<uint32_t>(gs->m_vtxQueue[2].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[2].v >> 4));
        }
    }
    if (renderQualityTraceEnabled())
    {
        static std::atomic<uint32_t> s_f44DrawCount{0};
        const uint32_t n = s_f44DrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
        {
            const uint64_t alpha = ctx.alpha;
            const uint64_t clamp = ctx.clamp;
            const uint64_t zbuf  = ctx.zbuf;
            std::fprintf(stderr,
                "[F44:draw] n=%u tme=%u abe=%u pabe=%u tfx=%u tcc=%u fbp=0x%x fbmsk=0x%x "
                "alpha(A=%u,B=%u,C=%u,D=%u,FIX=%u) "
                "clamp(wms=%u,wmt=%u) "
                "zbuf(zbp=0x%x,zmsk=%u) "
                "v0rgba=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<uint32_t>(ctx.tex0.tfx),
                static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.frame.fbp,
                ctx.frame.fbmsk,
                static_cast<uint32_t>(alpha & 0x3u),
                static_cast<uint32_t>((alpha >> 2) & 0x3u),
                static_cast<uint32_t>((alpha >> 4) & 0x3u),
                static_cast<uint32_t>((alpha >> 6) & 0x3u),
                static_cast<uint32_t>((alpha >> 32) & 0xFFu),
                static_cast<uint32_t>(clamp & 0x3u),
                static_cast<uint32_t>((clamp >> 2) & 0x3u),
                static_cast<uint32_t>(zbuf & 0x1FFu),
                static_cast<uint32_t>((zbuf >> 32) & 0x1u),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g,
                gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
        }
    }
    // [F53] F44's global cap is exhausted before the dungeon map packets arrive.
    // Capture the map texture's draw state independently so UI draws cannot hide it.
    if (renderQualityTraceEnabled() &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == 0x3220u &&
        ctx.tex0.psm == 0x13u)
    {
        static std::atomic<uint32_t> s_f53MapDrawCount{0};
        const uint32_t n = s_f53MapDrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 64u)
        {
            const uint64_t alpha = ctx.alpha;
            const uint64_t clamp = ctx.clamp;
            const uint64_t zbuf = ctx.zbuf;
            const uint64_t test = ctx.test;
            std::fprintf(stderr,
                "[F53:draw] n=%u prim=%u ctxt=%u fst=%u tme=%u abe=%u pabe=%u "
                "tfx=%u tcc=%u tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x "
                "fbp=0x%x fbw=%u fpsm=0x%x fbmsk=0x%x "
                "alpha(A=%u,B=%u,C=%u,D=%u,FIX=%u) "
                "clamp(wms=%u,wmt=%u) zbuf(zbp=0x%x,zmsk=%u) "
                "test(ate=%u,atst=%u,aref=%u,afail=%u,date=%u,datm=%u,zte=%u,ztst=%u) "
                "scissor=(%u,%u)-(%u,%u) of=(%d,%d) "
                "v0rgba=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.ctxt),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<uint32_t>(ctx.tex0.tfx),
                static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.tex0.tbp0,
                static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm),
                ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm),
                ctx.frame.fbp,
                static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                ctx.frame.fbmsk,
                static_cast<uint32_t>(alpha & 0x3u),
                static_cast<uint32_t>((alpha >> 2) & 0x3u),
                static_cast<uint32_t>((alpha >> 4) & 0x3u),
                static_cast<uint32_t>((alpha >> 6) & 0x3u),
                static_cast<uint32_t>((alpha >> 32) & 0xFFu),
                static_cast<uint32_t>(clamp & 0x3u),
                static_cast<uint32_t>((clamp >> 2) & 0x3u),
                static_cast<uint32_t>(zbuf & 0x1FFu),
                static_cast<uint32_t>((zbuf >> 32) & 0x1u),
                static_cast<uint32_t>(test & 0x1u),
                static_cast<uint32_t>((test >> 1) & 0x7u),
                static_cast<uint32_t>((test >> 4) & 0xFFu),
                static_cast<uint32_t>((test >> 12) & 0x3u),
                static_cast<uint32_t>((test >> 14) & 0x1u),
                static_cast<uint32_t>((test >> 15) & 0x1u),
                static_cast<uint32_t>((test >> 16) & 0x1u),
                static_cast<uint32_t>((test >> 17) & 0x3u),
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                static_cast<int>(ctx.xyoffset.ofx >> 4),
                static_cast<int>(ctx.xyoffset.ofy >> 4),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g,
                gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
        }
    }
    // [G37:seq] GS-order trace: RTT draws (fbp=0x139 = block 0x2720) and composites sampling
    // block 0x2720 to a display buffer. Shared counter with the upload trace in ps2_gs_gpu.cpp.
    {
        extern std::atomic<uint32_t> g_g37_seq;
        extern bool g_g37_seq_trace;
        extern std::atomic<int> g_g37_armed;
        if (g_g37_seq_trace)
        {
            const bool rttModelDraw = (ctx.frame.fbp == 0x139u && gs->m_prim.tme);
            const bool comp2720 = (gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u && ctx.frame.fbp != 0x139u);
            // Arm on the first model render INTO the RTT; thereafter log ONLY the events that can
            // clobber/consume the RTT (uploads here are logged in ps2_gs_gpu.cpp; composites here),
            // skipping the hundreds of model triangles so the upload/composite ordering is visible.
            if (rttModelDraw && g_g37_armed.load(std::memory_order_relaxed) == 0)
            {
                g_g37_armed.store(1, std::memory_order_relaxed);
                g_g37_seq.store(0u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G37:seq] s=0 MODELRENDER_START fbp=0x139 tbp=0x%x\n", ctx.tex0.tbp0);
                std::fflush(stderr);
            }
            if (comp2720 && g_g37_armed.load(std::memory_order_relaxed) == 1)
            {
                const uint32_t s = g_g37_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (s > 80u) g_g37_armed.store(2, std::memory_order_relaxed);
                if (s <= 80u)
                {
                    std::fprintf(stderr,
                        "[G37:seq] s=%u COMPOSITE destFbp=0x%x prim=%u abe=%u tbp=0x%x tpsm=0x%x tbw=%u v0=(%.0f,%.0f) rgba0=%02x,%02x,%02x,%02x\n",
                        s, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(gs->m_prim.abe), ctx.tex0.tbp0,
                        static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tbw),
                        gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                        gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
                    std::fflush(stderr);
                }
            }
        }
    }
    // [G38] One-shot dump at a costume body-strip draw (fbp=0x139). Decode the BOUND body
    // texture through the runtime's own sampler (GSMem::ReadP8 -> lookupCLUT) so we see what
    // the runtime actually samples for the denim/skin/cloth parts vs the HW .gs. Also dump
    // the raw VRAM blob. DC2_G38_VRAMDUMP=1; trigger texture via DC2_G38_TBP (default 0x34e0).
    {
        static const bool s_g38vram = (std::getenv("DC2_G38_VRAMDUMP") != nullptr);
        static const uint32_t s_g38tbp = []() {
            const char *e = std::getenv("DC2_G38_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0x34e0u;
        }();
        static std::atomic<int> s_g38done{0};
        const auto &tex = ctx.tex0;
        if (s_g38vram && gs->m_vram && gs->m_prim.tme && tex.tbp0 == s_g38tbp &&
            ctx.frame.fbp == 0x139u && s_g38done.exchange(1) == 0)
        {
            FILE *vf = std::fopen("captures/g38_runner_vram.bin", "wb");
            if (vf) { std::fwrite(gs->m_vram, 1, gs->m_vramSize, vf); std::fclose(vf); }
            const int dw = 1 << tex.tw, dh = 1 << tex.th;
            const uint32_t bwv = (tex.tbw > 1u) ? (tex.tbw >> 1) : 1u;
            char path[128];
            std::snprintf(path, sizeof(path), "captures/g38_tex_%x_runtime.ppm", tex.tbp0);
            FILE *fp = std::fopen(path, "wb");
            uint32_t nzColor = 0;
            if (fp)
            {
                std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                for (int yy = 0; yy < dh; ++yy)
                    for (int xx = 0; xx < dw; ++xx)
                    {
                        uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                         static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                        uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        if (c & 0x00FFFFFFu) ++nzColor;
                        uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                           static_cast<uint8_t>((c >> 8) & 0xFF),
                                           static_cast<uint8_t>((c >> 16) & 0xFF) };
                        std::fwrite(rgb, 1, 3, fp);
                    }
                std::fclose(fp);
            }
            // Sample a few CLUT entries directly so we can see if the palette decodes to black.
            uint32_t c0 = lookupCLUT(gs, 1, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c64 = lookupCLUT(gs, 64, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c128 = lookupCLUT(gs, 128, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            uint32_t c200 = lookupCLUT(gs, 200, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            std::fprintf(stderr,
                "[G38:texdump] tbp=0x%x %dx%d tbw=%u bwv=%u psm=0x%x cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "nzColor=%u/%d clut[1]=%08x clut[64]=%08x clut[128]=%08x clut[200]=%08x\n",
                tex.tbp0, dw, dh, (unsigned)tex.tbw, bwv, (unsigned)tex.psm, tex.cbp,
                (unsigned)tex.cpsm, (unsigned)tex.csm, (unsigned)tex.csa, nzColor, dw * dh,
                c0, c64, c128, c200);
            std::fflush(stderr);
        }
    }
    // [G231] Max's chest gem (empty-room repro). The gem's GS draw reaches the runner
    // byte-identically to the HW .gs (fbp=0x139, tbp=0x2720 CT24, TEXA As=128 opaque,
    // ALPHA=0x44, TEST=0x5000b, red vertex modulate) yet produces 0 red pixels. Gate on a
    // RED-vertex textured fbp=0x139 draw (title has no red verts here, so this only fires on
    // the room gem). One-shot: sample the CT24 texel the gem reads through the runtime's own
    // addressing, and dump the raw source pages so we can compare content to the ref freeze.
    {
        static const bool s_g231 = (std::getenv("DC2_G231_GEMDUMP") != nullptr);
        static std::atomic<uint32_t> s_g231n139{0};
        if (s_g231 && gs->m_vram && gs->m_prim.tme && ctx.frame.fbp == 0x139u)
        {
            const uint32_t n = s_g231n139.fetch_add(1, std::memory_order_relaxed) + 1u;
            // Log the distinct texture pages the character samples at fbp=0x139 (first sighting),
            // so we can see whether the gem source pages 0x34e0/0x3460 are ever bound here.
            {
                static std::atomic<uint64_t> s_seen[64];
                static std::atomic<int> s_seenN{0};
                const uint32_t tp = ctx.tex0.tbp0;
                bool known = false;
                const int cnt = s_seenN.load(std::memory_order_relaxed);
                for (int j = 0; j < cnt && j < 64; ++j)
                    if ((s_seen[j].load(std::memory_order_relaxed) & 0x3FFFu) == tp) { known = true; break; }
                if (!known && cnt < 64)
                {
                    int slot = s_seenN.fetch_add(1, std::memory_order_relaxed);
                    if (slot < 64)
                    {
                        s_seen[slot].store(tp, std::memory_order_relaxed);
                        std::fprintf(stderr, "[G231:tbp] fbp=0x139 samples tbp=0x%x psm=0x%x tbw=%u (drawN=%u)\n",
                                     tp, (unsigned)ctx.tex0.psm, (unsigned)ctx.tex0.tbw, n);
                        std::fflush(stderr);
                    }
                }
            }
            // The gem's source texture 0x34e0 (128x128 T8) holds jeans (upper-left) + the red
            // Atlamillia gem in the LOWER-RIGHT corner. Character draws are STQ (fst=0), so read
            // s/t/q and compute the normalized UV. Decode the sampled T8 texel through the CLUT
            // and count how many 0x34e0 draws sample a RED texel -> proves whether the gem
            // sub-mesh actually rasterizes the red gem region.
            if (ctx.tex0.tbp0 == 0x34e0u)
            {
                const uint32_t bwv = (ctx.tex0.tbw > 1u) ? (ctx.tex0.tbw >> 1) : 1u;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                bool sawRed = false; float unRed = 0, vnRed = 0; uint32_t cRed = 0;
                float unMax = 0, vnMax = 0;
                for (int t = 0; t < 3; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    float un = gs->m_vtxQueue[t].s / q;   // normalized U
                    float vn = gs->m_vtxQueue[t].t / q;   // normalized V
                    unMax = std::max(unMax, un); vnMax = std::max(vnMax, vn);
                    int tu = (int)(un * tw); int tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv,
                                          (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                          ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    if (r > 90 && r > g + 40 && r > b + 40) { sawRed = true; unRed = un; vnRed = vn; cRed = c; }
                }
                // Bright GEM red only (r>190,g<80,b<80) — excludes brown skin/leather.
                bool sawGem = false; uint32_t cGem = 0; float ug = 0, vg = 0;
                for (int t = 0; t < 3; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    float un = gs->m_vtxQueue[t].s / q, vn = gs->m_vtxQueue[t].t / q;
                    int tu = (int)(un * tw), tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv, (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    if (r > 190 && g < 80 && b < 80) { sawGem = true; cGem = c; ug = un; vg = vn; }
                }
                static std::atomic<uint32_t> s_34e0n{0}, s_34e0red{0}, s_gemN{0};
                const uint32_t k34 = s_34e0n.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (sawRed) s_34e0red.fetch_add(1, std::memory_order_relaxed);
                if (sawGem)
                {
                    const uint32_t gn = s_gemN.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (gn <= 30u)
                    {
                        // modulate: pixel = texel * vcol / 128 (PS2 modulate, TFX=MODULATE)
                        const uint32_t tr = cGem & 0xFF, tg = (cGem >> 8) & 0xFF, tb = (cGem >> 16) & 0xFF;
                        const uint32_t mr = (tr * gs->m_vtxQueue[0].r) >> 7;
                        const uint32_t mg = (tg * gs->m_vtxQueue[0].g) >> 7;
                        const uint32_t mb = (tb * gs->m_vtxQueue[0].b) >> 7;
                        std::fprintf(stderr,
                            "[G231:GEM] gemDraw#%u (of %u 0x34e0 draws) prim=%u texel=0x%06x UVn=(%.3f,%.3f) "
                            "vcol0=(%u,%u,%u,%u) tfx=%u => modulated=(%u,%u,%u) abe=%u alpha=0x%llx test=0x%llx\n",
                            gn, k34, (unsigned)gs->m_prim.type, cGem, ug, vg,
                            gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                            (unsigned)ctx.tex0.tfx, mr, mg, mb, (unsigned)gs->m_prim.abe,
                            (unsigned long long)(ctx.alpha & 0xFFFFFFFFFFull),
                            (unsigned long long)(ctx.test & 0xFFFFFFFFFFull));
                        std::fflush(stderr);
                    }
                }
                if ((k34 % 5000u) == 0u)
                    std::fprintf(stderr, "[G231:red34e0] CUM 0x34e0draws=%u brownRed=%u GEMbrightRed=%u\n",
                                 k34, s_34e0red.load(std::memory_order_relaxed), s_gemN.load(std::memory_order_relaxed));
            }
        }
    }
    // [G232] Localize the G231 gem pixel-write / composite loss. G231 proved the gem
    // rasterizes bright opaque red at fbp=0x139 yet no red reaches any framebuffer. Split
    // "written then lost" vs "never written" in one run: (a) log each gem-red draw's screen
    // XY / scissor / FBMSK / Z (G231 never logged XY), (b) scan the RTT page (0x272000) for
    // gem red at the NEXT drawPrimitive entry after a gem draw (= post-raster under serial
    // levers), (c) scan + dump the page at composite time (tme && tbp0=0x2720 && fbp!=0x139).
    // DC2_G232_RTTDUMP=1, default-off; run serial (G144/G178/G157/MTGS off) so the CPU path
    // rasters synchronously.
    {
        static const bool s_g232 = (std::getenv("DC2_G232_RTTDUMP") != nullptr);
        // Full 512x416 CT32 RTT extent = fbw 8 pages/row * 13 page-rows * 8KB = 0xD0000 bytes
        // (the first run scanned only 0x80000 = rows 0..255 and missed the character band).
        if (s_g232 && gs->m_vram && 0x272000u + 0xD0000u <= gs->m_vramSize)
        {
            static std::atomic<int> s_g232pending{0};
            static std::atomic<uint32_t> s_g232gemN{0}, s_g232postN{0}, s_g232compN{0}, s_g232dumpN{0};
            const auto scan2720 = [gs](uint32_t &nz, uint32_t &red, uint32_t &firstRedOff, uint32_t &lastNzOff) {
                nz = red = 0; firstRedOff = 0xFFFFFFFFu; lastNzOff = 0;
                const uint32_t *w = reinterpret_cast<const uint32_t *>(gs->m_vram + 0x272000u);
                for (uint32_t i = 0; i < 0xD0000u / 4u; ++i)
                {
                    const uint32_t c = w[i];
                    if (c & 0xFFFFFFu) { ++nz; lastNzOff = i * 4u; }
                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                    if (r > 150u && g < 90u && b < 110u)
                    {
                        ++red;
                        if (firstRedOff == 0xFFFFFFFFu) firstRedOff = i * 4u;
                    }
                }
            };
            // (b) post-raster scan armed by the previous (gem) draw.
            if (s_g232pending.exchange(0, std::memory_order_relaxed) == 1)
            {
                const uint32_t pn = s_g232postN.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (pn <= 30u || (pn % 50u) == 0u)
                {
                    uint32_t nz, red, fro, lnz;
                    scan2720(nz, red, fro, lnz);
                    // pageRow: CT32 fbw=8 -> 8KB page = 64x32 px; row ~ (off/8192)/8*32
                    std::fprintf(stderr,
                        "[G232:postGem] n=%u rtt2720 nz=%u gemRed=%u firstRedOff=0x%x (~row %u) lastNzOff=0x%x (~row %u) "
                        "CUMpx bbox=%u inside=%u zfail=%u write=%u redWrite=%u\n",
                        pn, nz, red, fro, (fro == 0xFFFFFFFFu) ? 0u : (fro / 8192u) / 8u * 32u,
                        lnz, (lnz / 8192u) / 8u * 32u,
                        g_g232PxBbox.load(std::memory_order_relaxed),
                        g_g232PxInside.load(std::memory_order_relaxed),
                        g_g232PxZfail.load(std::memory_order_relaxed),
                        g_g232PxWrite.load(std::memory_order_relaxed),
                        g_g232PxRedWrite.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                }
            }
            // (c) composite-time scan + one-shot dumps (only once a gem draw was seen this run).
            if (gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u && ctx.frame.fbp != 0x139u)
            {
                const uint32_t cn = s_g232compN.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (cn <= 40u || (cn % 100u) == 0u)
                {
                    uint32_t nz, red, fro, lnz;
                    scan2720(nz, red, fro, lnz);
                    std::fprintf(stderr,
                        "[G232:comp] n=%u destFbp=0x%x nz=%u gemRed=%u firstRedOff=0x%x (~row %u) "
                        "lastNzOff=0x%x (~row %u) gemDrawsSoFar=%u\n",
                        cn, ctx.frame.fbp, nz, red, fro,
                        (fro == 0xFFFFFFFFu) ? 0u : (fro / 8192u) / 8u * 32u,
                        lnz, (lnz / 8192u) / 8u * 32u,
                        s_g232gemN.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                    // One-shot after the room's gem draws exist: dump the FULL RTT extent + a
                    // whole-VRAM red census (0x20000 regions) to see where red DID land, if
                    // anywhere.
                    if (s_g232gemN.load(std::memory_order_relaxed) > 100u &&
                        s_g232dumpN.load(std::memory_order_relaxed) < 3u)
                    {
                        const uint32_t dn = s_g232dumpN.fetch_add(1, std::memory_order_relaxed);
                        if (dn < 3u)
                        {
                            char path[128];
                            std::snprintf(path, sizeof(path), "captures/g232_rtt_comp_%u.bin", dn);
                            FILE *fp = std::fopen(path, "wb");
                            if (fp)
                            {
                                std::fwrite(gs->m_vram + 0x272000u, 1, 0xD0000u, fp);
                                std::fclose(fp);
                                std::fprintf(stderr, "[G232:dump] %s full 0xD0000 (comp n=%u)\n", path, cn);
                            }
                            char line[1024]; int lp = 0;
                            lp += std::snprintf(line + lp, sizeof(line) - lp,
                                                "[G232:vramred] regions(0x20000) red:");
                            for (uint32_t rg = 0; rg < gs->m_vramSize / 0x20000u &&
                                                  lp + 24 < (int)sizeof(line); ++rg)
                            {
                                uint32_t rc = 0;
                                const uint32_t *w = reinterpret_cast<const uint32_t *>(
                                    gs->m_vram + rg * 0x20000u);
                                for (uint32_t i = 0; i < 0x20000u / 4u; ++i)
                                {
                                    const uint32_t c = w[i];
                                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu,
                                                   b = (c >> 16) & 0xFFu;
                                    if (r > 150u && g < 90u && b < 110u) ++rc;
                                }
                                if (rc)
                                    lp += std::snprintf(line + lp, sizeof(line) - lp, " 0x%x=%u",
                                                        rg * 0x20000u, rc);
                            }
                            std::fprintf(stderr, "%s\n", line);
                            std::fflush(stderr);
                        }
                    }
                }
            }
            // (a) gem-red draw detect (same texel test as G231's sawGem) + XY/state log + arm (b).
            if (gs->m_prim.tme && ctx.frame.fbp == 0x139u && ctx.tex0.tbp0 == 0x34e0u)
            {
                const uint32_t bwv = (ctx.tex0.tbw > 1u) ? (ctx.tex0.tbw >> 1) : 1u;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                bool sawGem = false;
                for (int t = 0; t < 3 && !sawGem; ++t)
                {
                    const float q = gs->m_vtxQueue[t].q;
                    if (q == 0.0f) continue;
                    const float un = gs->m_vtxQueue[t].s / q, vn = gs->m_vtxQueue[t].t / q;
                    int tu = (int)(un * tw), tv = (int)(vn * th);
                    if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; if (tv >= th) tv = th - 1;
                    const uint8_t ix = (uint8_t)GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, bwv,
                                          (uint32_t)tu, (uint32_t)tv);
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                          ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm) & 0xFFFFFF;
                    const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                    if (r > 190u && g < 80u && b < 80u) sawGem = true;
                }
                if (sawGem)
                {
                    const uint32_t gn = s_g232gemN.fetch_add(1, std::memory_order_relaxed) + 1u;
                    s_g232pending.store(1, std::memory_order_relaxed);
                    g_g232TriArm.store(1, std::memory_order_relaxed);
                    g_g232GemX.store((int)gs->m_vtxQueue[0].x - (int)(ctx.xyoffset.ofx >> 4),
                                     std::memory_order_relaxed);
                    g_g232GemY.store((int)gs->m_vtxQueue[0].y - (int)(ctx.xyoffset.ofy >> 4),
                                     std::memory_order_relaxed);
                    if (gn <= 40u || (gn % 200u) == 0u)
                    {
                        const int ofx = static_cast<int>(ctx.xyoffset.ofx >> 4);
                        const int ofy = static_cast<int>(ctx.xyoffset.ofy >> 4);
                        std::fprintf(stderr,
                            "[G232:gemXY] #%u prim=%u v0=(%.1f,%.1f,z=%.0f) v1=(%.1f,%.1f) v2=(%.1f,%.1f) "
                            "ofxy=(%d,%d) scissor=(%u,%u)-(%u,%u) fbw=%u fpsm=0x%x fbmsk=0x%x "
                            "zbp=0x%x zmsk=%u test=0x%llx\n",
                            gn, (unsigned)gs->m_prim.type,
                            gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y, gs->m_vtxQueue[0].z,
                            gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                            gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y,
                            ofx, ofy,
                            ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                            (unsigned)ctx.frame.fbw, (unsigned)ctx.frame.psm, ctx.frame.fbmsk,
                            (unsigned)(ctx.zbuf & 0x1FFu), (unsigned)((ctx.zbuf >> 32) & 1u),
                            (unsigned long long)(ctx.test & 0xFFFFFFFFFFull));
                        std::fflush(stderr);
                    }
                }
            }
        }
    }
    // [F51.2] Classify the render-to-texture draws that target the manager texture page
    // (fbp=0x139 -> VRAM byte 0x272000, aliasing tbp=0x2720). Is this textured compositing
    // (producing content) or a flat fill that clears the uploaded T8 data to black?
    if (renderQualityTraceEnabled() && ctx.frame.fbp == 0x139u)
    {
        static std::atomic<uint32_t> s_f512DrawCount{0};
        const uint32_t n = s_f512DrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 48u)
        {
            std::fprintf(stderr,
                "[F51.2:rtt139] n=%u prim=%u tme=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x "
                "tex(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x) "
                "scissor=(%u,%u)-(%u,%u) ofxy=(%d,%d) "
                "v0=(%g,%g) v1=(%g,%g) v2=(%g,%g) uv0=(%u,%u) uv1=(%u,%u) "
                "rgba0=%02x,%02x,%02x,%02x rgba1=%02x,%02x,%02x,%02x\n",
                n,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.ctxt),
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                static_cast<uint32_t>(ctx.tex0.tcc), static_cast<uint32_t>(ctx.tex0.tfx),
                ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                static_cast<int>(ctx.xyoffset.ofx >> 4), static_cast<int>(ctx.xyoffset.ofy >> 4),
                gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y,
                static_cast<uint32_t>(gs->m_vtxQueue[0].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[0].v >> 4),
                static_cast<uint32_t>(gs->m_vtxQueue[1].u >> 4), static_cast<uint32_t>(gs->m_vtxQueue[1].v >> 4),
                gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a);
        }
    }
    // [G37/G220] Historical costume-RTT workaround. G37 bulk-zeroed the shared 0x2720 page
    // because the costume clear rect used to collapse with its GetDrawRect bbox. The G37 bbox
    // repair now gives the game's own clear sprite its intended extent, and G220 proved this
    // synthetic clear redundant on the costume route. Worse, MAP-4 zoom legitimately reuses
    // fbp=0x139/tbp=0x2720 as a screen-effect work page and satisfies the old broad signature:
    // this loop then destroys the captured sky before G144 replays the display composite.
    // Keep the old behavior only as an opt-in historical A/B; never enable it by default.
    const float rttClearX0 = std::min(gs->m_vtxQueue[0].x, gs->m_vtxQueue[1].x) -
        static_cast<float>(ctx.xyoffset.ofx >> 4);
    const float rttClearY0 = std::min(gs->m_vtxQueue[0].y, gs->m_vtxQueue[1].y) -
        static_cast<float>(ctx.xyoffset.ofy >> 4);
    const float rttClearX1 = std::max(gs->m_vtxQueue[0].x, gs->m_vtxQueue[1].x) -
        static_cast<float>(ctx.xyoffset.ofx >> 4);
    const float rttClearY1 = std::max(gs->m_vtxQueue[0].y, gs->m_vtxQueue[1].y) -
        static_cast<float>(ctx.xyoffset.ofy >> 4);
    const bool g37FullRttClear =
        rttClearX0 <= 220.0f && rttClearY0 <= 0.0f &&
        rttClearX1 >= 512.0f && rttClearY1 >= 380.0f;
    if (ctx.frame.fbp == 0x139u && gs->m_prim.type == 6u && gs->m_prim.tme == 0u && gs->m_vram &&
        gs->m_vtxQueue[0].r == 0 && gs->m_vtxQueue[0].g == 0 &&
        gs->m_vtxQueue[0].b == 0 && gs->m_vtxQueue[0].a == 0 &&
        g37FullRttClear)
    {
        static const bool forceLegacyClear =
            envFlagEnabled("DC2_G37_FORCE_CLEAR") && !envFlagEnabled("DC2_G37_NOCLEAR");
        if (forceLegacyClear)
        {
            // Composite samples RTT x:[~224,512] y:[0,381]; clear a small margin wider.
            for (uint32_t y = 0u; y < 400u; ++y)
                for (uint32_t x = 208u; x < 512u; ++x)
                    GSMem::WriteCT32(gs->m_vram, 0x2720u, 8u, x, y, 0u);
            // G41: the model RTT re-renders every frame with depth test GEQUAL; its Z buffer
            // (ZBUF.ZBP, page->block) is never cleared by the runner, so stale z from prior
            // frames rejects current model pixels (shredded model). Clear it to 0 (farthest
            // for GEQUAL) here, mirroring the HW per-frame Z clear. Same rect as the color clear.
            static const bool s_noZClear = (std::getenv("DC2_NO_ZTEST") != nullptr);
            if (!s_noZClear)
            {
                const uint32_t czbp = static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5;
                const uint32_t czbw = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 8u;
                const uint32_t czpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
                const bool cz16 = (czpsm == 0x2u || czpsm == 0xAu || czpsm == 0xBu);
                if (g44FaceZTraceEnabled())
                {
                    static std::atomic<uint32_t> s_g44zc{0};
                    const uint32_t zcn = s_g44zc.fetch_add(1u, std::memory_order_relaxed);
                    if (zcn < 200u)
                    {
                        const uint32_t before38092 = cz16
                            ? GSMem::ReadZ16(gs->m_vram, czbp, czbw, 380u, 92u)
                            : GSMem::ReadZ32(gs->m_vram, czbp, czbw, 380u, 92u);
                        const uint32_t before380124 = cz16
                            ? GSMem::ReadZ16(gs->m_vram, czbp, czbw, 380u, 124u)
                            : GSMem::ReadZ32(gs->m_vram, czbp, czbw, 380u, 124u);
                        std::fprintf(stderr,
                            "[G44:zclear] n=%u zbufpage=0x%x czbp=0x%x czpsm=0x%x czbw=%u "
                            "before(380,92)=%u before(380,124)=%u v0=(%.1f,%.1f) v1=(%.1f,%.1f)\n",
                            zcn, static_cast<uint32_t>(ctx.zbuf & 0x1FFu), czbp, czpsm, czbw,
                            before38092, before380124,
                            gs->m_vtxQueue[0].x - static_cast<float>(ctx.xyoffset.ofx >> 4),
                            gs->m_vtxQueue[0].y - static_cast<float>(ctx.xyoffset.ofy >> 4),
                            gs->m_vtxQueue[1].x - static_cast<float>(ctx.xyoffset.ofx >> 4),
                            gs->m_vtxQueue[1].y - static_cast<float>(ctx.xyoffset.ofy >> 4));
                    }
                }
                static std::atomic<uint32_t> s_g41zc{0};
                if (renderQualityTraceEnabled() && s_g41zc.fetch_add(1u) < 4u)
                    std::fprintf(stderr, "[G41:zclear] zbufpage=0x%x czbp(block)=0x%x czpsm=0x%x czbw=%u\n",
                        (unsigned)(ctx.zbuf & 0x1FFu), czbp, czpsm, czbw);
                // G45: the costume model's Z buffer (block 0x1a00 in the runner's FLAT 4MB VRAM)
                // ALIASES the menu's text/sprite VRAM. That aliasing is bidirectional: the menu
                // text sprites redrawn every frame stomp the model's stored Z (-> GEQUAL rejects
                // model triangles next frame -> flicker / disappearing parts, esp. Red Vest in the
                // interactive/zoom pose), and a wide Z clear here stomps the text. Fix: the
                // costume model (fbp=0x139) uses a PRIVATE off-screen Z buffer (costumeZBuf) that
                // aliases nothing. Clear it FULLY every frame here; per-pixel Z test/write below
                // route to it instead of GSMem on m_vram. No VRAM Z writes -> text intact.
                (void)cz16; (void)czbp; (void)czbw;
                costumeZClear();
            }
            static std::atomic<uint32_t> s_g37clear{0};
            const uint32_t cn = s_g37clear.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (renderQualityTraceEnabled() && (cn <= 4u || (cn % 240u) == 0u))
            {
                std::fprintf(stderr, "[G37:rttclear] n=%u zeroed 0x2720 CT32 region x[208,512) y[0,400)\n", cn);
                std::fflush(stderr);
            }
        }
    }
    // [F51.4] Enumerate what the DISPLAYED dungeon buffer (fbp=0x68) draws sample — the real
    // visible-black gate, distinct from the fbp=0x139 manager RTT bucket. Log distinct TEX0 for
    // textured draws + a tally of textured vs untextured(=flat) draws and their vertex color.
    if (renderQualityTraceEnabled() && ctx.frame.fbp == 0x68u)
    {
        static std::atomic<uint32_t> s_f514Tex{0};
        static std::atomic<uint32_t> s_f514Flat{0};
        static std::atomic<uint64_t> s_f514Seen[32] = {};
        if (gs->m_prim.tme)
        {
            const uint64_t key = (static_cast<uint64_t>(ctx.tex0.tbp0) << 40) |
                                 (static_cast<uint64_t>(ctx.tex0.psm) << 32) |
                                 (static_cast<uint64_t>(ctx.tex0.cbp) << 8) |
                                 (static_cast<uint64_t>(ctx.tex0.cpsm));
            bool fresh = true;
            const uint32_t slots = static_cast<uint32_t>(s_f514Tex.load(std::memory_order_relaxed));
            for (uint32_t i = 0; i < slots && i < 32u; ++i)
                if (s_f514Seen[i].load(std::memory_order_relaxed) == key) { fresh = false; break; }
            if (fresh)
            {
                const uint32_t idx = s_f514Tex.fetch_add(1u, std::memory_order_relaxed);
                if (idx < 32u)
                {
                    s_f514Seen[idx].store(key, std::memory_order_relaxed);
                    std::fprintf(stderr,
                        "[F51.4:fbp68tex] new=%u prim=%u abe=%u tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u "
                        "tcc=%u tfx=%u cbp=0x%x cpsm=0x%x v0rgba=%02x,%02x,%02x,%02x\n",
                        idx,
                        static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                        ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                        static_cast<uint32_t>(ctx.tex0.psm),
                        static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                        static_cast<uint32_t>(ctx.tex0.tcc), static_cast<uint32_t>(ctx.tex0.tfx),
                        ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                        gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
                }
            }
        }
        else
        {
            const uint32_t fn = s_f514Flat.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (fn <= 8u || (fn % 65536u) == 0u)
                std::fprintf(stderr,
                    "[F51.4:fbp68flat] n=%u prim=%u abe=%u v0rgba=%02x,%02x,%02x,%02x v1rgba=%02x,%02x,%02x,%02x\n",
                    fn, static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                    gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                    gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b, gs->m_vtxQueue[1].a);
        }
    }
    // [G34] The costume model RTT->display composite (DrawDivSprite4) samples the 0x2720
    // work buffer as CT32 (psm=0). [F51.4] (fbp==0x68 only) and [F509] (paletted only) both
    // miss a CT32 bind, so log every CT32 0x2720 textured draw and the buffer it lands on —
    // tells whether the composite reaches a display buffer (fbp 0x0/0x68), an offscreen one,
    // or never happens.
    if (renderQualityTraceEnabled() && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u)
    {
        // Log EVERY textured draw sampling block 0x2720 (any psm): brick=T8(0x13),
        // the model RTT composite=CT32(0x0). inDiv>0 => emitted from DrawDivSprite4 (the
        // composite). Shows whether the CT32 composite reaches rasterization and its psm.
        const int inDiv = g_g34_in_divsprite.load(std::memory_order_relaxed);
        static std::atomic<uint32_t> s_g34comp{0};
        const uint32_t n = s_g34comp.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 96u || inDiv > 0 || (n % 512u) == 0u)
            std::fprintf(stderr,
                "[G34:composite] n=%u inDiv=%d destFbp=0x%x destFbw=%u destPsm=0x%x prim=%u abe=%u "
                "tbp=0x2720 tpsm=0x%x tbw=%u tw=%u th=%u\n",
                n, inDiv, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th));

        // G37: one-shot dump of the CT32 RTT (block 0x2720, 512x512) that the costume
        // composite samples, so we can SEE whether the model RTT is fully rendered or
        // mostly stale brick. Fires on the first CT32 (psm=0) composite draw.
        static std::atomic<uint32_t> s_g37dump{0};
        const uint32_t g37dn = s_g37dump.fetch_add(1u, std::memory_order_relaxed);
        static const bool s_g37RttDump = (std::getenv("DC2_G37_RTTDUMP") != nullptr);
        if (ctx.tex0.psm == 0u && gs->m_vram &&
            (g268LiveEnvRead() ? (std::getenv("DC2_G37_RTTDUMP") != nullptr) : s_g37RttDump) &&
            (g37dn % 200u) == 150u)  // a late, steady-state composite (overwrites each window)
        {
            FILE *f = std::fopen("D:/ps2r/dc2/captures/g37_rtt_2720.ppm", "wb");
            if (f)
            {
                std::fprintf(f, "P6\n512 512\n255\n");
                for (uint32_t yy = 0u; yy < 512u; ++yy)
                    for (uint32_t xx = 0u; xx < 512u; ++xx)
                    {
                        const uint32_t px = GSMem::ReadCT32(gs->m_vram, 0x2720u, 8u, xx, yy);
                        const unsigned char rgb[3] = {
                            static_cast<unsigned char>(px & 0xFFu),
                            static_cast<unsigned char>((px >> 8u) & 0xFFu),
                            static_cast<unsigned char>((px >> 16u) & 0xFFu)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                std::fclose(f);
                std::fprintf(stderr, "[G37:rttdump] wrote captures/g37_rtt_2720.ppm tbw=%u\n",
                             static_cast<uint32_t>(ctx.tex0.tbw));
                std::fflush(stderr);
            }
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << gs->m_vtxQueue[0].x << "," << gs->m_vtxQueue[0].y << ")"
                      << " uv0=(" << (gs->m_vtxQueue[0].u >> 4) << "," << (gs->m_vtxQueue[0].v >> 4) << ")"
                      << " stq0=(" << gs->m_vtxQueue[0].s << "," << gs->m_vtxQueue[0].t << "," << gs->m_vtxQueue[0].q << ")"
                      << " v1=(" << gs->m_vtxQueue[1].x << "," << gs->m_vtxQueue[1].y << ")"
                      << " uv1=(" << (gs->m_vtxQueue[1].u >> 4) << "," << (gs->m_vtxQueue[1].v >> 4) << ")"
                      << " stq1=(" << gs->m_vtxQueue[1].s << "," << gs->m_vtxQueue[1].t << "," << gs->m_vtxQueue[1].q << ")"
                      << " v2=(" << gs->m_vtxQueue[2].x << "," << gs->m_vtxQueue[2].y << ")"
                      << " uv2=(" << (gs->m_vtxQueue[2].u >> 4) << "," << (gs->m_vtxQueue[2].v >> 4) << ")"
                      << " stq2=(" << gs->m_vtxQueue[2].s << "," << gs->m_vtxQueue[2].t << "," << gs->m_vtxQueue[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(gs->m_vtxQueue[0].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(gs->m_vtxQueue[1].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(gs->m_vtxQueue[2].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((gs->m_prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
    {
        gs->m_hasPreferredDisplaySource = false;
    }

    if (s_g141PerfOn)
    {
        if (gs->m_prim.type == GS_PRIM_SPRITE)
            g_g141SpriteCalls.fetch_add(1u, std::memory_order_relaxed);
        else if (gs->m_prim.type == GS_PRIM_TRIANGLE || gs->m_prim.type == GS_PRIM_TRISTRIP ||
                 gs->m_prim.type == GS_PRIM_TRIFAN)
            g_g141TriCalls.fetch_add(1u, std::memory_order_relaxed);
        static uint64_t s_covTick = 0ull;
        if ((++s_covTick % 8000ull) == 0ull)
        {
            static uint64_t lb = 0, li = 0, ld = 0;
            const uint64_t bb = g_g141BboxPx.load(std::memory_order_relaxed);
            const uint64_t in = g_g141InsidePx.load(std::memory_order_relaxed);
            const uint64_t dr = g_g141DrawnPx.load(std::memory_order_relaxed);
            const uint64_t dbb = bb - lb, din = in - li, ddr = dr - ld;
            lb = bb; li = in; ld = dr;
            std::fprintf(stderr,
                         "[G141:cov] triCalls=%llu sprCalls=%llu | per-8000-prim: bboxPx=%llu insidePx=%llu drawnPx=%llu coverage=%.0f%% overdraw=%.2fx\n",
                         static_cast<unsigned long long>(g_g141TriCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_g141SpriteCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(dbb), static_cast<unsigned long long>(din),
                         static_cast<unsigned long long>(ddr),
                         dbb ? 100.0 * static_cast<double>(din) / static_cast<double>(dbb) : 0.0,
                         din ? static_cast<double>(ddr) / static_cast<double>(din) : 0.0);
        }
    }

    // ===== G144 frame-level tile-binning capture/flush (G168: DEFAULT 8 lanes) =====
    // Runs AFTER the perf counters (so deferred tris still count) and BEFORE the switch (so the
    // flush's parallel raster time is still inside this drawPrimitive's G141PerfScope → measurable).
    if (g_g144Threads < 0)
    {
        const char *e144 = std::getenv("DC2_G144_TILEBIN");
        int n144 = (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
                       ? 0
                       : (e144 ? std::atoi(e144) : 8);
        g_g144Threads = (n144 < 0) ? 0 : (n144 > 16 ? 16 : n144);
    }
    if (g_g144Threads > 1 && !t_g144InReplay)
    {
        g_g144LastGs = gs;
        g_g144LastVram = gs->m_vram;
        // Assign the flush closure ONCE (keeps this member's friend access). Parallel band replay:
        // each lane owns a disjoint scanline band and replays every overlapping entry IN SUBMISSION
        // ORDER, clipped to its band via the thread_local clamp → disjoint pixel writes,
        // order-preserved blend/Z. Bit-exact. Uses g_g144LastGs so g144FlushPending() (frame end)
        // can invoke it too.
        if (!g_g144FlushFn)
        {
            GSRasterizer *self = this;
            g_g144FlushFn = [self]() {
                const bool g290P = g290ProbeOn();
                const auto g290T0 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g_g144LastGs)
                    g219Sentinel(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize, "flush-preg162");
                // G166: resolve any batched T8 GPU-decode requests FIRST, unconditionally -- every
                // e.decoded the replay below reads must already be pixel-filled (see
                // g162ResolvePendingT8Batch's comment for why this ordering is safe by construction).
                g162ResolvePendingT8Batch();
                const auto g290T1 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290ResolveNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T1 - g290T0)
                            .count());
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                g219Sentinel(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize, "flush-start");
                // G178: whole-flush GPU attempt (default OFF, DC2_G178_GPU=1). On success the
                // batch is already rendered AND read back into VRAM — skip the CPU replay
                // entirely. On ANY unsupported entry it returns false having changed nothing,
                // and the proven CPU band replay below runs for the whole flush.
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
                    if (g290P)
                    {
                        ++g_g290GpuOk;
                        g_g290GpuOkNs += static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g290T1)
                                .count());
                    }
                    g_g144List.clear();
                    return;
                }
                const auto g290T2 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                {
                    ++g_g290GpuFail;
                    g_g290GpuFailNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T2 - g290T1)
                            .count());
                    g_g290CpuEntries += g_g144List.size();
                    static uint64_t s_g290FailLogs = 0;
                    if (s_g290FailLogs < 48u || (g_g290GpuFail % 64u) == 0u)
                    {
                        ++s_g290FailLogs;
                        const G144Entry &e0 = g_g144List[0];
                        std::fprintf(stderr,
                                     "[G290:gpufail] n=%llu entries=%zu fbp=%03x fbw=%u fpsm=%x "
                                     "zbp=%03x rej=%s site=%d e0(prim=%u tme=%u tpsm=%x tbp=%x "
                                     "abe=%u sc=%u,%u..%u,%u)\n",
                                     (unsigned long long)g_g290GpuFail, g_g144List.size(),
                                     e0.ctx.frame.fbp, (unsigned)e0.ctx.frame.fbw,
                                     e0.ctx.frame.psm, (unsigned)(e0.ctx.zbuf & 0x1FFu),
                                     kG266FrName[g_g266FlushRej & 0xF], g_g266ClassifySite,
                                     (unsigned)e0.prim.type, e0.prim.tme ? 1u : 0u,
                                     e0.prim.tme ? e0.ctx.tex0.psm : 0u,
                                     e0.prim.tme ? e0.ctx.tex0.tbp0 : 0u,
                                     e0.prim.abe ? 1u : 0u, e0.ctx.scissor.x0,
                                     e0.ctx.scissor.y0, e0.ctx.scissor.x1, e0.ctx.scissor.y1);
                    }
                }
                // G272: CPU fallback consumes/writes guest VRAM. Any earlier display wave must be
                // published first; the current failed batch has not been marked resident.
                (void)g278FlushPendingDepth(3); // CPU replay needs authoritative guest Z
                (void)g289MaterializeOwner(3);
                g272MaterializeAll(3);
                g277CensusHardEdge(3);
                g276FlushPendingDisplay(3); // G276: publish deferred display readbacks first
                // The proven CPU replay and its decoded textures consume guest VRAM directly.
                // Publish any RTT rows left GPU-resident by a G261 wave (only where this list's
                // target/Z/texture/CLUT pages actually touch them), then any GPU-owned depth.
                g261PrepareCpuReplay();
                g242PrepareVramReadAll(g_g144LastGs->m_vram);
                const auto g290T3 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290PubNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T3 - g290T2)
                            .count());
                // Pre-run the title-Z fbp-transition clear ONCE, single-threaded (replay skips it).
                // G203: universal-Z uses no artificial clears — the game's own clear-sprite populates
                // VRAM Z during the replay below (the g202 clear here is what stomped block 0x1a00 /
                // the menu font staging into inventory/loading; skip it entirely under universal-Z).
                if (!g203UniversalZEnabled())
                {
                    if (g106TitleZPrivateEnabled())
                        g106TitleZClearIfNeeded(g_g144BlockFbp);
                    const GSContext &c = g_g144List[0].ctx;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = g_g144List[0].townDepthScope;
                    g202TownZClearIfNeeded(g_g144LastGs->m_vram, c.frame.fbp, c.zbuf, c.frame.fbw);
                    t_g144TownDepthScopeOverride = false;
                }
                int y0 = 0x7fffffff, y1 = -0x7fffffff;
                for (const auto &e : g_g144List)
                {
                    if (e.minY < y0) y0 = e.minY;
                    if (e.maxY > y1) y1 = e.maxY;
                }
                if (y1 >= y0)
                {
                    GS *rgs = g_g144LastGs;
                    GSRowPool::instance().run(y0, y1, g_g144Threads, [self, rgs](int a, int b) {
                        static thread_local GS s_rg;
                        s_rg.m_vram = rgs->m_vram;
                        s_rg.m_vramSize = rgs->m_vramSize;
                        s_rg.m_privRegs = rgs->m_privRegs;
                        t_g144InReplay = true;
                        t_g144Banded = true;
                        t_g144BandY0 = a;
                        t_g144BandY1 = b;
                        for (const auto &e : g_g144List)
                        {
                            if (e.maxY < a || e.minY > b)
                                continue; // bbox does not intersect this band
                            s_rg.m_ctx[0] = e.ctx;
                            s_rg.m_prim = e.prim;
                            s_rg.m_prim.ctxt = false; // active context forced to m_ctx[0] = snapshot
                            s_rg.m_texa = e.texa;
                            s_rg.m_texclut = e.texclut;
                            s_rg.m_pabe = e.pabe;
                            t_g144TitleRockScopeOverride = true;
                            t_g144TitleRockScope = e.titleRockScope;
                            t_g144TownDepthScopeOverride = true;
                            t_g144TownDepthScope = e.townDepthScope;
                            s_rg.m_vtxQueue[0] = e.v[0];
                            s_rg.m_vtxQueue[1] = e.v[1];
                            s_rg.m_vtxQueue[2] = e.v[2];
                            t_g258EntryIndex = static_cast<size_t>(&e - g_g144List.data());
                            if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                            else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                            // G172: widened G144 to also defer SPRITE entries (see the capture site);
                            // dispatch by the captured primitive type instead of always drawTriangle.
                            if (g270LineType(static_cast<uint32_t>(e.prim.type)))
                                self->drawLine(&s_rg);
                            else if (e.prim.type == GS_PRIM_SPRITE)
                                self->drawSprite(&s_rg);
                            else
                                self->drawTriangle(&s_rg);
                        }
                        t_g149Px = nullptr; t_g149Valid = nullptr;
                        t_g258EntryIndex = static_cast<size_t>(-1);
                        t_g144TitleRockScopeOverride = false;
                        t_g144TownDepthScopeOverride = false;
                        t_g144InReplay = false;
                        t_g144Banded = false;
                    });
                }
                const auto g290T4 = g290P ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                if (g290P)
                    g_g290ReplayNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(g290T4 - g290T3)
                            .count());
                g242NoteCpuReplayWrites(g_g144LastGs->m_vram);
                // G261: the wave arm disables the G260 pre-execution RTT generation bump (it
                // would poison GPU-resident targets); publish the replay's RTT writes here,
                // after the bytes actually changed.
                g261NoteCpuRttWrites(g_g144LastGs->m_vram);
                g286NoteCpuTransientWrites(g_g144LastGs->m_vram);
                g255CompareCpuVerify(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize);
                if (g290P)
                    g_g290NoteNs += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - g290T4)
                            .count());
                g_g144List.clear();
            };
            // Sequential drain (frame end): no pool, no bands — replay every entry full-range on the
            // calling (guest) thread, in order. Correctness-safe fallback for the trailing tail.
            g_g144FlushSeqFn = [self]() {
                // G166: same unconditional resolve as g_g144FlushFn above (either closure may run
                // first, e.g. under DC2_G144_SEQ, so both must resolve before reading e.decoded).
                g162ResolvePendingT8Batch();
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                // G178: same whole-flush GPU attempt as the parallel closure above.
                if (g178TryFlushGpu(self, g_g144LastGs, g_g144LastGs->m_vram, g_g144LastGs->m_vramSize))
                {
                    g_g144List.clear();
                    return;
                }
                (void)g278FlushPendingDepth(3); // CPU replay needs authoritative guest Z
                (void)g289MaterializeOwner(3);
                g272MaterializeAll(3); // CPU replay boundary (see parallel closure)
                g277CensusHardEdge(3);
                g276FlushPendingDisplay(3); // G276: publish deferred display readbacks first
                g261PrepareCpuReplay(); // G261: materialize touched GPU-resident RTT rows first
                g242PrepareVramReadAll(g_g144LastGs->m_vram);
                // G203: skip all artificial Z pre-clears under universal-Z (see the parallel closure).
                if (!g203UniversalZEnabled())
                {
                    if (g106TitleZPrivateEnabled())
                        g106TitleZClearIfNeeded(g_g144BlockFbp);
                    const GSContext &c = g_g144List[0].ctx;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = g_g144List[0].townDepthScope;
                    g202TownZClearIfNeeded(g_g144LastGs->m_vram, c.frame.fbp, c.zbuf, c.frame.fbw);
                    t_g144TownDepthScopeOverride = false;
                }
                static thread_local GS s_seq;
                GS *rgs = g_g144LastGs;
                s_seq.m_vram = rgs->m_vram;
                s_seq.m_vramSize = rgs->m_vramSize;
                s_seq.m_privRegs = rgs->m_privRegs;
                t_g144InReplay = true;
                t_g144Banded = false; // full row range
                for (const auto &e : g_g144List)
                {
                    s_seq.m_ctx[0] = e.ctx;
                    s_seq.m_prim = e.prim;
                    s_seq.m_prim.ctxt = false;
                    s_seq.m_texa = e.texa;
                    s_seq.m_texclut = e.texclut;
                    s_seq.m_pabe = e.pabe;
                    t_g144TitleRockScopeOverride = true;
                    t_g144TitleRockScope = e.titleRockScope;
                    t_g144TownDepthScopeOverride = true;
                    t_g144TownDepthScope = e.townDepthScope;
                    s_seq.m_vtxQueue[0] = e.v[0];
                    s_seq.m_vtxQueue[1] = e.v[1];
                    s_seq.m_vtxQueue[2] = e.v[2];
                    t_g258EntryIndex = static_cast<size_t>(&e - g_g144List.data());
                    if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                    else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                    // G172: see the parallel-replay closure above — dispatch by captured prim type.
                    if (g270LineType(static_cast<uint32_t>(e.prim.type)))
                        self->drawLine(&s_seq);
                    else if (e.prim.type == GS_PRIM_SPRITE)
                        self->drawSprite(&s_seq);
                    else
                        self->drawTriangle(&s_seq);
                }
                t_g149Px = nullptr; t_g149Valid = nullptr;
                t_g258EntryIndex = static_cast<size_t>(-1);
                t_g144TitleRockScopeOverride = false;
                t_g144TownDepthScopeOverride = false;
                t_g144InReplay = false;
                g242NoteCpuReplayWrites(g_g144LastGs->m_vram);
                // G261: the wave arm disables the G260 pre-execution RTT generation bump (it
                // would poison GPU-resident targets); publish the replay's RTT writes here,
                // after the bytes actually changed.
                g261NoteCpuRttWrites(g_g144LastGs->m_vram);
                g286NoteCpuTransientWrites(g_g144LastGs->m_vram);
                g255CompareCpuVerify(g_g144LastGs->m_vram, g_g144LastGs->m_vramSize);
                g_g144List.clear();
            };
        }

        const uint32_t pt144 = static_cast<uint32_t>(gs->m_prim.type);
        const bool isTri144 = (pt144 == GS_PRIM_TRIANGLE || pt144 == GS_PRIM_TRISTRIP || pt144 == GS_PRIM_TRIFAN);
        const bool isLine144 = g270LineType(pt144);
        // G172: G171 found that EVERY sprite draw forced a full mid-frame flush of the pending
        // triangle list (the "non-deferrable primitive" drain below) before rasterizing inline —
        // with ~100+ sprites/frame (2D title UI: logo/press-start/menu overlays, all alpha-blended
        // to fbp=0x0) this fragmented one large deferred batch into 100+ tiny ones, paying the
        // parallel-dispatch overhead that many times over (measured ~76-79ms/frame, ~99% of the
        // whole packed-register dispatch cost). Widen the SAME proven defer/band-replay path to
        // GS_PRIM_SPRITE at the same display-framebuffer gate, so sprites replay in the same
        // band-parallel pass as triangles instead of each forcing their own flush.
        // G144 tile-bin is itself default-ON since G168. G172 sprite-widening (root-caused +
        // fixed at G173, dungeon-3D soak clean at G187) is promoted to DEFAULT-ON here too —
        // kill via DC2_G172_NO_SPRITE_DEFER=1 or DC2_G172_SPRITE_DEFER=0.
        // G178: GPU mode needs sprites in the deferred list too (an inline sprite would blend
        // against a VRAM framebuffer the GPU owns mid-frame) — force the proven G172/G173 sprite
        // deferral on whenever the GPU lever is on (redundant now that it's default-on, kept for
        // the kill-switch-off + GPU-on combination).
        static const bool s_g172SpriteDefer =
            (!envFlagEnabled("DC2_G172_NO_SPRITE_DEFER") && !envFlagDisabled("DC2_G172_SPRITE_DEFER")) ||
            g178GpuOn();
        const bool isSprite144 = s_g172SpriteDefer && (pt144 == GS_PRIM_SPRITE);
        const bool diagOff144 = !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() && !f50_12_trace_enabled();
        // Deferrable by default = a triangle/sprite to a DISPLAY framebuffer (0x0/0x68), never
        // RTT or any diagnostic build. G252 below adds a narrow default-off RTT experiment.
        // Triangles still require textured (tme) as before; sprites defer whether textured or a
        // solid fill (both are equally safe to replay — writePixel/sampleTexture are unchanged).
        // G178: untextured triangles are as replay-safe as untextured sprites (drawTriangle is
        // unchanged) — widen the tme gate under the GPU lever so display-fbp shade-only tris
        // don't rasterize inline against a framebuffer the GPU owns mid-frame.
        // G252 (DEFAULT ON since G259): CPU band replay for the five measured RTT sprite targets.
        // G259 revalidated a material, repeatable MAP-0 win on current source (control 1.957 fps ->
        // G252 2.297 fps steady, 3 reps/arm, +17.4% FPS / -14.7% frame time, arms non-overlapping
        // and far outside run-to-run variance), with the full G253 route matrix (~199M texels,
        // bad=0), the exact golden title frame (211646), and an 896-frame dense title temporal gate
        // (all 211642..211650, no dropout) clean. Promoted default-on. Kill switch
        // DC2_G252_NO_RTT_DEFER=1 restores inline RTT rasterization. Target switches and every
        // upload/local-copy drain this list, preserving render-target write -> later texture-read
        // order. GPU RTT (DC2_G252_GPU_RTT / G255/G256) remains separately disabled.
        static const bool s_g252RttDefer =
            !envFlagEnabled("DC2_G252_NO_RTT_DEFER") || g255GpuRttOn();
        const bool g252RttTarget =
            s_g252RttDefer && isSprite144 && g248TargetIndex(ctx.frame.fbp) >= 0;
        // G286 admits only the measured opaque textured full-screen sprite class. Other 0x13b
        // shapes remain on the proven CPU path instead of silently widening shader semantics.
        const bool g286Transient =
            g286TransientTarget(ctx.frame.fbp) && diagOff144 && isSprite144 &&
            gs->m_prim.tme && !gs->m_prim.abe;
        // G260 (opt-in): under the native-renderer command graph, RTT-family TRIANGLES (both
        // textured and untextured — the census shows both) become recordable too, closing the
        // G247 structural exclusion that rasterized them inline scalar on the GS thread. They
        // join the same per-target batches as the G252 RTT sprites and execute through the same
        // banded replay, in submission order. Without DC2_G260_NR this bool is constant false
        // and the shipped eligibility is bit-identical.
        static const bool s_g260NoRttTri = envFlagEnabled("DC2_G260_NO_RTTTRI"); // bisect lever
        const bool g260RttTri = g260NrOn() && !s_g260NoRttTri && isTri144 && diagOff144 &&
                                g248TargetIndex(ctx.frame.fbp) >= 0;
        const bool g270LineWave = g270LineWaveOn() && isLine144 && !gs->m_prim.tme &&
                                  diagOff144 &&
                                  (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u);
        const bool deferrable144 = ((isTri144 || isSprite144) && diagOff144 &&
                                    (isTri144 ? (gs->m_prim.tme || g178GpuOn()) : true) &&
                                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u ||
                                      g252RttTarget)) ||
                                   g260RttTri || g270LineWave || g286Transient;
        // G156: count deferred vs inline tris + the non-deferrable reason (see g156Report). Report
        // fires from HERE (every tri) so a line prints even if 100% defer (inline=0) or 100% inline.
        if (g_g156Stat && isTri144)
        {
            if (deferrable144)
                g_g156DeferredTri.fetch_add(1u, std::memory_order_relaxed);
            else
            {
                g_g156InlineTri.fetch_add(1u, std::memory_order_relaxed);
                if (!gs->m_prim.tme)
                    g_g156NoTme.fetch_add(1u, std::memory_order_relaxed);
                else if (ctx.frame.fbp != 0x0u && ctx.frame.fbp != 0x68u)
                    g_g156BadFbp.fetch_add(1u, std::memory_order_relaxed);
                else
                    g_g156Diag.fetch_add(1u, std::memory_order_relaxed);
            }
            static std::atomic<uint64_t> s_g156Tick{0};
            if ((s_g156Tick.fetch_add(1u, std::memory_order_relaxed) % 8000u) == 0u)
                g156Report();
        }
        // DC2_G144_SEQ: route mid-frame flushes through the SEQUENTIAL (no-pool) path too — isolates
        // deferral/ordering correctness from the parallel band logic.
        static const bool s_g144Seq = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &g144DoFlush = s_g144Seq ? g_g144FlushSeqFn : g_g144FlushFn;
        auto g144TimedMidFlush = [&]() {
            G146PerfScope g146FlushScope(g_g146G144FlushMidNs, g_g146G144FlushMidCount);
            g144DoFlush();
        };
        if (deferrable144)
        {
            if (g286Transient)
                g_g286Captured.fetch_add(1u, std::memory_order_relaxed);
            const std::chrono::steady_clock::time_point g156CapT0 =
                g_g156Stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (g_g156Stat)
                g_g156PrologueNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(g156CapT0 - g156FnT0)
                        .count(),
                    std::memory_order_relaxed);
            // G266: a same-fbp FBW switch is a batch boundary too (see g266FbwSplitOn). The
            // legacy default path (split off) keeps the historical fbp-only condition — its CPU
            // replay handles mixed widths and the default must stay bit-identical.
            const bool g266FbwSwitch = g266FbwSplitOn() && !g_g144List.empty() &&
                                       ctx.frame.fbw != g_g144List.front().ctx.frame.fbw;
            if (!g_g144List.empty() && (ctx.frame.fbp != g_g144BlockFbp || g266FbwSwitch))
            {
                if (g254DependencyStatEnabled())
                {
                    G144Entry candidate{};
                    candidate.ctx = ctx;
                    candidate.prim = gs->m_prim;
                    candidate.texclut = gs->m_texclut;
                    const uint32_t depMask = g254DependencyMask(candidate);
                    g254NoteSwitch(depMask, g_g144BlockFbp, ctx.frame.fbp, g_g144List.size());
                }
                g253NoteMidBarrier(true, g_g144List.front().ctx.frame.fbp,
                                   ctx.frame.fbp, g_g144List.size(), pt144,
                                   gs->m_prim.tme, gs->m_prim.abe);
                // G260: a target switch is a batch boundary, not an execution point. Close the
                // open batch into the frame graph (O(1) move) and keep recording; the graph
                // executes at the next real dependency edge or the frame boundary, batch by
                // batch in submission order. Legacy path (NR off): drain immediately as before.
                if (g260NrOn())
                {
                    g260CloseOpenBatch();
                    // DC2_G260_EAGER=1 (bisect lever): execute at every batch close — restores
                    // the immediate-drain cadence while keeping NR plumbing, isolating "delayed
                    // execution across upload edges" from "batch/replay plumbing".
                    static const bool s_g260Eager = envFlagEnabled("DC2_G260_EAGER");
                    if (s_g260Eager)
                        g260ExecuteAll(kG260Legacy, true);
                    else if (g_g260Graph.size() > 96u)
                        g260ExecuteAll(kG260Runaway, true); // fail-safe cap, keeps memory bounded
                }
                else
                {
                    g144TimedMidFlush(); // fbp changed → different target; drain before switching blocks
                }
            }
            g_g144BlockFbp = ctx.frame.fbp;
            // Screen bbox rows computed EXACTLY as drawTriangle/drawSprite (same ofy + scissor
            // clamp) so the band prefilter is tight and never narrower than the real coverage.
            const GSVertex &cv0 = gs->m_vtxQueue[0];
            const GSVertex &cv1 = gs->m_vtxQueue[1];
            const int cofy = ctx.xyoffset.ofy >> 4;
            int crawMinY, crawMaxY;
            G144Entry e;
            if (isSprite144)
            {
                // Sprite bbox: 2-vertex unclipped rect, min/max SWAPPED (drawSprite's own formula),
                // not the triangle 3-way min/max.
                const float sfy0 = cv0.y - static_cast<float>(cofy);
                const float sfy1 = cv1.y - static_cast<float>(cofy);
                crawMinY = static_cast<int>(std::floor(std::min(sfy0, sfy1)));
                crawMaxY = static_cast<int>(std::ceil(std::max(sfy0, sfy1)));
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = GSVertex{};
                // G172: hoist drawSprite()'s "looksLikeDisplayCopy" detection to capture time and
                // apply it to the LIVE gs immediately (same instant it would fire inline) — this
                // mutates gs->m_hasPreferredDisplaySource/m_preferredDisplaySourceFrame/
                // m_preferredDisplayDestFbp, read later by latchHostPresentationFrame(). Duplicated
                // (not shared) so the already-proven inline drawSprite() path is never touched;
                // the replay's own drawSprite() call redundantly recomputes it on the throwaway
                // scratch GS, which is harmless (never read back).
                const int sox = ctx.xyoffset.ofx >> 4;
                int sx0 = static_cast<int>(cv0.x) - sox;
                int sx1 = static_cast<int>(cv1.x) - sox;
                int sy0 = static_cast<int>(cv0.y) - cofy;
                int sy1 = static_cast<int>(cv1.y) - cofy;
                if (sx0 > sx1) std::swap(sx0, sx1);
                if (sy0 > sy1) std::swap(sy0, sy1);
                const uint64_t alphaReg = ctx.alpha;
                const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
                const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
                const bool looksLikeDisplayCopy144 =
                    gs->m_prim.tme && gs->m_prim.abe && gs->m_prim.fst && gs->m_prim.ctxt &&
                    ctx.frame.fbp != ctx.tex0.tbp0 &&
                    alphaMode == 0x64u && (alphaFix == 0x60u || alphaFix == 0x80u) &&
                    sx0 <= 0 && sy0 <= 0 && sx1 >= 639 && sy1 >= 447;
                if (looksLikeDisplayCopy144)
                {
                    gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
                    gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
                    gs->m_hasPreferredDisplaySource = true;
                }
            }
            else if (isLine144)
            {
                // drawLine truncates the already pixel-unit vertex coordinates before applying
                // XYOFFSET. Preserve that exact row range for band prefiltering and replay.
                const int ly0 = static_cast<int>(cv0.y) - cofy;
                const int ly1 = static_cast<int>(cv1.y) - cofy;
                crawMinY = std::min(ly0, ly1);
                crawMaxY = std::max(ly0, ly1);
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = GSVertex{};
                g270NoteCapture(cv0, cv1);
            }
            else
            {
                const GSVertex &cv2 = gs->m_vtxQueue[2];
                const float cfy0 = cv0.y - static_cast<float>(cofy);
                const float cfy1 = cv1.y - static_cast<float>(cofy);
                const float cfy2 = cv2.y - static_cast<float>(cofy);
                crawMinY = static_cast<int>(std::floor(std::min({cfy0, cfy1, cfy2})));
                crawMaxY = static_cast<int>(std::ceil(std::max({cfy0, cfy1, cfy2})));
                e.v[0] = cv0;
                e.v[1] = cv1;
                e.v[2] = cv2;
            }
            e.ctx = ctx;
            e.prim = gs->m_prim;
            e.texa = gs->m_texa;
            e.texclut = gs->m_texclut;
            e.pabe = gs->m_pabe;
            e.titleRockScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
            e.townDepthScope = g_dc2TownDepthScope.load(std::memory_order_relaxed);
            e.minY = clampInt(crawMinY, ctx.scissor.y0, ctx.scissor.y1);
            e.maxY = clampInt(crawMaxY, ctx.scissor.y0, ctx.scissor.y1);
            // G149: decode this tri's texture ONCE now (guest thread), so all band lanes read it
            // lock-free at replay. Null unless texcache-enabled + eligible. Sprites never use the
            // G149 texcache hook (drawSprite() samples VRAM directly, same as its inline path).
            e.decoded = isTri144 ? g149BuildDecoded(this, gs, ctx, gs->m_texa, gs->m_vram) : G149Buf{};
            if (isTri144)
                g161CensusEligibility(ctx, gs->m_prim.abe);
            // G219 (default-off, DC2_G219_SKYPX=1): fingerprint the 0x2720 work-page range at
            // CAPTURE time for the display composite that samples it — paired with the identical
            // fingerprint at raster time in drawTriangle, this discriminates "VRAM clobbered
            // between capture and flush" from "sampler reads a different address in replay".
            {
                static const bool s_g219f = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219f && isTri144 && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u &&
                    gs->m_vram && 0x272000u + 0x8000u <= gs->m_vramSize)
                {
                    static std::atomic<uint32_t> s_g219fn{0};
                    const uint32_t n = s_g219fn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u)
                    {
                        uint64_t sum = 0;
                        const uint64_t *p = reinterpret_cast<const uint64_t *>(gs->m_vram + 0x272000u);
                        for (uint32_t i = 0; i < 0x1000u; ++i)
                            sum += p[i];
                        std::fprintf(stderr, "[G219:cap] n=%u fbp=0x%x sum=%016llx\n",
                                     n, ctx.frame.fbp, (unsigned long long)sum);
                        extern uint8_t g_g219Snap[0x8000];
                        std::memcpy(g_g219Snap, gs->m_vram + 0x272000u, 0x8000u);
                    }
                }
            }
            if (g260NrOn())
                g260NoteAppend(e); // accumulate this entry's R/W block ranges (open batch)
            g_g144List.push_back(e);
            if (g_g156Stat)
                g_g156CaptureNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g156CapT0)
                        .count(),
                    std::memory_order_relaxed);
            return; // deferred — rasterized later at flush
        }
        // Non-deferrable primitive (line/point/untextured-off-target/RTT sprite/etc — anything that
        // didn't satisfy deferrable144 above, e.g. a sprite/tri to a non-display fbp like 0x139):
        // drain the pending list FIRST so global submission order (3D under the 2D menu) is
        // preserved, then draw this prim normally.
        // G219 (default-off, DC2_G219_SKYPX=1): log the non-deferrable drains around the 0x139
        // work-page cycle so the copy/composite/clear interleave is visible in the trace.
        {
            static const bool s_g219d = (std::getenv("DC2_G219_SKYPX") != nullptr);
            if (s_g219d && ctx.frame.fbp == 0x139u)
            {
                static std::atomic<uint32_t> s_g219dn{0};
                const uint32_t n = s_g219dn.fetch_add(1u, std::memory_order_relaxed);
                if (n < 2000u)
                    std::fprintf(stderr,
                        "[G219:nondef] n=%u prim=%u tme=%u fbp=0x%x tbp=0x%x list=%zu\n",
                        n, static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(gs->m_prim.tme), ctx.frame.fbp, ctx.tex0.tbp0,
                        g_g144List.size());
            }
        }
        if (g260NrOn())
        {
            // G260: an inline (non-recordable) primitive is about to read/write VRAM directly.
            // Execute the whole pending graph first so it observes the same VRAM evolution
            // order as the immediate-drain baseline. Covers both the open batch and any closed
            // batches (the legacy branch below only ever has the open list).
            if (g260HasPending())
            {
                if (!g_g144List.empty())
                    g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                                       ctx.frame.fbp, g_g144List.size(), pt144,
                                       gs->m_prim.tme, gs->m_prim.abe);
                g260ExecuteAll(kG260NonDef, true);
            }
        }
        else if (!g_g144List.empty())
        {
            g253NoteMidBarrier(false, g_g144List.front().ctx.frame.fbp,
                               ctx.frame.fbp, g_g144List.size(), pt144,
                               gs->m_prim.tme, gs->m_prim.abe);
            g144TimedMidFlush();
        }
        // This primitive will execute on the CPU (RTT/line/point/diagnostic path).  The preceding
        // drain may have left guest Z resident on the GPU, so restore authoritative bytes before
        // any CPU depth test, texture sample, or Z write.
        // G261: same for GPU-resident RTT rows — materialize any resident target this
        // primitive's frame/Z/texture/CLUT pages touch before the CPU reads or writes them.
        // G278 range-exact edge: culled or disjoint inline primitives do not break a same-key Z
        // run; real target/Z/texture/CLUT aliases publish before resident color materialization.
        (void)g278FlushPendingDepthForInline(
            gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        (void)g278FlushPendingDepthForDisplayOwners(4);
        const G289InlineAudit g289Inline =
            s_g289Owner.active
                ? g289AuditInline(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut)
                : G289InlineAudit{};
        if (s_g289Owner.active && g289StatOn())
        {
            static std::atomic<uint32_t> s_g289InlineLog{0};
            const uint32_t n = s_g289InlineLog.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(
                    stderr,
                    "[G289:inline-edge] n=%u owner=%x..%x carry=%u empty=%u unknown=%u "
                    "prim=%u tme=%u abe=%u fst=%u "
                    "frame=%03x/%u/%x sc=%u,%u..%u,%u tex=%x/%u/%x/%u/%u "
                    "zbuf=%llx test=%llx box=%d,%d..%d,%d "
                    "ranges(c=%x..%x z=%x..%x t=%x..%x p=%x..%x) "
                    "v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                    n + 1u, s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock,
                    g289Inline.carry ? 1u : 0u, g289Inline.empty ? 1u : 0u,
                    g289Inline.unknown ? 1u : 0u,
                    static_cast<unsigned>(gs->m_prim.type), gs->m_prim.tme ? 1u : 0u,
                    gs->m_prim.abe ? 1u : 0u, gs->m_prim.fst ? 1u : 0u,
                    ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw), ctx.frame.psm,
                    ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                    ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), ctx.tex0.psm,
                    static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                    static_cast<unsigned long long>(ctx.zbuf),
                    static_cast<unsigned long long>(ctx.test),
                    g289Inline.x0, g289Inline.y0, g289Inline.x1, g289Inline.y1,
                    g289Inline.color.first, g289Inline.color.last,
                    g289Inline.depth.first, g289Inline.depth.last,
                    g289Inline.texture.first, g289Inline.texture.last,
                    g289Inline.clut.first, g289Inline.clut.last,
                    gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                    gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                    gs->m_vtxQueue[2].x, gs->m_vtxQueue[2].y);
        }
        if (g289Inline.carry)
            g_g289InlineCarries.fetch_add(1u, std::memory_order_relaxed);
        else
            (void)g289MaterializeOwner(4);
        g272MaterializeAll(4); // inline CPU primitive: conservative display-color consumer edge
        g277CensusInlineEdge(gs->m_prim, gs->m_vtxQueue, ctx, gs->m_texclut);
        g276FlushPendingDisplay(4); // G276: publish deferred display readbacks first
        g261PrepareInline(ctx, gs->m_prim, gs->m_texclut);
        g242PrepareVramReadAll(gs->m_vram);
    }

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        // G156: time inline (non-deferred) tri raster on the calling thread. Under MTGS+G144 only
        // NON-deferrable tris reach here (deferrable ones returned early), so inlineMs pairs with
        // g_g156InlineTri. Replay calls drawTriangle directly (never this switch) → no double count.
        if (g_g156Stat && !t_g144InReplay)
        {
            const auto g156T0 = std::chrono::steady_clock::now();
            drawTriangle(gs);
            g_g156InlineTriNs.fetch_add(
                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g156T0)
                    .count(),
                std::memory_order_relaxed);
        }
        else
            drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixel(gs, px, py, v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }

    // G178: an INLINE (non-deferred) draw just wrote guest VRAM directly (e.g. the costume RTT at
    // fbp=0x139). Bump the write generations of the pages its scissored frame region can touch so
    // any GPU-resident texture decoded from those pages (RTT-as-texture) is re-decoded, not stale.
    // Conservative (scissor rect ⊇ written pixels); a no-op single boolean read when the lever is
    // off, and skipped during replay (replay is the GPU path's own CPU fallback, whose target is
    // the display fb the front-end re-uploads unconditionally anyway).
    if (g178GpuOn() && !t_g144InReplay)
    {
        const auto &ctx178 = gs->activeContext();
        // G277-R: bump the prim's CLAMPED bbox ∩ scissor, not the whole scissor rect. Writes are
        // confined to that intersection by construction, so this stays conservative — but a fully
        // scissored-out prim (e.g. the offscreen sceGsSwapDBuff sync linestrips at x≈1792..2303)
        // rasterizes NOTHING and must bump NOTHING. The old whole-scissor bump invalidated the
        // full display target ~1×/frame per buffer, forcing a spurious fb re-upload every batch
        // (and, under G276 deferral, the stale-VRAM clobber behind the MAP-0 Max-body flicker).
        // Kill (restores whole-scissor bump): DC2_G277_NO_INLINE_BBOX_BUMP=1.
        static const bool s_g277BboxBump = !envFlagEnabled("DC2_G277_NO_INLINE_BBOX_BUMP");
        int bx0 = ctx178.scissor.x0, bx1 = ctx178.scissor.x1;
        int by0 = ctx178.scissor.y0, by1 = ctx178.scissor.y1;
        bool bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        if (s_g277BboxBump && !bumpEmpty)
        {
            int nv = 3;
            switch (gs->m_prim.type)
            {
            case GS_PRIM_POINT: nv = 1; break;
            case GS_PRIM_LINE:
            case GS_PRIM_LINESTRIP:
            case GS_PRIM_SPRITE: nv = 2; break;
            default: nv = 3; break;
            }
            const int ofx178 = ctx178.xyoffset.ofx >> 4;
            const int ofy178 = ctx178.xyoffset.ofy >> 4;
            float mnx = gs->m_vtxQueue[0].x, mxx = gs->m_vtxQueue[0].x;
            float mny = gs->m_vtxQueue[0].y, mxy = gs->m_vtxQueue[0].y;
            for (int vi = 1; vi < nv; ++vi)
            {
                mnx = std::min(mnx, gs->m_vtxQueue[vi].x);
                mxx = std::max(mxx, gs->m_vtxQueue[vi].x);
                mny = std::min(mny, gs->m_vtxQueue[vi].y);
                mxy = std::max(mxy, gs->m_vtxQueue[vi].y);
            }
            bx0 = std::max(bx0, static_cast<int>(std::floor(mnx)) - ofx178);
            bx1 = std::min(bx1, static_cast<int>(std::ceil(mxx)) - ofx178);
            by0 = std::max(by0, static_cast<int>(std::floor(mny)) - ofy178);
            by1 = std::min(by1, static_cast<int>(std::ceil(mxy)) - ofy178);
            bumpEmpty = !(bx1 >= bx0 && by1 >= by0);
        }
        if (!bumpEmpty)
        {
            g178NoteVramWriteRect(gs->m_vram,
                                  GSInternal::framePageBaseToBlock(ctx178.frame.fbp),
                                  ctx178.frame.fbw, ctx178.frame.psm,
                                  static_cast<uint32_t>(std::max(bx0, 0)),
                                  static_cast<uint32_t>(std::max(by0, 0)),
                                  static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                  static_cast<uint32_t>(by1 - std::max(by0, 0) + 1));
            if (((ctx178.zbuf >> 32u) & 1u) == 0u)
                g178NoteVramWriteRect(gs->m_vram,
                                      static_cast<uint32_t>(ctx178.zbuf & 0x1FFu) << 5u,
                                      ctx178.frame.fbw,
                                      static_cast<uint32_t>((ctx178.zbuf >> 24u) & 0xFu),
                                      static_cast<uint32_t>(std::max(bx0, 0)),
                                      static_cast<uint32_t>(std::max(by0, 0)),
                                      static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
                                      static_cast<uint32_t>(by1 - std::max(by0, 0) + 1));
        }
    }
}

void GSRasterizer::writePixel(GS *gs, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const auto &ctx = gs->activeContext();
    // PHASE F30: writePixel entry counter, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint64_t> s_f30PxEntry{0};
        const uint64_t n = s_f30PxEntry.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 8ull || (n & 0xFFFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F30:px.enter] n=%llu x=%d y=%d fbp=0x%x fbw=%u psm=%u sciss=[%d,%d,%d,%d]\n",
                         static_cast<unsigned long long>(n), x, y,
                         ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm,
                         ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
    }
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 ||
        y < ctx.scissor.y0 || y > ctx.scissor.y1)
    {
        if (phaseDiagnosticsEnabled())
        {
        static std::atomic<uint64_t> s_f30Sciss{0};
        const uint64_t n = s_f30Sciss.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 4ull || (n & 0x3FFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F30:px.scissCull] n=%llu x=%d y=%d sciss=[%d,%d,%d,%d]\n",
                             static_cast<unsigned long long>(n), x, y,
                             ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
        }
        return;
    }

    const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);
    if (!alphaTest.writeFramebuffer)
    {
        if (phaseDiagnosticsEnabled())
        {
        static std::atomic<uint64_t> s_f30Alpha{0};
        const uint64_t n = s_f30Alpha.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 4ull || (n & 0x3FFFull) == 0ull)
        {
            std::fprintf(stderr, "[F30:px.alphaCull] n=%llu a=%u\n",
                         static_cast<unsigned long long>(n), a);
        }
        }
        return;
    }

    // PHASE F30: writePixel commit counter — track RGB-nonzero @ fbp=0x68 separately
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint64_t> s_f30PxCommit68{0};
        static std::atomic<uint64_t> s_f30PxRgbNz68{0};
        static std::atomic<uint64_t> s_f30PxRgbNz68Logged{0};
        if (ctx.frame.fbp == 0x68u)
        {
            const uint64_t k = s_f30PxCommit68.fetch_add(1, std::memory_order_relaxed) + 1ull;
            const bool rgbNz = (r | g | b) != 0u;
            if (rgbNz)
            {
                s_f30PxRgbNz68.fetch_add(1, std::memory_order_relaxed);
                const uint64_t lg = s_f30PxRgbNz68Logged.fetch_add(1, std::memory_order_relaxed) + 1ull;
                if (lg <= 8ull || (lg & 0x1FFFFull) == 0ull)
                {
                    std::fprintf(stderr,
                                 "[F30:px.rgbNz68] lg=%llu x=%d y=%d rgba=%02x%02x%02x%02x\n",
                                 static_cast<unsigned long long>(lg), x, y, r, g, b, a);
                }
            }
            if ((k & 0xFFFFFull) == 0ull)
            {
                std::fprintf(stderr,
                             "[F30:px.summary68] total=%llu rgbNz=%llu\n",
                             static_cast<unsigned long long>(k),
                             static_cast<unsigned long long>(s_f30PxRgbNz68.load()));
            }
        }
    }

    const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
    const uint32_t bytesPerPixel =
        (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S) ? 2u : 4u;

    uint32_t off = 0u;
    if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
    {
        off = GSPSMCT32::addrPSMCT32(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                     widthBlocks,
                                     static_cast<uint32_t>(x),
                                     static_cast<uint32_t>(y));
    }
    else
    {
        off = addrPSMCT16Family(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                widthBlocks,
                                ctx.frame.psm,
                                static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y));
    }

    if (off + bytesPerPixel > gs->m_vramSize)
        return;

    // G219 (default-off, DC2_G219_SKYPX=1): watch writes landing in the qwords the [G219:dif]
    // capture-vs-replay diff showed being zeroed (0x278400..) — identifies the clobbering draw.
    // (The F51.2 watch below can't serve: renderQualityTraceEnabled() disables deferral itself.)
    {
        static const bool s_g219w2 = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219w2 && off >= 0x278400u && off < 0x278440u)
        {
            static std::atomic<uint32_t> s_g219wpn{0};
            const uint32_t n = s_g219wpn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u || (n % 512u) == 0u)
                std::fprintf(stderr,
                    "[G219:wpx] n=%u off=0x%x xy=(%d,%d) fbp=0x%x fbw=%u fpsm=0x%x prim=%u tme=%u "
                    "tbp=0x%x rgba=%02x,%02x,%02x,%02x replay=%u\n",
                    n, off, x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                    static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0, r, g, b, a,
                    t_g144InReplay ? 1u : 0u);
        }
    }

    // [F51.2] Watch writes that land in the manager texture page (0x272000..0x282000).
    // Identify which draw zeroes the uploaded T8 data (pageNz drops 62381 -> ~8192).
    if (renderQualityTraceEnabled() && off >= 0x272000u && off < 0x282000u)
    {
        static std::atomic<uint32_t> s_f512Watch{0};
        const uint32_t wn = s_f512Watch.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool rgbZero = (r | g | b) == 0u;
        if (wn <= 32u || (rgbZero && wn <= 2000u && (wn % 200u) == 0u))
        {
            std::fprintf(stderr,
                "[F51.2:texwrite] wn=%u off=0x%x x=%d y=%d fbp=0x%x fbw=%u fpsm=0x%x "
                "tme=%u abe=%u rgba=%02x,%02x,%02x,%02x tbp=0x%x tpsm=0x%x\n",
                wn, off, x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                static_cast<uint32_t>(gs->m_prim.tme), static_cast<uint32_t>(gs->m_prim.abe),
                r, g, b, a, ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.psm));
        }
    }

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t pixelIndex = s_debugPixelCount.fetch_add(1, std::memory_order_relaxed);
        if (pixelIndex < 32u)
        {
            std::cout << "[gs:pixel] idx=" << pixelIndex
                      << " xy=(" << x << "," << y << ")"
                      << " rgba=(" << static_cast<uint32_t>(r) << ","
                      << static_cast<uint32_t>(g) << ","
                      << static_cast<uint32_t>(b) << ","
                      << static_cast<uint32_t>(a) << ")"
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " off=0x" << std::hex << off << std::dec
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if (ctx.frame.fbp == 150u &&
            s_debugFbp150PixelCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:fbp150-pixel]"
                      << " xy=(" << x << "," << y << ")"
                      << " rgba=(" << static_cast<uint32_t>(r) << ","
                      << static_cast<uint32_t>(g) << ","
                      << static_cast<uint32_t>(b) << ","
                      << static_cast<uint32_t>(a) << ")"
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " off=0x" << std::hex << off << std::dec << std::endl;
        }
    });

    const uint8_t srcR = r;
    const uint8_t srcG = g;
    const uint8_t srcB = b;
    const uint8_t srcA = a;

    if (gs->m_prim.abe)
    {
        uint32_t existing = 0u;
        if (bytesPerPixel == 2u)
        {
            uint16_t packed = 0u;
            std::memcpy(&packed, gs->m_vram + off, 2);
            existing = decodePSMCT16(packed);
        }
        else
        {
            std::memcpy(&existing, gs->m_vram + off, 4);
        }
        uint8_t dr = existing & 0xFF;
        uint8_t dg = (existing >> 8) & 0xFF;
        uint8_t db = (existing >> 16) & 0xFF;
        uint8_t da = (existing >> 24) & 0xFF;

        // G48: the costume model (fbp=0x139) renders into an RTT the runner CLEARS TO BLACK. PS2
        // alpha-over (Cv=(Cs-Cd)*A>>7+Cd) against a black dest = Cs*A/128, which DARKENS or hides
        // any sub-mesh whose source alpha < 128 when it is the FIRST fragment at a pixel. On real
        // HW the opaque base mesh covers a pixel before the alpha-blended detail/hat/face decals
        // land, so those blends meet real color, not black. The runner's head sub-meshes interleave
        // (skin / hair / hat-decal / face decals) so decals frequently hit the still-black RTT first
        // -> the hat (a decal page) vanished under the old "skip decal over black" heuristic, and
        // face decals over black showed as dark dots. Reproduce HW: when the costume RTT dest is
        // still black (uncovered), write the source color OPAQUELY (skip the blend) for ANY head
        // mesh; subsequent fragments then blend over real color. Replaces the G45 NoDot decal-skip.
        const bool g48OpaqueOverBlack =
            (ctx.frame.fbp == 0x139u) && ((existing & 0x00FFFFFFu) == 0u);
        if (g48OpaqueOverBlack)
        {
            r = srcR; g = srcG; b = srcB;
        }
        // PABE disables alpha blending when the source alpha MSB is clear.
        else if (!(gs->m_pabe && (a & 0x80u) == 0u))
        {
            uint64_t alphaReg = ctx.alpha;
            uint8_t asel = alphaReg & 3;
            uint8_t bsel = (alphaReg >> 2) & 3;
            uint8_t csel = (alphaReg >> 4) & 3;
            uint8_t dsel = (alphaReg >> 6) & 3;
            uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                       : fix;

            r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
            g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
            b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            if (g258TraceOn() && g_g255Verify.pending && g_g255Verify.fbp == 0x146u &&
                g_g255Verify.traceBatch <= 4u && x == 52 && y == 1)
            {
                const int nr = (pickRGB(asel, srcR, dr) - pickRGB(bsel, srcR, dr)) * cAlpha;
                const int ng = (pickRGB(asel, srcG, dg) - pickRGB(bsel, srcG, dg)) * cAlpha;
                const int nb = (pickRGB(asel, srcB, db) - pickRGB(bsel, srcB, db)) * cAlpha;
                std::fprintf(stderr,
                    "[G258:cpu.blend] batch=%llu entry=%zu dst=%02x%02x%02x%02x "
                    "src=%02x%02x%02x%02x c=%d num=(%d,%d,%d) out=%02x%02x%02x%02x\n",
                    static_cast<unsigned long long>(g_g255Verify.traceBatch), t_g258EntryIndex,
                    dr, dg, db, da, srcR, srcG, srcB, srcA, cAlpha, nr, ng, nb,
                    r, g, b, srcA);
            }
        }
        else
        {
            r = srcR;
            g = srcG;
            b = srcB;
        }
    }

    uint32_t mask = ctx.frame.fbmsk;
    if (!alphaTest.preserveDestinationAlpha &&
        (ctx.fba & 0x1ull) != 0ull &&
        ctx.frame.psm != GS_PSM_CT24)
    {
        a = static_cast<uint8_t>(a | 0x80u);
    }

    if (bytesPerPixel == 2u)
    {
        uint16_t pixel = encodePSMCT16(r, g, b, a);
        if ((mask & 0xFFFFu) != 0u)
        {
            uint16_t existing = 0u;
            std::memcpy(&existing, gs->m_vram + off, 2);
            pixel = static_cast<uint16_t>((pixel & ~mask) | (existing & mask));
        }
        if (g195ForegroundTraceTarget(ctx, x, y))
        {
            uint16_t before = 0u;
            std::memcpy(&before, gs->m_vram + off, 2);
            g195LogForegroundPixel(ctx, x, y, off, bytesPerPixel,
                                   static_cast<uint32_t>(gs->m_prim.type),
                                   gs->m_prim.tme, gs->m_prim.abe,
                                   gs->m_prim.fst, gs->m_prim.ctxt, gs->m_pabe,
                                   decodePSMCT16(before), srcR, srcG, srcB, srcA,
                                   decodePSMCT16(pixel));
        }
        f50_12_log_framebuffer_commit(ctx, x, y, off, bytesPerPixel, decodePSMCT16(pixel),
                                      gs->m_prim.type, gs->m_prim.tme, gs->m_prim.abe,
                                      gs->m_prim.fst, gs->m_prim.ctxt);
        std::memcpy(gs->m_vram + off, &pixel, 2);
        return;
    }

    uint32_t pixel = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);

    if (mask != 0)
    {
        uint32_t existing;
        std::memcpy(&existing, gs->m_vram + off, 4);
        pixel = (pixel & ~mask) | (existing & mask);
    }

    if (alphaTest.preserveDestinationAlpha)
    {
        uint32_t existing = 0u;
        std::memcpy(&existing, gs->m_vram + off, 4);
        pixel = (pixel & 0x00FFFFFFu) | (existing & 0xFF000000u);
    }

    if (g195ForegroundTraceTarget(ctx, x, y))
    {
        uint32_t before = 0u;
        std::memcpy(&before, gs->m_vram + off, 4);
        g195LogForegroundPixel(ctx, x, y, off, bytesPerPixel,
                               static_cast<uint32_t>(gs->m_prim.type),
                               gs->m_prim.tme, gs->m_prim.abe,
                               gs->m_prim.fst, gs->m_prim.ctxt, gs->m_pabe,
                               before, srcR, srcG, srcB, srcA, pixel);
    }

    if (g43FacePixelTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        x >= 344 && x <= 412 &&
        y >= 76 && y <= 144 &&
        ((x & 3) == 0) &&
        ((y & 3) == 0))
    {
        static std::atomic<uint32_t> s_g43FacePx{0};
        const uint32_t n = s_g43FacePx.fetch_add(1u, std::memory_order_relaxed);
        if (n < 4000u)
        {
            std::fprintf(stderr,
                "[G43:px] n=%u xy=(%d,%d) prim=%u tme=%u abe=%u fst=%u iip=%u "
                "tbp=0x%x tpsm=0x%x cbp=0x%x csa=%u src=%02x,%02x,%02x,%02x "
                "final=0x%08x alpha=0x%llx test=0x%llx zbuf=0x%llx\n",
                n, x, y,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.iip),
                ctx.tex0.tbp0,
                static_cast<uint32_t>(ctx.tex0.psm),
                ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.csa),
                srcR, srcG, srcB, a,
                pixel,
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf));
        }
    }

    f50_12_log_framebuffer_commit(ctx, x, y, off, bytesPerPixel, pixel,
                                  gs->m_prim.type, gs->m_prim.tme, gs->m_prim.abe,
                                  gs->m_prim.fst, gs->m_prim.ctxt);
    std::memcpy(gs->m_vram + off, &pixel, 4);
}

// F62 (PR #132): texel reads now go through the GSMem swizzle module. GSMem's
// address math is byte-identical to the local GSPSMCT32/GSPSMT4 helpers (proven for
// 32-bit and the index formats) and is the canonical swizzle for the 16-bit family
// (which the old upload wrote linearly — see processImageData). GSMem takes the
// buffer width in PAGES, so PSMT4's 128-wide page needs tbw>>1; the 32/24/16-bit and
// the *H index formats (64-wide page) use tbw directly.
uint32_t GSRasterizer::readTexelPSMCT32(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;
    return GSMem::ReadCT32(gs->m_vram, tbp0, tbw,
                           static_cast<uint32_t>(texU), static_cast<uint32_t>(texV));
}

uint32_t GSRasterizer::readTexelPSMCT16(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;
    const uint8_t psm = gs->activeContext().tex0.psm;
    uint32_t raw;
    switch (psm)
    {
    case GS_PSM_CT16S: raw = GSMem::ReadCT16S(gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    case GS_PSM_Z16:   raw = GSMem::ReadZ16  (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    case GS_PSM_Z16S:  raw = GSMem::ReadZ16S (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    default:           raw = GSMem::ReadCT16 (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    }
    return decodePSMCT16(static_cast<uint16_t>(raw));
}

uint32_t GSRasterizer::readTexelPSMT4(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;

    const uint8_t psm = gs->activeContext().tex0.psm;
    const uint32_t ux = static_cast<uint32_t>(texU);
    const uint32_t uy = static_cast<uint32_t>(texV);

    // T4HH/T4HL hide the 4-bit index in the upper byte of a 32-bit word (F37). GSMem's
    // P4HH/P4HL readers (BitOffset 28/24 over the C32 page layout) extract exactly that
    // nibble — byte-identical to the old upper-byte read.
    if (psm == GS_PSM_T4HH)
        return GSMem::ReadP4HH(gs->m_vram, tbp0, tbw, ux, uy);
    if (psm == GS_PSM_T4HL)
        return GSMem::ReadP4HL(gs->m_vram, tbp0, tbw, ux, uy);

    // Plain PSMT4: 128-wide page → tbw>>1.
    const uint32_t pageBwT4 = ((tbw >> 1u) != 0u) ? (tbw >> 1u) : 1u;
    return GSMem::ReadP4(gs->m_vram, tbp0, pageBwT4, ux, uy);
}

uint32_t GSRasterizer::lookupCLUT(GS *gs,
                                  uint8_t index,
                                  uint32_t cbp,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, csm, csa, sourcePsm);
    const uint32_t clutWidth = (gs->m_texclut.cbw != 0u) ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(gs->m_texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(gs->m_texclut.cov) + (clutIndex >> 4);

    // F62 (PR #132): CLUT fetch via GSMem (CT32 == local addrPSMCT32, byte-identical;
    // 16-bit uses the canonical swizzle). NOTE: unlike the PR's hunk, the 16-bit CLUT
    // is still run through decodePSMCT16 (the PR dropped that, which would feed raw
    // 5-5-5-1 into applyTexa).
    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(gs->m_texa, cpsm, GSMem::ReadCT32(gs->m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(gs->m_texa, cpsm, GSMem::ReadCT24(gs->m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(gs->m_texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16(gs->m_vram, cbp, clutWidth, clutX, clutY))));
    case GS_PSM_CT16S:
        return applyTexa(gs->m_texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16S(gs->m_vram, cbp, clutWidth, clutX, clutY))));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSRasterizer::sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = gs->activeContext();
    const auto &tex = ctx.tex0;

    int texW = 1 << tex.tw;
    int texH = 1 << tex.th;

    float texUf, texVf;
    if (gs->m_prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto f50_12_log_pal_sample = [&](int sampleU, int sampleV, uint32_t idx, uint32_t color) {
        if (!f50_12_trace_enabled())
            return;

        static std::atomic<uint64_t> s_total{0};
        static std::atomic<uint64_t> s_bad{0};
        static std::atomic<uint64_t> s_title{0};
        static std::atomic<uint64_t> s_manager{0};
        static std::atomic<uint64_t> s_other{0};

        const bool bad = tex.tbp0 == 0x2580u && tex.cbp == 0x2980u && tex.cpsm == GS_PSM_CT16;
        const bool title = tex.tbp0 == 0x1A00u && tex.cbp == 0x3FE0u;
        const bool manager = tex.tbp0 == 0x2720u || tex.tbp0 == 0x2820u ||
                             tex.tbp0 == 0x29A0u || tex.tbp0 == 0x2F20u ||
                             tex.tbp0 == 0x3420u;
        const char *bucket = "other";
        std::atomic<uint64_t> *bucketCounter = &s_other;
        if (bad)
        {
            bucket = "bad2580";
            bucketCounter = &s_bad;
        }
        else if (title)
        {
            bucket = "title";
            bucketCounter = &s_title;
        }
        else if (manager)
        {
            bucket = "manager";
            bucketCounter = &s_manager;
        }

        const uint64_t n = s_total.fetch_add(1u, std::memory_order_relaxed) + 1ull;
        const uint64_t bucketN = bucketCounter->fetch_add(1u, std::memory_order_relaxed) + 1ull;
        const bool shouldLog = bucketN <= 32ull ||
                               (bad && (bucketN % 1048576ull) == 0ull) ||
                               (manager && (bucketN % 65536ull) == 0ull) ||
                               ((n % 1048576ull) == 0ull);
        if (!shouldLog)
            return;

        std::fprintf(stderr,
                     "[F512:samples] n=%llu bucket=%s bucketN=%llu prim=%u tme=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u texclut(cbw=%u,cou=%u,cov=%u) uv=(%d,%d) idx=%u color=0x%08x\n",
                     static_cast<unsigned long long>(n),
                     bucket,
                     static_cast<unsigned long long>(bucketN),
                     static_cast<uint32_t>(gs->m_prim.type),
                     static_cast<uint32_t>(gs->m_prim.tme),
                     static_cast<uint32_t>(gs->m_prim.abe),
                     static_cast<uint32_t>(gs->m_prim.fst),
                     static_cast<uint32_t>(gs->m_prim.ctxt),
                     ctx.frame.fbp,
                     static_cast<uint32_t>(ctx.frame.fbw),
                     static_cast<uint32_t>(ctx.frame.psm),
                     static_cast<unsigned long long>(f50_12_tex0_raw(tex)),
                     tex.tbp0,
                     static_cast<uint32_t>(tex.tbw),
                     static_cast<uint32_t>(tex.psm),
                     static_cast<uint32_t>(tex.tw),
                     static_cast<uint32_t>(tex.th),
                     static_cast<uint32_t>(tex.tcc),
                     static_cast<uint32_t>(tex.tfx),
                     tex.cbp,
                     static_cast<uint32_t>(tex.cpsm),
                     static_cast<uint32_t>(tex.csm),
                     static_cast<uint32_t>(tex.csa),
                     static_cast<uint32_t>(tex.cld),
                     static_cast<uint32_t>(gs->m_texclut.cbw),
                     static_cast<uint32_t>(gs->m_texclut.cou),
                     static_cast<uint32_t>(gs->m_texclut.cov),
                     sampleU,
                     sampleV,
                     idx,
                     color);
    };

    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    const int minU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
    const int maxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
    const int minV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
    const int maxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, wrapU, texW, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, wrapV, texH, minV, maxV);

        if (tex.psm == GS_PSM_CT32 || tex.psm == GS_PSM_CT24)
            return applyTexa(gs->m_texa, tex.psm, readTexelPSMCT32(gs, tex.tbp0, tex.tbw, sampleU, sampleV));

        if (tex.psm == GS_PSM_CT16 || tex.psm == GS_PSM_CT16S)
            return applyTexa(gs->m_texa, tex.psm, readTexelPSMCT16(gs, tex.tbp0, tex.tbw, sampleU, sampleV));

        if (tex.psm == GS_PSM_T4 ||
            tex.psm == GS_PSM_T4HL ||
            tex.psm == GS_PSM_T4HH)
        {
            // G5: one-shot decode of DC2's 4HH debug-font atlas. This separates
            // VRAM/CLUT extraction defects from coordinate wrapping defects.
            if (tex.psm == GS_PSM_T4HH && tex.tbp0 == 0x1A00u && envFlagEnabled("DC2_DUMP_FONT"))
            {
                static std::atomic<uint32_t> s_dumped4hh{0};
                if (s_dumped4hh.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    const int dw = 1 << tex.tw;
                    const int dh = 1 << tex.th;
                    char path[128];
                    std::snprintf(path, sizeof(path), "captures/font4hh_%x.ppm", tex.tbp0);
                    FILE *fp = std::fopen(path, "wb");
                    if (fp)
                    {
                        std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                        for (int yy = 0; yy < dh; ++yy)
                        {
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                const uint8_t ix = static_cast<uint8_t>(
                                    GSMem::ReadP4HH(gs->m_vram, tex.tbp0, tex.tbw,
                                                   static_cast<uint32_t>(xx),
                                                   static_cast<uint32_t>(yy)));
                                const uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm,
                                                              tex.csm, tex.csa, tex.psm);
                                const uint32_t alpha = std::min<uint32_t>((c >> 24) & 0xFFu, 0x80u);
                                const uint8_t rgb[3] = {
                                    static_cast<uint8_t>(((c & 0xFFu) * alpha) / 0x80u),
                                    static_cast<uint8_t>((((c >> 8) & 0xFFu) * alpha) / 0x80u),
                                    static_cast<uint8_t>((((c >> 16) & 0xFFu) * alpha) / 0x80u)
                                };
                                std::fwrite(rgb, 1, 3, fp);
                            }
                        }
                        std::fclose(fp);
                    }

                    const char *vramPath = "captures/font4hh_vram.bin";
                    FILE *vramFp = std::fopen(vramPath, "wb");
                    if (vramFp)
                    {
                        std::fwrite(gs->m_vram, 1, gs->m_vramSize, vramFp);
                        std::fclose(vramFp);
                    }

                    std::fprintf(stderr,
                                 "[G5:font4hh] path=%s vram=%s tbp=0x%x tbw=%u psm=0x%x size=%dx%d "
                                 "cbp=0x%x cpsm=0x%x csa=%u csm=%u cld=%u clamp=0x%llx\n",
                                 path, vramPath, tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                 static_cast<uint32_t>(tex.psm), dw, dh, tex.cbp,
                                 static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                                 static_cast<uint32_t>(tex.csm), static_cast<uint32_t>(tex.cld),
                                 static_cast<unsigned long long>(ctx.clamp));

                    for (uint32_t bw = 1u; bw <= 16u; ++bw)
                    {
                        uint32_t nonzero = 0u;
                        uint32_t hash = 2166136261u;
                        for (int yy = 0; yy < dh; ++yy)
                        {
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                const uint8_t ix = static_cast<uint8_t>(
                                    GSMem::ReadP4HH(gs->m_vram, tex.tbp0, bw,
                                                   static_cast<uint32_t>(xx),
                                                   static_cast<uint32_t>(yy)));
                                nonzero += (ix != 0u) ? 1u : 0u;
                                hash = (hash ^ ix) * 16777619u;
                            }
                        }
                        std::fprintf(stderr, "[G5:font4hh-bw] bw=%u nonzero=%u hash=0x%08x%s\n",
                                     bw, nonzero, hash, (bw == tex.tbw) ? " active" : "");
                    }

                    for (uint32_t csa = 0u; csa < 32u; ++csa)
                    {
                        std::fprintf(stderr, "[G5:font4hh-csa] csa=%u", csa);
                        for (uint32_t ix = 0u; ix < 16u; ++ix)
                        {
                            const uint32_t c = lookupCLUT(gs, static_cast<uint8_t>(ix), tex.cbp,
                                                          tex.cpsm, tex.csm,
                                                          static_cast<uint8_t>(csa), tex.psm);
                            std::fprintf(stderr, " %08x", c);
                        }
                        std::fputc('\n', stderr);
                    }
                }
            }

            uint32_t idx = readTexelPSMT4(gs, tex.tbp0, tex.tbw, sampleU, sampleV);
            uint32_t clutColor = lookupCLUT(gs, static_cast<uint8_t>(idx), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            f50_12_log_pal_sample(sampleU, sampleV, idx, clutColor);
            if (renderQualityTraceEnabled())
            {
                // F50.10: capture paletted samples that are NOT the dominant tbp=0x2580
                // material, to see whether the data-backed streamed textures (0x2820/
                // 0x29a0/0x2f20, proven present in VRAM) are ever actually drawn.
                if (tex.tbp0 != 0x2580u)
                {
                    static std::atomic<int> s_f510other{0};
                    const int o = s_f510other.fetch_add(1, std::memory_order_relaxed);
                    if (o < 48)
                        std::fprintf(stderr,
                                     "[F510:othertex] o=%d psm=0x%x tbp=0x%x tbw=%u cbp=0x%x cpsm=0x%x cld=%u idx=%u color=0x%08x\n",
                                     o, (uint32_t)tex.psm, tex.tbp0, (uint32_t)tex.tbw,
                                     tex.cbp, (uint32_t)tex.cpsm, (uint32_t)tex.cld, idx, clutColor);
                }
                static std::atomic<int> s_f508T4{0};
                const int k = s_f508T4.fetch_add(1, std::memory_order_relaxed);
                if (k < 96)
                {
                    const uint32_t cw = gs->m_texclut.cbw ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;
                    const uint32_t coff = GSPSMCT32::addrPSMCT32(tex.cbp, cw,
                                                                 static_cast<uint32_t>(gs->m_texclut.cou),
                                                                 gs->m_texclut.cov);
                    uint32_t cbase = 0u;
                    if (coff + 4u <= gs->m_vramSize)
                        std::memcpy(&cbase, gs->m_vram + coff, 4);
                    std::fprintf(stderr,
                                 "[F508:t4clut] k=%d psm=0x%x tbp=0x%x tbw=%u cbp=0x%x cpsm=0x%x csa=%u cld=%u idx=%u color=0x%08x clutbase@0x%x=0x%08x\n",
                                 k, static_cast<uint32_t>(tex.psm), tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                 tex.cbp, static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                                 static_cast<uint32_t>(tex.cld),
                                 idx, clutColor, coff, cbase);
                    // F50.9: one-time dump of BOTH GS contexts' TEX0 + active context, to
                    // check whether the texture bind targeted one context while the draw
                    // samples the (stale) other context (PRIM.CTXT mismatch).
                    if (k == 0)
                    {
                        const auto &t0 = gs->m_ctx[0].tex0;
                        const auto &t1 = gs->m_ctx[1].tex0;
                        std::fprintf(stderr,
                                     "[F509:ctxdump] primCtxt=%u activeIsT0=%d "
                                     "ctx0(tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u) "
                                     "ctx1(tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u)\n",
                                     gs->m_prim.ctxt ? 1u : 0u, (&ctx == &gs->m_ctx[0]) ? 1 : 0,
                                     t0.tbp0, (unsigned)t0.tbw, (unsigned)t0.psm, t0.cbp, (unsigned)t0.cpsm, (unsigned)t0.cld,
                                     t1.tbp0, (unsigned)t1.tbw, (unsigned)t1.psm, t1.cbp, (unsigned)t1.cpsm, (unsigned)t1.cld);
                    }
                    // F50.9: one-time dump of the actual upload destinations seen in the
                    // BITBLTBUF histogram (textures @0x2820/0x29a0/0x2f20, CLUTs @0x3fb4..0x3fbc)
                    // to confirm the uploaded data physically landed, vs the (empty) pages the
                    // draws reference (tbp=0x2580 cbp=0x2980).
                    if (k == 60)
                    {
                        auto dwAt = [&](uint32_t blk) -> uint32_t {
                            const uint32_t byteOff = blk * 256u;
                            uint32_t w = 0u;
                            if (byteOff + 4u <= gs->m_vramSize)
                                std::memcpy(&w, gs->m_vram + byteOff, 4);
                            return w;
                        };
                        // Count nonzero bytes per 64KB region across all of VRAM so we can see
                        // WHERE uploaded image data actually landed (vs the empty draw pages).
                        auto nzRegion = [&](uint32_t startByte, uint32_t len) -> uint32_t {
                            uint32_t nz = 0u;
                            const uint32_t end = (startByte + len <= gs->m_vramSize) ? (startByte + len) : gs->m_vramSize;
                            for (uint32_t i = startByte; i < end; ++i)
                                if (gs->m_vram[i] != 0u) ++nz;
                            return nz;
                        };
                        std::fprintf(stderr,
                                     "[F508:vramdump] tex@0x2820=0x%08x tex@0x29a0=0x%08x tex@0x2f20=0x%08x "
                                     "clut@0x3fb4=0x%08x clut@0x3fb8=0x%08x clut@0x3fbc=0x%08x clut@0x3fe0=0x%08x "
                                     "drawTex@0x2580=0x%08x drawClut@0x2980=0x%08x vramSize=0x%x\n",
                                     dwAt(0x2820u), dwAt(0x29a0u), dwAt(0x2f20u),
                                     dwAt(0x3fb4u), dwAt(0x3fb8u), dwAt(0x3fbcu), dwAt(0x3fe0u),
                                     dwAt(0x2580u), dwAt(0x2980u), gs->m_vramSize);
                        // 0x100000-byte (1MB) buckets across 4MB VRAM.
                        std::fprintf(stderr,
                                     "[F508:vramnz] nz[0-1MB]=%u nz[1-2MB]=%u nz[2-3MB]=%u nz[3-4MB]=%u "
                                     "nz@tex0x258000(64K)=%u nz@tex0x282000(64K)=%u nz@clut0x3fb000(16K)=%u\n",
                                     nzRegion(0x000000u, 0x100000u), nzRegion(0x100000u, 0x100000u),
                                     nzRegion(0x200000u, 0x100000u), nzRegion(0x300000u, 0x100000u),
                                     nzRegion(0x258000u, 0x10000u), nzRegion(0x282000u, 0x10000u),
                                     nzRegion(0x3fb000u, 0x4000u));
                    }
                }
            }
            return clutColor;
        }

        if (tex.psm == GS_PSM_T8)
        {
            if (tex.tbw == 0)
                return 0xFFFF00FFu;
            // G2: one-shot dump of the decoded T8 atlas as the sampler sees it (full
            // tw/th, GSMem::ReadP8 -> lookupCLUT). DC2_DUMP_FONT=1. Reveals whether the
            // CT32-alias upload + T8 read recover a coherent glyph atlas, or garbage.
            {
                static const bool s_dumpFont = []() {
                    const char *e = std::getenv("DC2_DUMP_FONT");
                    return e && e[0] != '0';
                }();
                if (s_dumpFont)
                {
                    static std::atomic<uint32_t> s_dumped{0};
                    // dump each distinct font-ish tbp once
                    static std::atomic<uint32_t> s_seen2720{0}, s_seen2f20{0}, s_seen3220{0}, s_seen2aa0{0};
                    std::atomic<uint32_t> *seen =
                        (tex.tbp0 == 0x2720u) ? &s_seen2720 :
                        (tex.tbp0 == 0x2f20u) ? &s_seen2f20 :
                        (tex.tbp0 == 0x2aa0u) ? &s_seen2aa0 :
                        (tex.tbp0 == 0x3220u) ? &s_seen3220 : nullptr;
                    if (seen && seen->fetch_add(1u, std::memory_order_relaxed) == 0u && s_dumped.load() < 6u)
                    {
                        s_dumped.fetch_add(1u, std::memory_order_relaxed);
                        const int dw = 1 << tex.tw, dh = 1 << tex.th;
                        for (uint32_t bwv = 1u; bwv <= 4u; ++bwv)
                        {
                            char path[128];
                            std::snprintf(path, sizeof(path), "captures/font_%x_bw%u.ppm", tex.tbp0, bwv);
                            FILE *fp = std::fopen(path, "wb");
                            if (!fp) continue;
                            std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                            for (int yy = 0; yy < dh; ++yy)
                                for (int xx = 0; xx < dw; ++xx)
                                {
                                    uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                                     static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                                    uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                                    uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                                       static_cast<uint8_t>((c >> 8) & 0xFF),
                                                       static_cast<uint8_t>((c >> 16) & 0xFF) };
                                    std::fwrite(rgb, 1, 3, fp);
                                }
                            std::fclose(fp);
                        }
                        {
                            std::fprintf(stderr, "[G2:fontdump] wrote %x bw-sweep dw=%d dh=%d tbw=%u cbp=0x%x cpsm=%u csa=%u\n",
                                tex.tbp0, dw, dh, tex.tbw, tex.cbp, (unsigned)tex.cpsm, (unsigned)tex.csa);
                        }
                        // Variant 2: read the page as RAW LINEAR bytes (base + y*dw + x),
                        // i.e. exactly how a flat host buffer would lay it out. If THIS is
                        // legible, the data landed linearly and the T8 swizzle read is wrong.
                        char path2[128];
                        std::snprintf(path2, sizeof(path2), "captures/font_%x_linear.ppm", tex.tbp0);
                        FILE *fp2 = std::fopen(path2, "wb");
                        if (fp2)
                        {
                            const uint32_t base = tex.tbp0 * 256u;
                            std::fprintf(fp2, "P6\n%d %d\n255\n", dw, dh);
                            for (int yy = 0; yy < dh; ++yy)
                                for (int xx = 0; xx < dw; ++xx)
                                {
                                    const uint32_t o = base + static_cast<uint32_t>(yy * dw + xx);
                                    uint8_t ix = (o < gs->m_vramSize) ? gs->m_vram[o] : 0u;
                                    uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                                    uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                                       static_cast<uint8_t>((c >> 8) & 0xFF),
                                                       static_cast<uint8_t>((c >> 16) & 0xFF) };
                                    std::fwrite(rgb, 1, 3, fp2);
                                }
                            std::fclose(fp2);
                        }
                        // G3: raw page-region snapshot at SAMPLE time. Compare vs the
                        // post-upload snapshot ([G3:postup]) to detect an overwrite of
                        // the font page between upload and sampling. DC2_DUMP_VRAMRGN=1.
                        {
                            const char *er = std::getenv("DC2_DUMP_VRAMRGN");
                            if (er && er[0] != '0')
                            {
                                const uint32_t rbase = tex.tbp0 * 256u;
                                const uint32_t rlen = 0x40000u;
                                if (rbase + rlen <= gs->m_vramSize)
                                {
                                    char pr[128];
                                    std::snprintf(pr, sizeof(pr), "captures/vram_atsample_%x.bin", tex.tbp0);
                                    FILE *fpr = std::fopen(pr, "wb");
                                    if (fpr) { std::fwrite(gs->m_vram + rbase, 1, rlen, fpr); std::fclose(fpr); }
                                    std::fprintf(stderr, "[G3:atsample] wrote %s\n", pr);
                                }
                            }
                        }
                    }
                }
            }
            // [F51.3] env-gated experiment: override the PSMT8 sample stride. DC2 uploads its
            // manager T8 textures via a CT32 alias (BITBLT dbw=2) but declares TEX0.TBW=6
            // (< TW-width/64=8); the upload-consistent stride is dbw*2=4. Default OFF (no behavior
            // change) — set DC2_T8_ALIAS_TBW=4 to test whether this makes the dungeon non-black.
            uint32_t t8tbw = tex.tbw;
            {
                static const int s_aliasTbw = []() {
                    const char *e = std::getenv("DC2_T8_ALIAS_TBW");
                    return e ? std::atoi(e) : 0;
                }();
                if (s_aliasTbw > 0)
                    t8tbw = static_cast<uint32_t>(s_aliasTbw);
            }
            uint32_t off = GSPSMT8::addrPSMT8(tex.tbp0, t8tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
            if (renderQualityTraceEnabled())
            {
                static std::atomic<int> s_t8Probe{0};
                if (s_t8Probe.fetch_add(1, std::memory_order_relaxed) < 5)
                {
                    uint8_t vb = (off < gs->m_vramSize) ? gs->m_vram[off] : 0xFFu;
                    std::fprintf(stderr, "[F37:t8-sample] tbp=0x%x tbw=%u uv=(%d,%d) off=0x%x vram_byte=0x%02x cbp=0x%x cpsm=%u csm=%u\n",
                        tex.tbp0, tex.tbw, sampleU, sampleV, off, vb, tex.cbp, tex.cpsm, tex.csm);
                }
                // G2 self-check: is GSMem::ReadP8 byte-identical to the local addrPSMT8 read?
                // If g!=l ever fires, the swizzles differ; if never, the read fix is a no-op
                // and the corruption is upstream (CT32-alias upload / wrong format).
                {
                    static std::atomic<int> s_g2chk{0};
                    static std::atomic<int> s_g2mismatch{0};
                    const uint32_t pageBwT8 = ((t8tbw >> 1u) != 0u) ? (t8tbw >> 1u) : 1u;
                    uint8_t lb = (off < gs->m_vramSize) ? gs->m_vram[off] : 0xFFu;
                    uint8_t gb = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                                     static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
                    if (lb != gb) s_g2mismatch.fetch_add(1, std::memory_order_relaxed);
                    int n = s_g2chk.fetch_add(1, std::memory_order_relaxed);
                    if (n < 16 || (lb != gb && n < 4096 && (n % 64) == 0))
                        std::fprintf(stderr, "[G2:t8cmp] n=%d tbp=0x%x tbw=%u uv=(%d,%d) local=0x%02x gsmem=0x%02x %s mism=%d\n",
                            n, tex.tbp0, t8tbw, sampleU, sampleV, lb, gb, (lb==gb?"EQ":"DIFF"),
                            s_g2mismatch.load(std::memory_order_relaxed));
                }
                // F51: one-shot per-TBW scan for the manager texture. The CT32-alias
                // upload uses BITBLTBUF.DBW; the sampler reads addrPSMT8 with TEX0.TBW.
                // Probe which TBW (if any) makes the sampled texel land on uploaded data,
                // to decide whether the upload page-stride (DBW) or the sample TBW is off.
                if (tex.tbp0 == 0x2720u)
                {
                    static std::atomic<int> s_t8tbw{0};
                    if (s_t8tbw.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        char line[384];
                        int n = std::snprintf(line, sizeof(line),
                            "[F51:tbwscan] uv=(%d,%d) sampleTbw=%u:",
                            sampleU, sampleV, static_cast<uint32_t>(tex.tbw));
                        for (uint32_t tw = 2u; tw <= 8u && n > 0 && n < (int)sizeof(line); ++tw)
                        {
                            uint32_t o = GSPSMT8::addrPSMT8(tex.tbp0, tw,
                                static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
                            uint8_t b = (o < gs->m_vramSize) ? gs->m_vram[o] : 0xFFu;
                            n += std::snprintf(line + n, sizeof(line) - n,
                                " tbw%u=0x%02x@0x%x", tw, b, o);
                        }
                        // locate the first nonzero byte in the CT32-written window (pages 0x139..0x141)
                        uint32_t firstNz = 0u; uint8_t firstNzB = 0u;
                        for (uint32_t a = 0x272000u; a < 0x282000u && a < gs->m_vramSize; ++a)
                            if (gs->m_vram[a] != 0u) { firstNz = a; firstNzB = gs->m_vram[a]; break; }
                        // linear nonzero count of the whole 64KB page at SAMPLE time. Compare vs
                        // upload's nzT8=62381: ~62000 => data present (read at wrong addr = swizzle bug);
                        // ~0 => data overwritten (RTT/clear corruption).
                        uint32_t pageNz = 0u;
                        for (uint32_t a = 0x272000u; a < 0x282000u && a < gs->m_vramSize; ++a)
                            if (gs->m_vram[a] != 0u) ++pageNz;
                        std::fprintf(stderr, "%s | firstNz@0x%x=0x%02x pageNz=%u\n", line, firstNz, firstNzB, pageNz);
                    }
                }
            }
            // G2: route the PSMT8 read through the byte-exact GSMem swizzle (like the F62
            // T8 *upload* GSMem::WriteP8, and the T4/CT32/CT16 reads). The old local
            // GSPSMT8::addrPSMT8 is NOT byte-identical to GSMem's P8 page/column table, so
            // textures DC2 uploads via a CT32-alias BITBLT (dpsm=CT32, written by byte-exact
            // GSMem::WriteCT32) and then samples as T8 came back garbled — this is the
            // menu/dialog/HUD font-atlas corruption (fonts are T8: tbp 0x2720/0x2f20, cbp
            // CT32). GSMem buffer width is in PAGES (T8 page = 128 wide) → tbw>>1, symmetric
            // with the upload. Default ON; DC2_T8_GSMEM=0 falls back to the old path for A/B.
            static const bool s_t8Gsmem = []() {
                const char *e = std::getenv("DC2_T8_GSMEM");
                return !(e && (e[0] == '0'));
            }();
            uint8_t idx;
            if (s_t8Gsmem)
            {
                const uint32_t pageBwT8 = ((t8tbw >> 1u) != 0u) ? (t8tbw >> 1u) : 1u;
                idx = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                                                         static_cast<uint32_t>(sampleU),
                                                         static_cast<uint32_t>(sampleV)));
            }
            else
            {
                if (off >= gs->m_vramSize)
                    return 0xFFFF00FFu;
                idx = gs->m_vram[off];
            }
            uint32_t clutColor = lookupCLUT(gs, idx, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            f50_12_log_pal_sample(sampleU, sampleV, idx, clutColor);
            // [F51.5] decisive: the displayed map texture is tbp=0x3220. Log its actual idx/color.
            // idx==0 -> texture reads 0 at the sampled UV (UV/overwrite); idx!=0 && color==0 -> CLUT
            // lookup broken (same class as 0x1a00 with the never-uploaded 0x3fe0 CLUT).
            if (renderQualityTraceEnabled() && tex.tbp0 == 0x3220u)
            {
                static std::atomic<int> s_f515{0};
                if (s_f515.fetch_add(1, std::memory_order_relaxed) < 12)
                    std::fprintf(stderr,
                        "[F51.5:mapsample] tbp=0x3220 tbw=%u uv=(%d,%d) off=0x%x byte=0x%02x idx=%u "
                        "cbp=0x%x cpsm=%u csa=%u cld=%u color=0x%08x\n",
                        static_cast<uint32_t>(tex.tbw), sampleU, sampleV, off,
                        (off < gs->m_vramSize ? gs->m_vram[off] : 0xFFu), static_cast<uint32_t>(idx),
                        tex.cbp, static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                        static_cast<uint32_t>(tex.cld), clutColor);
            }
            return clutColor;
        }

        // F62 (PR #132): formats the old sampler returned magenta for. GSMem decodes
        // them; DC2 is not known to source these as textures, so this only adds
        // coverage (no change for the CT32/CT16/T4/T8 paths above).
        if (tex.psm == GS_PSM_T8H)
        {
            const uint32_t idx = GSMem::ReadP8H(gs->m_vram, tex.tbp0, tex.tbw,
                                                static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
            return lookupCLUT(gs, static_cast<uint8_t>(idx), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        }
        if (tex.psm == GS_PSM_Z32)
            return applyTexa(gs->m_texa, tex.psm, GSMem::ReadZ32(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
        if (tex.psm == GS_PSM_Z24)
            return applyTexa(gs->m_texa, tex.psm, GSMem::ReadZ24(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
        if (tex.psm == GS_PSM_Z16)
            return applyTexa(gs->m_texa, tex.psm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadZ16(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)))));
        if (tex.psm == GS_PSM_Z16S)
            return applyTexa(gs->m_texa, tex.psm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadZ16S(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)))));

        return 0xFFFF00FFu;
    };

    // G45 experiment: the costume head's detailed face texture is point-sampled at native
    // 512-wide res, so its dark detail texels (nose/brow/freckle) pop as hard dots vs PCSX2's
    // upscaled+filtered smooth face. Optionally force bilinear for the costume model RTT only.
    static const bool s_g45Bilinear = (std::getenv("DC2_G45_BILINEAR") != nullptr);
    const bool g45ForceLinear = s_g45Bilinear && gs->activeContext().frame.fbp == 0x139u;

    // G51: minification box-filter for the costume model RTT. The face texture (0x3480) is a
    // detailed 128x128 with fine dark feature texels (brows/eye-liner/nostril/lip/eye-socket
    // shadow); it is minified ~2.5x onto the ~50px on-screen face. Point sampling (and even a 2x2
    // bilinear tap) skips texels between samples -> the dark features ALIAS into scattered dark
    // "speckle" dots, vs PCSX2's internal-res-upscaled smooth reference. Average a (2k+1)^2 texel
    // box around the sample to approximate area minification. Scoped to fbp==0x139; env-gated
    // DC2_G51_MINFILT = box radius k in texels (0 = off). Decisive root-cause test + the fix.
    static const int s_g51MinFilt = []() -> int {
        const char *e = std::getenv("DC2_G51_MINFILT");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g51MinFilt > 0 && gs->activeContext().frame.fbp == 0x139u)
    {
        const int cu = static_cast<int>(std::floor(texUf));
        const int cv = static_cast<int>(std::floor(texVf));
        int rsum = 0, gsum = 0, bsum = 0, asum = 0, cnt = 0;
        for (int dv = -s_g51MinFilt; dv <= s_g51MinFilt; ++dv)
            for (int du = -s_g51MinFilt; du <= s_g51MinFilt; ++du)
            {
                const uint32_t c = samplePoint(cu + du, cv + dv);
                rsum += static_cast<int>(c & 0xFFu);
                gsum += static_cast<int>((c >> 8) & 0xFFu);
                bsum += static_cast<int>((c >> 16) & 0xFFu);
                asum += static_cast<int>((c >> 24) & 0xFFu);
                ++cnt;
            }
        const uint32_t rr = static_cast<uint32_t>(rsum / cnt);
        const uint32_t gg = static_cast<uint32_t>(gsum / cnt);
        const uint32_t bb = static_cast<uint32_t>(bsum / cnt);
        const uint32_t aa = static_cast<uint32_t>(asum / cnt);
        return rr | (gg << 8) | (bb << 16) | (aa << 24);
    }

    if (!g45ForceLinear && !tex1UsesLinearFilter(ctx.tex1))
    {
        const uint32_t sampled = samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
        if (renderQualityTraceEnabled())
            f508LogSample(tex.psm, sampled);
        return sampled;
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    const uint32_t sampledLinear = static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
    if (renderQualityTraceEnabled())
        f508LogSample(tex.psm, sampledLinear);
    return sampledLinear;
}

void GSRasterizer::drawSprite(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();
    G248SpriteScope g248Scope(ctx.frame.fbp, gs->m_prim.tme, gs->m_prim.abe);
    // G248 state census (DEFAULT OFF): print each distinct RTT-sprite state once.  Kept separate
    // from DC2_G248_STAT because stderr I/O would distort the timing discriminator above.
    {
        static const bool s_g248State = (std::getenv("DC2_G248_STATE") != nullptr);
        if (s_g248State && g248TargetIndex(ctx.frame.fbp) >= 0)
        {
            char key[768];
            std::snprintf(
                key, sizeof(key),
                "fbp=0x%x fbw=%u fpsm=0x%x fbmsk=0x%x prim(tme=%u abe=%u fst=%u ctxt=%u) "
                "tex(tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x "
                "cpsm=0x%x csm=%u csa=%u cld=%u) tex1=0x%llx clamp=0x%llx alpha=0x%llx "
                "test=0x%llx zbuf=0x%llx texa(ta0=%u aem=%u ta1=%u) "
                "texclut(cbw=%u cou=%u cov=%u) "
                "pabe=%u fba=0x%llx",
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), ctx.frame.fbmsk,
                static_cast<uint32_t>(gs->m_prim.tme), static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.ctxt),
                ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.tbw),
                static_cast<uint32_t>(ctx.tex0.psm), static_cast<uint32_t>(ctx.tex0.tw),
                static_cast<uint32_t>(ctx.tex0.th), static_cast<uint32_t>(ctx.tex0.tcc),
                static_cast<uint32_t>(ctx.tex0.tfx), ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm), static_cast<uint32_t>(ctx.tex0.csm),
                static_cast<uint32_t>(ctx.tex0.csa), static_cast<uint32_t>(ctx.tex0.cld),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf),
                static_cast<uint32_t>(gs->m_texa.ta0),
                static_cast<uint32_t>(gs->m_texa.aem),
                static_cast<uint32_t>(gs->m_texa.ta1),
                static_cast<uint32_t>(gs->m_texclut.cbw),
                static_cast<uint32_t>(gs->m_texclut.cou),
                static_cast<uint32_t>(gs->m_texclut.cov),
                static_cast<uint32_t>(gs->m_pabe),
                static_cast<unsigned long long>(ctx.fba));
            static std::mutex s_mutex;
            static std::unordered_set<std::string> s_seen;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_seen.size() < 128u && s_seen.insert(key).second)
                std::fprintf(stderr, "[G248:rttstate] %s\n", key);
        }
    }

    // G219 (default-off, DC2_G219_SKYPX=1): sprite writer log at the two probe pixels inside the
    // zoom black rectangle (mirrors the drawTriangle skycol probe, so the full writer sequence at
    // one pixel can be reconstructed across both primitive paths).
    auto g219SpritePixelProbe = [&](const auto &pctx, GS *pgs, int px, int py,
                                    uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa, int texd) {
        static const bool s_g219sp = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (!s_g219sp)
            return;
        if (!((px == 470 && py == 30) || (px == 500 && py == 100)))
            return;
        if (pctx.frame.fbp != 0x0u && pctx.frame.fbp != 0x68u)
            return;
        static std::atomic<uint32_t> s_g219spn{0};
        const uint32_t n = s_g219spn.fetch_add(1u, std::memory_order_relaxed);
        if (n < 2000u)
            std::fprintf(stderr,
                "[G219:sprcol] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x abe=%u texd=%d "
                "rgba=(%u,%u,%u,%u) replay=%u\n",
                n, px, py, pctx.frame.fbp, static_cast<uint32_t>(pgs->m_prim.tme),
                pctx.tex0.tbp0, static_cast<uint32_t>(pgs->m_prim.abe), texd,
                pr, pg, pb, pa, t_g144InReplay ? 1u : 0u);
    };

    // G203: honor the guest's real ZBUF/TEST on SPRITES too. DC2 CLEARS its Z buffer every frame
    // via a full-screen ztst=ALWAYS,zmsk=0 sprite (seen in ttle/map_0/Inventory .gs freezes);
    // drawSprite previously ignored Z entirely, so that clear never reached VRAM and the triangle
    // path read stale Z — which is why prior phases used private title/costume Z buffers and the
    // town artificial clear. Reproducing the clear here lets universal-Z retire all of those.
    // Pure ztst=ALWAYS + zmsk (no-op: always passes, never writes) sprites skip the Z work.
    static const bool s_spriteZDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const bool sZuni = g203UniversalZEnabled();
    const uint32_t sZte  = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t sZtst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    const uint32_t sZbp  = static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5;
    const uint32_t sZpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool sZmsk     = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const uint32_t sZbw  = ctx.frame.fbw;
    const bool sZ16 = (sZpsm == 0x2u || sZpsm == 0xAu || sZpsm == 0xBu);
    const bool sZ24 = (sZpsm == 0x1u);
    const bool sZActive = sZuni && !s_spriteZDisabled && sZte != 0u && gs->m_vram != nullptr &&
                          !(sZtst == 1u && sZmsk); // ALWAYS + no-write == no-op, fast-skip
    uint32_t sZi = 0u;
    if (sZActive)
    {
        float zf = v1.z; // sprite Z is flat: the kick (2nd) vertex's z
        if (zf < 0.0f) zf = 0.0f;
        const float zmax = sZ16 ? 65535.0f : (sZ24 ? 16777215.0f : 4294967295.0f);
        if (zf > zmax) zf = zmax;
        sZi = static_cast<uint32_t>(zf);
    }
    const bool sZWrite = sZActive && !sZmsk && sZtst != 0u; // ZTST=NEVER never writes
    // G219 (default-off, DC2_G219_SKYPX=1): flag any sprite whose Z BUFFER lands in the
    // 0x139-0x140 work-page range — a deferred display sprite with such a zbp would zero the
    // work page during replay (the [G219:dif] clobber signature).
    {
        static const bool s_g219z = (std::getenv("DC2_G219_SKYPX") != nullptr);
        const uint32_t sZbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
        if (s_g219z && sZWrite && sZbpPage >= 0x139u && sZbpPage <= 0x148u)
        {
            static std::atomic<uint32_t> s_g219zn{0};
            const uint32_t n = s_g219zn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
                std::fprintf(stderr,
                    "[G219:spzbp] n=%u fbp=0x%x zbpPage=0x%x zpsm=0x%x ztst=%u zi=%u tme=%u "
                    "tbp=0x%x v=(%.0f,%.0f)-(%.0f,%.0f) replay=%u\n",
                    n, ctx.frame.fbp, sZbpPage, sZpsm, sZtst, sZi,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    v0.x, v0.y, v1.x, v1.y, t_g144InReplay ? 1u : 0u);
        }
    }
    auto spriteZPass = [&](int px, int py) -> bool {
        if (!sZActive) return true;
        const uint32_t zdst =
            sZ16 ? GSMem::ReadZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                 : (sZ24 ? GSMem::ReadZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                         : GSMem::ReadZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py)));
        switch (sZtst)
        {
        case 0:  return false;          // NEVER
        case 1:  return true;           // ALWAYS
        case 2:  return sZi >= zdst;    // GEQUAL
        default: return sZi >  zdst;    // GREATER
        }
    };
    auto spriteZStore = [&](int px, int py) {
        if (!sZWrite) return;
        if (sZ16)      GSMem::WriteZ16(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else if (sZ24) GSMem::WriteZ24(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
        else           GSMem::WriteZ32(gs->m_vram, sZbp, sZbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sZi);
    };

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
    {
        // maybe a log here idk ?
        return;
    }

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);
    // G173: during a tile-binning flush, restrict this sprite to the calling lane's scanline band,
    // exactly like drawTriangle's clamp — without this, every lane whose band intersects the bbox
    // replayed the FULL rect (cross-lane write races, alpha blend applied once per lane, submission
    // order broken across bands = the G172 costume prompt-box regression). Row UV interpolation is
    // anchored on unclippedY0, so clamping the row range is exact, not an approximation. If the
    // intersection is empty the loops run zero iterations.
    if (t_g144Banded)
    {
        if (drawY0 < t_g144BandY0) drawY0 = t_g144BandY0;
        if (drawY1 > t_g144BandY1) drawY1 = t_g144BandY1;
    }
    if (drawX1 >= drawX0 && drawY1 >= drawY0)
        g248Scope.setPixels(static_cast<uint64_t>(drawX1 - drawX0 + 1) *
                            static_cast<uint64_t>(drawY1 - drawY0 + 1));

    // G219 (default-off, DC2_G219_SKYPX=1): log any SPRITE touching Z inside the zoom black
    // rectangle (per-sprite, not per-pixel) — verifies the game's Z-clear sprite reaches
    // cols 417-511 / rows 0-140 and with what Z value.
    {
        static const bool s_g219s = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219s && (sZActive || sZWrite) &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            drawX1 >= 417 && drawY0 <= 140)
        {
            static std::atomic<uint32_t> s_g219sn{0};
            const uint32_t n = s_g219sn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u || (n % 1024u) == 0u)
                std::fprintf(stderr,
                    "[G219:spz] n=%u fbp=0x%x rect=(%d,%d)-(%d,%d) ztst=%u zmsk=%u zwr=%u "
                    "zi=%u tme=%u tbp=0x%x replay=%u banded=%u\n",
                    n, ctx.frame.fbp, drawX0, drawY0, drawX1, drawY1, sZtst,
                    static_cast<uint32_t>(sZmsk), static_cast<uint32_t>(sZWrite), sZi,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u);
        }
    }

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    const bool looksLikeDisplayCopy =
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        gs->m_prim.fst &&
        gs->m_prim.ctxt &&
        ctx.frame.fbp != ctx.tex0.tbp0 &&
        alphaMode == 0x64u &&
        (alphaFix == 0x60u || alphaFix == 0x80u) &&
        unclippedX0 <= 0 &&
        unclippedY0 <= 0 &&
        unclippedX1 >= 639 &&
        unclippedY1 >= 447;
    if (looksLikeDisplayCopy)
    {
        gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
        gs->m_hasPreferredDisplaySource = true;
    }

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    // G9: broad trace of every wide costume-screen sprite (textured or not) to
    // locate how the highlight bar is drawn and from which texture page.
    {
        static const bool g9b = envFlagEnabled("DC2_TRACE_G9");
        if (g9b && spanX >= 100 && spanY <= 60)
        {
            static std::atomic<uint32_t> s_g9b{0};
            const uint32_t nb = s_g9b.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (nb <= 20u || (nb % 600u) == 0u)
                std::fprintf(stderr,
                    "[G9:wide] n=%u tme=%u abe=%u tbp=0x%x psm=0x%x cbp=0x%x csa=%u "
                    "scr=(%d,%d)-(%d,%d) spanX=%d spanY=%d rgba=(%u,%u,%u,%u) alpha=0x%llx\n",
                    nb, gs->m_prim.tme, gs->m_prim.abe, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                    ctx.tex0.cbp, (unsigned)ctx.tex0.csa, x0, y0, x1, y1, spanX, spanY,
                    v1.r, v1.g, v1.b, v1.a, static_cast<unsigned long long>(ctx.alpha));
            // G11: one-shot decoded dump of the bar texture AT costume-draw time, using
            // the draw's own TEX0/CLUT, so we see whether 0x2aa0 holds the fukusel sheet
            // or stale title data.
            if (gs->m_prim.tme && (ctx.tex0.tbp0 == 0x2aa0u || ctx.tex0.tbp0 == 0x2720u))
            {
                static std::atomic<uint32_t> s_seen2aa0{0}, s_seen2720{0};
                std::atomic<uint32_t> *sp = (ctx.tex0.tbp0 == 0x2aa0u) ? &s_seen2aa0 : &s_seen2720;
                if (sp->fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    const auto &tx = ctx.tex0;
                    const int dw = 1 << tx.tw, dh = 1 << tx.th;
                    char path[128];
                    std::snprintf(path, sizeof(path), "captures/g11_bar_%x_cbp%x.ppm", tx.tbp0, tx.cbp);
                    FILE *fp = std::fopen(path, "wb");
                    if (fp)
                    {
                        std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                        for (int yy = 0; yy < dh; ++yy)
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tx.tbp0,
                                                 tx.tbw >> 1, (uint32_t)xx, (uint32_t)yy));
                                uint32_t c = lookupCLUT(gs, ix, tx.cbp, tx.cpsm, tx.csm, tx.csa, tx.psm);
                                uint8_t rgb[3] = { (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF) };
                                std::fwrite(rgb, 1, 3, fp);
                            }
                        std::fclose(fp);
                        std::fprintf(stderr, "[G11:bardump] wrote %s dw=%d dh=%d tbw=%u cbp=0x%x csa=%u\n",
                            path, dw, dh, tx.tbw, tx.cbp, (unsigned)tx.csa);
                    }
                }
            }
        }
    }

    if (gs->m_prim.tme)
    {
        const auto &tex = ctx.tex0;
        int texW = 1 << tex.tw;
        int texH = 1 << tex.th;
        if (texW == 0)
            texW = 1;
        if (texH == 0)
            texH = 1;

        // G9: env-gated trace of the costume-select highlight bar (wide T8 sprite
        // from the 0x2720 manager atlas). Logs full CLUT params + the sampled centre
        // texel/colour so the runner's brown-vs-dark can be A/B'd vs Select_costume.gs.
        {
            static const bool g9trace = envFlagEnabled("DC2_TRACE_G9");
            if (g9trace && (tex.tbp0 == 0x2720u || tex.tbp0 == 0x2aa0u))
            {
                const int sX = std::abs(static_cast<int>(v1.x) - static_cast<int>(v0.x)) >> 4;
                if (sX >= 150)
                {
                    static std::atomic<uint32_t> s_g9{0};
                    const uint32_t n = s_g9.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 24u)
                    {
                        const int cu = ((static_cast<int>(v0.u) + static_cast<int>(v1.u)) / 2) >> 4;
                        const int cv = ((static_cast<int>(v0.v) + static_cast<int>(v1.v)) / 2) >> 4;
                        uint32_t off = GSPSMT8::addrPSMT8(tex.tbp0, tex.tbw,
                            static_cast<uint32_t>(cu), static_cast<uint32_t>(cv));
                        uint8_t ix = (off < gs->m_vramSize) ? gs->m_vram[off] : 0u;
                        uint32_t col = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        std::fprintf(stderr,
                            "[G9:bar] n=%u tbp=0x%x tbw=%u cbp=0x%x cpsm=%u csm=%u csa=%u "
                            "scr=(%d,%d)-(%d,%d) uv0=(%d,%d) uv1=(%d,%d) ctr=(%d,%d) ix=0x%02x col=0x%08x "
                            "vrgba=(%u,%u,%u,%u) alpha=0x%llx\n",
                            n, tex.tbp0, tex.tbw, tex.cbp, (unsigned)tex.cpsm, (unsigned)tex.csm,
                            (unsigned)tex.csa, x0, y0, x1, y1,
                            static_cast<int>(v0.u) >> 4, static_cast<int>(v0.v) >> 4,
                            static_cast<int>(v1.u) >> 4, static_cast<int>(v1.v) >> 4,
                            cu, cv, ix, col, v1.r, v1.g, v1.b, v1.a,
                            static_cast<unsigned long long>(ctx.alpha));
                    }
                }
            }
        }

        // G8: env-gated one-shot trace of the 2D sprite sampling parameters
        // (filter mode, UV span vs screen span) to characterise font/HUD blur+dots.
        {
            static const bool g8trace = envFlagEnabled("DC2_TRACE_G8");
            if (g8trace)
            {
                static std::atomic<uint32_t> s_g8{0};
                const uint32_t n = s_g8.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 48u)
                {
                    const bool lin = tex1UsesLinearFilter(ctx.tex1);
                    const int uvu = (static_cast<int>(v1.u) - static_cast<int>(v0.u));
                    const int uvv = (static_cast<int>(v1.v) - static_cast<int>(v0.v));
                    std::fprintf(stderr,
                        "[G8:sprite] n=%u tbp=0x%x psm=0x%x tw=%d th=%d tex1=0x%llx lin=%d tfx=%u tcc=%u "
                        "scr=(%d,%d)-(%d,%d) spanX=%d spanY=%d uv0=(%d,%d) uv1=(%d,%d) duv16=(%d,%d) wrap(s=%u,t=%u)\n",
                        n, tex.tbp0, static_cast<uint32_t>(tex.psm), texW, texH,
                        static_cast<unsigned long long>(ctx.tex1), lin ? 1 : 0,
                        static_cast<uint32_t>(tex.tfx), static_cast<uint32_t>(tex.tcc),
                        x0, y0, x1, y1, spanX, spanY,
                        static_cast<int>(v0.u), static_cast<int>(v0.v),
                        static_cast<int>(v1.u), static_cast<int>(v1.v),
                        uvu, uvv,
                        static_cast<uint32_t>(ctx.clamp & 0x3u),
                        static_cast<uint32_t>((ctx.clamp >> 2) & 0x3u));
                }
            }
        }

        float u0f, v0f, u1f, v1f;
        if (gs->m_prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // G9: decisive one-shot diagnostic for the costume-screen black UI boxes
        // (textured T8 sprites from tbp=0x2720, CLUT cbp=0x3fdc rendering black).
        // Distinguishes "atlas region empty" from "CLUT empty" from "center index/color".
        {
            static const bool g9 = envFlagEnabled("DC2_TRACE_COSTUME");
            if (g9 && gs->m_prim.tme && tex.tbp0 == 0x2720u && tex.psm == GS_PSM_T8)
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 40u)
                {
                    const int cu = static_cast<int>((u0f + u1f) * 0.5f);
                    const int cv = static_cast<int>((v0f + v1f) * 0.5f);
                    const uint32_t centerColor =
                        sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                      static_cast<uint16_t>(clampInt(cu * 16, 0, 0xFFFF)),
                                      static_cast<uint16_t>(clampInt(cv * 16, 0, 0xFFFF)));
                    // CLUT @ cbp first 4 CT32 entries (raw VRAM bytes).
                    const uint32_t clutByteOff = tex.cbp * 256u;
                    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                    if (clutByteOff + 16u <= gs->m_vramSize)
                    {
                        std::memcpy(&c0, gs->m_vram + clutByteOff + 0, 4);
                        std::memcpy(&c1, gs->m_vram + clutByteOff + 4, 4);
                        std::memcpy(&c2, gs->m_vram + clutByteOff + 8, 4);
                        std::memcpy(&c3, gs->m_vram + clutByteOff + 12, 4);
                    }
                    // Nonzero byte count in the 0x2720 atlas page (0x272000..0x282000).
                    uint32_t atlasNz = 0;
                    for (uint32_t o = 0x272000u; o < 0x282000u && o < gs->m_vramSize; ++o)
                        if (gs->m_vram[o]) ++atlasNz;
                    std::fprintf(stderr,
                        "[G9:costume] n=%u scr=(%d,%d)-(%d,%d) cu=%d cv=%d cbp=0x%x csa=%u "
                        "centerColor=0x%08x clut[0..3]=%08x,%08x,%08x,%08x atlas0x2720nz=%u tbw=%u\n",
                        n, x0, y0, x1, y1, cu, cv, tex.cbp, static_cast<uint32_t>(tex.csa),
                        centerColor, c0, c1, c2, c3, atlasNz, static_cast<uint32_t>(tex.tbw));
                }
            }
        }

        // G8: per-pixel sampling bias for axis-aligned (FST) sprites. The PS2 GS
        // truncates the interpolated texel coordinate; sampling at the pixel CENTER
        // (+0.5) on a minified font (e.g. 9 source texels mapped to 8 pixels) drops a
        // MIDDLE texel column (breaks glyph strokes) and keeps the trailing inter-glyph
        // gap texel (adds speckle). Left-edge sampling (bias 0.0) instead drops only the
        // trailing gap texel, matching the crisp reference. Override via DC2_GS_UV_BIAS.
        static const float s_uvBias = []() {
            const char *e = std::getenv("DC2_GS_UV_BIAS");
            return (e && *e) ? static_cast<float>(std::atof(e)) : 0.0f;
        }();

        // G248/G250 (PERF, DEFAULT ON): the character/effect RTT family is dominated by
        // textured sprite sampling (~138 ms/guest-frame on map_0).  sampleTexture() re-decodes
        // invariant GS texture state for every pixel and, for the observed PSMT8 linear sprites,
        // repeats four CLUT decodes per pixel.  Hoist the exact point/bilinear sampler used by
        // sampleTexture(), add a per-draw CLUT cache, and reuse an identical bilinear texel quad.
        // Scope is deliberately narrow: only the measured RTT targets, FST sprites, CT32/CT24/T8,
        // default T8 swizzle, no quality/phase traces, and no G45/G51 costume filter override.
        // Unsupported or diagnostic states retain the existing sampleTexture() path.  G250
        // promoted the path after the cross-route soak; DC2_G248_NO_FASTSPRITE=1 is the rollback.
        // DC2_G248_FASTSPRITE remains accepted as a compatibility no-op for older harnesses.
        // Prove per-pixel parity with DC2_G248_VERIFY=1 (bad must remain 0).
        static const bool s_g248FastSprite =
            (std::getenv("DC2_G248_NO_FASTSPRITE") == nullptr);
        static const bool s_g248Verify = (std::getenv("DC2_G248_VERIFY") != nullptr);
        static const bool s_g248NoClutCache = (std::getenv("DC2_G248_NO_CLUTCACHE") != nullptr);
        static const bool s_g248NoTexQuad = (std::getenv("DC2_G248_NO_TEXQUAD") != nullptr);
        static const bool s_g248T8GsmemDefault =
            !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
        static const bool s_g248T8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);
        const uint8_t g248Psm = tex.psm;
        const bool g248FastPsm =
            g248Psm == GS_PSM_CT32 || g248Psm == GS_PSM_CT24 ||
            (g248Psm == GS_PSM_T8 && s_g248T8GsmemDefault && !s_g248T8AliasSet);
        // G268: these four were raw getenv() per sprite draw (and per banded replay lane).
        static const bool s_g248EnvTraceOff = std::getenv("DC2_DUMP_FONT") == nullptr &&
                                              std::getenv("DC2_DUMP_VRAMRGN") == nullptr;
        static const bool s_g248EnvFilterOff = std::getenv("DC2_G45_BILINEAR") == nullptr &&
                                               std::getenv("DC2_G51_MINFILT") == nullptr;
        const bool g248TraceOff =
            !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() &&
            !f50_12_trace_enabled() &&
            (g268LiveEnvRead()
                 ? (std::getenv("DC2_DUMP_FONT") == nullptr &&
                    std::getenv("DC2_DUMP_VRAMRGN") == nullptr)
                 : s_g248EnvTraceOff);
        const bool g248SpecialFilterOff =
            g268LiveEnvRead()
                ? (std::getenv("DC2_G45_BILINEAR") == nullptr &&
                   std::getenv("DC2_G51_MINFILT") == nullptr)
                : s_g248EnvFilterOff;
        const bool g248FastSampleSprite =
            s_g248FastSprite && g248TargetIndex(ctx.frame.fbp) >= 0 && gs->m_prim.fst &&
            g248FastPsm && tex.tbw != 0u && g248TraceOff && g248SpecialFilterOff;
        const uint8_t g248WrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
        const uint8_t g248WrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
        const int g248MinU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
        const int g248MaxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
        const int g248MinV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
        const int g248MaxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
        const bool g248Linear = tex1UsesLinearFilter(ctx.tex1);
        const bool g248UseClutCache =
            g248FastSampleSprite && g248Psm == GS_PSM_T8 && !s_g248NoClutCache;
        // Thread-local avoids growing/clearing every drawSprite stack frame while the prototype is
        // disabled.  Each eligible paletted draw explicitly invalidates its own entries below.
        static thread_local uint32_t g248Clut[256];
        static thread_local uint8_t g248ClutValid[256];
        if (g248UseClutCache)
            std::memset(g248ClutValid, 0, sizeof(g248ClutValid));
        auto g248ClutLookup = [&](uint8_t idx) -> uint32_t {
            if (!g248ClutValid[idx])
            {
                g248Clut[idx] = lookupCLUT(gs, idx, tex.cbp, tex.cpsm,
                                           tex.csm, tex.csa, tex.psm);
                g248ClutValid[idx] = 1u;
            }
            return g248Clut[idx];
        };
        auto g248SamplePoint = [&](int su0, int sv0) -> uint32_t {
            const int su = wrapTextureCoordinate(su0, g248WrapU, texW, g248MinU, g248MaxU);
            const int sv = wrapTextureCoordinate(sv0, g248WrapV, texH, g248MinV, g248MaxV);
            if (g248Psm == GS_PSM_CT32 || g248Psm == GS_PSM_CT24)
                return applyTexa(gs->m_texa, g248Psm,
                                 readTexelPSMCT32(gs, tex.tbp0, tex.tbw, su, sv));
            const uint32_t pageBwT8 = ((tex.tbw >> 1u) != 0u) ? (tex.tbw >> 1u) : 1u;
            const uint8_t idx = static_cast<uint8_t>(
                GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                              static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            return g248UseClutCache
                ? g248ClutLookup(idx)
                : lookupCLUT(gs, idx, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        };
        bool g248QValid = false;
        int g248QU0 = 0, g248QV0 = 0;
        uint32_t g248QC00 = 0, g248QC10 = 0, g248QC01 = 0, g248QC11 = 0;
        auto g248FastSample = [&](uint16_t u, uint16_t v) -> uint32_t {
            const float texUf = static_cast<float>(u) / 16.0f;
            const float texVf = static_cast<float>(v) / 16.0f;
            if (!g248Linear)
                return g248SamplePoint(static_cast<int>(texUf), static_cast<int>(texVf));

            const float sampleU = texUf - 0.5f;
            const float sampleV = texVf - 0.5f;
            const int u0 = static_cast<int>(std::floor(sampleU));
            const int v0 = static_cast<int>(std::floor(sampleV));
            const int u1 = u0 + 1;
            const int v1 = v0 + 1;
            const float fx = sampleU - static_cast<float>(u0);
            const float fy = sampleV - static_cast<float>(v0);
            uint32_t c00, c10, c01, c11;
            if (!s_g248NoTexQuad && g248QValid && u0 == g248QU0 && v0 == g248QV0)
            {
                c00 = g248QC00; c10 = g248QC10; c01 = g248QC01; c11 = g248QC11;
            }
            else
            {
                c00 = g248SamplePoint(u0, v0);
                c10 = g248SamplePoint(u1, v0);
                c01 = g248SamplePoint(u0, v1);
                c11 = g248SamplePoint(u1, v1);
                g248QValid = true; g248QU0 = u0; g248QV0 = v0;
                g248QC00 = c00; g248QC10 = c10; g248QC01 = c01; g248QC11 = c11;
            }
            const uint8_t rr = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                            static_cast<uint8_t>(c10 & 0xFFu),
                                            static_cast<uint8_t>(c01 & 0xFFu),
                                            static_cast<uint8_t>(c11 & 0xFFu), fx, fy);
            const uint8_t gg = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 8) & 0xFFu), fx, fy);
            const uint8_t bb = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 16) & 0xFFu), fx, fy);
            const uint8_t aa = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                            static_cast<uint8_t>((c11 >> 24) & 0xFFu), fx, fy);
            return static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8) |
                   (static_cast<uint32_t>(bb) << 16) | (static_cast<uint32_t>(aa) << 24);
        };

        for (int y = drawY0; y <= drawY1; ++y)
        {
            g248QValid = false;
            float ty = (static_cast<float>(y - unclippedY0) + s_uvBias) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + s_uvBias) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                uint16_t g258SampleU = 0u, g258SampleV = 0u;
                if (gs->m_prim.fst)
                {
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texUf * 16.0f)), 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texVf * 16.0f)), 0, 0xFFFF));
                    g258SampleU = sampleU;
                    g258SampleV = sampleV;
                    if (g248FastSampleSprite)
                    {
                        texel = g248FastSample(sampleU, sampleV);
                        if (s_g248Verify)
                        {
                            const uint32_t ref = sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                                               sampleU, sampleV);
                            static std::atomic<uint64_t> s_g248VerifyTotal{0}, s_g248VerifyBad{0};
                            const uint64_t total =
                                s_g248VerifyTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (ref != texel)
                            {
                                const uint64_t bad =
                                    s_g248VerifyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (bad <= 16u)
                                    std::fprintf(stderr,
                                        "[G248:vfy] MISMATCH ref=%08x fast=%08x fbp=0x%x "
                                        "psm=0x%x uv=(%u,%u) linear=%u\n",
                                        ref, texel, ctx.frame.fbp, static_cast<uint32_t>(g248Psm),
                                        static_cast<uint32_t>(sampleU),
                                        static_cast<uint32_t>(sampleV),
                                        static_cast<uint32_t>(g248Linear));
                                texel = ref;
                            }
                            if ((total & 0xFFFFFull) == 0ull)
                                std::fprintf(stderr, "[G248:vfy] tot=%llu bad=%llu\n",
                                    static_cast<unsigned long long>(total),
                                    static_cast<unsigned long long>(
                                        s_g248VerifyBad.load(std::memory_order_relaxed)));
                        }
                    }
                    else
                    {
                        texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                    }
                }
                else
                {
                    texel = sampleTexture(gs,
                                          texUf / static_cast<float>(texW),
                                          texVf / static_cast<float>(texH),
                                          1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                if (g258TraceOn() && g_g255Verify.pending && g_g255Verify.fbp == 0x146u &&
                    g_g255Verify.traceBatch <= 4u && x == 52 && y == 1 && gs->m_prim.fst)
                {
                    const float su = static_cast<float>(g258SampleU) / 16.0f - 0.5f;
                    const float sv = static_cast<float>(g258SampleV) / 16.0f - 0.5f;
                    const int pu = static_cast<int>(std::floor(su));
                    const int pv = static_cast<int>(std::floor(sv));
                    const int fru = static_cast<int>(std::lround((su - static_cast<float>(pu)) * 16.0f));
                    const int frv = static_cast<int>(std::lround((sv - static_cast<float>(pv)) * 16.0f));
                    const uint32_t c00 = g248SamplePoint(pu, pv);
                    const uint32_t c10 = g248SamplePoint(pu + 1, pv);
                    const uint32_t c01 = g248SamplePoint(pu, pv + 1);
                    const uint32_t c11 = g248SamplePoint(pu + 1, pv + 1);
                    std::fprintf(stderr,
                        "[G258:cpu.sample] batch=%llu entry=%zu uv16=(%u,%u) p0=(%d,%d) fr=(%d,%d) "
                        "taps=%08x/%08x/%08x/%08x sample=%02x%02x%02x%02x "
                        "vc=%02x%02x%02x%02x src=%02x%02x%02x%02x\n",
                        static_cast<unsigned long long>(g_g255Verify.traceBatch), t_g258EntryIndex,
                        static_cast<uint32_t>(g258SampleU), static_cast<uint32_t>(g258SampleV),
                        pu, pv, fru, frv, c00, c10, c01, c11,
                        tr, tg, tb, ta, r, g, b, a, color.r, color.g, color.b, color.a);
                }
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f31TexSampleCount{0};
                    if (s_f31TexSampleCount.load(std::memory_order_relaxed) < 32u)
                    {
                        const uint32_t n = s_f31TexSampleCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 32u)
                        {
                            const uint64_t tex0Raw =
                                static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) |
                                (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 14) |
                                (static_cast<uint64_t>(tex.psm & 0x3Fu) << 20) |
                                (static_cast<uint64_t>(tex.tw & 0xFu) << 26) |
                                (static_cast<uint64_t>(tex.th & 0xFu) << 30) |
                                (static_cast<uint64_t>(tex.tcc & 0x1u) << 34) |
                                (static_cast<uint64_t>(tex.tfx & 0x3u) << 35) |
                                (static_cast<uint64_t>(tex.cbp & 0x3FFFu) << 37) |
                                (static_cast<uint64_t>(tex.cpsm & 0xFu) << 51) |
                                (static_cast<uint64_t>(tex.csm & 0x1u) << 55) |
                                (static_cast<uint64_t>(tex.csa & 0x1Fu) << 56) |
                                (static_cast<uint64_t>(tex.cld & 0x7u) << 61);
                            std::fprintf(stderr,
                                         "[F31:texsample] n=%u xy=(%d,%d) tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u cbp=0x%x texel=%02x,%02x,%02x,%02x vtx=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x\n",
                                         n, x, y, static_cast<unsigned long long>(tex0Raw),
                                         tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                         static_cast<uint32_t>(tex.psm),
                                         static_cast<uint32_t>(tex.tfx),
                                         static_cast<uint32_t>(tex.tcc),
                                         tex.cbp,
                                         tr, tg, tb, ta,
                                         r, g, b, a,
                                         color.r, color.g, color.b, color.a);
                        }
                    }
                }
                if (!spriteZPass(x, y)) // G203: guest Z test
                    continue;
                const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, color.a);
                g219SpritePixelProbe(ctx, gs, x, y, color.r, color.g, color.b, color.a, 1);
                if (alphaTest.writeFramebuffer)
                    writePixel(gs, x, y, color.r, color.g, color.b, color.a);
                if (alphaTest.writeDepth)
                    spriteZStore(x, y); // G203: guest Z write (e.g. the game's clear-sprite)
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
            {
                if (!spriteZPass(x, y)) // G203: guest Z test
                    continue;
                const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);
                g219SpritePixelProbe(ctx, gs, x, y, r, g, b, a, 0);
                if (alphaTest.writeFramebuffer)
                    writePixel(gs, x, y, r, g, b, a);
                if (alphaTest.writeDepth)
                    spriteZStore(x, y); // G203: guest Z write (e.g. the game's clear-sprite)
            }
    }
}

// G144: drain any trailing deferred triangles at frame end (called from the mgEndFrame override on
// the GUEST thread, before the frame is presented) so tris submitted after the last non-deferrable
// primitive still reach VRAM this frame. No-op when tile-binning is off or the list is empty. Same
// thread as capture ⇒ no data race on g_g144List.
static void g178CensusScan();

void g144FlushPending()
{
    g248Report();
    // G219 (default-off, DC2_G219_SKYPX=1): log the frame-end drain cadence + pending size so
    // "how long do deferred entries live before the trailing flush" is directly visible.
    {
        static const bool s_g219fl = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219fl)
        {
            static std::atomic<uint32_t> s_g219fln{0};
            const uint32_t n = s_g219fln.fetch_add(1u, std::memory_order_relaxed);
            if (n < 200u || (n % 64u) == 0u || !g_g144List.empty())
                std::fprintf(stderr, "[G219:framefl] n=%u list=%zu\n", n, g_g144List.size());
        }
    }
    // G148 sampler-stat dump (DC2_G148_SAMPLESTAT=1). Fires once per guest frame (this runs from the
    // mgEndFrame override). Prints cumulative + 30-frame-window deltas so we can read the steady-state
    // quad hit/miss ratio and leaf-read volume that decide whether a de-swizzle cache is worthwhile.
    {
        static const bool s_g148Stat = (std::getenv("DC2_G148_SAMPLESTAT") != nullptr);
        if (s_g148Stat)
        {
            static uint64_t s_frame = 0;
            static uint64_t s_lastSamp = 0, s_lastHit = 0, s_lastMiss = 0, s_lastPoint = 0, s_lastLeaf = 0;
            if ((++s_frame % 30ull) == 0ull)
            {
                const uint64_t samp = g_g148TexSamples.load(std::memory_order_relaxed);
                const uint64_t hit = g_g148QuadHit.load(std::memory_order_relaxed);
                const uint64_t miss = g_g148QuadMiss.load(std::memory_order_relaxed);
                const uint64_t point = g_g148PointSamp.load(std::memory_order_relaxed);
                const uint64_t leaf = g_g148LeafReads.load(std::memory_order_relaxed);
                const uint64_t dSamp = samp - s_lastSamp;
                const uint64_t dHit = hit - s_lastHit;
                const uint64_t dMiss = miss - s_lastMiss;
                const uint64_t dPoint = point - s_lastPoint;
                const uint64_t dLeaf = leaf - s_lastLeaf;
                const uint64_t bilinear = dHit + dMiss;
                std::fprintf(stderr,
                    "[G148:samplestat] frame=%llu win=30 | samples/f=%llu bilinear/f=%llu point/f=%llu "
                    "quadHit=%.1f%% leafReads/f=%llu leafPerSample=%.2f\n",
                    (unsigned long long)s_frame,
                    (unsigned long long)(dSamp / 30ull),
                    (unsigned long long)(bilinear / 30ull),
                    (unsigned long long)(dPoint / 30ull),
                    bilinear ? 100.0 * (double)dHit / (double)bilinear : 0.0,
                    (unsigned long long)(dLeaf / 30ull),
                    dSamp ? (double)dLeaf / (double)dSamp : 0.0);
                s_lastSamp = samp; s_lastHit = hit; s_lastMiss = miss; s_lastPoint = point; s_lastLeaf = leaf;
            }
        }
    }
    // G149 cache stats (DC2_G149_TEXCACHE=1): builds/hits/ineligible per 30-frame window. A high
    // hit:build ratio means the decode is amortized across many triangles/bands (the intended win);
    // builds ≈ hits means thrash (VRAM gen churning) — check the upload cadence.
    {
        static const bool s_g149Stat = (std::getenv("DC2_G149_TEXCACHE") != nullptr);
        if (s_g149Stat)
        {
            static uint64_t s_frame = 0;
            static uint64_t s_lastHit = 0, s_lastBuild = 0, s_lastInel = 0, s_lastGpu = 0, s_lastGpuFb = 0;
            if ((++s_frame % 30ull) == 0ull)
            {
                const uint64_t hit = g_g149Hits.load(std::memory_order_relaxed);
                const uint64_t build = g_g149Builds.load(std::memory_order_relaxed);
                const uint64_t inel = g_g149Ineligible.load(std::memory_order_relaxed);
                const uint64_t gpu = g_g162GpuHits.load(std::memory_order_relaxed);
                const uint64_t gpuFb = g_g162GpuFallback.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "[G149:texcache] frame=%llu win=30 | hits/f=%llu builds/f=%llu ineligible/f=%llu gen=%llu | g162gpu/f=%llu g162fallback/f=%llu\n",
                    (unsigned long long)s_frame,
                    (unsigned long long)((hit - s_lastHit) / 30ull),
                    (unsigned long long)((build - s_lastBuild) / 30ull),
                    (unsigned long long)((inel - s_lastInel) / 30ull),
                    (unsigned long long)g_g149VramGen.load(std::memory_order_relaxed),
                    (unsigned long long)((gpu - s_lastGpu) / 30ull),
                    (unsigned long long)((gpuFb - s_lastGpuFb) / 30ull));
                s_lastHit = hit; s_lastBuild = build; s_lastInel = inel; s_lastGpu = gpu; s_lastGpuFb = gpuFb;
            }
        }
    }
    g178CensusScan();
    // G260: the frame boundary is the natural full-graph execution point. Under the default
    // MTGS+pipeline config this function runs INSIDE the worker's frame-boundary marker closure
    // (see g150_frame_barrier), i.e. on the same thread that runs the mid-frame band-pool
    // flushes today — the parallel closure is proven there. In the non-pipelined/serial
    // configs it runs on the EE thread at a point where the band pool is NOT proven (the
    // G144-era present-thread race), so fall back to the sequential closure exactly like the
    // legacy tail drain below.
    if (g260NrOn() && g260HasPending())
    {
        extern bool g150_pipeline_enabled();
        G146PerfScope g146FlushScope(g_g146G144FlushFrameNs, g_g146G144FlushFrameCount);
        g260ExecuteAll(kG260Boundary, g150_pipeline_enabled());
    }
    // Sequential (no pool) — safe to call from the mgEndFrame override to drain the trailing tail.
    if (g_g144FlushSeqFn && !g_g144List.empty())
    {
        G146PerfScope g146FlushScope(g_g146G144FlushFrameNs, g_g146G144FlushFrameCount);
        g_g144FlushSeqFn();
    }
    // Conservative phase-1 ownership boundary: frame presentation still sees color readback as
    // before, and guest VRAM leaves mgEndFrame with real Z materialized.  Once route canaries prove
    // every downstream reader has an explicit barrier this can be profiled as a removable fence.
    if (g_g144LastVram)
    {
        // Normal downstream presentation and dumps read guest VRAM. This fence is the display
        // color ownership boundary: at most one readback per dirty display target per frame.
        (void)g278FlushPendingDepth(5); // never cross presentation/frame ownership
        // G289's measured 0x14a work owner is not itself a display surface. Let it cross this
        // boundary only in the exact post-upload state; copyFrameToHostRgbaUnlocked audits every
        // actual display/preferred/fallback source before reading. Raw-VRAM diagnostics retain the
        // old all-authoritative boundary.
        const bool g289FrameCarry =
            s_g289Owner.active && s_g289Owner.dstFbp == 0x14au &&
            s_g289Owner.firstOwnedBlock >= 0x2b20u &&
            s_g289Owner.firstOwnedBlock <= s_g289Owner.lastOwnedBlock &&
            s_g289Owner.lastOwnedBlock == 0x363fu &&
            !g289RawVramDiagOn();
        if (g289FrameCarry)
            g_g289FrameCarries.fetch_add(1u, std::memory_order_relaxed);
        else
            (void)g289MaterializeOwner(5);
        g272MaterializeAll(5);
        g277CensusHardEdge(5);
        g276FlushPendingDisplay(5); // G276: publish the frame's coalesced display readback here
        g242PrepareVramReadAll(g_g144LastVram);
    }
}

// G178 census (DEFAULT OFF, DC2_G178_CENSUS=1): distinct GS render-state keys in the deferred
// list, scanned at EVERY drain point (frame-end AND mid-frame upload flushes — whichever path
// actually drains the list on the live config), printed every ~5s. MEASURE-first (the G161
// lesson): decides exactly which blend equations / texture PSMs / test modes the G178 GPU
// backend's shader+GL-state mapping must implement for the live workload. Diagnostic only.
// Called only from the threads that own g_g144List at a drain point, so no locking needed.
static void g178CensusScan()
{
    static const bool s_g178Census = (std::getenv("DC2_G178_CENSUS") != nullptr);
    if (s_g178Census && !g_g144List.empty())
    {
        static std::map<std::string, uint64_t> s_combo;
        static std::mutex s_comboMx; // frame-end (EE) and upload flush (worker) can interleave
        {
            std::lock_guard<std::mutex> lk(s_comboMx);
            for (const auto &e : g_g144List)
            {
                const uint64_t al = e.ctx.alpha, ts = e.ctx.test, zb = e.ctx.zbuf,
                               t1 = e.ctx.tex1, cl = e.ctx.clamp;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    "prim=%d iip=%u tme=%u fge=%u abe=%u fst=%u pabe=%u | fbp=0x%x fpsm=%u fbmsk=0x%x | "
                    "tex psm=%u cpsm=%u tfx=%u tcc=%u tw=%u th=%u | alpha A=%llu B=%llu C=%llu D=%llu FIX=%llu | "
                    "test ATE=%llu ATST=%llu AREF=%llu AFAIL=%llu DATE=%llu ZTE=%llu ZTST=%llu | zpsm=%llu zmsk=%llu | "
                    "wms=%llu wmt=%llu mmag=%llu mmin=%llu fba=%llu",
                    (int)e.prim.type, e.prim.iip ? 1u : 0u, e.prim.tme ? 1u : 0u, e.prim.fge ? 1u : 0u,
                    e.prim.abe ? 1u : 0u, e.prim.fst ? 1u : 0u, e.pabe ? 1u : 0u,
                    e.ctx.frame.fbp, (unsigned)e.ctx.frame.psm, e.ctx.frame.fbmsk,
                    (unsigned)e.ctx.tex0.psm, (unsigned)e.ctx.tex0.cpsm, (unsigned)e.ctx.tex0.tfx,
                    (unsigned)e.ctx.tex0.tcc, (unsigned)e.ctx.tex0.tw, (unsigned)e.ctx.tex0.th,
                    (unsigned long long)(al & 3u), (unsigned long long)((al >> 2) & 3u),
                    (unsigned long long)((al >> 4) & 3u), (unsigned long long)((al >> 6) & 3u),
                    (unsigned long long)((al >> 32) & 0xffu),
                    (unsigned long long)(ts & 1u), (unsigned long long)((ts >> 1) & 7u),
                    (unsigned long long)((ts >> 4) & 0xffu),
                    (unsigned long long)((ts >> 12) & 3u), (unsigned long long)((ts >> 14) & 1u),
                    (unsigned long long)((ts >> 16) & 1u), (unsigned long long)((ts >> 17) & 3u),
                    (unsigned long long)((zb >> 24) & 0xfu), (unsigned long long)((zb >> 32) & 1u),
                    (unsigned long long)(cl & 3u), (unsigned long long)((cl >> 2) & 3u),
                    (unsigned long long)((t1 >> 5) & 1u), (unsigned long long)((t1 >> 6) & 7u),
                    (unsigned long long)(e.ctx.fba & 1u));
                s_combo[buf]++;
            }
            static auto s_lastPrint = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastPrint > std::chrono::seconds(5))
            {
                s_lastPrint = now;
                std::fprintf(stderr, "[G178:census] distinct=%zu\n", s_combo.size());
                for (const auto &kv : s_combo)
                    std::fprintf(stderr, "[G178:census] %8llu | %s\n",
                                 (unsigned long long)kv.second, kv.first.c_str());
            }
        }
    }
}

// PARALLEL flush for the texture-upload path (called during active guest rendering, not from the
// mgEndFrame context that races the present thread). Keeps the batch parallelism instead of forcing
// a slow single-threaded drain on every mid-frame texture upload. DC2_G144_SEQUPLOAD forces the
// sequential path (A/B / fallback if the parallel upload flush proves unsafe).
// G260: external-linkage bridge for ps2_gs_gpu.cpp — the gate itself (g260NrOn) lives inside
// this file's anonymous namespace, and the transfer hooks in ps2_gs_gpu.cpp need the answer to
// pick the range-tested barrier path.
bool g260NrEnabled()
{
    return g260NrOn();
}

void g144FlushPendingUpload()
{
    // G260: callers of this no-range entry point (local→host reads, any path that did not
    // range-test) get the conservative treatment — execute the entire pending graph.
    if (g260NrOn())
    {
        if (g260HasPending())
        {
            g178CensusScan();
            G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
            static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
            g260ExecuteAll(kG260LocalHost, !s_seqUpload260);
        }
        // G261: an un-range-tested VRAM read/write follows — every GPU-resident RTT row must be
        // in guest VRAM first (conservative, like the execute above).
        (void)g278FlushPendingDepth(6);
        (void)g289MaterializeOwner(6);
        g272MaterializeAll(6);
        g277CensusHardEdge(6);
        g276FlushPendingDisplay(6); // G276: publish deferred display readbacks first
        g261MaterializeAllDirty(kG261MatL2H);
        return;
    }
    if (g_g144List.empty())
        return;
    g178CensusScan();
    G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
    static const bool s_seqUpload = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
    if (s_seqUpload)
    {
        if (g_g144FlushSeqFn)
            g_g144FlushSeqFn();
    }
    else if (g_g144FlushFn)
    {
        g_g144FlushFn();
    }
}

// G145 (experimental, opt-in DC2_G145_DIRTY_UPLOAD=1): dirty-region upload flush gate. G144 flushed
// before every upload/local-copy because any VRAM write can invalidate deferred texture sampling.
// This keeps that behavior on uncertainty, but skips flushes whose destination does not overlap any
// pending texture/CLUT/target range. For local-to-local copies, a source overlap with the pending
// render target also flushes so the copy reads already-rasterized pixels.
void g144FlushPendingUploadRange(uint32_t dbp,
                                 uint8_t dbw,
                                 uint8_t dpsm,
                                 uint32_t dsax,
                                 uint32_t dsay,
                                 uint32_t rrw,
                                 uint32_t rrh)
{
    // G260: a host→local upload is a WAR/WAW edge only when its destination intersects a
    // pending batch's read set (texture/CLUT the recorded draws will sample at execution) or
    // write set (target/Z alias). Real edges execute the whole graph in order; independent
    // uploads record through with no synchronization. Unresolvable ranges fail closed.
    if (g260NrOn())
    {
        const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
        const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
        const bool g289UploadOverwrite =
            !dstUnknown &&
            g289PrepareUploadOverwrite(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, dst);
        if (g289StatOn() && s_g289Owner.active && s_g289Owner.dstFbp == 0x14au)
        {
            const G144BlockRange owner = g289OwnerRange();
            const bool overlap =
                dstUnknown || !owner.valid || (dst.valid && g144RangeOverlaps(owner, dst));
            if (overlap)
            {
                static std::atomic<uint32_t> s_g289UploadEdgeLogs{0};
                const uint32_t n =
                    s_g289UploadEdgeLogs.fetch_add(1u, std::memory_order_relaxed);
                if (n < 32u)
                    std::fprintf(
                        stderr,
                        "[G289:upload-edge] n=%u owner(dst=%03x range=%x..%x valid=%u) "
                        "upload(dbp=%x dbw=%u psm=%x xy=%u,%u size=%ux%u "
                        "range=%x..%x valid=%u unknown=%u)\n",
                        n + 1u, s_g289Owner.dstFbp, owner.first, owner.last,
                        owner.valid ? 1u : 0u, dbp, static_cast<unsigned>(dbw), dpsm,
                        dsax, dsay, rrw, rrh, dst.first, dst.last,
                        dst.valid ? 1u : 0u, dstUnknown ? 1u : 0u);
            }
        }
        if (!g289UploadOverwrite)
            g289MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, 7);
        if (g260HasPending())
        {
            if (g260VramEventNeedsExecute(G144BlockRange{}, dst, false, dstUnknown))
            {
                g178CensusScan();
                G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
                static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
                if (g271PrefixBarrierOn())
                    g271ExecuteConflictPrefix(G144BlockRange{}, dst, false, dstUnknown,
                                              kG260Upload, !s_seqUpload260);
                else
                    g260ExecuteAll(kG260Upload, !s_seqUpload260);
            }
            else
            {
                ++g_g260UploadSkips;
            }
        }
        // G261: the caller writes `dst` into guest VRAM next — if it lands on (or the range is
        // unresolvable near) a GPU-resident RTT row, materialize first so the upload composes
        // over current bytes and the residency record cannot go stale-partial.
        // G264: eligible rects are deferred to a VRAM→FBO mirror at the next flush edge instead
        // (the mask suppresses only the exact targets that accepted the deferral; fail-closed).
        const uint32_t g264Mask = dstUnknown
            ? 0u
            : g264NoteUploadForMirror(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
        (void)g278FlushPendingDepthForRanges(
            7, G144BlockRange{}, dst, false, dstUnknown);
        g261MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, kG261MatUpload,
                                 g264Mask);
        (void)g278FlushPendingDepthForDisplayOwners(7);
        g272MaterializeForRanges(G144BlockRange{}, dst, false, dstUnknown, 7);
        g277CensusRangeEdge(7, G144BlockRange{}, dst, false, dstUnknown);
        // G276/G284: publish deferred display readbacks before this l2h upload edge — unless G284
        // proves the upload dst is disjoint from the pending display rows (then keep it coalesced).
        g276FlushPendingDisplayForRanges(7, G144BlockRange{}, dst, false, dstUnknown);
        return;
    }
    if (g_g144List.empty())
        return;

    static const bool s_noDirty = (std::getenv("DC2_G145_NO_DIRTY_UPLOAD") != nullptr);
    const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!s_noDirty && !g144PendingNeedsFlushForRanges({}, dst))
    {
        g144DirtyStat("upload", false);
        return;
    }

    g144DirtyStat("upload", true);
    g144FlushPendingUpload();
}

void g144FlushPendingLocalTransferRange(uint32_t sbp,
                                        uint8_t sbw,
                                        uint8_t spsm,
                                        uint32_t ssax,
                                        uint32_t ssay,
                                        uint32_t dbp,
                                        uint8_t dbw,
                                        uint8_t dpsm,
                                        uint32_t dsax,
                                        uint32_t dsay,
                                        uint32_t rrw,
                                        uint32_t rrh)
{
    // G260: a local→local copy READS its source (must observe pending target writes → RAW) and
    // WRITES its destination (WAR/WAW against pending reads and writes). Same fail-closed range
    // logic as the upload edge.
    if (g260NrOn())
    {
        const G144BlockRange src = g144RangeForRect(sbp, spsm, sbw, ssax, ssay, rrw, rrh);
        const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
        const bool srcUnknown = (rrw != 0u && rrh != 0u && !src.valid);
        const bool dstUnknown = (rrw != 0u && rrh != 0u && !dst.valid);
        g289MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        if (g260HasPending())
        {
            if (g260VramEventNeedsExecute(src, dst, srcUnknown, dstUnknown))
            {
                g178CensusScan();
                G146PerfScope g146FlushScope(g_g146G144FlushUploadNs, g_g146G144FlushUploadCount);
                static const bool s_seqUpload260 = (std::getenv("DC2_G144_SEQUPLOAD") != nullptr);
                g260ExecuteAll(kG260LocalCopy, !s_seqUpload260);
            }
            else
            {
                ++g_g260UploadSkips;
            }
        }
        // G261: the local→local copy READS src (must observe wave-rendered pixels) and WRITES
        // dst — both must see real VRAM bytes for any GPU-resident RTT row they touch.
        g266NoteLocalCopy(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
                          src, dst, srcUnknown, dstUnknown);
        (void)g278FlushPendingDepthForRanges(
            8, src, dst, srcUnknown, dstUnknown);
        const uint32_t g285Consumed = g285PrepareL2lConsume(
            sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh,
            src, dst, srcUnknown, dstUnknown);
        g261MaterializeForRanges(src, dst, srcUnknown, dstUnknown, kG261MatLocal,
                                 g285Consumed);
        (void)g278FlushPendingDepthForDisplayOwners(8);
        g272MaterializeForRanges(src, dst, srcUnknown, dstUnknown, 8);
        g277CensusRangeEdge(8, src, dst, srcUnknown, dstUnknown);
        // G276/G284: publish deferred display readbacks before this l2l edge — unless G284 proves the
        // copy src+dst are both disjoint from the pending display rows (then keep it coalesced).
        if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                                    dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
            g276FlushPendingDisplayForRanges(8, src, dst, srcUnknown, dstUnknown);
        return;
    }
    if (g_g144List.empty())
        return;

    static const bool s_noDirty = (std::getenv("DC2_G145_NO_DIRTY_UPLOAD") != nullptr);
    const G144BlockRange src = g144RangeForRect(sbp, spsm, sbw, ssax, ssay, rrw, rrh);
    const G144BlockRange dst = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!s_noDirty && !g144PendingNeedsFlushForRanges(src, dst))
    {
        g144DirtyStat("local", false);
        return;
    }

    g144DirtyStat("local", true);
    g144FlushPendingUpload();
}

void GSRasterizer::drawTriangle(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const GSVertex &v2 = gs->m_vtxQueue[2];
    const auto &ctx = gs->activeContext();

    // G104: clip title-rock triangles after GS primitive assembly, not per VU vertex. G102's
    // VU-side replacement was non-local for strips/fans: one shared behind-camera vertex can need
    // two different edge intersections. Here the complete triangle is available, so mixed q<=0/q>0
    // primitives are split and re-enter the normal raster path. Default uses screen-space q crossing
    // because GS XY is already packed; DC2_G104_HOMOG_CLIP restores the homogeneous A/B.
    if (!g_g104TriClipReentry &&
        g104TriClipEnabled() &&
        g104TitleRockTriangle(gs->m_prim.tme, ctx.frame.fbp, ctx.tex0.tbp0) &&
        !g106NoTriClipTriangle(gs->m_prim.tme,
                               static_cast<uint32_t>(gs->m_prim.type),
                               ctx.frame.fbp,
                               ctx.tex0.tbp0))
    {
        const bool in0 = g104InsideQ(v0);
        const bool in1 = g104InsideQ(v1);
        const bool in2 = g104InsideQ(v2);
        const int insideCount = (in0 ? 1 : 0) + (in1 ? 1 : 0) + (in2 ? 1 : 0);

        static const bool s_g104stat = envFlagEnabled("DC2_G104_STAT");
        static std::atomic<uint64_t> s_g104Pass{0};
        static std::atomic<uint64_t> s_g104Clip{0};
        static std::atomic<uint64_t> s_g104Drop{0};
        static std::atomic<uint64_t> s_g104Emit{0};
        auto g104LogMaybe = [&]() {
            if (!s_g104stat)
                return;
            const uint64_t total =
                s_g104Pass.load(std::memory_order_relaxed) +
                s_g104Clip.load(std::memory_order_relaxed) +
                s_g104Drop.load(std::memory_order_relaxed);
            if ((total & 0x3FFFull) == 0ull)
                std::fprintf(stderr,
                             "[G104:triclip] pass=%llu clip=%llu drop=%llu emit=%llu mode=%s wnear=%.2f qnear=%.5g\n",
                             static_cast<unsigned long long>(s_g104Pass.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Clip.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Drop.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(s_g104Emit.load(std::memory_order_relaxed)),
                             g104ScreenClipEnabled() ? "screen" : "homog",
                             g104Wnear(), g104Qnear());
        };

        // G128: near-plane REJECT for title-rock straddlers that cross the camera plane.
        // A vertex with q<=0 is BEHIND the camera; its perspective-divided screen XY was
        // saturated/wrapped by FTOI4 (12.4, 16-bit) before the GS saw it, so the homogeneous
        // clip's reconstruction clip=screen*W recovers garbage -> the screen-spanning dark
        // wedges (g127_smoke_check.png). The pre-FTOI4 float position needed for a correct
        // clip is not available here (it would require a VU-side side channel that desyncs
        // against the copy packer). Dropping the straddling triangle (HW's near-plane clip
        // would keep a sliver; we lose it) removes the wedges cleanly with at most a
        // one-triangle-deep ragged edge. Title-scoped (inside g104TitleRockTriangle). Kill
        // with DC2_G128_NO_BEHIND_DROP=1 to restore the G125/G126 homog-clip-only behaviour.
        // G139: RETIRED by default (was default-ON). Natural gate + fixed VU positions leave no
        // straddler wedges to drop (A/B g100_g139nb_keepz.png). Re-enable DC2_G128_FORCE_BEHIND_DROP=1.
        static const bool s_g128NoBehindDrop =
            std::getenv("DC2_G128_FORCE_BEHIND_DROP") == nullptr ||
            envFlagEnabled("DC2_G128_NO_BEHIND_DROP");
        static std::atomic<uint64_t> s_g128BehindDrop{0};
        if (!s_g128NoBehindDrop)
        {
            const bool behind0 = !std::isfinite(v0.q) || v0.q <= 0.0f;
            const bool behind1 = !std::isfinite(v1.q) || v1.q <= 0.0f;
            const bool behind2 = !std::isfinite(v2.q) || v2.q <= 0.0f;
            if (behind0 || behind1 || behind2)
            {
                s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
                if (s_g104stat)
                {
                    const uint64_t bd = s_g128BehindDrop.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if ((bd & 0x3FFFull) == 0ull)
                        std::fprintf(stderr, "[G128:behinddrop] dropped=%llu\n",
                                     static_cast<unsigned long long>(bd));
                }
                return;
            }
        }

        if (insideCount == 0)
        {
            s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
            g104LogMaybe();
            return;
        }

        if (insideCount == 3)
        {
            s_g104Pass.fetch_add(1u, std::memory_order_relaxed);
            g104LogMaybe();
        }
        else
        {
            GSVertex inPoly[6] = {v0, v1, v2};
            GSVertex outPoly[6]{};
            int inCount = 3;
            int outCount = 0;
            const float wNear = g104Wnear();

            for (int i = 0; i < inCount; ++i)
            {
                const GSVertex &a = inPoly[i];
                const GSVertex &b = inPoly[(i + 1) % inCount];
                const bool aIn = g104InsideQ(a);
                const bool bIn = g104InsideQ(b);

                if (aIn && bIn)
                {
                    outPoly[outCount++] = b;
                }
                else if (aIn && !bIn)
                {
                    outPoly[outCount++] = g104IntersectNearW(a, b, wNear);
                }
                else if (!aIn && bIn)
                {
                    outPoly[outCount++] = g104IntersectNearW(a, b, wNear);
                    outPoly[outCount++] = b;
                }
            }

            if (outCount < 3)
            {
                s_g104Drop.fetch_add(1u, std::memory_order_relaxed);
                g104LogMaybe();
                return;
            }

            s_g104Clip.fetch_add(1u, std::memory_order_relaxed);
            s_g104Emit.fetch_add(static_cast<uint64_t>(outCount - 2), std::memory_order_relaxed);
            g104LogMaybe();

            const GSVertex saved0 = gs->m_vtxQueue[0];
            const GSVertex saved1 = gs->m_vtxQueue[1];
            const GSVertex saved2 = gs->m_vtxQueue[2];
            const bool savedReentry = g_g104TriClipReentry;
            g_g104TriClipReentry = true;
            for (int i = 1; i + 1 < outCount; ++i)
            {
                gs->m_vtxQueue[0] = outPoly[0];
                gs->m_vtxQueue[1] = outPoly[i];
                gs->m_vtxQueue[2] = outPoly[i + 1];
                drawTriangle(gs);
            }
            g_g104TriClipReentry = savedReentry;
            gs->m_vtxQueue[0] = saved0;
            gs->m_vtxQueue[1] = saved1;
            gs->m_vtxQueue[2] = saved2;
            return;
        }
    }

    // G82: per-TRIANGLE (vertex-comparable) rock colour aggregator, so we can compare the
    // runner's raw per-vertex rock colours to HW (g82_gs_verts.py, per-vertex) apples-to-apples
    // -- the per-pixel [G82:rockavg] is area-weighted and not comparable. Gated DC2_G82_VTX.
    {
        static const bool s_g82vtx = (std::getenv("DC2_G82_VTX") != nullptr);
        if (s_g82vtx && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::mutex s_vmtx;
            struct VAcc { uint32_t tbp; uint64_t n, r,g,b,a; };
            static VAcc s_v[96]{}; static uint32_t s_vN = 0;
            std::lock_guard<std::mutex> lk(s_vmtx);
            uint32_t idx = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < s_vN; ++i) if (s_v[i].tbp == ctx.tex0.tbp0) { idx = i; break; }
            if (idx == 0xFFFFFFFFu && s_vN < 96u) { idx = s_vN++; s_v[idx].tbp = ctx.tex0.tbp0; }
            if (idx != 0xFFFFFFFFu) {
                VAcc &A = s_v[idx];
                A.n += 3; A.r += v0.r+v1.r+v2.r; A.g += v0.g+v1.g+v2.g; A.b += v0.b+v1.b+v2.b; A.a += v0.a+v1.a+v2.a;
            }
            static uint64_t s_vt = 0;
            if ((++s_vt % 40000ull) == 0ull)
                for (uint32_t i = 0; i < s_vN; ++i) { VAcc &A = s_v[i]; if (!A.n) continue;
                    std::fprintf(stderr, "[G82:vtx] tbp=0x%x verts=%llu avgcol=(%llu,%llu,%llu,%llu)\n",
                        A.tbp, (unsigned long long)A.n, (unsigned long long)(A.r/A.n),
                        (unsigned long long)(A.g/A.n), (unsigned long long)(A.b/A.n), (unsigned long long)(A.a/A.n)); }
        }
    }

    // G86: title-scoped texel dump. Decode the BOUND texture page through the runtime's own
    // sampler for each distinct rock tbp the first time it is bound in a title draw (fbp!=0x139).
    // Tells us whether the page the runner samples holds detailed brown rock (right content) or
    // green/uniform/garbage (wrong texture addressing). DC2_G86_TEXDUMP=1.
    {
        static const bool s_g86 = (std::getenv("DC2_G86_TEXDUMP") != nullptr);
        if (s_g86 && gs->m_vram && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3a00u)
        {
            static std::mutex s_m86;
            static uint32_t s_seen[64]{}; static uint32_t s_seenN = 0;
            const auto &tex = ctx.tex0;
            std::lock_guard<std::mutex> lk(s_m86);
            bool dumped = false;
            for (uint32_t i = 0; i < s_seenN; ++i) if (s_seen[i] == tex.tbp0) { dumped = true; break; }
            if (!dumped && s_seenN < 64u)
            {
                s_seen[s_seenN++] = tex.tbp0;
                const int dw = 1 << tex.tw, dh = 1 << tex.th;
                const uint32_t bwv = (tex.tbw > 1u) ? (tex.tbw >> 1) : 1u;
                char path[128];
                std::snprintf(path, sizeof(path), "captures/g86_tex_%x.ppm", tex.tbp0);
                FILE *fp = std::fopen(path, "wb");
                uint64_t sr = 0, sg = 0, sb = 0; uint32_t nz = 0;
                if (fp) std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                for (int yy = 0; yy < dh; ++yy)
                    for (int xx = 0; xx < dw; ++xx)
                    {
                        uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                         static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                        uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                        uint8_t rr = c & 0xFF, gg = (c >> 8) & 0xFF, bb = (c >> 16) & 0xFF;
                        sr += rr; sg += gg; sb += bb; if (c & 0xFFFFFF) ++nz;
                        uint8_t rgb[3] = { rr, gg, bb };
                        if (fp) std::fwrite(rgb, 1, 3, fp);
                    }
                if (fp) std::fclose(fp);
                const uint32_t np = (uint32_t)(dw * dh);
                std::fprintf(stderr,
                    "[G86:texdump] tbp=0x%x %dx%d tbw=%u psm=0x%x cbp=0x%x avgtexel=(%llu,%llu,%llu) nz=%u/%u\n",
                    tex.tbp0, dw, dh, (unsigned)tex.tbw, (unsigned)tex.psm, tex.cbp,
                    (unsigned long long)(sr/np), (unsigned long long)(sg/np),
                    (unsigned long long)(sb/np), nz, np);
                std::fflush(stderr);
            }
        }
    }

    // G240: compare the foliage cutout's effective T8 indices and CT32 CLUT alpha against
    // the supplied hardware dump. The source IMAGE packets are byte-identical; this probe
    // observes the final values at the exact GEQUAL/AREF=64 draw boundary. Default-off.
    {
        static const bool s_g240TexAlpha = (std::getenv("DC2_G240_TEXALPHA") != nullptr);
        const auto &tex = ctx.tex0;
        const bool foliage2920 = tex.tbp0 == 0x2920u && tex.cbp == 0x3fb4u;
        const bool foliage2a20 = tex.tbp0 == 0x2a20u && tex.cbp == 0x3fb0u;
        if (s_g240TexAlpha && gs->m_vram && gs->m_prim.tme && tex.psm == GS_PSM_T8 &&
            tex.tcc != 0u && (foliage2920 || foliage2a20) &&
            (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u) && ctx.test == 0x5040bu)
        {
            static std::mutex s_g240Mtx;
            static uint32_t s_g240Seen = 0u;
            std::lock_guard<std::mutex> lock(s_g240Mtx);
            const uint32_t bit = foliage2920 ? 1u : 2u;
            if ((s_g240Seen & bit) == 0u)
            {
                s_g240Seen |= bit;
                const int width = 1 << tex.tw;
                const int height = 1 << tex.th;
                const uint32_t pageBwT8 = (tex.tbw > 1u) ? (tex.tbw >> 1u) : 1u;
                char indexPath[128], alphaPath[128], clutPath[128];
                std::snprintf(indexPath, sizeof(indexPath), "captures/g240_tex_%x_index.pgm", tex.tbp0);
                std::snprintf(alphaPath, sizeof(alphaPath), "captures/g240_tex_%x_alpha.pgm", tex.tbp0);
                std::snprintf(clutPath, sizeof(clutPath), "captures/g240_clut_%x.rgba", tex.cbp);
                FILE *indexFile = std::fopen(indexPath, "wb");
                FILE *alphaFile = std::fopen(alphaPath, "wb");
                FILE *clutFile = std::fopen(clutPath, "wb");
                if (indexFile) std::fprintf(indexFile, "P5\n%d %d\n255\n", width, height);
                if (alphaFile) std::fprintf(alphaFile, "P5\n%d %d\n255\n", width, height);

                uint64_t alphaHist[256]{};
                uint64_t indexHist[256]{};
                for (int y = 0; y < height; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        const uint8_t index = static_cast<uint8_t>(GSMem::ReadP8(
                            gs->m_vram, tex.tbp0, pageBwT8,
                            static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                        const uint32_t color = lookupCLUT(gs, index, tex.cbp, tex.cpsm,
                                                          tex.csm, tex.csa, tex.psm);
                        const uint8_t alpha = static_cast<uint8_t>(color >> 24u);
                        ++indexHist[index];
                        ++alphaHist[alpha];
                        if (indexFile) std::fwrite(&index, 1, 1, indexFile);
                        if (alphaFile) std::fwrite(&alpha, 1, 1, alphaFile);
                    }
                }
                if (indexFile) std::fclose(indexFile);
                if (alphaFile) std::fclose(alphaFile);

                if (clutFile)
                {
                    for (uint32_t index = 0; index < 256u; ++index)
                    {
                        const uint32_t color = lookupCLUT(gs, static_cast<uint8_t>(index),
                                                          tex.cbp, tex.cpsm, tex.csm,
                                                          tex.csa, tex.psm);
                        std::fwrite(&color, 1, sizeof(color), clutFile);
                    }
                    std::fclose(clutFile);
                }

                uint64_t below64 = 0u, atLeast64 = 0u;
                for (uint32_t alpha = 0; alpha < 256u; ++alpha)
                {
                    if (alpha < 64u) below64 += alphaHist[alpha];
                    else atLeast64 += alphaHist[alpha];
                }
                std::fprintf(stderr,
                    "[G240:texalpha] tbp=0x%x cbp=0x%x size=%dx%d tbw=%u cpsm=0x%x csm=%u csa=%u "
                    "cld=%u tex1=0x%llx texclut=(%u,%u,%u) test=0x%llx texa=(%u,%u,%u) "
                    "alpha_lt64=%llu alpha_ge64=%llu idx0=%llu idx173=%llu vtxa=(%u,%u,%u)\n",
                    tex.tbp0, tex.cbp, width, height, static_cast<unsigned>(tex.tbw),
                    static_cast<unsigned>(tex.cpsm), static_cast<unsigned>(tex.csm),
                    static_cast<unsigned>(tex.csa), static_cast<unsigned>(tex.cld),
                    static_cast<unsigned long long>(ctx.tex1),
                    static_cast<unsigned>(gs->m_texclut.cbw),
                    static_cast<unsigned>(gs->m_texclut.cou),
                    static_cast<unsigned>(gs->m_texclut.cov),
                    static_cast<unsigned long long>(ctx.test),
                    static_cast<unsigned>(gs->m_texa.ta0), static_cast<unsigned>(gs->m_texa.aem),
                    static_cast<unsigned>(gs->m_texa.ta1),
                    static_cast<unsigned long long>(below64),
                    static_cast<unsigned long long>(atLeast64),
                    static_cast<unsigned long long>(indexHist[0]),
                    static_cast<unsigned long long>(indexHist[173]),
                    static_cast<unsigned>(v0.a), static_cast<unsigned>(v1.a),
                    static_cast<unsigned>(v2.a));
                std::fflush(stderr);
            }
        }
    }

    // G87: streaks diagnosis. Dump the first rock triangles' SCREEN positions + texcoords
    // (FST/ST/Q + derived UV s/q,t/q) so we can tell horizontal-geometry-stretch from degenerate
    // texture coords. DC2_G87_UV=1, title scope (fbp!=0x139), rock tbps.
    {
        static const bool s_g87uv = (std::getenv("DC2_G87_UV") != nullptr);
        if (s_g87uv && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (k < 24u)
            {
                const int ox = ctx.xyoffset.ofx >> 4, oy = ctx.xyoffset.ofy >> 4;
                const int tw = 1 << ctx.tex0.tw, th = 1 << ctx.tex0.th;
                auto uvof = [&](const GSVertex &v, float &uu, float &vv) {
                    if (gs->m_prim.fst) { uu = v.u / 16.0f; vv = v.v / 16.0f; }
                    else { float q = (v.q != 0.0f) ? v.q : 1.0f; uu = v.s / q * tw; vv = v.t / q * th; }
                };
                float u0,vv0,u1,vv1,u2,vv2; uvof(v0,u0,vv0); uvof(v1,u1,vv1); uvof(v2,u2,vv2);
                std::fprintf(stderr,
                    "[G87:uv] k=%u tbp=0x%x fst=%u tw=%d th=%d  px0=(%.0f,%.0f) px1=(%.0f,%.0f) px2=(%.0f,%.0f)  "
                    "stq0=(%.4f,%.4f,%.4f) uv=(%.0f,%.0f)|(%.0f,%.0f)|(%.0f,%.0f)\n",
                    k, ctx.tex0.tbp0, (unsigned)gs->m_prim.fst, tw, th,
                    v0.x-ox, v0.y-oy, v1.x-ox, v1.y-oy, v2.x-ox, v2.y-oy,
                    v0.s, v0.t, v0.q, u0,vv0, u1,vv1, u2,vv2);
                std::fflush(stderr);
            }
        }
    }

    // G88: streaks. Compact per-triangle log of ALL title rock draws (no 24-cap, all rock
    // tbps) so a python pass can bucket per (prim,tbp): count / on-screen ratio / X-Y ranges
    // = the runner analogue of tools/g84_gs_geom.py on HW. DC2_G88_GEO=1, title scope.
    {
        static const bool s_g88geo = (std::getenv("DC2_G88_GEO") != nullptr);
        if (s_g88geo && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t k = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (k < 20000u)
            {
                const int ox = ctx.xyoffset.ofx >> 4, oy = ctx.xyoffset.ofy >> 4;
                std::fprintf(stderr, "[G88:geo] blk=%d prim=%u tbp=0x%x ofx=%d ofy=%d sc=[%u,%u,%u,%u] raw0=(%.0f,%.0f) raw1=(%.0f,%.0f) raw2=(%.0f,%.0f)\n",
                    g_dc2TitleCurBlock.load(std::memory_order_relaxed),
                    (unsigned)gs->m_prim.type, ctx.tex0.tbp0, ox, oy,
                    ctx.scissor.x0, ctx.scissor.x1, ctx.scissor.y0, ctx.scissor.y1,
                    v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
            }
        }
    }

    // G106: default-off title batch isolation. Drop one fully assembled title-rock texture
    // page and/or primitive class so we can identify which runner batch hides the HW mural.
    if (g106ShouldSkipTriangle(gs->m_prim.tme,
                               static_cast<uint32_t>(gs->m_prim.type),
                               ctx.frame.fbp,
                               ctx.tex0.tbp0))
    {
        if (g106SkipStatEnabled())
        {
            static std::atomic<uint64_t> s_skip{0};
            const uint64_t n = s_skip.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n & 0x3FFFull) == 0ull)
                std::fprintf(stderr,
                    "[G106:skip] n=%llu prim=%u tbp=0x%x fbp=0x%x\n",
                    static_cast<unsigned long long>(n),
                    static_cast<uint32_t>(gs->m_prim.type),
                    ctx.tex0.tbp0,
                    ctx.frame.fbp);
        }
        return;
    }

    // G89: GUARD-BAND CULL for the title rock copy path. The title rock is forced through the
    // copy packer (G78/G80, VU prog 0x1b68) which -- unlike the transform program (0x1d00, the
    // +2048 ADC gate) -- NEVER sets the per-vertex ADC bit. On HW ~60% of the rock tristrip verts
    // carry ADC=1 (drawing-kick disabled = strip restart), so verts that project off-screen never
    // form triangles. The runner's copy path emits ADC=0 for every vert, so the GS forms triangles
    // that connect on-screen verts to far-off-screen verts; the scissor then clips them into full-
    // width horizontal bands = the "green streaks". Root cause: the runner's GS rasterizer has no
    // guard-band primitive culling (a real GS feature). Replicate HW's ADC strip-restart by culling
    // any title-rock triangle that has a vertex far outside the drawing rect. The legitimate cavern
    // triangles span <=60px and sit on-screen; the streak triangles have verts hundreds of px past
    // the guard (e.g. screen-X -1760 and +2127 in the same tri). A guard of 512px keeps ~31% of the
    // rock triangles, matching HW's drawn fraction (tools/g84_gs_geom.py on new_game.gs: tri 31%,
    // tstrip 35%). NOTE this overturns G88's "4x world-matrix over-scale": the title LW matrix is
    // unit-scale on BOTH runner and HW (verified live, frame 0x981390 +0x70) and the per-prim screen
    // centroids match HW (~2050) -- the geometry is correctly positioned; only the off-screen strip
    // segments were wrongly drawn. Scoped to the title rock (tme + fbp!=0x139 + rock tbp range) so
    // dungeon/costume are untouched. Default-ON; kill DC2_G89_NO_GUARD_CULL; guard DC2_G89_GUARD.
    {
        // G139: RETIRED by default (was default-ON). The natural FMAND guard gate (G138) is the
        // real HW cull; the spatial guard approximation is obsolete (A/B g100_g139nb_keepz.png).
        // Re-enable with DC2_G89_FORCE_GUARD_CULL=1 (DC2_G89_GUARD still tunes the width).
        static const bool s_g89off = (std::getenv("DC2_G89_FORCE_GUARD_CULL") == nullptr) ||
                                     (std::getenv("DC2_G89_NO_GUARD_CULL") != nullptr);
        if (!g_g104TriClipReentry && !s_g89off && g144EffectiveTitleRockScope() &&
            gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
            ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
        {
            static const float s_guard = []() -> float {
                const char *e = std::getenv("DC2_G89_GUARD");
                if (e) return static_cast<float>(std::atof(e));
                // G100: the native forced-draw route (default) draws the full transform
                // geometry, so the guard that approximates HW's ADC strip-restart must be
                // wider than the old copy-path 512 (which left blue triangular holes). 1024
                // fills the holes at the settled menu while still culling the far off-screen
                // strip segments. The reverted copy path (DC2_G100_NO_FORCEDRAW) keeps 512.
                return (std::getenv("DC2_G100_NO_FORCEDRAW") == nullptr) ? 1024.0f : 512.0f;
            }();
            const float ox = static_cast<float>(ctx.xyoffset.ofx >> 4);
            const float oy = static_cast<float>(ctx.xyoffset.ofy >> 4);
            const float loX = static_cast<float>(ctx.scissor.x0) - s_guard;
            const float hiX = static_cast<float>(ctx.scissor.x1) + s_guard;
            const float loY = static_cast<float>(ctx.scissor.y0) - s_guard;
            const float hiY = static_cast<float>(ctx.scissor.y1) + s_guard;
            const float sx0 = v0.x - ox, sy0 = v0.y - oy;
            const float sx1 = v1.x - ox, sy1 = v1.y - oy;
            const float sx2 = v2.x - ox, sy2 = v2.y - oy;
            if (sx0 < loX || sx0 > hiX || sy0 < loY || sy0 > hiY ||
                sx1 < loX || sx1 > hiX || sy1 < loY || sy1 > hiY ||
                sx2 < loX || sx2 > hiX || sy2 < loY || sy2 > hiY)
                return;

            // G101: cull the "vertex explosion" -- stretched spanning connector tris. HW restarts
            // the tristrip at off-screen verts (per-vertex ADC) so it never draws the long tris
            // that bridge an on-screen vert to an off-screen one; the runner's G100 forced-draw
            // has no per-vertex ADC, so those connectors draw as giant stretched triangles that
            // swim across the screen as the title camera pans. They are distinguishable from real
            // geometry: every FULLY on-screen rock tri has a short screen-space edge (<=256px in
            // the new_game reference), while the connectors have >=1 off-screen vert AND a long
            // edge. So: if a tri has any vert outside the scissor AND its longest screen-space edge
            // exceeds the threshold, drop it -- fully on-screen tris (any size) and compact edge
            // overhangs are kept. NOTE (G101 A/B): culling these connectors regresses to blue holes
            // because they also carry on-screen coverage -- explosion and coverage are the SAME
            // tris. Left as an OPT-IN diagnostic (DC2_G101_EDGE_CULL) pending the gate-natural fix;
            // tune DC2_G101_MAXEDGE.
            static const bool s_edgeOn = (std::getenv("DC2_G101_EDGE_CULL") != nullptr);
            if (s_edgeOn)
            {
                static const float s_maxEdge = []() -> float {
                    const char *e = std::getenv("DC2_G101_MAXEDGE");
                    return e ? static_cast<float>(std::atof(e)) : 384.0f;
                }();
                const float scX0 = static_cast<float>(ctx.scissor.x0);
                const float scX1 = static_cast<float>(ctx.scissor.x1);
                const float scY0 = static_cast<float>(ctx.scissor.y0);
                const float scY1 = static_cast<float>(ctx.scissor.y1);
                const bool spanning =
                    sx0 < scX0 || sx0 > scX1 || sy0 < scY0 || sy0 > scY1 ||
                    sx1 < scX0 || sx1 > scX1 || sy1 < scY0 || sy1 > scY1 ||
                    sx2 < scX0 || sx2 > scX1 || sy2 < scY0 || sy2 > scY1;
                if (spanning)
                {
                    const float d01x = sx0 - sx1, d01y = sy0 - sy1;
                    const float d12x = sx1 - sx2, d12y = sy1 - sy2;
                    const float d20x = sx2 - sx0, d20y = sy2 - sy0;
                    const float e01 = d01x * d01x + d01y * d01y;
                    const float e12 = d12x * d12x + d12y * d12y;
                    const float e20 = d20x * d20x + d20y * d20y;
                    float emax2 = e01 > e12 ? e01 : e12;
                    if (e20 > emax2) emax2 = e20;
                    if (emax2 > s_maxEdge * s_maxEdge)
                        return;
                }
            }
        }
    }

    const uint32_t g43Skip = g43SkipTbp();
    if (g43Skip != 0xFFFFFFFFu &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == g43Skip)
    {
        return;
    }

    // [F51.6:entry] Confirm whether drawTriangle is reached for the displayed buffer at all.
    if (renderQualityTraceEnabled())
    {
        static std::atomic<uint32_t> s_f516entry{0};
        const uint32_t n = s_f516entry.fetch_add(1u, std::memory_order_relaxed);
        if (n < 80u)
            std::fprintf(stderr, "[F51.6:entry] n=%u fbp=0x%x tme=%u type=%u ctxt=%u tbp=0x%x psm=0x%x\n",
                n, ctx.frame.fbp, (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.type,
                (unsigned)gs->m_prim.ctxt, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm);
    }

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    // G32: for the costume model tristrips (verts in the ~2000-unit packed range), report
    // the screen mapping so we can tell on-screen-but-occluded from scissored-off. Gated.
    if (renderQualityTraceEnabled() && gs->m_prim.type == GS_PRIM_TRISTRIP && v0.x > 1500.0f)
    {
        static std::atomic<uint32_t> s_g32tri{0};
        const uint32_t n = s_g32tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 400u)
            std::fprintf(stderr,
                "[G32:tri] n=%u fbp=0x%x fbw=%u psm=0x%x tme=%u abe=%u tbp=0x%x tfx=%u tcc=%u "
                "scis=(%u,%u)-(%u,%u) ofxy=(%d,%d) v0px=(%.1f,%.1f) v1px=(%.1f,%.1f) v2px=(%.1f,%.1f) "
                "rgba0=(%u,%u,%u,%u) rgba1=(%u,%u,%u,%u) rgba2=(%u,%u,%u,%u) "
                "z=(%.0f,%.0f,%.0f) zbuf(zbp=0x%x,zpsm=0x%x,zmsk=%u) test(zte=%u,ztst=%u)\n",
                n, ctx.frame.fbp, (unsigned)ctx.frame.fbw, (unsigned)ctx.frame.psm,
                (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.abe, ctx.tex0.tbp0,
                (unsigned)ctx.tex0.tfx, (unsigned)ctx.tex0.tcc,
                ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1, ofx, ofy,
                v0.x - (float)ofx, v0.y - (float)ofy, v1.x - (float)ofx, v1.y - (float)ofy,
                v2.x - (float)ofx, v2.y - (float)ofy,
                (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                (unsigned)v1.r, (unsigned)v1.g, (unsigned)v1.b, (unsigned)v1.a,
                (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a,
                v0.z, v1.z, v2.z,
                (unsigned)(ctx.zbuf & 0x1FFu), (unsigned)((ctx.zbuf >> 24) & 0xFu),
                (unsigned)((ctx.zbuf >> 32) & 0x1u),
                (unsigned)((ctx.test >> 16) & 0x1u), (unsigned)((ctx.test >> 17) & 0x3u));
    }

    const bool g43FaceUv =
        g43FaceUvTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        (ctx.tex0.tbp0 == 0x3460u || ctx.tex0.tbp0 == 0x3480u) &&
        gs->m_prim.type == GS_PRIM_TRISTRIP &&
        v0.x > 1500.0f;
    if (g43FaceUv)
    {
        static std::atomic<uint32_t> s_g43tri{0};
        const uint32_t n = s_g43tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 220u)
        {
            std::fprintf(stderr,
                "[G43:triuv] n=%u fbp=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x csa=%u fst=%u tfx=%u tcc=%u "
                "px0=(%.1f,%.1f) px1=(%.1f,%.1f) px2=(%.1f,%.1f) "
                "uv0=(%u,%u) uv1=(%u,%u) uv2=(%u,%u) "
                "stq0=(%.5g,%.5g,%.5g) stq1=(%.5g,%.5g,%.5g) stq2=(%.5g,%.5g,%.5g) "
                "rgba0=(%u,%u,%u,%u) rgba1=(%u,%u,%u,%u) rgba2=(%u,%u,%u,%u)\n",
                n, ctx.frame.fbp, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                ctx.tex0.cbp, (unsigned)ctx.tex0.cpsm, (unsigned)ctx.tex0.csa,
                (unsigned)gs->m_prim.fst, (unsigned)ctx.tex0.tfx, (unsigned)ctx.tex0.tcc,
                v0.x - (float)ofx, v0.y - (float)ofy,
                v1.x - (float)ofx, v1.y - (float)ofy,
                v2.x - (float)ofx, v2.y - (float)ofy,
                (unsigned)(v0.u >> 4), (unsigned)(v0.v >> 4),
                (unsigned)(v1.u >> 4), (unsigned)(v1.v >> 4),
                (unsigned)(v2.u >> 4), (unsigned)(v2.v >> 4),
                v0.s, v0.t, v0.q, v1.s, v1.t, v1.q, v2.s, v2.t, v2.q,
                (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                (unsigned)v1.r, (unsigned)v1.g, (unsigned)v1.b, (unsigned)v1.a,
                (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a);
        }
    }

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int rawMinX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int rawMaxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int rawMinY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int rawMaxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    int minX = clampInt(rawMinX, ctx.scissor.x0, ctx.scissor.x1);
    int maxX = clampInt(rawMaxX, ctx.scissor.x0, ctx.scissor.x1);
    int minY = clampInt(rawMinY, ctx.scissor.y0, ctx.scissor.y1);
    int maxY = clampInt(rawMaxY, ctx.scissor.y0, ctx.scissor.y1);
    // G144: during a tile-binning flush, restrict this triangle to the calling lane's scanline band
    // (intersect the bbox with [BandY0,BandY1]). Everything downstream (x-span, y-loop, Z, write)
    // then operates only on in-band rows, so lanes touch disjoint pixels. If the intersection is
    // empty this entry does not cover the band and the loops run zero iterations.
    if (t_g144Banded)
    {
        if (minY < t_g144BandY0) minY = t_g144BandY0;
        if (maxY > t_g144BandY1) maxY = t_g144BandY1;
    }

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);

    // G219 (default-off, DC2_G219_SKYPX=1): triangle-level probe for the sky dome (tbp=0x2820)
    // on the display targets — logs every such triangle that reaches the raster stage with its
    // clamped bbox, so a zero-pixel result in the black region can be split into "triangle never
    // got here" vs "bbox/span clipped it".
    {
        static const bool s_g219t = (std::getenv("DC2_G219_SKYPX") != nullptr);
        // G219: raster-time fingerprint pair for the [G219:cap] capture-time one above.
        if (s_g219t && gs->m_prim.tme && ctx.tex0.tbp0 == 0x2720u &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            gs->m_vram && 0x272000u + 0x8000u <= gs->m_vramSize)
        {
            static std::atomic<uint32_t> s_g219rn{0};
            const uint32_t n = s_g219rn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
            {
                uint64_t sum = 0;
                const uint64_t *p = reinterpret_cast<const uint64_t *>(gs->m_vram + 0x272000u);
                for (uint32_t i = 0; i < 0x1000u; ++i)
                    sum += p[i];
                std::fprintf(stderr, "[G219:ras] n=%u fbp=0x%x sum=%016llx replay=%u banded=%u\n",
                             n, ctx.frame.fbp, (unsigned long long)sum,
                             t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u);
                // Diff vs the capture-time snapshot: the changed offsets identify the clobbering
                // writer (color-page vs Z-page vs copy stride patterns).
                extern uint8_t g_g219Snap[0x8000];
                const uint64_t *q = reinterpret_cast<const uint64_t *>(g_g219Snap);
                uint32_t shown = 0, diffs = 0;
                for (uint32_t i = 0; i < 0x1000u; ++i)
                {
                    if (p[i] != q[i])
                    {
                        ++diffs;
                        if (shown < 6u)
                        {
                            std::fprintf(stderr,
                                "[G219:dif] off=0x%05x old=%016llx new=%016llx\n",
                                0x272000u + i * 8u, (unsigned long long)q[i], (unsigned long long)p[i]);
                            ++shown;
                        }
                    }
                }
                if (diffs)
                    std::fprintf(stderr, "[G219:dif] totalDiffQwords=%u of 4096\n", diffs);
            }
        }
        if (s_g219t &&
            (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
            rawMaxX >= 440 && rawMinY <= 100 && (rawMaxX - rawMinX) > 60)
        {
            static std::atomic<uint32_t> s_g219tn{0};
            const uint32_t n = s_g219tn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 300u || (n % 256u) == 0u)
                std::fprintf(stderr,
                    "[G219:skytri] n=%u fbp=0x%x prim=%u tme=%u tbp=0x%x bbox=(%d,%d)-(%d,%d) "
                    "raw=(%d,%d)-(%d,%d) denom=%g replay=%u banded=%u band=(%d,%d) "
                    "px=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) q=(%.4g,%.4g,%.4g)\n",
                    n, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY,
                    denom, t_g144InReplay ? 1u : 0u, t_g144Banded ? 1u : 0u,
                    t_g144BandY0, t_g144BandY1,
                    fx0, fy0, fx1, fy1, fx2, fy2, v0.q, v1.q, v2.q);
        }
    }

    static const bool s_g195FgTrace = envFlagEnabled("DC2_G195_FG_TRACE");
    if ((g268LiveEnvRead() ? envFlagEnabled("DC2_G195_FG_TRACE") : s_g195FgTrace) &&
        gs->m_vram &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 == 0x2a20u &&
        (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u))
    {
        static std::atomic<uint32_t> s_g195FgTri{0};
        const uint32_t n = s_g195FgTri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 256u || (n % 512u) == 0u)
        {
            std::fprintf(stderr,
                "[G195:fgtri] n=%u blk=%d fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u abe=%u "
                "tex0=0x%016llx tbw=%u psm=0x%x tw=%u th=%u tfx=%u tcc=%u cbp=0x%x cpsm=0x%x csa=%u "
                "alpha=0x%llx test=0x%llx zbuf=0x%llx tex1=0x%llx clamp=0x%llx "
                "bbox=(%d,%d)-(%d,%d) rawbbox=(%d,%d)-(%d,%d) denom=%g of=(%d,%d) "
                "v0=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u) "
                "v1=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u) "
                "v2=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u)\n",
                n, g_dc2TitleCurBlock.load(std::memory_order_relaxed),
                ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.iip),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
                static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.psm),
                static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
                static_cast<uint32_t>(ctx.tex0.tfx), static_cast<uint32_t>(ctx.tex0.tcc),
                ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.cpsm),
                static_cast<uint32_t>(ctx.tex0.csa),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY, denom, ofx, ofy,
                fx0, fy0, v0.z, static_cast<uint32_t>(v0.r), static_cast<uint32_t>(v0.g),
                static_cast<uint32_t>(v0.b), static_cast<uint32_t>(v0.a),
                v0.s, v0.t, v0.q, static_cast<uint32_t>(v0.u), static_cast<uint32_t>(v0.v),
                fx1, fy1, v1.z, static_cast<uint32_t>(v1.r), static_cast<uint32_t>(v1.g),
                static_cast<uint32_t>(v1.b), static_cast<uint32_t>(v1.a),
                v1.s, v1.t, v1.q, static_cast<uint32_t>(v1.u), static_cast<uint32_t>(v1.v),
                fx2, fy2, v2.z, static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a),
                v2.s, v2.t, v2.q, static_cast<uint32_t>(v2.u), static_cast<uint32_t>(v2.v));
        }
    }

    if (g195DofTraceEnabled() &&
        gs->m_vram &&
        gs->m_prim.tme &&
        ctx.tex0.tbp0 >= 0x2720u &&
        ctx.tex0.tbp0 <= 0x3a00u &&
        ctx.frame.fbp != 0x139u)
    {
        struct G195State
        {
            uint32_t fbp, fbw, tbp, tbw, cbp;
            uint8_t fpsm, psm, tfx, tcc, tw, th, cpsm, csm, csa;
            uint8_t prim, fst, iip, abe;
            uint64_t alpha, test, tex1, clamp;
            uint32_t hits;
        };
        static std::mutex s_stateMutex;
        static G195State s_seen[256]{};
        static uint32_t s_seenN = 0;
        const G195State key{
            ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw), ctx.tex0.tbp0,
            static_cast<uint32_t>(ctx.tex0.tbw), ctx.tex0.cbp,
            static_cast<uint8_t>(ctx.frame.psm), static_cast<uint8_t>(ctx.tex0.psm),
            static_cast<uint8_t>(ctx.tex0.tfx), static_cast<uint8_t>(ctx.tex0.tcc),
            static_cast<uint8_t>(ctx.tex0.tw), static_cast<uint8_t>(ctx.tex0.th),
            static_cast<uint8_t>(ctx.tex0.cpsm), static_cast<uint8_t>(ctx.tex0.csm),
            static_cast<uint8_t>(ctx.tex0.csa), static_cast<uint8_t>(gs->m_prim.type),
            static_cast<uint8_t>(gs->m_prim.fst), static_cast<uint8_t>(gs->m_prim.iip),
            static_cast<uint8_t>(gs->m_prim.abe), ctx.alpha, ctx.test, ctx.tex1, ctx.clamp, 0u
        };
        uint32_t idx = 0xFFFFFFFFu;
        bool firstHit = false;
        uint32_t hits = 0;
        {
            std::lock_guard<std::mutex> lk(s_stateMutex);
            for (uint32_t i = 0; i < s_seenN; ++i)
            {
                const G195State &s = s_seen[i];
                if (s.fbp == key.fbp && s.fbw == key.fbw && s.tbp == key.tbp &&
                    s.tbw == key.tbw && s.cbp == key.cbp && s.fpsm == key.fpsm &&
                    s.psm == key.psm && s.tfx == key.tfx && s.tcc == key.tcc &&
                    s.tw == key.tw && s.th == key.th && s.cpsm == key.cpsm &&
                    s.csm == key.csm && s.csa == key.csa && s.prim == key.prim &&
                    s.fst == key.fst && s.iip == key.iip && s.abe == key.abe &&
                    s.alpha == key.alpha && s.test == key.test && s.tex1 == key.tex1 &&
                    s.clamp == key.clamp)
                {
                    idx = i;
                    break;
                }
            }
            if (idx == 0xFFFFFFFFu && s_seenN < 256u)
            {
                idx = s_seenN++;
                s_seen[idx] = key;
                firstHit = true;
            }
            if (idx != 0xFFFFFFFFu)
                hits = ++s_seen[idx].hits;
        }
        if (idx != 0xFFFFFFFFu && (firstHit || hits == 2u || hits == 4u || hits == 16u || hits == 256u))
        {
            std::fprintf(stderr,
                "[G195:state] k=%u hits=%u fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u abe=%u "
                "tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u tw=%u th=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "alpha=0x%llx test=0x%llx tex1=0x%llx clamp=0x%llx bbox=(%d,%d)-(%d,%d) "
                "rawbbox=(%d,%d)-(%d,%d) rgba2=(%u,%u,%u,%u)\n",
                idx, hits, key.fbp, key.fbw, static_cast<uint32_t>(key.fpsm),
                static_cast<uint32_t>(key.prim), static_cast<uint32_t>(key.fst),
                static_cast<uint32_t>(key.iip), static_cast<uint32_t>(key.abe),
                key.tbp, key.tbw, static_cast<uint32_t>(key.psm),
                static_cast<uint32_t>(key.tfx), static_cast<uint32_t>(key.tcc),
                static_cast<uint32_t>(key.tw), static_cast<uint32_t>(key.th), key.cbp,
                static_cast<uint32_t>(key.cpsm), static_cast<uint32_t>(key.csm),
                static_cast<uint32_t>(key.csa), static_cast<unsigned long long>(key.alpha),
                static_cast<unsigned long long>(key.test), static_cast<unsigned long long>(key.tex1),
                static_cast<unsigned long long>(key.clamp),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY,
                static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a));
        }
    }

    const bool g195DofTri =
        g195DofTraceEnabled() &&
        gs->m_vram &&
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        ctx.tex0.tbp0 >= 0x2720u &&
        ctx.tex0.tbp0 <= 0x2a80u &&
        ctx.tex0.psm == GS_PSM_T8 &&
        ctx.tex0.tcc == 1u &&
        ctx.tex0.cbp >= 0x3fb0u &&
        ctx.tex0.cbp <= 0x3fbcu &&
        (ctx.alpha == 0x44u || ctx.alpha == 0x48u || ctx.alpha == 0x800000002au) &&
        (ctx.frame.fbp == 0u || ctx.frame.fbp == 0x68u);
    if (g195DofTri)
    {
        static std::atomic<uint32_t> s_tri{0};
        const uint32_t n = s_tri.fetch_add(1u, std::memory_order_relaxed);
        if (n < 96u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                "[G195:doftri] n=%u fbp=0x%x fbw=%u fpsm=0x%x prim=%u fst=%u iip=%u "
                "tex0=0x%016llx tbw=%u tw=%u th=%u tex1=0x%llx clamp=0x%llx alpha=0x%llx test=0x%llx "
                "bbox=(%d,%d)-(%d,%d) rawbbox=(%d,%d)-(%d,%d) denom=%g "
                "v0=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u) "
                "v1=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u) "
                "v2=(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u stq=%.5g,%.5g,%.5g uv=%u,%u)\n",
                n, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.fst), static_cast<uint32_t>(gs->m_prim.iip),
                static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
                static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.tw),
                static_cast<uint32_t>(ctx.tex0.th),
                static_cast<unsigned long long>(ctx.tex1),
                static_cast<unsigned long long>(ctx.clamp),
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                minX, minY, maxX, maxY, rawMinX, rawMinY, rawMaxX, rawMaxY, denom,
                fx0, fy0, v0.z, static_cast<uint32_t>(v0.r), static_cast<uint32_t>(v0.g),
                static_cast<uint32_t>(v0.b), static_cast<uint32_t>(v0.a),
                v0.s, v0.t, v0.q, static_cast<uint32_t>(v0.u), static_cast<uint32_t>(v0.v),
                fx1, fy1, v1.z, static_cast<uint32_t>(v1.r), static_cast<uint32_t>(v1.g),
                static_cast<uint32_t>(v1.b), static_cast<uint32_t>(v1.a),
                v1.s, v1.t, v1.q, static_cast<uint32_t>(v1.u), static_cast<uint32_t>(v1.v),
                fx2, fy2, v2.z, static_cast<uint32_t>(v2.r), static_cast<uint32_t>(v2.g),
                static_cast<uint32_t>(v2.b), static_cast<uint32_t>(v2.a),
                v2.s, v2.t, v2.q, static_cast<uint32_t>(v2.u), static_cast<uint32_t>(v2.v));
        }

        bool doDump = false;
        {
            static std::mutex s_dumpMutex;
            static uint64_t s_dumpedKeys[32]{};
            static uint32_t s_dumpedN = 0;
            const uint64_t dumpKey =
                (static_cast<uint64_t>(ctx.tex0.tbp0) << 40) |
                (static_cast<uint64_t>(ctx.tex0.tbw & 0x3fu) << 32) |
                (static_cast<uint64_t>(ctx.tex0.cbp & 0x3fffu) << 16) |
                (static_cast<uint64_t>(ctx.tex0.tw & 0xfu) << 8) |
                static_cast<uint64_t>(ctx.tex0.th & 0xfu);
            std::lock_guard<std::mutex> lk(s_dumpMutex);
            bool seen = false;
            for (uint32_t i = 0; i < s_dumpedN; ++i)
            {
                if (s_dumpedKeys[i] == dumpKey)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen && s_dumpedN < 32u)
            {
                s_dumpedKeys[s_dumpedN++] = dumpKey;
                doDump = true;
            }
        }
        if (doDump)
        {
            const int dw = 1 << ctx.tex0.tw;
            const int dh = 1 << ctx.tex0.th;
            const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
            char rgbPath[128];
            char alphaPath[128];
            std::snprintf(rgbPath, sizeof(rgbPath), "D:/ps2r/dc2/captures/g195_dof_%04x_rgb.ppm", ctx.tex0.tbp0);
            std::snprintf(alphaPath, sizeof(alphaPath), "D:/ps2r/dc2/captures/g195_dof_%04x_alpha.ppm", ctx.tex0.tbp0);
            FILE *rgb = std::fopen(rgbPath, "wb");
            FILE *alpha = std::fopen(alphaPath, "wb");
            if (rgb) std::fprintf(rgb, "P6\n%d %d\n255\n", dw, dh);
            if (alpha) std::fprintf(alpha, "P6\n%d %d\n255\n", dw, dh);
            uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            uint32_t nzRgb = 0, nzA = 0, idx0 = 0, idxNon0 = 0, a80 = 0, a7fPlus = 0;
            for (int yy = 0; yy < dh; ++yy)
            {
                for (int xx = 0; xx < dw; ++xx)
                {
                    const uint8_t ix = static_cast<uint8_t>(
                        GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                      static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                    const uint32_t c = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                                  ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                    const uint8_t rr = static_cast<uint8_t>(c & 0xFFu);
                    const uint8_t gg = static_cast<uint8_t>((c >> 8) & 0xFFu);
                    const uint8_t bb = static_cast<uint8_t>((c >> 16) & 0xFFu);
                    const uint8_t aa = static_cast<uint8_t>((c >> 24) & 0xFFu);
                    sumR += rr; sumG += gg; sumB += bb; sumA += aa;
                    if ((rr | gg | bb) != 0u) ++nzRgb;
                    if (aa != 0u) ++nzA;
                    if (ix == 0u) ++idx0; else ++idxNon0;
                    if (aa == 0x80u) ++a80;
                    if (aa >= 0x7fu) ++a7fPlus;
                    if (rgb)
                    {
                        std::fputc(rr, rgb); std::fputc(gg, rgb); std::fputc(bb, rgb);
                    }
                    if (alpha)
                    {
                        std::fputc(aa, alpha); std::fputc(aa, alpha); std::fputc(aa, alpha);
                    }
                }
            }
            if (rgb) std::fclose(rgb);
            if (alpha) std::fclose(alpha);
            const uint32_t pixels = static_cast<uint32_t>(dw * dh);
            std::fprintf(stderr,
                "[G195:dofdump] tex=0x%x size=%dx%d tbw=%u pageBw=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u "
                "avg=(%llu,%llu,%llu,%llu) nzRgb=%u/%u nzA=%u/%u idx0=%u idxNon0=%u a80=%u a7fPlus=%u "
                "files=%s,%s\n",
                ctx.tex0.tbp0, dw, dh, static_cast<uint32_t>(ctx.tex0.tbw), pageBwT8, ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.cpsm), static_cast<uint32_t>(ctx.tex0.csm),
                static_cast<uint32_t>(ctx.tex0.csa),
                pixels ? static_cast<unsigned long long>(sumR / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumG / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumB / pixels) : 0ull,
                pixels ? static_cast<unsigned long long>(sumA / pixels) : 0ull,
                nzRgb, pixels, nzA, pixels, idx0, idxNon0, a80, a7fPlus,
                rgbPath, alphaPath);
        }
    }

    // G50: tally costume-model triangles per texture page (drawn vs degenerate/dropped + bbox).
    if (g50PartsEnabled() && ctx.frame.fbp == 0x139u && gs->m_prim.tme)
    {
        g50Record(ctx.tex0.tbp0, std::fabs(denom) < 0.001f,
                  fx0, fy0, fx1, fy1, fx2, fy2);
        g50DumpMaybe();
    }

    // G214: per-tick head-page triangle counter (see g214CapFlushLocked).
    if (g214CapTraceEnabled() && ctx.frame.fbp == 0x139u && gs->m_prim.tme)
    {
        extern std::atomic<uint64_t> g_dc2PresentTick; // defined in ps2_runtime.cpp
        const uint64_t tk = g_dc2PresentTick.load(std::memory_order_relaxed);
        const uint32_t tbp = ctx.tex0.tbp0;
        std::lock_guard<std::mutex> lk(g_g214Mutex);
        if (tk != g_g214Tick)
            g214CapFlushLocked(tk);
        switch (tbp)
        {
        case 0x35a0u: g_g214Cap++;  break;
        case 0x3420u: g_g214Face++; break;
        case 0x34c0u: g_g214Hair++; break;
        case 0x3480u: g_g214Head++; break;
        case 0x3460u: g_g214Body++; break;
        case 0x34a0u: g_g214Neck++; break;
        default: break;
        }
    }

    // [F51.7] Global textured-triangle classifier: where does textured 3D geometry go, and does it
    // rasterize? Logs raw (pre-offset) GS vertex coords so a collapsed transform is visible.
    // Gated DC2_TRACE_RENDER_QUALITY.
    const bool f516 = renderQualityTraceEnabled();
    const bool f53map = f516 &&
                        gs->m_prim.tme &&
                        ctx.tex0.tbp0 == 0x3220u &&
                        ctx.tex0.psm == 0x13u;
    static std::atomic<uint32_t> s_f53MapTotal{0};
    static std::atomic<uint32_t> s_f53MapPositive{0};
    static std::atomic<uint32_t> s_f53MapZero{0};
    static std::atomic<uint32_t> s_f53MapDegen{0};
    static std::atomic<uint32_t> s_f53MapSummary{0};
    int f516cov = 0;
    if (std::fabs(denom) < 0.001f)
    {
        if (f53map)
        {
            const uint32_t total = s_f53MapTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const uint32_t degen = s_f53MapDegen.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (degen <= 16u)
                std::fprintf(stderr,
                    "[F53:tri] total=%u class=degen degen=%u fbp=0x%x denom=%g "
                    "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) "
                    "of=(%d,%d)\n",
                    total, degen, ctx.frame.fbp, denom,
                    v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                    ofx, ofy);
        }
        if (f516)
        {
            static std::atomic<uint32_t> s_f517deg{0};
            if (s_f517deg.fetch_add(1u) < 60u)
                std::fprintf(stderr,
                    "[F51.7:tri] DEGEN fbp=0x%x tme=%u tbp=0x%x psm=0x%x fst=%u denom=%g "
                    "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) "
                    "st0=(%.3f,%.3f) of=(%d,%d) scissor=(%u,%u)-(%u,%u)\n",
                    ctx.frame.fbp, (unsigned)gs->m_prim.tme, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm,
                    (unsigned)gs->m_prim.fst, denom,
                    v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                    v0.s, v0.t, ofx, ofy,
                    ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
        return;
    }

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // [G42] EXPERIMENT: backface cull for the costume model RTT (fbp=0x139). The head/cap
    // renders as a "hollow face" because the concave cap's back-facing interior triangles draw
    // over the front of the face and the GEQUAL Z-test cannot separate them (back-cap Z competes
    // with face Z). PS2 culls back faces in the VU1 microprogram; the runner emits both windings.
    // DC2_G42_CULL=1 culls denom<0, =2 culls denom>0 (try both to find the back-facing sign).
    static const int s_g42cull = []() {
        const char *e = std::getenv("DC2_G42_CULL");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g42cull && ctx.frame.fbp == 0x139u && gs->m_prim.type == GS_PRIM_TRISTRIP)
    {
        if ((s_g42cull == 1 && denom < 0.0f) || (s_g42cull == 2 && denom > 0.0f))
            return;
    }

    // [G51] PARITY-AWARE backface cull for the costume model (fbp=0x139). A naive fixed-winding
    // cull (G42) shreds the model because a TRISTRIP's geometric winding ALTERNATES every triangle
    // (the GS/VU1 flips the cull test per strip-triangle). The dark face "speckle" dots are the
    // head's BACK-FACING tristrip triangles (far side of the skinned head surface) bleeding through
    // the front skin's sub-pixel coverage gaps: their interpolated vertex shade is dark (~0x38-0x60
    // vs front skin ~0x96) and their q is tiny (far) — see the [G43:sample] trace. They are NOT a
    // texture/CLUT/filtering defect (the decoded 0x3480 texture is clean; a 7x7 texel box-average
    // does not remove the dots) and NOT the decal pages (0x34a0/0x35a0 skip does not remove them).
    // Cull by the PARITY-NORMALIZED winding so all front faces share one sign. Strip parity is
    // derived header-free from vertex continuity: a strip continuation re-uses the previous
    // triangle's last two vertices as its first two (the sliding window in ps2_gs_gpu.cpp).
    static const int s_g51cull = []() {
        const char *e = std::getenv("DC2_G51_CULL");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g51cull && ctx.frame.fbp == 0x139u && gs->m_prim.type == GS_PRIM_TRISTRIP)
    {
        static float s_pv1x = 0.0f, s_pv1y = 0.0f, s_pv2x = 0.0f, s_pv2y = 0.0f;
        static bool s_have = false;
        static int s_parity = 0;
        const bool cont = s_have && v0.x == s_pv1x && v0.y == s_pv1y &&
                          v1.x == s_pv2x && v1.y == s_pv2y;
        s_parity = cont ? (s_parity ^ 1) : 0;
        s_pv1x = v1.x; s_pv1y = v1.y; s_pv2x = v2.x; s_pv2y = v2.y; s_have = true;
        const float adj = winding * (s_parity ? -1.0f : 1.0f);
        if ((s_g51cull == 1 && adj < 0.0f) || (s_g51cull == 2 && adj > 0.0f))
            return;
    }

    // G41: per-pixel depth test. The PS2 GS depth-tests every primitive via TEST.ZTE/ZTST
    // and reads/writes the Z buffer at ZBUF.ZBP. The rasterizer previously ignored Z entirely
    // (painter's order), so overlapping skinned-model tristrips drew back-over-front (the costume
    // Max "hollow face" — back of cap/hair showed through the front). Honor it now.
    // Decode once: TEST.ZTE=bit16, ZTST=bits17-18; ZBUF.ZBP=bits0-8, ZPSM=bits24-27, ZMSK=bit32.
    static const bool s_zTestDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const bool g203Uni = g203UniversalZEnabled();
    const bool g106TitleZ =
        g106TitleZScope(gs->m_prim.tme, ctx.frame.fbp, ctx.tex0.tbp0);
    // G144: skip the fbp-transition clear during a parallel replay (it uses a racy file-static
    // last-fbp and would spuriously wipe the shared title-Z mid-band). The flush pre-runs it once,
    // single-threaded, before dispatching the bands.
    // G203: in universal-Z mode the game's own clear-sprite populates VRAM Z — no artificial clear.
    if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled() && !t_g144InReplay)
        g106TitleZClearIfNeeded(ctx.frame.fbp);
    const uint32_t zte  = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    // ZBUF.ZBP is a page base; GSMem Z addressing wants a block base (page<<5), matching
    // writePixel's framePageBaseToBlock(fbp) convention.
    const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
    const uint32_t zbp  = zbpPage << 5;
    const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool zmsk     = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const uint32_t zbw  = ctx.frame.fbw;
    const bool g202TownZ = g202TownZScope(ctx.frame.fbp, zbpPage, zpsm);
    if (!g203Uni && g202TownZ && !t_g144InReplay)
        g202TownZClearIfNeeded(gs->m_vram, ctx.frame.fbp, ctx.zbuf, ctx.frame.fbw);
    // G203: universal-Z honors guest ZBUF/TEST for ALL targets against real VRAM Z; the legacy path
    // kept the per-screen whitelist (costume 0x139, title-rock, town state-scope).
    const bool zScope = g203Uni || (ctx.frame.fbp == 0x139u) || g106TitleZ || g202TownZ;
    const bool zEnabled = !s_zTestDisabled && zScope && zte != 0u && gs->m_vram != nullptr;
    const bool z16 = (zpsm == 0x2u || zpsm == 0xAu || zpsm == 0xBu); // PSMZ16 / PSMZ16S
    const bool z24 = (zpsm == 0x1u); // PSMZ24
    const bool zWrite = zEnabled && !zmsk && ztst != 0u; // ZTST=NEVER never writes
    // G219 (default-off, DC2_G219_SKYPX=1): triangle twin of the [G219:spzbp] sprite watch.
    {
        static const bool s_g219z = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219z && zWrite && zbpPage >= 0x139u && zbpPage <= 0x148u)
        {
            static std::atomic<uint32_t> s_g219tzn{0};
            const uint32_t n = s_g219tzn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u)
                std::fprintf(stderr,
                    "[G219:trzbp] n=%u fbp=0x%x zbpPage=0x%x zpsm=0x%x ztst=%u tme=%u tbp=0x%x "
                    "bbox=(%d,%d)-(%d,%d) replay=%u\n",
                    n, ctx.frame.fbp, zbpPage, zpsm, ztst,
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0,
                    minX, minY, maxX, maxY, t_g144InReplay ? 1u : 0u);
        }
    }
    const bool g44FaceZ =
        g44FaceZTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        gs->m_prim.type == GS_PRIM_TRISTRIP &&
        g44FaceZTargetTbp(ctx.tex0.tbp0) &&
        maxX >= 344 && minX <= 412 &&
        maxY >= 40 && minY <= 144;

    // G141 sub-profile: local coverage tally (flushed to the globals once after the loop, so no
    // per-pixel atomic). bbox area vs inside-count exposes bounding-box scan waste.
    long g141BboxPx = (maxX >= minX && maxY >= minY)
                          ? static_cast<long>(maxX - minX + 1) * static_cast<long>(maxY - minY + 1)
                          : 0L;
    long g141InsidePx = 0;

    // [G232] consume the gem-draw arm set by drawPrimitive for this same primitive.
    // s_g232w short-circuits first so the default path pays no atomics here.
    static const bool s_g232w = (std::getenv("DC2_G232_RTTDUMP") != nullptr);
    const bool g232tri = s_g232w && g_g232TriArm.exchange(0, std::memory_order_relaxed) != 0;
    if (g232tri && g141BboxPx > 0)
        g_g232PxBbox.fetch_add(static_cast<uint32_t>(g141BboxPx), std::memory_order_relaxed);
    const int g232wx = s_g232w ? g_g232GemX.load(std::memory_order_relaxed) : -1;
    const int g232wy = s_g232w ? g_g232GemY.load(std::memory_order_relaxed) : -1;
    // Watch every triangle touching the gem pixel on ANY target sharing the game's single Z
    // page (0xd0) — room draws (fbp 0/0x68) and character RTT draws (0x139) alike.
    const bool g232watch = s_g232w && g232wx >= 0 && zbpPage == 0xd0u;

    // G141 (eliminate-work, behavior-identical): on each scanline the inside set is a contiguous
    // interval (convex triangle + affine barycentric), so narrow the x scan to a conservative span
    // derived from the SAME edge functions instead of edge-testing every bbox pixel (measured ~70%
    // of per-pixel work was wasted on outside pixels). The per-pixel EXACT float test in the inner
    // loop is UNCHANGED; the span is computed in double and padded by floor/ceil ±1px so no pixel
    // the exact test would accept is ever skipped -> output is bit-identical (golden 211646 /
    // frame_001500.ppm byte-equal). Per-x slope of each barycentric = its px coefficient. Kill
    // with DC2_G141_NO_XSPAN=1 to restore the full-bbox scan for A/B.
    static const bool s_g141NoXspan = (std::getenv("DC2_G141_NO_XSPAN") != nullptr);
    // G141 cost-split PROBES (measurement levers, default off, render garbage when on — used only to
    // apportion the ~170 ms GS raster between the per-pixel texture sample and the pixel write before
    // refactoring either). DC2_G141_SKIP_SAMPLE bypasses sampleTexture; DC2_G141_SKIP_WRITE bypasses
    // writePixel. Delta in [G141:perf] GSraster ms = that stage's cost. Never ship enabled.
    static const bool s_g141SkipSample = (std::getenv("DC2_G141_SKIP_SAMPLE") != nullptr);
    static const bool s_g141SkipWrite = (std::getenv("DC2_G141_SKIP_WRITE") != nullptr);
    const double g141S0 = static_cast<double>(fy1 - fy2) * static_cast<double>(winding) * static_cast<double>(invAbsDenom);
    const double g141S1 = static_cast<double>(fy2 - fy0) * static_cast<double>(winding) * static_cast<double>(invAbsDenom);
    const double g141S2 = -g141S0 - g141S1;
    // G141 (drawn-pixel lever, user-approved SAFE inline+hoist): the ~68 ms/frame texture sampler
    // (skip-probe = 40% of GS raster) is a NON-inlined sampleTexture call per drawn pixel that
    // re-derefs activeContext() and re-decodes texW/clamp/wrap/psm every pixel (all per-triangle
    // invariant). For the dominant cavern case (textured, non-RTT, POINT sampling, traces off,
    // common PSM) run an inlined fast-path that hoists those invariants and reuses the EXACT leaf
    // reads (ReadP8/readTexel*/lookupCLUT/applyTexa/wrapTextureCoordinate) -> bit-identical by
    // construction. Everything else (bilinear, costume RTT fbp=0x139, Z-as-texture, non-default T8
    // config, any trace) FALLS BACK to sampleTexture. Kill DC2_G141_NO_FASTSAMPLE=1. Bit-exactness
    // is PROVEN per-pixel with DC2_G141_SAMPLE_VERIFY=1 ([G141:vfy] tot/bad; bad must stay 0).
    static const bool s_g141NoFastSample = (std::getenv("DC2_G141_NO_FASTSAMPLE") != nullptr);
    static const bool s_g141SampleVerify = (std::getenv("DC2_G141_SAMPLE_VERIFY") != nullptr);
    static const bool s_g141T8GsmemDefault =
        !(std::getenv("DC2_T8_GSMEM") && std::getenv("DC2_T8_GSMEM")[0] == '0');
    static const bool s_g141T8AliasSet = (std::getenv("DC2_T8_ALIAS_TBW") != nullptr);
    const uint8_t g141Psm = ctx.tex0.psm;
    const bool g141FastPsm =
        g141Psm == GS_PSM_CT32 || g141Psm == GS_PSM_CT24 ||
        g141Psm == GS_PSM_CT16 || g141Psm == GS_PSM_CT16S ||
        g141Psm == GS_PSM_T4 || g141Psm == GS_PSM_T4HL || g141Psm == GS_PSM_T4HH ||
        (g141Psm == GS_PSM_T8 && s_g141T8GsmemDefault && !s_g141T8AliasSet);
    const int g141TexW = 1 << ctx.tex0.tw;
    const int g141TexH = 1 << ctx.tex0.th;
    const uint8_t g141WrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t g141WrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    const int g141MinU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
    const int g141MaxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
    const int g141MinV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
    const int g141MaxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);
    const bool g141TraceOff =
        !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() && !f50_12_trace_enabled();
    // The cavern is BILINEAR (mmag=1) — sampleTexture's 4-tap path is the bulk of the 68 ms — so the
    // fast path covers both POINT and BILINEAR (g141Linear), matching sampleTexture's own selection
    // (for fbp!=0x139: point iff !tex1UsesLinearFilter, else bilinear; box-filter/g45ForceLinear are
    // fbp==0x139-only and excluded here).
    const bool g141Linear = tex1UsesLinearFilter(ctx.tex1);
    const bool g141FastSampleTri =
        !s_g141NoFastSample && gs->m_prim.tme && g141TraceOff &&
        ctx.frame.fbp != 0x139u && g141FastPsm && ctx.tex0.tbw != 0u;
    // G141 one-shot: report WHY textured non-RTT tris do/don't take the fast path (DC2_G141_FASTDIAG).
    {
        static const bool s_g141FastDiag = (std::getenv("DC2_G141_FASTDIAG") != nullptr);
        if (s_g141FastDiag && gs->m_prim.tme && ctx.frame.fbp != 0x139u)
        {
            static std::atomic<uint32_t> s_fd{0};
            const uint32_t k = s_fd.fetch_add(1u, std::memory_order_relaxed);
            if (k < 24u)
                std::fprintf(stderr,
                    "[G141:fastdiag] k=%u fbp=0x%x psm=0x%x tbw=%u tex1=0x%llx mmag=%u mmin=%u "
                    "fastPsm=%u linear=%u tbw0=%u traceOff=%u -> fast=%u\n",
                    k, ctx.frame.fbp, (unsigned)g141Psm, (unsigned)ctx.tex0.tbw,
                    (unsigned long long)ctx.tex1,
                    (unsigned)((ctx.tex1 >> 5) & 0x1u), (unsigned)((ctx.tex1 >> 6) & 0x7u),
                    (unsigned)g141FastPsm, (unsigned)tex1UsesLinearFilter(ctx.tex1),
                    (unsigned)(ctx.tex0.tbw == 0u), (unsigned)g141TraceOff,
                    (unsigned)g141FastSampleTri);
        }
    }
    // G142 (drawn-pixel eliminate-work, SAFE/bit-exact): per-triangle CLUT decode cache. The
    // bilinear cavern samples 4 CLUT-decoded taps/pixel (g141FastSample -> 4x g141SamplePoint ->
    // lookupCLUT = resolveClutIndex + VRAM CLUT read + applyTexa). The CLUT decode is per-triangle
    // INVARIANT (GS state + CLUT VRAM are constant within a primitive), so memoize each distinct
    // texel index ONCE per triangle: g141ClutLookup(idx) == lookupCLUT(gs, idx, ...) by construction
    // (proven per-pixel by DC2_G141_SAMPLE_VERIFY). Lazy decode-on-first-use => never issues more
    // lookupCLUT than the uncached path (safe for point/small tris too); caps at <=256 (T8) / 16 (T4)
    // decodes/tri vs 4*~230 today. Fresh per triangle => NO cross-draw staleness, no invalidation
    // needed. Kill DC2_G141_NO_CLUTCACHE=1.
    static const bool s_g141NoClutCache = (std::getenv("DC2_G141_NO_CLUTCACHE") != nullptr);
    // G148 sampler-stat gate (measurement only; see the g_g148* globals). Captured once per triangle.
    static const bool s_g148SampleStat = (std::getenv("DC2_G148_SAMPLESTAT") != nullptr);
    const bool g148Stat = s_g148SampleStat;
    const bool g141Paletted =
        g141Psm == GS_PSM_T8 || g141Psm == GS_PSM_T4 ||
        g141Psm == GS_PSM_T4HL || g141Psm == GS_PSM_T4HH;
    const bool g141UseClutCache = g141FastSampleTri && g141Paletted && !s_g141NoClutCache;
    // G143: thread_local so row-parallel lanes each get their own cache; reset per lane in g143RangeFn.
    static thread_local uint32_t g141Clut[256];
    static thread_local uint8_t g141ClutValid[256];
    auto g141ClutLookup = [&](uint8_t idx) -> uint32_t {
        if (!g141ClutValid[idx])
        {
            g141Clut[idx] = lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm,
                                       ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            g141ClutValid[idx] = 1u;
        }
        return g141Clut[idx];
    };
    // G149 (perf, DEFAULT OFF): the decoded, CLUT-pre-applied buffer for THIS triangle's texture is
    // built ONCE at DEFER time on the single guest thread (see g149BuildForDefer at the tilebin defer
    // site) and handed to every band lane via a thread_local, so the read below is LOCK-FREE — no
    // mutex on the hot path. t_g149Decoded is non-null only inside a G144 tilebin replay of an
    // eligible fast-sampler tri; nullptr for direct/non-tilebin draws → the normal sampler runs.
    static const bool s_g149Verify = (std::getenv("DC2_G149_TCVERIFY") != nullptr);
    const uint32_t *g149Px = t_g149Px; // eager-decoded read-only buffer (null unless texcache replay)
    // Inner point sampler = sampleTexture's samplePoint lambda (wrap + format read), leaves reused.
    auto g141SamplePoint = [&](int su0, int sv0) -> uint32_t {
        if (g148Stat) g_g148LeafReads.fetch_add(1u, std::memory_order_relaxed);
        const int su = wrapTextureCoordinate(su0, g141WrapU, g141TexW, g141MinU, g141MaxU);
        const int sv = wrapTextureCoordinate(sv0, g141WrapV, g141TexH, g141MinV, g141MaxV);
        // G149 fast path: single read-only linear lookup of the pre-decoded, CLUT-pre-applied buffer.
        const size_t g149Idx = static_cast<size_t>(sv) * static_cast<size_t>(g141TexW) + static_cast<size_t>(su);
        if (g149Px && !s_g149Verify)
            return g149Px[g149Idx];
        uint32_t leaf;
        switch (g141Psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
            leaf = applyTexa(gs->m_texa, g141Psm, readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv));
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            leaf = applyTexa(gs->m_texa, g141Psm, readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv));
            break;
        case GS_PSM_T8:
        {
            const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
            const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                    static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            leaf = g141UseClutCache ? g141ClutLookup(idx)
                                    : lookupCLUT(gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            break;
        }
        default: // T4 / T4HL / T4HH
        {
            const uint32_t idx = readTexelPSMT4(gs, ctx.tex0.tbp0, ctx.tex0.tbw, su, sv);
            leaf = g141UseClutCache ? g141ClutLookup(static_cast<uint8_t>(idx))
                                    : lookupCLUT(gs, static_cast<uint8_t>(idx), ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            break;
        }
        }
        // G149 verify (DC2_G149_TCVERIFY): A/B the pre-decoded buffer vs the live leaf — bad must be 0.
        if (g149Px && s_g149Verify)
        {
            const uint32_t cached = g149Px[g149Idx];
            static std::atomic<uint64_t> s_tcTot{0}, s_tcBad{0};
            const uint64_t tt = s_tcTot.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (cached != leaf)
            {
                const uint64_t bb = s_tcBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (bb <= 16u)
                    std::fprintf(stderr, "[G149:vfy] MISMATCH cached=%08x leaf=%08x psm=0x%x su=%d sv=%d\n",
                                 cached, leaf, (unsigned)g141Psm, su, sv);
            }
            if ((tt & 0xFFFFFull) == 0ull)
                std::fprintf(stderr, "[G149:vfy] tot=%llu bad=%llu\n",
                             (unsigned long long)tt, (unsigned long long)s_tcBad.load(std::memory_order_relaxed));
        }
        return leaf;
    };
    // G142 texel-quad cache (bilinear MAGNIFIED textures, e.g. the cavern MMAG=1): consecutive
    // pixels along a scanline map to the SAME 4 taps (u0,v0 unchanged), so cache the 4 corner
    // colours keyed on (u0,v0) and skip the 4x wrap+ReadP8+CLUT when unchanged (the per-pixel
    // fx/fy lerp still runs). Reuse ONLY on identical tap coords => bit-exact (proven by
    // DC2_G141_SAMPLE_VERIFY). Fresh per triangle. Kill DC2_G141_NO_TEXQUAD=1.
    static const bool s_g141NoTexQuad = (std::getenv("DC2_G141_NO_TEXQUAD") != nullptr);
    // G143: thread_local (per row-parallel lane); g141QValid reset per row in g143RangeFn.
    static thread_local bool g141QValid;
    static thread_local int g141QU0, g141QV0;
    static thread_local uint32_t g141QC00, g141QC10, g141QC01, g141QC11;
    auto g141FastSample = [&](float s, float t, float q, uint16_t u, uint16_t v) -> uint32_t {
        if (g148Stat) g_g148TexSamples.fetch_add(1u, std::memory_order_relaxed);
        float texUf, texVf;
        if (gs->m_prim.fst)
        {
            texUf = static_cast<float>(u) / 16.0f;
            texVf = static_cast<float>(v) / 16.0f;
        }
        else
        {
            const float invQ = 1.0f / fabsQ(q);
            texUf = s * invQ * static_cast<float>(g141TexW);
            texVf = t * invQ * static_cast<float>(g141TexH);
        }
        if (!g141Linear)
        {
            if (g148Stat) g_g148PointSamp.fetch_add(1u, std::memory_order_relaxed);
            return g141SamplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
        }
        // Bilinear = sampleTexture's 4-tap path (identical float ops + lerpChannel).
        const float sampleU = texUf - 0.5f;
        const float sampleV = texVf - 0.5f;
        const int u0 = static_cast<int>(std::floor(sampleU));
        const int v0 = static_cast<int>(std::floor(sampleV));
        const int u1 = u0 + 1;
        const int v1 = v0 + 1;
        const float fx = sampleU - static_cast<float>(u0);
        const float fy = sampleV - static_cast<float>(v0);
        uint32_t c00, c10, c01, c11;
        if (!s_g141NoTexQuad && g141QValid && u0 == g141QU0 && v0 == g141QV0)
        {
            if (g148Stat) g_g148QuadHit.fetch_add(1u, std::memory_order_relaxed);
            c00 = g141QC00; c10 = g141QC10; c01 = g141QC01; c11 = g141QC11;
        }
        else
        {
            if (g148Stat) g_g148QuadMiss.fetch_add(1u, std::memory_order_relaxed);
            c00 = g141SamplePoint(u0, v0);
            c10 = g141SamplePoint(u1, v0);
            c01 = g141SamplePoint(u0, v1);
            c11 = g141SamplePoint(u1, v1);
            g141QValid = true; g141QU0 = u0; g141QV0 = v0;
            g141QC00 = c00; g141QC10 = c10; g141QC01 = c01; g141QC11 = c11;
        }
        const uint8_t rr = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu), static_cast<uint8_t>(c10 & 0xFFu),
                                       static_cast<uint8_t>(c01 & 0xFFu), static_cast<uint8_t>(c11 & 0xFFu), fx, fy);
        const uint8_t gg = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu), static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 8) & 0xFFu), static_cast<uint8_t>((c11 >> 8) & 0xFFu), fx, fy);
        const uint8_t bb = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu), static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 16) & 0xFFu), static_cast<uint8_t>((c11 >> 16) & 0xFFu), fx, fy);
        const uint8_t aa = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu), static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                       static_cast<uint8_t>((c01 >> 24) & 0xFFu), static_cast<uint8_t>((c11 >> 24) & 0xFFu), fx, fy);
        return static_cast<uint32_t>(rr) | (static_cast<uint32_t>(gg) << 8) |
               (static_cast<uint32_t>(bb) << 16) | (static_cast<uint32_t>(aa) << 24);
    };
    // G143 (perf, DEFAULT OFF): the per-row body as a lane function so a worker pool can rasterize
    // disjoint scanline ranges concurrently. Bit-exact: disjoint rows => disjoint FB/Z pixel writes;
    // the CLUT/texel-quad caches are thread_local (reset per lane / per row here); the pixel counters
    // are per-lane; cross-triangle submission order is preserved (drawTriangle still finishes before
    // the next). insideAcc/drawnAcc replace the g141InsidePx/f516cov increments so lanes don't race.
    auto g143RangeFn = [&](int y0, int y1, long &insideAcc, int &drawnAcc)
    {
        if (g141UseClutCache)
            std::memset(g141ClutValid, 0, sizeof(g141ClutValid));
        for (int y = y0; y <= y1; ++y)
        {
            g141QValid = false;
            float py = static_cast<float>(y) + 0.5f;
        int xStart = minX, xEnd = maxX;
        if (!s_g141NoXspan)
        {
            const double eps = static_cast<double>(kEdgeEpsilon);
            const double basePx = static_cast<double>(minX) + 0.5;
            const double pyd = static_cast<double>(py);
            const double w0b = ((static_cast<double>(fy1 - fy2)) * (basePx - static_cast<double>(fx2)) +
                                (static_cast<double>(fx2 - fx1)) * (pyd - static_cast<double>(fy2))) *
                               static_cast<double>(winding) * static_cast<double>(invAbsDenom);
            const double w1b = ((static_cast<double>(fy2 - fy0)) * (basePx - static_cast<double>(fx2)) +
                                (static_cast<double>(fx0 - fx2)) * (pyd - static_cast<double>(fy2))) *
                               static_cast<double>(winding) * static_cast<double>(invAbsDenom);
            const double w2b = 1.0 - w0b - w1b;
            // Solve wi(t) = wib + t*Si >= -eps for t = (x - minX). Lower/upper bounds per edge.
            double tLo = 0.0;
            double tHi = static_cast<double>(maxX - minX);
            const double tiny = 1e-9;
            const double lo0 = -eps - w0b, lo1 = -eps - w1b, lo2 = -eps - w2b;
            if (g141S0 > tiny)       { const double c = lo0 / g141S0; if (c > tLo) tLo = c; }
            else if (g141S0 < -tiny) { const double c = lo0 / g141S0; if (c < tHi) tHi = c; }
            if (g141S1 > tiny)       { const double c = lo1 / g141S1; if (c > tLo) tLo = c; }
            else if (g141S1 < -tiny) { const double c = lo1 / g141S1; if (c < tHi) tHi = c; }
            if (g141S2 > tiny)       { const double c = lo2 / g141S2; if (c > tLo) tLo = c; }
            else if (g141S2 < -tiny) { const double c = lo2 / g141S2; if (c < tHi) tHi = c; }
            double tLoC = std::floor(tLo) - 1.0;
            if (tLoC < 0.0) tLoC = 0.0;
            double tHiC = std::ceil(tHi) + 1.0;
            const double span = static_cast<double>(maxX - minX);
            if (tHiC > span) tHiC = span;
            xStart = minX + static_cast<int>(tLoC);
            xEnd = minX + static_cast<int>(tHiC);
        }
        for (int x = xStart; x <= xEnd; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;
            ++insideAcc;
            if (g232tri)
                g_g232PxInside.fetch_add(1u, std::memory_order_relaxed);

            // G41: depth test (Z is interpolated linearly in screen space by the GS).
            uint32_t zi = 0u;
            uint32_t zdst = 0u;
            bool zPass = true;
            if (zEnabled)
            {
                float zf = v0.z * w0 + v1.z * w1 + v2.z * w2;
                if (zf < 0.0f) zf = 0.0f;
                const float zmax = z16 ? 65535.0f : (z24 ? 16777215.0f : 4294967295.0f);
                if (zf > zmax) zf = zmax;
                zi = static_cast<uint32_t>(zf);
                // G45: costume model (fbp=0x139) uses the private, non-aliasing Z buffer.
                // G203: universal-Z reads the real VRAM Z for every target (private buffers retired).
                if (!g203Uni && ctx.frame.fbp == 0x139u)
                    zdst = costumeZRead(x, y);
                else if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled())
                    zdst = g106TitleZRead(x, y, ctx.frame.fbp);
                else
                    zdst = z16
                        ? GSMem::ReadZ16(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                        : (z24
                            ? GSMem::ReadZ24(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                            : GSMem::ReadZ32(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                switch (ztst)
                {
                case 0:  zPass = false; break;            // NEVER
                case 1:  zPass = true;  break;            // ALWAYS
                case 2:  zPass = (zi >= zdst); break;     // GEQUAL
                default: zPass = (zi >  zdst); break;     // GREATER
                }
            }
            // G219 (default-off, DC2_G219_SKYPX=1): MAP-4 zoom top-right black region. Trace the
            // sky-dome pixels (display fbp, tbp=0x2820) inside the black rectangle (x>=417,
            // y<=140) — zi vs zdst discriminates "sky fails Z" from "sky never rasterized here".
            {
                static const bool s_g219 = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219 &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    gs->m_prim.tme && ctx.tex0.tbp0 == 0x2820u &&
                    x >= 417 && y <= 140 && ((x & 7) == 1) && ((y & 7) == 1))
                {
                    static std::atomic<uint32_t> s_g219n{0};
                    const uint32_t n = s_g219n.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u || (n % 4096u) == 0u)
                        std::fprintf(stderr,
                            "[G219:skypx] n=%u xy=(%d,%d) fbp=0x%x prim=%u zen=%u ztst=%u "
                            "zi=%u zdst=%u pass=%u replay=%u vz=(%.0f,%.0f,%.0f)\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.type),
                            static_cast<uint32_t>(zEnabled), ztst, zi, zdst,
                            static_cast<uint32_t>(zPass), t_g144InReplay ? 1u : 0u,
                            v0.z, v1.z, v2.z);
                }
            }
            if (g106TitleZ && g106TitleZTraceEnabled())
            {
                static std::atomic<uint64_t> s_g106Test{0};
                static std::atomic<uint64_t> s_g106Reject{0};
                const uint64_t tn = s_g106Test.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (!zPass)
                    s_g106Reject.fetch_add(1u, std::memory_order_relaxed);
                if (tn <= 32u || (tn & 0x3FFFFull) == 0ull)
                    std::fprintf(stderr,
                        "[G106:zpx] n=%llu fbp=0x%x tbp=0x%x prim=%u xy=(%d,%d) "
                        "zen=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u private=%u "
                        "zi=%u zdst=%u pass=%u reject=%llu\n",
                        static_cast<unsigned long long>(tn), ctx.frame.fbp, ctx.tex0.tbp0,
                        static_cast<uint32_t>(gs->m_prim.type), x, y,
                        static_cast<uint32_t>(zEnabled), ztst, zbp, zbw, zpsm,
                        static_cast<uint32_t>(zmsk),
                        g106TitleZPrivateEnabled() ? 1u : 0u,
                        zi, zdst, static_cast<uint32_t>(zPass),
                        static_cast<unsigned long long>(s_g106Reject.load(std::memory_order_relaxed)));
            }
            if (g195ForegroundTraceTarget(ctx, x, y) && ctx.tex0.tbp0 == 0x2a20u)
            {
                static std::atomic<uint32_t> s_g195Zpx{0};
                const uint32_t zn = s_g195Zpx.fetch_add(1u, std::memory_order_relaxed);
                if (zn < 512u)
                {
                    std::fprintf(stderr,
                        "[G195:zpx] n=%u xy=(%d,%d) fbp=0x%x tbp=0x%x prim=%u "
                        "zen=%u scopeTown=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u "
                        "zi=%u zdst=%u pass=%u z=(%.0f,%.0f,%.0f)\n",
                        zn, x, y, ctx.frame.fbp, ctx.tex0.tbp0,
                        static_cast<uint32_t>(gs->m_prim.type),
                        static_cast<uint32_t>(zEnabled), static_cast<uint32_t>(g202TownZ),
                        ztst, zbp, zbw, zpsm, static_cast<uint32_t>(zmsk),
                        zi, zdst, static_cast<uint32_t>(zPass),
                        v0.z, v1.z, v2.z);
                    std::fflush(stderr);
                }
            }
            if (g44FaceZ &&
                x >= 344 && x <= 412 && y >= 40 && y <= 144 &&
                (static_cast<uint32_t>(x) & 3u) == 0u &&
                (static_cast<uint32_t>(y) & 3u) == 0u)
            {
                static std::atomic<uint32_t> s_g44z{0};
                const uint32_t zn = s_g44z.fetch_add(1u, std::memory_order_relaxed);
                if (zn < 5000u)
                {
                    std::fprintf(stderr,
                        "[G44:zpx] n=%u xy=(%d,%d) tbp=0x%x psm=0x%x prim=%u abe=%u fst=%u "
                        "zen=%u ztst=%u zbp=0x%x zbw=%u zpsm=0x%x zmsk=%u zi=%u zdst=%u pass=%u "
                        "w=(%.3f,%.3f,%.3f) vz=(%.0f,%.0f,%.0f) bbox=(%d,%d)-(%d,%d)\n",
                        zn, x, y, ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.psm),
                        static_cast<uint32_t>(gs->m_prim.type), static_cast<uint32_t>(gs->m_prim.abe),
                        static_cast<uint32_t>(gs->m_prim.fst),
                        static_cast<uint32_t>(zEnabled), ztst, zbp, zbw, zpsm,
                        static_cast<uint32_t>(zmsk), zi, zdst, static_cast<uint32_t>(zPass),
                        w0, w1, w2, v0.z, v1.z, v2.z, minX, minY, maxX, maxY);
                }
            }
            if (g232tri)
            {
                const uint32_t zl = g_g232PxZlog.fetch_add(1u, std::memory_order_relaxed);
                if (zl < 16u)
                    std::fprintf(stderr,
                        "[G232:gempx] xy=(%d,%d) zen=%u ztst=%u zi=%u zdst=%u pass=%u zbp=0x%x zbw=%u zpsm=0x%x\n",
                        x, y, static_cast<uint32_t>(zEnabled), ztst, zi, zdst,
                        static_cast<uint32_t>(zPass), zbp, zbw, zpsm);
                if (!zPass)
                    g_g232PxZfail.fetch_add(1u, std::memory_order_relaxed);
            }
            if (g232watch && x == g232wx && y == g232wy)
            {
                static std::atomic<uint32_t> s_g232zh{0};
                const uint32_t n = s_g232zh.fetch_add(1u, std::memory_order_relaxed);
                if (n < 500u)
                    std::fprintf(stderr,
                        "[G232:zhist] xy=(%d,%d) fbp=0x%x tbp=0x%x prim=%u tme=%u abe=%u zen=%u ztst=%u "
                        "zi=%u zdst=%u pass=%u zwr=%u gem=%u\n",
                        x, y, ctx.frame.fbp, ctx.tex0.tbp0, (unsigned)gs->m_prim.type,
                        (unsigned)gs->m_prim.tme, (unsigned)gs->m_prim.abe,
                        (unsigned)zEnabled, ztst, zi, zdst, (unsigned)zPass,
                        (unsigned)zWrite, g232tri ? 1u : 0u);
            }
            if (!zPass)
                continue;

            uint8_t r, g, b, a;
            if (gs->m_prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (gs->m_prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (gs->m_prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // G222: real GS STQ semantics — S/T/Q interpolate LINEARLY in screen space and
                    // the single divide happens per pixel in the sampler (u = lerp(S)/lerp(Q)*texW).
                    // The old path divided per VERTEX then re-multiplied; the iq factors cancel to a
                    // pure AFFINE interpolation of s/q, visible as bent texture seams at a quad's
                    // shared diagonal on large deep triangles (MAP-2 dock floor, G222).
                    // DC2_G222_AFFINE_TEX=1 restores the old affine behavior.
                    static const bool s_g222Affine = envFlagEnabled("DC2_G222_AFFINE_TEX");
                    if (s_g222Affine)
                    {
                        const float invQ0 = 1.0f / fabsQ(v0.q);
                        const float invQ1 = 1.0f / fabsQ(v1.q);
                        const float invQ2 = 1.0f / fabsQ(v2.q);
                        const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                        const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                        const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                        iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                        is = sOverQ * iq;
                        it = tOverQ * iq;
                    }
                    else
                    {
                        is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                        it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                        iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    }
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel;
                if (s_g141SkipSample)
                {
                    texel = 0xFFFFFFFFu;
                }
                else if (g141FastSampleTri)
                {
                    texel = g141FastSample(is, it, iq, iu, iv);
                    if (s_g141SampleVerify)
                    {
                        const uint32_t ref = sampleTexture(gs, is, it, iq, iu, iv);
                        static std::atomic<uint64_t> s_vTot{0}, s_vBad{0};
                        const uint64_t tt = s_vTot.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (ref != texel)
                        {
                            const uint64_t bb = s_vBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (bb <= 16u)
                                std::fprintf(stderr, "[G141:vfy] MISMATCH ref=%08x fast=%08x psm=0x%x fst=%u\n",
                                             ref, texel, (unsigned)g141Psm, (unsigned)gs->m_prim.fst);
                        }
                        if ((tt & 0xFFFFFull) == 0ull)
                            std::fprintf(stderr, "[G141:vfy] tot=%llu bad=%llu\n",
                                         (unsigned long long)tt,
                                         (unsigned long long)s_vBad.load(std::memory_order_relaxed));
                    }
                }
                else
                {
                    texel = sampleTexture(gs, is, it, iq, iu, iv);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);
                // G82: title rock renders bright green instead of brown. Log sampled texel vs
                // per-vertex shade vs MODULATE output for rock-range textures so we can tell a
                // texel-wrong fault (texture data / CLUT / UV) from a shade-wrong fault (vertex
                // colour). HW rock: T8 texel brown/grey, shade greenish-grey (46,69,61). Gated.
                {
                    static const bool s_g82rock = (std::getenv("DC2_G82_ROCK") != nullptr);
                    if (s_g82rock && gs->m_prim.tme && ctx.frame.fbp != 0x139u &&
                        ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u)
                    {
                        // Aggregate avg texel + avg shade per (tbp,fbp) so we can compare the
                        // runner's per-vertex shade to HW (g82_gs_verts.py) apples-to-apples per
                        // texture. Periodically dump the table.
                        static std::mutex s_g82mtx;
                        struct Acc { uint32_t tbp,fbp; uint64_t n, tr,tg,tb, sr,sg,sb, alo,ahi; };
                        static Acc s_acc[96]{};
                        static uint32_t s_accN = 0;
                        std::lock_guard<std::mutex> lk(s_g82mtx);
                        uint32_t idx = 0xFFFFFFFFu;
                        for (uint32_t i = 0; i < s_accN; ++i)
                            if (s_acc[i].tbp == ctx.tex0.tbp0 && s_acc[i].fbp == ctx.frame.fbp) { idx = i; break; }
                        if (idx == 0xFFFFFFFFu && s_accN < 96u) { idx = s_accN++; s_acc[idx].tbp = ctx.tex0.tbp0; s_acc[idx].fbp = ctx.frame.fbp; }
                        if (idx != 0xFFFFFFFFu) {
                            Acc &A = s_acc[idx];
                            A.n++; A.tr += tr; A.tg += tg; A.tb += tb; A.sr += r; A.sg += g; A.sb += b;
                            if (A.n == 1) { A.alo = A.ahi = ta; } else { if (ta < A.alo) A.alo = ta; if (ta > A.ahi) A.ahi = ta; }
                        }
                        static uint64_t s_tick = 0;
                        if ((++s_tick % 200000ull) == 0ull)
                            for (uint32_t i = 0; i < s_accN; ++i) {
                                Acc &A = s_acc[i]; if (!A.n) continue;
                                std::fprintf(stderr, "[G82:rockavg] fbp=0x%x tbp=0x%x n=%llu texel=(%llu,%llu,%llu) shade=(%llu,%llu,%llu) ta=[%llu,%llu]\n",
                                    A.fbp, A.tbp, (unsigned long long)A.n,
                                    (unsigned long long)(A.tr/A.n),(unsigned long long)(A.tg/A.n),(unsigned long long)(A.tb/A.n),
                                    (unsigned long long)(A.sr/A.n),(unsigned long long)(A.sg/A.n),(unsigned long long)(A.sb/A.n),
                                    (unsigned long long)A.alo,(unsigned long long)A.ahi);
                            }
                    }
                }
                if (g43FaceUv && x >= 300 && x <= 450 && y >= 90 && y <= 180)
                {
                    static std::atomic<uint32_t> s_g43sample{0};
                    const uint32_t sn = s_g43sample.fetch_add(1u, std::memory_order_relaxed);
                    if (sn < 240u)
                    {
                        std::fprintf(stderr,
                            "[G43:sample] n=%u xy=(%d,%d) w=(%.3f,%.3f,%.3f) uv=(%u,%u) stq=(%.5g,%.5g,%.5g) "
                            "texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x\n",
                            sn, x, y, w0, w1, w2,
                            (unsigned)(iu >> 4), (unsigned)(iv >> 4),
                            is, it, iq,
                            tr, tg, tb, ta,
                            shadeR, shadeG, shadeB, shadeA,
                            color.r, color.g, color.b, color.a);
                    }
                }
                if (phaseDiagnosticsEnabled())
                {
                    static std::atomic<uint32_t> s_f31TriTexSampleCount{0};
                    if (s_f31TriTexSampleCount.load(std::memory_order_relaxed) < 64u)
                    {
                        const uint32_t n = s_f31TriTexSampleCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u)
                        {
                            const uint64_t tex0Raw =
                                static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) |
                                (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 14) |
                                (static_cast<uint64_t>(tex.psm & 0x3Fu) << 20) |
                                (static_cast<uint64_t>(tex.tw & 0xFu) << 26) |
                                (static_cast<uint64_t>(tex.th & 0xFu) << 30) |
                                (static_cast<uint64_t>(tex.tcc & 0x1u) << 34) |
                                (static_cast<uint64_t>(tex.tfx & 0x3u) << 35) |
                                (static_cast<uint64_t>(tex.cbp & 0x3FFFu) << 37) |
                                (static_cast<uint64_t>(tex.cpsm & 0xFu) << 51) |
                                (static_cast<uint64_t>(tex.csm & 0x1u) << 55) |
                                (static_cast<uint64_t>(tex.csa & 0x1Fu) << 56) |
                                (static_cast<uint64_t>(tex.cld & 0x7u) << 61);
                            std::fprintf(stderr,
                                         "[F31:tritex] n=%u xy=(%d,%d) w=(%.3f,%.3f,%.3f) tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tfx=%u tcc=%u cbp=0x%x texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x out=%02x,%02x,%02x,%02x st=(%.4f,%.4f,%.4f) uv=(%u,%u)\n",
                                         n, x, y, w0, w1, w2,
                                         static_cast<unsigned long long>(tex0Raw),
                                         tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                         static_cast<uint32_t>(tex.psm),
                                         static_cast<uint32_t>(tex.tfx),
                                         static_cast<uint32_t>(tex.tcc),
                                         tex.cbp,
                                         tr, tg, tb, ta,
                                         shadeR, shadeG, shadeB, shadeA,
                                         color.r, color.g, color.b, color.a,
                                         is, it, iq, iu, iv);
                        }
                    }
                }

                if (g195DofTri)
                {
                    static std::atomic<uint32_t> s_g195Px{0};
                    const uint32_t pn = s_g195Px.fetch_add(1u, std::memory_order_relaxed);
                    const bool targetPx =
                        (x == 170 && y == 160) ||
                        (x == 250 && y == 180) ||
                        (x == 380 && y == 160) ||
                        (x == 230 && y == 300) ||
                        (x == 400 && y == 300);
                    if (targetPx || pn < 160u || (pn % 4096u) == 0u)
                    {
                        const bool linear = tex1UsesLinearFilter(ctx.tex1);
                        float texUf = 0.0f, texVf = 0.0f;
                        if (gs->m_prim.fst)
                        {
                            texUf = static_cast<float>(iu) / 16.0f;
                            texVf = static_cast<float>(iv) / 16.0f;
                        }
                        else
                        {
                            const float invQ = 1.0f / fabsQ(iq);
                            texUf = is * invQ * static_cast<float>(g141TexW);
                            texVf = it * invQ * static_cast<float>(g141TexH);
                        }

                        const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
                        auto readIdx = [&](int su0, int sv0, uint8_t &ix, uint32_t &col, int &su, int &sv) {
                            su = wrapTextureCoordinate(su0, g141WrapU, g141TexW, g141MinU, g141MaxU);
                            sv = wrapTextureCoordinate(sv0, g141WrapV, g141TexH, g141MinV, g141MaxV);
                            ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, ctx.tex0.tbp0, pageBwT8,
                                                                     static_cast<uint32_t>(su),
                                                                     static_cast<uint32_t>(sv)));
                            col = lookupCLUT(gs, ix, ctx.tex0.cbp, ctx.tex0.cpsm,
                                             ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                        };

                        int su0 = 0, sv0 = 0, su1 = 0, sv1 = 0, su2 = 0, sv2 = 0, su3 = 0, sv3 = 0;
                        uint8_t ix0 = 0, ix1 = 0, ix2 = 0, ix3 = 0;
                        uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                        float fx = 0.0f, fy = 0.0f;
                        if (linear)
                        {
                            const float sampleU = texUf - 0.5f;
                            const float sampleV = texVf - 0.5f;
                            const int u0 = static_cast<int>(std::floor(sampleU));
                            const int v0s = static_cast<int>(std::floor(sampleV));
                            fx = sampleU - static_cast<float>(u0);
                            fy = sampleV - static_cast<float>(v0s);
                            readIdx(u0,     v0s,     ix0, c0, su0, sv0);
                            readIdx(u0 + 1, v0s,     ix1, c1, su1, sv1);
                            readIdx(u0,     v0s + 1, ix2, c2, su2, sv2);
                            readIdx(u0 + 1, v0s + 1, ix3, c3, su3, sv3);
                        }
                        else
                        {
                            readIdx(static_cast<int>(texUf), static_cast<int>(texVf), ix0, c0, su0, sv0);
                        }

                        const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
                        const uint32_t bytesPerPixel =
                            (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S) ? 2u : 4u;
                        uint32_t off = 0u;
                        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
                        {
                            off = GSPSMCT32::addrPSMCT32(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                                         widthBlocks,
                                                         static_cast<uint32_t>(x),
                                                         static_cast<uint32_t>(y));
                        }
                        else
                        {
                            off = addrPSMCT16Family(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                                    widthBlocks,
                                                    ctx.frame.psm,
                                                    static_cast<uint32_t>(x),
                                                    static_cast<uint32_t>(y));
                        }

                        uint32_t dst = 0u;
                        if (off + bytesPerPixel <= gs->m_vramSize)
                        {
                            if (bytesPerPixel == 2u)
                            {
                                uint16_t packed = 0u;
                                std::memcpy(&packed, gs->m_vram + off, 2);
                                dst = decodePSMCT16(packed);
                            }
                            else
                            {
                                std::memcpy(&dst, gs->m_vram + off, 4);
                            }
                        }

                        const AlphaTestResult at = classifyAlphaTest(ctx.test, color.a);
                        uint8_t finalR = color.r, finalG = color.g, finalB = color.b, finalA = color.a;
                        const uint8_t dr = static_cast<uint8_t>(dst & 0xFFu);
                        const uint8_t dg = static_cast<uint8_t>((dst >> 8) & 0xFFu);
                        const uint8_t db = static_cast<uint8_t>((dst >> 16) & 0xFFu);
                        const uint8_t da = static_cast<uint8_t>((dst >> 24) & 0xFFu);
                        if (at.writeFramebuffer && gs->m_prim.abe &&
                            !(gs->m_pabe && (color.a & 0x80u) == 0u))
                        {
                            const uint64_t alphaReg = ctx.alpha;
                            const uint8_t asel = alphaReg & 3u;
                            const uint8_t bsel = (alphaReg >> 2) & 3u;
                            const uint8_t csel = (alphaReg >> 4) & 3u;
                            const uint8_t dsel = (alphaReg >> 6) & 3u;
                            const uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
                            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int {
                                if (sel == 0u) return cs;
                                if (sel == 1u) return cd;
                                return 0;
                            };
                            const int cAlpha = (csel == 0u) ? color.a : (csel == 1u) ? da : fix;
                            finalR = clampU8(((pickRGB(asel, color.r, dr) - pickRGB(bsel, color.r, dr)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.r, dr));
                            finalG = clampU8(((pickRGB(asel, color.g, dg) - pickRGB(bsel, color.g, dg)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.g, dg));
                            finalB = clampU8(((pickRGB(asel, color.b, db) - pickRGB(bsel, color.b, db)) * cAlpha >> 7) +
                                             pickRGB(dsel, color.b, db));
                        }
                        if (!at.preserveDestinationAlpha &&
                            (ctx.fba & 0x1ull) != 0ull &&
                            ctx.frame.psm != GS_PSM_CT24)
                        {
                            finalA = static_cast<uint8_t>(finalA | 0x80u);
                        }
                        uint32_t finalPixel = static_cast<uint32_t>(finalR) |
                            (static_cast<uint32_t>(finalG) << 8) |
                            (static_cast<uint32_t>(finalB) << 16) |
                            (static_cast<uint32_t>(finalA) << 24);
                        if (ctx.frame.fbmsk != 0u && off + bytesPerPixel <= gs->m_vramSize)
                            finalPixel = (finalPixel & ~ctx.frame.fbmsk) | (dst & ctx.frame.fbmsk);
                        if (at.preserveDestinationAlpha)
                            finalPixel = (finalPixel & 0x00FFFFFFu) | (dst & 0xFF000000u);

                        std::fprintf(stderr,
                            "[G195:dofpx] n=%u target=%u xy=(%d,%d) fbp=0x%x off=0x%x prim=%u tbp=0x%x cbp=0x%x "
                            "bbox=(%d,%d)-(%d,%d) lin=%u texuv=(%.3f,%.3f) fx=%.3f fy=%.3f "
                            "tap0=(%d,%d ix=%u col=%08x) tap1=(%d,%d ix=%u col=%08x) "
                            "tap2=(%d,%d ix=%u col=%08x) tap3=(%d,%d ix=%u col=%08x) "
                            "texel=%02x,%02x,%02x,%02x shade=%02x,%02x,%02x,%02x src=%02x,%02x,%02x,%02x "
                            "dst=%08x drgba=%02x,%02x,%02x,%02x alpha=0x%llx test=0x%llx atWrite=%u atKeepA=%u "
                            "final=%08x w=(%.4f,%.4f,%.4f) st=(%.5g,%.5g,%.5g) uv=(%u,%u)\n",
                            pn, targetPx ? 1u : 0u, x, y, ctx.frame.fbp, off,
                            static_cast<uint32_t>(gs->m_prim.type), ctx.tex0.tbp0, ctx.tex0.cbp,
                            minX, minY, maxX, maxY, linear ? 1u : 0u, texUf, texVf, fx, fy,
                            su0, sv0, static_cast<uint32_t>(ix0), c0,
                            su1, sv1, static_cast<uint32_t>(ix1), c1,
                            su2, sv2, static_cast<uint32_t>(ix2), c2,
                            su3, sv3, static_cast<uint32_t>(ix3), c3,
                            tr, tg, tb, ta,
                            shadeR, shadeG, shadeB, shadeA,
                            color.r, color.g, color.b, color.a,
                            dst, dr, dg, db, da,
                            static_cast<unsigned long long>(ctx.alpha),
                            static_cast<unsigned long long>(ctx.test),
                            at.writeFramebuffer ? 1u : 0u,
                            at.preserveDestinationAlpha ? 1u : 0u,
                            finalPixel,
                            w0, w1, w2, is, it, iq, static_cast<uint32_t>(iu), static_cast<uint32_t>(iv));
                    }
                }

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            // G240: alpha-test failure controls framebuffer and depth writes independently.
            // The old path let writePixel suppress AFAIL=KEEP framebuffer writes, then wrote Z
            // unconditionally below. Transparent cutout texels consequently occluded later
            // primitives. Keep ZB_ONLY depth-only behavior while suppressing depth for KEEP,
            // FB_ONLY, and RGB_ONLY, matching TEST.AFAIL.
            const AlphaTestResult fragmentAlpha = classifyAlphaTest(ctx.test, a);

            // G48: the costume head sub-meshes (skin / hair / hat / dark face-shadow decals)
            // interleave, and the RTT is cleared to BLACK, so a DARK alpha-blended decal that hits
            // a still-uncovered pixel BEFORE the opaque skin reaches it locks in a dark
            // decal-over-black dot (and its Z write then rejects the later skin). On HW the opaque
            // skin covers the pixel first, then the dark decal blends onto real skin. Reproduce
            // that ordering without reordering the VU1 draw stream: at a still-black costume RTT
            // pixel, SKIP a DARK source fragment so a later brighter mesh (skin) can fill it; BRIGHT
            // fragments (skin, the brown hat) are written and made opaque by the opaque-over-black
            // path in writePixel. Distinguishing dark vs bright by source luma is what lets the hat
            // (a decal page, bright brown) survive while the dark face-shadow decals defer to skin.
            // The old G45 NoDot skipped ALL decals over black -> hid the hat. Tune: DC2_G48_DARK.
            //
            // G50: the dark-skip must be SCOPED TO THE HEAD/FACE BAND. The lower body (striped
            // trousers tbp~0x3460/0x34a0/0x34e0/0x3500/0x3540 at RTT y>=185, brown boots tbp=0x35e0
            // at y288-352) is legitimately DARK base-mesh geometry with no brighter mesh coming, so
            // a global dark-skip wrongly leaves those pixels black -> the composite shows the wall
            // background THROUGH the legs/boots (sparse "strands" = only the lighter trouser stripes
            // survive). The face dark-DECAL-over-black problem is confined to the head (RTT y<=125
            // per the G50 per-page screen bbox). Bound the skip to the head band so the lower body's
            // dark base fragments are written. Boundary env-overridable: DC2_G48_HEADY (RTT-space y).
            static const int s_g48HeadY = []() -> int {
                const char *e = std::getenv("DC2_G48_HEADY");
                return e ? std::atoi(e) : 160;
            }();
            // G50: exempt the head's opaque BASE meshes (hat/skin/hair) so their dark fragments are
            // WRITTEN (full hat, no black holes in the face); keep the skip for decal pages.
            static const bool s_g50NoExempt = (std::getenv("DC2_G50_NOEXEMPT") != nullptr);
            const bool g50BaseMesh = !s_g50NoExempt && g50HeadBaseMeshPage(ctx.tex0.tbp0);
            if (ctx.frame.fbp == 0x139u && gs->m_vram && y <= s_g48HeadY && !g50BaseMesh)
            {
                // G52: the G48 luma dark-skip was a HEURISTIC band-aid for the face "speckle" dots,
                // whose REAL cause (spurious fully-zero RGBAQ model verts) is now fixed at source in
                // ps2_gs_gpu.cpp (hold-last-color). The dark-skip is therefore obsolete AND harmful:
                // it eats dark hat-brim fragments on costume combos whose hat texture pages are not in
                // the Red-Vest-derived base-mesh exemption list (e.g. the default Hunting Cap + Denim
                // Overalls combo, ref/dumps/Select_costume.png — the brim renders with missing chunks).
                // Default OFF; the whole machinery stays env-gated for diagnostics (DC2_G48_DARK=200
                // restores the old behavior). With it off the face stays clean (G52) and the hat is full.
                static const int s_g48Dark = []() -> int {
                    const char *e = std::getenv("DC2_G48_DARK");
                    return e ? std::atoi(e) : 0;
                }();
                if (s_g48Dark > 0 &&
                    (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) < s_g48Dark)
                {
                    const uint32_t fblk = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
                    const uint32_t wblk = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
                    const uint32_t doff = GSPSMCT32::addrPSMCT32(fblk, wblk,
                                                                 static_cast<uint32_t>(x),
                                                                 static_cast<uint32_t>(y));
                    if (doff + 4u <= gs->m_vramSize)
                    {
                        uint32_t dst = 0u;
                        std::memcpy(&dst, gs->m_vram + doff, 4);
                        if ((dst & 0x00FFFFFFu) == 0u)
                            continue; // dark fragment over still-black RTT -> let skin fill first
                    }
                }
            }

            // [G51] Diagnostic: the skin (face + hands) "speckle" dots are LOW-ALPHA dark cel/toon
            // shadow-overlay fragments (interpolated vertex alpha ~0x30-0x76 vs opaque skin 0x80; see
            // the [G43:sample] trace). Skip fragments whose alpha is below DC2_G51_SKIPLOWA on the
            // costume RTT skin pages to test whether removing the overlay pass removes the dots.
            static const int s_g51LowA = []() -> int {
                const char *e = std::getenv("DC2_G51_SKIPLOWA");
                return e ? std::atoi(e) : 0;
            }();
            if (s_g51LowA > 0 && ctx.frame.fbp == 0x139u &&
                (ctx.tex0.tbp0 == 0x3480u || ctx.tex0.tbp0 == 0x3460u) &&
                static_cast<int>(a) < s_g51LowA)
            {
                continue;
            }

            // G219 (default-off, DC2_G219_SKYPX=1): color-level probe for the sky-dome trifan
            // pixels inside the black rectangle — logs the post-interpolation fragment color
            // heading into writePixel, discriminating flat-black fragments from blend loss.
            {
                static const bool s_g219c = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219c &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    ((x == 470 && y == 30) || (x == 500 && y == 100)))
                {
                    static std::atomic<uint32_t> s_g219cn{0};
                    const uint32_t n = s_g219cn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 2000u)
                        std::fprintf(stderr,
                            "[G219:skycol] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x abe=%u iip=%u "
                            "rgba=(%u,%u,%u,%u) vrgba0=(%u,%u,%u,%u) vrgba2=(%u,%u,%u,%u) replay=%u\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.tme),
                            ctx.tex0.tbp0, static_cast<uint32_t>(gs->m_prim.abe),
                            static_cast<uint32_t>(gs->m_prim.iip),
                            r, g, b, a,
                            (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a,
                            (unsigned)v2.r, (unsigned)v2.g, (unsigned)v2.b, (unsigned)v2.a,
                            t_g144InReplay ? 1u : 0u);
                }
            }
            if (g232tri)
            {
                g_g232PxWrite.fetch_add(1u, std::memory_order_relaxed);
                if (r > 150u && g < 90u && b < 110u)
                    g_g232PxRedWrite.fetch_add(1u, std::memory_order_relaxed);
            }
            if (!s_g141SkipWrite && fragmentAlpha.writeFramebuffer)
                writePixel(gs, x, y, r, g, b, a);
            // G219 (default-off, DC2_G219_SKYPX=1): identify every draw WRITING Z inside the
            // black rectangle — finds what poisons zdst so the sky's GEQUAL fails there.
            {
                static const bool s_g219w = (std::getenv("DC2_G219_SKYPX") != nullptr);
                if (s_g219w && zWrite && fragmentAlpha.writeDepth &&
                    (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u) &&
                    x >= 417 && y <= 140 && ((x & 15) == 1) && ((y & 15) == 1))
                {
                    static std::atomic<uint32_t> s_g219wn{0};
                    const uint32_t n = s_g219wn.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 400u || (n % 4096u) == 0u)
                        std::fprintf(stderr,
                            "[G219:zwr] n=%u xy=(%d,%d) fbp=0x%x tme=%u tbp=0x%x prim=%u "
                            "zi=%u replay=%u\n",
                            n, x, y, ctx.frame.fbp, static_cast<uint32_t>(gs->m_prim.tme),
                            ctx.tex0.tbp0, static_cast<uint32_t>(gs->m_prim.type), zi,
                            t_g144InReplay ? 1u : 0u);
                }
            }
            if (zWrite && fragmentAlpha.writeDepth)
            {
                // G45: costume model (fbp=0x139) writes its private, non-aliasing Z buffer so it
                // neither corrupts nor is corrupted by the menu text VRAM it would otherwise alias.
                // G203: universal-Z writes the real VRAM Z for every target (private buffers retired).
                if (!g203Uni && ctx.frame.fbp == 0x139u)
                    costumeZWrite(x, y, zi);
                else if (!g203Uni && g106TitleZ && g106TitleZPrivateEnabled())
                    g106TitleZWrite(x, y, ctx.frame.fbp, zi);
                else if (z16)
                    GSMem::WriteZ16(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
                else if (z24)
                    GSMem::WriteZ24(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
                else
                    GSMem::WriteZ32(gs->m_vram, zbp, zbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zi);
            }
            ++drawnAcc;
        }
        }
    };
    // G143 dispatch: sequential unless DC2_G143_THREADS>1 AND the triangle is big enough AND no
    // per-pixel diagnostic is active (those use shared static/atomic state that would race). The
    // costume RTT (fbp=0x139) is excluded. Bit-exact by construction; DEFAULT OFF pending a
    // full-frame golden diff before it is ever promoted to default-on.
    {
        static const int s_g143Threads = []() {
            const char *e = std::getenv("DC2_G143_THREADS");
            int n = e ? std::atoi(e) : 0;
            return n < 0 ? 0 : (n > 16 ? 16 : n);
        }();
        // NOTE: DC2_PERF is intentionally NOT a gate — under threading each lane increments its own
        // local counters (no race; the per-triangle coverage stat just reads 0), while the frame /
        // GSraster wall-clock scopes are measured on the guest thread and stay valid, so the speedup
        // is measurable with DC2_PERF=1. DC2_G82_ROCK genuinely mutates shared state per pixel → gate.
        static const bool s_g143G82 = (std::getenv("DC2_G82_ROCK") != nullptr);
        const bool g143DiagActive =
            s_g143G82 || g43FaceUv || g44FaceZ ||
            (g106TitleZ && g106TitleZTraceEnabled());
        // Thresholds env-tunable for sweeps (row-within-tri threading only pays when a lane's pixel
        // work >> the per-dispatch barrier cost; default high so only genuinely big tris thread).
        // Defaults are the measured sweet spot: threading pays down to fairly small tris here (the
        // cavern is many medium tris, not a few huge ones), and GSraster plateaus ~129 ms (from
        // ~154) at threads=8. Below this the per-dispatch barrier starts to dominate.
        static const int s_g143MinRows = []() { const char *e = std::getenv("DC2_G143_MINROWS"); return e ? std::atoi(e) : 8; }();
        static const long s_g143MinArea = []() { const char *e = std::getenv("DC2_G143_MINAREA"); return e ? std::atol(e) : 512L; }();
        const int g143Rows = maxY - minY + 1;
        const bool g143DoThread =
            s_g143Threads > 1 && !g143DiagActive && g141FastSampleTri &&
            ctx.frame.fbp != 0x139u && g143Rows >= s_g143MinRows &&
            !t_g144InReplay && // never nest G143 row-threads inside a G144 band replay (same pool)
            (static_cast<long>(g143Rows) * static_cast<long>(maxX - minX + 1)) >= s_g143MinArea;
        if (g143DoThread)
        {
            GSRowPool::instance().run(minY, maxY, s_g143Threads, [&](int a, int b) {
                long ia = 0;
                int da = 0;
                g143RangeFn(a, b, ia, da);
            });
        }
        else
        {
            g143RangeFn(minY, maxY, g141InsidePx, f516cov);
        }
    }

    // G141 sub-profile flush (one atomic add per triangle, gated).
    {
        static const bool s_g141GsPerfOn = (std::getenv("DC2_PERF") != nullptr);
        if (s_g141GsPerfOn)
        {
            g_g141BboxPx.fetch_add(static_cast<uint64_t>(g141BboxPx), std::memory_order_relaxed);
            g_g141InsidePx.fetch_add(static_cast<uint64_t>(g141InsidePx), std::memory_order_relaxed);
            g_g141DrawnPx.fetch_add(static_cast<uint64_t>(f516cov), std::memory_order_relaxed);
        }
    }

    if (f516)
    {
        static std::atomic<uint32_t> s_f517end{0};
        if (s_f517end.fetch_add(1u) < 80u)
            std::fprintf(stderr,
                "[F51.7:tri] OK fbp=0x%x tme=%u cov=%d tbp=0x%x psm=0x%x fst=%u bbox=(%d,%d)-(%d,%d) "
                "rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) rawv2=(%.1f,%.1f,%.1f) st0=(%.3f,%.3f) of=(%d,%d)\n",
                ctx.frame.fbp, (unsigned)gs->m_prim.tme, f516cov, ctx.tex0.tbp0, (unsigned)ctx.tex0.psm, (unsigned)gs->m_prim.fst,
                minX, minY, maxX, maxY,
                v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v0.s, v0.t, ofx, ofy);
    }
    if (f53map)
    {
        const uint32_t total = s_f53MapTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool positive = f516cov > 0;
        const uint32_t classCount = positive
            ? s_f53MapPositive.fetch_add(1u, std::memory_order_relaxed) + 1u
            : s_f53MapZero.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((positive && classCount <= 32u) || (!positive && classCount <= 16u))
            std::fprintf(stderr,
                "[F53:tri] total=%u class=%s classCount=%u fbp=0x%x cov=%d "
                "bbox=(%d,%d)-(%d,%d) rawv0=(%.1f,%.1f,%.1f) rawv1=(%.1f,%.1f,%.1f) "
                "rawv2=(%.1f,%.1f,%.1f) of=(%d,%d)\n",
                total, positive ? "positive" : "zero", classCount, ctx.frame.fbp, f516cov,
                minX, minY, maxX, maxY,
                v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                ofx, ofy);
        if ((total % 256u) == 0u &&
            s_f53MapSummary.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr,
                "[F53:tri-summary] total=%u positive=%u zero=%u\n",
                total,
                s_f53MapPositive.load(std::memory_order_relaxed),
                s_f53MapZero.load(std::memory_order_relaxed));
    }
}

void GSRasterizer::drawLine(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (gs->m_prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        // G270 CPU fallback: every band replays the whole Bresenham walk (so color/step state is
        // identical) but writes only its owned rows. Bands remain disjoint and submission order is
        // preserved within each band, matching the established G144 triangle/sprite contract.
        if (!t_g144Banded || (y0 >= t_g144BandY0 && y0 <= t_g144BandY1))
            writePixel(gs, x0, y0, r, g, b, a);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}
