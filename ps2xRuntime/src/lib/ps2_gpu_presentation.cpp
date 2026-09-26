#include "ps2_gpu_presentation.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#endif

namespace dc2::gpu_present
{
namespace
{
bool flag(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
std::atomic<uint64_t> published{0}, fallback{0}, readbacks{0}, bytes{0};
void note(bool hit, uint64_t count)
{
    static const bool stats = flag("DC2_G764_STATS");
    if (!stats) return;
    const auto n = hit ? ++published : ++fallback;
    if (hit) bytes += count;
    if (n <= 4 || n % 512 == 0)
        std::fprintf(stderr, "[G764:shared] frames=%llu fallback=%llu cpuReads=%llu avoidedReadBytes=%llu\n",
            (unsigned long long)published.load(), (unsigned long long)fallback.load(),
            (unsigned long long)readbacks.load(), (unsigned long long)bytes.load());
}
#if defined(_WIN32)
constexpr GLenum Framebuffer = 0x8d40, ColorAttachment = 0x8ce0, Complete = 0x8cd5;
constexpr GLenum Rgba8 = 0x8058, SyncGpuCommandsComplete = 0x9117;
struct GL
{
    void* (APIENTRY* fence)(GLenum, GLbitfield) = nullptr;
    void (APIENTRY* wait)(void*, GLbitfield, uint64_t) = nullptr;
    void (APIENTRY* erase)(void*) = nullptr;
    void (APIENTRY* genFbo)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* bindFbo)(GLenum, GLuint) = nullptr;
    void (APIENTRY* attach)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (APIENTRY* status)(GLenum) = nullptr;
    template<class T> static bool load(T& fn, const char* name)
    {
        auto p = wglGetProcAddress(name);
        const auto n = reinterpret_cast<uintptr_t>(p);
        if (n <= 3 || n == std::numeric_limits<uintptr_t>::max()) return false;
        fn = reinterpret_cast<T>(p); return true;
    }
    bool init()
    {
        return load(fence, "glFenceSync") && load(wait, "glWaitSync") &&
            load(erase, "glDeleteSync") && load(genFbo, "glGenFramebuffers") &&
            load(bindFbo, "glBindFramebuffer") && load(attach, "glFramebufferTexture2D") &&
            load(status, "glCheckFramebufferStatus");
    }
};
struct State
{
    HDC dc = nullptr;
    HGLRC reader = nullptr;
    GL gl;
    bool loaded = false;
    GLuint readerFbo = 0;
    std::mutex readMutex;
    std::array<FramePtr, 8> pool; // G767: first poolSize() slots are used (was a fixed 3)
};
// Same lifetime as the renderer's shared-context group. Keeping this owner alive
// also keeps leased snapshots valid through asynchronous runtime shutdown.
State& state() { static State* s = new State; return *s; }
void awaitAndErase(GL& gl, void*& sync)
{
    if (sync) { gl.wait(sync, 0, ~uint64_t(0)); gl.erase(sync); sync = nullptr; }
}
#endif
}
bool enabled()
{
    // Windows release uses a shared GL context and has a complete CPU fallback
    // when the driver cannot create it. Keep explicit opt-in on other hosts
    // until their context-sharing path is validated.
#if defined(_WIN32)
    static const bool on = !flag("DC2_G764_NO_SHARED_PRESENT");
#else
    static const bool on = flag("DC2_G764_SHARED_PRESENT") && !flag("DC2_G764_NO_SHARED_PRESENT");
#endif
    return on;
}
void initialize(void* deviceContext, void* mainContext)
{
#if defined(_WIN32)
    if (!enabled()) return;
    auto& s = state();
    if (s.reader) return;
    s.dc = static_cast<HDC>(deviceContext);
    HGLRC reader = wglCreateContext(s.dc);
    if (!reader || !wglShareLists(static_cast<HGLRC>(mainContext), reader))
    {
        if (reader) wglDeleteContext(reader);
        std::fprintf(stderr, "[G764:shared] context unavailable; CPU fallback\n");
        return;
    }
    s.reader = reader;
#endif
}
// G767: snapshot pool depth. With 3 slots (one latched, one presenting) the only free slot is the
// one the presenter read last, and begin() queued a glWaitSync on its consumption fence into the
// PRODUCER's GL stream: on a GPU-bound route (s05, `[G495:gpu] dispread` ~5 ms per latch) the render
// queue idled behind the presenter. A deeper pool hands out a slot consumed frames ago.
//   DC2_G767_PRESENT_POOL=<3..8>  (default 3 = the G764 behaviour).
// ⛔ MEASURED NULL at 6 (s05 diag, `[G495:gpu] dispread` 6.2-6.9 vs 5.8-6.3 ms/f at 3): that class's
// GPU window absorbs the drain of the render work queued ahead of the latch, not a fence bubble.
// Kept tunable, default unchanged (plans/phase-G767-ledger.md §8).
static size_t poolSize()
{
    static const size_t n = [] {
        const char *e = std::getenv("DC2_G767_PRESENT_POOL");
        long v = e ? std::strtol(e, nullptr, 10) : 3;
        if (v < 3) v = 3;
        if (v > 8) v = 8;
        return static_cast<size_t>(v);
    }();
    return n;
}
FramePtr begin(uint32_t width, uint32_t height)
{
#if defined(_WIN32)
    auto& s = state();
    if (!enabled() || !s.reader || !width || !height || width > 8192 || height > 8192) return {};
    if (!s.loaded) { if (!s.gl.init()) return {}; s.loaded = true; }
    // Prefer the free slot whose consumption fence is oldest: cycle a cursor through the pool.
    static size_t cursor = 0;
    const size_t n = poolSize();
    for (size_t k = 0; k < n; ++k)
    {
        auto& item = s.pool[(cursor + k) % n];
        if (!item) item = std::make_shared<Frame>();
        if (item.use_count() != 1) continue;
        std::lock_guard<std::mutex> lock(item->mutex);
        awaitAndErase(s.gl, item->consumed);
        awaitAndErase(s.gl, item->ready);
        if (!item->texture || item->width != width || item->height != height)
        {
            GLint previous = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
            if (!item->texture) glGenTextures(1, &item->texture);
            glBindTexture(GL_TEXTURE_2D, item->texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812f);
            glTexImage2D(GL_TEXTURE_2D, 0, Rgba8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const bool ok = item->texture && glGetError() == GL_NO_ERROR;
            glBindTexture(GL_TEXTURE_2D, previous);
            if (!ok) { item->width = item->height = 0; note(false, 0); return {}; }
            item->width = width; item->height = height;
        }
        cursor = (cursor + k + 1) % n;
        return item;
    }
    note(false, 0);
#endif
    return {};
}
bool publish(const FramePtr& frame)
{
#if defined(_WIN32)
    if (!frame) return false;
    auto& s = state();
    std::lock_guard<std::mutex> lock(frame->mutex);
    frame->ready = s.gl.fence(SyncGpuCommandsComplete, 0);
    if (!frame->ready) { glFinish(); note(false, 0); return false; }
    // A fence in an unflushed producer context cannot be waited in another one.
    glFlush(); note(true, uint64_t(frame->width) * frame->height * 4);
    return true;
#else
    return false;
#endif
}
bool waitReady(const FramePtr& frame)
{
#if defined(_WIN32)
    if (!frame || !frame->ready || !state().loaded) return false;
    state().gl.wait(frame->ready, 0, ~uint64_t(0));
    return true;
#else
    return false;
#endif
}
void finishRead(const FramePtr& frame)
{
#if defined(_WIN32)
    if (!frame) return;
    auto& gl = state().gl;
    std::lock_guard<std::mutex> lock(frame->mutex);
    // Main presentation is the sole asynchronous consumer. Its newer fence
    // includes all earlier reads in that context. CPU readPixels is synchronous.
    if (frame->consumed) gl.erase(frame->consumed);
    frame->consumed = gl.fence(SyncGpuCommandsComplete, 0);
    if (!frame->consumed) glFinish();
    else glFlush();
#endif
}
bool readPixels(const FramePtr& frame, std::vector<uint8_t>& rgba)
{
#if defined(_WIN32)
    if (!frame) return false;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.readMutex);
    const HDC oldDC = wglGetCurrentDC();
    const HGLRC oldRC = wglGetCurrentContext();
    if (!s.reader || !wglMakeCurrent(s.dc, s.reader)) return false;
    struct Restore { HDC dc; HGLRC rc; ~Restore() { wglMakeCurrent(dc, rc); } } restore{oldDC, oldRC};
    if (!waitReady(frame)) return false;
    if (!s.readerFbo) s.gl.genFbo(1, &s.readerFbo);
    s.gl.bindFbo(Framebuffer, s.readerFbo);
    s.gl.attach(Framebuffer, ColorAttachment, GL_TEXTURE_2D, frame->texture, 0);
    if (s.gl.status(Framebuffer) != Complete) return false;
    rgba.resize(size_t(frame->width) * frame->height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, frame->width, frame->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (glGetError() != GL_NO_ERROR) { rgba.clear(); return false; }
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    ++readbacks;
    return true;
#else
    return false;
#endif
}
}
