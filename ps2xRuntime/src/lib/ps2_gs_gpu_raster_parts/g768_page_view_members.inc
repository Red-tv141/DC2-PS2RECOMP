public: // G768: the bridge functions (gpu_raster_bridge_and_stubs.inc) call the four entry points below
    // ===== G768 (members): THE GENERAL GPU PAGE VIEW — every page-periodic PS2 storage format read
    // =====   from the resident FBOs and a persistent GPU mirror of guest VRAM, CLUT included =====
    // See g768_view_packet.inc for the packet contract and g768_page_view.inc for the front end.
    // Execution is FIFO-ordered on this worker like every other job: producer pages -> (mirror
    // uploads) -> CLUT pass -> view pass -> the consumer batch that samples the view. No waiter: a
    // failed build drops the view's texture, so its consumer misses loudly (batch -> CPU fallback).
    GLuint m_g768Prog = 0u, m_g768ClutProg = 0u, m_g768Mirror = 0u, m_g768ClutTex = 0u,
           m_g768EntTex = 0u;
    GLuint m_g768Lut[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    bool m_g768Failed = false;
    uint64_t m_g768ClutSig = 0u;
    std::atomic<uint64_t> m_g768MirrorEpoch{0u};
    std::atomic<uint64_t> m_g768ExecSerial{0u}; // last packet serial fully processed (ok or failed)
    GLint m_locG768Fmt = -1, m_locG768PgX = -1, m_locG768PgY = -1, m_locG768SrcX = -1,
          m_locG768SrcY = -1, m_locG768Fbo = -1, m_locG768Scale = -1, m_locG768Centre = -1,
          m_locG768Ta0 = -1, m_locG768Ta1 = -1, m_locG768Aem = -1;
    GLint m_locG768CFbo[4] = {-1, -1, -1, -1};
    GLint m_locG768CScale = -1, m_locG768CCentre = -1, m_locG768CTa0 = -1,
          m_locG768CTa1 = -1, m_locG768CAem = -1;
    std::atomic<uint64_t> m_g768Builds{0u}, m_g768Partials{0u}, m_g768Fails{0u},
        m_g768MirPages{0u}, m_g768ClutPasses{0u}, m_g768ClutReuse{0u}, m_g768Rects{0u};

    uint64_t g768MirrorEpoch() const { return m_g768MirrorEpoch.load(std::memory_order_acquire); }
    uint64_t g768ExecSerial() const { return m_g768ExecSerial.load(std::memory_order_acquire); }

    void g768Report()
    {
        std::fprintf(stderr,
                     "[G768:backend] builds=%llu partial=%llu fails=%llu rects=%llu mirPages=%llu "
                     "clutPasses=%llu clutReuse=%llu mirrorEpoch=%llu\n",
                     (unsigned long long)m_g768Builds.load(), (unsigned long long)m_g768Partials.load(),
                     (unsigned long long)m_g768Fails.load(), (unsigned long long)m_g768Rects.load(),
                     (unsigned long long)m_g768MirPages.load(),
                     (unsigned long long)m_g768ClutPasses.load(),
                     (unsigned long long)m_g768ClutReuse.load(),
                     (unsigned long long)m_g768MirrorEpoch.load());
    }

    GLuint g768LinkProgram(const char *vsSrc, const char *fsSrc, const char *what)
    {
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, vsSrc, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, fsSrc, fs, log))
        {
            if (vs != 0u)
                m_f.glDeleteShader_(vs);
            std::fprintf(stderr, "[G768:view] FAIL: compile %s: %s\n", what, log.c_str());
            return 0u;
        }
        GLuint prog = m_f.glCreateProgram_();
        m_f.glAttachShader_(prog, vs);
        m_f.glAttachShader_(prog, fs);
        m_f.glLinkProgram_(prog);
        m_f.glDeleteShader_(vs);
        m_f.glDeleteShader_(fs);
        GLint ok = 0;
        m_f.glGetProgramiv_(prog, GL158_LINK_STATUS, &ok);
        if (!ok)
        {
            m_f.glDeleteProgram_(prog);
            std::fprintf(stderr, "[G768:view] FAIL: link %s\n", what);
            return 0u;
        }
        return prog;
    }

    bool g768SetupPrograms()
    {
        if (m_g768Prog != 0u && m_g768ClutProg != 0u)
            return true;
        if (m_g768Failed)
            return false;
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        // Shared colour conversions: exactly GSRasterizer::decodePSMCT16 + applyTexa.
        static const char *kConv =
            "uniform int uTa0;\n"
            "uniform int uTa1;\n"
            "uniform int uAem;\n"
            "vec4 ct16(int v){\n"
            "    int r = (v & 31) << 3; int g = ((v >> 5) & 31) << 3; int b = ((v >> 10) & 31) << 3;\n"
            "    int a = ((v & 0x8000) != 0) ? uTa1 : ((uAem != 0 && r == 0 && g == 0 && b == 0) ? 0 : uTa0);\n"
            "    return vec4(float(r), float(g), float(b), float(a)) * (1.0 / 255.0);\n"
            "}\n"
            "vec4 conv(ivec4 b, int mode){\n"
            "    if (mode == 0) return vec4(b) * (1.0 / 255.0);\n"
            "    if (mode == 1) {\n"
            "        int a = (uAem != 0 && b.r == 0 && b.g == 0 && b.b == 0) ? 0 : uTa0;\n"
            "        return vec4(float(b.r), float(b.g), float(b.b), float(a)) * (1.0 / 255.0);\n"
            "    }\n"
            "    return ct16(mode == 2 ? (b.r | (b.g << 8)) : (b.b | (b.a << 8)));\n"
            "}\n";
        const std::string viewFs = std::string(
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform sampler2D uLut;\n"
            "uniform sampler2D uClut;\n"
            "uniform int uFmt;\n"
            "uniform int uPgX;\n"
            "uniform int uPgY;\n"
            "uniform int uSrcX;\n"
            "uniform int uSrcY;\n"
            "uniform int uFbo;\n"
            "uniform int uScale;\n"
            "uniform int uCentre;\n") + kConv +
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec4 l = ivec4(floor(texelFetch(uLut, p - ivec2(uPgX, uPgY), 0) * 255.0 + 0.5));\n"
            "    ivec2 s = ivec2(uSrcX, uSrcY) + l.xy;\n"
            "    vec4 c = (uFbo != 0)\n"
            "        ? texelFetch(uSrc, ivec2(s.x, 511 - s.y) * uScale + ivec2(uCentre), 0)\n"
            "        : texelFetch(uSrc, s, 0);\n"
            "    ivec4 b = clamp(ivec4(floor(c * 255.0 + 0.5)), 0, 255);\n"
            "    int bit = l.z;\n"
            "    int sel = bit >> 3;\n"
            "    int v8 = (sel == 0) ? b.r : ((sel == 1) ? b.g : ((sel == 2) ? b.b : b.a));\n"
            "    if (uFmt == 1) o = texelFetch(uClut, ivec2(v8, 0), 0);\n"
            "    else if (uFmt == 2) o = texelFetch(uClut, ivec2((v8 >> (bit & 7)) & 15, 0), 0);\n"
            "    else if (uFmt == 3) o = conv(b, 0);\n"
            "    else if (uFmt == 4) o = conv(b, 1);\n"
            "    else o = ct16((bit == 0) ? (b.r | (b.g << 8)) : (b.b | (b.a << 8)));\n"
            "}\n";
        const std::string clutFs = std::string(
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uEnt;\n"
            "uniform sampler2D uS0;\n"
            "uniform sampler2D uS1;\n"
            "uniform sampler2D uS2;\n"
            "uniform sampler2D uS3;\n"
            "uniform int uFbo0;\n"
            "uniform int uFbo1;\n"
            "uniform int uFbo2;\n"
            "uniform int uFbo3;\n"
            "uniform int uScale;\n"
            "uniform int uCentre;\n") + kConv +
            "void main(){\n"
            "    ivec4 e = ivec4(floor(texelFetch(uEnt, ivec2(int(gl_FragCoord.x), 0), 0) + 0.5));\n"
            "    if (e.x < 0) discard;\n"
            "    int f = (e.x == 0) ? uFbo0 : ((e.x == 1) ? uFbo1 : ((e.x == 2) ? uFbo2 : uFbo3));\n"
            "    ivec2 q = (f != 0) ? ivec2(e.y, 511 - e.z) * uScale + ivec2(uCentre) : e.yz;\n"
            "    vec4 c = (e.x == 0) ? texelFetch(uS0, q, 0) : ((e.x == 1) ? texelFetch(uS1, q, 0)\n"
            "           : ((e.x == 2) ? texelFetch(uS2, q, 0) : texelFetch(uS3, q, 0)));\n"
            "    o = conv(clamp(ivec4(floor(c * 255.0 + 0.5)), 0, 255), e.w);\n"
            "}\n";
        if (m_g768Prog == 0u)
            m_g768Prog = g768LinkProgram(kVS, viewFs.c_str(), "view");
        if (m_g768ClutProg == 0u && m_g768Prog != 0u)
            m_g768ClutProg = g768LinkProgram(kVS, clutFs.c_str(), "clut");
        if (m_g768Prog == 0u || m_g768ClutProg == 0u)
        {
            m_g768Failed = true;
            return false;
        }
        m_f.glUseProgram_(m_g768Prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768Prog, "uSrc"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768Prog, "uLut"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768Prog, "uClut"), 2);
        m_locG768Fmt = m_f.glGetUniformLocation_(m_g768Prog, "uFmt");
        m_locG768PgX = m_f.glGetUniformLocation_(m_g768Prog, "uPgX");
        m_locG768PgY = m_f.glGetUniformLocation_(m_g768Prog, "uPgY");
        m_locG768SrcX = m_f.glGetUniformLocation_(m_g768Prog, "uSrcX");
        m_locG768SrcY = m_f.glGetUniformLocation_(m_g768Prog, "uSrcY");
        m_locG768Fbo = m_f.glGetUniformLocation_(m_g768Prog, "uFbo");
        m_locG768Scale = m_f.glGetUniformLocation_(m_g768Prog, "uScale");
        m_locG768Centre = m_f.glGetUniformLocation_(m_g768Prog, "uCentre");
        m_locG768Ta0 = m_f.glGetUniformLocation_(m_g768Prog, "uTa0");
        m_locG768Ta1 = m_f.glGetUniformLocation_(m_g768Prog, "uTa1");
        m_locG768Aem = m_f.glGetUniformLocation_(m_g768Prog, "uAem");
        m_f.glUseProgram_(m_g768ClutProg);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768ClutProg, "uEnt"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768ClutProg, "uS0"), 2);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768ClutProg, "uS1"), 3);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768ClutProg, "uS2"), 4);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g768ClutProg, "uS3"), 5);
        m_locG768CFbo[0] = m_f.glGetUniformLocation_(m_g768ClutProg, "uFbo0");
        m_locG768CFbo[1] = m_f.glGetUniformLocation_(m_g768ClutProg, "uFbo1");
        m_locG768CFbo[2] = m_f.glGetUniformLocation_(m_g768ClutProg, "uFbo2");
        m_locG768CFbo[3] = m_f.glGetUniformLocation_(m_g768ClutProg, "uFbo3");
        m_locG768CScale = m_f.glGetUniformLocation_(m_g768ClutProg, "uScale");
        m_locG768CCentre = m_f.glGetUniformLocation_(m_g768ClutProg, "uCentre");
        m_locG768CTa0 = m_f.glGetUniformLocation_(m_g768ClutProg, "uTa0");
        m_locG768CTa1 = m_f.glGetUniformLocation_(m_g768ClutProg, "uTa1");
        m_locG768CAem = m_f.glGetUniformLocation_(m_g768ClutProg, "uAem");
        std::fprintf(stderr, "[G768:view] PASS: general page-view + GPU CLUT programs ready\n");
        return true;
    }

    // A texture of the given internal format, NEAREST / CLAMP, created once.
    GLuint g768MakeTex(GLint internalFmt, int w, int h, GLenum fmt, GLenum type, const void *px)
    {
        GLuint id = 0u;
        glGenTextures(1, &id);
        if (id == 0u)
            return 0u;
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        return id;
    }

    // The source texture of one rect / CLUT source, or 0. `fbo` reports whether it is an FBO.
    GLuint g768SourceTex(int kind, int key, int scale)
    {
        if (kind == kG768SrcMirror)
            return m_g768Mirror;
        if (kind != kG768SrcFbo)
            return 0u;
        const auto it = m_fbos.find(static_cast<uint32_t>(key));
        if (it == m_fbos.end() || it->second.colorTex == 0u || it->second.h != 512 || it->second.w <= 0)
            return 0u;
        const int ps = (it->second.physW > 0 ? it->second.physW : it->second.w) / it->second.w;
        return ps == scale ? it->second.colorTex : 0u;
    }

    bool g768BuildViewOnWorker(const G768ViewPacket &pk)
    {
        const auto fail = [&](const char *stage) {
            m_g768Fails.fetch_add(1u, std::memory_order_relaxed);
            static uint32_t s_log = 0u;
            if (s_log++ < 24u)
                std::fprintf(stderr, "[G768:view-fail] stage=%s fmt=%d tex=%dx%d rects=%zu full=%d\n",
                             stage, pk.fmt, pk.texW, pk.texH, pk.rects.size() / 8u, pk.full ? 1 : 0);
            return false;
        };
        // (1) Mirror uploads FIRST and unconditionally: the front end has already recorded these
        // pages as mirrored, so they must land even if the view itself fails below. A mirror that
        // cannot be written bumps the epoch, which makes the front end forget every mirrored page.
        if (!pk.mirPages.empty())
        {
            if (pk.mirPixels.size() != pk.mirPages.size() * 2048u)
            {
                m_g768MirrorEpoch.fetch_add(1u, std::memory_order_acq_rel);
                return fail("mirargs");
            }
            m_f.glActiveTexture_(GL158_TEXTURE0);
            if (m_g768Mirror == 0u)
                m_g768Mirror = g768MakeTex(GL158_RGBA8, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            if (m_g768Mirror == 0u)
            {
                m_g768MirrorEpoch.fetch_add(1u, std::memory_order_acq_rel);
                return fail("mirror");
            }
            glBindTexture(GL_TEXTURE_2D, m_g768Mirror);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            for (size_t i = 0u; i < pk.mirPages.size(); ++i)
            {
                const uint32_t p = pk.mirPages[i] & 511u;
                glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>((p & 15u) * 64u),
                                static_cast<GLint>((p >> 4u) * 32u), 64, 32, GL_RGBA,
                                GL_UNSIGNED_BYTE, pk.mirPixels.data() + i * 2048u);
            }
            m_g768MirPages.fetch_add(pk.mirPages.size(), std::memory_order_relaxed);
        }
        if (pk.texKey == 0u || pk.texW <= 0 || pk.texH <= 0 || pk.rects.empty() ||
            (pk.rects.size() % 8u) != 0u || pk.lut == nullptr || pk.lutId < 0 || pk.lutId >= 8 ||
            pk.lut->size() != static_cast<size_t>(pk.lutW) * static_cast<size_t>(pk.lutH))
            return fail("args");
        const bool indexed = pk.fmt == kG768FmtIdx8 || pk.fmt == kG768FmtIdx4;
        if (indexed && (pk.clutN <= 0 || pk.clutN > 256 ||
                        pk.clutConst.size() != static_cast<size_t>(pk.clutN) ||
                        (pk.clutGpu && pk.clutEnt.size() != static_cast<size_t>(pk.clutN) * 4u)))
            return fail("clutargs");
        if (!g768SetupPrograms())
            return fail("program");
        const int s = std::max(1, dc2GetNativeScale());
        const int centre = s > 1 ? dc2NativeSubPixelCentre() : 0;
        for (size_t k = 0u; k + 8u <= pk.rects.size(); k += 8u)
        {
            const int dx = pk.rects[k + 4], dy = pk.rects[k + 5], w = pk.rects[k + 6], h = pk.rects[k + 7];
            if (w <= 0 || h <= 0 || dx < 0 || dy < 0 || dx + w > pk.texW || dy + h > pk.texH ||
                w > pk.lutW || h > pk.lutH)
                return fail("rect");
            if (g768SourceTex(pk.rects[k], pk.rects[k + 1], s) == 0u)
                return fail("source");
        }
        // (2) The per-format page LUT.
        m_f.glActiveTexture_(GL158_TEXTURE0);
        if (m_g768Lut[pk.lutId] == 0u)
        {
            m_g768Lut[pk.lutId] = g768MakeTex(GL158_RGBA8, pk.lutW, pk.lutH, GL_RGBA, GL_UNSIGNED_BYTE,
                                              pk.lut->data());
            if (m_g768Lut[pk.lutId] == 0u)
                return fail("lut");
        }
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return fail("objects");
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_f.glBindVertexArray_(m_vao);
        // (3) The CLUT: constants, then (when any entry lives in an FBO) one gather pass.
        if (indexed)
        {
            if (m_g768ClutTex == 0u)
                m_g768ClutTex = g768MakeTex(GL158_RGBA8, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            if (m_g768ClutTex == 0u)
                return fail("clut");
            if (pk.clutSig != 0u && pk.clutSig == m_g768ClutSig)
                m_g768ClutReuse.fetch_add(1u, std::memory_order_relaxed);
            else
            {
                m_g768ClutSig = 0u;
                glBindTexture(GL_TEXTURE_2D, m_g768ClutTex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pk.clutN, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                pk.clutConst.data());
                if (pk.clutGpu)
                {
                    GLuint srcTex[4] = {0u, 0u, 0u, 0u};
                    int fboFlag[4] = {0, 0, 0, 0};
                    for (int i = 0; i < 4; ++i)
                    {
                        if (pk.clutSrcKind[i] < 0)
                            continue;
                        srcTex[i] = g768SourceTex(pk.clutSrcKind[i], pk.clutSrcKey[i], s);
                        if (srcTex[i] == 0u)
                            return fail("clutsource");
                        fboFlag[i] = pk.clutSrcKind[i] == kG768SrcFbo ? 1 : 0;
                    }
                    std::vector<float> ent(static_cast<size_t>(pk.clutN) * 4u);
                    for (size_t i = 0u; i < ent.size(); ++i)
                        ent[i] = static_cast<float>(pk.clutEnt[i]);
                    if (m_g768EntTex == 0u)
                        m_g768EntTex = g768MakeTex(0x8814 /* GL_RGBA32F */, 256, 1, GL_RGBA, GL_FLOAT, nullptr);
                    if (m_g768EntTex == 0u)
                        return fail("enttex");
                    glBindTexture(GL_TEXTURE_2D, m_g768EntTex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pk.clutN, 1, GL_RGBA, GL_FLOAT, ent.data());
                    m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
                    m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0,
                                                GL_TEXTURE_2D, m_g768ClutTex, 0);
                    if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
                        return fail("clutfbo");
                    glViewport(0, 0, 256, 1);
                    glDisable(GL_SCISSOR_TEST);
                    m_f.glUseProgram_(m_g768ClutProg);
                    for (int i = 0; i < 4; ++i)
                        m_f.glUniform1i_(m_locG768CFbo[i], fboFlag[i]);
                    m_f.glUniform1i_(m_locG768CScale, s);
                    m_f.glUniform1i_(m_locG768CCentre, centre);
                    m_f.glUniform1i_(m_locG768CTa0, pk.ta0 & 0xFF);
                    m_f.glUniform1i_(m_locG768CTa1, pk.ta1 & 0xFF);
                    m_f.glUniform1i_(m_locG768CAem, pk.aem != 0 ? 1 : 0);
                    m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
                    glBindTexture(GL_TEXTURE_2D, m_g768EntTex);
                    for (int i = 0; i < 4; ++i)
                    {
                        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u + static_cast<unsigned>(i));
                        // An unused sampler still needs a complete texture bound; the CLUT texture is
                        // never read by an entry that does not name that source.
                        glBindTexture(GL_TEXTURE_2D, srcTex[i] != 0u ? srcTex[i] : m_g768EntTex);
                    }
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(0, 0, pk.clutN, 1);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glDisable(GL_SCISSOR_TEST);
                    for (int i = 0; i < 5; ++i)
                    {
                        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u + static_cast<unsigned>(i));
                        glBindTexture(GL_TEXTURE_2D, 0u);
                    }
                    m_f.glActiveTexture_(GL158_TEXTURE0);
                    m_g768ClutPasses.fetch_add(1u, std::memory_order_relaxed);
                }
                m_g768ClutSig = pk.clutSig;
            }
        }
        // (4) The view texture: allocated on a full build, patched in place on a partial one.
        TexEntry &out = m_texCache[pk.texKey];
        const bool reuse = out.id != 0u && out.g492Ct24Alloc == 7680u && out.w == pk.texW &&
                           out.h == pk.texH;
        if (!pk.full && !reuse)
        {
            if (out.id == 0u)
                m_texCache.erase(pk.texKey);
            return fail("partial-missing");
        }
        if (out.id == 0u)
            glGenTextures(1, &out.id);
        else if (!reuse)
            m_texBytes -= std::min(m_texBytes, static_cast<size_t>(out.w) * out.h * 4u);
        if (out.id == 0u)
        {
            m_texCache.erase(pk.texKey);
            return fail("output");
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out.id);
        if (!reuse)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, pk.texW, pk.texH, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            out.g496MinF = GL_NEAREST;
            out.g496MagF = GL_NEAREST;
            out.g496WrapS = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.g496WrapT = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.w = pk.texW;
            out.h = pk.texH;
            out.g492Ct24Alloc = 7680u; // G768 general page-view storage
            m_texBytes += static_cast<size_t>(pk.texW) * pk.texH * 4u;
        }
        out.lastUse = m_batchCounter + 1u;
        g741LruTouch(pk.texKey, out); // G741
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    out.id, 0);
        // A reused view texture was validated as a complete RGBA8 attachment when it was created
        // (same storage, same size): only a fresh allocation pays the driver's completeness check.
        if (!reuse &&
            m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
        {
            glDeleteTextures(1, &out.id);
            m_texBytes -= std::min(m_texBytes, static_cast<size_t>(pk.texW) * pk.texH * 4u);
            g741LruUnlink(out); // G741
            m_texCache.erase(pk.texKey);
            return fail("framebuffer");
        }
        glViewport(0, 0, pk.texW, pk.texH);
        glEnable(GL_SCISSOR_TEST);
        m_f.glUseProgram_(m_g768Prog);
        m_f.glUniform1i_(m_locG768Fmt, pk.fmt);
        m_f.glUniform1i_(m_locG768Scale, s);
        m_f.glUniform1i_(m_locG768Centre, centre);
        m_f.glUniform1i_(m_locG768Ta0, pk.ta0 & 0xFF);
        m_f.glUniform1i_(m_locG768Ta1, pk.ta1 & 0xFF);
        m_f.glUniform1i_(m_locG768Aem, pk.aem != 0 ? 1 : 0);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g768Lut[pk.lutId]);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, indexed ? m_g768ClutTex : m_g768Lut[pk.lutId]);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        GLuint bound = 0u;
        for (size_t k = 0u; k + 8u <= pk.rects.size(); k += 8u)
        {
            const GLuint tex = g768SourceTex(pk.rects[k], pk.rects[k + 1], s);
            if (tex != bound)
            {
                glBindTexture(GL_TEXTURE_2D, tex);
                bound = tex;
            }
            m_f.glUniform1i_(m_locG768Fbo, pk.rects[k] == kG768SrcFbo ? 1 : 0);
            m_f.glUniform1i_(m_locG768SrcX, pk.rects[k + 2]);
            m_f.glUniform1i_(m_locG768SrcY, pk.rects[k + 3]);
            m_f.glUniform1i_(m_locG768PgX, pk.rects[k + 4]);
            m_f.glUniform1i_(m_locG768PgY, pk.rects[k + 5]);
            glScissor(pk.rects[k + 4], pk.rects[k + 5], pk.rects[k + 6], pk.rects[k + 7]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, 0u);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, 0u);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        {
            std::lock_guard<std::mutex> lk(m_residentMtx);
            m_resident.insert(pk.texKey);
        }
        m_g768Rects.fetch_add(pk.rects.size() / 8u, std::memory_order_relaxed);
        if (pk.full)
            m_g768Builds.fetch_add(1u, std::memory_order_relaxed);
        else
            m_g768Partials.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    // ORACLE ONLY (DC2_G768_VERIFY=1): read a view texture back, row 0 = texel row v = 0.
    bool g768ReadViewOnWorker(uint64_t key, int w, int h, std::vector<uint32_t> &out)
    {
        const auto it = m_texCache.find(key);
        if (it == m_texCache.end() || it->second.id == 0u || it->second.w != w || it->second.h != h)
            return false;
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    it->second.id, 0);
        if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
            return false;
        m_f.glBindBuffer_(GL433_PIXEL_PACK_BUFFER, 0u);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        out.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        return true;
    }
    bool g768ReadViewSync(uint64_t key, int w, int h, std::vector<uint32_t> &out)
    {
        if (key == 0u || w <= 0 || h <= 0 || !m_ready.load(std::memory_order_acquire))
            return false;
        SyncWait wait;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g768ReadKey = key;
            item.g768ReadOut = &out;
            item.g281TexW = w;
            item.g281TexH = h;
            item.promise = wait.promise;
            item.doneFlag = &wait.done;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return wait.wait();
    }

    // Fire-and-forget: the packet is owned by the queue item; FIFO order places it before the batch
    // that samples the view.
    bool queueG768View(std::shared_ptr<G768ViewPacket> pk)
    {
        if (!pk || pk->texKey == 0u || !m_ready.load(std::memory_order_acquire))
            return false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop || !m_ready.load(std::memory_order_acquire))
                return false;
            QueueItem item;
            item.g281TexKey = pk->texKey;
            item.g281TexW = pk->texW;
            item.g281TexH = pk->texH;
            item.g768View = std::move(pk);
            item.g767Async = true;
            m_queue.push_back(std::move(item));
        }
        m_submitSeq.fetch_add(1u, std::memory_order_release);
        m_cvWork.notify_one();
        return true;
    }
private: // G768: restore the access the members following this include were declared with
