    // ===== G766 (members): apply the armed post-upload page pulls to this batch's FBO =====
    // Called by renderBatch right after the VRAM seed upload (and the G716 seed overlay) and
    // before the first draw. Leaves the batch FBO bound with the full physical viewport, which is
    // exactly the state the draw loop expects; the draw loop resets the cached texture/scissor
    // state at its top (G496), so the copies' raw binds cannot desync the cache.
    // Returns false only when pulls WERE armed for this batch and did not all land (G767 then refuses
    // to fold the batch's result into any resident: the render saw a stale seed).
    bool g766ApplyPostUpload(const G178Batch &b, FboEntry &fbo, int viewportW, int viewportH)
    {
        const uint32_t armKey = b.fboKey != 0u ? b.fboKey : b.fbp;
        const uint32_t key = g349FboKey(armKey, b.fbW); // the surface this batch really renders
        std::vector<int32_t> rects;
        {
            std::lock_guard<std::mutex> lk(g_g766Post.mtx);
            if (!g_g766Post.armed)
                return true;
            g_g766Post.armed = false;
            if (g_g766Post.dstKey != armKey)
            {
                ++g_g766Post.mismatchN;
                g_g766Post.lastApplied = false;
                return false;
            }
            rects.swap(g_g766Post.rects7);
        }
        bool ok = true;
        std::vector<int32_t> r6;
        r6.reserve(rects.size());
        size_t k = 0u;
        size_t n = 0u;
        while (k + 7u <= rects.size())
        {
            const uint32_t src = static_cast<uint32_t>(rects[k]);
            r6.clear();
            while (k + 7u <= rects.size() && static_cast<uint32_t>(rects[k]) == src)
            {
                r6.insert(r6.end(), rects.begin() + static_cast<std::ptrdiff_t>(k + 1u),
                          rects.begin() + static_cast<std::ptrdiff_t>(k + 7u));
                k += 7u;
                ++n;
            }
            const bool copied = copyColorRectsOnWorker(src, key, r6);
            if (!copied)
            {
                // G766: name the refused copy (bounded). A refusal means the page did not land.
                static uint32_t s_log = 0u;
                if (s_log++ < 24u)
                {
                    auto is = m_fbos.find(src);
                    auto id = m_fbos.find(key);
                    std::fprintf(stderr,
                                 "[G766:post-fail] batchFbp=0x%x fbW=%d armKey=0x%x dst=0x%x(%dx%d) "
                                 "src=0x%x(%dx%d) rects=%zu r0=(%d,%d)->(%d,%d) %dx%d\n",
                                 b.fbp, b.fbW, armKey, key,
                                 id != m_fbos.end() ? id->second.w : -1,
                                 id != m_fbos.end() ? id->second.h : -1, src,
                                 is != m_fbos.end() ? is->second.w : -1,
                                 is != m_fbos.end() ? is->second.h : -1, r6.size() / 6u,
                                 r6.size() >= 6u ? r6[0] : -1, r6.size() >= 6u ? r6[1] : -1,
                                 r6.size() >= 6u ? r6[2] : -1, r6.size() >= 6u ? r6[3] : -1,
                                 r6.size() >= 6u ? r6[4] : -1, r6.size() >= 6u ? r6[5] : -1);
                }
            }
            ok = copied && ok;
        }
        m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
        glViewport(0, 0, viewportW, viewportH);
        std::lock_guard<std::mutex> lk(g_g766Post.mtx);
        g_g766Post.lastApplied = ok;
        g_g766Post.rectsN += n;
        if (ok)
            ++g_g766Post.appliedN;
        else
            ++g_g766Post.failN;
        return ok;
    }
