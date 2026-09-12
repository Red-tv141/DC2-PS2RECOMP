// ===== G496b: REDUNDANT GL STATE ELISION in the render-batch draw loop =========================
//
// WHY THIS EXISTS — the arithmetic that named it.
//
// The G496 derivative probe (g496_gpu_ballast.inc) established two things that do not fit together
// under the arc's inherited model:
//
//   1. The frame IS sensitive to GPU time: injecting 1.08 ms/f of extra fragment work costs the
//      frame ~0.85 ms/f (sensitivity 0.7-0.9 over five paired runs). GPU time is on the critical
//      path essentially 1:1.
//   2. There is almost no GPU time to remove. Replaying the frame's ENTIRE geometry — 34 batches,
//      849 draws, ~31,800 vertices, at FULL scissor, with a blend that forces a real framebuffer
//      read-modify-write — costs the GPU **1.08 ms**. Re-issuing all 20 full-screen 512x512 passes
//      (9 composites + 10 T8 views + 1 CT24 view) costs **0.19 ms**, i.e. 9.5 us each.
//
// So the GPU's own execution for a whole frame is on the order of ~1.3 ms, inside a 22.8 ms frame
// whose pole thread spends 9.3 ms/f BLOCKED on that same GL worker. `[G495:gpu]`'s per-item spans
// (rend=8.7, comp=1.0, t8=0.53) are therefore ~85% idle-waiting-for-commands, exactly as G495 §5
// warned — and the G495 roadmap's "19 full-screen passes ~ 1.5 ms/f of GPU timeline" candidate is
// really 0.19 ms/f, which is below anything worth building.
//
// What is left in the gap between "1.3 ms of GPU execution" and "9.3 ms/f of blocking wait" is the
// DRIVER: the NVIDIA threaded driver's own worker translating GL commands into a hardware command
// stream. G495 §3a measured the readback drain rising with the number of DRAWS queued ahead
// (0 draws -> 0.33 ms, 410 draws -> 1.65 ms) and read that as the GPU catching up. The ballast says
// those 410 draws are worth ~0.5 ms of GPU execution at most, so the drain is mostly the command
// stream being PREPARED, not executed. That is the same mechanism G494 already found and priced at
// 4.6 ms/f in a different guise: work handed to the driver's worker that produces no output byte.
//
// This part removes it directly. The draw loop issues, per draw:
//
//     glScissor · glEnable/glDisable(DEPTH_TEST) · glDepthFunc · glDepthMask ·
//     glEnable/glDisable(BLEND) · glBlendEquationSeparate · glBlendFuncSeparate ·
//     glBindTexture · glTexParameteri x4 · glUniform1i x1..8 · glDrawArrays
//
// ~13-18 calls for every one of the frame's 849 draws, and the great majority set the value that is
// already there — consecutive draws in a batch share a scissor rect, a depth mode, a blend mode and
// very often a texture, and a texture's four sampler parameters are OBJECT state that survives
// every rebind, so re-setting them on every use is pure waste. (glTexParameteri is the worst of
// them: on several drivers it can force a texture-object revalidation.)
//
// EXACTNESS. A call is skipped only when the cache proves GL already holds that exact value, so the
// GL state vector after the loop is bit-identical either way, and so is every rendered byte. The
// cache is written on EVERY path — including when elision is disabled — so it can never go stale
// and the A/B arm can flip without the candidate inheriting a wrong record.
//
// SCOPE / INVALIDATION.
//   * Non-texture state (scissor, depth, blend, bound texture, the m_prog uniforms) is reset at the
//     top of every draw loop: other backend items (composite, T8/CT24 view, copy, writecolor) run
//     between batches and change all of it. Within a batch — ~25 draws — the redundancy is real.
//   * Texture SAMPLER state lives on the texture object, so it is cached per TexEntry and stays
//     valid across batches. A fresh/evicted entry records 0 (= unknown), which forces a real call.
//     glTexImage2D does not disturb sampler parameters, so a re-upload keeps the record valid. The
//     ONE site outside this loop that parameterises an m_texCache entry is the CT24 view's output
//     (prepareCt24ViewOnWorker); it records what it set. Every other glTexParameteri in this backend
//     targets an FboEntry colour texture or a dedicated G309 texture, never a TexEntry.
//
// FLAGS
//   DC2_G419_AB=glstate  paired gate (thread-level: it changes how much the GL worker hands the
//                        driver, so it takes the G438 block hold like every other thread-level arm)
//   DC2_G496_NO_GLCACHE=1  rollback of the promoted default
//   DC2_G496_GLSTAT=1      census: issued vs elided, per call class
// ==============================================================================================

static const bool s_g674G496GlCacheDefaultOn =
    (std::getenv("DC2_G496_NO_GLCACHE") == nullptr);
// ⭐ G738: ONE flag arms the counters AND the reporter. `DC2_G738_GLCENSUS` gates the cold report
// in ps2_g738_glcall.inc; if it did not also arm the counters here, a run with only that flag set
// would print zeros — which is G704's rule verbatim ("a flag that needs a second, unrelated flag to
// speak reads exactly like a flag that measured nothing"). `DC2_G496_GLSTAT` is kept as an alias so
// existing scripts still arm the counting half.
static const bool s_g674G496GlStatOn = (std::getenv("DC2_G496_GLSTAT") != nullptr) ||
                                       (std::getenv("DC2_G738_GLCENSUS") != nullptr);
static DC2_G674_HOT_FLAG_INLINE bool g496GlCacheDefaultOn()
{
    return s_g674G496GlCacheDefaultOn;
}
static DC2_G674_HOT_FLAG_INLINE bool g496GlStatOn() { return s_g674G496GlStatOn; }

// Call classes for the census.
//
// ⭐⭐⭐ G738: the class list now covers the WHOLE stream this loop issues, not just the six that
// the G496 cache happens to elide. `glDrawArrays` had no class at all, which is why every figure
// ever quoted from `[G496:gl]` described state calls only. The numbering of 0..5 is unchanged so
// the existing cache sites below are byte-identical; `kG496GlClsCount` follows
// `kG738GlClsCount`, and `ps2_g738_glcall_api.inc` is the single definition of both.
//
// ⛔ That header is included by ps2_gs_gpu_raster.cpp at GLOBAL scope, above the anonymous
// namespace these parts sit inside. It must not be included from here: the parts are inside
// `namespace { }`, so a first inclusion at this point would give `G738GlSnapshot` and
// `g738_glcall_snapshot` INTERNAL linkage and the cold reporter's call would fail to link — the
// same C7631/LNK2001 trap G734 documented for `g713_exec_busy_ns`.
enum G496GlCls
{
    kG496GlScissor = kG738GlScissor,
    kG496GlDepth = kG738GlDepth,
    kG496GlBlend = kG738GlBlend,
    kG496GlBindTex = kG738GlBindTex,
    kG496GlTexParam = kG738GlTexParam,
    kG496GlUniform = kG738GlUniform,
    kG496GlClsCount = kG738GlClsCount
};

// GL-worker thread only, like every other resource in this backend — but READ from the EE thread
// once per `[G154:perf]` window (g738_glcall_snapshot), so these are relaxed atomics rather than
// plain counters. A relaxed `fetch_add` is a locked op, which is why the census stays opt-in and
// why Rule 63 forbids arming it in the same run as the ballast (see ps2_g738_glcall_api.inc).
static std::atomic<uint64_t> g_g496GlIssued[kG496GlClsCount];
static std::atomic<uint64_t> g_g496GlElided[kG496GlClsCount];
// G738: the ballast's own call counts — the denominator that turns a frame delta into ns/call.
static std::atomic<uint64_t> g_g738DrawBallastCalls{0ull};
static std::atomic<uint64_t> g_g738StateBallastCalls{0ull};
// G738: workload identity for the census. `batches` is render batches whose draw loop ran and
// `drawsSeen` is G178Draw entries visited, so a reader can divide calls by draws without needing
// `[G559:runs]` and without assuming the two instruments share a denominator.
static std::atomic<uint64_t> g_g738Batches{0ull};
static std::atomic<uint64_t> g_g738DrawsSeen{0ull};

static inline void g496GlNote(int cls, bool issued)
{
    if (!g496GlStatOn())
        return;
    if (issued)
        g_g496GlIssued[cls].fetch_add(1ull, std::memory_order_relaxed);
    else
        g_g496GlElided[cls].fetch_add(1ull, std::memory_order_relaxed);
}

// Bulk form, for a site that issues a known number of calls in one statement group (the four
// `glTexParameteri` of the producer-surface branch, the three `glColorMask` of the RTT second
// pass, the six-call batch prologue). One atomic per site instead of one per call.
static inline void g496GlNoteN(int cls, uint64_t issued, uint64_t elided)
{
    if (!g496GlStatOn())
        return;
    if (issued != 0ull)
        g_g496GlIssued[cls].fetch_add(issued, std::memory_order_relaxed);
    if (elided != 0ull)
        g_g496GlElided[cls].fetch_add(elided, std::memory_order_relaxed);
}

// ⛔⛔⛔ G738 REMOVED `g496GlStatReport()` AND `g496GlNoteFrame()`.
//
// They were the defect. `g496GlNoteFrame()` was called once per render batch with
// `!b.skipReadback` and treated that as a frame; measured on the shipped G732 image it is **8.12
// rendered frames**, so every rate the reporter printed was ~8x too small — and it printed
// `frames=600` beside them, which reads like a real denominator. The report now lives in
// `ps2_g738_glcall.inc`, is called from the `[G154:perf]` window on the EE thread, and divides by
// that window's own rendered-frame count. Nothing on this thread decides what a frame is.
//
// A successor must not re-add a frame clock here: there is no frame signal on the GL worker. The
// one that looked like a frame signal was a readback batch.

// ===== G738: THE DIRECT PRICE OF A GL CALL =====================================================
//
// G629's law says price a mechanism before building against it, and the two numbers this project
// has for GL call issue are both inadmissible (a stage timer, and a counter with a readback-batch
// denominator — see ps2_g738_glcall_api.inc). These two ballasts give an EVENT price instead.
//
//   DC2_G738_DRAWBALLAST=N   after each physical draw, issue N extra `glDrawArrays(GL_TRIANGLES,
//                            0, 0)`. A zero-vertex draw emits no primitive, rasterizes nothing and
//                            writes no byte, so the GPU cost is zero and the output is unchanged —
//                            what it buys is N more trips through the driver's draw-validation and
//                            command-encode path, which is exactly the quantity a draw-call
//                            reduction would remove.
//   DC2_G738_STATEBALLAST=N  the same for redundant STATE: N extra `glDepthMask(GL_TRUE)` calls
//                            with the value the state already holds. Inert by construction (the
//                            draw loop sets depth state per draw through the G496 cache, and the
//                            ballast runs AFTER the draw), and it prices the other 8,482 calls.
//
// ⛔ Both are PRICES, not levers: they can only make the frame slower. ns/call =
// (Δframe_ms × 1e6) / (N × calls/f), read against the census's own ballast counters so the
// denominator is measured rather than assumed.
//
// ⛔ Do NOT arm DC2_G738_GLCENSUS in a ballast run (Rule 63): the census's cost scales with the
// call count the ballast changes. The ballast counters below are the exception — they are one
// atomic per BALLAST GROUP, not per call, and they are what make the price computable.
static const int s_g738DrawBallast = [] {
    const char *v = std::getenv("DC2_G738_DRAWBALLAST");
    if (v == nullptr || v[0] == '\0')
        return 0;
    const int n = std::atoi(v);
    return n < 0 ? 0 : (n > 64 ? 64 : n);
}();
static const int s_g738StateBallast = [] {
    const char *v = std::getenv("DC2_G738_STATEBALLAST");
    if (v == nullptr || v[0] == '\0')
        return 0;
    const int n = std::atoi(v);
    return n < 0 ? 0 : (n > 64 ? 64 : n);
}();
// ⭐⭐⭐ DC2_G738_UNIBALLAST=N — THE ONLY ONE OF THE THREE THAT CANNOT BE ELIDED.
//
// ⛔ MEASURED, and this is why it exists. The first two ballasts both price ~0: 106,400 extra
// zero-count `glDrawArrays` per frame (32x the frame's entire draw stream) moved `dungeon1` by
// **-1.44 ms/f**, and 69,920 extra redundant `glDepthMask` by ~+8 ns/call. Both arms set a value
// the driver already holds, or draw a primitive count of zero, so a compare-and-skip in nvoglv64
// makes the arm a no-op and the "price" is the price of nothing. A flat response across a 4x
// change in N is the tell, and it appeared exactly as this file's own caveat predicted.
//
// This form alternates `uMode` between the real value and the real value with BIT 30 set, an even
// number of times, so that:
//   * every call is a genuine state change and no driver can compare-and-skip it;
//   * the LAST write restores the real value, so the G496 uniform cache's record still describes
//     GL's real state and the next draw is unaffected;
//   * bit 30 is unused by every shader in this backend (uMode's live fields stop at bit 21, and
//     the G560 build masks `uMode & 65535`), and the ballast runs AFTER the draw, so even the
//     intermediate value cannot reach a fragment.
// It prices the `uniform` class, which the corrected census puts at 4,484 issued/frame — the
// single largest class in the real stream.
static const int s_g738UniBallast = [] {
    const char *v = std::getenv("DC2_G738_UNIBALLAST");
    if (v == nullptr || v[0] == '\0')
        return 0;
    int n = std::atoi(v);
    if (n < 0)
        n = 0;
    if (n > 64)
        n = 64;
    return n & ~1; // even, so the final write restores the real value
}();
static DC2_G674_HOT_FLAG_INLINE int g738DrawBallast() { return s_g738DrawBallast; }
static DC2_G674_HOT_FLAG_INLINE int g738StateBallast() { return s_g738StateBallast; }

// The non-elidable uniform ballast. Templated on the uniform functor for the same reason
// `g496Uniform1i` is: `m_f.glUniform1i_` is a member of the backend, not reachable from here.
// `mode` is written LAST, so GL and the G496 cache agree when this returns.
template <typename UniFn>
static DC2_G674_HOT_FLAG_INLINE void g738UniBallastIssue(UniFn &&uniFn, GLint loc, int mode)
{
    const int n = s_g738UniBallast;
    if (n == 0 || loc < 0)
        return;
    const int flipped = mode | (1 << 30);
    for (int i = 0; i < n; i += 2)
    {
        uniFn(loc, flipped);
        uniFn(loc, mode);
    }
    g_g738StateBallastCalls.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

// Workload identity for the census, one atomic per batch / per draw-loop iteration. Armed by the
// census flag OR either ballast, because a ballast run needs `drawsSeen` to compute ns/call and
// must be able to get it without arming the per-call census (Rule 63).
static DC2_G674_HOT_FLAG_INLINE bool g738IdentityOn()
{
    return g496GlStatOn() || s_g738DrawBallast != 0 || s_g738StateBallast != 0;
}

// One extra trip through the driver's draw path per call, with nothing to draw.
//
// ⛔ WHY A ZERO-COUNT DRAW IS THE RIGHT BALLAST. `glDrawArrays(GL_TRIANGLES, 0, 0)` still
// validates the bound program, VAO, framebuffer completeness and every piece of dirty state, and
// still encodes a command; it assembles no primitive, so it rasterizes nothing, writes no byte and
// cannot change a pixel. That makes the arm exact by construction (the workload invariant is the
// census's own `drawsSeen`, which the ballast does not touch) while isolating the CPU-side cost
// this project has never measured directly.
//
// ⚠️ CAVEAT a successor must check before quoting the price: if a driver short-circuits count==0
// before the encode, this UNDER-states the per-draw cost, making any conclusion drawn from it
// conservative in the safe direction. The tell is a near-zero ns/call at large N.
static DC2_G674_HOT_FLAG_INLINE void g738DrawBallastIssue()
{
    const int n = s_g738DrawBallast;
    if (n == 0)
        return;
    for (int i = 0; i < n; ++i)
        glDrawArrays(GL_TRIANGLES, 0, 0);
    g_g738DrawBallastCalls.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

// The state ballast needs `g_g496Gl`, which is declared below, so it is defined at the bottom of
// this file. Both are declared here so the call sites read in one place.
static DC2_G674_HOT_FLAG_INLINE void g738StateBallastIssue();

// --- the cache ---------------------------------------------------------------------------------
struct G496GlState
{
    int scX = -1, scY = -1, scW = -1, scH = -1;
    int depthTest = -1;   // -1 unknown, 0 disabled, 1 enabled
    int depthFunc = -1;
    int depthMask = -1;
    int blendOn = -1;
    int eqRgb = -1, eqA = -1;
    int sRgb = -1, dRgb = -1, sA = -1, dA = -1;
    long long boundTex = -1;
    // m_prog uniform shadow. Index order must match kG496U* below.
    int uni[10] = {};
    bool uniSet[10] = {};
};

enum
{
    kG496UMode = 0,
    kG496ULinear = 1,
    kG496UWrapU = 2,
    kG496UWrapV = 3,
    kG496UFogCol = 4,
    kG496UOriginX = 5,
    kG496UOriginY = 6,
    kG496URoundU = 7,
    kG496URoundV = 8,
    kG496UFlipV = 9 // G624: srcFbp draws sample a V-flipped producer FBO
};

static G496GlState g_g496Gl;

// Reset at the top of every draw loop: other backend items run between batches and own all of this
// state. Texture SAMPLER records are per-TexEntry and deliberately survive.
static inline void g496GlReset()
{
    g_g496Gl = G496GlState();
}

static inline void g496Scissor(int x, int y, int w, int h, bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (cacheOn && s.scX == x && s.scY == y && s.scW == w && s.scH == h)
    {
        g496GlNote(kG496GlScissor, false);
        return;
    }
    glScissor(x, y, w, h);
    s.scX = x;
    s.scY = y;
    s.scW = w;
    s.scH = h;
    g496GlNote(kG496GlScissor, true);
}

// Depth test disabled + writes masked (the depthFunc==0 arm of the draw loop).
static inline void g496DepthOff(bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (!cacheOn || s.depthTest != 0)
    {
        glDisable(GL_DEPTH_TEST);
        s.depthTest = 0;
        g496GlNote(kG496GlDepth, true);
    }
    else
        g496GlNote(kG496GlDepth, false);
    if (!cacheOn || s.depthMask != 0)
    {
        glDepthMask(GL_FALSE);
        s.depthMask = 0;
        g496GlNote(kG496GlDepth, true);
    }
    else
        g496GlNote(kG496GlDepth, false);
}

static inline void g496DepthOn(GLenum func, bool write, bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (!cacheOn || s.depthTest != 1)
    {
        glEnable(GL_DEPTH_TEST);
        s.depthTest = 1;
        g496GlNote(kG496GlDepth, true);
    }
    else
        g496GlNote(kG496GlDepth, false);
    if (!cacheOn || s.depthFunc != static_cast<int>(func))
    {
        glDepthFunc(func);
        s.depthFunc = static_cast<int>(func);
        g496GlNote(kG496GlDepth, true);
    }
    else
        g496GlNote(kG496GlDepth, false);
    const int want = write ? 1 : 0;
    if (!cacheOn || s.depthMask != want)
    {
        glDepthMask(write ? GL_TRUE : GL_FALSE);
        s.depthMask = want;
        g496GlNote(kG496GlDepth, true);
    }
    else
        g496GlNote(kG496GlDepth, false);
}

static inline void g496BlendOff(bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (cacheOn && s.blendOn == 0)
    {
        g496GlNote(kG496GlBlend, false);
        return;
    }
    glDisable(GL_BLEND);
    s.blendOn = 0;
    g496GlNote(kG496GlBlend, true);
}

// Enable + equation + function. The equation and function calls are cached independently of the
// enable, because a run of blended draws normally shares all three.
template <typename EqFn, typename FnFn>
static inline void g496BlendOn(EqFn &&eqFn, FnFn &&fnFn, GLenum eqRgb, GLenum eqA, GLenum sRgb,
                               GLenum dRgb, GLenum sA, GLenum dA, bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (!cacheOn || s.blendOn != 1)
    {
        glEnable(GL_BLEND);
        s.blendOn = 1;
        g496GlNote(kG496GlBlend, true);
    }
    else
        g496GlNote(kG496GlBlend, false);
    if (!cacheOn || s.eqRgb != static_cast<int>(eqRgb) || s.eqA != static_cast<int>(eqA))
    {
        eqFn(eqRgb, eqA);
        s.eqRgb = static_cast<int>(eqRgb);
        s.eqA = static_cast<int>(eqA);
        g496GlNote(kG496GlBlend, true);
    }
    else
        g496GlNote(kG496GlBlend, false);
    if (!cacheOn || s.sRgb != static_cast<int>(sRgb) || s.dRgb != static_cast<int>(dRgb) ||
        s.sA != static_cast<int>(sA) || s.dA != static_cast<int>(dA))
    {
        fnFn(sRgb, dRgb, sA, dA);
        s.sRgb = static_cast<int>(sRgb);
        s.dRgb = static_cast<int>(dRgb);
        s.sA = static_cast<int>(sA);
        s.dA = static_cast<int>(dA);
        g496GlNote(kG496GlBlend, true);
    }
    else
        g496GlNote(kG496GlBlend, false);
}

static inline void g496BindTex(GLuint id, bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (cacheOn && s.boundTex == static_cast<long long>(id))
    {
        g496GlNote(kG496GlBindTex, false);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, id);
    s.boundTex = static_cast<long long>(id);
    g496GlNote(kG496GlBindTex, true);
}

// Texture-object sampler state. `cur*` are the TexEntry's own records — object state, so they stay
// valid across batches and across rebinds. A value of 0 means "never set by us" and always issues.
static inline void g496TexFilter(int &curMin, int &curMag, GLint minF, GLint magF, bool cacheOn)
{
    if (!cacheOn || curMin != static_cast<int>(minF))
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
        curMin = static_cast<int>(minF);
        g496GlNote(kG496GlTexParam, true);
    }
    else
        g496GlNote(kG496GlTexParam, false);
    if (!cacheOn || curMag != static_cast<int>(magF))
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
        curMag = static_cast<int>(magF);
        g496GlNote(kG496GlTexParam, true);
    }
    else
        g496GlNote(kG496GlTexParam, false);
}

static inline void g496TexWrap(int &curS, int &curT, GLint wrapS, GLint wrapT, bool cacheOn)
{
    if (!cacheOn || curS != static_cast<int>(wrapS))
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
        curS = static_cast<int>(wrapS);
        g496GlNote(kG496GlTexParam, true);
    }
    else
        g496GlNote(kG496GlTexParam, false);
    if (!cacheOn || curT != static_cast<int>(wrapT))
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
        curT = static_cast<int>(wrapT);
        g496GlNote(kG496GlTexParam, true);
    }
    else
        g496GlNote(kG496GlTexParam, false);
}

// m_prog uniform shadow. Valid only inside the draw loop (g496GlReset clears it, and the loop is
// the only place m_prog is the current program with these locations live).
template <typename UniFn>
static inline void g496Uniform1i(UniFn &&uniFn, int slot, GLint loc, int value, bool cacheOn)
{
    G496GlState &s = g_g496Gl;
    if (cacheOn && s.uniSet[slot] && s.uni[slot] == value)
    {
        g496GlNote(kG496GlUniform, false);
        return;
    }
    uniFn(loc, value);
    s.uni[slot] = value;
    s.uniSet[slot] = true;
    g496GlNote(kG496GlUniform, true);
}

// ⭐ G738: the redundant-STATE ballast, defined here because it reads `g_g496Gl`.
//
// `glDepthMask` with the value the state already holds. The driver must marshal the call and the
// value it writes is the value that was there, so the arm is inert: it cannot change a pixel and
// it cannot desynchronise the G496 cache (the cache's `depthWrite` record still describes GL's
// real state afterwards). Issued AFTER the draw, so the draw it follows is unaffected.
//
// This is the price of the OTHER 8,482 calls/frame — the cached-state classes. Divide the frame
// delta by (N x drawsSeen/f) exactly as for the draw ballast.
static DC2_G674_HOT_FLAG_INLINE void g738StateBallastIssue()
{
    const int n = s_g738StateBallast;
    if (n == 0)
        return;
    // -1 is "unknown" in this cache. Re-issuing a guess there would be a real state change, so the
    // ballast abstains and does not count the draw — that keeps the arm inert by construction and
    // keeps the ns/call denominator honest (it counts only calls that were actually issued).
    const int cur = g_g496Gl.depthMask;
    if (cur < 0)
        return;
    const GLboolean v = cur != 0 ? GL_TRUE : GL_FALSE;
    for (int i = 0; i < n; ++i)
        glDepthMask(v);
    g_g738StateBallastCalls.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}
