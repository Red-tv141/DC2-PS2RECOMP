// ===== G767: GPU TIME PER DRAW CLASS (diagnostic, DC2_G767_DRAWGPU=<N>, default OFF) =====
//
// WHY. Once the readbacks are folded away the frame on the GPU-heavy routes is GPU time spent in
// renderBatch's draws (`[G765:gpustage] draw=`), but nothing splits it by WHAT is drawn. This stamps
// GL_TIMESTAMP around every main-path glDrawArrays of every N-th batch (N defaults to 8), keys the
// delta by a draw-state class (target, textured, blend, depth test/write, alpha test, fog, exact
// sampler, bilinear, dual-source, screen sprite) and sums GPU ns and logical fragment area per
// class. Timestamps are harvested a full ring later, so reading them never waits. Never quote a
// frame time from a run with this armed.
static int g767DrawGpuEvery()
{
    static const int n = [] {
        const char *e = std::getenv("DC2_G767_DRAWGPU");
        if (e == nullptr)
            return 0;
        const int v = std::atoi(e);
        return v <= 0 ? 8 : v;
    }();
    return n;
}

struct G767DrawGpu
{
    static constexpr int kRing = 8192;
    PFN765GENQUERIES glGenQueries_ = nullptr;
    PFN765QUERYCOUNTER glQueryCounter_ = nullptr;
    PFN765GETQUERYOBJECTUI64V glGetQueryObjectui64v_ = nullptr;
    GLuint q[kRing] = {};
    uint32_t cls[kRing] = {};
    float px[kRing] = {};
    uint8_t kind[kRing] = {}; // 0 empty, 1 begin, 2 end
    int cursor = 0;
    bool tried = false, ok = false;
    uint64_t beginTs = 0u;
    uint64_t firstTs = 0u, lastTs = 0u;
    uint64_t ends = 0u;
    struct Acc
    {
        uint64_t ns = 0u, draws = 0u;
        double px = 0.0;
    };
    std::unordered_map<uint32_t, Acc> acc;
};
static G767DrawGpu g_g767DrawGpu;

static void g767DrawGpuHarvest(int slot)
{
    G767DrawGpu &g = g_g767DrawGpu;
    if (g.kind[slot] == 0)
        return;
    uint64_t ts = 0u;
    g.glGetQueryObjectui64v_(g.q[slot], GL765_QUERY_RESULT, &ts);
    if (g.kind[slot] == 1)
        g.beginTs = ts;
    else if (g.beginTs != 0u && ts >= g.beginTs)
    {
        G767DrawGpu::Acc &a = g.acc[g.cls[slot]];
        a.ns += ts - g.beginTs;
        ++a.draws;
        a.px += g.px[slot];
        g.beginTs = 0u;
    }
    if (g.firstTs == 0u)
        g.firstTs = ts;
    g.lastTs = ts;
    g.kind[slot] = 0;
}

static void g767DrawGpuReport()
{
    G767DrawGpu &g = g_g767DrawGpu;
    const double spanMs = static_cast<double>(g.lastTs - g.firstTs) / 1.0e6;
    if (spanMs <= 0.0 || g.acc.empty())
        return;
    std::vector<std::pair<uint32_t, G767DrawGpu::Acc>> v(g.acc.begin(), g.acc.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
    double total = 0.0;
    for (const auto &kv : v)
        total += static_cast<double>(kv.second.ns);
    std::fprintf(stderr, "[G767:drawgpu] sampledMs/s=%.1f classes=%zu (every %d-th batch)\n",
                 total / 1.0e6 / spanMs * 1000.0, v.size(), g767DrawGpuEvery());
    for (size_t k = 0u; k < v.size() && k < 14u; ++k)
    {
        const uint32_t c = v[k].first;
        const G767DrawGpu::Acc &a = v[k].second;
        std::fprintf(stderr,
                     "[G767:drawgpu]  %5.1f%% fbp=%03x tex=%u blend=%u dfunc=%u dw=%u atest=%u fog=%u "
                     "exact=%u bilin=%u dual=%u spr=%u fbo=%u | draws=%llu Mpx=%.2f ns/px=%.3f\n",
                     100.0 * static_cast<double>(a.ns) / total, c & 511u, (c >> 9) & 1u,
                     (c >> 10) & 7u, (c >> 13) & 3u, (c >> 15) & 1u, (c >> 16) & 1u,
                     (c >> 17) & 1u, (c >> 18) & 1u, (c >> 19) & 1u, (c >> 20) & 1u,
                     (c >> 21) & 1u, (c >> 22) & 1u, static_cast<unsigned long long>(a.draws),
                     a.px / 1.0e6, a.px > 0.0 ? static_cast<double>(a.ns) / a.px : 0.0);
    }
    g.acc.clear();
    g.firstTs = g.lastTs;
}

static void g767DrawGpuStamp(bool end, uint32_t cls, float px)
{
    G767DrawGpu &g = g_g767DrawGpu;
    if (!g.tried)
    {
        g.tried = true;
        g.ok = loadProc(g.glGenQueries_, "glGenQueries") &&
               loadProc(g.glQueryCounter_, "glQueryCounter") &&
               loadProc(g.glGetQueryObjectui64v_, "glGetQueryObjectui64v");
        if (g.ok)
            g.glGenQueries_(G767DrawGpu::kRing, g.q);
        std::fprintf(stderr, "[G767:drawgpu] timestamps %s\n", g.ok ? "loaded" : "UNAVAILABLE");
    }
    if (!g.ok)
        return;
    const int i = g.cursor;
    g767DrawGpuHarvest(i);
    g.glQueryCounter_(g.q[i], GL765_TIMESTAMP);
    g.kind[i] = end ? 2u : 1u;
    g.cls[i] = cls;
    g.px[i] = px;
    g.cursor = (i + 1) % G767DrawGpu::kRing;
    if (end && (++g.ends % 60000u) == 0u)
        g767DrawGpuReport();
}
