// ==================================================================================================
// G714 — BATCHED GPU-RESIDENT RAW-VRAM LOCAL->LOCAL EXECUTION (backend half).
//
// Included INSIDE the G178Backend class body. Owns three compute programs and the persistent raw
// GS VRAM image they operate on (2048 x 512 R32UI, row p == guest page p — G630's representation,
// shared with `m_g629ColorTex` so the two prototypes never disagree about what raw VRAM is).
//
//   g714Copy    raw -> raw. ONE INVOCATION PER DESTINATION WORD, so a sub-word format (PSMT8's
//               four byte lanes per CT32 word) needs no atomics and has no intra-dispatch write
//               race. The per-PSM addressing is two CPU-PROBED permutation tables (G594's law:
//               never hand-derive a swizzle) — `fwd` texel->byte-offset and its inverse.
//   g714In      CT32 FBO -> raw. The exact inverse of G630's view: a resident target owns those
//               guest pages, so guest VRAM is stale there and the SOURCE must come off the GPU.
//               This is the half nothing in this tree had before G714.
//   g714Out     raw -> CT32 FBO. Byte-identical GLSL to G630's view program.
//
// Nothing here reads back to the CPU. The whole point of the phase is that an admitted transfer
// costs no `g261Materialize`, so a `glReadPixels` anywhere in this file would defeat it.
// ==================================================================================================

public:
    // Read from the GS front thread at ADMISSION time. Set once, on the GL worker, at backend
    // init — so a transfer can only ever be admitted when the programs and the raw image already
    // exist. Never let a front-end admission depend on state the worker may still be building:
    // by the time a submit failed the CPU copy has already been skipped, and the destination
    // bytes would simply be missing.
    bool g714Available() const { return m_g714Avail.load(std::memory_order_acquire); }

    bool setupG714Programs()
    {
        if (m_g714Ready)
            return true;
        if (m_g714Failed)
            return false;
        if (m_e.glDispatchCompute_ == nullptr || m_e.glBindImageTexture_ == nullptr ||
            m_e.glMemoryBarrier_ == nullptr)
        {
            m_g714Failed = true;
            return false;
        }

        // ---- raw -> raw same-format copy -----------------------------------------------------
        static const char *kCopyCS = R"GLSL(#version 430 core
layout(local_size_x=64, local_size_y=1, local_size_z=1) in;
layout(r32ui, binding=0) uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gFwd;
layout(r32ui, binding=2) readonly uniform uimage2D gInv;
uniform int uSbp, uDbp, uSppr, uDppr, uPw, uPh, uBpp;
uniform int uSsax, uSsay, uDsax, uDsay, uRrw, uRrh;
uniform int uPgX0, uPgY0, uPgNx, uPgNy;
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
void main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint pageIdx = gid >> 11u;          // 2048 words per 8 KiB page
    uint word    = gid & 2047u;
    if (pageIdx >= uint(uPgNx * uPgNy)) return;
    int pgx = uPgX0 + int(pageIdx % uint(uPgNx));
    int pgy = uPgY0 + int(pageIdx / uint(uPgNx));
    uint dstWord = ((uint(uDbp) * 256u + uint(pgy * uDppr + pgx) * 8192u) >> 2u) + word;
    int elems = 32 / uBpp;
    uint fieldMask = (uBpp == 32) ? 0xFFFFFFFFu : ((1u << uint(uBpp)) - 1u);
    uint acc = 0u, msk = 0u;
    for (int e = 0; e < elems; ++e)
    {
        uint t = imageLoad(gInv, dc2048(word * uint(elems) + uint(e))).r;
        int lx = int(t % uint(uPw));
        int ly = int(t / uint(uPw));
        int gx = pgx * uPw + lx;
        int gy = pgy * uPh + ly;
        if (gx < uDsax || gx >= uDsax + uRrw || gy < uDsay || gy >= uDsay + uRrh) continue;
        int col = gx - uDsax;
        int row = gy - uDsay;
        // TRXPOS reverses traversal at BOTH endpoints. For disjoint storage, each destination
        // coordinate therefore reads the same relative source coordinate in every direction.
        int sx = uSsax + col;
        int sy = uSsay + row;
        uint sOff = uint(uSbp) * 256u
                  + uint((sy / uPh) * uSppr + (sx / uPw)) * 8192u
                  + imageLoad(gFwd, dc2048(uint((sy % uPh) * uPw + (sx % uPw)))).r;
        sOff = sOff & 4194303u;
        uint sw = imageLoad(gVram, dc2048(sOff >> 2u)).r;
        uint v = (sw >> ((sOff & 3u) * 8u)) & fieldMask;
        acc |= v << uint(e * uBpp);
        msk |= fieldMask << uint(e * uBpp);
    }
    if (msk != 0u)
    {
        ivec2 p = dc2048(dstWord & 1048575u);
        uint d = imageLoad(gVram, p).r;
        imageStore(gVram, p, uvec4((d & ~msk) | (acc & msk), 0u, 0u, 0u));
    }
}
)GLSL";

        // ---- CT32 FBO -> raw (the inverse of G630's view) --------------------------------------
        static const char *kInCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) writeonly uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gPageMap;
layout(rgba8, binding=2) readonly uniform image2D gFbo;
layout(r32ui, binding=3) readonly uniform uimage2D gPageMask;
uniform int uBlock, uFbw, uWidth, uHeight, uOriginY, uRows, uMaskMode;
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
uint ct32Word(int x, int y)
{
    int page = (uBlock >> 5) + (y >> 5) * uFbw + (x >> 6);
    uint local = uint((y & 31) * 64 + (x & 63));
    return uint(page) * 2048u + imageLoad(gPageMap, ivec2(int(local), 0)).r;
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uWidth || p.y >= uRows) return;
    int y = p.y + uOriginY;
    uint addr = ct32Word(p.x, y) & 1048575u;
    if (uMaskMode != 0) {
        bool marked = imageLoad(gPageMask, ivec2(int(addr >> 11u), 0)).r != 0u;
        if ((uMaskMode == 1 && marked) || (uMaskMode == 2 && !marked)) return;
    }
    vec4 c = imageLoad(gFbo, ivec2(p.x, uHeight - 1 - y));
    uint w = uint(c.r * 255.0 + 0.5)
           | (uint(c.g * 255.0 + 0.5) << 8u)
           | (uint(c.b * 255.0 + 0.5) << 16u)
           | (uint(c.a * 255.0 + 0.5) << 24u);
    imageStore(gVram, dc2048(ct32Word(p.x, y) & 1048575u), uvec4(w, 0u, 0u, 0u));
}
)GLSL";

        // ---- raw -> CT32 FBO (G630's view program, verbatim) ----------------------------------
        static const char *kOutCS = R"GLSL(#version 430 core
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(r32ui, binding=0) readonly uniform uimage2D gVram;
layout(r32ui, binding=1) readonly uniform uimage2D gPageMap;
layout(rgba8, binding=2) writeonly uniform image2D gOut;
layout(r32ui, binding=3) readonly uniform uimage2D gPageMask;
uniform int uBlock, uFbw, uWidth, uHeight, uOriginY, uRows, uMaskMode;
ivec2 dc2048(uint i) { return ivec2(int(i & 2047u), int(i >> 11u)); }
uint ct32Word(int x, int y)
{
    int page = (uBlock >> 5) + (y >> 5) * uFbw + (x >> 6);
    uint local = uint((y & 31) * 64 + (x & 63));
    return uint(page) * 2048u + imageLoad(gPageMap, ivec2(int(local), 0)).r;
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uWidth || p.y >= uRows) return;
    int y = p.y + uOriginY;
    uint addr = ct32Word(p.x, y) & 1048575u;
    if (uMaskMode != 0) {
        bool marked = imageLoad(gPageMask, ivec2(int(addr >> 11u), 0)).r != 0u;
        if ((uMaskMode == 1 && marked) || (uMaskMode == 2 && !marked)) return;
    }
    uint w = imageLoad(gVram, dc2048(ct32Word(p.x, y) & 1048575u)).r;
    vec4 c = vec4(float(w & 255u), float((w >> 8u) & 255u),
                  float((w >> 16u) & 255u), float((w >> 24u) & 255u)) / 255.0;
    imageStore(gOut, ivec2(p.x, uHeight - 1 - y), c);
}
)GLSL";

        if (!g714BuildProgram(kCopyCS, m_g714CopyProg, "copy") ||
            !g714BuildProgram(kInCS, m_g714InProg, "fbo->raw") ||
            !g714BuildProgram(kOutCS, m_g714OutProg, "raw->fbo"))
        {
            m_g714Failed = true;
            return false;
        }
        m_locG714 = {};
        const char *kCopyU[] = {"uSbp", "uDbp", "uSppr", "uDppr", "uPw", "uPh", "uBpp",
                                "uSsax", "uSsay", "uDsax", "uDsay", "uRrw", "uRrh",
                                "uPgX0", "uPgY0", "uPgNx", "uPgNy"};
        for (int i = 0; i < 17; ++i)
        {
            m_locG714.copy[i] = m_f.glGetUniformLocation_(m_g714CopyProg, kCopyU[i]);
            if (m_locG714.copy[i] < 0)
            {
                std::fprintf(stderr, "[G714:backend] FAIL: copy uniform %s missing\n", kCopyU[i]);
                m_g714Failed = true;
                return false;
            }
        }
        const char *kViewU[] = {"uBlock", "uFbw", "uWidth", "uHeight", "uOriginY", "uRows", "uMaskMode"};
        for (int i = 0; i < 7; ++i)
        {
            m_locG714.in[i] = m_f.glGetUniformLocation_(m_g714InProg, kViewU[i]);
            m_locG714.out[i] = m_f.glGetUniformLocation_(m_g714OutProg, kViewU[i]);
            if (m_locG714.in[i] < 0 || m_locG714.out[i] < 0)
            {
                std::fprintf(stderr, "[G714:backend] FAIL: view uniform %s missing\n", kViewU[i]);
                m_g714Failed = true;
                return false;
            }
        }
        // The raw image is 4 MiB of GS local memory, allocated once and never resized.
        if (m_g629ColorTex == 0u || m_g629ColorRows != 512u)
        {
            std::vector<uint32_t> zero(static_cast<size_t>(2048) * 512, 0u);
            if (!g629Upload(m_g629ColorTex, m_g629ColorRows, 2048u, zero, m_g714Pad))
            {
                std::fprintf(stderr, "[G714:backend] FAIL: raw VRAM image allocation\n");
                m_g714Failed = true;
                return false;
            }
        }
        if (g158EnvFlag("DC2_G716_AUTHORITY") && !setupG630T8()) return false;
        m_g714Ready = true;
        std::fprintf(stderr, "[G714:backend] PASS: raw-VRAM local->local compute ready\n");
        return true;
    }

    bool g716SeedOnWorker(const G178Batch &b)
    {
        if (b.g716SeedPages.empty()) return true;
        if (!setupG714Programs() || !m_g714PageMapReady) return g714Fail("seed setup");
        std::vector<uint32_t> mask(512u, 0u);
        for (uint32_t pg : b.g716SeedPages) if (pg < 512) mask[pg] = 1;
        if (!g629Upload(m_g716MaskTex, m_g716MaskRows, 512u, mask, m_g714Pad)) return g714Fail("seed mask");
        m_e.glMemoryBarrier_(0xFFFFFFFFu);
        return g714ViewBand(m_g714OutProg, m_locG714.out, b.fbp,
                            b.g716SeedFbw, b.fbW, b.g716SeedLo, b.g716SeedHi, false, 2);
    }

    void g716TraceSurface(const G178Batch &b, GLuint colorTex, int phase)
    {
        static const bool on = g158EnvFlag("DC2_G716_TRACE");
        static const uint64_t limit = [] {
            const char *p = std::getenv("DC2_G716_TRACE_LIMIT");
            return p ? static_cast<uint64_t>(std::strtoull(p, nullptr, 10)) : 20000ull;
        }();
        // Key on the SCRIPT clock, not m_batchCounter: boot is I/O-timed, so equal batch indices
        // are NOT the same script instant across arms. Within one script frame the batch stream is
        // deterministic (the pixel gate's two controls are bit-identical), so (sf, index-in-sf)
        // is a by-construction cross-arm key. Window it so both arms cover the same script span.
        static const uint32_t sfLo = [] {
            const char *p = std::getenv("DC2_G716_TRACE_SF_LO");
            return p ? uint32_t(std::strtoul(p, nullptr, 10)) : 0u;
        }();
        static const uint32_t sfHi = [] {
            const char *p = std::getenv("DC2_G716_TRACE_SF_HI");
            return p ? uint32_t(std::strtoul(p, nullptr, 10)) : 0xFFFFFFFFu;
        }();
        if (!on || m_batchCounter > limit || b.draws.empty()) return;
        // Latch the key at phase 0 and reuse it at phase 1, so a script-frame edge landing between
        // the two phases of ONE batch cannot split that batch across two keys.
        if (!phase)
        {
            const uint32_t now = g_dc2ScriptFrame.load(std::memory_order_relaxed);
            if (now != m_g716TraceSf) { m_g716TraceSf = now; m_g716TraceIdx = 0u; }
            else ++m_g716TraceIdx;
        }
        const uint32_t sf = m_g716TraceSf, bi = m_g716TraceIdx;
        if (sf < sfLo || sf > sfHi) return;
        GLint savedTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);
        const auto hashTexture = [&](GLuint texture, int w, int h) {
            std::vector<uint32_t> pixels(size_t(w) * h);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            uint64_t hash = 14695981039346656037ull;
            for (uint32_t word : pixels) { hash ^= word; hash *= 1099511628211ull; }
            return hash;
        };
        const uint64_t surface = hashTexture(colorTex, b.fbW, b.fbH);
        uint64_t texture = 0;
        if (!phase && !m_g620DrawTex.empty() && m_g620DrawTex[0])
        {
            const auto &t = *m_g620DrawTex[0];
            texture = hashTexture(t.id, t.w, t.h);
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
        uint64_t vertices = 14695981039346656037ull;
        for (const auto &v : b.verts)
        {
            const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
            for (size_t i = 0; i < 29; ++i) { vertices ^= p[i]; vertices *= 1099511628211ull; }
        }
        const auto &d = b.draws.front();
        std::fprintf(stderr, "[G716:trace] sf=%u bi=%u phase=%d id=%llu fbp=%x w=%d up=%d raw=%zu draws=%zu verts=%zu vh=%llx fh=%llx th=%llx tk=%llx src=%x blend=%u rtta=%d\n",
            sf, bi, phase, (unsigned long long)m_batchCounter, b.fbp, b.fbW, int(b.uploadFb),
            b.g716SeedPages.size(), b.draws.size(), b.verts.size(),
            (unsigned long long)vertices, (unsigned long long)surface, (unsigned long long)texture,
            (unsigned long long)d.texKey, d.srcFbp, unsigned(d.blend), int(b.rttRawAlpha));
    }

    bool g714BuildProgram(const char *src, GLuint &prog, const char *what)
    {
        if (prog != 0u)
            return true;
        GLuint cs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL541_COMPUTE_SHADER, src, cs, log))
        {
            std::fprintf(stderr, "[G714:backend] FAIL: %s compile: %s\n", what, log.c_str());
            return false;
        }
        prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(prog, cs);
        m_f.glLinkProgram_(prog);
        GLint linked = 0;
        m_f.glGetProgramiv_(prog, GL158_LINK_STATUS, &linked);
        m_f.glDeleteShader_(cs);
        if (!linked)
        {
            GLint len = 0;
            m_f.glGetProgramiv_(prog, GL158_INFO_LOG_LENGTH, &len);
            std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
            m_f.glGetProgramInfoLog_(prog, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
            std::fprintf(stderr, "[G714:backend] FAIL: %s link: %s\n", what, buf.data());
            m_f.glDeleteProgram_(prog);
            prog = 0u;
            return false;
        }
        return true;
    }

    // One CT32 target row band <-> raw VRAM, in either direction. `rowLo/rowHi` are inclusive GS
    // rows and are always 32-aligned bands (whole GS page rows) so the conversion is page-exact.
    bool g714ViewBand(GLuint prog, const GLint *loc, uint32_t fbp, int fbw, int fbW,
                      int rowLo, int rowHi, bool toRaw, int maskMode = 0)
    {
        if (fbw <= 0 || fbW <= 0 || fbW > 512 || rowLo < 0 || rowHi >= 512 || rowHi < rowLo)
            return false;
        auto it = m_fbos.find(g349FboKey(fbp, fbW));
        if (it == m_fbos.end() || it->second.fbo == 0 || it->second.colorTex == 0 ||
            it->second.w != fbW || it->second.h != 512)
            return false;
        const FboEntry &e = it->second;
        // Rule 50/51: raw GS VRAM is a LOGICAL resource. A scaled FBO would have to be decimated
        // to enter it, which is not the identity, so refuse instead of guessing.
        if (e.physW != e.w || e.physH != e.h)
            return false;
        const int rows = rowHi - rowLo + 1;
        m_f.glUseProgram_(prog);
        m_e.glBindImageTexture_(0u, m_g629ColorTex, 0, GL_FALSE, 0,
                                toRaw ? GL178_WRITE_ONLY : GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g714PageMapTex, 0, GL_FALSE, 0,
                                GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(2u, e.colorTex, 0, GL_FALSE, 0,
                                toRaw ? GL542_READ_ONLY : GL178_WRITE_ONLY, GL158_RGBA8);
        m_f.glUniform1i_(loc[0], static_cast<GLint>(fbp << 5u));
        m_f.glUniform1i_(loc[1], fbw);
        m_f.glUniform1i_(loc[2], fbW);
        m_f.glUniform1i_(loc[3], 512);
        m_f.glUniform1i_(loc[4], rowLo);
        m_f.glUniform1i_(loc[5], rows);
        m_f.glUniform1i_(loc[6], maskMode);
        if (maskMode) m_e.glBindImageTexture_(3u, m_g716MaskTex, 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
        m_e.glDispatchCompute_(static_cast<GLuint>((fbW + 7) / 8),
                               static_cast<GLuint>((rows + 7) / 8), 1u);
        // The output surface returns to framebuffer draws/readback and deferred texture uploads.
        // Shader/texture-fetch barriers alone do not make image stores visible to those clients
        // (OpenGL 4.3, Memory Barriers). Keep raw-image ingestion scoped to shader consumers.
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_FETCH_BARRIER_BIT |
                             (toRaw ? 0u : (GL178_FRAMEBUFFER_BARRIER_BIT |
                                            GL178_TEXTURE_UPDATE_BARRIER_BIT)));
        return true;
    }

    // ⭐ Nothing waits on a G714 packet, so a silent failure would be INVISIBLE and would show up
    // only as missing destination bytes on screen (the front end has already skipped the CPU copy
    // by the time the worker runs). Every refusal below is therefore LOUD and counted.
    bool g714Fail(const char *stage)
    {
        const uint64_t n = ++m_g714Fails;
        if (n <= 16u || (n % 256u) == 0u)
            std::fprintf(stderr, "[G714:fail] n=%llu stage=%s\n",
                         static_cast<unsigned long long>(n), stage);
        return false;
    }

    bool runG714OnWorker(const G714L2lPacket &p)
    {
        if (!setupG714Programs())
            return g714Fail("setup");
        if (((p.g716Mode == 0u || p.g716Mode == 2u) && (p.pw == 0u || p.ph == 0u || p.bpp == 0u || p.rrw == 0u || p.rrh == 0u ||
            p.dPgNx == 0u || p.dPgNy == 0u)) || (p.fboIn.size() % 5u) != 0u ||
            (p.fboOut.size() % 5u) != 0u ||
            p.patchWords.size() != p.patchPages.size() * 2048u)
            return g714Fail("shape");
        while (glGetError() != GL_NO_ERROR) {}

        if (!p.ct32PageWords.empty())
        {
            if (p.ct32PageWords.size() != 64u * 32u ||
                !g629Upload(m_g714PageMapTex, m_g714PageMapRows, 2048u, p.ct32PageWords,
                            m_g714Pad))
                return g714Fail("pagemap");
            m_g714PageMapReady = true;
        }
        if (!m_g714PageMapReady)
            return g714Fail("nopagemap");
        if (p.tableKey != 0xFFFFFFFFu)
        {
            if (p.fwd.empty() || p.inv.size() != p.fwd.size() ||
                !g629Upload(m_g714FwdTex, m_g714FwdRows, 2048u, p.fwd, m_g714Pad) ||
                !g629Upload(m_g714InvTex, m_g714InvRows, 2048u, p.inv, m_g714Pad))
                return g714Fail("tables");
            m_g714TableKey = p.tableKey;
            m_g714FwdCpu = p.fwd;
        }
        if ((p.g716Mode == 0u || p.g716Mode == 2u) && m_g714TableKey == 0xFFFFFFFFu)
            return g714Fail("notables");

        // ⭐⭐⭐ G715: MAKE THE FBO'S OWN CONTENT VISIBLE TO `imageLoad` BEFORE INGESTING IT.
        // Step (1) reads every source/destination target's colour attachment through
        // `imageLoad`, and those attachments were last written by ORDINARY FRAMEBUFFER DRAWS on
        // this same worker. In OpenGL, a framebuffer write is NOT automatically visible to a
        // later shader image access: the reader must be preceded by
        // `glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT)` (the bit names the access AFTER
        // the barrier that has to observe prior writes). G714 issued its barriers only AFTER each
        // dispatch, so the very first read of the frame's freshly drawn characters could return
        // stale texels — which is exactly the defect the pixel gate sees: low amplitude, confined
        // to the moving models, and INVISIBLE to the raw->raw oracle, because a stale ingest
        // produces a raw image the copy then transforms perfectly.
        // ⛔ This is the phase's own rule turned on the ingest itself: "an oracle proves a
        // TRANSFORM, never the PROVENANCE of its input."
        // ⭐ GL_ALL_BARRIER_BITS deliberately. This runs ~11 times per frame (`[G713:l2l]`), so its
        // cost is irrelevant, and a full barrier removes the whole "was that the right bit" class of
        // doubt in one line. `DC2_G715_SYNCIN=1` escalates to the sledgehammer: if a glFinish here
        // is correct and the barrier is not, the defect is still visibility and not logic.
        m_e.glMemoryBarrier_(0xFFFFFFFFu);
        if (g715SyncInOn())
            glFinish();

        if (p.g716Mode != 0u)
        {
            uint64_t calls = 0;
            if (!g631UploadPatchPages(m_g629ColorTex, p.patchPages, p.patchWords, calls)) return g714Fail("authority patch");
            std::vector<uint32_t> mask(512u, 0u);
            for (uint32_t page : p.g716Protected) if (page < 512) mask[page] = 1;
            if (!g629Upload(m_g716MaskTex, m_g716MaskRows, 512u, mask, m_g714Pad)) return g714Fail("authority mask");
            m_e.glMemoryBarrier_(0xFFFFFFFFu);
        }

        // (1) Ingest the resident bands. Their guest-owned pages are overlaid below, matching
        // the CPU publication's FBO-unpack followed by G536 page preservation.
        for (size_t i = 0; i < p.fboIn.size(); i += 5u)
        {
            if (!g714ViewBand(m_g714InProg, m_locG714.in,
                              static_cast<uint32_t>(p.fboIn[i]), p.fboIn[i + 1],
                              p.fboIn[i + 2], p.fboIn[i + 3], p.fboIn[i + 4], true, p.g716Mode ? 1 : 0))
                return g714Fail("fboIn");
            ++m_g714InBands;
        }

        if (p.g716Mode == 1u) return true;
        if (p.g716Mode == 3u)
        {
            for (size_t i = 0; i < p.fboOut.size(); i += 5u)
                if (!g714ViewBand(m_g714OutProg, m_locG714.out, p.fboOut[i], p.fboOut[i + 1],
                                  p.fboOut[i + 2], p.fboOut[i + 3], p.fboOut[i + 4], false, 2))
                    return g714Fail("raw alias restore");
            return true;
        }

        // G715 identity bisect: ingest, write straight back, stop. Nothing else in this function
        // has run and nothing else will, so the only change against the control is one FBO->raw->FBO
        // round trip per admitted band.
        if (p.identOnly != 0u)
        {
            m_e.glMemoryBarrier_(0xFFFFFFFFu);
            for (size_t i = 0; i < p.fboOut.size(); i += 5u)
            {
                if (!g714ViewBand(m_g714OutProg, m_locG714.out,
                                  static_cast<uint32_t>(p.fboOut[i]), p.fboOut[i + 1],
                                  p.fboOut[i + 2], p.fboOut[i + 3], p.fboOut[i + 4], false))
                    return g714Fail("identOut");
                ++m_g714OutBands;
            }
            ++m_g714Copies;
            if (g714StatBackendOn() && (m_g714Copies <= 4u || (m_g714Copies % 4096u) == 0u))
                std::fprintf(stderr,
                             "[G714:gpu] copies=%llu guestPages=0 inBands=%llu outBands=%llu "
                             "fails=%llu\n",
                             static_cast<unsigned long long>(m_g714Copies),
                             static_cast<unsigned long long>(m_g714InBands),
                             static_cast<unsigned long long>(m_g714OutBands),
                             static_cast<unsigned long long>(m_g714Fails));
            return glGetError() == GL_NO_ERROR ? true : g714Fail("identGlError");
        }

        // (2) Apply the authoritative guest pages AFTER ingestion. Image stores require a
        // texture-update barrier before glTexSubImage2D can replace their contents.
        m_e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                             GL178_TEXTURE_UPDATE_BARRIER_BIT);
        uint64_t patchCalls = 0u;
        if (p.g716Mode == 0u && !p.patchPages.empty() &&
            !g631UploadPatchPages(m_g629ColorTex, p.patchPages, p.patchWords, patchCalls))
            return g714Fail("patch");
        m_g714Pages += p.patchPages.size();
        // ⭐ And the reverse direction of the same rule: `g631UploadPatchPages` writes the raw
        // image with `glTexSubImage2D`, and step (3) reads those very words back through
        // `imageLoad`. The barrier above only ordered the ingest's image STORES against the
        // upload; nothing ordered the upload against the copy's image LOADS.
        if (!p.patchPages.empty())
        {
            m_e.glMemoryBarrier_(0xFFFFFFFFu);
            if (g715SyncInOn())
                glFinish();
        }

        // ---- oracle, half 1: raw VRAM as the copy sees it, i.e. after the ingest ----------------
        const bool verify = g714VerifyOn();
        uint32_t vPgLo = 0u, vPgHi = 0u;
        std::vector<uint32_t> rawBefore;
        if (verify && g714VerifyRange(p, vPgLo, vPgHi))
        {
            if (!readG630ImageOnWorker(false, rawBefore, vPgLo, vPgHi - vPgLo + 1u) ||
                rawBefore.size() != static_cast<size_t>(vPgHi - vPgLo + 1u) * 2048u)
                rawBefore.clear();
        }

        // (3) the copy itself.
        m_f.glUseProgram_(m_g714CopyProg);
        m_e.glBindImageTexture_(0u, m_g629ColorTex, 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
        m_e.glBindImageTexture_(1u, m_g714FwdTex, 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
        m_e.glBindImageTexture_(2u, m_g714InvTex, 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
        const GLint cu[17] = {
            static_cast<GLint>(p.sbp), static_cast<GLint>(p.dbp),
            static_cast<GLint>(p.sPagesPerRow), static_cast<GLint>(p.dPagesPerRow),
            static_cast<GLint>(p.pw), static_cast<GLint>(p.ph), static_cast<GLint>(p.bpp),
            static_cast<GLint>(p.ssax), static_cast<GLint>(p.ssay),
            static_cast<GLint>(p.dsax), static_cast<GLint>(p.dsay),
            static_cast<GLint>(p.rrw), static_cast<GLint>(p.rrh),
            static_cast<GLint>(p.dPgX0), static_cast<GLint>(p.dPgY0),
            static_cast<GLint>(p.dPgNx), static_cast<GLint>(p.dPgNy)};
        for (int i = 0; i < 17; ++i)
            m_f.glUniform1i_(m_locG714.copy[i], cu[i]);
        const uint32_t invocations = p.dPgNx * p.dPgNy * 2048u;
        m_e.glDispatchCompute_(static_cast<GLuint>((invocations + 63u) / 64u), 1u, 1u);
        // Full barrier: the next reader may be the raw->FBO view (image load), the oracle, or
        // VRAMDST's `readG630ImageOnWorker`, which attaches the raw texture to an FBO and reads
        // pixels — a framebuffer client, not a shader one.
        m_e.glMemoryBarrier_(0xFFFFFFFFu);

        // ---- oracle, half 2: the copy kernel against a CPU reference built from the SAME raw
        // bytes and the SAME shipped permutation table. Every element the transfer moves is
        // compared; a non-trivial denominator is printed beside `bad` so `bad=0 checks=0` can
        // never masquerade as a pass (G629's law).
        if (verify && !rawBefore.empty())
            g714VerifyCopy(p, rawBefore, vPgLo, vPgHi);

        // ⭐⭐⭐ G715 NOOUT: keep the ingest, the patch and the copy, and DO NOT publish the result
        // into the destination FBO. The transfer's bytes then reach nothing at all — guest VRAM is
        // stale by design and the FBO is untouched. Three outcomes, each conclusive:
        //   unchanged from the armed arm -> the FBO write is not what moves those pixels;
        //   worse                        -> the FBO write is carrying real content;
        //   BETTER                       -> writing the copy into the FBO is itself the defect,
        //                                   i.e. the destination's rendered colour must survive and
        //                                   the l2l bytes belong in guest VRAM (which is exactly
        //                                   what the shipped tag + G536 guard arrange).
        // G715 VRAMDST: hand the destination pages back instead of publishing them into the FBO.
        if (p.dstOut != nullptr && p.dstPgCount != 0u)
        {
            if (!readG630ImageOnWorker(false, *p.dstOut, p.dstPgLo, p.dstPgCount) ||
                p.dstOut->size() != static_cast<size_t>(p.dstPgCount) * 2048u)
                return g714Fail("dstRead");
        }
        if (p.noFboOut != 0u || g715NoOutOn())
        {
            ++m_g714Copies;
            if (g714StatBackendOn() && (m_g714Copies <= 4u || (m_g714Copies % 4096u) == 0u))
                std::fprintf(stderr,
                             "[G714:gpu] copies=%llu guestPages=%llu inBands=%llu outBands=0 "
                             "fails=%llu\n",
                             static_cast<unsigned long long>(m_g714Copies),
                             static_cast<unsigned long long>(m_g714Pages),
                             static_cast<unsigned long long>(m_g714InBands),
                             static_cast<unsigned long long>(m_g714Fails));
            return glGetError() == GL_NO_ERROR ? true : g714Fail("noOutGlError");
        }

        // (4) raw -> FBO for the rows the copy changed.
        for (size_t i = 0; i < p.fboOut.size(); i += 5u)
        {
            if (!g714ViewBand(m_g714OutProg, m_locG714.out,
                              static_cast<uint32_t>(p.fboOut[i]), p.fboOut[i + 1],
                              p.fboOut[i + 2], p.fboOut[i + 3], p.fboOut[i + 4], false))
                return g714Fail("fboOut");
            ++m_g714OutBands;
        }
        if (glGetError() != GL_NO_ERROR)
            return g714Fail("glerror");
        g715VerifyRoundTrip(p);
        ++m_g714Copies;
        if (g714StatBackendOn() && (m_g714Copies <= 4u || (m_g714Copies % 4096u) == 0u))
            std::fprintf(stderr,
                         "[G714:gpu] copies=%llu guestPages=%llu inBands=%llu outBands=%llu "
                         "fails=%llu\n",
                         static_cast<unsigned long long>(m_g714Copies),
                         static_cast<unsigned long long>(m_g714Pages),
                         static_cast<unsigned long long>(m_g714InBands),
                         static_cast<unsigned long long>(m_g714OutBands),
                         static_cast<unsigned long long>(m_g714Fails));
        return true;
    }

    // ⭐⭐⭐ G715: THE ORACLE G714 NEVER HAD — `g714Out` FOLLOWED BY `g714In` MUST BE THE IDENTITY.
    //
    // G714's `[G714:verify]` compares raw->raw and passes 587,693,273 elements with `bad=0`. It is
    // scoped to the COPY KERNEL and is blind to the two legs that actually cross the FBO boundary.
    // Those legs are what the pixel gate implicates: the defect tracks the number of GPU copies
    // (79,872 copies -> p50 2.75; 73,728 -> 2.75; 16,384 -> 0.00/max 0.17) and survives a
    // population-preserving `g261Materialize` (MATKEEP: 82.38% taken, p50 2.75), so it is not the
    // publication and not the copy.
    //
    // The check: raw was just written INTO the FBO by `g714Out`. Running `g714In` on the same band
    // must therefore reproduce the raw image byte for byte. Any difference is the FBO<->raw round
    // trip failing — a lossy format conversion, a wrong row/flip mapping, or a band whose FBO is not
    // what the write-back left. Diagnostic only (two full raw readbacks per band); never in a gate.
    void g715VerifyRoundTrip(const G714L2lPacket &p)
    {
        if (!g715RoundTripVfyOn() || p.fboOut.empty())
            return;
        for (size_t i = 0; i + 4 < p.fboOut.size(); i += 5u)
        {
            const uint32_t fbp = static_cast<uint32_t>(p.fboOut[i]);
            const int fbw = p.fboOut[i + 1];
            const int rowLo = p.fboOut[i + 3], rowHi = p.fboOut[i + 4];
            if (fbw <= 0 || rowLo < 0 || rowHi < rowLo || rowHi >= 512)
                continue;
            const uint32_t pgLo = fbp + static_cast<uint32_t>(rowLo / 32) *
                                            static_cast<uint32_t>(fbw);
            const uint32_t pgHi = fbp + static_cast<uint32_t>(rowHi / 32) *
                                            static_cast<uint32_t>(fbw) +
                                  static_cast<uint32_t>(fbw) - 1u;
            if (pgHi < pgLo || pgHi >= 512u)
                continue;
            const uint32_t nPg = pgHi - pgLo + 1u;
            std::vector<uint32_t> a, b;
            if (!readG630ImageOnWorker(false, a, pgLo, nPg) ||
                a.size() != static_cast<size_t>(nPg) * 2048u)
                continue;
            if (!g714ViewBand(m_g714InProg, m_locG714.in, fbp, fbw, p.fboOut[i + 2],
                              rowLo, rowHi, true))
                continue;
            m_e.glMemoryBarrier_(0xFFFFFFFFu);
            if (!readG630ImageOnWorker(false, b, pgLo, nPg) || b.size() != a.size())
                continue;
            uint64_t bad = 0u;
            for (size_t w = 0; w < a.size(); ++w)
                if (a[w] != b[w])
                    ++bad;
            m_g715RtWords += a.size();
            m_g715RtBad += bad;
            ++m_g715RtBands;
            if (bad != 0u && m_g715RtReports < 16u)
            {
                ++m_g715RtReports;
                std::fprintf(stderr,
                             "[G715:rtvfy-BAD] fbp=0x%x fbw=%d rows=%d..%d pages=%u..%u bad=%llu"
                             " of %llu\n",
                             fbp, fbw, rowLo, rowHi, pgLo, pgHi,
                             static_cast<unsigned long long>(bad),
                             static_cast<unsigned long long>(a.size()));
            }
        }
        if ((m_g715RtBands % 512u) == 0u && m_g715RtBands != 0u)
        {
            std::fprintf(stderr, "[G715:rtvfy] bands=%llu words=%llu bad=%llu\n",
                         static_cast<unsigned long long>(m_g715RtBands),
                         static_cast<unsigned long long>(m_g715RtWords),
                         static_cast<unsigned long long>(m_g715RtBad));
            std::fflush(stderr);
        }
    }
    uint64_t m_g715RtBands = 0u, m_g715RtWords = 0u, m_g715RtBad = 0u, m_g715RtReports = 0u;

    // Fire-and-forget: nothing waits on this item, exactly like G264's mirror. Its ORDER against
    // every other GL item is what carries the correctness argument, and the single FIFO worker
    // gives that for free.
    //
    // ⭐ G715 VRAMDST needs the destination words back before the front end may continue, so it
    // takes the SAME queue slot and waits on the promise, exactly like `readColor`. Correctness
    // first: this reintroduces a round trip, and shrinking it back out is a separate, measurable
    // step once the image is proven.
    bool prepareG714L2l(const std::shared_ptr<G714L2lPacket> &p)
    {
        if (!p || !m_ready.load(std::memory_order_acquire))
            return false;
        auto promise = std::make_shared<std::promise<bool>>();
        std::future<bool> future = promise->get_future();
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g714L2l = p;
            item.promise = promise;
            m_queue.push_back(std::move(item));
            m_g630Futures.push_back(std::move(future));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }

    // ⛔⛔⛔ G715: THE BLOCKING FORM MUST USE `SyncWait`, NOT A BARE FUTURE.
    // G705 is promoted and DEFAULT-ON, so in the shipping configuration there is **no worker
    // thread**: the GL context is adopted by the GS front end and queue items run INLINE when the
    // owner drains. A bare `future.get()` therefore blocks the one thread that could ever execute
    // the item — which is exactly what the first VRAMDST arm did, and why its capture contains a
    // single `[G714:gpu] ... READY` line and not one completed copy. `SyncWait::wait()` pumps the
    // queue inline through `g_g705Pump` in fused mode and falls back to `g447WaitFuture` otherwise,
    // which is why `readColor` has always worked from this same thread.
    bool prepareG714L2lSync(const std::shared_ptr<G714L2lPacket> &p)
    {
        if (!p || !m_ready.load(std::memory_order_acquire))
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g714L2l = p;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        G447WaitScope g447Scope(1);
        return wait.wait();
    }

private:
    // ---- the same-run exactness oracle (DC2_G714_VERIFY=1; never in a timing run) ---------------
    static bool g714VerifyOn()
    {
        static const bool on = g158EnvFlag("DC2_G714_VERIFY");
        return on;
    }

    // Guest page span covering both endpoints, so one band read serves the whole check.
    bool g714VerifyRange(const G714L2lPacket &p, uint32_t &lo, uint32_t &hi) const
    {
        const uint32_t sPg0 = (p.sbp >> 5u) + (p.ssay / p.ph) * p.sPagesPerRow + p.ssax / p.pw;
        const uint32_t sPg1 = (p.sbp >> 5u) +
                              ((p.ssay + p.rrh - 1u) / p.ph) * p.sPagesPerRow +
                              (p.ssax + p.rrw - 1u) / p.pw;
        const uint32_t dPg0 = (p.dbp >> 5u) + p.dPgY0 * p.dPagesPerRow + p.dPgX0;
        const uint32_t dPg1 = (p.dbp >> 5u) + (p.dPgY0 + p.dPgNy - 1u) * p.dPagesPerRow +
                              (p.dPgX0 + p.dPgNx - 1u);
        lo = std::min(std::min(sPg0, sPg1), std::min(dPg0, dPg1));
        hi = std::max(std::max(sPg0, sPg1), std::max(dPg0, dPg1));
        return hi < 512u && hi >= lo && (hi - lo) < 512u;
    }

    void g714VerifyCopy(const G714L2lPacket &p, const std::vector<uint32_t> &before,
                        uint32_t pgLo, uint32_t pgHi)
    {
        if (m_g714FwdCpu.size() != static_cast<size_t>(p.pw) * p.ph)
            return;
        std::vector<uint32_t> after;
        if (!readG630ImageOnWorker(false, after, pgLo, pgHi - pgLo + 1u) ||
            after.size() != before.size())
            return;
        const uint64_t base = static_cast<uint64_t>(pgLo) * 8192ull;
        const uint64_t end = base + static_cast<uint64_t>(before.size()) * 4ull;
        const uint32_t fieldMask =
            (p.bpp >= 32u) ? 0xFFFFFFFFu : ((1u << p.bpp) - 1u);
        const auto elem = [&](const std::vector<uint32_t> &v, uint64_t addr) -> uint32_t {
            const size_t w = static_cast<size_t>((addr - base) >> 2u);
            return (v[w] >> ((addr & 3ull) * 8ull)) & fieldMask;
        };
        for (uint32_t row = 0; row < p.rrh; ++row)
            for (uint32_t col = 0; col < p.rrw; ++col)
            {
                const uint32_t sc = (p.revX != 0u) ? (p.rrw - 1u - col) : col;
                const uint32_t sr = (p.revY != 0u) ? (p.rrh - 1u - row) : row;
                const uint32_t sx = p.ssax + sc, sy = p.ssay + sr;
                // Match the shipped CPU loop: transferCoord is applied at both endpoints.
                // Keeping traversal here makes the oracle independent of the shader's gather.
                const uint32_t dx = p.dsax + sc, dy = p.dsay + sr;
                const uint64_t sAddr =
                    static_cast<uint64_t>(p.sbp) * 256ull +
                    (static_cast<uint64_t>(sy / p.ph) * p.sPagesPerRow + sx / p.pw) * 8192ull +
                    m_g714FwdCpu[(sy % p.ph) * p.pw + (sx % p.pw)];
                const uint64_t dAddr =
                    static_cast<uint64_t>(p.dbp) * 256ull +
                    (static_cast<uint64_t>(dy / p.ph) * p.dPagesPerRow + dx / p.pw) * 8192ull +
                    m_g714FwdCpu[(dy % p.ph) * p.pw + (dx % p.pw)];
                if (sAddr < base || sAddr + 4ull > end || dAddr < base || dAddr + 4ull > end)
                    continue;
                const uint32_t want = elem(before, sAddr);
                const uint32_t have = elem(after, dAddr);
                ++m_g714VfyChecks;
                if (want != have)
                {
                    const uint64_t bad = ++m_g714VfyBad;
                    if (bad <= 16u || (bad % 65536u) == 0u)
                        std::fprintf(stderr,
                                     "[G714:vfy-BAD] bad=%llu checks=%llu psm=%u col=%u row=%u "
                                     "sAddr=%llu dAddr=%llu want=0x%x have=0x%x\n",
                                     static_cast<unsigned long long>(bad),
                                     static_cast<unsigned long long>(m_g714VfyChecks),
                                     p.bpp, col, row,
                                     static_cast<unsigned long long>(sAddr),
                                     static_cast<unsigned long long>(dAddr), want, have);
                }
            }
        if ((++m_g714VfyRuns % 512u) == 0u)
            std::fprintf(stderr, "[G714:verify] runs=%llu checks=%llu bad=%llu\n",
                         static_cast<unsigned long long>(m_g714VfyRuns),
                         static_cast<unsigned long long>(m_g714VfyChecks),
                         static_cast<unsigned long long>(m_g714VfyBad));
    }

    struct G714Locs
    {
        GLint copy[17] = {};
        GLint in[7] = {};
        GLint out[7] = {};
    };
    G714Locs m_locG714{};
    GLuint m_g714CopyProg = 0u, m_g714InProg = 0u, m_g714OutProg = 0u;
    GLuint m_g714PageMapTex = 0u, m_g714FwdTex = 0u, m_g714InvTex = 0u;
    uint32_t m_g714PageMapRows = 0u, m_g714FwdRows = 0u, m_g714InvRows = 0u;
    uint32_t m_g714TableKey = 0xFFFFFFFFu;
    GLuint m_g716MaskTex = 0; uint32_t m_g716MaskRows = 0;
    uint32_t m_g716TraceSf = 0xFFFFFFFFu, m_g716TraceIdx = 0u; // script-clock trace key
    std::vector<uint32_t> m_g714Pad;
    bool m_g714Ready = false, m_g714Failed = false, m_g714PageMapReady = false;
    std::atomic<bool> m_g714Avail{false};
    uint64_t m_g714Copies = 0u, m_g714Pages = 0u, m_g714InBands = 0u, m_g714OutBands = 0u;
    uint64_t m_g714Fails = 0u;
    // CPU mirror of the permutation the GPU is using, so the oracle's reference is computed from
    // the very table the kernel indexes rather than from a second derivation of the swizzle.
    std::vector<uint32_t> m_g714FwdCpu;
    uint64_t m_g714VfyRuns = 0u, m_g714VfyChecks = 0u, m_g714VfyBad = 0u;
