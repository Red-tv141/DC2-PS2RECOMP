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
