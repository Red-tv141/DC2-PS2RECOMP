    // ===== G767 (members): LOCAL -> LOCAL BYTE MOVE BETWEEN RESIDENT FBO PAGES =====
    // gather:    {holderKey, holderX, holderY(GS top-origin), atlasTile} x nA — page rects into atlas A
    // map:       64 x 32 x nDest texels x 4 lanes (uint), rows in GL order per tile (row 31 - gsRow):
    //            bit31 valid | tile<<13 | lane<<11 | row<<6 | col
    // destTiles: dest tile -> atlas tile holding its current bytes
    // scatter:   {homeKey, homeX, homeY(GS top-origin), destTile} — finished pages into every home
    // A and B hold one 64x32 page per tile, stacked vertically, at the physical scale S; every physical
    // sample moves its own bytes (sub-sample k of the source texel -> sub-sample k of the destination).
    GLuint m_g767L2lProg = 0u, m_g767L2lA = 0u, m_g767L2lB = 0u, m_g767L2lM = 0u;
    GLuint m_g767L2lFboA = 0u, m_g767L2lFboB = 0u;
    int m_g767L2lARows = 0, m_g767L2lBRows = 0, m_g767L2lMRows = 0, m_g767L2lS = 0;
    bool m_g767L2lFailed = false;
    GLint m_locG767L2lS = -1, m_locG767L2lT = -1, m_locG767L2lDestA = -1;

    bool g767SetupL2lProgram()
    {
        if (m_g767L2lProg != 0u)
            return true;
        if (m_g767L2lFailed)
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
            "uniform sampler2D uA;\n"
            "uniform usampler2D uMap;\n"
            "uniform int uS;\n"
            "uniform int uT;\n"
            "uniform int uDestA;\n"
            "float lanePick(vec4 v, uint l){ return l == 0u ? v.r : (l == 1u ? v.g : (l == 2u ? v.b : v.a)); }\n"
            "void main(){\n"
            "    ivec2 P = ivec2(gl_FragCoord.xy);\n"
            "    int lx = P.x / uS; int sx = P.x - lx * uS;\n"
            "    int gy = P.y / uS; int sy = P.y - gy * uS;\n"
            "    int ly = gy - uT * 32;\n"
            "    uvec4 m = texelFetch(uMap, ivec2(lx, uT * 32 + ly), 0);\n"
            "    vec4 own = texelFetch(uA, ivec2(P.x, (uDestA * 32 + ly) * uS + sy), 0);\n"
            "    vec4 r = own;\n"
            "    for (int k = 0; k < 4; ++k) {\n"
            "        uint e = m[k];\n"
            "        if ((e & 0x80000000u) == 0u) continue;\n"
            "        int tile = int((e >> 13) & 255u);\n"
            "        uint lane = (e >> 11) & 3u;\n"
            "        int yy = int((e >> 6) & 31u);\n"
            "        int xx = int(e & 63u);\n"
            "        vec4 s = texelFetch(uA, ivec2(xx * uS + sx, (tile * 32 + yy) * uS + sy), 0);\n"
            "        float b = lanePick(s, lane);\n"
            "        if (k == 0) r.r = b; else if (k == 1) r.g = b; else if (k == 2) r.b = b; else r.a = b;\n"
            "    }\n"
            "    o = r;\n"
            "}\n";
        GLuint vs = 0u, fs = 0u;
        std::string log;
        if (!g158CompileShader(m_f, GL158_VERTEX_SHADER, kVS, vs, log) ||
            !g158CompileShader(m_f, GL158_FRAGMENT_SHADER, kFS, fs, log))
        {
            if (vs != 0u)
                m_f.glDeleteShader_(vs);
            std::fprintf(stderr, "[G767:l2l] FAIL: compile: %s\n", log.c_str());
            m_g767L2lFailed = true;
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
            std::fprintf(stderr, "[G767:l2l] FAIL: link\n");
            m_g767L2lFailed = true;
            return false;
        }
        m_g767L2lProg = prog;
        m_f.glUseProgram_(prog);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(prog, "uA"), 0);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(prog, "uMap"), 1);
        m_locG767L2lS = m_f.glGetUniformLocation_(prog, "uS");
        m_locG767L2lT = m_f.glGetUniformLocation_(prog, "uT");
        m_locG767L2lDestA = m_f.glGetUniformLocation_(prog, "uDestA");
        std::fprintf(stderr, "[G767:l2l] PASS: GPU local->local byte-move program ready\n");
        return true;
    }

    // Grow-only colour texture + FBO pair of (64*S) x rows (physical).
    bool g767L2lEnsure(GLuint &tex, GLuint &fbo, int &haveRows, int needRows, int s)
    {
        if (tex != 0u && fbo != 0u && haveRows >= needRows && m_g767L2lS == s)
            return true;
        if (tex != 0u)
            glDeleteTextures(1, &tex);
        if (fbo == 0u)
            m_f.glGenFramebuffers_(1, &fbo);
        const int rows = std::max(needRows, 32 * s * 8);
        tex = g767MakeTex(64 * s, rows, nullptr);
        if (tex == 0u || fbo == 0u)
            return false;
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo);
        m_f.glFramebufferTexture2D_(GL158_FRAMEBUFFER, GL158_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (m_f.glCheckFramebufferStatus_(GL158_FRAMEBUFFER) != GL158_FRAMEBUFFER_COMPLETE)
            return false;
        haveRows = rows;
        return true;
    }

    bool g767L2lOnWorker(const std::vector<int32_t> &gather, const std::vector<uint32_t> &map,
                         const std::vector<int32_t> &destTiles, const std::vector<int32_t> &scatter)
    {
        const auto fail = [&](const char *stage) {
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr, "[G767:l2l-worker-fail] stage=%s tiles=%zu dest=%zu scatter=%zu\n",
                             stage, gather.size() / 4u, destTiles.size(), scatter.size() / 4u);
            return false;
        };
        const int nA = static_cast<int>(gather.size() / 4u);
        const int nD = static_cast<int>(destTiles.size());
        if (nA <= 0 || nD <= 0 || (gather.size() % 4u) != 0u || (scatter.size() % 4u) != 0u ||
            map.size() != static_cast<size_t>(64 * 32 * nD) * 4u)
            return fail("args");
        if (!g767SetupL2lProgram())
            return fail("program");
        const int s = std::max(1, dc2GetNativeScale());
        // Validate every holder and home before any GL write (nothing lands on a refusal).
        const auto fboOk = [&](int32_t key, int32_t x, int32_t y) -> const FboEntry * {
            auto it = m_fbos.find(static_cast<uint32_t>(key));
            if (it == m_fbos.end() || it->second.fbo == 0u || it->second.colorTex == 0u ||
                it->second.h != 512 || x < 0 || y < 0 || x + 64 > it->second.w || y + 32 > 512)
                return nullptr;
            const int ps = it->second.w > 0
                               ? (it->second.physW > 0 ? it->second.physW : it->second.w) / it->second.w
                               : 0;
            return ps == s ? &it->second : nullptr;
        };
        for (size_t k = 0u; k + 4u <= gather.size(); k += 4u)
            if (fboOk(gather[k], gather[k + 1], gather[k + 2]) == nullptr || gather[k + 3] < 0 ||
                gather[k + 3] >= nA)
                return fail("holder");
        for (size_t k = 0u; k + 4u <= scatter.size(); k += 4u)
            if (fboOk(scatter[k], scatter[k + 1], scatter[k + 2]) == nullptr || scatter[k + 3] < 0 ||
                scatter[k + 3] >= nD)
                return fail("home");
        for (int d = 0; d < nD; ++d)
            if (destTiles[static_cast<size_t>(d)] < 0 || destTiles[static_cast<size_t>(d)] >= nA)
                return fail("desttile");
        if (!g767L2lEnsure(m_g767L2lA, m_g767L2lFboA, m_g767L2lARows, 32 * s * nA, s) ||
            !g767L2lEnsure(m_g767L2lB, m_g767L2lFboB, m_g767L2lBRows, 32 * s * nD, s))
            return fail("atlas");
        m_g767L2lS = s;
        // The map (integer texture), grow-only.
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        if (m_g767L2lM == 0u || m_g767L2lMRows < 32 * nD)
        {
            if (m_g767L2lM != 0u)
                glDeleteTextures(1, &m_g767L2lM);
            glGenTextures(1, &m_g767L2lM);
            m_g767L2lMRows = std::max(32 * nD, 32 * 8);
            glBindTexture(GL_TEXTURE_2D, m_g767L2lM);
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(0x8D70u) /*GL_RGBA32UI*/, 64,
                         m_g767L2lMRows, 0, static_cast<GLenum>(0x8D99u) /*GL_RGBA_INTEGER*/,
                         GL_UNSIGNED_INT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glBindTexture(GL_TEXTURE_2D, m_g767L2lM);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32 * nD, static_cast<GLenum>(0x8D99u),
                        GL_UNSIGNED_INT, map.data());
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glDisable(GL_SCISSOR_TEST);
        // Gather: holder page rect -> atlas tile (GL orientation preserved: tile row 0 = page GS row 31).
        glBindTexture(GL_TEXTURE_2D, m_g767L2lA);
        for (size_t k = 0u; k + 4u <= gather.size(); k += 4u)
        {
            const FboEntry *src = fboOk(gather[k], gather[k + 1], gather[k + 2]);
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, src->fbo);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, gather[k + 3] * 32 * s, gather[k + 1] * s,
                                (511 - (gather[k + 2] + 31)) * s, 64 * s, 32 * s);
        }
        // Assemble every destination page into B.
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g767L2lFboB);
        glViewport(0, 0, 64 * s, m_g767L2lBRows);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_SCISSOR_TEST);
        m_f.glUseProgram_(m_g767L2lProg);
        m_f.glUniform1i_(m_locG767L2lS, s);
        m_f.glBindVertexArray_(m_vao);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_g767L2lA);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, m_g767L2lM);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        for (int d = 0; d < nD; ++d)
        {
            m_f.glUniform1i_(m_locG767L2lT, d);
            m_f.glUniform1i_(m_locG767L2lDestA, destTiles[static_cast<size_t>(d)]);
            glScissor(0, d * 32 * s, 64 * s, 32 * s);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        m_f.glActiveTexture_(GL158_TEXTURE0 + 1u);
        glBindTexture(GL_TEXTURE_2D, 0u);
        m_f.glActiveTexture_(GL158_TEXTURE0);
        // Scatter: every finished page into every home.
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, m_g767L2lFboB);
        for (size_t k = 0u; k + 4u <= scatter.size(); k += 4u)
        {
            const FboEntry *dst = fboOk(scatter[k], scatter[k + 1], scatter[k + 2]);
            glBindTexture(GL_TEXTURE_2D, dst->colorTex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, scatter[k + 1] * s, (511 - (scatter[k + 2] + 31)) * s,
                                0, scatter[k + 3] * 32 * s, 64 * s, 32 * s);
        }
        return true;
    }
