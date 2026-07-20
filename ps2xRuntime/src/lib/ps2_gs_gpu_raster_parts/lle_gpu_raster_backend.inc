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
        if (!m_ready.load(std::memory_order_acquire) || m_asyncPending)
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            m_asyncSlot = std::move(batch); // vectors moved out of the caller's s_batch, scalars copied
            QueueItem item;
            item.batch = &m_asyncSlot;
            item.promise = promise;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
            m_queue.push_back(std::move(item));
        }
        m_asyncFut = std::move(fut);
        m_asyncPending = true;
        m_cvWork.notify_one();
        return true;
    }

    // G305: block until the in-flight async packet (if any) completes. Returns false when there
    // was nothing pending; otherwise sets ok to the backend render/readback result. After this
    // returns the worker is done with m_asyncSlot, so the front-end may reuse it.
    bool drainAsync(bool &ok)
    {
        if (!m_asyncPending)
            return false;
        m_asyncPending = false;
        try { ok = m_asyncFut.get(); }
        catch (...) { ok = false; }
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

    // G280: synchronous GPU->GPU page-tile copies between two persistent target FBOs (alias
    // view resolve). Geometry is validated on the worker against both FBOs' live dimensions.
    bool copyColor(uint32_t srcFbp, uint32_t dstFbp, const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || rects6.empty() || (rects6.size() % 6u) != 0u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.colorFbp = dstFbp;
            item.copySrcFbp = srcFbp;
            item.copyRects = &rects6;
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

    // G289: same synchronous bit-exact color copy as G280, but the one measured work target may
    // be first-use. Allocate its 512x512 FBO on the worker before validating/copying.
    bool copyColorEnsureDest(uint32_t srcFbp, uint32_t dstFbp, int dstW, int dstH,
                             const std::vector<int32_t> &rects6)
    {
        if (srcFbp == dstFbp || dstW <= 0 || dstH <= 0 || rects6.empty() ||
            (rects6.size() % 6u) != 0u || !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
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
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
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
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309Readback = readback ? &out : nullptr;
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

    // G310: use G309's exact atlas compositor once to populate the persistent logical FBO.
    // Unlike G309's timing arm, downstream draws sample this one ordinary texture; unlike the
    // verifier, no readback follows the composition pass.
    bool buildLogicalComposite(const std::vector<uint32_t> &pageMap,
                               const std::vector<uint32_t> &basePixels)
    {
        if (pageMap.size() != 128u || basePixels.size() != 512u * 512u ||
            !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> fut = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g309PageMap = &pageMap;
            item.g309BasePixels = &basePixels;
            item.g309ForceDraw = true;
            item.g309OutputFbp = kG310LogicalFbp;
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
                    std::vector<float> *depthReadback,
                    int depthReadY = 0, int depthReadRows = 0,
                    bool renderThenReadDepth = false)
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
            item.depthReadY = depthReadY;
            item.depthReadRows = depthReadRows;
            item.renderThenReadDepth = renderThenReadDepth;
            item.promise = promise;
            if (g299ProfileOn())
                item.g299Enqueue = std::chrono::steady_clock::now();
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
        // G309 one-pass five-source authoritative CT32 atlas composite.
        const std::vector<uint32_t> *g309PageMap = nullptr;
        const std::vector<uint32_t> *g309BasePixels = nullptr;
        std::vector<uint32_t> *g309Readback = nullptr;
        bool g309ForceDraw = false;
        uint32_t g309OutputFbp = kG309CompositeFbp;
        std::shared_ptr<std::promise<bool>> promise;
        std::chrono::steady_clock::time_point g299Enqueue{};
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
        if (!setupProgram() || (g256ExactRttOn() && !setupExactProgram()) ||
            (g281T8ViewRequested() && !setupT8ViewProgram()) ||
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
            const bool g299ProfileBatch =
                item.batch != nullptr && item.depthReadback == nullptr;
            const auto g299WorkStart =
                g299ProfileBatch && g299ProfileOn()
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
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
                                     item.depthReadY, item.depthReadRows);
            }
            catch (...)
            {
                ok = false;
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
        // 7..8 = G265 AREF code (0=1, 1=64, 2=127), 9 = G286 exact opaque sampler/combine,
        // 10 = G289 CT24 direct-owner source (force TEXA TA0=128),
        // 11 = G309 direct authoritative-page source selection.
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
            "void main(){\n"
            "    if ((uMode & 512) != 0) {\n"
            "        ivec4 vc = clamp(ivec4(floor(vColor * 255.0 + 0.5)), 0, 255);\n"
            "        vec2 uv = vSTQ.xy / max(vSTQ.z, 1e-8);\n"
            "        ivec4 tc = sampleExact(uv * vec2(textureSize(uTex, 0)));\n"
            "        if ((uMode & 1024) != 0) tc.a = 128;\n"
            "        ivec4 ec = combineExact(vc, tc);\n"
            "        if ((uMode & 64) != 0) {\n"
            "            int refCode = (uMode >> 7) & 3;\n"
            "            int refA = (refCode == 0) ? 1 : ((refCode == 1) ? 64 : 127);\n"
            "            if (ec.a < refA) discard;\n"
            "        }\n"
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
            "        vec4 t = ((uMode & 2048) != 0) ? g309Linear(uv) : texture(uTex, uv);\n"
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
        m_locLinear = m_f.glGetUniformLocation_(m_prog, "uLinear");
        m_locWrapU = m_f.glGetUniformLocation_(m_prog, "uWrapU");
        m_locWrapV = m_f.glGetUniformLocation_(m_prog, "uWrapV");
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
        return glGetError() == GL_NO_ERROR;
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

        while (glGetError() != GL_NO_ERROR) {}
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
            return glGetError() == GL_NO_ERROR;
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
        if (glGetError() != GL_NO_ERROR)
            return fail("draw");

        if (readback != nullptr)
        {
            readback->resize(512u * 512u);
            m_f.glActiveTexture_(GL158_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, out.colorTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          readback->data());
            if (glGetError() != GL_NO_ERROR)
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

        while (glGetError() != GL_NO_ERROR) {}
        if (m_g281MapTex == 0u)
            glGenTextures(1, &m_g281MapTex);
        if (m_g281ClutTex == 0u)
            glGenTextures(1, &m_g281ClutTex);
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281MapTex == 0u || m_g281ClutTex == 0u || m_g281Fbo == 0u)
            return fail("objects");

        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g281MapTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, mapRgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));

        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, m_g281ClutTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, 256, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, clutRgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));

        TexEntry &out = m_texCache[texKey];
        if (out.id == 0u)
        {
            glGenTextures(1, &out.id);
            if (out.id == 0u)
            {
                dropOutput();
                return fail("output");
            }
        }
        else
            m_texBytes -= static_cast<size_t>(out.w) * out.h * 4u;
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        out.w = texW;
        out.h = texH;
        out.lastUse = m_batchCounter + 1u;
        m_texBytes += static_cast<size_t>(texW) * texH * 4u;

        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, out.id, 0);
        if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
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
        if (glGetError() != GL_NO_ERROR)
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

    bool renderBatch(G178Batch &b, uint64_t depthKey, const std::vector<float> *depthUpload,
                     int depthY, int depthRows,
                     std::vector<float> *fusedDepthReadback = nullptr,
                     int depthReadY = 0, int depthReadRows = 0)
    {
        if (!validateBatchInputs(b, depthKey, depthUpload, depthY, depthRows))
            return false;
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
            const bool g286Exact = b.fbp == 0x13bu;
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
                        glBindTexture(GL_TEXTURE_2D, m_fbos.find(kSources[t])->second.colorTex);
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
                    glBindTexture(GL_TEXTURE_2D, src.colorTex);
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
                glBindTexture(GL_TEXTURE_2D, texture->id);
                // Newly created cache textures default to a mipmapped MIN filter while this
                // cache intentionally uploads level 0 only. The first G287 batch therefore
                // sampled the GL incomplete-texture value until another path happened to make
                // the object complete. Integer taps do not need filtering, but the sampler
                // object must still be complete from first use.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                mode |= 1 | ((d.tfx & 3) << 1) | (d.tcc ? 8 : 0);
                mode |= 512;
                m_f.glUniform1i_(m_locLinear, d.bilinear ? 1 : 0);
                m_f.glUniform1i_(m_locWrapU, static_cast<GLint>(d.wrapU));
                m_f.glUniform1i_(m_locWrapV, static_cast<GLint>(d.wrapV));
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
            m_e.glFramebufferRenderbuffer_(GL158_FRAMEBUFFER, GL178_DEPTH_ATTACHMENT,
                                           GL178_RENDERBUFFER, fbo.depthRb);
        return true;
    }

    GLFns m_f;
    G178GLExt m_e;
    GLuint m_prog = 0, m_progExact = 0, m_g281Prog = 0, m_g309Prog = 0;
    GLuint m_vao = 0, m_vbo = 0;
    GLuint m_g281MapTex = 0, m_g281ClutTex = 0, m_g281Fbo = 0;
    GLuint m_g309MapTex = 0, m_g309BaseTex = 0;
    GLint m_locFbSize = -1, m_locMode = -1, m_locLinear = -1;
    GLint m_locWrapU = -1, m_locWrapV = -1;
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

    // G305: one-slot move-owned async submit (default-off DC2_G305_ASYNC=1). A skip-readback
    // color wave is moved into m_asyncSlot so the front-end can reuse its s_batch immediately;
    // the backend renders it while the GS worker prepares the next flush. At most one packet in
    // flight (the front-end drains at every flush top before the next submit).
    G178Batch m_asyncSlot;
    std::future<bool> m_asyncFut;
    bool m_asyncPending = false;

    std::mutex m_residentMtx;
    std::unordered_set<uint64_t> m_resident;
    uint64_t m_g299Total = 0u;
    uint64_t m_g299Count[4]{};
    uint64_t m_g299QueueNs[4]{};
    uint64_t m_g299WorkNs[4]{};
    uint64_t m_g299PacketBytes[4]{};
    uint64_t m_g299Draws[4]{};
};
} // namespace

