    // ===== G767 (members): the CT32 / CT24 PAGE VIEW — a texture assembled on the GPU from the
    // =====   resident FBOs that own its pages =====
    // A CT32/CT24 texture read whose pages live (newest) in resident FBOs used to publish every
    // overlapping resident to guest VRAM and decode the texture from there. A CT32 page is layout
    // independent, so the texture IS the list of its pages: each page is gathered from the FBO that
    // holds it, through the SAME centre tap the G675 physical->logical resolve uses (so the texels
    // are bit-identical to what a publication + decode would have produced), into a logical-size
    // RGBA8 texture that the consumer samples like any decoded texture (any prim, STQ, REPEAT/CLAMP,
    // its own filter). CT24 applies TEXA exactly as GSMem does (the G338 rule).
    // rects7: {srcKey, srcGsX, srcGsY, dstX, dstY, w, h} — GS top-origin logical source coords,
    // texel (top-origin) destination coords.
    bool m_g767DualOk = false; // G767: the main program carries the dual-source raw-alpha output
    // Drop a view texture (a failed async build): the next draw that samples the key misses loudly.
    void g767DropTex(uint64_t texKey)
    {
        const auto it = m_texCache.find(texKey);
        if (it != m_texCache.end())
        {
            if (it->second.id != 0u)
                glDeleteTextures(1, &it->second.id);
            m_texBytes -= std::min(m_texBytes,
                                   static_cast<size_t>(it->second.w) * it->second.h * 4u);
            g741LruUnlink(it->second); // G741
            m_texCache.erase(it);
        }
        std::lock_guard<std::mutex> lk(m_residentMtx);
        m_resident.erase(texKey);
    }
    GLuint m_g767ViewProg = 0u;
    bool m_g767ViewFailed = false;
    GLint m_locG767Src = -1, m_locG767OffX = -1, m_locG767OffY = -1, m_locG767Scale = -1, m_locG767Centre = -1,
          m_locG767Ct24 = -1, m_locG767Ta0 = -1, m_locG767Aem = -1;
    uint64_t m_g767ViewBuilds = 0u, m_g767ViewFails = 0u;

    bool g767SetupViewProgram()
    {
        if (m_g767ViewProg != 0u)
            return true;
        if (m_g767ViewFailed)
            return false;
        static const char *kVS =
            "#version 330 core\n"
            "void main(){\n"
            "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        // uOff = srcGs - dst: GS source pixel of view texel p is (p.x + uOff.x, p.y + uOff.y), and
        // the FBO stores GS row y at logical GL row 511 - y.
        static const char *kFS =
            "#version 330 core\n"
            "out vec4 o;\n"
            "uniform sampler2D uSrc;\n"
            "uniform int uOffX;\n"
            "uniform int uOffY;\n"
            "uniform int uScale;\n"
            "uniform int uCentre;\n"
            "uniform int uCt24;\n"
            "uniform int uTa0;\n"
            "uniform int uAem;\n"
            "void main(){\n"
            "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "    ivec2 gs = p + ivec2(uOffX, uOffY);\n"
            "    ivec2 gl = ivec2(gs.x, 511 - gs.y);\n"
            "    vec4 c = texelFetch(uSrc, gl * uScale + ivec2(uCentre), 0);\n"
            "    if (uCt24 != 0) {\n"
            "        ivec3 raw = clamp(ivec3(floor(c.rgb * 255.0 + 0.5)), 0, 255);\n"
            "        bool rgb0 = raw.r == 0 && raw.g == 0 && raw.b == 0;\n"
            "        int a = (uAem != 0 && rgb0) ? 0 : uTa0;\n"
            "        c = vec4(vec3(raw) * (1.0 / 255.0), float(a) * (1.0 / 255.0));\n"
            "    }\n"
            "    o = c;\n"
            "}\n";
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            if (vs != 0u)
                m_f.glDeleteShader_(vs);
            std::fprintf(stderr, "[G767:view] FAIL: compile: %s\n", log.c_str());
            m_g767ViewFailed = true;
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
            std::fprintf(stderr, "[G767:view] FAIL: link\n");
            m_g767ViewFailed = true;
            return false;
        }
        m_g767ViewProg = prog;
        m_f.glUseProgram_(prog);
        m_locG767Src = m_f.glGetUniformLocation_(prog, "uSrc");
        m_f.glUniform1i_(m_locG767Src, 0);
        m_locG767OffX = m_f.glGetUniformLocation_(prog, "uOffX");
        m_locG767OffY = m_f.glGetUniformLocation_(prog, "uOffY");
        m_locG767Scale = m_f.glGetUniformLocation_(prog, "uScale");
        m_locG767Centre = m_f.glGetUniformLocation_(prog, "uCentre");
        m_locG767Ct24 = m_f.glGetUniformLocation_(prog, "uCt24");
        m_locG767Ta0 = m_f.glGetUniformLocation_(prog, "uTa0");
        m_locG767Aem = m_f.glGetUniformLocation_(prog, "uAem");
        std::fprintf(stderr, "[G767:view] PASS: resident CT32/CT24 page-view program ready\n");
        return true;
    }

    bool g767PrepareViewOnWorker(uint64_t texKey, int texW, int texH, bool ct24, int ta0, int aem,
                                 const std::vector<int32_t> &rects7,
                                 const std::vector<uint32_t> &vramPixels)
    {
        const auto fail = [&](const char *stage) {
            ++m_g767ViewFails;
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr, "[G767:view-fail] stage=%s tex=%dx%d rects=%zu\n", stage, texW,
                             texH, rects7.size() / 7u);
            return false;
        };
        if (texKey == 0u || texW <= 0 || texH <= 0 || rects7.empty() || (rects7.size() % 7u) != 0u)
            return fail("args");
        if (!g767SetupViewProgram())
            return fail("program");
        const int s = std::max(1, dc2GetNativeScale());
        const int centre = s > 1 ? dc2NativeSubPixelCentre() : 0;
        // Validate every source before touching the output (fail closed, nothing written).
        size_t vramTexels = 0u;
        for (size_t k = 0u; k + 7u <= rects7.size(); k += 7u)
        {
            if (rects7[k] == 0)
            {
                // A page no FBO holds: its texels arrive decoded from guest VRAM (front end).
                const int dx = rects7[k + 3], dy = rects7[k + 4], w = rects7[k + 5], h = rects7[k + 6];
                if (w <= 0 || h <= 0 || dx < 0 || dy < 0 || dx + w > texW || dy + h > texH)
                    return fail("vramrect");
                vramTexels += static_cast<size_t>(w) * static_cast<size_t>(h);
                continue;
            }
            auto it = m_fbos.find(static_cast<uint32_t>(rects7[k]));
            if (it == m_fbos.end() || it->second.colorTex == 0u || it->second.h != 512)
                return fail("source");
            const int sx = rects7[k + 1], sy = rects7[k + 2], dx = rects7[k + 3], dy = rects7[k + 4];
            const int w = rects7[k + 5], h = rects7[k + 6];
            if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || sx + w > it->second.w || sy + h > 512 ||
                dx < 0 || dy < 0 || dx + w > texW || dy + h > texH)
                return fail("rect");
            const int ps = it->second.w > 0
                               ? (it->second.physW > 0 ? it->second.physW : it->second.w) / it->second.w
                               : 0;
            if (ps != s)
                return fail("scale");
        }
        if (vramTexels != vramPixels.size())
            return fail("vrampixels");
        if (m_g281Fbo == 0u)
            m_f.glGenFramebuffers_(1, &m_g281Fbo);
        if (m_g281Fbo == 0u)
            return fail("objects");
        TexEntry &out = m_texCache[texKey];
        const bool reuse = out.id != 0u && out.g492Ct24Alloc == 767u && out.w == texW && out.h == texH;
        if (out.id == 0u)
            glGenTextures(1, &out.id);
        else if (!reuse)
            m_texBytes -= std::min(m_texBytes, static_cast<size_t>(out.w) * out.h * 4u);
        if (out.id == 0u)
        {
            m_texCache.erase(texKey);
            return fail("output");
        }
        m_f.glActiveTexture_(GL158_TEXTURE0);
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
            out.g492Ct24Alloc = 767u; // G767: storage this builder allocated at (w,h)
            m_texBytes += static_cast<size_t>(texW) * texH * 4u;
        }
        out.lastUse = m_batchCounter + 1u;
        g741LruTouch(texKey, out); // G741
        // Guest-VRAM-sourced pages first (the output texture is bound on unit 0 here).
        if (vramTexels != 0u)
        {
            size_t at = 0u;
            for (size_t k = 0u; k + 7u <= rects7.size(); k += 7u)
            {
                if (rects7[k] != 0)
                    continue;
                const int dx = rects7[k + 3], dy = rects7[k + 4], w = rects7[k + 5], h = rects7[k + 6];
                glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                                vramPixels.data() + at);
                at += static_cast<size_t>(w) * static_cast<size_t>(h);
            }
        }
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
        m_f.glUseProgram_(m_g767ViewProg);
        m_f.glUniform1i_(m_locG767Scale, s);
        m_f.glUniform1i_(m_locG767Centre, centre);
        m_f.glUniform1i_(m_locG767Ct24, ct24 ? 1 : 0);
        m_f.glUniform1i_(m_locG767Ta0, ta0 & 0xFF);
        m_f.glUniform1i_(m_locG767Aem, aem != 0 ? 1 : 0);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        GLuint bound = 0u;
        for (size_t k = 0u; k + 7u <= rects7.size(); k += 7u)
        {
            if (rects7[k] == 0)
                continue; // uploaded above
            const FboEntry &src = m_fbos.find(static_cast<uint32_t>(rects7[k]))->second;
            if (src.colorTex != bound)
            {
                glBindTexture(GL_TEXTURE_2D, src.colorTex);
                bound = src.colorTex;
            }
            const int sx = rects7[k + 1], sy = rects7[k + 2], dx = rects7[k + 3], dy = rects7[k + 4];
            m_f.glUniform1i_(m_locG767OffX, sx - dx);
            m_f.glUniform1i_(m_locG767OffY, sy - dy);
            glScissor(dx, dy, rects7[k + 5], rects7[k + 6]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        {
            std::lock_guard<std::mutex> lk(m_residentMtx); // G767: views are resident textures
            m_resident.insert(texKey);
        }
        ++m_g767ViewBuilds;
        return true;
    }
