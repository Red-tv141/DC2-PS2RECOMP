void g50Record(uint32_t tbp, bool degen,
                   float x0, float y0, float x1, float y1, float x2, float y2)
    {
        std::lock_guard<std::mutex> lk(g_g50Mutex);
        G50PartStat &s = g_g50Parts[tbp];
        s.nTotal++;
        float mnx = std::min({x0, x1, x2}), mxx = std::max({x0, x1, x2});
        float mny = std::min({y0, y1, y2}), mxy = std::max({y0, y1, y2});
        if (degen)
        {
            s.nDegen++;
            s.dMinX = std::min(s.dMinX, mnx);
            s.dMaxX = std::max(s.dMaxX, mxx);
            s.dMinY = std::min(s.dMinY, mny);
            s.dMaxY = std::max(s.dMaxY, mxy);
        }
        else
        {
            s.minX = std::min(s.minX, mnx);
            s.maxX = std::max(s.maxX, mxx);
            s.minY = std::min(s.minY, mny);
            s.maxY = std::max(s.maxY, mxy);
        }
    }

    void g50DumpMaybe()
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % 4000ull != 0ull)
            return;
        std::lock_guard<std::mutex> lk(g_g50Mutex);
        std::fprintf(stderr, "[G50:parts] === snapshot @%llu fbp139 model tris ===\n",
                     (unsigned long long)n);
        for (auto &kv : g_g50Parts)
        {
            const G50PartStat &s = kv.second;
            std::fprintf(stderr,
                         "[G50:parts] tbp=0x%04x total=%llu degen=%llu (%.0f%%) "
                         "drawn=(%.0f,%.0f)-(%.0f,%.0f) degen=(%.0f,%.0f)-(%.0f,%.0f)\n",
                         kv.first, (unsigned long long)s.nTotal, (unsigned long long)s.nDegen,
                         s.nTotal ? 100.0 * (double)s.nDegen / (double)s.nTotal : 0.0,
                         s.minX, s.minY, s.maxX, s.maxY,
                         s.dMinX, s.dMinY, s.dMaxX, s.dMaxY);
        }
    }

    uint32_t g43SkipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G43_SKIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    bool f50_12_trace_enabled()
    {
        static const bool enabled = envFlagEnabled("DC2_TRACE_F50_12");
        return enabled;
    }

    uint64_t f50_12_tex0_raw(const GSTex0Reg &tex)
    {
        return static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) |
               (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 14) |
               (static_cast<uint64_t>(tex.psm & 0x3Fu) << 20) |
               (static_cast<uint64_t>(tex.tw & 0xFu) << 26) |
               (static_cast<uint64_t>(tex.th & 0xFu) << 30) |
               (static_cast<uint64_t>(tex.tcc & 0x1u) << 34) |
               (static_cast<uint64_t>(tex.tfx & 0x3u) << 35) |
               (static_cast<uint64_t>(tex.cbp & 0x3FFFu) << 37) |
               (static_cast<uint64_t>(tex.cpsm & 0xFu) << 51) |
               (static_cast<uint64_t>(tex.csm & 0x1u) << 55) |
               (static_cast<uint64_t>(tex.csa & 0x1Fu) << 56) |
               (static_cast<uint64_t>(tex.cld & 0x7u) << 61);
    }

    void g195LogForegroundPixel(const GSContext &ctx,
                                int x,
                                int y,
                                uint32_t off,
                                uint32_t bytesPerPixel,
                                uint32_t primType,
                                bool primTme,
                                bool primAbe,
                                bool primFst,
                                bool primCtxt,
                                bool pabe,
                                uint32_t dstBefore,
                                uint8_t srcR,
                                uint8_t srcG,
                                uint8_t srcB,
                                uint8_t srcA,
                                uint32_t finalRgba)
    {
        static std::atomic<uint32_t> s_count{0};
        const uint32_t n = s_count.fetch_add(1u, std::memory_order_relaxed);
        if (n >= 1024u)
            return;
        std::fprintf(stderr,
            "[G195:fgpx] n=%u blk=%d xy=(%d,%d) fbp=0x%x fbw=%u fpsm=0x%x off=0x%x bpp=%u "
            "prim=%u tme=%u abe=%u fst=%u ctxt=%u tbp=0x%x tbw=%u tpsm=0x%x tw=%u th=%u "
            "cbp=0x%x csa=%u tex0=0x%016llx alpha=0x%llx test=0x%llx zbuf=0x%llx fba=0x%llx pabe=%u "
            "src=%02x,%02x,%02x,%02x dst=%08x final=%08x scissor=(%d,%d)-(%d,%d)\n",
            n, g_dc2TitleCurBlock.load(std::memory_order_relaxed),
            x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
            static_cast<uint32_t>(ctx.frame.psm), off, bytesPerPixel, primType,
            primTme ? 1u : 0u, primAbe ? 1u : 0u, primFst ? 1u : 0u,
            primCtxt ? 1u : 0u, ctx.tex0.tbp0,
            static_cast<uint32_t>(ctx.tex0.tbw), static_cast<uint32_t>(ctx.tex0.psm),
            static_cast<uint32_t>(ctx.tex0.tw), static_cast<uint32_t>(ctx.tex0.th),
            ctx.tex0.cbp, static_cast<uint32_t>(ctx.tex0.csa),
            static_cast<unsigned long long>(f50_12_tex0_raw(ctx.tex0)),
            static_cast<unsigned long long>(ctx.alpha),
            static_cast<unsigned long long>(ctx.test),
            static_cast<unsigned long long>(ctx.zbuf),
            static_cast<unsigned long long>(ctx.fba),
            pabe ? 1u : 0u,
            srcR, srcG, srcB, srcA, dstBefore, finalRgba,
            ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
    }

    struct F50_12FramebufferBucket
    {
        std::atomic<uint64_t> commits{0};
        std::atomic<uint64_t> rgbNz{0};
        std::atomic<uint64_t> rgbNzLogged{0};
    };

    void f50_12_log_framebuffer_commit(const GSContext &ctx,
                                       int x,
                                       int y,
                                       uint32_t off,
                                       uint32_t bytesPerPixel,
                                       uint32_t finalRgba,
                                       uint32_t primType,
                                       bool primTme,
                                       bool primAbe,
                                       bool primFst,
                                       bool primCtxt)
    {
        if (!f50_12_trace_enabled())
            return;

        static F50_12FramebufferBucket s_buckets[4];
        const uint32_t fbp = ctx.frame.fbp;
        const uint32_t bucketIndex =
            (fbp == 0u) ? 0u :
            (fbp == 0x68u) ? 1u :
            (fbp == 0x139u) ? 2u : 3u;
        const char *bucketName =
            (bucketIndex == 0u) ? "fbp0" :
            (bucketIndex == 1u) ? "fbp68" :
            (bucketIndex == 2u) ? "fbp139" : "other";

        F50_12FramebufferBucket &bucket = s_buckets[bucketIndex];
        const uint64_t bucketN = bucket.commits.fetch_add(1, std::memory_order_relaxed) + 1ull;
        const bool rgbNz = (finalRgba & 0x00FFFFFFu) != 0u;
        uint64_t rgbNzTotal = bucket.rgbNz.load(std::memory_order_relaxed);
        bool shouldLog = bucketN <= 16ull || (bucketN & 0xFFFFFull) == 0ull;
        if (rgbNz)
        {
            rgbNzTotal = bucket.rgbNz.fetch_add(1, std::memory_order_relaxed) + 1ull;
            const uint64_t logged = bucket.rgbNzLogged.fetch_add(1, std::memory_order_relaxed) + 1ull;
            shouldLog = shouldLog || logged <= 16ull || (logged & 0xFFFFull) == 0ull;
        }

        if (!shouldLog)
            return;

        const auto &tex = ctx.tex0;
        std::fprintf(stderr,
                     "[F512:pxfbp] bucket=%s bucketN=%llu rgbNz=%llu fbp=0x%x fbw=%u fpsm=0x%x "
                     "prim=%u tme=%u abe=%u fst=%u ctxt=%u tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x "
                     "xy=(%d,%d) off=0x%x bpp=%u final=0x%08x\n",
                     bucketName,
                     static_cast<unsigned long long>(bucketN),
                     static_cast<unsigned long long>(rgbNzTotal),
                     fbp,
                     ctx.frame.fbw,
                     static_cast<uint32_t>(ctx.frame.psm),
                     primType,
                     primTme ? 1u : 0u,
                     primAbe ? 1u : 0u,
                     primFst ? 1u : 0u,
                     primCtxt ? 1u : 0u,
                     static_cast<unsigned long long>(f50_12_tex0_raw(tex)),
                     tex.tbp0,
                     tex.tbw,
                     static_cast<uint32_t>(tex.psm),
                     tex.cbp,
                     static_cast<uint32_t>(tex.cpsm),
                     x,
                     y,
                     off,
                     bytesPerPixel,
                     finalRgba);
    }

    // PHASE F50.8: per-sample texel-color histogram. Quiet unless DC2_TRACE_RENDER_QUALITY.
    // Answers: are textured dungeon draws sampling RGB=0 (black), and is it the paletted path?
    void f508LogSample(uint32_t psm, uint32_t color)
    {
        static std::atomic<uint64_t> total{0};
        static std::atomic<uint64_t> rgbZero{0};
        static std::atomic<uint64_t> palTotal{0};
        static std::atomic<uint64_t> palZero{0};
        const bool paletted = (psm == GS_PSM_T4 || psm == GS_PSM_T4HL ||
                               psm == GS_PSM_T4HH || psm == GS_PSM_T8);
        const bool z = (color & 0x00FFFFFFu) == 0u;
        const uint64_t t = total.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (z)
            rgbZero.fetch_add(1, std::memory_order_relaxed);
        if (paletted)
        {
            palTotal.fetch_add(1, std::memory_order_relaxed);
            if (z)
                palZero.fetch_add(1, std::memory_order_relaxed);
        }
        if ((t & 0xFFFFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F508:texsum] total=%llu rgbZero=%llu palTotal=%llu palZero=%llu\n",
                         static_cast<unsigned long long>(t),
                         static_cast<unsigned long long>(rgbZero.load()),
                         static_cast<unsigned long long>(palTotal.load()),
                         static_cast<unsigned long long>(palZero.load()));
        }
    }

    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    thread_local bool g_g104TriClipReentry = false;

    bool g104TriClipEnabled()
    {
        // G139: RETIRED by default. The G138 FM-swap/MAC-pipeline root fix plus the G139
        // same-pair VF hazard fix make the natural VU gate + positions HW-faithful; the
        // rasterizer-side near-plane tri clip is no longer needed (A/B g100_g139nb_keepz.png
        // == g100_g139fix.png). Re-enable with DC2_G104_FORCE_TRI_CLIP=1.
        static const bool enabled = std::getenv("DC2_G104_FORCE_TRI_CLIP") != nullptr;
        return enabled;
    }

    float g104Wnear()
    {
        static const float value = []() -> float {
            const char *e = std::getenv("DC2_G104_WNEAR");
            const float v = e ? static_cast<float>(std::atof(e)) : 4.0f;
            return (std::isfinite(v) && v > 0.0f) ? v : 4.0f;
        }();
        return value;
    }

    bool g104ScreenClipEnabled()
    {
        // G125 (DECISIVE, default-ON homog): the title camera/projection matrix matches HW
        // byte-for-byte (G124's "X-scale" was refuted), so the title cavern transforms correctly;
        // the residual defect is the near-plane clip of straddling tristrips. The old DEFAULT
        // screen-space lerp interpolated screen XY of a behind vertex whose 12.4-fixed position is
        // already saturated -> wrong intersection -> dark "sheets". The homogeneous path
        // reconstructs clip = screen*W (q=1/W preserved in GSVertex) and interpolates in clip
        // space = the mathematically correct intersection. Make homog the DEFAULT; restore the old
        // screen-space clip with DC2_G125_NO_HOMOG_CLIP=1 (or DC2_G104_SCREEN_CLIP=1).
        static const bool screen =
            envFlagEnabled("DC2_G125_NO_HOMOG_CLIP") ||
            envFlagEnabled("DC2_G104_SCREEN_CLIP");
        return screen;
    }

    float g104Qnear()
    {
        static const float value = []() -> float {
            const char *e = std::getenv("DC2_G104_QNEAR");
            const float v = e ? static_cast<float>(std::atof(e)) : 1.0e-4f;
            return (std::isfinite(v) && v > 0.0f) ? v : 1.0e-4f;
        }();
        return value;
    }

    bool g104TitleRockTriangle(bool primTme, uint32_t fbp, uint32_t tbp)
    {
        return g144EffectiveTitleRockScope() &&
               primTme &&
               fbp != 0x139u &&
               tbp >= 0x2720u &&
               tbp <= 0x3960u;
    }

    bool g106TitleZEnabled()
    {
        // G125 (default-ON): the title cavern has real depth. Without a depth test the back
        // geometry overdraws the bright front mural in painter order -> dark/muddy center (the
        // mural vanished even when the geometry+clip were correct). A private, per-frame-cleared,
        // non-aliasing title Z buffer (g106TitleZPrivateEnabled) restores HW-correct ordering so
        // the front mural shows. Honors the guest's own ZTE/ZTST. Tightly title-scoped
        // (g106TitleZScope: title-rock tris, fbp 0/0x68). Kill with DC2_G125_NO_TITLE_Z=1.
        static const bool enabled = !envFlagEnabled("DC2_G125_NO_TITLE_Z");
        return enabled;
    }

    bool g106TitleZPrivateEnabled()
    {
        static const bool enabled = (std::getenv("DC2_G106_TITLE_Z_VRAM") == nullptr);
        return enabled;
    }

    bool g106TitleZTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G106_TITLE_Z_STAT");
        return enabled;
    }

    bool g202TownZEnabled()
    {
        // G202 (default-ON): normal town/field display targets use the guest's real ZBUF/TEST
        // state instead of painter's order. DC2_G202_NO_TOWN_Z kills the promoted behavior; the
        // older G195 opt-in still forces it for A/B against the original diagnostic.
        static const bool enabled =
            !envFlagEnabled("DC2_G202_NO_TOWN_Z") || envFlagEnabled("DC2_G195_TOWN_Z");
        return enabled;
    }

    bool g202TownZTraceEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G202_TOWN_Z_STAT");
        return enabled;
    }

    bool g202TownZForced()
    {
        static const bool enabled = envFlagEnabled("DC2_G195_TOWN_Z");
        return enabled;
    }

    bool g203UniversalZEnabled()
    {
        // G203 (default-ON): honor the guest's real ZBUF/TEST for EVERY draw + primitive against the
        // real VRAM Z buffer, and let the game's own full-screen ztst=ALWAYS,zmsk=0 clear-sprite
        // populate it (drawSprite now writes Z). Replaces the per-screen Z whitelist — title
        // private-Z (g106), costume private-Z (fbp 0x139), and the town game-state scope + artificial
        // Z-page clear (g202) — which was not HW-faithful and leaked: g202's blind clear of zbp 0xd0
        // (= VRAM block 0x1a00, the debug/menu font staging per G5) corrupted inventory/loading
        // textures whenever the town scope stayed active into a menu. Kill DC2_G203_LEGACY_Z=1 to
        // restore the old scoped/private paths for A/B. DC2_NO_ZTEST=1 still disables Z globally.
        static const bool enabled = !envFlagEnabled("DC2_G203_LEGACY_Z");
        return enabled;
    }

    bool g202TownZScope(uint32_t fbp, uint32_t zbpPage, uint32_t zpsm)
    {
        return g202TownZEnabled() &&
               (g202TownZForced() || g144EffectiveTownDepthScope()) &&
               !g144EffectiveTitleRockScope() &&
               (fbp == 0u || fbp == 0x68u) &&
               zbpPage == 0xd0u &&
               zpsm == 0x1u; // Z24, matching the MAP-0 HW dump's town scene.
    }

    void g202TownZClear(uint8_t *vram, uint32_t fbp, uint32_t zbpBlock, uint32_t zbw)
    {
        if (vram == nullptr)
            return;
        const uint32_t width = std::max(1u, std::min(1024u, zbw * 64u));
        constexpr uint32_t kHeight = 512u;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < width; ++x)
                GSMem::WriteZ24(vram, zbpBlock, zbw, x, y, 0u);
        if (g202TownZTraceEnabled())
        {
            static std::atomic<uint32_t> s_clearTrace{0};
            const uint32_t n = s_clearTrace.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u || (n % 120u) == 0u)
                std::fprintf(stderr, "[G202:zclear] n=%u fbp=0x%x zbp=0x%x zbw=%u size=%ux%u\n",
                             n, fbp, zbpBlock, zbw, width, kHeight);
        }
    }

    void g202TownZClearIfNeeded(uint8_t *vram, uint32_t fbp, uint32_t zbuf, uint32_t fbw)
    {
        const uint32_t zbpPage = static_cast<uint32_t>(zbuf & 0x1FFu);
        const uint32_t zpsm = static_cast<uint32_t>((zbuf >> 24) & 0xFu);
        if (!g202TownZScope(fbp, zbpPage, zpsm))
            return;
        const uint32_t zbw = (fbw != 0u) ? fbw : 8u;
        const uint32_t key = (fbp & 0x1FFu) | (zbpPage << 9) | ((zbw & 0x3Fu) << 18) | (zpsm << 24);
        static std::atomic<uint32_t> s_lastKey{0xFFFFFFFFu};
        const uint32_t prev = s_lastKey.exchange(key, std::memory_order_relaxed);
        if (prev == key)
            return;
        g202TownZClear(vram, fbp, zbpPage << 5, zbw);
    }

    bool g106TitleZScope(bool primTme, uint32_t fbp, uint32_t tbp)
    {
        return g106TitleZEnabled() &&
               (fbp == 0u || fbp == 0x68u) &&
               g104TitleRockTriangle(primTme, fbp, tbp);
    }

    constexpr uint32_t kG106TitleZW = 512u;
    constexpr uint32_t kG106TitleZH = 512u;

    uint32_t g106TitleZIndex(uint32_t fbp)
    {
        return (fbp == 0x68u) ? 1u : 0u;
    }

    std::vector<uint32_t> &g106TitleZBuf(uint32_t fbp)
    {
        static std::vector<uint32_t> s_buf0(kG106TitleZW * kG106TitleZH, 0u);
        static std::vector<uint32_t> s_buf68(kG106TitleZW * kG106TitleZH, 0u);
        return (g106TitleZIndex(fbp) == 0u) ? s_buf0 : s_buf68;
    }

    void g106TitleZClear(uint32_t fbp)
    {
        auto &buf = g106TitleZBuf(fbp);
        std::fill(buf.begin(), buf.end(), 0u);
    }

    void g106TitleZClearIfNeeded(uint32_t fbp)
    {
        static std::atomic<uint32_t> s_lastFbp{0xFFFFFFFFu};
        const uint32_t prev = s_lastFbp.exchange(fbp, std::memory_order_relaxed);
        if (prev == fbp)
            return;
        g106TitleZClear(fbp);
        if (g106TitleZTraceEnabled())
            std::fprintf(stderr, "[G106:zclear] fbp=0x%x private=%u\n",
                         fbp, g106TitleZPrivateEnabled() ? 1u : 0u);
    }

    uint32_t g106TitleZRead(int x, int y, uint32_t fbp)
    {
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= kG106TitleZW ||
            static_cast<uint32_t>(y) >= kG106TitleZH)
            return 0u;
        return g106TitleZBuf(fbp)[static_cast<uint32_t>(y) * kG106TitleZW + static_cast<uint32_t>(x)];
    }

    void g106TitleZWrite(int x, int y, uint32_t fbp, uint32_t z)
    {
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= kG106TitleZW ||
            static_cast<uint32_t>(y) >= kG106TitleZH)
            return;
        g106TitleZBuf(fbp)[static_cast<uint32_t>(y) * kG106TitleZW + static_cast<uint32_t>(x)] = z;
    }

    uint32_t g106SkipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G106_SKIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    int g106SkipPrim()
    {
        static const int value = []() {
            const char *e = std::getenv("DC2_G106_SKIP_PRIM");
            return e ? std::atoi(e) : -1;
        }();
        return value;
    }

    bool g106SkipStatEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_G106_SKIP_STAT");
        return enabled;
    }

    uint32_t g106NoTriClipTbp()
    {
        static const uint32_t value = []() {
            const char *e = std::getenv("DC2_G106_NO_TRICLIP_TBP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0xFFFFFFFFu;
        }();
        return value;
    }

    int g106NoTriClipPrim()
    {
        static const int value = []() {
            const char *e = std::getenv("DC2_G106_NO_TRICLIP_PRIM");
            return e ? std::atoi(e) : -1;
        }();
        return value;
    }

    bool g106NoTriClipTriangle(bool primTme, uint32_t primType, uint32_t fbp, uint32_t tbp)
    {
        const uint32_t noClipTbp = g106NoTriClipTbp();
        const int noClipPrim = g106NoTriClipPrim();
        if (noClipTbp == 0xFFFFFFFFu && noClipPrim < 0)
            return false;
        if (!g104TitleRockTriangle(primTme, fbp, tbp))
            return false;
        if (noClipTbp != 0xFFFFFFFFu && tbp != noClipTbp)
            return false;
        if (noClipPrim >= 0 && static_cast<uint32_t>(noClipPrim) != primType)
            return false;
        return true;
    }

    bool g106ShouldSkipTriangle(bool primTme, uint32_t primType, uint32_t fbp, uint32_t tbp)
    {
        const uint32_t skipTbp = g106SkipTbp();
        const int skipPrim = g106SkipPrim();
        if (skipTbp == 0xFFFFFFFFu && skipPrim < 0)
            return false;
        if (!g104TitleRockTriangle(primTme, fbp, tbp))
            return false;
        if (skipTbp != 0xFFFFFFFFu && tbp != skipTbp)
            return false;
        if (skipPrim >= 0 && static_cast<uint32_t>(skipPrim) != primType)
            return false;
        return true;
    }

    bool g104InsideQ(const GSVertex &v)
    {
        // G126: "inside" = in FRONT of the near plane, i.e. view-space depth W=1/q >= wNear.
        // The old test (q>0) also accepted verts BETWEEN the camera and the near plane (tiny
        // W, huge q) whose projected screen XY is extreme -> "stretched" triangles spanning the
        // screen. Treat those as outside so the near-plane clip pulls the edge onto the plane
        // (W=wNear) instead of passing the triangle stretched. q<=0 (behind camera) stays
        // outside. Restore the old behaviour with DC2_G126_NO_NEAR_CULL=1.
        if (!std::isfinite(v.q) || v.q <= 0.0f)
            return false;
        static const bool s_noNearCull = envFlagEnabled("DC2_G126_NO_NEAR_CULL");
        if (s_noNearCull)
            return true;
        const float wNear = g104Wnear();
        const float qNear = (wNear > 0.0f) ? (1.0f / wNear) : 1.0e9f; // q at the near plane
        return v.q <= qNear;
    }

    float g104SafeW(float q)
    {
        if (!std::isfinite(q))
            return (q < 0.0f) ? -1.0e8f : 1.0e8f;
        if (std::fabs(q) < 1.0e-8f)
            return (q < 0.0f) ? -1.0e8f : 1.0e8f;
        return 1.0f / q;
    }

    float g104Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    uint8_t g104LerpU8(uint8_t a, uint8_t b, float t)
    {
        return clampU8(static_cast<int>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t)));
    }

    uint16_t g104LerpU16(uint16_t a, uint16_t b, float t)
    {
        const int v = static_cast<int>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
        return static_cast<uint16_t>(clampInt(v, 0, 0xFFFF));
    }

    GSVertex g104IntersectNearW(const GSVertex &a, const GSVertex &b, float wNear)
    {
        if (g104ScreenClipEnabled())
        {
            const float qNear = g104Qnear();
            const float denomQ = b.q - a.q;
            float t = (std::fabs(denomQ) > 1.0e-8f) ? ((qNear - a.q) / denomQ) : 0.0f;
            t = std::max(0.0f, std::min(1.0f, t));
            GSVertex out{};
            out.x = g104Lerp(a.x, b.x, t);
            out.y = g104Lerp(a.y, b.y, t);
            out.z = g104Lerp(a.z, b.z, t);
            out.r = g104LerpU8(a.r, b.r, t);
            out.g = g104LerpU8(a.g, b.g, t);
            out.b = g104LerpU8(a.b, b.b, t);
            out.a = g104LerpU8(a.a, b.a, t);
            out.q = qNear;
            out.s = g104Lerp(a.s, b.s, t);
            out.t = g104Lerp(a.t, b.t, t);
            out.u = g104LerpU16(a.u, b.u, t);
            out.v = g104LerpU16(a.v, b.v, t);
            out.fog = g104LerpU8(a.fog, b.fog, t);
            return out;
        }

        const float wa = g104SafeW(a.q);
        const float wb = g104SafeW(b.q);
        const float denom = wb - wa;
        float t = (std::fabs(denom) > 1.0e-8f) ? ((wNear - wa) / denom) : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));

        GSVertex out{};
        auto clipLerp = [&](float av, float bv) -> float {
            const float ac = std::isfinite(av) ? (av * wa) : 0.0f;
            const float bc = std::isfinite(bv) ? (bv * wb) : 0.0f;
            return (ac + (bc - ac) * t) / wNear;
        };

        out.x = clipLerp(a.x, b.x);
        out.y = clipLerp(a.y, b.y);
        out.z = clipLerp(a.z, b.z);
        out.r = g104LerpU8(a.r, b.r, t);
        out.g = g104LerpU8(a.g, b.g, t);
        out.b = g104LerpU8(a.b, b.b, t);
        out.a = g104LerpU8(a.a, b.a, t);
        out.q = 1.0f / wNear;
        out.s = g104Lerp(a.s, b.s, t);
        out.t = g104Lerp(a.t, b.t, t);
        out.u = g104LerpU16(a.u, b.u, t);
        out.v = g104LerpU16(a.v, b.v, t);
        out.fog = g104LerpU8(a.fog, b.fog, t);
        return out;
    }

    uint32_t decodePSMCT16(uint16_t pixel)
    {
        const uint32_t r = ((pixel >> 0) & 0x1Fu) << 3;
        const uint32_t g = ((pixel >> 5) & 0x1Fu) << 3;
        const uint32_t b = ((pixel >> 10) & 0x1Fu) << 3;
        const uint32_t a = (pixel & 0x8000u) ? 0x80u : 0u;
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint16_t encodePSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};