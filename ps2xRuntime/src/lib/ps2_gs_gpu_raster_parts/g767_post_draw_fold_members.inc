    // ===== G767 (members): apply the armed post-draw fold of this batch's FBO into residents =====
    // Called by renderBatch right after the draw loop and BEFORE any readback. Validates every rect
    // of every destination first (all-or-nothing), then issues the copies, then restores the batch
    // FBO binding and viewport so the depth readback / fence code that follows sees the state it
    // expects. The next batch's draw loop resets the cached texture/scissor state at its top (G496).
    void g767ApplyPostDraw(const G178Batch &b, FboEntry &fbo, int viewportW, int viewportH,
                           bool pullsOk)
    {
        const uint32_t armKey = b.fboKey != 0u ? b.fboKey : b.fbp;
        const uint32_t key = g349FboKey(armKey, b.fbW); // the surface this batch really rendered
        std::vector<int32_t> rects;
        {
            std::lock_guard<std::mutex> lk(g_g767Fold.mtx);
            if (!g_g767Fold.armed)
                return;
            g_g767Fold.armed = false;
            if (g_g767Fold.srcKey != armKey)
            {
                ++g_g767Fold.mismatchN;
                g_g767Fold.lastApplied = false;
                return;
            }
            rects.swap(g_g767Fold.rects7);
        }
        // Pass 1: every destination exists and every rect lies inside both surfaces.
        bool ok = pullsOk && !rects.empty() && (rects.size() % 7u) == 0u;
        auto is = m_fbos.find(key);
        if (ok && (is == m_fbos.end() || is->second.fbo == 0))
            ok = false;
        for (size_t k = 0u; ok && k + 7u <= rects.size(); k += 7u)
        {
            const uint32_t dst = static_cast<uint32_t>(rects[k]);
            auto id = m_fbos.find(dst);
            if (dst == key || id == m_fbos.end() || id->second.colorTex == 0)
            {
                ok = false;
                break;
            }
            const FboEntry &s = is->second;
            const FboEntry &d = id->second;
            const int32_t sx = rects[k + 1], sy = rects[k + 2], dx = rects[k + 3], dy = rects[k + 4];
            const int32_t w = rects[k + 5], h = rects[k + 6];
            if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0 || sx > s.w - w ||
                sy > s.h - h || dx > d.w - w || dy > d.h - h)
                ok = false;
            // Every colour FBO must carry the same integer scale (G675) for a page copy to be exact.
            const int ss = s.w > 0 ? (s.physW > 0 ? s.physW : s.w) / s.w : 0;
            const int ds = d.w > 0 ? (d.physW > 0 ? d.physW : d.w) / d.w : 0;
            if (ss <= 0 || ss != ds)
                ok = false;
        }
        size_t n = 0u;
        if (ok)
        {
            std::vector<int32_t> r6;
            r6.reserve(rects.size());
            size_t k = 0u;
            while (k + 7u <= rects.size())
            {
                const uint32_t dst = static_cast<uint32_t>(rects[k]);
                r6.clear();
                while (k + 7u <= rects.size() && static_cast<uint32_t>(rects[k]) == dst)
                {
                    r6.insert(r6.end(), rects.begin() + static_cast<std::ptrdiff_t>(k + 1u),
                              rects.begin() + static_cast<std::ptrdiff_t>(k + 7u));
                    k += 7u;
                    ++n;
                }
                if (!copyColorRectsOnWorker(key, dst, r6))
                {
                    // Cannot happen after pass 1; if it does, name it loudly (bounded).
                    ok = false;
                    static uint32_t s_log = 0u;
                    if (s_log++ < 16u)
                        std::fprintf(stderr,
                                     "[G767:fold-fail] batchFbp=0x%x fbW=%d src=0x%x dst=0x%x rects=%zu "
                                     "(partial fold after validation)\n",
                                     b.fbp, b.fbW, key, dst, r6.size() / 6u);
                }
            }
            m_f.glBindFramebuffer_(GL158_FRAMEBUFFER, fbo.fbo);
            glViewport(0, 0, viewportW, viewportH);
        }
        else
        {
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr,
                             "[G767:fold-refused] batchFbp=0x%x fbW=%d src=0x%x rects=%zu (nothing "
                             "landed)\n",
                             b.fbp, b.fbW, key, rects.size() / 7u);
        }
        std::lock_guard<std::mutex> lk(g_g767Fold.mtx);
        g_g767Fold.lastApplied = ok;
        g_g767Fold.rectsN += n;
        if (ok)
            ++g_g767Fold.appliedN;
        else
            ++g_g767Fold.failN;
    }
