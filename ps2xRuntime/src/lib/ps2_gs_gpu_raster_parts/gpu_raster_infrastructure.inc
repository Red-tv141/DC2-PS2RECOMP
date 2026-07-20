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

bool g299ProfileOn()
{
    static const bool on = g158EnvFlag("DC2_G299_PROFILE");
    return on;
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

