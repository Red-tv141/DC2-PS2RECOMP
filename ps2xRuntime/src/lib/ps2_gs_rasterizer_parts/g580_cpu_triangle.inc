// G580: exact CPU leaves for replayed textured/blended/depth-tested FBP 0x13b triangles.
//
// Coverage, interpolation, perspective correction, bilinear T8 sampling, CLUT lookup, and texture
// combine stay in drawTriangle's proven scalar loop.  This file lowers only the two measured leaves:
// row-hoisted access to G403's authoritative shared-Z mirror, and the exact CT32 ALPHA=0x44 commit.
// Bring-up is default-OFF.  DC2_G580_VERIFY_TRI_13B keeps generic reads/commits authoritative and
// compares every selected Z value and committed word before performance may be measured.

namespace
{

inline bool g580Tri13bVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G580_VERIFY_TRI_13B");
    return on;
}

inline bool g580Tri13bStatOn()
{
    static const bool on = envFlagEnabled("DC2_G580_STAT") || g580Tri13bVerifyOn();
    return on;
}

inline bool g580Tri13bCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G580_CENSUS");
    return on;
}

inline bool g580Tri13bRequested()
{
    static const bool requested = []() {
        const char *ab = std::getenv("DC2_G419_AB");
        return envFlagEnabled("DC2_G580_TRI_13B") || g580Tri13bVerifyOn() ||
               g580Tri13bCensusOn() ||
               (ab != nullptr && std::strcmp(ab, "tri13bfast") == 0);
    }();
    return requested;
}

inline bool g580Tri13bSelected()
{
    static const bool paired = []() {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "tri13bfast") == 0;
    }();
    if (paired)
        return g580Tri13bFastArm() > 0;
    static const bool on = !envFlagEnabled("DC2_G580_NO_TRI_13B") &&
                           (envFlagEnabled("DC2_G580_TRI_13B") || g580Tri13bVerifyOn());
    return on;
}

inline bool g580Tri13bQuiet()
{
    static const bool quiet =
        g570Cpu139Quiet() &&
        !envFlagEnabled("DC2_G148_SAMPLESTAT") &&
        !envFlagEnabled("DC2_G149_TCVERIFY") &&
        !envFlagEnabled("DC2_G141_NO_TEXQUAD") &&
        !envFlagEnabled("DC2_G222_AFFINE_TEX") &&
        !envFlagEnabled("DC2_G408_INTEGER_STQ_CENTER") &&
        !envFlagEnabled("DC2_G580_CENSUS");
    return quiet;
}

// Exact ALPHA=0x44: ((Cs - Cd) * As >> 7) + Cd.  The caller requires CT32, FBMSK=0,
// FBA=0 and PABE=0, so the resulting alpha byte is the source alpha.
inline bool g580Commit44(uint8_t *vram, size_t vramSize,
                         const G530RowAddr &row, uint32_t groupBase, int x,
                         uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
                         uint32_t &off, uint32_t &pixel)
{
    off = groupBase + static_cast<uint32_t>(row.ct[x & 7]) * 4u;
    if (off + 4u > vramSize)
        return false;
    uint32_t dst = 0u;
    std::memcpy(&dst, vram + off, sizeof(dst));
    const int dr = static_cast<int>(dst & 0xffu);
    const int dg = static_cast<int>((dst >> 8u) & 0xffu);
    const int db = static_cast<int>((dst >> 16u) & 0xffu);
    const int alpha = static_cast<int>(sa);
    const uint8_t r = clampU8(((static_cast<int>(sr) - dr) * alpha >> 7) + dr);
    const uint8_t g = clampU8(((static_cast<int>(sg) - dg) * alpha >> 7) + dg);
    const uint8_t b = clampU8(((static_cast<int>(sb) - db) * alpha >> 7) + db);
    pixel = static_cast<uint32_t>(r) |
            (static_cast<uint32_t>(g) << 8u) |
            (static_cast<uint32_t>(b) << 16u) |
            (static_cast<uint32_t>(sa) << 24u);
    return true;
}

// Revision 2: G580's first leaf-only kernel left replay cost unchanged because the Ridepod
// population commits only about 6.4 pixels per replayed triangle. The expensive repeated work is
// the lazy per-triangle CLUT decode: each band copy starts a fresh 256-byte validity map and resolves
// the same palette entries again. A command-graph batch cannot contain a CLUT-overlapping mutation,
// so one fully decoded palette per exact CLUT state is immutable for the complete parallel replay.
struct G580ClutKey
{
    uint32_t cbp = 0u;
    uint8_t cpsm = 0u, csm = 0u, csa = 0u, sourcePsm = 0u;
    uint8_t ta0 = 0u, ta1 = 0u, cbw = 0u, cou = 0u;
    uint16_t cov = 0u;
    bool aem = false;
};

inline bool g580SameClutKey(const G580ClutKey &a, const G580ClutKey &b)
{
    return a.cbp == b.cbp && a.cpsm == b.cpsm && a.csm == b.csm &&
           a.csa == b.csa && a.sourcePsm == b.sourcePsm && a.ta0 == b.ta0 &&
           a.ta1 == b.ta1 && a.cbw == b.cbw && a.cou == b.cou &&
           a.cov == b.cov && a.aem == b.aem;
}

struct G580SharedSample
{
    bool ready = false;
    uint16_t keyIndex = 0u;
    uint16_t tableIndex = 0u;
    uint32_t tbp0 = 0u, pageBw = 1u;
    int texW = 1, texH = 1;
    int minU = 0, maxU = 0, minV = 0, maxV = 0;
    uint8_t wrapU = 0u, wrapV = 0u;
};

struct G580QuadCache
{
    bool valid = false;
    int u0 = 0, v0 = 0;
    uint32_t c00 = 0u, c10 = 0u, c01 = 0u, c11 = 0u;
};

std::vector<G580SharedSample> g_g580SharedSamples;
std::vector<std::array<uint32_t, 256>> g_g580SharedCluts;
static thread_local const G580SharedSample *t_g580SharedSample = nullptr;
static thread_local G580QuadCache t_g580Quad{};

inline bool g580SharedEntryEligible(const G144Entry &e)
{
    const GSVertex &v0 = e.v[0], &v1 = e.v[1], &v2 = e.v[2];
    if (e.ctx.frame.fbp != 0x13bu || e.ctx.frame.fbw != 8u ||
        e.ctx.frame.psm != GS_PSM_CT32 || !e.prim.tme || !e.prim.abe ||
        (e.prim.type != GS_PRIM_TRIANGLE && e.prim.type != GS_PRIM_TRISTRIP) ||
        !e.prim.iip || e.prim.fst || e.prim.fge || e.prim.aa1 ||
        e.ctx.tex0.psm != GS_PSM_T8 || e.ctx.tex0.tfx != 0u ||
        e.ctx.tex0.tcc != 1u || e.ctx.tex0.cpsm != GS_PSM_CT32 ||
        e.ctx.tex0.csm != 0u || e.ctx.tex0.csa != 0u || e.ctx.tex0.tbw == 0u ||
        e.ctx.tex1 != 0x260ull || e.ctx.alpha != 0x44ull ||
        e.ctx.test != 0x5000bull || e.ctx.zbuf != 0x010000d0ull ||
        e.ctx.frame.fbmsk != 0u || e.ctx.fba != 0u || e.pabe)
        return false;

    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    const float x0 = v0.x - static_cast<float>(ofx);
    const float y0 = v0.y - static_cast<float>(ofy);
    const float x1 = v1.x - static_cast<float>(ofx);
    const float y1 = v1.y - static_cast<float>(ofy);
    const float x2 = v2.x - static_cast<float>(ofx);
    const float y2 = v2.y - static_cast<float>(ofy);
    const float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::fabs(denom) < 0.001f)
        return false;
    const int minX = std::max(static_cast<int>(e.ctx.scissor.x0),
                              static_cast<int>(std::floor(std::min({x0, x1, x2}))));
    const int maxX = std::min(static_cast<int>(e.ctx.scissor.x1),
                              static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
    return minX >= 0 && maxX < static_cast<int>(kG403DisplayZW) &&
           e.minY >= 0 && e.maxY < static_cast<int>(kG403DisplayZH);
}

static bool g580SharedClutBatchSelected(const std::vector<G144Entry> &entries)
{
    if (!g580Tri13bRequested() || !g580Tri13bQuiet() || entries.empty() ||
        entries.front().ctx.frame.fbp != 0x13bu || !g203UniversalZEnabled() ||
        !g404SharedZScope(0x13bu, 8u, 0xd0u, 1u))
        return false;
    return g580Tri13bSelected();
}

static bool g580PrepareSharedCluts(const std::vector<G144Entry> &entries,
                                   const G580ClutFillFn &fill)
{
    struct BuildKey
    {
        G580ClutKey key{};
        size_t representative = 0u;
        uint32_t refs = 0u;
    };
    constexpr size_t kMaxKeys = 128u;
    static const uint32_t minRefs = []() {
        const char *value = std::getenv("DC2_G580_CLUT_MIN_REFS");
        return static_cast<uint32_t>(value ? std::max(1, std::min(256, std::atoi(value))) : 8);
    }();

    g_g580SharedSamples.assign(entries.size(), G580SharedSample{});
    g_g580SharedCluts.clear();
    std::vector<BuildKey> keys;
    keys.reserve(32u);
    uint64_t eligible = 0u;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const G144Entry &e = entries[i];
        if (!g580SharedEntryEligible(e))
            continue;
        ++eligible;
        const G580ClutKey key{
            e.ctx.tex0.cbp, e.ctx.tex0.cpsm, e.ctx.tex0.csm, e.ctx.tex0.csa,
            e.ctx.tex0.psm, e.texa.ta0, e.texa.ta1, e.texclut.cbw,
            e.texclut.cou, e.texclut.cov, e.texa.aem};
        size_t keyIndex = 0u;
        while (keyIndex < keys.size() && !g580SameClutKey(keys[keyIndex].key, key))
            ++keyIndex;
        if (keyIndex == keys.size())
        {
            if (keys.size() == kMaxKeys)
            {
                g_g580SharedSamples.clear();
                return false;
            }
            keys.push_back(BuildKey{key, i, 0u});
        }
        ++keys[keyIndex].refs;
        G580SharedSample &sample = g_g580SharedSamples[i];
        sample.keyIndex = static_cast<uint16_t>(keyIndex);
        sample.tbp0 = e.ctx.tex0.tbp0;
        sample.pageBw = std::max(1u, static_cast<uint32_t>(e.ctx.tex0.tbw) >> 1u);
        sample.texW = 1 << e.ctx.tex0.tw;
        sample.texH = 1 << e.ctx.tex0.th;
        sample.wrapU = static_cast<uint8_t>(e.ctx.clamp & 0x3u);
        sample.wrapV = static_cast<uint8_t>((e.ctx.clamp >> 2u) & 0x3u);
        sample.minU = static_cast<int>((e.ctx.clamp >> 4u) & 0x3ffu);
        sample.maxU = static_cast<int>((e.ctx.clamp >> 14u) & 0x3ffu);
        sample.minV = static_cast<int>((e.ctx.clamp >> 24u) & 0x3ffu);
        sample.maxV = static_cast<int>((e.ctx.clamp >> 34u) & 0x3ffu);
    }

    std::vector<int> active(keys.size(), -1);
    uint64_t shared = 0u;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i].refs < minRefs)
            continue;
        active[i] = static_cast<int>(g_g580SharedCluts.size());
        g_g580SharedCluts.emplace_back();
        fill(entries[keys[i].representative], g_g580SharedCluts.back());
        shared += keys[i].refs;
    }
    for (G580SharedSample &sample : g_g580SharedSamples)
    {
        if (sample.keyIndex < active.size() && active[sample.keyIndex] >= 0)
        {
            sample.tableIndex = static_cast<uint16_t>(active[sample.keyIndex]);
            sample.ready = true;
        }
    }

    if (g580Tri13bStatOn())
    {
        static std::atomic<uint64_t> batches{0u}, totalEligible{0u}, totalShared{0u};
        const uint64_t n = batches.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t e = totalEligible.fetch_add(eligible, std::memory_order_relaxed) + eligible;
        const uint64_t s = totalShared.fetch_add(shared, std::memory_order_relaxed) + shared;
        if (n <= 8u || (n % 100u) == 0u)
            std::fprintf(stderr,
                         "[G580:clut] batches=%llu eligible=%llu shared=%llu keys=%zu tables=%zu minRefs=%u\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(e),
                         static_cast<unsigned long long>(s), keys.size(),
                         g_g580SharedCluts.size(), minRefs);
    }
    return shared != 0u;
}

static void g580BindSharedSample(size_t entryIndex)
{
    t_g580SharedSample = entryIndex < g_g580SharedSamples.size() &&
                                 g_g580SharedSamples[entryIndex].ready
                             ? &g_g580SharedSamples[entryIndex]
                             : nullptr;
}

static void g580ClearSharedSample()
{
    t_g580SharedSample = nullptr;
    t_g580Quad.valid = false;
}

inline bool g580SharedSampleReady()
{
    return t_g580SharedSample != nullptr;
}

inline void g580BeginSharedSampleRow()
{
    t_g580Quad.valid = false;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint32_t g580SampleSharedT8(uint8_t *vram, float s, float t, float q)
{
    const G580SharedSample &sample = *t_g580SharedSample;
    const std::array<uint32_t, 256> &clut = g_g580SharedCluts[sample.tableIndex];
    const float invQ = 1.0f / fabsQ(q);
    const float texUf = s * invQ * static_cast<float>(sample.texW);
    const float texVf = t * invQ * static_cast<float>(sample.texH);
    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);
    if (!t_g580Quad.valid || t_g580Quad.u0 != u0 || t_g580Quad.v0 != v0)
    {
        auto point = [&](int u, int v) {
            const int su = wrapTextureCoordinate(u, sample.wrapU, sample.texW,
                                                 sample.minU, sample.maxU);
            const int sv = wrapTextureCoordinate(v, sample.wrapV, sample.texH,
                                                 sample.minV, sample.maxV);
            const uint8_t index = static_cast<uint8_t>(GSMem::ReadP8(
                vram, sample.tbp0, sample.pageBw,
                static_cast<uint32_t>(su), static_cast<uint32_t>(sv)));
            return clut[index];
        };
        t_g580Quad.valid = true;
        t_g580Quad.u0 = u0;
        t_g580Quad.v0 = v0;
        t_g580Quad.c00 = point(u0, v0);
        t_g580Quad.c10 = point(u0 + 1, v0);
        t_g580Quad.c01 = point(u0, v0 + 1);
        t_g580Quad.c11 = point(u0 + 1, v0 + 1);
    }
    const uint32_t c00 = t_g580Quad.c00, c10 = t_g580Quad.c10;
    const uint32_t c01 = t_g580Quad.c01, c11 = t_g580Quad.c11;
    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00), static_cast<uint8_t>(c10),
                                  static_cast<uint8_t>(c01), static_cast<uint8_t>(c11), fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>(c00 >> 8u), static_cast<uint8_t>(c10 >> 8u),
                                  static_cast<uint8_t>(c01 >> 8u), static_cast<uint8_t>(c11 >> 8u), fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>(c00 >> 16u), static_cast<uint8_t>(c10 >> 16u),
                                  static_cast<uint8_t>(c01 >> 16u), static_cast<uint8_t>(c11 >> 16u), fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>(c00 >> 24u), static_cast<uint8_t>(c10 >> 24u),
                                  static_cast<uint8_t>(c01 >> 24u), static_cast<uint8_t>(c11 >> 24u), fx, fy);
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

std::atomic<uint64_t> g_g580Tris{0u};
std::atomic<uint64_t> g_g580Pixels{0u};
std::atomic<uint64_t> g_g580Bad{0u};

inline void g580NoteTri13b(uint64_t pixels, uint64_t bad)
{
    if (!g580Tri13bStatOn())
        return;
    const uint64_t tris = g_g580Tris.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px = g_g580Pixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
    const uint64_t totalBad = g_g580Bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    const bool firstBad = bad != 0u && totalBad == bad;
    if (tris <= 8u || (tris % 20000u) == 0u || firstBad)
        std::fprintf(stderr, "[G580:tri13b] tris=%llu px=%llu bad=%llu mode=%s\n",
                     static_cast<unsigned long long>(tris),
                     static_cast<unsigned long long>(px),
                     static_cast<unsigned long long>(totalBad),
                     g580Tri13bVerifyOn() ? "verify" : "fast");
}

inline void g580NoteTri13bState(const GSPrimReg &prim, bool pabe, const GSContext &ctx)
{
    if (!g580Tri13bCensusOn())
        return;
    static std::atomic<uint64_t> count{0u};
    const uint64_t n = count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 16u || (n % 65536u) == 0u)
        std::fprintf(stderr,
                     "[G580:state] n=%llu prim=%u iip=%u fst=%u fge=%u aa1=%u "
                     "test=%llx zbuf=%llx alpha=%llx fbmsk=%08x fba=%llx pabe=%u "
                     "tbp=%x tbw=%u psm=%x tfx=%u tcc=%u tex1=%llx clamp=%llx\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned>(prim.type), prim.iip ? 1u : 0u,
                     prim.fst ? 1u : 0u, prim.fge ? 1u : 0u,
                     prim.aa1 ? 1u : 0u,
                     static_cast<unsigned long long>(ctx.test),
                     static_cast<unsigned long long>(ctx.zbuf),
                     static_cast<unsigned long long>(ctx.alpha), ctx.frame.fbmsk,
                     static_cast<unsigned long long>(ctx.fba), pabe ? 1u : 0u,
                     ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                     static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.tex0.tfx),
                     static_cast<unsigned>(ctx.tex0.tcc),
                     static_cast<unsigned long long>(ctx.tex1),
                     static_cast<unsigned long long>(ctx.clamp));
}

} // namespace
