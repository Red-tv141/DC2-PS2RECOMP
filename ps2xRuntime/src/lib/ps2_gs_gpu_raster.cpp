// G158a: GPU-raster infrastructure prototype (default-off, DC2_G158_GPURASTER=1).
//
// Scope (deliberate, see plans/phase-G158-fix-log.md): prove the FBO + shader + readback
// round-trip on a SYNTHETIC textured triangle, entirely on the main thread, reusing raylib's
// already-current GL context (no second/shared context, no MTGS worker-thread integration yet
// -- those are separate, harder axes deferred to a later increment once this one is proven).
// This file is NOT wired into drawPrimitive()/drawTriangle() in this phase -- it runs once at
// startup, self-contained, and has zero effect on real title geometry or the shipped default
// (DC2_G158_GPURASTER unset -> a single cached boolean read, nothing else executes).
//
// No header edits (matches the G157 precedent: PS2Recomp/ps2xRuntime/include/runtime/
// ps2_gs_gpu.h is pulled into the generated dc2_game MSVC target -- touching it risks a 30+
// hour full rebuild). g158RunSelfTest()/g158_gpu_raster_enabled() are declared as inline
// `extern` forward declarations at their call site (ps2_runtime.cpp), the same idiom already
// used for g150_pipeline_enabled()/g150_mtgs_enabled().

#include "ps2_gs_gpu_lle.h" // G178: private front-end<->backend interface (both build branches)

#if defined(_WIN32) && !defined(PLATFORM_VITA)

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

// ===== G162: persistent GPU de-swizzle+CLUT decode worker for PSMT8 textures =====
//
// G161 measured the signed-off G158 plan's full-raster eligibility gate at 0% real-geometry
// match (every title triangle has blend+alpha-test on; most are PSMT8). This is a narrower,
// lower-risk widening of just the PSM axis: it does NOT touch blend, Z-test, or VRAM-commit at
// all. Instead it accelerates G149's existing, already-bit-exact CPU texture decode (the "11
// whole-texture decodes/frame" G149 measured as a net loss because the CPU nested swizzle+CLUT
// loop was slower than the sparse sampler it replaced) by moving the per-texel GATHER (byte
// fetch + CLUT resolve) onto the GPU. Correctness strategy, deliberately conservative:
//   - The swizzle ADDRESS math is NEVER re-derived in GLSL. `GSMem::AddressP8` (ps2_gs_memory.cpp,
//     G162) is the SAME live `PixelStorageTraits<P8>::Address()` call `GSMem::ReadP8` already uses
//     internally -- called once per texel on the CPU (cheap: pure LUT-indexed arithmetic, no VRAM
//     touch) to build an offset table, which the GPU only GATHERS (texelFetch), never computes.
//   - The CLUT resolve is NEVER re-derived in GLSL either. `GSRasterizer::lookupCLUT` (the same
//     proven function `g149BuildDecoded`'s CPU path already calls) is called up to 256 times on
//     the CPU to build a small RGBA8 palette texture (TEXA already applied, exactly matching what
//     the CPU path would produce per index); the GPU only GATHERS a palette entry, never resolves
//     CLUT format/CSM/CSA/TEXA itself.
//   - The shader is therefore two texelFetch chains, no interpolation (GL_NEAREST, pixel-exact via
//     gl_FragCoord, no UV rounding), no blend, no Z -- the same "GL_NEAREST + no blend = exact by
//     construction" pattern G158a's self-test already established.
//   - Verified per-pixel against the live CPU expression via the EXISTING `DC2_G149_TCVERIFY` A/B
//     (zero new verify code): if this phase's GPU decode is byte-for-byte identical, `bad=0`.
// Threading: EAGERLY starts a persistent worker thread at `PS2Runtime::initialize()` (same safe
// startup point G160 validated -- before raylib's frame loop makes the main context continuously
// current), using G160's confirmed release-before-share pattern, but UNLIKE G160's one-shot proof
// this thread's context is never torn down: it compiles the decode shader once, then blocks on an
// empty work queue (condition_variable, zero CPU while idle) for the process lifetime, draining
// jobs submitted (blocking, via std::future) from the guest/MTGS-worker thread at G144 capture
// time -- the same single-threaded call site G149's existing CPU decode already runs from, so no
// new cross-thread VRAM-read hazard versus the already-shipped G149/G144 design.
struct G162DecodeJob
{
    std::vector<uint32_t> offsets; // texW*texH byte offsets into vram (GSMem::AddressP8, CPU-built)
    std::vector<uint32_t> clut256; // up to 256 RGBA8 entries (lookupCLUT, CPU-built, TEXA-applied)
    // G163: BOUNDED vram slice -- only the row range [vramRowStart, vramRowStart+vramRowCount)
    // of the 2048-wide addressing (derived from the min/max of `offsets`), not the whole 4MB.
    // G162's first cut copied+uploaded the ENTIRE 4MB vram on every decode (~64MB/frame bulk
    // transfer for 8 decodes/frame) and was measured a net loss; this bounds both the CPU-side
    // copy and the GPU upload to what this texture's addressing can actually reach.
    std::vector<uint8_t> vram;
    int vramRowStart = 0;
    int vramRowCount = 0;
    int texW = 0;
    int texH = 0;
};

// G166: one texture's request within a batch (see G162BatchJob below). `atlasY` is assigned by
// the caller (g162DecodeT8Batch) before submit -- the vertical position in the shared offset/
// color atlas this texture's data occupies; `clutRow` is its row in the (256 x N) CLUT atlas.
struct G162BatchSubJob
{
    std::vector<uint32_t> offsets; // texW*texH byte offsets into vram
    int texW = 0;
    int texH = 0;
    int atlasY = 0;
    int clutRow = 0;
};

// G166: a whole frame's worth of T8 decode requests, resolved in ONE cross-thread round-trip
// (one shared VRAM upload bounded to the union of all subs' touched rows, one CLUT atlas upload,
// N offset-texture sub-uploads + N draws into disjoint atlas Y-bands of the SAME persistent
// color attachment, then exactly ONE glReadPixels covering the whole used atlas height) instead
// of one round-trip per texture. Amortizes the draw/readback cross-thread synchronization cost
// G164/G165 isolated as dominant (see plans/phase-G165-fix-log.md) over the whole frame's
// requests rather than paying it ~8x/frame.
struct G162BatchJob
{
    std::vector<G162BatchSubJob> subs;
    std::vector<uint32_t> clutAtlas; // 256 * subs.size() RGBA8 entries, row-major by clutRow
    std::vector<uint8_t> vram;       // bounded to the UNION of all subs' touched rows
    int vramRowStart = 0;
    int vramRowCount = 0;
    int totalHeight = 0; // sum of atlasY extents = height actually used in the atlas
};

namespace
{
constexpr GLenum GL162_R8UI = 0x8232;
constexpr GLenum GL162_R32UI = 0x8236;
constexpr GLenum GL162_RED_INTEGER = 0x8D94;
constexpr int kG162VramTexW = 2048; // 2048*2048*1B = 4MB == GSMem::MEMORY_SIZE, exact fit
constexpr int kG162MaxTexDim = 1024; // G165: fixed max size for the persistent offset/color textures
// G166: batch ALL of one frame's T8 decodes into ONE cross-thread round-trip (G164/G165 isolated
// draw+readback synchronization -- not texture/FBO reallocation -- as the dominant remaining cost;
// batching amortizes that per-decode synchronization over the whole frame's requests instead of
// paying it ~8x/frame). Sub-textures are packed into a vertical atlas of fixed width
// kG162MaxTexDim, height up to kG162AtlasMaxH; up to kG162MaxBatchTex distinct textures/frame.
constexpr int kG162AtlasMaxH = 4096;
constexpr int kG162MaxBatchTex = 64;

// G164 (MEASURE ONLY, DC2_G164_PROFILE=1): per-stage wall-clock breakdown of one GPU decode call,
// to find which stage dominates before writing more "fix" code blind (G163 bounded the VRAM
// upload volume and only partially closed the gap -- the remaining cost is unattributed).
std::atomic<uint64_t> g_g164Calls{0};
std::atomic<uint64_t> g_g164ColorTexAllocNs{0};
std::atomic<uint64_t> g_g164VramUploadNs{0};
std::atomic<uint64_t> g_g164ClutUploadNs{0};
std::atomic<uint64_t> g_g164OffsetUploadNs{0};
std::atomic<uint64_t> g_g164FboBindNs{0};
std::atomic<uint64_t> g_g164DrawNs{0};
std::atomic<uint64_t> g_g164ReadbackNs{0};
std::atomic<uint64_t> g_g164CleanupNs{0};

class G162GpuDecoder
{
public:
    static G162GpuDecoder &instance()
    {
        static G162GpuDecoder d;
        return d;
    }

    // std::thread's destructor calls std::terminate() if the thread is still joinable, and this
    // is a Meyer's singleton (static storage duration) whose destructor runs at process exit --
    // must signal the worker to drain+exit and join it, same shutdown shape as G150Mtgs.
    ~G162GpuDecoder()
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

    // Called once, eagerly, from PS2Runtime::initialize() (main thread, before the frame loop).
    // Mirrors g158StartDedicatedGpuThread()'s release-before-share dance, but the spawned thread's
    // context is kept alive for the process lifetime instead of torn down after a one-shot test.
    void startEager()
    {
        if (m_started)
            return;
        if (!s_g158MainRC || !s_g158MainDC)
        {
            std::fprintf(stderr, "[G162:decoder] SKIP: no captured main context\n");
            return;
        }
        if (!wglMakeCurrent(nullptr, nullptr))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: could not release main context, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return;
        }
        m_started = true;
        m_thread = std::thread([this] { worker(); });
        // Block (one-time startup cost, same class as G160's join()) until the worker has either
        // set up its persistent context or given up, so the main thread never races the restore.
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvReady.wait(lk, [&] { return m_ready || m_failed; });
        }
        if (!wglMakeCurrent(s_g158MainDC, s_g158MainRC))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: could not restore main context on the main thread, "
                                 "GetLastError=%lu -- raylib's subsequent rendering will be broken\n",
                         static_cast<unsigned long>(GetLastError()));
        }
    }

    // Synchronous submit: blocks the calling (guest/MTGS-worker) thread until the GPU thread has
    // decoded this texture. Returns false (caller must fall back to the CPU path) if the decoder
    // never started or its context setup failed.
    bool decode(G162DecodeJob &&job, std::vector<uint32_t> &outPx)
    {
        if (!m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<std::vector<uint32_t>>>();
        std::future<std::vector<uint32_t>> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_queue.push_back({std::move(job), promise});
        }
        m_cvWork.notify_one();
        outPx = fut.get();
        return !outPx.empty();
    }

    // G166: batched submit -- one cross-thread round-trip decodes ALL of `batch.subs`, instead of
    // one round-trip per texture. Returns false (caller falls back per-texture) if the decoder
    // isn't ready or the batch doesn't fit the atlas; on success `outPerTex[i]` holds sub i's
    // texW*texH pixels, same layout `decode()` would have produced for that texture alone.
    bool decodeBatch(G162BatchJob &&batch, std::vector<std::vector<uint32_t>> &outPerTex)
    {
        if (!m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<std::vector<std::vector<uint32_t>>>>();
        std::future<std::vector<std::vector<uint32_t>>> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            QueueItem item;
            item.isBatch = true;
            item.batchJob = std::move(batch);
            item.batchPromise = promise;
            m_queue.push_back(std::move(item));
        }
        m_cvWork.notify_one();
        outPerTex = fut.get();
        return !outPerTex.empty();
    }

    bool isReady() const { return m_ready.load(std::memory_order_acquire); }
    bool isFailed() const { return m_failed.load(std::memory_order_acquire); }

private:
    struct QueueItem
    {
        G162DecodeJob job;
        std::shared_ptr<std::promise<std::vector<uint32_t>>> promise;
        bool isBatch = false;
        G162BatchJob batchJob;
        std::shared_ptr<std::promise<std::vector<std::vector<uint32_t>>>> batchPromise;
    };

    void worker()
    {
        HGLRC rc = wglCreateContext(s_g158MainDC);
        if (!rc || !wglShareLists(s_g158MainRC, rc) || !wglMakeCurrent(s_g158MainDC, rc))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: context setup, GetLastError=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            if (rc) wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        GLFns f;
        if (!g158LoadGL(f))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: could not resolve GL entry points\n");
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        static const char *kVS =
            "#version 330 core\n"
            "layout(location=0) in vec2 aPos;\n"
            "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 FragColor;\n"
            "uniform usampler2D uOffsetTex;\n"
            "uniform usampler2D uVramTex;\n"
            "uniform sampler2D uClutTex;\n"
            "uniform int uVramTexW;\n"
            "uniform int uVramRowOffset;\n" // G163: rows [uVramRowOffset, uVramRowOffset+N) of the
                                            // full addressing were uploaded starting at row 0
            "uniform int uClutRow;\n" // G166: which row of the (possibly multi-texture) CLUT atlas
                                      // this draw's texture uses; 0 for the single-decode path.
            "void main(){\n"
            "    ivec2 xy = ivec2(gl_FragCoord.xy);\n"
            "    uint offset = texelFetch(uOffsetTex, xy, 0).r;\n"
            "    int vw = uVramTexW;\n"
            "    ivec2 vxy = ivec2(int(offset) % vw, int(offset) / vw - uVramRowOffset);\n"
            "    uint idx = texelFetch(uVramTex, vxy, 0).r;\n"
            "    FragColor = texelFetch(uClutTex, ivec2(int(idx), uClutRow), 0);\n"
            "}\n";
        std::string log;
        GLuint vs = 0, fs = 0;
        if (!g158CompileShader(f, GL158_VERTEX_SHADER, kVS, vs, log))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: vertex shader: %s\n", log.c_str());
            m_failed = true; m_cvReady.notify_all(); return;
        }
        if (!g158CompileShader(f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: fragment shader: %s\n", log.c_str());
            f.glDeleteShader_(vs);
            m_failed = true; m_cvReady.notify_all(); return;
        }
        m_prog = f.glCreateProgram_();
        f.glAttachShader_(m_prog, vs);
        f.glAttachShader_(m_prog, fs);
        f.glLinkProgram_(m_prog);
        GLint linkStatus = 0;
        f.glGetProgramiv_(m_prog, GL158_LINK_STATUS, &linkStatus);
        f.glDeleteShader_(vs);
        f.glDeleteShader_(fs);
        if (!linkStatus)
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: program link\n");
            m_failed = true; m_cvReady.notify_all(); return;
        }

        const float verts[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f}; // oversized full-viewport tri
        f.glGenVertexArrays_(1, &m_vao);
        f.glBindVertexArray_(m_vao);
        f.glGenBuffers_(1, &m_vbo);
        f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
        f.glBufferData_(GL158_ARRAY_BUFFER, sizeof(verts), verts, GL158_STATIC_DRAW);
        f.glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        f.glEnableVertexAttribArray_(0);

        glGenTextures(1, &m_vramTex);
        glBindTexture(GL_TEXTURE_2D, m_vramTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL162_R8UI, kG162VramTexW, kG162VramTexW, 0,
                     GL162_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);

        // G166: clutTex is an ATLAS (256 x kG162MaxBatchTex, one row per distinct texture in a
        // batch); row 0 is used unchanged by the single-decode path (runDecode always passes
        // uClutRow=0).
        glGenTextures(1, &m_clutTex);
        glBindTexture(GL_TEXTURE_2D, m_clutTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 256, kG162MaxBatchTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        // G165: offsetTex/colorTex are allocated ONCE here at a fixed max size and updated via
        // glTexSubImage2D per decode (only the sub-region actually used), instead of G162/G163's
        // per-decode glTexImage2D reallocation (profiled at ~135us for offsetTex alone, vs ~20us
        // for vramTex which already used glTexSubImage2D against a reused buffer -- direct
        // evidence the REALLOCATION, not the data volume, was the cost). colorTex is attached to
        // m_fbo ONCE here too (was re-attached + re-validated via glCheckFramebufferStatus_ every
        // decode; both profiled as large AND monotonically growing across a run -- see
        // plans/phase-G164-fix-log.md). G166: both sized to a full vertical ATLAS
        // (kG162MaxTexDim x kG162AtlasMaxH) so a whole frame's T8 decodes can be packed into one
        // draw-set + one shared readback; the single-decode path just uses the top band.
        glGenTextures(1, &m_offsetTex);
        glBindTexture(GL_TEXTURE_2D, m_offsetTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL162_R32UI, kG162MaxTexDim, kG162AtlasMaxH, 0,
                     GL162_RED_INTEGER, GL_UNSIGNED_INT, nullptr);

        glGenTextures(1, &m_colorTex);
        glBindTexture(GL_TEXTURE_2D, m_colorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, kG162MaxTexDim, kG162AtlasMaxH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        f.glGenFramebuffers_(1, &m_fbo);
        f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_fbo);
        f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);
        const GLenum setupFboStatus = f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER);
        f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
        if (setupFboStatus != GL158_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[G162:decoder] FAIL: FBO incomplete at setup (status=0x%x)\n",
                         static_cast<unsigned>(setupFboStatus));
            m_failed = true; m_cvReady.notify_all(); return;
        }

        m_ready = true;
        m_cvReady.notify_all();

        std::unique_lock<std::mutex> lk(m_mtx);
        for (;;)
        {
            m_cvWork.wait(lk, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty())
                break;
            QueueItem item = std::move(m_queue.front());
            m_queue.pop_front();
            lk.unlock();

            if (item.isBatch)
            {
                std::vector<std::vector<uint32_t>> outBatch;
                try
                {
                    outBatch = runDecodeBatch(f, item.batchJob);
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[G162:decoder] batch exception: %s\n", e.what());
                }
                catch (...)
                {
                    std::fprintf(stderr, "[G162:decoder] batch unknown exception\n");
                }
                item.batchPromise->set_value(std::move(outBatch));
            }
            else
            {
                std::vector<uint32_t> outPx;
                try
                {
                    outPx = runDecode(f, item.job);
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[G162:decoder] exception: %s\n", e.what());
                }
                catch (...)
                {
                    std::fprintf(stderr, "[G162:decoder] unknown exception\n");
                }
                item.promise->set_value(std::move(outPx));
            }

            lk.lock();
        }

        f.glDeleteFramebuffers_(1, &m_fbo);
        glDeleteTextures(1, &m_vramTex);
        glDeleteTextures(1, &m_clutTex);
        glDeleteTextures(1, &m_offsetTex);
        glDeleteTextures(1, &m_colorTex);
        f.glDeleteBuffers_(1, &m_vbo);
        f.glDeleteVertexArrays_(1, &m_vao);
        f.glDeleteProgram_(m_prog);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }

    std::vector<uint32_t> runDecode(GLFns &f, const G162DecodeJob &job)
    {
        const int texW = job.texW, texH = job.texH;
        if (texW <= 0 || texH <= 0 || job.offsets.size() != static_cast<size_t>(texW) * texH ||
            job.vramRowCount <= 0 || job.vramRowStart < 0 ||
            job.vramRowStart + job.vramRowCount > kG162VramTexW ||
            job.vram.size() != static_cast<size_t>(job.vramRowCount) * kG162VramTexW)
            return {};

        static const bool s_g164Profile = (std::getenv("DC2_G164_PROFILE") != nullptr);
        using Clock = std::chrono::steady_clock;
        auto elapsedNs = [](Clock::time_point t0) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        };

        // G163: upload only the touched row range, starting at row 0 of the (fixed-size, reused)
        // texture object -- the shader subtracts uVramRowOffset to map back, so rows outside this
        // range simply hold stale/unrelated data from a prior decode, which is harmless because
        // this decode's offset table never indexes outside [vramRowStart, vramRowStart+vramRowCount).
        Clock::time_point t0 = Clock::now();
        glBindTexture(GL_TEXTURE_2D, m_vramTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kG162VramTexW, job.vramRowCount,
                        GL162_RED_INTEGER, GL_UNSIGNED_BYTE, job.vram.data());
        if (s_g164Profile) g_g164VramUploadNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);

        t0 = Clock::now();
        glBindTexture(GL_TEXTURE_2D, m_clutTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, job.clut256.data());
        if (s_g164Profile) g_g164ClutUploadNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);

        // G165: offsetTex is a FIXED kG162MaxTexDim x kG162MaxTexDim buffer (allocated once at
        // worker startup); only update the top-left texW x texH sub-region this decode needs.
        if (texW > kG162MaxTexDim || texH > kG162MaxTexDim)
            return {};
        t0 = Clock::now();
        glBindTexture(GL_TEXTURE_2D, m_offsetTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW, texH, GL162_RED_INTEGER,
                        GL_UNSIGNED_INT, job.offsets.data());
        if (s_g164Profile) g_g164OffsetUploadNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);

        // G165: colorTex + its FBO attachment are set up ONCE at worker startup (see startEager/
        // worker()) -- no per-decode glGenTextures/glFramebufferTexture2D_/
        // glCheckFramebufferStatus_ (profiled as large AND monotonically growing per-decode in
        // G164; see plans/phase-G164-fix-log.md). Just bind the already-complete FBO.
        t0 = Clock::now();
        f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_fbo);
        if (s_g164Profile) g_g164FboBindNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);
        std::vector<uint32_t> outPx;
        {
            GLint prevViewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, prevViewport);
            glViewport(0, 0, texW, texH);

            f.glUseProgram_(m_prog);
            f.glActiveTexture_(GL158_TEXTURE0 + 0);
            glBindTexture(GL_TEXTURE_2D, m_offsetTex);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uOffsetTex"), 0);
            f.glActiveTexture_(GL158_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, m_vramTex);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramTex"), 1);
            f.glActiveTexture_(GL158_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, m_clutTex);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uClutTex"), 2);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramTexW"), kG162VramTexW);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramRowOffset"), job.vramRowStart);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uClutRow"), 0);

            f.glBindVertexArray_(m_vao);
            t0 = Clock::now();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (s_g164Profile)
            {
                glFinish(); // force the draw to complete HERE so drawNs isn't just enqueue time
                g_g164DrawNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);
            }

            outPx.resize(static_cast<size_t>(texW) * texH);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            // Row 0 of this readback = the first scanline processed (gl_FragCoord row 0), which is
            // the SAME row index the offset texture's row 0 was uploaded from (job.offsets[vv=0]
            // row) -- texelFetch is a raw array index on both ends, no implicit y-flip, and the
            // FBO-readback<->gl_FragCoord pairing is internally consistent (same GL window-space
            // convention on both sides). So row r of `outPx` IS texture row vv=r, straight copy.
            t0 = Clock::now();
            glReadPixels(0, 0, texW, texH, GL_RGBA, GL_UNSIGNED_BYTE, outPx.data());
            if (s_g164Profile) g_g164ReadbackNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);

            glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        }

        t0 = Clock::now();
        f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
        f.glUseProgram_(0);
        f.glBindVertexArray_(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (s_g164Profile)
        {
            g_g164CleanupNs.fetch_add(elapsedNs(t0), std::memory_order_relaxed);
            const uint64_t n = g_g164Calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((n % 64u) == 0u)
            {
                std::fprintf(stderr,
                    "[G164:profile] n=%llu avgUs/decode: colorTexAlloc=%.1f vramUpload=%.1f "
                    "clutUpload=%.1f offsetUpload=%.1f fboBind=%.1f draw=%.1f readback=%.1f cleanup=%.1f\n",
                    (unsigned long long)n,
                    g_g164ColorTexAllocNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164VramUploadNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164ClutUploadNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164OffsetUploadNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164FboBindNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164DrawNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164ReadbackNs.load(std::memory_order_relaxed) / 1e3 / n,
                    g_g164CleanupNs.load(std::memory_order_relaxed) / 1e3 / n);
            }
        }
        return outPx;
    }

    // G166: decode ALL of `job.subs` with ONE shared VRAM upload, ONE shared CLUT-atlas upload,
    // N offset-texture sub-uploads + N draws into disjoint Y-bands of the SAME persistent color
    // attachment, and exactly ONE glReadPixels for the whole batch. Splitting each sub's pixels
    // out of the single readback buffer is a straight per-row slice (same "no y-flip, texelFetch
    // is a raw index on both ends" argument as runDecode's single-texture case, applied per band).
    std::vector<std::vector<uint32_t>> runDecodeBatch(GLFns &f, const G162BatchJob &job)
    {
        std::vector<std::vector<uint32_t>> results;
        if (job.subs.empty() || job.totalHeight <= 0 || job.totalHeight > kG162AtlasMaxH ||
            job.vramRowCount <= 0 || job.vramRowStart < 0 ||
            job.vramRowStart + job.vramRowCount > kG162VramTexW ||
            job.vram.size() != static_cast<size_t>(job.vramRowCount) * kG162VramTexW ||
            job.clutAtlas.size() != static_cast<size_t>(job.subs.size()) * 256u)
            return {};
        for (const auto &s : job.subs)
            if (s.texW <= 0 || s.texH <= 0 || s.texW > kG162MaxTexDim ||
                s.atlasY < 0 || s.atlasY + s.texH > kG162AtlasMaxH ||
                s.offsets.size() != static_cast<size_t>(s.texW) * s.texH ||
                s.clutRow < 0 || s.clutRow >= kG162MaxBatchTex)
                return {};

        glBindTexture(GL_TEXTURE_2D, m_vramTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kG162VramTexW, job.vramRowCount,
                        GL162_RED_INTEGER, GL_UNSIGNED_BYTE, job.vram.data());

        glBindTexture(GL_TEXTURE_2D, m_clutTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, static_cast<GLsizei>(job.subs.size()),
                        GL_RGBA, GL_UNSIGNED_BYTE, job.clutAtlas.data());

        f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_fbo);
        GLint prevViewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        f.glUseProgram_(m_prog);
        f.glActiveTexture_(GL158_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, m_offsetTex);
        f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uOffsetTex"), 0);
        f.glActiveTexture_(GL158_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, m_vramTex);
        f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramTex"), 1);
        f.glActiveTexture_(GL158_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, m_clutTex);
        f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uClutTex"), 2);
        f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramTexW"), kG162VramTexW);
        f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uVramRowOffset"), job.vramRowStart);
        f.glBindVertexArray_(m_vao);

        // One offset sub-upload + one draw per texture, each into its own atlas Y-band (glViewport's
        // y-offset shifts gl_FragCoord to already read/write ATLAS-ABSOLUTE coordinates, so no extra
        // local-vs-atlas remapping is needed on either the offset lookup or the color write).
        // MUST reactivate unit 0 first -- the active unit is still 2 (uClutTex) from the uniform
        // setup above, so an unqualified glBindTexture here would silently clobber unit 2's
        // m_clutTex binding with m_offsetTex instead of updating unit 0's (already-correct) one.
        f.glActiveTexture_(GL158_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, m_offsetTex);
        for (const auto &s : job.subs)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, s.atlasY, s.texW, s.texH, GL162_RED_INTEGER,
                            GL_UNSIGNED_INT, s.offsets.data());
            glViewport(0, s.atlasY, s.texW, s.texH);
            f.glUniform1i_(f.glGetUniformLocation_(m_prog, "uClutRow"), s.clutRow);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        std::vector<uint32_t> atlasPx(static_cast<size_t>(kG162MaxTexDim) * job.totalHeight);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, kG162MaxTexDim, job.totalHeight, GL_RGBA, GL_UNSIGNED_BYTE, atlasPx.data());
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        f.glBindFramebuffer_(GL158_FRAMEBUFFER, 0);
        f.glUseProgram_(0);
        f.glBindVertexArray_(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        results.resize(job.subs.size());
        for (size_t i = 0; i < job.subs.size(); ++i)
        {
            const auto &s = job.subs[i];
            std::vector<uint32_t> &out = results[i];
            out.resize(static_cast<size_t>(s.texW) * s.texH);
            for (int row = 0; row < s.texH; ++row)
                std::memcpy(out.data() + static_cast<size_t>(row) * s.texW,
                           atlasPx.data() + static_cast<size_t>(s.atlasY + row) * kG162MaxTexDim,
                           static_cast<size_t>(s.texW) * sizeof(uint32_t));
        }
        return results;
    }

    std::mutex m_mtx;
    std::condition_variable m_cvWork, m_cvReady;
    std::deque<QueueItem> m_queue;
    std::thread m_thread;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    bool m_started = false;
    bool m_stop = false;
    GLuint m_prog = 0, m_vao = 0, m_vbo = 0, m_fbo = 0;
    GLuint m_vramTex = 0, m_clutTex = 0, m_offsetTex = 0, m_colorTex = 0;
};
} // namespace

void g162StartPersistentDecoder()
{
    if (!g158_gpu_raster_enabled())
        return;
    static const bool s_on = (std::getenv("DC2_G162_GPU_DECODE") != nullptr);
    if (!s_on)
        return;
    G162GpuDecoder::instance().startEager();
    if (G162GpuDecoder::instance().isReady())
        std::fprintf(stderr, "[G162:decoder] PASS: persistent GPU T8-decode thread ready\n");
}

// G162: extern-declared at this call site (no header edit -- same idiom as g150_pipeline_enabled()
// etc.) to reuse the live, proven swizzle addressing without re-deriving it in this file.
namespace GSMem { extern std::uint32_t AddressP8(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t); }

// G166: batched entry point -- decodes `count` PSMT8 textures in ONE cross-thread round-trip.
// Parallel arrays (not a shared struct) so no type needs to cross the TU boundary beyond POD
// pointers -- same header-avoidance discipline as the rest of this file. `clut256Flat` is
// `count*256` RGBA8 entries (texture i's palette at `clut256Flat[i*256 .. i*256+256)`).
// `outPxArr[i]` must already be sized `texWArr[i]*texHArr[i]` by the caller. Returns false (caller
// must fall back to per-texture decode, e.g. the CPU loop) on ANY failure -- a partially-filled
// `outPxArr` on failure must not be trusted; success is all-or-nothing for the whole batch.
bool g162DecodeT8Batch(int count, const uint32_t *tbp0Arr, const uint32_t *tbwArr,
                       const int *texWArr, const int *texHArr, const uint32_t *clut256Flat,
                       const uint8_t *vram, size_t vramSize, uint32_t **outPxArr)
{
    static const bool s_on = (std::getenv("DC2_G162_GPU_DECODE") != nullptr) && g158_gpu_raster_enabled();
    if (!s_on || count <= 0 || count > kG162MaxBatchTex || !tbp0Arr || !tbwArr || !texWArr ||
        !texHArr || !clut256Flat || !vram || !outPxArr)
        return false;
    if (vramSize != static_cast<size_t>(kG162VramTexW) * kG162VramTexW)
        return false;

    G162BatchJob batch;
    batch.subs.resize(static_cast<size_t>(count));
    uint32_t unionMinOff = 0xFFFFFFFFu, unionMaxOff = 0u;
    int cumY = 0;
    for (int i = 0; i < count; ++i)
    {
        const int texW = texWArr[i], texH = texHArr[i];
        if (texW <= 0 || texH <= 0 || texW > kG162MaxTexDim || !outPxArr[i])
            return false;
        G162BatchSubJob &s = batch.subs[static_cast<size_t>(i)];
        s.texW = texW;
        s.texH = texH;
        s.atlasY = cumY;
        s.clutRow = i;
        s.offsets.resize(static_cast<size_t>(texW) * texH);
        for (int vv = 0; vv < texH; ++vv)
            for (int uu = 0; uu < texW; ++uu)
            {
                const uint32_t off = GSMem::AddressP8(tbp0Arr[i], tbwArr[i],
                                                      static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                s.offsets[static_cast<size_t>(vv) * texW + uu] = off;
                if (off < unionMinOff) unionMinOff = off;
                if (off > unionMaxOff) unionMaxOff = off;
            }
        cumY += texH;
        if (cumY > kG162AtlasMaxH)
            return false; // this frame's batch doesn't fit -- caller falls back per-texture
    }
    batch.totalHeight = cumY;
    batch.clutAtlas.assign(clut256Flat, clut256Flat + static_cast<size_t>(count) * 256u);

    const int rowStart = static_cast<int>(unionMinOff / static_cast<uint32_t>(kG162VramTexW));
    const int rowEndIncl = static_cast<int>(unionMaxOff / static_cast<uint32_t>(kG162VramTexW));
    const int rowCount = rowEndIncl - rowStart + 1;
    if (rowStart < 0 || rowCount <= 0 || rowStart + rowCount > kG162VramTexW)
        return false;
    batch.vramRowStart = rowStart;
    batch.vramRowCount = rowCount;
    batch.vram.assign(vram + static_cast<size_t>(rowStart) * kG162VramTexW,
                      vram + static_cast<size_t>(rowStart + rowCount) * kG162VramTexW);

    std::vector<std::vector<uint32_t>> results;
    if (!G162GpuDecoder::instance().decodeBatch(std::move(batch), results))
        return false;
    if (results.size() != static_cast<size_t>(count))
        return false;
    for (int i = 0; i < count; ++i)
    {
        if (results[static_cast<size_t>(i)].size() != static_cast<size_t>(texWArr[i]) * texHArr[i])
            return false;
    }
    for (int i = 0; i < count; ++i)
        std::memcpy(outPxArr[i], results[static_cast<size_t>(i)].data(),
                   results[static_cast<size_t>(i)].size() * sizeof(uint32_t));
    return true;
}

bool g162DecodeT8ToRgba(uint32_t tbp0, uint32_t tbw, int texW, int texH,
                        const uint32_t *clut256, const uint8_t *vram, size_t vramSize,
                        uint32_t *outPx)
{
    static const bool s_on = (std::getenv("DC2_G162_GPU_DECODE") != nullptr) && g158_gpu_raster_enabled();
    if (!s_on || texW <= 0 || texH <= 0 || !clut256 || !vram || !outPx)
        return false;
    if (vramSize != static_cast<size_t>(kG162VramTexW) * kG162VramTexW)
        return false;

    static const bool s_g164Profile = (std::getenv("DC2_G164_PROFILE") != nullptr);
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tBuildStart = s_g164Profile ? Clock::now() : Clock::time_point{};

    G162DecodeJob job;
    job.texW = texW;
    job.texH = texH;
    job.offsets.resize(static_cast<size_t>(texW) * texH);
    uint32_t minOff = 0xFFFFFFFFu, maxOff = 0u;
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint32_t off = GSMem::AddressP8(tbp0, tbw, static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
            job.offsets[static_cast<size_t>(vv) * texW + uu] = off;
            if (off < minOff) minOff = off;
            if (off > maxOff) maxOff = off;
        }
    job.clut256.assign(clut256, clut256 + 256);

    // G163: bound the CPU copy + GPU upload to the row range this texture's addressing actually
    // touches (derived from the offset table above), instead of the whole 4MB -- G162's whole-VRAM
    // copy/upload on every decode was the measured cost driver (net loss vs baseline).
    const int rowStart = static_cast<int>(minOff / static_cast<uint32_t>(kG162VramTexW));
    const int rowEndIncl = static_cast<int>(maxOff / static_cast<uint32_t>(kG162VramTexW));
    const int rowCount = rowEndIncl - rowStart + 1;
    if (rowStart < 0 || rowCount <= 0 || rowStart + rowCount > kG162VramTexW)
        return false;
    job.vramRowStart = rowStart;
    job.vramRowCount = rowCount;
    job.vram.assign(vram + static_cast<size_t>(rowStart) * kG162VramTexW,
                    vram + static_cast<size_t>(rowStart + rowCount) * kG162VramTexW);

    static std::atomic<uint64_t> s_buildNs{0};
    static std::atomic<uint64_t> s_submitWaitNs{0};
    static std::atomic<uint64_t> s_calls{0};
    if (s_g164Profile)
        s_buildNs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tBuildStart).count()),
            std::memory_order_relaxed);

    const Clock::time_point tSubmit = s_g164Profile ? Clock::now() : Clock::time_point{};
    std::vector<uint32_t> outVec;
    const bool ok = G162GpuDecoder::instance().decode(std::move(job), outVec);
    if (s_g164Profile)
    {
        s_submitWaitNs.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tSubmit).count()),
            std::memory_order_relaxed);
        const uint64_t n = s_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 64u) == 0u)
            std::fprintf(stderr, "[G164:profile] n=%llu avgUs/decode: cpuBuild=%.1f submitWait(=full GPU round-trip)=%.1f\n",
                         (unsigned long long)n,
                         s_buildNs.load(std::memory_order_relaxed) / 1e3 / n,
                         s_submitWaitNs.load(std::memory_order_relaxed) / 1e3 / n);
    }
    if (!ok)
        return false;
    if (outVec.size() != static_cast<size_t>(texW) * texH)
        return false;
    std::memcpy(outPx, outVec.data(), outVec.size() * sizeof(uint32_t));
    return true;
}

// ============================== G178: LLE GPU rasterizer backend ==============================
// (default OFF, DC2_G178_GPU=1; see plans/gpu-raster-arc-plan.md + plans/phase-G178-fix-log.md)
//
// The GL half of the bespoke LLE GPU rasterizer: a persistent dedicated GPU thread (the SAME
// proven release-before-share pattern as G160/G162 above) that owns one FBO per guest display
// framebuffer (color RGBA8 + 32F depth), a GPU-RESIDENT texture cache (decoded RGBA8 textures
// uploaded once and kept until invalidated/evicted — the residency G149's per-frame CPU decode
// could never amortize), and renders whole state-batched G144 display-list flushes submitted by
// the front-end in ps2_gs_rasterizer.cpp (which owns validation, vertex translation, texture
// decode, page-generation invalidation, and all VRAM swizzle I/O). One synchronous round-trip
// per flush (title: ~1/frame, G155 measured mid-frame flushes = 0), ending in ONE glReadPixels
// whose pixels the front-end swizzles back into guest VRAM — presentation/dumps/golden
// verification all keep reading VRAM exactly as before.
namespace
{
bool g178EnvEnabled()
{
    static const bool on = g178DefaultOn();
    return on;
}

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
constexpr GLenum GL257_SHADER_IMAGE_LOAD = 0x82A4;
constexpr GLenum GL257_SHADER_IMAGE_STORE = 0x82A5;
// GPU texture-cache residency cap (bytes of decoded RGBA8). Title uses a handful of ≤512²
// textures (~1-4MB); the cap only matters on texture-heavy routes. LRU by last-used batch.
constexpr size_t kG178TexCacheCapBytes = 256ull * 1024 * 1024;

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

    // G242: universal-Z batches carry the authoritative guest PSMZ24 rows beside the existing
    // color-only G178 interface. Keep this private side channel out of ps2_gs_gpu_lle.h: that
    // header is deliberately frozen for the project's no-header-rebuild rule. Submission is
    // synchronous, so the caller-owned vectors remain alive until the worker finishes.
    bool submitDepth(G178Batch &batch, uint64_t depthKey,
                     const std::vector<float> *depthUpload, int depthY, int depthRows)
    {
        return submitImpl(&batch, depthKey, depthUpload, depthY, depthRows, nullptr);
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
        return submitImpl(&readBatch, depthKey, nullptr, depthY, depthRows, &depthReadback);
    }

    // G261: synchronous FBO color row-window readback (deferred RTT-wave materialization).
    bool readColor(uint32_t fbp, int width, int height, int glY, int rows,
                   std::vector<uint32_t> &out)
    {
        if (width <= 0 || height <= 0 || glY < 0 || rows <= 0 ||
            glY > height || rows > height - glY || !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
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
            item.promise = promise;
            m_queue.push_back(std::move(item));
        }
        m_cvWork.notify_one();
        try
        {
            return fut.get();
        }
        catch (...)
        {
            return false;
        }
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
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
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
            item.promise = promise;
            m_queue.push_back(std::move(item));
        }
        m_cvWork.notify_one();
        try
        {
            return fut.get();
        }
        catch (...)
        {
            return false;
        }
    }

private:
    bool submitImpl(G178Batch *batch, uint64_t depthKey,
                    const std::vector<float> *depthUpload, int depthY, int depthRows,
                    std::vector<float> *depthReadback)
    {
        if (batch == nullptr || !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.batch = batch;
            item.depthKey = depthKey;
            item.depthUpload = depthUpload;
            item.depthY = depthY;
            item.depthRows = depthRows;
            item.depthReadback = depthReadback;
            item.promise = promise;
            m_queue.push_back(std::move(item));
        }
        m_cvWork.notify_one();
        try
        {
            return fut.get();
        }
        catch (...)
        {
            return false;
        }
    }

    struct QueueItem
    {
        G178Batch *batch = nullptr;
        uint64_t depthKey = 0u;
        const std::vector<float> *depthUpload = nullptr;
        int depthY = 0;
        int depthRows = 0;
        std::vector<float> *depthReadback = nullptr;
        // G261 color-readback job (batch == nullptr, colorReadback != nullptr)
        uint32_t colorFbp = 0u;
        std::vector<uint32_t> *colorReadback = nullptr;
        // G264 color-write job (batch == nullptr, colorWrite != nullptr): mirror guest upload
        // bytes into the resident FBO texture instead of materializing around them.
        const std::vector<uint32_t> *colorWrite = nullptr;
        int colorTexW = 0, colorTexH = 0;
        int colorX = 0, colorY = 0, colorW = 0, colorRows = 0;
        std::shared_ptr<std::promise<bool>> promise;
    };
    struct TexEntry
    {
        GLuint id = 0;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
    };
    struct FboEntry
    {
        GLuint fbo = 0, colorTex = 0, depthRb = 0;
        int w = 0, h = 0;
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
        if (!setupProgram() || (g256ExactRttOn() && !setupExactProgram()) || !setupBuffers() ||
            (g257ImageStoreTestOn() && !runG257ImageStoreDiagnostic()))
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
            m_failed = true;
            m_cvReady.notify_all();
            return;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        m_ready.store(true, std::memory_order_release);
        m_cvReady.notify_all();
        std::fprintf(stderr, "[G178:backend] PASS: persistent LLE GPU raster thread ready\n");

        for (;;)
        {
            QueueItem item;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty())
                    break;
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }
            bool ok = false;
            try
            {
                if (item.colorReadback != nullptr)
                    ok = readColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                           item.colorY, item.colorRows, *item.colorReadback);
                else if (item.colorWrite != nullptr)
                    ok = writeColorOnWorker(item.colorFbp, item.colorTexW, item.colorTexH,
                                            item.colorX, item.colorY, item.colorW,
                                            item.colorRows, *item.colorWrite);
                else if (item.depthReadback != nullptr)
                    ok = readDepthOnWorker(item.depthKey, item.batch->fbW, item.batch->fbH,
                                           item.depthY, item.depthRows, *item.depthReadback);
                else
                    ok = renderBatch(*item.batch, item.depthKey, item.depthUpload,
                                     item.depthY, item.depthRows);
            }
            catch (...)
            {
                ok = false;
            }
            item.promise->set_value(ok);
        }

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
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
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
            "uniform vec2 uFbSize;\n"
            "noperspective out vec3 vSTQ;\n"
            "noperspective out vec4 vColor;\n"
            "noperspective out float vZ;\n"
            "void main(){\n"
            "    vSTQ = aSTQ;\n"
            "    vColor = aColor;\n"
            "    vZ = aPos.z;\n"
            "    float ndcX = aPos.x / uFbSize.x * 2.0 - 1.0;\n"
            "    float ndcY = 1.0 - aPos.y / uFbSize.y * 2.0;\n"
            "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
            "}\n";
        // uMode bits: 0 = textured, 1..2 = TFX, 3 = TCC, 4 = clamp Z to PSMZ24,
        // 5 = store raw source alpha (G255 RTT second pass), 6 = G265 GEQUAL alpha test,
        // 7..8 = G265 AREF code (0=1, 1=64, 2=127).
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
            "out vec4 o;\n"
            "uniform sampler2D uTex;\n"
            "uniform int uMode;\n"
            "void main(){\n"
            "    vec4 c = vColor;\n"
            "    bool alphaTest = (uMode & 64) != 0;\n"
            "    int alphaByte = 0;\n"
            "    if (alphaTest) alphaByte = int(floor(vColor.a * 255.0 + 0.5));\n"
            "    if ((uMode & 1) != 0) {\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        vec4 t = texture(uTex, uv);\n"
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
        const GLint locTex = m_f.glGetUniformLocation_(m_prog, "uTex");
        m_f.glUniform1i_(locTex, 0);
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
        static_assert(sizeof(G178Vtx) == 28, "G178Vtx must be tightly packed (6 floats + 4 bytes)");
        m_f.glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(0));
        m_f.glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(12));
        m_f.glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx),
                                   reinterpret_cast<const void *>(24));
        m_f.glEnableVertexAttribArray_(0);
        m_f.glEnableVertexAttribArray_(1);
        m_f.glEnableVertexAttribArray_(2);
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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

    void destroyFbo(FboEntry &e)
    {
        if (e.fbo != 0)
            m_f.glDeleteFramebuffers_(1, &e.fbo);
        if (e.colorTex != 0)
            glDeleteTextures(1, &e.colorTex);
        if (e.depthRb != 0)
            m_e.glDeleteRenderbuffers_(1, &e.depthRb);
        e = FboEntry{};
    }

    FboEntry &ensureFbo(uint32_t fbp, int w, int h)
    {
        FboEntry &e = m_fbos[fbp];
        if (e.fbo != 0 && e.w == w && e.h == h)
            return e;
        destroyFbo(e);
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
        auto it = m_fbos.find(fbp);
        if (it == m_fbos.end() || it->second.fbo == 0 || it->second.w != w || it->second.h != h)
            return false;
        out.resize(static_cast<size_t>(w) * rows);
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, it->second.fbo);
        glDisable(GL_SCISSOR_TEST);
        glReadPixels(0, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
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
        auto it = m_fbos.find(fbp);
        if (it == m_fbos.end() || it->second.colorTex == 0 || it->second.w != texW ||
            it->second.h != texH)
            return false;
        glBindTexture(GL_TEXTURE_2D, it->second.colorTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, glX, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                        in.data());
        return glGetError() == GL_NO_ERROR;
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
        if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
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
                if (d.texKey != 0u || d.srcFbp == b.fbp)
                    return false;
                const auto it = m_fbos.find(d.srcFbp);
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

    bool renderBatch(G178Batch &b, uint64_t depthKey, const std::vector<float> *depthUpload,
                     int depthY, int depthRows)
    {
        if (!validateBatchInputs(b, depthKey, depthUpload, depthY, depthRows))
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

        FboEntry &fbo = ensureFbo(b.fbp, b.fbW, b.fbH);
        if (fbo.fbo == 0)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
        glViewport(0, 0, b.fbW, b.fbH);

        if (b.uploadFb)
        {
            glBindTexture(GL_TEXTURE_2D, fbo.colorTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE,
                            b.fbPixels.data());
        }

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
            if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
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
            if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
                return false;
        }
        if (b.clearDepth && !guestDepth)
        {
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            glClear(GL_DEPTH_BUFFER_BIT);
        }

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
        for (size_t drawIndex = 0; drawIndex < b.draws.size(); ++drawIndex)
        {
            const G178Draw &d = b.draws[drawIndex];
            // GS scissor is inclusive, top-origin; GL is bottom-origin.
            const int scH = d.scY1 - d.scY0 + 1;
            glScissor(d.scX0, b.fbH - 1 - d.scY1, d.scX1 - d.scX0 + 1, scH);
            const uint8_t depthFunc = d.depthFunc & 0x0Fu;
            if (depthFunc == 0)
            {
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
            }
            else
            {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(depthFunc == 1 ? GL_ALWAYS : (depthFunc == 2 ? GL_GEQUAL : GL_GREATER));
                glDepthMask(d.depthWrite ? GL_TRUE : GL_FALSE);
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
                glDisable(GL_BLEND);
            }
            else
            {
                glEnable(GL_BLEND);
                // Alpha channel: keep dest alpha = written src value (CPU writePixel stores the
                // source alpha unblended). blend==3 (G262): (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd
                // additive; blend==4 (G262): (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs subtractive
                // (GL_FUNC_REVERSE_SUBTRACT = dst - src, both clamp at the [0,1] rail like GS
                // COLCLAMP). RGB equation must be reset per draw — it is persistent GL state.
                m_e.glBlendEquationSeparate_(d.blend == 4 ? GL178_FUNC_REVERSE_SUBTRACT
                                                          : GL178_FUNC_ADD,
                                             GL178_FUNC_ADD);
                m_e.glBlendFuncSeparate_(d.blend == 5 ? GL_ZERO :
                                             ((d.blend == 3 || d.blend == 4) ? GL_ONE : GL_SRC_ALPHA),
                                         (d.blend == 1 || d.blend == 5) ?
                                             GL_ONE_MINUS_SRC_ALPHA : GL_ONE,
                                         GL_ONE, GL_ZERO);
            }
            int mode = 0;
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
                const FboEntry &src = m_fbos.find(d.srcFbp)->second;
                glBindTexture(GL_TEXTURE_2D, src.colorTex);
                const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
            }
            else if (d.texKey != 0)
            {
                TexEntry *const texture = drawTextures[drawIndex];
                glBindTexture(GL_TEXTURE_2D, texture->id);
                const GLint filter = d.bilinear ? GL_LINEAR : GL_NEAREST;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                d.wrapU == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                d.wrapV == 0 ? GL_REPEAT : static_cast<GLint>(GL158_CLAMP_TO_EDGE));
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
            }
            if (g255RttAlpha && d.blend != 0u)
            {
                // Fixed-function blending needs As/128 in the shader alpha channel, while PS2
                // framebuffer alpha stores the unblended source A byte. Preserve RGB blending in
                // pass one with alpha masked, then overwrite alpha only with the raw source value.
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                m_f.glUniform1i_(m_locMode, mode);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                glDisable(GL_BLEND);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                m_f.glUniform1i_(m_locMode, mode | 32);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            else
            {
                if (g255RttAlpha)
                    mode |= 32;
                m_f.glUniform1i_(m_locMode, mode);
                glDrawArrays(GL_TRIANGLES, d.firstVtx, d.vtxCount);
            }
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
        if (!b.skipReadback)
            glReadPixels(0, 0, b.fbW, b.fbH, GL_RGBA, GL_UNSIGNED_BYTE, b.readback.data());
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
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
        return true;
    }

    GLFns m_f;
    G178GLExt m_e;
    GLuint m_prog = 0, m_progExact = 0, m_vao = 0, m_vbo = 0;
    GLint m_locFbSize = -1, m_locMode = -1;
    GLint m_locExactFbSize = -1, m_locExactMode = -1, m_locExactLinear = -1;
    GLint m_locExactWrapU = -1, m_locExactWrapV = -1;
    std::map<uint32_t, FboEntry> m_fbos;
    std::unordered_map<uint64_t, DepthEntry> m_depths;
    std::unordered_map<uint64_t, TexEntry> m_texCache;
    size_t m_texBytes = 0;
    uint64_t m_batchCounter = 0;
    uint64_t m_g257RealBatches = 0;
    uint64_t m_g258TraceBatches = 0;
    std::vector<uint32_t> m_g257ImageAfter;
    size_t m_g257Changed = 0, m_g257Covered = 0;
    GLenum m_g257PreError = GL_NO_ERROR, m_g257PostError = GL_NO_ERROR;
    unsigned m_g257PreErrorCount = 0, m_g257PostErrorCount = 0;

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cvWork;
    std::condition_variable m_cvReady;
    std::deque<QueueItem> m_queue;
    bool m_stop = false;
    bool m_started = false;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};

    std::mutex m_residentMtx;
    std::unordered_set<uint64_t> m_resident;
};
} // namespace

bool g178_backend_ready()
{
    return g178EnvEnabled() && G178Backend::instance().ready();
}

bool g178_backend_submit(G178Batch &batch)
{
    return G178Backend::instance().submit(batch);
}

// G242 private .cpp-to-.cpp side channel; intentionally not added to a header.
bool g242_backend_submit_depth(G178Batch &batch, uint64_t depthKey,
                               const std::vector<float> *depthUpload,
                               int depthY, int depthRows)
{
    return G178Backend::instance().submitDepth(batch, depthKey, depthUpload, depthY, depthRows);
}

bool g242_backend_read_depth(uint64_t depthKey, int width, int height,
                             int depthY, int depthRows, std::vector<float> &depthReadback)
{
    return G178Backend::instance().readDepth(depthKey, width, height,
                                             depthY, depthRows, depthReadback);
}

bool g178_backend_has_tex(uint64_t key)
{
    return G178Backend::instance().hasTex(key);
}

// G261: deferred RTT-wave materialization (front-end writes the rows back into guest VRAM).
bool g178_backend_read_color(uint32_t fbp, int width, int height, int glY, int rows,
                             std::vector<uint32_t> &out)
{
    return G178Backend::instance().readColor(fbp, width, height, glY, rows, out);
}

// G264: upload-into-FBO mirror (front-end pushes guest upload bytes into the resident FBO).
bool g178_backend_write_color(uint32_t fbp, int width, int height, int glY, int rows,
                              const std::vector<uint32_t> &in)
{
    return G178Backend::instance().writeColor(fbp, width, height, 0, glY, width, rows, in);
}

// G264 alias-aware partial mirror. Kept as a private .cpp-to-.cpp bridge so the runtime's
// generated-target headers remain untouched.
bool g264_backend_write_color_rect(uint32_t fbp, int texWidth, int texHeight,
                                   int glX, int glY, int width, int rows,
                                   const std::vector<uint32_t> &in)
{
    return G178Backend::instance().writeColor(fbp, texWidth, texHeight,
                                               glX, glY, width, rows, in);
}

// Called once from PS2Runtime::initialize() (after g158CaptureMainContext). No-op unless
// DC2_G178_GPU=1.
void g178StartPersistentBackend()
{
    if (!g178EnvEnabled())
        return;
    G178Backend::instance().startEager();
    if (G178Backend::instance().ready())
        std::fprintf(stderr, "[G178:backend] persistent LLE GPU raster backend started\n");
}

#else // !(_WIN32 && !PLATFORM_VITA)

bool g158_gpu_raster_enabled() { return false; }
void g158RunSelfTest() {}
void g158CaptureMainContext() {}
void g158RunWorkerContextTest() {}
void g158StartDedicatedGpuThread() {}
void g162StartPersistentDecoder() {}
bool g162DecodeT8ToRgba(uint32_t, uint32_t, int, int, const uint32_t *, const uint8_t *, size_t, uint32_t *) { return false; }
bool g162DecodeT8Batch(int, const uint32_t *, const uint32_t *, const int *, const int *,
                       const uint32_t *, const uint8_t *, size_t, uint32_t **) { return false; }
bool g178_backend_ready() { return false; }
bool g178_backend_submit(G178Batch &) { return false; }
bool g242_backend_submit_depth(G178Batch &, uint64_t, const std::vector<float> *, int, int) { return false; }
bool g242_backend_read_depth(uint64_t, int, int, int, int, std::vector<float> &) { return false; }
bool g178_backend_has_tex(uint64_t) { return false; }
bool g178_backend_read_color(uint32_t, int, int, int, int, std::vector<uint32_t> &) { return false; }
bool g178_backend_write_color(uint32_t, int, int, int, int, const std::vector<uint32_t> &) { return false; }
bool g264_backend_write_color_rect(uint32_t, int, int, int, int, int, int,
                                   const std::vector<uint32_t> &) { return false; }
void g178StartPersistentBackend() {}

#endif
