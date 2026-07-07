#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
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
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <vector>

using namespace GSInternal;

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
        return g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
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
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false};
        case 3: // RGB_ONLY
            return {true, true};
        case 0: // KEEP
        case 2: // ZB_ONLY
        default:
            return {false, false};
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

// ===== G144 FRAME-LEVEL TILE-BINNING (DEFAULT OFF, DC2_G144_TILEBIN=N) =====
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
    GSVertex v[3];
    int minY, maxY; // screen-space bbox rows (post-scissor) for band-overlap prefilter
    G149Buf decoded; // G149: pre-decoded texture buffer (null unless texcache eligible); kept alive here
};
static std::vector<G144Entry> g_g144List;       // reused across frames (clear keeps capacity)
static int g_g144Threads = -1;                  // -1 = env not read yet; 0/1 = off; >1 = lanes
static uint32_t g_g144BlockFbp = 0xFFFFFFFFu;   // fbp of the current deferred block (flush on change)
static GS *g_g144LastGs = nullptr;              // live GS for VRAM at flush (set each capture)
// Flush closures, assigned ONCE inside GSRasterizer::drawPrimitive so they keep that member's friend
// access to GS/private drawTriangle even when invoked later from g144FlushPending() (frame end).
static std::function<void()> g_g144FlushFn;     // PARALLEL band replay (mid-frame, on guest thread)
static std::function<void()> g_g144FlushSeqFn;  // SEQUENTIAL drain (frame end; no pool, full range)
// Replay control (thread_local: each band lane its own; set only during a flush).
static thread_local bool t_g144InReplay = false;
static thread_local bool t_g144Banded = false;
static thread_local int t_g144BandY0 = 0;
static thread_local int t_g144BandY1 = 0;

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
    uint32_t *d = tex->px.data();
    const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
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

void GSRasterizer::drawPrimitive(GS *gs)
{
    static const bool s_g141PerfOn = (std::getenv("DC2_PERF") != nullptr);
    static double s_g141Sum = 0.0;
    static uint32_t s_g141Win = 0u;
    G141PerfScope g141Scope(s_g141PerfOn, &s_g141Sum, &s_g141Win, "gs.drawPrimitive", 4000u);
    g141Scope.accumNs = &g_g141GsRasterNs;

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
    // [G37] The costume preview's RTT black-fill clear (untextured black sprite to the
    // 0x139/0x2720 work page) is collapsed to an 18px center point because the model's VU0
    // AABB->screen projection in GetDrawRect degenerates. The composite then samples the
    // uncleared remainder of the 0x2720 page, which still holds the time-shared font/menu
    // atlas -> garbage in the preview. Reproduce what the game's clear INTENDS: zero the
    // CT32 composite region of block 0x2720 the moment that black-fill draws. GS-order is
    // clear -> model render -> composite, so the model overwrites its pixels and the rest
    // stays transparent black. G44 tightened the signature to the first full-RTT black sprite:
    // the costume path emits later smaller black sprites between face/head pages, and treating
    // those as full clears wipes Z between skin/detail/cap draws. Escape: DC2_G37_NOCLEAR.
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
        static const bool noClear = (std::getenv("DC2_G37_NOCLEAR") != nullptr);
        if (!noClear)
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
        if (ctx.tex0.psm == 0u && gs->m_vram &&
            std::getenv("DC2_G37_RTTDUMP") != nullptr &&
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

    // ===== G144 frame-level tile-binning capture/flush (DEFAULT OFF, DC2_G144_TILEBIN=N) =====
    // Runs AFTER the perf counters (so deferred tris still count) and BEFORE the switch (so the
    // flush's parallel raster time is still inside this drawPrimitive's G141PerfScope → measurable).
    if (g_g144Threads < 0)
    {
        const char *e144 = std::getenv("DC2_G144_TILEBIN");
        int n144 = e144 ? std::atoi(e144) : 0;
        g_g144Threads = (n144 < 0) ? 0 : (n144 > 16 ? 16 : n144);
    }
    if (g_g144Threads > 1 && !t_g144InReplay)
    {
        g_g144LastGs = gs;
        // Assign the flush closure ONCE (keeps this member's friend access). Parallel band replay:
        // each lane owns a disjoint scanline band and replays every overlapping entry IN SUBMISSION
        // ORDER, clipped to its band via the thread_local clamp → disjoint pixel writes,
        // order-preserved blend/Z. Bit-exact. Uses g_g144LastGs so g144FlushPending() (frame end)
        // can invoke it too.
        if (!g_g144FlushFn)
        {
            GSRasterizer *self = this;
            g_g144FlushFn = [self]() {
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                // Pre-run the title-Z fbp-transition clear ONCE, single-threaded (replay skips it).
                if (g106TitleZPrivateEnabled())
                    g106TitleZClearIfNeeded(g_g144BlockFbp);
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
                            s_rg.m_vtxQueue[0] = e.v[0];
                            s_rg.m_vtxQueue[1] = e.v[1];
                            s_rg.m_vtxQueue[2] = e.v[2];
                            if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                            else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                            self->drawTriangle(&s_rg);
                        }
                        t_g149Px = nullptr; t_g149Valid = nullptr;
                        t_g144InReplay = false;
                        t_g144Banded = false;
                    });
                }
                g_g144List.clear();
            };
            // Sequential drain (frame end): no pool, no bands — replay every entry full-range on the
            // calling (guest) thread, in order. Correctness-safe fallback for the trailing tail.
            g_g144FlushSeqFn = [self]() {
                if (g_g144List.empty() || !g_g144LastGs)
                    return;
                if (g106TitleZPrivateEnabled())
                    g106TitleZClearIfNeeded(g_g144BlockFbp);
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
                    s_seq.m_vtxQueue[0] = e.v[0];
                    s_seq.m_vtxQueue[1] = e.v[1];
                    s_seq.m_vtxQueue[2] = e.v[2];
                    if (e.decoded) { t_g149Px = e.decoded->px.data(); t_g149Valid = e.decoded->valid.data(); }
                    else { t_g149Px = nullptr; t_g149Valid = nullptr; }
                    self->drawTriangle(&s_seq);
                }
                t_g149Px = nullptr; t_g149Valid = nullptr;
                t_g144InReplay = false;
                g_g144List.clear();
            };
        }

        const uint32_t pt144 = static_cast<uint32_t>(gs->m_prim.type);
        const bool isTri144 = (pt144 == GS_PRIM_TRIANGLE || pt144 == GS_PRIM_TRISTRIP || pt144 == GS_PRIM_TRIFAN);
        const bool diagOff144 = !renderQualityTraceEnabled() && !phaseDiagnosticsEnabled() && !f50_12_trace_enabled();
        // Deferrable = a textured triangle to a DISPLAY framebuffer (0x0/0x68), never the RTT
        // (0x139) or any diagnostic build. Static-texture 3D scene → no mid-block RTT sampling.
        const bool deferrable144 = isTri144 && gs->m_prim.tme && diagOff144 &&
                                   (ctx.frame.fbp == 0x0u || ctx.frame.fbp == 0x68u);
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
            if (!g_g144List.empty() && ctx.frame.fbp != g_g144BlockFbp)
                g144TimedMidFlush(); // fbp changed → different target; drain before switching blocks
            g_g144BlockFbp = ctx.frame.fbp;
            // Screen bbox rows computed EXACTLY as drawTriangle (same ofy + scissor clamp) so the
            // band prefilter is tight and never narrower than the real coverage.
            const GSVertex &cv0 = gs->m_vtxQueue[0];
            const GSVertex &cv1 = gs->m_vtxQueue[1];
            const GSVertex &cv2 = gs->m_vtxQueue[2];
            const int cofy = ctx.xyoffset.ofy >> 4;
            const float cfy0 = cv0.y - static_cast<float>(cofy);
            const float cfy1 = cv1.y - static_cast<float>(cofy);
            const float cfy2 = cv2.y - static_cast<float>(cofy);
            const int crawMinY = static_cast<int>(std::floor(std::min({cfy0, cfy1, cfy2})));
            const int crawMaxY = static_cast<int>(std::ceil(std::max({cfy0, cfy1, cfy2})));
            G144Entry e;
            e.ctx = ctx;
            e.prim = gs->m_prim;
            e.texa = gs->m_texa;
            e.texclut = gs->m_texclut;
            e.pabe = gs->m_pabe;
            e.v[0] = cv0;
            e.v[1] = cv1;
            e.v[2] = cv2;
            e.minY = clampInt(crawMinY, ctx.scissor.y0, ctx.scissor.y1);
            e.maxY = clampInt(crawMaxY, ctx.scissor.y0, ctx.scissor.y1);
            // G149: decode this tri's texture ONCE now (guest thread), so all band lanes read it
            // lock-free at replay. Null unless texcache-enabled + eligible.
            e.decoded = g149BuildDecoded(this, gs, ctx, gs->m_texa, gs->m_vram);
            g_g144List.push_back(e);
            return; // deferred — rasterized later at flush
        }
        // Non-deferrable primitive (sprite/line/point/RTT/etc.): drain the pending list FIRST so
        // global submission order (3D under the 2D menu) is preserved, then draw this prim normally.
        if (!g_g144List.empty())
            g144TimedMidFlush();
    }

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
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
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

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

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + s_uvBias) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + s_uvBias) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (gs->m_prim.fst)
                {
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texUf * 16.0f)), 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texVf * 16.0f)), 0, 0xFFFF));
                    texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
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
                writePixel(gs, x, y, color.r, color.g, color.b, color.a);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                writePixel(gs, x, y, r, g, b, a);
    }
}

// G144: drain any trailing deferred triangles at frame end (called from the mgEndFrame override on
// the GUEST thread, before the frame is presented) so tris submitted after the last non-deferrable
// primitive still reach VRAM this frame. No-op when tile-binning is off or the list is empty. Same
// thread as capture ⇒ no data race on g_g144List.
void g144FlushPending()
{
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
            static uint64_t s_lastHit = 0, s_lastBuild = 0, s_lastInel = 0;
            if ((++s_frame % 30ull) == 0ull)
            {
                const uint64_t hit = g_g149Hits.load(std::memory_order_relaxed);
                const uint64_t build = g_g149Builds.load(std::memory_order_relaxed);
                const uint64_t inel = g_g149Ineligible.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "[G149:texcache] frame=%llu win=30 | hits/f=%llu builds/f=%llu ineligible/f=%llu gen=%llu\n",
                    (unsigned long long)s_frame,
                    (unsigned long long)((hit - s_lastHit) / 30ull),
                    (unsigned long long)((build - s_lastBuild) / 30ull),
                    (unsigned long long)((inel - s_lastInel) / 30ull),
                    (unsigned long long)g_g149VramGen.load(std::memory_order_relaxed));
                s_lastHit = hit; s_lastBuild = build; s_lastInel = inel;
            }
        }
    }
    // Sequential (no pool) — safe to call from the mgEndFrame override to drain the trailing tail.
    if (g_g144FlushSeqFn && !g_g144List.empty())
    {
        G146PerfScope g146FlushScope(g_g146G144FlushFrameNs, g_g146G144FlushFrameCount);
        g_g144FlushSeqFn();
    }
}

// PARALLEL flush for the texture-upload path (called during active guest rendering, not from the
// mgEndFrame context that races the present thread). Keeps the batch parallelism instead of forcing
// a slow single-threaded drain on every mid-frame texture upload. DC2_G144_SEQUPLOAD forces the
// sequential path (A/B / fallback if the parallel upload flush proves unsafe).
void g144FlushPendingUpload()
{
    if (g_g144List.empty())
        return;
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
        if (!g_g104TriClipReentry && !s_g89off && g_dc2TitleRockScope.load(std::memory_order_relaxed) &&
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

    // G50: tally costume-model triangles per texture page (drawn vs degenerate/dropped + bbox).
    if (g50PartsEnabled() && ctx.frame.fbp == 0x139u && gs->m_prim.tme)
    {
        g50Record(ctx.tex0.tbp0, std::fabs(denom) < 0.001f,
                  fx0, fy0, fx1, fy1, fx2, fy2);
        g50DumpMaybe();
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
    const bool g106TitleZ =
        g106TitleZScope(gs->m_prim.tme, ctx.frame.fbp, ctx.tex0.tbp0);
    // G144: skip the fbp-transition clear during a parallel replay (it uses a racy file-static
    // last-fbp and would spuriously wipe the shared title-Z mid-band). The flush pre-runs it once,
    // single-threaded, before dispatching the bands.
    if (g106TitleZ && g106TitleZPrivateEnabled() && !t_g144InReplay)
        g106TitleZClearIfNeeded(ctx.frame.fbp);
    const uint32_t zte  = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    // ZBUF.ZBP is a page base; GSMem Z addressing wants a block base (page<<5), matching
    // writePixel's framePageBaseToBlock(fbp) convention.
    const uint32_t zbp  = static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5;
    const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool zmsk     = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const uint32_t zbw  = ctx.frame.fbw;
    // Scope to the costume model RTT (fbp=0x139) for now: that is the only 3D draw target
    // validated this phase. Enabling Z globally risks the dungeon 3D map (which currently
    // renders correctly via painter order and whose per-frame Z clear the runner does not yet
    // honor on sprite clears). Generalizing needs the dungeon free-roam route validated.
    const bool zScope = (ctx.frame.fbp == 0x139u) || g106TitleZ;
    const bool zEnabled = !s_zTestDisabled && zScope && zte != 0u && gs->m_vram != nullptr;
    const bool z16 = (zpsm == 0x2u || zpsm == 0xAu || zpsm == 0xBu); // PSMZ16 / PSMZ16S
    const bool z24 = (zpsm == 0x1u); // PSMZ24
    const bool zWrite = zEnabled && !zmsk && ztst != 0u; // ZTST=NEVER never writes
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
                if (ctx.frame.fbp == 0x139u)
                    zdst = costumeZRead(x, y);
                else if (g106TitleZ && g106TitleZPrivateEnabled())
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
                    const float invQ0 = 1.0f / fabsQ(v0.q);
                    const float invQ1 = 1.0f / fabsQ(v1.q);
                    const float invQ2 = 1.0f / fabsQ(v2.q);
                    const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                    const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                    const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                    iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                    is = sOverQ * iq;
                    it = tOverQ * iq;
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

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

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

            if (!s_g141SkipWrite)
                writePixel(gs, x, y, r, g, b, a);
            if (zWrite)
            {
                // G45: costume model (fbp=0x139) writes its private, non-aliasing Z buffer so it
                // neither corrupts nor is corrupted by the menu text VRAM it would otherwise alias.
                if (ctx.frame.fbp == 0x139u)
                    costumeZWrite(x, y, zi);
                else if (g106TitleZ && g106TitleZPrivateEnabled())
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
