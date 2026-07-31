#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// G419: shared arm register for the within-process randomized A/B instrument. Defined in
// ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc; the tick lives in the GS front-end
// (ps2_gs_rasterizer_parts/g419_ab_instrument.inc), which owns the contract. Declared at global
// scope because the backend consumers below sit inside an anonymous namespace.
extern std::atomic<uint32_t> g_g419AbArm;
extern std::atomic<uint32_t> g_g419AbUsed;

namespace
{
bool g158EnvFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' &&
           std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 && std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

// G189 (2026-07-10): opt-OUT check for the same-named var — true only if NAME is explicitly
// set to a falsy value ("0"/"false"/"off"). Used to promote a lever to default-on while keeping
// its existing env var as a kill switch (set it to "0" to disable).
bool g158EnvDisabled(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr &&
           (value[0] == '\0' || std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
            std::strcmp(value, "OFF") == 0);
}

// G178 promoted to DEFAULT-ON (2026-07-10): title/costume/dungeon-3D route-verified (G178-G188),
// no known correctness regression (G179's route-coverage bugs fixed). Kill via
// DC2_G178_NO_GPU=1 or DC2_G178_GPU=0.
bool g178DefaultOn()
{
    return !g158EnvFlag("DC2_G178_NO_GPU") && !g158EnvDisabled("DC2_G178_GPU");
}

bool g299ProfileOn()
{
    static const bool on = g158EnvFlag("DC2_G299_PROFILE");
    return on;
}

// G415: per-fbp stage census for the G299 command-zero submit class (a batch that renders and
// then reads its color target back synchronously). Splits the backend's measured work into
// setup / draw-loop / colour-readback so the 2.25 ms per-occurrence cost can be attributed to a
// real edge instead of guessed. Behaviour-pure, default-off.
bool g415CensusOn()
{
    static const bool on = g158EnvFlag("DC2_G415_CENSUS");
    return on;
}

// G415 counters for the narrowed colour readback (default-off print, behaviour-pure).
bool g415StatOn()
{
    static const bool on = g158EnvFlag("DC2_G415_STAT");
    return on;
}

// G415 FBO geometry recycling. `m_fbos` is keyed by target, so DC2's fbp=0x139 — written at both
// fbw=8 (512-wide) and fbw=2 (128-wide) — destroys and reallocates a 512x512 RGBA8 colour texture
// plus a 512x512 depth renderbuffer on every width flip. The recycled path parks the outgoing
// geometry instead of deleting it and revives it on the flip back, re-establishing the identical
// fresh-create post-state (attachments re-bound, texture parameters reset, colour+depth cleared).
// No key, ownership, residency, FIFO order, or publication boundary changes.
// G415 PROMOTED to default-ON (2026-07-30): lean MAP-0 3x3 measured 66.060 ms/frame against
// 68.032 ms for its same-binary control with non-overlapping run-mean ranges (-1.972 ms / -2.90%),
// matching the 2.09 ms/frame the stage census attributed to fbp=0x139 FBO churn. Revive reproduces
// the fresh-create post-state exactly, so the change is bit-exact by construction.
// Kill with DC2_G415_NO_FBO_RECYCLE=1 or DC2_G415_FBO_RECYCLE=0.
bool g415FboRecycleOn()
{
    static const bool on = !g158EnvFlag("DC2_G415_NO_FBO_RECYCLE") &&
                           !g158EnvDisabled("DC2_G415_FBO_RECYCLE");
    return on;
}

// G415 fence attribution probe. Inserts a glFinish immediately BEFORE the colour readback so the
// census can separate "drain of previously queued GL work" from "this readback's own transfer".
// Diagnostic only: it adds a stall, so it must never run inside a timing arm.
bool g415FenceProbeOn()
{
    static const bool on = g415CensusOn() && g158EnvFlag("DC2_G415_FENCE_PROBE");
    return on;
}

// G415 narrowed colour readback. The front-end consumes only the batch's own GS row window
// (rowLo..rowHi) out of a full-height readback buffer; every row outside it is copied out of the
// FBO and then discarded. When the front-end publishes an exact window through
// g415_backend_set_color_window(), read back only those GL rows into their existing offsets in
// the same full-height buffer. No GS ownership, FIFO order, or publication boundary moves.
// G419: is this backend lever the one the within-process A/B instrument is alternating? Cached, so
// the shipping path costs one static-guard load per readback batch (once per frame) and no getenv.
bool g419NarrowArmSelected()
{
    static const bool on = [] {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr &&
               (std::strcmp(value, "narrow") == 0 || std::strcmp(value, "all") == 0);
    }();
    return on;
}

// G419 ballast repeats for THIS lever (DC2_G419_BALLAST). In ballast mode both buckets run the
// control form and the candidate bucket re-reads the same rect N extra times, which is idempotent
// (identical bytes into the same destination) and therefore measures only how much of a known added
// cost at this exact site reaches the frame.
int g419NarrowBallastReps()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G419_BALLAST");
        if (value == nullptr)
            return 0;
        int parsed = std::atoi(value);
        return parsed < 0 ? 0 : (parsed > 16 ? 16 : parsed);
    }();
    if (n <= 0 || !g419NarrowArmSelected())
        return 0;
    return g_g419AbArm.load(std::memory_order_relaxed) != 0u ? n : 0;
}

// G419 PROMOTED to default-ON (2026-07-30). G415 built this arm, proved it exact, measured the row
// saving at 0.127 ms/frame and shelved it as "below the timing floor". G418's within-process
// randomized instrument lowered that floor, and G419's A/B over 6,000 MAP-0 frames measured
// -0.332 ms/frame (blocked -0.429, se 0.145, t = -2.96, 8/10 windows favouring) - larger than the
// transfer saving alone, see the fix log's open discrepancy.
// Kill with DC2_G415_NO_NARROW_READBACK=1 or DC2_G415_NARROW_READBACK=0.
bool g415NarrowReadbackOn()
{
    static const bool on = !g158EnvFlag("DC2_G415_NO_NARROW_READBACK") &&
                           !g158EnvDisabled("DC2_G415_NARROW_READBACK");
    if (!g419NarrowArmSelected())
        return on;
    // Instrument armed: the per-frame arm decides, and the consumer records which bucket actually
    // ran so the tick can charge that frame period to it (see g419_ab_instrument.inc).
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    static const bool ballast = [] {
        const char *value = std::getenv("DC2_G419_BALLAST");
        return value != nullptr && std::atoi(value) > 0;
    }();
    return ballast ? false : (arm != 0u);
}

// G420 narrow-readback MECHANISM PROBE (DC2_G419_AB=narrowtouch, diagnostic only, default-off).
//
// G419 promoted the narrowed colour readback on a measured -0.332 ms/frame (blocked -0.429) that
// is 3-4x its accounted mechanism: 96 rows x 1.32 us/row = 0.127 ms of hardware-rate transfer,
// scaled by the site's measured 0.82 critical-path sensitivity, predicts about 0.104 ms. The
// leading hypothesis is that the missing part is DESTINATION-side: the full read also WRITES those
// 96 rows (196 KiB) into the readback buffer, through the driver's staging copy, and the consumer
// then discards every one of them.
//
// This probe measures exactly that term. Both buckets run the promoted NARROW read; the candidate
// bucket additionally writes 196 KiB - the same volume the full read's discarded rows carry - into
// an inert scratch buffer. That adds the destination-side traffic WITHOUT adding any GL transfer,
// any GL state, or any observable byte, so:
//
//     narrowtouch delta ~= (narrow delta) - (transfer term)   => the discrepancy is destination-side
//     narrowtouch delta ~= 0                                   => it is NOT, and the mechanism is
//                                                                 elsewhere (look at the GL side)
//
// Exact by construction: the scratch buffer is backend-private and is never read.
bool g420NarrowTouchSelected()
{
    static const bool on = [] {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "narrowtouch") == 0;
    }();
    return on;
}

// Returns true when THIS frame's bucket should perform the extra destination-side write. Must be
// called unconditionally at the site: the instrument charges a frame period to the arm a consumer
// reported, so a call skipped by a short-circuit drops the interval instead of charging it (G419).
bool g420NarrowTouchArm()
{
    if (!g420NarrowTouchSelected())
        return false;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    return arm != 0u;
}

// G420 narrow-readback SHAPE probe (DC2_G420_NARROW_ROWS=<n>, diagnostic only, default-off).
//
// With the destination-side hypothesis refuted (see g420NarrowTouchArm) and the forced-finish edge
// split reproducing only 0.123 ms of the narrow lever's 0.33-0.43 ms payoff, one discriminator is
// left: is the extra cost PROPORTIONAL to the rows read, or a STEP the full-height read pays for
// covering the entire attachment (a driver full-surface resolve/staging path that a sub-rect read
// avoids)?
//
// This widens the narrow read to exactly <n> rows. Any n >= the published window is EXACT: the read
// still covers every row the consumer reads, into the same offsets of the same full-height buffer,
// and rows outside the consumed window are discarded either way. n=511 vs the 512-row full read is
// therefore the decisive pair — proportional predicts ~1/96th of the payoff, a step predicts nearly
// all of it.
int g420NarrowRowsOverride()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G420_NARROW_ROWS");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(4096, std::atoi(value)));
    }();
    return n;
}

// G417 framebuffer-upload edge attribution. Diagnostic only: the backend inserts a finish
// immediately before and after glTexSubImage2D so prior deferred work, CPU submission, and upload
// completion are measured separately. The finishes perturb scheduling, so this must never be armed
// in a lean timing or correctness arm.
bool g417ProfileOn()
{
    static const bool on = g158EnvFlag("DC2_G417_PROFILE");
    return on;
}

// G418 colour-readback edge attribution. Splits the one readback batch per frame into the drain of
// GL work queued by earlier skip-readback batches (a glFinish immediately before the readback) and
// the readback's own transfer/completion. Diagnostic only: the finish is a deliberate stall, so it
// must never be armed inside a timing arm. It is independent of DC2_G415_CENSUS so the split can be
// taken without the nine-mark stage census riding along.
bool g418ProfileOn()
{
    static const bool on = g158EnvFlag("DC2_G418_PROFILE");
    return on;
}

// G418 early command kick. A skip-readback batch leaves its GL work queued and nothing in the frame
// fences until the one colour-readback batch, which then pays that whole backlog as drain (G415
// measured 1.119 ms drain + 0.850 ms transfer). Kicking each skip-readback batch lets the driver
// start executing it while the front-end prepares the next batch, so the readback finds less
// outstanding work. glFlush has no effect on results, ordering, ownership, or any GL state: it only
// guarantees that already-issued commands complete in finite time.
bool g418EarlyKickOn()
{
    // G445: PROMOTED to default-ON. G418 left this opt-in after a cross-run block read it neutral,
    // but that block was taken while VU1 bound the frame and every GS-resident lever was multiplied
    // by a sensitivity near zero. With the GS worker the pole (G444/G445: gsslow 0.84, vu1slow 0.05)
    // the same code is worth -6.3 ms/f, measured twice in the within-process paired instrument
    // (DC2_G419_AB=earlykick, t = -118 and -154). Rollback: DC2_G418_NO_EARLY_KICK=1.
    static const bool on = !g158EnvFlag("DC2_G418_NO_EARLY_KICK");
    return on;
}

// G445: the same decision, but consulted through the within-process randomized A/B instrument when
// DC2_G419_AB=earlykick is armed. The env form above was only ever gated by a cross-run block taken
// while VU1 bound the frame; this host's run-level drift (~1.4x within one session) is several times
// the lever, so the paired within-process form is the only admissible gate. -1 means "not the
// selected lever" and the env decision stands unchanged, so the shipping path is untouched.
// (g445KickArm is declared at global scope in the parent ps2_gs_gpu_raster.cpp — these parts are
// compiled inside an anonymous namespace, so a declaration here would name a different symbol.)
bool g445EarlyKickActive()
{
    const int arm = g445KickArm();
    if (arm < 0)
        return g418EarlyKickOn();
    return arm != 0;
}

// G445 extension: the same kick for the copy/composite/upload backend items. Default OFF — it is a
// probe until it is measured. DC2_G445_KICK_ALL=1 forces it on; DC2_G419_AB=kickall gates it.
bool g445KickAllOn()
{
    static const bool on = g158EnvFlag("DC2_G445_KICK_ALL");
    return on;
}
bool g445KickAllActive()
{
    const int arm = g445KickAllArm();
    if (arm < 0)
        return g445KickAllOn();
    return arm != 0;
}

// --- Minimal GL>1.1 loader. Microsoft's <GL/gl.h> only declares GL 1.1; every entry point used
// here beyond that is resolved lazily via wglGetProcAddress on first use (no glad/GLEW
// dependency -- keeps this file self-contained and avoids pulling raylib.h in here at all,
// which sidesteps raylib's known macro collisions with <windows.h> types like Rectangle). ---
typedef char GLcharLocal;
typedef ptrdiff_t GLsizeiptrLocal;

typedef void(APIENTRY *PFNGENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFNFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *PFNCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void(APIENTRY *PFNDELETEFRAMEBUFFERS)(GLsizei, const GLuint *);
typedef GLuint(APIENTRY *PFNCREATESHADER)(GLenum);
typedef void(APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const GLcharLocal *const *, const GLint *);
typedef void(APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void(APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei *, GLcharLocal *);
typedef GLuint(APIENTRY *PFNCREATEPROGRAM)(void);
typedef void(APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void(APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void(APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNGETPROGRAMINFOLOG)(GLuint, GLsizei, GLsizei *, GLcharLocal *);
typedef void(APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef void(APIENTRY *PFNDELETEPROGRAM)(GLuint);
typedef void(APIENTRY *PFNDELETESHADER)(GLuint);
typedef void(APIENTRY *PFNGENVERTEXARRAYS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNBINDVERTEXARRAY)(GLuint);
typedef void(APIENTRY *PFNDELETEVERTEXARRAYS)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFNGENBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFNBUFFERDATA)(GLenum, GLsizeiptrLocal, const void *, GLenum);
typedef void(APIENTRY *PFNDELETEBUFFERS)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFNVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void(APIENTRY *PFNENABLEVERTEXATTRIBARRAY)(GLuint);
typedef GLint(APIENTRY *PFNGETUNIFORMLOCATION)(GLuint, const GLcharLocal *);
typedef void(APIENTRY *PFNUNIFORM1I)(GLint, GLint);
typedef void(APIENTRY *PFNACTIVETEXTURE)(GLenum);

struct GLFns
{
    PFNGENFRAMEBUFFERS glGenFramebuffers_ = nullptr;
    PFNBINDFRAMEBUFFER glBindFramebuffer_ = nullptr;
    PFNFRAMEBUFFERTEXTURE2D glFramebufferTexture2D_ = nullptr;
    PFNCHECKFRAMEBUFFERSTATUS glCheckFramebufferStatus_ = nullptr;
    PFNDELETEFRAMEBUFFERS glDeleteFramebuffers_ = nullptr;
    PFNCREATESHADER glCreateShader_ = nullptr;
    PFNSHADERSOURCE glShaderSource_ = nullptr;
    PFNCOMPILESHADER glCompileShader_ = nullptr;
    PFNGETSHADERIV glGetShaderiv_ = nullptr;
    PFNGETSHADERINFOLOG glGetShaderInfoLog_ = nullptr;
    PFNCREATEPROGRAM glCreateProgram_ = nullptr;
    PFNATTACHSHADER glAttachShader_ = nullptr;
    PFNLINKPROGRAM glLinkProgram_ = nullptr;
    PFNGETPROGRAMIV glGetProgramiv_ = nullptr;
    PFNGETPROGRAMINFOLOG glGetProgramInfoLog_ = nullptr;
    PFNUSEPROGRAM glUseProgram_ = nullptr;
    PFNDELETEPROGRAM glDeleteProgram_ = nullptr;
    PFNDELETESHADER glDeleteShader_ = nullptr;
    PFNGENVERTEXARRAYS glGenVertexArrays_ = nullptr;
    PFNBINDVERTEXARRAY glBindVertexArray_ = nullptr;
    PFNDELETEVERTEXARRAYS glDeleteVertexArrays_ = nullptr;
    PFNGENBUFFERS glGenBuffers_ = nullptr;
    PFNBINDBUFFER glBindBuffer_ = nullptr;
    PFNBUFFERDATA glBufferData_ = nullptr;
    PFNDELETEBUFFERS glDeleteBuffers_ = nullptr;
    PFNVERTEXATTRIBPOINTER glVertexAttribPointer_ = nullptr;
    PFNENABLEVERTEXATTRIBARRAY glEnableVertexAttribArray_ = nullptr;
    PFNGETUNIFORMLOCATION glGetUniformLocation_ = nullptr;
    PFNUNIFORM1I glUniform1i_ = nullptr;
    PFNACTIVETEXTURE glActiveTexture_ = nullptr;
};

template <typename T>
bool loadProc(T &fn, const char *name)
{
    fn = reinterpret_cast<T>(reinterpret_cast<void *>(wglGetProcAddress(name)));
    return fn != nullptr;
}

bool g158LoadGL(GLFns &f)
{
    bool ok = true;
    ok &= loadProc(f.glGenFramebuffers_, "glGenFramebuffers");
    ok &= loadProc(f.glBindFramebuffer_, "glBindFramebuffer");
    ok &= loadProc(f.glFramebufferTexture2D_, "glFramebufferTexture2D");
    ok &= loadProc(f.glCheckFramebufferStatus_, "glCheckFramebufferStatus");
    ok &= loadProc(f.glDeleteFramebuffers_, "glDeleteFramebuffers");
    ok &= loadProc(f.glCreateShader_, "glCreateShader");
    ok &= loadProc(f.glShaderSource_, "glShaderSource");
    ok &= loadProc(f.glCompileShader_, "glCompileShader");
    ok &= loadProc(f.glGetShaderiv_, "glGetShaderiv");
    ok &= loadProc(f.glGetShaderInfoLog_, "glGetShaderInfoLog");
    ok &= loadProc(f.glCreateProgram_, "glCreateProgram");
    ok &= loadProc(f.glAttachShader_, "glAttachShader");
    ok &= loadProc(f.glLinkProgram_, "glLinkProgram");
    ok &= loadProc(f.glGetProgramiv_, "glGetProgramiv");
    ok &= loadProc(f.glGetProgramInfoLog_, "glGetProgramInfoLog");
    ok &= loadProc(f.glUseProgram_, "glUseProgram");
    ok &= loadProc(f.glDeleteProgram_, "glDeleteProgram");
    ok &= loadProc(f.glDeleteShader_, "glDeleteShader");
    ok &= loadProc(f.glGenVertexArrays_, "glGenVertexArrays");
    ok &= loadProc(f.glBindVertexArray_, "glBindVertexArray");
    ok &= loadProc(f.glDeleteVertexArrays_, "glDeleteVertexArrays");
    ok &= loadProc(f.glGenBuffers_, "glGenBuffers");
    ok &= loadProc(f.glBindBuffer_, "glBindBuffer");
    ok &= loadProc(f.glBufferData_, "glBufferData");
    ok &= loadProc(f.glDeleteBuffers_, "glDeleteBuffers");
    ok &= loadProc(f.glVertexAttribPointer_, "glVertexAttribPointer");
    ok &= loadProc(f.glEnableVertexAttribArray_, "glEnableVertexAttribArray");
    ok &= loadProc(f.glGetUniformLocation_, "glGetUniformLocation");
    ok &= loadProc(f.glUniform1i_, "glUniform1i");
    ok &= loadProc(f.glActiveTexture_, "glActiveTexture");
    return ok;
}

// ABI-stable GL enum values not declared in the GL1.1-only <GL/gl.h>; namespaced (GL158_*) so
// they can never collide with a real declaration if one is ever pulled in elsewhere.
constexpr GLenum GL158_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL158_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL158_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL158_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL158_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL158_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL158_LINK_STATUS = 0x8B82;
constexpr GLenum GL158_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL158_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL158_STATIC_DRAW = 0x88E4;
constexpr GLenum GL158_TEXTURE0 = 0x84C0;
constexpr GLenum GL158_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL158_RGBA8 = 0x8058;

bool g158CompileShader(GLFns &f, GLenum type, const char *src, GLuint &outShader, std::string &log)
{
    GLuint sh = f.glCreateShader_(type);
    f.glShaderSource_(sh, 1, &src, nullptr);
    f.glCompileShader_(sh);
    GLint status = 0;
    f.glGetShaderiv_(sh, GL158_COMPILE_STATUS, &status);
    if (!status)
    {
        GLint len = 0;
        f.glGetShaderiv_(sh, GL158_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1);
        f.glGetShaderInfoLog_(sh, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
        log.assign(buf.data());
        f.glDeleteShader_(sh);
        return false;
    }
    outShader = sh;
    return true;
}
} // namespace

bool g158_gpu_raster_enabled()
{
    static const bool on = g158EnvFlag("DC2_G158_GPURASTER");
    return on;
}

// G159: raylib's main-thread HGLRC/HDC, captured once (main thread, at init, before the MTGS
// worker thread exists -- no reader can race the write). Consumed by g158RunWorkerContextTest()
// to create a SECOND, SHARED context on the worker thread. Plain statics (not atomics): the
// write-once-before-any-reader-exists ordering makes this safe without synchronization, same
// reasoning as G150's other one-shot init statics.
HGLRC s_g158MainRC = nullptr;
HDC s_g158MainDC = nullptr;

// Shared core of the synthetic FBO/shader/readback round-trip proof, parameterized only by the
// log-line tag so the G158a main-thread self-test and the G159 worker-thread variant stay
// byte-identical in behavior (and the G158a fix-log's recorded log strings/verification stay
// valid unchanged). Assumes a GL context is already current on the calling thread.
bool g158RunSelfTestCore(const char *tag)
{
    GLFns f;
    if (!g158LoadGL(f))
    {
        std::fprintf(stderr, "[%s] FAIL: could not resolve required GL entry points\n", tag);
        return false;
    }

    static const char *kVS =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUV;\n"
        "out vec2 vUV;\n"
        "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char *kFS =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D uTex;\n"
        "void main(){ FragColor = texture(uTex, vUV); }\n";

    std::string log;
    GLuint vs = 0, fs = 0;
    if (!g158CompileShader(f, GL158_VERTEX_SHADER, kVS, vs, log))
    {
        std::fprintf(stderr, "[%s] FAIL: vertex shader: %s\n", tag, log.c_str());
        return false;
    }
    if (!g158CompileShader(f, GL158_FRAGMENT_SHADER, kFS, fs, log))
    {
        std::fprintf(stderr, "[%s] FAIL: fragment shader: %s\n", tag, log.c_str());
        f.glDeleteShader_(vs);
        return false;
    }
    GLuint prog = f.glCreateProgram_();
    f.glAttachShader_(prog, vs);
    f.glAttachShader_(prog, fs);
    f.glLinkProgram_(prog);
    GLint linkStatus = 0;
    f.glGetProgramiv_(prog, GL158_LINK_STATUS, &linkStatus);
    if (!linkStatus)
    {
        GLint len = 0;
        f.glGetProgramiv_(prog, GL158_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1);
        f.glGetProgramInfoLog_(prog, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
        std::fprintf(stderr, "[%s] FAIL: program link: %s\n", tag, buf.data());
        f.glDeleteShader_(vs);
        f.glDeleteShader_(fs);
        f.glDeleteProgram_(prog);
        return false;
    }
    f.glDeleteShader_(vs);
    f.glDeleteShader_(fs);

    // 2x2 known-value source texture (row 0 = v=0, matching glTexImage2D convention).
    const uint8_t texels[2 * 2 * 4] = {
        255, 0, 0, 255, 0, 255, 0, 255,     // row0: red, green
        0, 0, 255, 255, 255, 255, 255, 255, // row1: blue, white
    };
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL158_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL158_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);

    // Oversized full-viewport triangle (classic "big tri" trick): UV stays affinely correct
    // across the visible [-1,1]^2 region regardless of triangle size.
    const float verts[] = {
        // x,    y,    u,   v
        -1.f, -1.f, 0.f, 0.f,
        3.f, -1.f, 2.f, 0.f,
        -1.f, 3.f, 0.f, 2.f,
    };
    GLuint vao = 0, vbo = 0;
    f.glGenVertexArrays_(1, &vao);
    f.glBindVertexArray_(vao);
    f.glGenBuffers_(1, &vbo);
    f.glBindBuffer_(GL158_ARRAY_BUFFER, vbo);
    f.glBufferData_(GL158_ARRAY_BUFFER, sizeof(verts), verts, GL158_STATIC_DRAW);
    f.glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(0));
    f.glEnableVertexAttribArray_(0);
    f.glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void *>(2 * sizeof(float)));
    f.glEnableVertexAttribArray_(1);

    const int W = 4, H = 4;
    GLuint colorTex = 0;
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    GLuint fbo = 0;
    f.glGenFramebuffers_(1, &fbo);
    f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo);
    f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    const GLenum fboStatus = f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);

    bool pass = false;
    std::vector<uint8_t> pixels;
    if (fboStatus != GL158_FRAMEBUFFER_COMPLETE)
    {
        std::fprintf(stderr, "[%s] FAIL: FBO incomplete (status=0x%x)\n",
                     tag, static_cast<unsigned>(fboStatus));
    }
    else
    {
        GLint prevViewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        glViewport(0, 0, W, H);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);

        f.glUseProgram_(prog);
        f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        f.glUniform1i_(f.glGetUniformLocation_(prog, "uTex"), 0);
        f.glBindVertexArray_(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        pixels.resize(static_cast<size_t>(W) * H * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Expected: quadrant-based exact match to the 2x2 source texel array (nearest sample,
        // no blend -> deterministic, bit-exact by construction, not a tolerance comparison).
        uint8_t expected[4 * 4][4];
        for (int py = 0; py < H; ++py)
        {
            for (int px = 0; px < W; ++px)
            {
                const int col = (px < W / 2) ? 0 : 1;
                const int row = (py < H / 2) ? 0 : 1;
                const int srcIdx = (row * 2 + col) * 4;
                uint8_t *dst = expected[py * W + px];
                dst[0] = texels[srcIdx + 0];
                dst[1] = texels[srcIdx + 1];
                dst[2] = texels[srcIdx + 2];
                dst[3] = texels[srcIdx + 3];
            }
        }
        int mismatches = 0;
        for (int i = 0; i < W * H; ++i)
        {
            const uint8_t *got = &pixels[static_cast<size_t>(i) * 4];
            const uint8_t *exp = expected[i];
            if (got[0] != exp[0] || got[1] != exp[1] || got[2] != exp[2] || got[3] != exp[3])
                ++mismatches;
        }
        pass = (mismatches == 0);
        std::fprintf(stderr, "[%s] %s: %dx%d readback, %d/%d pixel mismatches\n",
                     tag, pass ? "PASS" : "FAIL", W, H, mismatches, W * H);

        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }

    // Leave the (single, shared) GL context exactly as raylib expects to find it -- this runs
    // once at init before raylib's own frame loop starts, but be a good citizen regardless.
    f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
    f.glUseProgram_(0);
    f.glBindVertexArray_(0);
    f.glBindBuffer_(GL158_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    f.glDeleteFramebuffers_(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteTextures(1, &tex);
    f.glDeleteBuffers_(1, &vbo);
    f.glDeleteVertexArrays_(1, &vao);
    f.glDeleteProgram_(prog);

    if (pass && !pixels.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories("D:/ps2r/dc2/captures", ec);
        const std::string ppmPath = std::string("D:/ps2r/dc2/captures/g158_selftest_") + tag + ".ppm";
        std::ofstream out(ppmPath, std::ios::binary);
        if (out)
        {
            out << "P6\n" << W << " " << H << "\n255\n";
            // glReadPixels row 0 = bottom; flip to top-down for a conventional PPM viewer.
            for (int py = H - 1; py >= 0; --py)
                for (int px = 0; px < W; ++px)
                {
                    const uint8_t *p = &pixels[(static_cast<size_t>(py) * W + px) * 4];
                    out.put(static_cast<char>(p[0]));
                    out.put(static_cast<char>(p[1]));
                    out.put(static_cast<char>(p[2]));
                }
        }
    }
    return pass;
}

// G158a stop-condition proof (unchanged behavior/log strings from the original phase): runs
// once, at startup, on the caller's (main) already-current GL context.
void g158RunSelfTest()
{
    if (!g158_gpu_raster_enabled())
        return;

    if (wglGetCurrentContext() == nullptr)
    {
        std::fprintf(stderr, "[G158:selftest] SKIP: no current GL context\n");
        return;
    }

    g158RunSelfTestCore("G158:selftest");
}

// G159 (empirical, default OFF via the same DC2_G158_GPURASTER master switch): capture raylib's
// main-thread HGLRC/HDC once, at init, BEFORE the MTGS worker thread can exist (the worker only
// starts lazily on the first GIF window submission -- see G150Mtgs::startLocked in
// ps2_gif_arbiter.cpp). This is a plain write with no reader yet, so no synchronization is
// needed. A true no-op when the env var is off (both statics stay null; the worker-side call
// below is itself gated on the same flag).
void g158CaptureMainContext()
{
    // G178 also needs the captured main context (its persistent backend thread shares against
    // it, same as G160/G162) — capture when EITHER master switch is set. G178 is default-on.
    static const bool s_g178on = g178DefaultOn();
    if (!g158_gpu_raster_enabled() && !s_g178on)
        return;
    s_g158MainRC = wglGetCurrentContext();
    s_g158MainDC = wglGetCurrentDC();
    if (!s_g158MainRC || !s_g158MainDC)
        std::fprintf(stderr, "[G158:workerctx] WARN: no current GL context to capture at init\n");
}

// G159: the empirical question this increment answers -- does a SECOND GL context, created on
// the MTGS worker thread and wglShareLists'd against raylib's main context, actually work? Named
// risk (plans/phase-G158-fix-log.md, G158a fix-log "Remaining Blocker"): wglShareLists's MSDN
// contract says neither context may be current on ANY thread at call time, but s_g158MainRC IS
// continuously current on the main/present thread by the time the worker thread first runs (the
// worker starts lazily on the first GIF window, well after raylib's frame loop is live) -- so
// this is exactly the empirical case the plan flagged as unresolved-by-construction. Runs ONCE
// (guarded), on the worker thread only, still fully isolated from real geometry: this only
// proves/refutes the plumbing, same synthetic-triangle proof as G158a, from the new location.
void g158RunWorkerContextTest()
{
    if (!g158_gpu_raster_enabled())
        return;

    static std::atomic<bool> s_attempted{false};
    if (s_attempted.exchange(true, std::memory_order_relaxed))
        return;

    if (!s_g158MainRC || !s_g158MainDC)
    {
        std::fprintf(stderr, "[G158:workerctx] SKIP: no captured main context\n");
        return;
    }

    HGLRC workerRC = wglCreateContext(s_g158MainDC);
    if (!workerRC)
    {
        std::fprintf(stderr, "[G158:workerctx] FAIL: wglCreateContext failed, GetLastError=%lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    if (!wglShareLists(s_g158MainRC, workerRC))
    {
        std::fprintf(stderr,
                     "[G158:workerctx] FAIL: wglShareLists failed, GetLastError=%lu -- likely the "
                     "MSDN 'context must not be current on any thread' restriction (main context is "
                     "live on the present thread); fallback per the plan is a dedicated GPU thread "
                     "fed by a queue instead of sharing onto the worker thread\n",
                     static_cast<unsigned long>(GetLastError()));
        wglDeleteContext(workerRC);
        return;
    }

    if (!wglMakeCurrent(s_g158MainDC, workerRC))
    {
        std::fprintf(stderr, "[G158:workerctx] FAIL: wglMakeCurrent failed on worker thread, GetLastError=%lu\n",
                     static_cast<unsigned long>(GetLastError()));
        wglDeleteContext(workerRC);
        return;
    }

    std::fprintf(stderr, "[G158:workerctx] PASS: shared context created and made current on the MTGS worker thread\n");

    // Left current on the worker thread for the rest of the process lifetime (this thread never
    // calls any raylib/rlgl function, so there is no conflict) and intentionally not deleted --
    // matches the forward-looking G158b design (the worker would keep submitting GPU draws
    // through this same context for the rest of the run); a single process-lifetime GL context is
    // not a leak in any practical sense here.
    g158RunSelfTestCore("G158:workerctx:selftest");
}

// G160: the fallback the G159 fix-log named. G159 proved wglShareLists fails when the source
// context is CURRENTLY current on another thread (ERROR_BUSY, per MSDN). The fix implied by that
// contract: make it NOT current, share, then restore it -- so this releases the main context on
// the MAIN thread (wglMakeCurrent(null,null)), creates+shares+makes-current a NEW context on a
// brand-new dedicated GPU thread, waits (one-time startup stall, main thread blocks via
// std::thread::join) for that thread to finish its one-shot setup + the same synthetic self-test
// proof, then restores the main context on the main thread so raylib's subsequent present loop is
// unaffected. Bounded scope, same as G158a/G159: proves the plumbing only -- the GPU thread exits
// after its one-shot test in this phase; keeping it alive and feeding it a real draw queue is
// G158b-equivalent follow-up work, not attempted here.
void g158StartDedicatedGpuThread()
{
    if (!g158_gpu_raster_enabled())
        return;

    if (!s_g158MainRC || !s_g158MainDC)
    {
        std::fprintf(stderr, "[G158:gputhread] SKIP: no captured main context\n");
        return;
    }

    if (!wglMakeCurrent(nullptr, nullptr))
    {
        std::fprintf(stderr, "[G158:gputhread] FAIL: could not release main context, GetLastError=%lu -- "
                             "aborting (main context left untouched, still current)\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    std::thread gpuThread([]() {
        HGLRC gpuRC = wglCreateContext(s_g158MainDC);
        if (!gpuRC)
        {
            std::fprintf(stderr, "[G158:gputhread] FAIL: wglCreateContext failed, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return;
        }
        if (!wglShareLists(s_g158MainRC, gpuRC))
        {
            std::fprintf(stderr, "[G158:gputhread] FAIL: wglShareLists failed, GetLastError=%lu -- the "
                                 "release-before-share fix did not resolve it either\n",
                         static_cast<unsigned long>(GetLastError()));
            wglDeleteContext(gpuRC);
            return;
        }
        if (!wglMakeCurrent(s_g158MainDC, gpuRC))
        {
            std::fprintf(stderr, "[G158:gputhread] FAIL: wglMakeCurrent failed on the dedicated GPU thread, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            wglDeleteContext(gpuRC);
            return;
        }
        std::fprintf(stderr, "[G158:gputhread] PASS: shared context created and made current on a "
                             "dedicated GPU thread (release-before-share)\n");
        g158RunSelfTestCore("G158:gputhread:selftest");
        // One-shot proof only in this phase -- release + delete before the thread exits so the
        // context doesn't outlive it in an undefined state; a persistent queue-fed version is
        // future (G158b-equivalent) work, not this phase's scope.
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gpuRC);
    });
    gpuThread.join();

    if (!wglMakeCurrent(s_g158MainDC, s_g158MainRC))
    {
        std::fprintf(stderr, "[G158:gputhread] FAIL: could not restore main context on the main thread, "
                             "GetLastError=%lu -- raylib's subsequent rendering will be broken\n",
                     static_cast<unsigned long>(GetLastError()));
    }
}
