// ===== G433: pixel-pack-buffer (PBO) colour readback ===========================================
//
// WHY. The G433 ceiling probe (DC2_G433_NO_RB=1, workload-invariant: identical rows and Mpx on
// both arms) priced the whole blocking colour readback at −3.25 ms/f on lean MAP-0. The same run
// timed the bare `glReadPixels` on the GL worker at **1.97 ms per call / 21.7 ns per pixel**
// (~186 MB/s). That is one to two orders of magnitude below what an RGBA8 framebuffer read should
// cost on this hardware, so the dominant term is NOT the pixel traffic and NOT a pipeline stall
// draining conserved rendering (the render bucket is only ~5 ms/f over 34 batches): it is the
// driver's synchronous read-to-client-memory path.
//
// WHAT. Route the read through a persistent GL_PIXEL_PACK_BUFFER: `glReadPixels` with a NULL
// pointer becomes a server-side DMA into the PBO, and the client copy is a plain mapped-memory
// memcpy. Same bytes, same order, same format — this is an exact transformation, not an
// approximation. It remains synchronous (the map still waits for the transfer), so it changes no
// ordering or visibility contract anywhere in the publication path; only the transport changes.
//
// FALLBACK. Any failure (no entry points, map returns null, buffer alloc fails) returns false and
// the caller falls straight back to the direct `glReadPixels`, so an unsupported driver is a
// perf regression to baseline, never a correctness one.
//
// RESULT: bit-exact (418,922,496 pixels compared, bad=0) but NOT faster — 22.79 ns/px against the
// direct read's 21.70 ns/px. The transport was never the cost. Left in the tree default-OFF as the
// standing evidence that this readback is sync-bound; do not re-open it as a speed lever.
//
// Levers: DC2_G433_PBO=1 enables the PBO transport (opt-in, measured slower).
// DC2_G433_PBO_VERIFY=1 performs BOTH reads and byte-compares them (bring-up oracle).

#ifndef GL433_PIXEL_PACK_BUFFER
#define GL433_PIXEL_PACK_BUFFER 0x88EB
#define GL433_STREAM_READ 0x88E1
#define GL433_READ_ONLY 0x88B8
#endif

typedef void *(APIENTRY *PFN433MAPBUFFER)(GLenum, GLenum);
typedef GLboolean(APIENTRY *PFN433UNMAPBUFFER)(GLenum);

struct G433PboFns
{
    PFN433MAPBUFFER glMapBuffer_ = nullptr;
    PFN433UNMAPBUFFER glUnmapBuffer_ = nullptr;
    bool tried = false;
    bool ok = false;
};

static G433PboFns &g433PboFns()
{
    static G433PboFns f;
    if (!f.tried)
    {
        f.tried = true;
        f.ok = loadProc(f.glMapBuffer_, "glMapBuffer") &&
               loadProc(f.glUnmapBuffer_, "glUnmapBuffer");
        std::fprintf(stderr, "[G433:pbo] entry points %s\n", f.ok ? "loaded" : "UNAVAILABLE");
    }
    return f;
}

// MEASURED SLOWER — opt-in only. Direct read 21.70 ns/px vs PBO 22.79 ns/px on lean MAP-0
// (465 Mpx per arm, identical workload). Kept because the bit-exactness result is the evidence
// that the readback is sync-bound rather than transport-bound; see plans/phase-G433-fix-log.md.
static bool g433PboOn()
{
    static const bool on = (std::getenv("DC2_G433_PBO") != nullptr);
    return on;
}

static bool g433PboVerifyOn()
{
    static const bool on = (std::getenv("DC2_G433_PBO_VERIFY") != nullptr);
    return on;
}

// GL-worker thread only (single owner, like every other resource in this backend).
static GLuint g_g433Pbo = 0;
static size_t g_g433PboBytes = 0;

// Read `rows` scanlines of the currently bound READ framebuffer into `out` through the PBO.
// `out` must already be sized to w*rows. Returns false if the PBO path is unavailable, in which
// case nothing has been written and the caller must do the direct read.
static bool g433ReadPixelsViaPbo(GLFns &f, int glY, int w, int rows, uint32_t *out)
{
    G433PboFns &pf = g433PboFns();
    if (!pf.ok || f.glGenBuffers_ == nullptr || f.glBindBuffer_ == nullptr ||
        f.glBufferData_ == nullptr)
        return false;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(rows) * 4u;
    if (bytes == 0)
        return false;
    if (g_g433Pbo == 0)
    {
        f.glGenBuffers_(1, &g_g433Pbo);
        if (g_g433Pbo == 0)
            return false;
        g_g433PboBytes = 0;
    }
    f.glBindBuffer_(GL433_PIXEL_PACK_BUFFER, g_g433Pbo);
    if (bytes > g_g433PboBytes)
    {
        // Orphan-and-grow. STREAM_READ tells the driver this is a GPU->CPU staging buffer.
        f.glBufferData_(GL433_PIXEL_PACK_BUFFER, static_cast<GLsizeiptrLocal>(bytes), nullptr,
                        GL433_STREAM_READ);
        if (glGetError() != GL_NO_ERROR)
        {
            f.glBindBuffer_(GL433_PIXEL_PACK_BUFFER, 0);
            g_g433PboBytes = 0;
            return false;
        }
        g_g433PboBytes = bytes;
    }
    glReadPixels(0, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    void *mapped = pf.glMapBuffer_(GL433_PIXEL_PACK_BUFFER, GL433_READ_ONLY);
    if (mapped == nullptr)
    {
        f.glBindBuffer_(GL433_PIXEL_PACK_BUFFER, 0);
        return false;
    }
    std::memcpy(out, mapped, bytes);
    pf.glUnmapBuffer_(GL433_PIXEL_PACK_BUFFER);
    f.glBindBuffer_(GL433_PIXEL_PACK_BUFFER, 0);
    return true;
}

// The one place the backend reads colour pixels. Picks the PBO transport when available and
// falls back to the direct read otherwise. DC2_G433_PBO_VERIFY=1 runs both and byte-compares.
static void g433DoReadPixels(GLFns &f, int glY, int w, int rows, uint32_t *out)
{
    if (g433PboOn() && g433ReadPixelsViaPbo(f, glY, w, rows, out))
    {
        if (g433PboVerifyOn())
        {
            static std::vector<uint32_t> s_ref; // GL-worker thread only
            static uint64_t s_checked = 0ull, s_bad = 0ull;
            const size_t n = static_cast<size_t>(w) * static_cast<size_t>(rows);
            s_ref.resize(n);
            glReadPixels(0, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, s_ref.data());
            if (std::memcmp(s_ref.data(), out, n * 4u) != 0)
                ++s_bad;
            s_checked += n;
            if (s_bad != 0 && s_bad <= 8)
                std::fprintf(stderr, "[G433:pbo-verify] MISMATCH glY=%d w=%d rows=%d bad=%llu\n",
                             glY, w, rows, static_cast<unsigned long long>(s_bad));
            static uint64_t s_rep = 0ull;
            if (++s_rep % 512u == 0u)
                std::fprintf(stderr, "[G433:pbo-verify] checked=%llu px bad=%llu\n",
                             static_cast<unsigned long long>(s_checked),
                             static_cast<unsigned long long>(s_bad));
        }
        return;
    }
    // G491: read in the driver's native packed format and restore RGBA byte order on the CPU.
    // Placed here, at the ONE site that reads colour pixels, so both arms share every other
    // statement in the call (G421's control-arm rule). The control arm below is byte-identical to
    // the pre-G491 body.
    g491ProbeReadFormat();
    if (g491NativeReadActive() && g491ReadPixelsNative(glY, w, rows, out))
    {
        if (g491RbVerifyOn())
            g491VerifyNative(glY, w, rows, out);
        return;
    }
    glReadPixels(0, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
