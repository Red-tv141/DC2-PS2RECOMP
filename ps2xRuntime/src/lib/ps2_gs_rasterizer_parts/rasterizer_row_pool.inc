bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool writeDepth;
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false, false};
        case 2: // ZB_ONLY
            return {false, true, false};
        case 3: // RGB_ONLY
            return {true, false, true};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        if (sourcePsm == GS_PSM_T4 ||
            sourcePsm == GS_PSM_T4HL ||
            sourcePsm == GS_PSM_T4HH)
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);
            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        else if ((sourcePsm == GS_PSM_T8 || sourcePsm == GS_PSM_T8H) && csm == 0u)
        {
            // F62: T8H shares the T8 CSM1 CLUT swizzle (spec-correct; the old code
            // only swizzled plain T8).
            clutIndex = swizzleClutIndexCSM1(clutIndex);
        }

        return clutIndex;
    }

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    // G45: private off-screen Z buffer for the costume model RTT (fbp=0x139). The game's Z block
    // (0x1a00) aliases menu text VRAM in the runner's flat 4MB, so storing model Z there both
    // corrupts the text and is corrupted BY the per-frame text sprites -> model flicker. This
    // buffer aliases nothing; it is cleared fully once per costume frame (costumeZClear) and the
    // model's GEQUAL test/write route here. 512x512 covers the whole RTT at native res.
    constexpr uint32_t kCostumeZW = 512u;
    constexpr uint32_t kCostumeZH = 512u;
    inline std::vector<uint32_t> &costumeZBuf()
    {
        static std::vector<uint32_t> buf(kCostumeZW * kCostumeZH, 0u);
        return buf;
    }
    inline uint32_t costumeZRead(int x, int y)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return 0u;
        return costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)];
    }
    inline void costumeZWrite(int x, int y, uint32_t z)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= kCostumeZW || static_cast<uint32_t>(y) >= kCostumeZH)
            return;
        costumeZBuf()[static_cast<uint32_t>(y) * kCostumeZW + static_cast<uint32_t>(x)] = z;
    }
    inline void costumeZClear()
    {
        std::fill(costumeZBuf().begin(), costumeZBuf().end(), 0u);
    }

    int wrapTextureCoordinate(int coordinate, uint8_t mode, int size, int regionMin, int regionMax)
    {
        switch (mode)
        {
        case 0: // REPEAT
            return coordinate & (size - 1);
        case 1: // CLAMP
            return clampInt(coordinate, 0, size - 1);
        case 2: // REGION_CLAMP
            return clampInt(coordinate, regionMin, regionMax);
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) &
                                     static_cast<uint32_t>(regionMin)) |
                                    static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

// G143 (perf, DEFAULT OFF): persistent worker pool that rasterizes DISJOINT scanline ranges of a
// single triangle concurrently. A dispatch (`run`) splits [y0,y1] into `lanes` contiguous chunks;
// lane 0 runs on the CALLER, lanes 1..lanes-1 on workers; `run` blocks until all lanes finish, so
// the triangle is fully drawn before drawTriangle returns (cross-triangle order preserved). Single
// caller only (the guest thread inside a VU1 XGKICK) — dispatches never overlap. No allocation on
// the hot path. Deadlock-safe: workers sleep on a generation counter; the barrier is a pending
// countdown. Correct/exact ONLY when the lane job touches disjoint pixels + thread-local scratch.
class GSRowPool
{
public:
    static GSRowPool &instance()
    {
        static GSRowPool p;
        return p;
    }
    int laneCap() const { return static_cast<int>(m_threads.size()) + 1; }

    void run(int y0, int y1, int lanes, const std::function<void(int, int)> &job)
    {
        lanes = std::max(1, std::min(lanes, laneCap()));
        if (lanes <= 1 || y1 < y0)
        {
            if (y1 >= y0)
                job(y0, y1);
            return;
        }
        const int total = y1 - y0 + 1;
        auto bound = [&](int i) { return y0 + static_cast<int>(static_cast<int64_t>(total) * i / lanes); };

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_job = &job;
            m_y0 = y0;
            m_total = total;
            m_lanes = lanes;
            m_nextLane.store(1, std::memory_order_relaxed);
            m_pending = static_cast<int>(m_threads.size()); // every worker wakes and reports
            ++m_gen;
        }
        m_cvWork.notify_all();

        // Caller runs lane 0 itself (no wasted thread).
        job(bound(0), bound(1) - 1);

        std::unique_lock<std::mutex> lk(m_mtx);
        m_cvDone.wait(lk, [&] { return m_pending == 0; });
        m_job = nullptr;
    }

private:
    GSRowPool()
    {
        unsigned hc = std::thread::hardware_concurrency();
        int n = (hc > 1u) ? static_cast<int>(hc) - 1 : 0;
        n = std::min(n, 15);
        for (int i = 0; i < n; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }
    ~GSRowPool()
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
            ++m_gen;
        }
        m_cvWork.notify_all();
        for (auto &t : m_threads)
            if (t.joinable())
                t.join();
    }
    void workerLoop()
    {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lk(m_mtx);
        for (;;)
        {
            m_cvWork.wait(lk, [&] { return m_stop || m_gen != seen; });
            if (m_stop)
                return;
            seen = m_gen;
            const std::function<void(int, int)> *job = m_job;
            const int y0 = m_y0, total = m_total, lanes = m_lanes;
            for (;;)
            {
                const int lane = m_nextLane.fetch_add(1, std::memory_order_relaxed);
                if (lane >= lanes)
                    break;
                const int a = y0 + static_cast<int>(static_cast<int64_t>(total) * lane / lanes);
                const int b = y0 + static_cast<int>(static_cast<int64_t>(total) * (lane + 1) / lanes) - 1;
                lk.unlock();
                if (job && b >= a)
                    (*job)(a, b);
                lk.lock();
            }
            if (--m_pending == 0)
                m_cvDone.notify_one();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mtx;
    std::condition_variable m_cvWork, m_cvDone;
    const std::function<void(int, int)> *m_job = nullptr;
    int m_y0 = 0, m_total = 0, m_lanes = 1, m_pending = 0;
    std::atomic<int> m_nextLane{0};
    uint64_t m_gen = 0;
    bool m_stop = false;
};