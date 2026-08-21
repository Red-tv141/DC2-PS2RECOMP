#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_memory.h"
#include "ps2_log.h"
#include "ps2_syscalls.h"
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
#include <limits>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <condition_variable>
#include <functional>
#include <vector>

using namespace GSInternal;

// G570 private .cpp-to-.cpp bridge. The validated 0x139 shadow batch is packed into plain word
// vectors so the GPU worker ABI stays out of the generated runtime headers. Submission is
// synchronous; every caller-owned vector therefore remains live until the worker has read it.
bool g570_backend_shadow139(const std::vector<uint32_t> &color,
                            const std::vector<uint32_t> &depth,
                            const std::vector<uint32_t> &triWords,
                            const std::vector<uint32_t> &spanWords,
                            const std::vector<uint32_t> &tileOffsets,
                            const std::vector<uint32_t> &tileTris,
                            std::vector<uint32_t> &out);

// G629 private .cpp-to-.cpp bridge, same contract as G570's above: the admitted CPU-band-replay
// batch is packed into plain word vectors, submission is synchronous, and every caller-owned
// vector stays live until the worker has read it.
//   ⛔ It MUST be declared here, at GLOBAL scope above the first part include, not beside its
//   call site. Every `.inc` below sits inside an anonymous namespace, so a declaration written
//   there names an INTERNAL-LINKAGE symbol that the backend TU's definition can never satisfy —
//   `LNK2019 ... "bool __cdecl `anonymous namespace'::g629_backend_replay(...)"`, which is exactly
//   what this cost before it was moved (the trap `appendix-dc2-project.md` §3 and the G604 fix log
//   §4.2 both name).
bool g629_backend_replay(const std::vector<uint32_t> &recs,
                         const std::vector<uint32_t> &texDesc,
                         const std::vector<uint32_t> &texData,
                         const std::vector<uint32_t> &tileOff,
                         const std::vector<uint32_t> &tileRef,
                         std::vector<uint32_t> &color,
                         std::vector<uint32_t> &depth,
                         int width, int height, int tilesX,
                         int block, int fbw, int firstPage,
                         bool wantDepth);

// G630 persistent-domain bridge. Replay and view packets own every byte until the GL worker
// retires them; poll(false) is non-blocking, while reads are explicit CPU-coherence boundaries.
bool g630_backend_submit(const std::shared_ptr<G630ReplayPacket> &packet);
bool g630_backend_prepare_ct32_view(const std::shared_ptr<G630ViewPacket> &packet);
bool g630_backend_submit_upload(const std::shared_ptr<G630UploadPacket> &packet);
bool g630_backend_prepare_t8_view(const std::shared_ptr<G630T8Packet> &packet);
bool g630_backend_poll(bool wait, std::vector<uint8_t> &results);
bool g630_backend_read_vram(std::vector<uint32_t> &out);
// G631: band form of the above. `out.size()` tells the caller whether it got the requested band
// (width*rowCount) or the whole-image fallback (width*512).
bool g630_backend_read_vram_rows(uint32_t rowLo, uint32_t rowCount, std::vector<uint32_t> &out);
bool g630_backend_read_depth(std::vector<uint32_t> &out);

// Private live PSMT8 address helper (implemented in ps2_gs_memory.cpp, intentionally absent
// from the public generated-target header).
namespace GSMem
{
std::uint32_t AddressP8(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

// G512: ROW-BATCHED swizzled readers for g178DecodeWhole's inner loop (implemented in
// ps2_gs_memory_parts/g512_texdecode_row_readers.inc, likewise absent from the public header — a
// header edit costs a 30+ hour full rebuild). MUST be declared HERE, at true global scope, and not
// inside the anonymous namespace this file opens below: a `namespace GSMem` reopened in there
// would name `(anonymous)::GSMem` and bind to a symbol that does not exist (the anonymous-namespace
// linkage trap, appendix-dc2-project.md §3).
void G512ReadRowCT32(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t *);
void G512ReadRowP8(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t *);
void G512ReadRowP4(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t *);
void G512ReadRowP4HL(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t *);
void G512ReadRowP4HH(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t *);
// G620: the PSMCT32 group reader's exactness-oracle counters (DC2_G620_VERIFY=1). Defined beside
// the reader in g512_texdecode_row_readers.inc; declared here for the same reason as the readers.
extern std::atomic<std::uint64_t> g_g620RowVerifyN;
extern std::atomic<std::uint64_t> g_g620RowVerifyBad;
}

// G512: the fast-decode arm accessor, its exactness-oracle gate and its psm census sink. All four
// are DEFINED with external linkage in g419_ab_instrument.inc, which this TU includes long after
// rasterizer_command_graph.inc — but the call site (g178DecodeWhole) sits inside that file's
// anonymous namespace, where a forward declaration would name a different, never-defined symbol.
// So they are declared HERE, in the same true-global gap GSMem::AddressP8 above uses and for the
// same reason rasterizer_command_graph.inc:30-38 hoists `int g476PageBumpArm();` out of it.
bool g512FastDecActive();
bool g512VerifyOn();
// G513: the 8-lane source-page hash's arm accessor, its decision oracle and its byte counter. Same
// anonymous-namespace linkage trap as the four below — the call sites (g178HashPages and the
// texture pass in rasterizer_vram_materialization.inc) are inside rasterizer_command_graph.inc's
// unnamed namespace, where a forward declaration would name a never-defined symbol.
bool g513HashLaneActive();
bool g513VerifyOn();
bool g512PsmCensusOn();
void g512PsmNote(std::uint8_t psm, std::uint64_t pixels, bool fast, int texW, int texH, int lanes);

// G264 private .cpp-to-.cpp bridge: alias-aware partial FBO patch. Deliberately kept out of
// ps2_gs_gpu_lle.h so no generated-target header is touched.
bool g264_backend_write_color_rect(uint32_t fbp, int texWidth, int texHeight,
                                   int glX, int glY, int width, int rows,
                                   const std::vector<uint32_t> &in);
// G627 private .cpp-to-.cpp bridge: per-byte-lane masked FBO patch. `mask` carries one byte per
// CT32 channel (non-zero => replace that byte lane from `in`), so a PSMT8 local-transfer
// destination whose boundary CT32 words are only partially covered can be reconciled into the
// resident surface without publishing it first. Declared HERE, in the true-global gap, for the
// same reason g264_backend_write_color_rect above is: the call site sits inside an anonymous
// namespace where a forward declaration would name a different, never-defined symbol (G568).
bool g627_backend_write_color_rect_masked(uint32_t fbp, int texWidth, int texHeight,
                                          int glX, int glY, int width, int rows,
                                          const std::vector<uint32_t> &in,
                                          const std::vector<uint32_t> &mask);
// G281 private .cpp-to-.cpp bridge: build one decoded RGBA texture view directly from a
// resident CT32 target FBO. `mapRgba` encodes {srcX,srcGlY,byteLane} per PSMT8 texel; the
// backend combines those bytes with `clutRgba` on-GPU and installs the result under `texKey`.
// Kept out of ps2_gs_gpu_lle.h for the project's no-header-rebuild rule.
bool g281_backend_prepare_t8_view(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                                  int texW, int texH,
                                  const std::vector<uint32_t> &mapRgba,
                                  const std::vector<uint32_t> &clutRgba,
                                  bool verify, uint64_t &verifyBad);
// G338: install a resident CT24-alpha view of a CT32 producer FBO under `texKey` (RGB copied
// from srcFbp's FBO, alpha re-derived per-texel from TEXA: a=(aem&&rgb==0)?0:ta0).
bool g338_backend_prepare_ct24_view(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                                    int texW, int texH, int ta0, int aem,
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

struct DC2NativeRenderConfig
{
    bool isConfigured = false;
    bool isNativeHighRes = false;
    bool isFullscreen = false;
    int requestedWidth = 0;
    int requestedHeight = 0;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

inline DC2NativeRenderConfig &getDC2NativeRenderConfig()
{
    static DC2NativeRenderConfig s_config = []()
    {
        DC2NativeRenderConfig cfg{};
        const char *wEnv = std::getenv("DC2_RESOLUTION_WIDTH");
        const char *hEnv = std::getenv("DC2_RESOLUTION_HEIGHT");
        const char *fsEnv = std::getenv("DC2_FULLSCREEN");

        int w = 0, h = 0;
        if (wEnv && wEnv[0] != '\0')
            w = std::atoi(wEnv);
        if (hEnv && hEnv[0] != '\0')
            h = std::atoi(hEnv);

        cfg.isFullscreen = (fsEnv && fsEnv[0] != '\0' && std::strcmp(fsEnv, "0") != 0);

        if (w > 0 && h > 0 && (w != 512 || h != 448) && (w != 640 || h != 448))
        {
            cfg.requestedWidth = w;
            cfg.requestedHeight = h;
            cfg.scaleX = static_cast<float>(w) / 640.0f;
            cfg.scaleY = static_cast<float>(h) / 448.0f;
            cfg.isNativeHighRes = true;
        }
        else if (w > 0 && h > 0)
        {
            cfg.requestedWidth = w;
            cfg.requestedHeight = h;
            cfg.scaleX = 1.0f;
            cfg.scaleY = 1.0f;
            cfg.isNativeHighRes = false;
        }
        else
        {
            cfg.requestedWidth = 640;
            cfg.requestedHeight = 448;
            cfg.scaleX = 1.0f;
            cfg.scaleY = 1.0f;
            cfg.isNativeHighRes = false;
        }
        cfg.isConfigured = true;
        return cfg;
    }();
    return s_config;
}

inline bool dc2IsNativeHighResActive()
{
    return getDC2NativeRenderConfig().isNativeHighRes;
}

inline float dc2GetNativeScaleX()
{
    return getDC2NativeRenderConfig().scaleX;
}

inline float dc2GetNativeScaleY()
{
    return getDC2NativeRenderConfig().scaleY;
}

// G141: measure-first perf scope (skill 17-performance-optimization.md), env-gated
// (DC2_PERF=1), aggregate-only print. Self-contained per-TU; drawPrimitive runs on the
// single game thread, no cross-thread sharing needed.
// G141: global GS-rasterization nanosecond accumulator (external linkage) so mgEndFrame can
// read a per-frame GS total and compute the non-overlapping GS-vs-VU1-vs-other split. The GS
// raster runs SYNCHRONOUSLY inside VU1 XGKICK, so vu1.run time double-counts it — this global
// lets the frame decomposition subtract it out.
// G434: l2l-edge workload invariant for the DC2_G434_NO_L2LPIN ceiling probe.
std::atomic<uint64_t> g_g434L2lEdges{0};
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

// G364: GS per-pixel FOG. `g_g364FogCol` is the live FOGCOL register (defined in ps2_gs_gpu.cpp);
// `t_g364FogCol` is the value snapshotted into the G144 entry, used while replaying a deferred
// tile-bin band (FOGCOL is global GS state and can change between capture and replay).
extern uint32_t g_g364FogCol;
thread_local uint32_t t_g364FogCol = 0u;

// G419: shared arm register for the within-process randomized A/B instrument (defined in
// ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc; see g419_ab_instrument.inc for the contract).
// Declared here, at global scope, because the parts that use it are inside anonymous namespaces.
extern std::atomic<uint32_t> g_g419AbArm;
extern std::atomic<uint32_t> g_g419AbUsed;

// G446 env-read census (defined at global scope in ps2_gif_arbiter.cpp; no-op unless
// DC2_G446_ENVCENSUS=1).
extern void g446NoteEnvRead(const char *name);

namespace
{

    // G446 (diagnosis only, default OFF, DC2_G446_ENVCENSUS=1): the host PC sampler measured
    // getenv() at 1.4% of ALL GS-worker samples on a thread whose span IS the frame, but a PC
    // sampler cannot name the CALLER. Bucketing by the flag NAME does, exactly: every call site
    // passes a distinct string literal, so the top row names the defective predicate directly.
    // Cost when off: one predictable branch on a `static const bool`.
    bool envFlagEnabled(const char *name)
    {
        g446NoteEnvRead(name);
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
        g446NoteEnvRead(name);
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

    // ===== G626: THE PER-PIXEL DIAGNOSTIC PREDICATE CACHE (G619's law, in the pixel loop) =====
    //
    // WHAT THE MEASUREMENT SAID. `[G605:leaf]` on `ridepod` (the route's GS is the sole pole,
    // injection sensitivity 1.05) ranks its whole CPU band replay as TWO triangle leaves:
    //
    //   discov tri-fast   65.5% of replay CPU   43.1 inside px/call    324.3 cyc/inside
    //   discov tri-untex  30.6% of replay CPU   11.8 inside px/call  **961.9 cyc/inside**
    //
    // The UNTEXTURED leaf — which does no sampling at all and should be the cheapest kernel in the
    // rasteriser — costs 3x per covered pixel what the TEXTURED one does. It costs that because
    // `tme=0` is refused by G605's span kernel (`g605EntryRejectReason`: `if (!e.prim.tme) return
    // kG608EntPrim`), so it runs the generic per-pixel nest, and that nest still pays the
    // "~20 non-inlined calls per pixel" G530 named as the actual price of a replayed pixel.
    //
    // Six of those calls are DEFAULT-OFF DIAGNOSTIC PREDICATES, and every one of them held its
    // flag in a FUNCTION-LOCAL `static const bool` — which on MSVC is an `_Init_global_epoch` TLS
    // read plus a compare on EVERY call, behind a call the compiler cannot inline away because the
    // function is defined in another part of the TU chain. That is exactly the defect G619 measured
    // at **20.7% of the whole EE thread**, one level down: here it is per PIXEL, not per node.
    //
    //   g195ForegroundTraceTarget  DC2_G195_FG_TRACE          3 calls/px (tri nest + writePixel x2)
    //   renderQualityTraceEnabled  DC2_TRACE_RENDER_QUALITY   6 calls/px (writePixel)
    //   phaseDiagnosticsEnabled    DC2_PHASE_TRACE            4 calls/px (writePixel)
    //   g407AutoScanEnabled        DC2_G407_AUTOSCAN          2 calls/px (writePixel)
    //   g43FacePixelTraceEnabled   DC2_TRACE_G43_FACE_PIXEL   1 call/px  (writePixel)
    //   g258TraceOn                DC2_G258_TRACE             1 call/px  (writePixel)
    //
    // EXACTNESS. Nothing here changes WHAT is decided, only WHERE the flag is read: the environment
    // is process-constant in every DC2 harness (G268's premise, already shipped for the VIF1/VU1
    // kick-mode reads and by G619 for the EE overrides), and the acceptance logic below is
    // `envFlagEnabled`'s, character for character. The bodies, their arguments and their order are
    // untouched. Rollback `DC2_G626_LIVE_PIXFLAG=1` restores the historical per-call read and is
    // the CONTROL ARM of this lever's gate.
    //
    // These are NAMESPACE-scope statics deliberately (G619's own note): dynamic init at load time
    // has no guard, so the shipped read is a plain load of a global that MSVC can hoist out of the
    // pixel loop and predict.
    //
    // ⚠️ The initialiser must not use `envFlagEnabled` itself: that calls `g446NoteEnvRead`, which
    // would run during dynamic initialisation, before the sampler's own state is constructed. This
    // helper is CRT-only (`getenv` + `strcmp`) and therefore safe at static-init time.
    static bool g626EnvOnce(const char *name)
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

    static const bool g_g626LivePixFlag = g626EnvOnce("DC2_G626_LIVE_PIXFLAG");
    static const bool g_g626PhaseTrace = g626EnvOnce("DC2_PHASE_TRACE");
    static const bool g_g626RenderQuality = g626EnvOnce("DC2_TRACE_RENDER_QUALITY");
    static const bool g_g626G195Dof = g626EnvOnce("DC2_G195_DOF_TRACE");
    static const bool g_g626G195Fg = g626EnvOnce("DC2_G195_FG_TRACE");
    static const bool g_g626G43FaceUv = g626EnvOnce("DC2_TRACE_G43_FACE_UV");
    static const bool g_g626G43FacePixel = g626EnvOnce("DC2_TRACE_G43_FACE_PIXEL");
    static const bool g_g626G44FaceZ = g626EnvOnce("DC2_TRACE_G44_FACE_Z");

    // ⛔⛔⛔ THE CONTROL ARM MUST REPRODUCE THE ORIGINAL CODEGEN, NOT AN OLDER MECHANISM.
    // The first spelling of this rollback was `g_g626LivePixFlag ? envFlagEnabled(name) : cached`,
    // i.e. a LIVE `getenv` per pixel. That is not what the pre-G626 code did: it held a
    // FUNCTION-LOCAL `static const bool`, which calls `envFlagEnabled` exactly ONCE and thereafter
    // costs an `_Init_global_epoch` TLS read plus a compare. Measuring against a live `getenv`
    // therefore does not price this lever — it prices a defect that never shipped, and it read
    // `disp sprite` PRO 92 ms (candidate) against 9,185 ms (that bogus control), a 99x arm
    // asymmetry that stopped the control run reaching the boss window at all.
    //
    // `G626_LEGACY_FLAG` restores the ORIGINAL construct verbatim — same storage class, same
    // initialiser, same `envFlagEnabled` call — so the control arm's codegen is the shipped
    // pre-G626 codegen and the paired delta is this lever's own.
    //
    // The candidate path is one `.bss` load and one not-taken branch; the extra branch is on the
    // candidate side, so the measurement is CONSERVATIVE (it understates the saving).
#define G626_LEGACY_FLAG(NAME)                                                                     \
    do {                                                                                           \
        if (g_g626LivePixFlag)                                                                     \
        {                                                                                          \
            static const bool legacy = envFlagEnabled(NAME);                                       \
            return legacy;                                                                         \
        }                                                                                          \
    } while (0)

    bool phaseDiagnosticsEnabled()
    {
        G626_LEGACY_FLAG("DC2_PHASE_TRACE");
        return g_g626PhaseTrace;
    }

    bool renderQualityTraceEnabled()
    {
        G626_LEGACY_FLAG("DC2_TRACE_RENDER_QUALITY");
        return g_g626RenderQuality;
    }

    bool g195DofTraceEnabled()
    {
        G626_LEGACY_FLAG("DC2_G195_DOF_TRACE");
        return g_g626G195Dof;
    }

    bool g195ForegroundTraceTarget(const GSContext &ctx, int x, int y)
    {
        // G626: the flag test is now a plain global load, so the whole body folds to one predicted
        // branch on the shipped default. Order of the remaining tests, and every value they
        // compare, are unchanged. The control arm keeps the original function-local static.
        if (g_g626LivePixFlag)
        {
            static const bool legacy = envFlagEnabled("DC2_G195_FG_TRACE");
            if (!legacy)
                return false;
        }
        else if (!g_g626G195Fg)
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
        G626_LEGACY_FLAG("DC2_TRACE_G43_FACE_UV");
        return g_g626G43FaceUv;
    }

    bool g43FacePixelTraceEnabled()
    {
        G626_LEGACY_FLAG("DC2_TRACE_G43_FACE_PIXEL");
        return g_g626G43FacePixel;
    }

    bool g44FaceZTraceEnabled()
    {
        G626_LEGACY_FLAG("DC2_TRACE_G44_FACE_Z");
        return g_g626G44FaceZ;
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
