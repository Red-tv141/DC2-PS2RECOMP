    // ===== G767 (members): the PALETTED PAGE VIEW — PSMT8 / PSMT8H / PSMT4HL / PSMT4HH decoded on
    // =====   the GPU from the FBOs (or guest-VRAM pages) that hold the texture's pages =====
    // G281's T8 view needs a CPU-built per-texel byte map and is admitted for two hard-coded tuples
    // (G598's generalisation is default OFF because the map build explodes the CPU). A paletted page
    // is layout independent exactly like a CT32 page: PSMT8 texel (x%128, y%64) of a page always
    // lives in the same CT32 word + byte lane of that page (GSMem::AddressP8 composed with the CT32
    // inverse), so ONE 128x64 lookup table serves every PSMT8 texture; the H formats use the CT32
    // page geometry directly with the index in the alpha byte. Per page the shader fetches the
    // holder's texel (G675 centre tap for an FBO, raw for a guest-VRAM page) and looks the index up
    // in the CLUT the front end decoded exactly as the legacy decode does.
    // rects7: {srcKey | 0 (guest VRAM staging), srcGsX | stagingIndex, srcGsY, dstX, dstY, w, h}.
    GLuint m_g767T8Prog = 0u, m_g767T8Lut = 0u, m_g767T8ClutTex = 0u, m_g767T8Stage = 0u;
    int m_g767T8StageRows = 0;
    bool m_g767T8Failed = false;
    GLint m_locG767T8Kind = -1, m_locG767T8PgX = -1, m_locG767T8PgY = -1, m_locG767T8SrcX = -1,
          m_locG767T8SrcY = -1, m_locG767T8Fbo = -1, m_locG767T8Scale = -1,
          m_locG767T8Centre = -1;
    uint64_t m_g767T8Builds = 0u, m_g767T8Fails = 0u;

    bool g767SetupT8Program()
    {
        if (m_g767T8Prog != 0u)
            return true;
        if (m_g767T8Failed)
            return false;
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
            "uniform sampler2D uLut;\n"
            "uniform sampler2D uClut;\n"
            "uniform int uKind;\n"
            "uniform int uPgX;\n"
            "uniform int uPgY;\n"
            "uniform int uSrcX;\n"
            "uniform int uSrcY;\n"
            "uniform int uFbo;\n"
            "uniform int uScale;\n"
            "uniform int uCentre;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec2 loc = p - ivec2(uPgX, uPgY);\n"
            "    ivec2 w = loc;\n"
            "    int lane = 3;\n"
            "    if (uKind == 0) {\n"
            "        ivec4 l = ivec4(floor(texelFetch(uLut, loc, 0) * 255.0 + 0.5));\n"
            "        w = l.xy;\n"
            "        lane = l.z;\n"
            "    }\n"
            "    ivec2 s = ivec2(uSrcX, uSrcY) + w;\n"
            "    vec4 c = (uFbo != 0)\n"
            "        ? texelFetch(uSrc, ivec2(s.x, 511 - s.y) * uScale + ivec2(uCentre), 0)\n"
            "        : texelFetch(uSrc, s, 0);\n"
            "    float f = (lane == 0) ? c.r : ((lane == 1) ? c.g : ((lane == 2) ? c.b : c.a));\n"
            "    int b = clamp(int(floor(f * 255.0 + 0.5)), 0, 255);\n"
            "    if (uKind == 2) b = b & 15;\n"
            "    else if (uKind == 3) b = b >> 4;\n"
            "    o = texelFetch(uClut, ivec2(b, 0), 0);\n"
            "}\n";
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            if (vs != 0u)
                m_f.glDeleteShader_(vs);
            std::fprintf(stderr, "[G767:t8view] FAIL: compile: %s\n", log.c_str());
            m_g767T8Failed = true;
            return false;
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
            std::fprintf(stderr, "[G767:t8view] FAIL: link\n");
            m_g767T8Failed = true;
            return false;
        }
        m_g767T8Prog = prog;
        m_f.glUseProgram_(prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(prog, "uSrc"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(prog, "uLut"), 1);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(prog, "uClut"), 2);
        m_locG767T8Kind = m_f.glGetUniformLocation_(prog, "uKind");
        m_locG767T8PgX = m_f.glGetUniformLocation_(prog, "uPgX");
        m_locG767T8PgY = m_f.glGetUniformLocation_(prog, "uPgY");
        m_locG767T8SrcX = m_f.glGetUniformLocation_(prog, "uSrcX");
        m_locG767T8SrcY = m_f.glGetUniformLocation_(prog, "uSrcY");
        m_locG767T8Fbo = m_f.glGetUniformLocation_(prog, "uFbo");
        m_locG767T8Scale = m_f.glGetUniformLocation_(prog, "uScale");
        m_locG767T8Centre = m_f.glGetUniformLocation_(prog, "uCentre");
        std::fprintf(stderr, "[G767:t8view] PASS: paletted page-view program ready\n");
        return true;
    }

    // A small RGBA8 texture helper (NEAREST, CLAMP), created once.
    GLuint g767MakeTex(int w, int h, const void *px)
    {
        GLuint id = 0u;
        glGenTextures(1, &id);
        if (id == 0u)
            return 0u;
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
        return id;
    }

    bool g767PrepareT8ViewOnWorker(uint64_t texKey, int texW, int texH, int kind,
                                   const std::vector<int32_t> &rects7,
                                   const std::vector<uint32_t> &vramPages,
                                   const std::vector<uint32_t> &clut256,
                                   const std::vector<uint32_t> &lut)
    {
        const auto fail = [&](const char *stage) {
            ++m_g767T8Fails;
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr, "[G767:t8view-fail] stage=%s kind=%d tex=%dx%d rects=%zu\n",
                             stage, kind, texW, texH, rects7.size() / 7u);
            return false;
        };
        if (texKey == 0u || texW <= 0 || texH <= 0 || rects7.empty() || (rects7.size() % 7u) != 0u ||
            clut256.size() != 256u || kind < 0 || kind > 3 || (kind == 0 && lut.size() != 128u * 64u) ||
            (vramPages.size() % (64u * 32u)) != 0u)
            return fail("args");
        if (!g767SetupT8Program())
            return fail("program");
        const int s = std::max(1, dc2GetNativeScale());
        const int centre = s > 1 ? dc2NativeSubPixelCentre() : 0;
        const int stagedPages = static_cast<int>(vramPages.size() / (64u * 32u));
        for (size_t k = 0u; k + 7u <= rects7.size(); k += 7u)
        {
            const int dx = rects7[k + 3], dy = rects7[k + 4], w = rects7[k + 5], h = rects7[k + 6];
            if (w <= 0 || h <= 0 || dx < 0 || dy < 0 || dx + w > texW || dy + h > texH)
                return fail("rect");
            if (rects7[k] == 0)
            {
                if (rects7[k + 1] < 0 || rects7[k + 1] >= stagedPages)
                    return fail("stage");
                continue;
            }
            auto it = m_fbos.find(static_cast<uint32_t>(rects7[k]));
            if (it == m_fbos.end() || it->second.colorTex == 0u || it->second.h != 512 ||
                rects7[k + 1] < 0 || rects7[k + 2] < 0 || rects7[k + 1] + 64 > it->second.w ||
                rects7[k + 2] + 32 > 512)
                return fail("source");
            const int ps = it->second.w > 0
                               ? (it->second.physW > 0 ? it->second.physW : it->second.w) / it->second.w
                               : 0;
            if (ps != s)
                return fail("scale");
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
        if (m_g767T8Lut == 0u && kind == 0)
        {
            m_g767T8Lut = g767MakeTex(128, 64, lut.data());
            if (m_g767T8Lut == 0u)
                return fail("lut");
        }
        if (m_g767T8ClutTex == 0u)
            m_g767T8ClutTex = g767MakeTex(256, 1, nullptr);
        if (m_g767T8ClutTex == 0u)
            return fail("clut");
        glBindTexture(GL_TEXTURE_2D, m_g767T8ClutTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, clut256.data());
        if (stagedPages > 0)
        {
            const int rows = stagedPages * 32;
            if (m_g767T8Stage == 0u || m_g767T8StageRows < rows)
            {
                if (m_g767T8Stage != 0u)
                    glDeleteTextures(1, &m_g767T8Stage);
                m_g767T8StageRows = std::max(rows, 256);
                m_g767T8Stage = g767MakeTex(64, m_g767T8StageRows, nullptr);
                if (m_g767T8Stage == 0u)
                {
                    m_g767T8StageRows = 0;
                    return fail("stagealloc");
                }
            }
            glBindTexture(GL_TEXTURE_2D, m_g767T8Stage);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                            vramPages.data());
        }
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return fail("objects");
        TexEntry &out = m_texCache[texKey];
        const bool reuse = out.id != 0u && out.g492Ct24Alloc == 768u && out.w == texW && out.h == texH;
        if (out.id == 0u)
            glGenTextures(1, &out.id);
        else if (!reuse)
            m_texBytes -= std::min(m_texBytes, static_cast<size_t>(out.w) * out.h * 4u);
        if (out.id == 0u)
        {
            m_texCache.erase(texKey);
            return fail("output");
        }
        glBindTexture(GL_TEXTURE_2D, out.id);
        if (!reuse)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL158_RGBA8, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL158_CLAMP_TO_EDGE));
            out.g496MinF = GL_NEAREST;
            out.g496MagF = GL_NEAREST;
            out.g496WrapS = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.g496WrapT = static_cast<int>(GL158_CLAMP_TO_EDGE);
            out.w = texW;
            out.h = texH;
            out.g492Ct24Alloc = 768u; // G767 paletted view storage
            m_texBytes += static_cast<size_t>(texW) * texH * 4u;
        }
        out.lastUse = m_batchCounter + 1u;
        g741LruTouch(texKey, out); // G741
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g281Fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    out.id, 0);
        if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
        {
            glDeleteTextures(1, &out.id);
            m_texBytes -= std::min(m_texBytes, static_cast<size_t>(texW) * texH * 4u);
            g741LruUnlink(out); // G741
            m_texCache.erase(texKey);
            return fail("framebuffer");
        }
        glViewport(0, 0, texW, texH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_SCISSOR_TEST);
        m_f.glUseProgram_(m_g767T8Prog);
        m_f.glUniform1i_(m_locG767T8Kind, kind);
        m_f.glUniform1i_(m_locG767T8Scale, s);
        m_f.glUniform1i_(m_locG767T8Centre, centre);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g767T8Lut);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, m_g767T8ClutTex);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        GLuint bound = 0u;
        for (size_t k = 0u; k + 7u <= rects7.size(); k += 7u)
        {
            const bool fbo = rects7[k] != 0;
            const GLuint tex = fbo ? m_fbos.find(static_cast<uint32_t>(rects7[k]))->second.colorTex
                                   : m_g767T8Stage;
            if (tex != bound)
            {
                glBindTexture(GL_TEXTURE_2D, tex);
                bound = tex;
            }
            m_f.glUniform1i_(m_locG767T8Fbo, fbo ? 1 : 0);
            m_f.glUniform1i_(m_locG767T8SrcX, fbo ? rects7[k + 1] : 0);
            m_f.glUniform1i_(m_locG767T8SrcY, fbo ? rects7[k + 2] : rects7[k + 1] * 32);
            m_f.glUniform1i_(m_locG767T8PgX, rects7[k + 3]);
            m_f.glUniform1i_(m_locG767T8PgY, rects7[k + 4]);
            glScissor(rects7[k + 3], rects7[k + 4], rects7[k + 5], rects7[k + 6]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        // Leave units 1/2 unbound so later programs never sample the view's LUT / CLUT by accident.
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, 0u);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 2u);
        glBindTexture(GL_TEXTURE_2D, 0u);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        {
            std::lock_guard<std::mutex> lk(m_residentMtx); // G767: views are resident textures
            m_resident.insert(texKey);
        }
        ++m_g767T8Builds;
        return true;
    }
