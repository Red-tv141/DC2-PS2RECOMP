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

static bool g496GlCacheDefaultOn()
{
    static const bool on = (std::getenv("DC2_G496_NO_GLCACHE") == nullptr);
    return on;
}

static bool g496GlStatOn()
{
    static const bool on = (std::getenv("DC2_G496_GLSTAT") != nullptr);
    return on;
}

// Call classes for the census.
enum G496GlCls
{
    kG496GlScissor = 0,
    kG496GlDepth = 1,
    kG496GlBlend = 2,
    kG496GlBindTex = 3,
    kG496GlTexParam = 4,
    kG496GlUniform = 5,
    kG496GlClsCount = 6
};

static const char *const kG496GlClsName[kG496GlClsCount] = {
    "scissor", "depth", "blend", "bindtex", "texparam", "uniform"};

// GL-worker thread only, like every other resource in this backend.
static uint64_t g_g496GlIssued[kG496GlClsCount];
static uint64_t g_g496GlElided[kG496GlClsCount];
static uint64_t g_g496GlLastIssued[kG496GlClsCount];
static uint64_t g_g496GlLastElided[kG496GlClsCount];
static uint64_t g_g496GlFrames = 0ull, g_g496GlLastFrames = 0ull;

static inline void g496GlNote(int cls, bool issued)
{
    if (!g496GlStatOn())
        return;
    if (issued)
        ++g_g496GlIssued[cls];
    else
        ++g_g496GlElided[cls];
}

// Per-frame windowed report, driven by the same GL-worker frame clock as the calibration census.
static void g496GlStatReport()
{
    const double frames = static_cast<double>(g_g496GlFrames - g_g496GlLastFrames);
    if (frames <= 0.0)
        return;
    char line[512];
    int off = std::snprintf(line, sizeof(line), "[G496:gl] frames=%.0f", frames);
    uint64_t totI = 0ull, totE = 0ull;
    for (int i = 0; i < kG496GlClsCount; ++i)
    {
        const uint64_t di = g_g496GlIssued[i] - g_g496GlLastIssued[i];
        const uint64_t de = g_g496GlElided[i] - g_g496GlLastElided[i];
        totI += di;
        totE += de;
        if ((di + de) > 0ull && off < static_cast<int>(sizeof(line)) - 1)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.0f/%.0f",
                                 kG496GlClsName[i], static_cast<double>(di) / frames,
                                 static_cast<double>(di + de) / frames);
        g_g496GlLastIssued[i] = g_g496GlIssued[i];
        g_g496GlLastElided[i] = g_g496GlElided[i];
    }
    g_g496GlLastFrames = g_g496GlFrames;
    std::fprintf(stderr, "%s | issued/f=%.0f of %.0f (elided %.1f%%)\n", line,
                 static_cast<double>(totI) / frames,
                 static_cast<double>(totI + totE) / frames,
                 (totI + totE) > 0ull ? 100.0 * static_cast<double>(totE) /
                                            static_cast<double>(totI + totE)
                                      : 0.0);
}

static inline void g496GlNoteFrame()
{
    if (!g496GlStatOn())
        return;
    if ((++g_g496GlFrames % 600ull) == 0ull)
        g496GlStatReport();
}

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
