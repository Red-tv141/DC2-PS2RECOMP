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