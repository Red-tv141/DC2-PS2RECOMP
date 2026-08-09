bool g178EnvEnabled()
{
    static const bool on = g178DefaultOn();
    return on;
}

// G541 is diagnostic-only. Startup must provision compute for either the standalone arm or the
// paired within-process selector; neither form changes the CPU-authoritative VU1/GS result.
bool g541NullComputeRequested()
{
    static const bool on = [] {
        if (g158EnvFlag("DC2_G541_NULL_GPU"))
            return true;
        const char *ab = std::getenv("DC2_G419_AB");
        return ab != nullptr && std::strcmp(ab, "vu1null") == 0;
    }();
    return on;
}

// G542 offline ABI replay is explicit and default-off. It never changes the CPU-authoritative VU
// or GS path; it only provisions compute long enough to validate a saved G541 corpus.
bool g542AbiReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G542_ABI_REPLAY");
    return on;
}

// G543 executes saved VU1 records on the GPU, but is still an offline, default-off oracle. CPU
// state, VU RAM, packets, and the live graphics path remain authoritative.
bool g543SemanticReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G543_VU_REPLAY");
    return on;
}

// G544 chains every saved record through one persistent GPU invocation and ordered packet ring.
// It remains a default-off offline oracle; gameplay still consumes only the CPU executor's state.
bool g544ChainReplayRequested()
{
    static const bool on = g158EnvFlag("DC2_G544_CHAIN_REPLAY");
    return on;
}

// G545 executes the real worker stream as a persistent, batched GPU shadow. It is default-off;
// CPU VU state and packets remain authoritative and any mismatch permanently disables the shadow.
bool g545LiveShadowRequested()
{
    static const bool on = g158EnvFlag("DC2_G545_LIVE_SHADOW");
    return on;
}

// G324 diagnostics mirror the front-end armed/census split. They time only the existing logical
// composite and T8-view QueueItems; default runs pay cached-boolean branches only.
bool g324BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G324_CENSUS");
    return on;
}
bool g324BackendArmed()
{
    static const bool on = g324BackendCensusOn() || g158EnvFlag("DC2_G324_EMPTY");
    return on;
}

// G332 (diagnosis only, default OFF): clean per-type GS-backend GL-busy census. The backend is a
// single worker thread and (async default-off) synchronous, so backend GL busy over a frame ≈ the
// GS worker's fut.get() backend-wait. Timing ONE choke point (the worker loop) classified by item
// type avoids per-draw self-cost (G312/G316). Buckets: 0=render(submit) 1=readback 2=copy 3=other.
std::atomic<uint64_t> g_g332BackendNs[4] = {};
std::atomic<uint64_t> g_g332BackendCount[4] = {};
bool g332BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G332_CENSUS");
    return on;
}
bool g332BackendArmed()
{
    static const bool on = g332BackendCensusOn() || g158EnvFlag("DC2_G332_EMPTY");
    return on;
}
// G453 (diagnosis only, default OFF): split G332's broad backend-other class into the four
// directly-dispatched operation families. One timestamp at the existing worker choke point avoids
// perturbing the GL bodies. Buckets: 0=T8 view, 1=CT24 view, 2=composite, 3=color upload.
std::atomic<uint64_t> g_g453BackendOtherNs[4] = {};
std::atomic<uint64_t> g_g453BackendOtherCount[4] = {};
bool g453BackendCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G453_CENSUS");
    return on;
}
// G447 (diagnosis only, default OFF, DC2_G447_EDGE=1): time the GS worker's blocking wait at EVERY
// backend entry point, classified by item type. G332 above times the same work from INSIDE the GL
// worker; this times it from the caller's side, so the difference is the promise/future round-trip
// and the queue latency. Counted only on the GS worker thread — the EE thread reaches several of
// these at the frame barrier and must not be pooled in.
struct G447WaitScope
{
    std::chrono::steady_clock::time_point t0;
    int edge;
    bool on;
    explicit G447WaitScope(int e)
        : edge(e), on(g447EdgeOn() && g447OnGsWorker())
    {
        if (on)
            t0 = std::chrono::steady_clock::now();
    }
    ~G447WaitScope()
    {
        if (on)
            g447NoteWait(edge, static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()));
    }
};

// G447 LEVER — spin-then-block on the backend round trip (default OFF, DC2_G447_SPIN_US=<us>, or
// the paired within-process arm DC2_G419_AB=spinwait).
//
// G447's edge census measured the GS worker (the pole, sensitivity 0.98) blocked 16.7 ms/f, ALL of
// it in these round trips, over ~74 backend items/frame — while [G332:gsw] times the GL worker's own
// work at only 13.0-13.7 ms/f. The ~2.7 ms/f difference is the promise/future handoff: the caller
// sleeps, the kernel must schedule it back on when set_value lands, and the item is typically only
// ~130-200 us of GL work. Spinning on a plain release/acquire flag for a bounded budget removes the
// caller-side wake for every item that completes inside the budget.
//
// Exactness: the result still comes from the future and the GL command stream is untouched. The only
// change is HOW the caller waits. The cost is CPU burn on a thread that is otherwise blocked — which
// is why it is measured, not assumed.
// PROMOTED default-ON (G447): DC2_G419_AB=spinwait measured −1.23…−1.56 ms/f standalone
// (t = −4.9…−8.7, 4/4 windows favouring) and −0.44 ms/f marginal over the worker-side spin. The win
// saturates by 120 us of budget, which is the tell that what it removes is one thread wake, not
// work. Rollback: DC2_G447_NO_SPIN=1. Budget override: DC2_G447_SPIN_US=<us>.
long long g447SpinBudgetUs()
{
    static const long long us = [] {
        const char *v = std::getenv("DC2_G447_SPIN_US");
        const long long n = (v != nullptr) ? std::atoll(v) : 0LL;
        return (n > 0LL && n <= 5000LL) ? n : 400LL;
    }();
    return us;
}

bool g447SpinActive()
{
    const int arm = g447SpinArm(); // -1 unless DC2_G419_AB=spinwait
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G447_NO_SPIN") != nullptr);
    return !off;
}

// The SUBMIT direction of the same round trip: the GL worker is asleep when an item is queued, so
// the item does not start until the kernel schedules that thread. Separate lever id because it is a
// separate architectural decision (which thread burns CPU to avoid a wake), measured separately.
long long g447WorkerSpinBudgetUs()
{
    static const long long us = [] {
        const char *v = std::getenv("DC2_G447_WSPIN_US");
        const long long n = (v != nullptr) ? std::atoll(v) : 0LL;
        return (n > 0LL && n <= 20000LL) ? n : 0LL;
    }();
    return us > 0LL ? us : 400LL;
}

// PROMOTED default-ON (G447): −0.88…−0.95 ms/f standalone (t = −4.55, 4/4 favouring) and −0.55 ms/f
// marginal over the caller-side spin. Unlike the caller spin this one needs the FULL budget — at
// 150 us its marginal value collapses to −0.20 (t = −0.97), because the mean gap between backend
// items on this route is ~430 us. Rollback: DC2_G447_NO_WSPIN=1.
bool g447WorkerSpinActive()
{
    const int arm = g447WorkerSpinArm(); // -1 unless DC2_G419_AB=wspin
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G447_NO_WSPIN") != nullptr);
    return !off;
}

// G471 CLOSED the shape of these two spins; do not re-open it.
//
// Both loops are "load the flag, read steady_clock::now(), PAUSE" — one QueryPerformanceCounter per
// PAUSE — and that looked like pure waste: G471's PC sampler on the GS worker measured the shipped
// default spending ~18% of the thread's whole span inside the spin bodies and a further ~9% in
// RtlQueryPerformanceCounter plus the CRT's QPC wrappers, and rolling BOTH spins back
// (DC2_G447_NO_SPIN=1 + DC2_G447_NO_WSPIN=1) removed both buckets entirely.
//
// It is not waste. Polling the flag 32x between clock reads measured **+0.18 ms/f (t = +3.51, 0/7
// windows favouring candidate)** on the paired MAP-0 gate: removing the clock makes the loop
// tighter, so it hammers the shared completion line ~30x more often, and that coherence traffic
// costs more than the clock. Inverting it — one flag load per 16 PAUSEs, backing BOTH rates off —
// recovered the loss but only to neutral (−0.05 ms/f, t = −0.08, 3/7 windows). The clock read is
// doing the job of a backoff, and the loop is at a local optimum in both directions. Neither arm was
// promoted and both were removed; the ~2.4 ms/f of QPC inside these spins is NOT recoverable frame
// time. See plans/phase-G471-fix-log.md.
// G491 — DO NOT SPIN ON THE READBACK EDGE.
//
// The spin above is a flat 400 us budget on EVERY backend round trip, promoted by G447 on edges
// whose GL work is "typically only ~130-200 us". `[G447:edge]` prices backend-readcolor at
// 3.2-3.4 ms/f over SIX calls, i.e. ~545 us each: the readback wait CANNOT complete inside the
// budget, so every one of those six spins burns its full 400 us and then blocks anyway. That is
// ~2.3 ms/f of a core spent on the pole thread with no possible return, and `[G446:gsprof]` sees
// it directly — 17.66% of the whole GS-worker span sits on one instruction inside this loop.
//
// Exactness: identical to g447WaitFuture with the spin disabled. The result still comes from the
// future; only the caller's wait strategy changes. Kept as a SEPARATE function so the control
// arm's call shape stays byte-identical (17a law 17 — the lever-OFF path IS the control arm).
//
// Levers: DC2_G419_AB=rbspin (paired gate), DC2_G491_NO_RB_SPIN=1 (standalone arm),
// DC2_G491_RB_SPIN=1 (force the historical spin back on).
bool g491RbNoSpinActive()
{
    const int arm = g491RbSpinArm(); // -1 unless DC2_G419_AB=rbspin
    if (arm >= 0)
        return arm == 1; // candidate arm = NO spin on this edge
    static const bool on = (std::getenv("DC2_G491_NO_RB_SPIN") != nullptr) &&
                           (std::getenv("DC2_G491_RB_SPIN") == nullptr);
    return on;
}

bool g491WaitFutureNoSpin(std::future<bool> &fut)
{
    try
    {
        return fut.get();
    }
    catch (...)
    {
        return false;
    }
}

bool g447WaitFuture(std::future<bool> &fut, std::atomic<uint32_t> &done)
{
    if (g447SpinActive())
    {
        static const long long budgetUs =
            g447SpinBudgetUs() > 0LL ? g447SpinBudgetUs() : 400LL; // A/B default
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::microseconds(budgetUs);
        while (done.load(std::memory_order_acquire) == 0u)
        {
            if (std::chrono::steady_clock::now() >= until)
                break;
            YieldProcessor();
        }
    }
    try
    {
        return fut.get();
    }
    catch (...)
    {
        return false;
    }
}

struct SyncWait
{
    std::atomic<uint32_t> done{0u};
    std::shared_ptr<std::promise<bool>> promise;
    std::future<bool> future;

    SyncWait() : promise(std::make_shared<std::promise<bool>>()),
                 future(promise->get_future()) {}

    bool wait()
    {
        return g447WaitFuture(future, done);
    }
};

// NOTE: this whole .inc is inside the anonymous namespace opened in persistent_t8_decoder.inc
// (closed at the end of this file). The cross-TU accessor g332_backend_snapshot() is therefore
// defined AFTER that close (see bottom of file) so it keeps external linkage.

// G256/G258 diagnostic only: exact PS2 fragment arithmetic for the dependency-preserving G255
// RTT path.  G258 found this backend performance-neutral after parity, so do not expose it as a
// standalone behavior arm: require the CPU shadow oracle whenever it is requested.
bool g256ExactRttOn()
{
    static const bool on = g158EnvFlag("DC2_G256_EXACT_RTT") &&
                           g158EnvFlag("DC2_G255_VERIFY");
    return on;
}

// G257 (default OFF): prove image-store, barrier, coverage, and direct texture readback on the
// actual persistent G178 worker context without touching a guest batch.
bool g257ImageStoreTestOn()
{
    static const bool on = g158EnvFlag("DC2_G257_IMAGESTORE_TEST");
    return on;
}

// G258 (default OFF): synchronously read one fbp=0x146 destination pixel after selected
// exact-shader draws.  This is a localization probe only; never enable it for timing.
bool g258TraceOn()
{
    static const bool on = g158EnvFlag("DC2_G258_TRACE");
    return on;
}

bool g281T8ViewRequested()
{
    static const bool g281 =
        (g158EnvFlag("DC2_G281_T8_VIEW") ||
         (g158EnvFlag("DC2_G282_TIGHT_ROWS") &&
          !g158EnvFlag("DC2_G282_NO_TIGHT_ROWS") &&
          !g158EnvDisabled("DC2_G282_TIGHT_ROWS"))) &&
                           !g158EnvFlag("DC2_G281_NO_T8_VIEW") &&
                           !g158EnvDisabled("DC2_G281_T8_VIEW") &&
                           !g158EnvFlag("DC2_G280_NO_ALIAS_VIEW");
    static const bool g310 =
        !g158EnvFlag("DC2_G310_NO_LOGICAL") &&
        !g158EnvDisabled("DC2_G310_LOGICAL") &&
        !g158EnvFlag("DC2_G310_NO_T8_VIEW") &&
        !g158EnvDisabled("DC2_G310_T8_VIEW");
    return g281 || g310;
}

bool g309Requested()
{
    static const bool g309 =
        !g158EnvFlag("DC2_G309_NO_MULTI_SOURCE") &&
        !g158EnvDisabled("DC2_G309_MULTI_SOURCE") &&
        (g158EnvFlag("DC2_G309_MULTI_SOURCE") ||
         g158EnvFlag("DC2_G309_VERIFY"));
    // G310 reuses the exact G309 atlas-composition program to populate one ordinary
    // persistent texture. Its front-end arm is independent, so request shader setup here
    // without enabling G309's direct multi-source display path.
    static const bool g310 =
        !g158EnvFlag("DC2_G310_NO_LOGICAL") &&
        !g158EnvDisabled("DC2_G310_LOGICAL");
    return g309 || g310;
}

// G338: the resident CT24-alpha view program is needed when the CT24 display-composite consumer
// of a resident CT32 FBO is being retired. G352 PROMOTION (2026-07-23): DEFAULT-ON (accept-floor
// close of arc-total-closure); rollback DC2_G338_NO_CT24_VIEW=1 or DC2_G338_CT24_VIEW=0. Mirrors
// the front-end g338Ct24ViewOn() gate so the program is built iff the consumer is retired.
bool g338Ct24ViewRequested()
{
    static const bool on = !g158EnvFlag("DC2_G338_NO_CT24_VIEW") &&
                           !g158EnvDisabled("DC2_G338_CT24_VIEW");
    return on;
}

static constexpr uint32_t kG289ZeroFbpToken = 0xfffffffdu;
static constexpr uint32_t kG289AliasCt32Token = 0xfffffffcu;
static constexpr uint32_t kG289AliasFbp = 0xfffffffbu;
static constexpr uint32_t kG289AliasCt24Token = 0xfffffffau;
static constexpr uint32_t kG289WorkCt24Token = 0xfffffff9u;
static constexpr uint32_t kG289WorkCt32Token = 0xfffffff8u;
static constexpr uint32_t kG309CompositeToken = 0xfffffff7u;
static constexpr uint32_t kG309CompositeFbp = 0xfffffff6u;
// G310: one persistent 512x512 logical CT32 backing for the physical 0x2720/tbw=8 atlas.
// It is an ordinary private FBO source (not a shader token): producer-time page copies and
// guest-authoritative page uploads keep it current, so the display draw performs one normal
// texture sample. Kept outside the public LLE header by cpp-to-cpp bridges below.
static constexpr uint32_t kG310LogicalFbp = 0xfffffff5u;
static constexpr uint32_t kG310LogicalCt24Token = 0xfffffff4u;
// G349 (closure core, color side): the 0x139 display-strip residency-teardown root. [G349:fbolife]
// confirmed 0x139's FBO is destroyed+recreated ~910x/run (ONLY 0x139) because m_fbos is keyed by
// fbp ALONE while 0x139 is written at BOTH fbw=8 (512-wide display strip) and fbw=2 (128-wide narrow
// population, G347 w2=22%). Each width flip clears the other's FBO → the snapInv 100% FBO-read-fail.
// De-collide: route the narrow (w!=512) 0x139 population to a private sibling FBO key so the 512-wide
// display FBO PERSISTS across frames. This is the substrate for the G350 front-end upload-skip; the
// shadow oracle (DC2_G347_SHADOW) eq% on the now-persistent wide FBO is that skip's GO/NO-GO signal.
// Default-off (DC2_G349_DECOL=1; DC2_G349_NO_DECOL=1 wins); a DC2_G345_CLOSURE stack member. VRAM
// stays the coherence medium (the narrow batch still reads back to VRAM), so composition is exact by
// construction; verified visually. Only the three (w,h)-checked FBO accessors are remapped — every
// other consumer reads 0x139 at 512 (the wide display FBO), unaffected.
static constexpr uint32_t kG349Narrow139 = 0xfffffff3u;
static bool g349DecolOn()
{
    static const bool on = g158EnvFlag("DC2_G349_DECOL") && !g158EnvDisabled("DC2_G349_DECOL") &&
                           !g158EnvFlag("DC2_G349_NO_DECOL");
    return on;
}
static uint32_t g349FboKey(uint32_t fbp, int w)
{
    return (g349DecolOn() && fbp == 0x139u && w != 512) ? kG349Narrow139 : fbp;
}
static uint32_t g289RealSourceFbp(uint32_t fbp)
{
    if (fbp == kG289ZeroFbpToken)
        return 0u;
    if (fbp == kG289AliasCt32Token || fbp == kG289AliasCt24Token)
        return kG289AliasFbp;
    if (fbp == kG289WorkCt24Token || fbp == kG289WorkCt32Token)
        return 0x13bu;
    if (fbp == kG309CompositeToken)
        return kG309CompositeFbp;
    if (fbp == kG310LogicalCt24Token)
        return kG310LogicalFbp;
    return fbp;
}
static bool g289SourceIsCt24(uint32_t fbp)
{
    return fbp == kG289AliasCt24Token || fbp == kG289WorkCt24Token ||
           fbp == kG310LogicalCt24Token;
}
static bool g289SourceIsOwner(uint32_t fbp)
{
    return fbp == kG289AliasCt32Token || fbp == kG289AliasCt24Token ||
           fbp == kG289WorkCt24Token || fbp == kG289WorkCt32Token;
}

// GL entry points needed beyond GLFns (loaded once on the G178 thread).
typedef void(APIENTRY *PFN178BLENDFUNCSEPARATE)(GLenum, GLenum, GLenum, GLenum);
typedef void(APIENTRY *PFN178BLENDEQUATIONSEPARATE)(GLenum, GLenum);
typedef void(APIENTRY *PFN178GENRENDERBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN178BINDRENDERBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFN178RENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY *PFN178FRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef void(APIENTRY *PFN178DELETERENDERBUFFERS)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFN178UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void(APIENTRY *PFN178BINDIMAGETEXTURE)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void(APIENTRY *PFN178MEMORYBARRIER)(GLbitfield);
typedef void(APIENTRY *PFN178GETINTERNALFORMATIV)(GLenum, GLenum, GLenum, GLsizei, GLint *);
typedef void(APIENTRY *PFN541DISPATCHCOMPUTE)(GLuint, GLuint, GLuint);

struct G178GLExt
{
    PFN178BLENDFUNCSEPARATE glBlendFuncSeparate_ = nullptr;
    PFN178BLENDEQUATIONSEPARATE glBlendEquationSeparate_ = nullptr;
    PFN178GENRENDERBUFFERS glGenRenderbuffers_ = nullptr;
    PFN178BINDRENDERBUFFER glBindRenderbuffer_ = nullptr;
    PFN178RENDERBUFFERSTORAGE glRenderbufferStorage_ = nullptr;
    PFN178FRAMEBUFFERRENDERBUFFER glFramebufferRenderbuffer_ = nullptr;
    PFN178DELETERENDERBUFFERS glDeleteRenderbuffers_ = nullptr;
    PFN178UNIFORM2F glUniform2f_ = nullptr;
    PFN178BINDIMAGETEXTURE glBindImageTexture_ = nullptr;
    PFN178MEMORYBARRIER glMemoryBarrier_ = nullptr;
    PFN178GETINTERNALFORMATIV glGetInternalformativ_ = nullptr;
    PFN541DISPATCHCOMPUTE glDispatchCompute_ = nullptr;
};

bool g178LoadGLExt(G178GLExt &e)
{
    bool ok = true;
    ok &= loadProc(e.glBlendFuncSeparate_, "glBlendFuncSeparate");
    ok &= loadProc(e.glBlendEquationSeparate_, "glBlendEquationSeparate");
    ok &= loadProc(e.glGenRenderbuffers_, "glGenRenderbuffers");
    ok &= loadProc(e.glBindRenderbuffer_, "glBindRenderbuffer");
    ok &= loadProc(e.glRenderbufferStorage_, "glRenderbufferStorage");
    ok &= loadProc(e.glFramebufferRenderbuffer_, "glFramebufferRenderbuffer");
    ok &= loadProc(e.glDeleteRenderbuffers_, "glDeleteRenderbuffers");
    ok &= loadProc(e.glUniform2f_, "glUniform2f");
    if (g256ExactRttOn() || g257ImageStoreTestOn())
    {
        ok &= loadProc(e.glBindImageTexture_, "glBindImageTexture");
        ok &= loadProc(e.glMemoryBarrier_, "glMemoryBarrier");
        ok &= loadProc(e.glGetInternalformativ_, "glGetInternalformativ");
    }
    else if (g541NullComputeRequested() || g542AbiReplayRequested() ||
             g543SemanticReplayRequested() || g544ChainReplayRequested() ||
             g545LiveShadowRequested())
    {
        // Optional prototype: missing compute support must not disable the proven graphics backend.
        (void)loadProc(e.glBindImageTexture_, "glBindImageTexture");
        (void)loadProc(e.glMemoryBarrier_, "glMemoryBarrier");
    }
    if (g541NullComputeRequested() || g542AbiReplayRequested() ||
        g543SemanticReplayRequested() || g544ChainReplayRequested() ||
        g545LiveShadowRequested())
        (void)loadProc(e.glDispatchCompute_, "glDispatchCompute");
    return ok;
}

constexpr GLenum GL178_FUNC_ADD = 0x8006;
constexpr GLenum GL178_FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum GL178_RENDERBUFFER = 0x8D41;
constexpr GLenum GL178_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL178_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL178_STREAM_DRAW = 0x88E0;
constexpr GLenum GL178_READ_WRITE = 0x88BA;
constexpr GLbitfield GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT = 0x00000020;
constexpr GLbitfield GL178_TEXTURE_FETCH_BARRIER_BIT = 0x00000008;
constexpr GLbitfield GL178_TEXTURE_UPDATE_BARRIER_BIT = 0x00000100;
constexpr GLbitfield GL178_FRAMEBUFFER_BARRIER_BIT = 0x00000400;
constexpr GLenum GL541_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL541_R32UI = 0x8236;
constexpr GLenum GL541_RED_INTEGER = 0x8D94;
constexpr GLenum GL257_SHADER_IMAGE_LOAD = 0x82A4;
constexpr GLenum GL257_SHADER_IMAGE_STORE = 0x82A5;
#include "g542_vu_gpu_abi.inc"
#include "g543_vu_gpu_semantics.inc"
#include "g544_vu_gpu_chain.inc"
#include "g545_vu_gpu_live.inc"
// GPU texture-cache residency cap (bytes of decoded RGBA8). Title uses a handful of ≤512²
// textures (~1-4MB); the cap only matters on texture-heavy routes. LRU by last-used batch.
constexpr size_t kG178TexCacheCapBytes = 256ull * 1024 * 1024;

// G415 private .cpp-to-.cpp side channel for the colour-readback window of the NEXT synchronous
// batch submit. G178Batch lives in the frozen ps2_gs_gpu_lle.h, so the window travels beside it
// instead of inside it (the same idiom G242 used for depthKey). Only the synchronous submit path
// publishes a window: the async path never reads colour back, and the front-end/GS-worker thread
// is the sole writer, consumed by the worker under the queue mutex handoff.
struct G415ColorWindow
{
    bool valid = false;
    int glY = 0;   // first GL (bottom-first) row to read
    int rows = 0;  // number of GL rows to read
};
G415ColorWindow g_g415PendWindow;

// G415 aggregate counters for the narrowed colour readback ([G415:narrow] under DC2_G415_STAT=1).
std::atomic<uint64_t> g_g415NarrowBatches{0};
std::atomic<uint64_t> g_g415NarrowRows{0};
std::atomic<uint64_t> g_g415FullBatches{0};
std::atomic<uint64_t> g_g415FullRows{0};

// G541: one queued GL command can absorb every logical VU1 kick submitted before that command is
// popped. The count is mutated only while m_mtx is held; after pop, the worker owns it exclusively.
// This is the prototype form of a sequential GPU command buffer: many ordered kicks, one dispatch.
struct G541NullBatch
{
    uint64_t kicks = 0ull;
};

class G178Backend
{
public:
    static G178Backend &instance()
    {
        static G178Backend b;
        return b;
    }

    ~G178Backend()
    {
        if (!m_started)
            return;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_cvWork.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    // Same startup dance as G162GpuDecoder::startEager (release main context -> spawn thread ->
    // create+share context there -> restore main). Called once from PS2Runtime::initialize().
    void startEager()
    {
        if (m_started)
            return;
        if (!s_g158MainRC || !s_g158MainDC)
        {
            std::fprintf(stderr, "[G178:backend] SKIP: no captured main context\n");
            return;
        }
        if (!wglMakeCurrent(nullptr, nullptr))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not release main context, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return;
        }
        m_started = true;
        m_thread = std::thread([this] { worker(); });
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvReady.wait(lk, [&] { return m_ready.load() || m_failed.load(); });
        }
        if (!wglMakeCurrent(s_g158MainDC, s_g158MainRC))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not restore main context, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
        }
    }

    bool ready() const { return m_ready.load(std::memory_order_acquire); }

    // G541: enqueue-only transport. The VU1 worker never waits and the GL worker never reads its
    // persistent image back. Logical kicks coalesce into the one still-queued command; once the GL
    // worker pops it, the next kick creates a successor command before publishing its CPU packets.
    // Therefore every kick still has a compute command ahead of its dependent GS work in the same
    // FIFO, without paying one host queue item and one driver dispatch per kick.
    bool submitG541NullCompute()
    {
        if (!g541NullComputeRequested() || !m_ready.load(std::memory_order_acquire) ||
            !m_g541Available)
        {
            m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        size_t depth = 0u;
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
            {
                m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            std::shared_ptr<G541NullBatch> batch = m_g541QueuedBatch.lock();
            if (!batch)
            {
                if (m_queue.size() >= 8192u)
                {
                    m_g541Rejected.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                batch = std::make_shared<G541NullBatch>();
                QueueItem item;
                item.g541NullBatch = batch;
                m_queue.push_back(std::move(item));
                m_g541QueuedBatch = batch;
                pushed = true;
            }
            ++batch->kicks;
            depth = m_queue.size();
        }
        m_g541Submitted.fetch_add(1u, std::memory_order_relaxed);
        if (pushed)
        {
            uint64_t peak = m_g541HighWater.load(std::memory_order_relaxed);
            while (peak < depth && !m_g541HighWater.compare_exchange_weak(
                       peak, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            m_submitSeq.fetch_add(1u, std::memory_order_release);
            m_cvWork.notify_one();
        }
        return true;
    }

    // G545: move-owned, enqueue-only live command. Commands coalesce only while their queue item
    // has not been popped; the mutex therefore makes each batch immutable before the GL worker
    // reads it. A size boundary creates a FIFO successor instead of rejecting or blocking VU1.
    bool submitG545Live(std::vector<uint8_t> &&command)
    {
        std::string why;
        if (!g545LiveShadowRequested() || !m_ready.load(std::memory_order_acquire) ||
            !m_g545Available || m_g545Disabled.load(std::memory_order_acquire) ||
            !g545ValidateBlob(command, why))
        {
            m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }

        size_t depth = 0u;
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire) ||
                m_g545Disabled.load(std::memory_order_relaxed))
            {
                m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            std::shared_ptr<G545LiveBatch> batch = m_g545QueuedBatch.lock();
            if (batch && (batch->blobs.size() >= G545_LIVE_MAX_BATCH_COMMANDS ||
                          batch->bytes + command.size() > G545_LIVE_MAX_BATCH_BYTES))
                batch.reset();
            if (!batch)
            {
                if (m_queue.size() >= 8192u)
                {
                    m_g545Rejected.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                batch = std::make_shared<G545LiveBatch>();
                QueueItem item;
                item.g545LiveBatch = batch;
                m_queue.push_back(std::move(item));
                m_g545QueuedBatch = batch;
                pushed = true;
            }
            batch->bytes += command.size();
            batch->blobs.push_back(std::move(command));
            depth = m_queue.size();
        }
        m_g545Submitted.fetch_add(1u, std::memory_order_relaxed);
        if (pushed)
        {
            uint64_t peak = m_g545HighWater.load(std::memory_order_relaxed);
            while (peak < depth && !m_g545HighWater.compare_exchange_weak(
                       peak, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            m_submitSeq.fetch_add(1u, std::memory_order_release);
            m_cvWork.notify_one();
        }
        return true;
    }

    bool hasTex(uint64_t key)
    {
        std::lock_guard<std::mutex> lk(m_residentMtx);
        return m_resident.count(key) != 0;
    }

    // Synchronous: blocks the calling (EE or MTGS-worker) thread until the batch is rendered and
    // read back. The batch is used in place (no copy); the caller owns its lifetime.
    bool submit(G178Batch &batch)
    {
        return submitImpl(&batch, 0u, nullptr, 0, 0, nullptr);
    }

    // G305: non-blocking submit of a skip-readback color batch (no guest depth, no readback out).
    // Moves the caller's batch vectors into a backend-owned slot (scalars copied) and returns
    // WITHOUT waiting; the render overlaps the caller's next-flush preparation. Fails closed (the
    // caller falls back to synchronous submit) if the backend is not ready or a packet is already
    // in flight — the caller is contracted to drainAsync() before every submit, so the in-flight
    // guard only trips on a contract violation. All calls are on the single GS-worker/front-end
    // thread, so m_asyncPending / m_asyncFut / m_asyncSlot need no cross-thread synchronization
    // beyond the queue mutex that hands the slot to the worker.
    bool submitAsync(G178Batch &batch)
    {
        static constexpr size_t kAsyncSlots = 1u;
        if (!m_ready.load(std::memory_order_acquire) || m_asyncSlots.size() >= kAsyncSlots)
            return false;
        // G415: an async packet is skip-readback by contract, so no colour window can apply.
        // Consume any published window here so it can never leak onto a later submit.
        g_g415PendWindow.valid = false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            // Recycle completed vector storage back to the caller before installing the new packet.
            // Scalar fields deliberately remain copied in `batch`: the optimistic epilogue reads
            // them after submission, exactly as it did under the original move-assignment.
            G178Batch next = std::move(batch);
            batch.fbPixels = std::move(m_asyncRecycle.fbPixels);
            batch.texUploads = std::move(m_asyncRecycle.texUploads);
            batch.verts = std::move(m_asyncRecycle.verts);
            batch.draws = std::move(m_asyncRecycle.draws);
            batch.readback = std::move(m_asyncRecycle.readback);
            AsyncSlot slot;
            slot.batch = std::move(next);
            slot.future = std::move(fut);
            m_asyncSlots.push_back(std::move(slot));
            QueueItem item;
            item.batch = &m_asyncSlots.back().batch;
            item.promise = promise;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    // G305: block until the in-flight async packet (if any) completes. Returns false when there
    // was nothing pending; otherwise sets ok to the backend render/readback result. After this
    // returns the worker is done with m_asyncSlot, so the front-end may reuse it.
    bool drainAsync(bool &ok)
    {
        if (m_asyncSlots.empty())
            return false;
        AsyncSlot &slot = m_asyncSlots.front();
        try { ok = slot.future.get(); }
        catch (...) { ok = false; }
        m_asyncRecycle = std::move(slot.batch);
        m_asyncSlots.pop_front();
        return true;
    }

    // G242: universal-Z batches carry the authoritative guest PSMZ24 rows beside the existing
    // color-only G178 interface. Keep this private side channel out of ps2_gs_gpu_lle.h: that
    // header is deliberately frozen for the project's no-header-rebuild rule. Submission is
    // synchronous, so the caller-owned vectors remain alive until the worker finishes.
    bool submitDepth(G178Batch &batch, uint64_t depthKey,
                     const std::vector<float> *depthUpload, int depthY, int depthRows)
    {
        return submitImpl(&batch, depthKey, depthUpload, depthY, depthRows, nullptr);
    }

    // G275: render a guest-depth batch and read its written row window while that texture is
    // still attached. This preserves the synchronous GPU->VRAM contract while avoiding a second
    // worker job, promise wait, scratch-FBO bind, and depth reattachment for every write wave.
    bool submitDepthReadback(G178Batch &batch, uint64_t depthKey,
                             const std::vector<float> *depthUpload,
                             int depthY, int depthRows,
                             int readY, int readRows,
                             std::vector<float> &depthReadback)
    {
        if (depthKey == 0u || readY < 0 || readRows <= 0 ||
            readY > batch.fbH || readRows > batch.fbH - readY)
            return false;
        return submitImpl(&batch, depthKey, depthUpload, depthY, depthRows,
                          &depthReadback, readY, readRows, true);
    }

    bool readDepth(uint64_t depthKey, int width, int height,
                   int depthY, int depthRows, std::vector<float> &depthReadback)
    {
        if (depthKey == 0u || width <= 0 || height <= 0 || depthY < 0 || depthRows <= 0 ||
            depthY > height || depthRows > height - depthY)
            return false;
        G178Batch readBatch;
        readBatch.fbW = width;
        readBatch.fbH = height;
        return submitImpl(&readBatch, depthKey, nullptr, depthY, depthRows,
                          &depthReadback, depthY, depthRows, false);
    }

    // G261: synchronous FBO color row-window readback (deferred RTT-wave materialization).
    bool readColor(uint32_t fbp, int width, int height, int glY, int rows,
                   std::vector<uint32_t> &out)
    {
        if (width <= 0 || height <= 0 || glY < 0 || rows <= 0 ||
            glY > height || rows > height - glY || !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeReadColor, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = fbp;
            item.colorReadback = &out;
            item.colorTexW = width;
            item.colorTexH = height;
            item.colorW = width;
            item.colorY = glY;
            item.colorRows = rows;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(1);
        // G491: this edge (and only this edge) can elect not to spin — see g491RbNoSpinActive.
        // Six calls per frame, so the branch itself is free.
        if (g491RbNoSpinActive())
            return g491WaitFutureNoSpin(wait.future);
        return wait.wait();
    }

    bool writeColor(uint32_t fbp, int texWidth, int texHeight,
                    int glX, int glY, int width, int rows,
                    const std::vector<uint32_t> &in)
    {
        if (texWidth <= 0 || texHeight <= 0 || glX < 0 || glY < 0 ||
            width <= 0 || rows <= 0 || glX > texWidth || width > texWidth - glX ||
            glY > texHeight || rows > texHeight - glY ||
            in.size() != static_cast<size_t>(width) * rows ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeWriteColor, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = fbp;
            item.colorWrite = &in;
            item.colorTexW = texWidth;
            item.colorTexH = texHeight;
            item.colorX = glX;
            item.colorW = width;
            item.colorY = glY;
            item.colorRows = rows;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(2);
        return wait.wait();
    }

    // G280: synchronous GPU->GPU page-tile copies between two persistent target FBOs (alias
    // view resolve). Geometry is validated on the worker against both FBOs' live dimensions.
    bool copyColor(uint32_t srcFbp, uint32_t dstFbp, const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || rects6.empty() || (rects6.size() % 6u) != 0u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeCopy, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = dstFbp;
            item.copySrcFbp = srcFbp;
            item.copyRects = &rects6;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(3);
        return wait.wait();
    }

    // G289: same synchronous bit-exact color copy as G280, but the one measured work target may
    // be first-use. Allocate its 512x512 FBO on the worker before validating/copying.
    bool copyColorEnsureDest(uint32_t srcFbp, uint32_t dstFbp, int dstW, int dstH,
                             const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || dstW <= 0 || dstH <= 0 || rects6.empty() ||
            (rects6.size() % 6u) != 0u || !m_ready.load(std::memory_order_acquire))
            return false;
        g492Note(kG492EdgeCopy, 0ull, false); // G492 invariant only — never skipped here
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = dstFbp;
            item.copySrcFbp = srcFbp;
            item.copyRects = &rects6;
            item.copyEnsureDest = true;
            item.colorTexW = dstW;
            item.colorTexH = dstH;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(3);
        return wait.wait();
    }

    // G281: synchronously build/update one decoded RGBA texture directly from a resident CT32
    // target FBO. The front-end supplies an exact PSMT8 byte-address map and current CLUT.
    bool prepareT8View(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                       int texW, int texH,
                       const std::vector<uint32_t> &mapRgba,
                       const std::vector<uint32_t> &clutRgba,
                       bool verify, uint64_t &verifyBad)
    {
        verifyBad = 0u;
        if (texKey == 0u || srcFbp == 0u || srcFbW <= 0 || texW <= 0 || texH <= 0 ||
            mapRgba.size() != static_cast<size_t>(texW) * texH ||
            clutRgba.size() != 256u || !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable): the resident view texture keeps last frame's
        // content on a candidate frame. Excluded when the verifier out-param is requested.
        {
            const bool g492Skip = g492NoOtherActive() && !verify;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g281TexKey = texKey;
            item.g281SrcFbp = srcFbp;
            item.g281SrcFbW = srcFbW;
            item.g281TexW = texW;
            item.g281TexH = texH;
            item.g281Map = &mapRgba;
            item.g281Clut = &clutRgba;
            item.g281Verify = verify;
            item.g281VerifyBad = &verifyBad;
            if (g324BackendArmed())
            {
                item.g324Class = 1u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G338: build/update one resident CT24-alpha view texture from a resident CT32 target FBO.
    // Same worker-queue contract as prepareT8View; alpha is re-derived from TEXA (ta0/aem).
    bool prepareCt24View(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                         int texW, int texH, int ta0, int aem,
                         bool verify, uint64_t &verifyBad)
    {
        verifyBad = 0u;
        if (texKey == 0u || srcFbp == 0u || srcFbW <= 0 || texW <= 0 || texH <= 0 ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable) — see prepareT8View above.
        {
            const bool g492Skip = g492NoOtherActive() && !verify;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g338Ct24View = true;
            item.g281TexKey = texKey;
            item.g281SrcFbp = srcFbp;
            item.g281SrcFbW = srcFbW;
            item.g281TexW = texW;
            item.g281TexH = texH;
            item.g338Ta0 = ta0;
            item.g338Aem = aem;
            item.g281Verify = verify;
            item.g281VerifyBad = &verifyBad;
            if (g324BackendArmed())
            {
                item.g324Class = 1u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G309: synchronously prepare five live target FBOs plus the measured 8x16 authority map.
    // The timing path samples them directly; only the verifier builds/reads a 512x512 reference.
    // Caller-owned staging remains live until this job completes.
    bool buildAuthoritativeComposite(const std::vector<uint32_t> &pageMap,
                                     const std::vector<uint32_t> &basePixels,
                                     bool readback, std::vector<uint32_t> &out)
    {
        out.clear();
        if (pageMap.size() != 128u || basePixels.size() != 512u * 512u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable). Excluded when the caller wants the composite read
        // back — `out` would then stay empty and the caller's size check would flip.
        {
            const bool g492Skip = g492NoOtherActive() && !readback;
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309Readback = readback ? &out : nullptr;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

    // G310: use G309's exact atlas compositor once to populate the persistent logical FBO.
    // Unlike G309's timing arm, downstream draws sample this one ordinary texture; unlike the
    // verifier, no readback follows the composition pass.
    bool buildLogicalComposite(const std::vector<uint32_t> &pageMap,
                               const std::vector<uint32_t> &basePixels)
    {
        if (pageMap.size() != 128u || basePixels.size() != 512u * 512u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        // G492 CEILING PROBE (never promotable): the persistent logical FBO keeps last frame's
        // composition on a candidate frame. No out-param, so nothing downstream changes size.
        {
            const bool g492Skip = g492NoOtherActive();
            g492Note(kG492EdgeOther, 0ull, g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309ForceDraw = true;
            item.g309OutputFbp = kG310LogicalFbp;
            if (g324BackendArmed())
            {
                item.g324Class = 2u;
                item.g324Enqueue = std::chrono::steady_clock::now();
            }
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(4);
        return wait.wait();
    }

private:
    bool submitImpl(G178Batch *batch, uint64_t depthKey,
                    const std::vector<float> *depthUpload, int depthY, int depthRows,
                    std::vector<float> *depthReadback,
                    int depthReadY = 0, int depthReadRows = 0,
                    bool renderThenReadDepth = false)
    {
        if (batch == nullptr || !m_ready.load(std::memory_order_acquire))
            return false;
        // G415: consume the front-end's published colour-readback window for THIS submit only.
        // Consume-and-clear on every submit so a window can never survive onto an unrelated job
        // (readDepth() reaches this same path with a synthetic batch).
        const G415ColorWindow g415Window = g_g415PendWindow;
        g_g415PendWindow.valid = false;
        // G492 CEILING PROBE (never promotable). Delete the whole round trip at this edge and
        // report success, so the caller keeps its post-submit epilogue and its VRAM unpack instead
        // of falling into the CPU-replay path. Items carrying a depth-readback OUT vector are
        // excluded: their size is validated by the caller, and a stale one would change its control
        // flow. See g492_overlap_ceiling.inc.
        {
            // The arm accessor marks the frame as having consumed an arm, so it must be called on
            // EVERY entry — never behind a short-circuit that a depth item could skip.
            const bool g492Arm = g492NoRenderActive();
            const bool g492Skip = g492Arm && batch != nullptr && depthReadback == nullptr;
            g492Note(kG492EdgeRender,
                     batch != nullptr ? static_cast<unsigned long long>(batch->draws.size()) : 0ull,
                     g492Skip);
            if (g492Skip)
                return true;
        }
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.batch = batch;
            item.g415Window = g415Window;
            item.depthKey = depthKey;
            item.depthUpload = depthUpload;
            item.depthY = depthY;
            item.depthRows = depthRows;
            item.depthReadback = depthReadback;
            item.depthReadY = depthReadY;
            item.depthReadRows = depthReadRows;
            item.renderThenReadDepth = renderThenReadDepth;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(0);
        return wait.wait();
    }

    struct QueueItem
    {
        std::shared_ptr<G541NullBatch> g541NullBatch;
        std::shared_ptr<G545LiveBatch> g545LiveBatch;
        G178Batch *batch = nullptr;
        uint64_t depthKey = 0u;
        const std::vector<float> *depthUpload = nullptr;
        int depthY = 0;
        int depthRows = 0;
        std::vector<float> *depthReadback = nullptr;
        int depthReadY = 0;
        int depthReadRows = 0;
        bool renderThenReadDepth = false;
        // G261 color-readback job (batch == nullptr, colorReadback != nullptr)
        uint32_t colorFbp = 0u;
        std::vector<uint32_t> *colorReadback = nullptr;
        // G264 color-write job (batch == nullptr, colorWrite != nullptr): mirror guest upload
        // bytes into the resident FBO texture instead of materializing around them.
        const std::vector<uint32_t> *colorWrite = nullptr;
        int colorTexW = 0, colorTexH = 0;
        int colorX = 0, colorY = 0, colorW = 0, colorRows = 0;
        // G280 GPU->GPU alias-page copy job (batch == nullptr, copyRects != nullptr): copy CT32
        // page tiles from copySrcFbp's FBO into colorFbp's FBO color texture, 6 ints per rect.
        uint32_t copySrcFbp = 0u;
        const std::vector<int32_t> *copyRects = nullptr;
        bool copyEnsureDest = false;
        // G281 resident-CT32 -> decoded-PSMT8 texture-view job.
        uint64_t g281TexKey = 0u;
        uint32_t g281SrcFbp = 0u;
        int g281SrcFbW = 0, g281TexW = 0, g281TexH = 0;
        const std::vector<uint32_t> *g281Map = nullptr;
        const std::vector<uint32_t> *g281Clut = nullptr;
        bool g281Verify = false;
        uint64_t *g281VerifyBad = nullptr;
        // G338 resident-CT32 -> CT24-alpha texture-view job (reuses the g281* common fields for
        // texKey/srcFbp/dims/verify; distinguished by g338Ct24View).
        bool g338Ct24View = false;
        int g338Ta0 = 0, g338Aem = 0;
        // G309 one-pass five-source authoritative CT32 atlas composite.
        const std::vector<uint32_t> *g309PageMap = nullptr;
        const std::vector<uint32_t> *g309BasePixels = nullptr;
        std::vector<uint32_t> *g309Readback = nullptr;
        bool g309ForceDraw = false;
        uint32_t g309OutputFbp = kG309CompositeFbp;
        std::shared_ptr<std::promise<bool>> promise;
        // G447: optional caller-owned completion flag, stored (release) by the worker IMMEDIATELY
        // BEFORE promise->set_value so a spinning caller can never observe completion earlier than
        // the promise is satisfied — the caller's stack frame therefore always outlives this store.
        // Null for the async (G305) path, whose caller does not stay on its frame.
        std::atomic<uint32_t> *doneFlag = nullptr;
        // G415 exact colour-readback window published by the front-end for this submit only.
        G415ColorWindow g415Window;
        std::chrono::steady_clock::time_point g299Enqueue{};
        uint8_t g324Class = 0u; // 1=G310 T8 view, 2=G310 logical composite
        std::chrono::steady_clock::time_point g324Enqueue{};
    };
    struct TexEntry
    {
        GLuint id = 0;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
        // G492: set only by prepareCt24ViewOnWorker, and only for storage IT allocated at (w,h).
        // Lives on the entry so eviction/recreation or a build by any other path clears it — the
        // storage-reuse arm can then never skip an allocation it did not make.
        uint32_t g492Ct24Alloc = 0;
        // G496b: this texture OBJECT's sampler parameters, as last set through g496TexFilter /
        // g496TexWrap. 0 = never set by us (GL's own defaults), which always forces a real call.
        // Object state, so it survives rebinds and batches; glTexImage2D does not disturb it. Only
        // prepareCt24ViewOnWorker parameterises an m_texCache entry outside the draw loop, and it
        // records what it set.
        int g496MinF = 0, g496MagF = 0, g496WrapS = 0, g496WrapT = 0;
    };
    struct FboEntry
    {
        GLuint fbo = 0, colorTex = 0, depthRb = 0;
        int w = 0, h = 0;
        // G415: one parked geometry per key. A target written at two widths (DC2's fbp=0x139 at
        // fbw=8 and fbw=2) otherwise destroys and reallocates both attachments on every flip.
        GLuint spareFbo = 0, spareColorTex = 0, spareDepthRb = 0;
        int spareW = 0, spareH = 0;
    };
    struct DepthEntry
    {
        GLuint tex = 0;
        int w = 0, h = 0;
        bool initialized = false;
    };

    void worker()
    {
        HGLRC rc = wglCreateContext(s_g158MainDC);
        if (!rc || !wglShareLists(s_g158MainRC, rc) || !wglMakeCurrent(s_g158MainDC, rc))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: context setup, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            if (rc)
                wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (!g158LoadGL(m_f) || !g178LoadGLExt(m_e))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: could not resolve GL entry points\n");
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (!setupProgram() || (g256ExactRttOn() && !setupExactProgram()) ||
            (g281T8ViewRequested() && !setupT8ViewProgram()) ||
            (g338Ct24ViewRequested() && !setupCt24ViewProgram()) ||
            (g309Requested() && !setupAuthoritativeCompositeProgram()) ||
            !setupBuffers() ||
            (g257ImageStoreTestOn() && !runG257ImageStoreDiagnostic()))
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        if (g541NullComputeRequested())
        {
            m_g541Available = setupG541NullCompute();
            std::fprintf(stderr, "[G541:null] compute transport %s (async, no readback)\n",
                         m_g541Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g545LiveShadowRequested())
        {
            m_g545Available = setupG545LiveShadow();
            std::fprintf(stderr,
                         "[G545:live] persistent semantic shadow %s (batched, delayed readback)\n",
                         m_g545Available ? "READY" : "UNAVAILABLE; CPU path unchanged");
        }
        if (g542AbiReplayRequested())
            (void)g542RunAbiReplay(m_f, m_e);
        if (g543SemanticReplayRequested())
            (void)g543RunSemanticReplay(m_f, m_e);
        if (g544ChainReplayRequested())
            (void)g544RunChainReplay(m_f, m_e);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        m_ready.store(true, std::memory_order_release);
        m_cvReady.notify_all();
        std::fprintf(stderr, "[G178:backend] PASS: persistent LLE GPU raster thread ready\n");

        for (;;)
        {
            QueueItem item;
            {
                // G447 (second half of the round trip, DC2_G419_AB=wspin / DC2_G447_WSPIN_US): the
                // caller-side spin removes the GS worker's wake, but the SUBMIT direction has the
                // same latency — this thread is asleep when the item is queued and the kernel must
                // schedule it back on. Spin on a submit sequence counter for a bounded budget before
                // falling into the condition variable. Purely a hint: the cv predicate below is
                // still the authority, so the queue discipline and ordering are unchanged.
                // G471 tested both a cheaper and a more backed-off poll here too (the two spins read
                // the same shared QPC page, so they were armed together); see g447WaitFuture above.
                if (g447WorkerSpinActive())
                {
                    const uint64_t seen = m_submitSeq.load(std::memory_order_acquire);
                    const auto until = std::chrono::steady_clock::now() +
                                       std::chrono::microseconds(g447WorkerSpinBudgetUs());
                    while (m_submitSeq.load(std::memory_order_acquire) == seen)
                    {
                        if (std::chrono::steady_clock::now() >= until)
                            break;
                        YieldProcessor();
                    }
                }
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty())
                    break;
                item = std::move(m_queue.front());
                if (item.g541NullBatch)
                {
                    const std::shared_ptr<G541NullBatch> queued = m_g541QueuedBatch.lock();
                    if (queued.get() == item.g541NullBatch.get())
                        m_g541QueuedBatch.reset();
                }
                if (item.g545LiveBatch)
                {
                    const std::shared_ptr<G545LiveBatch> queued = m_g545QueuedBatch.lock();
                    if (queued.get() == item.g545LiveBatch.get())
                        m_g545QueuedBatch.reset();
                }
                m_queue.pop_front();
            }
            if (item.g545LiveBatch)
            {
                if (m_g545Available && !m_g545Disabled.load(std::memory_order_acquire))
                    (void)runG545LiveBatch(*item.g545LiveBatch);
                else
                    m_g545Rejected.fetch_add(item.g545LiveBatch->blobs.size(),
                                             std::memory_order_relaxed);
                continue;
            }
            if (item.g541NullBatch)
            {
                const uint64_t batchKicks = item.g541NullBatch->kicks;
                if (m_g541Available && batchKicks != 0ull)
                {
                    // One GPU-resident dependency word models persistent cross-kick VU state. A
                    // queued batch is one sequential command-buffer dispatch in the normal case.
                    // The barrier orders the following GS consumer on-GPU; no CPU wait/readback.
                    m_f.glUseProgram_(m_g541NullProg);
                    m_e.glBindImageTexture_(7u, m_g541StateTex, 0, GL_FALSE, 0,
                                            GL178_READ_WRITE, GL541_R32UI);
                    uint64_t remaining = batchKicks;
                    uint64_t dispatches = 0ull;
                    while (remaining != 0ull)
                    {
                        const uint32_t chunk = static_cast<uint32_t>(
                            std::min<uint64_t>(remaining, 0x7fffffffull));
                        m_f.glUniform1i_(m_locG541KickCount, static_cast<GLint>(chunk));
                        m_e.glDispatchCompute_(1u, 1u, 1u);
                        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                        remaining -= chunk;
                        ++dispatches;
                    }
                    m_g541Batches.fetch_add(dispatches, std::memory_order_relaxed);
                    uint64_t maxBatch = m_g541MaxBatch.load(std::memory_order_relaxed);
                    while (maxBatch < batchKicks && !m_g541MaxBatch.compare_exchange_weak(
                               maxBatch, batchKicks, std::memory_order_relaxed,
                               std::memory_order_relaxed)) {}
                    const uint64_t n = m_g541Executed.fetch_add(
                        batchKicks, std::memory_order_relaxed) + batchKicks;
                    if (n >= m_g541NextReport)
                    {
                        m_g541NextReport = ((n >> 16u) + 1ull) << 16u;
                        std::fprintf(stderr,
                                     "[G541:null] kicks(sub=%llu exec=%llu reject=%llu) "
                                     "batches=%llu maxBatch=%llu qPeak=%llu\n",
                                     static_cast<unsigned long long>(
                                         m_g541Submitted.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(n),
                                     static_cast<unsigned long long>(
                                         m_g541Rejected.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541Batches.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541MaxBatch.load(std::memory_order_relaxed)),
                                     static_cast<unsigned long long>(
                                         m_g541HighWater.load(std::memory_order_relaxed)));
                    }
                }
                else
                    m_g541Rejected.fetch_add(batchKicks, std::memory_order_relaxed);
                continue;
            }
            // G472 premise census: price how many synchronous render round trips could collapse
            // into one worker item without crossing an observable backend dependency.  Restrict the
            // candidate runs to skip-readback colour batches with no guest-depth side channel and
            // no framebuffer upload; any other backend item, FBO geometry change, or key change is
            // a hard boundary.  Diagnostic only: no queue, promise, or GL behaviour changes.
            static const bool s_g472Census = g158EnvFlag("DC2_G472_CENSUS");
            if (s_g472Census)
            {
                static uint64_t s_renderItems = 0u;
                static uint64_t s_eligibleItems = 0u;
                static uint64_t s_closedRuns = 0u;
                static uint64_t s_avoidableTrips = 0u;
                static uint64_t s_runBins[8]{}; // 1,2,3,4,5-8,9-16,17-32,33+
                // Item classes: eligible render, depth render, upload/other render, readback render,
                // read job, write job, copy job, other.  The successor row explains exactly what
                // prevents eligible submits from becoming a run.
                static uint64_t s_classCounts[8]{};
                static uint64_t s_afterEligible[8]{};
                static unsigned s_prevClass = 8u;
                static uint64_t s_renderPairs = 0u, s_sameFboRenderPairs = 0u;
                static uint64_t s_renderPairClass[16]{};
                static uint64_t s_sameFboPairClass[16]{};
                static bool s_prevRender = false;
                static unsigned s_prevRenderClass = 0u;
                static uint32_t s_prevRenderKey = 0u;
                static int s_prevRenderW = 0, s_prevRenderH = 0;
                static uint64_t s_run = 0u;
                static uint32_t s_key = 0u;
                static int s_w = 0, s_h = 0;
                const bool renderItem =
                    item.batch != nullptr &&
                    !(item.depthReadback != nullptr && !item.renderThenReadDepth);
                const bool eligible =
                    renderItem && item.batch->skipReadback && item.depthKey == 0u &&
                    !item.batch->uploadFb;
                const uint32_t key = eligible
                    ? (item.batch->fboKey != 0u ? item.batch->fboKey : item.batch->fbp)
                    : 0u;
                const int w = eligible ? item.batch->fbW : 0;
                const int h = eligible ? item.batch->fbH : 0;
                unsigned itemClass = 7u;
                if (renderItem)
                {
                    if (eligible)
                        itemClass = 0u;
                    else if (item.depthKey != 0u)
                        itemClass = 1u;
                    else if (item.batch->uploadFb)
                        itemClass = 2u;
                    else
                        itemClass = 3u;
                }
                else if (item.colorReadback != nullptr || item.depthReadback != nullptr)
                    itemClass = 4u;
                else if (item.colorWrite != nullptr)
                    itemClass = 5u;
                else if (item.copyRects != nullptr)
                    itemClass = 6u;
                ++s_classCounts[itemClass];
                if (s_prevClass == 0u)
                    ++s_afterEligible[itemClass];
                s_prevClass = itemClass;
                if (renderItem)
                {
                    const uint32_t renderKey =
                        item.batch->fboKey != 0u ? item.batch->fboKey : item.batch->fbp;
                    if (s_prevRender)
                    {
                        ++s_renderPairs;
                        ++s_renderPairClass[s_prevRenderClass * 4u + itemClass];
                        if (renderKey == s_prevRenderKey && item.batch->fbW == s_prevRenderW &&
                            item.batch->fbH == s_prevRenderH)
                        {
                            ++s_sameFboRenderPairs;
                            ++s_sameFboPairClass[s_prevRenderClass * 4u + itemClass];
                        }
                    }
                    s_prevRender = true;
                    s_prevRenderClass = itemClass;
                    s_prevRenderKey = renderKey;
                    s_prevRenderW = item.batch->fbW;
                    s_prevRenderH = item.batch->fbH;
                }
                else
                {
                    s_prevRender = false;
                }
                const auto closeRun = [&] {
                    if (s_run == 0u)
                        return;
                    ++s_closedRuns;
                    s_avoidableTrips += s_run - 1u;
                    const unsigned bin = s_run <= 4u
                        ? static_cast<unsigned>(s_run - 1u)
                        : (s_run <= 8u ? 4u : (s_run <= 16u ? 5u : (s_run <= 32u ? 6u : 7u)));
                    ++s_runBins[bin];
                    s_run = 0u;
                };
                if (eligible && s_run != 0u && key == s_key && w == s_w && h == s_h)
                {
                    ++s_run;
                }
                else
                {
                    closeRun();
                    if (eligible)
                    {
                        s_run = 1u;
                        s_key = key;
                        s_w = w;
                        s_h = h;
                    }
                }
                if (renderItem)
                    ++s_renderItems;
                if (eligible)
                    ++s_eligibleItems;
                if (renderItem && (s_renderItems % 4096u) == 0u)
                {
                    const uint64_t liveAvoid = s_run != 0u ? s_run - 1u : 0u;
                    std::fprintf(stderr,
                                 "[G472:census] render=%llu eligible=%llu runs=%llu live=%llu "
                                 "avoidable=%llu bins=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "class=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "afterElig=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "renderPairs=%llu sameFbo=%llu "
                                 "pairClass=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                                 "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                                 "sameClass=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                                 "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
                                 static_cast<unsigned long long>(s_renderItems),
                                 static_cast<unsigned long long>(s_eligibleItems),
                                 static_cast<unsigned long long>(s_closedRuns + (s_run != 0u ? 1u : 0u)),
                                 static_cast<unsigned long long>(s_run),
                                 static_cast<unsigned long long>(s_avoidableTrips + liveAvoid),
                                 static_cast<unsigned long long>(s_runBins[0]),
                                 static_cast<unsigned long long>(s_runBins[1]),
                                 static_cast<unsigned long long>(s_runBins[2]),
                                 static_cast<unsigned long long>(s_runBins[3]),
                                 static_cast<unsigned long long>(s_runBins[4]),
                                 static_cast<unsigned long long>(s_runBins[5]),
                                 static_cast<unsigned long long>(s_runBins[6]),
                                 static_cast<unsigned long long>(s_runBins[7]),
                                 static_cast<unsigned long long>(s_classCounts[0]),
                                 static_cast<unsigned long long>(s_classCounts[1]),
                                 static_cast<unsigned long long>(s_classCounts[2]),
                                 static_cast<unsigned long long>(s_classCounts[3]),
                                 static_cast<unsigned long long>(s_classCounts[4]),
                                 static_cast<unsigned long long>(s_classCounts[5]),
                                 static_cast<unsigned long long>(s_classCounts[6]),
                                 static_cast<unsigned long long>(s_classCounts[7]),
                                 static_cast<unsigned long long>(s_afterEligible[0]),
                                 static_cast<unsigned long long>(s_afterEligible[1]),
                                 static_cast<unsigned long long>(s_afterEligible[2]),
                                 static_cast<unsigned long long>(s_afterEligible[3]),
                                 static_cast<unsigned long long>(s_afterEligible[4]),
                                 static_cast<unsigned long long>(s_afterEligible[5]),
                                 static_cast<unsigned long long>(s_afterEligible[6]),
                                 static_cast<unsigned long long>(s_afterEligible[7]),
                                 static_cast<unsigned long long>(s_renderPairs),
                                 static_cast<unsigned long long>(s_sameFboRenderPairs),
                                 static_cast<unsigned long long>(s_renderPairClass[0]),
                                 static_cast<unsigned long long>(s_renderPairClass[1]),
                                 static_cast<unsigned long long>(s_renderPairClass[2]),
                                 static_cast<unsigned long long>(s_renderPairClass[3]),
                                 static_cast<unsigned long long>(s_renderPairClass[4]),
                                 static_cast<unsigned long long>(s_renderPairClass[5]),
                                 static_cast<unsigned long long>(s_renderPairClass[6]),
                                 static_cast<unsigned long long>(s_renderPairClass[7]),
                                 static_cast<unsigned long long>(s_renderPairClass[8]),
                                 static_cast<unsigned long long>(s_renderPairClass[9]),
                                 static_cast<unsigned long long>(s_renderPairClass[10]),
                                 static_cast<unsigned long long>(s_renderPairClass[11]),
                                 static_cast<unsigned long long>(s_renderPairClass[12]),
                                 static_cast<unsigned long long>(s_renderPairClass[13]),
                                 static_cast<unsigned long long>(s_renderPairClass[14]),
                                 static_cast<unsigned long long>(s_renderPairClass[15]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[0]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[1]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[2]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[3]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[4]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[5]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[6]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[7]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[8]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[9]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[10]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[11]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[12]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[13]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[14]),
                                 static_cast<unsigned long long>(s_sameFboPairClass[15]));
                }
            }
            bool ok = false;
            const bool g299ProfileBatch =
                item.batch != nullptr && item.depthReadback == nullptr;
            const auto g299WorkStart =
                g299ProfileBatch && g299ProfileOn()
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
            const bool g324Profile =
                item.g324Class != 0u && g324BackendArmed();
            const auto g324WorkStart = g324Profile
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            // G332: classify this item into one backend GL-busy bucket before the work runs.
            const bool g332Armed = g332BackendArmed();
            uint32_t g332Cls = 0u; // 0=render(submit) 1=readback 2=copy 3=other
            if (g332Armed)
            {
                if (item.g309PageMap != nullptr || item.g281Map != nullptr ||
                    item.g338Ct24View || item.colorWrite != nullptr)
                    g332Cls = 3u; // decode / composite / upload
                else if (item.copyRects != nullptr)
                    g332Cls = 2u; // GPU->GPU alias copy
                else if (item.colorReadback != nullptr ||
                         (item.depthReadback != nullptr && !item.renderThenReadDepth))
                    g332Cls = 1u; // blocking glReadPixels
                else
                    g332Cls = 0u; // renderBatch native submission (incl. renderThenReadDepth)
            }
            const auto g332WorkStart = g332Armed ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
            const bool g453On = g453BackendCensusOn();
            uint32_t g453Cls = 4u;
            if (g453On)
            {
                if (item.g281Map != nullptr)
                    g453Cls = 0u;
                else if (item.g338Ct24View)
                    g453Cls = 1u;
                else if (item.g309PageMap != nullptr)
                    g453Cls = 2u;
                else if (item.colorWrite != nullptr)
                    g453Cls = 3u;
            }
            const auto g453WorkStart = g453Cls < 4u ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
            // G425 readback census keeps its own clock so it does not depend on G332 being armed.
            static const bool s_g425RbOn = (std::getenv("DC2_G425_RBCENSUS") != nullptr);
            const auto g425WorkStart = s_g425RbOn ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
            // G495 queued-ahead census (DC2_G495_RB=1, default OFF). Classify and price this item
            // BEFORE it runs, so a readback never counts itself in its own "ahead" set, and mark
            // whether it is a pipeline fence so the accumulator can be cleared after it.
            bool g495Fence = false;
            int g495Cls = kG495ClsRenderSkip;
            if (g495RbCensusOn() || g495GpuCensusOn())
            {
                uint64_t g495Draws = 0ull, g495Verts = 0ull, g495TexN = 0ull;
                uint64_t g495TexBytes = 0ull, g495FbBytes = 0ull;
                if (item.g309PageMap != nullptr)
                    g495Cls = kG495ClsComposite;
                else if (item.g281Map != nullptr)
                    g495Cls = kG495ClsT8;
                else if (item.g338Ct24View)
                    g495Cls = kG495ClsCt24;
                else if (item.copyRects != nullptr)
                    g495Cls = kG495ClsCopy;
                else if (item.colorReadback != nullptr)
                {
                    g495Cls = kG495ClsReadColor;
                    g495Fence = true;
                }
                else if (item.colorWrite != nullptr)
                {
                    g495Cls = kG495ClsWriteColor;
                    g495TexBytes = static_cast<uint64_t>(item.colorWrite->size()) * 4ull;
                }
                else if (item.depthReadback != nullptr && !item.renderThenReadDepth)
                {
                    g495Cls = kG495ClsReadDepth;
                    g495Fence = true;
                }
                else if (item.batch != nullptr)
                {
                    g495Cls = item.batch->skipReadback ? kG495ClsRenderSkip : kG495ClsRenderRb;
                    g495Fence = !item.batch->skipReadback;
                    g495Draws = item.batch->draws.size();
                    g495Verts = item.batch->verts.size();
                    g495TexN = item.batch->texUploads.size();
                    for (const auto &u : item.batch->texUploads)
                        g495TexBytes += static_cast<uint64_t>(u.px.size()) * 4ull;
                    g495FbBytes = static_cast<uint64_t>(item.batch->fbPixels.size()) * 4ull;
                }
                g495NoteItem(g495Cls, g495Draws, g495Verts, g495TexN, g495TexBytes, g495FbBytes);
            }
            // G495b GPU-side clock (DC2_G495_GPU=1): GL_TIME_ELAPSED around this item's whole GL
            // body, so the per-class sum is GPU busy ms/f. Never quote a frame time from this run.
            g495GpuBegin(g495Cls);
            try
            {
                if (item.g309PageMap != nullptr)
                    ok = buildAuthoritativeCompositeOnWorker(
                        *item.g309PageMap, *item.g309BasePixels,
                        item.g309Readback, item.g309ForceDraw,
                        item.g309OutputFbp);
                else if (item.g281Map != nullptr)
                    ok = prepareT8ViewOnWorker(
                        item.g281TexKey, item.g281SrcFbp, item.g281SrcFbW,
                        item.g281TexW, item.g281TexH, *item.g281Map, *item.g281Clut,
                        item.g281Verify, *item.g281VerifyBad);
                else if (item.g338Ct24View)
                    ok = prepareCt24ViewOnWorker(
                        item.g281TexKey, item.g281SrcFbp, item.g281SrcFbW,
                        item.g281TexW, item.g281TexH, item.g338Ta0, item.g338Aem,
                        item.g281Verify, *item.g281VerifyBad);
                else if (item.copyRects != nullptr)
                {
                    if (item.copyEnsureDest)
                    {
                        FboEntry &dst =
                            ensureFbo(item.colorFbp, item.colorTexW, item.colorTexH);
                        ok = dst.fbo != 0 &&
                             copyColorRectsOnWorker(item.copySrcFbp, item.colorFbp,
                                                    *item.copyRects);
                    }
                    else
                        ok = copyColorRectsOnWorker(item.copySrcFbp, item.colorFbp,
                                                    *item.copyRects);
                }
                else if (item.colorReadback != nullptr)
                    ok = readColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                           item.colorY, item.colorRows, *item.colorReadback);
                else if (item.colorWrite != nullptr)
                    ok = writeColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                            item.colorX, item.colorY, item.colorW,
                                            item.colorRows, *item.colorWrite);
                else if (item.depthReadback != nullptr && !item.renderThenReadDepth)
                    ok = readDepthOnWorker(item.depthKey, item.batch->fbW, item.batch->fbH,
                                           item.depthReadY, item.depthReadRows,
                                           *item.depthReadback);
                else
                    ok = renderBatch(*item.batch, item.depthKey, item.depthUpload,
                                     item.depthY, item.depthRows,
                                     item.renderThenReadDepth ? item.depthReadback : nullptr,
                                     item.depthReadY, item.depthReadRows,
                                     item.g415Window);
            }
            catch (...)
            {
                ok = false;
            }
            g495GpuEnd();
            // G425: readback-redundancy ceiling census (default OFF, DC2_G425_RBCENSUS=1).
            // Question: how much of the GS worker's blocking colour-readback time re-reads a
            // window that NOTHING has mutated since the previous readback of the same window?
            // The mutation clock is deliberately GLOBAL (any render/copy/write/composite item
            // ticks it), so the answer is a strict LOWER bound on what a per-surface readback
            // cache could recover — exactly the ceiling G420's rule demands before building one.
            {
                if (s_g425RbOn)
                {
                    static uint64_t s_mutSeq = 0ull;
                    static std::unordered_map<uint64_t, uint64_t> s_lastSeq;
                    static uint64_t s_reads = 0ull, s_redundant = 0ull;
                    static uint64_t s_readNs = 0ull, s_redundantNs = 0ull;
                    const bool isReadback = item.colorReadback != nullptr;
                    if (!isReadback)
                        ++s_mutSeq;
                    else
                    {
                        const uint64_t key =
                            ((uint64_t)item.colorFbp << 40) ^
                            ((uint64_t)(uint32_t)item.colorY << 24) ^
                            ((uint64_t)(uint32_t)item.colorRows << 12) ^
                            (uint64_t)(uint32_t)item.colorTexW;
                        const uint64_t ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - g425WorkStart).count());
                        ++s_reads;
                        s_readNs += ns;
                        auto it = s_lastSeq.find(key);
                        if (it != s_lastSeq.end() && it->second == s_mutSeq)
                        {
                            ++s_redundant;
                            s_redundantNs += ns;
                        }
                        if (it != s_lastSeq.end())
                            it->second = s_mutSeq;
                        else
                            s_lastSeq.emplace(key, s_mutSeq);
                        if ((s_reads % 2000ull) == 0ull)
                            std::fprintf(stderr,
                                "[G425:rb] reads=%llu ms=%.1f | redundant=%llu (%.1f%%) ms=%.1f "
                                "(%.1f%% ofReadbackMs) distinctWindows=%zu\n",
                                (unsigned long long)s_reads, s_readNs / 1e6,
                                (unsigned long long)s_redundant,
                                100.0 * (double)s_redundant / (double)s_reads,
                                s_redundantNs / 1e6,
                                100.0 * (double)s_redundantNs /
                                    (double)(s_readNs ? s_readNs : 1ull),
                                s_lastSeq.size());
                    }
                }
            }
            // G445 extension probe: G418's kick covers only the skip-readback RENDER batches. The
            // alias copies and the decode/composite/upload class leave GL work queued and never
            // fence either, so they feed the same readback drain. Kick them on the same exactness
            // argument as G418 (glFlush changes no result, no GL state, no command order, no
            // ownership). Never kick a readback item: it has already fenced by construction.
            {
                const bool g445Kickable =
                    item.g309PageMap != nullptr || item.g281Map != nullptr ||
                    item.g338Ct24View || item.colorWrite != nullptr ||
                    item.copyRects != nullptr;
                if (g445Kickable && g445KickAllActive())
                    glFlush();
            }
            if (g332Armed)
            {
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g332WorkStart).count());
                if (g332BackendCensusOn())
                {
                    g_g332BackendNs[g332Cls].fetch_add(workNs, std::memory_order_relaxed);
                    g_g332BackendCount[g332Cls].fetch_add(1u, std::memory_order_relaxed);
                }
            }
            if (g453Cls < 4u)
            {
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g453WorkStart).count());
                g_g453BackendOtherNs[g453Cls].fetch_add(workNs, std::memory_order_relaxed);
                g_g453BackendOtherCount[g453Cls].fetch_add(1u, std::memory_order_relaxed);
            }
            // G494 failsafe: while the sync-query elision is armed, one real glGetError every 64th
            // backend item (~1.1/frame against ~110 elided) catches anything the elided checks would
            // have caught, one frame late, and latches them all back on. Inert otherwise.
            g494SentinelTick();
            // G495: a fence has just emptied the pipeline; everything after it is a new "ahead" set.
            if (g495Fence)
                g495FenceReset();
            // G495 per-surface mutation clock (census only). Only items whose OUTPUT surface is
            // identified by an fbp tick it; anything else bumps the unknown epoch, which unproves
            // every cached window. T8/CT24 views write a texture object, not a target FBO.
            if (g495RbCensusOn())
            {
                if (g495Cls == kG495ClsRenderSkip || g495Cls == kG495ClsRenderRb)
                    g495NoteWrite(item.batch->fbp, item.batch->fboKey != 0u);
                else if (g495Cls == kG495ClsCopy || g495Cls == kG495ClsWriteColor)
                    g495NoteWrite(item.colorFbp, false);
                else if (g495Cls == kG495ClsComposite)
                    g495NoteWrite(item.g309OutputFbp, item.g309OutputFbp == 0u);
            }
            if (g324Profile)
            {
                const auto done = std::chrono::steady_clock::now();
                const uint64_t queueNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g324WorkStart - item.g324Enqueue).count());
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        done - g324WorkStart).count());
                if (g324BackendCensusOn())
                {
                    const unsigned cls = std::min<unsigned>(item.g324Class, 2u);
                    ++m_g324Count[cls];
                    m_g324QueueNs[cls] += queueNs;
                    m_g324WorkNs[cls] += workNs;
                    ++m_g324Total;
                    if ((m_g324Total % 256u) == 0u)
                    {
                        std::fprintf(stderr, "[G324:backend] total=%llu",
                                     static_cast<unsigned long long>(m_g324Total));
                        for (unsigned i = 1u; i <= 2u; ++i)
                        {
                            const uint64_t n = m_g324Count[i];
                            if (n == 0u)
                                continue;
                            std::fprintf(
                                stderr,
                                " c%u(n=%llu q=%.3fms work=%.3fms qTotal=%.3fms workTotal=%.3fms)",
                                i, static_cast<unsigned long long>(n),
                                static_cast<double>(m_g324QueueNs[i]) / 1.0e6 / n,
                                static_cast<double>(m_g324WorkNs[i]) / 1.0e6 / n,
                                static_cast<double>(m_g324QueueNs[i]) / 1.0e6,
                                static_cast<double>(m_g324WorkNs[i]) / 1.0e6);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            if (g299ProfileBatch && g299ProfileOn())
            {
                const auto done = std::chrono::steady_clock::now();
                const uint64_t queueNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g299WorkStart - item.g299Enqueue).count());
                const uint64_t workNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        done - g299WorkStart).count());
                const G178Batch &b = *item.batch;
                uint64_t packetBytes =
                    static_cast<uint64_t>(b.fbPixels.size()) * sizeof(uint32_t) +
                    static_cast<uint64_t>(b.verts.size()) * sizeof(G178Vtx) +
                    static_cast<uint64_t>(b.draws.size()) * sizeof(G178Draw);
                for (const G178TexUpload &up : b.texUploads)
                    packetBytes += static_cast<uint64_t>(up.px.size()) * sizeof(uint32_t);
                if (item.depthUpload != nullptr)
                    packetBytes += static_cast<uint64_t>(item.depthUpload->size()) *
                                   sizeof(float);
                const unsigned cls =
                    (b.skipReadback ? 1u : 0u) |
                    (item.depthKey != 0u ? 2u : 0u);
                ++m_g299Count[cls];
                m_g299QueueNs[cls] += queueNs;
                m_g299WorkNs[cls] += workNs;
                m_g299PacketBytes[cls] += packetBytes;
                m_g299Draws[cls] += b.draws.size();
                ++m_g299Total;
                if ((m_g299Total % 300u) == 0u)
                {
                    std::fprintf(stderr, "[G299:backend] total=%llu",
                                 static_cast<unsigned long long>(m_g299Total));
                    for (unsigned i = 0; i < 4u; ++i)
                    {
                        const uint64_t n = m_g299Count[i];
                        if (n == 0u)
                            continue;
                        std::fprintf(
                            stderr,
                            " c%u(n=%llu q=%.3fms work=%.3fms packet=%.1fKiB draws=%.1f)",
                            i, static_cast<unsigned long long>(n),
                            static_cast<double>(m_g299QueueNs[i]) / 1.0e6 / n,
                            static_cast<double>(m_g299WorkNs[i]) / 1.0e6 / n,
                            static_cast<double>(m_g299PacketBytes[i]) / 1024.0 / n,
                            static_cast<double>(m_g299Draws[i]) / n);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            // G447: publish completion to a spinning caller BEFORE satisfying the promise (see the
            // lifetime argument on QueueItem::doneFlag). Exact: the caller still takes its result
            // from the future, so nothing about the value or its ordering changes.
            if (item.doneFlag != nullptr)
                item.doneFlag->store(1u, std::memory_order_release);
            item.promise->set_value(ok);
        }

        if (m_g545Available)
            reportG545Status(true);
        m_ready.store(false, std::memory_order_release);
        for (auto &kv : m_fbos)
            destroyFbo(kv.second);
        m_fbos.clear();
        for (auto &kv : m_depths)
            if (kv.second.tex != 0)
                glDeleteTextures(1, &kv.second.tex);
        m_depths.clear();
        for (auto &kv : m_texCache)
            if (kv.second.id != 0)
                glDeleteTextures(1, &kv.second.id);
        m_texCache.clear();
        m_texBytes = 0;
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.clear();
        }
        if (m_vbo != 0)
            m_f.glDeleteBuffers_(1, &m_vbo);
        if (m_vao != 0)
            m_f.glDeleteVertexArrays_(1, &m_vao);
        if (m_prog != 0)
            m_f.glDeleteProgram_(m_prog);
        if (m_progExact != 0)
            m_f.glDeleteProgram_(m_progExact);
        if (m_g541NullProg != 0)
            m_f.glDeleteProgram_(m_g541NullProg);
        if (m_g541StateTex != 0)
            glDeleteTextures(1, &m_g541StateTex);
        if (m_g545Prog != 0)
            m_f.glDeleteProgram_(m_g545Prog);
        if (m_g545CommandTex != 0)
            glDeleteTextures(1, &m_g545CommandTex);
        if (m_g545PayloadTex != 0)
            glDeleteTextures(1, &m_g545PayloadTex);
        if (m_g545WorkspaceTex != 0)
            glDeleteTextures(1, &m_g545WorkspaceTex);
        if (m_g281Prog != 0)
            m_f.glDeleteProgram_(m_g281Prog);
        if (m_g281MapTex != 0)
            glDeleteTextures(1, &m_g281MapTex);
        if (m_g281ClutTex != 0)
            glDeleteTextures(1, &m_g281ClutTex);
        if (m_g281Fbo != 0)
            m_f.glDeleteFramebuffers_(1, &m_g281Fbo);
        if (m_g309Prog != 0)
            m_f.glDeleteProgram_(m_g309Prog);
        if (m_g309MapTex != 0)
            glDeleteTextures(1, &m_g309MapTex);
        if (m_g309BaseTex != 0)
            glDeleteTextures(1, &m_g309BaseTex);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }

    bool setupG545LiveShadow()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        std::string why;
        if (!g545LinkProgram(m_f, m_g545Prog, m_locG545CommandCount, why))
        {
            std::fprintf(stderr, "[G545:live] setup failed: %s\n", why.c_str());
            if (m_g545Prog != 0u)
                m_f.glDeleteProgram_(m_g545Prog);
            m_g545Prog = 0u;
            return false;
        }

        while (glGetError() != GL_NO_ERROR) {}
        const uint32_t rows = (G545_LIVE_WORKSPACE_WORDS + G542_IMAGE_WIDTH - 1u) /
                              G542_IMAGE_WIDTH;
        std::vector<uint32_t> zero(static_cast<size_t>(rows) * G542_IMAGE_WIDTH, 0u);
        glGenTextures(1, &m_g545WorkspaceTex);
        glBindTexture(GL_TEXTURE_2D, m_g545WorkspaceTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                     static_cast<GLsizei>(G542_IMAGE_WIDTH), static_cast<GLsizei>(rows), 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, zero.data());
        const GLenum err = glGetError();
        if (m_g545WorkspaceTex == 0u || err != GL_NO_ERROR)
        {
            std::fprintf(stderr, "[G545:live] workspace setup failed gl=0x%x\n",
                         static_cast<unsigned>(err));
            return false;
        }

        m_g545CheckInterval = [] {
            const char *v = std::getenv("DC2_G545_STATUS_CHECK_KICKS");
            if (v == nullptr || v[0] == '\0')
                return 16384ull;
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(v, &end, 0);
            if (end == v)
                return 16384ull;
            return std::max<unsigned long long>(256ull,
                std::min<unsigned long long>(1048576ull, parsed));
        }();
        m_g545NextCheck = m_g545CheckInterval;
        return true;
    }

    void reportG545Status(bool final)
    {
        const uint64_t executed = m_g545Executed.load(std::memory_order_relaxed);
        if (executed == 0ull || (final && executed == m_g545Verified))
            return;
        const uint32_t rows = (G545_LIVE_WORKSPACE_WORDS + G542_IMAGE_WIDTH - 1u) /
                              G542_IMAGE_WIDTH;
        std::vector<uint32_t> image(static_cast<size_t>(rows) * G542_IMAGE_WIDTH, 0u);
        while (glGetError() != GL_NO_ERROR) {}
        glBindTexture(GL_TEXTURE_2D, m_g545WorkspaceTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, image.data());
        const GLenum err = glGetError();
        ++m_g545Readbacks;
        if (err != GL_NO_ERROR || image[0] != G545_LIVE_STATUS_MAGIC || image[1] != 1u)
        {
            std::fprintf(stderr,
                         "[G545:live] CHECK FAIL status/readback gl=0x%x magic=%08x version=%u; shadow disabled, CPU path unchanged\n",
                         static_cast<unsigned>(err), image[0], image[1]);
            m_g545Disabled.store(true, std::memory_order_release);
            return;
        }
        const uint64_t gpuExecuted = static_cast<uint64_t>(image[8]) |
                                     (static_cast<uint64_t>(image[9]) << 32u);
        const uint64_t lastSequence = static_cast<uint64_t>(image[10]) |
                                      (static_cast<uint64_t>(image[11]) << 32u);
        if (image[2] != 0u || gpuExecuted != executed)
        {
            const uint64_t failSequence = static_cast<uint64_t>(image[3]) |
                                          (static_cast<uint64_t>(image[4]) << 32u);
            std::fprintf(stderr,
                         "[G545:live] CHECK FAIL errors=%08x failSeq=%llu batchRec=%u pc=%04x word=%08x gpuExec=%llu hostExec=%llu; shadow disabled, CPU path unchanged\n",
                         image[2], static_cast<unsigned long long>(failSequence), image[5],
                          image[6], image[7], static_cast<unsigned long long>(gpuExecuted),
                          static_cast<unsigned long long>(executed));
            if ((image[2] & 0x20u) != 0u)
            {
                std::fprintf(stderr,
                             "[G545:live] packet detail gpu=%02x cpu=%02x offset=%u\n",
                             (image[7] >> 24u) & 0xffu, (image[7] >> 16u) & 0xffu,
                             image[7] & 0xffffu);
            }
            m_g545Disabled.store(true, std::memory_order_release);
            return;
        }

        m_g545Verified = executed;
        std::fprintf(stderr,
                     "[G545:live] %s PASS kicks=%llu lastSeq=%llu batches=%llu maxBatch=%llu packets=%u dataChecks=%u seeds=%u readbacks=%llu qPeak=%llu CPU-authoritative=1\n",
                     final ? "FINAL" : "CHECK", static_cast<unsigned long long>(executed),
                     static_cast<unsigned long long>(lastSequence),
                     static_cast<unsigned long long>(m_g545Batches),
                     static_cast<unsigned long long>(m_g545MaxBatch), image[13], image[14],
                     image[15], static_cast<unsigned long long>(m_g545Readbacks),
                     static_cast<unsigned long long>(m_g545HighWater.load(std::memory_order_relaxed)));
    }

    bool runG545LiveBatch(const G545LiveBatch &batch)
    {
        std::vector<uint32_t> commands;
        std::vector<uint32_t> payload;
        std::string why;
        if (!g545PackBatch(batch, commands, payload, why))
        {
            std::fprintf(stderr, "[G545:live] batch rejected: %s; shadow disabled\n", why.c_str());
            m_g545Disabled.store(true, std::memory_order_release);
            return false;
        }
        if (m_g545Batches == 0ull)
            std::fprintf(stderr,
                         "[G545:live] first dispatch commands=%zu payloadWords=%zu watchdogCap=%zu\n",
                         batch.blobs.size(), payload.size(), G545_LIVE_MAX_BATCH_COMMANDS);
        while (glGetError() != GL_NO_ERROR) {}
        if (!g545UploadImage(m_g545CommandTex, m_g545CommandRows, commands, why) ||
            !g545UploadImage(m_g545PayloadTex, m_g545PayloadRows, payload, why))
        {
            std::fprintf(stderr, "[G545:live] upload failed: %s; shadow disabled\n", why.c_str());
            m_g545Disabled.store(true, std::memory_order_release);
            return false;
        }

        m_f.glUseProgram_(m_g545Prog);
        m_e.glBindImageTexture_(2u, m_g545CommandTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(3u, m_g545PayloadTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(4u, m_g545WorkspaceTex, 0, GL_FALSE, 0,
                                GL178_READ_WRITE, GL541_R32UI);
        m_f.glUniform1i_(m_locG545CommandCount, static_cast<GLint>(batch.blobs.size()));
        m_e.glDispatchCompute_(1u, 1u, 1u);
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT);
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            std::fprintf(stderr,
                         "[G545:live] dispatch failed gl=0x%x; shadow disabled, CPU path unchanged\n",
                         static_cast<unsigned>(err));
            m_g545Disabled.store(true, std::memory_order_release);
            return false;
        }

        ++m_g545Batches;
        m_g545MaxBatch = std::max<uint64_t>(m_g545MaxBatch, batch.blobs.size());
        const uint64_t executed = m_g545Executed.fetch_add(batch.blobs.size(),
            std::memory_order_relaxed) + batch.blobs.size();
        if (executed >= m_g545NextCheck)
        {
            reportG545Status(false);
            m_g545NextCheck = executed + m_g545CheckInterval;
        }
        return !m_g545Disabled.load(std::memory_order_acquire);
    }

    bool setupG541NullCompute()
    {
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
            return false;

        static const char *kCS =
            "#version 430 core\n"
            "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;\n"
            "layout(r32ui, binding=7) uniform coherent uimage2D gState;\n"
            "uniform int uKickCount;\n"
            "void main(){\n"
            "  uint v = imageLoad(gState, ivec2(0,0)).r;\n"
            "  imageStore(gState, ivec2(0,0), uvec4(v + uint(uKickCount), 0u, 0u, 0u));\n"
            "}\n";
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, kCS, cs, log))
        {
            std::fprintf(stderr, "[G541:null] compute compile failed: %s\n", log.c_str());
            return false;
        }
        m_g541NullProg = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g541NullProg, cs);
        m_f.glLinkProgram_(m_g541NullProg);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g541NullProg, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g541NullProg, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g541NullProg, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G541:null] compute link failed: %s\n", buf.data());
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }
        m_locG541KickCount = m_f.glGetUniformLocation_(m_g541NullProg, "uKickCount");
        if (m_locG541KickCount < 0)
        {
            std::fprintf(stderr, "[G541:null] missing uKickCount uniform\n");
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }

        while (glGetError() != GL_NO_ERROR) {}
        const uint32_t zero = 0u;
        glGenTextures(1, &m_g541StateTex);
        glBindTexture(GL_TEXTURE_2D, m_g541StateTex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI), 1, 1, 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        const GLenum err = glGetError();
        if (m_g541StateTex == 0u || err != GL_NO_ERROR)
        {
            std::fprintf(stderr, "[G541:null] state image setup failed gl=0x%x\n",
                         static_cast<unsigned>(err));
            if (m_g541StateTex != 0u)
                glDeleteTextures(1, &m_g541StateTex);
            m_g541StateTex = 0u;
            m_f.glDeleteProgram_(m_g541NullProg);
            m_g541NullProg = 0u;
            return false;
        }
        return true;
    }

    bool setupProgram()
    {
        // Vertex positions are already screen pixels; depth is written in the fragment shader
        // from the raw GS Z (noperspective varying) so the tiny title Z values keep full float
        // precision near 0 instead of being crushed by the [-1,1] clip-space round-trip.
        // ALL varyings are `noperspective`: the CPU rasterizer interpolates color, s/|q|, t/|q|
        // and 1/|q| linearly in screen space (barycentric on screen X/Y) — see drawTriangle.
        static const char *kVS =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aSTQ;\n"
            "layout(location=2) in vec4 aColor;\n"
            "layout(location=3) in vec4 aFog;\n"
            "uniform vec2 uFbSize;\n"
            "noperspective out vec3 vSTQ;\n"
            "noperspective out vec4 vColor;\n"
            "noperspective out float vZ;\n"
            "noperspective out float vFog;\n"
            "void main(){\n"
            "    vSTQ = aSTQ;\n"
            "    vColor = aColor;\n"
            "    vFog = aFog.x;\n"
            "    vZ = aPos.z;\n"
            "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
            "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
            "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
            "}\n";
        // uMode bits: 0 = textured, 1..2 = TFX, 3 = TCC, 4 = clamp Z to PSMZ24,
        // 5 = store raw source alpha (G255 RTT second pass), 6 = G265 GEQUAL alpha test,
        // 7..8 = G265 AREF code (0=1, 1=64, 2=127), 9 = G286 exact opaque sampler/combine,
        // 10 = G289 CT24 direct-owner source (force TEXA TA0=128),
        // 11 = G309 direct authoritative-page source selection,
        // 12 = G362 guarded fixed-point POINT fetch (see below),
        // 13 = G364 PRIM.FGE (blend RGB towards uFogCol by the interpolated per-vertex F),
        // 14 = G406 accurate GS 12.4 UV rounding for axis-aligned point sprites,
        //
        // G362: `texture()` with GL_NEAREST decides the texel from floor() of an interpolated
        // float. The front-end shifts FST sprite UVs by -0.5 texel-per-pixel so a fragment's
        // center maps to the CPU sampler's texel coordinate exactly — which puts every sample
        // of an axis-aligned 2D sprite EXACTLY on a texel boundary. Whether floor() then returns
        // the intended texel or the one before it is decided by the last ULP of the raster
        // interpolation, so a whole row/column of a menu sprite could sample the neighbouring
        // atlas patch (Items-menu grey slot panel: brown separator rows bleeding in at the cell
        // seams; per-icon black notch from the atlas row above the icon tile). The CPU path
        // computes the same coordinate in exact 12.4 fixed point and truncates, so it never
        // mis-picks. Bit 12 routes point-sampled draws through sampleExact()'s integer branch,
        // which reconstructs the 12.4 coordinate with the G258 +0.5001 rounding guard and is
        // therefore decided by the intended mapping instead of by float error.
        // PS2 color/alpha scale: 0x80 == 1.0, so MODULATE is (t*c)>>7 -> t*c*255/128 in [0,1]
        // space (1.9921875), clamped — identical to combineTexture(). The written alpha is the
        // BLEND FACTOR encoding (As/128 clamped to 1): glBlendFunc consumes it as SRC_ALPHA. The
        // stored framebuffer alpha therefore differs from the CPU path (which stores raw src A);
        // nothing on the title path reads framebuffer alpha (present + dumps are RGB) — G179
        // must revisit (dual-source blending) before any route that samples an RTT's alpha.
        static const char *kFS =
            "#version 330 core\n"
            "noperspective in vec3 vSTQ;\n"
            "noperspective in vec4 vColor;\n"
            "noperspective in float vZ;\n"
            "noperspective in float vFog;\n"
            "out vec4 o;\n"
            "uniform int uFogCol;\n" // G364: packed 0x00BBGGRR (int keeps to the loaded uniform set)
            "uniform sampler2D uTex;\n"
            "uniform sampler2D uG309Src1;\n"
            "uniform sampler2D uG309Src2;\n"
            "uniform sampler2D uG309Src3;\n"
            "uniform sampler2D uG309Src4;\n"
            "uniform sampler2D uG309PageMap;\n"
            "uniform sampler2D uG309Base;\n"
            "uniform int uMode;\n"
            "uniform int uLinear;\n"
            "uniform int uWrapU;\n"
            "uniform int uWrapV;\n"
            "uniform vec2 uFbSize;\n"
            "uniform int uUvOriginX;\n"
            "uniform int uUvOriginY;\n"
            "uniform int uUvRoundU;\n"
            "uniform int uUvRoundV;\n"
            "int wrapCoord(int p, int n, int mode){\n"
            "    if (mode != 0) return clamp(p, 0, n - 1);\n"
            "    int r = p % n; return r < 0 ? r + n : r;\n"
            "}\n"
            "int floorDiv16(int v){ return v >= 0 ? v / 16 : -((-v + 15) / 16); }\n"
            "vec4 g309Owner(int owner, ivec2 p){\n"
            "    if (owner == 0) return texelFetch(uTex, p, 0);\n"
            "    if (owner == 1) return texelFetch(uG309Src1, p, 0);\n"
            "    if (owner == 2) return texelFetch(uG309Src2, p, 0);\n"
            "    if (owner == 3) return texelFetch(uG309Src3, p, 0);\n"
            "    return texelFetch(uG309Src4, p, 0);\n"
            "}\n"
            "vec4 g309Filtered(int owner, vec2 uv){\n"
            "    if (owner == 0) return texture(uTex, uv);\n"
            "    if (owner == 1) return texture(uG309Src1, uv);\n"
            "    if (owner == 2) return texture(uG309Src2, uv);\n"
            "    if (owner == 3) return texture(uG309Src3, uv);\n"
            "    return texture(uG309Src4, uv);\n"
            "}\n"
            "vec4 g309Pixel(ivec2 p){\n"
            "    p = clamp(p, ivec2(0), ivec2(511));\n"
            "    int gsY = 511 - p.y;\n"
            "    ivec2 page = ivec2(p.x >> 6, gsY >> 5);\n"
            "    ivec2 local = ivec2(p.x & 63, gsY & 31);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uG309PageMap,page,0)*255.0+0.5)),0,255);\n"
            "    if (m.r == 0) return texelFetch(uG309Base, p, 0);\n"
            "    ivec2 srcGs = ivec2(m.g * 64 + local.x, m.b * 32 + local.y);\n"
            "    return g309Owner(m.r - 1, ivec2(srcGs.x, 511 - srcGs.y));\n"
            "}\n"
            "vec4 g309Linear(vec2 uv){\n"
            "    vec2 q = uv * 512.0 - vec2(0.5);\n"
            "    ivec2 p = ivec2(floor(q)); vec2 f = fract(q);\n"
            "    if (all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,ivec2(511))) &&\n"
            "        (p.x >> 6) == ((p.x + 1) >> 6) &&\n"
            "        ((511-p.y) >> 5) == ((510-p.y) >> 5)) {\n"
            "        int gsY = 511-p.y; ivec2 page=ivec2(p.x>>6,gsY>>5);\n"
            "        ivec4 m=clamp(ivec4(floor(texelFetch(uG309PageMap,page,0)*255.0+0.5)),0,255);\n"
            "        if (m.r == 0) return texture(uG309Base, uv);\n"
            "        int owner=m.r-1; ivec2 local=ivec2(p.x&63,gsY&31);\n"
            "        ivec2 srcGs=ivec2(m.g*64+local.x,m.b*32+local.y);\n"
            "        ivec2 srcGl=ivec2(srcGs.x,511-srcGs.y);\n"
            "        float sw=(owner==0)?512.0:((owner==1||owner==3)?128.0:64.0);\n"
            "        return g309Filtered(owner,(vec2(srcGl)+f+vec2(0.5))/vec2(sw,512.0));\n"
            "    }\n"
            "    vec4 a = mix(g309Pixel(p), g309Pixel(p + ivec2(1,0)), f.x);\n"
            "    vec4 b = mix(g309Pixel(p + ivec2(0,1)), g309Pixel(p + ivec2(1,1)), f.x);\n"
            "    return mix(a, b, f.y);\n"
            "}\n"
            "ivec4 texel8(ivec2 p){\n"
            "    ivec2 n = textureSize(uTex, 0);\n"
            "    p.x = wrapCoord(p.x, n.x, uWrapU);\n"
            "    p.y = wrapCoord(p.y, n.y, uWrapV);\n"
            "    return clamp(ivec4(floor(texelFetch(uTex, p, 0) * 255.0 + 0.5)), 0, 255);\n"
            "}\n"
            "float g406RoundAxis(float value, int pixel, int origin, int flags){\n"
            "    bool legacy = (flags & 4) != 0;\n"
            "    bool halfBoundary = legacy || uLinear != 0;\n"
            "    float boundary = halfBoundary ? floor(value * 2.0 + 0.5) * 0.5\n"
            "                                  : floor(value + 0.5);\n"
            "    float threshold = legacy ? 0.25 : (1.0 / 64.0);\n"
            "    if (abs(value - boundary) > threshold) return value;\n"
            "    bool down = ((flags & 2) != 0) && pixel != origin;\n"
            "    bool up = ((flags & 1) != 0) || (((flags & 2) != 0) && pixel == origin);\n"
            "    float downNudge = legacy ? 0.25 : (1.0 / 16.0);\n"
            "    float upNudge = legacy ? 0.25 : 0.0;\n"
            "    return down ? boundary - downNudge : (up ? boundary + upNudge : value);\n"
            "}\n"
            "vec2 g406RoundUv(vec2 uvTex){\n"
            "    if ((uMode & 16384) == 0) return uvTex;\n"
            "    int px = int(floor(gl_FragCoord.x));\n"
            "    int py = int(floor(uFbSize.y - gl_FragCoord.y));\n"
            "    vec2 rounded = vec2(g406RoundAxis(uvTex.x, px, uUvOriginX, uUvRoundU),\n"
            "                        g406RoundAxis(uvTex.y, py, uUvOriginY, uUvRoundV));\n"
            "    return uLinear != 0 ? trunc(rounded * 16.0) * (1.0 / 16.0) : rounded;\n"
            "}\n"
            "ivec4 sampleExact(vec2 uvTex){\n"
            "    ivec2 f16 = clamp(ivec2(floor(uvTex * 16.0 + 0.5001)), ivec2(0), ivec2(65535));\n"
            "    if (uLinear == 0) return texel8(f16 / 16);\n"
            "    ivec2 shifted = f16 - ivec2(8);\n"
            "    ivec2 p0 = ivec2(floorDiv16(shifted.x), floorDiv16(shifted.y));\n"
            "    ivec2 fr = shifted - p0 * 16;\n"
            "    ivec4 c00 = texel8(p0), c10 = texel8(p0 + ivec2(1,0));\n"
            "    ivec4 c01 = texel8(p0 + ivec2(0,1)), c11 = texel8(p0 + ivec2(1,1));\n"
            "    int wx0 = 16 - fr.x, wy0 = 16 - fr.y;\n"
            "    ivec4 sum = c00 * (wx0 * wy0) + c10 * (fr.x * wy0) +\n"
            "                c01 * (wx0 * fr.y) + c11 * (fr.x * fr.y);\n"
            "    return clamp((sum + ivec4(128)) / 256, 0, 255);\n"
            "}\n"
            "ivec4 combineExact(ivec4 vc, ivec4 tc){\n"
            "    int tfx = (uMode >> 1) & 3; bool tcc = (uMode & 8) != 0;\n"
            "    ivec4 c = tc;\n"
            "    if (tfx == 0) {\n"
            "        c.rgb = (tc.rgb * vc.rgb) >> 7; c.a = tcc ? ((tc.a * vc.a) >> 7) : vc.a;\n"
            "    } else if (tfx == 1) { c.a = tcc ? tc.a : vc.a;\n"
            "    } else {\n"
            "        c.rgb = ((tc.rgb * vc.rgb) >> 7) + ivec3(vc.a);\n"
            "        c.a = (tfx == 2) ? (tcc ? tc.a + vc.a : vc.a) : (tcc ? tc.a : vc.a);\n"
            "    }\n"
            "    return clamp(c, 0, 255);\n"
            "}\n"
            // G364: identical integer form to the CPU rasterizer's g364FogLerp, so the GPU and the
            // DC2_G178_NO_GPU=1 oracle agree bit-for-bit. F=255 -> unfogged, F=0 -> pure fog colour.
            "ivec3 fogExact(ivec3 c){\n"
            "    if ((uMode & 8192) == 0) return c;\n"
            "    int f = clamp(int(floor(vFog * 255.0 + 0.5)), 0, 255);\n"
            "    ivec3 fc = ivec3(uFogCol & 255, (uFogCol >> 8) & 255, (uFogCol >> 16) & 255);\n"
            "    return (c * f + fc * (255 - f) + ivec3(127)) / 255;\n"
            "}\n"
            "void main(){\n"
            "    if ((uMode & 512) != 0) {\n"
            "        ivec4 vc = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255);\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        ivec4 tc = sampleExact(g406RoundUv(uv * vec2(textureSize(uTex, 0))));\n"
            "        if ((uMode & 1024) != 0) tc.a = 128;\n"
            "        ivec4 ec = combineExact(vc, tc);\n"
            "        if ((uMode & 64) != 0) {\n"
            "            int refCode = (uMode >> 7) & 3;\n"
            "            int refA = (refCode == 0) ? 1 : ((refCode == 1) ? 64 : 127);\n"
            "            if (ec.a < refA) discard;\n"
            "        }\n"
            "        ec.rgb = fogExact(ec.rgb);\n"
            "        o = vec4(ec) * (1.0 / 255.0);\n"
            "        float ez = max(vZ, 0.0);\n"
            "        if ((uMode & 16) != 0) ez = floor(min(ez, 16777215.0));\n"
            "        gl_FragDepth = ez * (1.0 / 4294967296.0);\n"
            "        return;\n"
            "    }\n"
            "    vec4 c = vColor;\n"
            "    bool alphaTest = (uMode & 64) != 0;\n"
            "    int alphaByte = 0;\n"
            "    if (alphaTest) alphaByte = int(floor(vColor.a * 255.0 + 0.5));\n"
            "    if ((uMode & 1) != 0) {\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        vec4 t;\n"
            "        if ((uMode & 2048) != 0) t = g309Linear(uv);\n"
            "        else if ((uMode & 4096) != 0)\n"
            "            t = vec4(sampleExact(g406RoundUv(uv * vec2(textureSize(uTex, 0))))) * (1.0 / 255.0);\n"
            "        else t = texture(uTex, uv);\n"
            "        if ((uMode & 1024) != 0) t.a = 128.0 / 255.0;\n"
            "        int tfx = (uMode >> 1) & 3;\n"
            "        bool tcc = (uMode & 8) != 0;\n"
            "        vec3 rgb; float a;\n"
            "        if (tfx == 0) {\n"
            "            rgb = min(t.rgb * c.rgb * 1.9921875, vec3(1.0));\n"
            "            a = tcc ? min(t.a * c.a * 1.9921875, 1.0) : c.a;\n"
            "        } else if (tfx == 1) {\n"
            "            rgb = t.rgb;\n"
            "            a = tcc ? t.a : c.a;\n"
            "        } else {\n"
            "            rgb = min(t.rgb * c.rgb * 1.9921875 + c.aaa, vec3(1.0));\n"
            "            a = (tfx == 2) ? (tcc ? min(t.a + c.a, 1.0) : c.a)\n"
            "                           : (tcc ? t.a : c.a);\n"
            "        }\n"
            "        if (alphaTest) {\n"
            "            int texA = int(floor(t.a * 255.0 + 0.5));\n"
            "            int vertA = alphaByte;\n"
            "            if (tfx == 0)\n"
            "                alphaByte = tcc ? min((texA * vertA) / 128, 255) : vertA;\n"
            "            else if (tfx == 1)\n"
            "                alphaByte = tcc ? texA : vertA;\n"
            "            else\n"
            "                alphaByte = (tfx == 2) ? (tcc ? min(texA + vertA, 255) : vertA)\n"
            "                                       : (tcc ? texA : vertA);\n"
            "        }\n"
            "        c = vec4(rgb, a);\n"
            "    }\n"
            "    if (alphaTest) {\n"
            "        int refCode = (uMode >> 7) & 3;\n"
            "        int refA = (refCode == 0) ? 1 : ((refCode == 1) ? 64 : 127);\n"
            "        if (alphaByte < refA) discard;\n"
            "    }\n"
            "    float outA = ((uMode & 32) != 0) ? c.a : min(c.a * 1.9921875, 1.0);\n"
            "    if ((uMode & 8192) != 0)\n"
            "        c.rgb = vec3(fogExact(clamp(ivec3(floor(c.rgb * 255.0 + 0.5)), 0, 255))) * (1.0 / 255.0);\n"
            "    o = vec4(c.rgb, outA);\n"
            "    float z = max(vZ, 0.0);\n"
            "    if ((uMode & 16) != 0) z = floor(min(z, 16777215.0));\n"
            "    gl_FragDepth = z * (1.0 / 4294967296.0);\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G178:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_prog, vs);
        m_f.glAttachShader_(m_prog, fs);
        m_f.glLinkProgram_(m_prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            std::fprintf(stderr, "[G178:backend] FAIL: program link\n");
            return false;
        }
        m_f.glUseProgram_(m_prog);
        m_locFbSize = m_f.glGetUniformLocation_(m_prog, "uFbSize");
        m_locMode = m_f.glGetUniformLocation_(m_prog, "uMode");
        m_locFogCol = m_f.glGetUniformLocation_(m_prog, "uFogCol"); // G364
        m_locLinear = m_f.glGetUniformLocation_(m_prog, "uLinear");
        m_locWrapU = m_f.glGetUniformLocation_(m_prog, "uWrapU");
        m_locWrapV = m_f.glGetUniformLocation_(m_prog, "uWrapV");
        m_locUvOriginX = m_f.glGetUniformLocation_(m_prog, "uUvOriginX");
        m_locUvOriginY = m_f.glGetUniformLocation_(m_prog, "uUvOriginY");
        m_locUvRoundU = m_f.glGetUniformLocation_(m_prog, "uUvRoundU");
        m_locUvRoundV = m_f.glGetUniformLocation_(m_prog, "uUvRoundV");
        const GLint locTex = m_f.glGetUniformLocation_(m_prog, "uTex");
        m_f.glUniform1i_(locTex, 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src1"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src2"), 2);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src3"), 3);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Src4"), 4);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309PageMap"), 5);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_prog, "uG309Base"), 6);
        return true;
    }

    bool setupT8ViewProgram()
    {
        // Full-screen triangle generated from gl_VertexID. The RGBA8 map texture carries the
        // front-end's exact AddressP8 -> CT32 inverse result, so this shader performs no guessed
        // PSM layout math: it only selects the mapped byte lane and applies the captured CLUT.
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform sampler2D uMap;\n"
            "uniform sampler2D uClut;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uMap,p,0)*255.0+0.5)),0,255);\n"
            "    ivec2 sp = ivec2(m.r | ((m.b & 8) << 5), m.g | ((m.b & 1) << 8));\n"
            "    int lane = (m.b >> 1) & 3;\n"
            "    ivec4 raw = clamp(ivec4(floor(texelFetch(uSrc,sp,0)*255.0+0.5)),0,255);\n"
            "    int ix = lane==0 ? raw.r : (lane==1 ? raw.g : (lane==2 ? raw.b : raw.a));\n"
            "    o = texelFetch(uClut, ivec2(ix,0), 0);\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G281:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G281:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g281Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g281Prog, vs);
        m_f.glAttachShader_(m_g281Prog, fs);
        m_f.glLinkProgram_(m_g281Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g281Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g281Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g281Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G281:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g281Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uSrc"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uMap"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g281Prog, "uClut"), 2);
        std::fprintf(stderr, "[G281:backend] PASS: resident PSMT8 view program ready\n");
        return true;
    }

    bool setupCt24ViewProgram()
    {
        // G338: resident CT24-alpha view. A CT24 texture whose base/width equal a resident CT32
        // FBO is that FBO's RGB with alpha re-derived per-texel from TEXA (a=(aem&&rgb==0)?0:ta0,
        // exactly GSMem applyTexa). This shader copies RGB straight from the producer FBO and
        // re-writes only alpha, producing a texture byte-identical to the legacy CT24->RGBA8
        // decode. The consumer then does its own GL_LINEAR, so filtering matches the legacy path
        // regardless of scale. Identity map: view texel (x,y)=GS pixel (x,y); the FBO stores GS
        // row y at GL row (uSrcH-1-y), so flip the sampled row here so the OUTPUT texture is
        // top-origin like any decoded texture (consumer samples it without the g261Bound flip).
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform int uSrcH;\n"
            "uniform int uTa0;\n"
            "uniform int uAem;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec2 sp = ivec2(p.x, (uSrcH - 1) - p.y);\n"
            "    ivec4 raw = clamp(ivec4(floor(texelFetch(uSrc, sp, 0) * 255.0 + 0.5)), 0, 255);\n"
            "    bool rgb0 = (raw.r == 0 && raw.g == 0 && raw.b == 0);\n"
            "    int a = (uAem != 0 && rgb0) ? 0 : uTa0;\n"
            "    o = vec4(vec3(raw.rgb) * (1.0 / 255.0), float(a) * (1.0 / 255.0));\n"
            "}\n";
        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G338:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G338:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g338Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g338Prog, vs);
        m_f.glAttachShader_(m_g338Prog, fs);
        m_f.glLinkProgram_(m_g338Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g338Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g338Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g338Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G338:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g338Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g338Prog, "uSrc"), 0);
        m_locG338SrcH = m_f.glGetUniformLocation_(m_g338Prog, "uSrcH");
        m_locG338Ta0 = m_f.glGetUniformLocation_(m_g338Prog, "uTa0");
        m_locG338Aem = m_f.glGetUniformLocation_(m_g338Prog, "uAem");
        std::fprintf(stderr, "[G338:backend] PASS: resident CT24-alpha view program ready\n");
        return true;
    }

    // G338: build/refresh the resident CT24-alpha view of a CT32 producer FBO. Runs on the backend
    // worker thread inside the GL context (same contract as prepareT8ViewOnWorker). Returns true
    // when m_texCache[texKey] holds the exact CT24->RGBA8 decode of srcFbp's current pixels.
    bool prepareCt24ViewOnWorker(uint64_t texKey, uint32_t srcFbp, int srcFbW, int texW, int texH,
                                 int ta0, int aem, bool verify, uint64_t &verifyBad)
    {
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G338:backend-fail] n=%llu stage=%s src=%03x fbw=%d tex=%dx%d\n",
                             static_cast<unsigned long long>(s_failN), stage, srcFbp, srcFbW,
                             texW, texH);
            return false;
        };
        verifyBad = 0u;
        if (m_g338Prog == 0u || texKey == 0u || texW <= 0 || texH <= 0)
            return fail("args");
        const auto is = m_fbos.find(srcFbp);
        if (is == m_fbos.end() || is->second.colorTex == 0u ||
            is->second.w != srcFbW || is->second.h != 512)
            return fail("source");
        const FboEntry &src = is->second;

        auto dropOutput = [&]() {
            const auto it = m_texCache.find(texKey);
            if (it != m_texCache.end())
            {
                if (it->second.id != 0u)
                    glDeleteTextures(1, &it->second.id);
                m_texBytes -= std::min(
                    m_texBytes, static_cast<size_t>(it->second.w) * it->second.h * 4u);
                m_texCache.erase(it);
            }
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.erase(texKey);
        };

        g494DrainErrors(kG494Ct24Drain);
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return fail("objects");

        TexEntry &out = m_texCache[texKey];
        // G492: the full-screen draw below rewrites every texel of this output, so reallocating its
        // storage each build is redundant. Reuse it only when THIS function allocated it at exactly
        // these dimensions (g492Ct24Alloc), which makes the skip bit-exact by construction.
        // See g492_overlap_ceiling.inc.
        const bool g492Reuse = out.id != 0u && out.g492Ct24Alloc != 0u &&
                               out.w == texW && out.h == texH && g492Ct24ReuseActive();
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u)
            {
                dropOutput();
                return fail("output");
            }
        }
        else if (!g492Reuse)
            m_texBytes -= static_cast<size_t>(out.w) * out.h * 4u;
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        if (!g492Reuse)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            // G496b: the ONE site outside the draw loop that parameterises an m_texCache entry.
            // Record what it set so the per-object sampler cache never goes stale.
            out.g496MinF = GL_NEAREST;
            out.g496MagF = GL_NEAREST;
            out.g496WrapS = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.g496WrapT = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.w = texW;
            out.h = texH;
            out.g492Ct24Alloc = 1u;
            m_texBytes += static_cast<size_t>(texW) * texH * 4u;
        }
        out.lastUse = m_batchCounter + 1u;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, out.id, 0);
        if (g494FboStatus(kG494Ct24Fbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            dropOutput();
            return fail("framebuffer");
        }
        glViewport(0, 0, texW, texH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g338Prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_f.glUniform1i_(m_locG338SrcH, src.h);
        m_f.glUniform1i_(m_locG338Ta0, ta0 & 0xFF);
        m_f.glUniform1i_(m_locG338Aem, aem != 0 ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494Ct24Err) != GL_NO_ERROR)
        {
            dropOutput();
            return fail("draw");
        }

        if (verify)
        {
            std::vector<uint32_t> srcPx(static_cast<size_t>(src.w) * src.h);
            std::vector<uint32_t> outPx(static_cast<size_t>(texW) * texH);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, srcPx.data());
            glBindTexture(GL_TEXTURE_2D, out.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outPx.data());
            const uint32_t ta = static_cast<uint32_t>(ta0 & 0xFF);
            for (int yo = 0; yo < texH; ++yo)
                for (int x = 0; x < texW; ++x)
                {
                    // out row yo (top-origin) == GS row yo == src GL row (src.h-1-yo).
                    const int sRow = src.h - 1 - yo;
                    if (x >= src.w || sRow < 0)
                    {
                        ++verifyBad;
                        continue;
                    }
                    const uint32_t raw = srcPx[static_cast<size_t>(sRow) * src.w + x];
                    const bool rgb0 = (raw & 0x00FFFFFFu) == 0u;
                    const uint32_t a = (aem != 0 && rgb0) ? 0u : ta;
                    const uint32_t expected = (raw & 0x00FFFFFFu) | (a << 24u);
                    if (outPx[static_cast<size_t>(yo) * texW + x] != expected)
                        ++verifyBad;
                }
            static uint64_t s_verifyN = 0u;
            ++s_verifyN;
            if (verifyBad != 0u || s_verifyN <= 8u || (s_verifyN % 256u) == 0u)
                std::fprintf(stderr, "[G338:vfy] n=%llu texels=%zu bad=%llu\n",
                             static_cast<unsigned long long>(s_verifyN), outPx.size(),
                             static_cast<unsigned long long>(verifyBad));
        }
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(texKey);
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool setupAuthoritativeCompositeProgram()
    {
        // Full-screen transient composite. gl_FragCoord is in GL bottom-first space; convert
        // it to the strip's GS top-first page/local coordinates, select that page's owner, then
        // map the identical CT32 within-page coordinate into the owner's own FRAME/FBW layout.
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc0;\n"
            "uniform sampler2D uSrc1;\n"
            "uniform sampler2D uSrc2;\n"
            "uniform sampler2D uSrc3;\n"
            "uniform sampler2D uSrc4;\n"
            "uniform sampler2D uPageMap;\n"
            "uniform sampler2D uBase;\n"
            "vec4 fetchOwner(int owner, ivec2 p){\n"
            "    if (owner == 0) return texelFetch(uSrc0, p, 0);\n"
            "    if (owner == 1) return texelFetch(uSrc1, p, 0);\n"
            "    if (owner == 2) return texelFetch(uSrc2, p, 0);\n"
            "    if (owner == 3) return texelFetch(uSrc3, p, 0);\n"
            "    return texelFetch(uSrc4, p, 0);\n"
            "}\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    int gsY = 511 - p.y;\n"
            "    ivec2 page = ivec2(p.x >> 6, gsY >> 5);\n"
            "    ivec2 local = ivec2(p.x & 63, gsY & 31);\n"
            "    ivec4 m = clamp(ivec4(floor(texelFetch(uPageMap,page,0)*255.0+0.5)),0,255);\n"
            "    if (m.r == 0) { o = texelFetch(uBase, p, 0); return; }\n"
            "    int owner = m.r - 1;\n"
            "    ivec2 srcGs = ivec2(m.g * 64 + local.x, m.b * 32 + local.y);\n"
            "    ivec2 srcGl = ivec2(srcGs.x, 511 - srcGs.y);\n"
            "    o = fetchOwner(owner, srcGl);\n"
            "}\n";
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G309:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G309:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_g309Prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_g309Prog, vs);
        m_f.glAttachShader_(m_g309Prog, fs);
        m_f.glLinkProgram_(m_g309Prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_g309Prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_g309Prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_g309Prog, static_cast<GLsizei>(buf.size()),
                                     nullptr, buf.data());
            std::fprintf(stderr, "[G309:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_g309Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc0"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc1"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc2"), 2);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc3"), 3);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uSrc4"), 4);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uPageMap"), 5);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g309Prog, "uBase"), 6);
        std::fprintf(stderr, "[G309:backend] PASS: authoritative-page composite program ready\n");
        return true;
    }

    bool setupExactProgram()
    {
        // G256 is sprite-only and color-only by contract.  The vertex path intentionally keeps
        // the already-proven G255 integer rectangle coverage; the fragment path below replaces
        // normalized sampling/combine/fixed-function blending with the CPU oracle's arithmetic.
        static const char *kVS =
            "#version 430 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aSTQ;\n"
            "layout(location=2) in vec4 aColor;\n"
            "uniform vec2 uFbSize;\n"
            "noperspective out vec3 vSTQ;\n"
            "noperspective out vec4 vColor;\n"
            "void main(){\n"
            "    vSTQ = aSTQ; vColor = aColor;\n"
            "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
            "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
            "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
            "}\n";
        static const char *kFS =
            "#version 430 core\n"
            "noperspective in vec3 vSTQ;\n"
            "noperspective in vec4 vColor;\n"
            "layout(location=0) out vec4 oCoverage;\n"
            "layout(rgba8, binding=1) uniform coherent image2D uFb;\n"
            "uniform sampler2D uTex;\n"
            "uniform int uMode;\n"
            "uniform int uLinear;\n"
            "uniform int uWrapU;\n"
            "uniform int uWrapV;\n"
            "int wrapCoord(int p, int n, int mode){\n"
            "    if (mode != 0) return clamp(p, 0, n - 1);\n"
            "    int r = p % n; return r < 0 ? r + n : r;\n"
            "}\n"
            "int floorDiv16(int v){ return v >= 0 ? v / 16 : -((-v + 15) / 16); }\n"
            "int floorDiv128(int v){ return v >= 0 ? v / 128 : -((-v + 127) / 128); }\n"
            "ivec4 texel8(ivec2 p){\n"
            "    ivec2 n = textureSize(uTex, 0);\n"
            "    p.x = wrapCoord(p.x, n.x, uWrapU);\n"
            "    p.y = wrapCoord(p.y, n.y, uWrapV);\n"
            "    return clamp(ivec4(floor(texelFetch(uTex, p, 0) * 255.0 + 0.5)), 0, 255);\n"
            "}\n"
            "ivec4 sampleExact(vec2 uvTex){\n"
            // CPU drawSprite reaches exact 12.4 half-steps and std::lround resolves them upward.
            // GPU noperspective interpolation can land a few ulps below the same mathematical
            // tie (G258: 409.5 -> 409), so stabilize only that quantization boundary.
            "    ivec2 f16 = clamp(ivec2(floor(uvTex * 16.0 + 0.5001)), ivec2(0), ivec2(65535));\n"
            "    if (uLinear == 0) return texel8(f16 / 16);\n"
            "    ivec2 shifted = f16 - ivec2(8);\n"
            "    ivec2 p0 = ivec2(floorDiv16(shifted.x), floorDiv16(shifted.y));\n"
            "    ivec2 fr = shifted - p0 * 16;\n"
            "    ivec4 c00 = texel8(p0);\n"
            "    ivec4 c10 = texel8(p0 + ivec2(1,0));\n"
            "    ivec4 c01 = texel8(p0 + ivec2(0,1));\n"
            "    ivec4 c11 = texel8(p0 + ivec2(1,1));\n"
            "    int wx0 = 16 - fr.x, wy0 = 16 - fr.y;\n"
            "    ivec4 sum = c00 * (wx0 * wy0) + c10 * (fr.x * wy0) +\n"
            "                c01 * (wx0 * fr.y) + c11 * (fr.x * fr.y);\n"
            "    return clamp((sum + ivec4(128)) / 256, 0, 255);\n"
            "}\n"
            "ivec4 combineExact(ivec4 vc, ivec4 tc){\n"
            "    int tfx = (uMode >> 1) & 3; bool tcc = (uMode & 8) != 0;\n"
            "    ivec4 c = tc;\n"
            "    if (tfx == 0) {\n"
            "        c.rgb = (tc.rgb * vc.rgb) >> 7; c.a = tcc ? ((tc.a * vc.a) >> 7) : vc.a;\n"
            "    } else if (tfx == 1) { c.a = tcc ? tc.a : vc.a;\n"
            "    } else {\n"
            "        c.rgb = ((tc.rgb * vc.rgb) >> 7) + ivec3(vc.a);\n"
            "        c.a = (tfx == 2) ? (tcc ? tc.a + vc.a : vc.a) : (tcc ? tc.a : vc.a);\n"
            "    }\n"
            "    return clamp(c, 0, 255);\n"
            "}\n"
            "int blendStd(int s, int d, int a){ return clamp(floorDiv128((s-d)*a) + d, 0, 255); }\n"
            "int blendAdd(int s, int d, int a){ return clamp((s*a)/128 + d, 0, 255); }\n"
            "void main(){\n"
            "    ivec4 src = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255);\n"
            "    if ((uMode & 1) != 0) {\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        src = combineExact(src, sampleExact(uv * vec2(textureSize(uTex, 0))));\n"
            "    }\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec4 dst = clamp(ivec4(floor(imageLoad(uFb, p) * 255.0 + 0.5)), 0, 255);\n"
            "    int blend = (uMode >> 8) & 3; ivec4 outc = src;\n"
            "    bool opaque139 = (uMode & 1024) != 0 && (dst.r | dst.g | dst.b) == 0;\n"
            "    if (!opaque139 && blend == 1) {\n"
            "        outc.r = blendStd(src.r,dst.r,src.a); outc.g = blendStd(src.g,dst.g,src.a);\n"
            "        outc.b = blendStd(src.b,dst.b,src.a);\n"
            "    } else if (!opaque139 && blend == 2) {\n"
            "        outc.r = blendAdd(src.r,dst.r,src.a); outc.g = blendAdd(src.g,dst.g,src.a);\n"
            "        outc.b = blendAdd(src.b,dst.b,src.a);\n"
            "    }\n"
            "    outc.a = src.a;\n"
            "    imageStore(uFb, p, vec4(outc) * (1.0 / 255.0));\n"
            "    oCoverage = vec4(1.0);\n"
            "}\n";

        GLuint vs = 0, fs = 0;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G256:backend] FAIL: VS compile: %s\n", log.c_str());
            return false;
        }
        if (!g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G256:backend] FAIL: FS compile: %s\n", log.c_str());
            m_f.glDeleteShader_(vs);
            return false;
        }
        m_progExact = m_f.glCreateProgram_();
        m_f.glAttachShader_(m_progExact, vs);
        m_f.glAttachShader_(m_progExact, fs);
        m_f.glLinkProgram_(m_progExact);
        GLint linked = 0;
        m_f.glGetProgramiv_(m_progExact, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(m_progExact, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(m_progExact, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
            std::fprintf(stderr, "[G256:backend] FAIL: program link: %s\n", buf.data());
            return false;
        }
        m_f.glUseProgram_(m_progExact);
        m_locExactFbSize = m_f.glGetUniformLocation_(m_progExact, "uFbSize");
        m_locExactMode = m_f.glGetUniformLocation_(m_progExact, "uMode");
        m_locExactLinear = m_f.glGetUniformLocation_(m_progExact, "uLinear");
        m_locExactWrapU = m_f.glGetUniformLocation_(m_progExact, "uWrapU");
        m_locExactWrapV = m_f.glGetUniformLocation_(m_progExact, "uWrapV");
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_progExact, "uTex"), 0);
        std::fprintf(stderr, "[G256:backend] PASS: exact RTT fragment program ready\n");
        return true;
    }

    bool setupBuffers()
    {
        m_f.glGenVertexArrays_(1, &m_vao);
        m_f.glBindVertexArray_(m_vao);
        m_f.glGenBuffers_(1, &m_vbo);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        static_assert(sizeof(G178Vtx) == 32,
                      "G178Vtx must be tightly packed (6 floats + RGBA + G364 fog/pad)");
        m_f.glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(0));
        m_f.glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(12));
        m_f.glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(24));
        // G364: per-vertex fog byte (normalized; .x carries F, .yzw are the struct padding).
        m_f.glVertexAttribPointer_(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(28));
        m_f.glEnableVertexAttribArray_(0);
        m_f.glEnableVertexAttribArray_(1);
        m_f.glEnableVertexAttribArray_(2);
        m_f.glEnableVertexAttribArray_(3);
        return true;
    }

    bool runG257ImageStoreDiagnostic()
    {
        constexpr int kW = 4;
        constexpr int kH = 4;
        static const char *kVS =
            "#version 430 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "void main(){ gl_Position = vec4(aPos.xy, 0.0, 1.0); }\n";
        static const char *kFS =
            "#version 430 core\n"
            "layout(location=0) out vec4 oCoverage;\n"
            "layout(rgba8, binding=0) uniform writeonly image2D uImage;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    imageStore(uImage, p, vec4(float(p.x + 1), float(p.y + 17), 90.0, 195.0) / 255.0);\n"
            "    oCoverage = vec4(1.0, 0.0, 1.0, 1.0);\n"
            "}\n";

        auto drainErrors = []() {
            unsigned count = 0;
            GLenum first = GL_NO_ERROR;
            for (GLenum e = glGetError(); e != GL_NO_ERROR && count < 32; e = glGetError())
            {
                if (count == 0)
                    first = e;
                ++count;
            }
            return std::pair<GLenum, unsigned>{first, count};
        };
        const auto staleErrors = drainErrors();
        const char *glVersion = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        const char *glslVersion = reinterpret_cast<const char *>(glGetString(0x8B8C));
        GLint imageLoad = 0;
        GLint imageStore = 0;
        m_e.glGetInternalformativ_(GL_TEXTURE_2D, GL158_RGBA8, GL257_SHADER_IMAGE_LOAD, 1,
                                   &imageLoad);
        m_e.glGetInternalformativ_(GL_TEXTURE_2D, GL158_RGBA8, GL257_SHADER_IMAGE_STORE, 1,
                                   &imageStore);
        const auto capabilityErrors = drainErrors();
        std::fprintf(stderr,
                     "[G257:image] caps gl=%s glsl=%s rgba8Load=%d rgba8Store=%d staleErr=0x%x/%u capErr=0x%x/%u\n",
                     glVersion ? glVersion : "?", glslVersion ? glslVersion : "?",
                     imageLoad, imageStore, staleErrors.first, staleErrors.second,
                     capabilityErrors.first, capabilityErrors.second);

        GLuint vs = 0;
        GLuint fs = 0;
        GLuint prog = 0;
        GLuint imageTex = 0;
        GLuint coverageTex = 0;
        GLuint coverageFbo = 0;
        auto cleanup = [&]() {
            m_e.glBindImageTexture_(0, 0, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
            m_f.glUseProgram_(m_prog);
            m_f.glBindVertexArray_(m_vao);
            m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            if (coverageFbo != 0)
                m_f.glDeleteFramebuffers_(1, &coverageFbo);
            if (coverageTex != 0)
                glDeleteTextures(1, &coverageTex);
            if (imageTex != 0)
                glDeleteTextures(1, &imageTex);
            if (prog != 0)
                m_f.glDeleteProgram_(prog);
            if (vs != 0)
                m_f.glDeleteShader_(vs);
            if (fs != 0)
                m_f.glDeleteShader_(fs);
        };

        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G257:image] FAIL: shader compile: %s\n", log.c_str());
            cleanup();
            return false;
        }
        prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(prog, vs);
        m_f.glAttachShader_(prog, fs);
        m_f.glLinkProgram_(prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(prog, GL158_LINK_STATUS, &linked);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> linkLog(len > 0 ? static_cast<size_t>(len) : 1u);
            m_f.glGetProgramInfoLog_(prog, static_cast<GLsizei>(linkLog.size()), nullptr,
                                     linkLog.data());
            std::fprintf(stderr, "[G257:image] FAIL: program link: %s\n", linkLog.data());
            cleanup();
            return false;
        }

        std::vector<uint8_t> sentinel(static_cast<size_t>(kW * kH * 4));
        for (size_t i = 0; i < sentinel.size(); i += 4)
        {
            sentinel[i + 0] = 7;
            sentinel[i + 1] = 11;
            sentinel[i + 2] = 13;
            sentinel[i + 3] = 17;
        }
        glGenTextures(1, &imageTex);
        glBindTexture(GL_TEXTURE_2D, imageTex);
        // The direct timing path can bilinearly sample a clean guest-VRAM page. The verifier uses
        // texelFetch, so linear filtering preserves both contracts.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     sentinel.data());

        glGenTextures(1, &coverageTex);
        glBindTexture(GL_TEXTURE_2D, coverageTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        m_f.glGenFramebuffers_(1, &coverageFbo);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverageFbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    coverageTex, 0);
        const GLenum fboStatus = m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
        if (fboStatus != GL158_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[G257:image] FAIL: scratch FBO incomplete 0x%x\n", fboStatus);
            cleanup();
            return false;
        }

        std::vector<uint8_t> before(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, before.data());
        const bool sentinelOk = before == sentinel;
        const auto setupErrors = drainErrors();

        const G178Vtx tri[3] = {
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            { 3.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f,  3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
        };
        m_f.glUseProgram_(prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        m_f.glBufferData_(GL158_ARRAY_BUFFER, sizeof(tri), tri, GL178_STREAM_DRAW);
        m_e.glBindImageTexture_(0, imageTex, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
        glViewport(0, 0, kW, kH);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const auto drawErrors = drainErrors();

        std::vector<uint8_t> beforeBarrier(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, beforeBarrier.data());
        const bool preBarrierChanged = beforeBarrier != sentinel;
        const auto preBarrierErrors = drainErrors();

        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT |
                             GL178_FRAMEBUFFER_BARRIER_BIT);
        const auto barrierErrors = drainErrors();
        std::vector<uint8_t> afterBarrier(sentinel.size());
        glBindTexture(GL_TEXTURE_2D, imageTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, afterBarrier.data());
        std::vector<uint8_t> coverage(sentinel.size());
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverageFbo);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, coverage.data());
        const auto readErrors = drainErrors();

        size_t patternPixels = 0;
        size_t coveragePixels = 0;
        for (int y = 0; y < kH; ++y)
        {
            for (int x = 0; x < kW; ++x)
            {
                const size_t i = static_cast<size_t>((y * kW + x) * 4);
                if (afterBarrier[i + 0] == static_cast<uint8_t>(x + 1) &&
                    afterBarrier[i + 1] == static_cast<uint8_t>(y + 17) &&
                    afterBarrier[i + 2] == 90 && afterBarrier[i + 3] == 195)
                    ++patternPixels;
                if (coverage[i + 0] == 255 && coverage[i + 1] == 0 &&
                    coverage[i + 2] == 255 && coverage[i + 3] == 255)
                    ++coveragePixels;
            }
        }
        const bool pass = imageLoad != 0 && imageStore != 0 && sentinelOk &&
                          setupErrors.second == 0 && drawErrors.second == 0 &&
                          preBarrierErrors.second == 0 && barrierErrors.second == 0 &&
                          readErrors.second == 0 && patternPixels == kW * kH &&
                          coveragePixels == kW * kH;
        std::fprintf(stderr,
                     "[G257:image] %s sentinel=%d preBarrierChanged=%d pattern=%zu/%d coverage=%zu/%d "
                     "errSetup=0x%x/%u errDraw=0x%x/%u errPre=0x%x/%u errBarrier=0x%x/%u errRead=0x%x/%u\n",
                     pass ? "PASS" : "FAIL", sentinelOk ? 1 : 0, preBarrierChanged ? 1 : 0,
                     patternPixels, kW * kH, coveragePixels, kW * kH,
                     setupErrors.first, setupErrors.second, drawErrors.first, drawErrors.second,
                     preBarrierErrors.first, preBarrierErrors.second,
                     barrierErrors.first, barrierErrors.second, readErrors.first, readErrors.second);
        cleanup();
        return pass;
    }

    // G415: destroy only the ACTIVE geometry, leaving any parked spare intact. The create path
    // below uses this so parking survives the immediately following fresh allocation.
    void destroyActiveFbo(FboEntry &e)
    {
        if (e.fbo != 0)
            m_f.glDeleteFramebuffers_(1, &e.fbo);
        if (e.colorTex != 0)
            glDeleteTextures(1, &e.colorTex);
        if (e.depthRb != 0)
            m_e.glDeleteRenderbuffers_(1, &e.depthRb);
        e.fbo = 0;
        e.colorTex = 0;
        e.depthRb = 0;
        e.w = 0;
        e.h = 0;
    }

    void destroySpareFbo(FboEntry &e)
    {
        if (e.spareFbo != 0)
            m_f.glDeleteFramebuffers_(1, &e.spareFbo);
        if (e.spareColorTex != 0)
            glDeleteTextures(1, &e.spareColorTex);
        if (e.spareDepthRb != 0)
            m_e.glDeleteRenderbuffers_(1, &e.spareDepthRb);
        e.spareFbo = 0;
        e.spareColorTex = 0;
        e.spareDepthRb = 0;
        e.spareW = 0;
        e.spareH = 0;
    }

    void destroyFbo(FboEntry &e)
    {
        destroyActiveFbo(e);
        destroySpareFbo(e);
        e = FboEntry{};
    }

    FboEntry &ensureFbo(uint32_t fbp, int w, int h)
    {
        // G349 de-collide: the narrow (w!=512) 0x139 population gets a private sibling FBO key so it
        // stops destroying the persistent 512-wide display FBO (default-off; identity otherwise).
        const uint32_t fboKey = g349FboKey(fbp, w);
        FboEntry &e = m_fbos[fboKey];
        // G349 (behavior-pure, DC2_G349_FBOLIFE=1): the G348 Stale-When prerequisite for the
        // closure-core build — is a family target's persistent FBO DESTROYED between flushes, or
        // merely not-yet-bound? m_fbos is keyed by fbp ALONE, and destroyFbo()+recreate fires on any
        // (w,h) change (the new FBO is cleared, line below). Hypothesis: 0x139 is written at both
        // fbw=8 (512-wide display strip) and fbw=2 (128-wide narrow population, G347 census w2=22%),
        // so the two widths MUTUALLY destroy each other's FBO every frame → the snapInv 100%
        // FBO-read-fail. Trace confirms/refutes before any persistence is built.
        static const bool s_g349FboLife = (std::getenv("DC2_G349_FBOLIFE") != nullptr);
        if (s_g349FboLife &&
            (fbp == 0x139u || fbp == 0x13bu || fbp == 0x143u ||
             fbp == 0x146u || fbp == 0x14au || fbp == 0x155u))
        {
            static const uint32_t s_fam[6] = {0x139u, 0x13bu, 0x143u, 0x146u, 0x14au, 0x155u};
            static std::atomic<uint64_t> s_ev[6][3] = {}; // [fi][0=reuse 1=recreate 2=fresh-create]
            static std::atomic<uint32_t> s_recPrints{0};
            static std::atomic<uint64_t> s_agg{0};
            const bool reuse = (e.fbo != 0 && e.w == w && e.h == h);
            const int cls = reuse ? 0 : (e.fbo != 0 ? 1 : 2);
            int fi = 0;
            for (; fi < 6; ++fi)
                if (s_fam[fi] == fbp)
                    break;
            const uint64_t n = s_ev[fi][cls].fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (cls == 1 && s_recPrints.fetch_add(1u, std::memory_order_relaxed) < 40u)
                std::fprintf(stderr, "[G349:fbolife] RECREATE fbp=0x%x (%dx%d)->(%dx%d) n=%llu\n",
                             fbp, e.w, e.h, w, h, (unsigned long long)n);
            if ((s_agg.fetch_add(1u, std::memory_order_relaxed) % 512u) == 0u)
            {
                char b[288];
                int o = 0;
                b[0] = '\0';
                for (int i = 0; i < 6; ++i)
                {
                    const uint64_t ru = s_ev[i][0].load(std::memory_order_relaxed);
                    const uint64_t rc = s_ev[i][1].load(std::memory_order_relaxed);
                    const uint64_t cr = s_ev[i][2].load(std::memory_order_relaxed);
                    if ((ru | rc | cr) != 0u && o < 236)
                        o += std::snprintf(b + o, sizeof(b) - o,
                                           " %x:re=%llu rec=%llu new=%llu", s_fam[i],
                                           (unsigned long long)ru, (unsigned long long)rc,
                                           (unsigned long long)cr);
                }
                std::fprintf(stderr, "[G349:fbolife-agg]%s\n", b);
            }
        }
        // G356: display-FBO (0x0/0x68) reuse-vs-recreate census. A recreate (destroy+clear below)
        // would wipe the persistent field the Sindain menu circle depends on.
        {
            static const bool s_g356Tr = (std::getenv("DC2_G356_TRACE") != nullptr);
            if (s_g356Tr && (fbp == 0x0u || fbp == 0x68u))
            {
                const bool reuse = (e.fbo != 0 && e.w == w && e.h == h);
                static std::atomic<uint32_t> s_g356F{0};
                const uint32_t n = s_g356F.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (!reuse || n <= 8u || (n % 200u) == 0u)
                    std::fprintf(stderr,
                        "[G356:fbo] n=%u fbp=0x%x %s old(%dx%d fbo=%u) new(%dx%d)\n",
                        n, fbp, reuse ? "REUSE" : "RECREATE", e.w, e.h, e.fbo, w, h);
            }
        }
        if (e.fbo != 0 && e.w == w && e.h == h)
            return e;
        // G415: recycle the parked geometry for this key instead of deleting and reallocating a
        // colour texture + depth renderbuffer on every width flip. The revived FBO is put into the
        // exact post-state a fresh create leaves behind: attachments re-bound, completeness
        // re-checked, creation-time texture parameters restored, colour and depth cleared with the
        // same current clear state. Fail-closed: anything unexpected drops both geometries and
        // takes the original create path.
        if (g415FboRecycleOn())
        {
            if (e.spareFbo != 0 && e.spareColorTex != 0 && e.spareDepthRb != 0 &&
                e.spareW == w && e.spareH == h)
            {
                std::swap(e.fbo, e.spareFbo);
                std::swap(e.colorTex, e.spareColorTex);
                std::swap(e.depthRb, e.spareDepthRb);
                std::swap(e.w, e.spareW);
                std::swap(e.h, e.spareH);
                glBindTexture(GL_TEXTURE_2D, e.colorTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                // A fresh GL texture starts at the GL_REPEAT default; sampling sites may have left
                // CLAMP_TO_EDGE on the parked one.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, e.fbo);
                m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, e.colorTex, 0);
                m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                               GL178_RENDERBUFFER, e.depthRb);
                // G494: the REVIVE path re-attaches the same colour texture and the same depth
                // renderbuffer this FBO was created and validated with, so its completeness is
                // invariant. The CREATE path's check below is deliberately NOT elided.
                if (g494FboStatus(kG494EnsureFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                        return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                    }) == GL158_FRAMEBUFFER_COMPLETE)
                {
                    glDisable(GL_SCISSOR_TEST);
                    glDepthMask(GL_TRUE);
                    glClearDepth(0.0);
                    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
                    ++m_g415FboRevives;
                    return e;
                }
                destroyFbo(e);
            }
            else if (e.fbo != 0)
            {
                destroySpareFbo(e);
                e.spareFbo = e.fbo;
                e.spareColorTex = e.colorTex;
                e.spareDepthRb = e.depthRb;
                e.spareW = e.w;
                e.spareH = e.h;
                e.fbo = 0;
                e.colorTex = 0;
                e.depthRb = 0;
                e.w = 0;
                e.h = 0;
                ++m_g415FboParks;
            }
        }
        ++m_g415FboCreates;
        destroyActiveFbo(e);
        glGenTextures(1, &e.colorTex);
        glBindTexture(GL_TEXTURE_2D, e.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_e.glGenRenderbuffers_(1, &e.depthRb);
        m_e.glBindRenderbuffer_(GL178_RENDERBUFFER, e.depthRb);
        m_e.glRenderbufferStorage_(GL178_RENDERBUFFER, GL178_DEPTH_COMPONENT32F, w, h);
        m_f.glGenFramebuffers_(1, &e.fbo);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, e.fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    e.colorTex, 0);
        m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                       GL178_RENDERBUFFER, e.depthRb);
        const GLenum status = m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
        if (status != GL158_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[G178:backend] FAIL: FBO incomplete 0x%x (fbp=0x%x %dx%d)\n",
                         status, fbp, w, h);
            destroyFbo(e);
            return e;
        }
        e.w = w;
        e.h = h;
        // New FBO contents are undefined — force a depth clear; color comes from uploadFb (the
        // front-end always uploads on its first batch for an fbp, via the gen-snapshot mismatch).
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(0.0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        return e;
    }

    DepthEntry &ensureDepth(uint64_t key, int w, int h)
    {
        DepthEntry &e = m_depths[key];
        if (e.tex != 0 && e.w == w && e.h == h)
            return e;
        GLuint newTex = 0;
        glGenTextures(1, &newTex);
        if (newTex == 0)
            return e;
        glBindTexture(GL_TEXTURE_2D, newTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL158_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL158_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL178_DEPTH_COMPONENT32F,
                     w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        if (e.tex != 0)
            glDeleteTextures(1, &e.tex);
        e.tex = newTex;
        e.w = w;
        e.h = h;
        e.initialized = false;
        return e;
    }

    // G261: read a row window of a target's persistent FBO color attachment. The FBO must already
    // exist with matching dimensions (a wave rendered into it); anything else is a caller bug and
    // fails loudly so the front-end can drop residency instead of consuming stale VRAM silently.
    bool readColorOnWorker(uint32_t fbp, int w, int h, int glY, int rows,
                           std::vector<uint32_t> &out)
    {
        if (w <= 0 || h <= 0 || rows <= 0 || glY < 0 || glY > h || rows > h - glY)
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, w)); // G349 de-collide (narrow 0x139 → sibling)
        if (it == m_fbos.end() || it->second.fbo == 0 || it->second.w != w || it->second.h != h)
            return false;
        out.resize(static_cast<size_t>(w) * rows);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, it->second.fbo);
        glDisable(GL_SCISSOR_TEST);
        // ===== G433 ceiling probe (default OFF) =====================================================
        // DC2_G433_NO_RB=1 skips ONLY the glReadPixels call. Everything else on both arms is
        // identical: the front thread still queues the job, still blocks on the promise, still
        // resizes the buffer and still runs the full g418 unpack over the same pixel count. So the
        // arms are workload-invariant by construction (same rows, same Mpx — asserted by the
        // counters below), and the frame delta is the exact price of the blocking readback,
        // i.e. the ceiling of any asynchronous-publication scheme. Output is KNOWINGLY WRONG on the
        // probe arm (stale/garbage rows are published into guest VRAM) — measurement only.
        static const bool s_g433NoRb = (std::getenv("DC2_G433_NO_RB") != nullptr);
        static const bool s_g433Stat = (std::getenv("DC2_G433_RBSTAT") != nullptr);
        const auto t0 = s_g433Stat ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        // ===== G495 drain/transfer split (default OFF, DC2_G495_RB=1) ================================
        // [G418:edge] instruments only the ONE render-batch readback; these SIX readColor items have
        // never been split. glFinish changes no result, no GL state and no command order, so the
        // split is exact — it only moves wall time out of the transfer half and names it.
        const bool g495On = g495RbCensusOn();
        uint64_t g495DrainNs = 0ull, g495XferNs = 0ull;
        std::chrono::steady_clock::time_point g495T1;
        if (g495On)
        {
            const auto g495T0 = std::chrono::steady_clock::now();
            glFinish();
            g495T1 = std::chrono::steady_clock::now();
            g495DrainNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g495T1 - g495T0).count());
        }
        if (!s_g433NoRb)
        {
            g433DoReadPixels(m_f, glY, w, rows, out.data());
        }
        if (g495On)
        {
            g495XferNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g495T1).count());
            g495NoteRead(fbp, glY, rows, w * rows, g495DrainNs, g495XferNs);
        }
        if (s_g433Stat)
        {
            static uint64_t s_n = 0ull, s_px = 0ull, s_ns = 0ull;
            s_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            ++s_n;
            s_px += static_cast<uint64_t>(w) * static_cast<uint64_t>(rows);
            if ((s_n % 512u) == 0u)
                std::fprintf(stderr,
                             "[G433:rb] norb=%d pbo=%d n=%llu ms=%.1f Mpx=%.2f usPerRead=%.1f "
                             "nsPerPx=%.2f\n",
                             s_g433NoRb ? 1 : 0, g433PboOn() ? 1 : 0,
                             static_cast<unsigned long long>(s_n),
                             (double)s_ns / 1.0e6, (double)s_px / 1.0e6,
                             (double)s_ns / 1.0e3 / (double)s_n,
                             s_px ? (double)s_ns / (double)s_px : 0.0);
        }
        return true;
    }

    // G264: write a row window of RGBA8 pixels into a target's persistent FBO color texture
    // (glTexSubImage2D — no draw, no readback). Same existence/dimension contract as
    // readColorOnWorker; the front-end owns row flipping and byte layout (VRAM CT32 == RGBA8).
    bool writeColorOnWorker(uint32_t fbp, int texW, int texH,
                            int glX, int glY, int w, int rows,
                            const std::vector<uint32_t> &in)
    {
        if (texW <= 0 || texH <= 0 || glX < 0 || glY < 0 || w <= 0 || rows <= 0 ||
            glX > texW || w > texW - glX || glY > texH || rows > texH - glY ||
            in.size() != static_cast<size_t>(w) * rows)
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, texW)); // G349 de-collide (narrow 0x139 → sibling)
        if (it == m_fbos.end() || it->second.colorTex == 0 || it->second.w != texW ||
            it->second.h != texH)
            return false;
        glBindTexture(GL_TEXTURE_2D, it->second.colorTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, glX, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                        in.data());
        return g494GetError(kG494WColErr) == GL_NO_ERROR;
    }

    // G280: copy aligned CT32 page tiles from one persistent target FBO into another target's
    // FBO color texture. glCopyTexSubImage2D is a GL 1.1 core entry point (statically linked,
    // no loader) that reads the bound READ framebuffer; binding GL_FRAMEBUFFER sets both READ
    // and DRAW bindings on 3.x core. RGBA8 -> RGBA8 same-size rect copy is bit-exact. Fails
    // closed (no partial validation): every rect is bounds-checked against BOTH live FBOs
    // before the first copy so a rejected job leaves the destination untouched.
    bool copyColorRectsOnWorker(uint32_t srcFbp, uint32_t dstFbp,
                                const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || rects6.empty() || (rects6.size() % 6u) != 0u)
            return false;
        auto is = m_fbos.find(srcFbp);
        auto id = m_fbos.find(dstFbp);
        if (is == m_fbos.end() || is->second.fbo == 0 ||
            id == m_fbos.end() || id->second.colorTex == 0)
            return false;
        const FboEntry &src = is->second;
        const FboEntry &dst = id->second;
        for (size_t k = 0; k < rects6.size(); k += 6u)
        {
            const int32_t sx = rects6[k], sy = rects6[k + 1];
            const int32_t dx = rects6[k + 2], dy = rects6[k + 3];
            const int32_t w = rects6[k + 4], h = rects6[k + 5];
            if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
                sx > src.w - w || sy > src.h - h || dx > dst.w - w || dy > dst.h - h)
                return false;
        }
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, src.fbo);
        glBindTexture(GL_TEXTURE_2D, dst.colorTex);
        for (size_t k = 0; k < rects6.size(); k += 6u)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, rects6[k + 2], rects6[k + 3],
                                rects6[k], rects6[k + 1], rects6[k + 4], rects6[k + 5]);
        return g494GetError(kG494CopyErr) == GL_NO_ERROR;
    }

    bool buildAuthoritativeCompositeOnWorker(
        const std::vector<uint32_t> &pageMap,
        const std::vector<uint32_t> &basePixels,
        std::vector<uint32_t> *readback,
        bool forceDraw = false,
        uint32_t outputFbp = kG309CompositeFbp)
    {
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G309:backend-fail] n=%llu stage=%s\n",
                             static_cast<unsigned long long>(s_failN), stage);
            return false;
        };
        if (m_g309Prog == 0u || pageMap.size() != 128u ||
            basePixels.size() != 512u * 512u)
            return fail("args");

        static constexpr uint32_t kSources[5] =
            {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
        GLuint sourceTex[5] = {};
        int sourceW[5] = {};
        for (uint32_t t = 0u; t < 5u; ++t)
        {
            const auto it = m_fbos.find(kSources[t]);
            if (it == m_fbos.end() || it->second.colorTex == 0u ||
                it->second.w <= 0 || it->second.h != 512)
                return fail("source");
            sourceTex[t] = it->second.colorTex;
            sourceW[t] = it->second.w;
            m_f.glActiveTexture_(GL158_TEXTURE0 + t);
            glBindTexture(GL_TEXTURE_2D, sourceTex[t]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        for (const uint32_t m : pageMap)
        {
            const uint32_t owner1 = m & 0xFFu;
            const uint32_t srcCol = (m >> 8u) & 0xFFu;
            const uint32_t srcRow = (m >> 16u) & 0xFFu;
            if (owner1 == 0u)
                continue;
            if (owner1 > 5u || srcRow >= 16u ||
                srcCol * 64u + 64u > static_cast<uint32_t>(sourceW[owner1 - 1u]))
                return fail("map");
        }

        g494DrainErrors(kG494CompDrain);
        if (m_g309MapTex == 0u)
            glGenTextures(1, &m_g309MapTex);
        if (m_g309MapTex == 0u)
            return fail("map-object");
        m_f.glActiveTexture_(GL158_TEXTURE0 + 5u);
        glBindTexture(GL_TEXTURE_2D, m_g309MapTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 8, 16, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pageMap.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        const bool allocateBase = m_g309BaseTex == 0u;
        if (allocateBase)
            glGenTextures(1, &m_g309BaseTex);
        if (m_g309BaseTex == 0u)
            return fail("base-object");
        m_f.glActiveTexture_(GL158_TEXTURE0 + 6u);
        glBindTexture(GL_TEXTURE_2D, m_g309BaseTex);
        if (allocateBase)
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 512, 512, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        // Upload only contiguous clean-page runs. GL_UNPACK_ROW_LENGTH keeps the front-end's
        // 512-pixel pitch, avoiding both a 1 MiB upload and a repack allocation.
        static constexpr GLenum kG309UnpackRowLength = 0x0CF2u;
        glPixelStorei(kG309UnpackRowLength, 512);
        for (uint32_t yp = 0u; yp < 16u; ++yp)
        {
            uint32_t xp = 0u;
            while (xp < 8u)
            {
                while (xp < 8u &&
                       (pageMap[static_cast<size_t>(yp) * 8u + xp] & 0xFFu) != 0u)
                    ++xp;
                const uint32_t runLo = xp;
                while (xp < 8u &&
                       (pageMap[static_cast<size_t>(yp) * 8u + xp] & 0xFFu) == 0u)
                    ++xp;
                if (xp == runLo)
                    continue;
                const uint32_t glY = 512u - (yp + 1u) * 32u;
                const uint32_t x = runLo * 64u;
                glTexSubImage2D(
                    GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(glY),
                    static_cast<GLsizei>((xp - runLo) * 64u), 32,
                    GL_RGBA, GL_UNSIGNED_BYTE,
                    basePixels.data() + static_cast<size_t>(glY) * 512u + x);
            }
        }
        glPixelStorei(kG309UnpackRowLength, 0);

        FboEntry &out = ensureFbo(outputFbp, 512, 512);
        if (out.fbo == 0u || out.colorTex == 0u)
            return fail("output");
        if (readback == nullptr && !forceDraw)
        {
            // Timing path samples the prepared sources/map directly in the display shader.
            // Keep the transient FBO only as the verifier's exact reference surface.
            m_f.glActiveTexture_(GL158_TEXTURE0);
            return g494GetError(kG494CompErr) == GL_NO_ERROR;
        }
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, out.fbo);
        glViewport(0, 0, 512, 512);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g309Prog);
        m_f.glBindVertexArray_(m_vao);
        for (uint32_t t = 0u; t < 5u; ++t)
        {
            m_f.glActiveTexture_(GL158_TEXTURE0 + t);
            glBindTexture(GL_TEXTURE_2D, sourceTex[t]);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494CompErr) != GL_NO_ERROR)
            return fail("draw");

        if (readback != nullptr)
        {
            readback->resize(512u * 512u);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, out.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          readback->data());
            if (g494GetError(kG494CompErr) != GL_NO_ERROR)
            {
                readback->clear();
                return fail("readback");
            }
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool prepareT8ViewOnWorker(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                               int texW, int texH,
                               const std::vector<uint32_t> &mapRgba,
                               const std::vector<uint32_t> &clutRgba,
                               bool verify, uint64_t &verifyBad)
    {
        static uint64_t s_failN = 0u;
        const auto fail = [&](const char *stage) {
            ++s_failN;
            if (s_failN <= 8u)
                std::fprintf(stderr,
                             "[G281:backend-fail] n=%llu stage=%s src=%03x fbw=%d "
                             "tex=%dx%d\n",
                             static_cast<unsigned long long>(s_failN), stage,
                             srcFbp, srcFbW, texW, texH);
            return false;
        };
        verifyBad = 0u;
        if (m_g281Prog == 0u || texKey == 0u || texW <= 0 || texH <= 0 ||
            mapRgba.size() != static_cast<size_t>(texW) * texH ||
            clutRgba.size() != 256u)
            return fail("args");
        const auto is = m_fbos.find(srcFbp);
        if (is == m_fbos.end() || is->second.colorTex == 0u ||
            is->second.w != srcFbW || is->second.h != 512)
            return fail("source");
        const FboEntry &src = is->second;

        auto dropOutput = [&]() {
            const auto it = m_texCache.find(texKey);
            if (it != m_texCache.end())
            {
                if (it->second.id != 0u)
                    glDeleteTextures(1, &it->second.id);
                m_texBytes -= std::min(
                    m_texBytes, static_cast<size_t>(it->second.w) * it->second.h * 4u);
                m_texCache.erase(it);
            }
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.erase(texKey);
        };

        g494DrainErrors(kG494T8Drain);
        if (m_g281MapTex == 0u)
            glGenTextures(1, &m_g281MapTex);
        if (m_g281ClutTex == 0u)
            glGenTextures(1, &m_g281ClutTex);
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281MapTex == 0u || m_g281ClutTex == 0u || m_g281Fbo == 0u)
            return fail("objects");

        // G431: this job runs ~8.7 times per frame on MAP-0 and the GS front thread BLOCKS on it
        // (0.18 ms per job, measured by [G324:census] t8Wall). Three of those milliseconds were
        // pure GL storage churn: glTexImage2D REALLOCATES the texture every build even though the
        // map/CLUT/destination geometry is identical build after build (the G415 lesson, applied
        // to the T8 view path). Reuse the allocation with glTexSubImage2D and set the immutable
        // sampler state only when the object is first created. Pixel content, filtering, wrap and
        // draw order are unchanged, so the produced view is bit-identical.
        //
        // MEASURED NEUTRAL (G431): pooled 2v2 lean MAP-0, both orders — 48.65 ON vs 48.89 OFF,
        // arms fully overlapping. The blocking cost is the worker round-trip itself (~0.05 ms
        // wall vs 0.005 ms CPU), not the storage churn. Kept OPT-IN (DC2_G431_VIEW_REUSE=1) as
        // an exact instrument; do not quote it as a win.
        static const bool s_g431Reuse = g158EnvFlag("DC2_G431_VIEW_REUSE");
        const bool reuseStorage = s_g431Reuse;
        static GLuint s_g431MapId = 0u, s_g431ClutId = 0u;
        static int s_g431MapW = -1, s_g431MapH = -1;

        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g281MapTex);
        const bool mapFresh = m_g281MapTex != s_g431MapId ||
                              texW != s_g431MapW || texH != s_g431MapH;
        if (!reuseStorage || mapFresh)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, mapRgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            s_g431MapId = m_g281MapTex;
            s_g431MapW = texW;
            s_g431MapH = texH;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW, texH,
                            GL_RGBA, GL_UNSIGNED_BYTE, mapRgba.data());
        }

        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, m_g281ClutTex);
        const bool clutFresh = m_g281ClutTex != s_g431ClutId;
        if (!reuseStorage || clutFresh)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 256, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, clutRgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            s_g431ClutId = m_g281ClutTex;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, clutRgba.data());
        }

        TexEntry &out = m_texCache[texKey];
        bool g431OutFresh = false;
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u)
            {
                dropOutput();
                return fail("output");
            }
            g431OutFresh = true;
        }
        else
            m_texBytes -= static_cast<size_t>(out.w) * out.h * 4u;
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        // The destination is fully rewritten by the full-viewport draw below, so when the
        // existing allocation already has these exact dimensions nothing needs re-specifying.
        if (!reuseStorage || g431OutFresh || out.w != texW || out.h != texH)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        }
        out.w = texW;
        out.h = texH;
        out.lastUse = m_batchCounter + 1u;
        m_texBytes += static_cast<size_t>(texW) * texH * 4u;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, out.id, 0);
        if (g494FboStatus(kG494T8Fbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            dropOutput();
            return fail("framebuffer");
        }
        glViewport(0, 0, texW, texH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glUseProgram_(m_g281Prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        g496PassBallast(); // G496 GPU derivative probe, `pass` site (idempotent re-issue)
        if (g494GetError(kG494T8Err) != GL_NO_ERROR)
        {
            dropOutput();
            return fail("draw");
        }

        if (verify)
        {
            std::vector<uint32_t> srcPx(static_cast<size_t>(src.w) * src.h);
            std::vector<uint32_t> outPx(static_cast<size_t>(texW) * texH);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, srcPx.data());
            glBindTexture(GL_TEXTURE_2D, out.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outPx.data());
            for (size_t k = 0; k < outPx.size(); ++k)
            {
                const uint32_t m = mapRgba[k];
                const uint32_t sx = (m & 0xFFu) |
                                    ((((m >> 16u) & 8u) >> 3u) << 8u);
                const uint32_t sy = ((m >> 8u) & 0xFFu) |
                                    (((m >> 16u) & 1u) << 8u);
                const uint32_t lane = (m >> 17u) & 3u;
                if (sx >= static_cast<uint32_t>(src.w) ||
                    sy >= static_cast<uint32_t>(src.h))
                {
                    ++verifyBad;
                    continue;
                }
                const uint32_t raw = srcPx[static_cast<size_t>(sy) * src.w + sx];
                const uint32_t index = (raw >> (lane * 8u)) & 0xFFu;
                if (outPx[k] != clutRgba[index])
                    ++verifyBad;
            }
            static uint64_t s_verifyN = 0u;
            ++s_verifyN;
            if (verifyBad != 0u || s_verifyN <= 8u || (s_verifyN % 256u) == 0u)
                std::fprintf(stderr, "[G281:vfy] n=%llu texels=%zu bad=%llu\n",
                             static_cast<unsigned long long>(s_verifyN), outPx.size(),
                             static_cast<unsigned long long>(verifyBad));
        }
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(texKey);
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        return true;
    }

    bool readDepthOnWorker(uint64_t depthKey, int w, int h, int depthY, int depthRows,
                           std::vector<float> &out)
    {
        if (depthKey == 0u || w <= 0 || h <= 0 || depthRows <= 0 || depthY < 0 ||
            depthY > h || depthRows > h - depthY)
            return false;
        auto it = m_depths.find(depthKey);
        if (it == m_depths.end() || it->second.tex == 0 ||
            it->second.w != w || it->second.h != h || !it->second.initialized)
            return false;
        out.resize(static_cast<size_t>(w) * depthRows);
        FboEntry &scratch = ensureFbo(0xFFFFFFFFu, w, h);
        if (scratch.fbo == 0)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, scratch.fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                    GL_TEXTURE_2D, it->second.tex, 0);
        if (g494FboStatus(kG494DepthFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
            }) != GL158_FRAMEBUFFER_COMPLETE)
        {
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, scratch.depthRb);
            return false;
        }
        glReadPixels(0, depthY, w, depthRows, GL_DEPTH_COMPONENT, GL_FLOAT, out.data());
        m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                       GL178_RENDERBUFFER, scratch.depthRb);
        return true;
    }

    void evictIfNeeded()
    {
        while (m_texBytes > kG178TexCacheCapBytes && !m_texCache.empty())
        {
            uint64_t oldestKey = 0;
            uint64_t oldestUse = ~0ull;
            for (const auto &kv : m_texCache)
            {
                if (kv.second.lastUse < oldestUse && kv.second.lastUse != m_batchCounter)
                {
                    oldestUse = kv.second.lastUse;
                    oldestKey = kv.first;
                }
            }
            if (oldestUse == ~0ull)
                break; // everything is in use by the current batch — can't evict
            TexEntry &t = m_texCache[oldestKey];
            m_texBytes -= static_cast<size_t>(t.w) * t.h * 4u;
            glDeleteTextures(1, &t.id);
            m_texCache.erase(oldestKey);
            {
                std::lock_guard<std::mutex> lk(m_residentMtx);
                m_resident.erase(oldestKey);
            }
        }
    }

    // Validate every condition that can make a submission fail before the first draw. In
    // particular, a late missing-texture failure would be unsafe for G242: earlier draws may
    // already have changed a persistent guest-depth texture even though the front-end is about
    // to replay the whole batch on the CPU.
    bool validateBatchInputs(const G178Batch &b, uint64_t depthKey,
                             const std::vector<float> *depthUpload,
                             int depthY, int depthRows)
    {
        if (b.fbW <= 0 || b.fbH <= 0)
            return false;
        const uint64_t fbPixelCount = static_cast<uint64_t>(b.fbW) *
                                      static_cast<uint64_t>(b.fbH);
        if (b.uploadFb && static_cast<uint64_t>(b.fbPixels.size()) != fbPixelCount)
            return false;

        const bool guestDepth = depthKey != 0u;
        if (guestDepth)
        {
            if (depthRows <= 0 || depthY < 0 || depthY > b.fbH ||
                depthRows > b.fbH - depthY)
                return false;
            if (depthUpload != nullptr &&
                static_cast<uint64_t>(depthUpload->size()) !=
                    static_cast<uint64_t>(b.fbW) * static_cast<uint64_t>(depthRows))
                return false;
            const auto it = m_depths.find(depthKey);
            const bool initialized = it != m_depths.end() && it->second.tex != 0 &&
                                     it->second.w == b.fbW && it->second.h == b.fbH &&
                                     it->second.initialized;
            // A new/recreated depth texture has undefined rows. Its first upload must therefore
            // cover the entire target; later submissions may omit it or update a row subset.
            if (!initialized &&
                (depthUpload == nullptr || depthY != 0 || depthRows != b.fbH))
                return false;
        }
        else if (depthUpload != nullptr || depthY != 0 || depthRows != 0)
        {
            return false;
        }

        std::unordered_set<uint64_t> uploadedKeys;
        uploadedKeys.reserve(b.texUploads.size());
        for (const G178TexUpload &up : b.texUploads)
        {
            if (up.key == 0u || up.w <= 0 || up.h <= 0 ||
                static_cast<uint64_t>(up.px.size()) !=
                    static_cast<uint64_t>(up.w) * static_cast<uint64_t>(up.h))
                return false;
            uploadedKeys.insert(up.key);
        }

        for (const G178Draw &d : b.draws)
        {
            if (d.firstVtx < 0 || d.vtxCount <= 0 || (d.firstVtx % 3) != 0 ||
                (d.vtxCount % 3) != 0 ||
                static_cast<size_t>(d.firstVtx) > b.verts.size() ||
                static_cast<size_t>(d.vtxCount) >
                    b.verts.size() - static_cast<size_t>(d.firstVtx))
                return false;
            if (d.scX0 > d.scX1 || d.scY0 > d.scY1 || d.blend > 5u || d.tfx > 3u ||
                d.wrapU > 1u || d.wrapV > 1u ||
                (d.depthFunc & 0x0Fu) > 3u)
                return false;
            if (d.srcFbp != 0u)
            {
                // G261 FBO-texture source: no texKey, never the batch's own target (feedback),
                // and the source FBO must already exist (a wave rendered into it).
                const uint32_t srcFbp = g289RealSourceFbp(d.srcFbp);
                if (d.texKey != 0u || srcFbp == b.fbp)
                    return false;
                const auto it = m_fbos.find(srcFbp);
                if (it == m_fbos.end() || it->second.fbo == 0 || it->second.colorTex == 0)
                    return false;
            }
            else if (d.texKey != 0u && uploadedKeys.count(d.texKey) == 0u)
            {
                const auto it = m_texCache.find(d.texKey);
                if (it == m_texCache.end() || it->second.id == 0)
                    return false;
            }
        }
        return true;
    }

    // G415 stage census bookkeeping. Keyed by (fbp, skipReadback, guest-depth) so the G299
    // command-zero family (readback, no guest depth) is directly comparable with the skip-readback
    // families that share the same target. Periodic aggregate print, never an early-capped one.
    // Stage marks written by renderBatch when the census is armed. kG415Marks-1 intervals:
    // 0->1 texture uploads, 1->2 FBO ensure/bind, 2->3 framebuffer upload, 3->4 depth setup,
    // 4->5 program/VBO/state, 5->6 draw loop, 6->7 pre-readback drain probe, 7->8 colour readback.
    static constexpr int kG415Marks = 9;
    struct G415Stage
    {
        uint64_t n = 0, draws = 0, rows = 0, winRows = 0, upRows = 0;
        uint64_t ns[kG415Marks - 1]{};
    };
    void g415NoteStages(const G178Batch &b, uint64_t depthKey,
                        const std::chrono::steady_clock::time_point *marks,
                        int readRows, int winRows, int upRows)
    {
        const uint64_t key = (static_cast<uint64_t>(b.fbp) << 8) |
                             (b.skipReadback ? 2u : 0u) | (depthKey != 0u ? 1u : 0u);
        G415Stage &s = m_g415Stages[key];
        ++s.n;
        for (int i = 0; i + 1 < kG415Marks; ++i)
            s.ns[i] += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    marks[i + 1] - marks[i]).count());
        s.draws += b.draws.size();
        s.rows += static_cast<uint64_t>(readRows);
        s.winRows += static_cast<uint64_t>(winRows);
        s.upRows += static_cast<uint64_t>(upRows);
        if ((++m_g415Total % 600u) != 0u)
            return;
        static const char *const kNames[kG415Marks - 1] = {
            "tex", "fbo", "up", "z", "state", "draw", "drain", "rb"};
        std::fprintf(stderr, "[G415:stage] total=%llu",
                     static_cast<unsigned long long>(m_g415Total));
        for (const auto &kv : m_g415Stages)
        {
            const G415Stage &e = kv.second;
            if (e.n == 0u)
                continue;
            std::fprintf(stderr, " fbp=0x%x/%s%s(n=%llu",
                         static_cast<unsigned>(kv.first >> 8),
                         (kv.first & 2u) ? "skip" : "read", (kv.first & 1u) ? "+z" : "",
                         static_cast<unsigned long long>(e.n));
            for (int i = 0; i + 1 < kG415Marks; ++i)
                std::fprintf(stderr, " %s=%.3f", kNames[i],
                             static_cast<double>(e.ns[i]) / 1.0e6 / e.n);
            std::fprintf(stderr, " rows=%.1f win=%.1f upRows=%.1f draws=%.1f)",
                         static_cast<double>(e.rows) / e.n,
                         static_cast<double>(e.winRows) / e.n,
                         static_cast<double>(e.upRows) / e.n,
                         static_cast<double>(e.draws) / e.n);
        }
        std::fprintf(stderr, "\n");
    }

    // G417 diagnostic-only split of the framebuffer upload GL edge. `drainNs` is work queued
    // before the upload, `submitNs` is the host call after that queue is empty, and `waitNs` is
    // completion of this upload itself. Key by target and geometry because width-flipping 0x139
    // and the narrow work targets have materially different byte counts.
    struct G417UploadStage
    {
        uint64_t n = 0, bytes = 0;
        uint64_t drainNs = 0, submitNs = 0, waitNs = 0;
    };
    void g417NoteUpload(const G178Batch &b, int rows, uint64_t drainNs,
                        uint64_t submitNs, uint64_t waitNs)
    {
        const uint64_t key = (static_cast<uint64_t>(b.fbp) << 32) |
                             (static_cast<uint64_t>(static_cast<uint32_t>(b.fbW)) << 16) |
                             static_cast<uint32_t>(b.fbH);
        G417UploadStage &s = m_g417Uploads[key];
        ++s.n;
        s.bytes += static_cast<uint64_t>(b.fbW) *
                   static_cast<uint64_t>(rows) * sizeof(uint32_t);
        s.drainNs += drainNs;
        s.submitNs += submitNs;
        s.waitNs += waitNs;
        if ((++m_g417UploadTotal % 512u) != 0u)
            return;
        std::fprintf(stderr, "[G417:backend] total=%llu",
                     static_cast<unsigned long long>(m_g417UploadTotal));
        for (const auto &kv : m_g417Uploads)
        {
            const G417UploadStage &e = kv.second;
            if (e.n == 0u)
                continue;
            std::fprintf(
                stderr,
                " fbp=0x%x/%ux%u(n=%llu KiB=%.1f drain=%.3f submit=%.3f wait=%.3f)",
                static_cast<unsigned>(kv.first >> 32),
                static_cast<unsigned>((kv.first >> 16) & 0xffffu),
                static_cast<unsigned>(kv.first & 0xffffu),
                static_cast<unsigned long long>(e.n),
                static_cast<double>(e.bytes) / 1024.0 / e.n,
                static_cast<double>(e.drainNs) / 1.0e6 / e.n,
                static_cast<double>(e.submitNs) / 1.0e6 / e.n,
                static_cast<double>(e.waitNs) / 1.0e6 / e.n);
        }
        std::fprintf(stderr, "\n");
    }

    // G418 diagnostic-only split of the colour-readback GL edge. `drainNs` is the glFinish that
    // empties GL work queued by earlier skip-readback batches; `rbNs` is this readback's own
    // transfer and completion once that queue is empty. `kickNs` is the cost of the early-kick
    // glFlush on the skip-readback batches that produce that queue, so the drain saving and the
    // kick cost are directly comparable inside one run. Keyed by target.
    struct G418ReadEdge
    {
        uint64_t n = 0, rows = 0, drainNs = 0, rbNs = 0;
        uint64_t kicks = 0, kickNs = 0;
    };
    void g418NoteReadEdge(uint32_t fbp, int rows, uint64_t drainNs, uint64_t rbNs)
    {
        G418ReadEdge &s = m_g418Edges[fbp];
        ++s.n;
        s.rows += static_cast<uint64_t>(rows);
        s.drainNs += drainNs;
        s.rbNs += rbNs;
        g418ReportEdges();
    }
    void g418NoteKick(uint32_t fbp, uint64_t kickNs)
    {
        G418ReadEdge &s = m_g418Edges[fbp];
        ++s.kicks;
        s.kickNs += kickNs;
        g418ReportEdges();
    }
    void g418ReportEdges()
    {
        if ((++m_g418Total % 600u) != 0u)
            return;
        std::fprintf(stderr, "[G418:edge] total=%llu",
                     static_cast<unsigned long long>(m_g418Total));
        for (const auto &kv : m_g418Edges)
        {
            const G418ReadEdge &e = kv.second;
            std::fprintf(stderr, " fbp=0x%x(", static_cast<unsigned>(kv.first));
            if (e.n != 0u)
                std::fprintf(stderr, "rb=%llu rows=%.1f drain=%.3f xfer=%.3f",
                             static_cast<unsigned long long>(e.n),
                             static_cast<double>(e.rows) / e.n,
                             static_cast<double>(e.drainNs) / 1.0e6 / e.n,
                             static_cast<double>(e.rbNs) / 1.0e6 / e.n);
            if (e.kicks != 0u)
                std::fprintf(stderr, "%skick=%llu kickMs=%.4f",
                             e.n != 0u ? " " : "",
                             static_cast<unsigned long long>(e.kicks),
                             static_cast<double>(e.kickNs) / 1.0e6 / e.kicks);
            std::fprintf(stderr, ")");
        }
        std::fprintf(stderr, "\n");
    }

    bool renderBatch(G178Batch &b, uint64_t depthKey, const std::vector<float> *depthUpload,
                     int depthY, int depthRows,
                     std::vector<float> *fusedDepthReadback = nullptr,
                     int depthReadY = 0, int depthReadRows = 0,
                     const G415ColorWindow &g415Window = G415ColorWindow{})
    {
        if (!validateBatchInputs(b, depthKey, depthUpload, depthY, depthRows))
            return false;
        // G415 stage census: attribute this batch's backend work to texture upload / FBO bind /
        // framebuffer upload / depth setup / state / draw loop / drain / colour readback.
        // Behaviour-pure, default-off (DC2_G415_CENSUS=1).
        const bool g415Census = g415CensusOn();
        std::chrono::steady_clock::time_point g415Marks[kG415Marks]{};
        int g415Mark = 0;
        const auto g415Note = [&] {
            if (g415Census && g415Mark < kG415Marks)
                g415Marks[g415Mark] = std::chrono::steady_clock::now();
            ++g415Mark;
        };
        g415Note(); // 0: entry
        // Validate the fused read before uploads or draws: a rejected combined job must leave
        // persistent color/depth state untouched so the front-end can safely replay on the CPU.
        if (fusedDepthReadback != nullptr &&
            (depthKey == 0u || depthReadY < 0 || depthReadRows <= 0 ||
             depthReadY > b.fbH || depthReadRows > b.fbH - depthReadY))
            return false;
        ++m_batchCounter;

        // Texture (re)uploads: replace-in-place keyed by the front-end's descriptor hash.
        for (G178TexUpload &up : b.texUploads)
        {
            TexEntry &t = m_texCache[up.key];
            if (t.id == 0)
            {
                glGenTextures(1, &t.id);
                if (t.id == 0)
                    return false;
            }
            else
                m_texBytes -= static_cast<size_t>(t.w) * t.h * 4u;
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, up.w, up.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         up.px.data());
            t.w = up.w;
            t.h = up.h;
            t.lastUse = m_batchCounter;
            m_texBytes += static_cast<size_t>(up.w) * up.h * 4u;
            {
                std::lock_guard<std::mutex> lk(m_residentMtx);
                m_resident.insert(up.key);
            }
        }

        // Protect every texture used by this batch from LRU eviction, then resolve all handles.
        // No texture-cache operation occurs after this point, so draw submission has no late
        // lookup failure path and pointers into the unordered_map remain stable.
        for (const G178Draw &d : b.draws)
            if (d.texKey != 0u)
                m_texCache.find(d.texKey)->second.lastUse = m_batchCounter;
        evictIfNeeded();
        std::vector<TexEntry *> drawTextures(b.draws.size(), nullptr);
        for (size_t i = 0; i < b.draws.size(); ++i)
        {
            if (b.draws[i].texKey == 0u)
                continue;
            const auto it = m_texCache.find(b.draws[i].texKey);
            if (it == m_texCache.end() || it->second.id == 0)
                return false;
            drawTextures[i] = &it->second;
        }

        // Allocate all caller-visible output storage before drawing. A bad_alloc is caught by the
        // worker and reported as a clean failure while persistent color/depth are still untouched.
        // G261 skipReadback waves leave the result GPU-resident: no output storage, no readback.
        if (b.skipReadback)
            b.readback.clear();
        else
            b.readback.resize(static_cast<size_t>(b.fbW) * b.fbH);
        g415Note(); // 1: texture uploads + eviction + handle resolve + output alloc

        // G351: an explicit surface key (Owner-B sprite → sibling FBO) overrides the fbp key.
        FboEntry &fbo = ensureFbo(b.fboKey != 0u ? b.fboKey : b.fbp, b.fbW, b.fbH);
        if (fbo.fbo == 0)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
        glViewport(0, 0, b.fbW, b.fbH);
        g415Note(); // 2: FBO ensure/bind

        const int g417UploadRowsThis = b.uploadFb ? b.fbH : 0;
        if (b.uploadFb)
        {
            glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
            if (g417ProfileOn())
            {
                const auto drainT0 = std::chrono::steady_clock::now();
                glFinish();
                const auto submitT0 = std::chrono::steady_clock::now();
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, b.fbW, b.fbH,
                                GL_RGBA, GL_UNSIGNED_BYTE, b.fbPixels.data());
                const auto waitT0 = std::chrono::steady_clock::now();
                glFinish();
                const auto done = std::chrono::steady_clock::now();
                const auto ns = [](const auto &a, const auto &z) {
                    return static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count());
                };
                g417NoteUpload(b, b.fbH, ns(drainT0, submitT0),
                               ns(submitT0, waitT0), ns(waitT0, done));
            }
            else
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, b.fbW, b.fbH,
                                GL_RGBA, GL_UNSIGNED_BYTE, b.fbPixels.data());
            }
        }
        g415Note(); // 3: framebuffer upload

        // G242: the real guest Z target owns one persistent texture keyed by ZBP/ZBW/ZPSM, not by
        // the alternating color FBP. The front-end uploads only when guest VRAM changed and reads
        // back only at a CPU/transfer/frame boundary.
        const bool guestDepth = depthKey != 0u;
        if (guestDepth)
        {
            DepthEntry &depth = ensureDepth(depthKey, b.fbW, b.fbH);
            if (depth.tex == 0 || depth.w != b.fbW || depth.h != b.fbH)
                return false;
            if (depthUpload != nullptr)
            {
                glBindTexture(GL_TEXTURE_2D, depth.tex);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, depthY, b.fbW, depthRows,
                                GL_DEPTH_COMPONENT, GL_FLOAT, depthUpload->data());
                depth.initialized = true;
            }
            m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                        GL_TEXTURE_2D, depth.tex, 0);
            // G494: `depth` was just validated to be exactly b.fbW x b.fbH against the same
            // colour attachment, so completeness here is a property of an invariant attachment set.
            if (g494FboStatus(kG494RenderFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                    return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                }) != GL158_FRAMEBUFFER_COMPLETE)
            {
                m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                               GL178_RENDERBUFFER, fbo.depthRb);
                return false;
            }
        }
        else
        {
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
            // G494: this attaches the FBO's OWN depth renderbuffer — the same object, at the same
            // size, that ensureFbo already validated when it built this FBO.
            if (g494FboStatus(kG494RenderFbo, GL158_FRAMEBUFFER_COMPLETE, [&]() {
                    return m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
                }) != GL158_FRAMEBUFFER_COMPLETE)
                return false;
        }
        if (b.clearDepth && !guestDepth)
        {
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        g415Note(); // 4: depth attach / clear

        m_f.glUseProgram_(m_prog);
        m_f.glBindVertexArray_(m_vao);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        m_f.glBufferData_(GL158_ARRAY_BUFFER,
                          static_cast<GLsizeiptrLocal>(b.verts.size() * sizeof(G178Vtx)),
                          b.verts.data(), GL178_STREAM_DRAW);
        m_e.glUniform2f_(m_locFbSize, static_cast<GLfloat>(b.fbW), static_cast<GLfloat>(b.fbH));
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glEnable(GL_SCISSOR_TEST);

        // G268: was a raw getenv() per submitted batch; env is process-constant
        // (DC2_G268_LIVE_ENVREAD=1 restores the per-call read for A/B).
        static const bool s_g255GpuRttEnvLive = g158EnvFlag("DC2_G268_LIVE_ENVREAD");
        static const bool s_g255GpuRttEnv = (std::getenv("DC2_G255_GPU_RTT") != nullptr);
        const bool g255RttAlpha =
            (b.rttRawAlpha || (s_g255GpuRttEnvLive
                                   ? (std::getenv("DC2_G255_GPU_RTT") != nullptr)
                                   : s_g255GpuRttEnv)) &&
            (b.fbp == 0x139u || b.fbp == 0x13cu || b.fbp == 0x143u ||
             b.fbp == 0x146u || b.fbp == 0x155u);
        const bool g256Exact = g256ExactRttOn() && m_progExact != 0u &&
            (b.fbp == 0x139u || b.fbp == 0x13cu || b.fbp == 0x143u ||
             b.fbp == 0x146u || b.fbp == 0x155u) && !guestDepth;
        const bool g257RealDiagnostic = g256Exact && g257ImageStoreTestOn() &&
                                        m_g257RealBatches < 32u;
        const bool g258Trace = g256Exact && g258TraceOn() && b.fbp == 0x146u &&
                               m_g258TraceBatches < 4u;
        const uint64_t g258TraceBatch = g258Trace ? ++m_g258TraceBatches : 0u;
        std::vector<uint32_t> g257ImageBefore;
        GLuint g257CoverageFbo = 0;
        GLenum g257PreError = GL_NO_ERROR;
        unsigned g257PreErrorCount = 0;
        auto g257DrainErrors = [](GLenum &first, unsigned &count) {
            first = GL_NO_ERROR;
            count = 0;
            for (GLenum e = glGetError(); e != GL_NO_ERROR && count < 32; e = glGetError())
            {
                if (count == 0)
                    first = e;
                ++count;
            }
        };
        if (g256Exact)
        {
            // Do not rasterize with the image-load/store target attached to the active FBO.
            // That is undefined feedback even with color writes masked off.  A persistent
            // same-sized scratch FBO supplies fragment coverage; the guest color texture is
            // updated exclusively through uFb and remains attached to its own inactive FBO.
            FboEntry &coverage = ensureFbo(0xFFFFFFFEu, b.fbW, b.fbH);
            if (coverage.fbo == 0)
                return false;
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, coverage.fbo);
            g257CoverageFbo = coverage.fbo;
            glViewport(0, 0, b.fbW, b.fbH);
            if (g257RealDiagnostic)
            {
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                g257ImageBefore.resize(static_cast<size_t>(b.fbW) * b.fbH);
                glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              g257ImageBefore.data());
                g257DrainErrors(g257PreError, g257PreErrorCount);
            }
            m_f.glUseProgram_(m_progExact);
            m_e.glUniform2f_(m_locExactFbSize, static_cast<GLfloat>(b.fbW),
                             static_cast<GLfloat>(b.fbH));
            m_e.glBindImageTexture_(1u, fbo.colorTex, 0, GL_FALSE, 0, GL178_READ_WRITE,
                                    GL158_RGBA8);
            m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                 GL178_FRAMEBUFFER_BARRIER_BIT);
            // Keep a real framebuffer output live in the scratch attachment.  Some Windows GL
            // drivers elide a fragment invocation whose only framebuffer output is fully masked,
            // even though imageStore is a side effect.  The scratch result is deliberately ignored.
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_BLEND);
        }
        g415Note(); // 5: program / VBO / uniform / exact-path state
        // G496b: redundant GL state elision. The arm is read ONCE per batch (thread-level, block
        // held) and the non-texture cache is reset here, because the composite / T8 / CT24 / copy /
        // writecolor items that run between batches own all of this state. Both arms leave GL
        // holding the same state vector — the cache is written on every path, elision only decides
        // whether the call that would set the value already there is issued.
        const int g496GlArm = g496GlStateArm(); // -1 unless DC2_G419_AB=glstate
        const bool g496GlCache =
            (g496GlArm >= 0) ? (g496GlArm == 1) : g496GlCacheDefaultOn();
        g496GlReset();
        const auto g496Uni = [&](GLint loc, int value) { m_f.glUniform1i_(loc, value); };
        for (size_t drawIndex = 0; drawIndex < b.draws.size(); ++drawIndex)
        {
            const G178Draw &d = b.draws[drawIndex];
            // GS scissor is inclusive, top-origin; GL is bottom-origin.
            const int scH = d.scY1 - d.scY0 + 1;
            g496Scissor(d.scX0, b.fbH - 1 - d.scY1, d.scX1 - d.scX0 + 1, scH, g496GlCache);
            const uint8_t depthFunc = d.depthFunc & 0x0Fu;
            if (depthFunc == 0)
            {
                g496DepthOff(g496GlCache);
            }
            else
            {
                g496DepthOn(depthFunc == 1 ? GL_ALWAYS :
                                (depthFunc == 2 ? GL_GEQUAL : GL_GREATER),
                            d.depthWrite != 0u, g496GlCache);
            }
            if (g256Exact)
            {
                int mode = (static_cast<int>(d.blend & 3u) << 8);
                if (b.fbp == 0x139u)
                    mode |= 1024; // CPU writePixel's established opaque-over-black RTT rule.
                if (d.texKey != 0u)
                {
                    TexEntry *const texture = drawTextures[drawIndex];
                    glBindTexture(GL_TEXTURE_2D, texture->id);
                    // A newly uploaded GL texture defaults to a mipmapped MIN filter, but the
                    // cache intentionally provides level 0 only.  Even texelFetch returns the
                    // incomplete-texture value (0,0,0,1) in that state.  G256 performs its own
                    // point/bilinear taps, so NEAREST here only completes the sampler object.
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                }
                m_f.glUniform1i_(m_locExactMode, mode);
                m_f.glUniform1i_(m_locExactLinear, d.bilinear ? 1 : 0);
                m_f.glUniform1i_(m_locExactWrapU, static_cast<GLint>(d.wrapU));
                m_f.glUniform1i_(m_locExactWrapV, static_cast<GLint>(d.wrapV));
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                // G256 keeps one captured sprite per draw.  Publish its image stores before the
                // next sprite reads the same destination, preserving CPU submission/blend order.
                m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                     GL178_TEXTURE_FETCH_BARRIER_BIT |
                                     GL178_FRAMEBUFFER_BARRIER_BIT);
                if (g258Trace && (drawIndex == 0u || drawIndex == 6u))
                {
                    std::vector<uint32_t> afterDraw(static_cast<size_t>(b.fbW) * b.fbH);
                    glFinish();
                    glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, afterDraw.data());
                    const size_t watch = static_cast<size_t>(b.fbH - 2) * b.fbW + 52u;
                    std::fprintf(stderr,
                                 "[G258:gpu.draw] batch=%llu draw=%zu rgba=%08x\n",
                                 static_cast<unsigned long long>(g258TraceBatch),
                                 drawIndex, watch < afterDraw.size() ? afterDraw[watch] : 0u);
                }
                continue;
            }
            if (d.blend == 0)
            {
                g496BlendOff(g496GlCache);
            }
            else
            {
                // Alpha channel: keep dest alpha = written src value (CPU writePixel stores the
                // source alpha unblended). blend==3 (G262): (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd
                // additive; blend==4 (G262): (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs subtractive
                // (GL_FUNC_REVERSE_SUBTRACT = dst - src, both clamp at the [0,1] rail like GS
                // COLCLAMP). The RGB equation is persistent GL state, so it must be re-set whenever
                // it is not already the one this draw wants — which is exactly what the cache
                // tracks; a run of same-mode blended draws now sets it once.
                g496BlendOn([&](GLenum eqRgb, GLenum eqA)
                            { m_e.glBlendEquationSeparate_(eqRgb, eqA); },
                            [&](GLenum sRgb, GLenum dRgb, GLenum sA, GLenum dA)
                            { m_e.glBlendFuncSeparate_(sRgb, dRgb, sA, dA); },
                            d.blend == 4 ? GL178_FUNC_REVERSE_SUBTRACT : GL178_FUNC_ADD,
                            GL178_FUNC_ADD,
                            d.blend == 5 ? GL_ZERO
                                         : ((d.blend == 3 || d.blend == 4) ? GL_ONE
                                                                           : GL_SRC_ALPHA),
                            (d.blend == 1 || d.blend == 5) ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE,
                            GL_ONE, GL_ZERO, g496GlCache);
            }
            int mode = 0;
            const bool g286Exact = b.fbp == 0x13bu;
            // G362: point-sampled draws on the legacy shader path take the guarded fixed-point
            // fetch (uMode bit 12) instead of GL_NEAREST's float floor(). Default-ON; roll back
            // with DC2_G362_NO_UVGUARD=1. Bilinear draws keep texture()'s filtered path, and the
            // G256/G286 exact programs already reconstruct the coordinate themselves.
            static const bool s_g362NoGuard = g158EnvFlag("DC2_G362_NO_UVGUARD");
            const bool g362PointGuard = !s_g362NoGuard && !g256Exact && !g286Exact && !d.bilinear;
            // Bit 4 asks the fragment shader to clamp interpolated GS Z to PSMZ24 before
            // normalization, matching the CPU path's post-interpolation uint24 clamp.
            if ((d.depthFunc & 0x10u) != 0u)
                mode |= 16;
            // G265: the admitted nontrivial alpha-test shapes are GEQUAL with AFAIL=KEEP.
            // Fragment discard suppresses color and depth together, exactly matching KEEP.
            if ((d.depthFunc & 0x20u) != 0u)
                mode |= 64;
            mode |= (static_cast<int>((d.depthFunc >> 6u) & 3u) << 7u);
            if (d.srcFbp != 0u)
            {
                // G261: sample the producer target's persistent FBO color texture directly. The
                // front-end already remapped UVs into the FBO's full fbW x fbH bottom-first space,
                // and only admits in-range UVs — CLAMP here is inert insurance, never wrap.
                if (d.srcFbp == kG309CompositeToken)
                {
                    static constexpr uint32_t kSources[5] =
                        {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
                    for (uint32_t t = 0u; t < 5u; ++t)
                    {
                        m_f.glActiveTexture_(GL158_TEXTURE0 + t);
                        const GLuint g496Src = m_fbos.find(kSources[t])->second.colorTex;
                        // G496b: only unit 0 is cached - it is the unit every other draw in this
                        // loop binds through, and this branch leaves kSources[0] resident on it.
                        if (t == 0u)
                            g496BindTex(g496Src, g496GlCache);
                        else
                            glBindTexture(GL_TEXTURE_2D, g496Src);
                    }
                    m_f.glActiveTexture_(GL158_TEXTURE0 + 5u);
                    glBindTexture(GL_TEXTURE_2D, m_g309MapTex);
                    m_f.glActiveTexture_(GL158_TEXTURE0 + 6u);
                    glBindTexture(GL_TEXTURE_2D, m_g309BaseTex);
                    m_f.glActiveTexture_(GL158_TEXTURE0);
                    mode |= 2048;
                }
                else
                {
                    const FboEntry &src =
                        m_fbos.find(g289RealSourceFbp(d.srcFbp))->second;
                    // G496b: the BIND is cached; the four sampler params are NOT. An FboEntry
                    // colour texture is re-parameterised by the T8, CT24, composite-source and
                    // spare-swap paths, so a per-object record here could go stale.
                    g496BindTex(src.colorTex, g496GlCache);
                    const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    const bool ownerSource = g289SourceIsOwner(d.srcFbp);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                    ownerSource && d.wrapU == 0u
                                        ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                    ownerSource && d.wrapV == 0u
                                        ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                }
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                if (g289SourceIsCt24(d.srcFbp))
                    mode |= 1024;
                // G362: the G309 composite branch (bit 11) does its own page-mapped fetch.
                if (g362PointGuard && (mode & 2048) == 0)
                    mode |= 4096;
            }
            if (g286Exact)
            {
                // G287: G286's exact branch selected integer sampling/combine but skipped the
                // ordinary texture-bind branch below. It therefore sampled whichever texture a
                // previous draw had left bound, and uMode silently described TFX=MODULATE/TCC=0.
                // Bind this draw's decoded PSMT8+CLUT texture explicitly and carry its real
                // combine state into the exact shader. G287 promotes the narrowly admitted
                // fbp=0x13b shape after exact-oracle and downstream-presentation gates.
                TexEntry *const texture = drawTextures[drawIndex];
                if (texture == nullptr)
                    return false;
                g496BindTex(texture->id, g496GlCache);
                // Newly created cache textures default to a mipmapped MIN filter while this
                // cache intentionally uploads level 0 only. The first G287 batch therefore
                // sampled the GL incomplete-texture value until another path happened to make
                // the object complete. Integer taps do not need filtering, but the sampler
                // object must still be complete from first use.
                g496TexFilter(texture->g496MinF, texture->g496MagF, GL_NEAREST, GL_NEAREST,
                              g496GlCache);
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                mode |= 512;
                g496Uniform1i(g496Uni, kG496ULinear, m_locLinear, d.bilinear ? 1 : 0, g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapU, m_locWrapU, static_cast<int>(d.wrapU),
                              g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapV, m_locWrapV, static_cast<int>(d.wrapV),
                              g496GlCache);
            }
            else if (d.texKey != 0)
            {
                TexEntry *const texture = drawTextures[drawIndex];
                g496BindTex(texture->id, g496GlCache);
                const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                g496TexFilter(texture->g496MinF, texture->g496MagF, filter, filter, g496GlCache);
                g496TexWrap(texture->g496WrapS, texture->g496WrapT,
                            d.wrapU == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE),
                            d.wrapV == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE),
                            g496GlCache);
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                if (g362PointGuard)
                    mode |= 4096;
            }
            const bool g407AccurateUv =
                ((d.uvRoundU | d.uvRoundV) & 8u) != 0u;
            if (g407AccurateUv && (mode & 1) != 0 && (mode & 512) == 0)
                mode |= 4096;
            if ((mode & 4096) != 0)
            {
                // Accurate UV uses exact 12.4 point/bilinear taps. The wrap mode travels as a
                // uniform because integer taps do not use the GL sampler's addressing state.
                g496Uniform1i(g496Uni, kG496ULinear, m_locLinear, d.bilinear ? 1 : 0, g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapU, m_locWrapU, static_cast<int>(d.wrapU),
                              g496GlCache);
                g496Uniform1i(g496Uni, kG496UWrapV, m_locWrapV, static_cast<int>(d.wrapV),
                              g496GlCache);
                m_g362GuardDraws += 1u;
            }
            else if ((mode & 1) != 0)
            {
                m_g362PlainDraws += 1u;
            }
            // G364: GS per-pixel fog. Bit 13 + the packed FOGCOL travel per draw (FOGCOL is global
            // GS state; the front-end snapshots it into each G178Draw so a mid-frame change splits
            // the batch through g178StateEqual).
            if (d.fge != 0u)
            {
                mode |= 8192;
                g496Uniform1i(g496Uni, kG496UFogCol, m_locFogCol,
                              static_cast<int>(d.fogCol & 0x00FFFFFFu), g496GlCache);
            }
            if (d.uvRoundU != 0u || d.uvRoundV != 0u)
            {
                mode |= 16384;
                g496Uniform1i(g496Uni, kG496UOriginX, m_locUvOriginX,
                              static_cast<int>(d.uvOriginX), g496GlCache);
                g496Uniform1i(g496Uni, kG496UOriginY, m_locUvOriginY,
                              static_cast<int>(d.uvOriginY), g496GlCache);
                g496Uniform1i(g496Uni, kG496URoundU, m_locUvRoundU,
                              static_cast<int>(d.uvRoundU), g496GlCache);
                g496Uniform1i(g496Uni, kG496URoundV, m_locUvRoundV,
                              static_cast<int>(d.uvRoundV), g496GlCache);
            }
            if (g255RttAlpha && d.blend != 0u)
            {
                // Fixed-function blending needs As/128 in the shader alpha channel, while PS2
                // framebuffer alpha stores the unblended source A byte. Preserve RGB blending in
                // pass one with alpha masked, then overwrite alpha only with the raw source value.
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                g496Uniform1i(g496Uni, kG496UMode, m_locMode, mode, g496GlCache);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                // Routed through the cache so the record tracks the real GL state: this branch
                // disables blending after g496BlendOn enabled it, and the next draw must not
                // believe blending is still on.
                g496BlendOff(g496GlCache);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                g496Uniform1i(g496Uni, kG496UMode, m_locMode, mode | 32, g496GlCache);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            else
            {
                if (g255RttAlpha)
                    mode |= 32;
                g496Uniform1i(g496Uni, kG496UMode, m_locMode, mode, g496GlCache);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
            }
        }

        // G496 GPU DERIVATIVE PROBE (diagnostic only — DC2_G419_AB=gpuslow / DC2_G496_FORCE=1).
        // Re-issue this batch's whole vertex range with an IDENTITY BLEND: dst = 0*src + 1*dst, with
        // the depth test disabled and depth writes masked. Every fragment is rasterized, shaded,
        // blended and written, so no driver can elide it (the colour-mask form the G495 roadmap
        // proposed is elidable on this platform — see the note above the exact program's scratch
        // attachment), and the byte written is the byte that was already there. One draw call covers
        // the whole batch, so the CPU cost added to the GL worker is one call per batch.
        // Excluded on the G256 exact path: that shader's imageStore is a side effect no blend can
        // undo. g256ExactRttOn() needs two env flags and is off on every default/perf run.
        const int g496Reps = g256Exact ? 0 : g496BallastForBatch(m_batchCounter);
        if (g496Reps > 0 && b.verts.size() >= 3u)
        {
            g496CalBegin();
            glScissor(0, 0, b.fbW, b.fbH);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_BLEND);
            m_e.glBlendEquationSeparate_(GL178_FUNC_ADD, GL178_FUNC_ADD);
            m_e.glBlendFuncSeparate_(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
            const GLsizei g496Vtx = static_cast<GLsizei>((b.verts.size() / 3u) * 3u);
            for (int rep = 0; rep < g496Reps; ++rep)
                glDrawArrays(GL_TRIANGLES, 0, g496Vtx);
            glDisable(GL_BLEND);
            g496CalEnd(static_cast<uint64_t>(g496Reps),
                       static_cast<uint64_t>(g496Vtx) * static_cast<uint64_t>(g496Reps));
        }

        if (g256Exact)
        {
            m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                 GL178_TEXTURE_FETCH_BARRIER_BIT |
                                 GL178_FRAMEBUFFER_BARRIER_BIT);
            std::vector<uint32_t> g257ImageAfter;
            std::vector<uint32_t> g257Coverage;
            GLenum g257PostError = GL_NO_ERROR;
            unsigned g257PostErrorCount = 0;
            if (g257RealDiagnostic)
            {
                g257ImageAfter.resize(static_cast<size_t>(b.fbW) * b.fbH);
                glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              g257ImageAfter.data());
                g257Coverage.resize(static_cast<size_t>(b.fbW) * b.fbH);
                m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, g257CoverageFbo);
                glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE,
                             g257Coverage.data());
                g257DrainErrors(g257PostError, g257PostErrorCount);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_e.glBindImageTexture_(1u, 0u, 0, GL_FALSE, 0, GL178_READ_WRITE, GL158_RGBA8);
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
            m_f.glUseProgram_(m_prog);

            if (g257RealDiagnostic)
            {
                size_t changed = 0;
                size_t covered = 0;
                for (size_t i = 0; i < g257ImageAfter.size(); ++i)
                {
                    if (g257ImageAfter[i] != g257ImageBefore[i])
                        ++changed;
                    if (g257Coverage[i] != 0u)
                        ++covered;
                }
                // Preserve the direct texture snapshot until after the normal FBO readback below.
                m_g257ImageAfter.swap(g257ImageAfter);
                m_g257Changed = changed;
                m_g257Covered = covered;
                m_g257PreError = g257PreError;
                m_g257PreErrorCount = g257PreErrorCount;
                m_g257PostError = g257PostError;
                m_g257PostErrorCount = g257PostErrorCount;
            }
        }

        glDisable(GL_SCISSOR_TEST);
        g415Note(); // 6: draw loop (+ exact-path epilogue)
        // G418 early kick: this batch leaves its GL work queued and never fences, so without a kick
        // the frame's one readback batch pays the whole backlog as drain. glFlush only guarantees
        // that already-issued commands finish in finite time - it changes no result, no GL state,
        // no command order, and no ownership - so the arm is exact by construction.
        const bool g418Profile = g418ProfileOn();
        if (b.skipReadback && g445EarlyKickActive())
        {
            const auto kickT0 = g418Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            glFlush();
            if (g418Profile)
                g418NoteKick(b.fbp,
                             static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - kickT0).count()));
        }
        // G415 fence attribution: drain everything already queued on this context BEFORE the
        // readback, so the census can tell "waiting for prior GL work" apart from "this transfer".
        // Diagnostic only (DC2_G415_CENSUS=1 + DC2_G415_FENCE_PROBE=1).
        // G418 takes the same split without the nine-mark stage census riding along; only one of the
        // two probes ever issues the finish.
        uint64_t g418DrainNs = 0u;
        std::chrono::steady_clock::time_point g418RbT0;
        if (!b.skipReadback && (g415FenceProbeOn() || g418Profile))
        {
            const auto drainT0 = std::chrono::steady_clock::now();
            glFinish();
            g418RbT0 = std::chrono::steady_clock::now();
            g418DrainNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g418RbT0 - drainT0).count());
        }
        g415Note(); // 7: pre-readback drain probe
        int g415ReadRows = 0;
        if (!b.skipReadback)
        {
            // G496 calibration frame clock on the GL worker: exactly one render batch per frame
            // reads back (G495 §2 measured rb/frames = 0.99), so this is the divisor that turns the
            // ballast's accumulated GPU nanoseconds into a real ms/FRAME. No-op unless DC2_G496_CAL.
            g496NoteFrame();
            g496GlNoteFrame();
            // G415: read only the GS row window this batch's caller actually consumes, into that
            // window's existing offsets in the full-height buffer. Buffer row index == GL row, so
            // the destination pointer offset is glY*fbW and every consumer index is unchanged.
            // Any missing/odd window falls back to the full-height read (fail-closed).
            // G419: evaluate the arm FIRST and unconditionally. Inside a short-circuiting && chain
            // the instrument's consumer record would be skipped whenever an earlier term failed,
            // and the tick would drop that frame period instead of charging it.
            const bool narrowArm = g415NarrowReadbackOn();
            // G420 mechanism probe: evaluated here, unconditionally, for the same short-circuit
            // reason as narrowArm above.
            const bool g420Touch = g420NarrowTouchArm();
            const bool narrow =
                narrowArm && g415Window.valid && g415Window.rows > 0 &&
                g415Window.glY >= 0 && g415Window.rows <= b.fbH - g415Window.glY &&
                b.readback.size() == static_cast<size_t>(b.fbW) * b.fbH;
            // G420 shape probe: widen the read to a forced row count. Exact for any count >= the
            // published window — the read still covers every consumed row at its existing offset,
            // and rows outside the consumed window are discarded either way (see
            // g420NarrowRowsOverride).
            int narrowY = g415Window.glY;
            int narrowRows = g415Window.rows;
            if (narrow)
            {
                const int forced = std::min(g420NarrowRowsOverride(), b.fbH);
                if (forced > narrowRows)
                {
                    narrowRows = forced;
                    narrowY = std::max(0, std::min(narrowY, b.fbH - forced));
                }
            }
            if (narrow)
            {
                glReadPixels(0, narrowY, b.fbW, narrowRows,
                             GL_RGBA, GL_UNSIGNED_BYTE,
                             b.readback.data() +
                                 static_cast<size_t>(narrowY) * b.fbW);
                g415ReadRows = narrowRows;
                if (g415StatOn())
                {
                    g_g415NarrowBatches.fetch_add(1u, std::memory_order_relaxed);
                    g_g415NarrowRows.fetch_add(static_cast<uint64_t>(narrowRows),
                                               std::memory_order_relaxed);
                }
            }
            else
            {
                glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE, b.readback.data());
                g415ReadRows = b.fbH;
                if (g415StatOn())
                {
                    g_g415FullBatches.fetch_add(1u, std::memory_order_relaxed);
                    g_g415FullRows.fetch_add(static_cast<uint64_t>(b.fbH),
                                             std::memory_order_relaxed);
                }
            }
            // G419 ballast (diagnostic only, DC2_G419_BALLAST with DC2_G419_AB=narrow): re-read the
            // SAME rect into the SAME destination N extra times. Byte-idempotent, so the measured
            // frame-time delta divided by the known added transfer cost is this site's critical-path
            // sensitivity - the multiplier a removed-row saving has to be scaled by.
            for (int rep = g419NarrowBallastReps(); rep > 0; --rep)
            {
                if (narrow)
                    glReadPixels(0, narrowY, b.fbW, narrowRows,
                                 GL_RGBA, GL_UNSIGNED_BYTE,
                                 b.readback.data() +
                                     static_cast<size_t>(narrowY) * b.fbW);
                else
                    glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE,
                                 b.readback.data());
            }
            // G420 mechanism probe (see g420NarrowTouchArm): add the DESTINATION-side write volume
            // of the rows the full read would have produced and the consumer discards, into an
            // inert backend-private scratch buffer. No GL call, no observable byte.
            if (g420Touch && g415ReadRows > 0 && g415ReadRows < b.fbH)
            {
                const size_t words = static_cast<size_t>(b.fbW) *
                                     static_cast<size_t>(b.fbH - g415ReadRows);
                if (m_g420TouchScratch.size() < words)
                    m_g420TouchScratch.resize(words);
                std::memset(m_g420TouchScratch.data(), 0, words * sizeof(uint32_t));
            }
            if (g415StatOn() && (++m_g415StatN % 300u) == 0u)
            {
                const uint64_t nn = g_g415NarrowBatches.load(std::memory_order_relaxed);
                const uint64_t nr = g_g415NarrowRows.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[G415:narrow] narrow=%llu rows=%llu avgRows=%.1f full=%llu "
                             "fullRows=%llu fboCreate=%llu fboPark=%llu fboRevive=%llu\n",
                             static_cast<unsigned long long>(nn),
                             static_cast<unsigned long long>(nr),
                             nn ? static_cast<double>(nr) / nn : 0.0,
                             static_cast<unsigned long long>(
                                 g_g415FullBatches.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 g_g415FullRows.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(m_g415FboCreates),
                             static_cast<unsigned long long>(m_g415FboParks),
                             static_cast<unsigned long long>(m_g415FboRevives));
            }
        }
        g415Note(); // 8: colour readback
        if (!b.skipReadback && g418Profile)
            g418NoteReadEdge(b.fbp, g415ReadRows, g418DrainNs,
                             static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - g418RbT0).count()));
        if (g415Census && g415Mark == kG415Marks)
            g415NoteStages(b, depthKey, g415Marks, g415ReadRows,
                           g415Window.valid ? g415Window.rows : 0,
                           g417UploadRowsThis);
        if (fusedDepthReadback != nullptr)
        {
            fusedDepthReadback->resize(static_cast<size_t>(b.fbW) * depthReadRows);
            glReadPixels(0, depthReadY, b.fbW, depthReadRows,
                         GL_DEPTH_COMPONENT, GL_FLOAT, fusedDepthReadback->data());
        }
        if (g257RealDiagnostic)
        {
            GLenum fboReadError = GL_NO_ERROR;
            unsigned fboReadErrorCount = 0;
            g257DrainErrors(fboReadError, fboReadErrorCount);
            size_t directFboBad = 0;
            for (size_t i = 0; i < b.readback.size(); ++i)
                if (b.readback[i] != m_g257ImageAfter[i])
                    ++directFboBad;
            ++m_g257RealBatches;
            std::fprintf(stderr,
                         "[G257:real] n=%llu fbp=0x%x draws=%zu changed=%zu/%zu coverage=%zu/%zu "
                         "directFboBad=%zu preErr=0x%x/%u postErr=0x%x/%u fboErr=0x%x/%u\n",
                         static_cast<unsigned long long>(m_g257RealBatches), b.fbp, b.draws.size(),
                         m_g257Changed, b.readback.size(), m_g257Covered, b.readback.size(),
                         directFboBad, m_g257PreError, m_g257PreErrorCount,
                         m_g257PostError, m_g257PostErrorCount,
                         fboReadError, fboReadErrorCount);
            m_g257ImageAfter.clear();
        }
        if (guestDepth)
        {
            // Keep the upload-safe post-state: a later glTexSubImage2D on a guest depth texture
            // must not target storage still attached to the bound FBO.
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
        }
        // G362 census (default OFF, DC2_G362_CENSUS=1): how much of the textured legacy path is
        // actually served by the guarded fetch. Periodic aggregate, not an early-capped print.
        {
            static const bool s_g362Census = g158EnvFlag("DC2_G362_CENSUS");
            if (s_g362Census)
            {
                static uint64_t s_next = 0;
                const uint64_t total = m_g362GuardDraws + m_g362PlainDraws;
                if (total >= s_next)
                {
                    s_next = total + 20000u;
                    std::fprintf(stderr, "[G362:pt] guarded=%llu plain=%llu\n",
                                 static_cast<unsigned long long>(m_g362GuardDraws),
                                 static_cast<unsigned long long>(m_g362PlainDraws));
                }
            }
        }
        return true;
    }

    GLFns m_f;
    G178GLExt m_e;
    GLuint m_prog = 0, m_progExact = 0, m_g281Prog = 0, m_g309Prog = 0;
    GLuint m_g541NullProg = 0, m_g541StateTex = 0;
    GLint m_locG541KickCount = -1;
    bool m_g541Available = false;
    std::weak_ptr<G541NullBatch> m_g541QueuedBatch;
    std::atomic<uint64_t> m_g541Submitted{0ull};
    std::atomic<uint64_t> m_g541Executed{0ull};
    std::atomic<uint64_t> m_g541Batches{0ull};
    std::atomic<uint64_t> m_g541MaxBatch{0ull};
    std::atomic<uint64_t> m_g541Rejected{0ull};
    std::atomic<uint64_t> m_g541HighWater{0ull};
    uint64_t m_g541NextReport = 65536ull; // GL-worker only
    GLuint m_g545Prog = 0, m_g545CommandTex = 0, m_g545PayloadTex = 0;
    GLuint m_g545WorkspaceTex = 0;
    GLint m_locG545CommandCount = -1;
    uint32_t m_g545CommandRows = 0u, m_g545PayloadRows = 0u;
    bool m_g545Available = false;
    std::atomic<bool> m_g545Disabled{false};
    std::weak_ptr<G545LiveBatch> m_g545QueuedBatch;
    std::atomic<uint64_t> m_g545Submitted{0ull};
    std::atomic<uint64_t> m_g545Executed{0ull};
    std::atomic<uint64_t> m_g545Rejected{0ull};
    std::atomic<uint64_t> m_g545HighWater{0ull};
    uint64_t m_g545Batches = 0ull, m_g545MaxBatch = 0ull;
    uint64_t m_g545CheckInterval = 16384ull, m_g545NextCheck = 16384ull;
    uint64_t m_g545Readbacks = 0ull, m_g545Verified = 0ull;
    GLuint m_g338Prog = 0;
    GLint m_locG338SrcH = -1, m_locG338Ta0 = -1, m_locG338Aem = -1;
    GLuint m_vao = 0, m_vbo = 0;
    GLuint m_g281MapTex = 0, m_g281ClutTex = 0, m_g281Fbo = 0;
    GLuint m_g309MapTex = 0, m_g309BaseTex = 0;
    GLint m_locFbSize = -1, m_locMode = -1, m_locLinear = -1, m_locFogCol = -1;
    GLint m_locWrapU = -1, m_locWrapV = -1;
    GLint m_locExactFbSize = -1, m_locExactMode = -1, m_locExactLinear = -1;
    GLint m_locExactWrapU = -1, m_locExactWrapV = -1;
    GLint m_locUvOriginX = -1, m_locUvOriginY = -1;
    GLint m_locUvRoundU = -1, m_locUvRoundV = -1;
    std::map<uint32_t, FboEntry> m_fbos;
    std::unordered_map<uint64_t, DepthEntry> m_depths;
    std::unordered_map<uint64_t, TexEntry> m_texCache;
    size_t m_texBytes = 0;
    uint64_t m_batchCounter = 0;
    uint64_t m_g257RealBatches = 0;
    uint64_t m_g258TraceBatches = 0;
    // G362 census: textured legacy-path draws taking the guarded fixed-point point fetch vs the
    // float floor()/texture() path. Reported by [G362:pt] under DC2_G362_CENSUS=1.
    uint64_t m_g362GuardDraws = 0;
    uint64_t m_g362PlainDraws = 0;
    std::vector<uint32_t> m_g257ImageAfter;
    size_t m_g257Changed = 0, m_g257Covered = 0;
    GLenum m_g257PreError = GL_NO_ERROR, m_g257PostError = GL_NO_ERROR;
    unsigned m_g257PreErrorCount = 0, m_g257PostErrorCount = 0;

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cvWork;
    // G447: monotone submit counter — a lock-free hint the worker spins on before sleeping.
    std::atomic<uint64_t> m_submitSeq{0ull};
    std::condition_variable m_cvReady;
    std::deque<QueueItem> m_queue;
    bool m_stop = false;
    bool m_started = false;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};

    struct AsyncSlot
    {
        G178Batch batch;
        std::future<bool> future;
    };
    // G452: stable deque storage keeps QueueItem::batch pointers valid while up to eight packets
    // are in flight. Legacy G305 still drains at every flush and therefore uses this as one slot.
    std::deque<AsyncSlot> m_asyncSlots;
    G178Batch m_asyncRecycle;

    std::mutex m_residentMtx;
    std::unordered_set<uint64_t> m_resident;
    uint64_t m_g299Total = 0u;
    uint64_t m_g299Count[4]{};
    uint64_t m_g299QueueNs[4]{};
    uint64_t m_g299WorkNs[4]{};
    uint64_t m_g299PacketBytes[4]{};
    uint64_t m_g299Draws[4]{};
    uint64_t m_g324Total = 0u;
    uint64_t m_g324Count[3]{};
    uint64_t m_g324QueueNs[3]{};
    uint64_t m_g324WorkNs[3]{};
    // G415: worker-thread-only stage census state (see g415NoteStages).
    std::map<uint64_t, G415Stage> m_g415Stages;
    uint64_t m_g415Total = 0u;
    uint64_t m_g415StatN = 0u;
    // G417 diagnostic-only framebuffer-upload split (worker thread only).
    std::map<uint64_t, G417UploadStage> m_g417Uploads;
    uint64_t m_g417UploadTotal = 0u;
    // G418 diagnostic-only colour-readback drain/transfer split (worker thread only).
    std::map<uint32_t, G418ReadEdge> m_g418Edges;
    uint64_t m_g418Total = 0u;
    // G415 FBO geometry recycling counters (worker thread only).
    uint64_t m_g415FboCreates = 0u;
    uint64_t m_g415FboParks = 0u;
    uint64_t m_g415FboRevives = 0u;
    // G420 narrow-readback mechanism probe: inert destination-side write target (worker thread
    // only, allocated only when DC2_G419_AB=narrowtouch is armed).
    std::vector<uint32_t> m_g420TouchScratch;
};
} // namespace

// G332: global-scope accessor (external linkage) so ps2_gif_arbiter.cpp's frameDrain print can read
// the per-type backend GL-busy accumulators. Anon-namespace names above remain file-visible here.
void g332_backend_snapshot(uint64_t nsOut[4], uint64_t cntOut[4])
{
    for (int i = 0; i < 4; ++i)
    {
        nsOut[i] = g_g332BackendNs[i].load(std::memory_order_relaxed);
        cntOut[i] = g_g332BackendCount[i].load(std::memory_order_relaxed);
    }
}

// G453: global-scope accessor for the 30-frame GS-worker boundary report.
void g453_backend_other_snapshot(uint64_t nsOut[4], uint64_t cntOut[4])
{
    for (int i = 0; i < 4; ++i)
    {
        nsOut[i] = g_g453BackendOtherNs[i].load(std::memory_order_relaxed);
        cntOut[i] = g_g453BackendOtherCount[i].load(std::memory_order_relaxed);
    }
}

// G494: global-scope accessor for the driver-round-trip census, read from the same 30-frame GS-worker
// boundary that prints [G453:other]. Returns the site count so the printer never has to hard-code it.
int g494_backend_sync_snapshot(uint64_t nsOut[], uint64_t nOut[], uint64_t badOut[], int cap,
                               const char *const **namesOut)
{
    const int n = cap < kG494SyncSites ? cap : kG494SyncSites;
    for (int i = 0; i < n; ++i)
    {
        nsOut[i] = g_g494SyncNs[i].load(std::memory_order_relaxed);
        nOut[i] = g_g494SyncN[i].load(std::memory_order_relaxed);
        badOut[i] = g_g494SyncBad[i].load(std::memory_order_relaxed);
    }
    if (namesOut != nullptr)
        *namesOut = kG494SyncName;
    return n;
}
