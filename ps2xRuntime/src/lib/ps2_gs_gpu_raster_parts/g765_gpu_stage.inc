#include <algorithm>
// ===== G765: GPU-SIDE STAGE CLOCK FOR renderBatch (diagnostic, DC2_G765_GPUSTAGE=1) =====
//
// WHY. `[G495:gpu]` (GL_TIME_ELAPSED per backend item) reads gpuBusyMs/f ~16 on g765e dragon at
// 1600x900 against a ~17 ms frame, with `rend` ~11.2 ms/f of it — while the same items' CPU bodies
// are ~2.4 ms/f. The GPU, not the executor, is now the thing the readbacks wait for, and nothing in
// the tree splits renderBatch's GPU time. This stamps GL_TIMESTAMP at renderBatch's existing G415
// marks (entry, tex, fbo, fb upload, depth, state, draws, drain, readback) and sums the GPU-side
// deltas per stage, plus the GPU time between consecutive batches ("gap": other backend items).
// Timestamps are harvested a full ring later, so the read never waits on the GPU. Never quote a
// frame time from a run with this armed (one driver query per mark).
#ifndef GL765_TIMESTAMP
#define GL765_TIMESTAMP 0x8E28
#define GL765_QUERY_RESULT 0x8866
#define GL765_QUERY_RESULT_AVAILABLE 0x8867
#endif
typedef void(APIENTRY *PFN765GENQUERIES)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN765QUERYCOUNTER)(GLuint, GLenum);
typedef void(APIENTRY *PFN765GETQUERYOBJECTUI64V)(GLuint, GLenum, uint64_t *);

static bool g765GpuStageOn()
{
    static const bool on = (std::getenv("DC2_G765_GPUSTAGE") != nullptr);
    return on;
}

static constexpr int kG765StageRing = 4096;
static constexpr int kG765StageMarks = 9;

struct G765GpuStage
{
    PFN765GENQUERIES glGenQueries_ = nullptr;
    PFN765QUERYCOUNTER glQueryCounter_ = nullptr;
    PFN765GETQUERYOBJECTUI64V glGetQueryObjectui64v_ = nullptr;
    GLuint q[kG765StageRing] = {};
    int mark[kG765StageRing] = {};
    uint16_t fbp[kG765StageRing] = {};
    uint32_t draws[kG765StageRing] = {};
    uint64_t fbpNs[512] = {};   // per-target GPU time of the draw + post-draw intervals (5->7)
    uint64_t fbpBatches[512] = {};
    uint64_t fbpDraws[512] = {};
    bool armed[kG765StageRing] = {};
    bool tried = false, ok = false;
    int cursor = 0;
    // harvest state: the previous harvested stamp (to form a delta)
    bool havePrev = false;
    int prevMark = -1;
    uint64_t prevTs = 0;
    uint64_t ns[kG765StageMarks] = {}; // [0..7] stage i->i+1, [8] gap (last mark -> next entry)
    uint64_t n[kG765StageMarks] = {};
    uint64_t firstTs = 0, lastTs = 0;
    uint64_t stamps = 0;
};
static G765GpuStage g_g765GpuStage;

static void g765GpuStageHarvest(int slot)
{
    G765GpuStage &g = g_g765GpuStage;
    if (!g.armed[slot])
        return;
    uint64_t ts = 0;
    g.glGetQueryObjectui64v_(g.q[slot], GL765_QUERY_RESULT, &ts);
    const int m = g.mark[slot];
    if (g.havePrev && ts >= g.prevTs)
    {
        const uint64_t d = ts - g.prevTs;
        if (m == g.prevMark + 1 && m >= 1 && m <= 8)
        {
            g.ns[m - 1] += d;
            ++g.n[m - 1];
            if (m == 6 || m == 7)
            {
                const uint16_t f = g.fbp[slot] & 511u;
                g.fbpNs[f] += d;
                if (m == 6)
                {
                    ++g.fbpBatches[f];
                    g.fbpDraws[f] += g.draws[slot];
                }
            }
        }
        else if (m == 0)
        {
            g.ns[8] += d;
            ++g.n[8];
        }
    }
    if (g.firstTs == 0)
        g.firstTs = ts;
    g.lastTs = ts;
    g.havePrev = true;
    g.prevMark = m;
    g.prevTs = ts;
    g.armed[slot] = false;
}

static void g765GpuStageReport()
{
    G765GpuStage &g = g_g765GpuStage;
    static const char *const kNames[kG765StageMarks] = {
        "tex", "fbo", "up", "z", "state", "draw", "drain", "rb", "gap"};
    const double spanMs = static_cast<double>(g.lastTs - g.firstTs) / 1.0e6;
    if (spanMs <= 0.0)
        return;
    char line[640];
    int off = std::snprintf(line, sizeof(line), "[G765:gpustage] spanMs=%.0f", spanMs);
    double total = 0.0;
    for (int i = 0; i < kG765StageMarks; ++i)
    {
        const double share = static_cast<double>(g.ns[i]) / 1.0e6 / spanMs * 1000.0; // ms per s
        total += (i < 8) ? share : 0.0;
        off += std::snprintf(line + off, sizeof(line) - off, " %s=%.1f/%llu", kNames[i], share,
                             static_cast<unsigned long long>(g.n[i]));
    }
    std::fprintf(stderr, "%s | batchGpuMsPerSec=%.1f\n", line, total);
    int order[512];
    for (int i = 0; i < 512; ++i)
        order[i] = i;
    std::sort(order, order + 512, [&](int a, int b) { return g.fbpNs[a] > g.fbpNs[b]; });
    off = std::snprintf(line, sizeof(line), "[G765:gpufbp]");
    for (int k = 0; k < 8 && g.fbpNs[order[k]] != 0u; ++k)
    {
        const int f = order[k];
        off += std::snprintf(line + off, sizeof(line) - off, " %03x=%.1fms/s(b=%llu d=%.1f)", f,
                             static_cast<double>(g.fbpNs[f]) / 1.0e6 / spanMs * 1000.0,
                             static_cast<unsigned long long>(g.fbpBatches[f]),
                             g.fbpBatches[f] ? static_cast<double>(g.fbpDraws[f]) / g.fbpBatches[f] : 0.0);
    }
    std::fprintf(stderr, "%s\n", line);
    for (int i = 0; i < 512; ++i)
    {
        g.fbpNs[i] = 0;
        g.fbpBatches[i] = 0;
        g.fbpDraws[i] = 0;
    }
    for (int i = 0; i < kG765StageMarks; ++i)
    {
        g.ns[i] = 0;
        g.n[i] = 0;
    }
    g.firstTs = g.lastTs;
}

// Called at every G415 mark of renderBatch with the mark index about to be recorded.
static void g765GpuStamp(int markIndex, uint32_t fbp, uint32_t draws)
{
    if (!g765GpuStageOn())
        return;
    G765GpuStage &g = g_g765GpuStage;
    if (!g.tried)
    {
        g.tried = true;
        g.ok = loadProc(g.glGenQueries_, "glGenQueries") &&
               loadProc(g.glQueryCounter_, "glQueryCounter") &&
               loadProc(g.glGetQueryObjectui64v_, "glGetQueryObjectui64v");
        if (g.ok)
            g.glGenQueries_(kG765StageRing, g.q);
        std::fprintf(stderr, "[G765:gpustage] timestamps %s\n", g.ok ? "loaded" : "UNAVAILABLE");
    }
    if (!g.ok || markIndex < 0 || markIndex >= kG765StageMarks)
        return;
    const int i = g.cursor;
    g765GpuStageHarvest(i); // a full ring ago: complete, never waits
    g.glQueryCounter_(g.q[i], GL765_TIMESTAMP);
    g.mark[i] = markIndex;
    g.fbp[i] = static_cast<uint16_t>(fbp & 511u);
    g.draws[i] = draws;
    g.armed[i] = true;
    g.cursor = (i + 1) % kG765StageRing;
    if ((++g.stamps % (kG765StageRing * 4u)) == 0u)
        g765GpuStageReport();
}
